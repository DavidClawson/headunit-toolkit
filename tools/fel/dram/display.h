/* display.h: the LCD pipeline (docs/display.md): DSI panel + TCON + DE2 mixer 0. */
#pragma once
#include <stdint.h>

#define FB0 0x41000000u          /* two 720x1280 XRGB8888 buffers, 3.5 MiB each */
#define FB1 0x41400000u

int  display_running(void);      /* pipeline already up (from an earlier program)? */
int  display_up(uint32_t fb);    /* bring everything up scanning fb; no-op if running */
void display_flip(uint32_t fb);  /* scan out fb from the next frame; waits for it */
int  display_vblank_wait(void);  /* wait for the next frame end (TCON TRI_FINISH); -1 on timeout */
uint32_t display_fb(void);       /* buffer being scanned out */
void display_brightness(int level);   /* 1..10: backlight PWM (PD22 = PWM7) */
