/* Known-answer values for the device self-test (computed on the Mac). */
#include <stdio.h>
#include "../pair.h"
#include "../third_party/monocypher/monocypher-ed25519.h"
static void rng(uint8_t *o, int n) { (void)o; (void)n; }
int main(void)
{
	static pair_ctx p; static srp_server s; uint8_t seed[32], salt[16], b[32], h[64];
	for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
	pair_init(&p, seed, "x", "3939", rng);
	for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
	for (int i = 0; i < 32; i++) b[i] = (uint8_t)(i + 1);
	srp_start(&s, "Pair-Setup", "3939", salt, b);
	crypto_sha512(h, s.Bbytes, BN_BYTES);
	printf("ltpk[0..7] "); for (int i = 0; i < 8; i++) printf("0x%02x,", p.ltpk[i]);
	printf("\nH(B)[0..7] "); for (int i = 0; i < 8; i++) printf("0x%02x,", h[i]); printf("\n");
	return 0;
}
