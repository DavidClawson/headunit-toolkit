/* Host test for srp.c: "B <salt> <b>" prints B; "F <salt> <b> <A> <M1>" prints K and M2. Hex args. */
#include <stdio.h>
#include <string.h>
#include "../srp.h"
static int unhex(uint8_t *o, const char *s) { int n = 0; while (s[0] && s[1]) { sscanf(s, "%2hhx", &o[n++]); s += 2; } return n; }
static void hex(const uint8_t *p, int n) { for (int i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }
int main(int argc, char **argv)
{
	static srp_server s; uint8_t salt[16], b[32], A[400], M1[64];
	unhex(salt, argv[2]); unhex(b, argv[3]);
	srp_start(&s, "Pair-Setup", "3939", salt, b);
	if (argv[1][0] == 'B') { hex(s.Bbytes, BN_BYTES); return 0; }
	int al = unhex(A, argv[4]); unhex(M1, argv[5]);
	int r = srp_finish(&s, A, al, M1);
	printf("%d\n", r); hex(s.K, 64); hex(s.M2, 64);
	return 0;
}
