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
#include "twi.h"
#include "pair.h"
#include "hashes.h"
#include "bplist.h"
#include "third_party/monocypher/monocypher.h"
#include "third_party/monocypher/monocypher-ed25519.h"

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

static int g_config, ncm_on, notify_pending;
static uint32_t cfg_at, n_iap_in, n_frames, n_ntb_bad, n_class;
static uint16_t types[4], ports[4];
static uint32_t n_mdns, n_icmp6;

static int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
	return 0;
}
static void memmove_(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
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
	case 0x8A: return 0;                                 /* SET_CRC_MODE (we advertise it, as stock does): no CRC */
	case 0x89: put16(buf, 0); return 2;                  /* GET_CRC_MODE */
	case 0x83: put16(buf, 0); return 2;
	case 0x87: put16(buf, 1514); return 2;
	}
	return -1;
}
static void iap2_reset(void);
static void g_configured(int c) { g_config = c; if (c) cfg_at = millis(); else ncm_on = 0; iap2_reset(); }
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

/* ---- Apple MFi 3.0 coprocessor (TWI2, 0x10): certificate and challenge signing ----
   The chip wants: write the register address, STOP, then a separate read; it NACKs
   while busy, so retry. Registers: 0x10 control/status, 0x11 signature length,
   0x12 signature, 0x20 challenge length, 0x21 challenge, 0x30 certificate length,
   0x31.. certificate (128 bytes per register). The certificate is unique to the
   unit: it goes only to the phone and is never shown or logged. */
#define MFI 0x10
static uint8_t mfi_ver, mfi_proto;
static int cert_len, mfi_err, mfi_stage, mfi_len_read;
static uint8_t cert[1024];
static int sig_len, sig_ms, sig_status = -1;
static uint8_t sig[128];

static int mfi_last;                      /* last I2C error (TWI_*) */
static int mfi_rd(uint8_t reg, uint8_t *b, int n)
{
	for (int w = 0; w < 10; w++) {                       /* set the register pointer (the chip NACKs while busy) */
		if ((mfi_last = twi_write(2, MFI, reg, 1, 0, 0)) != TWI_OK) { udelay(2000); continue; }
		for (int i = 0; i < 100; i++) {                  /* then retry only the read, up to ~100 ms */
			udelay(1000);
			if ((mfi_last = twi_read(2, MFI, 0, 0, b, (unsigned)n)) == TWI_OK) return 0;
		}
	}
	mfi_err++;
	return -1;
}
static int mfi_wr(uint8_t reg, const uint8_t *b, int n)
{
	for (int i = 0; i < 40; i++) {
		if (twi_write(2, MFI, reg, 1, b, (unsigned)n) == TWI_OK) return 0;
		udelay(2000);
	}
	mfi_err++;
	return -1;
}
static void mfi_init(void)
{
	uint8_t b[2];
	twi_init(2);
	twi_timeout_us = 50000;                              /* the chip may stretch the clock while it fetches */
	if (!mfi_rd(0x00, b, 1)) mfi_ver = b[0];
	if (!mfi_rd(0x02, b, 1)) mfi_proto = b[0];
	mfi_stage = 1;                                       /* 1: length read failed */
	if (mfi_rd(0x30, b, 2)) return;
	mfi_len_read = b[0] << 8 | b[1];
	mfi_stage = 2;                                       /* 2: length too big */
	if (mfi_len_read > (int)sizeof cert) return;
	for (int off = 0, r = 0x31; off < mfi_len_read; off += 128, r++) {
		mfi_stage = 0x31 == r ? 0x31 : r;                /* 0x3n: reading that register failed */
		if (mfi_rd((uint8_t)r, cert + off, mfi_len_read - off < 128 ? mfi_len_read - off : 128)) return;
	}
	mfi_stage = 0;
	cert_len = mfi_len_read;
}
static int mfi_sign(const uint8_t *ch, int n)
{
	uint8_t b[2] = { (uint8_t)(n >> 8), (uint8_t)n }, st = 0;
	mfi_wr(0x20, b, 2);                                  /* challenge length (fixed at 32 on 3.0 chips) */
	if (mfi_wr(0x21, ch, n)) return -1;
	uint8_t go = 1;
	if (mfi_wr(0x10, &go, 1)) return -1;                 /* start signature generation */
	uint32_t t0 = millis();
	for (;;) {
		udelay(5000);
		if (!mfi_rd(0x10, &st, 1) && ((st >> 4) & 7)) break;
		if (millis() - t0 > 3000) return -2;
	}
	sig_ms = (int)(millis() - t0);
	sig_status = st;
	if (((st >> 4) & 7) != 1) return -3;                 /* 1 = signature generated */
	if (mfi_rd(0x11, b, 2)) return -1;
	sig_len = b[0] << 8 | b[1];
	if (sig_len > (int)sizeof sig) return -4;
	return mfi_rd(0x12, sig, sig_len);
}

/* ---- iAP2 link layer (just enough to open a link) ----
   Packet: FF 5A, length (BE, whole packet), control, seq, ack, session, header
   checksum, then payload + payload checksum (each checksum makes its bytes sum to 0).
   Control bits: SYN 80, ACK 40, EAK 20, RST 10, SLP 08. We send the detect packet,
   then a SYN offering one control session (id 10); the phone answers SYN+ACK; we ACK
   it and every data packet after, and show the control-message IDs it sends. */
#define IAP_SESS 10
static int ls;                            /* 0 idle, 1 detect sent, 2 detect answered, 3 SYN sent, 4 linked */
static uint8_t my_seq = 0x2B, their_seq;
static int syn_tries, got_detect, n_rst, n_msgs, n_data, n_badsum;
static uint32_t ls_at;
static uint8_t their_syn[24]; static int their_syn_n; static uint8_t their_ctl;
static uint16_t msg_ids[6]; static uint16_t msg_len[6];
static uint8_t last_data[16]; static int last_data_n;
static uint8_t irx[4096 + 512]; static int irx_n;

static uint8_t sum8(const uint8_t *p, int n) { uint8_t s = 0; while (n--) s += *p++; return (uint8_t)-s; }

static void link_send(uint8_t ctl, uint8_t sess, const uint8_t *pl, int n)
{
	static uint8_t o[2200];
	int len = 9 + (n ? n + 1 : 0);
	if (len > (int)sizeof o) return;
	o[0] = 0xFF; o[1] = 0x5A; o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len;
	o[4] = ctl; o[5] = my_seq; o[6] = (ctl & 0x40) ? their_seq : 0; o[7] = sess;
	o[8] = sum8(o, 8);
	if (n) { memcpy(o + 9, pl, n); o[9 + n] = sum8(pl, n); }
	musb_send(1, o, len, 512);
}

static void iap2_reset(void) { ls = 0; irx_n = 0; syn_tries = 0; }

/* one control-session message: 40 40, length, id, then parameters (length, id, data) */
static int n_sent_msgs;
static void send_msg(uint16_t id, uint16_t pid, const uint8_t *data, int n)
{
	static uint8_t m[1200];
	int len = 6 + 4 + n;
	if (len > (int)sizeof m) return;
	m[0] = 0x40; m[1] = 0x40; m[2] = (uint8_t)(len >> 8); m[3] = (uint8_t)len; m[4] = (uint8_t)(id >> 8); m[5] = (uint8_t)id;
	m[6] = (uint8_t)((n + 4) >> 8); m[7] = (uint8_t)(n + 4); m[8] = (uint8_t)(pid >> 8); m[9] = (uint8_t)pid;
	memcpy(m + 10, data, n);
	my_seq++;
	link_send(0x40, IAP_SESS, m, len);
	n_sent_msgs++;
}

/* ---- building a control message with nested parameters ----
   Parameter: 2-byte length (including this 4-byte header), 2-byte ID, data.
   A group parameter holds parameters as its data. */
static uint8_t mb[2048];
static int mb_n, mb_depth, mb_over;
static int mb_open[6];
static void mb_begin(uint16_t id) { mb[0] = 0x40; mb[1] = 0x40; mb[4] = (uint8_t)(id >> 8); mb[5] = (uint8_t)id; mb_n = 6; mb_depth = 0; mb_over = 0; }
static void mb_param(uint16_t pid, const void *d, int n)
{
	if (mb_n + 4 + n > (int)sizeof mb) { mb_over = 1; return; }
	mb[mb_n] = (uint8_t)((n + 4) >> 8); mb[mb_n + 1] = (uint8_t)(n + 4); mb[mb_n + 2] = (uint8_t)(pid >> 8); mb[mb_n + 3] = (uint8_t)pid;
	if (n) memcpy(mb + mb_n + 4, d, n);
	mb_n += 4 + n;
}
static void mb_none(uint16_t pid) { mb_param(pid, 0, 0); }
static void mb_u8(uint16_t pid, uint8_t v) { mb_param(pid, &v, 1); }
static void mb_u16(uint16_t pid, uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; mb_param(pid, b, 2); }
static void mb_u32(uint16_t pid, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; mb_param(pid, b, 4); }
static void mb_str(uint16_t pid, const char *s) { int n = 0; while (s[n]) n++; mb_param(pid, s, n + 1); }   /* UTF-8, NUL-terminated */
static void mb_group(uint16_t pid) { if (mb_depth < 6) { mb_open[mb_depth++] = mb_n; mb_param(pid, 0, 0); } }
static void mb_end(void)
{
	if (!mb_depth) return;
	int at = mb_open[--mb_depth], n = mb_n - at;
	mb[at] = (uint8_t)(n >> 8); mb[at + 1] = (uint8_t)n;
}
static void mb_send(void)
{
	if (mb_over) return;
	mb[2] = (uint8_t)(mb_n >> 8); mb[3] = (uint8_t)mb_n;
	my_seq++;
	link_send(0x40, IAP_SESS, mb, mb_n);
	n_sent_msgs++;
}

static int ident_result;                  /* 0 none, 1 accepted (1D02), -1 rejected (1D03) */
static int ident_len;                     /* bytes we sent */
static uint16_t rej_ids[8]; static int n_rej;
static void send_identification(void);    /* filled in from docs/carplay-iap2.md */
static void power_updates(void);

static int have_time; static uint64_t t_secs; static int t_tz, t_dst;
static void on_time(const uint8_t *m, int n)              /* 4E0B DeviceTimeUpdate */
{
	for (int o = 6; o + 4 <= n; ) {
		int pl = m[o] << 8 | m[o + 1], id = m[o + 2] << 8 | m[o + 3];
		if (pl < 4 || o + pl > n) break;
		const uint8_t *d = m + o + 4;
		if (id == 0 && pl == 12) { t_secs = 0; for (int i = 0; i < 8; i++) t_secs = t_secs << 8 | d[i]; have_time = 1; }
		if (id == 1 && pl == 6) t_tz = (int16_t)(d[0] << 8 | d[1]);
		if (id == 2 && pl == 5) t_dst = (int8_t)d[0];
		o += pl;
	}
}
static char *fmt_time(char *p, uint64_t secs)             /* Y-M-D h:m:s (civil from days) */
{
	int64_t z = (int64_t)(secs / 86400) + 719468;
	uint32_t sod = (uint32_t)(secs % 86400);
	int64_t era = z / 146097; uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = (int64_t)yoe + era * 400; uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153, dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
	if (mm <= 2) y++;
	p = str_u(p, (uint32_t)y); *p++ = '-'; if (mm < 10) *p++ = '0'; p = str_u(p, mm); *p++ = '-'; if (dd < 10) *p++ = '0'; p = str_u(p, dd);
	*p++ = ' '; uint32_t h = sod / 3600, mi = sod / 60 % 60, s = sod % 60;
	if (h < 10) *p++ = '0'; p = str_u(p, h); *p++ = ':'; if (mi < 10) *p++ = '0'; p = str_u(p, mi); *p++ = ':'; if (s < 10) *p++ = '0'; p = str_u(p, s);
	return p;
}

