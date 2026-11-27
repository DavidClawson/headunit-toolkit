/* spinor.c: see spinor.h. SPI0 at 4 MHz from the 24 MHz crystal, hardware chip
   select, one transfer per flash command (FIFO 64 bytes, so data goes in chunks). */
#include "lib.h"
#include "../bootsel/fwboot.h"
#include "spinor.h"

#define SPI   0x04025000u
#define GCR   (SPI + 0x04)
#define TCR   (SPI + 0x08)
#define ISR   (SPI + 0x14)
#define FCR   (SPI + 0x18)
#define FSR   (SPI + 0x1C)
#define MBC   (SPI + 0x30)
#define MTC   (SPI + 0x34)
#define BCC   (SPI + 0x38)
#define TXD   (SPI + 0x200)
#define RXD   (SPI + 0x300)
#define CHUNK 48

static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }

static uint32_t keep[6];

/* what we change, put back after each operation */
static void save_ctrl(void)
{
	keep[0] = rd32(0x02000060); keep[1] = rd32(0x02001940); keep[2] = rd32(0x0200196C);
	keep[3] = rd32(GCR); keep[4] = rd32(TCR); keep[5] = 1;
}
static void restore_ctrl(void)
{
	if (!keep[5]) return;
	wr32(TCR, keep[4]); wr32(GCR, keep[3]);
	wr32(0x02001940, keep[1]); wr32(0x02000060, keep[0]);
	keep[5] = 0;
}

static void setup(void)
{
	/* PC2 CLK, PC3 CS0, PC4 MOSI, PC5 MISO, PC6 WP, PC7 HOLD: function 2 */
	wr32(0x02000060, (rd32(0x02000060) & 0x000000FFu) | 0x22222200u);
	wr32(0x02001940, 0x80000005u);                     /* SPI0 clock: on, 24 MHz / 6 = 4 MHz */
	wr32(0x0200196C, rd32(0x0200196C) | 1u | (1u << 16));   /* bus gate + out of reset */
	wr32(GCR, 1u << 31);                               /* soft reset */
	for (int i = 0; i < 1000 && (rd32(GCR) & (1u << 31)); i++) udelay(1);
	wr32(GCR, (1u << 7) | (1u << 1) | 1u);             /* transmit pause, master, enable */
	wr32(TCR, (1u << 13) | (1u << 8) | (1u << 2));     /* normal sample, discard dummy rx, CS active low */
}

void spinor_init(void) { }                            /* setup happens per operation (begin/end) */
static void begin(void) { save_ctrl(); setup(); }
static void end(void) { restore_ctrl(); }

/* one command: ntx bytes out, then nrx bytes in (ntx + nrx <= 64) */
static int xfer(const uint8_t *tx, unsigned ntx, uint8_t *rx, unsigned nrx)
{
	wr32(FCR, (1u << 31) | (1u << 15));                /* reset both FIFOs */
	for (int i = 0; i < 1000 && (rd32(FCR) & ((1u << 31) | (1u << 15))); i++) udelay(1);
	wr32(MBC, ntx + nrx);
	wr32(MTC, ntx);
	wr32(BCC, ntx);
	for (unsigned i = 0; i < ntx; i++) wr8(TXD, tx[i]);
	wr32(ISR, 0xFFFFFFFFu);
	wr32(TCR, rd32(TCR) | (1u << 31));                 /* start */
	uint32_t t0 = millis();
	while (rd32(TCR) & (1u << 31))
		if (millis() - t0 > 50) return 1;
	for (unsigned i = 0; i < nrx; i++) {
		t0 = millis();
		while (!(rd32(FSR) & 0xFF))
			if (millis() - t0 > 50) return 2;
		rx[i] = rd8(RXD);
	}
	return 0;
}

int spinor_id(uint8_t id[3]) { static const uint8_t c = 0x9F; begin(); int e = xfer(&c, 1, id, 3); end(); return e; }

int spinor_read(uint32_t addr, void *buf, unsigned n)
{
	uint8_t *p = buf;
	int e = 0;
	begin();
	while (n && !e) {
		unsigned k = n > CHUNK ? CHUNK : n;
		uint8_t c[4] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
		e = xfer(c, 4, p, k);
		addr += k; p += k; n -= k;
	}
	end();
	return e;
}

static int wait_ready(unsigned ms)
{
	uint32_t t0 = millis();
	for (;;) {
		static const uint8_t c = 0x05;
		uint8_t sr;
		if (xfer(&c, 1, &sr, 1)) return 1;
		if (!(sr & 1)) return 0;                           /* WIP clear */
		if (millis() - t0 > ms) return 2;
		udelay(200);
	}
}

static int wren(void) { static const uint8_t c = 0x06; return xfer(&c, 1, 0, 0); }

int spinor_settings_erase(void)
{
	uint32_t a = FW_SETTINGS;
	uint8_t c[4] = { 0x20, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a };   /* 4 KB sector erase */
	begin();
	int e = wren() || xfer(c, 4, 0, 0) ? 1 : wait_ready(1000);
	end();
	return e;
}

int spinor_settings_write(unsigned off, const void *buf, unsigned n)
{
	if (off >= 0x1000 || n > 0x1000 - off) return 9;        /* never outside the settings sector */
	const uint8_t *p = buf;
	uint32_t a = FW_SETTINGS + off;
	int e = 0;
	begin();
	while (n && !e) {
		unsigned k = n > 32 ? 32 : n;
		unsigned room = 256 - (a & 255);                       /* don't cross a 256-byte page */
		if (k > room) k = room;
		uint8_t c[4 + 32] = { 0x02, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a };
		for (unsigned i = 0; i < k; i++) c[4 + i] = p[i];
		if (wren() || xfer(c, 4 + k, 0, 0) || wait_ready(20)) e = 1;
		a += k; p += k; n -= k;
	}
	end();
	return e;
}
