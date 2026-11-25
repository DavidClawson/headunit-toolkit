/*
 * melis.h — minimal, clean-room interface for writing Melis kernel modules
 * for the Allwinner F133 (W022) head unit.
 *
 * Written from the module ABI as documented in docs/format-minfs.md and
 * verified against the stock firmware (descriptor layout from all 63 stock
 * modules; syscall numbers from their `ecall` sites). No vendor code here.
 *
 * Kernel calls: `ecall` with a7 = 0x10000000 | (group << 8) | slot,
 * arguments in a0..a5, result in a0.
 */
#ifndef HOSTMOD_MELIS_H
#define HOSTMOD_MELIS_H

#include <stdint.h>
#include <stddef.h>

/* ---- module descriptor (the 'MAGIC' section) ---------------------------- */

typedef struct melis_mp {          /* handle MOpen returns */
    uint8_t mid;
    uint8_t userdef;
} melis_mp_t;

typedef struct melis_mif {
    int32_t     (*init)(void);
    int32_t     (*exit)(void);
    melis_mp_t *(*open)(uint32_t mid, uint32_t mode);
    int32_t     (*close)(melis_mp_t *mp);
    uint32_t    (*read)(void *buf, uint32_t size, uint32_t n, melis_mp_t *mp);
    uint32_t    (*write)(const void *buf, uint32_t size, uint32_t n, melis_mp_t *mp);
    long        (*ioctl)(melis_mp_t *mp, uint32_t cmd, int32_t aux, void *buf);
} melis_mif_t;

typedef struct melis_module_desc {
    char        magic[8];          /* "ePDK.mod" */
    uint32_t    version;           /* 0x01000000 in every stock module */
    uint8_t     type;              /* slot: load address = 0xe0000000 + type MiB */
    uint32_t    heapaddr;          /* 0xF0000 in every stock module */
    uint32_t    heapsize;          /* 0x400   in every stock module */
    melis_mif_t mif;
} melis_module_desc_t;             /* exactly 80 bytes: the kernel copies the
                                      whole MAGIC section into an 80-byte
                                      stack struct, so never make it larger */

_Static_assert(sizeof(melis_module_desc_t) == 80, "descriptor must be 80 bytes");

/* Declare a module: MELIS_MODULE(0x75, init, exit, open, close, read, write, ioctl) */
#define MELIS_MODULE(TYPE, I, E, O, C, R, W, X)                               \
    __attribute__((section(".magic"), used))                                  \
    const melis_module_desc_t melis_module_desc = {                           \
        {'e', 'P', 'D', 'K', '.', 'm', 'o', 'd'}, 0x01000000, (TYPE),         \
        0xF0000, 0x400, {I, E, O, C, R, W, X}                                 \
    }

#define EPDK_OK    0
#define EPDK_FAIL  (-1)

/* ---- syscalls ------------------------------------------------------------ */

#define MELIS_SYS(group, slot)  (0x10000000UL | ((group) << 8) | (slot))

/* group 0: serial I/O */
#define SYS_SIOS_PUTSTR   MELIS_SYS(0x00, 0x03)
/* group 1: kernel */
#define SYS_KRNL_TCREATE  MELIS_SYS(0x01, 0x00)
#define SYS_KRNL_TDEL     MELIS_SYS(0x01, 0x01)
#define SYS_KRNL_TIMEDLY  MELIS_SYS(0x01, 0x0c)
/* group 3: filesystem */
#define SYS_FSYS_FCLOSE   MELIS_SYS(0x03, 0x02)
#define SYS_FSYS_FOPEN    MELIS_SYS(0x03, 0x08)
#define SYS_FSYS_FWRITE   MELIS_SYS(0x03, 0x15)

static inline long melis_syscall4(unsigned long n, long a, long b, long c, long d)
{
    register long a7 __asm__("a7") = (long)n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a7), "r"(a1), "r"(a2), "r"(a3)
                     : "memory");
    return a0;
}

static inline void melis_puts(const char *s)
{
    melis_syscall4(SYS_SIOS_PUTSTR, (long)s, 0, 0, 0);
}

/* Thread priority levels 1..6 (0 and 7 reserved); tick = 10 ms (100 Hz). */
#define KRNL_PRIO(mid, level)  ((uint16_t)(((mid) << 8) | (level)))

static inline uint32_t melis_thread_create(void (*fn)(void *), void *arg,
                                           uint32_t stack, uint16_t prio)
{
    return (uint32_t)melis_syscall4(SYS_KRNL_TCREATE, (long)fn, (long)arg,
                                    stack, prio);
}

static inline void melis_sleep_ticks(uint16_t ticks)
{
    melis_syscall4(SYS_KRNL_TIMEDLY, ticks, 0, 0, 0);
}

static inline void *melis_fopen(const char *path, const char *mode)
{
    return (void *)melis_syscall4(SYS_FSYS_FOPEN, (long)path, (long)mode, 0, 0);
}

static inline uint32_t melis_fwrite(const void *buf, uint32_t size, uint32_t n, void *fp)
{
    return (uint32_t)melis_syscall4(SYS_FSYS_FWRITE, (long)buf, size, n, (long)fp);
}

static inline int32_t melis_fclose(void *fp)
{
    return (int32_t)melis_syscall4(SYS_FSYS_FCLOSE, (long)fp, 0, 0, 0);
}

#endif /* HOSTMOD_MELIS_H */
