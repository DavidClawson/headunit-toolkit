/*
 * memtest.c: check the 64 MB of DRAM after `xfel ddr f133`.
 * Tests everything from the end of this program to the stack (about 63 MB):
 * data-bus walking ones, an address test (each word holds its own address),
 * then 0x55../0xAA.. patterns. Reports the first few failures.
 */
#include "lib.h"

extern char __image_end[];
#define TOP 0x43F00000ul

static unsigned errors;

static void fail(const char *test, uintptr_t a, uint64_t want, uint64_t got)
{
	if (errors++ < 8)
		logf("  %s: @%lx want %016lx got %016lx\n", test, (uint64_t)a, want, got);
}

int main(void)
{
	log_reset();
	uintptr_t lo = ((uintptr_t)__image_end + 0xFFFF) & ~0xFFFFul;
	volatile uint64_t *p;
	logf("memtest %lx..%lx (%lu MB)\n", (uint64_t)lo, (uint64_t)TOP, (uint64_t)(TOP - lo) >> 20);
	uint32_t t0 = ticks();

	p = (volatile uint64_t *)lo;
	for (int b = 0; b < 64; b++) {
		*p = 1ull << b;
		if (*p != 1ull << b) fail("databus", lo, 1ull << b, *p);
	}

	for (p = (volatile uint64_t *)lo; (uintptr_t)p < TOP; p++) *p = (uintptr_t)p;
	for (p = (volatile uint64_t *)lo; (uintptr_t)p < TOP; p++)
		if (*p != (uintptr_t)p) fail("address", (uintptr_t)p, (uintptr_t)p, *p);

	static const uint64_t pat[2] = { 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull };
	for (int k = 0; k < 2; k++) {
		for (p = (volatile uint64_t *)lo; (uintptr_t)p < TOP; p++) *p = pat[k] ^ (uintptr_t)p;
		for (p = (volatile uint64_t *)lo; (uintptr_t)p < TOP; p++)
			if (*p != (pat[k] ^ (uintptr_t)p)) fail(k ? "pattern-aa" : "pattern-55", (uintptr_t)p, pat[k] ^ (uintptr_t)p, *p);
	}

	logf("%s: %u errors, %u ms (CPU %lu MHz)\n", errors ? "FAIL" : "PASS", errors, elapsed_ms(t0), cpu_hz() / 1000000);
	return errors != 0;
}
