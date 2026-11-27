/*
 * carplaytest.c: the wired-CarPlay USB role switch, as stock does it
 * (docs/carplay-usb.md). USB0 starts as host with USB-A's 5 V on (PD18), reads the
 * iPhone (as iphonetest.c does), then sends exactly what stock sends:
 *   SET_CONFIGURATION 1;  C0 53 0000 0000 0004 (capability query, 4 bytes back);
 *   40 51 0000 0000 0000 (role switch), only if the query's first byte is 01.
 * The phone then becomes the USB host. USB0 turns into a device presenting stock's
 * gadget layout: an iAP interface (FF/F0/00, "iAP Interface") and CDC-NCM. Once the
 * phone configures it we send the 6-byte iAP2 "detect" packet (FF 55 02 00 EE 10)
 * and show what the phone sends back, plus NCM frame counts. No authentication,
 * no link set-up beyond that: the phone will give up on its own. The serial-number
 * string is never read. Unplugging the phone ends everything; power-cycle to rerun.
 *
 * Run with: tools/fel/felrun.py dram/carplaytest.bin --detach
 */
#include "lib.h"
#include "musb.h"
#include "display.h"
#include "gfx.h"

#define USB0      0x04100000u
#define FIFO0     (USB0 + 0x00)
#define POWER     (USB0 + 0x40)
#define DEVCTL    (USB0 + 0x41)
#define INDEX     (USB0 + 0x42)
#define VEND0     (USB0 + 0x43)
#define INTRTX    (USB0 + 0x44)
#define INTRRX    (USB0 + 0x46)
#define INTRUSB   (USB0 + 0x4C)
#define INTRUSBE  (USB0 + 0x50)
#define CSR0      (USB0 + 0x82)
#define COUNT0    (USB0 + 0x88)
#define TYPE0     (USB0 + 0x8A)
#define NAKLIM0   (USB0 + 0x8B)
#define FADDR     (USB0 + 0x98)       /* device mode */
#define TXFUNCADDR (USB0 + 0x98)      /* host mode, indexed */
#define TXHUBADDR (USB0 + 0x9A)
#define TXHUBPORT (USB0 + 0x9B)
#define RXFUNCADDR (USB0 + 0x9C)
#define ISCR      (USB0 + 0x400)

#define H_RXPKTRDY  0x01
#define H_TXPKTRDY  0x02
#define H_RXSTALL   0x04
#define H_SETUPPKT  0x08
#define H_ERROR     0x10
#define H_REQPKT    0x20
#define H_STATUSPKT 0x40
#define H_NAKTIMEOUT 0x80

static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint16_t rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }

static const char hexd[] = "0123456789ABCDEF";
static char *hexn(char *p, uint32_t v, int digits) { for (int i = (digits - 1) * 4; i >= 0; i -= 4) *p++ = hexd[(v >> i) & 15]; *p = 0; return p; }

/* ---- what we learned, for the screen ---- */
static char st_line[96] = "waiting";
static int connected, speed;                    /* speed: 0 none, 1 low, 2 full, 3 high */
static uint8_t devd[18];
static int have_dev;
static char prod[64], manu[64];
static int mp0 = 64;
static uint32_t n_conn, n_xfer_err;
static uint8_t last_csr, devctl0;

/* ---- host-mode control transfers on EP0 ---- */
static int wait_csr(uint16_t clear, uint16_t set, int ms)
{
	uint32_t t0 = millis();
	for (;;) {
		uint16_t c = rd16(CSR0);
		last_csr = (uint8_t)c;
		if (c & (H_RXSTALL | H_ERROR | H_NAKTIMEOUT)) { wr16(CSR0, 0); return -(int)c; }
		if (set ? (c & set) : !(c & clear)) return 0;
		if ((int)(millis() - t0) > ms) return -1000;
	}
}

