/* dsi.h: MIPI-DSI command path (dsi.c). */
#pragma once
#include <stdint.h>

#define CCU        0x02001000u
#define DSI        0x05450000u
#define DPHY       0x05451000u
#define TCON_TOP   0x05460000u

int ccu_setup(void);                          /* PLL_VIDEO0 420 MHz, DSI/TCON clocks + resets */
void pins_setup(void);                        /* PD0-PD9 -> DSI */
void dsi_init(unsigned lanes);                /* controller, command mode */
void dphy_power_on(unsigned lanes, uint32_t hs_hz);
int short_write(uint8_t dt, uint8_t d0, uint8_t d1);   /* 0 ok, -1 timeout */
uint32_t read_reg(uint8_t dt, uint8_t reg);   /* logs; returns RX word 0 */
