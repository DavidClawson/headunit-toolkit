/*
 * bt.h: the Bluetooth module, a LITIANTAI WB020 on UART4 (PB2/PB3, 115200).
 * Text protocol: we send "AT#xx\r\n", it sends status lines (docs/mcu-protocol.md).
 * It keeps its own pairing list and reconnects the phone by itself.
 * Audio comes out into BD3702 input 0x0A (SOUND_IN_BT).
 */
#pragma once
#include <stdint.h>

typedef struct {
	int connected;          /* a phone is connected (IB, or any music status seen) */
	int playing;            /* MB = playing, MA = paused */
	char title[64], artist[64], album[64];   /* ASCII (accents folded, others '?') */
	unsigned pos, len;      /* seconds */
	uint32_t pos_ms;        /* millis() when pos arrived, to run the clock between updates */
	int call;               /* BT_CALL_* */
	char number[32];        /* caller / callee */
	char caller[48];        /* contact name, when the phone sends one (IQ line) */
	uint32_t call_ms;       /* millis() when the call was answered */
} bt_state;

enum { BT_CALL_NONE, BT_CALL_RINGING, BT_CALL_OUTGOING, BT_CALL_ACTIVE };
extern bt_state bt;

void bt_init(void);
int  bt_poll(void);         /* read status lines; 1 when bt.* changed */
void bt_cmd(const char *c); /* sends AT#c\r\n */
#define bt_play()  bt_cmd("MB")
#define bt_pause() bt_cmd("MA")
#define bt_next()  bt_cmd("MD")
#define bt_prev()  bt_cmd("ME")
#define bt_answer() bt_cmd("CE")
#define bt_reject() bt_cmd("CF")
#define bt_hangup() bt_cmd("CG")
