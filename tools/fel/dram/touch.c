/*
 * touch.c: touch demo. Home screen up, GT911 initialised, then for 30 s:
 * the tile under your finger lights up, each touch leaves a dot, and the
 * bottom bar shows screen and raw coordinates. Logs the first touches raw,
 * to check the mapping.
 *
 *   felrun.py dram/touch.bin --timeout 60
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "home.h"
#include "gt911.h"

#define SECONDS 30

static void bar(const char *s)
{
	gfx_fill(0, GH - 56, GW, 56, 0x0b1222);
	gfx_text(32, GH - 40, s, &font8x16, 1, 0x8aa0c8);
}

int main(void)
{
	log_reset();
	if (display_up(FB0)) { logf("touch: display_up failed\n"); return 1; }
	uint32_t back = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(back);
	home_draw();

	char id[5], s[160], *p;
	uint16_t xmax = 0, ymax = 0, fw = 0;
	int e = gt911_init(id, &xmax, &ymax, &fw);
	p = s;
	if (e) {
		p = str_cat(p, "GT911 not answering (TWI error "); p = str_u(p, (unsigned)e); str_cat(p, ")");
	} else {
		p = str_cat(p, "GT911 '"); p = str_cat(p, id); p = str_cat(p, "' ");
		p = str_u(p, xmax); p = str_cat(p, "x"); p = str_u(p, ymax);
		str_cat(p, ": touch the screen (30 s)");
	}
	bar(s);
	display_flip(back);
	logf("touch: gt911_init %d id '%s' fw %x res %ux%u\n", e, e ? "" : id, fw, xmax, ymax);
	if (e || !xmax || !ymax) return 1;

	static const uint32_t dot[5] = { 0xffffff, 0xffd43b, 0x63e6be, 0xff8787, 0x74c0fc };
	int pressed = -1, logged = 0;
	unsigned frames = 0, touches = 0, errors = 0;
	uint32_t t0 = ticks();
	while (ticks() - t0 < SECONDS * 1000000u * TICKS_PER_US) {
		touch_pt pt[5];
		int n = gt911_poll(pt, 5);
		if (n == -1) { udelay(5000); continue; }
		if (n < -1) { errors++; udelay(20000); continue; }
		frames++;
		int hit = -1;
		for (int i = 0; i < n; i++) {
			int sx = pt[i].x * GW / xmax, sy = pt[i].y * GH / ymax;
			if (i == 0) {
				hit = home_hit(sx, sy);
				touches++;
				if (logged < 12) {
					logf("  raw %4u,%4u -> screen %4d,%3d  tile %d\n", pt[i].x, pt[i].y, sx, sy, hit);
					logged++;
				}
				p = s;
				p = str_cat(p, "screen "); p = str_u(p, (unsigned)sx); p = str_cat(p, ",");
				p = str_u(p, (unsigned)sy); p = str_cat(p, "   raw "); p = str_u(p, pt[i].x);
				p = str_cat(p, ","); p = str_u(p, pt[i].y); p = str_cat(p, "   ");
				str_cat(p, hit >= 0 ? home_tile_name(hit) : "-");
				bar(s);
			}
			if (hit != pressed) {
				if (pressed >= 0) home_tile(pressed, 0);
				if (hit >= 0) home_tile(hit, 1);
				pressed = hit;
			}
			gfx_rrect(sx - 5, sy - 5, 11, 11, 5, dot[pt[i].id % 5]);
		}
		if (n == 0 && pressed >= 0) { home_tile(pressed, 0); pressed = -1; }
	}
	if (pressed >= 0) home_tile(pressed, 0);
	p = s; p = str_cat(p, "touch demo ended: "); p = str_u(p, touches);
	str_cat(p, " touch reports");
	bar(s);
	logf("touch: %u frames, %u touch reports, %u bus errors\n", frames, touches, errors);
	return 0;
}
