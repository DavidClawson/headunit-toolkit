# Hosting `carplay.mod` on our own firmware (2026-11-27)

This is static analysis of what a replacement host must provide so the stock
`carplay.mod` binary, loaded from the unit's own flash, runs under our firmware
instead of Melis. It's "Option B" in [carplay-inventory.md](carplay-inventory.md).
That doc and its tables in [`reference/carplay/`](reference/carplay/) are the
starting point; this one adds what a guest host needs that the inventory didn't
settle (address map, trap path, struct layouts, threading model, device
ioctls), and corrects one label.

Syscall names come from Allwinner's public D1s Melis 4.0 SDK (`kapi.h`,
`SWIHandler_*_t`), used **only as a reference**; nothing from it is copied
here. **Nothing in this doc has been tested on hardware.** Addresses are
virtual addresses in `carplay.mod` (`0xe88…`) or `init.axf` (`0xe91…`/`0xe92…`).
"Confirmed" means read directly from the disassembly; "inferred" means a
reasonable reading that hasn't been checked on hardware.

## Verdict

**The module itself is easy to host. The hard part is the host services
behind its callbacks: USB device mode and H.264 decode.**

- The guest contract is small: **44 kernel calls** (43 plus `esSIOS_putstr`),
  all of them threads, semaphores, queues, ticks, heap, a few files and
  `ioctl`. Two device files (`/dev/twi2`, `/dev/spinor2`). One event callback,
  two USB-send callbacks, 25 `MIoctrl` commands. No MMIO, no interrupts, no
  clocks, no other modules.
- **New for our firmware:** the module is linked at `0xe8800000` with about
  780 absolute pointers and no relocation table, so the host needs the **MMU**
  (Sv39, S-mode). It also needs a **preemptive scheduler** with 10 ms ticks.
  Both are well-trodden. Melis 4 *is* RT-Thread plus a thin µC/OS-style shim
  (see "Which scheduler" below), so RT-Thread (Apache-2.0) is the natural
  choice for the scheduler.
- **The heavy items sit outside the module:** (1) a USB **device** stack on
  the OTG port with an iAP2 bulk interface plus CDC-NCM, and the USB role
  switch that gets the iPhone to act as host; (2) **H.264 decode** of the
  phone's video stream into a video layer, with a 90° rotation for our
  portrait-scanned panel. Audio out, touch and the MFi chip are small,
  because our firmware already drives the codec, GT911 and TWI.

Rough size: the guest runtime (loader, MMU, trap dispatcher, scheduler, the 44
calls, the two device files) is weeks of focused work. USB device + NCM and
H.264 decode are each a project of their own. Wired CarPlay showing a picture
needs all of it except AEC and the mic.

## The guest image

| Item | Value | Evidence |
|---|---|---|
| Sections | `.text` `0xe8800000` 0x9cd78, `.rodata` `0xe889cd78` 0x36af0, `.data` `0xe88d3868` 0x148c, `.bss` `0xe88d4cf4` 0x11210, `.magic` `0xffff0000` 0x50 | `headunit decompress --elf` |
| Footprint | ~0.94 MB mapped (text+rodata+data+bss), plus heap | section sizes |
| Address forming | PC-relative (`auipc`, ~9 000 sites); `gp` never used; `tp` never written | disassembly scan |
| Absolute pointers | 685 in `.rodata`, 98 in `.data` (64-bit, zero-extended, e.g. `0x00000000e887b8ea`); **no relocation table** | word scan of both sections |
| ISA | RV64GC + FP (`fflags` writes) + **T-Head custom extensions**: indexed loads/stores (`th.lrbu`, `th.lurw`), bit-field extract (`th.extu`), paired loads/stores. Roughly 8 600 custom instructions | T-Head-aware disassembly |
| Pointer width at the host boundary | **32-bit**: the event struct stores buffer pointers with `sw` (`0xe8874ba4`), the host reads them with `lwu` (`init.axf` `0xe9216418`). Semaphore handles are stored as `u32` and re-extended with `th.extu …,31,0` (`0xe887c000`) | both sides |

What that means for the host:

