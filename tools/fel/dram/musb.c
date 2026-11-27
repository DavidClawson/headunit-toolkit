/* musb.c: see musb.h. Polled USB device mode on USB0. */
#include "lib.h"
#include "musb.h"

#define USB0        0x04100000u
#define FIFO(n)     (USB0 + 0x00 + (n) * 4)
#define POWER       (USB0 + 0x40)
#define DEVCTL      (USB0 + 0x41)
#define INDEX       (USB0 + 0x42)
#define VEND0       (USB0 + 0x43)
#define INTRTX      (USB0 + 0x44)
#define INTRRX      (USB0 + 0x46)
#define INTRTXE     (USB0 + 0x48)
#define INTRRXE     (USB0 + 0x4A)
#define INTRUSB     (USB0 + 0x4C)
#define INTRUSBE    (USB0 + 0x50)
#define TXMAXP      (USB0 + 0x80)
#define CSR0        (USB0 + 0x82)          /* TXCSR for endpoints 1.. */
#define TXCSR       (USB0 + 0x82)
#define RXMAXP      (USB0 + 0x84)
#define RXCSR       (USB0 + 0x86)
#define COUNT0      (USB0 + 0x88)          /* RXCOUNT for endpoints 1.. */
#define RXCOUNT     (USB0 + 0x88)
#define TXFIFOSZ    (USB0 + 0x90)
#define TXFIFOADD   (USB0 + 0x92)
#define RXFIFOSZ    (USB0 + 0x94)
#define RXFIFOADD   (USB0 + 0x96)
#define FADDR       (USB0 + 0x98)

#define POWER_HSMODE   0x10
#define POWER_HSENAB   0x20
#define POWER_SOFTCONN 0x40
#define USB_SUSPEND 0x01
#define USB_RESUME  0x02
#define USB_RESET   0x04
#define USB_DISCON  0x20
#define CSR0_RXPKTRDY     0x01
#define CSR0_TXPKTRDY     0x02
#define CSR0_SENTSTALL    0x04
#define CSR0_DATAEND      0x08
#define CSR0_SETUPEND     0x10
#define CSR0_SENDSTALL    0x20
#define CSR0_SVDRXPKTRDY  0x40
#define CSR0_SVDSETUPEND  0x80
#define TXCSR_TXPKTRDY    0x0001
#define TXCSR_FLUSH       0x0008
#define TXCSR_SENTSTALL   0x0020
#define TXCSR_CLRTOG      0x0040
#define TXCSR_MODE_TX     0x2000
#define RXCSR_RXPKTRDY    0x0001
#define RXCSR_FLUSH       0x0010
#define RXCSR_SENTSTALL   0x0040
#define RXCSR_CLRTOG      0x0080

static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint16_t rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }

static const struct musb_class *cls;
static int config, hs;
static uint8_t alt[8];
static uint32_t n_resets, n_setups;
static uint8_t ep0buf[512];

static void fifo_write(int ep, const uint8_t *p, int n) { while (n--) wr8(FIFO(ep), *p++); }
static void fifo_read(int ep, uint8_t *p, int n) { while (n--) *p++ = rd8(FIFO(ep)); }

static int size_code(unsigned n) { int c = 0; while ((8u << c) < n) c++; return c; }   /* 2^(c+3) bytes */

static void setup_eps(void)
{
	unsigned addr = 64;                                   /* EP0 owns FIFO RAM 0..63 */
	for (int i = 0; i < cls->neps; i++) {
		const struct musb_ep *e = &cls->eps[i];
		unsigned mp = hs ? e->maxp : (e->maxp > 64 ? 64 : e->maxp);
		wr8(INDEX, e->num);
		if (e->in) {
			wr8(TXFIFOSZ, size_code(mp));
			wr16(TXFIFOADD, addr >> 3);
			wr16(TXMAXP, mp);
			wr16(TXCSR, TXCSR_MODE_TX | TXCSR_CLRTOG | TXCSR_FLUSH);
		} else {
			wr8(RXFIFOSZ, size_code(mp));
			wr16(RXFIFOADD, addr >> 3);
			wr16(RXMAXP, mp);
			wr16(RXCSR, RXCSR_CLRTOG | RXCSR_FLUSH);
		}
		addr += mp < 64 ? 64 : mp;
	}
	wr8(INDEX, 0);
}

static void bus_reset(void)
{
	n_resets++;
	hs = (rd8(POWER) & POWER_HSMODE) != 0;
	wr8(FADDR, 0);
	if (config && cls->configured) cls->configured(0);
	config = 0;
	memset(alt, 0, sizeof alt);
	setup_eps();
}

static int wait_csr0(uint16_t clear_mask, uint16_t abort_mask)
{
	uint32_t t0 = millis();
	for (;;) {
		uint16_t c = rd16(CSR0);
		if (c & abort_mask) return -1;
		if (!(c & clear_mask)) return 0;
		if (millis() - t0 > 100) return -1;
	}
}

