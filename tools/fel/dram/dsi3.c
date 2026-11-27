/*
 * dsi3.c: display bring-up stage 3 (docs/display.md): TCON test pattern over
 * DSI video. Stage 2 (panel init), then TCON LCD0 in CPU/DSI mode with its
 * built-in pattern generator, DSI in non-burst video mode, HS clock + data.
 *
 *   felrun.py dram/dsi3.bin           (pattern 1 = colour check)
 * Ported from Linux sun4i_tcon.c (mode_set_cpu) and sun6i_mipi_dsi.c.
 */
#include "lib.h"
#include "dsi.h"

int main(void)
{
	log_reset();
	logf("dsi3: TCON pattern over DSI video\n");
	if (ccu_setup()) return 1;
	pins_setup();
	dsi_init(LANES);
	dphy_power_on(LANES, (uint32_t)PCLK_KHZ * 1000u * BPP / LANES);
	udelay(10000);
	panel_init_send();
	read_reg(0x06, 0x0A);
	tcon_setup(1);
	dsi_video();
	start_hs();
	udelay(200000);
	logf("DSI BASIC_CTL0=%08x CTL=%08x CMD_CTL=%08x DEBUG=%08x\n",
	     rd32(DSI + 0x010), rd32(DSI), rd32(DSI + 0x200), rd32(DSI + 0x2f8));
	logf("TCON GINT0=%08x\n", rd32(TCON + 0x04));
	/* backlight on (PG15, PD22 high) */
	wr32(0x02000124, (rd32(0x02000124) & ~(0xFu << 28)) | (1u << 28));
	wr32(0x02000130, rd32(0x02000130) | (1u << 15));
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 24)) | (1u << 24));
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 22));
	logf("video running, backlight on\n");
	return 0;
}
