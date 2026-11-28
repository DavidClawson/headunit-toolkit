/* Host test: lines "S <hex>" (pair-setup) / "V <hex>" (pair-verify) on stdin, hex replies on stdout.
   argv[1] = PIN. First line printed: our long-term public key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pair.h"
static void rng(uint8_t *o, int n) { FILE *f = fopen("/dev/urandom", "rb"); fread(o, 1, (size_t)n, f); fclose(f); }
int main(int argc, char **argv)
{
	static pair_ctx p; static char line[8192]; static uint8_t in[4096], out[4096];
	uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
	pair_init(&p, seed, "02:f1:33:00:00:02", argc > 1 ? argv[1] : "3939", rng);
	for (int i = 0; i < 32; i++) printf("%02x", p.ltpk[i]); printf("\n"); fflush(stdout);
	while (fgets(line, sizeof line, stdin)) {
		int n = 0; for (char *s = line + 2; s[0] > ' ' && s[1] > ' '; s += 2) sscanf(s, "%2hhx", &in[n++]);
		int r = line[0] == 'S' ? pair_setup(&p, in, n, out, sizeof out) : pair_verify(&p, in, n, out, sizeof out);
		if (r < 0) { printf("ERR %d\n", p.last_err); fflush(stdout); continue; }
		for (int i = 0; i < r; i++) printf("%02x", out[i]); printf(" %d %d\n", p.last_err, p.verified); fflush(stdout);
	}
	return 0;
}
