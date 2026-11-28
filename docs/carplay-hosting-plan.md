# Hosting `carplay.mod`, or writing our own receiver: a staged plan (2026-11-27)

This picks up from [carplay-host.md](carplay-host.md) (the guest contract:
44 kernel calls, 25 `MIoctrl` commands, callbacks),
[carplay-usb.md](carplay-usb.md) and [carplay-iap2.md](carplay-iap2.md), and
the devlog's "Where wired CarPlay stands" entry. Our own firmware now does
everything up to the NCM link:
- the role switch;
- the iAP2 gadget;
- the iAP2 link;
- MFi authentication;
- identification;
- NCM, with the phone sending DHCP and mDNS.

The question here is how to get from there to a picture: host stock
`carplay.mod`, or write the CarPlay session ourselves.

**Nothing in this doc has been tested on hardware.** It's static analysis of
the user's own decompressed `carplay.mod` and `kernel.bin`, plus web research.
The Melis SDK was used **only as a reference**, and nothing from it or from
the stock binaries is copied here. Labels used below:
- **Confirmed:** read directly from the disassembly.
- **Inferred:** a reasonable reading, not checked.
- **Guess:** a guess.

Module addresses are VAs in `carplay.mod` (`0xe88…`). Kernel addresses are in
`kernel.bin` loaded at `0x40000000`.

## Verdict

| Question | Answer |
|---|---|
| Is hosting technically feasible? | **Yes, and simpler than [carplay-host.md](carplay-host.md) assumed.** The MMU isn't needed: the module's absolute pointers form a clean, checkable set, so it can be relocated at load time and run in M-mode inside our firmware (see "The loader"). It still needs a preemptive scheduler. |
| Can hosting pass the licence check by genuine flash reads alone? | **No. This is the blocker.** `/dev/spinor2` ioctl 9 isn't a flash read. The stock kernel answers it by adding a **secret 16-byte constant held in the kernel** to a buffer the module supplies. Without that answer, the module skips its OTP check, and **every H.264 frame it hands to the host gets one of its first 16 bytes altered**. Satisfying ioctl 9 means our host reproduces the vendor kernel's authenticity handshake. That's outside this project's constraint (genuine read-only pass-through only) and is the §1201 grey area in [legal-and-scope.md](legal-and-scope.md). **This needs the owner's decision; this doc does not do it.** |
| RT-Thread on F133 with the MMU? | Upstream `bsp/allwinner/d1s` exists and is in CI. It runs in **S-mode under OpenSBI**, and it **sets up the MMU only in RT-Thread Smart** (`RT_USING_SMART`). Its drivers and OpenSBI boot don't fit our firmware. **If hosting goes ahead,** use only the RT-Thread kernel core (Apache-2.0), on an M-mode RV64 port of its legacy RISC-V context switch, with no MMU. |
| Our own receiver? | **Feasible, and now publicly demonstrated.** [xcertplay](https://github.com/shilapi/xcertplay) (GPL-3.0, Kotlin, created 2026-09-12) is a working CarPlay receiver that uses a real MFi chip over I2C, wired over NCM or wireless. Using it as a protocol reference (not copying code; our repo is MIT), the session is roughly **6–10k lines of our own C**, plus lwIP, a crypto library and an H.264 decoder. It keeps the iAP2/MFi code we already wrote on the critical path, and needs no MMU, no scheduler and no licence check. |
| Recommendation | **Write our own receiver.** Do the H.264 decode spike first, since both routes need it. Keep stock firmware as the "CarPlay mode" fallback until ours shows a picture. Park hosting, unless the owner decides ioctl 9 is acceptable. |

## 1. The licence check in detail (new)

[carplay-host.md](carplay-host.md) listed the four `/dev/spinor2` ioctls and
left ioctl 9 open. Tracing both sides settles it.

### Kernel side

The spinor2 ioctl handler starts at `0x40053d16`. It checks `cmd − 5 ≤ 8`,
then jumps through a table of relative words at `0x4017ce90` (confirmed):

| ioctl | Target | What it does | Touches the flash? |
|---|---|---|---|
| 5 | `0x40053d44` | Reads the JEDEC ID, picks a UID length per manufacturer (8, 12 or 16), and reads the **unique ID** (`0x400539f4`) into the caller's buffer. As a side effect it also folds the UID into a 6-byte kernel buffer at `0x4023bba8`. | yes, read-only |
| 6 | `0x40053e90` | Reads the JEDEC ID, maps the requested offset to that vendor's **security (OTP) register** address, and reads 16 bytes (`0x40053c4e` or `0x40053b04`, depending on vendor) | yes, read-only |
| 8 | `0x40053fc8` | `*(u32*)arg[0] = JEDEC ID` (`0x400539a8`) | yes, read-only |
| **9** | **`0x40053fd0`** | Loads 16 bytes from **`0x4017d168`** (stored right after the string `"spinor2"`). Then, for i in 0..15, `b[i] = a[i] + K[i]` (T-Head `th.lrbu`/`th.srb` loop at `0x40053ff0`). Returns 0. | **no: pure arithmetic with a constant baked into the kernel** |
| 10, 12, 13 | | other device functions, not used by `carplay.mod` | |

### Module side

The check is the function at `0xe8875dd6`, called from `lib_carplay_init`
(cmd 1, `0xe8873eca`):
1. **ioctl 9**, then it checks `b[i] == (a[i] + K'[i]) & 0xff` for all 16
   bytes, where `K'` is at `0xe88cc958`. **`K'` is byte-for-byte identical to
   the kernel's `K`** (compared in the scratch work, not reproduced here). On
   any mismatch it returns 0 and the rest is skipped.
2. **ioctl 8** (`rdid`), then **ioctl 5** (the UID).

The caller then reads the OTP at offset `0x30` with ioctl 6
(`0xe8875ee4`). It tests that against (UID, cmd 15 licence string at
`0xe88e36b0`) in four variants (`0xe88760aa`, modes 1–4). If none match, it
reads offset `0x80` and tries the four variants again.

### What a failure does

At cmd 1, the module allocates two 32-byte buffers, A (`*0xe88e3728`) and B
(`*0xe88e3750`), and fills A with random bytes (`0xe8873e9a`–`0xe8873ec4`).
**Only when the licence check passes** does it copy 16 bytes of A into B
(`0xe8873efc`).

The video-frame emitter `0xe8874b4a` sends **event 2** (`{2, ptr, len}`) to
the host. Before each send it computes `i = (ticks / 10) & 15`, then does
`frame[i] += A[i] − B[i]` (`0xe8874b74`–`0xe8874b84`, confirmed by decoding the
T-Head indexed loads and stores). With a passed check, `A[i] == B[i]` and the
frame is untouched. Otherwise, one byte among the first 16 of every access
unit changes. That's the start code and NAL header region, so the video
breaks. Audio, touch and iAP2 aren't affected as far as traced.

### What this means under the project's constraint

- **Ioctls 5, 6 and 8** are genuine read-only flash reads: RDID `0x9F`, the
  unique ID `0x4B`, and security-register read `0x48`. A host can pass them
  through from the real chip with our `spinor.c`. One open semantics item:
  the kernel maps the module's OTP offset (`0x30`, `0x80`) to a per-vendor
  security-register address. A pass-through must reproduce that mapping by
  reading the stock kernel (`0x40053e90` onward), and it must use **read
  commands only**. It must never program, erase or lock a security register.
- **Ioctl 9 can't be satisfied by a flash read.** The only honest
  pass-through is to return an error, and that gives corrupted video. The
  alternatives are:
  - returning `a + K`, which means our code reproducing a secret the vendor
    put in its kernel so that modules only run under it;
  - undoing the corruption in the host, which is a bypass.

  **Both are flagged, not proposed.** This plan doesn't implement either.
- **The cmd 15 string** comes from the stock host's settings struct (offset
  146; see [carplay-host.md](carplay-host.md)), which is the unit's own data.
  Where that struct is loaded from is still untraced.