- **The MMU is mandatory.** The code itself would run anywhere, but ~780 data
  words hold absolute `0xe88…` addresses (function-pointer tables, string
  tables, the `.magic` interface). Our firmware runs bare in M-mode at
  `0x40000000`, and C906 M-mode doesn't translate instruction fetches. The
  host maps 2 MiB pages from VA `0xe8800000` to DRAM in an Sv39 table and runs
  the guest in S-mode. Melis does the same: its kernel has
  `ekernel/arch/riscv/rv64gc/mmu/module.c`. A fallback is a heuristic
  relocation pass (fix every 64-bit word in `[0xe8800000, 0xe88e5f04)`), but
  it risks false positives and isn't recommended.
- **Everything the guest touches must be below 4 GiB:** heap blocks, kernel
  object handles, and buffers passed to callbacks. That's free on F133 (DRAM
  at `0x40000000`, 64 MB), but the heap must not hand out sign-extended or
  high-mapped addresses.
- **T-Head extensions must be enabled** (`mxstatus.THEADISAEE`, bit 22) or the
  guest faults on its first `th.*` instruction. I haven't checked whether
  BROM/boot0 already set it for our firmware.
- **FP state** has to be saved per thread (`mstatus.FS` is already on in
  `tools/fel/dram/start.S`).
- Melis identity-maps MMIO: the stock decoder driver writes the VE at
  physical `0x01c0e000` (`vdecoder.drv` `0xe560dd60`). An identity map of the
  device range plus DRAM, with the module window added, matches the stock
  layout. On C906, page attributes for MMIO (strong order, non-cacheable)
  come from the T-Head PTE bits when `mxstatus.MAEE` is set; this needs
  checking before any host code runs with paging on.

## Loading and entry points

The `.magic` descriptor (80 B at `0xffff0000`) is the SDK's `mod_test`
template: `"ePDK.mod"`, version `0x01000000`, type `0x88`, heap fields
`0xF0000`/`0x400`, then seven 64-bit `mif` pointers:

| Entry | Address | What it does (confirmed) |
|---|---|---|
| `MInit` | `0xe887b8ea` | prints `CARPLAY_MInit`, returns 0 |
| `MExit` | `0xe887b902` | returns 0 |
| `MOpen(mid, mode)` | `0xe887b906` | stores `mid` in a static `{u8 mid; u8 refs}` at `0xe88e3788`, bumps `refs`, returns that pointer as the handle |
| `MClose(mp)` | `0xe887b92c` | decrements `refs`, returns 0 |
| `MRead` / `MWrite` | `0xe887b942` / `0xe887b946` | return 0 |
| `MIoctrl(mp, cmd, aux, pbuffer)` | `0xe887b94a` | `cmd-1` > 24 → −1; otherwise jumps through a relative table at `0xe88cca10` (25 entries) |

A minimal loader: read the section table from MINFS (our `minfs.py` already
does), LZMA-decode each section to its VA, zero `.bss`, read `.magic`, call
`MInit`, then `MOpen`. There's no signature check and no relocation. The host
can read `.magic` straight from the section table and never needs to map
`0xffff0000`.

## Host → module: `MIoctrl`

All 25 commands are in [`carplay_mioctrl.csv`](reference/carplay/carplay_mioctrl.csv).
The calling convention (confirmed from `init.axf`'s wrappers at
`0xe9218a34`–`0xe9218d10`) is **`MIoctrl(handle, cmd, 0, u64 args[])`**, with
one 64-bit slot per argument:

| Cmd | `args[]` | Note |
|---|---|---|
| 1 | `{event_cb, ncm_send_cb, iap2_send_cb}` | see below; **correction**: the CSV calls [1]/[2] "iap send A/B". [1] goes to `lib_net_init` (`0xe88837bc`) and [2] to `lib_iap_init` (`0xe880e8ba`) |
| 8 | `{state, x1, y1, x2, y2}` | two touch points, from the host's two `int[2]` arrays (`0xe9218b06`) |
| 9 / 24 | `{w, h}` | video size the host asks for / physical screen size |
| 10 | `{fps}` | frame rate |
| 12 | `{key}` | key/knob |
| 13 | `{kind, ptr, len}` | USB data in. `kind` 0 = iAP2 bytes, 1 = NCM Ethernet frame for the module's own lwIP (`0xe9218bd8`) |
| 14, 15, 16, 17, 18 | `{char*}` / `{ptr}` | strings set before start; 17 = I2C device name, 15 = licence blob |