static int control_in(int addr, const uint8_t setup[8], uint8_t *buf, int len, int mp)
{
	wr8(INDEX, 0);
	wr8(TXFUNCADDR, (uint8_t)addr); wr8(TXHUBADDR, 0); wr8(TXHUBPORT, 0);
	wr8(TYPE0, speed == 3 ? 0x40 : speed == 2 ? 0x80 : 0xC0);
	wr8(NAKLIM0, 16);
	for (int i = 0; i < 8; i++) wr8(FIFO0, setup[i]);
	wr16(CSR0, H_SETUPPKT | H_TXPKTRDY);
	int e = wait_csr(H_TXPKTRDY, 0, 200);
	if (e) return e;
	int got = 0;
	while (got < len) {
		wr16(CSR0, H_REQPKT);
		if ((e = wait_csr(0, H_RXPKTRDY, 500))) return e;
		int k = rd16(COUNT0);
		for (int i = 0; i < k; i++) { uint8_t b = rd8(FIFO0); if (got + i < len) buf[got + i] = b; }
		wr16(CSR0, 0);
		got += k;
		if (k < mp) break;
	}
	wr16(CSR0, H_STATUSPKT | H_TXPKTRDY);                /* status stage: a zero-length OUT */
	if ((e = wait_csr(H_TXPKTRDY, 0, 200))) return e;
	wr16(CSR0, 0);
	return got < len ? got : len;
}

static int control_nodata(int addr, const uint8_t setup[8])
{
	wr8(INDEX, 0);
	wr8(TXFUNCADDR, (uint8_t)addr);
	for (int i = 0; i < 8; i++) wr8(FIFO0, setup[i]);
	wr16(CSR0, H_SETUPPKT | H_TXPKTRDY);
	int e = wait_csr(H_TXPKTRDY, 0, 200);
	if (e) return e;
	wr16(CSR0, H_STATUSPKT | H_REQPKT);                  /* status stage: a zero-length IN */
	if ((e = wait_csr(0, H_RXPKTRDY, 500))) return e;
	wr16(CSR0, 0);
	return 0;
}

static int get_desc(int addr, int type, int idx, uint8_t *buf, int len, int mp)
{
	uint8_t s[8] = { 0x80, 6, (uint8_t)idx, (uint8_t)type, type == 3 ? 0x09 : 0, type == 3 ? 0x04 : 0, (uint8_t)len, (uint8_t)(len >> 8) };
	return control_in(addr, s, buf, len, mp);
}

static void ascii(const uint8_t *d, int n, char *out, int max)
{
	int k = 0;
	for (int i = 2; i + 1 < n && i < d[0] && k < max - 1; i += 2) out[k++] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '?';
	out[k] = 0;
}


static int enumerate(void)
{
	have_dev = 0; prod[0] = manu[0] = 0;
	wr8(POWER, rd8(POWER) | 0x20 | 0x08);                /* bus reset, 50 ms */
	udelay(50000);
	wr8(POWER, rd8(POWER) & ~0x08);
	udelay(20000);
	uint8_t dc = rd8(DEVCTL), pw = rd8(POWER);
	speed = (pw & 0x10) ? 3 : (dc & 0x20) ? 1 : 2;
	uint8_t b[256];
	int e = get_desc(0, 1, 0, b, 8, 8);
	if (e < 8) { str_cat(str_u(str_cat(st_line, "device descriptor failed: "), (uint32_t)(-e)), ""); n_xfer_err++; return -1; }
	mp0 = b[7] ? b[7] : 8;
	uint8_t sa[8] = { 0x00, 5, 1, 0, 0, 0, 0, 0 };      /* SET_ADDRESS 1 */
	if ((e = control_nodata(0, sa))) { str_cat(str_u(str_cat(st_line, "SET_ADDRESS failed: "), (uint32_t)(-e)), ""); n_xfer_err++; return -1; }
	udelay(10000);
	if (get_desc(1, 1, 0, devd, 18, mp0) != 18) { str_cat(st_line, "device descriptor (18) failed"); n_xfer_err++; return -1; }
	have_dev = 1;
	if (devd[14] && (e = get_desc(1, 3, devd[14], b, 255, mp0)) > 2) ascii(b, e, manu, sizeof manu);
	if (devd[15] && (e = get_desc(1, 3, devd[15], b, 255, mp0)) > 2) ascii(b, e, prod, sizeof prod);
	return 0;
}

/* ---- the role switch (host side) ---- */
static int phase;                         /* 0 waiting for the phone, 1 switch sent, now a device */
static uint8_t q53[4];
static int r_setcfg = 1, r53 = 1, r51 = 1;   /* results; 1 = not tried */
static char sw_line[96] = "";
static int disc_ms = -2;                 /* ms until the phone dropped off after 0x51; -1 never */

