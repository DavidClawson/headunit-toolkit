/*
 * ncmtest.c: USB networking (CDC-NCM, the transport CarPlay runs over) to the Mac.
 * Takes over USB0 from the boot ROM's FEL device (as usbtest.c does) and comes back
 * as a USB network adapter, which macOS supports natively. A tiny responder runs on
 * top: a DHCP server (gives the Mac 192.168.133.2; no router or DNS, so the Mac's
 * own internet route is untouched), ARP and ping for 192.168.133.1.
 * The screen shows live counters; after SECONDS they freeze there until a power
 * cycle. A fault paints the screen red.
 *
 * Run with: tools/fel/felrun.py dram/ncmtest.bin --usb-net 60
 */
#include "lib.h"
#include "musb.h"
#include "display.h"
#include "gfx.h"

#ifndef SECONDS
#define SECONDS 90
#endif

/* ---- descriptors ---- */
static const uint8_t dev[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x02, 0x00,               /* VID 0x1209, PID 0x0002 (pid.codes test) */
	0x00, 0x01, 1, 2, 3, 1,
};
#define CFG_LEN (9 + 9 + 5 + 5 + 13 + 6 + 7 + 9 + 9 + 7 + 7)
#define CFG(lo, hi, intv)                                                                   \
	{ 9, 2, CFG_LEN, 0, 2, 1, 0, 0x80, 50,                                                  \
	  9, 4, 0, 0, 1, 0x02, 0x0D, 0x00, 0,           /* IF0: CDC comm, NCM */                \
	  5, 0x24, 0x00, 0x10, 0x01,                    /* header, CDC 1.10 */                  \
	  5, 0x24, 0x06, 0x00, 0x01,                    /* union: master 0, slave 1 */          \
	  13, 0x24, 0x0F, 4, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,   /* ethernet: MAC string 4, max segment 1514 */ \
	  6, 0x24, 0x1A, 0x00, 0x01, 0x00,              /* NCM 1.00, no optional features */    \
	  7, 5, 0x81, 0x03, 16, 0, intv,                /* EP1 IN interrupt: notifications */   \
	  9, 4, 1, 0, 0, 0x0A, 0x00, 0x01, 0,           /* IF1 alt 0: data, no endpoints */     \
	  9, 4, 1, 1, 2, 0x0A, 0x00, 0x01, 0,           /* IF1 alt 1: data, NTB protocol */     \
	  7, 5, 0x02, 0x02, lo, hi, 0,                  /* EP2 OUT bulk */                      \
	  7, 5, 0x82, 0x02, lo, hi, 0 }                 /* EP2 IN bulk */
static const uint8_t cfg_hs[CFG_LEN] = CFG(0x00, 0x02, 9);
static const uint8_t cfg_fs[CFG_LEN] = CFG(64, 0, 32);
/* string 4 = the MAC address the HOST uses for its side; ours is ...02 */
static const char *const strings[] = { "headunit-toolkit", "Head Unit network (USB test)", "F133-0002", "02F133000001" };
static const struct musb_ep eps[] = { { 1, 1, 3, 16 }, { 2, 0, 2, 512 }, { 2, 1, 2, 512 } };

static const uint8_t my_mac[6] = { 0x02, 0xF1, 0x33, 0x00, 0x00, 0x02 };
static const uint8_t my_ip[4] = { 192, 168, 133, 1 }, peer_ip[4] = { 192, 168, 133, 2 };

#define NTB_OUT_MAX 2048
#define NTB_IN_MAX  2048
static int data_on, notify_pending;
static uint32_t n_rx, n_tx, n_arp, n_ping, n_dhcp, n_bad, n_other;
static uint16_t last_types[4];
static uint16_t in_seq;