**Stock start-up order** (`init.axf` `0xe92165f8`): `esMODS_MInstall("d:\mod\carplay.mod")`
and `MOpen` (`0xe92189c8`), then 9 (`carplayVideoWidth/Height`), 24 (screen
w/h), 15, 17 (`carplayI2cName=/dev/twi2`), 14, 18, 16, then **1**, then the
host registers its `/dev/udv` receive callback (`0xe9213144`) and starts its
own CarPlay thread. The cmd 15 string comes from offset 146 of a host
settings struct (pointer at `0xe9309a78`, getter `0xe918c1b2`); where that
struct is loaded from isn't traced.

`MIoctrl` runs synchronously on the caller's thread. Cmd 1 does the heavy
initialisation (lwIP, iAP, the CarPlay plug-in, the MFi probe, the licence
check) and starts module threads.

## Module → host: callbacks

**Event callback** (`args[0]` of cmd 1, stored at `0xe88e35a0`). The module
calls `event_cb(&ev)`, where `ev` is a stack struct of 32-bit words:
`{u32 type; u32 a; u32 b; u32 c; u32 d}`. Pointers go in as 32-bit words.
Types and the stock handling are in
[`host_event_callback.csv`](reference/carplay/host_event_callback.csv). The
ones a host must implement:

| Type | Fields (confirmed from host dispatcher `0xe92163b2`) | Host job |
|---|---|---|
| 0 | `w, h` (`0xe8874afe`) | open H.264 decoder (stock ignores the fields and uses its own cached `carplayVideoWidth/Height`) |
| 1 | – | close decoder |
| 2 | `ptr, len` (`0xe8874ba4`) | one H.264 access unit (Annex-B assumed, not verified) |
| 3 / 4 / 5 | open `(u8, u8, c)` / close / write `ptr, len` | PCM out. Stock: `/dev/audio_play6`. Called from the module's `AudioOutputThread`, so a blocking write paces the module |
| 6 / 7 / 8 | open / close / read `ptr, len` | mic PCM in (host fills the buffer) |
| 11 | `code` (2–28) | status → UI text |
| 12 / 13 / 14 | AEC open / close / data | stock `/dev/audio_aec` (WebRTC AEC in the kernel) |
| 16 | date/time | set the RTC from the phone |
| 9, 10, 15 | – | stock ignores 9 and 10 |

The dispatcher returns 0 for everything. Callbacks run on **module threads**
(video, audio, status), so host code behind them must be thread-safe and may
block.

**USB send callbacks** (`args[1]`, `args[2]`): `cb(&{u32 flag; u32 ptr; u32 len})`.
Stock sends only when `flag == 0`. It copies into a `len+512` buffer and calls
`ioctl(udv_fd, 201, {u64 channel, ptr, len})`, with channel 1 for NCM and
channel 0 for iAP2 (`0xe9213176`). It returns 0 on success, −1 on failure.

**Receive path:** the host's `/dev/udv` callback (`0xe921654a`) forwards
`kind` 0/1 to `MIoctrl 13`; `kind` 2 is a one-byte link status the host keeps.

## Module → kernel: the 44 calls

The encoding is `ecall` with `a7 = 0x10000000 | group<<8 | slot`. Each number
has **exactly one `ecall` site**, a 7-instruction stub between `0xe887c29a`
and `0xe887c7b4`
([`syscalls_carplay.csv`](reference/carplay/syscalls_carplay.csv) lists the
stub addresses). Arguments follow the C ABI in `a0`–`a5`, and the result
comes back in `a0`. The host can either trap `ecall` (S-mode → M-mode,
`mcause` 9, `mepc += 4`) or patch the 44 stubs into direct calls.

Effort key: **T** trivial, **S** needs the scheduler, **F** filesystem,
**D** device driver.

