/* gt911.h: Goodix GT911 touch controller on TWI3 at 0x5d (INT PG07, RESET PG08). */
#pragma once
#include <stdint.h>

typedef struct { uint8_t id; uint16_t x, y, size; } touch_pt;

/* Reset (selecting 0x5d), probe. Fills the product ID string and the
   configured resolution. 0 on success, else a TWI_* error. */
int gt911_init(char id[5], uint16_t *xmax, uint16_t *ymax, uint16_t *fw);
/* Poll: returns the number of points (0-5) if a new frame was ready, -1 if
   no new frame, or -2 - TWI error. Clears the ready flag. */
int gt911_poll(touch_pt *pts, int max);
