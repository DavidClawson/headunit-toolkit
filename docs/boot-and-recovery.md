# Boot and recovery

How to get a W022 back from a bad flash, and what we know about its boot chain.

**The short version:** the firmware lives on a 16 MB SPI flash chip that you can
read and rewrite **directly with a SOIC-8 clip and a Raspberry Pi Pico**, without
the unit's software doing anything. That has been done on the bench unit and
recovered it from a boot hang (2026-11-25). Back up the chip before any risky
experiment and a bad flash costs about ten minutes.

## Recovering a bricked unit: SPI flash clip (verified)

Use this when the unit won't boot far enough to take a USB update (for example,
stuck at the splash) and reflashing the stock image from a stick does nothing.
That's what happened after the first `hostmod` test: the USB updater lives in
the app, so if boot hangs before the app runs, a stick can't fix it.

### What you need

- A **SOIC-8 test clip** (the kind sold with CH341A programmer kits; use only the
  clip and its little 8-pin adapter board).
- A **Raspberry Pi Pico** (RP2040) and jumper wires.
- `flashrom` on the Mac: `brew install flashrom` (tested with 1.8.0).
- To build the Pico firmware: the Pico SDK (`PICO_SDK_PATH`, tested with 2.2.0),
  `cmake`, and `arm-none-eabi-gcc`.

> **Skip the CH341A programmer in those kits.** The common black board drives its
> data lines at ~5 V even with the jumper on 3.3 V. This chip is 3.3 V, and
> in-circuit those signals also reach the F133. The Pico is 3.3 V throughout.

### The flash chip

On the mainboard (silkscreen `FR4 REV.09`), next to a small silver 24 MHz crystal
marked `YL240` and with an orange factory dot on it:

| | |
|---|---|
| Part | **XMC XM25QH128D** (`XM25QH128DHIQ`), 16 MB, 3.3 V |
| Package | SOP-8 **208 mil** (wide body; standard clips fit) |
| JEDEC ID | `0x20 0x4018` |
| flashrom name | `XM25QH128C/XM25QH128D` |
| Pin 1 | the corner with the small round dimple |

Pins count counter-clockwise from pin 1 (viewed from above): 1–4 along pin 1's
side, 5–8 back along the other.

### 1. Turn the Pico into a flash programmer