| Group | Calls | Notes | Effort |
|---|---|---|---|
| `0x00` SIOS | `esSIOS_putstr` | console | T |
| `0x01` KRNL tasks | `TCreate(entry, arg, stksize, prio)`, `TDel(prio)` (`0xff` = self), `TaskNameSet`, `GetTIDCur`, `TimeDly(ticks)` | pthread shim at `0xe887beca`–`0xe887bf30`: default stack 0x2000, workers 0x4000, **priority 5 everywhere**; a dispatch-queue pool creates up to 128 workers (`0xe884e8bc`) | S |
| `0x01` KRNL sync | `SemCreate/Del/Pend/Post`, `MutexCreate/Pend/Post`, `QCreate/Del/Pend/Post/QAccept` | pthread mutexes are `SemCreate(1)`; queues and the one mutex back lwIP's `sys_arch` (`0xe8897720`…). Timeouts are in ticks | S |
| `0x01` KRNL time | `TimeGet`, `Time` | both return **ticks**; the module multiplies by 10 for ms (`0xe887baa8`, `0xe8897716`), so **tick = 10 ms** | T |
| `0x02` MEMS | `esMEMS_Malloc/Mfree/Realloc(heap, …)` | ignore `heap`; blocks must be below 4 GiB | T |
| `0x03` FSYS | `fopen/fclose/fread/fwrite/mkdir/remove` | `e:\CarPlay\airplay.conf`, `default.keychain` (pairing identity), `d:\apps\CarPlay\icon_*.png` | F (small) |
| `0x09` TIME | `esTIME_GetTime/GetDate` | RTC or clock | T |
| `0x0f` KSRV | `esKSRV_Random(max)` | needs real entropy for crypto quality | T |
| `0x1a` newlib | `_open_r/_close_r/_read_r/_write_r/_lseek_r/_fstat_r/_isatty_r` | first arg is `struct _reent*`, ignore it. `open` is used for `/dev/spinor2` and `/dev/twi2` (via `0xe8800430`, `O_RDWR`) | F/D |
| `0x1a` newlib heap | `_malloc_r/_realloc_r/_calloc_r/_free_r` | same heap rules | T |
| `0x1a` `ioctl` | 8 sites | see next section | D |

There are no calls into `esMODS`, `esDEV`, `esINT`, `esCLK`, `esPINS` or DMA.

## Which scheduler: Melis 4 is RT-Thread underneath

**Melis 4.x is RT-Thread plus a µC/OS-II-flavoured compatibility shim.** The
`esKRNL_*` calls `carplay.mod` uses are thin wrappers over RT-Thread objects;
the only semantics of their own are IDs, priority levels and timeout
conventions inherited from µC/OS-II. Sources (SDK, `weidongshan/D1s-Melis`
on Gitee, `master`, read only):

- `ekernel/core/rt-thread/` is the RT-Thread kernel (`ipc.c`, `clock.c`,
  `thread.c`, `rtdef.h`, …).
- `ekernel/legacy/mod_api/epos.c` implements every `esKRNL_*` call, and
  `ekernel/legacy/mod_api/swi.c` is the syscall table.
- `ekernel/drivers/osal/src/hal_sem.c`, `hal_queue.c` and `hal_mutex.c`, with
  headers in `ekernel/drivers/include/osal/`, form a `hal_*` layer between
  them. It has both an RT-Thread backend (`CONFIG_RTTKERNEL`) and a FreeRTOS
  one.
- `projects/d1s-nezha/configs/defconfig` sets `CONFIG_HZ=100`,
  `CONFIG_RT_THREAD_PRIORITY_MAX=32` and enables semaphore, mutex, mailbox
  and message queue.
- The shipped kernel matches: `kernel.bin` contains `epos.c`'s messages
  (`failure to create melis thread.!`, `pend sem in sched lock.`, `thread %d
  has been deleted!`), the object names `melis_app` and `hal_layer`, and
  `RT-Thread shell commands:`.

How each call the module uses maps (from `epos.c` and the osal sources):

