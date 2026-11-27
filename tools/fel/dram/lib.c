#include "lib.h"
#include <stdarg.h>

extern char __log[], __log_size[];
struct log { uint32_t magic, len; char buf[]; };
#define LOG ((volatile struct log *)__log)
#define LOG_CAP ((uint32_t)(uintptr_t)__log_size - 8)

uint64_t cpu_hz(void)
{
	uint32_t pll = rd32(0x02001000), clk = rd32(0x02001D00);
	uint64_t hz = 24000000ull * (((pll >> 8) & 0xff) + 1) / ((pll & 3) + 1);
	if (((clk >> 24) & 7) != 5)          /* not running from PLL_CPU: assume 24 MHz HOSC */
		return 24000000ull;
	return hz / ((clk & 0x1f) + 1);
}

#define TMR1_CTRL 0x02050020u
#define TMR1_INTV 0x02050024u
#define TMR1_CUR  0x02050028u

void lib_init(void)
{
	wr32(TMR1_CTRL, 0);
	wr32(TMR1_INTV, 0xFFFFFFFFu);
	wr32(TMR1_CTRL, (1u << 2) | (1u << 1));        /* 24 MHz HOSC, /1, reload */
	wr32(TMR1_CTRL, (1u << 2) | (1u << 1) | 1u);   /* enable */
}

uint32_t ticks(void) { return ~rd32(TMR1_CUR); }  /* down-counter -> up */

/* milliseconds since start, wrap-free for 49 days; needs a call at least every ~178 s */
uint32_t millis(void)
{
	static uint32_t last, frac, ms;
	uint32_t t = ticks();
	frac += t - last; last = t;
	ms += frac / (TICKS_PER_US * 1000u); frac %= TICKS_PER_US * 1000u;
	return ms;
}

void udelay(uint32_t us)
{
	uint32_t t0 = ticks();
	while (ticks() - t0 < us * TICKS_PER_US)       /* us < ~178 s */
		;
}

void log_reset(void) { LOG->magic = 0x474F4C46; LOG->len = 0; }   /* "FLOG" */
void log_putc(char c) { if (LOG->len < LOG_CAP) LOG->buf[LOG->len++] = c; }
void log_puts(const char *s) { while (*s) log_putc(*s++); }

static void put_num(uint64_t v, unsigned base, int width, char pad, int neg)
{
	char tmp[24]; int n = 0;
	do { unsigned d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
	if (neg) tmp[n++] = '-';
	while (n < width) tmp[n++] = pad;
	while (n) log_putc(tmp[--n]);
}

void logf(const char *f, ...)
{
	va_list ap; va_start(ap, f);
	for (; *f; f++) {
		if (*f != '%') { log_putc(*f); continue; }
		char pad = ' '; int width = 0, lng = 0;
		f++;
		if (*f == '0') { pad = '0'; f++; }
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		while (*f == 'l') { lng = 1; f++; }
		switch (*f) {
		case 's': log_puts(va_arg(ap, const char *)); break;
		case 'c': log_putc((char)va_arg(ap, int)); break;
		case 'd': { int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			put_num(v < 0 ? -v : v, 10, width, pad, v < 0); break; }
		case 'u': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, pad, 0); break;
		case 'x': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, width, pad, 0); break;
		case 'p': log_puts("0x"); put_num((uintptr_t)va_arg(ap, void *), 16, 0, ' ', 0); break;
		case '%': log_putc('%'); break;
		default: log_putc('?');
		}
	}
	va_end(ap);
}

/* 8 bytes at a time once both pointers are 8-aligned (Doom and the blits lean on these) */
void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n && ((uintptr_t)p & 7)) { *p++ = (uint8_t)c; n--; }
	uint64_t w = 0x0101010101010101ull * (uint8_t)c;
	for (; n >= 32; n -= 32, p += 32) {
		((uint64_t *)p)[0] = w; ((uint64_t *)p)[1] = w; ((uint64_t *)p)[2] = w; ((uint64_t *)p)[3] = w;
	}
	for (; n >= 8; n -= 8, p += 8) *(uint64_t *)p = w;
	while (n--) *p++ = (uint8_t)c;
	return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *p = d; const uint8_t *q = s;
	if ((((uintptr_t)p ^ (uintptr_t)q) & 7) == 0) {
		while (n && ((uintptr_t)p & 7)) { *p++ = *q++; n--; }
		for (; n >= 32; n -= 32, p += 32, q += 32) {
			uint64_t a = ((const uint64_t *)q)[0], b = ((const uint64_t *)q)[1];
			uint64_t c = ((const uint64_t *)q)[2], e = ((const uint64_t *)q)[3];
			((uint64_t *)p)[0] = a; ((uint64_t *)p)[1] = b; ((uint64_t *)p)[2] = c; ((uint64_t *)p)[3] = e;
		}
		for (; n >= 8; n -= 8, p += 8, q += 8) *(uint64_t *)p = *(const uint64_t *)q;
	} else if ((((uintptr_t)p ^ (uintptr_t)q) & 3) == 0) {
		while (n && ((uintptr_t)p & 3)) { *p++ = *q++; n--; }
		for (; n >= 4; n -= 4, p += 4, q += 4) *(uint32_t *)p = *(const uint32_t *)q;
	}
	while (n--) *p++ = *q++;
	return d;
}
