/*
 * radio.c: first radio-audio test (FEL). Every step logs; nothing is written
 * to flash. Sequence, from the stock app (init.axf, see docs/radio.md):
 *   1. MCU source = Radio: 02 80 A7 02 04 00 21 on UART1 (believed to power
 *      the tuner), then log what the MCU says for a second.
 *   2. TWI1 scan; Si4730 expected at 0x11 once powered.
 *   3. Si4730: reset (PB5 low 100 ms, high 50 ms), POWER_UP FM, GET_REV,
 *      the app's 31 properties + blend, FM_TUNE_FREQ 101.1 MHz, tune status.
 *   4. BD3702 (TWI2 0x40): the app's init table, input 0x05 = 0x0B (radio),
 *      volume 0x20 = 0x8A (-10 dB).
 * The chips keep their settings after this returns, so the audio keeps playing.
 */
#include "lib.h"
#include "twi.h"
#include "uart.h"

#define SI   0x11
#define BD   0x40
#define FREQ 10110      /* 10 kHz units */

static void mcu_send(uint8_t cmd, const uint8_t *d, unsigned n)
{
	uint8_t x = 0x80 ^ cmd ^ (uint8_t)n;
	uart_putc(1, 0x02); uart_putc(1, 0x80); uart_putc(1, cmd); uart_putc(1, (uint8_t)n);
	for (unsigned i = 0; i < n; i++) { uart_putc(1, d[i]); x ^= d[i]; }
	uart_putc(1, x);
}

static unsigned acks;

/* log what the MCU sends; answer its keep-alive (81 B2 [00 CE ..]) with
   B0 [CE 00 00 00 00], as the app's key forwarder does (init.axf 0xe918cfc4) */
static void mcu_listen(unsigned ms)
{
	static const uint8_t ping[6] = { 0x02, 0x81, 0xB2, 0x06, 0x00, 0xCE };
	static const uint8_t pong[5] = { 0xCE, 0x00, 0x00, 0x00, 0x00 };
	static unsigned m;
	static int pending;
	uint32_t t0 = ticks();
	unsigned n = 0;
	while (elapsed_ms(t0) < ms) {
		int c = uart_getc(1);
		if (c < 0) continue;
		m = (c == ping[m]) ? m + 1 : (c == ping[0]);
		if (m == 6) { m = 0; pending = 6; }        /* rest of the frame: 4 data + xor */
		else if (pending && --pending == 1) { pending = 0; mcu_send(0xB0, pong, 5); acks++; }
		if (n % 16 == 0) logf("%s  mcu:", n ? "\n" : "");
		logf(" %02x", c);
		n++;
	}
	logf(n ? "\n  (%u bytes from the MCU)\n" : "  mcu: silent\n", n);
}

static void scan(unsigned bus)
{
	logf("  TWI%u:", bus);
	unsigned found = 0;
	for (unsigned a = 0x08; a < 0x78; a++)
		if (twi_write(bus, (uint8_t)a, 0, 0, 0, 0) == TWI_OK) { logf(" %02x", a); found++; }
	logf(found ? "\n" : " nothing\n");
}

/* send a command, wait for CTS, read the response (status byte first) */
static int si_cmd(const uint8_t *c, unsigned n, uint8_t *resp, unsigned rn)
{
	int e = twi_write(1, SI, 0, 0, c, n);
	if (e) return e;
	uint8_t st[16];
	uint32_t t0 = ticks();
	for (;;) {
		e = twi_read(1, SI, 0, 0, st, rn ? rn : 1);
		if (e) return e;
		if (st[0] & 0x80) break;
		if (elapsed_ms(t0) > 500) return 100;
		udelay(1000);
	}
	for (unsigned i = 0; i < rn; i++) resp[i] = st[i];
	return 0;
}

static int si_prop(uint16_t p, uint16_t v)
{
	uint8_t c[6] = { 0x12, 0, (uint8_t)(p >> 8), (uint8_t)p, (uint8_t)(v >> 8), (uint8_t)v };
	return si_cmd(c, 6, 0, 0);
}

static const uint16_t props[][2] = {
	{0x0001,0x0004},{0x0102,0x0000},{0x0104,0x0000},{0x0201,0x8000},{0x0202,0x0001},
	{0x1100,0x0001},{0x1105,0x0014},{0x1106,0x0014},{0x1107,0x0000},{0x1108,0x001E},
	{0x1200,0x008F},{0x1201,0x001E},{0x1202,0x0006},{0x1203,0x0032},{0x1204,0x0018},
	{0x1205,0x0000},{0x1206,0x0000},{0x1207,0x00B2},{0x1300,0x0040},{0x1302,0x0020},
	{0x1303,0x0002},{0x1400,0x222E},{0x1401,0x2A30},{0x1402,0x000A},{0x1403,0x0005},
	{0x1404,0x0005},{0x1500,0x0000},{0x1501,0x000E},{0x1502,0x0001},{0x4000,0x003F},
	{0x4001,0x0000},{0x1800,0x000F},{0x1801,0x000A},
};

static const uint8_t bd_init[][2] = {
	{0x01,0xF4},{0x02,0x03},{0x03,0x19},{0x05,0x00},{0x06,0x06},{0x20,0x00},
	{0x28,0x80},{0x29,0x80},{0x2A,0x80},{0x2B,0x80},{0x2C,0xFF},{0x30,0xFF},
	{0x41,0x00},{0x44,0x00},{0x47,0x00},{0x51,0x00},{0x54,0x00},{0x57,0x00},
	{0x75,0x7F},{0x90,0x30},
};


