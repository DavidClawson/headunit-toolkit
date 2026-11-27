/*
 * bttest.c: Bluetooth audio and key-LED test (FEL; nothing written to flash).
 * Commands from the stock app (init.axf, see docs/mcu-protocol.md):
 *   B0 [FE 00 00 t m]  key LED: m = 0 cycle, 1 purple, 2 red, 3 yellow, 4 cyan,
 *                      5 white, 6 blue, 7 green, FF off; t = cycle seconds
 *   A7 [0B 00]         source = BT music;  BD3702 input 0x05 = 0x0A
 *   D4 "AT#xx\r\n"     BT command through the AB5301 (MA play, MB pause, MD next)
 * Logs every MCU frame; D4 lines as text (MI = title|artist|album|...).
 */
#include "lib.h"
#include "uart.h"
#include "twi.h"
#include "mcu.h"

static unsigned pongs;

static void send(uint8_t cmd, const void *d, unsigned n)
{
	const uint8_t *b = d;
	uint8_t x = 0x80 ^ cmd ^ (uint8_t)n;
	uart_putc(1, 0x02); uart_putc(1, 0x80); uart_putc(1, cmd); uart_putc(1, (uint8_t)n);
	for (unsigned i = 0; i < n; i++) { uart_putc(1, b[i]); x ^= b[i]; }
	uart_putc(1, x);
}

static void at(const char *s)
{
	char buf[40], *p = str_cat(buf, "AT#");
	p = str_cat(p, s); p = str_cat(p, "\r\n");
	logf("> AT#%s\n", s);
	send(0xD4, buf, (unsigned)(p - buf));
}

static void at4(const char *s)
{
	logf("> UART4 AT#%s\n", s);
	const char *p = "AT#";
	while (*p) uart_putc(4, (uint8_t)*p++);
	while (*s) uart_putc(4, (uint8_t)*s++);
	uart_putc(4, '\r'); uart_putc(4, '\n');
}

static void led(uint8_t mode, uint8_t t)
{
	uint8_t d[5] = { 0xFE, 0, 0, t, mode };
	logf("> key LED mode %u\n", mode);
	send(0xB0, d, 5);
}

static void listen(unsigned ms)
{
	static const uint8_t pong[5] = { 0xCE, 0, 0, 0, 0 };
	uint32_t t0 = millis();
	mcu_frame f;
	static char l4[120]; static unsigned n4;
	while (millis() - t0 < ms) {
		int c;
		while ((c = uart_getc(4)) >= 0) {             /* UART4: raw AT text lines */
			if (c == '\n' || c == '\r') { if (n4) { l4[n4] = 0; logf("  U4: %s\n", l4); n4 = 0; } }
			else if (n4 < sizeof l4 - 1) l4[n4++] = c == 0xFF ? '|' : (c >= 32 && c < 127) ? (char)c : '.';
		}
		if (!mcu_poll(&f)) continue;
		if (f.cmd == 0xB2 && f.len == 6 && f.data[1] == 0xCE) { send(0xB0, pong, 5); pongs++; continue; }
		if (f.cmd == 0xD4) {
			char s[70]; unsigned n = 0;
			for (unsigned i = 0; i < f.len && n < sizeof s - 1; i++) {
				uint8_t c = f.data[i];
				if (c == '\r' || c == '\n') continue;
				s[n++] = c == 0xFF ? '|' : (c >= 32 && c < 127) ? (char)c : '.';
			}
			s[n] = 0;
			logf("  BT: %s\n", s);
			continue;
		}
		logf("  mcu %02x %02x:", f.dir, f.cmd);
		for (unsigned i = 0; i < f.len && i < 16; i++) logf(" %02x", f.data[i]);
		logf("\n");
	}
}

static const uint8_t bd_init[][2] = {
	{0x01,0xF4},{0x02,0x03},{0x03,0x19},{0x06,0x06},
	{0x28,0x80},{0x29,0x80},{0x2A,0x80},{0x2B,0x80},{0x2C,0xFF},{0x30,0xFF},
	{0x41,0x00},{0x44,0x00},{0x47,0x00},{0x51,0x00},{0x54,0x00},{0x57,0x00},
	{0x75,0x7F},{0x90,0x30},
};
static void bd(uint8_t r, uint8_t v) { twi_write(2, 0x40, r, 1, &v, 1); }

int main(void)
{
	log_reset();
	logf("BT + key LED test\n1. MCU start-up\n");
	mcu_init();
	uart_init(4, 115200);
	static const uint8_t b7[1] = { 0x02 }, a7main[2] = { 0x0F, 0x00 }, btm[2] = { 0x0B, 0x00 };
	static const uint8_t up[5] = { 0xCE, 0x01, 0, 0, 0 };
	send(0xA4, 0, 0); listen(300);
	send(0xA0, 0, 0); listen(1000);
	send(0xB7, b7, 1); listen(300);
	send(0xC3, 0, 0); listen(300);
	send(0xBA, 0, 0); listen(300);
	send(0xA7, a7main, 2); listen(300);
	send(0xB0, up, 5); listen(1500);

	led(5, 5); listen(300);

	logf("3. sound: BD3702 input 0x0A (BT), -10 dB; source = BT music\n");
	twi_init(2);
	bd(0x20, 0xFF);
	for (unsigned i = 0; i < sizeof bd_init / sizeof bd_init[0]; i++) bd(bd_init[i][0], bd_init[i][1]);
	bd(0x05, 0x0A);
	bd(0x20, 0x8A);
	send(0xA7, btm, 2); listen(2000);

	logf("4. listen on UART1 + UART4; commands via the MCU (D4), then raw on UART4\n");
	listen(5000);
	at("MA"); listen(10000);
	at("MY"); listen(3000);
	at("CY"); listen(3000);
	at("MD"); listen(12000);
	logf("-- raw UART4 at 115200 --\n");
	at4("MB"); listen(6000);
	at4("MA"); listen(6000);
	at4("MY"); listen(3000);
	at4("MD"); listen(12000);
	logf("done; keep-alives answered: %u\n", pongs);
	return 0;
}
