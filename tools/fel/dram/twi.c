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

static int step(uint32_t bits)
{
	wr32(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | bits);
	uint32_t t0 = ticks();
	while (!(rd32(base + 0x0C) & CNTR_IFLG))
		if (ticks() - t0 > 2000 * TICKS_PER_US) return -1;
	return (int)rd32(base + 0x10);
}

static void stop(void)
{
	wr32(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | CNTR_M_STP);
	uint32_t t0 = ticks();
	while ((rd32(base + 0x0C) & CNTR_M_STP) && ticks() - t0 < 2000 * TICKS_PER_US) ;
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

void twi_init(unsigned bus)
{
	if (bus == 1) {                /* PB4 SCK, PB5 SDA: function 4, pull-up (radio tuner). sys_config's
	                                  PG14 is wrong: PG14 function 4 is EMAC (Linux pinctrl-sun20i-d1.c) */
		wr32(0x02000030, (rd32(0x02000030) & ~(0xFFu << 16)) | (4u << 16) | (4u << 20));
		wr32(0x02000054, (rd32(0x02000054) & ~(0xFu << 8)) | (1u << 8) | (1u << 10));
	}
	if (bus == 2) {                /* PE12 SCL, PE13 SDA: function 2, pull-up (sound processor, MFi, camera) */
		wr32(0x020000C4, (rd32(0x020000C4) & ~(0xFFu << 16)) | (2u << 16) | (2u << 20));
		wr32(0x020000E4, (rd32(0x020000E4) & ~(0xFu << 24)) | (1u << 24) | (1u << 26));
	}
	if (bus == 3) {                /* PG10 SCL, PG11 SDA: function 3, pull-up */
		wr32(0x02000124, (rd32(0x02000124) & ~(0xFFu << 8)) | (3u << 8) | (3u << 12));
		wr32(0x02000144, (rd32(0x02000144) & ~(0xFu << 20)) | (1u << 20) | (1u << 22));
	}
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
