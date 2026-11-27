/*
 * libc.c: just enough C library for doomgeneric on bare metal.
 *   memory: a bump allocator over HEAP (32 MB above the framebuffers); free() is a no-op
 *           (Doom allocates its zone once and very little else)
 *   files:  "doom1.wad" reads from the WAD that felrun --load put at WAD_ADDR; anything
 *           opened for writing (config, savegames) is a sink; everything else is missing
 *   output: printf/fprintf go to our log (lib.h logf)
 *   exit(): jumps back to doom_main's setjmp point so the program returns to FEL
 * memset/memcpy come from ../dram/lib.c.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "lib.h"
#include "libc_f133.h"

int errno;

/* ---- memory ---- */
#define HEAP      0x41800000u
#define HEAP_SIZE 0x02000000u
static uintptr_t brk_ = HEAP;
void *malloc(size_t n)
{
	uintptr_t p = (brk_ + 15) & ~(uintptr_t)15;
	if (p + 16 + n > HEAP + HEAP_SIZE) { logf("libc: out of memory (%lu)\n", (unsigned long)n); return 0; }
	*(size_t *)p = n;
	brk_ = p + 16 + n;
	return (void *)(p + 16);
}
void free(void *p) { (void)p; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *p, size_t n)
{
	if (!p) return malloc(n);
	size_t old = *(size_t *)((uintptr_t)p - 16);
	if (n <= old) return p;
	void *q = malloc(n);
	if (q) memcpy(q, p, old);
	return q;
}
size_t libc_heap_used(void) { return brk_ - HEAP; }

/* ---- strings ---- */
void *memmove(void *d, const void *s, size_t n)
{
	uint8_t *dp = d; const uint8_t *sp = s;
	if (dp < sp) while (n--) *dp++ = *sp++;
	else { dp += n; sp += n; while (n--) *--dp = *--sp; }
	return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *p = a, *q = b;
	for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
	return 0;
}
void *memchr(const void *s, int c, size_t n)
{
	const uint8_t *p = s;
	for (; n; n--, p++) if (*p == (uint8_t)c) return (void *)p;
	return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) d[i] = s[i];
	for (; i < n; i++) d[i] = 0;
	return d;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n)
{
	char *e = d + strlen(d);
	while (n-- && *s) *e++ = *s++;
	*e = 0;
	return d;
}
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) { if (*a != *b) return (uint8_t)*a - (uint8_t)*b; if (!*a) return 0; }
	return 0;
}
int strcasecmp(const char *a, const char *b)
{
	while (*a && tolower(*a) == tolower(*b)) a++, b++;
	return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
		if (x != y) return x - y;
		if (!x) return 0;
	}
	return 0;
}
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n)
{
	size_t k = strlen(n);
	for (; *h; h++) if (!strncmp(h, n, k)) return (char *)h;
	return k ? 0 : (char *)h;
}
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
char *strerror(int e) { (void)e; return "error"; }

/* ---- numbers ---- */
unsigned long strtoul(const char *s, char **end, int base)
{
	while (isspace((uint8_t)*s)) s++;
	if (*s == '+') s++;
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
	else if (base == 0 && s[0] == '0') base = 8;
	else if (base == 0) base = 10;
	unsigned long v = 0;
	for (;; s++) {
		int d = isdigit((uint8_t)*s) ? *s - '0' : isalpha((uint8_t)*s) ? (*s | 32) - 'a' + 10 : 99;
		if (d >= base) break;
		v = v * (unsigned)base + (unsigned)d;
	}
	if (end) *end = (char *)s;
	return v;
}
long strtol(const char *s, char **end, int base)
{
	while (isspace((uint8_t)*s)) s++;
	int neg = *s == '-';
	if (neg || *s == '+') s++;
	long v = (long)strtoul(s, end, base);
	return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
double strtod(const char *s, char **end)
{
	while (isspace((uint8_t)*s)) s++;
	int neg = *s == '-';
	if (neg || *s == '+') s++;
	double v = 0, f = 1;
	while (isdigit((uint8_t)*s)) v = v * 10 + (*s++ - '0');
	if (*s == '.') { s++; while (isdigit((uint8_t)*s)) { f /= 10; v += (*s++ - '0') * f; } }
	if (end) *end = (char *)s;
	return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, 0); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
static unsigned long rnd = 1;
int rand(void) { rnd = rnd * 6364136223846793005ul + 1; return (int)(rnd >> 33); }
void srand(unsigned s) { rnd = s; }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	uint8_t *b = base, tmp[256];
	if (sz > sizeof tmp) return;
	for (size_t i = 1; i < n; i++)                         /* insertion sort: Doom sorts little */
		for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) {
			memcpy(tmp, b + j * sz, sz); memcpy(b + j * sz, b + (j - 1) * sz, sz); memcpy(b + (j - 1) * sz, tmp, sz);
		}
}

