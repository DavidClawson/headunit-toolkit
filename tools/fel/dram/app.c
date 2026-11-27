/*
 * app.c: event-loop demo. One loop: input events (touch + front-panel keys)
 * change the state, and a changed state is redrawn into the back buffer and
 * flipped on vblank. Runs 60 s.
 *
 *   knob CW/CCW: move the focus ring    knob push: open the focused tile
 *   tap a tile: open it                 "< Back" (or knob push) on a page: return
 *   phone key: banner for 2 s
 *
 *   felrun.py dram/app.bin --timeout 120
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "home.h"
#include "input.h"
#include "mcu.h"

#define SECONDS 60

static struct {
	int page;            /* -1 = home, else the open tile */
	int focus;           /* focused tile on home */
	int pressed;         /* tile (home) or 0 = back button (page) held down, -1 none */
	uint32_t banner_until;
	char status[96];
} st = { -1, 0, -1, 0, "" };

static const uint32_t page_col[HOME_TILES] = { 0xd9480f, 0x1971c2, 0x2f9e44, 0x6741d9 };
#define BACK_X 24
#define BACK_Y 88
#define BACK_W 200
#define BACK_H 72

static uint32_t now_ms(void) { return ticks() / (1000 * TICKS_PER_US); }

static void draw(void)
{
	if (st.page < 0) {
		home_draw();
		if (st.pressed >= 0) home_tile(st.pressed, 1);
		int x, y, w, h;
		home_tile_rect(st.focus, &x, &y, &w, &h);
		gfx_rring(x - 12, y - 12, w + 24, h + 24, 40, 5, 0xffffff);
	} else {
		uint32_t c = page_col[st.page];
		gfx_vgrad(0, 0, GW, GH, gfx_mix(c, 0x000000, 120), 0x070a12);
		gfx_fill(0, 0, GW, 64, 0x0b1222);
		gfx_text(32, 16, "HEADUNIT", &font16x32, 1, 0xffffff);
		gfx_rrect(BACK_X, BACK_Y, BACK_W, BACK_H, 20, st.pressed == 0 ? gfx_mix(c, 0xffffff, 90) : c);
		gfx_text(BACK_X + 36, BACK_Y + 20, "< Back", &font16x32, 1, 0xffffff);
		const char *n = home_tile_name(st.page);
		gfx_text((GW - gfx_text_w(n, &font16x32, 3)) / 2, 260, n, &font16x32, 3, 0xffffff);
		const char *m = "nothing here yet: knob push or Back to return";
		gfx_text((GW - gfx_text_w(m, &font8x16, 1)) / 2, 400, m, &font8x16, 1, 0xc8d4ec);
	}
	if (now_ms() < st.banner_until) {
		gfx_rrect(340, 560, 600, 90, 30, 0x2b8a3e);
		const char *b = "Phone key";
		gfx_text((GW - gfx_text_w(b, &font16x32, 1)) / 2, 589, b, &font16x32, 1, 0xffffff);
	}
	gfx_fill(0, GH - 40, GW, 40, 0x0b1222);
	gfx_text(24, GH - 28, st.status, &font8x16, 1, 0x8aa0c8);
}

static int in_back(int x, int y)
{
	return x >= BACK_X && x < BACK_X + BACK_W && y >= BACK_Y && y < BACK_Y + BACK_H;
}

static void describe(const event *e)
{
	static const char *names[] = { "", "touch down", "touch move", "touch up", "key" };
	char *p = str_cat(st.status, "last event: ");
	p = str_cat(p, names[e->type]);
	if (e->type == EV_KEY) {
		p = str_cat(p, e->key == MCU_KEY_CW ? " knob CW" : e->key == MCU_KEY_CCW ? " knob CCW" :
		               e->key == MCU_KEY_KNOB ? " knob push" : e->key == MCU_KEY_PHONE ? " phone" : " ?");
	} else {
		p = str_cat(p, " "); p = str_u(p, (unsigned)e->x); p = str_cat(p, ","); p = str_u(p, (unsigned)e->y);
	}
	p = str_cat(p, "   at "); p = str_u(p, e->ms); str_cat(p, " ms");
}

/* returns 1 if the screen needs redrawing */
static int handle(const event *e)
{
	int home = st.page < 0;
	switch (e->type) {
	case EV_KEY:
		if (e->key == MCU_KEY_CW && home) st.focus = (st.focus + 1) % HOME_TILES;
		else if (e->key == MCU_KEY_CCW && home) st.focus = (st.focus + HOME_TILES - 1) % HOME_TILES;
		else if (e->key == MCU_KEY_KNOB) st.page = home ? st.focus : -1;
		else if (e->key == MCU_KEY_PHONE) st.banner_until = now_ms() + 2000;
		return 1;
	case EV_TOUCH_DOWN:
	case EV_TOUCH_MOVE: {
		int p = home ? home_hit(e->x, e->y) : (in_back(e->x, e->y) ? 0 : -1);
		if (p == st.pressed) return e->type == EV_TOUCH_DOWN;
		st.pressed = p;
		return 1;
	}
	case EV_TOUCH_UP:
		if (st.pressed >= 0) {
			if (home) { st.focus = st.pressed; st.page = st.pressed; }
			else st.page = -1;
		}
		st.pressed = -1;
		return 1;
	}
	return 0;
}

int main(void)
{
	log_reset();
	if (display_up(FB0)) { logf("app: display_up failed\n"); return 1; }
	int ti = input_init();
	str_cat(st.status, ti ? "touch not answering; keys only" : "ready: touch a tile or turn the knob");
	logf("app: input_init %d\n", ti);

	uint32_t back = display_fb() == FB0 ? FB1 : FB0;
	int dirty = 1, banner = 0;
	unsigned events = 0, redraws = 0, worst = 0, logged = 0;
	uint32_t t0 = ticks();
	while (ticks() - t0 < SECONDS * 1000000u * TICKS_PER_US) {
		event e;
		while (input_next(&e)) {
			events++;
			if (e.type != EV_TOUCH_MOVE && logged < 40) {
				logf("  %5u ms  type %u key %02x  %d,%d\n", e.ms, e.type, e.key, e.x, e.y);
				logged++;
			}
			if (e.type != EV_TOUCH_MOVE) describe(&e);
			dirty |= handle(&e);
		}
		int b = now_ms() < st.banner_until;
		if (b != banner) { banner = b; dirty = 1; }
		if (!dirty) { udelay(1000); continue; }
		uint32_t a = ticks();
		gfx_target(back);
		draw();
		display_flip(back);
		back = back == FB0 ? FB1 : FB0;
		unsigned ms = (ticks() - a) / (1000 * TICKS_PER_US);
		if (ms > worst) worst = ms;
		redraws++;
		dirty = 0;
	}
	logf("app: %u events, %u redraws (slowest %u ms incl. vblank wait), %u bad MCU frames\n",
	     events, redraws, worst, mcu_bad_frames);
	return 0;
}
