/* tuner.c: see tuner.h. Si47xx commands per Silicon Labs AN332. */
#include "lib.h"
#include "twi.h"
#include "bbi2c.h"
#include "tuner.h"
#include "settings.h"

#define SI 0x11
#define BD 0x40

tuner_state tuner;
static uint32_t last_rsq, busy_t0;
static unsigned errs;

/* send a command, wait for CTS, read rn response bytes (status first) */
static int si_cmd(const uint8_t *c, unsigned n, uint8_t *resp, unsigned rn)
{
	if (bb_write(SI, c, n)) return 1;
	uint8_t st[16];
	uint32_t t0 = ticks();
	for (;;) {
		if (bb_read(SI, st, rn ? rn : 1)) return 1;
		if (st[0] & 0x80) break;
		if (elapsed_ms(t0) > 500) return 2;
		udelay(200);
	}
	for (unsigned i = 0; i < rn; i++) resp[i] = st[i];
	return 0;
}

static int si_prop(uint16_t p, uint16_t v)
{
	uint8_t c[6] = { 0x12, 0, (uint8_t)(p >> 8), (uint8_t)p, (uint8_t)(v >> 8), (uint8_t)v };
	return si_cmd(c, 6, 0, 0);
}

/* the stock app's property list (init.axf), then our overrides */
static const uint16_t props[][2] = {
	{0x0001,0x0004},{0x0102,0x0000},{0x0104,0x0000},{0x0201,0x8000},{0x0202,0x0001},
	{0x1100,0x0001},{0x1105,0x0014},{0x1106,0x0014},{0x1107,0x0000},{0x1108,0x001E},
	{0x1200,0x008F},{0x1201,0x001E},{0x1202,0x0006},{0x1203,0x0032},{0x1204,0x0018},
	{0x1205,0x0000},{0x1206,0x0000},{0x1207,0x00B2},{0x1300,0x0040},{0x1302,0x0020},
	{0x1303,0x0002},{0x1400,0x222E},{0x1401,0x2A30},{0x1402,0x000A},{0x1403,0x0005},
	{0x1404,0x0005},{0x1500,0x0000},{0x1501,0x000E},{0x1502,0x0001},{0x4000,0x003F},
	{0x4001,0x0000},{0x1800,0x000F},{0x1801,0x000A},
	/* overrides: 75 us de-emphasis (Americas), band 87.5-107.9 in 200 kHz steps,
	   seek stops only on RSSI >= 20 dBuV and SNR >= 6 dB */
	{0x1100,0x0002},{0x1400,FM_MIN},{0x1401,FM_MAX},{0x1402,FM_STEP},{0x1403,6},{0x1404,20},
};

int tuner_init(void)
{
	tuner.ok = 0;
	wr32(0x02000030, (rd32(0x02000030) & ~(0xFu << 20)) | (1u << 20));   /* PB5 out: reset */
	wr32(0x02000040, rd32(0x02000040) & ~(1u << 5));
	udelay(10000);
	wr32(0x02000040, rd32(0x02000040) | (1u << 5));
	udelay(10000);
	bb_init();
	static const uint8_t pu[3] = { 0x01, 0xD0, 0x05 };   /* FM, analog out, xtal */
	if (si_cmd(pu, 3, 0, 0)) return 1;
	udelay(200000);                                        /* crystal settles */
	static const uint8_t rev[1] = { 0x10 };
	uint8_t r[9];
	if (si_cmd(rev, 1, r, 9)) return 2;
	tuner.part = r[1]; tuner.fw_major = r[2]; tuner.fw_minor = r[3]; tuner.rev = r[8];
	for (unsigned i = 0; i < sizeof props / sizeof props[0]; i++)
		if (si_prop(props[i][0], props[i][1])) return 3;
	tuner.ok = 1;
	return 0;
}

void tuner_tune(uint16_t f)
{
	if (!tuner.ok) return;
	if (f < FM_MIN) f = FM_MAX;
	if (f > FM_MAX) f = FM_MIN;
	uint8_t c[5] = { 0x20, 0x00, (uint8_t)(f >> 8), (uint8_t)f, 0x00 };
	if (si_cmd(c, 5, 0, 0) == 0) { tuner.busy = 1; busy_t0 = ticks(); tuner.freq = f; tuner.valid = 0; }
}

void tuner_seek(int up, int wrap)
{
	if (!tuner.ok) return;
	uint8_t c[2] = { 0x21, (uint8_t)((up ? 0x08 : 0) | (wrap ? 0x04 : 0)) };
	if (si_cmd(c, 2, 0, 0) == 0) { tuner.busy = 1; busy_t0 = ticks(); tuner.valid = 0; tuner.band_limit = 0; }
}

