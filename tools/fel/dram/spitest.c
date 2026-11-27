/* spitest.c: READ-ONLY check of spinor.c (FEL). JEDEC ID, then known content:
   boot0's eGON header at 0, our "HUFW" image at 0x120000, the settings sector. */
#include "lib.h"
#include "../bootsel/fwboot.h"
#include "spinor.h"

static void dump(const char *what, uint32_t a)
{
	uint8_t b[16];
	int e = spinor_read(a, b, 16);
	logf("  %s at %x (err %d):", what, a, e);
	for (int i = 0; i < 16; i++) logf(" %02x", b[i]);
	logf("\n");
}

int main(void)
{
	log_reset();
	logf("SPI NOR read-only test\n");
	spinor_init();
	uint8_t id[3] = {0};
	int e = spinor_id(id);
	logf("  JEDEC ID (err %d): %02x %02x %02x\n", e, id[0], id[1], id[2]);
	dump("boot0 (eGON.BT0)", 0x0);
	dump("Melis package", 0x10000);
	dump("our image (HUFW)", 0x120000);
	dump("settings sector", FW_SETTINGS);
	dump("MINFS", 0x130000);
	/* a long read across chunks, to compare with xfel later */
	static uint8_t big[4096];
	uint32_t t0 = millis();
	e = spinor_read(0x120000, big, sizeof big);
	uint32_t sum = 0;
	for (unsigned i = 0; i < sizeof big; i++) sum = sum * 31 + big[i];
	logf("  4 KB read at 0x120000: err %d, %u ms, hash %08x\n", e, millis() - t0, sum);
	return 0;
}
