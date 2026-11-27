/* twi.h: I2C master on the F133 TWI controllers (ported from tools/fel/i2c.c). */
#pragma once
#include <stdint.h>

enum { TWI_OK = 0, TWI_TIMEOUT = 1, TWI_START = 2, TWI_NACK_ADDR = 3, TWI_NACK_DATA = 4, TWI_ARG = 5 };

void twi_init(unsigned bus);     /* clocks, pins (TWI3 = PG10/PG11 only), 100 kHz */
/* reglen = 0, 1 or 2 register-address bytes, sent MSB first. Return TWI_*. */
int twi_read(unsigned bus, uint8_t addr, uint32_t reg, unsigned reglen, uint8_t *buf, unsigned len);
int twi_write(unsigned bus, uint8_t addr, uint32_t reg, unsigned reglen, const uint8_t *buf, unsigned len);
