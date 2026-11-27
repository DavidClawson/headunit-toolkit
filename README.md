# Open Head Unit Toolkit

Tools and reverse-engineering notes for modding a family of cheap **Allwinner
F133** (RISC-V, "D1s") Android/Apple-projection car stereos that run
Allwinner's **Melis** RTOS. Firmware ships as a single `LTTF133.img`
(IMAGEWTY container) flashed from a FAT32 USB stick.

### ▶ [**Try it in your browser →**](https://davidclawson.github.io/headunit-toolkit/web/)

Drop in your own `LTTF133.img`, change the **boot splash, wallpapers, or menu
text** with a live crop-and-scale preview, and download a patched image. No
install, no CLI — it runs entirely client-side (Pyodide/WASM), so **your
firmware is never uploaded anywhere.**

---

**Verified on:** a [SJoyBring 7" QLED double-DIN](https://www.amazon.com/dp/B0CT5P32L7)
(reports as model **JOY-W022**, firmware `LTTF133.img`). These units are sold
under many brand names on the same Allwinner F133 platform — the toolkit targets
the *platform*, so other brands shipping the same `LTTF133` firmware are likely
compatible, but this is the specific unit we've tested on real hardware.

This repo ships **code and knowledge, not firmware**. Everything here operates
on an image *you* supply (the one the vendor sent you). It never contains or
redistributes vendor firmware or the proprietary CarPlay / Android Auto
modules. See [`docs/legal-and-scope.md`](docs/legal-and-scope.md) before
publishing anything built with it.

> ✅ **Validated on real hardware (bench unit, Nov 2026).** The USB-update
> pipeline and two modding lanes — **custom images** (boot splash) and
> **user-facing menu text** (relabels / translations) — flash and display
> correctly on a live unit, with CarPlay / Android Auto left intact.
>
> ⚠️ **Key hardware finding:** this firmware paints its prominent, *dynamic* UI in
> code, so editing `DATAV1` widget colors/positions does **not** reliably change
> the screen — re-skins go through **images**, and relabels must target the string
> file the shown screen actually reads (the launcher grid reads `Main.txt` /
> `SystemBar.txt`, not `Setup.txt`).
>
> ⚠️ **Still unvalidated:** the deeper findings (boot chain, loadable-module
> format, Bluetooth / OBD, the Lua-host design) are reverse-engineered and not yet
> confirmed on hardware. Keep your original firmware; see
> [Safety](#safety-in-one-line) before flashing anything.

## 🆕 Open firmware: it boots (bench unit, Nov 2026)

Beyond modding the stock firmware, there's now a **clean-sheet firmware** for
this hardware, written from scratch (no vendor code) and **booting from the
unit's own flash**:

- **Display:** MIPI-DSI panel (JD9365, 720×1280 portrait, mounted landscape)
  brought up from bare registers: clocks, D-PHY, DSI, TCON, the DE2 mixer,
  double-buffered page flips on vblank.
- **Drawing:** a home screen with tiles, rounded shapes, gradients and bitmap
  fonts, rendered on the device.
- **Input:** GT911 touch over I2C, and the rotary knob/keys via the front-panel
  MCU (Bluetrum AB5301) over UART, all in one event loop.
- **FM radio:** a Radio page (tune, seek, scan into a station list, favorites,
  signal meter, volume on the knob) driving the Si4730 tuner and BD3702 sound
  processor directly.
- **Boot selector:** a small patch to boot0 loads our image from NOR
  `0x120000`, checks it, and starts it under a watchdog.

It's built to be **hard to brick**, and every fallback has been tested on real
hardware:

| If… | then… |
|---|---|
| you want stock back | hold the status bar for 3 s: the unit boots stock once |
| our image is corrupt | the selector boots stock instead |
| our firmware hangs at start-up | an 8 s watchdog resets the unit into stock |
| it freezes later | the watchdog restarts it back into ours |
| anything worse | every boot opens a 3 s USB FEL window first, so it can be reflashed from a PC |

**FM radio now plays from our own code.** The tuner (an Si4730) sits on an I2C
bus the stock kernel bit-bangs in software (PG14/PB4), which is why it hid from
every hardware scan. The story is in [issue #2](https://github.com/DavidClawson/headunit-toolkit/issues/2)
and [`docs/devlog.md`](docs/devlog.md). Since then it has a proper Radio page. Still to come: saved settings, the
amp's standby control, and CarPlay/Android Auto
([`docs/strategy.md`](docs/strategy.md)). Help is welcome:
see the [open issues](https://github.com/DavidClawson/headunit-toolkit/issues).

**Getting started:** the first FEL entry needs the case open and a SOIC-8 clip on
the SPI NOR (once). After that, everything runs over the front USB port. Start
with [`tools/fel/README.md`](tools/fel/README.md),
[`docs/display.md`](docs/display.md),
[`docs/boot-and-recovery.md`](docs/boot-and-recovery.md) and
[`docs/mcu-protocol.md`](docs/mcu-protocol.md). **Use a bench unit, not the
one in your car.**

## What it does today

- **Parse / unpack** the IMAGEWTY image and list its sub-partitions.
- **Browse** the Melis **MINFS** app/data partition (the UI, fonts, logos).
- **Decode `DATAV1.0` screen layouts** — every widget, its geometry, and its
  sample text (e.g. the RDS station-name / program-type fields).
- **Build a custom boot splash** safely: re-encodes your image to fit the
  original slot, rewrites both logo copies, and fixes the partition checksum —
  no other byte moves.
- **Edit UI screens and menu strings** in place (`edit-widget`), and **repack**
  the MINFS partition to replace any file with content of a *different* size
  (`replace-file`) — including a curated menu-typo pass (`fix-strings`).
- **A browser configurator** (`web/`) for the safe edits (splash + live crop
  preview, wallpaper, typo fixes), running the toolkit client-side via Pyodide —
  your firmware never leaves the page. See [`web/README.md`](web/README.md).
  The hosted page keeps **anonymous, cookieless** visit/build counts (GoatCounter,
  no personal data) so we can gauge whether it's useful; the firmware itself is
  still never uploaded.

## Install

```sh
pip install -e .          # needs Python 3.10+ and Pillow
```

## Usage

```sh
headunit list   stock/LTTF133.img                 # show sub-partitions
headunit ls     stock/LTTF133.img                 # browse the MINFS tree
headunit cat    stock/LTTF133.img Radio.txt --out radio.txt
headunit screen stock/LTTF133.img RadioRds.data   # decode a UI screen
headunit decompress stock/LTTF133.img /apps/init.axf --elf --out init.elf  # unpack a module for Ghidra
headunit logo   stock/LTTF133.img my.png build/    # build a custom-splash image
headunit wallpaper stock/LTTF133.img all my.jpg --out build  # swap home wallpaper(s)
headunit fix-strings stock/LTTF133.img --out build # curated menu-typo fixes
headunit replace-file stock/LTTF133.img Setup.txt new.bin --out build  # swap any file
```

The build commands (`logo`, `wallpaper`, `fix-strings`, `replace-file`,
`edit-widget`) write
`build/Update/LTTF133.img`; copy the whole `Update` folder to a FAT32 USB stick
and follow the vendor's update steps. `fix-strings`/`replace-file` can change a
file's size — they **repack** the partition (relocating into free space) and fix
the checksum, so the output is still a drop-in update image.

For a few ready-made example wallpapers to try, run
`python3 scripts/make_sample_wallpapers.py` (writes gradients into `samples/`).

## Roadmap — what we know, what we're doing

**Working now (validated on real hardware):** custom boot splash, wallpaper, and
user-facing menu text / translations — all **flash-and-go over USB**, no case
opening, CarPlay/Android Auto preserved.

**In progress / exploring (iterating in public):**
- **Open firmware** (see above): a radio screen, amp control, then CarPlay/Android Auto on Melis.
- Browser configurator — adding vendor-ZIP in/out and a menu-relabel &
  translation editor (the newly-validated lane).
- Shareable engineer **config profiles** (`factory1.bin`) — decoding the format.
- Deeper reverse engineering (see [`docs/devlog.md`](docs/devlog.md) and
  [`docs/re-notes.md`](docs/re-notes.md)): the loadable-module format, a planned
  SD-card **Lua scripting host**, and **Bluetooth/OBD-II** ideas. These are **not
  yet hardware-validated** and are the "advanced" track — *not* required for the
  flash-and-go tools above.

Contributions and **"I tried this on my unit and…"** reports are very welcome —
this is exactly the stage where real-hardware data is most valuable.

## Documentation

| doc | contents |
|---|---|
| [`docs/format-imagewty.md`](docs/format-imagewty.md) | the outer firmware container |
| [`docs/format-minfs.md`](docs/format-minfs.md) | Melis read-only filesystem + checksum |
| [`docs/format-datav1.md`](docs/format-datav1.md) | compiled UI screen-layout format |
| [`docs/hardware.md`](docs/hardware.md) | SoC, OS, tuner, CarPlay/AA findings |
| [`docs/hardware-audit.md`](docs/hardware-audit.md) | what the vendor config claims vs what's actually on the board |
| [`docs/factory-menu.md`](docs/factory-menu.md) | factory/engineer access codes + full menu map |
| [`docs/boot-and-recovery.md`](docs/boot-and-recovery.md) | FEL recovery, the boot0 FEL window + boot selector (built and tested), NOR map |
| [`docs/display.md`](docs/display.md) | open firmware: display, drawing, touch, vblank, event loop |
| [`docs/mcu-protocol.md`](docs/mcu-protocol.md) | front-panel MCU (AB5301) UART protocol, keys, keep-alive, radio facade |
| [`tools/fel/README.md`](tools/fel/README.md) | FEL tools: running programs from RAM, flashing, I2C/UART probes |
| [`docs/modding-workflow.md`](docs/modding-workflow.md) | versioned variants, bench unit, safety |
| [`docs/lua-host.md`](docs/lua-host.md) | north-star design: an SD-card Lua scripting host (open app ecosystem) |
| [`docs/reference/control-inventory.md`](docs/reference/control-inventory.md) | UI control cross-reference (placed vs referenced; dormant features) |
| [`docs/re-notes.md`](docs/re-notes.md) | deeper RE working notes (link base, Ghidra recipe, module ABI) |
| [`docs/first-flash-bench.md`](docs/first-flash-bench.md) | first-flash bench checklist (USB update, verify, recover) |
| [`docs/devlog.md`](docs/devlog.md) | dated bench-test log — what was validated on real hardware, and the lessons |
| [`docs/feature-ideas.md`](docs/feature-ideas.md) | demand-backed roadmap (from user-complaint research) |
| [`docs/strategy.md`](docs/strategy.md) | goals: two tracks, and how we'll keep CarPlay/AA in our own firmware |
| [`docs/legal-and-scope.md`](docs/legal-and-scope.md) | what you can and can't ship |

## Safety in one line

Keep the vendor's original `LTTF133.img`. Resource edits (logos, strings,
screen layouts) are low-risk and reversible by reflashing stock. Anything that
touches the boot chain belongs on a **bench unit with a NOR backup** — see the
recovery doc.

## Status

**Early, but the core now works on real hardware.** Three tiers:

- **✅ Validated on a live unit** (bench W022, Nov 2026): the whole
  build → repack → checksum → USB-flash → boot path, with CarPlay/Android Auto
  intact. Two visible-mod lanes confirmed end-to-end: **custom images** (boot
  splash) and **user-facing menu text** (tile relabels; length-changing edits
  work — the repacker's in-place *and* relocation paths are both honored on the
  device — though labels are clipped to the layout's fixed field width, so a
  translation must fit or the text widget needs widening). Caveat learned on
  hardware:
  the firmware **code-draws its prominent/dynamic widgets**, so `DATAV1`
  widget-attribute edits (color/geometry) don't reliably surface — re-skins go
  through images, and relabels must target the string file the shown screen reads.
- **Offline-verified against a real `LTTF133.img`** (26-partition image, model
  "W022"): the IMAGEWTY / MINFS / `DATAV1.0` readers, the logo builder, the
  `DATAV1.0` widget editor, the LZMA payload decompressor, and the **MINFS
  repacker** — see `scripts/repack_selftest.py`. Factory-menu codes and the
  container/filesystem/screen format specs are high-confidence.
- **Reverse-engineered, not yet hardware-validated:** the open-boot-chain
  analysis, the loadable-module (`.mod`) format and injection path, the
  Bluetooth/OBD-II findings, and the Lua-host design. These come from static
  analysis of the stock image and will be confirmed or corrected on the bench.
- **✅ Open firmware on the bench unit:** boots from NOR through a boot0
  selector with tested fallbacks; display, touch, knob and FM radio work.

Contributions, corrections, and especially **"I tried this on my unit and…"**
reports are very welcome — this is exactly the stage where real-hardware data is
most valuable.
