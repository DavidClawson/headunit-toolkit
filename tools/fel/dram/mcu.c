/* mcu.c: see mcu.h. Frame: 02 | dir | cmd | len | data[len] | xor(dir..data). */
#include "lib.h"
#include "uart.h"
#include "mcu.h"

unsigned mcu_bad_frames;
static mcu_frame cur;
static unsigned pos, need;
static uint8_t x;

void mcu_init(void) { uart_init(1, 115200); pos = 0; }

int mcu_poll(mcu_frame *f)
{
	int c;
	while ((c = uart_getc(1)) >= 0) {
		uint8_t b = (uint8_t)c;
		switch (pos) {
		case 0: if (b == 0x02) pos = 1; continue;
		case 1: cur.dir = b; x = b; pos = 2; continue;
		case 2: cur.cmd = b; x ^= b; pos = 3; continue;
		case 3:
			if (b > sizeof cur.data) { mcu_bad_frames++; pos = 0; continue; }
			cur.len = b; x ^= b; need = b; pos = 4; continue;
		default:
			if (pos - 4 < need) { cur.data[pos - 4] = b; x ^= b; pos++; continue; }
			pos = 0;
			if (b != x) { mcu_bad_frames++; continue; }
			*f = cur;
			return 1;
		}
	}
	return 0;
}

int mcu_key(const mcu_frame *f)
{
	if (f->cmd != 0xB2 || f->len != 6 || f->data[1] == 0xCE || f->data[1] == 0xFE) return -1;
	return f->data[1];
}

/* 81 B2 [00 CE ..] = keep-alive ping: answer B0 [CE 00 00 00 00] (as the stock app's
   key forwarder does). 81 B2 [00 FE 00 00 t m] = the key LED setting echoed back. */
int mcu_service(const mcu_frame *f)
{
	if (f->cmd != 0xB2 || f->len != 6) return 0;
	if (f->data[1] == 0xCE) {
		static const uint8_t pong[5] = { 0xCE, 0, 0, 0, 0 };
		mcu_send(0xB0, pong, 5);
		return 1;
	}
	if (f->data[1] == 0xFE) { logf("mcu: key LED now mode %u\n", f->data[5]); return 1; }
	return 0;
}

void mcu_start(void)
{
	static const uint8_t b7[1] = { 0x02 }, a7[2] = { 0x0F, 0x00 }, up[5] = { 0xCE, 0x01, 0, 0, 0 };
	static const struct { uint8_t cmd, len; const uint8_t *d; } seq[] = {
		{ 0xA4, 0, 0 }, { 0xA0, 0, 0 }, { 0xB7, 1, b7 }, { 0xC3, 0, 0 }, { 0xBA, 0, 0 },
		{ 0xA7, 2, a7 }, { 0xB0, 5, up },
	};
	for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
		mcu_send(seq[i].cmd, seq[i].d, seq[i].len);
		uint32_t t0 = millis();
		mcu_frame f;
		while (millis() - t0 < (seq[i].cmd == 0xA0 ? 600u : 150u))    /* let it answer; keep pings answered */
			if (mcu_poll(&f)) mcu_service(&f);
	}
}

void mcu_send(uint8_t cmd, const void *data, unsigned len)
{
	const uint8_t *d = data;
	uint8_t x = 0x80 ^ cmd ^ (uint8_t)len;
	uart_putc(1, 0x02); uart_putc(1, 0x80); uart_putc(1, cmd); uart_putc(1, (uint8_t)len);
	for (unsigned i = 0; i < len; i++) { uart_putc(1, d[i]); x ^= d[i]; }
	uart_putc(1, x);
}

void mcu_key_led(uint8_t mode)
{
	uint8_t d[5] = { 0xFE, 0, 0, mode == LED_OFF ? 0 : 5, mode };
	mcu_send(0xB0, d, 5);
}
