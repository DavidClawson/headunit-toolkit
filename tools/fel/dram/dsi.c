/*
 * dsi.c: MIPI-DSI command path for the W022 (see docs/display.md).
 * Ported from Linux sun6i_mipi_dsi.c, phy-sun6i-mipi-dphy.c, ccu-sun20i-d1.c.
 */
#include "lib.h"
#include "dsi.h"


/* ---- CCU ---- */
int ccu_setup(void)
{
	/* PLL_VIDEO0: 4x = 24 MHz * 70 = 1680 MHz, so pll-video0 = 420 MHz.
	   bit31 enable, bit30 LDO, bit29 lock enable, bit27 output gate. */
	wr32(CCU + 0x040, (1u << 31) | (1u << 30) | (1u << 29) | ((70 - 1) << 8));
	uint32_t t0 = ticks();
	while (!(rd32(CCU + 0x040) & (1u << 28)))
		if (ticks() - t0 > 24000u * 20) { logf("PLL_VIDEO0 no lock\n"); return -1; }
	wr32(CCU + 0x040, rd32(CCU + 0x040) | (1u << 27));
	udelay(20);

	/* TCON LCD0 module clock: pll-video0 (mux 0), M=1, P=1 -> 420 MHz */
	wr32(CCU + 0xb60, (1u << 31) | (0u << 24));
	/* MIPI-DSI (D-PHY) module clock: pll-periph0 (600 MHz, mux 1) / 4 = 150 MHz */
	wr32(CCU + 0xb24, (1u << 31) | (1u << 24) | (4 - 1));
	/* bus gates + de-assert resets: DPSS TOP, MIPI-DSI, TCON LCD0 */
	wr32(CCU + 0xabc, (1u << 16) | 1u);
	wr32(CCU + 0xb4c, (1u << 16) | 1u);
	wr32(CCU + 0xb7c, (1u << 16) | 1u);
	udelay(10);
	/* TCON TOP: open the DSI clock gate (TCON_TOP_GATE_SRC_REG bit 16) */
	wr32(TCON_TOP + 0x20, rd32(TCON_TOP + 0x20) | (1u << 16));
	logf("CCU: PLL_VIDEO0=%08x TCON_LCD0=%08x MIPI_DSI=%08x TOP_GATE=%08x\n",
	     rd32(CCU + 0x040), rd32(CCU + 0xb60), rd32(CCU + 0xb24), rd32(TCON_TOP + 0x20));
	return 0;
}

/* ---- pins: PD0-PD9 function 4 (DSI) ---- */
void pins_setup(void)
{
	wr32(0x02000090, 0x44444444);                               /* PD0-PD7 */
	wr32(0x02000094, (rd32(0x02000094) & ~0xFFu) | 0x44);       /* PD8, PD9 */
}

