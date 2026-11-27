/*
 * bbscan.c: bit-banged I2C scan on arbitrary GPIO pairs, after putting the MCU
 * in radio mode. Lead: the Melis kernel has a software TWI driver, and the
 * board config gives TWI1 as PG14 (SCL) + PB4 (SDA), and PG14 can't be hardware
 * TWI1. Tries PG14/PB4, swapped, and PB4/PB5. Open drain is emulated: drive low =
 * output 0, release = input with the SoC pull-up. ~50 kHz.
 */
#include "lib.h"
#include "uart.h"

typedef struct { uint32_t cfg; unsigned csh; uint32_t dat; unsigned bit; uint32_t pull; unsigned psh; } pin;
static const pin PG14 = { 0x02000124, 24, 0x02000130, 14, 0x02000144, 28 };
static const pin PB4  = { 0x02000030, 16, 0x02000040,  4, 0x02000054,  8 };
static const pin PB5  = { 0x02000030, 20, 0x02000040,  5, 0x02000054, 10 };

static void mode(const pin *p, unsigned f) { wr32(p->cfg, (rd32(p->cfg) & ~(0xFu << p->csh)) | (f << p->csh)); }
static void lo(const pin *p)  { wr32(p->dat, rd32(p->dat) & ~(1u << p->bit)); mode(p, 1); }
static void rel(const pin *p) { mode(p, 0); }
static int  get(const pin *p) { return (rd32(p->dat) >> p->bit) & 1; }
static void prep(const pin *p) { wr32(p->pull, (rd32(p->pull) & ~(3u << p->psh)) | (1u << p->psh)); rel(p); }

static const pin *SCL, *SDA;
#define T() udelay(10)
static void start(void) { rel(SDA); rel(SCL); T(); lo(SDA); T(); lo(SCL); T(); }
static void stop(void)  { lo(SDA); T(); rel(SCL); T(); rel(SDA); T(); }
static int  wbyte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if (b >> i & 1) rel(SDA); else lo(SDA);
		T(); rel(SCL);
		for (int w = 0; w < 100 && !get(SCL); w++) udelay(10);   /* clock stretching */
		T(); lo(SCL);
	}
	rel(SDA); T(); rel(SCL); T();
	int ack = !get(SDA);
	lo(SCL); T();
	return ack;
}

static void scan(const pin *scl, const pin *sda, const char *name)
{
	SCL = scl; SDA = sda;
	prep(scl); prep(sda); udelay(1000);
	logf("  %s: idle SCL=%d SDA=%d:", name, get(scl), get(sda));
	unsigned n = 0;
	for (unsigned a = 0x08; a < 0x78; a++) {
		start();
		int ack = wbyte((uint8_t)(a << 1));
		stop();
		if (ack) { logf(" %02x", a); n++; }
	}
	logf(n ? "\n" : " nothing\n");
}

static void mcu_send(uint8_t cmd, const uint8_t *d, unsigned n)
{
	uint8_t x = 0x80 ^ cmd ^ (uint8_t)n;
	uart_putc(1, 0x02); uart_putc(1, 0x80); uart_putc(1, cmd); uart_putc(1, (uint8_t)n);
	for (unsigned i = 0; i < n; i++) { uart_putc(1, d[i]); x ^= d[i]; }
	uart_putc(1, x);
}
static void drain(unsigned ms) { uint32_t t0 = ticks(); while (elapsed_ms(t0) < ms) uart_getc(1); }

int main(void)
{
	log_reset();
	logf("bit-banged I2C scan\n1. MCU start-up + radio\n");
	uart_init(1, 115200);
	static const uint8_t b7[1] = { 0x02 }, a7f[2] = { 0x0F, 0x00 }, src[2] = { 0x04, 0x00 };
	static const uint8_t ce1[5] = { 0xCE, 0x01, 0, 0, 0 }, ce0[5] = { 0xCE, 0x00, 0, 0, 0 };
	mcu_send(0xA4, 0, 0); drain(300);
	mcu_send(0xA0, 0, 0); drain(1000);
	mcu_send(0xB7, b7, 1); drain(300);
	mcu_send(0xC3, 0, 0); drain(300);
	mcu_send(0xBA, 0, 0); drain(300);
	mcu_send(0xA7, a7f, 2); drain(300);
	mcu_send(0xB0, ce0, 5); drain(100);
	mcu_send(0xB0, ce1, 5); drain(300);
	mcu_send(0xA7, src, 2); drain(1000);
	mcu_send(0xB0, ce0, 5);

	logf("2. scans (PB5 held high as the radio reset where it isn't a bus line)\n");
	wr32(0x02000040, rd32(0x02000040) & ~(1u << 5)); mode(&PB5, 1); udelay(100000);
	wr32(0x02000040, rd32(0x02000040) | (1u << 5)); udelay(50000);
	scan(&PG14, &PB4, "SCL=PG14 SDA=PB4 (board config)");
	scan(&PB4, &PG14, "SCL=PB4 SDA=PG14 (swapped)");
	scan(&PB4, &PB5, "SCL=PB4 SDA=PB5");
	scan(&PB5, &PB4, "SCL=PB5 SDA=PB4");
	return 0;
}
