/* bt_ui.h: the Bluetooth "Now Playing" page (bt_ui.c), on top of bt.c. */
#pragma once
void bt_ui_open(void);            /* switches the sound input to Bluetooth */
void bt_ui_draw(int pressed);
int  bt_ui_hit(int x, int y);     /* button id (>= 1) or -1 */
int  bt_ui_press(int id);         /* 1 = redraw */
int  bt_ui_knob(int dir);         /* volume */
int  bt_ui_knob_push(void);       /* play / pause */
int  bt_ui_tick(void);            /* 1 = redraw (once a second while playing, for the clock) */