/* ---- D-PHY (TX, A100 variant), hs_clk_rate = 70 MHz * 24 bpp / 4 lanes = 420 MHz ---- */
void dphy_power_on(unsigned lanes, uint32_t hs_hz)
{
	uint32_t lm = (1u << lanes) - 1;
	wr32(DPHY + 0x04, 1u << 28);                                         /* TX_CTL: HS clk cont */
	wr32(DPHY + 0x10, 14 | (6 << 16) | (10u << 24));                     /* TIME0 */
	wr32(DPHY + 0x14, 7 | (50 << 8) | (3 << 16) | (10u << 24));          /* TIME1 */
	wr32(DPHY + 0x18, 30);                                               /* TIME2 */
	wr32(DPHY + 0x1c, 0);
	wr32(DPHY + 0x20, 3 | (3 << 8));                                     /* TIME4 */

	/* sun50i_a100_mipi_dphy_tx_power_on */
	wr32(DPHY + 0x5c, (2u << 25) | (4u << 20) | (3u << 17) | (3u << 12) | (1u << 8) |
	                  (1u << 10) | 2u | (3u << 2) | (2u << 4) | (3u << 6));
	wr32(DPHY + 0x54, rd32(DPHY + 0x54) | (1u << 4));                    /* EN_CK_CPU */
	wr32(DPHY + 0x54, rd32(DPHY + 0x54) | (1u << 1));                    /* ENIB */
	wr32(DPHY + 0x58, (1u << 18) | (1u << 25) | (1u << 24));             /* LDOR LDOC LDOD */
	wr32(DPHY + 0x4c, (4u << 4) | (1u << 2));                            /* PLR(4) SFB(1) */
	wr32(DPHY + 0x110, 1u);                                              /* COMBO EN_CP */
	unsigned ratio = (hs_hz + 264000000u - 1) / 264000000u, ob = 0;
	while ((1u << ob) < ratio) ob++;
	unsigned div = 16u >> ob;
	unsigned n = (unsigned)((uint64_t)hs_hz * div / 24000000u);
	wr32(DPHY + 0x104, (1u << 23) | (1u << 22) | (1u << 21) | (1u << 20) | (1u << 7) |
	                   (((div - 1) % 8) << 16) | ((n & 0xff) << 8) | (((div - 1) / 8) << 4) | 2u);
	wr32(DPHY + 0x10c, 0);                                               /* no SDM */
	wr32(DPHY + 0x5c, rd32(DPHY + 0x5c) | (1u << 31));                   /* EN_MIPI */
	wr32(DPHY + 0x110, rd32(DPHY + 0x110) | (1u << 3) | (1u << 1));      /* EN_MIPI, EN_COMBOLDO */
	wr32(DPHY + 0x118, 20);                                              /* HS_STOP_DLY */
	udelay(1);
	logf("DPHY PLL: div %u n %u -> VCO %u MHz\n", div, n, 24 * n);

	/* back in sun6i_dphy_tx_power_on */
	wr32(DPHY + 0x58, (rd32(DPHY + 0x58) & ~(0xFu << 28)) | (1u << 27) | (lm << 28));
	udelay(1);
	wr32(DPHY + 0x58, rd32(DPHY + 0x58) | (1u << 26));                   /* EN_DIV */
	udelay(1);
	wr32(DPHY + 0x54, rd32(DPHY + 0x54) | (1u << 4));
	udelay(1);
	wr32(DPHY + 0x50, rd32(DPHY + 0x50) | (1u << 31));                   /* VTTMODE */
	wr32(DPHY + 0x54, (rd32(DPHY + 0x54) & ~(0xFu << 24)) | (lm << 24)); /* EN_P2S_CPU */
	wr32(DPHY + 0x00, ((lanes - 1) << 4) | 1u);                          /* GCTL */
}

/* ---- DSI controller ---- */
enum { I_LP11 = 0, I_TBA, I_HSC, I_HSD, I_LPDT, I_HSCEXIT, I_NOP, I_DLY, I_END = 15 };
enum { M_STOP = 0, M_TBA, M_HS, M_ESCAPE, M_HSCEXIT, M_NOP };

static void inst(unsigned id, unsigned mode, int clk, unsigned data, unsigned pkt, unsigned esc)
{
	wr32(DSI + 0x20 + 4 * id, (mode << 28) | (esc << 24) | (pkt << 20) | (clk ? 1u << 4 : 0) | data);
}

void dsi_init(unsigned lanes)
{
	unsigned lm = (1u << lanes) - 1;
	wr32(DSI + 0x000, 1);                               /* CTL EN */
	wr32(DSI + 0x010, (1u << 16) | (1u << 17));         /* ECC, CRC */
	wr32(DSI + 0x060, 10);                              /* TRANS_START */
	wr32(DSI + 0x078, 0);
	inst(I_LP11, M_STOP, 1, lm, 0, 0);
	inst(I_TBA, M_TBA, 0, 1, 0, 0);
	inst(I_HSC, M_HS, 1, 0, 0, 0);
	inst(I_HSD, M_HS, 0, lm, 0, 0);
	inst(I_LPDT, M_ESCAPE, 0, 1, 1, 0);                 /* command packet, LPDT */
	inst(I_HSCEXIT, M_HSCEXIT, 1, 0, 0, 0);
	inst(I_NOP, M_STOP, 0, lm, 0, 0);
	inst(I_DLY, M_NOP, 1, lm, 0, 0);
	wr32(DSI + 0x04c, (I_NOP << 16) | (I_HSCEXIT << 20) | 1);   /* JUMP_CFG(0) */
	wr32(DSI + 0x2f8, 0xff);                            /* DEBUG_DATA */
}

