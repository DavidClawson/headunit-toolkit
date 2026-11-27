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

static char *fmt_u(char *p, unsigned v)            /* append decimal */
{
	char t[12]; int n = 0;
	do t[n++] = '0' + v % 10; while (v /= 10);
	while (n) *p++ = t[--n];
	*p = 0;
	return p;
}
static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static void tile(int x, int y, int w, int h, uint32_t c, const char *label, const char *sub)
{
	gfx_rrect(x + 6, y + 8, w, h, 28, 0x05070c);                  /* shadow */
	gfx_rrect(x, y, w, h, 28, c);
	gfx_rrect(x + 4, y + 4, w - 8, h / 2, 24, gfx_mix(c, 0xffffff, 40));  /* sheen */
	gfx_fill(x + 4, y + h / 2 - 20, w - 8, 24, c);
	int tw = gfx_text_w(label, &font16x32, 1);
	gfx_text(x + (w - tw) / 2, y + h - 92, label, &font16x32, 1, 0xffffff);
	tw = gfx_text_w(sub, &font8x16, 1);
	gfx_text(x + (w - tw) / 2, y + h - 48, sub, &font8x16, 1, gfx_mix(c, 0xffffff, 200));
}

static void draw_home(void)
{
	gfx_vgrad(0, 0, GW, GH, 0x1b2a4a, 0x070a12);
	/* status bar */
	gfx_fill(0, 0, GW, 64, 0x0b1222);
	gfx_fill(0, 64, GW, 2, 0x2a3f6a);
	gfx_text(32, 16, "HEADUNIT", &font16x32, 1, 0xffffff);
	gfx_text(32 + gfx_text_w("HEADUNIT ", &font16x32, 1), 24, "clean-sheet UI, drawn on the F133", &font8x16, 1, 0x8aa0c8);
	gfx_text(GW - 32 - gfx_text_w("12:00", &font16x32, 1), 16, "12:00", &font16x32, 1, 0xffffff);
	/* four tiles */
	static const struct { uint32_t c; const char *l, *s; } t[4] = {
		{ 0xd9480f, "Radio", "FM / AM" },
		{ 0x1971c2, "Bluetooth", "Phone & audio" },
		{ 0x2f9e44, "Media", "USB / SD" },
		{ 0x6741d9, "Settings", "Display & sound" },
	};
	int tw = 268, th = 300, gap = 32, x0 = (GW - 4 * tw - 3 * gap) / 2;
	for (int i = 0; i < 4; i++)
		tile(x0 + i * (tw + gap), 140, tw, th, t[i].c, t[i].l, t[i].s);
	/* font check, both sizes, all printable ASCII */
	char line[96];
	for (int i = 0; i < 95; i++) line[i] = (char)(32 + i);
	line[95] = 0;
	gfx_text(40, 500, line, &font8x16, 1, 0xc8d4ec);
	line[63] = 0;
	gfx_text(40, 530, line + 1, &font16x32, 1, 0xffffff);
}

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
	draw_home();
	unsigned draw_us = (ticks() - t0) / TICKS_PER_US;
	t0 = ticks();
	gfx_clear(0);
	unsigned clear_us = (ticks() - t0) / TICKS_PER_US;
	t0 = ticks();
	draw_home();                                       /* again, for the record */

	char s[128], *p = s;
	p = cat(p, "home screen drawn in "); p = fmt_u(p, draw_us / 1000);
	p = cat(p, " ms, full clear "); p = fmt_u(p, clear_us / 1000);
	p = cat(p, " ms, display up "); p = fmt_u(p, up_ms);
	p = cat(p, " ms. Buffer ");
	p = cat(p, back == FB0 ? "FB0" : "FB1");
	gfx_fill(0, GH - 56, GW, 56, 0x0b1222);
	gfx_text(32, GH - 40, s, &font8x16, 1, 0x8aa0c8);

	t0 = ticks();
	display_flip(back);
	unsigned flip_us = (ticks() - t0) / TICKS_PER_US;
	logf("ui: drew into %08x: home %u us, clear %u us, flip wait %u us, display up %u ms\n",
	     back, draw_us, clear_us, flip_us, up_ms);
	return 0;
}
