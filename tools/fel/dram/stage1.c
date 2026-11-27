/*
 * stage1.c: the boot0 selector's image (NOR 0x120000, linked at 0x40000000).
 * Loads stage two, the full firmware, from NOR S2_ADDR to S2_LOAD, checks it
 * exactly as the selector checks us (magic, version, load address, size, additive
 * checksum) and jumps to it. Any failure boots stock once (fw_boot_stock): the same
 * outcome as a bad image at 0x120000, so the unit always comes up.
 * GP5 stays TRYING and the selector's 8 s watchdog keeps running: stage two
 * calls fw_boot_ok() / fw_pet() exactly as the single-image firmware did.
 * Under FEL (not booted by the selector) it logs the reason and returns instead.
 */
#include "lib.h"
#include "fw.h"
#include "spinor.h"
#include "../bootsel/fwboot.h"

static uint32_t hdr[128];

static int fail(const char *why, uint32_t v)
{
	logf("stage1: %s (%x): booting stock\n", why, v);
	if (fw_booted()) fw_boot_stock();
	return 1;
}

int main(void)
{
	fw_init();
	log_reset();
	spinor_init();
	uint32_t t0 = millis();
	if (spinor_read(S2_ADDR, hdr, sizeof hdr)) return fail("header read", 0);
	if (hdr[0] != S2_MAGIC) return fail("magic", hdr[0]);
	if (hdr[1] != S2_VERSION) return fail("version", hdr[1]);
	if (hdr[2] != S2_LOAD) return fail("load address", hdr[2]);
	uint32_t size = hdr[3];
	if (!size || size > S2_MAX || (size & 3)) return fail("size", size);
	if (spinor_read(S2_ADDR + 512, (void *)(uintptr_t)S2_LOAD, size)) return fail("program read", size);
	uint32_t sum = 0;
	for (const uint32_t *p = (const uint32_t *)(uintptr_t)S2_LOAD; p < (const uint32_t *)(uintptr_t)(S2_LOAD + size); p++)
		sum += *p;
	if (sum != hdr[4]) return fail("checksum", sum);
	logf("stage1: stage two ok, %u bytes in %u ms\n", size, millis() - t0);
	dcache_flush();                                          /* in case boot0 left the D-cache on */
	__asm__ volatile("fence\n\tfence.i" ::: "memory");      /* the I-cache must see the new code */
	((void (*)(void))(uintptr_t)S2_LOAD)();
	return fail("stage two returned", 0);
}
