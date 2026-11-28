/* Host test: "R <hex>" parse and re-write (round trip); "B" write a built example. Hex out. */
#include <stdio.h>
#include "../bplist.h"
static int unhex(uint8_t *o, const char *s) { int n = 0; while (s[0] > ' ' && s[1] > ' ') { sscanf(s, "%2hhx", &o[n++]); s += 2; } return n; }
int main(void)
{
	static char a[4], d[200000]; static uint8_t in[100000], out[100000];
	while (scanf("%3s", a) == 1) {
		int r;
		bp_reset();
		if (a[0] == 'R') { scanf("%199999s", d); int n = unhex(in, d); int root = bp_parse(in, n); r = root < 0 ? -1 : bp_write(root, out, sizeof out); }
		else {
			static const uint8_t blob[5] = { 1, 2, 3, 0, 255 };
			int root = bp_new_dict(8), st = bp_new_array(2), s0 = bp_new_dict(4);
			bp_set(root, "eventPort", bp_new_int(51234));
			bp_set(root, "timingPort", bp_new_int(0));
			bp_set(root, "big", bp_new_int(5000000000LL));
			bp_set(root, "neg", bp_new_int(-7));
			bp_set(root, "ok", bp_new_bool(1));
			bp_set(root, "blob", bp_new_data(blob, 5));
			bp_set(root, "streams", st);
			bp_set(s0, "type", bp_new_int(110)); bp_set(s0, "dataPort", bp_new_int(7100)); bp_set(s0, "rate", bp_new_real(59.94));
			bp_push(st, s0);
			r = bp_write(root, out, sizeof out);
		}
		if (r < 0) printf("ERR\n"); else { for (int i = 0; i < r; i++) printf("%02x", out[i]); printf("\n"); }
		fflush(stdout);
	}
	return 0;
}
