/* home.h: mock home screen (home.c). */
#pragma once
#define HOME_TILES 4
void home_draw(void);                      /* whole screen */
void home_tile(int i, int pressed);        /* redraw one tile */
void home_tile_rect(int i, int *x, int *y, int *w, int *h);
int  home_hit(int sx, int sy);             /* tile under a screen point, or -1 */
const char *home_tile_name(int i);
