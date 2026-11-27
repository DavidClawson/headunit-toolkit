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

## Memory used

xfel's own helper code lives at `0x20000`–`0x32000` (DDR init, SPI) and
`0x45000` (read32/write32, used by `hexdump`). The probe sits at
`0x40000`–`0x40140`, clear of both. DRAM (`0x40000000`, 64 MB on the F133) is
only usable after `xfel ddr f133`.
