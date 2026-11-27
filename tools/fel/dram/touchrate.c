/*
 * touchrate.c: GT911 report-rate diagnostic. Dumps the config header
 * (0x8047-0x8056: resolution, touch count, INT mode, refresh rate), then polls
 * as fast as the bus allows for 10 s, with no drawing, and logs how often a
 * frame is ready and the intervals between frames.
 *
 *   felrun.py dram/touchrate.bin --timeout 40     (keep a finger on the screen, move it)
 */
#include "lib.h"
#include "twi.h"
#include "gt911.h"

int main(void)
{
	log_reset();
	char id[5]; uint16_t xm, ym, fw;
	int e = gt911_init(id, &xm, &ym, &fw);
	logf("gt911_init %d '%s' %ux%u fw %x\n", e, id, xm, ym, fw);
	if (e) return 1;
	uint8_t c[16];
	if (!twi_read(3, 0x5d, 0x8047, 2, c, 16)) {
		logf("config 8047:");
		for (int i = 0; i < 16; i++) logf(" %02x", c[i]);
		logf("\n  version %02x, out %ux%u, touch max %u, switch1 %02x (INT mode %u), switch2 %02x,"
		     " shake %02x, refresh %02x -> %u ms\n", c[0], c[1] | c[2] << 8, c[3] | c[4] << 8,
		     c[5] & 15, c[6], c[6] & 3, c[7], c[8], c[15], 5 + (c[15] & 15));
	}
	unsigned polls = 0, ready = 0, withpts = 0, errs = 0, hist[8] = { 0 };
	uint32_t t0 = ticks(), last = 0;
	while (ticks() - t0 < 10000000u * TICKS_PER_US) {
		uint8_t st;
		polls++;
		if (twi_read(3, 0x5d, 0x814E, 2, &st, 1)) { errs++; continue; }
		if (!(st & 0x80)) continue;
		uint32_t now = ticks();
		ready++;
		if (st & 15) withpts++;
		if (last) {
			unsigned ms = (now - last) / (1000 * TICKS_PER_US);
			hist[ms < 5 ? 0 : ms < 10 ? 1 : ms < 15 ? 2 : ms < 20 ? 3 : ms < 50 ? 4 : ms < 100 ? 5 : ms < 500 ? 6 : 7]++;
		}
		last = now;
		uint8_t z = 0;
		twi_write(3, 0x5d, 0x814E, 2, &z, 1);
	}
	logf("10 s: %u polls (%u us each), %u frames ready, %u with points, %u bus errors\n",
	     polls, 10000000u / (polls ? polls : 1), ready, withpts, errs);
	logf("intervals: <5ms %u, 5-10 %u, 10-15 %u, 15-20 %u, 20-50 %u, 50-100 %u, 100-500 %u, >500 %u\n",
	     hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
	return 0;
}