static int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
	return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void pbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int ncm_setup(const uint8_t r[8], uint8_t *buf, const uint8_t *out, int outlen)
{
	(void)out; (void)outlen;
	switch (r[1]) {
	case 0x80:                                           /* GET_NTB_PARAMETERS */
		memset(buf, 0, 28);
		put16(buf + 0, 28);
		put16(buf + 2, 1);                               /* NTB-16 only */
		put32(buf + 4, NTB_IN_MAX);
		put16(buf + 8, 4);                               /* IN: divisor 4, remainder 0, alignment 4 */
		put16(buf + 12, 4);
		put32(buf + 16, NTB_OUT_MAX);
		put16(buf + 20, 4);
		put16(buf + 24, 4);
		put16(buf + 26, 1);                              /* one datagram per OUT NTB is enough */
		return 28;
	case 0x85: put32(buf, NTB_IN_MAX); return 4;         /* GET_NTB_INPUT_SIZE */
	case 0x86: return 0;                                 /* SET_NTB_INPUT_SIZE: keep ours */
	case 0x43: return 0;                                 /* SET_ETHERNET_PACKET_FILTER */
	case 0x83: put16(buf, 0); return 2;                  /* GET_NTB_FORMAT: 16-bit */
	case 0x84: return 0;                                 /* SET_NTB_FORMAT */
	case 0x87: put16(buf, 1514); return 2;               /* GET_MAX_DATAGRAM_SIZE */
	case 0x88: return 0;                                 /* SET_MAX_DATAGRAM_SIZE */
	}
	return -1;
}
static void ncm_configured(int c) { if (!c) data_on = 0; }
static void ncm_set_alt(int iface, int a)
{
	if (iface != 1) return;
	data_on = a == 1;
	notify_pending = data_on;                            /* tell the host the link is up */
}

static const struct musb_class ncm = {
	dev, cfg_hs, cfg_fs, strings, 4, eps, 3, ncm_setup, ncm_configured, ncm_set_alt,
};

static void notify(void)
{
	/* CONNECTION_SPEED_CHANGE (480 Mbit/s both ways), then NETWORK_CONNECTION = connected */
	uint8_t sp[16] = { 0xA1, 0x2A, 0, 0, 0, 0, 8, 0 };
	put32(sp + 8, 480000000u); put32(sp + 12, 480000000u);
	uint8_t nc[8] = { 0xA1, 0x00, 1, 0, 0, 0, 0, 0 };
	musb_send(1, sp, 16, 16);
	musb_send(1, nc, 8, 16);
}

/* ---- sending one Ethernet frame as an NTB-16 ---- */
static uint8_t ntb_in[NTB_IN_MAX];
static void eth_send(const uint8_t *frame, int len)
{
	if (!data_on || len > 1514) return;
	int dg = 32;                                         /* NTH 12, NDP 16 at 12, datagram at 32 */
	int total = dg + len;
	memset(ntb_in, 0, dg);
	put32(ntb_in, 0x484D434E);                           /* "NCMH" */
	put16(ntb_in + 4, 12);
	put16(ntb_in + 6, in_seq++);
	put16(ntb_in + 8, (uint16_t)total);
	put16(ntb_in + 10, 12);
	put32(ntb_in + 12, 0x304D434E);                      /* "NCM0" */
	put16(ntb_in + 16, 16);
	put16(ntb_in + 18, 0);
	put16(ntb_in + 20, (uint16_t)dg);
	put16(ntb_in + 22, (uint16_t)len);
	memcpy(ntb_in + dg, frame, len);
	if (musb_send(2, ntb_in, total, musb_high_speed() ? 512 : 64) == 0) n_tx++;
}

/* ---- a very small IPv4 responder ---- */
static uint16_t csum(const uint8_t *p, int n, uint32_t s)
{
	for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
	if (n) s += (uint32_t)(p[0] << 8);
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}

static uint8_t tx[1600];

static void ip_header(uint8_t *ip, int payload, uint8_t proto, const uint8_t *dst)
{
	memset(ip, 0, 20);
	ip[0] = 0x45; pbe16(ip + 2, (uint16_t)(20 + payload)); ip[8] = 64; ip[9] = proto;
	memcpy(ip + 12, my_ip, 4); memcpy(ip + 16, dst, 4);
	pbe16(ip + 10, csum(ip, 20, 0));
}

static void on_arp(const uint8_t *f, int len)
{
	const uint8_t *a = f + 14;
	if (len < 42 || be16(a + 6) != 1 || memcmp(a + 24, my_ip, 4)) return;      /* a request for us */
	n_arp++;
	memcpy(tx, a + 8, 6); memcpy(tx + 6, my_mac, 6); pbe16(tx + 12, 0x0806);
	uint8_t *r = tx + 14;
	pbe16(r, 1); pbe16(r + 2, 0x0800); r[4] = 6; r[5] = 4; pbe16(r + 6, 2);
	memcpy(r + 8, my_mac, 6); memcpy(r + 14, my_ip, 4);
	memcpy(r + 18, a + 8, 6); memcpy(r + 24, a + 14, 4);
	eth_send(tx, 42);
}