static int auth_result;                   /* 0 none, 1 succeeded (AA05), -1 failed (AA04), -2 chip error */
/* CarPlayAvailability (0x4300) -> CarPlayStartSession (0x4301): the step the public
   receivers use to start wired CarPlay (docs/carplay-hosting-plan.md, "What gets the
   phone to connect"). The public key is a placeholder until pairing exists. */
static int n_avail, n_start, avail_wired = -1;
static pair_ctx pc;
static char pc_hex[65];                   /* our long-term Ed25519 public key, hex */
static void start_session(void)
{
	mb_begin(0x4301);
	mb_group(0);                                         /* wired */
		mb_str(0, "fe80::f1:33ff:fe00:2");               /* our link-local, no scope */
	mb_end();
	mb_u32(2, 7000);                                     /* AirPlay port */
	mb_str(3, "02:f1:33:00:00:02");                      /* device id: our NCM MAC (= pairing identifier) */
	mb_str(4, pc_hex);                                   /* our long-term Ed25519 public key (pairing identity) */
	mb_str(5, "320.14");                                 /* source version, as in our TXT */
	mb_send();
	n_start++;
}
static void on_avail(const uint8_t *m, int n)
{
	n_avail++;
	for (int o = 6; o + 4 <= n; ) {                      /* group 0 (wired): param 0 = available */
		int pl = m[o] << 8 | m[o + 1], id = m[o + 2] << 8 | m[o + 3];
		if (pl < 4 || o + pl > n) break;
		if (id == 0 && pl >= 9 && (m[o + 6] << 8 | m[o + 7]) == 0) avail_wired = m[o + 8];
		o += pl;
	}
	start_session();
}

static void on_message(const uint8_t *m, int n)
{
	uint16_t id = (uint16_t)(m[4] << 8 | m[5]);
	if (id == 0xAA00) {                                  /* RequestAuthenticationCertificate */
		if (cert_len) send_msg(0xAA01, 0, cert, cert_len);
		else auth_result = -2;
	} else if (id == 0xAA02 && n >= 10) {                /* RequestAuthenticationChallengeResponse */
		int pl = (m[6] << 8 | m[7]) - 4;
		if (pl > 0 && 10 + pl <= n && mfi_sign(m + 10, pl) == 0) send_msg(0xAA03, 0, sig, sig_len);
		else auth_result = -2;
	} else if (id == 0xAA05) auth_result = 1;
	else if (id == 0x1D00) send_identification();
	else if (id == 0x4E0B) on_time(m, n);
	else if (id == 0x4300) on_avail(m, n);
	else if (id == 0x1D02) { ident_result = 1; power_updates(); }
	else if (id == 0x1D03) {                             /* the parameters it rejected */
		ident_result = -1;
		for (int o = 6; o + 4 <= n && n_rej < 8; ) {
			int pl = m[o] << 8 | m[o + 1];
			rej_ids[n_rej++] = (uint16_t)(m[o + 2] << 8 | m[o + 3]);
			if (pl < 4) break;
			o += pl;
		}
	}
	else if (id == 0xAA04) auth_result = -1;
}

static void send_syn(void)
{
	static const uint8_t syn[] = {
		0x01,             /* link version 1 */
		0x04,             /* max outstanding packets */
		0x10, 0x00,       /* max received packet length 4096 */
		0x05, 0xDC,       /* retransmission timeout 1500 ms */
		0x00, 0x49,       /* cumulative ACK timeout 73 ms */
		30,               /* max retransmissions */
		3,                /* max cumulative ACKs */
		IAP_SESS, 0, 1,   /* session 10: control, version 1 */
	};
	link_send(0x80, 0, syn, sizeof syn);
	syn_tries++; ls = 3; ls_at = millis();
}

static void on_link_packet(const uint8_t *p, int len)
{
	uint8_t ctl = p[4], seq = p[5], sess = p[7];
	int pn = len > 9 ? len - 10 : 0;                  /* payload bytes (without its checksum) */
	if (pn && sum8(p + 9, pn) != p[9 + pn]) { n_badsum++; return; }
	if (ctl & 0x10) { n_rst++; ls = 2; return; }      /* RST: start over with a new SYN */
	if (ctl & 0x80) {                                  /* the phone's SYN (+ACK): its parameters */
		their_ctl = ctl; their_seq = seq;
		their_syn_n = pn < (int)sizeof their_syn ? pn : (int)sizeof their_syn;
		memcpy(their_syn, p + 9, their_syn_n);
		link_send(0x40, 0, 0, 0);                      /* ACK */
		ls = 4;
		return;
	}
	if (pn) {                                          /* data: ACK it (again, if a retransmit) */
		int fresh = seq != their_seq;
		their_seq = seq;
		link_send(0x40, 0, 0, 0);
		if (!fresh) return;
		n_data++;
		last_data_n = pn < 16 ? pn : 16;
		memcpy(last_data, p + 9, last_data_n);
		const uint8_t *m = p + 9;
		if (sess == IAP_SESS && pn >= 6 && m[0] == 0x40 && m[1] == 0x40) {
			for (int i = 5; i > 0; i--) { msg_ids[i] = msg_ids[i - 1]; msg_len[i] = msg_len[i - 1]; }
			msg_ids[0] = (uint16_t)(m[4] << 8 | m[5]); msg_len[0] = (uint16_t)(m[2] << 8 | m[3]);
			n_msgs++;
			on_message(m, pn);
		}
	}
}

/* IdentificationInformation, laid out exactly as stock's carplay.mod sends it
   (docs/carplay-iap2.md section 3), with our own name. The serial and parameter
   34 are stock's placeholder, the same in every unit. */
#define ACC_NAME "Head Unit"
static void send_identification(void)
{
	static const uint8_t sent[] = { 0xEA, 0x02, 0xAE, 0x00, 0xAE, 0x02, 0xAE, 0x03, 0xAA, 0x06, 0x50, 0x00, 0x50, 0x02, 0x50, 0x03,
	                                0x43, 0x01 };   /* + CarPlayStartSession (xcertplay/LIVI; stock lacks it) */
	static const uint8_t recv[] = { 0xAE, 0x01, 0x50, 0x01, 0x4E, 0x0B, 0x43, 0x00 };   /* + CarPlayAvailability */
	static const uint8_t langs[] = { 'e', 'n', 0, 'z', 'h', 0 };   /* stock's single SupportedLanguage parameter */
	mb_begin(0x1D01);
	mb_str(0, ACC_NAME);                                 /* Name */
	mb_str(1, "Linux");                                  /* ModelIdentifier */
	mb_str(2, ACC_NAME);                                 /* Manufacturer */
	mb_str(3, "0123456789ABCDEF");                       /* SerialNumber (stock placeholder) */
	mb_str(4, "sw1.1.0");                                /* FirmwareVersion */
	mb_str(5, "hw1.1.0");                                /* HardwareVersion */
	mb_param(6, sent, sizeof sent);                      /* MessagesSentByAccessory */
	mb_param(7, recv, sizeof recv);                      /* MessagesReceivedFromDevice */
	mb_u8(8, 2);                                         /* PowerProvidingCapability: advanced */
	mb_u16(9, 0);                                        /* MaximumCurrentDrawnFromDevice */
	mb_str(12, "en");                                    /* CurrentLanguage */
	mb_param(13, langs, sizeof langs);                   /* SupportedLanguage */
	mb_group(16);                                        /* USBHostTransportComponent */
		mb_u16(0, 0); mb_str(1, "host"); mb_none(2);     /* id, name, supports iAP2 */
		mb_u8(3, 1);                                     /* CarPlay interface: 1 (our NCM control) */
		mb_none(4);                                      /* supports CarPlay */
	mb_end();
	mb_group(20);                                        /* VehicleInformationComponent */
		mb_u16(0, 0); mb_str(1, ACC_NAME); mb_u8(2, 0); mb_str(6, ACC_NAME);
	mb_end();
	mb_str(34, "0123456789ABCDEF");                      /* stock placeholder (ProductPlanUID?) */
	ident_len = mb_n;
	mb_send();
}

static void power_updates(void)                         /* stock's reply to IdentificationAccepted */
{
	mb_begin(0xAE00); mb_none(0); mb_none(1); mb_send();            /* StartPowerUpdates */
	mb_begin(0xAE03); mb_u16(0, 1000); mb_u8(1, 1); mb_send();      /* PowerSourceUpdate: 1000 mA, charge */
}

static void iap2_poll(void)
{
	static uint8_t b[512];
	if (musb_rx_ready(1)) {
		int k = musb_read(1, b, sizeof b);
		n_iap_in += (uint32_t)k;
		if (irx_n + k > (int)sizeof irx) irx_n = 0;
		memcpy(irx + irx_n, b, k); irx_n += k;
	}
	/* parse whatever complete packets the stream holds */
	for (;;) {
		int i = 0;
		while (i < irx_n && irx[i] != 0xFF) i++;
		if (i) { memmove_(irx, irx + i, irx_n - i); irx_n -= i; }
		if (irx_n < 6) break;
		if (irx[1] == 0x55 && irx[2] == 0x02 && irx[3] == 0x00 && irx[4] == 0xEE && irx[5] == 0x10) {   /* detect reply */
			got_detect = 1; if (ls < 2) ls = 2;
			memmove_(irx, irx + 6, irx_n - 6); irx_n -= 6; continue;
		}
		if (irx[1] != 0x5A) { memmove_(irx, irx + 1, irx_n - 1); irx_n--; continue; }
		if (irx_n < 9) break;
		int len = irx[2] << 8 | irx[3];
		if (len < 9 || len > 4096 || sum8(irx, 8) != irx[8]) { n_badsum++; memmove_(irx, irx + 1, irx_n - 1); irx_n--; continue; }
		if (irx_n < len) break;
		on_link_packet(irx, len);
		memmove_(irx, irx + len, irx_n - len); irx_n -= len;
	}
	/* drive the handshake */
	if (ls == 0 && millis() - cfg_at > 100) {
		static const uint8_t detect[6] = { 0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10 };
		if (musb_send(1, detect, 6, 512) == 0) { ls = 1; ls_at = millis(); }
	}
	if (ls == 1 && millis() - ls_at > 1000) ls = 0;      /* no answer: detect again */
	if (ls == 2) send_syn();
	if (ls == 3 && millis() - ls_at > 1500 && syn_tries < 6) send_syn();
}

/* ---- read-only mDNS decoding (what the phone announces and asks for) ----
   Only service-type names (labels starting with '_') are kept: instance names can
   contain the phone's name and are never stored or shown. */
#define NSEEN 12
static char seen[NSEEN][44];
static int n_seen;
static uint32_t n_dhcp_req, n_mdns_resp, n_pings_sent;
static int want_airplay;                  /* the phone asked for _airplay._tcp */
static uint8_t phone_ip4[4];

static int dns_name(const uint8_t *msg, int mlen, int off, char *out, int max, int *end)
{
	int k = 0, jumped = 0, hops = 0;
	out[0] = 0;
	while (off < mlen && hops++ < 32) {
		int l = msg[off];
		if (l == 0) { off++; break; }
		if ((l & 0xC0) == 0xC0) {
			if (off + 1 >= mlen) return -1;
			if (!jumped && end) *end = off + 2;
			jumped = 1;
			off = (l & 0x3F) << 8 | msg[off + 1];
			continue;
		}
		off++;
		if (off + l > mlen) return -1;
		if (k && k < max - 1) out[k++] = '.';
		for (int i = 0; i < l && k < max - 1; i++) { uint8_t c = msg[off + i]; out[k++] = (c >= 32 && c < 127) ? (char)c : '?'; }
		off += l;
	}
	out[k] = 0;
	if (!jumped && end) *end = off;
	return 0;
}

