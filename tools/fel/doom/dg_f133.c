/*
 * dg_f133.c: doomgeneric on the head unit (bare metal, over FEL).
 *   Screen: Doom's 320x200 scaled 3x to 960x600, centred on the 1280x720 screen.
 *   Controls (one touch point, the knob and the phone key):
 *     touch in the game area = a virtual joystick from its centre: up/down walk, left/right turn
 *     left margin = Use (doors, switches)      right margin = Fire
 *     top strip = Esc (menu)                   bottom strip = Enter
 *     knob = turn (in menus: up/down)          knob push = Fire (in menus: Enter)
 *     phone key = Esc
 *   Doom's Quit returns to FEL (exit() -> setjmp in main).
 */
#include <stdint.h>
#include <string.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "mcu.h"
#include "tuner.h"
#include "audio.h"
#include "libc_f133.h"
#include "settings.h"

settings_t cfg;                            /* tuner.c mirrors volume into it; nothing is saved here */

extern int menuactive;                     /* m_menu.c (boolean) */
extern int messageToPrint, messageNeedsInput;   /* m_menu.c: a yes/no question is up */

#define SCALE 3
#define OX ((GW - 320 * SCALE) / 2)        /* 160 */
#define OY ((GH - 200 * SCALE) / 2)        /* 60 */

static uint32_t fb_back;

/* ---- key queue ---- */
static struct { uint8_t pressed, key; } q[64];
static unsigned qh, qt;
static void push(int pressed, uint8_t key)
{
	if (qt - qh < 64) { q[qt % 64].pressed = (uint8_t)pressed; q[qt % 64].key = key; qt++; }
}

/* held-key sets: touch-driven keys are diffed against what's down */
enum { H_UP, H_DOWN, H_LEFT, H_RIGHT, H_USE, H_FIRE, H_N };
static const uint8_t hkey[H_N] = { KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_USE, KEY_FIRE };
static uint8_t held[H_N];
static void set_held(const uint8_t want[H_N])
{
	for (int i = 0; i < H_N; i++)
		if (want[i] != held[i]) { push(want[i], hkey[i]); held[i] = want[i]; }
}

/* knob pulses: press now, release a little later */
static struct { uint8_t key; uint32_t at; } pend[8];
static void pulse(uint8_t key)
{
	push(1, key);
	for (int i = 0; i < 8; i++) if (!pend[i].key) { pend[i].key = key; pend[i].at = millis() + 70; return; }
	push(0, key);
}

static int asking(void) { return messageToPrint && messageNeedsInput; }

static void touch(int down, int x, int y)
{
	uint8_t want[H_N] = { 0 };
	if (down) {
		if (x < OX) want[H_USE] = 1;
		else if (x >= OX + 320 * SCALE) want[H_FIRE] = 1;
		else if (y < OY) { if (!held[H_UP]) pulse(KEY_ESCAPE); }
		else if (y >= OY + 200 * SCALE) { if (!held[H_UP]) pulse(asking() ? 'y' : KEY_ENTER); }
		else {
			int dx = x - (OX + 160 * SCALE), dy = y - (OY + 100 * SCALE);
			if (dy < -70) want[H_UP] = 1;
			if (dy > 70) want[H_DOWN] = 1;
			if (dx < -90) want[H_LEFT] = 1;
			if (dx > 90) want[H_RIGHT] = 1;
		}
	}
	set_held(want);
}

static void poll_input(void)
{
	event e;
	while (input_next(&e)) {
		if (e.type == EV_TOUCH_DOWN || e.type == EV_TOUCH_MOVE) touch(1, e.x, e.y);
		else if (e.type == EV_TOUCH_UP) touch(0, 0, 0);
		else if (e.type == EV_KEY) {
			if (e.key == MCU_KEY_CW) pulse(menuactive ? KEY_DOWNARROW : KEY_RIGHTARROW);
			else if (e.key == MCU_KEY_CCW) pulse(menuactive ? KEY_UPARROW : KEY_LEFTARROW);
			else if (e.key == MCU_KEY_KNOB) pulse(asking() ? 'y' : menuactive ? KEY_ENTER : KEY_FIRE);
			else if (e.key == MCU_KEY_PHONE) pulse(KEY_ESCAPE);
		}
	}
	uint32_t now = millis();
	for (int i = 0; i < 8; i++)
		if (pend[i].key && (int32_t)(now - pend[i].at) >= 0) { push(0, pend[i].key); pend[i].key = 0; }
}

