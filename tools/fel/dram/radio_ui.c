/*
 * radio_ui.c: the Radio page.
 *   knob turn: volume (default), favorites, or tune 0.2 MHz
 *   knob push: VOLUME -> FAVORITES -> TUNE -> VOLUME. A ring shows what the knob
 *     controls while it's in use; 5 s after the last knob move the ring hides
 *     and the knob goes back to volume.
 *   Seek < / >: next station down / up       - / + 0.2: tune a step
 *   Scan: search the band into the station list (opens it)
 *   Stations: open the list. Tap a station: listen. Hold: add to favorites.
 *   Favorites row: tap: listen. Hold: remove.
 *   Vol - / +, Mute
 * Stations and favorites live in RAM only for now (no flash writes yet).
 */
#include "lib.h"
#include "gfx.h"
#include "tuner.h"
#include "radio_ui.h"

/* button ids: main page, station list, popup */
enum { B_SEEKDN = 1, B_DN, B_UP, B_SEEKUP, B_SCAN, B_LIST, B_F1, B_F6 = B_F1 + 5,
       B_VOLDN, B_VOLUP, B_MUTE, B_MAIN_END,
       L_CLOSE = 40, L_PREV, L_NEXT, L_CHIP = 50, L_CHIP_END = L_CHIP + 20,
       P_OK = 80, P_CANCEL };
#define NFAV 6
#define NSTA 40
#define PER_PAGE 20
#define ACCENT 0xd9480f
#define BTN 0x2b3a5c

enum { KNOB_VOLUME, KNOB_FAV, KNOB_TUNE, KNOB_N };   /* push order; VOLUME is home */
static const char *const knob_name[KNOB_N] = { "KNOB: VOLUME", "KNOB: FAVORITES", "KNOB: TUNE" };
#define KNOB_IDLE_MS 5000

enum { POP_NONE, POP_ADD, POP_REMOVE, POP_FULL };

static struct {
	int opened, init_err;
	struct { uint16_t f; char name[24]; } fav[NFAV];
	struct { uint16_t f; uint8_t rssi; } sta[NSTA];
	int nsta;
	int knob;
	uint32_t ring_until;    /* ring visible (and knob mode held) until; 0 = hidden */
	int scanning;
	uint16_t before_scan;
	int list_open, list_page;
	int pop; uint16_t pop_f; int pop_i;
	char note[48];
	uint32_t note_until;
} r;

/* ---------- geometry ---------- */

static void rect(int id, int *x, int *y, int *w, int *h)
{
	*w = *h = 0;
	if (id >= B_SEEKDN && id <= B_LIST) { *w = 180; *h = 90; *x = 50 + (id - B_SEEKDN) * 200; *y = 330; return; }
	if (id >= B_F1 && id <= B_F6)       { *w = 180; *h = 90; *x = 50 + (id - B_F1) * 200; *y = 450; return; }
	if (id == B_VOLDN) { *x = 50;   *y = 570; *w = 180; *h = 90; return; }
	if (id == B_VOLUP) { *x = 870;  *y = 570; *w = 180; *h = 90; return; }
	if (id == B_MUTE)  { *x = 1070; *y = 570; *w = 160; *h = 90; return; }
	if (id >= L_CLOSE && id <= L_NEXT) { *x = 760 + (id - L_CLOSE == 0 ? 2 : id - L_PREV) * 160; *y = 96; *w = 140; *h = 60; return; }
	if (id >= L_CHIP && id < L_CHIP_END) {
		int i = id - L_CHIP;
		*w = 224; *h = 110; *x = 56 + (i % 5) * 236; *y = 172 + (i / 5) * 122; return;
	}
	if (id == P_OK)     { *x = 420; *y = 400; *w = 210; *h = 70; return; }
	if (id == P_CANCEL) { *x = 650; *y = 400; *w = 210; *h = 70; return; }
}

int radio_modal(void) { return r.list_open || r.pop != POP_NONE; }

int radio_hit(int x, int y)
{
	int lo, hi;
	if (r.pop != POP_NONE) { lo = r.pop == POP_FULL ? P_CANCEL : P_OK; hi = P_CANCEL; }
	else if (r.list_open) { lo = L_CLOSE; hi = L_CHIP_END - 1; }
	else { lo = B_SEEKDN; hi = B_MAIN_END - 1; }
	for (int id = lo; id <= hi; id++) {
		if (id > L_NEXT && id < L_CHIP) continue;
		if (id >= L_CHIP && id < L_CHIP_END && r.list_page * PER_PAGE + id - L_CHIP >= r.nsta) continue;
		int bx, by, bw, bh;
		rect(id, &bx, &by, &bw, &bh);
		if (bw && x >= bx && x < bx + bw && y >= by && y < by + bh) return id;
	}
	return -1;
}

