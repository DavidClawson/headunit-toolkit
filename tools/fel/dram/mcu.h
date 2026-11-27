/* mcu.h: Bluetrum AB5301 front-panel MCU on UART1 (docs/mcu-protocol.md).
   We never send A0, so the MCU's heartbeat watchdog stays off; other commands
   (key LED colour) are accepted without it. */
#pragma once
#include <stdint.h>

typedef struct { uint8_t dir, cmd, len, data[64]; } mcu_frame;   /* 02 dir cmd len data xor */

enum { MCU_KEY_KNOB = 0x01, MCU_KEY_CW = 0x04, MCU_KEY_CCW = 0x05, MCU_KEY_PHONE = 0x06 };

void mcu_init(void);
int  mcu_poll(mcu_frame *f);        /* 1 when a whole, checksum-good frame arrived */
int  mcu_key(const mcu_frame *f);   /* key code KK of a B2 key frame, else -1 */
void mcu_send(uint8_t cmd, const void *data, unsigned len);   /* 02 80 cmd len data xor */
enum { LED_CYCLE = 0, LED_PURPLE, LED_RED, LED_YELLOW, LED_CYAN, LED_WHITE, LED_BLUE, LED_GREEN, LED_OFF = 0xFF };
void mcu_key_led(uint8_t mode);     /* front-button LED colour (B0 [FE 00 00 5 mode]) */
extern unsigned mcu_bad_frames;