/* keep the part of a name from its first label that starts with '_' ("_carplay-ctrl._tcp.local") */
static const char *svc_part(const char *n)
{
	if (n[0] == '_') return n;
	for (const char *p = n; *p; p++) if (p[0] == '.' && p[1] == '_') return p + 1;
	return 0;
}

static void remember(char kind, const char *svc, const char *what, int port)
{
	char e[44], *p = e;
	*p++ = kind; *p++ = ' ';
	for (const char *s = svc; *s && p < e + 28; s++) *p++ = *s;
	*p++ = ' ';
	for (const char *s = what; *s && p < e + 36; s++) *p++ = *s;
	if (port >= 0) { *p++ = ' '; p = str_u(p, (uint32_t)port); }
	*p = 0;
	for (int i = 0; i < n_seen; i++) { const char *a = seen[i], *b = e; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) return; }
	if (n_seen < NSEEN) { char *d = seen[n_seen++]; for (const char *s = e; (*d++ = *s++); ) ; }
}

static const char *rtype(int ty) { return ty == 12 ? "PTR" : ty == 33 ? "SRV" : ty == 16 ? "TXT" : ty == 1 ? "A" : ty == 28 ? "AAAA" : ty == 255 ? "ANY" : "?"; }

static void on_mdns(const uint8_t *m, int n)
{
	if (n < 12) return;
	if ((m[2] & 0x80) && n_pings_sent) n_mdns_resp++;  /* responses after our first query */
	int qd = m[4] << 8 | m[5], rr = (m[6] << 8 | m[7]) + (m[8] << 8 | m[9]) + (m[10] << 8 | m[11]);
	int off = 12;
	char nm[96];
	for (int i = 0; i < qd && off < n; i++) {           /* questions: what the phone looks for */
		int end;
		if (dns_name(m, n, off, nm, sizeof nm, &end)) return;
		off = end;
		if (off + 4 > n) return;
		const char *s = svc_part(nm);
		if (s) remember('Q', s, rtype(m[off] << 8 | m[off + 1]), -1);
		if (s && s[1] == 'a' && s[2] == 'i' && s[3] == 'r' && s[8] == '.') want_airplay = 1;   /* "_airplay._tcp..." */
		off += 4;
	}
	for (int i = 0; i < rr && off < n; i++) {           /* answers: what the phone announces */
		int end;
		if (dns_name(m, n, off, nm, sizeof nm, &end)) return;
		off = end;
		if (off + 10 > n) return;
		int ty = m[off] << 8 | m[off + 1], rdl = m[off + 8] << 8 | m[off + 9];
		off += 10;
		if (off + rdl > n) return;
		const char *s = svc_part(nm);
		if (s && s[0] == '_' && !(s[1] == 's' && s[2] == 'e' && s[3] == 'r')) {   /* skip _services._dns-sd */
			if (ty == 33 && rdl >= 6) remember('A', s, "SRV", m[off + 4] << 8 | m[off + 5]);
			else if (ty == 12 || ty == 16) remember('A', s, rtype(ty), -1);
		} else if (ty == 12 && s) {                        /* _services._dns-sd._udp PTR -> a service type */
			char tg[96];
			if (!dns_name(m, n, off, tg, sizeof tg, 0) && tg[0] == '_') remember('A', tg, "type", -1);
		}
		off += rdl;
	}
}

/* ---- our side of the link: IPv6 link-local, neighbour discovery, mDNS answer ----
   MAC 02:F1:33:00:00:02 (the iMACAddress string names the host side, ...01, as
   in ncmtest.c). Link-local address from it by EUI-64: fe80::f1:33ff:fe00:2. */
static const uint8_t my_mac[6] = { 0x02, 0xF1, 0x33, 0x00, 0x00, 0x02 };
static const uint8_t my_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0xF1, 0x33, 0xFF, 0xFE, 0x00, 0x00, 0x02 };
static const uint8_t mdns6[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFB };
static const uint8_t all6[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01 };
static uint32_t n_ns, n_na_sent, n_ping6, n_mdns_ans, n_syn, n_pong;
static uint8_t phone_ll[16], phone_mac[6]; static int have_phone_ll;
static uint16_t syn_ports[4];
static uint16_t in_seq;
#define ADVERTISE 1                       /* answer _airplay._tcp as stock does (docs/carplay-iap2.md section 5) */

static void eth_send(const uint8_t *frame, int len)
{
	static uint8_t nb[1600 + 32];
	if (!ncm_on || len > 1514) return;
	int dg = 32, total = dg + len;
	memset(nb, 0, dg);
	put32(nb, 0x484D434E); put16(nb + 4, 12); put16(nb + 6, in_seq++); put16(nb + 8, (uint16_t)total); put16(nb + 10, 12);
	put32(nb + 12, 0x304D434E); put16(nb + 16, 16); put16(nb + 18, 0); put16(nb + 20, (uint16_t)dg); put16(nb + 22, (uint16_t)len);
	memcpy(nb + dg, frame, len);
	musb_send(2, nb, total, 512);
}

static uint16_t csum6(const uint8_t *src, const uint8_t *dst, uint8_t nh, const uint8_t *p, int n)
{
	uint32_t s = 0;
	for (int i = 0; i < 16; i += 2) { s += (uint32_t)(src[i] << 8 | src[i + 1]); s += (uint32_t)(dst[i] << 8 | dst[i + 1]); }
	s += (uint32_t)n; s += nh;
	for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
	if (n) s += (uint32_t)(p[0] << 8);
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}

/* an IPv6 packet from our link-local address; dst MAC from the address (multicast) or given */
static uint8_t tx6[1600];
static void send6(const uint8_t *dmac, const uint8_t *dst, uint8_t nh, int plen)
{
	uint8_t *e = tx6, *ip = tx6 + 14;
	if (dmac) memcpy(e, dmac, 6);
	else { e[0] = 0x33; e[1] = 0x33; memcpy(e + 2, dst + 12, 4); }
	memcpy(e + 6, my_mac, 6); e[12] = 0x86; e[13] = 0xDD;
	ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0; ip[4] = (uint8_t)(plen >> 8); ip[5] = (uint8_t)plen; ip[6] = nh; ip[7] = 255;
	memcpy(ip + 8, my_ll, 16); memcpy(ip + 24, dst, 16);
	uint8_t *p = ip + 40;
	uint16_t c;
	if (nh == 58) { p[2] = p[3] = 0; c = csum6(my_ll, dst, 58, p, plen); p[2] = (uint8_t)(c >> 8); p[3] = (uint8_t)c; }
	if (nh == 6) { p[16] = p[17] = 0; c = csum6(my_ll, dst, 6, p, plen); p[16] = (uint8_t)(c >> 8); p[17] = (uint8_t)c; }
	if (nh == 17) { p[6] = p[7] = 0; c = csum6(my_ll, dst, 17, p, plen); if (!c) c = 0xFFFF; p[6] = (uint8_t)(c >> 8); p[7] = (uint8_t)c; }
	eth_send(tx6, 14 + 40 + plen);
}

static void send_na(const uint8_t *dmac, const uint8_t *dst, int solicited)
{
	uint8_t *p = tx6 + 54;
	memset(p, 0, 32);
	p[0] = 136; p[4] = solicited ? 0x60 : 0x20;          /* NA: solicited + override, or just override */
	memcpy(p + 8, my_ll, 16);
	p[24] = 2; p[25] = 1; memcpy(p + 26, my_mac, 6);     /* target link-layer address */
	send6(dmac, dst, 58, 32);
	n_na_sent++;
}

/* DNS name from a dotted string ("Head Unit._airplay._tcp.local") */
static int dns_put(uint8_t *o, const char *s)
{
	int k = 0;
	while (*s) {
		const char *e = s; while (*e && *e != '.') e++;
		int l = (int)(e - s);
		o[k++] = (uint8_t)l; memcpy(o + k, s, l); k += l;
		s = *e ? e + 1 : e;
	}
	o[k++] = 0;
	return k;
}
static int rr_head(uint8_t *o, const char *name, uint16_t type, uint16_t cls, uint32_t ttl)
{
	int k = dns_put(o, name);
	o[k++] = (uint8_t)(type >> 8); o[k++] = (uint8_t)type; o[k++] = (uint8_t)(cls >> 8); o[k++] = (uint8_t)cls;
	o[k++] = (uint8_t)(ttl >> 24); o[k++] = (uint8_t)(ttl >> 16); o[k++] = (uint8_t)(ttl >> 8); o[k++] = (uint8_t)ttl;
	return k;                                            /* rdlength follows */
}

#define AP_INSTANCE "CarPlay._airplay._tcp.local"
#define AP_HOST     "Computer.local"
#define AP_PORT     7000
static const char *const ap_txt[] = {          /* stock's TXT record, in stock's order */
	"deviceid=00:11:22:AA:BB:CC",                    /* stock's constant placeholder */
	"features=0x4040280,0x21",
	"flags=0x4",
	"model=AirPlayGeneric1,1",
	"pi=5d1f7a2e-3c84-4b6a-9e21-f133c0ffee01",       /* our own pairing identity (stock keeps one per unit) */
	"srcvers=320.14",
};

static void send_airplay_answer(void)
{
	uint8_t *u = tx6 + 54, *d = u + 8;
	int k = 0;
	memset(d, 0, 12);
	d[2] = 0x84; d[7] = 4;                               /* response, authoritative; 4 answers */
	k = 12;
	/* PTR */
	k += rr_head(d + k, "_airplay._tcp.local", 12, 1, 4500);
	{ int at = k; k += 2; int n = dns_put(d + k, AP_INSTANCE); k += n; d[at] = (uint8_t)(n >> 8); d[at + 1] = (uint8_t)n; }
	/* SRV */
	k += rr_head(d + k, AP_INSTANCE, 33, 0x8001, 120);
	{ int at = k; k += 2; d[k++] = 0; d[k++] = 0; d[k++] = 0; d[k++] = 0; d[k++] = AP_PORT >> 8; d[k++] = AP_PORT & 255;
	  int n = 6 + dns_put(d + k, AP_HOST); k += n - 6; d[at] = (uint8_t)(n >> 8); d[at + 1] = (uint8_t)n; }
	/* TXT */
	k += rr_head(d + k, AP_INSTANCE, 16, 0x8001, 4500);
	{ int at = k; k += 2; int n = 0;
	  for (unsigned i = 0; i < sizeof ap_txt / sizeof ap_txt[0]; i++) { int l = 0; while (ap_txt[i][l]) l++; d[k + n++] = (uint8_t)l; memcpy(d + k + n, ap_txt[i], l); n += l; }
	  k += n; d[at] = (uint8_t)(n >> 8); d[at + 1] = (uint8_t)n; }
	/* AAAA */
	k += rr_head(d + k, AP_HOST, 28, 0x8001, 120);
	d[k++] = 0; d[k++] = 16; memcpy(d + k, my_ll, 16); k += 16;
	int ul = 8 + k;
	u[0] = 5353 >> 8; u[1] = 5353 & 255; u[2] = 5353 >> 8; u[3] = 5353 & 255; u[4] = (uint8_t)(ul >> 8); u[5] = (uint8_t)ul;
	send6(0, mdns6, 17, ul);
	n_mdns_ans++;
}

static void ping_phone(void)
{
	uint8_t *p = tx6 + 54;
	memset(p, 0, 16);
	p[0] = 128; p[5] = 0x33; p[7] = (uint8_t)n_pings_sent;   /* echo request, id 0x0033, seq */
	send6(phone_mac, phone_ll, 58, 16);
	n_pings_sent++;
}
static void ask_phone(void)                                 /* mDNS query: _remotepairing._tcp.local PTR */
{
	uint8_t *u = tx6 + 54, *d = u + 8;
	memset(d, 0, 12); d[5] = 1;
	int k = 12 + dns_put(d + 12, "_remotepairing._tcp.local");
	d[k++] = 0; d[k++] = 12; d[k++] = 0; d[k++] = 1;
	int ul = 8 + k;
	u[0] = 5353 >> 8; u[1] = 5353 & 255; u[2] = 5353 >> 8; u[3] = 5353 & 255; u[4] = (uint8_t)(ul >> 8); u[5] = (uint8_t)ul;
	send6(0, mdns6, 17, ul);
}

