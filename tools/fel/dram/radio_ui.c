/*
 * radio_ui.c: the Radio page.
 *   knob: tune 0.2 MHz        Seek < / >: next station down / up
 *   - / + 0.2: tune a step    Scan: search the band, the 6 strongest go to the presets
 *   preset tap: tune it       preset hold 1 s: store the current station there
 *   Vol - / +, Mute
 * Presets live in RAM only for now (no flash writes yet).
 */
#include "lib.h"
#include "gfx.h"
#include "tuner.h"
#include "radio_ui.h"

enum { B_SEEKDN = 1, B_DN, B_UP, B_SEEKUP, B_SCAN, B_P1, B_P6 = B_P1 + 5, B_VOLDN, B_VOLUP, B_MUTE, B_N };
#define NPRE 6
#define ACCENT 0xd9480f

static struct {
	int opened, init_err;
	uint16_t pre[NPRE];
	int vol, muted;
	int scanning, nfound;
	struct { uint16_t f; uint8_t rssi; } found[40];
	uint16_t before_scan;
	char note[48];
	uint32_t note_until;
} r = { .vol = 25 };

static void rect(int id, int *x, int *y, int *w, int *h)
{
	if (id <= B_SCAN) { *w = 220; *h = 90; *x = 50 + (id - 1) * 240; *y = 330; return; }
	if (id <= B_P6)   { *w = 180; *h = 90; *x = 50 + (id - B_P1) * 200; *y = 450; return; }
	*y = 570; *h = 90;
	if (id == B_VOLDN) { *x = 50;   *w = 180; }
	if (id == B_VOLUP) { *x = 870;  *w = 180; }
	if (id == B_MUTE)  { *x = 1070; *w = 160; }
}

int radio_hit(int x, int y)
{
	for (int id = 1; id < B_N; id++) {
		int bx, by, bw, bh;
		rect(id, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh) return id;
	}
	return -1;
}

static char *fmt_f(char *p, uint16_t f)
{
	p = str_u(p, f / 100u);
	*p++ = '.';
	return str_u(p, (f % 100u) / 10u);
}

static void say(const char *a, uint16_t f)
{
	char *p = str_cat(r.note, a);
	if (f) { p = fmt_f(p, f); str_cat(p, " MHz"); }
	r.note_until = millis() + 1500;
}

const char *radio_note(void)
{
	return millis() < r.note_until ? r.note : 0;
}

static void apply_volume(void) { sound_volume(r.muted ? 0 : r.vol); }

void radio_open(void)
{
	if (r.opened) return;
	r.opened = 1;
	sound_init();
	apply_volume();
	r.init_err = tuner_init();
	logf("radio: tuner_init %d, Si47%02u fw %c.%c rev %c\n", r.init_err, tuner.part,
	     tuner.fw_major, tuner.fw_minor, tuner.rev);
	if (!r.init_err) tuner_tune(10110);
}

static void button(int id, const char *a, const char *b, int pressed, uint32_t c)
{
	int x, y, w, h;
	rect(id, &x, &y, &w, &h);
	if (pressed) c = gfx_mix(c, 0xffffff, 90);
	gfx_rrect(x, y, w, h, 18, c);
	if (b) {
		gfx_text(x + (w - gfx_text_w(a, &font8x16, 1)) / 2, y + 14, a, &font8x16, 1, gfx_mix(c, 0xffffff, 190));
		gfx_text(x + (w - gfx_text_w(b, &font16x32, 1)) / 2, y + 40, b, &font16x32, 1, 0xffffff);
	} else
		gfx_text(x + (w - gfx_text_w(a, &font16x32, 1)) / 2, y + (h - 32) / 2, a, &font16x32, 1, 0xffffff);
}

