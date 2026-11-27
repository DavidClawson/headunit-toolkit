/*
 * display.c: LCD pipeline bring-up and buffer flips (docs/display.md).
 * DSI/D-PHY/panel from dsi.c; DE2 mixer 0 ported from Linux sun8i_mixer.c,
 * sun8i_ui_layer.c, sun8i_tcon_top.c.
 */
#include "lib.h"
#include "dsi.h"
#include "display.h"

#define DE_CLK  0x05000000u
#define MIXER   0x05100000u
#define BLD     (MIXER + 0x1000)
#define UI_CH   (MIXER + 0x2000 + 1 * 0x1000) /* D1 mixer 0: ch0 = VI, ch1 = UI */
#define SIZE(w, h) ((((h) - 1) << 16) | ((w) - 1))

static void de_setup(uint32_t fb)
{
	/* CCU: DE module clock = pll-periph0-2x (1200 MHz) / 4 = 300 MHz; bus gate + reset */
	wr32(0x02001600, (1u << 31) | (0u << 24) | (4 - 1));
	wr32(0x0200160c, (1u << 16) | 1u);
	udelay(10);
	/* DE2 clock block (H5 layout): mixer0 mod gate, bus gate, reset */
	wr32(DE_CLK + 0x00, rd32(DE_CLK + 0x00) | 1u);
	wr32(DE_CLK + 0x04, rd32(DE_CLK + 0x04) | 1u);
	wr32(DE_CLK + 0x08, rd32(DE_CLK + 0x08) | 1u);
	udelay(10);

	/* sun8i_mixer_bind: clear the mixer unit, disable post-processing engines */
	for (uint32_t i = 0; i < 0x6000; i += 4) wr32(MIXER + i, 0);
	static const uint32_t off[] = { 0xa0000, 0xa2000, 0xa4000, 0xa6000, 0xa8000, 0xaa000, 0xb0000 };
	for (unsigned i = 0; i < sizeof off / 4; i++) wr32(MIXER + off[i], 0);

	/* sun8i_mixer_init */
	wr32(MIXER + 0x0, 1);                            /* GLOBAL_CTL: RT_EN */
	wr32(BLD + 0x88, 0xff000000);                    /* background black */
	wr32(BLD + 0x00, 1);                             /* PIPE_CTL: fill colour on pipe 0 */
	wr32(BLD + 0x04, 0xff000000);
	for (int i = 0; i < 2; i++) wr32(BLD + 0x90 + 4 * i, 0x03010301);

	/* sun8i_mixer_mode_set */
	wr32(MIXER + 0xc, SIZE(HACT, VACT));             /* GLOBAL_SIZE */
	wr32(BLD + 0x8c, SIZE(HACT, VACT));              /* OUTSIZE */

	/* UI layer: XRGB8888 (fmt 4), global alpha 0xff, combined alpha mode */
	wr32(UI_CH + 0x00, (0xffu << 24) | (4u << 8) | (2u << 1) | 1u);
	wr32(UI_CH + 0x04, SIZE(HACT, VACT));
	wr32(UI_CH + 0x08, 0);
	wr32(UI_CH + 0x0c, HACT * 4);                    /* pitch, bytes */
	wr32(UI_CH + 0x10, fb);
	wr32(UI_CH + 0x80, 0);                           /* high address bits */
	wr32(UI_CH + 0x88, SIZE(HACT, VACT));            /* overlay size */

	/* sun8i_mixer_commit: channel 1 -> pipe 0 */
	wr32(BLD + 0x0c, 0);                             /* ATTR_COORD(0) */
	wr32(BLD + 0x08, SIZE(HACT, VACT));              /* ATTR_INSIZE(0) */
	wr32(BLD + 0x80, 1u << 0);                       /* ROUTE: pipe 0 = channel 1 */
	wr32(BLD + 0x00, (1u << 8) | 1u);                /* PIPE_CTL: pipe 0 on + fill */
	wr32(MIXER + 0x8, 1);                            /* DBUFF: latch on next vblank */

	/* TCON TOP: mixer 0 -> TCON LCD0 (0); park mixer 1 on TCON TV0 (2) */
	logf("TCON_TOP PORT_SEL was %08x\n", rd32(TCON_TOP + 0x1c));
	wr32(TCON_TOP + 0x1c, (2u << 4) | 0u);
	logf("DE: CCU_DE=%08x DE_CLK %08x %08x %08x  GLOBAL_CTL=%08x\n", rd32(0x02001600),
	     rd32(DE_CLK), rd32(DE_CLK + 4), rd32(DE_CLK + 8), rd32(MIXER));
}

int display_running(void)
{
	uint32_t ctl = rd32(TCON + 0x40);
	return rd32(MIXER) == 1 && (ctl >> 31) && (ctl & 7) == 0 && (rd32(DSI + 0x010) & 1);
}

static void backlight_on(void)
{
	wr32(0x02000124, (rd32(0x02000124) & ~(0xFu << 28)) | (1u << 28));   /* PG15 out, high */
	wr32(0x02000130, rd32(0x02000130) | (1u << 15));
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 24)) | (1u << 24));   /* PD22 out, high */
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 22));
}

int display_up(uint32_t fb)
{
	if (display_running()) { display_flip(fb); return 0; }
	if (ccu_setup()) return -1;
	pins_setup();
	dsi_init(LANES);
	dphy_power_on(LANES, (uint32_t)PCLK_KHZ * 1000u * BPP / LANES);
	udelay(10000);
	panel_init_send();
	de_setup(fb);
	tcon_setup(0);                                   /* source 0 = display engine */
	dsi_video();
	start_hs();
	udelay(50000);
	backlight_on();
	return display_running() ? 0 : -1;
}

void display_flip(uint32_t fb)
{
	wr32(UI_CH + 0x10, fb);
	wr32(MIXER + 0x8, 1);                            /* latch at the next vblank */
	uint32_t t0 = ticks();
	while ((rd32(MIXER + 0x8) & 1) && ticks() - t0 < 24000u * 50) ;
}

uint32_t display_fb(void) { return rd32(UI_CH + 0x10); }
