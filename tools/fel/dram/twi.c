/*
 * twi.c: see twi.h. Allwinner mv64xxx-style TWI: ADDR 0x00, DATA 0x08,
 * CNTR 0x0C, STAT 0x10, CCR 0x14, SRST 0x18, EFR 0x1C. Bases 0x02502000 +
 * 0x400 * n; clock gate/reset in CCU TWI_BGR 0x0200191C (bit n / 16 + n).
 * Every wait is bounded (2 ms) so a stuck bus returns an error.
 */
#include "lib.h"
#include "twi.h"

#define CNTR_BUS_EN (1u << 6)
#define CNTR_M_STA  (1u << 5)
#define CNTR_M_STP  (1u << 4)
#define CNTR_IFLG   (1u << 3)
#define CNTR_A_ACK  (1u << 2)

static uint32_t base;
unsigned twi_timeout_us = 2000;   /* per byte; a device may stretch the clock longer (the MFi chip) */

static int step(uint32_t bits)
{
	wr32(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | bits);
	uint32_t t0 = ticks();
	while (!(rd32(base + 0x0C) & CNTR_IFLG))
		if (ticks() - t0 > twi_timeout_us * TICKS_PER_US) return -1;
	return (int)rd32(base + 0x10);
}

static void stop(void)
{
	wr32(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | CNTR_M_STP);
	uint32_t t0 = ticks();
	while ((rd32(base + 0x0C) & CNTR_M_STP) && ticks() - t0 < twi_timeout_us * TICKS_PER_US) ;
}

static int start(uint8_t a8)
{
	int s = step(CNTR_M_STA);
	if (s < 0) return -TWI_TIMEOUT;
	if (s != 0x08 && s != 0x10) return -TWI_START;
	wr32(base + 0x08, a8);
	s = step(0);
	return s < 0 ? -TWI_TIMEOUT : s;
}

static void sel(unsigned bus) { base = 0x02502000u + 0x400u * bus; }

/* field helpers: 4-bit function fields and 2-bit pull fields */
static void fn(uint32_t reg, unsigned sh, unsigned f) { wr32(reg, (rd32(reg) & ~(0xFu << sh)) | (f << sh)); }
static void pu(uint32_t reg, unsigned sh) { wr32(reg, (rd32(reg) & ~(3u << sh)) | (1u << sh)); }
static unsigned fget(uint32_t reg, unsigned sh) { return (rd32(reg) >> sh) & 0xF; }

/*
 * TWI1 can come out on three pin pairs (F133 datasheet): 0 = PB4/PB5 fn 4,
 * 1 = PE0/PE1 fn 4, 2 = PG8/PG9 fn 3. Muxes the chosen pair with pull-ups and
 * turns the others back into inputs (only pins currently in their TWI1
 * function, so a GPIO use such as PB5 as an output is left alone).
 */
void twi1_route(unsigned pair)
{
	static const struct { uint32_t cfg, pull; unsigned sh0, sh1, psh0, psh1, f; } p[3] = {
		{ 0x02000030, 0x02000054, 16, 20,  8, 10, 4 },   /* PB4, PB5 */
		{ 0x020000C0, 0x020000E4,  0,  4,  0,  2, 4 },   /* PE0, PE1 */
		{ 0x02000124, 0x02000144,  0,  4, 16, 18, 3 },   /* PG8, PG9 */
	};
	for (unsigned i = 0; i < 3; i++) {
		if (i == pair) continue;
		if (fget(p[i].cfg, p[i].sh0) == p[i].f) fn(p[i].cfg, p[i].sh0, 0);
		if (fget(p[i].cfg, p[i].sh1) == p[i].f) fn(p[i].cfg, p[i].sh1, 0);
	}
	if (pair > 2) return;
	fn(p[pair].cfg, p[pair].sh0, p[pair].f); fn(p[pair].cfg, p[pair].sh1, p[pair].f);
	pu(p[pair].pull, p[pair].psh0);           pu(p[pair].pull, p[pair].psh1);
}

void twi_init(unsigned bus)
{
	if (bus == 1) twi1_route(0);
	if (bus == 2) {                /* PE12 SCL, PE13 SDA: function 2, pull-up (sound processor, MFi, camera) */
		wr32(0x020000C4, (rd32(0x020000C4) & ~(0xFFu << 16)) | (2u << 16) | (2u << 20));
		wr32(0x020000E4, (rd32(0x020000E4) & ~(0xFu << 24)) | (1u << 24) | (1u << 26));
	}
	if (bus == 3) {                /* PG10 SCL, PG11 SDA: function 3, pull-up */
		wr32(0x02000124, (rd32(0x02000124) & ~(0xFFu << 8)) | (3u << 8) | (3u << 12));
		wr32(0x02000144, (rd32(0x02000144) & ~(0xFu << 20)) | (1u << 20) | (1u << 22));
	}
	twi_ctrl_init(bus);
}

/* controller only: clock gate, reset, 100 kHz; leaves the pins alone */
void twi_ctrl_init(unsigned bus)
{
	sel(bus);
	wr32(0x0200191C, rd32(0x0200191C) | (1u << bus) | (1u << (16 + bus)));
	wr32(base + 0x18, 1);
	uint32_t t0 = ticks();
	while ((rd32(base + 0x18) & 1) && ticks() - t0 < 2000 * TICKS_PER_US) ;
	wr32(base + 0x1C, 0);
	wr32(base + 0x14, (5u << 3) | 2u);   /* 24 MHz / (2^2 * (5+1) * 10) = 100 kHz */
	wr32(base + 0x0C, CNTR_BUS_EN);
}

static int send_reg(uint8_t addr, uint32_t reg, unsigned reglen)
{
	int s = start((uint8_t)(addr << 1));
	if (s < 0) return -s;
	if (s != 0x18) return TWI_NACK_ADDR;
	for (unsigned i = reglen; i > 0; i--) {
		wr32(base + 0x08, (reg >> (8 * (i - 1))) & 0xff);
		s = step(0);
		if (s != 0x28) return s < 0 ? TWI_TIMEOUT : TWI_NACK_DATA;
	}
	return TWI_OK;
}

int twi_write(unsigned bus, uint8_t addr, uint32_t reg, unsigned reglen, const uint8_t *buf, unsigned len)
{
	sel(bus);
	int e = send_reg(addr, reg, reglen);
	for (unsigned i = 0; !e && i < len; i++) {
		wr32(base + 0x08, buf[i]);
		int s = step(0);
		if (s != 0x28) e = s < 0 ? TWI_TIMEOUT : TWI_NACK_DATA;
	}
	stop();
	return e;
}

int twi_read(unsigned bus, uint8_t addr, uint32_t reg, unsigned reglen, uint8_t *buf, unsigned len)
{
	sel(bus);
	int e = reglen ? send_reg(addr, reg, reglen) : TWI_OK;
	if (e) { stop(); return e; }
	int s = start((uint8_t)((addr << 1) | 1));
	if (s < 0) { stop(); return -s; }
	if (s != 0x40) { stop(); return TWI_NACK_ADDR; }
	for (unsigned i = 0; i < len; i++) {
		s = step(i + 1 == len ? 0 : CNTR_A_ACK);
		if (s < 0) { stop(); return TWI_TIMEOUT; }
		buf[i] = (uint8_t)rd32(base + 0x08);
	}
	stop();
	return TWI_OK;
}