void radio_draw(int pressed)
{
	char s[48], *p;
	if (r.init_err) {
		p = str_cat(s, "Tuner not answering (error ");
		p = str_u(p, (unsigned)r.init_err); str_cat(p, ")");
		gfx_text((GW - gfx_text_w(s, &font16x32, 1)) / 2, 300, s, &font16x32, 1, 0xff8080);
		return;
	}
	/* frequency */
	fmt_f(s, tuner.freq);
	int fw = gfx_text_w(s, &font16x32, 5);
	int fx = (GW - fw) / 2 - 60;
	gfx_text(fx, 100, s, &font16x32, 5, 0xffffff);
	gfx_text(fx + fw + 16, 212, "MHz", &font16x32, 1, 0xc8d4ec);
	gfx_text(fx - 90, 116, "FM", &font16x32, 1, ACCENT);
	/* status under the frequency */
	const char *st = r.scanning ? "scanning..." : tuner.busy ? "seeking..." :
	                 tuner.stereo >= 50 ? "STEREO" : "MONO";
	gfx_text((GW - gfx_text_w(st, &font16x32, 1)) / 2 - 60, 272, st, &font16x32, 1,
	         tuner.busy || r.scanning ? 0xffd43b : tuner.stereo >= 50 ? 0x69db7c : 0x8aa0c8);
	/* signal meter: 10 bars, full at 60 dBuV */
	for (int i = 0; i < 10; i++) {
		int bh = 16 + i * 12, lit = !tuner.busy && tuner.rssi >= (i + 1) * 6;
		gfx_rrect(1000 + i * 24, 250 - bh, 16, bh, 4,
		          lit ? (i < 3 ? 0xff922b : i < 6 ? 0xffd43b : 0x69db7c) : 0x2a3f6a);
	}
	p = str_cat(s, "RSSI "); p = str_u(p, tuner.rssi);
	p = str_cat(p, " dBuV  SNR "); p = str_u(p, tuner.snr); str_cat(p, " dB");
	gfx_text(1000, 266, s, &font8x16, 1, 0xc8d4ec);

	button(B_SEEKDN, "|<< Seek", 0, pressed == B_SEEKDN, 0x1971c2);
	button(B_DN, "- 0.2", 0, pressed == B_DN, 0x2b3a5c);
	button(B_UP, "+ 0.2", 0, pressed == B_UP, 0x2b3a5c);
	button(B_SEEKUP, "Seek >>|", 0, pressed == B_SEEKUP, 0x1971c2);
	button(B_SCAN, r.scanning ? "Stop" : "Scan", 0, pressed == B_SCAN, ACCENT);
	for (int i = 0; i < NPRE; i++) {
		char lab[8] = "P1", fq[8] = "--";
		lab[1] = (char)('1' + i);
		if (r.pre[i]) fmt_f(fq, r.pre[i]);
		int cur = r.pre[i] && r.pre[i] == tuner.freq;
		button(B_P1 + i, lab, fq, pressed == B_P1 + i, cur ? gfx_mix(ACCENT, 0x000000, 60) : 0x2b3a5c);
	}
	button(B_VOLDN, "Vol -", 0, pressed == B_VOLDN, 0x2b3a5c);
	button(B_VOLUP, "Vol +", 0, pressed == B_VOLUP, 0x2b3a5c);
	button(B_MUTE, r.muted ? "Unmute" : "Mute", 0, pressed == B_MUTE, r.muted ? 0xc92a2a : 0x2b3a5c);
	for (int i = 0; i < SOUND_MAX; i++)
		gfx_rrect(252 + i * 20, 595, 14, 40, 4,
		          i < r.vol ? (r.muted ? 0x5c6b8a : 0xc8d4ec) : 0x2a3f6a);
}

static void scan_start(void)
{
	r.scanning = 1; r.nfound = 0; r.before_scan = tuner.freq;
	tuner_tune(FM_MIN);
}

static void scan_finish(void)
{
	r.scanning = 0;
	/* six strongest, then in frequency order */
	int n = r.nfound;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (r.found[j].rssi > r.found[i].rssi) { typeof(r.found[0]) t = r.found[i]; r.found[i] = r.found[j]; r.found[j] = t; }
	if (n > NPRE) n = NPRE;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (r.found[j].f < r.found[i].f) { typeof(r.found[0]) t = r.found[i]; r.found[i] = r.found[j]; r.found[j] = t; }
	for (int i = 0; i < NPRE; i++) r.pre[i] = i < n ? r.found[i].f : 0;
	char *p = str_cat(r.note, "Scan: ");
	p = str_u(p, (unsigned)r.nfound); str_cat(p, r.nfound == 1 ? " station" : " stations");
	r.note_until = millis() + 2500;
	logf("radio: scan found %d\n", r.nfound);
	tuner_tune(n ? r.pre[0] : r.before_scan);
}

int radio_press(int id, uint32_t held_ms)
{
	if (r.init_err) return 0;
	if (r.scanning && id != B_SCAN && id < B_VOLDN) return 0;
	switch (id) {
	case B_SEEKDN: tuner_seek(0, 1); break;
	case B_SEEKUP: tuner_seek(1, 1); break;
	case B_DN: tuner_tune((uint16_t)(tuner.freq - FM_STEP)); break;
	case B_UP: tuner_tune((uint16_t)(tuner.freq + FM_STEP)); break;
	case B_SCAN:
		if (r.scanning) { r.nfound = r.nfound > 0 ? r.nfound : 0; scan_finish(); }
		else scan_start();
		break;
	case B_VOLDN: if (r.vol > 1) r.vol--; r.muted = 0; apply_volume(); break;
	case B_VOLUP: if (r.vol < SOUND_MAX) r.vol++; r.muted = 0; apply_volume(); break;
	case B_MUTE: r.muted = !r.muted; apply_volume(); break;
	default:
		if (id >= B_P1 && id <= B_P6) {
			int i = id - B_P1;
			if (held_ms >= 1000) { r.pre[i] = tuner.freq; say("Saved ", tuner.freq); }
			else if (r.pre[i]) tuner_tune(r.pre[i]);
		}
	}
	return 1;
}

int radio_knob(int dir)
{
	if (r.init_err || r.scanning) return 0;
	tuner_tune((uint16_t)(tuner.freq + dir * FM_STEP));
	return 1;
}

int radio_tick(void)
{
	if (!r.opened || r.init_err) return 0;
	int c = tuner_poll();
	if (c == 2 && r.scanning) {
		logf("radio: seek stop %u valid %u limit %u RSSI %u SNR %u\n",
		     tuner.freq, tuner.valid, tuner.band_limit, tuner.rssi, tuner.snr);
		uint16_t last = r.nfound ? r.found[r.nfound - 1].f : 0;
		if (tuner.freq > last && tuner.freq != FM_MIN && tuner.valid && r.nfound < 40) {
			r.found[r.nfound].f = tuner.freq; r.found[r.nfound].rssi = tuner.rssi; r.nfound++;
		}
		if (tuner.band_limit || tuner.freq < last || tuner.freq >= FM_MAX) scan_finish();
		else tuner_seek(1, 0);
	}
	return c != 0;
}
