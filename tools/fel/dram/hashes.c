/* SHA-1, SHA-256 (FIPS 180-4) and AES-128-CTR (FIPS 197), see hashes.h. */
#include "hashes.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* Merkle-Damgard padding shared by both hashes: calls blk() for each 64-byte block */
static void md_run(const uint8_t *d, int n, void (*blk)(uint32_t *, const uint8_t *), uint32_t *h)
{
	uint8_t last[128];
	int full = n / 64, rem = n % 64;
	for (int i = 0; i < full; i++) blk(h, d + 64 * i);
	for (int i = 0; i < 128; i++) last[i] = 0;
	for (int i = 0; i < rem; i++) last[i] = d[64 * full + i];
	last[rem] = 0x80;
	int tot = rem + 1 + 8 <= 64 ? 64 : 128;
	uint64_t bits = (uint64_t)n * 8;
	for (int i = 0; i < 8; i++) last[tot - 1 - i] = (uint8_t)(bits >> (8 * i));
	blk(h, last);
	if (tot == 128) blk(h, last + 64);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void sha1_blk(uint32_t *h, const uint8_t *b)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
	for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
		else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
		else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
		else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
		uint32_t t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(bb, 30); bb = a; a = t;
	}
	h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(uint8_t out[20], const uint8_t *d, int n)
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	md_run(d, n, sha1_blk, h);
	for (int i = 0; i < 5; i++) put32(out + 4 * i, h[i]);
}

static const uint32_t K256[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static void sha256_blk(uint32_t *h, const uint8_t *b)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g);
		uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
		uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & bb) ^ (a & c) ^ (bb & c);
		uint32_t t2 = S0 + mj;
		hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
	}
	h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(uint8_t out[32], const uint8_t *d, int n)
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	md_run(d, n, sha256_blk, h);
	for (int i = 0; i < 8; i++) put32(out + 4 * i, h[i]);
}

/* AES-128 encryption (only the forward cipher is needed for CTR) */
#define ROL8(x, n) ((uint8_t)((x) << (n) | (x) >> (8 - (n))))
static uint8_t sbox[256];
static void sbox_init(void)                         /* S-box from GF(2^8) inverse + affine map */
{
	if (sbox[0]) return;
	uint8_t p = 1, q = 1;
	do {
		p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));              /* p *= 3 */
		q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);   /* q /= 3 */
		if (q & 0x80) q ^= 0x09;
		uint8_t x = (uint8_t)(q ^ ROL8(q, 1) ^ ROL8(q, 2) ^ ROL8(q, 3) ^ ROL8(q, 4));
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
}
#undef ROL8
static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1B : 0)); }

static void aes128_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
	for (int r = 1; r <= 10; r++) {
		uint8_t t[16];
		for (int i = 0; i < 16; i++) t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];   /* SubBytes + ShiftRows */
		if (r < 10)
			for (int c = 0; c < 4; c++) {                                       /* MixColumns */
				uint8_t *m = t + 4 * c, a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3], all = a0 ^ a1 ^ a2 ^ a3;
				m[0] ^= all ^ xt(a0 ^ a1); m[1] ^= all ^ xt(a1 ^ a2); m[2] ^= all ^ xt(a2 ^ a3); m[3] ^= all ^ xt(a3 ^ a0);
			}
		for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
	}
	for (int i = 0; i < 16; i++) out[i] = s[i];
}

void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *buf, int n)
{
	uint8_t rk[176], ctr[16], ks[16], rc = 1;
	sbox_init();
	for (int i = 0; i < 16; i++) { rk[i] = key[i]; ctr[i] = iv[i]; }
	for (int i = 16; i < 176; i += 4) {                 /* key expansion */
		uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
		if (i % 16 == 0) {
			uint8_t u = t0;
			t0 = (uint8_t)(sbox[t1] ^ rc); t1 = sbox[t2]; t2 = sbox[t3]; t3 = sbox[u];
			rc = xt(rc);
		}
		rk[i] = rk[i - 16] ^ t0; rk[i + 1] = rk[i - 15] ^ t1; rk[i + 2] = rk[i - 14] ^ t2; rk[i + 3] = rk[i - 13] ^ t3;
	}
	for (int off = 0; off < n; off += 16) {
		aes128_block(rk, ctr, ks);
		for (int i = 0; i < 16 && off + i < n; i++) buf[off + i] ^= ks[i];
		for (int i = 15; i >= 0; i--) if (++ctr[i]) break;
	}
}
