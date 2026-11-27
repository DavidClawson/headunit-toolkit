/* home.c: the mock home screen (status bar, four tiles), shared by ui.c and touch.c. */
#include "gfx.h"
#include "home.h"

static const struct { uint32_t c; const char *l, *s; } tiles[HOME_TILES] = {
	{ 0xd9480f, "Radio", "FM / AM" },
	{ 0x1971c2, "Bluetooth", "Phone & audio" },
	{ 0x2f9e44, "Media", "USB / SD" },
	{ 0x6741d9, "Settings", "Display & sound" },
};

void home_tile_rect(int i, int *x, int *y, int *w, int *h)
{
	int tw = 268, th = 300, gap = 32, x0 = (GW - 4 * tw - 3 * gap) / 2;
	*x = x0 + i * (tw + gap); *y = 140; *w = tw; *h = th;
}

int home_hit(int sx, int sy)
{
	for (int i = 0; i < HOME_TILES; i++) {
		int x, y, w, h;
		home_tile_rect(i, &x, &y, &w, &h);
		if (sx >= x && sx < x + w && sy >= y && sy < y + h) return i;
	}
	return -1;
}

const char *home_tile_name(int i) { return tiles[i].l; }

static void tile(int x, int y, int w, int h, uint32_t c, const char *label, const char *sub, int pressed)
{
	if (pressed) c = gfx_mix(c, 0xffffff, 90);
	gfx_rrect(x + 6, y + 8, w, h, 28, 0x05070c);                  /* shadow */
	gfx_rrect(x, y, w, h, 28, c);
	gfx_rrect(x + 4, y + 4, w - 8, h / 2, 24, gfx_mix(c, 0xffffff, 40));  /* sheen */
	gfx_fill(x + 4, y + h / 2 - 20, w - 8, 24, c);
	int tw = gfx_text_w(label, &font16x32, 1);
	gfx_text(x + (w - tw) / 2, y + h - 92, label, &font16x32, 1, 0xffffff);
	tw = gfx_text_w(sub, &font8x16, 1);
	gfx_text(x + (w - tw) / 2, y + h - 48, sub, &font8x16, 1, gfx_mix(c, 0xffffff, 200));
}

void home_draw(void)
{
	gfx_vgrad(0, 0, GW, GH, 0x1b2a4a, 0x070a12);
	/* status bar */
	gfx_fill(0, 0, GW, 64, 0x0b1222);
	gfx_fill(0, 64, GW, 2, 0x2a3f6a);
	gfx_text(32, 16, "HEADUNIT", &font16x32, 1, 0xffffff);
	gfx_text(32 + gfx_text_w("HEADUNIT ", &font16x32, 1), 24, "clean-sheet UI, drawn on the F133", &font8x16, 1, 0x8aa0c8);
	gfx_text(GW - 32 - gfx_text_w("12:00", &font16x32, 1), 16, "12:00", &font16x32, 1, 0xffffff);
	/* four tiles */
	for (int i = 0; i < HOME_TILES; i++)
		home_tile(i, 0);
	/* font check, both sizes, all printable ASCII */
	char line[96];
	for (int i = 0; i < 95; i++) line[i] = (char)(32 + i);
	line[95] = 0;
	gfx_text(40, 500, line, &font8x16, 1, 0xc8d4ec);
	line[63] = 0;
	gfx_text(40, 530, line + 1, &font16x32, 1, 0xffffff);
}


void home_tile(int i, int pressed)
{
	int x, y, w, h;
	home_tile_rect(i, &x, &y, &w, &h);
	/* repaint the tile's area (background gradient + shadow margin) first */
	for (int j = 0; j < h + 8; j++)
		gfx_fill(x, y + j, w + 6, 1, gfx_mix(0x1b2a4a, 0x070a12, (unsigned)((y + j) * 256 / (GH - 1))));
	tile(x, y, w, h, tiles[i].c, tiles[i].l, tiles[i].s, pressed);
}
