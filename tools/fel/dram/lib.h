/* Tiny support library for FEL DRAM programs: MMIO, timing, a log the host reads. */
#pragma once
#include <stdint.h>
#include <stddef.h>

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
/* Raw mcycle. WARNING: on the bench unit its upper 32 bits sometimes jump by
   whole multiples of 2^32 during long DRAM-heavy runs (seen after
   `xfel ddr f133`). Don't use it for timing; use ticks()/udelay() instead. */
static inline uint64_t cycles(void) { uint64_t c; __asm__ volatile("csrr %0, mcycle" : "=r"(c)); return c; }

/* Time base: TIMER1 (0x02050020) free-running from the 24 MHz crystal,
   started by start.S via lib_init(). ticks() counts UP, 24 per microsecond,
   wraps every ~179 s (unsigned subtraction handles one wrap). */
void lib_init(void);
uint32_t ticks(void);
#define TICKS_PER_US 24u
static inline uint32_t elapsed_ms(uint32_t t0) { return (ticks() - t0) / (TICKS_PER_US * 1000u); }

/* CPU clock from PLL_CPU (0x02001000): 408 MHz in plain FEL, 1008 MHz after
   `xfel ddr f133` (its DRAM payload also raises the CPU PLL). */
uint64_t cpu_hz(void);
void udelay(uint32_t us);

/* Log: magic "FLOG", u32 length, then text. tools/fel/felrun.py prints it. */
void log_reset(void);
void log_putc(char c);
void log_puts(const char *s);
void logf(const char *fmt, ...);   /* %s %c %d %u %x %lx %lu %p, width/zero-pad for x/d/u */

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