static void role_switch(void)
{
	char *p;
	uint8_t sc[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };      /* SET_CONFIGURATION 1, as stock's kernel does */
	r_setcfg = control_nodata(1, sc);
	udelay(20000);
	uint8_t s53[8] = { 0xC0, 0x53, 0, 0, 0, 0, 4, 0 };  /* Apple capability query */
	r53 = control_in(1, s53, q53, 4, mp0);
	p = str_cat(sw_line, "setcfg "); p = str_cat(p, r_setcfg ? "FAIL" : "ok");
	p = str_cat(p, "  C0 53 -> ");
	if (r53 == 4) for (int i = 0; i < 4; i++) { p = hexn(p, q53[i], 2); *p++ = ' '; *p = 0; }
	else { p = str_cat(p, "error "); p = str_u(p, (uint32_t)(-r53)); }
	if (r53 != 4 || q53[0] != 0x01) { str_cat(p, " (stock stops here)"); return; }
	uint8_t s51[8] = { 0x40, 0x51, 0, 0, 0, 0, 0, 0 };  /* role switch */
	r51 = control_nodata(1, s51);
	p = str_cat(p, " 40 51 -> "); p = r51 ? str_u(str_cat(p, "error "), (uint32_t)(-r51)) : str_cat(p, "ok");
	if (r51 && (rd8(DEVCTL) & 0x04)) {                   /* rejected and still attached: try wValue 1 (CSDN post) */
		s51[2] = 1;
		r51 = control_nodata(1, s51);
		p = str_cat(p, ", w1 -> "); p = r51 ? str_u(str_cat(p, "error "), (uint32_t)(-r51)) : str_cat(p, "ok");
	}
	/* the phone should now drop off the bus (it stops being a device) */
	uint32_t t0 = millis();
	disc_ms = -1;
	while (millis() - t0 < 2000) {
		uint8_t u = rd8(INTRUSB);
		if (u) wr8(INTRUSB, u);
		if (u & 0x20) { disc_ms = (int)(millis() - t0); break; }
	}
	phase = 1;
}

/* ---- device side: stock's gadget layout (docs/carplay-usb.md section 4) ---- */
static const uint8_t g_dev[18] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 64,
	0x25, 0x05, 0xA1, 0xA4,               /* 0525:A4A1, the IDs stock uses */
	0x16, 0x03, 1, 2, 3, 1,
};
#define G_LEN 117
#define G_CFG(lo, hi)                                                                         \
	{ 9, 2, G_LEN, 0, 3, 1, 0, 0xC0, 0,                                                     \
	  9, 4, 0, 0, 2, 0xFF, 0xF0, 0x00, 4,        /* if0: iAP, "iAP Interface" */            \
	  7, 5, 0x81, 2, lo, hi, 0,                                                             \
	  7, 5, 0x01, 2, lo, hi, 0,                                                             \
	  8, 0x0B, 1, 2, 0x02, 0x0D, 0x00, 5,         /* IAD (stock's iFunction 8 has no string) */ \
	  9, 4, 1, 0, 1, 0x02, 0x0D, 0x00, 5,         /* if1: CDC-NCM */                         \
	  5, 0x24, 0x00, 0x10, 0x01,                                                            \
	  5, 0x24, 0x06, 1, 2,                                                                  \
	  13, 0x24, 0x0F, 7, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,                                   \
	  6, 0x24, 0x1A, 0x00, 0x01, 0x11,                                                      \
	  7, 5, 0x84, 3, 64, 0, 1,                                                              \
	  9, 4, 2, 0, 0, 0x0A, 0x00, 0x01, 6,                                                   \
	  9, 4, 2, 1, 2, 0x0A, 0x00, 0x01, 6,                                                   \
	  7, 5, 0x82, 2, lo, hi, 0,                                                             \
	  7, 5, 0x02, 2, lo, hi, 0 }
