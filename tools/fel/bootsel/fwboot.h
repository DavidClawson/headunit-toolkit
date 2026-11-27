/*
 * fwboot.h: the contract between the boot0 selector (stub.S, `sel`) and our
 * firmware (dram/fw.c). Plain #defines so both C and assembly can use it.
 *
 * NOR 0x120000 (64 KB, blank in stock: between the end of the Melis package
 * and MINFS at 0x130000):
 *   sector 0x900       header (below), rest of the sector unused
 *   sector 0x901...    the program, linked at FW_LOAD, padded to 4 bytes
 */
#pragma once

#define FW_SECTOR      0x900                 /* 0x120000 / 512 */
#define FW_MAX         (0x10000 - 512)       /* program bytes that fit in the slot */
#define FW_APP_MAX     (0xF000 - 512)        /* what mkfw.py builds: the slot's last 4 KB is settings */
#define FW_SETTINGS    0x12F000              /* settings sector (dram/settings.c); never erased by fw-write.sh */
#define FW_MAGIC       0x57465548            /* "HUFW" little-endian */
#define FW_VERSION     1
#define FW_LOAD        0x40000000
#define FW_HDR_BUF     0x42000000            /* DRAM scratch for the header sector */

/* header words: [0] magic [1] version [2] load [3] size [4] sum (32-bit
   additive over the program's words) [5..] zero */

#define RTC_GP5        0x07090114            /* survives a watchdog reset, not a power cycle */
#define GP5_TRYING     0x7E57B007            /* set by sel before the jump; fw clears it once up */
#define GP5_STOCK1     0x57C0B007            /* "boot stock once": set by fw, consumed by sel */

#define WDT_CTRL       0x020500B0            /* write WDT_RELOAD to restart the count */
#define WDT_RELOAD     ((0xA57 << 1) | 1)
#define WDT_CFG        0x020500B4
#define WDT_MODE       0x020500B8
#define WDT_KEY        0x16AA0000
#define FW_WDT_INTV    7                     /* WDT_MODE interval 7 = 8 s */
