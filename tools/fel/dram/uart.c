/* uart.c: see uart.h. Ported from tools/fel/uart.c (the SRAM payload). */
#include "lib.h"
#include "uart.h"

#define BASE(n) (0x02500000u + 0x400u * (n))

void uart_init(unsigned n, unsigned baud)
{
	if (n == 1) {                                     /* PG12/PG13 -> UART1, pull-ups */
		wr32(0x02000124, (rd32(0x02000124) & ~(0xFFu << 16)) | (0x77u << 16));
		wr32(0x02000144, (rd32(0x02000144) & ~(0xFu << 24)) | (1u << 24) | (1u << 26));
	}
	if (n == 4) {                                     /* PB2/PB3 -> UART4 (fn 7), pull-ups: the BT link */
		wr32(0x02000030, (rd32(0x02000030) & ~(0xFFu << 8)) | (0x77u << 8));
		wr32(0x02000054, (rd32(0x02000054) & ~(0xFu << 4)) | (1u << 4) | (1u << 6));
	}
	wr32(0x0200190C, rd32(0x0200190C) | (1u << n) | (1u << (16 + n)));
	uint32_t b = BASE(n), div = (24000000u + 8u * baud) / (16u * baud);
	wr32(b + 0x08, 0x07);                             /* FIFOs on + reset */
	wr32(b + 0x0C, 0x80);                             /* DLAB */
	wr32(b + 0x00, div & 0xFF);
	wr32(b + 0x04, div >> 8);
	wr32(b + 0x0C, 0x03);                             /* 8N1 */
	wr32(b + 0x04, 0);                                /* no interrupts */
	while (rd32(b + 0x14) & 1) (void)rd32(b);
}

int uart_getc(unsigned n)
{
	uint32_t b = BASE(n);
	return (rd32(b + 0x14) & 1) ? (int)(rd32(b) & 0xFF) : -1;
}

void uart_putc(unsigned n, uint8_t c)
{
	uint32_t b = BASE(n), t0 = ticks();
	while (!(rd32(b + 0x14) & (1u << 5)) && ticks() - t0 < 10000u * TICKS_PER_US) ;
	wr32(b, c);
}
