/* twiscan.c: scan TWI1 and TWI2 (bus setup in twi.c). */
#include "lib.h"
#include "twi.h"
int main(void)
{
	log_reset();
	for (unsigned bus = 1; bus <= 2; bus++) {
		twi_init(bus);
		logf("TWI%u:", bus);
		for (unsigned a = 0x08; a < 0x78; a++)
			if (twi_write(bus, (uint8_t)a, 0, 0, 0, 0) == TWI_OK) logf(" %02x", a);
		logf("\n");
	}
	return 0;
}
