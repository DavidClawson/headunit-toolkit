/* mixtest.c: does the BD3702 mix the F133 (input 0) over the radio?
   Radio plays (input 0x0B); the F133 holds a 1.5 kHz tone the whole time.
   Every 2 s a new mixer setting: reg 0x03 (mixing setup) x reg 0x30 (mixing gain).
   Step 0 and the last step are the stock "off" (0x03 19, 0x30 FF). */
#include "lib.h"
#include "tuner.h"
#include "twi.h"
#include "audio.h"

static void bd(uint8_t reg, uint8_t v) { twi_write(2, 0x40, reg, 1, &v, 1); }

static const struct { uint8_t r03, r30; } step[] = {
	{ 0x19, 0xFF },                                   /* stock: mix muted */
	{ 0x19, 0x80 }, { 0x19, 0x00 },
	{ 0x09, 0x80 }, { 0x00, 0x80 }, { 0x01, 0x80 }, { 0x08, 0x80 },
	{ 0x10, 0x80 }, { 0x18, 0x80 }, { 0x1A, 0x80 }, { 0x1B, 0x80 },
	{ 0x39, 0x80 }, { 0x59, 0x80 }, { 0x99, 0x80 }, { 0xF9, 0x80 },
	{ 0x19, 0xFF },
};

int main(void)
{
	log_reset();
	logf("mix test\n");
	sound_init();
	snd_vol = 22; snd_muted = 0;
	if (tuner_init()) logf("tuner init failed\n");
	tuner_tune(10030);
	sound_input(0x0B);                                /* radio */
	if (audio_init()) return 1;
	uint32_t t = millis();
	while (millis() - t < 2500) audio_service();      /* past the HP ramp */
	audio_beep(1500, 60000, 64);                      /* the tone, all along */
	uint32_t t0 = millis();
	for (unsigned i = 0; i < sizeof step / sizeof step[0]; i++) {
		bd(0x03, step[i].r03);
		bd(0x30, step[i].r30);
		logf("step %2u at %5u ms: 03=%02x 30=%02x\n", i, millis() - t0, step[i].r03, step[i].r30);
		t = millis();
		while (millis() - t < 2000) audio_service();
	}
	logf("done, %u underruns\n", audio_stats.underruns);
	return 0;
}
