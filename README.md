# Open Head Unit Toolkit

Tools and reverse-engineering notes for modding a family of cheap **Allwinner
F133** (RISC-V, "D1s") Android/Apple-projection car stereos that run
Allwinner's **Melis** RTOS. Firmware ships as a single `LTTF133.img`
(IMAGEWTY container) flashed from a FAT32 USB stick.

This repo ships **code and knowledge, not firmware**. Everything here operates
on an image *you* supply (the one the vendor sent you). It never contains or
redistributes vendor firmware or the proprietary CarPlay / Android Auto
modules. See [`docs/legal-and-scope.md`](docs/legal-and-scope.md) before
publishing anything built with it.

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

## Documentation

| doc | contents |
|---|---|
| [`docs/format-imagewty.md`](docs/format-imagewty.md) | the outer firmware container |
| [`docs/format-minfs.md`](docs/format-minfs.md) | Melis read-only filesystem + checksum |
| [`docs/format-datav1.md`](docs/format-datav1.md) | compiled UI screen-layout format |
| [`docs/hardware.md`](docs/hardware.md) | SoC, OS, tuner, CarPlay/AA findings |
| [`docs/factory-menu.md`](docs/factory-menu.md) | factory/engineer access codes + full menu map |
| [`docs/boot-and-recovery.md`](docs/boot-and-recovery.md) | SD-boot, FEL recovery, a planned boot selector |
| [`docs/modding-workflow.md`](docs/modding-workflow.md) | versioned variants, bench unit, safety |
| [`docs/lua-host.md`](docs/lua-host.md) | north-star design: an SD-card Lua scripting host (open app ecosystem) |
| [`docs/first-flash-bench.md`](docs/first-flash-bench.md) | first-flash bench checklist (USB update, verify, recover) |
| [`docs/feature-ideas.md`](docs/feature-ideas.md) | demand-backed roadmap (from user-complaint research) |
| [`docs/legal-and-scope.md`](docs/legal-and-scope.md) | what you can and can't ship |

## Safety in one line

Keep the vendor's original `LTTF133.img`. Resource edits (logos, strings,
screen layouts) are low-risk and reversible by reflashing stock. Anything that
touches the boot chain belongs on a **bench unit with a NOR backup** — see the
recovery doc.

## Status

Early. The IMAGEWTY / MINFS / `DATAV1.0` readers, the logo builder, the
`DATAV1.0` widget editor, and the **MINFS repacker** (resize/replace any file,
checksum-correct) are verified against a real `LTTF133.img` (26-partition image,
model "W022"); see `scripts/repack_selftest.py`. The u-boot SD boot selector is
designed but not yet built.
