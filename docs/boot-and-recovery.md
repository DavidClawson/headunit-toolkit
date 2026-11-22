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

## No secure boot (confirmed) — and the boot software

**Verdict: the boot chain is open.** boot0 ships as a plain `eGON.BT0` image,
not a secure `TOC0` container — and a secure-efuse part *requires* a `TOC0` at
the boot0 slot and rejects `eGON.BT0` ("toc0 magic is error, need secure image"),
so the fact that the stock `eGON.BT0` boots proves the secure efuse is **not
burned**. The only integrity check over any partition is the 32-bit additive
checksum (re-derived and matched on boot0/fes1). The sunxi verified-boot *code*
(`sunxi_verify_rotpk_hash`, `burn_secure_mode`, efuse R/W) is compiled into
u-boot but dormant — tooling to *optionally* burn secure mode, not enforcement.
No AVB/dm-verity/signature/pubkey anywhere. **A checksum-correct repack flashes
and boots.**

**u-boot:** `U-Boot 2018.07-g1ffdfbf (Oct 18 2023)`, platform `sun8iw19p1`,
RISC-V toolchain. Full interactive console is enabled and reachable — **but at
115200 baud**, while the Melis/kernel debug UART on the *same* PB06/PB07 pads is
**576000**. So: interrupt autoboot at **115200** to drop to the u-boot prompt,
then the OS log comes up at 576000. Autoboot delay is ambiguous in the env
(`bootdelay` both `0` and `2` present) — confirm on UART. `fastboot`, `sprite`
(in-system flash), `recovery`, and `efex`/FEL modes are all compiled in. u-boot
re-entry to FEL: `run usb efex`, a misc boot-mode flag, `reboot efex`, or an
IR/key-at-boot path.

**NOR partition layout** (GPT, ~16 MB):

| name | ~size | content |
|---|---|---|
| (pre-LBA32) | — | boot0 + GPT/MBR (raw NOR) |
| `bootA` | 1.18 MB | `melis_pkg_nor.fex` |
| `ROOTFS` | 14.6 MB | `data_udisk.fex` (the MINFS we mod) |
| `UDISK` | 0.92 MB | user data |

There is **no `env` partition** (env/env-redund are commented out of the config),
so a boot-selector that persists a *default* choice would have to carve one; the
SD-insert and button-at-boot variants need none.

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

## Hardware probing / HAL access (talk to the board directly)

"Could we build a little utility firmware to send commands and explore the
HAL?" — yes, eventually, but **two cheaper probes already exist on this chip**
before any custom build, and they're where to start. Three tiers, cheapest and
safest first:

### Tier 1 — FEL (USB, no flash, lowest risk)

FEL *is* "connect over USB and send commands." It's a BootROM USB protocol, so
it needs no firmware of ours and touches no NOR. With `sunxi-fel` (from
`sunxi-tools`, which supports D1/F133) over the **USB-OTG** path you can:

- `sunxi-fel version` — confirm the link and read the SoC ID.
- `sunxi-fel read <addr> <len> out.bin` / `write <addr> file` — peek/poke
  arbitrary memory (SRAM/DRAM).
- `sunxi-fel exe <addr>` — run a small payload from SRAM.

Recovery is just re-entering FEL, so this is the safest surface for live
experimentation. Entering it: the ROM drops to FEL when no valid boot code is
found (see the recovery recipe above — briefly starve `boot0` at power-on), or
via a FEL trigger pin if the board exposes one.

### Tier 2 — UART serial console (live visibility, ~$3)

Melis almost certainly emits boot/debug output — and possibly an interactive
shell — on a **debug UART**. A USB-TTL adapter (3.3V!) on the right pads gives a
running log of what the HAL is actually doing, which is the fastest way to see a
driver probe, a mount, or a crash. The pin assignment is **already in the stock
build** (see "Finding the ports" below) — no guessing at pads.

### Tier 3 — a custom "HAL explorer" app (the powerful version)

Replace `init.axf` with our own Melis app that opens a USB-serial REPL and lets
us call `esDEV_*` / `esMODS_*` and the `disp` / `g2d` / `cedar` ioctls
interactively. This is the version that becomes the *foundation* for every
ambitious feature (boot video, RadioText, a new mode), not just a diagnostic.

Two caveats keep it in Tier 3:
- **Prerequisite we don't have yet:** a Melis app build toolchain. We have the
  exported API *symbols* and leaked source paths (`ekernel/arch/riscv/...`) but
  not the SDK/linker setup to produce a loadable `.axf`. Reconstructing that ABI
  is the real cost — and the real unlock.
