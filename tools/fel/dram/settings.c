/* settings.c: see settings.h. Sector layout: header {magic, version, length, crc32}
   then the settings_t bytes. Anything that doesn't check out means defaults. */
#include "lib.h"
#include "../bootsel/fwboot.h"
#include "spinor.h"
#include "mcu.h"
#include "settings.h"

#define MAGIC 0x54455348u   /* "HSET" */
#define VERSION 1
#define DELAY_MS 3000

settings_t cfg;
static settings_t saved;
static uint32_t changed_ms;
static int pending;

static const settings_t defaults = { .vol = 25, .source = SRC_RADIO, .led = LED_WHITE, .freq = 10110 };

static uint32_t crc32(const void *d, unsigned n)
{
	const uint8_t *p = d;
	uint32_t c = 0xFFFFFFFFu;
	while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
	return ~c;
}

static int same(const settings_t *a, const settings_t *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (unsigned i = 0; i < sizeof *a; i++) if (x[i] != y[i]) return 0;
	return 1;
}

int settings_load(void)
{
	spinor_init();
	struct { uint32_t magic, version, len, crc; settings_t s; } b;
	cfg = defaults;
	int bad = spinor_read(FW_SETTINGS, &b, sizeof b) || b.magic != MAGIC || b.version != VERSION ||
	          b.len != sizeof b.s || b.crc != crc32(&b.s, sizeof b.s);
	if (!bad) cfg = b.s;
	if (cfg.vol < 1 || cfg.vol > 30) cfg.vol = defaults.vol;
	if (cfg.freq < 8750 || cfg.freq > 10790) cfg.freq = defaults.freq;
	saved = cfg;
	logf("settings: %s (vol %u, source %u, %u.%u MHz)\n", bad ? "defaults" : "loaded",
	     cfg.vol, cfg.source, cfg.freq / 100, cfg.freq % 100 / 10);
	return bad;
}

int settings_tick(void)
{
	if (same(&cfg, &saved)) { pending = 0; return 0; }
	if (!pending) { pending = 1; changed_ms = millis(); return 0; }
	if (millis() - changed_ms < DELAY_MS) return 0;
	struct { uint32_t magic, version, len, crc; settings_t s; } b = { MAGIC, VERSION, sizeof cfg, crc32(&cfg, sizeof cfg), cfg };
	uint32_t t0 = millis();
	int e = spinor_settings_erase();
	if (!e) e = spinor_settings_write(0, &b, sizeof b);
	logf("settings: saved (err %d, %u ms)\n", e, millis() - t0);
	saved = cfg;               /* even on error: don't hammer the flash; it retries on the next change */
	pending = 0;
	return 1;
}
