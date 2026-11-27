/*
 * uart.c: UART transmit + timed receive payload for FEL, driven by mcu.py.
 *
 * Loaded at 0x40000. Host fills the request block at 0x40F00, runs
 * `xfel exec 0x40000`, reads it back. Sets up UART n (16550-style, 24 MHz
 * APB1 in FEL) on the given pins, sends tx[], then records received bytes for
 * rx_ms milliseconds: each entry at 0x41000 is (ms << 8) | byte.
 * Timing from TIMER1 (24 MHz crystal), not mcycle (unreliable, see dram/lib.h).
 */
#include <stdint.h>

struct req {
	uint32_t uart;      /* UART number (1 = MCU link) */
	uint32_t divisor;   /* 24 MHz / (16 * baud): 13 = 115200 */
	uint32_t ntx;       /* bytes in tx[] to send first */
	uint32_t rx_ms;     /* then listen this long */
	uint32_t nrx;       /* out: entries recorded */
	uint32_t status;    /* out: 0 ok, 0xEE untouched */
	uint32_t pad[2];
	uint8_t  tx[64];
};
#define REQ  ((volatile struct req *)0x40F00)
#define RX   ((volatile uint32_t *)0x41000)
#define RX_MAX 4096

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#define TMR1_CTRL 0x02050020u
#define TMR1_INTV 0x02050024u
#define TMR1_CUR  0x02050028u
static uint32_t ticks(void) { return ~rd(TMR1_CUR); }

__attribute__((section(".text.start"))) void _start(void)
{
	volatile struct req *r = REQ;
	uintptr_t base = 0x02500000u + 0x400u * r->uart;

	wr(TMR1_CTRL, 0);
	wr(TMR1_INTV, 0xFFFFFFFFu);
	wr(TMR1_CTRL, (1u << 2) | (1u << 1));
	wr(TMR1_CTRL, (1u << 2) | (1u << 1) | 1u);

	/* clock gate + de-assert reset (UART_BGR 0x0200190C: bit n, bit 16+n) */
	wr(0x0200190C, rd(0x0200190C) | (1u << r->uart) | (1u << (16 + r->uart)));

	wr(base + 0x08, 0x07);                  /* FCR: enable + reset FIFOs */
	wr(base + 0x0C, 0x80);                  /* LCR: DLAB */
	wr(base + 0x00, r->divisor & 0xFF);
	wr(base + 0x04, r->divisor >> 8);
	wr(base + 0x0C, 0x03);                  /* 8N1 */
	wr(base + 0x04, 0);                     /* IER off */
	while (rd(base + 0x14) & 1) (void)rd(base);   /* drain */

	for (uint32_t i = 0; i < r->ntx && i < 64; i++) {
		uint32_t t0 = ticks();
		while (!(rd(base + 0x14) & (1u << 5)))  /* THRE */
			if (ticks() - t0 > 24000 * 10) break;
		wr(base + 0x00, r->tx[i]);
	}

	uint32_t n = 0, t0 = ticks(), lim = r->rx_ms * 24000u;
	while (ticks() - t0 < lim && n < RX_MAX)
		if (rd(base + 0x14) & 1)
			RX[n++] = (((ticks() - t0) / 24000u) << 8) | (rd(base) & 0xFF);
	r->nrx = n;
	r->status = 0;
}