/* The CarPlay receiver's network side (docs/carplay-pairing.md, docs/carplay-session.md):
   TCP connections on a few fixed ports, RTSP on the control and event channels, the
   screen stream, and NTP-style timing over UDP. Only request lines, plist key names and
   our own status are shown: bodies and headers can carry phone identifiers. */
enum { C_CTRL, C_EVT, C_SCR, NCONN };
#define EVENT_PORT     7001
#define SCREEN_PORT    7002
#define TIMING_PORT    7010
#define KEEPALIVE_PORT 7011
static const uint16_t c_port[NCONN] = { AP_PORT, EVENT_PORT, SCREEN_PORT };

typedef struct {
	int state;                     /* 0 listen, 1 SYN received, 2 established, 3 closed by peer */
	uint8_t mac[6], ip[16];
	uint16_t pport, win;
	uint32_t iss, una, nxt, rcv, rx, conns, fins, rsts, last_tx, retx;
	uint8_t txq[16384]; int txq_n; /* sent or queued, not yet acknowledged; starts at una */
	int enc;                       /* encrypted RTSP framing (control after pair-verify; events) */
	uint8_t kr[32], kw[32]; uint64_t rc, wc;
	uint8_t eb[0x4000 + 64]; int eb_n;
	uint8_t rq[8192]; int rq_n;    /* plaintext request bytes not yet handled */
	uint32_t ef_in, ef_bad, ef_out;
} tconn;
static tconn cn[NCONN];

static char req_lines[2][72]; static int n_req;      /* last two control request lines */
static char req_log[96]; static int req_log_n;       /* short tags of the session requests, in order */
static uint32_t n_enc_rx;
static const char *pair_status = "";
static int ps_last, pv_last;
static uint32_t pair_ms;
static char first_ctype[40];

static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int s_starts(const char *a, const char *b) { while (*b && *a == *b) { a++; b++; } return *b == 0; }
static void mmove(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }   /* d < s only */
static void log_tag(const char *t)
{
	if (req_log_n > (int)sizeof req_log - 12) return;
	if (req_log_n) req_log[req_log_n++] = ' ';
	while (*t && req_log_n < (int)sizeof req_log - 1) req_log[req_log_n++] = *t++;
	req_log[req_log_n] = 0;
}

static void tcp_seg(tconn *c, uint8_t flags, uint32_t seq, const uint8_t *d, int n, int mss)
{
	uint8_t *p = tx6 + 54;
	int hl = mss ? 24 : 20;
	uint16_t lp = c_port[c - cn];
	memset(p, 0, (size_t)hl);
	p[0] = (uint8_t)(lp >> 8); p[1] = (uint8_t)lp; p[2] = (uint8_t)(c->pport >> 8); p[3] = (uint8_t)c->pport;
	uint32_t a = c->rcv;
	p[4] = (uint8_t)(seq >> 24); p[5] = (uint8_t)(seq >> 16); p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
	p[8] = (uint8_t)(a >> 24); p[9] = (uint8_t)(a >> 16); p[10] = (uint8_t)(a >> 8); p[11] = (uint8_t)a;
	p[12] = (uint8_t)(hl / 4 << 4); p[13] = flags; p[14] = (uint8_t)(c->win >> 8); p[15] = (uint8_t)c->win;
	if (mss) { p[20] = 2; p[21] = 4; p[22] = (uint8_t)(1400 >> 8); p[23] = (uint8_t)(1400 & 255); }
	if (n) memcpy(p + hl, d, (size_t)n);
	send6(c->mac, c->ip, 6, hl + n);
}

static void tcp_flush(tconn *c, int from_una)       /* send queued data (all of it again after a timeout) */
{
	uint32_t start = from_una ? c->una : c->nxt;
	int off = (int)(start - c->una);
	while (off < c->txq_n) {
		int n = c->txq_n - off; if (n > 1400) n = 1400;
		tcp_seg(c, off + n == c->txq_n ? 0x18 : 0x10, c->una + (uint32_t)off, c->txq + off, n, 0);
		off += n;
	}
	c->nxt = c->una + (uint32_t)c->txq_n;
	c->last_tx = millis();
}

static void tcp_write(tconn *c, const void *d, int n)
{
	if (c->txq_n + n > (int)sizeof c->txq) return;
	memcpy(c->txq + c->txq_n, d, (size_t)n); c->txq_n += n;
	tcp_flush(c, 0);
}

static void tcp_poll(void)                          /* retransmit after 300 ms without an ACK */
{
	for (int i = 0; i < NCONN; i++) {
		tconn *c = &cn[i];
		if (c->state == 2 && c->txq_n && millis() - c->last_tx > 300) { c->retx++; tcp_flush(c, 1); }
	}
}

/* encrypted RTSP framing: [u16 LE length = AAD][ChaCha20-Poly1305 ciphertext][16-byte tag],
   nonce = 4 zero bytes + 64-bit LE counter per direction */
static void ctr_nonce(uint8_t n[12], uint64_t c) { for (int i = 0; i < 4; i++) n[i] = 0; for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(c >> (8 * i)); }
static void hk(uint8_t out[32], const char *salt, const char *info)   /* HKDF over the session secret */
{
	const uint8_t *sec = pc.transient ? pc.srp.K : pc.pv_shared;
	int sl = pc.transient ? 64 : 32, s = 0, i = 0;
	while (salt[s]) s++;
	while (info[i]) i++;
	crypto_sha512_hkdf(out, 32, sec, (size_t)sl, (const uint8_t *)salt, (size_t)s, (const uint8_t *)info, (size_t)i);
}
static void enc_start(tconn *c, const char *salt, const char *rd_info, const char *wr_info)
{
	hk(c->kr, salt, rd_info); hk(c->kw, salt, wr_info);
	c->rc = c->wc = 0; c->eb_n = 0; c->enc = 1;
}

static void send_maybe_enc(tconn *c, const uint8_t *d, int n)
{
	if (!c->enc) { tcp_write(c, d, n); return; }
	static uint8_t fr[0x4000 + 18];
	while (n > 0) {
		int k = n > 0x4000 ? 0x4000 : n;
		uint8_t nonce[12]; crypto_aead_ctx x;
		fr[0] = (uint8_t)k; fr[1] = (uint8_t)(k >> 8);
		ctr_nonce(nonce, c->wc++);
		crypto_aead_init_ietf(&x, c->kw, nonce);
		crypto_aead_write(&x, fr + 2, fr + 2 + k, fr, 2, d, (size_t)k);
		tcp_write(c, fr, 2 + k + 16);
		c->ef_out++;
		d += k; n -= k;
	}
}

static void respond_t(tconn *c, const char *proto, const char *cseq, const char *ctype, const uint8_t *body, int n)
{
	static char h[256];
	char *q = str_cat(h, proto); q = str_cat(q, " 200 OK\r\nContent-Length: ");
	q = str_u(q, (uint32_t)n);
	if (n) { q = str_cat(q, "\r\nContent-Type: "); q = str_cat(q, ctype); }
	if (cseq[0]) { q = str_cat(q, "\r\nCSeq: "); q = str_cat(q, cseq); }
	q = str_cat(q, "\r\nServer: AirTunes/320.14\r\n\r\n");
	static uint8_t all[12288];
	int hl = (int)(q - h);
	if (hl + n > (int)sizeof all) return;
	memcpy(all, h, (size_t)hl); if (n) memcpy(all + hl, body, (size_t)n);
	send_maybe_enc(c, all, hl + n);
}
#define PLIST "application/x-apple-binary-plist"

static void dev_rng(uint8_t *o, int n);
/* auth-setup (MFi-SAP): prove the MFi chip is present, bound to this session's X25519 keys */
static const char *auth_status = "";
static uint32_t auth_ms;
static int auth_setup(const uint8_t *body, int n, uint8_t *out, int cap)
{
	if (n != 33 || body[0] != 0x01) { auth_status = "auth-setup: unexpected body"; return -1; }
	uint32_t t0 = millis();
	uint8_t esec[32], epub[32], sh[32], m[64], dg[32], kin[7 + 32], key[20], iv[20];
	dev_rng(esec, 32);
	crypto_x25519_public_key(epub, esec);
	crypto_x25519(sh, esec, body + 1);
	crypto_wipe(esec, 32);
	memcpy(m, epub, 32); memcpy(m + 32, body + 1, 32);   /* ours, then the phone's */
	sha256(dg, m, 64);
	if (mfi_sign(dg, 32)) { auth_status = "auth-setup: MFi signing FAILED"; return -1; }
	memcpy(kin, "AES-KEY", 7); memcpy(kin + 7, sh, 32); sha1(key, kin, 39);
	memcpy(kin, "AES-IV", 6);  memcpy(kin + 6, sh, 32); sha1(iv, kin, 38);
	crypto_wipe(sh, 32);
	int need = 32 + 4 + cert_len + 4 + sig_len;
	if (need > cap || !cert_len) { auth_status = "auth-setup: no certificate"; return -1; }
	int o = 0;
	memcpy(out, epub, 32); o = 32;
	out[o++] = (uint8_t)(cert_len >> 24); out[o++] = (uint8_t)(cert_len >> 16); out[o++] = (uint8_t)(cert_len >> 8); out[o++] = (uint8_t)cert_len;
	memcpy(out + o, cert, (size_t)cert_len); o += cert_len;
	out[o++] = 0; out[o++] = 0; out[o++] = (uint8_t)(sig_len >> 8); out[o++] = (uint8_t)sig_len;
	memcpy(out + o, sig, (size_t)sig_len);
	aes128_ctr(key, iv, out + o, sig_len); o += sig_len;
	auth_ms = millis() - t0;
	auth_status = "auth-setup: chip signed, reply sent";
	return o;
}

/* ---- the session (docs/carplay-session.md) ---- */
#define DISPLAY_UUID "f1330000-0000-4000-8000-000000000110"
#define HID_UUID     "2a2a2a2a"
static uint8_t scr_key[32]; static int scr_keyed;
static uint32_t setup110_ms;
static int sess_setup1, n_info, n_record, n_cmd, n_feedback, n_s110, n_other;
static uint32_t n_timing, n_keepalive;
static char phone_feats[64];                         /* how many features the phone offered, and a few names */

/* a 2-finger touch screen: per finger contact id (8), tip (1) + pad (7), X (16), Y (16) */
static const uint8_t hid_desc[] = {
	0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,               /* Digitizer, Touch Screen, Collection (Application) */
#define FINGER \
	0x09, 0x22, 0xA1, 0x02,                           /* Finger, Collection (Logical) */ \
	0x09, 0x51, 0x15, 0x00, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   /* Contact Identifier */ \
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   /* Tip Switch */ \
	0x75, 0x07, 0x95, 0x01, 0x81, 0x03,               /* padding */ \
	0x05, 0x01, 0x09, 0x30, 0x15, 0x00, 0x26, 0x00, 0x05, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,   /* X 0..1280 */ \
	0x09, 0x31, 0x26, 0xD0, 0x02, 0x81, 0x02,         /* Y 0..720 */ \
	0x05, 0x0D, 0xC0
	FINGER, FINGER,
#undef FINGER
	0xC0,
};