/* ---------- helpers ---------- */

static char *fmt_f(char *p, uint16_t f)
{
	p = str_u(p, f / 100u);
	*p++ = '.';
	return str_u(p, (f % 100u) / 10u);
}

static void say(const char *a, uint16_t f, const char *b)
{
	char *p = str_cat(r.note, a);
	if (f) { p = fmt_f(p, f); p = str_cat(p, " MHz"); }
	if (b) str_cat(p, b);
	r.note_until = millis() + 2000;
}

const char *radio_note(void) { return millis() < r.note_until ? r.note : 0; }


static int fav_index(uint16_t f)
{
	for (int i = 0; i < NFAV; i++) if (r.fav[i].f && r.fav[i].f == f) return i;
	return -1;
}

void radio_open(void)
{
	sound_input(SOUND_IN_RADIO);                 /* every time the page opens */
	if (r.opened) return;
	r.opened = 1;
	r.init_err = tuner_init();
	logf("radio: tuner_init %d, Si47%02u fw %c.%c rev %c\n", r.init_err, tuner.part,
	     tuner.fw_major, tuner.fw_minor, tuner.rev);
	if (!r.init_err) tuner_tune(10110);
}

/* ---------- drawing ---------- */

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

static void bars(int x, int y, unsigned rssi, int n, int bw)
{
	for (int i = 0; i < n; i++) {
		int bh = 6 + i * 5, lit = rssi >= (unsigned)(i + 1) * (60 / n);
		gfx_fill(x + i * (bw + 3), y - bh, bw, bh, lit ? 0x69db7c : 0x2a3f6a);
	}
}

static void draw_list(int pressed)
{
	gfx_rrect(30, 80, 1220, 590, 28, 0x101a30);
	gfx_rring(30, 80, 1220, 590, 28, 2, 0x2a3f6a);
	char s[48], *p = str_cat(s, "Stations (");
	p = str_u(p, (unsigned)r.nsta); str_cat(p, ")");
	gfx_text(60, 100, s, &font16x32, 1, 0xffffff);
	gfx_text(60, 140, r.nsta ? "tap: listen    hold: add to favorites" : "none yet: tap Scan on the radio page",
	         &font8x16, 1, 0x8aa0c8);
	int pages = (r.nsta + PER_PAGE - 1) / PER_PAGE;
	if (pages > 1) {
		button(L_PREV, "< Prev", 0, pressed == L_PREV, r.list_page > 0 ? BTN : 0x1a2338);
		button(L_NEXT, "Next >", 0, pressed == L_NEXT, r.list_page + 1 < pages ? BTN : 0x1a2338);
	}
	button(L_CLOSE, "Close", 0, pressed == L_CLOSE, ACCENT);
	for (int k = 0; k < PER_PAGE; k++) {
		int i = r.list_page * PER_PAGE + k;
		if (i >= r.nsta) break;
		int x, y, w, h, id = L_CHIP + k;
		rect(id, &x, &y, &w, &h);
		int cur = r.sta[i].f == tuner.freq, fav = fav_index(r.sta[i].f) >= 0;
		uint32_t c = cur ? gfx_mix(ACCENT, 0x000000, 60) : BTN;
		if (pressed == id) c = gfx_mix(c, 0xffffff, 90);
		gfx_rrect(x, y, w, h, 18, c);
		fmt_f(s, r.sta[i].f);
		gfx_text(x + 18, y + 18, s, &font16x32, 2, 0xffffff);
		bars(x + 18, y + h - 12, r.sta[i].rssi, 5, 8);
		if (fav) { gfx_rrect(x + w - 64, y + h - 34, 50, 24, 8, 0xffd43b);
		           gfx_text(x + w - 57, y + h - 30, "FAV", &font8x16, 1, 0x101a30); }
	}
}

