# FEL bring-up kit

FEL is the F133 BootROM's USB device mode. With it, a Mac can load code into the
SoC's RAM over USB, run it, and read the results back, **without writing the
flash**. A hung test is a power-cycle away from a clean retry. This folder holds
the first-contact kit; the background is in
[docs/boot-and-recovery.md](../../docs/boot-and-recovery.md#fel-usb-recovery-verified-2026-11-26).

| File | What it does |
|---|---|
| `fel-enter.sh` | Backs up the chip twice (must match), then erases **only the first 4 KB** (boot0's `eGON.BT0` header) so the BootROM falls through to FEL. |
| `fel-exit.sh` | Writes that 4 KB back from the same backup, reads the whole chip and confirms it matches. |
| `probe.S` | First test program: reads CPU ID registers, times a loop, bumps a run counter, writes `0x600DF00D`, returns to FEL. Touches no peripherals. |
| `backlight.sh` | Backlight test: drives PG15 (`lcd_bl_en`) and PD22 (PWM7, held high) as GPIOs. `status`, `on`, `off`, `blink [n]`. **Works** (2026-11-26). |
| `pwm-backlight.sh` | Hardware PWM dimming on PWM7 (PD22, 50 kHz from 24 MHz / 480). `init`, `duty <0-100>`, `ramp [n]`, `status`. **Works** (2026-11-26). |
| `i2c.c` / `i2c.py` | I2C over FEL: SRAM payload + host tool. `scan`, `read`, `write`, `gt911-id`, `gt911-touch`. **Works** (2026-11-26). |
| `edges.c` / `uart_sniff.py` | Logic-analyser-style UART listener: timestamps every edge on an RX pin (mcycle, 0.2 µs polls), then estimates baud and decodes 8N1 on the Mac. `calibrate`, `listen uart1|uart4`. |
| `payload.ld` | Linker script for C payloads (entry at `0x40000`, request block at `0x40F00`). |
| `Makefile` | `make` builds `probe.bin` (load address `0x40000`). |

Both scripts use the Pico + SOIC-8 clip setup from the recovery doc and refuse
to write unless the two backup reads match and the boot0 header is where they
expect it. The erase/restore round trip has been checked against flashrom's
emulated chip, using a real dump of this unit's flash, and the whole
procedure was run on the bench unit on 2026-11-26 (results below).

## Tools

- `flashrom` (`brew install flashrom`) and the Pico running `pico-serprog`.
- `xfel` v1.4.0, built from source (needs `brew install libusb pkg-config`):
  `git clone https://github.com/xboot/xfel ~/src/xfel && make -C ~/src/xfel`.
- `riscv64-elf-gcc` (`brew install riscv64-elf-gcc`) to build `probe.bin`.

## First contact

1. **12 V off.** Clip on, Pico plugged in: `tools/fel/fel-enter.sh`
   (two ~45 s reads, then a few seconds of writing).
2. Take the clip off. Connect the unit's front USB port to the Mac with a cable
   whose **VBUS (red) wire is cut or taped**: both ends supply 5 V.
3. Power the unit on (the screen should stay dark: nothing boots), then:

   ```sh
   system_profiler SPUSBDataType | grep -B2 -A6 -i 1f3a   # Allwinner FEL = 1f3a:efe8
   ~/src/xfel/xfel version                                # expect a D1/F133 ID
   ~/src/xfel/xfel sid
   ```

4. If it's there, run the probe:

   ```sh
   make -C tools/fel
   X=~/src/xfel/xfel
   $X write 0x40000 tools/fel/probe.bin
   $X exec 0x40000
   $X hexdump 0x40100 0x40
   $X exec 0x40000 && $X hexdump 0x40100 0x10   # run counter goes 1 -> 2
   ```

   Expected: `0d f0 0d 60` at `+0x00`, run count `1`, `mvendorid` `0x5b7`
   (T-Head) at `+0x10`, `mhartid` `0` at `+0x28`, and a non-zero cycle count at `+0x38`.

5. Optional, while in FEL: `$X spinor` should detect the XMC chip over USB.
   If it does, the flash can be read and written with no clip
   (`$X spinor read 0 0x1000000 dump.bin`).
6. Put the boot0 header back, either way works:
   - **Over USB, no clip** (what we did on the bench):

     ```sh
     B=stock/nor/pre-fel-YYYYmmdd-HHMMSS.bin      # the backup fel-enter.sh made
     dd if=$B of=/tmp/hdr4k.bin bs=4096 count=1
     $X spinor write 0 /tmp/hdr4k.bin
     $X spinor read 0 0x1000000 stock/nor/post-fel-usb.bin && cmp stock/nor/post-fel-usb.bin $B
     ```

   - **With the clip:** power off, clip on, `tools/fel/fel-exit.sh`, clip off.

   Then power-cycle: normal boot.

**If nothing enumerates**, the front port probably doesn't reach the SoC's USB0
directly (the `U2` chip on the USB daughterboard may be a hub). Restore with
`fel-exit.sh` either way. The next step would be finding where USB0's data pins
(F133 pins 114/115) go.

## Bench result (2026-11-26)

```
$ xfel version
AWUSBFEX ID=0x00185900(D1/F133) dflag=0x44 dlength=0x08 scratchpad=0x00045000
$ xfel hexdump 0x40100 0x40          # after write + exec of probe.bin
00040100: 0d f0 0d 60 00 00 00 00 01 00 00 00 00 00 00 00   magic, runs=1
00040110: b7 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00   mvendorid 0x5b7, marchid 0
00040120: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   mimpid 0, mhartid 0
00040130: 2d 11 b4 00 00 00 00 80 69 a1 07 00 00 00 00 00   misa, ~500k cycles
```

A second `exec` without reloading gave runs=2. About 5 cycles per loop pass
suggests caches are off in FEL (xfel's own payloads switch them on at entry).
`xfel spinor` found the 16 MB chip and read at about 400 KB/s. The header was
restored over USB, a full read matched the backup, and the unit booted normally.

## Backlight (2026-11-26)

`tools/fel/backlight.sh on` lit the backlight from FEL and `blink` toggled it,
confirming the `sys_config` `[lcd0]` pin map: **PG15 = backlight enable, PD22
(PWM7) = brightness**, both active high, with no other power rail needing to be
switched on first. The panel isn't initialised, so the screen glows with no picture.

GPIO register layout on the F133 (base `0x02000000`, `0x30` per port): CFG0–3 at
`+0x00..0x0C`, DAT at `+0x10`, DRV at `+0x14..`, PULL at `+0x24..`. (The first
attempt used `+0x18` for PD_CFG2, which is actually PD_DRV1; harmless, but nothing lit.)

## PWM dimming (2026-11-26)

`tools/fel/pwm-backlight.sh init` then `ramp` dimmed the backlight smoothly up
and down. The register layout from Linux's `pwm-sun20i.c` is right for the F133:

```
CCU PWM_BGR 0x020017AC = 0x00010001   bus gate + reset de-assert (was 0 in FEL)
PCCR67      0x02000C2C = 0x00000000   24 MHz HOSC, no divide
PCGR        0x02000C40 = 0x00000080   ch7 clock gate
PER         0x02000C80 = 0x00000080   ch7 enable
PCR7        0x02000DE0 = 0x00000100   prescale /1, active high
PPR7        0x02000DE4 = 0x01df00f0   480-cycle period (50 kHz), 240 active (50%)
PD22 function 5 = PWM7
```

Duty changes written to PPR7 take effect while the channel runs.

## I2C and touch (2026-11-26)

`i2c.py` loads `i2c.bin` into SRAM, muxes the bus pins, and runs one transfer
per `xfel exec` (request block at `0x40F00`). A bus scan takes about 0.5 s.

| Bus | Pins | Device | How it was identified |
|---|---|---|---|
| TWI1 | PG14 SCL / PB04 SDA (func 4) | radio tuner (none answer in FEL) | `Config.ini` `radioI2cName=/dev/twi1`. Reset `radioResetGpio=105` = **PB5**, active-low (app numbering is port×100 + pin, A = 0). Still silent with PB5 high: **no tuner module fitted** on this build (`ANT` footprint empty, `bArmRadio=1`); FM runs on the Bluetrum companion chip over UART. |
| TWI2 | PE12 SCL / PE13 SDA (func 2) | `0x10`: **Apple MFi auth coprocessor** | `carplayI2cName=/dev/twi2`; `0x10` is the MFi chip's standard address; registers read all zero (not a tuner). |
| TWI2 | | `0x40`: **sound processor, likely ROHM BD3702** | `audioI2cName=/dev/twi2`; app has a `CAudioBD3702` driver; write-only (refuses reads), as BD37xx parts are. |
| TWI2 | | `0x44`: **Techpoint TP2850** camera decoder | Chip ID `0x28 0x50` at `0xFE/0xFF`; the kernel's `tp2804` driver (AHD/CVBS → BT656). |
| TWI3 | PG10 SCL / PG11 SDA (func 3) | `0x5d`: **Goodix GT911** touch | Product ID `911`. |

The app also carries drivers for parts this board doesn't use (QN8035, Si47xx,
TEF6686, TDA77xx tuners; PT2312 sound processor): one firmware for several board builds.

GT911: product `911`, firmware `0x1060`, stored config 1024 × 600, 10 points.
Live touches read cleanly. Raw frame is **landscape**: x 0→1024 left to right,
y 0→600 top to bottom (corners ≈ (90,112), (939,136), (925–955,482–505),
(85–106,449–483); a horizontal drag ran x 84→898 at y ≈ 310–347). The panel is
720 × 1280 portrait, so it's mounted sideways and the stock firmware rotates
the picture and scales touch to match. No reset sequence was needed: the GT911
was already at `0x5d` at power-on.

## UART listening and FEL clock (2026-11-26)

- **CPU clock in FEL ≈ 408 MHz** (`uart_sniff.py calibrate`, 1e9 mcycle in 2.45 s,
  twice). `xfel exec` returns before the payload finishes; time exec + the next
  read together.
- **UART1** (`mcuSerialName`, RX = PG13) and **UART4** (`btSerialName`, RX = PB03):
  no traffic in 5 s each. All four lines (PG12/PG13/PB02/PB03) read high even
  with the SoC's pull-down on, so they're held up externally: the link is
  wired, but the Bluetrum only answers when the F133 talks first.
- **Don't `xfel read32` arbitrary addresses.** Reading the CLINT timer at
  `0x1400BFF8` hung FEL (needed a power-cycle). Probe unknown regions from
  your own payload instead.

## Memory used

xfel's own helper code lives at `0x20000`–`0x32000` (DDR init, SPI) and
`0x45000` (read32/write32, used by `hexdump`). The probe sits at
`0x40000`–`0x40140`, clear of both. DRAM (`0x40000000`, 64 MB on the F133) is
only usable after `xfel ddr f133`.