static int info_plist(uint8_t *out, int cap)
{
	bp_reset();
	int r = bp_new_dict(24);
	bp_set(r, "sourceVersion", bp_new_str("320.14"));
	bp_set(r, "features", bp_new_int((int64_t)(0x615653AEE2ull & ~0x10004540A00ull)));   /* audio bits cleared */
	bp_set(r, "statusFlags", bp_new_int(4));
	bp_set(r, "model", bp_new_str("Linux"));
	bp_set(r, "manufacturer", bp_new_str(ACC_NAME));
	bp_set(r, "name", bp_new_str(ACC_NAME));
	bp_set(r, "deviceID", bp_new_str("02:f1:33:00:00:02"));
	int bt = bp_new_array(1); bp_push(bt, bp_new_str("02:f1:33:00:00:03")); bp_set(r, "bluetoothIDs", bt);
	bp_set(r, "keepAliveLowPower", bp_new_bool(1));
	bp_set(r, "keepAliveSendStatsAsBody", bp_new_bool(0));
	bp_set(r, "rightHandDrive", bp_new_bool(0));
	bp_set(r, "nightMode", bp_new_bool(0));
	/* modes: the screen (1) and main audio (2) resources, and three app states */
	int modes = bp_new_dict(2), res = bp_new_array(2), apps = bp_new_array(3);
	for (int id = 1; id <= 2; id++) {
		int d = bp_new_dict(6);
		bp_set(d, "resourceID", bp_new_int(id)); bp_set(d, "transferType", bp_new_int(1)); bp_set(d, "transferPriority", bp_new_int(100));
		bp_set(d, "takeConstraint", bp_new_int(100)); bp_set(d, "borrowConstraint", bp_new_int(100)); bp_set(d, "unborrowConstraint", bp_new_int(100));
		bp_push(res, d);
	}
	int a2 = bp_new_dict(2); bp_set(a2, "appStateID", bp_new_int(2)); bp_set(a2, "state", bp_new_bool(0)); bp_push(apps, a2);
	int a1 = bp_new_dict(2); bp_set(a1, "appStateID", bp_new_int(1)); bp_set(a1, "speechMode", bp_new_int(-1)); bp_push(apps, a1);
	int a3 = bp_new_dict(2); bp_set(a3, "appStateID", bp_new_int(3)); bp_set(a3, "state", bp_new_bool(0)); bp_push(apps, a3);
	bp_set(modes, "resources", res); bp_set(modes, "appStates", apps);
	bp_set(r, "modes", modes);
	int ext = bp_new_array(2); bp_push(ext, bp_new_str("vocoderInfo")); bp_push(ext, bp_new_str("enhancedRequestCarUI"));
	bp_set(r, "extendedFeatures", ext);
	/* the display: 1280x720, one full-screen view area */
	int disp = bp_new_array(1), d = bp_new_dict(16);
	bp_set(d, "uuid", bp_new_str(DISPLAY_UUID));
	bp_set(d, "type", bp_new_int(110));
	bp_set(d, "widthPixels", bp_new_int(1280)); bp_set(d, "heightPixels", bp_new_int(720));
	bp_set(d, "widthPixelsMax", bp_new_int(1280)); bp_set(d, "heightPixelsMax", bp_new_int(720));
	bp_set(d, "widthPhysical", bp_new_int(155)); bp_set(d, "heightPhysical", bp_new_int(87));
	bp_set(d, "maxFPS", bp_new_int(30));
	bp_set(d, "features", bp_new_int(0x08));                   /* high-fidelity touch, no knob */
	bp_set(d, "primaryInputDevice", bp_new_int(1));
	int va = bp_new_array(1), v = bp_new_dict(9), sa = bp_new_dict(5);
	bp_set(v, "viewAreaIndex", bp_new_int(0));
	bp_set(v, "widthPixels", bp_new_int(1280)); bp_set(v, "heightPixels", bp_new_int(720));
	bp_set(v, "originXPixels", bp_new_int(0)); bp_set(v, "originYPixels", bp_new_int(0));
	bp_set(v, "viewAreaTransitionControl", bp_new_bool(0));
	bp_set(v, "viewAreaStatusBarEdge", bp_new_int(0));
	bp_set(v, "viewAreaSupportsFocusTransfer", bp_new_bool(0));
	bp_set(sa, "widthPixels", bp_new_int(1280)); bp_set(sa, "heightPixels", bp_new_int(720));
	bp_set(sa, "originXPixels", bp_new_int(0)); bp_set(sa, "originYPixels", bp_new_int(0));
	bp_set(sa, "drawUIOutsideSafeArea", bp_new_bool(1));
	bp_set(v, "safeArea", sa);
	bp_push(va, v);
	bp_set(d, "viewAreas", va);
	bp_set(d, "initialViewArea", bp_new_int(0));
	bp_set(d, "adjacentViewAreas", bp_new_array(0));
	bp_push(disp, d);
	bp_set(r, "displays", disp);
	/* touch */
	int hids = bp_new_array(1), h = bp_new_dict(7);
	bp_set(h, "uuid", bp_new_str(HID_UUID));
	bp_set(h, "name", bp_new_str("Touch Screen"));
	bp_set(h, "hidVendorID", bp_new_int(2)); bp_set(h, "hidProductID", bp_new_int(1)); bp_set(h, "hidCountryCode", bp_new_int(0));
	bp_set(h, "displayUUID", bp_new_str(DISPLAY_UUID));
	bp_set(h, "hidDescriptor", bp_new_data(hid_desc, (int)sizeof hid_desc));
	bp_push(hids, h);
	bp_set(r, "hidDevices", hids);
	return bp_write(r, out, cap);
}

static char *u64dec(char *p, uint64_t v)
{
	char t[24]; int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *p++ = t[--n];
	*p = 0;
	return p;
}

static int setup_reply(const uint8_t *body, int n, uint8_t *out, int cap)
{
	bp_reset();
	int root = bp_parse(body, n);
	if (root < 0) { log_tag("SETUP?"); return -1; }
	int streams = bp_get(root, "streams");
	if (streams < 0) {                                   /* SETUP #1: the session */
		sess_setup1++; log_tag("SETUP1");
		int f = bp_get(root, "features");
		char *q = str_cat(phone_feats, "phone features ");
		q = str_u(q, f >= 0 ? (uint32_t)bp_nodes[f].n : 0);
		int ka = bp_int_of(bp_get(root, "keepAliveLowPower"), 0) != 0;
		int r = bp_new_dict(4), ef = bp_new_array(1);
		bp_set(r, "eventPort", bp_new_int(EVENT_PORT));
		bp_set(r, "timingPort", bp_new_int(TIMING_PORT));
		if (ka) bp_set(r, "keepAlivePort", bp_new_int(KEEPALIVE_PORT));
		bp_push(ef, bp_new_str("viewAreas"));
		bp_set(r, "enabledFeatures", ef);
		return bp_write(r, out, cap);
	}
	int resp = bp_new_dict(1), arr = bp_new_array(4);
	for (int k = 0; k < bp_nodes[streams].n && k < 8; k++) {
		int s = bp_item(streams, k);
		int64_t ty = bp_int_of(bp_get(s, "type"), -1);
		char tag[12] = "S"; u64dec(tag + 1, (uint64_t)(ty < 0 ? 0 : ty)); log_tag(tag);
		if (ty == 110) {                                 /* the main screen */
			n_s110++; setup110_ms = millis();
			char salt[48]; char *q = str_cat(salt, "DataStream-Salt");
			u64dec(q, (uint64_t)bp_int_of(bp_get(s, "streamConnectionID"), 0));   /* unsigned decimal */
			hk(scr_key, salt, "DataStream-Output-Encryption-Key");
			scr_keyed = 1;
			int e = bp_new_dict(2);
			bp_set(e, "type", bp_new_int(110)); bp_set(e, "dataPort", bp_new_int(SCREEN_PORT));
			bp_push(arr, e);
		} else n_other++;                                /* audio, alt screen, iAP channel: not offered */
	}
	bp_set(resp, "streams", arr);
	return bp_write(resp, out, cap);
}

static int hdr_is(const char *l, const char *name)  /* case-insensitive "Name:" prefix */
{
	int i = 0;
	for (; name[i]; i++) { char a = l[i], b = name[i]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) return 0; }
	return l[i] == ':';
}

static void enc_input(tconn *c, const uint8_t *d, int n);

static void handle_requests(tconn *c)
{
	for (;;) {
		int he = -1;                                    /* end of headers */
		for (int i = 0; i + 3 < c->rq_n; i++) if (c->rq[i] == '\r' && c->rq[i + 1] == '\n' && c->rq[i + 2] == '\r' && c->rq[i + 3] == '\n') { he = i + 4; break; }
		if (he < 0) { if (c->rq_n == (int)sizeof c->rq) c->rq_n = 0; return; }
		char line[160], method[16] = "", path[96] = "", proto[16] = "RTSP/1.0", cseq[16] = "";
		int clen = 0, first = 1;
		for (int i = 0; i < he - 2; ) {                 /* header lines */
			int k = 0;
			while (i < he - 2 && !(c->rq[i] == '\r' && c->rq[i + 1] == '\n')) { if (k < 159) line[k++] = (c->rq[i] >= 32 && c->rq[i] < 127) ? (char)c->rq[i] : '.'; i++; }
			line[k] = 0; i += 2;
			if (first) {
				first = 0;
				int a = 0, j = 0;
				while (line[j] && line[j] != ' ' && a < 15) method[a++] = line[j++];
				method[a] = 0; if (line[j]) j++;
				a = 0; while (line[j] && line[j] != ' ' && a < 95) path[a++] = line[j++];
				path[a] = 0; if (line[j]) j++;
				a = 0; while (line[j] && a < 15) proto[a++] = line[j++];
				proto[a] = 0;
				if (c == &cn[C_CTRL]) {
					memcpy(req_lines[0], req_lines[1], sizeof req_lines[0]);
					int m = 0; while (line[m] && m < 71) { req_lines[1][m] = line[m]; m++; }
					req_lines[1][m] = 0;
					n_req++;
				}
			} else if (hdr_is(line, "Content-Length")) {
				const char *v = line + 15; while (*v == ' ') v++; while (*v >= '0' && *v <= '9') clen = clen * 10 + (*v++ - '0');
			} else if (hdr_is(line, "Content-Type") && !first_ctype[0]) {
				const char *v = line + 13; int a = 0; while (*v == ' ') v++; while (*v && a < 39) first_ctype[a++] = *v++;
				first_ctype[a] = 0;
			} else if (hdr_is(line, "CSeq")) {
				const char *v = line + 5; int a = 0; while (*v == ' ') v++; while (*v && a < 15) cseq[a++] = *v++;
				cseq[a] = 0;
			}
		}
		if (clen < 0 || he + clen > (int)sizeof c->rq) { c->rq_n = 0; return; }
		if (c->rq_n < he + clen) return;                /* body not complete yet */
		const uint8_t *body = c->rq + he;
		static uint8_t out[12288];
		int answered = 0;
		if (c != &cn[C_CTRL]) {                         /* event channel: the phone's requests get a bare 200 */
			if (!s_starts(method, "RTSP/")) { respond_t(c, proto, cseq, "", 0, 0); }
			answered = 1;
		} else if (s_eq(path, "/pair-setup")) {
			uint32_t t0 = millis();
			int r = pair_setup(&pc, body, clen, out, sizeof out);
			pair_ms = millis() - t0;
			ps_last = pc.ps_state;
			pair_status = pc.last_err ? "pair-setup FAILED" : pc.ps_state == 6 ? "PAIRED (setup M6 sent)" : pc.ps_state == 4 ? (pc.transient ? "PIN proof OK, transient: PAIRED" : "PIN proof OK (M4 sent)") : "pair-setup M2 sent";
			if (r >= 0) respond_t(c, proto, cseq, "application/pairing+tlv8", out, r);
			answered = 1;
		} else if (s_eq(path, "/pair-verify")) {
			uint32_t t0 = millis();
			int r = pair_verify(&pc, body, clen, out, sizeof out);
			pair_ms = millis() - t0;
			pv_last = pc.pv_state;
			pair_status = pc.last_err ? "pair-verify FAILED" : pc.verified ? "VERIFIED: session keys ready" : "pair-verify M2 sent";
			if (r >= 0) respond_t(c, proto, cseq, "application/pairing+tlv8", out, r);
			answered = 1;
		} else if (s_eq(path, "/auth-setup")) {
			int a = auth_setup(body, clen, out, sizeof out);
			if (a >= 0) respond_t(c, proto, cseq, "application/octet-stream", out, a);
			log_tag("AUTH");
			answered = 1;
		} else if (s_eq(method, "SETUP")) {
			int a = setup_reply(body, clen, out, sizeof out);
			if (a >= 0) respond_t(c, proto, cseq, PLIST, out, a);
			answered = 1;
		} else if (s_eq(method, "GET") && s_eq(path, "/info")) {
			n_info++; log_tag("INFO");
			int a = info_plist(out, sizeof out);
			if (a >= 0) respond_t(c, proto, cseq, PLIST, out, a); else log_tag("INFO-FAIL");
			answered = 1;
		} else {                                        /* RECORD, /command, /feedback, TEARDOWN, ...: 200 */
			if (s_eq(method, "RECORD")) { n_record++; log_tag("REC"); }
			else if (s_starts(path, "/command")) { if (!n_cmd++) log_tag("CMD"); }
			else if (s_starts(path, "/feedback")) { if (!n_feedback++) log_tag("FB"); }
			else if (s_eq(method, "TEARDOWN")) log_tag("TEARDOWN");
			else { n_other++; log_tag(method); }
		}
		if (!answered) respond_t(c, proto, cseq, "", 0, 0);
		int used = he + clen;
		mmove(c->rq, c->rq + used, c->rq_n - used); c->rq_n -= used;
		if (c == &cn[C_CTRL] && pc.verified && !c->enc) {  /* from here on the channel is encrypted */
			enc_start(c, "Control-Salt", "Control-Write-Encryption-Key", "Control-Read-Encryption-Key");
			int left = c->rq_n; c->rq_n = 0;
			if (left) enc_input(c, c->rq, left);
			return;
		}
	}
}

