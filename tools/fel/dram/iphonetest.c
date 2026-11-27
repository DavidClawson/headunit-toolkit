/*
 * iphonetest.c: USB host mode on USB0, read-only. Switches the port from the boot
 * ROM's FEL device to host, waits for a device (the iPhone, plugged in after the
 * Mac's cable is pulled), resets it, gives it address 1 and reads its device,
 * configuration and product/manufacturer string descriptors. Nothing else is sent:
 * no SET_CONFIGURATION, no vendor requests, no role switch. The serial-number string
 * is deliberately not read. Results stay on screen until a power cycle (FEL is gone).
 * Re-plugging repeats the read. A fault paints the screen red.
 *
 * Run with: tools/fel/felrun.py dram/iphonetest.bin --detach
 */
#include "lib.h"
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
#define MAXIF 24
static struct { uint8_t cfg, ifn, alt, cls, sub, proto, neps; } ifs[MAXIF];
static int nifs, ncfgs, cfg_err;
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

static void enumerate(void)
{
	have_dev = 0; nifs = 0; ncfgs = 0; cfg_err = 0; prod[0] = manu[0] = 0;
	/* bus reset (50 ms); the controller does the high-speed chirp */
	wr8(POWER, rd8(POWER) | 0x20 | 0x08);
	udelay(50000);
	wr8(POWER, rd8(POWER) & ~0x08);
	udelay(20000);
	uint8_t dc = rd8(DEVCTL), pw = rd8(POWER);
	speed = (pw & 0x10) ? 3 : (dc & 0x20) ? 1 : 2;
	uint8_t b[512];
	int e = get_desc(0, 1, 0, b, 8, 8);                  /* first 8 bytes: max packet size */
	if (e < 8) { str_cat(str_u(str_cat(st_line, "device descriptor failed: "), (uint32_t)(-e)), ""); n_xfer_err++; return; }
	int mp0 = b[7] ? b[7] : 8;
	uint8_t sa[8] = { 0x00, 5, 1, 0, 0, 0, 0, 0 };      /* SET_ADDRESS 1 */
	if ((e = control_nodata(0, sa))) { str_cat(str_u(str_cat(st_line, "SET_ADDRESS failed: "), (uint32_t)(-e)), ""); n_xfer_err++; return; }
	udelay(10000);
	if (get_desc(1, 1, 0, devd, 18, mp0) != 18) { str_cat(st_line, "device descriptor (18) failed"); n_xfer_err++; return; }
	have_dev = 1;
	if (devd[14] && (e = get_desc(1, 3, devd[14], b, 255, mp0)) > 2) ascii(b, e, manu, sizeof manu);
	if (devd[15] && (e = get_desc(1, 3, devd[15], b, 255, mp0)) > 2) ascii(b, e, prod, sizeof prod);
	ncfgs = devd[17];
	for (int c = 0; c < ncfgs && c < 8; c++) {
		if (get_desc(1, 2, c, b, 9, mp0) != 9) { cfg_err++; continue; }
		int tl = b[2] | b[3] << 8;
		if (tl > (int)sizeof b) tl = sizeof b;
		int n = get_desc(1, 2, c, b, tl, mp0);
		if (n < 9) { cfg_err++; continue; }
		for (int i = 0; i + 2 <= n && b[i] >= 2; i += b[i])
			if (b[i + 1] == 4 && b[i] >= 9 && nifs < MAXIF) {
				ifs[nifs].cfg = b[5]; ifs[nifs].ifn = b[i + 2]; ifs[nifs].alt = b[i + 3];
				ifs[nifs].neps = b[i + 4]; ifs[nifs].cls = b[i + 5]; ifs[nifs].sub = b[i + 6]; ifs[nifs].proto = b[i + 7];
				nifs++;
			}
	}
	str_cat(st_line, "read OK (nothing else sent)");
}

