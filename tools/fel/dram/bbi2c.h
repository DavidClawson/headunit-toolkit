/* bbi2c.h: bit-banged I2C on two GPIOs (open drain emulated with the SoC
   pull-ups), ~50 kHz, with clock stretching. The radio tuner's bus: the stock
   kernel runs TWI1 in software on PG14 (SCL) + PB4 (SDA), per the board config. */
#pragma once
#include <stdint.h>
void bb_init(void);                                          /* SCL = PG14, SDA = PB4 */
int  bb_write(uint8_t addr, const uint8_t *buf, unsigned n); /* 0 = ok, 1 = NACK */
int  bb_read(uint8_t addr, uint8_t *buf, unsigned n);
