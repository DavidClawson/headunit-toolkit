/*
 * settings_ui.c: the Settings page.
 *   Button colour: 9 swatches (the MCU's modes), applied at once.
 *   Screen brightness: - / + (and the knob), 10 steps.
 *   System: reboot to stock (tap twice), the watchdog freeze test, versions.
 * Everything lands in cfg, which settings.c saves.
 */
#include "lib.h"
#include "gfx.h"
#include "display.h"
#include "mcu.h"
#include "bt.h"
#include "tuner.h"
#include "fw.h"
#include "settings.h"
#include "settings_ui.h"

enum { S_LED0 = 1, S_LED8 = S_LED0 + 6, S_BRDN, S_BRUP, S_STOCK, S_FREEZE, S_CLICKS, S_END };
#define BTN 0x2b3a5c

/* solid colours only: "cycle" and "off" didn't behave reliably on the bench (2026-11-27) */
static const uint8_t led_mode[7] = { LED_WHITE, LED_RED, LED_YELLOW, LED_GREEN, LED_CYAN, LED_BLUE, LED_PURPLE };
static const uint32_t led_rgb[7] = { 0xf1f3f5, 0xe03131, 0xfcc419, 0x37b24d, 0x22b8cf, 0x1c7ed6, 0x9c36b5 };
static const char *const led_name[7] = { "White", "Red", "Yellow", "Green", "Cyan", "Blue", "Purple" };
static uint32_t confirm_until;

static void rect(int id, int *x, int *y, int *w, int *h)
{
	if (id >= S_LED0 && id <= S_LED8) { *x = 60 + (id - S_LED0) * 160; *y = 196; *w = 128; *h = 72; return; }
	*h = 80; *y = 360;
	if (id == S_BRDN) { *x = 60;  *w = 140; return; }
	if (id == S_BRUP) { *x = 760; *w = 140; return; }
	*y = 520; *w = 340;
	*x = id == S_STOCK ? 60 : id == S_FREEZE ? 430 : 800;
}

int settings_ui_hit(int x, int y)
{
	for (int id = 1; id < S_END; id++) {
		int bx, by, bw, bh;
		rect(id, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh) return id;
	}
	return -1;
}

static int bright(void) { return cfg.bright ? cfg.bright : 10; }

static void button(int id, const char *a, int pressed, uint32_t c)
{
	int x, y, w, h;
	rect(id, &x, &y, &w, &h);
	if (pressed) c = gfx_mix(c, 0xffffff, 90);
	gfx_rrect(x, y, w, h, 18, c);
	gfx_text(x + (w - gfx_text_w(a, &font16x32, 1)) / 2, y + (h - 32) / 2, a, &font16x32, 1, 0xffffff);
}

void settings_ui_draw(int pressed)
{
	gfx_text(260, 108, "Settings", &font16x32, 1, 0xffffff);
	gfx_text(60, 166, "Button colour", &font16x32, 1, 0xc8d4ec);
	for (int i = 0; i < 7; i++) {
		int x, y, w, h;
		rect(S_LED0 + i, &x, &y, &w, &h);
		if (cfg.led == led_mode[i]) gfx_rring(x - 7, y - 7, w + 14, h + 14, 22, 4, 0xffffff);
		gfx_rrect(x, y, w, h, 16, pressed == S_LED0 + i ? gfx_mix(led_rgb[i], 0xffffff, 90) : led_rgb[i]);
		gfx_text(x + (w - gfx_text_w(led_name[i], &font8x16, 1)) / 2, y + h + 12, led_name[i], &font8x16, 1, 0xc8d4ec);
	}
	gfx_text(60, 326, "Screen brightness   (knob turns it too)", &font16x32, 1, 0xc8d4ec);
	button(S_BRDN, "-", pressed == S_BRDN, BTN);
	button(S_BRUP, "+", pressed == S_BRUP, BTN);
	for (int i = 0; i < 10; i++)
		gfx_rrect(222 + i * 53, 374, 44, 52, 8, i < bright() ? 0xffd43b : 0x2a3f6a);

	gfx_text(60, 486, "System", &font16x32, 1, 0xc8d4ec);
	int arm = millis() < confirm_until;
	button(S_STOCK, arm ? "Tap again: reboot" : "Reboot to stock", pressed == S_STOCK, arm ? 0xc92a2a : BTN);
	button(S_FREEZE, "Freeze test", pressed == S_FREEZE, BTN);
	button(S_CLICKS, cfg.no_clicks ? "Touch sounds: off" : "Touch sounds: on", pressed == S_CLICKS,
	       cfg.no_clicks ? BTN : 0x2f6b3a);
	gfx_text(800, 606, "clicks when Bluetooth is idle", &font8x16, 1, 0x8aa0c8);

	char s[160], *p = str_cat(s, "Tuner: ");
	if (tuner.ok) { p = str_cat(p, "Si47"); p = str_u(p, tuner.part); *p++ = ' '; *p++ = 'f'; *p++ = 'w';
	                *p++ = ' '; *p++ = (char)tuner.fw_major; *p++ = '.'; *p++ = (char)tuner.fw_minor; *p = 0; }
	else p = str_cat(p, "not started");
	p = str_cat(p, "    Bluetooth: "); p = str_cat(p, bt.version[0] ? bt.version : "?");
	p = str_cat(p, "    Built: " __DATE__ " " __TIME__);
	gfx_text(60, 630, s, &font8x16, 1, 0x8aa0c8);
}

static void set_bright(int v)
{
	if (v < 1) v = 1;
	if (v > 10) v = 10;
	cfg.bright = (uint8_t)v;
	display_brightness(v);
}

int settings_ui_press(int id)
{
	if (id >= S_LED0 && id <= S_LED8) { cfg.led = led_mode[id - S_LED0]; mcu_key_led(cfg.led); }
	if (id == S_BRDN) set_bright(bright() - 1);
	if (id == S_BRUP) set_bright(bright() + 1);
	if (id == S_STOCK) {
		if (millis() < confirm_until) fw_boot_stock();
		confirm_until = millis() + 3000;
	}
	if (id == S_CLICKS) cfg.no_clicks = !cfg.no_clicks;
	if (id == S_FREEZE) {
		logf("app: freeze test\n");
		if (fw_booted()) for (;;) ;
	}
	return 1;
}

int settings_ui_knob(int dir) { set_bright(bright() + dir); return 1; }

int settings_ui_tick(void)
{
	static int was;
	int arm = millis() < confirm_until;
	if (arm == was) return 0;
	was = arm;
	return 1;
}