static void draw_popup(int pressed)
{
	gfx_rrect(390, 220, 500, 280, 28, 0x1d2b4f);
	gfx_rring(390, 220, 500, 280, 28, 3, 0xc8d4ec);
	char s[48], *p;
	const char *title = r.pop == POP_ADD ? "Add to favorites?" : r.pop == POP_REMOVE ? "Remove favorite?" : "Favorites are full";
	gfx_text(640 - gfx_text_w(title, &font16x32, 1) / 2, 250, title, &font16x32, 1, 0xffffff);
	p = fmt_f(s, r.pop_f); str_cat(p, " MHz");
	if (r.pop == POP_FULL) str_cat(s, ": remove one first");
	gfx_text(640 - gfx_text_w(s, &font16x32, 1) / 2, 310, s, &font16x32, 1, 0xffd43b);
	if (r.pop != POP_FULL)
		button(P_OK, r.pop == POP_ADD ? "Add" : "Remove", 0, pressed == P_OK, r.pop == POP_ADD ? 0x2b8a3e : 0xc92a2a);
	button(P_CANCEL, r.pop == POP_FULL ? "OK" : "Cancel", 0, pressed == P_CANCEL, BTN);
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
	int mp = radio_modal() ? -1 : pressed;
	/* frequency */
	fmt_f(s, tuner.freq);
	int fw = gfx_text_w(s, &font16x32, 5);
	int fx = (GW - fw) / 2 - 60;
	gfx_text(fx, 100, s, &font16x32, 5, 0xffffff);
	gfx_text(fx + fw + 16, 212, "MHz", &font16x32, 1, 0xc8d4ec);
	gfx_text(fx - 90, 116, "FM", &font16x32, 1, ACCENT);
	int fi = fav_index(tuner.freq);
	if (fi >= 0 && r.fav[fi].name[0])
		gfx_text(fx + fw + 16, 120, r.fav[fi].name, &font16x32, 1, 0xffd43b);
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
	/* knob mode, and a ring round its area while the knob is in use */
	gfx_rrect(1000, 92, 240, 40, 14, BTN);
	const char *kn = knob_name[r.knob];
	gfx_text(1000 + (240 - gfx_text_w(kn, &font8x16, 1)) / 2, 104, kn, &font8x16, 1, 0xffffff);
	if (r.ring_until) {
		if (r.knob == KNOB_TUNE)   gfx_rring(fx - 110, 92, fw + 200, 170, 24, 3, 0xffffff);
		if (r.knob == KNOB_FAV)    gfx_rring(38, 438, 1204, 114, 24, 3, 0xffffff);
		if (r.knob == KNOB_VOLUME) gfx_rring(38, 558, 1204, 114, 24, 3, 0xffffff);
	}

	button(B_SEEKDN, "|<< Seek", 0, mp == B_SEEKDN, 0x1971c2);
	button(B_DN, "- 0.2", 0, mp == B_DN, BTN);
	button(B_UP, "+ 0.2", 0, mp == B_UP, BTN);
	button(B_SEEKUP, "Seek >>|", 0, mp == B_SEEKUP, 0x1971c2);
	button(B_SCAN, r.scanning ? "Stop" : "Scan", 0, mp == B_SCAN, ACCENT);
	button(B_LIST, "Stations", 0, mp == B_LIST, ACCENT);
	for (int i = 0; i < NFAV; i++) {
		char lab[24] = "Favorite 1", fq[8] = "--";
		if (r.fav[i].f) {
			fmt_f(fq, r.fav[i].f);
			if (r.fav[i].name[0]) str_cat(lab, r.fav[i].name);
		}
		if (!r.fav[i].name[0]) lab[9] = (char)('1' + i);
		int cur = r.fav[i].f && r.fav[i].f == tuner.freq;
		button(B_F1 + i, lab, fq, mp == B_F1 + i, cur ? gfx_mix(ACCENT, 0x000000, 60) : BTN);
	}
	button(B_VOLDN, "Vol -", 0, mp == B_VOLDN, BTN);
	button(B_VOLUP, "Vol +", 0, mp == B_VOLUP, BTN);
	button(B_MUTE, snd_muted ? "Unmute" : "Mute", 0, mp == B_MUTE, snd_muted ? 0xc92a2a : BTN);
	for (int i = 0; i < SOUND_MAX; i++)
		gfx_rrect(252 + i * 20, 595, 14, 40, 4,
		          i < snd_vol ? (snd_muted ? 0x5c6b8a : 0xc8d4ec) : 0x2a3f6a);

	if (r.list_open) draw_list(r.pop == POP_NONE ? pressed : -1);
	if (r.pop != POP_NONE) draw_popup(pressed);
}

/* ---------- actions ---------- */

static void scan_start(void)
{
	r.scanning = 1; r.nsta = 0; r.before_scan = tuner.freq;
	tuner_tune(FM_MIN);
}

static void scan_finish(void)
{
	r.scanning = 0;
	char *p = str_cat(r.note, "Scan: ");
	p = str_u(p, (unsigned)r.nsta); str_cat(p, r.nsta == 1 ? " station" : " stations");
	r.note_until = millis() + 2500;
	logf("radio: scan found %d\n", r.nsta);
	tuner_tune(r.before_scan);
	r.list_open = 1; r.list_page = 0;
}

static void fav_add(uint16_t f)
{
	for (int i = 0; i < NFAV; i++)
		if (!r.fav[i].f) { r.fav[i].f = f; r.fav[i].name[0] = 0; say("Added ", f, " to favorites"); return; }
}

