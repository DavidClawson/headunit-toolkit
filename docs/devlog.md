# Dev log

Dated, honest notes from real work on the bench — what we tried, what worked,
what didn't, and the lessons. Newest entries first. Nothing here is firmware;
it's reverse-engineering facts and test results from a unit we own.

---

## 2026-11-26 (night) — front-panel keys come from the MCU

- No F133 GPIO or key-ADC activity for any button (after opening the reset
  "I/O disabled" pins as inputs; in that state DAT always reads 0, which hid
  pins from the first watch).
- The MCU sends `02 81 B2 06 00 KK 00 00 00 00 XOR` per press on UART1 at
  115200: knob push `01`, knob CW `04`, CCW `05`, phone `06`. Decoded the app's
  `KK` → key-ID table. Home produced nothing anywhere in FEL.
- Touch didn't register in this session (GT911 answers on I2C); not chased.
- **First transmit to the MCU:** `A0` from FEL got a full handshake back,
  including its firmware string `AB5301_20260106_2125` (the MCU is a Bluetrum
  AB5301) and BT status lines (name `MMCAR BT`). It then pings `B2 CE` every 3 s,
  and ~2.5 min later the F133 was reset, apparently for not answering. Home
  still sent nothing; also not on PF0–PF6 or PC0–PC1.
- My mistake along the way: running two xfel tools at once makes both report
  "No FEL device found", which looked like the unit had reset. It hadn't.

---

## 2026-11-26 (late) — FEL without the clip: a watchdog-guarded window in boot0

- **Stock boot0 already has a "jump to FEL" path** (RTC GP2 = `0x5AA5A55A`,
  set by the Melis debug-shell `reboot efex`), but nothing reachable sets it.
- **Patch:** a 194-byte stub in boot0's padding makes every boot enter FEL with
  the watchdog armed (3 s) and an RTC GP3 "window used" flag set. If the Mac
  (`fel-catch.sh`) disarms it, the unit stays in FEL; otherwise the reset
  boots stock. Written over USB with `xfel spinor write`, verified by readback.
- **Snag 1:** the boot ROM's FEL entry disarms the watchdog itself, so the
  first version sat in FEL forever. Fix: enter the boot ROM just past that
  write, after repeating its CPU setup.
- **Snag 2 (my bug):** felrun's "DRAM ready" marker at `0x44F80` sat inside the
  boot ROM's FEL stack (top `0x44FFC`) and hung FEL twice. Moved to `0x40FF0`.
- **Verified:** uncaught window → stock UI; 12 V power cycle → caught; DRAM
  memtest 0 errors; `fel-boot-stock.sh` → stock UI without a power cycle.
- Seen again: `mcycle`'s upper word jumping (5.9 GHz "rate" in `clock`).

---

## 2026-11-26 (evening) — DRAM up over FEL; a C program kit with FEL as the console

- **Debug UART is a dead end on this board:** PB6/PB7 (legs 17/18) appear to
  have no traces, and continuity probing found nothing connected to them.
- **DRAM:** `xfel ddr f133` (its parameter table matches `sys_config`
  `[dram_para]` field for field). `tools/fel/dram/memtest`: 62 MB, data bus,
  address, 0x55/0xAA patterns, **0 errors** in ~4.5 s, repeated across power-ups.
