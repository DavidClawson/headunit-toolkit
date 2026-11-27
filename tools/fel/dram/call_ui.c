/* call_ui.c: phone calls through the Bluetooth module. Ringing: Answer /
   Decline. Dialling or in a call: Hang up. The sound input follows the call
   (Bluetooth during it, back to what was playing after). */
#include "lib.h"
#include "gfx.h"
#include "bt.h"
#include "tuner.h"
#include "call_ui.h"

enum { C_ANSWER = 90, C_DECLINE, C_HANGUP };
static int last_call;
static uint8_t saved_input;
static unsigned shown_s = ~0u;

static void rect(int id, int *x, int *y, int *w, int *h)
{
	*y = 470; *w = 260; *h = 100;
	*x = id == C_ANSWER ? 360 : id == C_DECLINE ? 660 : 510;
}

int call_ui_hit(int x, int y)
{
	int lo = bt.call == BT_CALL_RINGING ? C_ANSWER : C_HANGUP;
	int hi = bt.call == BT_CALL_RINGING ? C_DECLINE : C_HANGUP;
	for (int id = lo; id <= hi; id++) {
		int bx, by, bw, bh;
		rect(id, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh) return id;
	}
	return -1;
}

static void button(int id, const char *a, int pressed, uint32_t c)
{
	int x, y, w, h;
	rect(id, &x, &y, &w, &h);
	if (pressed) c = gfx_mix(c, 0xffffff, 90);
	gfx_rrect(x, y, w, h, 24, c);
	gfx_text(x + (w - gfx_text_w(a, &font16x32, 1)) / 2, y + (h - 32) / 2, a, &font16x32, 1, 0xffffff);
}

void call_ui_draw(int pressed)
{
	gfx_rrect(240, 110, 800, 500, 32, 0x0e1830);
	gfx_rring(240, 110, 800, 500, 32, 3, 0x2f9e44);
	const char *t = bt.call == BT_CALL_RINGING ? "Incoming call" :
	                bt.call == BT_CALL_OUTGOING ? "Calling..." : "On a call";
	gfx_text(640 - gfx_text_w(t, &font16x32, 1) / 2, 150, t, &font16x32, 1, 0x8ce99a);
	const char *n = bt.number[0] ? bt.number : "Unknown number";
	int sc = gfx_text_w(n, &font16x32, 2) <= 760 ? 2 : 1;
	gfx_text(640 - gfx_text_w(n, &font16x32, sc) / 2, 240, n, &font16x32, sc, 0xffffff);
	if (bt.call == BT_CALL_ACTIVE) {
		char s[16], *p;
		unsigned sec = (millis() - bt.call_ms) / 1000;
		p = str_u(s, sec / 60); *p++ = ':'; *p++ = (char)('0' + sec % 60 / 10); *p++ = (char)('0' + sec % 10); *p = 0;
		gfx_text(640 - gfx_text_w(s, &font16x32, 1) / 2, 350, s, &font16x32, 1, 0xc8d4ec);
	}
	if (bt.call == BT_CALL_RINGING) {
		button(C_ANSWER, "Answer", pressed == C_ANSWER, 0x2b8a3e);
		button(C_DECLINE, "Decline", pressed == C_DECLINE, 0xc92a2a);
	} else
		button(C_HANGUP, "Hang up", pressed == C_HANGUP, 0xc92a2a);
	const char *k = bt.call == BT_CALL_RINGING ? "knob push: answer    knob turn: volume" : "knob push: hang up    knob turn: volume";
	gfx_text(640 - gfx_text_w(k, &font8x16, 1) / 2, 588, k, &font8x16, 1, 0x8aa0c8);
}

int call_ui_press(int id)
{
	if (id == C_ANSWER) bt_answer();
	if (id == C_DECLINE) bt_reject();
	if (id == C_HANGUP) bt_hangup();
	return 1;
}

int call_ui_knob_push(void)
{
	if (bt.call == BT_CALL_RINGING) bt_answer(); else bt_hangup();
	return 1;
}

int call_ui_tick(void)
{
	int ch = 0;
	if (bt.call != last_call) {
		if (!last_call) { saved_input = snd_input; sound_input(SOUND_IN_BT); }
		if (!bt.call) sound_input(saved_input);
		logf("call: state %d -> %d\n", last_call, bt.call);
		last_call = bt.call; ch = 1;
	}
	if (bt.call == BT_CALL_ACTIVE) {
		unsigned s = (millis() - bt.call_ms) / 1000;
		if (s != shown_s) { shown_s = s; ch = 1; }
	}
	return ch;
}
