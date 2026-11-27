/*
 * gfx.h: drawing into the LCD framebuffer in landscape screen coordinates.
 * The screen is 1280 x 720 as seen; the buffer is 720 x 1280 XRGB8888 in
 * memory, rotated: screen (sx, sy) = buffer (x = 719 - sy, y = sx).
 * Colours are 0xRRGGBB. Everything clips to the screen.
 */
#pragma once
#include <stdint.h>

#define GW 1280
#define GH 720

typedef struct { uint8_t w, h; const uint16_t *rows; } font_t;   /* ASCII 32-126 */
extern const font_t font8x16, font16x32;

void gfx_target(uint32_t fb);                           /* buffer to draw into */
void gfx_clear(uint32_t c);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_rect(int x, int y, int w, int h, int t, uint32_t c);       /* outline, t px */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c);      /* filled, rounded */
void gfx_rring(int x, int y, int w, int h, int r, int t, uint32_t c);  /* rounded outline, t px */
void gfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
int  gfx_text(int x, int y, const char *s, const font_t *f, int scale, uint32_t c); /* returns end x */
int  gfx_text_w(const char *s, const font_t *f, int scale);
uint32_t gfx_mix(uint32_t a, uint32_t b, unsigned t);   /* t/256 of the way from a to b */
