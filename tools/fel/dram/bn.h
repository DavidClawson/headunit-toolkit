/* Fixed-size big numbers for SRP-6a (3072-bit group), Montgomery arithmetic.
   Numbers are little-endian arrays of 32-bit limbs; byte I/O is big-endian. */
#ifndef BN_H
#define BN_H
#include <stdint.h>

#define BN_LIMBS 96                     /* 3072 bits */
#define BN_BYTES (BN_LIMBS * 4)

typedef struct { uint32_t l[BN_LIMBS]; } bn;

typedef struct {
	bn n;                               /* modulus (odd) */
	bn r2;                              /* R^2 mod n, R = 2^3072 */
	uint32_t n0;                        /* -n^-1 mod 2^32 */
} bn_mod;

void bn_from_bytes(bn *a, const uint8_t *b, int len);     /* big-endian, len <= BN_BYTES */
void bn_to_bytes(uint8_t out[BN_BYTES], const bn *a);     /* big-endian, padded to BN_BYTES */
int  bn_cmp(const bn *a, const bn *b);
int  bn_is_zero(const bn *a);

void bn_mod_init(bn_mod *m, const bn *n);
void bn_reduce(bn *r, const bn *a, const bn_mod *m);       /* r = a mod n (a < 2^3072) */
void bn_modmul(bn *r, const bn *a, const bn *b, const bn_mod *m);   /* r = a*b mod n; a, b < n */
void bn_modadd(bn *r, const bn *a, const bn *b, const bn_mod *m);   /* r = a+b mod n; a, b < n */
void bn_modexp(bn *r, const bn *base, const uint8_t *e, int elen, const bn_mod *m);   /* e big-endian */

#endif