static const uint32_t ecc_tab[6] = {
	0xF12CB7, 0xF2555B, 0x749A6D, 0xB8E38E, 0xDF03F0, 0xEFFC00,
};
static uint32_t ecc(uint32_t d)
{
	uint32_t e = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t x = d & ecc_tab[i], p = 0;
		while (x) { p ^= x & 1; x >>= 1; }
		e |= p << i;
	}
	return e;
}
static uint32_t hdr(uint8_t dt, uint8_t d0, uint8_t d1)
{
	uint32_t p = dt | (d0 << 8) | ((uint32_t)d1 << 16);
	return p | (ecc(p) << 24);
}

static int wait_inst(void)
{
	uint32_t t0 = ticks();
	while (rd32(DSI + 0x010) & 1)
		if (ticks() - t0 > 24000u * 10) { wr32(DSI + 0x010, rd32(DSI + 0x010) & ~1u); return -1; }
	return 0;
}

static void start(int rx)
{
	if (rx)
		wr32(DSI + 0x048, (I_LPDT << (4 * I_LP11)) | (I_DLY << (4 * I_LPDT)) |
		                  (I_TBA << (4 * I_DLY)) | (I_END << (4 * I_TBA)));
	else
		wr32(DSI + 0x048, (I_LPDT << (4 * I_LP11)) | (I_END << (4 * I_LPDT)));
	wr32(DSI + 0x010, rd32(DSI + 0x010) & ~1u);
	wr32(DSI + 0x010, rd32(DSI + 0x010) | 1u);
}

int short_write(uint8_t dt, uint8_t d0, uint8_t d1)
{
	wait_inst();
	wr32(DSI + 0x200, (1u << 26) | (1u << 25) | (1u << 9));
	wr32(DSI + 0x300, hdr(dt, d0, d1));
	wr32(DSI + 0x200, (rd32(DSI + 0x200) & ~0xFFu) | 3);
	start(0);
	return wait_inst();
}

/* returns rx word 0 (byte0 = response type, byte1 = data), or 0xFFFFFFFF on timeout */
uint32_t read_reg(uint8_t dt, uint8_t reg)
{
	wait_inst();
	wr32(DSI + 0x200, (1u << 26) | (1u << 25) | (1u << 9));
	wr32(DSI + 0x300, hdr(dt, reg, 0));
	wr32(DSI + 0x200, 3);
	start(1);
	if (wait_inst()) return 0xFFFFFFFF;
	uint32_t ctl = rd32(DSI + 0x200), w0 = rd32(DSI + 0x240), w1 = rd32(DSI + 0x244);
	logf("  read %02x (dt %02x): CMD_CTL=%08x RX0=%08x RX1=%08x", reg, dt, ctl, w0, w1);
	uint8_t t = w0 & 0xff;
	if (t == 0x21 || t == 0x11) logf("  -> %02x\n", (w0 >> 8) & 0xff);
	else if (t == 0x22 || t == 0x12) logf("  -> %02x %02x\n", (w0 >> 8) & 0xff, (w0 >> 16) & 0xff);
	else if (t == 0x02) logf("  -> error report %04x\n", (w0 >> 8) & 0xffff);
	else if (t == 0x1c || t == 0x1a) logf("  -> long response, %u bytes\n", (w0 >> 8) & 0xffff);
	else logf("  -> ?\n");
	return w0;
}

