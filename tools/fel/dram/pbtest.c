/*
 * pbtest.c: phonebook download test (FEL). Asks the Bluetooth module (UART4)
 * for the phone's contacts and reports only how many lines of each kind came
 * back, plus the FORMAT of the first few: letters shown as 'x', digits as '#',
 * so no contact data ends up in logs.
 */
#include "lib.h"
#include "uart.h"

static void at(const char *s)
{
	logf("> AT#%s\n", s);
	const char *p = "AT#";
	while (*p) uart_putc(4, (uint8_t)*p++);
	while (*s) uart_putc(4, (uint8_t)*s++);
	uart_putc(4, '\r'); uart_putc(4, '\n');
}

static unsigned counts[26 * 26], shown[26 * 26];

static void masked(const char *l, unsigned n)
{
	char s[90]; unsigned o = 0;
	for (unsigned i = 0; i < n && o < sizeof s - 1; i++) {
		uint8_t c = (uint8_t)l[i];
		s[o++] = i < 2 ? (char)c : (c >= '0' && c <= '9') ? '#' :
		         ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c >= 0x80 ? 'x' : c == 0xFF ? '|' : c < 32 ? '.' : (char)c;
	}
	s[o] = 0;
	logf("  %s  (%u bytes)\n", s, n);
}

static void listen(unsigned ms)
{
	static char l[256]; static unsigned n;
	uint32_t t0 = millis();
	while (millis() - t0 < ms) {
		int c = uart_getc(4);
		if (c < 0) continue;
		if (c != '\r' && c != '\n') { if (n < sizeof l) l[n++] = (char)c; continue; }
		if (!n) continue;
		if (l[0] >= 'A' && l[0] <= 'Z' && l[1] >= 'A' && l[1] <= 'Z') {
			unsigned k = (unsigned)(l[0] - 'A') * 26 + (unsigned)(l[1] - 'A');
			counts[k]++;
			if (!(l[0] == 'M' && l[1] == 'P') && shown[k]++ < 3) masked(l, n);
		}
		n = 0;
	}
}

int main(void)
{
	log_reset();
	logf("phonebook test\n");
	uart_init(4, 115200);
	listen(1500);
	at("PB"); listen(45000);
	at("PA"); listen(30000);
	logf("line counts:");
	for (unsigned k = 0; k < 26 * 26; k++)
		if (counts[k]) logf(" %c%c=%u", 'A' + k / 26, 'A' + k % 26, counts[k]);
	logf("\ndone\n");
	return 0;
}
