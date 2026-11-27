/* dmatest.c: DMA playback under a busy main loop. The loop stalls 0..80 ms at random
   (like redraws) and calls audio_service() in between. Plays: a 3 s 1 kHz tone
   (gaps would show as dropouts), a C-E-G-C arpeggio, then 6 UI clicks. */
#include "lib.h"
#include "tuner.h"
#include "audio.h"

static uint32_t rnd = 12345;
static unsigned rand80(void) { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) % 81u; }

static void run(uint32_t ms)
{
	uint32_t t0 = millis();
	while (millis() - t0 < ms) {
		audio_service();
		udelay(rand80() * 1000u);                      /* "redraw" */
	}
}

int main(void)
{
	log_reset();
	logf("DMA audio test\n");
	sound_init();
	snd_vol = 22; snd_muted = 0;
	sound_input(SOUND_IN_F133);
	if (audio_init()) return 1;
	run(300);
	logf("1 kHz 3 s\n");
	audio_beep(1000, 3000, 128); run(3500);
	logf("arpeggio\n");
	static const unsigned notes[4] = { 523, 659, 784, 1047 };
	for (int i = 0; i < 4; i++) { audio_beep(notes[i], 220, 128); run(250); }
	run(500);
	logf("clicks\n");
	for (int i = 0; i < 6; i++) { audio_click(); run(400); }
	run(300);
	logf("done: %u frames, %u underruns, longest loop gap %u ms; DMA cur src %08x\n",
	     audio_stats.frames, audio_stats.underruns, audio_stats.max_gap_ms, rd32(0x03002110));
	return 0;
}
