/* SRP-6a server, 3072-bit group (RFC 5054) with SHA-512, as used by AirPlay/HomeKit
   pair-setup. Written for this project; see docs/carplay-pairing.md. */
#ifndef SRP_H
#define SRP_H
#include <stdint.h>
#include "bn.h"

typedef struct {
	bn_mod m;
	bn v, b, B;
	uint8_t salt[16];
	uint8_t Bbytes[BN_BYTES];          /* B, padded to 384 bytes */
	uint8_t K[64];                     /* session key, after srp_finish */
	uint8_t M2[64];                    /* server proof, after srp_finish */
	const char *user;
} srp_server;

/* salt: 16 random bytes; b: 32 random bytes (server secret exponent) */
void srp_start(srp_server *s, const char *user, const char *pass,
               const uint8_t salt[16], const uint8_t b[32]);
/* A: client public value; M1: client proof. Returns 0 if M1 is right (then K and M2 are set). */
int srp_finish(srp_server *s, const uint8_t *A, int alen, const uint8_t M1[64]);

#endif
