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
#include "panel_init.h"

#define TCON 0x05461000u

/* Panel mode (sys_config [lcd0]; Allwinner hbp/vbp include the sync pulse) */
#define HACT 720
#define HFP  140
#define HSW  8
#define HBP  40
#define HTOT 908
#define VACT 1280
#define VFP  22
#define VSW  6
#define VBP  9
#define VTOT 1317
#define PCLK_KHZ 70000
#define BPP 24
#define LANES 4

static uint32_t ecc24(uint32_t d)
{
	static const uint32_t t[6] = { 0xF12CB7, 0xF2555B, 0x749A6D, 0xB8E38E, 0xDF03F0, 0xEFFC00 };
	uint32_t e = 0;
	for (int i = 0; i < 6; i++) { uint32_t x = d & t[i], p = 0; while (x) { p ^= x & 1; x >>= 1; } e |= p << i; }
	return e;
}
static uint32_t sync_pkt(uint8_t dt, uint8_t d0, uint8_t d1)
{
	uint32_t v = (dt & 0x3f) | (d0 << 8) | ((uint32_t)d1 << 16);
	return v | (ecc24(v) << 24);
}
static uint16_t crc_zeros(unsigned len)          /* Linux crc_ccitt(0xffff, zeros, len) */
{
	uint16_t crc = 0xffff;
	while (len--) {
		crc ^= 0;
		for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}
static void blk(uint32_t reg0, unsigned wc)
{
	wr32(DSI + reg0, sync_pkt(0x19, wc & 0xff, wc >> 8));       /* blanking packet */
	wr32(DSI + reg0 + 4, ((uint32_t)crc_zeros(wc) << 16) | 0);   /* PF = CRC, PD = 0 */
}
static unsigned umax(unsigned a, unsigned b) { return a > b ? a : b; }

static void panel_init_send(void)
{
	short_write(0x37, 1, 0);
	for (unsigned i = 0; i < sizeof panel_init / 2; i++) {
		uint8_t r = panel_init[i][0], v = panel_init[i][1];
		if (r == 0xFF) { udelay(v * 1000u); continue; }
		if (r == 0x11 || r == 0x29) short_write(0x05, r, 0);
		else short_write(0x15, r, v);
	}
}

static void tcon_setup(unsigned pattern)
{
	unsigned div = 4;                                  /* SUN6I_DSI_TCON_DIV */
	wr32(TCON + 0x00, 0);                              /* GCTL off while configuring */
	wr32(TCON + 0x44, (0xFu << 28) | div);             /* DCLK: 420 MHz / 4 */
	wr32(TCON + 0x48, ((HACT - 1) << 16) | (VACT - 1));
	wr32(TCON + 0x40, (1u << 24) | (pattern & 7));     /* IF 8080 (CPU/DSI), source = pattern */
	wr32(TCON + 0xf8, 1u << 3);                        /* ECC FIFO */
	wr32(TCON + 0x60, (1u << 28) | (1u << 16) | (1u << 2) | 1u);
	unsigned space = HTOT * BPP / (div * LANES) - (HACT + 40);
	wr32(TCON + 0x160, ((space - 1) << 16) | (HACT - 1));
	wr32(TCON + 0x164, VACT - 1);
	unsigned sd = (VTOT - VACT - 10 - 1) * HTOT * 149 / (PCLK_KHZ / 1000) / 8;
	wr32(TCON + 0x168, ((sd & 0xffff) << 16) | 10);
	wr32(TCON + 0x1f0, (3000u << 16) | 3);
	wr32(TCON + 0x8c, 0xe0000000);
	wr32(TCON + 0x00, 1u << 31);                       /* GCTL enable */
	wr32(TCON + 0x40, rd32(TCON + 0x40) | (1u << 31)); /* TCON0 enable */
	logf("TCON: CTL=%08x DCLK=%08x CPU_IF=%08x TRI0=%08x TRI2=%08x\n",
	     rd32(TCON + 0x40), rd32(TCON + 0x44), rd32(TCON + 0x60), rd32(TCON + 0x160), rd32(TCON + 0x168));
}

static void dsi_video(void)
{
	unsigned Bpp = BPP / 8;
	unsigned hss = HACT + HFP, hse = hss + HSW;
	unsigned vss = VACT + VFP, vse = vss + VSW;
	unsigned delay = VTOT - (vss - VACT) + 1;
	wr32(DSI + 0x014, (delay << 4) | (1u << 2) | (1u << 1) | 1u);      /* BASIC_CTL1: video */
	unsigned drq = (HFP - 20) * BPP / 32;
	wr32(DSI + 0x07c, (1u << 28) | drq);                               /* TCON_DRQ */
	wr32(DSI + 0x040, (2u << 0) | (3u << (4 * 7)));                    /* INST_LOOP_SEL */
	wr32(DSI + 0x044, (49u << 16) | 49u);
	wr32(DSI + 0x054, (49u << 16) | 49u);
	uint32_t ph = (0x3E) | ((HACT * Bpp) << 8);                        /* RGB888, VC0 */
	wr32(DSI + 0x090, ph | (ecc24(ph) << 24));
	wr32(DSI + 0x098, 0xffff);
	wr32(DSI + 0x09c, 0xffffffffu);
	wr32(DSI + 0x080, (1u << 16) | 8);                                 /* PIXEL_CTL0: RGB888 */
	wr32(DSI + 0x00c, 0);                                              /* BASIC_CTL: non-burst */
	wr32(DSI + 0x0b0, sync_pkt(0x21, 0, 0));
	wr32(DSI + 0x0b4, sync_pkt(0x31, 0, 0));
	wr32(DSI + 0x0b8, sync_pkt(0x01, 0, 0));
	wr32(DSI + 0x0bc, sync_pkt(0x11, 0, 0));
	wr32(DSI + 0x018, (VSW) | ((VTOT - vse) << 16));
	wr32(DSI + 0x01c, VACT | (VTOT << 16));
	blk(0x0c0, umax(10, HSW * Bpp - 10));
	blk(0x0c8, umax(6, (HTOT - hse) * Bpp - 6));
	blk(0x0d0, umax(16, (hss - HACT) * Bpp - 16));
	blk(0x0e0, umax(10, (HTOT - HSW) * Bpp - 10));
	blk(0x0e8, 0);
}

static void start_hs(void)
{
	enum { I_LP11 = 0, I_TBA, I_HSC, I_HSD, I_LPDT, I_HSCEXIT, I_NOP, I_DLY, I_END = 15 };
	/* HS clock */
	wr32(DSI + 0x048, (I_HSC << (4 * I_LP11)) | (I_END << (4 * I_HSC)));
	wr32(DSI + 0x010, rd32(DSI + 0x010) & ~1u);
	wr32(DSI + 0x010, rd32(DSI + 0x010) | 1u);
	wr32(DSI + 0x020, rd32(DSI + 0x020) & ~(1u << 4));                 /* LP11 inst: clock lane off */
	udelay(1000);
	/* HS data loop */
	wr32(DSI + 0x048, (I_NOP << (4 * I_LP11)) | (I_HSD << (4 * I_NOP)) | (I_DLY << (4 * I_HSD)) |
	                  (I_NOP << (4 * I_DLY)) | (I_END << (4 * I_HSCEXIT)));
	wr32(DSI + 0x010, rd32(DSI + 0x010) & ~1u);
	wr32(DSI + 0x010, rd32(DSI + 0x010) | 1u);
}

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
