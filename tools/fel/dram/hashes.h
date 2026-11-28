/* SHA-1, SHA-256 and AES-128-CTR, one-shot, for AirPlay's MFi-SAP (auth-setup).
   Written for this project from FIPS 180-4 and FIPS 197. Host-testable. */
#ifndef HASHES_H
#define HASHES_H
#include <stdint.h>

void sha1(uint8_t out[20], const uint8_t *d, int n);
void sha256(uint8_t out[32], const uint8_t *d, int n);
/* AES-128 in counter mode (128-bit big-endian counter = iv), in place */
void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *buf, int n);

#endif