static int set8(uint8_t *a, uint8_t v) { if (*a == v) return 0; *a = v; return 1; }

int tuner_poll(void)
{
	if (!tuner.ok || elapsed_ms(last_rsq) < (tuner.busy ? 60u : 300u)) return 0;
	last_rsq = ticks();
	int ch = 0;
	if (tuner.busy) {
		static const uint8_t is[1] = { 0x14 };
		uint8_t s[1];
		int e = si_cmd(is, 1, s, 1);
		if (e) { if (errs++ < 20) logf("tuner: GET_INT_STATUS error %d\n", e); return 0; }
		int done = s[0] & 1, stuck = !done && elapsed_ms(busy_t0) > 10000;
		/* INTACK once finished; CANCEL + INTACK if a tune/seek runs past 10 s */
		uint8_t ts[2] = { 0x22, (uint8_t)(done ? 1 : stuck ? 3 : 0) };
		uint8_t t[8];
		e = si_cmd(ts, 2, t, 8);
		if (e) { if (errs++ < 20) logf("tuner: FM_TUNE_STATUS error %d\n", e); return 0; }
		if (stuck) {
			logf("tuner: no STC after 10 s at %u (status %02x %02x): cancelled\n",
			     (unsigned)(t[2] << 8 | t[3]), s[0], t[1]);
			done = 1;
		}
		uint16_t f = (uint16_t)(t[2] << 8 | t[3]);
		if (f != tuner.freq) { tuner.freq = f; ch = 1; }
		if (!done) return ch;
		tuner.busy = 0;
		tuner.band_limit = t[1] >> 7;
		tuner.valid = t[1] & 1;
		tuner.rssi = t[4]; tuner.snr = t[5];
		return 2;
	}
	static const uint8_t rsq[2] = { 0x23, 0x00 };
	uint8_t q[8];
	if (si_cmd(rsq, 2, q, 8)) return 0;
	ch |= set8(&tuner.valid, q[2] & 1);
	ch |= set8(&tuner.stereo, q[3] & 0x7F);
	ch |= set8(&tuner.rssi, q[4]);
	ch |= set8(&tuner.snr, q[5]);
	return ch;
}

static const uint8_t bd_init[][2] = {
	{0x01,0xF4},{0x02,0x03},{0x03,0x19},{0x05,0x00},{0x06,0x06},{0x20,0x00},
	{0x28,0x80},{0x29,0x80},{0x2A,0x80},{0x2B,0x80},{0x2C,0xFF},{0x30,0xFF},
	{0x41,0x00},{0x44,0x00},{0x47,0x00},{0x51,0x00},{0x54,0x00},{0x57,0x00},
	{0x75,0x7F},{0x90,0x30},
};

static void bd(uint8_t reg, uint8_t v) { twi_write(2, BD, reg, 1, &v, 1); }

int snd_vol = 25, snd_muted;
uint8_t snd_input = SOUND_IN_RADIO;
static int snd_ready;

void sound_init(void)
{
	if (snd_ready) return;
	snd_ready = 1;
	twi_init(2);
	bd(0x20, 0xFF);                                       /* muted while setting up */
	for (unsigned i = 0; i < sizeof bd_init / sizeof bd_init[0]; i++)
		if (bd_init[i][0] != 0x20) bd(bd_init[i][0], bd_init[i][1]);
	bd(0x05, SOUND_IN_RADIO);
	sound_apply();
}

void sound_input(uint8_t in) { sound_init(); snd_input = in; bd(0x05, in); }

void sound_step(int dir)
{
	snd_vol += dir;
	if (snd_vol < 1) snd_vol = 1;
	if (snd_vol > SOUND_MAX) snd_vol = SOUND_MAX;
	snd_muted = 0;
	sound_apply();
}

/* BD3702 volume register 0x20: 0x80 = 0 dB, each step above = -1 dB, 0xFF = mute.
   Level 30 = 0 dB, each level down = -2 dB. */
void sound_apply(void)
{
	cfg.vol = (uint8_t)snd_vol; cfg.muted = (uint8_t)snd_muted;   /* saved by settings.c */
	int level = snd_muted ? 0 : snd_vol;
	if (level <= 0) { bd(0x20, 0xFF); return; }
	if (level > SOUND_MAX) level = SOUND_MAX;
	bd(0x20, (uint8_t)(0x80 + 2 * (SOUND_MAX - level)));
}
