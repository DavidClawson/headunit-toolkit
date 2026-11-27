/* gt911.c: see gt911.h. Registers per the Goodix GT911 datasheet / Linux goodix.c. */
#include "lib.h"
#include "twi.h"
#include "gt911.h"

#define BUS  3
#define ADDR 0x5d
#define PG_CFG0 0x02000120u
#define PG_CFG1 0x02000124u
#define PG_DAT  0x02000130u

int gt911_init(char id[5], uint16_t *xmax, uint16_t *ymax, uint16_t *fw)
{
	/* reset: INT (PG07) low while RESET (PG08) rises selects address 0x5d */
	wr32(PG_CFG0, (rd32(PG_CFG0) & ~(0xFu << 28)) | (1u << 28));
	wr32(PG_CFG1, (rd32(PG_CFG1) & ~0xFu) | 1u);
	wr32(PG_DAT, rd32(PG_DAT) & ~((1u << 7) | (1u << 8)));
	udelay(20000);
	wr32(PG_DAT, rd32(PG_DAT) | (1u << 8));
	udelay(10000);
	wr32(PG_CFG0, rd32(PG_CFG0) & ~(0xFu << 28));      /* INT back to input */
	udelay(60000);

	twi_init(BUS);
	uint8_t b[11];
	int e = twi_read(BUS, ADDR, 0x8140, 2, b, 10);      /* ID[4], FW[2], X[2], Y[2] */
	if (e) return e;
	for (int i = 0; i < 4; i++) id[i] = (char)b[i];
	id[4] = 0;
	*fw = (uint16_t)(b[4] | b[5] << 8);
	*xmax = (uint16_t)(b[6] | b[7] << 8);
	*ymax = (uint16_t)(b[8] | b[9] << 8);
	uint8_t z = 0;
	twi_write(BUS, ADDR, 0x814E, 2, &z, 1);
	return 0;
}

int gt911_poll(touch_pt *pts, int max)
{
	uint8_t st;
	int e = twi_read(BUS, ADDR, 0x814E, 2, &st, 1);
	if (e) return -2 - e;
	if (!(st & 0x80)) return -1;
	int n = st & 0x0f;
	if (n > 5) n = 5;
	if (n > max) n = max;
	if (n) {
		uint8_t b[40];
		e = twi_read(BUS, ADDR, 0x814F, 2, b, (unsigned)(8 * n));
		if (e) return -2 - e;
		for (int i = 0; i < n; i++) {
			const uint8_t *p = b + 8 * i;
			pts[i].id = p[0];
			pts[i].x = (uint16_t)(p[1] | p[2] << 8);
			pts[i].y = (uint16_t)(p[3] | p[4] << 8);
			pts[i].size = (uint16_t)(p[5] | p[6] << 8);
		}
	}
	uint8_t z = 0;
	twi_write(BUS, ADDR, 0x814E, 2, &z, 1);
	return n;
}