static const uint8_t g_cfg_hs[G_LEN] = G_CFG(0x00, 0x02);
static const uint8_t g_cfg_fs[G_LEN] = G_CFG(64, 0);
static const char *const g_strings[] = {
	"headunit-toolkit", "NCM Gadget", "F133-0003", "iAP Interface",
	"CDC Network Control Model (NCM)", "CDC Network Data", "02F133000001",
};
static const struct musb_ep g_eps[] = {
	{ 1, 1, 2, 512 }, { 1, 0, 2, 512 }, { 4, 1, 3, 64 }, { 2, 1, 2, 512 }, { 2, 0, 2, 512 },
};

static int g_config, ncm_on, notify_pending, detect_sent;
static uint32_t cfg_at, n_iap_in, n_iap_pkts, n_frames, n_ntb_bad, n_class;
static uint8_t iap_first[24];
static int iap_first_n;
static uint16_t types[4];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static int g_setup(const uint8_t r[8], uint8_t *buf, const uint8_t *out, int outlen)
{
	(void)out; (void)outlen;
	n_class++;
	switch (r[1]) {                                      /* CDC-NCM class requests */
	case 0x80:
		memset(buf, 0, 28);
		put16(buf, 28); put16(buf + 2, 1); put32(buf + 4, 2048);
		put16(buf + 8, 4); put16(buf + 12, 4); put32(buf + 16, 2048);
		put16(buf + 20, 4); put16(buf + 24, 4); put16(buf + 26, 1);
		return 28;
	case 0x85: put32(buf, 2048); return 4;
	case 0x86: case 0x43: case 0x84: case 0x88: return 0;
	case 0x83: put16(buf, 0); return 2;
	case 0x87: put16(buf, 1514); return 2;
	}
	return -1;
}
static void g_configured(int c) { g_config = c; if (c) cfg_at = millis(); else { ncm_on = 0; detect_sent = 0; } }
static void g_set_alt(int iface, int a) { if (iface == 2) { ncm_on = a == 1; notify_pending = ncm_on; } }

static const struct musb_class gadget = {
	g_dev, g_cfg_hs, g_cfg_fs, g_strings, 7, g_eps, 5, g_setup, g_configured, g_set_alt,
};

static uint8_t devctl_dev;
static int attempts, gap, variant;
/* Host to device. Variant 0: end the session, force ID high, reconnect (the first try).
   1: also stock's PHY steps (udc_init in the SDK): PHY control 0x410 VBUSVLDEXT on and
   SIDDQ off, PHY select 0x420 OTG. 2: reset the whole USB0 (OTG) block in the CCU
   first, so the controller starts clean as a device, then as 1. */
static void become_device(void)
{
	wr8(POWER, 0x20);                                    /* soft-disconnect */
	wr8(DEVCTL, 0);                                      /* end the host session; PD18 (5 V) stays as it is */
	if (variant >= 2) {
		wr32(0x02001A8C, rd32(0x02001A8C) & ~(1u << 24));   /* USB_BGR: USBOTG0 reset asserted */
		udelay(2000);
		wr32(0x02001A8C, rd32(0x02001A8C) | (1u << 24) | (1u << 8));   /* released, clock gate on */
		udelay(2000);
	}
	uint32_t iscr = rd32(ISCR) & ~(0x70u);
	iscr = (iscr & ~((3u << 14) | (3u << 12))) | (3u << 14) | (3u << 12) | (1u << 17) | (1u << 16);
	wr32(ISCR, iscr);                                    /* ID high (device), VBUS valid */
	if (variant >= 1) {
		wr32(USB0 + 0x410, (rd32(USB0 + 0x410) | 0x20) & ~0x08u);   /* VBUSVLDEXT on, SIDDQ off */
		wr32(USB0 + 0x420, rd32(USB0 + 0x420) | 1);                 /* PHY to the OTG controller */
	}
	udelay(10000);
	wr8(DEVCTL, 0x01);
	musb_disconnect_ms = 0;                              /* stock connects straight away */
	musb_start(&gadget);
	devctl_dev = rd8(DEVCTL);
}

