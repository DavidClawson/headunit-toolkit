/* bltest.c: backlight PWM variants, 4 s each, step number on screen.
   1 GPIO high (known good)   2 PWM7 active-high 50 %   3 PWM7 active-low 50 %
   4 PWM7 from APB clock      5 PWM7 clock bypass        6 GPIO low (dark) */
#include "lib.h"
#include "display.h"
#include "gfx.h"

#define PWM 0x02000C00u
static void pd22(unsigned fn) { wr32(0x02000098, (rd32(0x02000098) & ~(0xFu << 24)) | (fn << 24)); }
static void gpio(int hi)
{
	pd22(1);
	wr32(0x020000A0, hi ? rd32(0x020000A0) | (1u << 22) : rd32(0x020000A0) & ~(1u << 22));
}
static void pwm(uint32_t src, int act_high, int bypass)
{
	wr32(0x020017AC, rd32(0x020017AC) | 1u | (1u << 16));
	wr32(PWM + 0x2C, src << 7);
	wr32(PWM + 0x40, (rd32(PWM + 0x40) | (1u << 7)) & ~(1u << 23));
	if (bypass) wr32(PWM + 0x40, rd32(PWM + 0x40) | (1u << 23));
	wr32(PWM + 0x1E0, act_high ? 1u << 8 : 0);
	wr32(PWM + 0x1E4, (1199u << 16) | 600u);
	wr32(PWM + 0x80, rd32(PWM + 0x80) | (1u << 7));
	pd22(5);
	logf("  PWM regs: bgr %08x ccr67 %08x gate %08x en %08x ctl %08x per %08x pd_cfg2 %08x\n",
	     rd32(0x020017AC), rd32(PWM + 0x2C), rd32(PWM + 0x40), rd32(PWM + 0x80),
	     rd32(PWM + 0x1E0), rd32(PWM + 0x1E4), rd32(0x02000098));
}
static void label(int n)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x1b2a4a);
	char s[16] = "STEP ";
	s[5] = (char)('0' + n); s[6] = 0;
	gfx_text((GW - gfx_text_w(s, &font16x32, 6)) / 2, 250, s, &font16x32, 6, 0xffffff);
	display_flip(fb);
}

int main(void)
{
	log_reset();
	logf("backlight test\n");
	if (display_up(FB0)) { logf("display_up failed\n"); return 1; }
	for (int n = 1; n <= 6; n++) {
		label(n);
		logf("step %d\n", n);
		if (n == 1) gpio(1);
		if (n == 2) pwm(0, 1, 0);
		if (n == 3) pwm(0, 0, 0);
		if (n == 4) pwm(1, 1, 0);
		if (n == 5) pwm(0, 1, 1);
		if (n == 6) gpio(0);
		udelay(4000000);
	}
	gpio(1);
	label(0);
	logf("done (GPIO high again)\n");
	return 0;
}
