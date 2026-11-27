/* fw.c: see fw.h and ../bootsel/fwboot.h (the contract with the selector). */
#include "lib.h"
#include "fw.h"
#include "../bootsel/fwboot.h"

static int booted;

int fw_init(void) { booted = rd32(RTC_GP5) == (uint32_t)GP5_TRYING; return booted; }
int fw_booted(void) { return booted; }

void fw_boot_ok(void)
{
	if (!booted) return;
	wr32(WDT_MODE, WDT_KEY);                 /* disarm */
	wr32(RTC_GP5, 0);
}

void fw_boot_stock(void)
{
	wr32(RTC_GP5, GP5_STOCK1);
	wr32(WDT_CFG, WDT_KEY | 1);              /* timeout resets the whole system */
	wr32(WDT_MODE, WDT_KEY | 1);             /* 0.5 s */
	for (;;) ;
}
