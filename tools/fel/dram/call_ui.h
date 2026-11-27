/* call_ui.h: the phone-call overlay (call_ui.c), shown over any page while bt.call != 0. */
#pragma once
void call_ui_draw(int pressed);
int  call_ui_hit(int x, int y);   /* button id or -1 */
int  call_ui_press(int id);       /* 1 = redraw */
int  call_ui_knob_push(void);     /* answer if ringing, else hang up */
int  call_ui_tick(void);          /* audio routing on call start/end; 1 = redraw (timer, state) */
