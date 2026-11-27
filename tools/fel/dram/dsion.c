/* dsion.c: re-send display on (0x29) to an initialised panel; read power mode. */
#include "lib.h"
#include "dsi.h"
int main(void)
{
	log_reset();
	if (ccu_setup()) return 1;
	pins_setup();
	dsi_init(4);
	dphy_power_on(4, 420000000u);
	udelay(10000);
	short_write(0x37, 1, 0);
	read_reg(0x06, 0x0A);
	logf("0x29 (dt 05): %d\n", short_write(0x05, 0x29, 0));
	udelay(20000);
	read_reg(0x06, 0x0A);
	logf("0x29 (dt 15, param 0): %d\n", short_write(0x15, 0x29, 0));
	udelay(20000);
	read_reg(0x06, 0x0A);
	return 0;
}
