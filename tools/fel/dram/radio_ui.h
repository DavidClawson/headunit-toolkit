/* radio_ui.h: the Radio page (radio_ui.c), on top of tuner.c. */
#pragma once
#include <stdint.h>
void radio_open(void);                    /* powers the tuner up the first time */
void radio_draw(int pressed);             /* page body; pressed = button id held down, or -1 */
int  radio_modal(void);                   /* 1 while the station list or a popup covers the page */
int  radio_hit(int x, int y);             /* button id (>= 1) under a point, or -1 */
int  radio_press(int id);                 /* tap released on id; 1 = redraw */
int  radio_hold(int id);                  /* id held for RADIO_HOLD_MS; 1 = redraw */
#define RADIO_HOLD_MS 800
int  radio_knob(int dir);                 /* +1 / -1, per the knob mode; 1 = redraw */
int  radio_knob_push(void);               /* next knob mode; 1 = redraw */
int  radio_tick(void);                    /* call every loop pass; 1 = redraw */
const char *radio_note(void);             /* short message for the banner, or 0 */
