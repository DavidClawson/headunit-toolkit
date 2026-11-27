/* gfx.c: see gfx.h. */
#include "gfx.h"

static uint32_t *fb;

void gfx_target(uint32_t addr) { fb = (uint32_t *)(uintptr_t)addr; }

/* A screen column sx is buffer row sx; screen rows y0..y1 run backwards along it. */
static inline uint32_t *col(int sx, int sy_last) { return fb + sx * GH + (GH - 1 - sy_last); }

void gfx_fill(int x, int y, int w, int h, uint32_t c)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > GW) w = GW - x;
	if (y + h > GH) h = GH - y;
	if (w <= 0 || h <= 0) return;
	for (int sx = x; sx < x + w; sx++) {
		uint32_t *p = col(sx, y + h - 1);
		for (int n = h; n; n--) *p++ = c;
	}
}

void gfx_clear(uint32_t c) { gfx_fill(0, 0, GW, GH, c); }

void gfx_rect(int x, int y, int w, int h, int t, uint32_t c)
{
	gfx_fill(x, y, w, t, c);
	gfx_fill(x, y + h - t, w, t, c);
	gfx_fill(x, y + t, t, h - 2 * t, c);
	gfx_fill(x + w - t, y + t, t, h - 2 * t, c);
}

static int isqrt(int v)
{
	int r = 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c)
{
	if (2 * r > w) r = w / 2;
	if (2 * r > h) r = h / 2;
	for (int i = 0; i < w; i++) {
		int j = i < w / 2 ? i : w - 1 - i;          /* distance from the nearer side */
		int inset = 0;
		if (j < r) {                                 /* pixel centre vs. corner circle, 2x units */
			int d2 = 2 * (r - j) - 1;
			inset = r - isqrt(4 * r * r - d2 * d2) / 2;
		}
		gfx_fill(x + i, y + inset, 1, h - 2 * inset, c);
	}
}

static int corner_inset(int j, int r)            /* rows to skip at column j of a radius-r corner */
{
	if (j >= r) return 0;
	int d2 = 2 * (r - j) - 1;
	return r - isqrt(4 * r * r - d2 * d2) / 2;
}

void gfx_rring(int x, int y, int w, int h, int r, int t, uint32_t c)
{
	if (2 * r > w) r = w / 2;
	if (2 * r > h) r = h / 2;
	int ri = r - t > 0 ? r - t : 0;
	for (int i = 0; i < w; i++) {
		int j = i < w / 2 ? i : w - 1 - i;
		int out = corner_inset(j, r);
		if (j < t) { gfx_fill(x + i, y + out, 1, h - 2 * out, c); continue; }
		int in = t + corner_inset(j - t, ri);       /* inner edge, inset by the thickness */
		gfx_fill(x + i, y + out, 1, in - out, c);
		gfx_fill(x + i, y + h - in, 1, in - out, c);
	}
}

uint32_t gfx_mix(uint32_t a, uint32_t b, unsigned t)
{
	uint32_t o = 0;
	for (int s = 0; s < 24; s += 8) {
		int ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
		o |= (uint32_t)((ca * (256 - (int)t) + cb * (int)t) >> 8) << s;
	}
	return o;
}

void gfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
	for (int j = 0; j < h; j++)
		gfx_fill(x, y + j, w, 1, gfx_mix(top, bottom, h > 1 ? (unsigned)(j * 256 / (h - 1)) : 0));
}

int gfx_text_w(const char *s, const font_t *f, int scale)
{
	int n = 0;
	while (s[n]) n++;
	return n * f->w * scale;
}

int gfx_text(int x, int y, const char *s, const font_t *f, int scale, uint32_t c)
{
	for (; *s; s++, x += f->w * scale) {
		unsigned ch = (unsigned char)*s;
		if (ch < 32 || ch > 126) ch = '?';
		const uint16_t *g = f->rows + (ch - 32) * f->h;
		for (int r = 0; r < f->h; r++) {
			uint16_t bits = g[r];
			for (int b = 0; b < f->w; b++)
				if (bits & (1u << (f->w - 1 - b))) {
					if (scale == 1) {
						int sx = x + b, sy = y + r;
						if ((unsigned)sx < GW && (unsigned)sy < GH) *col(sx, sy) = c;
					} else
						gfx_fill(x + b * scale, y + r * scale, scale, scale, c);
				}
		}
	}
	return x;
}
