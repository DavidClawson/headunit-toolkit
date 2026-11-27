/*
 * vsync.c: vblank wait check. Measures the frame period from TCON TRI_FINISH,
 * then animates horizontal bars sweeping up and down:
 *   5 s drawing straight into the visible buffer, no waiting (tears),
 *   5 s double-buffered, flipping on vblank (should be clean).
 * The panel scans buffer rows = screen columns, left to right, so tearing
 * shows as a bar broken by a vertical cut.
 *
 *   felrun.py dram/vsync.bin --timeout 40
 */
#include "lib.h"
#include "display.h"
#include "gfx.h"

static void scene(int frame, const char *label)
{
	gfx_clear(0x101820);
	for (int i = 0; i < 4; i++) {
		int span = GH - 80, y = (frame * (6 + 3 * i) + i * 150) % (2 * span);
		if (y >= span) y = 2 * span - y;
		static const uint32_t c[4] = { 0xff6b6b, 0xffd43b, 0x69db7c, 0x74c0fc };
		gfx_fill(0, y, GW, 40 + 8 * i, c[i]);
	}
	gfx_fill(0, 0, 520, 48, 0x000000);
	gfx_text(16, 8, label, &font16x32, 1, 0xffffff);
}

int main(void)
{
	log_reset();
	if (display_up(FB0)) { logf("vsync: display_up failed\n"); return 1; }

	/* frame period */
	unsigned per[8], ok = 1;
	display_vblank_wait();
	uint32_t t = ticks();
	for (int i = 0; i < 8; i++) {
		if (display_vblank_wait()) { ok = 0; break; }
		uint32_t n = ticks(); per[i] = (n - t) / TICKS_PER_US; t = n;
	}
	if (!ok) { logf("vsync: TRI_FINISH never set\n"); return 1; }
	logf("frame periods (us):");
	for (int i = 0; i < 8; i++) logf(" %u", per[i]);
	logf("\n");

	/* A: no sync, draw into the visible buffer */
	uint32_t front = display_fb();
	gfx_target(front);
	unsigned frames = 0;
	uint32_t t0 = ticks();
	while (ticks() - t0 < 5000000u * TICKS_PER_US) scene(frames++, "no vsync");
	logf("no vsync: %u frames in 5 s\n", frames);

	/* B: double-buffered, flip on vblank */
	uint32_t back = front == FB0 ? FB1 : FB0;
	unsigned f2 = 0, worst = 0;
	t0 = ticks();
	while (ticks() - t0 < 5000000u * TICKS_PER_US) {
		gfx_target(back);
		scene(f2++, "vsync, double buffered");
		uint32_t a = ticks();
		display_flip(back);
		unsigned w = (ticks() - a) / TICKS_PER_US;
		if (w > worst) worst = w;
		back = back == FB0 ? FB1 : FB0;
	}
	logf("vsync: %u frames in 5 s, longest flip wait %u us\n", f2, worst);
	return 0;
}
