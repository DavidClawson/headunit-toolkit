/* input.h: one event queue for touch (GT911) and front-panel keys (MCU). */
#pragma once
#include <stdint.h>

enum { EV_TOUCH_DOWN = 1, EV_TOUCH_MOVE, EV_TOUCH_UP, EV_KEY };
typedef struct { uint8_t type; uint8_t key; int16_t x, y; uint32_t ms; } event;

int  input_init(void);              /* 0 ok; touch failure is reported but keys still work */
int  input_next(event *e);          /* poll the sources, then pop one event; 0 if none */