| `esKRNL_*` | RT-Thread underneath | Semantics a host must reproduce |
|---|---|---|
| `TCreate(entry, arg, stk, level)` | `rt_thread_create(name, entry, arg, stk, prio, slice)` + `rt_thread_startup` | **Returns a short ID, not a pointer:** `(slot+1) \| (pid<<8)` from a 255-entry table (`USE_MELIS_SHORT_THREAD_ID`, `MELIS_THREAD_COUNT` in `include/melis/epos.h`). Priority **level** maps through a fixed table: levels 0–2 → prio 9, `0x38`… → 10–23, anything else → prio 24 with a 15-tick slice (otherwise 20). `carplay.mod` always passes level 5, so **all its threads are RT-Thread prio 24, slice 15** |
| `TDel(id)` | `rt_thread` stop via the ID table | `0xff` (`EXEC_prioself`) = delete self |
| `TaskNameSet(id, name)` | lookup in the ID table | by short ID |
| `GetTIDCur()` | `rt_thread_self()` | returns the short ID for module threads (detected by entry address outside the DRAM virtual base) and the raw thread pointer for kernel threads |
| `TimeDly(ticks)` | `rt_thread` sleep | ticks |
| `TimeGet()`, `Time()` | `rt_tick_get()` | 100 Hz |
| `SemCreate(n)` / `SemDel` / `SemPost` | `rt_sem_create("hal_layer", n, RT_IPC_FLAG_FIFO)` / `rt_sem_delete` / `rt_sem_release` | µC/OS error codes (`OS_NO_ERR` 0, `OS_SEM_OVF` 50) |
| `SemPend(sem, timeout, &err)` | `rt_sem_take(sem, ticks)` | **timeout 0 = wait forever** (RT-Thread's 0 means don't wait); `err` = `OS_TIMEOUT` (10) on failure |
| `MutexCreate(prio, &err)` / `MutexPost` | `rt_mutex_create` / `rt_mutex_release` | `prio` ignored |
| `MutexPend(m, timeout, &err)` | `rt_mutex_take(m, forever)` | **the timeout is ignored**; it always waits forever |
| `QCreate(n)` / `QDel` | `rt_mb_create("melis_app", n, FIFO)` / `rt_mb_delete` | a **mailbox** of word-sized messages, not an `rt_messagequeue` |
| `QPost(q, msg)` | `rt_mb_send` | `KRNL_Q_FULL` (30) when full. `msg` passes through `hal_mailbox_send(…, unsigned int)`, so **messages are truncated to 32 bits** (another 32-bit pointer constraint) |
| `QPend(q, timeout, &err)` / `QAccept` | `rt_mb_recv(q, &v, timeout or forever)` / `rt_mb_recv(…, 0)` | timeout 0 = forever; `QAccept` → `KRNL_Q_EMPTY` (31) |

Timers (`esKRNL_Tmr*`) also exist in `epos.c`, but `carplay.mod` doesn't use
them; its timers are its own, built on threads and `TimeDly`.

**What this means for the choice:**

- **RT-Thread is the natural host scheduler.** Every primitive the module
  uses has a direct RT-Thread counterpart with the same FIFO, tick and
  time-slice behaviour stock runs under, so timing quirks carry over. It's
  Apache-2.0. As far as I know, upstream RT-Thread has a C906 port and
  Allwinner D1/D1s BSPs (`libcpu/risc-v/t-head/c906`, `bsp/allwinner/d1s`),
  including MMU/S-mode support; **verify before relying on it**.
- **FreeRTOS would also work.** The SDK's own osal has a FreeRTOS backend for
  the same `hal_*` calls (mailbox → `xQueue` of words, mutex → FreeRTOS
  mutex). Time slicing and priority inheritance differ slightly from what
  stock runs.
- **Our own scheduler is feasible but buys little.** The module needs about
  15 primitives, but writing and debugging preemption, an FP context switch,
  timed waits and priority handling is exactly what an RTOS already gives us.