/* IN data stage: n bytes, the host asked for at most wlen */
static void ep0_in(const uint8_t *p, int n, int wlen)
{
	if (n > wlen) n = wlen;
	wr16(CSR0, CSR0_SVDRXPKTRDY);
	int sent = 0;
	for (;;) {
		int k = n - sent > 64 ? 64 : n - sent;
		if (wait_csr0(CSR0_TXPKTRDY, CSR0_SETUPEND)) return;
		fifo_write(0, p + sent, k);
		sent += k;
		int last = sent >= n && (k < 64 || n == wlen);     /* a short packet, or all the host asked for */
		wr16(CSR0, CSR0_TXPKTRDY | (last ? CSR0_DATAEND : 0));
		if (last) return;
	}
}

static int string_desc(int idx, uint8_t *b)
{
	if (idx == 0) { b[0] = 4; b[1] = 3; b[2] = 0x09; b[3] = 0x04; return 4; }
	if (idx > cls->nstrings) return -1;
	const char *s = cls->strings[idx - 1];
	int n = 2;
	while (*s && n < 254) { b[n++] = (uint8_t)*s++; b[n++] = 0; }
	b[0] = (uint8_t)n; b[1] = 3;
	return n;
}

static uint8_t recent[8][8];
int musb_recent(uint8_t req[][8], int max)
{
	int n = n_setups < 8 ? (int)n_setups : 8;
	if (n > max) n = max;
	for (int i = 0; i < n; i++) memcpy(req[i], recent[(n_setups - 1 - i) % 8], 8);
	return n;
}

static void ep0_setup(const uint8_t *r)
{
	memcpy(recent[n_setups % 8], r, 8);
	n_setups++;
	int type = r[0] & 0x60, dir_in = r[0] & 0x80, req = r[1];
	int wvalue = r[2] | r[3] << 8, wlen = r[6] | r[7] << 8;
	int n = -1;

	if (type == 0) {                                         /* standard */
		switch (req) {
		case 6: {                                            /* GET_DESCRIPTOR */
			int dt = wvalue >> 8, di = wvalue & 0xFF;
			if (dt == 1) { memcpy(ep0buf, cls->dev_desc, 18); ep0buf[7] = 64; n = 18; }
			else if (dt == 2) {
				const uint8_t *c = hs ? cls->cfg_desc_hs : cls->cfg_desc_fs;
				n = c[2] | c[3] << 8;
				memcpy(ep0buf, c, n);
			} else if (dt == 3) n = string_desc(di, ep0buf);
			else if (dt == 6) {                              /* device qualifier */
				const uint8_t *d = cls->dev_desc;
				uint8_t q[10] = { 10, 6, 0x00, 0x02, d[4], d[5], d[6], 64, 1, 0 };
				memcpy(ep0buf, q, 10); n = 10;
			}
			if (n >= 0) { ep0_in(ep0buf, n, wlen); return; }
			break;
		}
		case 5:                                              /* SET_ADDRESS */
			wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
			{   /* the address takes effect after the status stage */
				uint32_t t0 = millis();
				while (!(rd16(INTRTX) & 1) && millis() - t0 < 50) ;
				wr16(INTRTX, 1);
			}
			wr8(FADDR, wvalue & 0x7F);
			return;
		case 9:                                              /* SET_CONFIGURATION */
			config = wvalue & 0xFF;
			memset(alt, 0, sizeof alt);
			setup_eps();
			if (cls->configured) cls->configured(config);
			wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
			return;
		case 8: ep0buf[0] = (uint8_t)config; ep0_in(ep0buf, 1, wlen); return;
		case 0: ep0buf[0] = 0; ep0buf[1] = 0; ep0_in(ep0buf, 2, wlen); return;
		case 10: ep0buf[0] = alt[r[4] & 7]; ep0_in(ep0buf, 1, wlen); return;   /* GET_INTERFACE */
		case 11:                                             /* SET_INTERFACE */
			alt[r[4] & 7] = (uint8_t)wvalue;
			setup_eps();                                     /* fresh FIFOs and data toggles */
			wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
			if (cls->set_alt) cls->set_alt(r[4], wvalue);
			return;
		case 1: case 3:                                      /* CLEAR/SET_FEATURE */
			wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
			return;
		}
	} else if (cls->setup) {                                 /* class / vendor */
		if (!dir_in && wlen > 0) {                           /* OUT data stage */
			static uint8_t out[512];
			int got = 0;
			wr16(CSR0, CSR0_SVDRXPKTRDY);
			while (got < wlen && got < (int)sizeof out) {
				uint32_t t0 = millis();
				while (!(rd16(CSR0) & CSR0_RXPKTRDY)) if (millis() - t0 > 100) return;
				int k = rd16(COUNT0);
				if (got + k > (int)sizeof out) k = sizeof out - got;
				fifo_read(0, out + got, k);
				got += k;
				wr16(CSR0, CSR0_SVDRXPKTRDY | (got >= wlen ? CSR0_DATAEND : 0));
				if (k < 64) break;
			}
			cls->setup(r, ep0buf, out, got);
			return;
		}
		n = cls->setup(r, ep0buf, 0, 0);
		if (n > 0 && dir_in) { ep0_in(ep0buf, n, wlen); return; }
		if (n == 0) { wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND); return; }
	}
	wr16(CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
}

