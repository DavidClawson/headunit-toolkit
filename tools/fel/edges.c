/*
 * edges.c: poor man's logic analyser for FEL, driven by tools/fel/uart_sniff.py.
 *
 * Polls one GPIO data bit and records the CPU cycle count (mcycle) at every
 * level change, so the host can work out a UART's baud rate and decode bytes.
 * Only reads registers; the host sets the pin to input beforehand.
 *
 * Request block at 0x40F00 (u32 unless noted):
 *   +0x00 dat_addr   GPIO data register to sample
 *   +0x04 bit        bit number in it
 *   +0x08 max_edges  stop after this many edges (<= EDGE_MAX)
 *   +0x0C cycles     stop after this many mcycle ticks (u32, wraps fine)
 *   +0x10 out: n_edges
 *   +0x14 out: initial level
 *   +0x18 out: elapsed cycles
 *   +0x1C out: number of polls (to estimate sampling resolution)
 * Edge timestamps: u32 cycle offsets from the start, at 0x41000 (16 KB max,
 * kept clear of xfel's helper at 0x45000).
 */
#include <stdint.h>

#define REQ   ((volatile uint32_t *)0x40F00)
#define EDGES ((volatile uint32_t *)0x41000)
#define EDGE_MAX 4096u

static inline uint32_t cycles(void)
{
	uint64_t c;
	__asm__ volatile("csrr %0, mcycle" : "=r"(c));
	return (uint32_t)c;
}

__attribute__((section(".text.start")))
void _start(void)
{
	volatile uint32_t *dat = (volatile uint32_t *)(uintptr_t)REQ[0];
	uint32_t mask = 1u << REQ[1];
	uint32_t max = REQ[2] < EDGE_MAX ? REQ[2] : EDGE_MAX;
	uint32_t limit = REQ[3];
	uint32_t n = 0, polls = 0;

	uint32_t t0 = cycles();
	uint32_t level = *dat & mask;
	REQ[5] = level ? 1 : 0;
	uint32_t now = t0;

	while (n < max && (now - t0) < limit) {
		uint32_t v = *dat & mask;
		now = cycles();
		polls++;
		if (v != level) {
			level = v;
			EDGES[n++] = now - t0;
		}
	}
	REQ[4] = n;
	REQ[6] = now - t0;
	REQ[7] = polls;
}
