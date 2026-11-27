/*
 * audio.c: the F133 audio codec (D1 family), played by DMA.
 *
 * Clock: PLL_AUDIO1 = 3072 MHz, /5 = 614.4 MHz, /25 = 24.576 MHz DAC clock, FS /512 = 48 kHz.
 * Analog: ALDO + HPLDO on, headphone driver on (the F133's only analog output);
 *   the line-out bits are set too (harmless; the D1 die has them).
 * DMA (0x03002000, sun6i/H6 layout): channel AUD_CH loops over two descriptors, one
 *   per half of a ring buffer, to the codec's TX FIFO (DRQ 7), forever. No interrupts:
 *   audio_service() reads the channel's current source address and refills behind it.
 * The D-cache is write-back, so the ring is cleaned to RAM after each fill.
 * Register sources: Linux sun20i-codec.c (smaeul/linux), sun6i-dma.c, ccu-sun20i-d1.c.
 */
#include "lib.h"
#include "audio.h"
#include "sine256.h"

#define CCU     0x02001000u
#define CODEC   0x02030000u
#define DMAC    0x03002000u
#define AUD_CH  0
#define CH(o)   (DMAC + 0x100u + AUD_CH * 0x40u + (o))

#define RING    8192u                   /* frames (170 ms) */
#define LEAD    (48u * 100u)            /* keep ~100 ms queued */

static uint32_t ring[RING] __attribute__((aligned(64)));   /* L | R << 16 */
static struct lli { uint32_t cfg, src, dst, len, para, next, pad[2]; } lli[2] __attribute__((aligned(64)));
static uint64_t wabs, rabs;             /* absolute frames written / played */
static uint32_t last_r, last_ms;
audio_stats_t audio_stats;

/* voices */
#define NV 4
static struct { uint32_t phase, step, left, total; int level; } v[NV];

static int clocks(void)
{
	uint32_t pll = (1u << 31) | (1u << 30) | (1u << 29) | (127u << 8) | (4u << 20) | (1u << 16);
	if ((rd32(CCU + 0x080) & ~(1u << 28)) != (pll | (1u << 27))) {
		wr32(CCU + 0x080, pll);
		uint32_t t0 = millis();
		while (!(rd32(CCU + 0x080) & (1u << 28)))
			if (millis() - t0 > 50) return -1;
		wr32(CCU + 0x080, pll | (1u << 27));
	}
	wr32(CCU + 0xA50, (1u << 31) | (2u << 24) | 24u);            /* DAC clock 24.576 MHz */
	wr32(CCU + 0xA5C, rd32(CCU + 0xA5C) | 1u | (1u << 16));      /* codec bus gate + reset */
	wr32(CCU + 0x70C, rd32(CCU + 0x70C) | 1u | (1u << 16));      /* DMA bus gate + reset */
	wr32(CCU + 0x804, rd32(CCU + 0x804) | 1u);                   /* DMA MBUS gate */
	udelay(100);
	return 0;
}

static void codec_on(void)
{
	wr32(CODEC + 0x348, rd32(CODEC + 0x348) | (1u << 31) | (1u << 30));   /* ALDO, HPLDO */
	wr32(CODEC + 0x31C, (rd32(CODEC + 0x31C) & ~3u) | 1u);
	wr32(CODEC + 0x004, (1u << 16) | (0xA0u << 8) | 0xA0u);               /* digital vol 0 dB */
	/* 48 kHz, 16-bit at LSB, stereo, TX trigger 15, DRQ on */
	wr32(CODEC + 0x010, (0u << 29) | (1u << 24) | (0xFu << 8) | (1u << 4));
	wr32(CODEC + 0x000, 1u << 31);
	wr32(CODEC + 0x310, (rd32(CODEC + 0x310) & ~((1u << 12) | (1u << 10) | 0x1Fu))
	                    | (1u << 15) | (1u << 14) | (1u << 13) | (1u << 11) | 0x1Au);
	wr32(CODEC + 0x340, (rd32(CODEC + 0x340) & ~(7u << 28)) | (1u << 21) | (1u << 20));
	wr32(CODEC + 0x010, rd32(CODEC + 0x010) | 1u);                         /* flush FIFO */
}