static void device_poll(void)
{
	musb_poll();
	if (notify_pending) {
		notify_pending = 0;
		uint8_t sp[16] = { 0xA1, 0x2A, 0, 0, 1, 0, 8, 0 };
		put32(sp + 8, 480000000u); put32(sp + 12, 480000000u);
		uint8_t nc[8] = { 0xA1, 0x00, 1, 0, 1, 0, 0, 0 };
		musb_send(4, sp, 16, 64);
		musb_send(4, nc, 8, 64);
	}
	if (g_config && !detect_sent && millis() - cfg_at > 100) {
		static const uint8_t detect[6] = { 0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10 };   /* iAP2 detect */
		if (musb_write(1, detect, 6) == 6) detect_sent = 1;
	}
	static uint8_t b[512];
	if (g_config && musb_rx_ready(1)) {                  /* iAP bulk OUT */
		int k = musb_read(1, b, sizeof b);
		n_iap_pkts++; n_iap_in += (uint32_t)k;
		for (int i = 0; i < k && iap_first_n < (int)sizeof iap_first; i++) iap_first[iap_first_n++] = b[i];
	}
	static uint8_t ntb[2048 + 512];
	static int nl;
	if (ncm_on && musb_rx_ready(2)) {                    /* NCM bulk OUT: count frames and their types */
		int k = musb_read(2, ntb + nl, (int)sizeof ntb - nl);
		nl += k;
		int want = nl >= 12 ? get16(ntb + 8) : 0;
		if (k < 512 || (want && nl >= want) || nl >= 2048) {
			if (nl >= 12 && get32(ntb) == 0x484D434E) {
				int blk = get16(ntb + 8) < nl ? get16(ntb + 8) : nl, ndp = get16(ntb + 10);
				if (ndp && ndp + 8 <= blk && get32(ntb + ndp) == 0x304D434E)
					for (int i = ndp + 8; i + 4 <= ndp + get16(ntb + ndp + 4) && i + 4 <= blk; i += 4) {
						int idx = get16(ntb + i), l = get16(ntb + i + 2);
						if (!idx || !l || idx + l > blk || l < 14) break;
						n_frames++;
						uint16_t ty = (uint16_t)(ntb[idx + 12] << 8 | ntb[idx + 13]);
						if (ty != types[0]) { for (int j = 3; j > 0; j--) types[j] = types[j - 1]; types[0] = ty; }
					}
			} else if (nl) n_ntb_bad++;
			nl = 0;
		}
	}
}