- Booting our own app **temporarily removes CarPlay/AA**. Fine on a bench unit
  with a NOR backup; never on the car unit.

So the sequence is: **FEL first** (free, safe, immediate), **UART second** (cheap,
live), and the custom app as the payoff of cracking the build toolchain.

### Finding the ports (read the stock build, then look under the shield)

We shouldn't need to probe blindly. Two sources:

1. **`sys_config_nor.fex`** in the stock image is the board's pinmux/config
   table (same file that gave us `lcd_x`/`lcd_y` and the logo settings) — and it
   ships as **plain-text INI** (GBK comments, ASCII keys), so no decoder is
   needed. The verified pin map from the stock W022 image:

   | Interface | SoC pins | Notes |
   |---|---|---|
   | **Debug UART console** | **PB06 = TX, PB07 = RX** | `uart_debug_port=3`, **baud 576000** 8N1 |
   | **USB-OTG / FEL** | USB0 controller (`usbc0`, `usb_used=1`) | the `sunxi-fel` port |
   | **GT911 touch** | TWI3: PG10 SCL / PG11 SDA | addr `0x5d`, INT PG07, 1024×600 |
   | **Main I²C** (`twi_port=2`) | TWI2: PE12 SCL / PE13 SDA | likely the MFi/CarPlay bus |
   | **JTAG** | PE4… | disabled (`jtag_enable=0`) |

   Two gotchas this file reveals:
   - **The console is 576000 baud, not 115200.** A UART adapter at the usual
     115200 shows only garbage. Use **PB06/PB07 @ 576000 8N1**.
   - **Pin conflict:** PB06 is declared *both* as `uart_debug_tx` and as
     `usbc0`'s `usb_id_gpio`. The debug UART wins in practice (boot0 configures it
     first; `usb_detect_mode=0`), but it means the vendor config is a reused
     template — confirm on the bench. Also confirm which physical USB jack is
     wired to USB0 (FEL needs USB0, not the host-only `usbc1`).

   Extract it yourself with:
   ```sh
   # partition 0 of the IMAGEWTY; it's text INI
   iconv -f GBK -t UTF-8 sys_config_nor.fex | grep -A4 -iE '\[uart_para\]|\[usbc0\]|\[twi'
   ```
2. **Physically, under the RF shield.** When the bench unit arrives, check how
   easily the steel RF can comes off — the debug UART and FEL/USB test pads are
   usually on the main PCB near the SoC, often a labeled 3–4 pad row (RX/TX/GND,
   sometimes a boot/FEL pad). Cross-referencing the silkscreen against the
   `sys_config` pin names should identify them without a logic analyzer.

> **Bench tasks (once the unit is here):**
> 1. `sunxi-fel version` over USB-OTG — prove the FEL link works (safest first
>    contact with the hardware).
> 2. Pop the RF shield (note how it's attached — tabs vs. solder); photograph the
>    PCB around the SoC; look for a UART/FEL pad row.
> 3. Extract `sys_config_nor.fex` from the stock image and match its UART/USB/GPIO
>    pin names to the pads found in step 2.

## Planned feature: a GRUB-style boot selector

Implemented in **u-boot** (open source, well-supported on D1/F133):

- List bootable images from a FAT SD card.
- Chainload the selected Melis image.

> **No hardware buttons are wired on this board.** `sys_config` has
> `gpadc_used=0` and `tkey_used=0` — the ADC-key and touch-key tables are
> disabled template defaults. So the reliable power-on toggle is **"is the SD
> card inserted?"** (the zero-code A/B above), not a button hold. A touch read
> via the GT911 at boot is a possible alternative, but SD-insert is simplest.

Milestones:
1. **SD-insert → stock-or-SD** (no display init, no button; simplest, highest
   value — already works at the ROM level, this just formalizes it).
2. **Framebuffer menu** on the LCD — needs porting panel init into u-boot. The
   panel is `he0801a068` (MIPI-DSI 4-lane, 720×1280, dclk 70 MHz, timings
   `hbp48/ht908/hspw8 · vbp15/vt1317/vspw6`, backlight-enable PG15, PWM ch7) and
   the full `[dram_para]`/`[lcd0]` blocks are in `sys_config_nor.fex`, so it's a
   transcription port, not a reverse-engineer. Build into the **RISC-V** u-boot
   and ignore the stale ARM DTB (see [hardware.md](hardware.md)).
3. **Persisted default** via a small env partition (none exists today — it must
   be carved).

This touches the boot chain, so it is **bench-unit-only** work, done after a
NOR dump and a confirmed FEL path.
