/* dsi.h: MIPI-DSI command path (dsi.c). */
#pragma once
#include <stdint.h>

#define CCU        0x02001000u
#define DSI        0x05450000u
#define DPHY       0x05451000u
#define TCON_TOP   0x05460000u
#define TCON       0x05461000u


/* Panel mode (sys_config [lcd0]; Allwinner hbp/vbp include the sync pulse) */
#define HACT 720
#define HFP  140
#define HSW  8
#define HBP  40
#define HTOT 908
#define VACT 1280
#define VFP  22
#define VSW  6
#define VBP  9
#define VTOT 1317
#define PCLK_KHZ 70000
#define BPP 24
#define LANES 4

int ccu_setup(void);                          /* PLL_VIDEO0 420 MHz, DSI/TCON clocks + resets */
void pins_setup(void);                        /* PD0-PD9 -> DSI */
void dsi_init(unsigned lanes);                /* controller, command mode */
void dphy_power_on(unsigned lanes, uint32_t hs_hz);
int short_write(uint8_t dt, uint8_t d0, uint8_t d1);   /* 0 ok, -1 timeout */
uint32_t read_reg(uint8_t dt, uint8_t reg);   /* logs; returns RX word 0 */
void panel_init_send(void);                   /* [mipi_reg] list: sleep out, display on */
void tcon_setup(unsigned source);             /* 0 = display engine, 1-7 = TCON test pattern */
void dsi_video(void);                         /* non-burst video timing for the panel */
void start_hs(void);                          /* HS clock, then the HS video loop */
