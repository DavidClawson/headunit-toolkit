/*
 * snd_f133.c: Doom's sound effects through audio.c's PCM voices.
 * Sound lumps (DSxxxx) are DMX format: u16 format (3), u16 sample rate, u32 count,
 * then the 8-bit unsigned samples with 16 bytes of padding at each end.
 * Music (MUS -> OPL synthesis) isn't implemented: the music module is a silent stub.
 */
#include <stdio.h>
#include <string.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "audio.h"

static boolean use_prefix;

static boolean snd_init(boolean prefix) { use_prefix = prefix; return true; }
static void snd_shutdown(void) { }
static int snd_lump(sfxinfo_t *s)
{
	char name[16];
	if (s->link) s = s->link;
	snprintf(name, sizeof name, use_prefix ? "ds%s" : "%s", s->name);
	return W_GetNumForName(name);
}
static void snd_update(void) { }
static void snd_params(int ch, int vol, int sep) { audio_pcm_params(ch, vol, sep); }
static int snd_start(sfxinfo_t *s, int ch, int vol, int sep)
{
	if (s->lumpnum < 0) s->lumpnum = snd_lump(s);
	const uint8_t *d = W_CacheLumpNum(s->lumpnum, PU_STATIC);
	int n = W_LumpLength(s->lumpnum);
	if (n < 8 + 32 || d[0] != 3 || d[1] != 0) return -1;
	uint32_t rate = d[2] | d[3] << 8, count = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
	if (count > (uint32_t)n - 8) count = (uint32_t)n - 8;
	if (count <= 32) return -1;
	audio_pcm_start(ch, d + 8 + 16, count - 32, rate, vol, sep);
	return ch;
}
static void snd_stop(int ch) { audio_pcm_stop(ch); }
static boolean snd_playing(int ch) { return audio_pcm_playing(ch) ? true : false; }
static void snd_cache(sfxinfo_t *s, int n) { (void)s; (void)n; }

static snddevice_t devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                 SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module = {
	devices, sizeof devices / sizeof devices[0],
	snd_init, snd_shutdown, snd_lump, snd_update, snd_params, snd_start, snd_stop, snd_playing, snd_cache,
};

/* music: silent */
static boolean m_init(void) { return true; }
static void m_none(void) { }
static void m_vol(int v) { (void)v; }
static void *m_reg(void *d, int n) { (void)d; (void)n; return (void *)1; }
static void m_unreg(void *h) { (void)h; }
static void m_play(void *h, boolean loop) { (void)h; (void)loop; }
static boolean m_playing(void) { return false; }
music_module_t DG_music_module = {
	devices, sizeof devices / sizeof devices[0],
	m_init, m_none, m_vol, m_none, m_none, m_reg, m_unreg, m_play, m_none, m_playing, m_none,
};

/* referenced by m_config.c / i_sound.c when FEATURE_SOUND is on (SDL-side settings) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *timidity_cfg_path = "";
void I_InitTimidityConfig(void) { }
