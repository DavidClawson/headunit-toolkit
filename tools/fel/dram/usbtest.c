/*
 * usbtest.c: first USB device-mode test. Takes over USB0 from the boot ROM's FEL
 * device and comes back as a CDC-ACM serial port (no driver needed on macOS,
 * Linux or Windows 10+). Once the host opens it: a line every second, and
 * everything the host sends is echoed back. The screen shows live enumeration
 * stats; after SECONDS they freeze there until a power cycle (FEL is gone, so
 * it can't return to it). A fault paints the screen red with mcause/mepc/mtval.
 *
 * Run with: tools/fel/felrun.py dram/usbtest.bin --usb-serial 60
 * VID:PID 1209:0001 is the pid.codes test ID.
 */
#include "lib.h"
#include "musb.h"
#include "display.h"
#include "gfx.h"
#include "../bootsel/fwboot.h"

#ifndef SECONDS
#define SECONDS 60
#endif

static const uint8_t dev[18] = {
	18, 1, 0x00, 0x02,            /* USB 2.0 */
	0x02, 0x00, 0x00,             /* class CDC (interfaces say the rest) */
	64,                           /* EP0 max packet */
	0x09, 0x12, 0x01, 0x00,       /* VID 0x1209, PID 0x0001 (pid.codes test) */
	0x00, 0x01,                   /* bcdDevice 1.00 */
	1, 2, 3,                      /* strings: manufacturer, product, serial */
	1,                            /* one configuration */
};

#define CFG(bulk_lo, bulk_hi, intv)                                                        \
	{ 9, 2, 67, 0, 2, 1, 0, 0x80, 50,                    /* config: 2 interfaces, 100 mA */ \
	  9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,                /* IF0: CDC comm, ACM, AT */       \
	  5, 0x24, 0x00, 0x10, 0x01,                         /* header, CDC 1.10 */             \
	  5, 0x24, 0x01, 0x00, 0x01,                         /* call management, data IF 1 */   \
	  4, 0x24, 0x02, 0x02,                               /* ACM: line coding supported */   \
	  5, 0x24, 0x06, 0x00, 0x01,                         /* union: master 0, slave 1 */     \
	  7, 5, 0x81, 0x03, 16, 0, intv,                     /* EP1 IN interrupt */             \
	  9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,                /* IF1: CDC data */                \
	  7, 5, 0x02, 0x02, bulk_lo, bulk_hi, 0,             /* EP2 OUT bulk */                 \
	  7, 5, 0x82, 0x02, bulk_lo, bulk_hi, 0 }            /* EP2 IN bulk */
static const uint8_t cfg_hs[67] = CFG(0x00, 0x02, 9);   /* 512-byte bulk; interrupt every 32 ms */
static const uint8_t cfg_fs[67] = CFG(64, 0, 32);

static const char *const strings[] = { "headunit-toolkit", "Head Unit serial (USB test)", "F133-0001" };
static const struct musb_ep eps[] = {
	{ 1, 1, 3, 16 },              /* EP1 IN interrupt (notifications; never sent) */
	{ 2, 0, 2, 512 },             /* EP2 OUT bulk */
	{ 2, 1, 2, 512 },             /* EP2 IN bulk */
};

static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 (meaningless here) */
static int dtr;

static int acm_setup(const uint8_t r[8], uint8_t *buf, const uint8_t *out, int outlen)
{
	switch (r[1]) {
	case 0x20: if (out && outlen >= 7) memcpy(line_coding, out, 7); return 0;    /* SET_LINE_CODING */
	case 0x21: memcpy(buf, line_coding, 7); return 7;                            /* GET_LINE_CODING */
	case 0x22: dtr = r[2] & 1; logf("usb: control lines %x\n", r[2]); return 0;  /* SET_CONTROL_LINE_STATE */
	}
	return -1;
}

static void acm_configured(int c) { logf("usb: configuration %d (%s speed)\n", c, musb_high_speed() ? "high" : "full"); }

static const struct musb_class acm = {
	dev, cfg_hs, cfg_fs, strings, 3, eps, 3, acm_setup, acm_configured, 0,
};

static const char hexd[] = "0123456789ABCDEF";
static char *hex8(char *p, uint8_t v) { *p++ = hexd[v >> 4]; *p++ = hexd[v & 15]; *p = 0; return p; }
static char *hex32(char *p, uint32_t v) { for (int i = 28; i >= 0; i -= 4) *p++ = hexd[(v >> i) & 15]; *p = 0; return p; }