/* ---- screen ---- */
static void status(void)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x101820);
	char s[128], *p;
	gfx_text(30, 16, "CarPlay USB role-switch test", &font16x32, 2, 0xffffff);
	if (phase == 0) {
		if (!connected) {
			gfx_text(30, 110, "1. Unplug the Mac's USB cable from USB-A", &font16x32, 1, 0xffc040);
			gfx_text(30, 150, "2. Plug in your iPhone (unlocked)", &font16x32, 1, 0xffc040);
		}
		p = str_cat(s, (rd8(DEVCTL) & 0x04) ? "HOST " : "NOT HOST ");
		p = str_cat(p, "devctl "); p = hexn(p, rd8(DEVCTL), 2);
		p = str_cat(p, (rd32(0x020000A0) >> 18) & 1 ? "  5V on" : "  5V off");
		p = str_cat(p, "  connects "); p = str_u(p, n_conn); p = str_cat(p, "  errors "); p = str_u(p, n_xfer_err);
		gfx_text(30, 200, s, &font16x32, 1, 0xa0c0ff);
		p = str_cat(s, connected ? "connected: " : "no device: "); p = str_cat(p, st_line);
		gfx_text(30, 240, s, &font16x32, 1, have_dev ? 0x40ff60 : 0xffffff);
	} else {
		uint32_t resets, setups;
		musb_stats(&resets, &setups);
		p = str_cat(s, "DEVICE v"); p = str_u(p, (uint32_t)variant); p = str_cat(p, "  devctl "); p = hexn(p, rd8(DEVCTL), 2); *p++ = '/'; p = hexn(p, devctl_dev, 2);
		p = str_cat(p, "  power "); p = hexn(p, rd8(POWER), 2);
		p = str_cat(p, "  drop "); if (disc_ms < 0) p = str_cat(p, "none"); else { p = str_u(p, (uint32_t)disc_ms); p = str_cat(p, "ms"); }
		p = str_cat(p, "  reconnects "); p = str_u(p, (uint32_t)attempts);
		if (attempts) { *p++ = '/'; p = str_u(p, (uint32_t)gap); p = str_cat(p, "ms"); }
		gfx_text(30, 200, s, &font16x32, 1, 0xa0c0ff);
		p = str_cat(s, "resets "); p = str_u(p, resets); p = str_cat(p, "  setups "); p = str_u(p, setups);
		p = str_cat(p, "  class "); p = str_u(p, n_class);
		p = str_cat(p, g_config ? "  CONFIGURED" : "  not configured");
		p = str_cat(p, musb_high_speed() ? "  high speed" : "");
		gfx_text(30, 240, s, &font16x32, 1, g_config ? 0x40ff60 : 0xffc040);
	}
	if (have_dev) {
		p = str_cat(s, "phone "); p = hexn(p, devd[8] | devd[9] << 8, 4); *p++ = ':'; p = hexn(p, devd[10] | devd[11] << 8, 4);
		p = str_cat(p, "  \""); p = str_cat(p, manu); p = str_cat(p, "\" \""); p = str_cat(p, prod); p = str_cat(p, "\"");
		gfx_text(30, 290, s, &font16x32, 1, 0xffffff);
	}
	if (sw_line[0]) gfx_text(30, 330, sw_line, &font16x32, 1, 0xffffff);
	if (phase == 1) {
		p = str_cat(s, "iAP: detect "); p = str_cat(p, detect_sent ? "sent" : "not sent");
		p = str_cat(p, "  in "); p = str_u(p, n_iap_in); p = str_cat(p, " bytes: ");
		for (int i = 0; i < iap_first_n && i < 16; i++) { p = hexn(p, iap_first[i], 2); *p++ = ' '; *p = 0; }
		gfx_text(30, 380, s, &font16x32, 1, n_iap_in ? 0x40ff60 : 0xffffff);
		p = str_cat(s, "NCM: "); p = str_cat(p, ncm_on ? "up" : "down"); p = str_cat(p, "  frames "); p = str_u(p, n_frames);
		p = str_cat(p, "  bad "); p = str_u(p, n_ntb_bad); p = str_cat(p, "  types ");
		for (int i = 0; i < 4; i++) { p = hexn(p, types[i], 4); *p++ = ' '; *p = 0; }
		gfx_text(30, 420, s, &font16x32, 1, n_frames ? 0x40ff60 : 0xffffff);
		uint8_t rq[6][8];                                /* the phone's recent requests to us */
		int n = musb_recent(rq, 6);
		for (int i = 0; i < n; i++) {
			p = s; for (int j = 0; j < 8; j++) { p = hexn(p, rq[i][j], 2); *p++ = ' '; *p = 0; }
			gfx_text(30 + (i / 3) * 480, 470 + (i % 3) * 34, s, &font16x32, 1, 0xa0c0ff);
		}
	}
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
	gfx_text(40, 60, "CarPlay test crashed", &font16x32, 3, 0xffffff);
	p = str_cat(s, (cause >> 63) ? "interrupt " : "cause "); p = hexn(p, (uint32_t)cause, 8);
	gfx_text(40, 200, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "pc    "); p = hexn(p, (uint32_t)epc, 8);
	gfx_text(40, 270, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "addr  "); p = hexn(p, (uint32_t)tval, 8);
	gfx_text(40, 340, s, &font16x32, 2, 0xffffff);
	dcache_flush();
	display_flip(fb);
	for (;;) ;
}
__asm__(".align 4\n cp_trap_entry:\n la sp, __stack_top\n call on_trap\n 1: j 1b\n");
extern char cp_trap_entry[];

#ifdef MACTEST
/* Self-test with the Mac on the cable (no iPhone, PD18 left off: the Mac supplies
   5 V): host mode for 3 s, then each host-to-device variant for 8 s until the Mac
   resets us. Watch for 0525:A4A1 in ioreg on the Mac. */
