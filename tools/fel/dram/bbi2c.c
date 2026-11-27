/* bbi2c.c: see bbi2c.h. */
#include "lib.h"
#include "bbi2c.h"

typedef struct { uint32_t cfg; unsigned csh; uint32_t dat; unsigned bit; uint32_t pull; unsigned psh; } pin;
static const pin SCL = { 0x02000124, 24, 0x02000130, 14, 0x02000144, 28 };   /* PG14 */
static const pin SDA = { 0x02000030, 16, 0x02000040,  4, 0x02000054,  8 };   /* PB4 */

static void mode(const pin *p, unsigned f) { wr32(p->cfg, (rd32(p->cfg) & ~(0xFu << p->csh)) | (f << p->csh)); }
static void lo(const pin *p)  { wr32(p->dat, rd32(p->dat) & ~(1u << p->bit)); mode(p, 1); }
static void rel(const pin *p) { mode(p, 0); }
static int  get(const pin *p) { return (rd32(p->dat) >> p->bit) & 1; }
#define T() udelay(10)

static void sclhi(void) { rel(&SCL); for (int w = 0; w < 1000 && !get(&SCL); w++) udelay(10); }
static void start(void) { rel(&SDA); sclhi(); T(); lo(&SDA); T(); lo(&SCL); T(); }
static void stop(void)  { lo(&SDA); T(); sclhi(); T(); rel(&SDA); T(); }

static int wbyte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if (b >> i & 1) rel(&SDA); else lo(&SDA);
		T(); sclhi(); T(); lo(&SCL);
	}
	rel(&SDA); T(); sclhi(); T();
	int ack = !get(&SDA);
	lo(&SCL); T();
	return ack;
}

static uint8_t rbyte(int ack)
{
	uint8_t b = 0;
	rel(&SDA);
	for (int i = 0; i < 8; i++) { T(); sclhi(); T(); b = (uint8_t)(b << 1 | get(&SDA)); lo(&SCL); }
	if (ack) lo(&SDA); else rel(&SDA);
	T(); sclhi(); T(); lo(&SCL); rel(&SDA); T();
	return b;
}

void bb_init(void)
{
	wr32(SCL.pull, (rd32(SCL.pull) & ~(3u << SCL.psh)) | (1u << SCL.psh));
	wr32(SDA.pull, (rd32(SDA.pull) & ~(3u << SDA.psh)) | (1u << SDA.psh));
	rel(&SCL); rel(&SDA); udelay(100);
}

int bb_write(uint8_t addr, const uint8_t *buf, unsigned n)
{
	start();
	int ok = wbyte((uint8_t)(addr << 1));
	for (unsigned i = 0; ok && i < n; i++) ok = wbyte(buf[i]);
	stop();
	return !ok;
}

int bb_read(uint8_t addr, uint8_t *buf, unsigned n)
{
	start();
	int ok = wbyte((uint8_t)(addr << 1 | 1));
	for (unsigned i = 0; ok && i < n; i++) buf[i] = rbyte(i + 1 < n);
	stop();
	return !ok;
}
