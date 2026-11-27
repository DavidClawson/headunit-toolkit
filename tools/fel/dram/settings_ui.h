/* settings_ui.h: the Settings page (settings_ui.c). */
#pragma once
void settings_ui_draw(int pressed);
int  settings_ui_hit(int x, int y);   /* button id (>= 1) or -1 */
int  settings_ui_press(int id);       /* 1 = redraw */
int  settings_ui_knob(int dir);       /* brightness */
int  settings_ui_tick(void);          /* 1 = redraw (confirm prompt timing out) */
