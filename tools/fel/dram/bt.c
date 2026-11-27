/* bt.c: see bt.h. */
#include "lib.h"
#include "uart.h"
#include "bt.h"

#define U 4
bt_state bt;
static char line[256];
static unsigned n;

void bt_init(void) { uart_init(U, 115200); n = 0; }

void bt_cmd(const char *c)
{
	const char *p = "AT#";
	while (*p) uart_putc(U, (uint8_t)*p++);
	while (*c) uart_putc(U, (uint8_t)*c++);
	uart_putc(U, '\r'); uart_putc(U, '\n');
}

/* UTF-8 -> ASCII for the 8x16/16x32 fonts: common Latin accents folded to the
   base letter, curly quotes and dashes straightened, anything else '?' */
static char fold(uint8_t a, uint8_t b)
{
	if (a == 0xC3) {
		static const char lo[] = "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
		return b >= 0x80 && b <= 0xBF ? lo[b - 0x80] : '?';
	}
	return '?';
}

static void copy_field(char *dst, unsigned cap, const char *s, unsigned len)
{
	unsigned o = 0;
	for (unsigned i = 0; i < len && o + 1 < cap; i++) {
		uint8_t c = (uint8_t)s[i];
		if (c < 0x80) { dst[o++] = c >= 32 ? (char)c : ' '; continue; }
		if ((c & 0xE0) == 0xC0 && i + 1 < len) { dst[o++] = fold(c, (uint8_t)s[i + 1]); i += 1; continue; }
		if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			uint8_t b = (uint8_t)s[i + 1], d = (uint8_t)s[i + 2];
			dst[o++] = (c == 0xE2 && b == 0x80) ? (d == 0x98 || d == 0x99 ? '\'' : d == 0x9C || d == 0x9D ? '"' :
			           d == 0x93 || d == 0x94 ? '-' : d == 0xA6 ? '.' : '?') : '?';
			i += 2; continue;
		}
		if ((c & 0xF8) == 0xF0 && i + 3 < len) { dst[o++] = '?'; i += 3; continue; }
		dst[o++] = '?';
	}
	dst[o] = 0;
}

static unsigned strlen_(const char *s) { unsigned k = 0; while (s[k]) k++; return k; }

static unsigned num(const char *s, unsigned k)
{
	unsigned v = 0;
	for (unsigned i = 0; i < k && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (unsigned)(s[i] - '0');
	return v;
}

static int handle(void)
{
	if (n < 2) return 0;
	char a = line[0], b = line[1];
	const char *d = line + 2;
	unsigned dl = n - 2;
	if (a == 'M' && b == 'I') {                          /* title FF artist FF album FF ms FF */
		char *dst[3] = { bt.title, bt.artist, bt.album };
		unsigned f = 0, start = 0;
		for (unsigned i = 0; i <= dl && f < 4; i++)
			if (i == dl || (uint8_t)d[i] == 0xFF) {
				if (f < 3) copy_field(dst[f], 64, d + start, i - start);
				else bt.len = num(d + start, i - start) / 1000;
				f++; start = i + 1;
			}
		bt.connected = 1;
		return 1;
	}
	if (a == 'M' && b == 'P' && dl >= 16) {              /* pos(8) len(8), seconds */
		bt.pos = num(d, 8); bt.len = num(d + 8, 8); bt.pos_ms = millis();
		bt.connected = 1; bt.playing = 1;
		return 1;
	}
	if (a == 'M' && b == 'A' && dl == 0) { bt.playing = 0; bt.connected = 1; return 1; }
	if (a == 'M' && b == 'B' && dl == 0) { bt.playing = 1; bt.connected = 1; bt.pos_ms = millis(); return 1; }
	if (a == 'I' && b == 'B') { bt.connected = 1; return 1; }
	if (a == 'I' && (b == 'D' || b == 'C' || b == 'R')) {   /* ringing / dialling / caller number */
		if (dl) copy_field(bt.number, sizeof bt.number, d, dl);
		if (b == 'D') bt.call = BT_CALL_RINGING;
		if (b == 'C') bt.call = BT_CALL_OUTGOING;
		bt.connected = 1;
		return 1;
	}
	if (a == 'I' && b == 'Q') {                          /* number FF name: the phone's contact lookup */
		unsigned i = 0;
		while (i < dl && (uint8_t)d[i] != 0xFF) i++;
		if (i) copy_field(bt.number, sizeof bt.number, d, i);
		if (i < dl) {
			copy_field(bt.caller, sizeof bt.caller, d + i + 1, dl - i - 1);
			for (int k = (int)strlen_(bt.caller) - 1; k >= 0 && bt.caller[k] == ' '; k--) bt.caller[k] = 0;
		}
		return 1;
	}
	if (a == 'I' && b == 'G') { bt.call = BT_CALL_ACTIVE; bt.call_ms = millis(); return 1; }
	if (a == 'I' && b == 'F') { bt.call = BT_CALL_NONE; bt.number[0] = bt.caller[0] = 0; return 1; }
	if (a == 'I' && b == 'A') {
		bt.connected = bt.playing = 0;
		bt.call = BT_CALL_NONE; bt.number[0] = bt.caller[0] = 0;
		bt.title[0] = bt.artist[0] = bt.album[0] = 0; bt.pos = bt.len = 0;
		return 1;
	}
	return 0;
}

int bt_poll(void)
{
	int c, ch = 0;
	while ((c = uart_getc(U)) >= 0) {
		if (c == '\r' || c == '\n') {
			if (n) { line[n] = 0; ch |= handle(); n = 0; }
		} else if (n < sizeof line - 1) line[n++] = (char)c;
	}
	return ch;
}