/* ---- math (only what Doom references) ---- */
double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }
double sqrt(double x) { double r; __asm__("fsqrt.d %0, %1" : "=f"(r) : "f"(x)); return r; }
double floor(double x) { long i = (long)x; return (double)(i - (x < i)); }
double ceil(double x) { long i = (long)x; return (double)(i + (x > i)); }
double pow(double x, double y)                         /* integer powers only */
{
	double r = 1; long n = (long)y; int neg = n < 0;
	if (neg) n = -n;
	while (n--) r *= x;
	return neg ? 1 / r : r;
}

/* ---- printf ---- */
typedef struct { char *p; size_t left; size_t n; } sink;
static void put(sink *s, char c) { if (s->left > 1) { *s->p++ = c; s->left--; } s->n++; }
static void puts_(sink *s, const char *t, int width, int left, int prec)
{
	int len = 0;
	while (t[len] && (prec < 0 || len < prec)) len++;
	if (!left) for (int i = len; i < width; i++) put(s, ' ');
	for (int i = 0; i < len; i++) put(s, t[i]);
	if (left) for (int i = len; i < width; i++) put(s, ' ');
}
int vsnprintf(char *buf, size_t size, const char *f, va_list ap)
{
	sink s = { buf, size, 0 };
	for (; *f; f++) {
		if (*f != '%') { put(&s, *f); continue; }
		f++;
		int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
		for (;; f++) {
			if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1;
			else if (*f == ' ' || *f == '#') ; else break;
		}
		if (*f == '*') { width = va_arg(ap, int); f++; } else while (isdigit((uint8_t)*f)) width = width * 10 + (*f++ - '0');
		if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (isdigit((uint8_t)*f)) prec = prec * 10 + (*f++ - '0'); }
		while (*f == 'l' || *f == 'z' || *f == 'h') { if (*f != 'h') lng++; f++; }
		char tmp[40], *e = tmp + sizeof tmp, *p = e;
		*--p = 0;
		switch (*f) {
		case 'd': case 'i': {
			long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
			unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
			do *--p = (char)('0' + u % 10); while (u /= 10);
			while (prec > 0 && (e - 1 - p) < prec) *--p = '0';
			if (zero && !left) while ((e - 1 - p) < width - (v < 0 || plus)) *--p = '0';
			if (v < 0) *--p = '-'; else if (plus) *--p = '+';
			puts_(&s, p, width, left, -1);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': case 'p': {
			unsigned long long u = *f == 'p' ? (uintptr_t)va_arg(ap, void *) : lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			unsigned b = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
			const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			do *--p = dig[u % b]; while (u /= b);
			while (prec > 0 && (e - 1 - p) < prec) *--p = '0';
			if (zero && !left) while ((e - 1 - p) < width) *--p = '0';
			puts_(&s, p, width, left, -1);
			break;
		}
		case 'f': case 'g': case 'e': {
			double v = va_arg(ap, double);
			if (prec < 0) prec = 6;
			if (prec > 9) prec = 9;
			int neg = v < 0;
			if (neg) v = -v;
			unsigned long long ip = (unsigned long long)v;
			double fr = v - (double)ip;
			for (int i = 0; i < prec; i++) fr *= 10;
			unsigned long long fp = (unsigned long long)(fr + 0.5);
			if (prec) { for (int i = 0; i < prec; i++) { *--p = (char)('0' + fp % 10); fp /= 10; } *--p = '.'; }
			ip += fp;                                   /* rounding carried over */
			do *--p = (char)('0' + ip % 10); while (ip /= 10);
			if (neg) *--p = '-';
			puts_(&s, p, width, left, -1);
			break;
		}
		case 'c': tmp[0] = (char)va_arg(ap, int); tmp[1] = 0; puts_(&s, tmp, width, left, -1); break;
		case 's': { const char *t = va_arg(ap, const char *); puts_(&s, t ? t : "(null)", width, left, prec); break; }
		case '%': put(&s, '%'); break;
		default: put(&s, '%'); put(&s, *f); break;
		}
	}
	if (size) *s.p = 0;
	return (int)s.n;
}
int vsprintf(char *b, const char *f, va_list ap) { return vsnprintf(b, 4096, f, ap); }
int snprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int sprintf(char *b, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, 4096, f, ap); va_end(ap); return r; }
int vprintf(const char *f, va_list ap) { char b[512]; int r = vsnprintf(b, sizeof b, f, ap); logf("%s", b); return r; }
int printf(const char *f, ...) { va_list ap; va_start(ap, f); int r = vprintf(f, ap); va_end(ap); return r; }
int puts(const char *s) { logf("%s\n", s); return 0; }
int putchar(int c) { char b[2] = { (char)c, 0 }; logf("%s", b); return c; }
int sscanf(const char *s, const char *f, ...) { (void)s; (void)f; return 0; }   /* config files: never read */

