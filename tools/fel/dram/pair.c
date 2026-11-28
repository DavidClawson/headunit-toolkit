/* AirPlay/CarPlay pairing, accessory side. See pair.h and docs/carplay-pairing.md.
   Message flow (HomeKit-style; confirm against the doc):
     pair-setup  M1 {State 1, Method 0}            -> M2 {State 2, Salt, PublicKey B}
                 M3 {State 3, PublicKey A, Proof}  -> M4 {State 4, Proof M2}
                 M5 {State 5, EncryptedData}       -> M6 {State 6, EncryptedData}
     pair-verify M1 {State 1, PublicKey}           -> M2 {State 2, PublicKey, EncryptedData}
                 M3 {State 3, EncryptedData}       -> M4 {State 4}                         */
#include "pair.h"
#include "third_party/monocypher/monocypher.h"
#include "third_party/monocypher/monocypher-ed25519.h"

enum { T_METHOD = 0, T_ID = 1, T_SALT = 2, T_PUBKEY = 3, T_PROOF = 4, T_ENC = 5, T_STATE = 6,
       T_ERROR = 7, T_SIG = 10, T_FLAGS = 19 };
enum { E_AUTH = 2 };

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void cpy(uint8_t *d, const void *s, int n) { const uint8_t *q = s; for (int i = 0; i < n; i++) d[i] = q[i]; }

int tlv_get(const uint8_t *in, int n, uint8_t type, uint8_t *val, int cap)
{
	int got = -1, i = 0;
	while (i + 2 <= n) {
		uint8_t t = in[i], l = in[i + 1];
		if (i + 2 + l > n) return -1;
		if (t == type) {
			if (got < 0) got = 0;
			if (got + l > cap) return -1;
			cpy(val + got, in + i + 2, l);
			got += l;
			i += 2 + l;
			if (l < 255) break;                 /* a full fragment continues in the next item */
			if (!(i + 2 <= n && in[i] == type)) break;
			continue;
		}
		if (got >= 0) break;
		i += 2 + l;
	}
	return got;
}

int tlv_put(uint8_t *out, int pos, int cap, uint8_t type, const uint8_t *val, int len)
{
	do {
		int l = len > 255 ? 255 : len;
		if (pos < 0 || pos + 2 + l > cap) return -1;
		out[pos] = type; out[pos + 1] = (uint8_t)l;
		cpy(out + pos + 2, val, l);
		pos += 2 + l; val += l; len -= l;
	} while (len > 0);
	return pos;
}

static int put_u8(uint8_t *out, int pos, int cap, uint8_t type, uint8_t v) { return tlv_put(out, pos, cap, type, &v, 1); }

static int error_reply(pair_ctx *p, uint8_t *out, int cap, uint8_t state, int where)
{
	p->last_err = where;
	int o = put_u8(out, 0, cap, T_STATE, state);
	return put_u8(out, o, cap, T_ERROR, E_AUTH);
}

static void hkdf(uint8_t *okm, int n, const uint8_t *ikm, int ikmn, const char *salt, const char *info)
{
	crypto_sha512_hkdf(okm, (size_t)n, ikm, (size_t)ikmn, (const uint8_t *)salt, (size_t)slen(salt),
	                   (const uint8_t *)info, (size_t)slen(info));
}

static void nonce8(uint8_t n12[12], const char *tag)      /* 4 zero bytes + 8-byte tag, e.g. "PS-Msg05" */
{
	for (int i = 0; i < 4; i++) n12[i] = 0;
	cpy(n12 + 4, tag, 8);
}

static void seal(uint8_t *ct, const uint8_t key[32], const char *tag, const uint8_t *pt, int n)   /* ct = pt || mac */
{
	uint8_t nonce[12]; crypto_aead_ctx c;
	nonce8(nonce, tag);
	crypto_aead_init_ietf(&c, key, nonce);
	crypto_aead_write(&c, ct, ct + n, 0, 0, pt, (size_t)n);
	crypto_wipe(&c, sizeof c);
}

static int open_(uint8_t *pt, const uint8_t key[32], const char *tag, const uint8_t *ct, int n)   /* n includes the mac */
{
	if (n < 16) return -1;
	uint8_t nonce[12]; crypto_aead_ctx c;
	nonce8(nonce, tag);
	crypto_aead_init_ietf(&c, key, nonce);
	int r = crypto_aead_read(&c, pt, ct + n - 16, 0, 0, ct, (size_t)(n - 16));
	crypto_wipe(&c, sizeof c);
	return r ? -1 : n - 16;
}

