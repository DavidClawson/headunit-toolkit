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
	return (f->cmd == 0xB2 && f->len == 6) ? f->data[1] : -1;
}
