/* fw.c: see fw.h and ../bootsel/fwboot.h (the contract with the selector). */
#include "lib.h"
#include "fw.h"
#include "../bootsel/fwboot.h"

static int booted;

int fw_init(void) { booted = rd32(RTC_GP5) == (uint32_t)GP5_TRYING; return booted; }
int fw_booted(void) { return booted; }

/* From here a hang is no longer a boot failure: GP5 is cleared, so a watchdog
   reset boots ours again (self-healing) rather than falling back to stock. */
void fw_boot_ok(void)
{
	if (!booted) return;
	wr32(RTC_GP5, 0);
	fw_pet();
}

void fw_pet(void)
{
	if (booted) wr32(WDT_CTRL, WDT_RELOAD);
}

void fw_boot_stock(void)
{
	wr32(RTC_GP5, GP5_STOCK1);
	wr32(WDT_CFG, WDT_KEY | 1);              /* timeout resets the whole system */
	wr32(WDT_MODE, WDT_KEY | 1);             /* 0.5 s */
	for (;;) ;
}