- **Option A in [carplay-inventory.md](carplay-inventory.md)** (our app on
  stock Melis) and **booting stock as "CarPlay mode"** both pass the check
  genuinely, because the real kernel answers ioctl 9.

## 2. The OS question

### Upstream RT-Thread on D1s

Checked with `gh api` and a sparse clone of `master` on 2026-11-27.

**Paths and last activity:**

| Path | Last commit |
|---|---|
| `bsp/allwinner/d1s` (`rtconfig.py`: `VENDOR='t-head'`, `CPU='c906'`) | 2026-11-05 (`d42ffde1`, QEMU boot script, #11903) |
| `bsp/allwinner/d1` | 2026-05-08 (version sync only) |
| `bsp/allwinner/libraries` | 2026-08-26 |
| `libcpu/risc-v/legacy-t-head/c906` | moved here from `libcpu/risc-v/t-head/c906` by "Next RISC-V" (`8d8dc96b`, #10320), 2026-10-17 |
| `libcpu/risc-v/common64` | 2026-11-05 ("fix Smart MMU and user exception handling", #11892) |
| `libcpu/risc-v/legacy_common` (a generic **M-mode** port for 32-bit RISC-V, optional FPU) | 2026-10-17 |

**Mode and MMU:**
- **S-mode under OpenSBI.** The BSP ships its own `sbi.bin`, and its READMEs
  (`README-MQ.md` for the MangoPi MQ, `README-M7.md`) describe RT-Smart.
- **The MMU only in Smart.** `board/board.c` calls `rt_hw_mmu_map_init` and
  `rt_hw_mmu_setup` only under `#ifdef RT_USING_SMART`, which the default
  `rtconfig.h` defines. The plain kernel on this BSP runs without
  translation.

**T-Head PTE bits:** `legacy-t-head/c906/riscv_mmu.h` defines `PTE_SEC` 59,
`PTE_SHARE` 60, `PTE_BUF` 61, `PTE_CACHE` 62 and `PTE_SO` 63. **No RT-Thread
code sets `mxstatus`**; it assumes the firmware has set `MAEE`.

**Footprint:** `board.h` assumes 64 MB with a **fixed 16 MB heap** after
`.bss`, plus a page pool for Smart.

**Health:**
- `allwinner/d1s` is built in CI as the `allwinner-smart` job
  (`.github/ALL_BSP_COMPILE.json`); `allwinner/d1` isn't.
- Open issues: #11718 (2026-08-18, Smart hangs if a tty registers during
  board init) and #10457 (2025-07-04, no nested interrupts on RISC-V).
- The libcpu refactor landed in October 2026, so expect churn.

**Alternatives:**
- **NuttX** runs a C906 in S-mode with the MMU (`ox64`/BL808: `BUILD_KERNEL`,
  `ARCH_USE_S_MODE`, `ARCH_USE_MMU`, T-Head PTE extension). Its D1 port is
  S-mode with a flat address space.
- **FreeRTOS** has no D1 port with S-mode and the MMU.
- **Zephyr:** we found no D1 support.
- **xv6:** there's a Sv39 xv6 port for the Nezha D1, which notes the C906
  needs the A and D bits preset in every PTE.

**Verdict:** the upstream BSP is alive but brings three things we don't
want: OpenSBI, S-mode, and its own drivers (display, USB, SD). Its MMU path
is tied to Smart. **If hosting goes ahead, take only the kernel core:**
- `src/` (threads, scheduler, IPC, timers, heap);
- a small M-mode RV64 port, by widening `legacy_common` (`context_gcc.S`,
  `interrupt_gcc.S`, `cpuport.c`, about 900 lines today) to 64-bit with FP
  save/restore. We **guess** about 400 changed lines;
- the CLINT machine timer at 100 Hz.

Every `esKRNL_*` object the module uses is an RT-Thread object underneath
(see [carplay-host.md](carplay-host.md)), so using the same kernel keeps
stock's FIFO wait queues, mailboxes, mutex priority inheritance and 15-tick
time slices without re-deriving them.

### A home-grown scheduler instead

The module needs:
- one priority level for all its threads (RT-Thread prio 24, 15-tick
  slice);
- counting semaphores with timeouts;
- a mutex;
- word mailboxes;
- sleep and a tick.

A home-grown kernel is about 1,000–1,500 lines of C plus 150 lines of
assembly (**guess**). It's smaller to read, but timed waits, priority
inversion and FP context are exactly where bugs hide. **RT-Thread's core is
the safer choice. A home-grown kernel is only smaller, not safer.**

### Our drivers as threads

Our code today is one polling loop (`app.c`) with polled USB (`musb.c`) and
DMA audio. Under a scheduler:
- **The app loop becomes one thread** at a higher priority than the guest. It
  must block (sleep one tick, or wait on an event) each pass, or it starves
  the guest.
- **USB gets its own thread** woken by the MUSB interrupt, because NCM video
  traffic at several Mbit/s can't wait 10 ms per poll. This is new work: the
  devlog notes that the boot ROM's USB interrupt has to be masked off today,
  so interrupt-driven USB is untested.
- **Shared drivers need a mutex each.** The guest calls our callbacks
  (`ncm_send_cb`, `iap2_send_cb`, event 3/5 audio, event 2 video) **on its own
  threads**, so `musb.c`, `audio.c`, `twi.c` and the display/video layer each
  need a lock.
- **The D-cache must be on** (`mhcr 0x11ff`, as Doom does: 10× faster). That
  makes the existing flush-before-DMA discipline mandatory everywhere.

### Memory against our 64 MB

| Item | Size | Source |
|---|---|---|
| Our firmware image | ≤ 2 MB (the stage-two slot at NOR `0xD20000`) | `tools/fel/s2-write.sh`, `dram.ld` |
| Framebuffers | 7 MB (`0x41000000`, 2 × 3.5 MB) | `display.h` |
| Guest image | 0.94 MB (text 627 KB, rodata 219 KB, data 5 KB, bss 69 KB) | section table |
| Guest threads | up to 128 dispatch workers × 16 KB ≈ 2 MB worst case | `0xe884e8bc` |
| Guest heap | unknown. Budget 8–16 MB and measure at stage H3 (**guess**) | |
| RT-Thread core | tens of KB code, a few KB RAM (**guess**) | |
| H.264 decode | ~1.4 MB per 1280×720 NV12 frame, ~6 frames ≈ 8 MB; ~3.5 MB at 800×480 | arithmetic |

That's about 30 MB at most, so 64 MB is comfortable. Melis runs the same
module plus `init.axf`, cedar and the UI in the same 64 MB.

## 3. The loader

### What's in the file

Confirmed with `headunit decompress --elf` on the user's own image:

| Section | VA | Size |
|---|---|---|
| `.text` | `0xe8800000` | `0x9cd78` |
| `.rodata` | `0xe889cd78` | `0x36af0` |
| `.data` | `0xe88d3868` | `0x148c` |
| `.bss` | `0xe88d4cf4` | `0x11210` |
| `.magic` | `0xffff0000` | `0x50` |

- **The VA comes from the module type:** `0xE0000000 + type × 1 MiB`
  (`headunit/decompress.py`), and the type is `0x88`.
- **Sections are LZMA-compressed** in MINFS (5-byte properties plus raw
  LZMA1, no end marker). An on-unit loader needs a C LZMA decoder; the LZMA
  SDK's `LzmaDec.c` is public domain.

### Relocation instead of the MMU (new)

A scan of every 4-byte-aligned position in `.rodata` and `.data` for 64-bit
words pointing into `[0xe8800000, 0xe88e5f04)` finds (a scratch script, not in the repo):
- **783 pointers, all 8-byte aligned**, all zero-extended 64-bit;
- none unaligned, and **no lone 32-bit words** pointing into the module.

| From → to | Count |
|---|---|
| rodata → rodata | 585 (210 are string starts, the rest tables) |
| rodata → text | 95 |
| data → rodata | 68 |
| data → text | 23 |
| data → data | 7 |
| rodata → bss | 4 |
| rodata → data | 1 |

**All 118 code pointers land on a function start:** the previous instruction
is a return or jump, or the gap is `c.nop` alignment padding.

The code doesn't form module addresses absolutely:
- about 9,300 `auipc` sites, `gp` never used and `tp` never written;
- no `lui` of `0xe88…` (in RV64 that would sign-extend to an address Sv39
  can't map);
- the two `lui 0x40000` sites are float constants;
- the `MIoctrl` table at `0xe88cca10` is relative.

**So the loader can:**
1. decode the sections to any 2-byte-aligned DRAM address `P`;
2. add `P − 0xe8800000` to those 783 words, plus the seven `.magic` `mif`
   pointers;
3. zero `.bss`;
4. run the module **in M-mode with no MMU**, at a physical address below
   2 GiB.

This in-RAM patch is applied only to the user's own module, loaded from their
own flash, and is never stored or distributed.

The earlier doc called the MMU mandatory and the relocation heuristic
"risky". With the pointer set this clean, relocation is the cheaper route.
The **remaining risk** is a pointer computed at run time from a constant the
scan can't see. **Bench check:** load the module at two different bases and
compare the console output (stage H2).

### If the MMU is used anyway

Use it only if relocation fails on the bench. Run host and guest together in
**S-mode** with one Sv39 table:
- **Root (4 KB):**
  - GiB 0 (`0x0`–`0x3fffffff`): device identity gigapage, `SO|SH` (Melis's
    `PAGE_KERNEL_SO` clears `C` and `B` and adds `SO`);
  - GiB 1 (`0x40000000`–`0x7fffffff`): DRAM identity gigapage,
    `C|B|SH|RWX|G|A|D` (Melis's `PAGE_KERNEL_EXEC`);
  - GiB 3: a pointer to one level-1 table.
- **Level 1 (4 KB):** one 2 MiB megapage. `0xe8800000` is 2 MiB-aligned
  (entry `0x144`), mapped to a 2 MiB-aligned DRAM block, RWX. The 0.94 MB
  module fits in one megapage. 8 KB of tables in all.
- **Preset `A` and `D`.** The C906 doesn't update them in hardware.
- **Melis itself maps modules page by page** (4 KB) with `PAGE_KERNEL_EXEC`
  (`ekernel/arch/riscv/rv64gc/mmu/module.c`, `module_alloc`). Devices are
  identity-mapped in 2 MiB windows with `PAGE_KERNEL_SO`
  (`mmu-sv39.c`, table at line 127). Reference only.

**The cost is the S-mode lift:**
- an M-mode monitor: PMP opened for S-mode (`pmpaddr0` NAPOT all),
  `medeleg`/`mideleg`, a timer path, and ecall routing;
- an audit of every M-only CSR our drivers touch: `mhcr`, `mxstatus`,
  `mtvec`, `mie`, cache-maintenance instructions.

**How Melis routes module ecalls:** Melis runs in S-mode and handles the
module's ecalls as cause 9 (ecall from S) in its own trap code
(`ekernel/arch/riscv/common/exception.c`, `EXC_SYSCALL_FRM_S` →
`esSYSCALL_function`). That implies the vendor OpenSBI redirects them back
to S-mode (**inferred**).

### C906 state the guest needs

- **`mxstatus` bits:** stock boot0 sets `mxstatus |= 0x400000` (`THEADISAEE`)
  early, then `|= 0x638000`, which is `MM`, `UCME`, `CLINTEE`, `MAEE` and
  `THEADISAEE`. It also writes `mhcr = 0x11fb` and `mhint = 0x70013`
  (`boot0.dis` `0x203d0`, `0x20450`–`0x20462`).
  - **NOR boot:** our firmware inherits these, because stock boot0 still runs
    first.
  - **FEL:** the bits may be clear (no boot0), so the host sets
    `THEADISAEE` itself before the first guest instruction.
- **FP:** `mstatus.FS` is on (`start.S`). The context switch saves `f0`–`f31`
  and `fcsr`.

### Entry points and the "below 4 GiB" rule

- **The `.magic` record:** `"ePDK.mod"`, version, type `0x88`, heap fields,
  then 7 × u64 `mif` pointers (`MInit`, `MExit`, `MOpen`, `MClose`, `MRead`,
  `MWrite`, `MIoctrl`). `headunit/decompress.py` `Descriptor.parse` already
  reads it.
- **The host calls:**
  1. `MInit()`;
  2. `h = MOpen(mid, 0)`;
  3. `MIoctrl(h, cmd, 0, u64 args[])`, in stock's order
     ([carplay-host.md](carplay-host.md)).
- **What "below 4 GiB" means here:**
  - the module stores host pointers in **32-bit** fields: event structs
    (`sw`), semaphore handles (`th.extu …,31,0`), and mailbox messages
    (truncated to `u32` by `hal_mailbox_send`);
  - so every object handle, heap block and buffer address the host gives
    the guest must fit in 32 bits, zero-extended;
  - on F133 that's automatic: DRAM is `0x40000000`–`0x43ffffff`, and with
    relocation the guest itself sits there too;
  - the one way to break it is a host heap or handle table that hands out
    pointers with the top bit set or sign-extended. Don't.

## 4. The shim, by difficulty

The 44 calls and their RT-Thread mapping are in
[carplay-host.md](carplay-host.md). Grouped by what we must match:

| Group | Calls | Difficulty | Exact semantics to match |
|---|---|---|---|
| Trap path | `ecall`, `a7 = 0x10000000 \| group<<8 \| slot` | trivial | In M-mode, `mcause` 11 and `mepc += 4`. Arguments in `a0`–`a5`, result in `a0`. There's one stub per call, so patching is also possible, but trapping leaves the code untouched |
| Console, time, random | `esSIOS_putstr`, `TimeGet`/`Time` (ticks), `esTIME_GetTime/Date`, `esKSRV_Random(max)` | trivial | **tick = 10 ms**: the module multiplies by 10. `Random` needs real entropy; it seeds crypto |
| Memory | `esMEMS_Malloc/Mfree/Realloc`, newlib `_malloc_r/_calloc_r/_realloc_r/_free_r` | trivial | 32-bit-clean addresses; thread-safe; 8-byte alignment (**inferred**, could be 16) |
| Threads | `TCreate`, `TDel(0xff = self)`, `TaskNameSet`, `GetTIDCur`, `TimeDly` | medium | **Short IDs** `(slot+1) \| (pid<<8)` from a 255-entry table, not pointers. Level 5 maps to prio 24 with a 15-tick slice |
| Semaphores, mutex | `SemCreate/Del/Pend/Post`, `MutexCreate/Pend/Post` | medium | `SemPend` **timeout 0 = forever**; µC/OS error codes (`OS_TIMEOUT` 10). `MutexPend` **ignores its timeout** |
| Queues | `QCreate/Del/Pend/Post/QAccept` | medium | **Word mailboxes** (32-bit messages), FIFO; `KRNL_Q_FULL` 30, `KRNL_Q_EMPTY` 31 |
| Files | `esFSYS_fopen/fclose/fread/fwrite/mkdir/remove`, newlib `_open_r` … `_isatty_r` | small | `e:\CarPlay\airplay.conf`, `default.keychain` (the pairing identity, which must persist or the phone re-pairs every time), icons under `d:\apps\CarPlay\`. Start with a RAM store; persist the keychain later to a NOR area we own (never OTP) |
| `/dev/twi2` (MFi) | `ioctl(fd, 3, i2c_msg*)`, one message per call (`0xe887bdb2`, `0xe887be32`) | small, with one unknown | The module treats any non-zero return as failure (`sltiu a0,a0,1`). Our bench work showed the chip NACKs reads while busy, and re-sending the register pointer restarts it. **Unknown:** whether stock's TWI driver retries NACKed reads internally, or the module's MFi layer re-sends write+read. Trace the kernel TWI ioctl before choosing |
| `/dev/spinor2` | ioctls 5, 6, 8 | small | Genuine reads (UID, OTP, RDID), with stock's offset-to-register mapping. **Read commands only** |
| `/dev/spinor2` | **ioctl 9** | **blocked** | See §1. The honest answer is an error, and that corrupts video |
| Sockets | none | none | The module brings its own lwIP (`CarApp/…/Libs/3rd/lwip`) and mDNSResponder. The host only moves Ethernet frames |

**Size:**
- the shim is about 400–600 lines of our own C (**guess**), clean-room (no
  `epos.c`);
- the trap entry, loader and LZMA hookup are about 300 lines;
- plus the RT-Thread core and the M-mode port.

## 5. The host side of `MIoctrl`

What the module expects, and where our code fits:

| Need | Module interface | Our code today | Notes |
|---|---|---|---|
| Role switch (`C0 53`, `40 51`), back as a device, gadget `0525:A4A1` iAP + NCM | none: in stock, `init.axf` and the kernel `udv` driver do this | **done**: `carplaytest.c`, `musb.c` | Unchanged under hosting |
| iAP2 link, auth, identification | **The module owns all of it.** Its iAP2 link layer is Apple's `iAP2Link.c` (strings `iAP2LinkThread`, `Invalid SYN Params detected`; the detect packet `FF 55 02 00 EE 10` at `0xe88a0100`). It does MFi through `/dev/twi2` | **done** in `carplaytest.c` | **Under hosting, our iAP2 link, auth and identification code drops out of the path.** We pass raw bulk bytes instead |
| iAP2 bytes in | `MIoctrl 13 {kind 0, ptr, len}`: raw bytes from the iAP bulk OUT endpoint | `musb.c` endpoint reads | Synchronous on the caller's thread |
| iAP2 bytes out | `iap2_send_cb(&{u32 flag; u32 ptr; u32 len})` = `args[2]` of cmd 1, into `lib_iap_init` (`0xe880e8ba`). Stock sends only if `flag == 0` and returns 0 or −1 | bulk IN on the iAP endpoint | Called on module threads, so it needs a lock |
| NCM in | `MIoctrl 13 {kind 1, ptr, len}`: **one Ethernet frame**, already unpacked from the NTB | NTB-16 parser in `carplaytest.c`/`ncmtest.c` | The module's lwIP does DHCP, IPv6 and mDNS |
| NCM out | `ncm_send_cb` = `args[1]`, into `lib_net_init` (`0xe88837bc`); same struct | NTB-16 builder (one datagram per NTB) | |
| Link status | none; stock keeps "kind 2" itself | our gadget state | Start cmd 1 only after `SET_CONFIGURATION`, as stock does |
| MFi chip | `/dev/twi2`, cmd 17 string | `twi.c` (`twi_timeout_us`) | See the shim table |
| Video out | events 0, 1, 2 (`w,h` / close / `ptr,len` AU, Annex-B **inferred**) | **none** | H.264 decode is a separate project; needed by both routes |
| Audio out | events 3, 4, 5 (open `u8,u8,c` / close / write PCM), on `AudioOutputThread`; a blocking write paces it | `audio.c` DMA voices | Must reopen at a new rate per stream |
| Mic, AEC | events 6–8, 12–14 | `adctest.c` in progress | Stub first |
| Touch, keys | cmd 8 `{state, x1, y1, x2, y2}`, cmd 12 `{key}` | `gt911.c`, `input.c` | Rotate for the portrait panel |
| Status, time | events 11, 16 | UI, RTC | |
| Config strings | cmds 9, 24 (sizes), 10 (fps), 14–18 | constants | **cmd 15 needs the unit's own licence string** (source untraced) |

**Summary:**
- **What we keep:** our USB gadget and NCM framing.
- **What becomes a pipe:** our iAP2 work. The module wants **raw iAP2 link
  bytes, not iAP2 payloads.**
- **What's still missing:** the threading glue, and the two heavy services
  (H.264 decode, and robust interrupt-driven USB).

## 6. The alternative: our own receiver

After `IdentificationAccepted` and NCM up, wired CarPlay is an AirPlay-style
session over IP. The public picture comes from xcertplay's sources (GPL-3.0;
protocol reference only, no code copied into this MIT repo), UxPlay,
shairport-sync, the openairplay spec and wiomoc/iap2:

| Piece | What it is | Crypto and format | Public source |
|---|---|---|---|
| IP link | IPv6 link-local (plus stock's static IPv4) on NCM | lwIP (BSD), `NO_SYS=1` fits our polling loop | lwIP |
| Discovery | Advertise `_airplay._tcp`; browse the phone's `_carplay-ctrl._tcp`; `GET /ctrl-int/1/connect` | mDNS | wiomoc `carplay_bonjour.py`; [carplay-iap2.md](carplay-iap2.md) §4 |
| Control channel | RTSP/HTTP server; `GET /info` (binary plist); `/command`, `/feedback` | bplist | xcertplay `AirPlayInfoPlist.kt`, `BplistCodec.kt` |
| `/pair-setup` | SRP-6a with user `"Pair-Setup"` and **fixed PIN `3939`**, then a long-term Ed25519 identity | SRP-6a 3072-bit / SHA-512, Ed25519, HKDF-SHA-512, ChaCha20-Poly1305, TLV8 | xcertplay `PairSetup.kt`, `Srp6a.kt`; openairplay spec (HomeKit pairing) |
| `/pair-verify` | Session keys | X25519, Ed25519, HKDF-SHA-512, ChaCha20-Poly1305 | xcertplay `PairVerify.kt`; UxPlay `pairing.c` |
| `/auth-setup` (MFi-SAP) | Proves the accessory is MFi. The phone sends its X25519 key. We reply with our X25519 key, **the chip's certificate**, and **the chip's signature** over a hash of the two keys (SHA-1 for MFi 2.0, SHA-256 for 3.0), AES-128-CTR-encrypted under keys derived from the shared secret | X25519, SHA-1/SHA-256, AES-128-CTR, plus our **existing** MFi chip code (607-byte cert, `0x21`/`0x10`/`0x12` signing) | xcertplay `MfiSapAuthSetup.kt` |
| Encrypted control | Control channel sealed after pair-verify | ChaCha20-Poly1305 | xcertplay `ControlCipher.kt` |
| SETUP / RECORD | Stream set-up: screen 110/111, audio 100 (main) / 101 (alt) / 102 (main high), data 130; timing | bplist, an NTP-like clock | xcertplay `AirPlaySession.kt`, `NtpClock.kt` |
| Screen stream | TCP. Each message is a 128-byte header plus either a clear `avcC` config or a ChaCha20-Poly1305-sealed frame (8-byte little-endian counter nonce), with length-prefixed NALs | ChaCha20-Poly1305; H.264 (HEVC optional) | xcertplay `ScreenStream.kt` |
| Audio streams | RTP. Advertise **LPCM only**: xcertplay lists LPCM formats (e.g. 48 kHz mono/stereo), so AAC-ELD/Opus decoders are avoidable (**inferred**: the phone honours the list) | ChaCha20-Poly1305 | xcertplay `CarPlayAudioFormats.kt`, `AudioStream.kt` |
| Touch | HID reports over the session (`hidSendReport`), with descriptors declared in `/info` | | xcertplay `AirPlayHid.kt` |
| iAP2 inside AirPlay | Tunnelled iAP2 (used wirelessly; wired keeps iAP2 on USB) | | xcertplay `IapTunnel.kt` |

**Undocumented:** Apple publishes none of this. Everything above is reverse
engineered. Apple's own AirPlay/CarPlay source is known to have leaked; it's
not used or linked here.

**What can change under us:** iOS updates. xcertplay is being maintained
(last push 2026-11-21) and reports working with current phones.

**Size estimate (guess):**
- **Reference sizes:**
  - xcertplay's `shared/` is 27k lines of Kotlin. Of that, `airplay/` is about
    5.1k, `transport/` 7.6k (we already have the equivalent), `iap2/` 2.9k
    (we already have most) and `mfi/` 1.2k (done).
  - UxPlay's core `lib/`, without llhttp, mDNS and FairPlay, is about 15k
    lines of C. That includes mirroring we don't need.
- **Our new C: about 6–10k lines.**
- **Libraries (all permissive):**
  - lwIP (BSD, ~20k lines);
  - for crypto, Monocypher (BSD-2/CC0: X25519, Ed25519, ChaCha20-Poly1305,
    SHA-512) plus BearSSL (MIT: SHA-1/256, AES-CTR, big-integer modpow for
    3072-bit SRP);
  - a small binary-plist codec.
- **CPU:** ChaCha20 at a few Mbit/s is a few percent of a 1 GHz C906
  (**guess**). SRP's 3072-bit modexp runs once per pairing.
- **No scheduler or MMU is needed:** lwIP's raw API runs in our existing
  loop.

## 7. Staged plans

**Shared first stage (both routes): V0, H.264 decode spike.** A FEL-run
benchmark of a permissively licensed software decoder, or the VE route, on a
recorded clip at 800×480 and 1280×720, no phone.
- **Bench test:** decoded frames per second, and one frame shown rotated on
  the panel.
- **This decides the video path either way.**

### Hosting (only if the owner clears ioctl 9; otherwise stop after H3)

| Stage | Work | Rough size | Bench test |
|---|---|---|---|
| H0 | M-mode groundwork: set `mxstatus` (`THEADISAEE`, `MAEE`), D-cache on (`mhcr 0x11ff`), CLINT 100 Hz tick, trap handler with `ecall` dispatch | ~200 lines | A tick counter on screen; the existing app still runs with the cache on (flush audit) |
| H1 | Scheduler: RT-Thread core plus an M-mode RV64 port (FP context). The app loop as a thread | ~400 lines of port plus the vendored kernel | A synthetic workload in the guest's shapes: 130 threads at one priority, 15-tick slices, sems with timeout-0-forever, word mailboxes, all through the `esKRNL` shim; overnight with no deadlock |
| H2 | Loader: read MINFS from NOR (`spinor.c`), LZMA-decode, relocate the 783 + 7 pointers, zero `.bss`, call `MInit` | ~300 lines + `LzmaDec.c` | `CARPLAY_MInit` arrives via `esSIOS_putstr`. The image hash matches `headunit decompress` of the same flash dump. Load at two bases and get the same output |
| H3 | `MOpen`; cmds 9/24/10/14–18; cmd 1 with stub transport; file store; `/dev/twi2`; spinor2 5/6/8 genuine, **9 returns an error** | ~600 lines | Module threads start. The MFi probe hits `0x11` then `0x10` (I2C log). The licence path logs `fd`/`uuid` lines. **Gate:** event 2 frames would be corrupted. Stop here pending the decision |
| H4 | Transport: cmd 13 kinds 0/1 from `musb.c`; send callbacks; locks; interrupt-driven USB thread | ~500 lines | The module's own iAP2 reaches `AuthenticationSucceeded` and `IdentificationAccepted` (USB log). NCM comes up and the module's lwIP answers DHCP/mDNS. Event 11 status codes appear |
| H5 | Session: events 0/2 counted, events 3/5 to `audio.c`, cmd 8 touch | ~400 lines | Phone audio is audible; AU count and sizes logged |
| H6 | Video: AUs into the V0 decoder, then the video layer | from V0 | First CarPlay frame on the panel |

Rough total, our code: about 2.5–3k lines, plus RT-Thread and the LZMA
decoder.

### Our own receiver

| Stage | Work | Rough size | Bench test |
|---|---|---|---|
| R1 | lwIP on our NCM (`NO_SYS`), IPv6 link-local plus stock's static IPv4; mDNS: advertise `_airplay._tcp`, browse `_carplay-ctrl._tcp`; `GET /ctrl-int/1/connect` | ~1.5k lines + lwIP | The iPhone opens a TCP connection to our AirPlay port. A Mac on `ncmtest` can test mDNS and HTTP first, with no phone |
| R2 | HTTP/RTSP server, bplist, `/info`, `/pair-setup` (SRP-6a / PIN 3939), `/pair-verify` | ~2.5k lines + crypto | The phone completes pair-verify. The SRP and pair-verify code is unit-tested on the Mac against published test vectors first |
| R3 | `/auth-setup` (MFi-SAP) with our chip code | ~300 lines | The phone moves on to SETUP |
| R4 | SETUP/RECORD; timing; screen stream decrypt and AU extraction; LPCM audio streams to `audio.c` | ~2k lines | Audio plays; AU count and sizes logged |
| R5 | HID touch, `/command`, `/feedback` keep-alive, teardown and reconnect | ~1k lines | Touches reach the phone UI (audible clicks, app switches) |
| R6 | Video: AUs into V0's decoder | from V0 | First frame |

Rough total: about 7–8k lines of our own C, plus lwIP, Monocypher,
BearSSL and the decoder. R1–R3 can be developed mostly against a Mac and
unit tests.

## 8. Recommendation

**Write our own receiver. Run V0 first.**

1. **Hosting can't reach a usable picture under the project's own rule.**
   The licence path ends in a kernel-held secret (ioctl 9), not in the flash
   chip. Done honestly, hosting gets as far as corrupted video. Everything up
   to H3 is real engineering we'd then discard.
2. **Our own receiver builds on what we already have.** Our role switch,
   gadget, iAP2 link, MFi authentication and identification stay on the
   path; under hosting, the iAP2 half of that would be replaced. It also
   avoids the S-mode/MMU question and the scheduler port, both of which touch
   every driver.
3. **The protocol risk dropped this autumn.** xcertplay shows the complete
   wired session working with a genuine MFi chip, and its sources name every
   message and key derivation.
4. **The heavy items are shared.** H.264 decode is needed by both routes,
   and USB throughput by both too. Neither favours hosting.

Fallbacks while ours is built:
- boot stock as "CarPlay mode", as today;
- Option A (our app on stock Melis), which passes the licence check
  genuinely.

If the owner later decides ioctl 9 is acceptable, the hosting plan above
stands and is smaller in our own code (~3k lines). The trade is a licence
dependency on the vendor kernel's secret, and a frozen Apple stack.

## Open items and the cheapest next step for each

1. **V0, the decode spike.** Decides video for both routes.
2. **Stock TWI NACK behaviour** (hosting only): static trace of the kernel's
   `/dev/twi2` ioctl 3.
3. **Stock spinor2 offset-to-register mapping** (hosting only): static trace
   of `0x40053e90`–`0x40053c4e`.
4. **Where cmd 15's string comes from** (hosting only): trace `init.axf`'s
   settings loader (`0xe918c1b2`).
5. **Does the phone accept an LPCM-only `/info`?** (own receiver): check
   xcertplay's defaults and logs, then the first R4 session.
6. **Interrupt-driven MUSB** (both routes): a variant of `ncmtest` that
   handles MUSB interrupts instead of polling, with the boot ROM's handler
   replaced by our `mtvec`.

## Sources

- **This repo:**
  - [carplay-host.md](carplay-host.md), [carplay-usb.md](carplay-usb.md),
    [carplay-iap2.md](carplay-iap2.md), [carplay-inventory.md](carplay-inventory.md),
    [legal-and-scope.md](legal-and-scope.md), `docs/devlog.md`;
  - `headunit/decompress.py`;
  - `tools/fel/dram/` (`musb.c`, `carplaytest.c`, `twi.c`, `spinor.c`,
    `display.h`, `dram.ld`).
- **Stock binaries** (the user's own; decompressed in scratch, not in the
  repo): `carplay.mod`, `kernel.bin`, `boot0`.
- **Melis D1s SDK** (reference only): `ekernel/arch/riscv/rv64gc/mmu/`
  (`module.c`, `mmu-sv39.c`, `pgtable.h`, `pgtable-bits.h`),
  `ekernel/arch/riscv/common/exception.c`.
- **RT-Thread:** https://github.com/RT-Thread/rt-thread (`bsp/allwinner/d1s`,
  `libcpu/risc-v/legacy-t-head/c906`, `libcpu/risc-v/legacy_common`,
  `.github/ALL_BSP_COMPILE.json`, issues #11718 and #10457); checked
  2026-11-27.
- **NuttX:** the `ox64` defconfig and `bl808_mm_init.c`
  (https://github.com/apache/nuttx); the allwinner-d1 page on
  nuttx.apache.org.
- **xcertplay:** https://github.com/shilapi/xcertplay (GPL-3.0; created
  2026-09-12, pushed 2026-11-21); its fork DiPlay.
- **AirPlay receivers:**
  - UxPlay: https://github.com/FDH2/UxPlay
  - shairport-sync: https://github.com/mikebrady/shairport-sync
  - RPiPlay: https://github.com/FD-/RPiPlay
  - openairplay spec: https://openairplay.github.io/airplay-spec/pairing/hkp.html
- **iAP2:** wiomoc/iap2, https://github.com/wiomoc/iap2 (`carplay_bonjour.py`,
  `car_play.py`).
- **Talks and papers:** Troopers 2024, "Apple CarPlay: What's Under the
  Hood"; USENIX VehicleSec 2025 on CarPlay adapters (background only).

## What gets the phone to connect (added 2026-11-27)

The bench is stuck after `IdentificationAccepted`:
- NCM is up;
- we answer `_airplay._tcp` over IPv6 link-local with stock's records;
- but the phone never sends a neighbour solicitation for us, never SYNs,
  and never advertises `_carplay-ctrl._tcp`.

This section is what the two public open-source receivers do at that point.
They're used as protocol references only (both GPL-3.0; nothing copied):
- **xcertplay:** https://github.com/shilapi/xcertplay, Kotlin, read from a
  clone on 2026-11-27;
- **LIVI:** https://github.com/f-io/LIVI, Rust, created 2025-05, pushed
  2026-11-27. xcertplay credits LIVI as its reference.

### 1. The step we're missing: `CarPlayAvailability` → `CarPlayStartSession` over iAP2

**Confidence: high that this is what both do; medium that it fixes our
role-switch path.**

Both declare two CarPlay messages in `IdentificationInformation` that
stock's layout ([carplay-iap2.md](carplay-iap2.md) §2) lacks:
- **`0x4301` CarPlayStartSession** in param 6 (MessagesSentByAccessory);
- **`0x4300` CarPlayAvailability** in param 7 (MessagesReceivedFromDevice).

xcertplay `Iap2IdentificationClient.kt`, lists `MESSAGES_SENT_BY_ACCESSORY`
and `MESSAGES_RECEIVED_FROM_PHONE`.

The phone then sends `0x4300`, and the accessory **answers every `0x4300`
with `0x4301`**, without checking the availability flag inside it
(`Iap2WiredControlClient.kt`, `CARPLAY_AVAILABILITY` branch).

**`0x4300` layout** (LIVI `native/crates/iap2-csm/src/messages/car_play.rs`;
the same in xcertplay `Iap2Messages.kt`):
- group 0 (wired): {0 available `u8`, 1 USB transport identifier `str`};
- group 1: wireless;
- group 2: theme assets.

**`0x4301` parameters:**

| ID | Content | xcertplay's wired value |
|---|---|---|
| 0 | group, wired: {0 IP address string, repeated} | `fe80::2`, its link-local, with no `%scope` |
| 1 | group, wireless (SSID, passphrase, channel, IPs, security) | omitted on wired |
| 2 | u32, AirPlay port | its receiver port |
| 3 | string, device identifier | the NCM MAC as `aa:bb:cc:dd:ee:ff` |
| 4 | string, public key | its Ed25519 public key in hex (the same value as the `pk` TXT key) |
| 5 | string, source version | `"950.7.1"` |
| 6, 7, 8 | sdk version, cluster asset, mutual auth | optional, unset |

In xcertplay's wired path, **no mDNS runs at all.** `CarPlayBonjour` is only
started for wireless (`CarPlayController.kt` around line 1010). The phone
learns the address and port from `0x4301` and connects to the receiver
itself. So the old flow (phone advertises `_carplay-ctrl._tcp`, accessory
sends `GET /ctrl-int/1/connect`; stock and wiomoc) isn't what current
public receivers rely on.

**Our phone's behaviour fits this.** It queries `_airplay._tcp` but never
connects or advertises `_carplay-ctrl`, and stock's identification
declares neither `0x4300` nor `0x4301`. So the phone may simply never get a
start-session (**inferred**).

**The other things xcertplay sends after identification:**
- `AE03` PowerSourceUpdate;
- five subscriptions: `5000` (media group {1,4,6,12,26}, playback group
  {0,1,2,3,7}), `5200`, `AE00` (params 4/5/6), `4157` ({0,4,5}) and `4154`
  ({0,1,2,3,4,11});
- `6800` StartHID, declaring a media-remote HID component (identification
  param 18).

**Its identification also differs from stock's:**
- param 8 = 2;
- param 9 = 20;
- param 10, an EA protocol (its own name);
- the transport group 16 named `"USBHostTransport"`, with flags 2 and 4 and
  the CarPlay interface number.

None of these is shown to be required. `0x4300`/`0x4301` is the
CarPlay-specific difference.

**The big caveat: both projects use a different wired topology.** They are
the **USB host**: no role switch. They pick the iPhone configuration that has
usbmux plus NCM (`IphoneCarPlayConfiguration.kt`; our devlog's
configuration 3/4 family), run iAP2 over **usbmux → lockdown →
`com.apple.carkit.service`** (xcertplay `LockdownCarKitClient.kt`; LIVI
`iap2-wired/src/carkit.rs`), and use the phone's own NCM function. Group 16's
interface number there is the iPhone's NCM interface.

No public source shows `0x4300` arriving over the role-switched,
accessory-as-device path we use. **Whether the phone sends `0x4300` to a
role-switched gadget is unverified.** The cheapest test is to add the two IDs
to params 6 and 7 and log what arrives.

### 2. mDNS and NCM details

**Confidence: high for what the code does.**
- **Wired: xcertplay has no mDNS.** It's IPv6 only: link-local `fe80::2`,
  with neighbour discovery left to the Android kernel (`Ipv6NcmBridge.kt`,
  `CarPlayVpnService.kt`). There's no IPv4, no DHCP server and no IPv4
  link-local.
- **Device ID:** the MAC read from the iPhone NCM descriptor
  (`NcmUsbBridge.readNcmHostMac`) is used as the `0x4301` device identifier.
- **Wireless TXT** (`CarPlayBonjour.kt`): keys include `deviceid`,
  `features=0x44540380,0x61`, `srcvers`, `pk` and `model`.
- **Wireless connect direction:** the receiver browses `_carplay-ctrl._tcp`
  and sends a connect probe. So the receiver connects first in that mode
  too.

### 3. User-facing steps on the phone

| Step | Confidence |
|---|---|
| **Host-mode path:** lockdown pairing shows **"Trust This Computer?"** once; LIVI's log says "confirm the trust dialog on the phone" (`carkit.rs`, `pair_device`). The pair record is saved and reused. **Doesn't apply to our role-switch path**, which uses no lockdown | high |
| **xcertplay's usage notes** ask for Bluetooth pairing first, but that's for wireless | high |
| **The "Use CarPlay?"/"Allow CarPlay while locked" prompt** and the per-car entry in Settings > General > CarPlay: neither project's code triggers either explicitly. iOS presumably shows them when it first starts a session with a new accessory, i.e. after `0x4301`/the AirPlay connection. That we've never seen a prompt is consistent with the phone never deciding to start CarPlay. A car previously set to "don't use" in Settings would also block it | **guess** |

### Next bench step

1. Add `0x4301` to identification param 6 and `0x4300` to param 7.
2. On `0x4300`, send `0x4301` with our link-local address, `7000`, our MAC
   string, our Ed25519 public key in hex, and a source version string.
3. Have a TCP listener on port 7000 ready.
4. If `0x4300` never arrives on the role-switch path, the fallback is the
   host-mode topology both public projects use: usbmux/lockdown/carkit. That
   needs a usbmux client and a one-time Trust prompt on the phone.