static void enc_input(tconn *c, const uint8_t *d, int n)
{
	if (c == &cn[C_CTRL]) n_enc_rx += (uint32_t)n;
	if (c->eb_n + n > (int)sizeof c->eb) { c->ef_bad++; c->eb_n = 0; return; }
	memcpy(c->eb + c->eb_n, d, (size_t)n); c->eb_n += n;
	for (;;) {
		if (c->eb_n < 2) return;
		int len = c->eb[0] | c->eb[1] << 8;
		if (len > 0x4000) { c->ef_bad++; c->eb_n = 0; return; }
		if (c->eb_n < 2 + len + 16) return;
		uint8_t nonce[12]; crypto_aead_ctx x;
		ctr_nonce(nonce, c->rc);
		crypto_aead_init_ietf(&x, c->kr, nonce);
		if (c->rq_n + len > (int)sizeof c->rq ||
		    crypto_aead_read(&x, c->rq + c->rq_n, c->eb + 2 + len, c->eb, 2, c->eb + 2, (size_t)len)) { c->ef_bad++; c->eb_n = 0; return; }
		c->rc++; c->ef_in++; c->rq_n += len;
		int used = 2 + len + 16;
		mmove(c->eb, c->eb + used, c->eb_n - used); c->eb_n -= used;
		handle_requests(c);
	}
}

/* ---- the screen stream: 128-byte header (u32 LE size, opcode) + body ---- */
static uint8_t sbuf[1 << 20]; static int sb_n;
static uint64_t sc_ctr;
static uint32_t v_cfg, v_frames, v_bad, v_idr, v_bytes, v_first_ms, v_last_size, v_nal_sps;
static int v_profile = -1, v_level, v_w, v_h;

/* SPS width/height (Exp-Golomb); gives up quietly on scaling matrices */
static const uint8_t *bs_p; static int bs_n, bs_bit;
static uint32_t bs_u(int n) { uint32_t v = 0; while (n--) { int byte = bs_bit >> 3; v <<= 1; if (byte < bs_n) v |= (uint32_t)(bs_p[byte] >> (7 - (bs_bit & 7)) & 1); bs_bit++; } return v; }
static uint32_t bs_ue(void) { int z = 0; while (!bs_u(1) && z < 32) z++; return z ? ((1u << z) - 1 + bs_u(z)) : 0; }
static int32_t bs_se(void) { uint32_t k = bs_ue(); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
static void sps_size(const uint8_t *s, int n)
{
	static uint8_t rb[256]; int m = 0;                   /* drop emulation-prevention bytes */
	for (int i = 0; i < n && m < 256; i++) { if (i >= 2 && s[i] == 3 && s[i - 1] == 0 && s[i - 2] == 0) continue; rb[m++] = s[i]; }
	bs_p = rb; bs_n = m; bs_bit = 8;                     /* skip the NAL header */
	int prof = (int)bs_u(8); bs_u(8); bs_u(8); bs_ue();
	if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128) {
		uint32_t cf = bs_ue(); if (cf == 3) bs_u(1);
		bs_ue(); bs_ue(); bs_u(1);
		if (bs_u(1)) return;                             /* scaling matrices: not handled */
	}
	bs_ue();
	uint32_t poc = bs_ue();
	if (poc == 0) bs_ue();
	else if (poc == 1) { bs_u(1); bs_se(); bs_se(); uint32_t k = bs_ue(); for (uint32_t i = 0; i < k && i < 256; i++) bs_se(); }
	bs_ue(); bs_u(1);
	uint32_t wm = bs_ue(), hm = bs_ue(), fmo = bs_u(1);
	if (!fmo) bs_u(1);
	bs_u(1);
	uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
	if (bs_u(1)) { cl = bs_ue(); cr = bs_ue(); ct = bs_ue(); cb = bs_ue(); }
	v_w = (int)((wm + 1) * 16 - 2 * (cl + cr));
	v_h = (int)((2 - fmo) * (hm + 1) * 16 - 2 * (ct + cb) * (2 - fmo));
}

static void screen_msg(uint8_t *h, uint8_t *b, int n)
{
	if (h[4] == 1) {                                     /* config: avcC, in clear */
		v_cfg++;
		const uint8_t *rec = b; int rn = n;
		for (int i = 0; i + 4 <= n; i++) if (b[i] == 'a' && b[i + 1] == 'v' && b[i + 2] == 'c' && b[i + 3] == 'C') { rec = b + i + 4; rn = n - i - 4; break; }
		if (rn >= 8 && rec[0] == 1) {
			v_profile = rec[1]; v_level = rec[3];
			int sl = rec[6] << 8 | rec[7];
			if ((rec[5] & 31) && 8 + sl <= rn) { v_nal_sps++; sps_size(rec + 8, sl); }
		}
		return;
	}
	if (h[4] != 0) return;
	if (n >= 16) {
		uint8_t nonce[12]; crypto_aead_ctx x;
		ctr_nonce(nonce, sc_ctr);
		crypto_aead_init_ietf(&x, scr_key, nonce);
		if (!scr_keyed || crypto_aead_read(&x, b, b + n - 16, h, 128, b, (size_t)(n - 16))) { v_bad++; return; }
		sc_ctr++; n -= 16;
	}
	if (!v_frames) v_first_ms = millis() - setup110_ms;
	v_frames++; v_bytes += (uint32_t)n; v_last_size = (uint32_t)n;
	for (int o = 0; o + 4 < n; ) {                       /* AVCC: 4-byte big-endian NAL lengths */
		uint32_t l = (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3];
		if (l == 0 || o + 4 + (int)l > n) break;
		if ((b[o + 4] & 31) == 5) { v_idr++; break; }
		o += 4 + (int)l;
	}
}

static void screen_input(const uint8_t *d, int n)
{
	if (sb_n + n > (int)sizeof sbuf) { v_bad++; sb_n = 0; return; }
	memcpy(sbuf + sb_n, d, (size_t)n); sb_n += n;
	int off = 0;
	while (sb_n - off >= 128) {
		uint8_t *h = sbuf + off;
		uint32_t sz = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
		if (sz > sizeof sbuf - 128) { v_bad++; sb_n = 0; return; }
		if (sb_n - off < 128 + (int)sz) break;
		screen_msg(h, h + 128, (int)sz);
		off += 128 + (int)sz;
	}
	if (off) { mmove(sbuf, sbuf + off, sb_n - off); sb_n -= off; }
}

static void on_tcp(const uint8_t *f, const uint8_t *ip, const uint8_t *p, int plen)
{
	uint16_t sp = (uint16_t)(p[0] << 8 | p[1]), dp = (uint16_t)(p[2] << 8 | p[3]);
	int ci = -1;
	for (int i = 0; i < NCONN; i++) if (c_port[i] == dp) ci = i;
	if (ci < 0) return;
	tconn *c = &cn[ci];
	uint8_t fl = p[13];
	uint32_t seq = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
	uint32_t ack = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
	int hl = (p[12] >> 4) * 4;
	if (hl < 20 || hl > plen) return;
	if (fl & 0x04) { if (sp == c->pport) { c->rsts++; c->state = 0; } return; }
	if (fl & 0x02) {                                    /* SYN: (re)start this connection */
		if (c->state == 0 || sp != c->pport || c->state == 3) {
			memcpy(c->mac, f + 6, 6); memcpy(c->ip, ip + 8, 16); c->pport = sp;
			c->rcv = seq + 1; c->state = 1; c->conns++;
			c->iss = 0x0F133000u + (uint32_t)ci * 0x01000000u + c->conns * 0x10000u;
			c->una = c->nxt = c->iss + 1; c->txq_n = 0; c->rq_n = 0; c->eb_n = 0; c->enc = 0;
			c->win = ci == C_SCR ? 0xFFFF : 0x2000;
			if (ci == C_CTRL) pc.verified = 0;          /* a new control connection starts unencrypted */
			if (ci == C_EVT) enc_start(c, "Events-Salt", "Events-Read-Encryption-Key", "Events-Write-Encryption-Key");   /* not swapped */
			if (ci == C_SCR) { sb_n = 0; sc_ctr = 0; }
		}
		tcp_seg(c, 0x12, c->iss, 0, 0, 1);              /* SYN+ACK */
		return;
	}
	if (sp != c->pport || c->state == 0) return;
	if (fl & 0x10) {
		if (c->state == 1) c->state = 2;
		uint32_t acked = ack - c->una;
		if (acked && acked <= (uint32_t)c->txq_n) {
			mmove(c->txq, c->txq + acked, c->txq_n - (int)acked); c->txq_n -= (int)acked; c->una = ack;
			c->last_tx = millis();
		}
	}
	int dl = plen - hl;
	if (dl > 0) {
		if (seq == c->rcv) {
			c->rcv += (uint32_t)dl; c->rx += (uint32_t)dl;
			tcp_seg(c, 0x10, c->nxt, 0, 0, 0);          /* ACK before any slow work */
			if (ci == C_SCR) screen_input(p + hl, dl);
			else if (c->enc) enc_input(c, p + hl, dl);
			else {
				if (c->rq_n + dl <= (int)sizeof c->rq) { memcpy(c->rq + c->rq_n, p + hl, (size_t)dl); c->rq_n += dl; }
				handle_requests(c);
			}
		} else tcp_seg(c, 0x10, c->nxt, 0, 0, 0);       /* re-ACK out-of-order or duplicate data */
	}
	if ((fl & 0x01) && seq + (uint32_t)(dl > 0 ? dl : 0) == c->rcv) {   /* FIN */
		c->rcv++; c->fins++; c->state = 3;
		tcp_seg(c, 0x11, c->nxt, 0, 0, 0); c->nxt++;
	}
}

