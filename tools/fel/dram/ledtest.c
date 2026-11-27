/* ledtest.c: try encodings of key-LED "cycle" and "off". Each step: red for 2 s
   (so a change is visible), then the variant for 6 s; the MCU's echo is logged. */
#include "lib.h"
#include "display.h"
#include "gfx.h"
#include "mcu.h"

static const struct { uint8_t t, m; const char *what; } v[] = {
	{ 5, 0x00, "A cycle t5 m0" },  { 2, 0x00, "B cycle t2 m0" },  { 5, 0x08, "C t5 m8" },
	{ 5, 0x09, "D t5 m9" },        { 0, 0xFF, "E off t0 mFF" },   { 5, 0xFF, "F off t5 mFF" },
	{ 0, 0x00, "G t0 m0" },        { 0, 0x08, "H t0 m8" },
};

static void label(const char *s)
{
	uint32_t fb = display_fb() == FB0 ? FB1 : FB0;
	gfx_target(fb);
	gfx_clear(0x1b2a4a);
	gfx_text((GW - gfx_text_w(s, &font16x32, 3)) / 2, 280, s, &font16x32, 3, 0xffffff);
	display_flip(fb);
}

static void wait_ms(uint32_t ms)
{
	uint32_t t0 = millis();
	mcu_frame f;
	while (millis() - t0 < ms)
		if (mcu_poll(&f)) {
			if (f.cmd == 0xB2 && f.len == 6 && f.data[1] == 0xFE)
				logf("    echo: t %u m %02x\n", f.data[4], f.data[5]);
			else mcu_service(&f);
		}
}

static void led(uint8_t t, uint8_t m) { uint8_t d[5] = { 0xFE, 0, 0, t, m }; mcu_send(0xB0, d, 5); }

int main(void)
{
	log_reset();
	logf("key LED test\n");
	if (display_up(FB0)) return 1;
	display_brightness(10);
	mcu_init();
	mcu_start();
	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
		label("red");
		led(5, 2);
		wait_ms(2000);
		label(v[i].what);
		logf("%s\n", v[i].what);
		led(v[i].t, v[i].m);
		wait_ms(6000);
	}
	led(5, 5);
	label("done (white)");
	wait_ms(1000);
	return 0;
}
