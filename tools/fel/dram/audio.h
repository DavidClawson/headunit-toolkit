/* audio.h: F133 codec playback over DMA (audio.c). 48 kHz, 16-bit stereo,
   from the headphone driver into BD3702 input 0 (SOUND_IN_F133). */
#pragma once
#include <stdint.h>

#define SOUND_IN_F133 0x00      /* BD3702 input of the F133's own audio (stock armChannelA=0) */

int  audio_init(void);          /* clocks, codec power, DMA ring running (silence); 0 = ok */
void audio_service(void);       /* call often (main loop): tops the ring up ~100 ms ahead */
void audio_beep(unsigned hz, unsigned ms, int level);   /* level 0..256; soft edges */
void audio_click(void);         /* short UI tick */
int  audio_busy(void);          /* any voice still sounding */

typedef struct { uint32_t underruns, frames, max_gap_ms; } audio_stats_t;
extern audio_stats_t audio_stats;
