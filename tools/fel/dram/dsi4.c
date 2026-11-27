/*
 * dsi4.c: display bring-up stage 4 (docs/display.md): framebuffer.
 * Stage 3's video path with the TCON fed by DE2 mixer 0 instead of its
 * pattern generator. One UI layer (channel 1, layer 0) scans out a 720x1280
 * XRGB8888 buffer at FB in DRAM. Draws an orientation test image.
 *
 * After it returns the pipeline keeps running: anything written to FB over
 * FEL (xfel write) shows up on the next frame.
 * Ported from Linux sun8i_mixer.c, sun8i_ui_layer.c, sun8i_tcon_top.c.
 */
#include "lib.h"
#include "dsi.h"

#define FB      0x41000000u                  /* 720 * 1280 * 4 = 3.5 MiB */
#define DE_CLK  0x05000000u
#define MIXER   0x05100000u
#define BLD     (MIXER + 0x1000)
#define UI_CH   (MIXER + 0x2000 + 1 * 0x1000) /* D1 mixer 0: ch0 = VI, ch1 = UI */
#define SIZE(w, h) ((((h) - 1) << 16) | ((w) - 1))

static void de_setup(void)
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
	wr32(UI_CH + 0x10, FB);
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

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB;
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			fb[y * HACT + x] = c;
}

static void draw_test(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB;
	for (int y = 0; y < VACT; y++)                   /* background: grey ramp down the buffer */
		for (int x = 0; x < HACT; x++) {
			uint32_t g = (uint32_t)(y * 160 / VACT) + 16;
			fb[y * HACT + x] = (g << 16) | (g << 8) | g;
		}
	for (int i = 0; i < 6; i++) {                    /* white border, 6 px */
		fill(0, i, HACT, 1, 0xffffff); fill(0, VACT - 1 - i, HACT, 1, 0xffffff);
		fill(i, 0, 1, VACT, 0xffffff); fill(HACT - 1 - i, 0, 1, VACT, 0xffffff);
	}
	fill(20, 20, 160, 160, 0xff0000);                    /* first pixel corner: red */
	fill(HACT - 180, 20, 160, 160, 0x00ff00);            /* end of first line: green */
	fill(20, VACT - 180, 160, 160, 0x0000ff);            /* start of last line: blue */
	fill(HACT - 180, VACT - 180, 160, 160, 0xffff00);    /* last pixel corner: yellow */
	for (int i = 0; i < 8; i++)                          /* colour bars across the middle */
		fill(40 + i * 80, VACT / 2 - 100, 80, 200,
		     ((i & 4) ? 0xff0000 : 0) | ((i & 2) ? 0x00ff00 : 0) | ((i & 1) ? 0x0000ff : 0));
	dcache_flush();
}

int main(void)
{
	log_reset();
	logf("dsi4: DE2 framebuffer at %08x, mhcr=%08x\n", FB, mhcr());
	draw_test();
	if (ccu_setup()) return 1;
	pins_setup();
	dsi_init(LANES);
	dphy_power_on(LANES, (uint32_t)PCLK_KHZ * 1000u * BPP / LANES);
	udelay(10000);
	panel_init_send();
	read_reg(0x06, 0x0A);
	de_setup();
	tcon_setup(0);                                   /* source 0 = display engine */
	dsi_video();
	start_hs();
	udelay(200000);
	logf("TCON GINT0=%08x  mixer GLOBAL_STATUS=%08x DBUFF=%08x\n",
	     rd32(TCON + 0x04), rd32(MIXER + 0x4), rd32(MIXER + 0x8));
	wr32(0x02000124, (rd32(0x02000124) & ~(0xFu << 28)) | (1u << 28));   /* backlight */
	wr32(0x02000130, rd32(0x02000130) | (1u << 15));
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 24)) | (1u << 24));
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 22));
	logf("video running from the framebuffer\n");
	return 0;
}
