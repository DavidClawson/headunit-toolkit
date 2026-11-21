# Hardware & software findings

All derived from a real `LTTF133.img` (model "W022", 7-inch wireless CarPlay /
Android Auto head unit). Where something is inferred rather than confirmed on a
bench unit, it says so.

## SoC & OS

- **SoC:** Allwinner **F133** = the **D1s** — a single-core 64-bit **RISC-V**
  (XuanTie C906, `rv64gc`). Confirmed in the boot chain: C-SKY RISC-V
  toolchain string, "Bad Linux RISCV Image magic", `riscv64-unknown-linux-gnu`.
- **OS:** Allwinner **Melis** RTOS (RT-Thread based; "EPOS"). The binaries leak
  source paths like `ekernel/arch/riscv/rv64gc/mmu/module.c` — useful for RE.
- **App:** one stripped RISC-V binary, `init.axf` (LZMA-compressed inside
  MINFS), built against the Melis exported API.

## Boot chain

BootROM → `boot0` (SPI-NOR) → u-boot → Melis. The image also ships card-boot
artifacts (`boot0_card.fex`, `cardtool.fex`, `cardscript.fex`), i.e. the
platform supports booting from SD. **FEL** hardware recovery is referenced
throughout (`jump fel`, `crashdump mode, jump fel`). See
[boot-and-recovery.md](boot-and-recovery.md).

Storage is SPI-NOR holding a GPT/MBR with `bootA` / `ROOTFS` / `UDISK`
partitions. NOR is small and fully used; the SD card is the expansion surface.

## Display / touch

- Panel: **720×1280 portrait, rotated to 1280×720 landscape**
  (`sys_config_nor.fex`: `lcd_x=720`, `lcd_y=1280`). Startup/shutdown logo
  enables and types are configured there too.
- Touch: Goodix **GT911** (`gt911 read version` strings), plus drivers for
  several other controllers.

## FM tuner & RDS

The app has RDS fully compiled in — PS (station name), **RT (RadioText =
song/artist)**, PTY, TA/TP, AF, EON — and drivers for many RDS-capable tuner
chips: **TEF6686 / TEF665x**, **Si47xx** (4730/4732/4745/4754/4755/4702),
**TDA7703/7708/7786**, **QN8035**, TEF6621.

But: RadioText is never shown (no screen places `ViewShowStrRdsRt`), and the
default FM screen shows no RDS at all. Getting station name / track requires
(a) enabling RDS + setting the right region in Settings, (b) the specific
tuner fitted actually decoding RDS, and (c) for track text, UI + app work. See
[format-datav1.md](format-datav1.md).

**RadioText may be a resource-only mod — to be tested.** `ViewShowStrRdsRt` is
referenced *by name* inside `init.axf`, right alongside `ViewShowStrRdsPs`
(station name, which renders fine when placed) / `RdsEn` / `RdsSw`. These apps
typically update controls by name — "find control X on the active screen, set
its text." If so, the only reason RadioText is blank is that **no screen
instantiates the control**, which the MINFS repacker now lets us fix. The cheap
bench test is `scripts/experiment_rds_radiotext.py` (it places a
`ViewShowStrRdsRt` control on `RadioRds.data` by renaming the expendable
PTY-type label); if RadioText appears with no binary patch, now-playing-on-radio
is a pure resource edit. Pending hardware confirmation.

### Weather band (NOAA) — not possible, hardware limit

NOAA weather radio is **162.400–162.550 MHz** (VHF), above the FM broadcast
band (≤108 MHz). Every tuner this firmware drives (TEF668x, Si47xx 473x/475x,
TDA770x, QN8035) is **broadcast-only** — the synthesizer and RF front-end
cover FM + AM/MW/LW/SW and cannot reach 162 MHz; none is the dedicated NOAA part
(Si4707). The `init.axf` band enum is FM / AM / MW / SW / LW with region
band-plans (China, America1/2, Japan, Europe, Korea…) and **no 162-MHz
frequencies anywhere** (stray `WEATHER`/`WX` strings are fragments of embedded
base64, not a band). A firmware mod can't add a band the silicon can't tune.

## CarPlay / Android Auto (the part you can't rebuild)

Implemented as **licensed proprietary modules** loaded at runtime:
`carplay.mod` (872 KB) and `auto.mod` (920 KB), LZMA-compressed in
`data_udisk.fex`. The app gates them explicitly:

```
CarPlay invalid because no carplay.mod
CarPlay invalid because no license
AndroidAuto invalid because no auto.mod
AndroidAuto invalid because no license
```

Evidence points to an **MFi authentication coprocessor on I²C** (`mfi` string
in the kernel; `carplayI2cName` in the app). Architecturally the module is a
**link + H.264-decode + touch-injection engine**, not a self-contained UI —
the projected interface is rendered by the phone and streamed as video; the
head-unit app owns the surrounding chrome (`MainLinkCarPlay.data`,
`AndroidAuto.data`, `MirrorIphone/Android.data`). The app drives the module
through a narrow interface (`CarPlayInterface`, `carplayVideoWidth/Height`,
`carplayVolume`, `carplay_wifi`, `auto_wifi_name`).

Implication: these modules run **only on Melis, likely only on this hardware
(auth keyed per device)**. They are not yours to redistribute. Keeping
CarPlay/AA means keeping stock Melis + modules + license intact and modding
around them. See [legal-and-scope.md](legal-and-scope.md).

## Melis exported API surface (for a hypothetical compatibility layer)

~120 core functions: `esKRNL_*` (~53: tasks, timers, semaphores, queues,
mailboxes), `esFSYS_*` (~36), `esMEMS_*` (~11), `esDEV_*`/`esMODS_*` (~13).
The RTOS/FS half maps cleanly onto POSIX. The hard part of any wrapper is the
device/ioctl layer the media path needs — `disp` (compositor), `g2d` (2D),
`cedar`/VE (hardware H.264 decode), USB/WiFi, I²C — whose ioctl ABIs would have
to be reverse-engineered and bridged to Linux (DRM/KMS, Cedrus V4L2). That is
a libhybris-class effort and the dominant cost; it only makes sense if "open OS
*with* CarPlay on this exact chip" is a hard requirement.