/* the screen shows how far enumeration got, even if the host sees nothing */
static void status(uint32_t secs_left)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x101820);
	char s[96], *p;
	uint32_t resets, setups, st = musb_stats(&resets, &setups);
	gfx_text(40, 30, "USB device test", &font16x32, 2, 0xffffff);
	p = str_cat(s, "bus resets "); p = str_u(p, resets); p = str_cat(p, "   setups "); p = str_u(p, setups);
	gfx_text(40, 120, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, "power/devctl/addr "); p = hex32(p, st);
	p = str_cat(p, musb_high_speed() ? "  high speed" : "  full speed");
	gfx_text(40, 190, s, &font16x32, 2, 0xffffff);
	p = str_cat(s, musb_configured() ? "CONFIGURED" : "not configured");
	p = str_cat(p, dtr ? ", port open" : "");
	gfx_text(40, 260, s, &font16x32, 2, musb_configured() ? 0x40ff60 : 0xffc040);
	uint8_t r[6][8];
	int n = musb_recent(r, 6);
	for (int i = 0; i < n; i++) {
		p = s;
		for (int j = 0; j < 8; j++) { p = hex8(p, r[i][j]); *p++ = ' '; }
		*p = 0;
		gfx_text(40, 330 + i * 40, s, &font16x32, 1, 0xa0c0ff);
	}
	if (secs_left) { p = str_cat(s, "running: "); p = str_u(p, secs_left); p = str_cat(p, " s left"); }
	else p = str_cat(s, "DONE: take a photo, then power-cycle");
	gfx_text(40, 580, s, &font16x32, 1, 0x808080);
	dcache_flush();
	display_flip(fb);
}

/* a fault paints the screen red with its cause instead of vanishing */
void on_trap(void)
{
	uint64_t cause, epc, tval;
	__asm__ volatile("csrr %0, mcause\n\tcsrr %1, mepc\n\tcsrr %2, mtval" : "=r"(cause), "=r"(epc), "=r"(tval));
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0xa00000);
	char s[64], *p;
	gfx_text(40, 60, "USB test crashed", &font16x32, 3, 0xffffff);
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
__asm__(".align 4\n trap_entry:\n la sp, __stack_top\n call on_trap\n 1: j 1b\n");
extern char trap_entry[];

int main(void)
{
	log_reset();
	if (display_up(FB0)) logf("usbtest: display failed\n");
	__asm__ volatile("csrw mtvec, %0" :: "r"(trap_entry));
	/* the boot ROM drives FEL's USB with interrupts and leaves them enabled: its
	   handler would answer the host itself. We poll, so machine interrupts go off. */
	__asm__ volatile("csrci mstatus, 8\n\tcsrw mie, zero");
	display_brightness(10);
	logf("usbtest: taking over USB0\n");
	musb_start(&acm);
	uint32_t t0 = millis(), last = t0, n = 0, shown = 0;
	char line[96];
	uint8_t rx[512];
	while (millis() - t0 < SECONDS * 1000u) {
		musb_poll();
		if (millis() - shown >= 300) { shown = millis(); status(SECONDS - (millis() - t0) / 1000); }
		if (!musb_configured()) continue;
		int k = musb_read(2, rx, sizeof rx);
		if (k > 0) {
			char *p = str_cat(line, "echo: ");
			for (int i = 0; i < k && p < line + sizeof line - 3; i++) *p++ = (rx[i] >= 'a' && rx[i] <= 'z') ? rx[i] - 32 : rx[i];
			*p = 0;
			while (musb_write(2, line, p - line) < 0) musb_poll();
		}
		if (dtr && millis() - last >= 1000) {
			last = millis();
			char *p = str_cat(line, "hello from the head unit #");
			p = str_u(p, ++n);
			p = str_cat(p, musb_high_speed() ? " (high speed)\r\n" : " (full speed)\r\n");
			musb_write(2, line, p - line);
		}
	}
	uint32_t resets, setups, st = musb_stats(&resets, &setups);
	logf("usbtest: done; %u resets, %u setups, power/devctl/addr %x\n", resets, setups, st);
	/* keep the final numbers on screen until the power cycle (FEL is gone; no way back) */
	for (;;) { musb_poll(); status(0); udelay(200000); }
	return 0;
}