int main(void)
{
	log_reset();
	logf("radio test: 101.1 MHz\n");

	logf("1. MCU: A0 (init), then source = Radio (A7 04 00)\n");
	uart_init(1, 115200);
	while (uart_getc(1) >= 0) ;
	/* the app's start-up list (init.axf 0xe92742c8), minus B3 (skipped when
	   bArmRadio=1), then its final A7 [0F 00], then source = Radio */
	static const uint8_t b7[1] = { 0x02 }, a7f[2] = { 0x0F, 0x00 }, src[2] = { 0x04, 0x00 };
	static const uint8_t ce[5] = { 0xCE, 0x01, 0x00, 0x00, 0x00 };
	logf(" A4\n"); mcu_send(0xA4, 0, 0);   mcu_listen(300);
	logf(" A0\n"); mcu_send(0xA0, 0, 0);   mcu_listen(1000);
	logf(" B7 02\n"); mcu_send(0xB7, b7, 1); mcu_listen(300);
	logf(" C3\n"); mcu_send(0xC3, 0, 0);   mcu_listen(500);
	logf(" BA\n"); mcu_send(0xBA, 0, 0);   mcu_listen(300);
	logf(" A7 0F 00\n"); mcu_send(0xA7, a7f, 2); mcu_listen(300);
	logf(" B0 CE 01 (host up)\n"); mcu_send(0xB0, ce, 5); mcu_listen(300);
	logf(" A7 04 00 (radio)\n"); mcu_send(0xA7, src, 2); mcu_listen(3000);

	logf("2. scan\n");
	twi_init(1);
	twi_init(2);
	scan(1);
	scan(2);

	logf("3. Si4730\n");
	wr32(0x02000030, (rd32(0x02000030) & ~(0xFu << 20)) | (1u << 20));   /* PB5 output */
	wr32(0x02000040, rd32(0x02000040) & ~(1u << 5));
	udelay(100000);
	wr32(0x02000040, rd32(0x02000040) | (1u << 5));
	udelay(50000);
	scan(1);
	for (int i = 0; i < 50 && twi_write(1, SI, 0, 0, 0, 0) != TWI_OK; i++) udelay(100000);
	static const uint8_t pu[3] = { 0x01, 0xD0, 0x05 };
	int e = si_cmd(pu, 3, 0, 0);
	logf("  POWER_UP: %d\n", e);
	if (e) { logf("  no tuner on TWI1 (expected on this board): skipping to the sound processor\n"); goto sound; }
	udelay(200000);
	uint8_t r[9] = {0};
	static const uint8_t rev[1] = { 0x10 };
	e = si_cmd(rev, 1, r, 9);
	logf("  GET_REV: %d  part Si47%02u, fw %c.%c, chip rev %c\n", e, r[1], r[2], r[3], r[8]);
	unsigned bad = 0;
	for (unsigned i = 0; i < sizeof props / sizeof props[0]; i++)
		if (si_prop(props[i][0], props[i][1])) { bad++; logf("  prop %04x failed\n", props[i][0]); }
	logf("  %u properties set, %u failed\n", (unsigned)(sizeof props / sizeof props[0]), bad);
	uint8_t tune[5] = { 0x20, 0x00, FREQ >> 8, FREQ & 0xFF, 0x00 };
	e = si_cmd(tune, 5, 0, 0);
	logf("  FM_TUNE_FREQ: %d\n", e);
	uint32_t t0 = ticks();
	for (;;) {
		static const uint8_t is[1] = { 0x14 };
		uint8_t s[1];
		if (si_cmd(is, 1, s, 1) == 0 && (s[0] & 1)) break;
		if (elapsed_ms(t0) > 1000) { logf("  no STC after 1 s\n"); break; }
		udelay(5000);
	}
	static const uint8_t ts[2] = { 0x22, 0x01 };
	uint8_t t[8] = {0};
	e = si_cmd(ts, 2, t, 8);
	logf("  FM_TUNE_STATUS: %d  freq %u (10 kHz), valid %u, RSSI %u dBuV, SNR %u dB, took %u ms\n",
	     e, (t[2] << 8) | t[3], t[1] & 1, t[4], t[5], elapsed_ms(t0));

sound:
	logf("4. BD3702\n");
	bad = 0;
	for (unsigned i = 0; i < sizeof bd_init / sizeof bd_init[0]; i++)
		if (twi_write(2, BD, bd_init[i][0], 1, &bd_init[i][1], 1)) bad++;
	uint8_t v = 0x0B; bad += twi_write(2, BD, 0x05, 1, &v, 1) != 0;
	v = 0x8A;         bad += twi_write(2, BD, 0x20, 1, &v, 1) != 0;
	logf("  %u writes, %u failed; input 0x0B (radio), volume -10 dB\n",
	     (unsigned)(sizeof bd_init / sizeof bd_init[0]) + 2, bad);
	logf("5. radio input for 20 s (answering keep-alives), then inputs 0..15, 2 s each\n");
	mcu_listen(20000);
	logf("  keep-alives answered so far: %u\n", acks);
	for (uint8_t in = 0; in < 16; in++) { twi_write(2, BD, 0x05, 1, &in, 1); mcu_listen(2000); }
	v = 0x0B; twi_write(2, BD, 0x05, 1, &v, 1);
	logf("  keep-alives answered: %u\n", acks);
	logf("done\n");
	return 0;
}