/* ---- doomgeneric hooks ---- */
static void frame_chrome(uint32_t fb)
{
	gfx_target(fb);
	gfx_clear(0x101418);
	gfx_fill(0, OY, OX, 200 * SCALE, 0x1c2a1c);                         /* Use */
	gfx_fill(OX + 320 * SCALE, OY, OX, 200 * SCALE, 0x3a1c1c);          /* Fire */
	gfx_text(OX / 2 - gfx_text_w("USE", &font16x32, 1) / 2, GH / 2 - 16, "USE", &font16x32, 1, 0x9fd49f);
	gfx_text(OX + 320 * SCALE + OX / 2 - gfx_text_w("FIRE", &font16x32, 1) / 2, GH / 2 - 16, "FIRE", &font16x32, 1, 0xe09a9a);
	gfx_text(GW / 2 - gfx_text_w("MENU", &font8x16, 1) / 2, OY / 2 - 8, "MENU", &font8x16, 1, 0x8aa0c8);
	gfx_text(GW / 2 - gfx_text_w("ENTER", &font8x16, 1) / 2, GH - OY / 2 - 8, "ENTER", &font8x16, 1, 0x8aa0c8);
}

void DG_Init(void)
{
	frame_chrome(FB0);
	frame_chrome(FB1);
	dcache_flush();
	display_flip(FB0);
	fb_back = FB1;
}

void DG_DrawFrame(void)
{
	uint32_t *fb = (uint32_t *)(uintptr_t)fb_back;
	const uint32_t *src = DG_ScreenBuffer;
	/* screen (sx, sy) lives at fb[sx * GH + (GH - 1 - sy)]: write each screen column */
	for (int x = 0; x < 320; x++) {
		/* this pixel's 3 screen columns; each runs downwards in memory from screen row OY */
		uint32_t *c0 = fb + (OX + x * SCALE) * GH + (GH - 1 - OY), *c1 = c0 + GH, *c2 = c1 + GH;
		const uint32_t *sp = src + x;
		for (int y = 0; y < 200; y++, sp += 320) {
			uint32_t p = *sp;
			c0[0] = p; c0[-1] = p; c0[-2] = p; c0 -= 3;
			c1[0] = p; c1[-1] = p; c1[-2] = p; c1 -= 3;
			c2[0] = p; c2[-1] = p; c2[-2] = p; c2 -= 3;
		}
	}
	dcache_flush();
	display_flip(fb_back);
	fb_back = fb_back == FB0 ? FB1 : FB0;
	audio_service();
	poll_input();
	static uint32_t frames, t0;
	if (!t0) t0 = millis();
	if (++frames == 200) {
		uint32_t dt = millis() - t0;
		logf("doom: %u.%u fps\n", 200000 / dt, (2000000 / dt) % 10);
		frames = 0; t0 = millis();
	}
}

void DG_SleepMs(uint32_t ms)
{
	uint32_t t0 = millis();
	do { audio_service(); poll_input(); } while (millis() - t0 < ms);
}

uint32_t DG_GetTicksMs(void) { return millis(); }

int DG_GetKey(int *pressed, unsigned char *key)
{
	poll_input();
	if (qh == qt) return 0;
	*pressed = q[qh % 64].pressed;
	*key = q[qh % 64].key;
	qh++;
	return 1;
}

void DG_SetWindowTitle(const char *t) { (void)t; }

int main(void)
{
	log_reset();
	/* the D-cache is off under FEL (mhcr 0x109): invalidate it, then turn on D-cache,
	   write-allocate, return stack, branch prediction, BTB (xboot uses 0x11ff on D1) */
	logf("doom: starting; mhcr was %x\n", mhcr());
	__asm__ volatile(".long 0x0020000b\n\tli t0, 0x11ff\n\tcsrw 0x7c1, t0\n\tfence" ::: "t0", "memory");
	logf("doom: mhcr now %x\n", mhcr());
	if (display_up(FB0)) { logf("doom: display_up failed\n"); return 1; }
	display_brightness(10);
	input_init();
	sound_init();
	snd_vol = 20; snd_muted = 0;
	sound_input(SOUND_IN_F133);
	audio_init();
	static char *argv[] = { "doom", "-iwad", "doom1.wad", 0 };
	if (__builtin_setjmp(libc_exit_jmp) == 0) {
		doomgeneric_Create(3, argv);
		logf("doom: running; heap used %lu KB\n", (unsigned long)(libc_heap_used() >> 10));
		for (;;) doomgeneric_Tick();
	}
	logf("doom: back to FEL (exit %d)\n", libc_exit_code);
	return 0;
}
