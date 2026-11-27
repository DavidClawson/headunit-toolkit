/*
 * i2c.c: I2C (TWI) helper payload for FEL, driven by tools/fel/i2c.py.
 *
 * Loaded at 0x40000; the host fills the request block at 0x40F00, runs
 * `xfel exec 0x40000`, then reads the block back. Every wait has a timeout so a
 * stuck bus returns an error instead of hanging the SoC.
 *
 * F133 TWI controller (Allwinner mv64xxx variant, as in Linux i2c-mv64xxx.c with
 * irq_clear_inverted): ADDR 0x00, XADDR 0x04, DATA 0x08, CNTR 0x0C, STAT 0x10,
 * CCR 0x14, SRST 0x18, EFR 0x1C, LCR 0x20. Bases 0x02502000 + 0x400 * n.
 * Bus clock gate/reset: CCU TWI_BGR 0x0200191C, bit n / bit 16+n.
 */
#include <stdint.h>

#define REQ ((volatile struct req *)0x40F00)

struct req {
	uint32_t cmd;      /* 1 = scan, 2 = read, 3 = write */
	uint32_t bus;      /* TWI number 0-3 */
	uint32_t addr;     /* 7-bit device address */
	uint32_t reg;      /* register address (sent MSB first) */
	uint32_t reglen;   /* 0, 1 or 2 bytes of register address */
	uint32_t len;      /* bytes to read / write (<= 128) */
	uint32_t status;   /* out: 0 ok, else error code below */
	uint32_t stat;     /* out: last TWI STAT value */
	uint8_t  data[128];/* in (write) / out (read, scan: 1 per address that ACKed) */
};

enum { OK = 0, E_TIMEOUT = 1, E_START = 2, E_NACK_ADDR = 3, E_NACK_DATA = 4, E_ARG = 5 };

#define CNTR_INT_EN  (1u << 7)
#define CNTR_BUS_EN  (1u << 6)
#define CNTR_M_STA   (1u << 5)
#define CNTR_M_STP   (1u << 4)
#define CNTR_IFLG    (1u << 3)
#define CNTR_A_ACK   (1u << 2)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uintptr_t base;

static int wait_iflg(void)
{
	for (uint32_t i = 0; i < 400000; i++)
		if (rd(base + 0x0C) & CNTR_IFLG)
			return 1;
	return 0;
}

/* Kick the controller with `bits` (also clears IFLG, write-1-to-clear here). */
static int step(uint32_t bits)
{
	wr(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | bits);
	if (!wait_iflg())
		return -1;
	return (int)rd(base + 0x10);
}

static void stop(void)
{
	wr(base + 0x0C, CNTR_BUS_EN | CNTR_IFLG | CNTR_M_STP);
	for (uint32_t i = 0; i < 400000; i++)
		if (!(rd(base + 0x0C) & CNTR_M_STP))
			break;
}

static int start(uint8_t a8, int repeated)
{
	int s = step(CNTR_M_STA);
	if (s < 0) return -E_TIMEOUT;
	if (s != (repeated ? 0x10 : 0x08) && s != 0x08 && s != 0x10) return -E_START;
	wr(base + 0x08, a8);
	s = step(0);
	if (s < 0) return -E_TIMEOUT;
	return s;
}

static void init(uint32_t bus)
{
	base = 0x02502000 + 0x400 * bus;
	uint32_t v = rd(0x0200191C);
	wr(0x0200191C, v | (1u << bus) | (1u << (16 + bus)));  /* gate on, reset off */
	wr(base + 0x18, 1);                                    /* soft reset */
	for (uint32_t i = 0; i < 100000 && (rd(base + 0x18) & 1); i++)
		;
	wr(base + 0x1C, 0);
	wr(base + 0x14, (11u << 3) | 2u);   /* M = 11, N = 2: 24 MHz / 4 / 120 = 50 kHz */
	wr(base + 0x0C, CNTR_BUS_EN);
}

static uint32_t xfer(volatile struct req *r)
{
	uint32_t a = r->addr & 0x7f;
	int s;

	if (r->len > sizeof(r->data) || r->reglen > 2)
		return E_ARG;

	/* address + write, then register bytes (also the whole write command) */
	if (r->cmd == 3 || r->reglen) {
		s = start((uint8_t)(a << 1), 0);
		if (s < 0) { stop(); return (uint32_t)-s; }
		r->stat = (uint32_t)s;
		if (s != 0x18) { stop(); return E_NACK_ADDR; }
		for (uint32_t i = r->reglen; i > 0; i--) {
			wr(base + 0x08, (r->reg >> (8 * (i - 1))) & 0xff);
			s = step(0); r->stat = (uint32_t)s;
			if (s != 0x28) { stop(); return s < 0 ? E_TIMEOUT : E_NACK_DATA; }
		}
		if (r->cmd == 3) {
			for (uint32_t i = 0; i < r->len; i++) {
				wr(base + 0x08, r->data[i]);
				s = step(0); r->stat = (uint32_t)s;
				if (s != 0x28) { stop(); return s < 0 ? E_TIMEOUT : E_NACK_DATA; }
			}
			stop();
			return OK;
		}
	}

	/* (repeated) start + address + read, then the data bytes */
	s = start((uint8_t)((a << 1) | 1), r->reglen != 0);
	if (s < 0) { stop(); return (uint32_t)-s; }
	r->stat = (uint32_t)s;
	if (s != 0x40) { stop(); return E_NACK_ADDR; }
	for (uint32_t i = 0; i < r->len; i++) {
		int last = (i + 1 == r->len);
		s = step(last ? 0 : CNTR_A_ACK); r->stat = (uint32_t)s;
		if (s < 0) { stop(); return E_TIMEOUT; }
		r->data[i] = (uint8_t)rd(base + 0x08);
	}
	stop();
	return OK;
}

static void scan(volatile struct req *r)
{
	for (uint32_t a = 0; a < 128; a++) {
		r->data[a] = 0;
		if (a < 0x08 || a > 0x77)
			continue;
		int s = start((uint8_t)(a << 1), 0);    /* address + write, no data */
		if (s == 0x18)
			r->data[a] = 1;
		stop();
		if (s < 0) { r->status = E_TIMEOUT; r->stat = (uint32_t)rd(base + 0x10); return; }
	}
	r->status = OK;
}

__attribute__((section(".text.start")))
void _start(void)
{
	volatile struct req *r = REQ;
	if (r->bus > 3) { r->status = E_ARG; return; }
	init(r->bus);
	if (r->cmd == 1)
		scan(r);
	else if (r->cmd == 2 || r->cmd == 3)
		r->status = xfer(r);
	else
		r->status = E_ARG;
}