- **Either way, we write the shim ourselves.** It's about 300 lines,
  clean-room (don't copy `epos.c`): the short-ID table, the priority-level
  map, timeout-0-means-forever, `MutexPend` ignoring timeouts, µC/OS error
  codes, and 32-bit handles and messages.

## Device files

**`/dev/twi2`: the MFi authentication chip.** `ioctl(fd, 3, struct i2c_msg *)`
with **one Linux-style message per call**: `{u16 addr; u16 flags; u16 len;
u8 *buf}`, where `flags` 1 means read (`0xe887bdb2`–`0xe887be9c`). A
register access is a write message (the register address) followed by a
separate read message. The module probes 0x11 and then 0x10, and our bus
scan found the chip at 0x10. Our `tools/fel/dram/twi.c` can serve this
directly. Whether the stock kernel's cmd 3 also takes a message count is
**inferred** not to matter, because the module always sends one message. The
MFi protocol itself (certificate read, challenge signature) lives inside the
module (`AccessorySDK/Support/MFiSAP.c`), so the host only moves bytes.

**`/dev/spinor2`: the vendor licence check** (`0xe8875dd6`, `0xe8875ee4`):

| ioctl | Arg | Use (confirmed shape, meaning inferred) |
|---|---|---|
| 9 | `{u8 *a, u8 *b}` | two 16-byte blobs; the module requires `b[i] == a[i] + K[i]` for a 16-byte constant `K` in `.rodata` (`0xe88cc958`/`0xe88cc960`). Looks like an anti-clone check on the flash's unique ID or OTP |
| 8 | `{u32 *rdid}` | JEDEC ID, printed as `rdid=0x%x` |
| 5 | `{u8 *buf}` | unique ID; returns length (`uuid len %d:`) |
| 6 | `{u64 off, u8 *buf, u64 len=16}` | 16-byte OTP read, printed as `otp:` |

Our `spinor.c` can read the real chip, but **ioctl 9's semantics are the open
question**: which two values the stock kernel returns. Whether a failed check
blocks CarPlay, and how it relates to the cmd 15 blob, is also unknown.

## Where video, audio, USB and authentication go

- **Video: handed to the host as H.264.** The module never calls cedar. It
  emits decoder open/close and one access unit per event (types 0–2). Stock
  feeds them to `cedar.mod` in raw-stream mode, which drives `vdecoder.drv`
  (libcedarc, VE hardware), then a display video layer, using `/dev/g2d` for
  rotation. **The host picks the stream size and rate** (cmds 9 and 10), so
  it can ask for less than 1280×720 and scale on the display side.
  - Hosting `vdecoder.drv` as a second guest is possible in principle, but it
    makes **69 kernel calls** including `esINT_InsISR`, `esCLK_*`,
    `esMEMS_Palloc/VA2PA/CleanFlushDCacheRegion` and pthread/semaphore calls:
    a driver-class contract, not the easy kind.
- **Audio: handed to the host as PCM.** There's one output stream (open
  carries channels, rate and bits) and one mic stream. The module has
  "main"/"alt" audio internally but **appears** to expose only one sink, so
  the mixing is presumably inside (inferred). Our codec and DMA playback cover
  the output. The rate can change per session (media vs telephony), so the
  sink must reopen at a new rate. AEC (types 12–14) can be stubbed at first.
- **USB: bytes only; the host owns the link.** The module has its own
  **lwIP** (`CarApp/…/Libs/3rd/lwip`), **mDNSResponder** (Bonjour
  `_carplay-ctrl._tcp`, `_airplay._tcp`), and Apple's AccessorySDK/AirPlay
  receiver. It takes raw Ethernet frames (NCM) and raw iAP2 link bytes
  through cmd 13 and the two send callbacks. In stock, the kernel's
  `/dev/udv` driver does the USB work. Its strings show a **USB gadget**
  (`udc:` …, `ncm_desc_init`, `NCMHNCM0`, `receive_iap_thread`,
  `receive_ncm_thread`, plus `android`/`mirror` functions for the other
  phone links) alongside EHCI/OHCI host drivers. So wired CarPlay here runs
  with the **head unit as a USB device**, after a role switch, which is the
  usual CarPlay arrangement. Where the role-switch request is issued (kernel
  `udv` or `init.axf` through `udv` ioctls 7/100–102/300/301) is **not
  traced**.
- **MFi authentication: through the host's I2C device file**, as above.
- **Wireless CarPlay: not through `carplay.mod`.** `wireless.mod` (2.2 MB) is
  a separate, complete build with its own AirPlay/CarPlay stack, OpenSSL and
  WebRTC (`CarApp/Build/F133_Melis/mod_wireless`, `/openssl`, `/webrtc`),
  using a socket table its host passes in. It would need WiFi (SSV6x5x over
  SDIO in stock) and Bluetooth. That's out of scope for the wired-first plan.
- **Android Auto (free notes):** `auto.mod` has the same shape: 38 kernel
  calls from the same easy set, 22 `lib_auto_*` commands, and AOA over the
  same `/dev/udv`. A host built for `carplay.mod` covers most of it. It needs
  USB **host** mode (AOA) rather than device mode.

## What a replacement host must implement

