/*
 * dsi4.c: display bring-up stage 4 (docs/display.md): framebuffer.
 * Brings up the whole pipeline (display.c) scanning FB0 and draws the raw,
 * unrotated orientation test into it (red = first pixel in memory, green =
 * end of the first line, blue = start of the last line, yellow = last pixel).
 * The pipeline keeps running after this returns; fbshow.py and later
 * programs draw into it.
 */
#include "lib.h"
#include "dsi.h"
#include "display.h"

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB0;
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			fb[y * HACT + x] = c;
}

static void draw_test(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB0;
	for (int y = 0; y < VACT; y++)                   /* background: grey ramp down the buffer */
		for (int x = 0; x < HACT; x++) {
			uint32_t g = (uint32_t)(y * 160 / VACT) + 16;
			fb[y * HACT + x] = (g << 16) | (g << 8) | g;
		}
	for (int i = 0; i < 6; i++) {                    /* white border, 6 px */
		fill(0, i, HACT, 1, 0xffffff); fill(0, VACT - 1 - i, HACT, 1, 0xffffff);
		fill(i, 0, 1, VACT, 0xffffff); fill(HACT - 1 - i, 0, 1, VACT, 0xffffff);
	}
	fill(20, 20, 160, 160, 0xff0000);                    /* first pixel corner: red */
	fill(HACT - 180, 20, 160, 160, 0x00ff00);            /* end of first line: green */
	fill(20, VACT - 180, 160, 160, 0x0000ff);            /* start of last line: blue */
	fill(HACT - 180, VACT - 180, 160, 160, 0xffff00);    /* last pixel corner: yellow */
	for (int i = 0; i < 8; i++)                          /* colour bars across the middle */
		fill(40 + i * 80, VACT / 2 - 100, 80, 200,
		     ((i & 4) ? 0xff0000 : 0) | ((i & 2) ? 0x00ff00 : 0) | ((i & 1) ? 0x0000ff : 0));
	dcache_flush();
}

int main(void)
{
	log_reset();
	draw_test();
	int r = display_up(FB0);
	logf("dsi4: display_up %s, scanning %08x\n", r ? "FAILED" : "ok", display_fb());
	return r;
}
