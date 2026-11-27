/*
 * app.c: event-loop demo. One loop: input events (touch + front-panel keys)
 * change the state, and a changed state is redrawn into the back buffer and
 * flipped on vblank. Runs 60 s.
 *
 *   knob CW/CCW: move the focus ring    knob push: open the focused tile
 *   tap a tile: open it                 "< Back" (or knob push) on a page: return
 *   phone key: banner for 2 s
 *   Radio page: see radio_ui.c (knob tunes there)
 *   hold the status bar 3 s: reboot into stock firmware once (fw.c)
 *   phone key on the Settings page: deliberate freeze, to test that the
 *   watchdog (petted every loop pass when booted from NOR) restarts us
 *
 * Under FEL it runs 60 s and returns:   felrun.py dram/app.bin --timeout 120
 * As boot firmware (mkfw.py app.bin -> NOR 0x120000) it runs forever.
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "home.h"
#include "input.h"
#include "mcu.h"
#include "fw.h"
#include "radio_ui.h"

#ifndef SECONDS
#define SECONDS 60
#endif

static struct {
	int page;            /* -1 = home, else the open tile */
	int focus;           /* focused tile on home */
	int pressed;         /* tile (home) or 0 = back button (page) held down, -1 none */
	uint32_t banner_until;
	uint32_t hold_t0;    /* finger down on the status bar since (ms), 0 = no */
	int hold_left;       /* seconds left on the countdown shown */
	char status[96];
	uint32_t press_ms;   /* when the current press started */
} st = { -1, 0, -1, 0, 0, 0, "", 0 };

#define BAR_H 64
#define HOLD_MS 3000

static const uint32_t page_col[HOME_TILES] = { 0xd9480f, 0x1971c2, 0x2f9e44, 0x6741d9 };
#define BACK_X 24
#define BACK_Y 88
#define BACK_W 200
#define BACK_H 72

static uint32_t now_ms(void) { return millis(); }

static void status_bar(void)
{
	gfx_fill(0, 0, GW, BAR_H, st.hold_t0 ? 0x5c1a1a : 0x0b1222);
	gfx_text(32, 16, "HEADUNIT", &font16x32, 1, 0xffffff);
	if (st.hold_t0) {
		char s[48], *p = str_cat(s, "keep holding: stock firmware in ");
		p = str_u(p, (unsigned)st.hold_left); str_cat(p, " s");
		gfx_text(GW - 32 - gfx_text_w(s, &font16x32, 1), 16, s, &font16x32, 1, 0xffffff);
	} else
		gfx_text(GW - 32 - gfx_text_w(fw_booted() ? "NOR" : "FEL", &font16x32, 1), 16,
		         fw_booted() ? "NOR" : "FEL", &font16x32, 1, 0x8aa0c8);
}

