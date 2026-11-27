/*
 * musb.h: USB device mode on the F133's USB0 (Mentor MUSB core, Allwinner register
 * layout), polled, no interrupts. Endpoint 0 is handled here (standard requests);
 * class requests and the data endpoints belong to the caller.
 *
 * Register offsets follow the Allwinner remapping (Linux drivers/usb/musb/sunxi.c,
 * used as a reference). The controller at 0x04100000 is the port the FEL cable
 * uses: under FEL the boot ROM has already powered and clocked it, so musb_start()
 * only soft-disconnects, reprograms and reconnects.
 */
#pragma once
#include <stdint.h>

struct musb_ep {            /* one data endpoint (1..4), as the descriptors declare it */
	uint8_t num;            /* endpoint number */
	uint8_t in;             /* 1 = IN (device to host) */
	uint8_t type;           /* 2 bulk, 3 interrupt */
	uint16_t maxp;          /* high-speed max packet size; full speed uses min(maxp, 64) */
};

struct musb_class {
	const uint8_t *dev_desc;                         /* 18 bytes */
	const uint8_t *cfg_desc_hs, *cfg_desc_fs;        /* full configuration descriptors */
	const char *const *strings;                      /* index 1.. (0 = language list) */
	int nstrings;
	const struct musb_ep *eps;
	int neps;
	/* class/vendor request on EP0. Return length of IN data placed in buf (<= 64*8),
	   0 for no data stage, -1 to stall. For OUT data, `out` holds what the host sent. */
	int (*setup)(const uint8_t req[8], uint8_t *buf, const uint8_t *out, int outlen);
	void (*configured)(int config);                  /* SET_CONFIGURATION, 0 = deconfigured / reset */
};

int  musb_start(const struct musb_class *c);    /* disconnect, set up, connect; 0 ok */
void musb_poll(void);                           /* run often: bus events and EP0 */
int  musb_configured(void);
int  musb_high_speed(void);
int  musb_write(int ep, const void *buf, int n); /* one packet (<= maxp, 0 = ZLP) if the FIFO is free: n, else -1 */
int  musb_read(int ep, void *buf, int max);      /* one received packet: its length, 0 if none */
void musb_stop(void);                           /* soft-disconnect */
uint32_t musb_stats(uint32_t *resets, uint32_t *setups);
int  musb_recent(uint8_t req[][8], int max);   /* the last few SETUP packets, newest first (debugging) */
