/*
 * hang.c: boot-fallback test image. Shows a red screen, then hangs without
 * calling fw_boot_ok(), so the selector's 8 s watchdog must reset the unit and
 * the next boot must fall back to stock (RTC GP5 = TRYING).
 *
 *   mkfw.py hang.bin && fw-write.sh dram/hang.fw
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "fw.h"

int main(void)
{
	int nor = fw_init();
	if (display_up(FB0) == 0) {
		gfx_target(FB0);
		gfx_clear(0x8a1010);
		const char *m = nor ? "HANG TEST: the watchdog should reset this in 8 s" : "HANG TEST (under FEL: not hanging)";
		gfx_text((GW - gfx_text_w(m, &font16x32, 1)) / 2, GH / 2 - 16, m, &font16x32, 1, 0xffffff);
	}
	if (!nor) return 0;
	for (;;) ;
}