int main(void)
{
	log_reset();
	if (display_up(FB0)) logf("carplaytest: display failed\n");
	display_brightness(10);
	__asm__ volatile("csrw mtvec, %0" :: "r"(cp_trap_entry));
	__asm__ volatile("csrci mstatus, 8\n\tcsrw mie, zero");
	wr8(POWER, 0x20); udelay(200000);
	wr8(VEND0, 0); wr8(INTRUSBE, 0);
	wr8(DEVCTL, 0); udelay(200000);
	uint32_t iscr = rd32(ISCR) & ~(0x70u);
	iscr = (iscr & ~((3u << 14) | (3u << 12))) | (2u << 14) | (3u << 12) | (1u << 17) | (1u << 16);
	wr32(ISCR, iscr); udelay(100000);
	wr8(INTRUSB, rd8(INTRUSB));
	wr8(DEVCTL, 0x01);
	uint32_t t0 = millis();
	while (millis() - t0 < 3000) { if (!(rd8(DEVCTL) & 1)) wr8(DEVCTL, 1); status(); udelay(300000); }
	devctl0 = rd8(DEVCTL);
	str_cat(sw_line, "Mac self-test: host 3 s, then device variants");
	phase = 1;
	uint32_t shown = 0;
	for (variant = 0; ; variant = variant < 2 ? variant + 1 : 2) {
		become_device();
		uint32_t tv = millis(), resets = 0;
		while (millis() - tv < 8000 || variant == 2 || resets) {
			device_poll();
			musb_stats(&resets, 0);
			if (millis() - shown >= 300) { shown = millis(); status(); }
			if (resets && millis() - tv > 8000) break;
		}
		if (resets) { for (;;) { device_poll(); if (millis() - shown >= 300) { shown = millis(); status(); } } }
	}
	return 0;
}
#else
int main(void)
{
	log_reset();
	if (display_up(FB0)) logf("carplaytest: display failed\n");
	display_brightness(10);
	__asm__ volatile("csrw mtvec, %0" :: "r"(cp_trap_entry));
	__asm__ volatile("csrci mstatus, 8\n\tcsrw mie, zero");   /* keep the boot ROM's USB handler out */

	/* host mode with USB-A's 5 V on, as in iphonetest.c */
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 8)) | (1u << 8));    /* PD18 output */
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 18));                    /* PD18 high: VBUS on */
	wr8(POWER, 0x20);
	udelay(200000);
	wr8(VEND0, 0);
	wr8(INTRUSBE, 0);
	wr8(DEVCTL, 0);
	udelay(200000);
	uint32_t iscr = rd32(ISCR) & ~(0x70u);
	iscr = (iscr & ~((3u << 14) | (3u << 12))) | (2u << 14) | (3u << 12) | (1u << 17) | (1u << 16);
	wr32(ISCR, iscr);
	udelay(100000);
	wr8(INTRUSB, rd8(INTRUSB));
	wr8(DEVCTL, 0x01);
	udelay(100000);
	devctl0 = rd8(DEVCTL);
	uint32_t shown = 0;
	while (phase == 0) {
		uint8_t u = rd8(INTRUSB);
		if (u) wr8(INTRUSB, u);
		if (u & 0x10) {                                  /* CONN */
			connected = 1; n_conn++;
			str_cat(st_line, "reading...");
			status();
			udelay(200000);
			if (enumerate() == 0) {
				if ((devd[8] | devd[9] << 8) == 0x05AC) { str_cat(st_line, "read OK, sending the switch"); role_switch(); }
				else str_cat(st_line, "not an Apple device: nothing sent");
			}
		}
		if (u & 0x20) { connected = 0; have_dev = 0; speed = 0; str_cat(st_line, "disconnected"); }
		if (phase == 0 && !(rd8(DEVCTL) & 0x01)) wr8(DEVCTL, 0x01);
		if (millis() - shown >= 300) { shown = millis(); status(); }
	}
	variant = 2;                                         /* the only one that works (Mac self-test) */
	become_device();
	static const int gaps[] = { 100, 500, 1000, 2000 };
	uint32_t since = millis();
	for (;;) {
		device_poll();
		uint32_t resets;
		musb_stats(&resets, 0);
		if (!resets && millis() - since > 3000 && attempts < 8) {   /* phone hasn't found us: reconnect */
			gap = gaps[attempts % 4];
			attempts++;
			musb_stop();
			udelay(gap * 1000);
			wr8(POWER, 0x20 | 0x40);                     /* HS enabled, soft-connect */
			since = millis();
		}
		if (millis() - shown >= 300) { shown = millis(); status(); }
	}
	return 0;
}
#endif
