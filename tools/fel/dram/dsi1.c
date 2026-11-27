/*
 * dsi1.c: display bring-up stage 1 (docs/display.md): DSI command path.
 *
 * Clocks/resets for TCON LCD0, DPSS TOP and MIPI-DSI, PD0-PD9 to DSI, D-PHY
 * power-on (A100 variant), DSI controller in command mode, then reads a few
 * panel registers over DSI with bus turnaround. Nothing is written to the
 * panel except "set maximum return packet size".
 *
 * Register sequences ported from Linux: drivers/gpu/drm/sun4i/sun6i_mipi_dsi.c,
 * drivers/phy/allwinner/phy-sun6i-mipi-dphy.c, drivers/clk/sunxi-ng/ccu-sun20i-d1.c.
 */
#include "lib.h"
#include "dsi.h"

int main(void)
{
	log_reset();
	logf("dsi1: DSI command path, panel ID read\n");
	if (ccu_setup()) return 1;
	pins_setup();
	dsi_init(4);
	dphy_power_on(4, 420000000u);
	udelay(10000);
	logf("DSI CTL=%08x BASIC_CTL0=%08x  DPHY GCTL=%08x ANA3=%08x\n",
	     rd32(DSI), rd32(DSI + 0x10), rd32(DPHY), rd32(DPHY + 0x58));

	logf("set max return size 1: %s\n", short_write(0x37, 1, 0) ? "timeout" : "sent");
	static const uint8_t regs[] = { 0x0A, 0x0B, 0x0C, 0xDA, 0xDB, 0xDC, 0x04 };
	for (unsigned i = 0; i < sizeof regs; i++)
		read_reg(0x06, regs[i]);                    /* DCS read, no params */
	logf("generic read (0x14) of DA/DB/DC:\n");
	for (unsigned i = 3; i < 6; i++)
		read_reg(0x14, regs[i]);
	return 0;
}