/* ---- timing (NTP-style, UDP): answer each 210 with a 211 ---- */
static void udp_send(const uint8_t *dmac, const uint8_t *dst, uint16_t sport, uint16_t dport, const uint8_t *d, int n)
{
	uint8_t *u = tx6 + 54;
	int ul = 8 + n;
	u[0] = (uint8_t)(sport >> 8); u[1] = (uint8_t)sport; u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
	u[4] = (uint8_t)(ul >> 8); u[5] = (uint8_t)ul;
	memcpy(u + 8, d, (size_t)n);
	send6(dmac, dst, 17, ul);
}
static uint64_t ntp_now(void)                       /* the phone's clock from 4E0B if we have it, else uptime */
{
	static uint32_t base_ms; static uint64_t base_s; static int based;
	uint32_t ms = millis();
	if (have_time && !based) { base_ms = ms; base_s = t_secs + 2208988800ull; based = 1; }
	uint64_t s = based ? base_s + (ms - base_ms) / 1000 : 2208988800ull + ms / 1000;
	uint32_t frac_ms = based ? (ms - base_ms) % 1000 : ms % 1000;
	return s << 32 | (uint64_t)(((uint64_t)frac_ms << 32) / 1000);
}
static void on_udp_session(const uint8_t *f, const uint8_t *ip, const uint8_t *u, int ul)
{
	uint16_t sp = (uint16_t)(u[0] << 8 | u[1]), dp = (uint16_t)(u[2] << 8 | u[3]);
	const uint8_t *d = u + 8; int n = ul - 8;
	if (dp == KEEPALIVE_PORT) { n_keepalive++; return; }
	if (dp != TIMING_PORT || n < 32 || (d[1] & 0x7F) != (210 & 0x7F)) return;
	n_timing++;
	uint8_t r[32] = { 0 };
	uint64_t now = ntp_now();
	r[0] = 0x80; r[1] = 0xD3; r[2] = 0; r[3] = 7;
	memcpy(r + 8, d + 24, 8);
	for (int i = 0; i < 8; i++) { r[16 + i] = (uint8_t)(now >> (56 - 8 * i)); r[24 + i] = r[16 + i]; }
	udp_send(f + 6, ip + 8, TIMING_PORT, sp, r, 32);
}

/* randomness: cycle-counter jitter of every received frame, hashed */
static uint64_t ent[16]; static uint32_t ent_i, rng_ctr;
static void ent_mix(void) { uint32_t i = ent_i++; ent[i & 15] ^= cycles() * 0x9E3779B97F4A7C15ull + i; }
static void dev_rng(uint8_t *o, int n)
{
	while (n > 0) {
		uint8_t h[64];
		ent_mix(); rng_ctr++;
		crypto_sha512_ctx c;
		crypto_sha512_init(&c);
		crypto_sha512_update(&c, (const uint8_t *)ent, sizeof ent);
		crypto_sha512_update(&c, (const uint8_t *)&rng_ctr, sizeof rng_ctr);
		crypto_sha512_final(&c, h);
		int k = n > 32 ? 32 : n;
		memcpy(o, h, (size_t)k); o += k; n -= k;
		ent[rng_ctr & 15] ^= *(uint64_t *)(h + 32);    /* forward the pool */
	}
}

/* known-answer self-test: the same inputs give these bytes on the Mac (test/kat_host.c) */
static int kat_ok = -1; static uint32_t kat_srp_ms, prep_ms;
static void crypto_selftest(void)
{
	static const uint8_t want_pk[8] = { 0xe4,0x03,0x09,0x98,0xcf,0xd5,0xad,0x17 };
	static const uint8_t want_hb[8] = { 0x45,0x04,0x24,0x22,0x76,0xf5,0x6a,0xd8 };
	static pair_ctx k; static srp_server s;
	uint8_t seed[32], salt[16], b[32], h[64];
	for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
	pair_init(&k, seed, "x", "3939", dev_rng);
	for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
	for (int i = 0; i < 32; i++) b[i] = (uint8_t)(i + 1);
	uint32_t t0 = millis();
	srp_start(&s, "Pair-Setup", "3939", salt, b);
	kat_srp_ms = millis() - t0;
	crypto_sha512(h, s.Bbytes, BN_BYTES);
	kat_ok = !memcmp(k.ltpk, want_pk, 8) && !memcmp(h, want_hb, 8);
}

#define PAIR_ID "5d1f7a2e-3c84-4b6a-9e21-f133c0ffee01"   /* = the pi we advertise */
static void pairing_init(void)                      /* long-term key from this chip's unique ID */
{
	uint8_t sid[16], seed[64];
	for (int i = 0; i < 4; i++) { uint32_t w = rd32(0x03006200 + 4 * (uint32_t)i); for (int j = 0; j < 4; j++) sid[4 * i + j] = (uint8_t)(w >> (8 * j)); }
	crypto_sha512_ctx c;
	crypto_sha512_init(&c);
	crypto_sha512_update(&c, (const uint8_t *)"headunit-toolkit carplay ltsk v1", 32);
	crypto_sha512_update(&c, sid, 16);
	crypto_sha512_final(&c, seed);
	pair_init(&pc, seed, PAIR_ID, "3939", dev_rng);
	crypto_wipe(seed, sizeof seed); crypto_wipe(sid, sizeof sid);
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) { pc_hex[2 * i] = hx[pc.ltpk[i] >> 4]; pc_hex[2 * i + 1] = hx[pc.ltpk[i] & 15]; }
	pc_hex[64] = 0;
	crypto_selftest();
	uint32_t t0 = millis();
	pair_prepare(&pc);
	prep_ms = millis() - t0;
}

static void on_ip6(const uint8_t *f, int l)
{
	const uint8_t *ip = f + 14, *p = ip + 40;
	int plen = ip[4] << 8 | ip[5];
	if (l < 54 || 54 + plen > l) return;
	if (ip[8] == 0xFE && ip[9] == 0x80 && memcmp(ip + 8, my_ll, 16)) { memcpy(phone_ll, ip + 8, 16); memcpy(phone_mac, f + 6, 6); have_phone_ll = 1; }
	if (ip[6] == 58 && plen >= 8) {                      /* ICMPv6 */
		if (p[0] == 129 && !memcmp(ip + 24, my_ll, 16)) n_pong++;   /* the phone answered our ping */
		if (p[0] == 135 && plen >= 24 && !memcmp(p + 8, my_ll, 16)) {  /* neighbour solicitation for us */
			n_ns++;
			int dad = 1; for (int i = 0; i < 16; i++) if (ip[8 + i]) dad = 0;
			if (dad) send_na(0, all6, 0); else send_na(f + 6, ip + 8, 1);
		} else if (p[0] == 128 && !memcmp(ip + 24, my_ll, 16) && plen <= 1400) {   /* echo request */
			n_ping6++;
			memcpy(tx6 + 54, p, plen); tx6[54] = 129;
			send6(f + 6, ip + 8, 58, plen);
		}
	} else if (ip[6] == 6 && plen >= 20 && !memcmp(ip + 24, my_ll, 16)) {   /* TCP to us */
		uint16_t dp = (uint16_t)(p[2] << 8 | p[3]);
		if (p[13] & 0x02) {
			n_syn++;
			if (dp != syn_ports[0]) { for (int j = 3; j > 0; j--) syn_ports[j] = syn_ports[j - 1]; syn_ports[0] = dp; }
		}
		on_tcp(f, ip, p, plen);
	}
}

static void on_frame(const uint8_t *f, int l, uint16_t ty)
{
	ent_mix();
	if (ty == 0x86DD) on_ip6(f, l);
	const uint8_t *u = 0; int ul = 0;
	if (ty == 0x86DD && l >= 14 + 40 + 8 && f[20] == 17) { u = f + 54; ul = (f[18] << 8 | f[19]); }
	else if (ty == 0x0800 && l >= 14 + 20 + 8 && f[23] == 17) {
		int ih = (f[14] & 15) * 4;
		u = f + 14 + ih; ul = (f[16] << 8 | f[17]) - ih;
		if (f[26] || f[27] || f[28] || f[29]) for (int i = 0; i < 4; i++) phone_ip4[i] = f[26 + i];
	}
	if (!u || ul < 8 || u + ul > f + l) return;
	int dp = u[2] << 8 | u[3];
	if (dp == 5353) on_mdns(u + 8, ul - 8);
	if (ty == 0x86DD && (dp == TIMING_PORT || dp == KEEPALIVE_PORT)) on_udp_session(f, f + 14, u, ul);
	if (dp == 67) n_dhcp_req++;
}