static void draw(void)
{
	if (st.page < 0) {
		home_draw();
		if (st.pressed >= 0) home_tile(st.pressed, 1);
		int x, y, w, h;
		home_tile_rect(st.focus, &x, &y, &w, &h);
		gfx_rring(x - 12, y - 12, w + 24, h + 24, 40, 5, 0xffffff);
		status_bar();
	} else {
		uint32_t c = page_col[st.page];
		gfx_vgrad(0, 0, GW, GH, gfx_mix(c, 0x000000, 120), 0x070a12);
		status_bar();
		gfx_rrect(BACK_X, BACK_Y, BACK_W, BACK_H, 20, st.pressed == 0 ? gfx_mix(c, 0xffffff, 90) : c);
		gfx_text(BACK_X + 36, BACK_Y + 20, "< Back", &font16x32, 1, 0xffffff);
		if (st.page == 0) radio_draw(st.pressed);
		else {
			const char *n = home_tile_name(st.page);
			gfx_text((GW - gfx_text_w(n, &font16x32, 3)) / 2, 260, n, &font16x32, 3, 0xffffff);
			const char *m = st.page == 3 ? "phone key here = freeze test (watchdog restarts us)"
			                             : "nothing here yet: knob push or Back to return";
			gfx_text((GW - gfx_text_w(m, &font8x16, 1)) / 2, 400, m, &font8x16, 1, 0xc8d4ec);
		}
	}
	const char *note = st.page == 0 ? radio_note() : 0;
	if (now_ms() < st.banner_until || note) {
		gfx_rrect(340, 160, 600, 90, 30, 0x2b8a3e);
		const char *b = note ? note : "Phone key";
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

static void freeze_test(void)
{
	uint32_t fb = display_fb();
	gfx_target(fb);
	gfx_fill(0, GH / 2 - 60, GW, 120, 0x8a1010);
	const char *m = fw_booted() ? "Freeze test: the watchdog should restart us in ~8 s"
	                            : "Freeze test (under FEL: no watchdog, returning)";
	gfx_text((GW - gfx_text_w(m, &font16x32, 1)) / 2, GH / 2 - 16, m, &font16x32, 1, 0xffffff);
	logf("app: freeze test\n");
	if (!fw_booted()) return;
	for (;;) ;
}

/* returns 1 if the screen needs redrawing */
static int handle(const event *e)
{
	int home = st.page < 0;
	switch (e->type) {
	case EV_KEY:
		if (st.page == 0 && (e->key == MCU_KEY_CW || e->key == MCU_KEY_CCW))
			return radio_knob(e->key == MCU_KEY_CW ? 1 : -1);
		if (e->key == MCU_KEY_CW && home) st.focus = (st.focus + 1) % HOME_TILES;
		else if (e->key == MCU_KEY_CCW && home) st.focus = (st.focus + HOME_TILES - 1) % HOME_TILES;
		else if (e->key == MCU_KEY_KNOB) { st.page = home ? st.focus : -1; if (st.page == 0) radio_open(); }
		else if (e->key == MCU_KEY_PHONE && st.page == 3) freeze_test();
		else if (e->key == MCU_KEY_PHONE) st.banner_until = now_ms() + 2000;
		return 1;
	case EV_TOUCH_DOWN:
	case EV_TOUCH_MOVE: {
		int on_bar = e->y < BAR_H;
		if (on_bar && e->type == EV_TOUCH_DOWN) {
			st.hold_t0 = e->ms ? e->ms : 1; st.hold_left = HOLD_MS / 1000; st.pressed = -1;
			return 1;
		}
		if (st.hold_t0) {
			if (on_bar) return 0;
			st.hold_t0 = 0;                          /* slid off the bar: cancel */
			return 1;
		}
		int p = home ? home_hit(e->x, e->y) : in_back(e->x, e->y) ? 0 :
		        st.page == 0 ? radio_hit(e->x, e->y) : -1;
		if (e->type == EV_TOUCH_DOWN) st.press_ms = e->ms;
		if (p == st.pressed) return e->type == EV_TOUCH_DOWN;
		st.pressed = p;
		return 1;
	}
	case EV_TOUCH_UP:
		if (st.hold_t0) { st.hold_t0 = 0; return 1; }
		if (st.pressed >= 0) {
			if (home) { st.focus = st.pressed; st.page = st.pressed; if (st.page == 0) radio_open(); }
			else if (st.pressed == 0) st.page = -1;
			else if (st.page == 0) radio_press(st.pressed, e->ms - st.press_ms);
		}
		st.pressed = -1;
		return 1;
	}
	return 0;
}

int main(void)
{
	log_reset();
	int nor = fw_init();
	if (display_up(FB0)) { logf("app: display_up failed\n"); return 1; }
	int ti = input_init();
	fw_boot_ok();                                      /* up: the selector's watchdog can stand down */
	str_cat(st.status, ti ? "touch not answering; keys only" : "ready: touch a tile or turn the knob");
	logf("app: input_init %d, %s\n", ti, nor ? "booted from NOR" : "under FEL");

	uint32_t back = display_fb() == FB0 ? FB1 : FB0;
	int dirty = 1, banner = 0;
	unsigned events = 0, redraws = 0, worst = 0, logged = 0;
	uint32_t t0 = millis();
	while (nor || millis() - t0 < SECONDS * 1000u) {
		fw_pet();
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
		if (st.hold_t0) {
			uint32_t held = millis() - input_t0() - st.hold_t0;
			if (held >= HOLD_MS) {
				st.hold_t0 = 0;
				gfx_target(back);
				gfx_clear(0x000000);
				const char *m = "Rebooting into stock firmware...";
				gfx_text((GW - gfx_text_w(m, &font16x32, 1)) / 2, GH / 2 - 16, m, &font16x32, 1, 0xffffff);
				display_flip(back);
				logf("app: status bar held: rebooting to stock\n");
				udelay(300000);
				fw_boot_stock();
			}
			int left = (int)((HOLD_MS - held + 999) / 1000);
			if (left != st.hold_left) { st.hold_left = left; dirty = 1; }
		}
		dirty |= radio_tick() && st.page == 0;
		int b = now_ms() < st.banner_until || (st.page == 0 && radio_note());
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
