/*
 * bt_ui.c: Bluetooth "Now Playing".
 *   title / artist / album, progress bar, |<<  Play/Pause  >>|
 *   knob turn: volume; knob push: play / pause; Back leaves (music keeps playing)
 */
#include "lib.h"
#include "gfx.h"
#include "bt.h"
#include "tuner.h"
#include "bt_ui.h"

enum { B_PREV = 1, B_PLAY, B_NEXT, B_VOLDN, B_VOLUP, B_MUTE, B_END };
#define ACCENT 0x1971c2
#define BTN 0x2b3a5c

static unsigned shown_s = ~0u;

static void rect(int id, int *x, int *y, int *w, int *h)
{
	*y = 420; *h = 110; *w = 200;
	if (id <= B_NEXT) { *x = 300 + (id - B_PREV) * 240; return; }
	*y = 570; *h = 90;
	if (id == B_VOLDN) { *x = 50;   *w = 180; }
	if (id == B_VOLUP) { *x = 870;  *w = 180; }
	if (id == B_MUTE)  { *x = 1070; *w = 160; }
}

int bt_ui_hit(int x, int y)
{
	for (int id = 1; id < B_END; id++) {
		int bx, by, bw, bh;
		rect(id, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh) return id;
	}
	return -1;
}

void bt_ui_open(void) { sound_input(SOUND_IN_BT); shown_s = ~0u; }

static void button(int id, const char *a, int pressed, uint32_t c)
{
	int x, y, w, h;
	rect(id, &x, &y, &w, &h);
	if (pressed) c = gfx_mix(c, 0xffffff, 90);
	gfx_rrect(x, y, w, h, 18, c);
	gfx_text(x + (w - gfx_text_w(a, &font16x32, 1)) / 2, y + (h - 32) / 2, a, &font16x32, 1, 0xffffff);
}

static unsigned now_pos(void)
{
	unsigned p = bt.pos;
	if (bt.playing && bt.pos_ms) p += (millis() - bt.pos_ms) / 1000;
	return bt.len && p > bt.len ? bt.len : p;
}

static char *fmt_t(char *p, unsigned s)
{
	p = str_u(p, s / 60);
	*p++ = ':';
	*p++ = (char)('0' + (s % 60) / 10);
	*p++ = (char)('0' + s % 10);
	*p = 0;
	return p;
}

/* text centred, shrunk to fit: scale 2 if it fits, else 1, else cut with "..." */
static void centred(const char *s, int y, int big, uint32_t c)
{
	char buf[80];
	int scale = big && gfx_text_w(s, &font16x32, 2) <= GW - 120 ? 2 : 1;
	if (gfx_text_w(s, &font16x32, scale) > GW - 120) {
		unsigned k = (unsigned)((GW - 120) / 16 - 3), i;
		for (i = 0; i < k && s[i]; i++) buf[i] = s[i];
		buf[i] = 0; str_cat(buf + i, "...");
		s = buf;
	}
	gfx_text((GW - gfx_text_w(s, &font16x32, scale)) / 2, y, s, &font16x32, scale, c);
}

void bt_ui_draw(int pressed)
{
	const char *st = !bt.connected ? "No phone connected yet: it reconnects by itself"
	               : bt.playing ? "Bluetooth: playing" : "Bluetooth: paused";
	gfx_text(260, 112, st, &font16x32, 1, bt.connected ? 0x74c0fc : 0x8aa0c8);
	if (bt.title[0]) centred(bt.title, 180, 1, 0xffffff);
	else centred(bt.connected ? "Press Play" : "Pair or connect your phone to \"CAR BT\"", 196, 0, 0xc8d4ec);
	if (bt.artist[0]) centred(bt.artist, 262, 0, 0x74c0fc);
	if (bt.album[0]) centred(bt.album, 300, 0, 0x8aa0c8);
	/* progress */
	unsigned p = now_pos();
	gfx_rrect(100, 352, 1080, 12, 6, 0x2a3f6a);
	if (bt.len) gfx_rrect(100, 352, (int)(1080u * p / bt.len) < 12 ? 12 : (int)(1080u * p / bt.len), 12, 6, ACCENT);
	char s[16];
	fmt_t(s, p);
	gfx_text(100, 374, s, &font8x16, 1, 0xc8d4ec);
	if (bt.len) { fmt_t(s, bt.len); gfx_text(1180 - gfx_text_w(s, &font8x16, 1), 374, s, &font8x16, 1, 0xc8d4ec); }

	button(B_PREV, "|<<", pressed == B_PREV, BTN);
	button(B_PLAY, bt.playing ? "Pause" : "Play", pressed == B_PLAY, ACCENT);
	button(B_NEXT, ">>|", pressed == B_NEXT, BTN);
	button(B_VOLDN, "Vol -", pressed == B_VOLDN, BTN);
	button(B_VOLUP, "Vol +", pressed == B_VOLUP, BTN);
	button(B_MUTE, snd_muted ? "Unmute" : "Mute", pressed == B_MUTE, snd_muted ? 0xc92a2a : BTN);
	for (int i = 0; i < SOUND_MAX; i++)
		gfx_rrect(252 + i * 20, 595, 14, 40, 4, i < snd_vol ? (snd_muted ? 0x5c6b8a : 0xc8d4ec) : 0x2a3f6a);
	gfx_text(GW - 32 - gfx_text_w("knob: volume   push: play/pause", &font8x16, 1), 538,
	         "knob: volume   push: play/pause", &font8x16, 1, 0x8aa0c8);
}

static void play_pause(void)
{
	if (bt.playing) { bt_pause(); bt.playing = 0; }
	else bt_play();
}

int bt_ui_press(int id)
{
	switch (id) {
	case B_PREV: bt_prev(); break;
	case B_PLAY: play_pause(); break;
	case B_NEXT: bt_next(); break;
	case B_VOLDN: sound_step(-1); break;
	case B_VOLUP: sound_step(1); break;
	case B_MUTE: snd_muted = !snd_muted; sound_apply(); break;
	}
	return 1;
}

int bt_ui_knob(int dir) { sound_step(dir); return 1; }
int bt_ui_knob_push(void) { play_pause(); return 1; }

int bt_ui_tick(void)
{
	unsigned s = now_pos();
	if (s == shown_s) return 0;
	shown_s = s;
	return 1;
}