void pair_init(pair_ctx *p, const uint8_t seed[32], const char *id, const char *pin, pair_rng rng)
{
	uint8_t s[32];
	cpy(s, seed, 32);                                   /* key_pair wipes its seed */
	crypto_ed25519_key_pair(p->ltsk, p->ltpk, s);
	p->rng = rng; p->id = id; p->pin = pin;
	p->ps_state = 0; p->pv_state = 0; p->transient = 0; p->m1_len = 0; p->have_ctrl = 0; p->verified = 0; p->last_err = 0;
}

int pair_setup(pair_ctx *p, const uint8_t *in, int n, uint8_t *out, int cap)
{
	static uint8_t buf[1024], sub[512];
	uint8_t st;
	if (tlv_get(in, n, T_STATE, &st, 1) != 1) return -1;

	if (st == 1) {                                      /* M1 -> M2: salt and B */
		uint8_t salt[16], b[32], fl[4] = { 0 };
		int fn = tlv_get(in, n, T_FLAGS, fl, 4);
		p->transient = fn > 0 && (fl[0] & 0x10);        /* transient: stop after M4 */
		p->m1_len = n > 16 ? 16 : n; cpy(p->m1, in, p->m1_len);
		p->rng(salt, 16); p->rng(b, 32);
		srp_start(&p->srp, "Pair-Setup", p->pin, salt, b);
		crypto_wipe(b, 32);
		p->ps_state = 2;
		int o = put_u8(out, 0, cap, T_STATE, 2);
		o = tlv_put(out, o, cap, T_SALT, salt, 16);
		const uint8_t *B = p->srp.Bbytes; int bl = BN_BYTES;
		while (bl > 1 && *B == 0) { B++; bl--; }
		return tlv_put(out, o, cap, T_PUBKEY, B, bl);
	}
	if (st == 3 && p->ps_state == 2) {                  /* M3 -> M4: check the client's proof */
		uint8_t M1[64];
		int al = tlv_get(in, n, T_PUBKEY, buf, sizeof buf);
		if (al <= 0 || tlv_get(in, n, T_PROOF, M1, 64) != 64) return error_reply(p, out, cap, 4, 31);
		if (srp_finish(&p->srp, buf, al, M1)) return error_reply(p, out, cap, 4, 32);   /* wrong PIN */
		p->ps_state = 4;
		if (p->transient) p->verified = 1;              /* session secret: K (no long-term keys) */
		int o = put_u8(out, 0, cap, T_STATE, 4);
		return tlv_put(out, o, cap, T_PROOF, p->srp.M2, 64);
	}
	if (st == 5 && p->ps_state == 4) {                  /* M5 -> M6: exchange long-term keys */
		uint8_t key[32], x[32], sig[64];
		hkdf(key, 32, p->srp.K, 64, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
		int el = tlv_get(in, n, T_ENC, buf, sizeof buf);
		int pl = el > 0 ? open_(sub, key, "PS-Msg05", buf, el) : -1;
		if (pl < 0) return error_reply(p, out, cap, 6, 51);
		int il = tlv_get(sub, pl, T_ID, p->ctrl_id, sizeof p->ctrl_id);
		if (il <= 0 || tlv_get(sub, pl, T_PUBKEY, p->ctrl_ltpk, 32) != 32 || tlv_get(sub, pl, T_SIG, sig, 64) != 64)
			return error_reply(p, out, cap, 6, 52);
		/* the controller signed iOSDeviceX | id | LTPK */
		hkdf(x, 32, p->srp.K, 64, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info");
		int m = 0;
		cpy(buf + m, x, 32); m += 32;
		cpy(buf + m, p->ctrl_id, il); m += il;
		cpy(buf + m, p->ctrl_ltpk, 32); m += 32;
		if (crypto_ed25519_check(sig, p->ctrl_ltpk, buf, (size_t)m)) return error_reply(p, out, cap, 6, 53);
		p->ctrl_id_len = il; p->have_ctrl = 1;
		/* our reply: AccessoryX | id | LTPK, signed */
		hkdf(x, 32, p->srp.K, 64, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info");
		int idl = slen(p->id);
		m = 0;
		cpy(buf + m, x, 32); m += 32;
		cpy(buf + m, p->id, idl); m += idl;
		cpy(buf + m, p->ltpk, 32); m += 32;
		crypto_ed25519_sign(sig, p->ltsk, buf, (size_t)m);
		int s = tlv_put(sub, 0, sizeof sub, T_ID, (const uint8_t *)p->id, idl);
		s = tlv_put(sub, s, sizeof sub, T_PUBKEY, p->ltpk, 32);
		s = tlv_put(sub, s, sizeof sub, T_SIG, sig, 64);
		if (s < 0) return -1;
		seal(buf, key, "PS-Msg06", sub, s);
		p->ps_state = 6;
		int o = put_u8(out, 0, cap, T_STATE, 6);
		return tlv_put(out, o, cap, T_ENC, buf, s + 16);
	}
	return error_reply(p, out, cap, (uint8_t)(st + 1), 90 + st);
}

int pair_verify(pair_ctx *p, const uint8_t *in, int n, uint8_t *out, int cap)
{
	static uint8_t buf[512], sub[256];
	uint8_t st;
	if (tlv_get(in, n, T_STATE, &st, 1) != 1) return -1;

	if (st == 1) {                                      /* M1 -> M2 */
		if (tlv_get(in, n, T_PUBKEY, p->pv_peer, 32) != 32) return error_reply(p, out, cap, 2, 11);
		p->rng(p->pv_sec, 32);
		crypto_x25519_public_key(p->pv_pub, p->pv_sec);
		crypto_x25519(p->pv_shared, p->pv_sec, p->pv_peer);
		hkdf(p->pv_key, 32, p->pv_shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
		uint8_t sig[64];
		int idl = slen(p->id), m = 0;                   /* sign our pub | id | their pub */
		cpy(buf + m, p->pv_pub, 32); m += 32;
		cpy(buf + m, p->id, idl); m += idl;
		cpy(buf + m, p->pv_peer, 32); m += 32;
		crypto_ed25519_sign(sig, p->ltsk, buf, (size_t)m);
		int s = tlv_put(sub, 0, sizeof sub, T_ID, (const uint8_t *)p->id, idl);
		s = tlv_put(sub, s, sizeof sub, T_SIG, sig, 64);
		if (s < 0) return -1;
		seal(buf, p->pv_key, "PV-Msg02", sub, s);
		p->pv_state = 2; p->verified = 0;
		int o = put_u8(out, 0, cap, T_STATE, 2);
		o = tlv_put(out, o, cap, T_PUBKEY, p->pv_pub, 32);
		return tlv_put(out, o, cap, T_ENC, buf, s + 16);
	}
	if (st == 3 && p->pv_state == 2) {                  /* M3 -> M4 */
		uint8_t id[64], sig[64];
		int el = tlv_get(in, n, T_ENC, buf, sizeof buf);
		int pl = el > 0 ? open_(sub, p->pv_key, "PV-Msg03", buf, el) : -1;
		if (pl < 0) return error_reply(p, out, cap, 4, 31);
		int il = tlv_get(sub, pl, T_ID, id, sizeof id);
		if (il <= 0 || tlv_get(sub, pl, T_SIG, sig, 64) != 64) return error_reply(p, out, cap, 4, 32);
		if (!p->have_ctrl || il != p->ctrl_id_len) return error_reply(p, out, cap, 4, 33);   /* unknown phone */
		for (int i = 0; i < il; i++) if (id[i] != p->ctrl_id[i]) return error_reply(p, out, cap, 4, 33);
		int m = 0;                                      /* their pub | their id | our pub */
		cpy(buf + m, p->pv_peer, 32); m += 32;
		cpy(buf + m, id, il); m += il;
		cpy(buf + m, p->pv_pub, 32); m += 32;
		if (crypto_ed25519_check(sig, p->ctrl_ltpk, buf, (size_t)m)) return error_reply(p, out, cap, 4, 34);
		p->pv_state = 4; p->verified = 1;
		return put_u8(out, 0, cap, T_STATE, 4);
	}
	return error_reply(p, out, cap, (uint8_t)(st + 1), 80 + st);
}
