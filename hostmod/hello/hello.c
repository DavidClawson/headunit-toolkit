/*
 * hello.mod — first custom Melis module for the W022 head unit.
 *
 * Proof of load, two ways:
 *   1. prints a banner on the debug UART (PB06/PB07) from MInit and MOpen;
 *   2. MOpen starts a background thread that waits for removable storage and
 *      writes HELLO.TXT to the root of each drive F:..K: it can open
 *      (F: is the USB stick the factory menu exports to). Internal drives
 *      (d:, e:) are never touched.
 *
 * Loaded at boot by an `insmod` line in /startup.sh (see `headunit install-module`).
 * Keep MInit/MOpen short: startup.sh waits on them.
 */
#include "melis.h"

#define MODULE_TYPE 0x75   /* unused by every stock module */

static melis_mp_t g_mp;
static volatile uint32_t g_ticks_waited;

static const char k_msg[] =
    "Hello from a custom Melis module!\r\n"
    "Loaded by /startup.sh via insmod. headunit-toolkit hostmod/hello.\r\n";

static int write_hello(char drive)
{
    char path[] = "?:\\HELLO.TXT";
    path[0] = drive;
    void *fp = melis_fopen(path, "wb");
    if (!fp)
        return 0;
    melis_fwrite(k_msg, 1, sizeof k_msg - 1, fp);
    melis_fclose(fp);
    melis_puts("[hello.mod] wrote HELLO.TXT\r\n");
    return 1;
}

static void hello_thread(void *arg)
{
    (void)arg;
    /* Poll every 2 s for up to 10 minutes; stop after the first success. */
    for (g_ticks_waited = 0; g_ticks_waited < 60000; g_ticks_waited += 200) {
        for (char d = 'F'; d <= 'K'; d++)
            if (write_hello(d))
                goto done;
        melis_sleep_ticks(200);
    }
done:
    for (;;)                      /* park: never return from a Melis thread */
        melis_sleep_ticks(6000);
}

static int32_t hello_init(void)
{
    melis_puts("\r\n[hello.mod] MInit: custom module loaded\r\n");
    return EPDK_OK;
}

static int32_t hello_exit(void)
{
    return EPDK_OK;
}

static melis_mp_t *hello_open(uint32_t mid, uint32_t mode)
{
    (void)mode;
    g_mp.mid = (uint8_t)mid;
    melis_puts("[hello.mod] MOpen: starting writer thread\r\n");
    melis_thread_create(hello_thread, 0, 0x4000, KRNL_PRIO(mid, 6));
    return &g_mp;
}

static int32_t hello_close(melis_mp_t *mp)
{
    (void)mp;
    return EPDK_OK;
}

static uint32_t hello_read(void *b, uint32_t s, uint32_t n, melis_mp_t *mp)
{
    (void)b; (void)s; (void)n; (void)mp;
    return 0;
}

static uint32_t hello_write(const void *b, uint32_t s, uint32_t n, melis_mp_t *mp)
{
    (void)b; (void)s; (void)n; (void)mp;
    return 0;
}

static long hello_ioctl(melis_mp_t *mp, uint32_t cmd, int32_t aux, void *b)
{
    (void)mp; (void)cmd; (void)aux; (void)b;
    return EPDK_FAIL;
}

MELIS_MODULE(MODULE_TYPE, hello_init, hello_exit, hello_open, hello_close,
             hello_read, hello_write, hello_ioctl);