/* ---- files ---- */
struct FILE { const uint8_t *data; size_t size, pos; int sink, console; };
static FILE f_out = { 0, 0, 0, 1, 1 }, f_err = { 0, 0, 0, 1, 1 }, f_in = { 0, 0, 0, 0, 0 };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;
static FILE files[8];

static size_t wad_size(void)
{
	const uint8_t *w = (const uint8_t *)(uintptr_t)WAD_ADDR;
	if (memcmp(w + 1, "WAD", 3)) return 0;
	uint32_t n = w[4] | w[5] << 8 | w[6] << 16 | (uint32_t)w[7] << 24;
	uint32_t ofs = w[8] | w[9] << 8 | w[10] << 16 | (uint32_t)w[11] << 24;
	return ofs + n * 16;                                /* the directory is at the end */
}
FILE *fopen(const char *path, const char *mode)
{
	FILE *f = 0;
	for (int i = 0; i < 8; i++) if (!files[i].data && !files[i].sink) { f = &files[i]; break; }
	if (!f) { errno = EIO; return 0; }
	if (strchr(mode, 'w') || strchr(mode, 'a')) { *f = (FILE){ 0, 0, 0, 1, 0 }; return f; }
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (!strcasecmp(base, "doom1.wad") && wad_size()) {
		*f = (FILE){ (const uint8_t *)(uintptr_t)WAD_ADDR, wad_size(), 0, 0, 0 };
		return f;
	}
	errno = ENOENT;
	return 0;
}
int fclose(FILE *f) { if (f && f != stdout && f != stderr && f != stdin) *f = (FILE){ 0 }; return 0; }
size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
	if (!f->data || !sz) return 0;
	size_t want = sz * n, left = f->size - f->pos;
	if (want > left) want = left - left % sz;
	memcpy(p, f->data + f->pos, want);
	f->pos += want;
	return want / sz;
}
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
	if (f->console) { char b[256]; size_t k = sz * n < 255 ? sz * n : 255; memcpy(b, p, k); b[k] = 0; logf("%s", b); }
	return n;
}
int fseek(FILE *f, long off, int wh)
{
	long base = wh == SEEK_SET ? 0 : wh == SEEK_CUR ? (long)f->pos : (long)f->size;
	if (base + off < 0 || (size_t)(base + off) > f->size) return -1;
	f->pos = (size_t)(base + off);
	return 0;
}
long ftell(FILE *f) { return (long)f->pos; }
int fflush(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { return f->pos >= f->size; }
int ferror(FILE *f) { (void)f; return 0; }
int fgetc(FILE *f) { return f->data && f->pos < f->size ? f->data[f->pos++] : EOF; }
char *fgets(char *s, int n, FILE *f)
{
	int i = 0, c = 0;
	while (i < n - 1 && (c = fgetc(f)) != EOF) { s[i++] = (char)c; if (c == '\n') break; }
	s[i] = 0;
	return i ? s : 0;
}
int fputs(const char *s, FILE *f) { fwrite(s, 1, strlen(s), f); return 0; }
int fputc(int c, FILE *f) { char b = (char)c; fwrite(&b, 1, 1, f); return c; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { char b[512]; int r = vsnprintf(b, sizeof b, fmt, ap); fputs(b, f); return r; }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int remove(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
int access(const char *p, int m) { (void)m; FILE *f = fopen(p, "rb"); if (!f) return -1; fclose(f); return 0; }
int unlink(const char *p) { (void)p; return -1; }
int stat(const char *p, struct stat *st) { FILE *f = fopen(p, "rb"); if (!f) return -1; st->st_size = (long)f->size; st->st_mode = 0; fclose(f); return 0; }
int mkdir(const char *p, mode_t m) { (void)p; (void)m; return 0; }
int open(const char *p, int fl, ...) { (void)p; (void)fl; return -1; }
int close(int fd) { (void)fd; return 0; }
int isatty(int fd) { (void)fd; return 0; }

/* ---- system ---- */
char *getenv(const char *n) { (void)n; return 0; }
int system(const char *c) { (void)c; return -1; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }
sighandler_t signal(int s, sighandler_t h) { (void)s; (void)h; return 0; }
time_t time(time_t *t) { time_t v = (time_t)(millis() / 1000); if (t) *t = v; return v; }
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; uint32_t m = millis(); tv->tv_sec = m / 1000; tv->tv_usec = (m % 1000) * 1000; return 0; }
int usleep(unsigned us) { udelay(us); return 0; }
unsigned sleep(unsigned s) { udelay(s * 1000000u); return 0; }

void *libc_exit_jmp[5];
int libc_exit_code;
void exit(int code)
{
	logf("doom: exit(%d)\n", code);
	libc_exit_code = code;
	__builtin_longjmp(libc_exit_jmp, 1);
}
void abort(void) { exit(134); }
