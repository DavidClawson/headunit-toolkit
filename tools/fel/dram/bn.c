/* Fixed-size big numbers for SRP-6a, see bn.h. Written for this project. */
#include "bn.h"

void bn_from_bytes(bn *a, const uint8_t *b, int len)
{
	for (int i = 0; i < BN_LIMBS; i++) a->l[i] = 0;
	for (int i = 0; i < len; i++) {
		int bit = (len - 1 - i) * 8;
		a->l[bit / 32] |= (uint32_t)b[i] << (bit % 32);
	}
}

void bn_to_bytes(uint8_t out[BN_BYTES], const bn *a)
{
	for (int i = 0; i < BN_BYTES; i++) {
		int bit = (BN_BYTES - 1 - i) * 8;
		out[i] = (uint8_t)(a->l[bit / 32] >> (bit % 32));
	}
}

int bn_cmp(const bn *a, const bn *b)
{
	for (int i = BN_LIMBS - 1; i >= 0; i--)
		if (a->l[i] != b->l[i]) return a->l[i] > b->l[i] ? 1 : -1;
	return 0;
}

int bn_is_zero(const bn *a)
{
	uint32_t x = 0;
	for (int i = 0; i < BN_LIMBS; i++) x |= a->l[i];
	return x == 0;
}

static uint32_t bn_sub(bn *r, const bn *a, const bn *b)   /* r = a - b, returns borrow */
{
	uint64_t br = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		uint64_t d = (uint64_t)a->l[i] - b->l[i] - br;
		r->l[i] = (uint32_t)d;
		br = (d >> 32) & 1;
	}
	return (uint32_t)br;
}

static uint32_t bn_add(bn *r, const bn *a, const bn *b)   /* r = a + b, returns carry */
{
	uint64_t c = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		c += (uint64_t)a->l[i] + b->l[i];
		r->l[i] = (uint32_t)c;
		c >>= 32;
	}
	return (uint32_t)c;
}

void bn_modadd(bn *r, const bn *a, const bn *b, const bn_mod *m)
{
	bn t;
	uint32_t c = bn_add(&t, a, b);
	if (c || bn_cmp(&t, &m->n) >= 0) bn_sub(&t, &t, &m->n);
	*r = t;
}

void bn_reduce(bn *r, const bn *a, const bn_mod *m)          /* bit-serial; used only at setup */
{
	bn t = { { 0 } };
	for (int i = BN_LIMBS * 32 - 1; i >= 0; i--) {
		uint32_t top = t.l[BN_LIMBS - 1] >> 31;             /* t = 2t + bit */
		for (int j = BN_LIMBS - 1; j > 0; j--) t.l[j] = t.l[j] << 1 | t.l[j - 1] >> 31;
		t.l[0] = t.l[0] << 1 | (a->l[i / 32] >> (i % 32) & 1);
		if (top || bn_cmp(&t, &m->n) >= 0) bn_sub(&t, &t, &m->n);
	}
	*r = t;
}

/* Montgomery product: r = a*b*R^-1 mod n (CIOS) */
static void mont_mul(bn *r, const bn *a, const bn *b, const bn_mod *m)
{
	uint32_t t[BN_LIMBS + 2];
	for (int i = 0; i < BN_LIMBS + 2; i++) t[i] = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		uint64_t c = 0;
		for (int j = 0; j < BN_LIMBS; j++) {
			c += (uint64_t)t[j] + (uint64_t)a->l[j] * b->l[i];
			t[j] = (uint32_t)c;
			c >>= 32;
		}
		c += t[BN_LIMBS];
		t[BN_LIMBS] = (uint32_t)c;
		t[BN_LIMBS + 1] = (uint32_t)(c >> 32);
		uint32_t q = t[0] * m->n0;
		c = (uint64_t)t[0] + (uint64_t)q * m->n.l[0];
		c >>= 32;
		for (int j = 1; j < BN_LIMBS; j++) {
			c += (uint64_t)t[j] + (uint64_t)q * m->n.l[j];
			t[j - 1] = (uint32_t)c;
			c >>= 32;
		}
		c += t[BN_LIMBS];
		t[BN_LIMBS - 1] = (uint32_t)c;
		t[BN_LIMBS] = t[BN_LIMBS + 1] + (uint32_t)(c >> 32);
	}
	bn res;
	for (int i = 0; i < BN_LIMBS; i++) res.l[i] = t[i];
	if (t[BN_LIMBS] || bn_cmp(&res, &m->n) >= 0) bn_sub(&res, &res, &m->n);
	*r = res;
}

void bn_mod_init(bn_mod *m, const bn *n)
{
	m->n = *n;
	uint32_t inv = 1;                                       /* Newton: inv = n0^-1 mod 2^32 */
	for (int i = 0; i < 5; i++) inv *= 2 - n->l[0] * inv;
	m->n0 = (uint32_t)(0u - inv);
	bn t = { { 0 } };                                       /* R^2 mod n by doubling 1, 6144 times */
	t.l[0] = 1;
	for (int i = 0; i < 2 * BN_LIMBS * 32; i++) {
		uint32_t c = bn_add(&t, &t, &t);
		if (c || bn_cmp(&t, &m->n) >= 0) bn_sub(&t, &t, &m->n);
	}
	m->r2 = t;
}

void bn_modmul(bn *r, const bn *a, const bn *b, const bn_mod *m)
{
	bn t;
	mont_mul(&t, a, b, m);                                  /* a*b/R */
	mont_mul(r, &t, &m->r2, m);                             /* * R^2 / R = a*b */
}

void bn_modexp(bn *r, const bn *base, const uint8_t *e, int elen, const bn_mod *m)
{
	bn one = { { 0 } }, x, acc;
	one.l[0] = 1;
	mont_mul(&x, base, &m->r2, m);                          /* base in Montgomery form */
	mont_mul(&acc, &one, &m->r2, m);                        /* 1 in Montgomery form */
	for (int i = 0; i < elen; i++)
		for (int bit = 7; bit >= 0; bit--) {
			mont_mul(&acc, &acc, &acc, m);
			if (e[i] >> bit & 1) mont_mul(&acc, &acc, &x, m);
		}
	mont_mul(r, &acc, &one, m);                             /* out of Montgomery form */
}