/* ---- screen ---- */
static void status(void)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x101820);
	char s[128], *p;
	gfx_text(30, 16, "iPhone USB test (read only)", &font16x32, 2, 0xffffff);
	if (!connected) {
		gfx_text(30, 110, "1. Unplug the Mac's USB cable from the head unit", &font16x32, 1, 0xffc040);
		gfx_text(30, 150, "2. Plug in your iPhone (unlocked is best)", &font16x32, 1, 0xffc040);
	}
	p = str_cat(s, (rd8(DEVCTL) & 0x04) ? "HOST " : "NOT HOST ");
	p = str_cat(p, "devctl "); p = hexn(p, rd8(DEVCTL), 2); *p++ = '/'; p = hexn(p, devctl0, 2);
	p = str_cat(p, " iscr "); p = hexn(p, rd32(ISCR), 8);
	p = str_cat(p, (rd32(0x020000A0) >> 18) & 1 ? " 5V on" : " 5V off"); p = str_cat(p, " power "); p = hexn(p, rd8(POWER), 2);
	p = str_cat(p, "  connects "); p = str_u(p, n_conn); p = str_cat(p, "  errors "); p = str_u(p, n_xfer_err);
	p = str_cat(p, "  csr "); p = hexn(p, last_csr, 2);
	gfx_text(30, 200, s, &font16x32, 1, 0xa0c0ff);
	p = str_cat(s, connected ? (speed == 3 ? "connected, high speed: " : speed == 2 ? "connected, full speed: " : "connected, low speed: ") : "no device: ");
	p = str_cat(p, st_line);
	gfx_text(30, 240, s, &font16x32, 1, have_dev ? 0x40ff60 : 0xffffff);
	if (have_dev) {
		p = str_cat(s, "ID "); p = hexn(p, devd[8] | devd[9] << 8, 4); *p++ = ':'; p = hexn(p, devd[10] | devd[11] << 8, 4);
		p = str_cat(p, "  USB "); p = hexn(p, devd[2] | devd[3] << 8, 4); p = str_cat(p, "  configs "); p = str_u(p, ncfgs);
		if (cfg_err) { p = str_cat(p, " ("); p = str_u(p, cfg_err); p = str_cat(p, " unreadable)"); }
		gfx_text(30, 290, s, &font16x32, 1, 0xffffff);
		p = str_cat(s, "\""); p = str_cat(p, manu); p = str_cat(p, "\"  \""); p = str_cat(p, prod); p = str_cat(p, "\"");
		gfx_text(30, 330, s, &font16x32, 1, 0xffffff);
		/* interfaces: c<cfg> i<if>.<alt> class/sub/proto eps, two columns */
		for (int i = 0; i < nifs; i++) {
			p = str_cat(s, "c"); p = str_u(p, ifs[i].cfg); p = str_cat(p, " if"); p = str_u(p, ifs[i].ifn);
			*p++ = '.'; p = str_u(p, ifs[i].alt); *p++ = ' ';
			p = hexn(p, ifs[i].cls, 2); *p++ = '/'; p = hexn(p, ifs[i].sub, 2); *p++ = '/'; p = hexn(p, ifs[i].proto, 2);
			p = str_cat(p, " ep"); p = str_u(p, ifs[i].neps);
			gfx_text(30 + (i / 6) * 480, 380 + (i % 6) * 34, s, &font16x32, 1, 0xa0ffa0);
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
	gfx_text(40, 60, "iPhone test crashed", &font16x32, 3, 0xffffff);
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
__asm__(".align 4\n iph_trap_entry:\n la sp, __stack_top\n call on_trap\n 1: j 1b\n");
extern char iph_trap_entry[];

int main(void)
{
	log_reset();
	if (display_up(FB0)) logf("iphonetest: display failed\n");
	display_brightness(10);
	__asm__ volatile("csrw mtvec, %0" :: "r"(iph_trap_entry));
	__asm__ volatile("csrci mstatus, 8\n\tcsrw mie, zero");   /* keep the boot ROM's USB handler out */

	/* leave device mode, become host. The controller picks its role (A = host,
	   B = device) when a session STARTS, so: end the FEL session, force the PHY's
	   ID pin low (host) and VBUS valid, then start a new session. */
	/* USB-A's 5 V: stock's Config.ini usb0PowerGpio=318 = PD18 (docs/devlog.md), drive it high */
	wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 8)) | (1u << 8));    /* PD18 output */
	wr32(0x020000A0, rd32(0x020000A0) | (1u << 18));                    /* PD18 high: VBUS on */
	wr8(POWER, 0x20);                                    /* soft-disconnect from the Mac; keep HS enabled */
	udelay(200000);
	wr8(VEND0, 0);
	wr8(INTRUSBE, 0);
	wr8(DEVCTL, 0);                                      /* end the session */
	udelay(200000);
	uint32_t iscr = rd32(ISCR) & ~(0x70u);               /* don't write back the change-detect flags */
	iscr = (iscr & ~((3u << 14) | (3u << 12))) | (2u << 14) | (3u << 12) | (1u << 17) | (1u << 16);
	wr32(ISCR, iscr);
	udelay(100000);
	wr8(INTRUSB, rd8(INTRUSB));
	wr8(DEVCTL, 0x01);                                   /* new session: should come up as host (A) */
	udelay(100000);
	devctl0 = rd8(DEVCTL);
	uint32_t shown = 0;
	for (;;) {
		uint8_t u = rd8(INTRUSB);
		if (u) wr8(INTRUSB, u);
		if (u & 0x10) {                                  /* CONN */
			connected = 1; n_conn++;
			str_cat(st_line, "connected, reading...");
			status();
			udelay(200000);                              /* let the device settle */
			enumerate();
		}
		if (u & 0x20) {                                  /* DISCON */
			connected = 0; have_dev = 0; speed = 0;
			str_cat(st_line, "disconnected");
		}
		if (!(rd8(DEVCTL) & 0x01)) wr8(DEVCTL, 0x01);   /* keep a (host) session up */
		if (millis() - shown >= 300) { shown = millis(); status(); }
	}
	return 0;
}
