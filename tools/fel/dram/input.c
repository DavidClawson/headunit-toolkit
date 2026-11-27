/* input.c: see input.h. Touch is reduced to the first finger, in screen coordinates. */
#include "lib.h"
#include "gfx.h"
#include "gt911.h"
#include "mcu.h"
#include "input.h"

#define QN 32
static event q[QN];
static unsigned qh, qt;
static int touch_ok, down;
static uint16_t xmax, ymax;
static int16_t lx, ly;
static uint32_t t_start;

static void push(uint8_t type, uint8_t key, int x, int y)
{
	if (qt - qh >= QN) return;                       /* full: drop */
	event *e = &q[qt++ % QN];
	e->type = type; e->key = key; e->x = (int16_t)x; e->y = (int16_t)y;
	e->ms = (ticks() - t_start) / (1000 * TICKS_PER_US);
}

int input_init(void)
{
	t_start = ticks();
	mcu_init();
	char id[5]; uint16_t fw;
	touch_ok = gt911_init(id, &xmax, &ymax, &fw) == 0 && xmax && ymax;
	return touch_ok ? 0 : -1;
}

static void poll_sources(void)
{
	mcu_frame f;
	while (mcu_poll(&f)) {
		int k = mcu_key(&f);
		if (k >= 0) push(EV_KEY, (uint8_t)k, 0, 0);
	}
	if (!touch_ok) return;
	touch_pt p[1];
	int n = gt911_poll(p, 1);
	if (n < 0) return;                               /* no new frame (or a bus error) */
	if (n > 0) {
		int x = p[0].x * GW / xmax, y = p[0].y * GH / ymax;
		if (!down) { down = 1; push(EV_TOUCH_DOWN, 0, x, y); }
		else if (x != lx || y != ly) push(EV_TOUCH_MOVE, 0, x, y);
		lx = (int16_t)x; ly = (int16_t)y;
	} else if (down) {
		down = 0;
		push(EV_TOUCH_UP, 0, lx, ly);
	}
}

int input_next(event *e)
{
	if (qh == qt) poll_sources();
	if (qh == qt) return 0;
	*e = q[qh++ % QN];
	return 1;
}