int radio_press(int id)
{
	if (r.init_err) return 0;
	if (r.pop != POP_NONE) {
		if (id == P_OK && r.pop == POP_ADD) fav_add(r.pop_f);
		if (id == P_OK && r.pop == POP_REMOVE) { r.fav[r.pop_i].f = 0; r.fav[r.pop_i].name[0] = 0; say("Removed ", r.pop_f, 0); }
		r.pop = POP_NONE;
		return 1;
	}
	if (r.list_open) {
		int pages = (r.nsta + PER_PAGE - 1) / PER_PAGE;
		if (id == L_CLOSE) r.list_open = 0;
		else if (id == L_PREV && r.list_page > 0) r.list_page--;
		else if (id == L_NEXT && r.list_page + 1 < pages) r.list_page++;
		else if (id >= L_CHIP && id < L_CHIP_END && !r.scanning) tuner_tune(r.sta[r.list_page * PER_PAGE + id - L_CHIP].f);
		return 1;
	}
	if (r.scanning && id != B_SCAN && id < B_VOLDN) return 0;
	switch (id) {
	case B_SEEKDN: tuner_seek(0, 1); break;
	case B_SEEKUP: tuner_seek(1, 1); break;
	case B_DN: tuner_tune((uint16_t)(tuner.freq - FM_STEP)); break;
	case B_UP: tuner_tune((uint16_t)(tuner.freq + FM_STEP)); break;
	case B_SCAN: if (r.scanning) scan_finish(); else scan_start(); break;
	case B_LIST: r.list_open = 1; break;
	case B_VOLDN: sound_step(-1); break;
	case B_VOLUP: sound_step(1); break;
	case B_MUTE: snd_muted = !snd_muted; sound_apply(); break;
	default:
		if (id >= B_F1 && id <= B_F6) {
			int i = id - B_F1;
			if (r.fav[i].f) tuner_tune(r.fav[i].f);
			else say("Hold a station in the list to add it", 0, 0);
		}
	}
	return 1;
}

int radio_hold(int id)
{
	if (r.init_err || r.pop != POP_NONE) return 0;
	if (r.list_open && id >= L_CHIP && id < L_CHIP_END) {
		uint16_t f = r.sta[r.list_page * PER_PAGE + id - L_CHIP].f;
		int i = fav_index(f), free_slot = 0;
		for (int k = 0; k < NFAV; k++) if (!r.fav[k].f) free_slot = 1;
		r.pop_f = f; r.pop_i = i;
		r.pop = i >= 0 ? POP_REMOVE : free_slot ? POP_ADD : POP_FULL;
		return 1;
	}
	if (!r.list_open && id >= B_F1 && id <= B_F6 && r.fav[id - B_F1].f) {
		r.pop_i = id - B_F1; r.pop_f = r.fav[r.pop_i].f; r.pop = POP_REMOVE;
		return 1;
	}
	return 0;
}

int radio_knob_push(void)
{
	r.knob = (r.knob + 1) % KNOB_N;          /* at rest it is VOLUME, so the first push gives FAVORITES */
	r.ring_until = millis() + KNOB_IDLE_MS;
	return 1;
}

int radio_knob(int dir)
{
	if (r.init_err) return 0;
	r.ring_until = millis() + KNOB_IDLE_MS;
	if (r.knob == KNOB_VOLUME) {
		sound_step(dir);
		return 1;
	}
	if (r.scanning) return 1;
	if (r.knob == KNOB_TUNE) { tuner_tune((uint16_t)(tuner.freq + dir * FM_STEP)); return 1; }
	/* favorites: the next filled one after the current station, either way round */
	int cur = fav_index(tuner.freq);
	for (int k = 1; k <= NFAV; k++) {
		int i = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * k + 2 * NFAV) % NFAV;
		if (r.fav[i].f) { tuner_tune(r.fav[i].f); return 1; }
	}
	say("No favorites yet: hold a station in the list", 0, 0);
	return 1;
}

int radio_tick(void)
{
	if (!r.opened || r.init_err) return 0;
	int c = tuner_poll();
	if (r.ring_until && (int32_t)(millis() - r.ring_until) >= 0) { r.ring_until = 0; r.knob = KNOB_VOLUME; c |= 1; }
	if (c == 2 && r.scanning) {
		logf("radio: seek stop %u valid %u limit %u RSSI %u SNR %u\n",
		     tuner.freq, tuner.valid, tuner.band_limit, tuner.rssi, tuner.snr);
		uint16_t last = r.nsta ? r.sta[r.nsta - 1].f : 0;
		if (tuner.freq > last && tuner.freq != FM_MIN && tuner.valid && r.nsta < NSTA) {
			r.sta[r.nsta].f = tuner.freq; r.sta[r.nsta].rssi = tuner.rssi; r.nsta++;
		}
		if (tuner.band_limit || tuner.freq < last || tuner.freq >= FM_MAX) scan_finish();
		else tuner_seek(1, 0);
	}
	return c != 0;
}
