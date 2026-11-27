/*
 * dsi2.c: display bring-up stage 2 (docs/display.md): panel init.
 * Stage 1 setup, then the 212-entry [mipi_reg] sequence from sys_config
 * (DCS short writes), sleep out, display on, backlight on. Reads the power
 * mode before and after: 0x08 (sleeping, off) should become 0x9C (on).
 */
#include "lib.h"
#include "dsi.h"
#include "panel_init.h"

static void backlight_on(void)
{
	/* PG15 = 1 (enable), PD22 = 1 as a plain output (full brightness, no PWM). */
	wr32(0x02000124, (rd32(0x02000124) & ~(0xFu << 28)) | (1u << 28));       /* PG_CFG1 PG15 out */
	wr32(0x02000130, rd32(0x02000130) | (1u << 15));
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 24)) | (1u << 24));       /* PD_CFG2 PD22 out */
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 22));
}

int main(void)
{
	log_reset();
	logf("dsi2: panel init\n");
	if (ccu_setup()) return 1;
	pins_setup();
	dsi_init(4);
	dphy_power_on(4, 420000000u);
	udelay(10000);
	short_write(0x37, 1, 0);
	read_reg(0x06, 0x0A);

	unsigned sent = 0, fail = 0;
	for (unsigned i = 0; i < sizeof panel_init / 2; i++) {
		uint8_t r = panel_init[i][0], v = panel_init[i][1];
		if (r == 0xFF) { udelay(v * 1000u); continue; }
		int bad = (r == 0x11 || r == 0x29) ? short_write(0x05, r, 0)   /* no-parameter DCS */
		                                   : short_write(0x15, r, v);  /* DCS, 1 parameter */
		sent++; fail += bad != 0;
	}
	logf("sent %u commands, %u timeouts\n", sent, fail);
	udelay(20000);
	read_reg(0x06, 0x0A);
	read_reg(0x06, 0x0B);
	backlight_on();
	logf("backlight on\n");
	return 0;
}