| # | Item | Effort | Notes |
|---|---|---|---|
| 1 | Loader: MINFS sections → VAs, `.bss`, `MInit`/`MOpen` | small | logic exists in `headunit/minfs.py`; port to C |
| 2 | Sv39 MMU, S-mode guest, identity map + module window, C906 PTE attributes | medium | new for our firmware; caches and DMA need care (see the `dcache` work) |
| 3 | `ecall` trap and dispatcher for 44 calls | small | or patch the 44 stubs |
| 4 | Preemptive scheduler: threads, semaphores, queues, 10 ms tick, sleep, FP context | medium | RT-Thread (Apache-2.0), plus our own clean-room `esKRNL` shim of about 300 lines; see "Which scheduler" |
| 5 | Heap below 4 GiB, 32-bit handles | trivial | |
| 6 | Small file store (`e:\CarPlay\…`, icons) | small | RAM first; persist the keychain to the UDISK area (`0xf20000`, inferred to be `e:`) later |
| 7 | `/dev/twi2` I2C ioctl 3 | small | existing `twi.c` |
| 8 | `/dev/spinor2` ioctls 9/8/5/6 + cmd 15 licence string | small once understood | capture from stock first (unknown #2) |
| 9 | Touch, keys, night mode, status, time (cmds 8/12/21, events 11/16) | trivial | GT911 driver exists |
| 10 | Audio out sink (rate/channel changes) | small–medium | codec + DMA exist |
| 11 | Mic in; AEC | medium; AEC stub OK at first | `adctest.c` in progress |
| 12 | USB device controller (OTG, MUSB-style) with iAP2 bulk interface + CDC-NCM, and the role switch | **large** | no USB stack in our firmware yet. TinyUSB (MIT) has a MUSB device driver and an Allwinner F1C100s port, plus an NCM class; untested on F133 |
| 13 | H.264 decode → YUV video layer, rotated 90° for the panel | **large** | options below |
| 14 | Event-loop glue (start/stop/hide, attach/detach on plug) | small | |

Options for #13, cheapest to verify first: **software decode** at a reduced
CarPlay size (unknown speed on the C906). **Host `vdecoder.drv` as a guest**
(reuses vendor code, but adds the driver-class calls). **Our own VE driver**
written from public cedrus/libvdpau-sunxi knowledge (most work, cleanest
result).

## Biggest unknowns and the cheapest next experiment for each

1. **The USB link: who does the role switch, and what the gadget looks like.**
   Cheapest: capture the stock debug UART (UART3 on PB6/PB7, 576000 baud, per
   `melis-config.txt`) while plugging in an iPhone on stock firmware. The
   kernel prints gadget/NCM/iAP thread messages. Then statically trace the
   `udv` ioctl handler in `kernel.bin` (`udv_contrl`, `udvDevProbe`) for
   7/100–102/300/301. No custom code needed.
2. **The licence path: `spinor2` ioctl 9 and the cmd 15 blob.** Cheapest: a
   `hostmod` test module (the loading path is already proven) that opens
   `/dev/spinor2`, logs ioctls 9/8/5/6 under stock Melis, and also logs cmd
   15's string by reading `init.axf`'s settings struct. That makes the check
   reproducible under our host. A second, static step: find the caller of
   `0xe8875dd6` to see what a failed check disables.
3. **H.264 decode speed and path.** Cheapest: a FEL-run benchmark of a
   permissively licensed software H.264 decoder on our existing DRAM
   framework (like the Doom port), decoding a recorded 800×480 and 1280×720
   baseline clip, with no phone and no MMU. If it can't sustain ~30 fps at
   800×480, the VE route is mandatory and the next step is mapping
   `vdecoder.drv`'s 69 calls.
4. (Smaller) **C906 state for the guest:** whether `mxstatus.THEADISAEE` and
   `MAEE` are set when our firmware starts. Cheapest: read CSR `0x7c0` in any
   existing FEL program.

## Relationship to the earlier inventory

[carplay-inventory.md](carplay-inventory.md) chose **Option A** (keep Melis,
replace `init.axf`) for now. Nothing here changes that: Option A gets USB, VE,
G2D and AEC for free. This doc is the Option B work list. The guest runtime
(#1–#9) can be built and tested without a phone, by loading `carplay.mod`
under our host and checking that `MInit`/`MOpen`/cmd 1 reach the MFi probe on
`/dev/twi2`. That makes a good first Option B milestone, and it doesn't need
#12 or #13.
