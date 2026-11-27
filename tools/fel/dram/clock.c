/*
 * clock.c: compare three clocks without ever waiting on one of them.
 * Kept as the regression test for the mcycle upper-word jumps (see lib.h).
 * Runs a fixed number of loop iterations and reports:
 *   - mcycle delta (CPU cycle counter)
 *   - TIMER0 delta (24 MHz hardware down-counter at 0x02050000)
 *   - cpu_hz() from the PLL registers
 * The host times the whole run too (felrun.py prints it).
 */
#include "lib.h"

#define TMR_BASE   0x02050000u
#define TMR0_CTRL  (TMR_BASE + 0x10)
#define TMR0_INTV  (TMR_BASE + 0x14)
#define TMR0_CUR   (TMR_BASE + 0x18)

int main(void)
{
	log_reset();
	logf("PLL_CPU=%08x RISCV_CLK=%08x cpu_hz=%lu\n", rd32(0x02001000), rd32(0x02001D00), cpu_hz());

	wr32(TMR0_INTV, 0xFFFFFFFF);
	wr32(TMR0_CTRL, (1u << 2) | (1u << 1));        /* 24 MHz HOSC, /1, reload */
	wr32(TMR0_CTRL, (1u << 2) | (1u << 1) | 1u);   /* enable */


	for (int run = 0; run < 3; run++) {
		uint32_t n = 2000000u << run;             /* 2M, 4M, 8M iterations */
		uint32_t t0 = rd32(TMR0_CUR);
		uint64_t c0 = cycles();
		for (volatile uint32_t i = 0; i < n; i++)
			;
		uint64_t c1 = cycles();
		uint32_t t1 = rd32(TMR0_CUR);
		uint32_t ticks = t0 - t1;                  /* down-counter */
		uint64_t mhz = ticks ? (c1 - c0) * 24 / ticks : 0;
		logf("iters=%u mcycle=%lu timer24=%u (%u ms) -> mcycle rate %lu MHz\n",
		     n, c1 - c0, ticks, ticks / 24000, mhz);
	}
	wr32(TMR0_CTRL, 0);
	return 0;
}
