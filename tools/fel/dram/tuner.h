/*
 * tuner.h: FM radio hardware. Si4730 tuner on the bit-banged bus (bbi2c.c,
 * SCL PG14 / SDA PB4, reset PB5) and the BD3702 sound processor on TWI2 0x40.
 * Settings are the stock app's (init.axf), except US de-emphasis and 200 kHz
 * steps. Frequencies are in 10 kHz units (10110 = 101.1 MHz).
 * Tune and seek don't block: start them, then call tuner_poll() each loop.
 */
#pragma once
#include <stdint.h>

#define FM_MIN  8750
#define FM_MAX 10790
#define FM_STEP   20

typedef struct {
	int ok;                 /* tuner answered and powered up */
	uint8_t part, fw_major, fw_minor, rev;
	uint16_t freq;          /* current (or, while seeking, passing) frequency */
	uint8_t rssi, snr;      /* dBuV, dB */
	uint8_t stereo;         /* % blend; >= 50 shown as stereo */
	uint8_t valid;          /* the chip judges this a real station */
	uint8_t busy;           /* tune or seek in progress */
	uint8_t band_limit;     /* the last seek hit the band edge */
} tuner_state;

extern tuner_state tuner;

int  tuner_init(void);              /* reset, power up, set properties; 0 ok */
void tuner_tune(uint16_t freq);
void tuner_seek(int up, int wrap);
int  tuner_poll(void);              /* 1 when tuner.* changed; 2 when a tune/seek just finished */

/* sound: one volume for every source */
#define SOUND_MAX 30
#define SOUND_IN_RADIO 0x0B         /* BD3702 input select (register 0x05) */
#define SOUND_IN_BT    0x0A
extern int snd_vol, snd_muted;      /* 1..SOUND_MAX; muted flag */
void sound_init(void);              /* once: BD3702 table, volume applied */
void sound_input(uint8_t in);
void sound_apply(void);             /* write snd_vol / snd_muted */
void sound_step(int dir);           /* volume +/- 1 (unmutes) */
