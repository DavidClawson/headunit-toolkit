/*
 * ui.c: on-device drawing demo. Brings the display up if needed, draws a mock
 * home screen into the back buffer with gfx.c, flips to it at vblank, and
 * puts its own timings on screen.
 *
 *   felrun.py dram/ui.bin
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "home.h"


int main(void)
{
	log_reset();
	uint32_t t0 = ticks();
	int r = display_up(FB0);
	unsigned up_ms = elapsed_ms(t0);
	if (r) { logf("ui: display_up failed\n"); return 1; }

	uint32_t back = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(back);
	t0 = ticks();
	home_draw();
	unsigned draw_us = (ticks() - t0) / TICKS_PER_US;
	t0 = ticks();
	gfx_clear(0);
	unsigned clear_us = (ticks() - t0) / TICKS_PER_US;
	t0 = ticks();
	home_draw();                                       /* again, for the record */

	char s[128], *p = s;
	p = str_cat(p, "home screen drawn in "); p = str_u(p, draw_us / 1000);
	p = str_cat(p, " ms, full clear "); p = str_u(p, clear_us / 1000);
	p = str_cat(p, " ms, display up "); p = str_u(p, up_ms);
	p = str_cat(p, " ms. Buffer ");
	p = str_cat(p, back == FB0 ? "FB0" : "FB1");
	gfx_fill(0, GH - 56, GW, 56, 0x0b1222);
	gfx_text(32, GH - 40, s, &font8x16, 1, 0x8aa0c8);

	t0 = ticks();
	display_flip(back);
	unsigned flip_us = (ticks() - t0) / TICKS_PER_US;
	logf("ui: drew into %08x: home %u us, clear %u us, flip wait %u us, display up %u ms\n",
	     back, draw_us, clear_us, flip_us, up_ms);
	return 0;
}
