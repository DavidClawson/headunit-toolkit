/* Host test: "1 <hex>" sha1, "2 <hex>" sha256, "A <key> <iv> <hex>" aes128_ctr. Hex out. */
#include <stdio.h>
#include "../hashes.h"
static int unhex(uint8_t *o, const char *s) { int n = 0; while (s[0] > ' ' && s[1] > ' ') { sscanf(s, "%2hhx", &o[n++]); s += 2; } return n; }
int main(void)
{
	static char a[8], b[64], c[64], d[4096]; static uint8_t k[16], iv[16], buf[2048], h[32];
	while (scanf("%7s", a) == 1) {
		if (a[0] == 'A') { scanf("%63s %63s %4095s", b, c, d); unhex(k, b); unhex(iv, c); int n = unhex(buf, d[0] == '-' ? "" : d); aes128_ctr(k, iv, buf, n); for (int i = 0; i < n; i++) printf("%02x", buf[i]); }
		else { scanf("%4095s", d); int n = unhex(buf, d[0] == '-' ? "" : d); if (a[0] == '1') { sha1(h, buf, n); n = 20; } else { sha256(h, buf, n); n = 32; } for (int i = 0; i < n; i++) printf("%02x", h[i]); }
		printf("\n"); fflush(stdout);
	}
	return 0;
}
