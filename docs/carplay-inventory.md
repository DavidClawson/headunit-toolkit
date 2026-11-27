# CarPlay / Android Auto module inventory (2026-11-27)

This is static analysis of the stock `carplay.mod`, `auto.mod` and
`wireless.mod`, plus the vendor UI app (`init.axf`) that drives them. It answers
the question [strategy.md](strategy.md) poses: what would it take to keep these
modules under firmware of our own? Syscall names come from Allwinner's public
D1s Melis SDK, used only as a reference. The tables are in
[`reference/carplay/`](reference/carplay/). Static analysis only; **nothing here
is tested on hardware yet**.

## Verdict

**The modules are closed boxes with a narrow interface. The heavy lifting is
done by their host.**

- **52 kernel services cover all three modules, and all of them are the easy
  kind:** threads, semaphores, mutexes, queues, delays, time, memory, files,
  random, and newlib-style `open/read/write/ioctl`. They never touch
  esMODS, esDEV, interrupts, pins, clocks, DMA or power management.
- **No direct hardware access:** a scan of all three found no MMIO.
- **No calls into other modules,** and no calls into fixed outside addresses,
  with one exception: `wireless.mod` is handed a 51-function socket table by its
  host.
- **Everything heavy lives in the host:** USB, H.264 decode, display layers,
  audio and WiFi. The host (`init.axf` on the Melis kernel) passes the module a
  set of callbacks.

**Decision: Option A now, Option B kept open.**
- **Option A:** keep Melis, which already has the USB, video-decode, display
  and audio drivers, and replace `init.axf` with our own app that implements
  the module contract below.
- **Option B:** host the modules on our own OS. The narrow interface means this
  can come later, one subsystem at a time.

## What the modules need

| Category | Count | Detail |
|---|---|---|
| Kernel services | 52 | per-module lists in `reference/carplay/syscalls_*.csv` (`init.axf` itself uses 82) |
| Device files | 3 | `/dev/twi2` (only `I2C_RDWR`, for the MFi chip), `/dev/spinor2` (4 ioctls), `/dev/spinor` (UUID; wireless only) |
| Files | a few | `e:\CarPlay\airplay.conf`, `default.keychain`, `d:\apps\CarPlay\icon_*.png` |
| Loader | 1 | load the ELF sections at their fixed addresses and call the `ePDK.mod` interface (MInit/MOpen/MIoctrl) |
| Heavy host services | 4 | USB transport, H.264 decode + video layer, audio play/record/echo-cancel, WiFi + sockets (wireless only) |

## Host contract (from `init.axf`)

**Start-up:** `esMODS_MInstall("d:\mod\carplay.mod")`, `MOpen`, then MIoctrl
commands 9 (video size), 24 (screen size), 15, 17 (I2C device name), 14, 18,
16, then 1 (init, which registers three callbacks).

All 25 commands are in `reference/carplay/carplay_mioctrl.csv`. The main ones:

| Command | Meaning |
|---|---|
| 2 / 3 | start / stop |
| 5 | hide |
| 6 / 7 | iAP attach / detach |
| 8 | touch (two points) |
| 12 | key |
| 13 | USB data in (flag 0 = iAP2; anything else = an NCM Ethernet frame for the module's own lwIP stack) |
| 19 | start audio |
| 21 | night mode |
| 22 / 23 | disconnect / reconnect |
| 25 | audio state |

`auto.mod` has 22 commands in the same style (`lib_auto_*`). Its USB side is
Android's accessory protocol (AOA), run through the same host USB driver.

**Events from the module to the host,** via the callback registered with
command 1 (full list in `reference/carplay/host_event_callback.csv`):

| Type | Event | How the stock host handles it |
|---|---|---|
| 0 / 1 / 2 | H.264 decoder open (w, h) / close / access unit | cedar.mod raw-stream mode → `vdecoder.drv` → display video layer |
| 3 / 4 / 5 | audio out open / close / write | `/dev/audio_play6` (and `play1`) |
| 6 / 7 / 8 | mic open / close / read | `/dev/audio_rec4` / `rec0` (and `rec1`) |
| 12–14 | echo cancellation | `/dev/audio_aec` |
| 11 | status codes | UI messages |
| 16 | set the clock from the phone | RTC (inferred) |

**USB:** the host owns `/dev/udv`, a vendor USB driver in the Melis kernel.
ioctl 200 registers a receive callback, which the host forwards to MIoctrl 13;
ioctl 201 sends. The host/gadget role switch and the USB stack live in the
kernel.

## Authentication and a licence check

- **Apple MFi chip:** `carplay.mod` opens the I2C device named by command 17
  (`/dev/twi2`) and uses `ioctl(I2C_RDWR)`. It probes address 0x11, then
  **0x10**, which is where our bus scan found it.
- **Vendor licence tied to the flash chip:** all three modules read the SPI
  NOR's unique ID and OTP area through `/dev/spinor2` (ioctls 9, 8, 5, 6). They
  appear to check these against a licence string the host passes with command 15
  (inferred).
  - **Option A:** this passes untouched, because it's the real flash chip and the
    host's real licence string.
  - **Option B:** these ioctls would have to be answered from the real chip, by
    passing them through.
  - **Our own flash writes must never touch the OTP area.** They don't today.

## Open questions

- Where command 15's string comes from (Config.ini or flash), and the exact
  check it feeds.
- Commands 14, 16, 18 and 20, and the remaining `/dev/udv` ioctls (7, 100–102,
  300/301, 14).
- How much of `init.axf`'s CarPlay glue (video layer setup, audio routing, touch
  scaling, iAP attach flow) a replacement app needs before a phone shows its UI.
  This is the first bench milestone for Option A: our app loading `carplay.mod`
  on stock Melis.