int musb_start(const struct musb_class *c)
{
	cls = c;
	config = 0;
	wr8(POWER, rd8(POWER) & ~POWER_SOFTCONN);               /* the host sees a disconnect */
	udelay(500000);
	wr8(VEND0, 0);                                           /* PIO, no DMA */
	wr16(INTRTXE, 0xFFFF); wr16(INTRRXE, 0xFFFE); wr8(INTRUSBE, USB_RESET | USB_SUSPEND | USB_RESUME | USB_DISCON);
	wr8(INTRUSB, rd8(INTRUSB)); wr16(INTRTX, rd16(INTRTX)); wr16(INTRRX, rd16(INTRRX));
	wr8(FADDR, 0);
	hs = 0;
	setup_eps();
	wr8(POWER, POWER_HSENAB | POWER_SOFTCONN);
	return 0;
}

void musb_stop(void) { wr8(POWER, rd8(POWER) & ~POWER_SOFTCONN); }

void musb_poll(void)
{
	uint8_t u = rd8(INTRUSB);
	if (u) {
		wr8(INTRUSB, u);
		if (u & USB_RESET) bus_reset();
		if (u & USB_DISCON) { if (config && cls->configured) cls->configured(0); config = 0; }
	}
	uint16_t t = rd16(INTRTX);
	if (t) wr16(INTRTX, t);
	uint16_t rx = rd16(INTRRX);
	if (rx) wr16(INTRRX, rx);

	wr8(INDEX, 0);
	uint16_t c = rd16(CSR0);
	if (c & CSR0_SENTSTALL) { wr16(CSR0, 0); c = rd16(CSR0); }
	if (c & CSR0_SETUPEND) { wr16(CSR0, CSR0_SVDSETUPEND); c = rd16(CSR0); }
	if (c & CSR0_RXPKTRDY) {
		uint8_t r[8];
		int n = rd16(COUNT0);
		if (n != 8) { uint8_t junk[64]; fifo_read(0, junk, n > 64 ? 64 : n); wr16(CSR0, CSR0_SVDRXPKTRDY); return; }
		fifo_read(0, r, 8);
		ep0_setup(r);
	}
}

int musb_configured(void) { return config != 0; }
int musb_high_speed(void) { return hs; }

int musb_write(int ep, const void *buf, int n)
{
	wr8(INDEX, ep);
	uint16_t c = rd16(TXCSR);
	if (c & TXCSR_SENTSTALL) wr16(TXCSR, TXCSR_MODE_TX);
	if (c & TXCSR_TXPKTRDY) { wr8(INDEX, 0); return -1; }
	fifo_write(ep, buf, n);
	wr16(TXCSR, TXCSR_MODE_TX | TXCSR_TXPKTRDY);
	wr8(INDEX, 0);
	return n;
}

int musb_read(int ep, void *buf, int max)
{
	wr8(INDEX, ep);
	uint16_t c = rd16(RXCSR);
	if (!(c & RXCSR_RXPKTRDY)) { wr8(INDEX, 0); return 0; }
	int n = rd16(RXCOUNT);
	fifo_read(ep, buf, n < max ? n : max);
	wr16(RXCSR, 0);                                          /* clear RXPKTRDY: the FIFO is free again */
	wr8(INDEX, 0);
	return n;
}

int musb_rx_ready(int ep)
{
	wr8(INDEX, ep);
	int r = rd16(RXCSR) & RXCSR_RXPKTRDY;
	wr8(INDEX, 0);
	return r != 0;
}

int musb_send(int ep, const void *buf, int n, int maxp)
{
	const uint8_t *p = buf;
	int sent = 0;
	for (;;) {
		int k = n - sent > maxp ? maxp : n - sent;
		uint32_t t0 = millis();
		while (musb_write(ep, p + sent, k) < 0) {
			musb_poll();
			if (millis() - t0 > 200 || !config) return -1;
		}
		sent += k;
		if (k < maxp) return 0;                      /* a short packet (or ZLP) ends the transfer */
	}
}

uint32_t musb_stats(uint32_t *resets, uint32_t *setups)
{
	if (resets) *resets = n_resets;
	if (setups) *setups = n_setups;
	return rd8(POWER) | rd8(DEVCTL) << 8 | rd8(FADDR) << 16;
}
