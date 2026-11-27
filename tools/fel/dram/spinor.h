/*
 * spinor.h: the 16 MB SPI NOR flash (XMC 25QH128) on SPI0 (PC2-PC7), for our
 * settings. Reads may go anywhere; erase and program are ONLY allowed inside
 * the settings sector (FW_SETTINGS, 4 KB) and refuse anything else.
 */
#pragma once
#include <stdint.h>
void spinor_init(void);
int  spinor_id(uint8_t id[3]);                              /* JEDEC ID; 0 ok */
int  spinor_read(uint32_t addr, void *buf, unsigned n);    /* 0 ok */
int  spinor_settings_erase(void);                          /* the whole settings sector; 0 ok */
int  spinor_settings_write(unsigned off, const void *buf, unsigned n);   /* inside it; 0 ok */
