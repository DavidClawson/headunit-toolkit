/* fw.h: running as boot firmware (loaded from NOR by the boot0 selector). */
#pragma once
int  fw_init(void);       /* call first: 1 if the selector booted us, 0 under FEL */
int  fw_booted(void);     /* the value fw_init returned */
void fw_boot_ok(void);    /* we're up: clear TRYING; the selector's 8 s watchdog keeps running */
void fw_pet(void);        /* restart the watchdog count: call from the main loop (no-op under FEL) */
void fw_boot_stock(void); /* boot stock firmware once: set STOCK1, reset. Doesn't return. */
