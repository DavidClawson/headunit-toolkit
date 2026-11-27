/*
 * audtest.c: first sound from the F133's own audio codec.
 * The F133 has one analog output, the headphone driver (HPOUTL/R); stock routes
 * "ARM"/media audio to BD3702 input 0 (Config.ini armChannelA=0, mediaChannelA=0).
 * Registers follow the D1 codec driver (smaeul/linux sun20i-codec.c) and the CCU
 * driver: DAC clock = PLL_AUDIO1 (3072 MHz) / 5 / 25 = 24.576 MHz, FS /512 = 48 kHz.
 * Plays: 440 Hz 3 s, 1 kHz 3 s, a 200 Hz -> 4 kHz sweep 4 s, then 5 short clicks.
 * The line-out path is enabled too, in case the board uses it.
 */
#include "lib.h"
#include "tuner.h"
#include "sine256.h"

#define CCU    0x02001000u
#define CODEC  0x02030000u
#define DAC_DPC   (CODEC + 0x000)
#define DAC_VOL   (CODEC + 0x004)
#define DAC_FIFOC (CODEC + 0x010)
#define DAC_FIFOS (CODEC + 0x014)
#define DAC_TX    (CODEC + 0x020)
#define A_DAC     (CODEC + 0x310)
#define A_RAMP    (CODEC + 0x31C)
#define A_HP2     (CODEC + 0x340)
#define A_POWER   (CODEC + 0x348)

static void dump(const char *when)
{
	logf("%s: PLL_A1 %08x DACCLK %08x BGR %08x | DPC %08x VOL %08x FIFOC %08x FIFOS %08x | DAC %08x RAMP %08x HP2 %08x POWER %08x\n",
	     when, rd32(CCU + 0x080), rd32(CCU + 0xA50), rd32(CCU + 0xA5C), rd32(DAC_DPC), rd32(DAC_VOL),
	     rd32(DAC_FIFOC), rd32(DAC_FIFOS), rd32(A_DAC), rd32(A_RAMP), rd32(A_HP2), rd32(A_POWER));
}

static int codec_up(void)
{
	dump("before");
	/* PLL_AUDIO1: N = 128 -> 3072 MHz; div2 output /2, div5 output /5 */
	uint32_t pll = (1u << 31) | (1u << 30) | (1u << 29) | (127u << 8) | (4u << 20) | (1u << 16);
	wr32(CCU + 0x080, pll);
	uint32_t t0 = millis();
	while (!(rd32(CCU + 0x080) & (1u << 28)))
		if (millis() - t0 > 50) { logf("PLL_AUDIO1 didn't lock\n"); return -1; }
	wr32(CCU + 0x080, pll | (1u << 27));                           /* output gate */
	wr32(CCU + 0xA50, (1u << 31) | (2u << 24) | 24u);              /* pll-audio1-div5 / 25 = 24.576 MHz */
	wr32(CCU + 0xA5C, rd32(CCU + 0xA5C) | 1u | (1u << 16));        /* codec bus gate, out of reset */
	udelay(100);

	wr32(A_POWER, rd32(A_POWER) | (1u << 31) | (1u << 30));         /* ALDO (avcc), HPLDO (hpvcc) on */
	wr32(A_RAMP, (rd32(A_RAMP) & ~3u) | 1u);                       /* the driver's "magic" */
	wr32(DAC_VOL, (1u << 16) | (0xA0u << 8) | 0xA0u);              /* digital volume on, 0 dB */
	wr32(DAC_FIFOC, (0u << 29) | (1u << 24) | (0xFu << 8));        /* 48 kHz, 16-bit at LSB, stereo */
	wr32(DAC_DPC, 1u << 31);                                       /* DAC digital on, 0 dB */
	wr32(A_DAC, (rd32(A_DAC) & ~((1u << 12) | (1u << 10) | 0x1Fu)) /* unmute line out, vol 0x1a */
	              | (1u << 15) | (1u << 14) | (1u << 13) | (1u << 11) | 0x1Au);
	wr32(A_HP2, (rd32(A_HP2) & ~(7u << 28)) | (1u << 21) | (1u << 20));   /* HP driver on, out on, 0 dB */
	wr32(DAC_FIFOC, rd32(DAC_FIFOC) | 1u);                         /* flush */
	udelay(1000);
	dump("after");
	return 0;
}

static unsigned fifo_free(void) { return (rd32(DAC_FIFOS) >> 8) & 0x7FFFu; }

/* play n stereo frames, phase step in 1/65536 of a table entry; level 0..256 */
static uint32_t phase;
static unsigned long frames_out;
static int play(unsigned n, uint32_t step0, uint32_t step1, int level)
{
	uint32_t t0 = millis();
	for (unsigned i = 0; i < n; i++) {
		while (fifo_free() < 2)
			if (millis() - t0 > n / 48 + 500) { logf("FIFO stuck: FIFOS %08x\n", rd32(DAC_FIFOS)); return -1; }
		uint32_t step = step0 + (uint32_t)(((uint64_t)(step1 - step0) * i) / n);
		phase += step;
		int s = sine256[(phase >> 16) & 255] * level / 256;
		wr32(DAC_TX, (uint16_t)s);
		wr32(DAC_TX, (uint16_t)s);
		frames_out++;
	}
	return 0;
}
static uint32_t step_for(unsigned hz) { return (uint32_t)(((uint64_t)hz * 256u * 65536u) / 48000u); }

int main(void)
{
	log_reset();
	logf("audio codec test\n");
	sound_init();
	snd_vol = 22; snd_muted = 0;
	sound_input(0x00);                       /* BD3702 input 0: the F133 */
	if (codec_up()) return 1;
	uint32_t t0 = millis();
	logf("440 Hz\n");   if (play(3 * 48000, step_for(440), step_for(440), 128)) goto out;
	logf("1 kHz\n");    if (play(3 * 48000, step_for(1000), step_for(1000), 128)) goto out;
	logf("sweep\n");    if (play(4 * 48000, step_for(200), step_for(4000), 128)) goto out;
	logf("clicks\n");
	for (int k = 0; k < 5; k++) {
		if (play(48000 * 25 / 1000, step_for(2000), step_for(2000), 200)) goto out;   /* 25 ms */
		if (play(48000 * 475 / 1000, 0, 0, 0)) goto out;                              /* gap */
	}
	logf("done: %lu frames in %u ms (expect %u)\n", frames_out, millis() - t0, (unsigned)(frames_out / 48));
out:
	dump("end");
	return 0;
}