- **CPU clock after DRAM init: 1008 MHz** (xfel's payload raises PLL_CPU from 408).
- **`mcycle` is unreliable for timing here:** its upper word jumped by 5–7 × 2^32
  during DRAM-heavy runs (decoded from impossible timings: 34.3 s and 25.8 s
  reported for 4.5 s runs, and one 0.87 s loop reading as 26.6e9 cycles). A test
  that spun on it hung FEL once. Timing now uses the 24 MHz TIMER1, which agreed
  with the host clock to within rounding every time.
- **Kit:** `tools/fel/dram/` (start code that returns cleanly to FEL, `logf`,
  `udelay`, `ticks`) and `tools/fel/felrun.py` (DRAM init once per power-up,
  load, run, print the log). `clip-wait.sh` chimes when a reseated clip makes contact.

---

## 2026-11-26 — FEL over the front USB port works; clip-free recovery verified

- **Why:** hand-soldering to the 0.4 mm UART legs was doing more harm than good
  (bridges cleared with braid; unit boots again). New approach: a USB link to the
  SoC for bring-up, then a clean-sheet firmware with a watchdog fallback.
- **Entering FEL:** `tools/fel/fel-enter.sh` backed up the chip with the clip
  (two matching reads) and erased only the first 4 KB (boot0 header). Unit
  powered with the front USB port cabled to the Mac (VBUS cut).
- **Result:** `AWUSBFEX ID=0x00185900(D1/F133)` 9 s after power-on, so the front
  port reaches USB0 directly. `probe.bin` ran from SRAM at `0x40000` and returned
  to FEL twice (T-Head `mvendorid` `0x5b7`, hart 0, `misa` `0x8000000000b4112d`).
  `xfel spinor` reads the flash at about 400 KB/s.
- **Recovery without the clip:** boot0 header written back with
  `xfel spinor write`, full 16 MB read over USB matched the backup, unit booted
  normally.
- **Side note:** the pre-FEL backup differed from the 11-25 `after.bin` only
  from `0xf20000` up (UDISK settings), as expected after normal use.
- **Backlight lit from FEL** (second session, same day): `tools/fel/backlight.sh`
  drove PG15 (`lcd_bl_en`) and PD22 (PWM7) high as plain GPIOs; the screen lit
  and blinked on command. The `sys_config` `[lcd0]` pins are right, and no extra
  power rail was needed. First try used the wrong offset for PD_CFG2 (wrote
  PD_DRV1); fixed.
- **Hardware PWM dimming works:** `tools/fel/pwm-backlight.sh` opened the PWM
  bus clock (`0x020017AC`), set channel 7 to 24 MHz / 480 = 50 kHz active-high,
  switched PD22 to function 5, and swept the duty 0 → 100 → 0. The backlight
  dimmed smoothly. Register layout matches Linux `pwm-sun20i.c`, so the
  D1-family drivers can be reused for this board.
- **I2C + touch over FEL:** `tools/fel/i2c.c` (SRAM payload) + `i2c.py` (host).
  TWI3 (PG10/PG11) has the touch controller at `0x5d`: Goodix GT911, firmware
  `0x1060`, config 1024 × 600. Live touches read back correctly; the raw frame is
  landscape (x left→right, y top→bottom), so the 720 × 1280 panel is mounted
  sideways. TWI2 (PE12/PE13) has three more devices: `0x10`, `0x40`, `0x44`.
- **TWI2 identified:** `0x10` = Apple MFi auth coprocessor (`Config.ini`
  `carplayI2cName=/dev/twi2`, registers read zero), `0x40` = sound processor,
  write-only, likely ROHM BD3702 (app driver `CAudioBD3702`), `0x44` = Techpoint
  TP2850 reverse-camera decoder (chip ID `0x2850`, kernel driver `tp2804`).
  First guesses from addresses alone (QN8035 tuner, PT2312) were wrong, and an
  early read hit the wrong bus (tool defaulted to bus 3; `--bus` now required).
- **Tuner is on TWI1** (`radioI2cName=/dev/twi1`, PG14/PB04) and doesn't answer
  in FEL. Also `bHaveRds=0` in this unit's `Config.ini`.
- **App GPIO numbering traced** (`init.axf`): `SetGpioData` (`0xe91a4a54`) →
  `0xe9193016` splits the number as **`n / 100` = port, `n % 100` = pin**, and
  `0xe9192e74` fills Allwinner's `user_gpio_set_t` with `port = n/100 + 1`
  (1 = A). So `radioResetGpio=105` = **PB5**, not PD9 (a DSI lane, which is why
  it wasn't guessed at). `usb0PowerGpio=318` = PD18, `frontAuxSwitchGpio=319` =
  PD19. The reset getter is `0xe91ae27c` (config + `0xB9C`); all five tuner
  drivers pulse it low ~20 ticks then high, so it's **active-low**.
- **Released PB5 from FEL** (low pulse, then high): TWI1 still empty, with both
  bus lines idle high.
- **(Corrected below: this conclusion was wrong.)** **Why: the radio isn't an I2C tuner on this build.** Photos of the bench board
  show the 10-hole `ANT` tuner-module footprint **empty**, and the chip by the
  antenna can is a **Bluetrum ("AB") `CHXC14B5B`**, LQFP48, 26 MHz crystal
  (`YL260`). `Config.ini` has **`bArmRadio=1`** (the vendor calls the companion
  chip "ARM": `[ARM] armVolGain`, `armChannelA`), and `init.axf` contains
  `AB5301_`. So the radio most likely runs on the Bluetrum (Bluetrum audio parts
  commonly integrate FM), controlled over a UART, and the TWI1 tuner path and
  PB5 reset are only used on builds with a tuner module. The radio works on the
  owner's other, identical unit.
- **UARTs from `Config.ini` + `sys_config`:** `mcuSerialName=/dev/uart1` (PG12 TX /
  PG13 RX, func 7; `mcuChip=2`), `btSerialName=/dev/uart4` (PB02 TX / PB03 RX,
  func 7; AT commands like `AT#VN030606`). Which of these carries the radio
  commands isn't confirmed yet.
- **Listened on both from FEL** (`tools/fel/uart_sniff.py`, an edge-timestamping
  payload at 0.2 µs resolution): silent for 5 s each, and all four lines are held
  high externally (they stay high with the SoC's pull-down on). So the links are
  wired but command/response: the Bluetrum speaks only when spoken to. Next is
  reading the app's MCU protocol code and sending a harmless query.
- **Correction, from tracing the app:** `bArm*` flags mean the **F133** ("ARM")
  handles a feature instead of the external MCU. With `bArmRadio=1` the F133
  runs its own tuner drivers over I2C (`radioI2cName=/dev/twi1`); the MCU radio
  commands are the `bArmRadio=0` path. So the radio is *not* on the Bluetrum. The
  empty TWI1 in FEL most likely means the tuner is unpowered there. Also mapped:
  the MCU UART protocol (115200 baud, `02 80 CMD LEN DATA XOR`), its command
  set, and a separate MCU firmware-update mode. See
  [mcu-protocol.md](mcu-protocol.md).
- **FEL CPU clock ≈ 408 MHz.** One self-inflicted hang: `xfel read32` of the
  CLINT timer (`0x1400BFF8`) froze FEL; a power-cycle brought it back in FEL.
- **Next:** tuner reset pin, then panel init over MIPI-DSI,
  then a boot selector with watchdog fallback. Plan in
  [boot-and-recovery.md](boot-and-recovery.md#plan-clean-sheet-firmware-with-fallback).

---

## 2026-11-25 — hello.mod bricked the bench unit; recovered with a SPI clip

- **What happened:** the first `hostmod` test image (`hello.mod` in the
  `/mod/willow/readme.txt` slot, plus an `insmod` line in `/startup.sh`) flashed
  fine, then the unit hung at the boot splash on every power-up. Reflashing the
  stock image from a USB stick did nothing: the updater lives in the app, which
  never started. Cause still unknown (no serial log).
- **No SD slot.** The manual shows one, but this W022 has none fitted, so card
  recovery (PhoenixCard) was out. No labelled UART/FEL pads either.
- **Recovered** with a SOIC-8 clip on the XMC XM25QH128D flash and a Pico
  running `pico-serprog` with `flashrom` on the Mac. Two identical 16 MB reads,
  then the data partition was replaced with stock (only 3 × 4 KB blocks differed).
  A partition-only write verified and the unit booted normally.
  Full procedure: [boot-and-recovery.md](boot-and-recovery.md).
- **Debug UART located:** PB6/PB7 = F133 pins 80/79, the 17th/18th legs from
  the left on the top edge; the core-supply leg next to them (0.926 V) confirmed
  the orientation. A hand-soldering attempt bridged four legs (unit unpowered,
  to be cleaned up). Lesson: don't solder at 0.4 mm pitch without the right
  tools; probe with a needle, find a via, or use a repair shop.
- **Lessons:** back up the flash *before* running native code; never put
  untested code in the boot path without a skip switch; emulator passing ≠
  hardware passing.

---

## 2026-11-24 — First bench session: pipeline + two mod lanes validated on hardware

Unit: a second **W022** (badge **JOY-W022**), no speakers, driven entirely by
USB-stick firmware updates. Draws ~0.75 A @ 12.8 V idle.

### ✅ What works (validated on the live unit)

- **The whole USB-update pipeline.** build → MINFS repack → checksum fix →
  copy `Update/` to a FAT32 stick → unit auto-enters upgrade on insert → ~2 min →
  reboots itself. CarPlay/Android Auto stayed intact (phone paired, projection
  app worked) after flashing — confirming resource repacks don't disturb the
  licensed modules.
- **Custom boot splash (image lane).** Replaced the boot `logo1.jpg` with a
  generated "MODDED ✓" image; it showed on power-on. `logo1` is confirmed the
  boot-splash slot.
- **Menu relabels / translations (string lane).** Renamed the launcher "Logos"
  tile → "HELLO" (same length, in place) and "F-CAM" → "Backup Cam" (longer,
  forced a relocation). Both showed. So the repacker's **in-place *and*
  relocation** paths are both honored on-device.

### ⚠️ Lessons (the hard-won part)

- **The firmware code-draws its prominent, dynamic UI.** Editing `DATAV1` widget
  attributes (color/geometry) on dynamic widgets does **not** change the screen —
  the app (`init.axf`) repaints them at runtime. Confirmed by moving+recoloring
  the radio frequency (`Radio.data/ViewShowStrMainFreqNum`): the bytes changed,
  the screen didn't. The launcher is the same story — `MainApp.data` has **zero**
  editable widgets; the tiles' icons and labels are painted in code. **Re-skins
  therefore go through images, not widget edits.**
- **A label string lives in several files — edit the one the shown screen reads.**
  The launcher grid reads `Main.txt` / `SystemBar.txt`, **not** `Setup.txt`
  (editing Setup.txt's copy did nothing). Files are 34-col UTF-16LE TSV, BOM +
  CRLF, English = column index 2; a label often repeats across the language
  columns of its row (edit only col 2).
- **Labels clip to a fixed field width.** "Backup Cam" rendered as "Backup…".
  The clip width is a code constant (the tile text box is code-drawn), so
  longer translations must fit the space — widening the field would need an
  `init.axf` patch (code-tier, not a resource edit).
- **Each flash is a complete image, not a patch.** Every build starts from
  pristine `stock/` and applies only that build's edits; a prior relabel reverts
  unless re-included. Stack edits into one build to keep several at once.
- **The reset-hole step is unnecessary** on this unit — it auto-restarts after a
  flash. A 123 GB stick worked despite the vendor's "≤32 GB" guidance.

### Factory menu + resilience

- **Factory menu is installer-locked.** Settings → Factory Set → the stock
  default password **113266** is rejected ("invalid") — the installer changed it
  and stored it persistently (USB resource-flashes don't touch the config area).
- Of the five hardcoded 6-digit codes clustered in `init.axf`
  (`001106 / 112345 / 230762 / 123579 / 666666`), **`666666` opens a hidden
  "在线升级 / Online Upgrade" (OTA) screen.** With no network it blocks on a server
  call and freezes the UI (touch + buttons dead). **A power-cycle fully recovered
  it — no brick.** Worst case from a whole session of resource/menu experiments
  was a 5-second reboot: good evidence the flash-and-go path is low-risk.
- **Side-finding:** the firmware has a built-in **networked OTA** client. Relevant
  to the Bluetooth/WiFi network discussion and the common "I wish it had OTA"
  complaint — worth a future look (what server, is it configurable?).

### Takeaways for the project

- The two **flash-and-go** lanes that matter most — **custom images** and
  **menu relabels/translations** — are proven. They directly answer the loudest
  user complaints (ugly splash, confusing/poorly-translated menus).
- Teardown / NOR backup is **not** needed for any of this; it's a developer
  safety net for the *riskier* code-tier R&D (native modules, boot chain), so
  end users never open the case.
- The browser configurator (`web/`) can now ship these lanes with confidence —
  they're hardware-validated, not hypothetical.

---

## 2026-11-24 — Milestone 1 kickoff: `esMODS` trailer-table crack (bench session, no unit)

Offline RE against `stock/LTTF133.img`. Goal: map the module loader ABI
([lua-host.md](lua-host.md) milestone 1 — `esMODS_Install` contract, the
`auto.mod` descriptor/trailer format) well enough to emit our own loadable module.

### What was tried

- **The magic-split LZMA1 framing is confirmed structurally sound** (3-block `init.axf`,
  `auto.mod`), but **every** payload — including the working blocks — decodes with
  `needs_input` still true and a leftover tail. `auto.mod` block 0 (316,460 B stored)
  produces 521,207 B; the raw stream never reaches eof on its own.
- **The tails are not noise — they're the Melis symbol table.** `aenc.plg` ends in
  `r! nAbsType[%x] is not support!`, `d:\mod\cedar\aenc_pcm.drv`, `PDK.mod`,
  `eLIBs_fllseek` — the module export/reloc table, **uncompressed**, after the LZMA
  stream. `carplay.mod` block 2 has a high-entropy tail instead — so there are two tail
  styles, and the container is subtler than `[stream][table]`: no constant relates
  `orig = out + tail` (deltas ran −79…+317 across files).
- `charset.mod` is its own container variant: a 36-byte `26 00 40 e4`-prefixed header
  before the LZMA props; decodes to 2 bytes. Not cracked.
- Brute-force probing (per-block decodes, dict-size and props sweeps, tail-as-second-stream,
  FORMAT_ALONE, length-prefix theories) exhausted; all single-block-at-a-time hypotheses
  failed. The `orig`/`out`/`tail` relationship doesn't close, so this is now a
  **structural** problem: parse the ASCII tails as record tables (the `xx 05 f0 e4`-style
  u32 records around `PDK.mod`/`eLIBs_` strings) and match them against what
  `esMODS_Install` consumes — likely in a Ghidra session, not more Python probing.

### Findings worth keeping

- **The symbol table is in the trailer, uncompressed.** `aenc.plg`'s trailer (383 B) has
  `mpl_AencCom_newAdrvInfoChild`, `d:\mod\cedar\aenc_pcm.drv`, `PDK.mod`,
  `eLIBs_fllseek`, `%s:%04u` format strings, `40 e4`-flavored u32 records — first
  direct evidence of the module ABI's export/reloc layout.
- **Working symbol-table offsets in `init.axf`**: the loader's module-path pointer pool
  at `0x1583ec..0x158704` is where the `d:\mod\*.mod` names live (per
  [re-notes.md](re-notes.md)) — cross-referencing a trailer record against that pool
  is the next structural step.
- **Per-file trailer summary** (stream end / tail / ASCII tail start), all reproducible:
  `ldec_itx.plg` streams whole-file (no tail, out=21,312=orig exactly — the
  cleanest sample); `aenc.plg` stream ends at 2265, tail 383 (symbol table);
  `ldec_lrc.plg` 5222/466; `muxer.plg` 6704/712; `ardr_hw.plg` 1591/233.
  `auto.mod` blocks end 316,460/90532/10560 with out 521,207/329,875/69,117.
- Every `.plg`/`.drv` trailer observed ends in a **72-byte high-entropy record block**
  after the ASCII strings (`xx 05 f0 e4 00 00 00 00`-style u32 pairs) — that's the
  trailer's own structure, distinct from the strings.

### Status

`format-minfs.md` updated with these findings. Milestone 1 remains open at the
"crack the `.plg`/`charset.mod` containers" step; the Ghidra RE (loader ABI + trailer
tables) is the unblocked path.

---

## 2026-11-24 (later) — Milestone 1 cracked: the public D1s Melis SDK documents the whole module ABI

**Supersedes the "trailer-table" theory above.** Allwinner's Melis 4.0 source for the
D1s (F133 is the same die) is public — e.g. the `weidongshan/D1s-Melis` mirror on
Gitee. It contains `livedesk/beetles` (the exact framework path leaked from our
`init.axf`), the module loader (`ekernel/legacy/mod_install/`), the MINFS driver
(`ekernel/filesystem/fs/minfs/`), and a minimal sample module (`emodules/mod_test`).
Used here **as a reference only** — nothing from it is vendored into this repo
(Allwinner copyright, no clear licence; see [lua-host.md](lua-host.md) clean-room caveat).

### What it settled (all verified against `stock/LTTF133.img`)

- **MINFS stores modules as ELF *sections*, not one stream.** A dentry with
  `Attribute & 2` (MODULE) carries `ExtentLen` bytes of 32-byte section headers
  after its 4-aligned name: `Offset, RecSize, RecUnPackSize, Size, VAddr, Type, Flags,
  Attribute` (attr bit0 = magic section, bit1 = LZMA). Each section is
  **LZMA-compressed independently** (5-byte props + data, no end marker, unpacked
  size from the header — hence the old "`needs_input` never clears" symptom). The
  "tails" we chased were just the next section's bytes.
  → **All 170 sections across all 41 modules decode byte-exact**, including
  `carplay.mod`, `vdecoder.drv`, and every `cedar/*.plg` that used to be `complete=False`.
- **Modules are fixed-address, not PIC.** System modules load at their link
  address: `VA = 0xe0000000 + type × 1 MiB` (e.g. `carplay.mod` type `0x88` →
  `0xe8800000`, `auto.mod` `0x89`, `init.axf` `0x91`, `cedar.mod` `0x94`). The earlier
  "link base `0xe90e2e04`" was an artifact of flattening sections; `init.axf` `.text`
  is at **`0xe9100000`**.
- **The descriptor is the `ePDK.mod` magic section** (VA `0xffff0000`, 80 B):
  `magic[8], u32 version=0x01000000, u8 type, u32 heapaddr=0xF0000, u32 heapsize=0x400,
  mif{MInit, MExit, MOpen, MClose, MRead, MWrite, MIoctrl}` (64-bit pointers). Every
  stock module uses exactly the SDK's `mod_test/magic.c` template values.
  `esMODS_MInstall` maps PROGBITS/NOBITS(ALLOC) sections at their VAs, zeroes bss,
  then calls `MInit`. **There's no relocation table and no signature.**
- **Syscall ABI matches the SDK.** Kernel calls are `ecall` with
  `a7 = 0x10000000 | (group<<8) | slot` (the SDK's `CONFIG_KERNEL_USE_SBI` branch of
  `kapi.h`). Mapping every `ecall` in `init.axf`/`auto.mod`/`carplay.mod`/`cedar.mod`
  through the SDK's `SWIHandler_*_t` tables gives coherent names
  (`esFSYS_fopen/fread/fseek/mkdir/remove`, `esCFG_GetKeyValue`, `esKRNL_TCreate`,
  `esMODS_*`, `esMSTUB_*`), with only artifacts of my disassembly tracker left unmapped.
- Type IDs are a fixed table (`include/melis/emodules/mod_defs.h`); `0x75` =
  `EMOD_TYPE_DRV_SAMPLE` is an unused, collision-free slot for a test module.

### What this means for the Lua host

Milestone 1 is effectively **done on paper**. Building a module is now a plain recipe:
`riscv64-elf-gcc` (Homebrew), link at `0xe0000000 + type<<20` with the magic struct in
a `.magic` section at `0xffff0000`, write our own `ecall` stubs from the SDK's numbering,
then convert ELF → MINFS section format (our own Python; Allwinner's `tools/packtool/minfs`
is a Linux i386 binary we don't need).

### New gap found

The repacker can **replace** files but not **add** one. Loading at an absent slot
(`oak.mod` etc.) needs a new root-dir dentry *with a section extent*, which means
growing the directory. That's the next toolkit feature, along with teaching
`headunit decompress` the section-aware path (replacing the magic-split heuristic).

---

## 2026-11-24 (later still) — Toolkit catches up: section-aware decode, true MINFS root, `startup.sh`

### Shipped in the toolkit

- **`minfs.py` parses module section tables** (`Node.sections`, `Section`) and
  refuses to `repack` a module as a flat file.
- **`headunit decompress` rewritten around sections**: all **63 modules / 331
  sections** decode exactly; `--elf` emits an ELF with every section at its vaddr
  (Ghidra needs no base address). The magic-split heuristic is gone.
- **MINFS root now comes from the superblock** (root dir offset/size at +0x08). The
  old empirical scan started *inside* `/apps/` and ran on through sibling
  directories, merging them into a fake root. Effects of that bug: **28 files were
  invisible** (incl. 22 modules, `/apps/Config.ini`, `/startup.sh`, `mod/willow/`),
  and "aliases" like `/Setup.txt` = `/Language/Setup.txt` were really the same
  `Language/` entries read twice. True layout: `/apps/` (UI data, `Language/`,
  `Logo/`, `WallPaper/`, `init.axf`, `desktop.mod`, `Config.ini`), `/mod/` (modules,
  `cedar/`, `willow/`), `/etc/cedarx.conf`, `/res/`, `/startup.sh`, `/rootfs_ini.tmp`.
- **Legacy paths still work**: `find()` falls back to the unique node whose path
  ends with the given one (`/Setup.txt` → `/apps/Language/Setup.txt`,
  `/carplay.mod` → `/mod/carplay.mod`; ambiguous names like `readme.txt` raise).
  Every hardware-validated edit had hit the correct real file.
- **Regression:** `repack_selftest.py` passes; the web configurator's splash,
  wallpaper, logo-choice, typo-fix, and preview outputs are **byte-identical** to
  HEAD. Web `ASSET_V` bumped to 9.

### Findings

- **`/startup.sh` is a data-driven module load list**, run by the kernel shell at
  boot: `insmod d:\apps\desktop.mod` (+ commented `display.mod`/`orange.mod`).
  `insmod` = `esMODS_MInstall` + `MOpen`. **This is the preferred injection point**
  for the Lua host: one text-line edit, no slot interface to emulate. The SDK
  loader also accepts a plain (ELF32-layout) ELF from any path, so the host might
  load straight from SD/USB (bench: is removable media mounted that early?).
- **Absent `d:\mod\` slots are 5, not 10**: `oak`, `pace`, `miracast`, `carlife`,
  `eclink`. `update`, `wireless`, `sensor_play`, `vcoder`, `video_play` ship.
- **Old decoder offsets were shifted**: `init.axf` `.rodata` by +4 bytes, and its
  `.data` was garbled. `ViewShowStrRdsRt` is at vaddr `0xe9262348` (image offset
  1450824); docs now cite the vaddr. String sweeps that leaned on the old `.data`
  output may have missed a few hits there.
- `/apps/Config.ini` (366 lines, same sections as the device export) ships in the
  image, so it's likely the factory default and a useful reference for the Device
  settings editor.
- `/rootfs_ini.tmp` is the vendor packer's config: LZMA-compress `drv, mod, plg, axf`.

---

## 2026-11-24 (evening) — Milestone 2 built: `hostmod/hello.mod`, emulator-tested, ready for the bench

### Loading path (verified against the stock *kernel*, not just the SDK)

- Decompressed the kernel: `melis_pkg_nor.fex` is a `sunxi-package` whose
  `melis-lzma` item (at `0x400`) is a standard LZMA-alone stream → 2.24 MB image
  at `0x40000000` (OpenSBI then kernel).
- It contains the SDK's **plain-ELF fallback loader** (`invalid elf file to
  loader`, `module format cant recongnized`). Disassembly of `LDR_LoadELFFile`
  (`0x4010aef2`): `memset` 0x34 (ELF32 header), 0x28 (ELF32 section header), 0x80
  (magic peek), `fread(hdr, 0x34)`, `e_shentsize` from the file, `strncmp(name, 5)`
  for `MAGIC`, `strncmp(magic, 8)` for `ePDK.mod`/`ePDK.exf`.
- So a **plain MINFS file** (no module flag → ROMLDR ioctl returns `-EINVAL`)
  holding an ELF32-layout module loads via `insmod`. The stock image has two
  **empty placeholder files** (`/mod/cedar/readme.txt`, `/mod/willow/readme.txt`),
  so injection is *replace the placeholder + add one `/startup.sh` line*, both
  edits the repacker already does. No MINFS "add file" feature needed.
- Caught before hardware: `esMODS_MInstall` copies the **whole** MAGIC section
  into an 80-byte stack struct, so a descriptor padded to 128 B (to match the
  0x80 peek) would have smashed the kernel stack. Descriptor is fixed at 80 B,
  enforced by `_Static_assert` and by `pack-module`.

### Built

- `hostmod/`: clean-room `include/melis.h` (descriptor + `MELIS_MODULE()` macro +
  `ecall` wrappers whose numbers stock modules use), `module.ld`, Makefile
  (`riscv64-elf-gcc` 16.2, `rv64gc`, `medany`, `--no-relax`), `hello/hello.c`.
- `headunit pack-module` (ELF64 → Melis ELF32-layout, with descriptor/address
  checks) and `headunit install-module` (placeholder + `startup.sh` line,
  idempotent, CRLF-preserving).
- `scripts/emulate_module.py`: loads the packed file like the kernel, runs
  `MInit`/`MOpen`/the thread in Unicorn with a fake syscall layer. **PASS**: banner
  printed, handle returned, thread prio `0xd506` (= mid 213 << 8 | 6, the stock
  convention), `F:\HELLO.TXT` written (102 B); with no stick it polls F:–K: every 2 s.
- Test image: only `data_udisk.fex` + its checksum differ from stock.

### Next

Bench boot (procedure in [hostmod/README.md](../hostmod/README.md)). Success =
`HELLO.TXT` on the stick. Then: confirm the drive letter, try loading from the stick
directly (`insmod f:\x.mod`), and start on the Lua VM (milestone 3).

## 2026-11-26: first image on the LCD from our own code

Display stages 1–3 (`docs/display.md`) in one evening, ported from the
mainline Linux sun6i DSI / D-PHY (A100 variant) / sun4i TCON drivers into
DRAM programs run with `felrun.py`:

1. **DSI command path:** the JD9365 answers DCS reads with bus turnaround on
   the first try (power mode `0x08`, pixel format `0x70`). Its ID registers
   read zero.
2. **Panel init:** the 212-entry `[mipi_reg]` list from `sys_config` goes
   through as DCS short writes; the panel leaves sleep (`0x08` → `0x18`).
3. **TCON pattern over DSI video:** colour bars, stable. No display engine yet.

Next: stage 4, a DE2 mixer UI layer reading a framebuffer in DRAM.

Later the same evening: **stage 4, a framebuffer.** DE2 mixer 0 with one UI
layer scanning a 720×1280 buffer in DRAM, through TCON TOP into the TCON and
over DSI. Corner markers pinned down the orientation (buffer x runs up the
landscape screen, buffer y runs across). `tools/fel/fbshow.py` pushes any image
over FEL in ~8 s, and the screen shows it as it arrives. The display is now
ours: next is drawing on the device instead of from the host.

And then **drawing on the device**: `display.c` (pipeline bring-up, double
buffering) and `gfx.c` (landscape coordinates, rounded tiles, gradients,
Spleen bitmap fonts). A mock home screen draws in 44 ms on the F133, against
8 s to push one frame over FEL.

Then **touch**: `twi.c` + `gt911.c` in the DRAM kit. The GT911 answers after
its reset sequence and reports in landscape at 1024 × 600, so mapping to the
1280 × 720 screen is a plain scale. `touch.c` lights the tile under the finger
and leaves a dot trail: the dots land right under the finger. First
interactive screen from our own code.

**Vblank wait:** TCON0 `TRI_FINISH` gives the frame end in DSI mode, every
17,083 µs (58.54 Hz, exactly the panel timing). With `display_flip()` waiting
on it, double-buffered animation runs at 58.6 fps with no tearing; the same
bars drawn straight into the visible buffer show cuts.

**Event loop:** `uart.c` + `mcu.c` (listen-only AB5301 frame parser) +
`input.c` (touch and keys in one queue) + `app.c`. The knob moves a focus ring
and opens pages, taps open tiles and go back, the phone key shows a banner.
Everything works and feels instant. This is the skeleton of the firmware.

**Booting our firmware, step 1 (offline build):** a boot selector in the boot0
patch (`sel`, hooked at boot0's Melis load, reusing boot0's flash driver), an
image format at NOR `0x120000` (`mkfw.py`), and the firmware side (`fw.c`:
watchdog hand-off, "boot stock once"). The stock escape is holding the status
bar for 3 s, because the MCU doesn't report key holds (one frame per press).
Tested under FEL: the hold counts down, cancels on lift, and reboots into
stock. Also learned: calling boot0's flash driver from a FEL session hangs the
USB link, and RTC GP registers don't survive a power cycle. Nothing written to
the flash yet.

**It boots.** boot0 v2 written (`boot0-write.sh`, verified, the 16 KB after
boot0 untouched). Power on → BROM → boot0 → FEL window → selector → our image
at `0x120000` → our home screen with "NOR" in the status bar. No Mac, no FEL:
the unit runs our firmware from its own flash.

**Fallbacks verified:** a firmware image that hangs gets reset by the
selector's watchdog and the unit boots stock; an image with a broken checksum
is skipped (stock straight away); the status-bar hold boots stock once. The
bench unit now boots our firmware by default, with stock always one gesture
away and the FEL window still first on every boot.

**Watchdog petting:** the firmware now restarts the selector's 8 s watchdog
from its main loop instead of disarming it. No spurious resets in normal use;
a deliberate freeze (Settings page, phone key) restarts the unit back into our
firmware. Hangs at startup still fall back to stock.

**Amp standby pin: not an F133 GPIO, as far as the firmware shows.**
`sys_config`'s only amp pin is the generic template's `gpio-spk = PH9` under
`[codec]` with `codec_used = 0` (the F133 has no port H; the internal codec is
unused). The stock app's configurable GPIO keys (all listed in `init.axf`:
`radioPowerGpio`, `antPowerGpio`, `subwoofGpio1/2`, `frontPowerGpio`, …) have no
amp or mute pin, and the app mutes through `CSystemSetup_MuteOn`. The unit's `Config.ini` pins (app numbering port×100 + pin: 318 = PD18 `usb0PowerGpio`,
319 = PD19 `frontAuxSwitchGpio`) aren't amp pins either. Likely conclusion: the
amp's standby/mute is driven by the MCU or by the sound processor's output stage,
not by a pin the app names. Next evidence: the amp
chip's marking, and the mute frame stock sends the MCU (from `init.axf`
  around `CSystemSetup_MuteOn`; we can't watch stock's UART TX live).

**Radio audio, first attempt (2026-11-26/27, `tools/fel/dram/radio.c`).** Stock
plays 101.1 MHz from the rear RCA (checked through a USB line-in). From FEL: `A7
[04 00]` (source = Radio) alone gets no reply. After `A0` the MCU answers with
its version (`AB5301_20260106_2125W`), BT status text (`81 D4`), `A6`/`B9`/`BF`
frames and the heartbeat, and acks `A7` (`02 80 A7 01 00 26`). TWI1 stays empty
at every address even then, with PB5 pulsed. The app's `radioModule=0` driver is
an Si4730 at `0x11`, but the board's tuner-module footprint is empty, so the
radio on this build probably isn't that I2C tuner. As before, `A0` led to a reset
a couple of minutes later (heartbeat unanswered); the unit came back up in ours.
`twi.c` now sets up the TWI1 and TWI2 pins too.

**Radio audio, rounds 2–4.**
- **TWI1 is PB4 (SCK) + PB5 (SDA), function 4** (Linux `pinctrl-sun20i-d1.c`);
  `sys_config`'s `PG14` is EMAC in that slot, and the app's "radio reset" PB5 is
  the bus's SDA. Empty on the right pins too, with the MCU in radio mode, so this
  board has no I2C tuner: the app drives an absent Si4730.
- **MCU start-up list** (from `init.axf` `0xe92742c8`): `A4`, `A0`, `B7 [02]`,
  `C3`, `BA`, `A7 [0F 00]` (`B3` skipped when `bArmRadio=1`), all acked; `A7
  [04 00]` sets `A6` data[1] bit 0x40.
- **Keep-alive:** the MCU sends `81 B2 [00 CE 00 00 00 00]` every 2–3 s and resets
  the F133 after ~2.5 min if unanswered. The app echoes it as **`B0 [CE 00 00 00
  00]`** (`02 80 B0 05 CE 00 00 00 00 FB`); answering once stopped the pings.
  `B0 [CE 01 …]` is a separate "host up" announce.
- **Still no sound:** BD3702 init + every input 0–15 at −10 dB leaves the rear
  RCA at the noise floor, not even switching clicks. Stock mutes only through
  BD3702 volume (reg `0x20` = `0xFF`), so the dead output is past the BD3702 or
  the radio audio doesn't come in the way we think.
- **Warm hand-off attempt:** importing Config.ini in stock reboots the unit, but
  the app mutes at the "success" toast first (recording: strong signal, then
  digital zero), so it can't show whether the path survives an F133 reset. Our
  FEL runs sit at the iMic's analog noise floor instead, so the RCA stage may be
  live with nothing feeding it.
- **MCU-radio frames:** `B3 [01]`, `B3 [00]` (the app's MCU-radio init) and
  `B0 [60 00 00 hi lo]` (set frequency, tried in 10 kHz and 100 kHz units) get no
  reply and no sound. The MCU acks `A0`, `A4`, `A7`, `B7`, `BA`, `C3`; it
  doesn't ack `B0` or `B3`. RCA still at the iMic's floor for 100 s.
- **Open for next time:** where stock's FM actually comes from. Leads: (1) board
  photos around the antenna input/shield can for a tuner IC; (2) sniff stock with
  the Pico: TWI1 (does the Si4730 traffic get ACKs?), TWI2 (what stock writes to
  the BD3702), UART1 TX (every frame stock sends the MCU); (3) the debug UART log
  (`bPrintLog=1`, PB6 at 576000) would answer it directly, but needs the case
  open and a safer connection than hand-soldering.

**A tuner does answer in stock (2026-11-27).** On the bench unit, stock radio
scan fills empty presets, and seek and step tuning work. Those need tuner
readback (station valid, RSSI), and the app's only radio path is I2C to an
Si473x at `0x11` on `/dev/twi1`. So the tuner exists and answers in stock; FEL
hasn't reached it yet. The F133 datasheet lists three TWI1 pin options:
- PB4/PB5, function 4: scanned, empty;
- PE0/PE1, function 4: not scanned yet;
- PG8/PG9, function 3: not scanned yet. PG8 is also the GT911 reset line.

`sys_config`'s PG14 can't be TWI1 at all. Next FEL test: full MCU start-up plus
radio mode, then scan TWI1 on all three pin pairs.

**🎉 Radio from our own code (2026-11-27).** The tuner bus is **bit-banged**: the
stock Melis kernel has a software I2C driver (`hal_twi_sim_*`) and runs
`/dev/twi1` on plain GPIOs, **SCL = PG14, SDA = PB4**, exactly as the board config
says. (The config isn't wrong; PG14 just can't be the *hardware* TWI1.) PB5 is
the tuner's reset (active-low), as `radioResetGpio=105` says.
- **Found by** `tools/fel/dram/bbscan.c`, a software I2C scan after the MCU
  start-up plus radio mode: `0x11` answers on PG14/PB4 and nothing on the other
  combinations.
- **`radio.c` over `bbi2c.c` (software I2C, ~50 kHz):**
  - POWER_UP FM `01 D0 05`;
  - GET_REV: **Si4730, firmware 6.0, revision D**;
  - the app's 33 properties;
  - tune 101.1 MHz in 43 ms: valid, RSSI 72 dBµV, SNR 26 dB;
  - BD3702 init, input `0x0B`, volume `0x8A`.
- **Rear RCA level 0.010 RMS, the same as stock (0.009).** Signal over 30 s:
  RSSI 48–52 dBµV, SNR 25–29 dB, 100 % stereo (coat-hanger antenna).
- **Input sweep:** the radio is heard on several BD3702 input codes besides
  `0x0B` (silent on others). The exact input-selector mapping is still to work out.
- **Not yet checked:** whether the MCU start-up is needed at all (it may only
  matter for the MCU's own reasons), and whether RDS works. The Si4730 has no
  RDS; its sibling the Si4731 does.
- **Kernel analysis confirms it** (static, `melis_pkg` kernel at 0x40000000):
  - **Every `/dev/twiN` uses the software driver** (`init_twi_device` registers
    twi0–4 with the sim ops). That includes `/dev/twi2` for the BD3702 and the
    MFi chip.
  - Only the touch driver uses the hardware TWI controller.
  - **Pins come from `[twiN]` with the function forced to 0 (GPIO)** and pull-ups
    on. The first entry is SCL, the second SDA.
  - **SCL is push-pull with no clock stretching.** SDA is released only for ACK
    and read phases.
  - Default 100 kHz means a ~6 µs half-period, about 80 kHz in practice.
  - **The kernel does nothing to power the tuner,** and the config has no
    tuner pin besides the reset. So the tuner is likely always powered, and the
    MCU start-up may not be needed for the radio (untested).
  - `[twi_para]` is a bootloader setting the kernel never reads.

**Radio page in our firmware (2026-11-27).** The FM radio moved from a test
program into the boot firmware (`tools/fel/dram/tuner.c`, `radio_ui.c`):
- **Tuner and sound:** `tuner.c` drives the Si4730 without blocking (tune and
  seek run while the UI keeps drawing and the watchdog is petted) and the
  BD3702 (input, volume, mute).
  - US de-emphasis (75 µs), 200 kHz steps.
  - Seek stops at RSSI ≥ 20 dBµV and SNR ≥ 6 dB.
  - **No MCU traffic is needed for the radio.**
- **Page:**
  - large frequency, signal meter, RSSI/SNR, stereo indicator;
  - seek, ±0.2 MHz, Scan → a Stations list (20 per page);
  - tap to listen, hold to add or remove a favorite;
  - 6 favorites in the bottom row; volume and mute.
- **Knob:** volume by default; push for FAVORITES, push again for TUNE. A ring
  shows what the knob controls while it's in use. 5 s idle → back to volume.
- **Scan on the bench** (coat-hanger antenna): 31–33 stations, 88.1–107.5 MHz,
  in about 15 s.
- **Fixed on the way:** `ticks()` is a 32-bit 24 MHz counter that wraps every
  179 s. Test timeouts over that overflowed (a "10-minute" run quit after about
  65 s, which looked like a scan hang), and banners and press timing in the boot
  build would have glitched. Added `millis()`, a wrap-free millisecond clock.
- **Still RAM-only:** favorites and volume reset on power-up. Next: a settings
  sector (the last 4 KB of our slot), then names via an on-screen keyboard.
- **Size:** 39 KB of the 64 KB slot.

**Bluetooth audio and key LEDs (2026-11-27).** `tools/fel/dram/bttest.c`:
- **Audio:** BD3702 input `0x0A` plays the phone's Bluetooth audio from the
  rear RCA.
- **Controls:** play, pause and next work as raw `AT#` commands on UART4.
- **Track info:** the module reports it (`MI` title/artist/album/length, `MP`
  position every second).
- **The module is a LITIANTAI WB020,** not the AB5301 as first guessed. Details
  in [mcu-protocol.md](mcu-protocol.md).
- **Key LEDs:** set to a fixed colour with `B0 [FE 00 00 05 m]`, which ends the
  colour cycling.
