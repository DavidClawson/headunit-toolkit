/*
 * settings.h: what survives a power cycle, kept in the settings sector
 * (FW_SETTINGS, the last 4 KB of our slot). Modules just change cfg; settings_tick()
 * notices the difference and writes it 3 s after the last change.
 */
#pragma once
#include <stdint.h>

#define SET_NFAV 6
enum { SRC_RADIO, SRC_BT };

typedef struct {
	uint8_t vol, muted, source, led;
	uint16_t freq;
	uint8_t bright;         /* 1..10; 0 (older records) = 10 */
	uint8_t no_clicks;      /* touch sounds off (0 = on: the default, and older records) */
	struct { uint16_t f; char name[24]; } fav[SET_NFAV];
} settings_t;

extern settings_t cfg;

int  settings_load(void);   /* 0 = loaded, 1 = defaults (blank or bad sector) */
int  settings_tick(void);   /* call every loop pass; 1 when it just saved */
