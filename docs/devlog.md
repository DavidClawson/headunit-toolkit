# Dev log

Dated, honest notes from real work on the bench — what we tried, what worked,
what didn't, and the lessons. Newest entries first. Nothing here is firmware;
it's reverse-engineering facts and test results from a unit we own.

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