static void device_poll(void)
{
	musb_poll();
	tcp_poll();
	if (!pc.srp_ready && pc.ps_state != 2 && cn[C_CTRL].txq_n == 0 && millis() - cn[C_CTRL].last_tx > 2000) pair_prepare(&pc);
	if (notify_pending) {
		notify_pending = 0;
		uint8_t sp[16] = { 0xA1, 0x2A, 0, 0, 1, 0, 8, 0 };
		put32(sp + 8, 480000000u); put32(sp + 12, 480000000u);
		uint8_t nc[8] = { 0xA1, 0x00, 1, 0, 1, 0, 0, 0 };
		musb_send(4, sp, 16, 64);
		musb_send(4, nc, 8, 64);
	}
#ifndef MACTEST
	if (g_config) iap2_poll();
#endif
	static int was_up; static uint32_t up_at, last_ans;
	if (ncm_on && !was_up) { up_at = millis(); }
	was_up = ncm_on;
	static int announced;
	if (ncm_on && !announced && millis() - up_at > 500) { announced = 1; send_na(0, all6, 0); }   /* unsolicited NA */
	static uint32_t last_probe;
	if (ncm_on && have_phone_ll && millis() - up_at > 3000 && millis() - last_probe > 2000 && n_pings_sent < 10) {
		last_probe = millis(); ping_phone(); ask_phone();
	}
	static int n_announce;
	if (ADVERTISE && ncm_on && n_announce < 3 && millis() - up_at > 1000u + 1000u * (uint32_t)n_announce) { n_announce++; send_airplay_answer(); }
	if (ADVERTISE && ncm_on && want_airplay && millis() - last_ans > 1000) { want_airplay = 0; last_ans = millis(); send_airplay_answer(); }
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
						const uint8_t *f = ntb + idx;
						int up = -1;
						if (ty == 0x86DD && l >= 14 + 40 + 4 && f[20] == 17) up = f[56] << 8 | f[57];                 /* IPv6 UDP dst port */
						else if (ty == 0x0800 && l >= 14 + 20 + 4 && f[23] == 17) { int ih = (f[14] & 15) * 4; up = f[14 + ih + 2] << 8 | f[14 + ih + 3]; }
						if (ty == 0x86DD && l >= 14 + 40 && f[20] == 58) n_icmp6++;
						if (up >= 0 && up != ports[0]) { for (int j = 3; j > 0; j--) ports[j] = ports[j - 1]; ports[0] = (uint16_t)up; }
						if (up == 5353) n_mdns++;
						on_frame(f, l, ty);
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
	p = str_cat(s, "MFi v"); p = hexn(p, mfi_ver, 2); p = str_cat(p, " proto "); p = str_u(p, mfi_proto);
	p = str_cat(p, "  len "); p = str_u(p, (uint32_t)mfi_len_read); p = str_cat(p, "  cert "); p = str_u(p, (uint32_t)cert_len);
	if (mfi_stage) { p = str_cat(p, "  FAILED at "); p = hexn(p, (uint32_t)mfi_stage, 2); }
	p = str_cat(p, "  i2c errs "); p = str_u(p, (uint32_t)mfi_err); *p++ = '/'; p = str_u(p, (uint32_t)mfi_last);
	p = str_cat(p, "  sent "); p = str_u(p, (uint32_t)n_sent_msgs);
	if (sig_status >= 0) { p = str_cat(p, "  sig "); p = str_u(p, (uint32_t)sig_len); p = str_cat(p, "B/"); p = str_u(p, (uint32_t)sig_ms); p = str_cat(p, "ms"); }
	p = str_cat(p, auth_result == 1 ? "  AUTH OK" : auth_result == -1 ? "  AUTH FAILED" : auth_result == -2 ? "  CHIP ERROR" : "");
	if (ident_result || ident_len) {
		char s2[128], *q = str_cat(s2, "ident: sent "); q = str_u(q, (uint32_t)ident_len); q = str_cat(q, " bytes  ");
		q = str_cat(q, ident_result == 1 ? "ACCEPTED" : ident_result == -1 ? "REJECTED:" : "waiting");
		for (int i = 0; i < n_rej; i++) { *q++ = ' '; q = hexn(q, rej_ids[i], 4); }
		if (have_time) {
			q = str_cat(q, "   phone time "); q = fmt_time(q, t_secs + (uint64_t)((int64_t)(t_tz + t_dst) * 60));
			q = str_cat(q, " (UTC"); *q++ = t_tz + t_dst < 0 ? '-' : '+'; q = str_u(q, (uint32_t)((t_tz + t_dst) < 0 ? -(t_tz + t_dst) : (t_tz + t_dst)) / 60); q = str_cat(q, ")");
		}
		gfx_text(30, 472, s2, &font16x32, 1, ident_result == 1 ? 0x40ff60 : ident_result < 0 ? 0xff6060 : 0xffffff);
	}
	gfx_text(30, 506, s, &font16x32, 1, auth_result == 1 || (cert_len && !auth_result) ? 0x40ff60 : (auth_result < 0 || mfi_stage) ? 0xff6060 : 0xffffff);
	if (have_dev) {
		p = str_cat(s, "phone "); p = hexn(p, devd[8] | devd[9] << 8, 4); *p++ = ':'; p = hexn(p, devd[10] | devd[11] << 8, 4);
		p = str_cat(p, "  \""); p = str_cat(p, manu); p = str_cat(p, "\" \""); p = str_cat(p, prod); p = str_cat(p, "\"");
		gfx_text(30, 290, s, &font16x32, 1, 0xffffff);
	}
	if (req_log[0]) {                                    /* the session, once it starts (replaces the switch line) */
		p = str_cat(s, "session: "); p = str_cat(p, req_log);
		gfx_text(30, 330, s, &font16x32, 1, 0xffff60);
	} else if (sw_line[0]) gfx_text(30, 330, sw_line, &font16x32, 1, 0xffffff);
	if (phase == 1) {
		static const char *const lsn[] = { "idle", "detect sent", "detect answered", "SYN sent", "LINKED" };
		p = str_cat(s, "iAP2: "); p = str_cat(p, lsn[ls]);
		p = str_cat(p, "  detect "); p = str_cat(p, got_detect ? "yes" : "no");
		p = str_cat(p, "  SYN x"); p = str_u(p, (uint32_t)syn_tries);
		p = str_cat(p, "  in "); p = str_u(p, n_iap_in);
		p = str_cat(p, "  data "); p = str_u(p, (uint32_t)n_data);
		p = str_cat(p, "  rst "); p = str_u(p, (uint32_t)n_rst); p = str_cat(p, "  bad "); p = str_u(p, (uint32_t)n_badsum);
		gfx_text(30, 370, s, &font16x32, 1, ls == 4 ? 0x40ff60 : 0xffffff);
		if (n_s110 || cn[C_EVT].conns) {                 /* video and session channels (replaces the phone SYN line) */
			p = str_cat(s, "VIDEO: cfg "); p = str_u(p, v_cfg);
			if (v_profile >= 0) { p = str_cat(p, " prof "); p = str_u(p, (uint32_t)v_profile); p = str_cat(p, " L"); p = str_u(p, (uint32_t)v_level); }
			if (v_w) { *p++ = ' '; p = str_u(p, (uint32_t)v_w); *p++ = 'x'; p = str_u(p, (uint32_t)v_h); *p = 0; }
			p = str_cat(p, "  frames "); p = str_u(p, v_frames); p = str_cat(p, " IDR "); p = str_u(p, v_idr);
			p = str_cat(p, " "); p = str_u(p, v_bytes / 1024); p = str_cat(p, "KB bad "); p = str_u(p, v_bad);
			if (v_frames) { p = str_cat(p, " first "); p = str_u(p, v_first_ms); p = str_cat(p, "ms"); }
			gfx_text(30, 404, s, &font16x32, 1, v_frames ? 0x40ff60 : v_bad ? 0xff6060 : 0xffffff);
		} else if (their_syn_n) {
			p = str_cat(s, "phone SYN "); p = hexn(p, their_ctl, 2); p = str_cat(p, ": ");
			for (int i = 0; i < their_syn_n && i < 16; i++) { p = hexn(p, their_syn[i], 2); *p++ = ' '; *p = 0; }
			gfx_text(30, 404, s, &font16x32, 1, 0xa0ffa0);
		}
		if (n_msgs) {
			p = str_cat(s, "messages "); p = str_u(p, (uint32_t)n_msgs); p = str_cat(p, ":");
			for (int i = 0; i < 6 && i < n_msgs; i++) { *p++ = ' '; p = hexn(p, msg_ids[i], 4); *p++ = '/'; p = str_u(p, msg_len[i]); }
			gfx_text(30, 438, s, &font16x32, 1, 0x40ff60);
		}
		p = str_cat(s, "NCM: "); p = str_cat(p, ncm_on ? "up" : "down"); p = str_cat(p, "  frames "); p = str_u(p, n_frames);
		p = str_cat(p, "  bad "); p = str_u(p, n_ntb_bad); p = str_cat(p, "  types ");
		for (int i = 0; i < 4; i++) { p = hexn(p, types[i], 4); *p++ = ' '; *p = 0; }
		p = str_cat(p, " udp"); for (int i = 0; i < 4; i++) { *p++ = ' '; p = str_u(p, ports[i]); }
		p = str_cat(p, "  mdns "); p = str_u(p, n_mdns); p = str_cat(p, " icmp6 "); p = str_u(p, n_icmp6);
		gfx_text(30, 544, s, &font16x32, 1, n_frames ? 0x40ff60 : 0xffffff);
		p = str_cat(s, "us fe80::f1:33ff:fe00:2  NS "); p = str_u(p, n_ns); p = str_cat(p, " NA "); p = str_u(p, n_na_sent);
		p = str_cat(p, "  ping6 "); p = str_u(p, n_ping6); p = str_cat(p, "  mdns ans "); p = str_u(p, n_mdns_ans);
		p = str_cat(p, "  SYN "); p = str_u(p, n_syn);
		p = str_cat(p, "  | ping phone "); p = str_u(p, n_pings_sent); p = str_cat(p, " -> pong "); p = str_u(p, n_pong);
		p = str_cat(p, "  mdns resp "); p = str_u(p, n_mdns_resp);
		for (int i = 0; i < 4 && syn_ports[i]; i++) { *p++ = ' '; p = str_u(p, syn_ports[i]); }
		gfx_text(30, 616, s, &font8x16, 1, n_syn ? 0x40ff60 : 0xffffff);
		{
			static const char *const st[] = { "listen", "SYN", "up", "closed" };
			tconn *k = &cn[C_CTRL], *ev = &cn[C_EVT], *sc = &cn[C_SCR];
			p = str_cat(s, "ctrl "); p = str_cat(p, st[k->state]); p = str_cat(p, " x"); p = str_u(p, k->conns);
			p = str_cat(p, " rx "); p = str_u(p, k->rx); p = str_cat(p, " retx "); p = str_u(p, k->retx);
			p = str_cat(p, " | evt "); p = str_cat(p, st[ev->state]); p = str_cat(p, " in "); p = str_u(p, ev->ef_in); p = str_cat(p, " bad "); p = str_u(p, ev->ef_bad);
			p = str_cat(p, " | scr "); p = str_cat(p, st[sc->state]); p = str_cat(p, " "); p = str_u(p, sc->rx / 1024); p = str_cat(p, "KB");
			p = str_cat(p, " | ntp "); p = str_u(p, n_timing);
			gfx_text(30, 88, s, &font16x32, 1, k->state >= 2 ? 0x40ff60 : 0xffffff);
			p = str_cat(s, req_lines[0]); if (req_lines[0][0]) p = str_cat(p, "  |  "); p = str_cat(p, req_lines[1]);
			gfx_text(30, 124, s, &font16x32, 1, 0xffff60);
			if (!pair_status[0] && kat_ok >= 0) {
				p = str_cat(s, kat_ok ? "crypto self-test OK" : "crypto self-test FAILED");
				p = str_cat(p, "  (SRP B in "); p = str_u(p, kat_srp_ms); p = str_cat(p, " ms, prepared in "); p = str_u(p, prep_ms); p = str_cat(p, " ms)");
				gfx_text(30, 160, s, &font16x32, 1, kat_ok ? 0x40ff60 : 0xff6060);
			}
			if (pair_status[0]) {
				p = str_cat(s, pair_status);
				if (pc.last_err) { p = str_cat(p, " at "); p = str_u(p, (uint32_t)pc.last_err); }
				p = str_cat(p, "  ("); p = str_u(p, pair_ms); p = str_cat(p, " ms)");
				if (n_enc_rx) {
					p = str_cat(p, "  enc frames in "); p = str_u(p, cn[C_CTRL].ef_in); p = str_cat(p, " bad "); p = str_u(p, cn[C_CTRL].ef_bad);
					p = str_cat(p, " out "); p = str_u(p, cn[C_CTRL].ef_out);
				}
				gfx_text(30, 160, s, &font16x32, 1, pc.last_err || cn[C_CTRL].ef_bad ? 0xff6060 : 0x40ff60);
			}
			if (auth_status[0]) {
				p = str_cat(s, auth_status); p = str_cat(p, "  ("); p = str_u(p, auth_ms); p = str_cat(p, " ms)");
				if (phone_feats[0]) { p = str_cat(p, "   "); p = str_cat(p, phone_feats); }
				if (n_info) { p = str_cat(p, "   info x"); p = str_u(p, (uint32_t)n_info); }
				if (n_cmd) { p = str_cat(p, "  cmd x"); p = str_u(p, (uint32_t)n_cmd); }
				if (n_feedback) { p = str_cat(p, "  feedback x"); p = str_u(p, (uint32_t)n_feedback); }
				gfx_text(30, 196, s, &font8x16, 1, 0x40ff60);
			}
		}
		p = str_cat(s, "phone IPv4 "); for (int i = 0; i < 4; i++) { p = str_u(p, phone_ip4[i]); if (i < 3) *p++ = '.'; }
		p = str_cat(p, "  DHCP requests "); p = str_u(p, n_dhcp_req);
		p = str_cat(p, "   CarPlay avail x"); p = str_u(p, (uint32_t)n_avail);
		if (avail_wired >= 0) { p = str_cat(p, " wired="); p = str_u(p, (uint32_t)avail_wired); }
		p = str_cat(p, " -> start x"); p = str_u(p, (uint32_t)n_start);
		gfx_text(30, 580, s, &font16x32, 1, 0xa0c0ff);
		for (int i = 0; i < n_seen; i++)                 /* Q = phone asks, A = phone announces */
			gfx_text(30 + (i / 4) * 420, 638 + (i % 4) * 20, seen[i], &font8x16, 1, seen[i][0] == 'Q' ? 0xffc040 : 0x40ff60);
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
	dcache_enable();                                     /* ~10x for the pairing maths; USB is PIO, the screen flushes */
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
	for (variant = 2; ; variant = 2) {
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

	mfi_init();                                          /* certificate ready before the phone asks */
	pairing_init();
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