[`pico-serprog`](https://github.com/stacksmashing/pico-serprog) makes the Pico
speak flashrom's `serprog` protocol over USB. It has no release builds, so build
it:

```sh
git clone https://github.com/stacksmashing/pico-serprog && cd pico-serprog
cmake -S . -B build -DPICO_BOARD=pico      # pico2 for an RP2350 board
make -C build                              # -> build/pico_serprog.uf2
```

Hold **BOOTSEL** while plugging in the Pico, copy `pico_serprog.uf2` onto the
`RPI-RP2` drive, and it re-enumerates as `/dev/cu.usbmodem*`.

### 2. Wire the clip to the Pico

Plug the clip's ribbon into its 8-pin adapter board (red wire = pin 1), then run
jumpers from the adapter's numbered pins:

| Flash pin | Signal | Pico pin | Pico label |
|---|---|---|---|
| 1 | CS | 2 | GP1 |
| 2 | DO (MISO) | 6 | GP4 |
| 3 | WP# | 36 | 3V3(OUT) |
| 4 | GND | 3 | GND |
| 5 | DI (MOSI) | 5 | GP3 |
| 6 | CLK | 4 | GP2 |
| 7 | HOLD# | 36 | 3V3(OUT) |
| 8 | VCC | 36 | 3V3(OUT) |

**Never use Pico pins 39 (VSYS) or 40 (VBUS):** they're 5 V.

### 3. Connect and check

1. **Unplug the head unit's 12 V completely.** The Pico powers the chip.
2. Seat the clip with the red wire on pin 1. Look at all eight contacts from
   the side: each must sit on a leg, not on the plastic.
3. Plug the Pico into the Mac and probe (read-only):

```sh
P=$(ls /dev/cu.usbmodem* | head -1)
F="flashrom -p serprog:dev=$P:115200,spispeed=4M -c XM25QH128C/XM25QH128D"
flashrom -p serprog:dev=$P:115200,spispeed=4M      # expect: Found XMC flash chip "XM25QH128C/XM25QH128D"
```

If it reports `No EEPROM/flash device found`, look at the raw ID with
`-V | grep id1`:

| `id1` reads | Likely cause |
|---|---|
| `0x00` everywhere | clip not seated, or a wire missing. **This was it on our unit.** Reseat and retry. |
| `0xff` everywhere | DO (pin 2) not reaching GP4 |
| `0x20, 0x4018` but not "Found" | add `-c "XM25QH128C/XM25QH128D"` |

Powering the chip from the Pico in-circuit also back-feeds part of the board's
3.3 V rail. On our unit that was fine. If reads are flaky or the two dumps differ,
measure flash pin 8 to pin 4 (should be about 3.3 V). If it's sagging, power those
three 3V3 wires from a stronger 3.3 V source (a Raspberry Pi's pin 1, or a bench
supply) with its ground tied to the Pico's.

### 4. Back up twice, compare

```sh
mkdir -p stock/nor          # stock/ is gitignored: never commit flash dumps
$F -r stock/nor/dump1.bin   # ~45 s
$F -r stock/nor/dump2.bin
cmp stock/nor/dump1.bin stock/nor/dump2.bin && echo IDENTICAL
```

**Don't write anything until two reads match.** Keep these dumps: they're the
exact state of the chip, including your settings partition.

### 5. Repair only what's broken

The flash map (verified against our dump and the stock update image):

| Offset | Size | Contents | From the update image |
|---|---|---|---|
| `0x000000` | 48 KB | boot0 | `boot0_nor.fex` |
| `0x010000` | 1.06 MB | Melis kernel package | `melis_pkg_nor.fex` |
| `0x130000` | `0xdf0000` (13.9 MB) | MINFS data partition (`/apps`, `/mod`, `/startup.sh`, ...) | `data_udisk.fex` |
| `0xf20000` | to end (896 KB) | `UDISK`: user data and settings | not in the image |

A bad mod normally only touches the data partition. Rebuild the chip image with
the stock data partition and everything else as it was read:

```python
from headunit import ImageWTY
d = bytearray(open("stock/nor/dump1.bin", "rb").read())
stock = bytes(ImageWTY.from_path("stock/LTTF133.img").read("data_udisk.fex"))
d[0x130000:0x130000 + len(stock)] = stock
open("stock/nor/restored.bin", "wb").write(d)
```

Check that the boot0 and kernel regions in the dump already match the stock
image before trusting them (search the dump for each `.fex`). In our case
they did, and the repair changed only **three 4 KB blocks** (directory entries,
`startup.sh`, and the module slot).

### 6. Write just that partition, verify

A layout file lets flashrom touch only the data region:

```sh
printf '00000000:0012ffff boot\n00130000:00f1ffff data\n00f20000:00ffffff udisk\n' > stock/nor/layout.txt
$F -l stock/nor/layout.txt -i data -w stock/nor/restored.bin   # ends with "VERIFIED."
$F -r stock/nor/after.bin && cmp stock/nor/after.bin stock/nor/restored.bin && echo MATCHES
```

flashrom erases and rewrites only the sectors that differ (under a minute).

### 7. Boot

Unplug the Pico, **take the clip off** (dangling wires on the SPI bus can upset
the boot read), connect 12 V. Ours booted straight to the normal UI.

A few minutes on the bench with the die-cast back/heatsink off is fine if no
speakers are connected and nothing is playing. Keep the amp's metal tab clear of
anything conductive, and refit the heatsink before real use.

### Keep a known-good backup

Once a unit boots normally, read it once more (`after.bin` above) and keep that
file. It's the one to write back after any future experiment.

## The debug UART (located, not yet wired)

The kernel console is on **UART3**: `boot0`'s header and `[uart_para]` in
`sys_config_nor.fex` agree on **TX = PB6, RX = PB7, 576000 baud 8N1** (not
115200).

On the F133 (eLQFP128, [datasheet](https://mangopi.org/_media/f133_datasheet_v1.2.pdf)
§7.1), **PB6 is pin 80 and PB7 is pin 79.** Physically, with the chip's text upright
the pin-1 dot is at the **bottom-left**, so the datasheet's right-hand side is the
**top edge**. Counting legs from the **left** end of the top edge:

| Leg from left | Pin | Signal |
|---|---|---|
| 14 | 83 | VCC-IO (3.3 V) |
| 15 | 82 | PB5 |
| 16 | 81 | **VDD-SYS2: core supply, measured 0.926 V** |
| **17** | **80** | **PB6 = debug TX** |
| **18** | **79** | **PB7 = debug RX** |
| 19 | 78 | TVOUT0 |
| 20 | 77 | VCC-TVOUT |

The 0.9 V on the 16th leg is the landmark that confirms the orientation: nothing
else nearby sits that low.

> **Don't hand-solder to these legs** unless you have the tools and practice for
> 0.4 mm pitch. TX sits between the core supply and RX. A first attempt on the
> bench unit bridged four legs. Never power the unit with a suspected bridge on
> this edge; clear it with flux and fine wick, then check that the 16th leg
> doesn't beep to its neighbours. Better options:
>
> - **Read-only, no solder:** hold a fine needle on the 17th leg during boot,
>   with the Pico listening (below).
> - Follow the TX trace to a via or resistor and solder there instead.
> - Have a phone-repair shop attach the three wires.

`sys_config` also lists PB6 as `usbc0`'s `usb_id_gpio` and gives PB7 a mode
value that may mean "output", both probably vendor-template leftovers. Put a
1 kΩ resistor in series with anything you drive into RX.

**Pico as a 576000-baud USB serial adapter:** [`tools/pico-uartbridge`](../tools/pico-uartbridge)
(GP1 = RX from the unit's TX, GP0 = TX to the unit's RX, plus GND). The LED
blinks when data arrives.

```sh
cd tools/pico-uartbridge && cmake -S . -B build -DPICO_BOARD=pico && make -C build
# BOOTSEL, copy build/uartbridge.uf2, then:
cat /dev/cu.usbmodem*      # USB CDC: the baud setting here doesn't matter
```

What it should give: the full boot log (boot0, kernel, each `insmod` in
`startup.sh`, crash dumps), our modules' `melis_puts` output, and possibly the
Melis shell if RX works, which would allow loading test modules by hand
(`insmod f:\x.mod`) without writing them to flash.

## Boot order

Allwinner's BootROM tries, in order:

```
SD card (MMC0)  →  SPI-NOR (boot0)  →  FEL (USB recovery)
```

The firmware ships `boot0_card.fex`, `cardtool.fex`, and a card-product
`cardscript.fex` (boot0 at sector 16, boot package at 32800, image at 40960), and
`sys_config` enables `sdc0` on PF0–PF5. **But the bench W022 has no SD slot
fitted** (the manual shows one, front top-right; ours has none). So SD boot and
PhoenixCard-style card recovery aren't available on this board without adding
a slot, and the "pull the card to get stock back" A/B trick doesn't apply.

## No secure boot (confirmed)

**The boot chain is open.** boot0 ships as a plain `eGON.BT0` image, not a
secure `TOC0` container, and a secure-efuse part *requires* `TOC0` at the boot0
slot ("toc0 magic is error, need secure image"). The stock `eGON.BT0` boots, so
the secure efuse is **not burned**. The only integrity check is the 32-bit
additive checksum per partition. The sunxi verified-boot code is compiled into
u-boot but dormant. No AVB/dm-verity/signatures. **A checksum-correct repack
flashes and boots.**

**u-boot:** `U-Boot 2018.07-g1ffdfbf (Oct 18 2023)`, RISC-V. It is **not** in the
normal boot path (boot0 loads Melis directly). It's used for "sprite" flashing
(env `bootcmd=run sunxi_sprite_test`). Commands present include `fatload`,
`sunxi_flash`, `sprite_test`, `efex`, `fel`, `md`, `go`.

There is **no `env` partition** (commented out of the config), so a boot selector
that remembers a default would have to carve one.

## FEL (USB recovery, verified 2026-11-26)

FEL is the BootROM's USB device mode, entered when no valid boot code is found.
It uses the SoC's **USB0** (pins 114/115 on the F133). On the W022 the USB jack
is on a **daughterboard** (4-wire red/white/green/black cable to the mainboard,
its own QFN chip `U2`, footprints for USB-A and USB-C). **The front USB port
reaches USB0 directly:** FEL enumerates through it, so `U2` isn't a hub in the
way. Both controllers are configured as host in `sys_config`, which doesn't
matter in FEL (the ROM uses USB0 as a device).

**How to get in: [`tools/fel/`](../tools/fel/README.md).** Instead of shorting
flash pins at power-on, it uses the clip to erase only the first 4 KB (boot0's
`eGON.BT0` header), so the BootROM finds nothing to boot and drops into FEL
every time. Connect the front port to the Mac with an A-to-A/A-to-C cable with
**VBUS cut** (both sides supply 5 V) and power on; the screen stays dark.

**Bench result (2026-11-26):**

- The Mac saw `1f3a:efe8` 9 s after power-on: `xfel version` →
  `AWUSBFEX ID=0x00185900(D1/F133)`.
- A test program (`tools/fel/probe.bin`) loaded at `0x40000`, ran and returned
  to FEL, twice: T-Head `mvendorid` `0x5b7`, hart 0,
  `misa` `0x8000000000b4112d` (RV64 IMAFDC + S/U + V + X).
- `xfel spinor` read the flash at about 400 KB/s (16 MB in about 40 s), matching
  the clip dump.
- **The boot0 header was put back over USB** (`xfel spinor write 0 hdr4k.bin`),
  a full 16 MB read over USB matched the backup byte for byte, and the unit
  booted normally. So once in FEL, recovery needs no clip.

What this gives:

- `xfel` can load and run code in SRAM/DRAM and read memory back, a test loop
  that never writes the flash. That's the basis for working out the board's
  hardware (pins, clocks, display, touch) with small test programs, starting from
  the pin map in `sys_config_nor.fex`.
- `xfel spinor` can read and write the flash over USB, without the clip.
- As long as FEL works the unit can't be bricked for good: a bad boot0 just
  lands back in FEL.

### Plan: clean-sheet firmware with fallback

1. **FEL first contact** (above): load → run → read-back with `probe.bin`. **Done.**
2. **Map the hardware from RAM**: test programs that confirm the backlight,
   LCD (`he0801a068`, MIPI-DSI), GT911 touch, and UART pins from `sys_config`.
   Build on the existing open D1s work (xboot, RT-Thread, mainline U-Boot/Linux)
   rather than from nothing.
3. **Boot selector with watchdog fallback**: keep the stock boot0. The selector
   sets a "trying experimental" flag in a register that survives reset, arms the
   hardware watchdog, and boots the experimental image. A hang resets the unit,
   the selector sees the flag and boots stock. FEL and then the clip stay as the
   backstops.
4. **Clean stock → new firmware tests** once the above is proven on the bench.

## Other board notes (bench unit, opened 2026-11-24)

- SoC: **F133-B** (eLQFP128). Bluetooth: Bluetrum "AB" LQFP48 with a 26 MHz
  crystal. Buck regulators with 10 µH inductors (`100`).
- Front-panel button/LED board on its own FPC (`J19`); the USB board as above.
- The 10-hole `ANT` row is a tuner-module footprint.
- No labelled UART/FEL test pads on either side of the mainboard.

## Planned: a boot selector

Implementable in u-boot (open source, supports D1/F133): list images on FAT
storage and chainload the chosen Melis image. With no SD slot and no buttons
(`gpadc_used=0`, `tkey_used=0`), the toggle would have to be a USB stick or a
GT911 touch read at boot. The LCD is `he0801a068` (MIPI-DSI 4-lane, 720×1280);
full `[lcd0]`/`[dram_para]` blocks are in `sys_config_nor.fex` for a framebuffer
menu. This touches the boot chain: **bench unit only, after a flash backup.**