static void dma_start(void)
{
	/* src: SDRAM (drq 1), linear, burst 8, 16-bit; dst: codec (drq 7), IO mode, burst 8, 16-bit */
	uint32_t cfg = 1u | (0u << 8) | (2u << 6) | (1u << 9) | (7u << 16) | (1u << 24) | (2u << 22) | (1u << 25);
	for (int i = 0; i < 2; i++) {
		lli[i].cfg = cfg;
		lli[i].src = (uint32_t)(uintptr_t)&ring[i * RING / 2];
		lli[i].dst = CODEC + 0x020;
		lli[i].len = RING / 2 * 4;
		lli[i].para = 8;                                       /* NORMAL_WAIT */
		lli[i].next = (uint32_t)(uintptr_t)&lli[i ^ 1];
	}
	dcache_flush();
	wr32(DMAC + 0x28, 0x4);                                        /* clock auto-gate (H3+) */
	wr32(CH(0x00), 0);
	wr32(CH(0x08), (uint32_t)(uintptr_t)&lli[0]);
	wr32(CH(0x00), 1);
}

/* Frame the DMA is reading. Around a descriptor switch the current-source register can
   read a stale or in-between value, so take it only when two reads agree and it's in
   the ring; otherwise keep the last good one. */
static uint32_t last_good;
static uint32_t rpos(void)
{
	uint32_t a = rd32(CH(0x10)), b = rd32(CH(0x10));
	uint32_t base = (uint32_t)(uintptr_t)ring;
	if (a != b || a < base || a >= base + RING * 4) return last_good;
	return last_good = (a - base) / 4;
}

int audio_init(void)
{
	if (clocks()) { logf("audio: PLL_AUDIO1 didn't lock\n"); return -1; }
	codec_on();
	for (unsigned i = 0; i < RING; i++) ring[i] = 0;
	dma_start();
	udelay(2000);
	last_r = rpos();
	rabs = last_r;
	wabs = rabs + 48 * 10;                                         /* the ring is silence already */
	last_ms = millis();
	logf("audio: up; DMA ch%d cfg %08x cur src %08x left %u\n", AUD_CH, rd32(CH(0x0C)), rd32(CH(0x10)), rd32(CH(0x18)));
	return 0;
}

static int16_t mix(void)
{
	int32_t s = 0;
	for (int i = 0; i < NV; i++) {
		if (!v[i].left) continue;
		uint32_t done = v[i].total - v[i].left, ramp = 48 * 3;  /* 3 ms fade in/out */
		int32_t env = 256;
		if (done < ramp) env = (int32_t)(done * 256 / ramp);
		if (v[i].left < ramp) env = (int32_t)(v[i].left * 256 / ramp);
		v[i].phase += v[i].step;
		s += sine256[(v[i].phase >> 16) & 255] * v[i].level / 256 * env / 256;
		v[i].left--;
	}
	if (s > 32767) s = 32767;
	if (s < -32768) s = -32768;
	return (int16_t)s;
}

void audio_service(void)
{
	uint32_t now = millis();
	if (now - last_ms > audio_stats.max_gap_ms) audio_stats.max_gap_ms = now - last_ms;
	last_ms = now;
	uint32_t r = rpos(), d = (r - last_r + RING) % RING;
	if (d > RING - RING / 8) d = 0;          /* a small step backwards: a glitchy read, not a lap */
	rabs += d;
	last_r = r;
	if (wabs < rabs + 48) {                                        /* we fell behind: what's left is stale */
		audio_stats.underruns++;
		wabs = rabs + 48 * 5;
	}
	uint64_t target = rabs + LEAD;
	if (target > rabs + RING - 256) target = rabs + RING - 256;
	if (wabs >= target) return;
	while (wabs < target) {
		int16_t s = mix();
		ring[wabs % RING] = (uint16_t)s | ((uint32_t)(uint16_t)s << 16);
		wabs++;
		audio_stats.frames++;
	}
	dcache_flush();
}

void audio_beep(unsigned hz, unsigned ms, int level)
{
	int k = 0;
	for (int i = 0; i < NV; i++) if (!v[i].left) { k = i; break; }
	v[k].phase = 0;
	v[k].step = (uint32_t)(((uint64_t)hz * 256u * 65536u) / 48000u);
	v[k].level = level;
	v[k].total = v[k].left = 48u * ms;
}

void audio_click(void) { audio_beep(2400, 12, 160); }

int audio_busy(void)
{
	for (int i = 0; i < NV; i++) if (v[i].left) return 1;
	return 0;
}