static void on_icmp(const uint8_t *f, int len)
{
	const uint8_t *ip = f + 14;
	int ihl = (ip[0] & 15) * 4, tot = be16(ip + 2);
	const uint8_t *ic = ip + ihl;
	int n = tot - ihl;
	if (ic[0] != 8 || 14 + tot > len || n < 8 || n > 1480) return;           /* echo request (up to a full 1500-byte packet) */
	n_ping++;
	memcpy(tx, f + 6, 6); memcpy(tx + 6, my_mac, 6); pbe16(tx + 12, 0x0800);
	uint8_t *o = tx + 34;
	memcpy(o, ic, n);
	o[0] = 0; o[2] = o[3] = 0;
	pbe16(o + 2, csum(o, n, 0));
	ip_header(tx + 14, n, 1, ip + 12);
	eth_send(tx, 34 + n);
}

static void on_dhcp(const uint8_t *f, int len)
{
	const uint8_t *b = f + 42;                           /* BOOTP after Ethernet 14 + IP 20 + UDP 8 */
	if (len < 42 + 240 || b[0] != 1 || get32(b + 236) != 0x63538263) return;   /* request with magic cookie */
	int type = 0;
	for (const uint8_t *o = b + 240; o < f + len && *o != 255; o += o[0] ? 2 + o[1] : 1)
		if (o[0] == 53) type = o[2];
	if (type != 1 && type != 3) return;                  /* DISCOVER or REQUEST */
	n_dhcp++;
	static const uint8_t bcast[4] = { 255, 255, 255, 255 };
	memset(tx, 0, 42 + 300);
	memset(tx, 0xFF, 6); memcpy(tx + 6, my_mac, 6); pbe16(tx + 12, 0x0800);
	uint8_t *u = tx + 34, *d = tx + 42;
	d[0] = 2; d[1] = 1; d[2] = 6;
	memcpy(d + 4, b + 4, 4);                             /* xid */
	memcpy(d + 10, b + 10, 2);                           /* flags */
	memcpy(d + 16, peer_ip, 4);                          /* yiaddr */
	memcpy(d + 20, my_ip, 4);                            /* siaddr */
	memcpy(d + 28, b + 28, 16);                          /* chaddr */
	put32(d + 236, 0x63538263);
	uint8_t *o = d + 240;
	*o++ = 53; *o++ = 1; *o++ = type == 1 ? 2 : 5;       /* OFFER or ACK */
	*o++ = 54; *o++ = 4; memcpy(o, my_ip, 4); o += 4;    /* server id */
	*o++ = 51; *o++ = 4; pbe16(o, 0); pbe16(o + 2, 3600); o += 4;   /* lease 1 h */
	*o++ = 1; *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
	*o++ = 255;
	int blen = (int)(o - d);
	if (blen < 300) blen = 300;
	pbe16(u, 67); pbe16(u + 2, 68); pbe16(u + 4, (uint16_t)(8 + blen)); pbe16(u + 6, 0);
	ip_header(tx + 14, 8 + blen, 17, bcast);
	eth_send(tx, 42 + blen);
}

static void on_frame(const uint8_t *f, int len)
{
	if (len < 14) return;
	n_rx++;
	uint16_t type = be16(f + 12);
	if (type != last_types[0]) { for (int i = 3; i > 0; i--) last_types[i] = last_types[i - 1]; last_types[0] = type; }
	if (type == 0x0806) on_arp(f, len);
	else if (type == 0x0800 && len >= 34 && (f[14] >> 4) == 4) {
		uint8_t proto = f[23];
		if (proto == 1 && !memcmp(f + 30, my_ip, 4)) on_icmp(f, len);
		else if (proto == 17 && len >= 42 && be16(f + 36) == 67) on_dhcp(f, len);
		else n_other++;
	} else n_other++;
}

/* ---- receiving NTBs ---- */
static uint8_t ntb_out[NTB_OUT_MAX + 512];
static int out_len;

static void parse_ntb(const uint8_t *b, int n)
{
	if (n < 12 || get32(b) != 0x484D434E) { n_bad++; return; }
	int blk = get16(b + 8), ndp = get16(b + 10);
	if (blk > n) blk = n;
	while (ndp && ndp + 8 <= blk) {
		if (get32(b + ndp) != 0x304D434E) { n_bad++; return; }
		int ndplen = get16(b + ndp + 4), next = get16(b + ndp + 6);
		for (int i = ndp + 8; i + 4 <= ndp + ndplen && i + 4 <= blk; i += 4) {
			int idx = get16(b + i), l = get16(b + i + 2);
			if (!idx || !l) break;
			if (idx + l <= blk) on_frame(b + idx, l);
		}
		ndp = next;
	}
}

