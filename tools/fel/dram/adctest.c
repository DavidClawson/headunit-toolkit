/* adctest.c: is any audio wired into the F133's codec inputs (FMIN L/R, LINEIN L/R)?
   Radio plays (BD3702 input 0x0B). For each input setting, sample 2 s from ADC1/ADC2
   (48 kHz, 16-bit) and log RMS / peak per channel. Then the same with the BD3702
   muted: a level that stays is pre-volume (straight from the tuner), one that drops
   follows the BD3702 output. Read-only: nothing is written to flash. */
#include "lib.h"
#include "tuner.h"
#include "audio.h"

#define CODEC 0x02030000u
#define CCU   0x02001000u

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0, b = 1u << 30;
	while (b > v) b >>= 2;
	while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
	return r;
}

static void capture(const char *what, uint32_t a1, uint32_t a2)
{
	wr32(CODEC + 0x300, a1);
	wr32(CODEC + 0x304, a2);
	wr32(CODEC + 0x030, rd32(CODEC + 0x030) | 1u);               /* flush */
	udelay(200000);                                              /* settle */
	wr32(CODEC + 0x030, rd32(CODEC + 0x030) | 1u);
	uint64_t sq[2] = { 0, 0 };
	int32_t pk[2] = { 0, 0 };
	uint32_t n = 0, polls = 0, maxcnt = 0, fifos0 = rd32(CODEC + 0x038);
	uint32_t t0 = millis();
	while (millis() - t0 < 2000) {
		uint32_t st = rd32(CODEC + 0x038), cnt = (st >> 8) & 0xFF;
		polls++;
		if (cnt > maxcnt) maxcnt = cnt;
		while (cnt--) {
			int16_t v = (int16_t)rd32(CODEC + 0x040);
			int c = n & 1;
			sq[c] += (uint64_t)((int32_t)v * v);
			if (v > pk[c]) pk[c] = v;
			if (-v > pk[c]) pk[c] = -v;
			n++;
		}
	}
	uint32_t per = n / 2 ? n / 2 : 1;
	logf("  %-22s samples %u (FIFOS %x, max count %u): L rms %u peak %u | R rms %u peak %u\n", what, n, fifos0, maxcnt,
	     isqrt((uint32_t)(sq[0] / per)), pk[0], isqrt((uint32_t)(sq[1] / per)), pk[1]);
}

int main(void)
{
	log_reset();
	logf("codec input test\n");
	sound_init();
	snd_vol = 20; snd_muted = 0; sound_apply();
	if (tuner_init()) logf("tuner init failed\n");
	tuner_tune(10030);
	sound_input(0x0B);                                            /* radio */
	if (audio_init()) return 1;                                   /* clocks, analog power */
	wr32(CCU + 0xA54, (1u << 31) | (2u << 24) | 24u);              /* ADC clock 24.576 MHz */
	wr32(CODEC + 0x034, (0xA0u << 16) | (0xA0u << 8) | 0xA0u);     /* ADC digital vol 0 dB */
	wr32(CODEC + 0x050, (3u << 16) | 3u);                          /* vol on; ADC1+ADC2 */
	wr32(CODEC + 0x030, (0u << 29) | (1u << 28) | (1u << 24) | (0xFu << 4));   /* 48 kHz, on, 16-bit LSB */
	udelay(300000);
	const uint32_t EN = 1u << 31, FM = 1u << 27, LI = 1u << 23;
	for (int pass = 0; pass < 2; pass++) {
		logf(pass ? "BD3702 muted:\n" : "radio playing (vol 20):\n");
		capture("ADCs on, no input", EN, EN);
		capture("FMIN L/R", EN | FM, EN | FM);
		capture("LINEIN L/R", EN | LI, EN | LI);
		capture("FMIN+LINEIN, +6 dB", EN | FM | LI | (1u << 26) | (1u << 22), EN | FM | LI | (1u << 26) | (1u << 22));
		snd_muted = 1; sound_apply();
	}
	snd_muted = 0; sound_apply();
	logf("done\n");
	return 0;
}
