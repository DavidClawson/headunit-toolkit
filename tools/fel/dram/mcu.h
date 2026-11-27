/* mcu.h: Bluetrum AB5301 front-panel MCU on UART1 (docs/mcu-protocol.md).
   Listen-only here: we never send A0, so the MCU's heartbeat watchdog stays off. */
#pragma once
#include <stdint.h>

typedef struct { uint8_t dir, cmd, len, data[64]; } mcu_frame;   /* 02 dir cmd len data xor */

enum { MCU_KEY_KNOB = 0x01, MCU_KEY_CW = 0x04, MCU_KEY_CCW = 0x05, MCU_KEY_PHONE = 0x06 };

void mcu_init(void);
int  mcu_poll(mcu_frame *f);        /* 1 when a whole, checksum-good frame arrived */
int  mcu_key(const mcu_frame *f);   /* key code KK of a B2 key frame, else -1 */
extern unsigned mcu_bad_frames;