static void rx_poll(void)
{
	if (!data_on || !musb_rx_ready(2)) return;
	int mp = musb_high_speed() ? 512 : 64;
	int k = musb_read(2, ntb_out + out_len, (int)sizeof ntb_out - out_len);
	out_len += k;
	int want = out_len >= 12 ? get16(ntb_out + 8) : 0;
	if (k < mp || (want && out_len >= want) || out_len >= NTB_OUT_MAX) {   /* end of the transfer */
		if (out_len) parse_ntb(ntb_out, out_len);
		out_len = 0;
	}
}

/* ---- screen ---- */
static const char hexd[] = "0123456789ABCDEF";
static char *hex16(char *p, uint16_t v) { for (int i = 12; i >= 0; i -= 4) *p++ = hexd[(v >> i) & 15]; *p = 0; return p; }
static char *hex32(char *p, uint32_t v) { for (int i = 28; i >= 0; i -= 4) *p++ = hexd[(v >> i) & 15]; *p = 0; return p; }

static void status(uint32_t secs_left)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x101820);
	char s[96], *p;
	uint32_t resets, setups;
	musb_stats(&resets, &setups);
	gfx_text(40, 30, "USB network test (NCM)", &font16x32, 2, 0xffffff);
	p = str_cat(s, "resets "); p = str_u(p, resets); p = str_cat(p, "  setups "); p = str_u(p, setups);
	p = str_cat(p, musb_high_speed() ? "  high speed" : "  full speed");
	gfx_text(40, 110, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, musb_configured() ? (data_on ? "LINK UP" : "configured, link down") : "not configured");
	gfx_text(40, 180, s, &font16x32, 2, data_on ? 0x40ff60 : 0xffc040);
	p = str_cat(s, "frames in "); p = str_u(p, n_rx); p = str_cat(p, "  out "); p = str_u(p, n_tx);
	p = str_cat(p, "  bad "); p = str_u(p, n_bad);
	gfx_text(40, 250, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "dhcp "); p = str_u(p, n_dhcp); p = str_cat(p, "  arp "); p = str_u(p, n_arp);
	p = str_cat(p, "  PING "); p = str_u(p, n_ping);
	gfx_text(40, 320, s, &font16x32, 2, n_ping ? 0x40ff60 : 0xffffff);
	p = str_cat(s, "recent types ");
	for (int i = 0; i < 4; i++) { p = hex16(p, last_types[i]); *p++ = ' '; *p = 0; }
	gfx_text(40, 400, s, &font16x32, 1, 0xa0c0ff);
	if (secs_left) { p = str_cat(s, "running: "); p = str_u(p, secs_left); p = str_cat(p, " s left"); }
	else p = str_cat(s, "DONE: take a photo, then power-cycle");
	gfx_text(40, 580, s, &font16x32, 1, 0x808080);
	dcache_flush();
	display_flip(fb);
}

void on_trap(void)
{
	uint64_t cause, epc, tval;
	__asm__ volatile("csrr %0, mcause\n\tcsrr %1, mepc\n\tcsrr %2, mtval" : "=r"(cause), "=r"(epc), "=r"(tval));
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0xa00000);
	char s[64], *p;
	gfx_text(40, 60, "NCM test crashed", &font16x32, 3, 0xffffff);
	p = str_cat(s, (cause >> 63) ? "interrupt " : "cause "); p = hex32(p, (uint32_t)cause);
	gfx_text(40, 200, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "pc    "); p = hex32(p, (uint32_t)epc);
	gfx_text(40, 270, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "addr  "); p = hex32(p, (uint32_t)tval);
	gfx_text(40, 340, s, &font16x32, 2, 0xffffff);
	dcache_flush();
	display_flip(fb);
	for (;;) ;
}
__asm__(".align 4\n ncm_trap_entry:\n la sp, __stack_top\n call on_trap\n 1: j 1b\n");
extern char ncm_trap_entry[];

int main(void)
{
	log_reset();
	if (display_up(FB0)) logf("ncmtest: display failed\n");
	display_brightness(10);
	__asm__ volatile("csrw mtvec, %0" :: "r"(ncm_trap_entry));
	__asm__ volatile("csrci mstatus, 8\n\tcsrw mie, zero");   /* the boot ROM's USB interrupt handler must stay out */
	musb_start(&ncm);
	uint32_t t0 = millis(), shown = 0;
	while (millis() - t0 < SECONDS * 1000u) {
		musb_poll();
		if (notify_pending) { notify_pending = 0; notify(); }
		rx_poll();
		if (millis() - shown >= 300) { shown = millis(); status(SECONDS - (millis() - t0) / 1000); }
	}
	for (;;) { musb_poll(); rx_poll(); status(0); udelay(200000); }
	return 0;
}
