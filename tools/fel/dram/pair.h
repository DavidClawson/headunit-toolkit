/* AirPlay/CarPlay pairing, accessory side: pair-setup (SRP-6a, PIN) and pair-verify
   (X25519), HomeKit-style TLV8 messages. Written for this project from the public
   protocol description in docs/carplay-pairing.md. Host-testable: no hardware access. */
#ifndef PAIR_H
#define PAIR_H
#include <stdint.h>
#include "srp.h"

typedef void (*pair_rng)(uint8_t *out, int n);

typedef struct {
	pair_rng rng;
	const char *id;                    /* our pairing identifier */
	const char *pin;                   /* setup code ("3939" for CarPlay) */
	uint8_t ltsk[64], ltpk[32];        /* our long-term Ed25519 key pair */
	/* pair-setup */
	int ps_state;
	srp_server srp;
	/* the controller (phone) that paired with us */
	int have_ctrl;
	uint8_t ctrl_id[64]; int ctrl_id_len;
	uint8_t ctrl_ltpk[32];
	/* pair-verify */
	int pv_state;
	uint8_t pv_sec[32], pv_pub[32], pv_peer[32], pv_shared[32], pv_key[32];
	int verified;                      /* pair-verify finished: pv_shared is the session secret */
	int last_err;                      /* where it failed, for the screen */
} pair_ctx;

/* seed: 32 bytes the long-term key is derived from (fixed per unit) */
void pair_init(pair_ctx *p, const uint8_t seed[32], const char *id, const char *pin, pair_rng rng);
/* One request body in, one response body out. Returns the response length, or -1. */
int pair_setup(pair_ctx *p, const uint8_t *in, int n, uint8_t *out, int cap);
int pair_verify(pair_ctx *p, const uint8_t *in, int n, uint8_t *out, int cap);

/* TLV8 helpers (also used by tests) */
int tlv_get(const uint8_t *in, int n, uint8_t type, uint8_t *val, int cap);   /* merged fragments; -1 if absent */
int tlv_put(uint8_t *out, int pos, int cap, uint8_t type, const uint8_t *val, int len);

#endif
