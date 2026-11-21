# Boot, recovery, and a planned firmware selector

The single most important thing before any risky flashing: **this SoC has a
hardware recovery path that software cannot disable.** Set that up first and
the rest is low-stress.

## Boot order (the free A/B)

Allwinner's BootROM tries boot sources in priority order and falls back:

```
SD card (MMC0)  →  SPI-NOR (boot0)  →  FEL (USB recovery)
```

The firmware ships `boot0_card.fex` / `cardtool.fex`, confirming the platform
supports booting from SD. So the simplest "selector" needs **no code**:

- Stock firmware stays untouched on NOR.
- An experimental build goes on a **bootable SD card**.
- Card inserted → it boots your build. Card removed → it boots stock.

That is already an A/B with the most reliable toggle possible: *is the card
in?* Pull it and you're back to safe stock.

> Verify on a bench unit that the SD slot is wired to the **bootable MMC
> controller**, not just as a media peripheral. Likely, given `boot0_card.fex`
> ships — but some cheap boards cut that corner.

## FEL — the unbrickable recovery

FEL is a USB device mode in the mask boot ROM, entered when the ROM finds no
valid boot code (or via a FEL trigger pin). The boot chain references it
directly (`jump fel`, `crashdump mode, jump fel`), and the update image even
*contains* the FEL/FES payloads (`fes1.fex`, `usbtool.fex`).

Recovery recipe (to validate on the bench unit):

1. Connect the SoC's **USB-OTG** path to a PC with `sunxi-tools` installed
   (`sunxi-fel` supports D1/F133).
2. Force FEL: briefly ground the SPI-NOR's CLK or DO at power-on so `boot0`
   fails to load, dropping the ROM into FEL. (Exact pads are board-specific.)
3. `sunxi-fel` pushes `fes1` + u-boot into RAM; reflash from there.

## The real safety net: dump the NOR first

The flash is almost certainly an **SOIC-8 SPI-NOR**. With a ~$5 CH341A clip:

1. **Read the entire NOR to a file before touching anything** (`nor_backup/`).
2. If any flash goes wrong, write that dump back byte-for-byte — total
   recovery, independent of FEL.

Do this once per bench unit and experimentation becomes cheap.

## Planned feature: a GRUB-style boot selector

Implemented in **u-boot** (open source, well-supported on D1/F133):

- Read a **button GPIO** at power-on ("hold these keys").
- List bootable images from a FAT SD card.
- Chainload the selected Melis image.

No spare non-volatile memory is required: the *menu* is u-boot code, the
*choices* are files on the SD card, and the *selection* is a live button read.
Persisting a default (optional) can use u-boot's environment block.

Milestones:
1. **Button → stock-or-SD** (no display init; simplest, high value).
2. **Framebuffer menu** on the LCD — needs porting panel init into u-boot;
   the panel timings are already in `sys_config_nor.fex`, so it's a port, not
   a reverse-engineer.
3. **Persisted default** via a small env partition.

This touches the boot chain, so it is **bench-unit-only** work, done after a
NOR dump and a confirmed FEL path.
