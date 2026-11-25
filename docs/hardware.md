# Hardware & software findings

All derived from a real `LTTF133.img` (model "W022", 7-inch wireless CarPlay /
Android Auto head unit). Where something is inferred rather than confirmed on a
bench unit, it says so.

## SoC & OS

- **SoC:** Allwinner **F133** = the **D1s** — a single-core 64-bit **RISC-V**
  (XuanTie C906, `rv64gc`). Confirmed in the boot chain: C-SKY RISC-V
  toolchain string, "Bad Linux RISCV Image magic", `riscv64-unknown-linux-gnu`.
- **OS:** Allwinner **Melis** RTOS (RT-Thread based; "EPOS"), **version
  4.x-release-1.2** (from a build-path leak in `auto.mod`). The app is built on
  Melis's **`livedesk/beetles`** application framework (`…/beetles/CarApp/Build/
  F133_Melis/…`). Binaries leak source paths like
  `ekernel/arch/riscv/rv64gc/mmu/module.c` — useful for RE.
- **Loadable modules:** the app pulls native code at runtime via the `esMODS_*`
  loader from a **fixed list of `d:\mod\*.mod` slots** (13 named: auto, carplay,
  cedar, carlife, eclink, miracast, oak, pace, sensor_play, update, vcoder,
  video_play, wireless). **8 ship** in `/mod/` (`auto`, `carplay`, `cedar`,
  `sensor_play`, `update`, `vcoder`, `video_play`, `wireless`, alongside
  `display`, `mixture`, `ginkgo`, `slib`, `charset`, and the `willow/` image
  decoders). **5 are absent** and skipped gracefully: `oak`, `pace`, `miracast`,
  `carlife`, `eclink`. (An earlier parser bug hid half of `/mod/`; corrected
  2026-11-24.) Modules are **fixed-address RISC-V ELF sections** stored per
  section in MINFS, with an `ePDK.mod` descriptor (see
  [format-minfs.md](format-minfs.md)); `auto.mod` even statically links OpenSSL.
  The absent slots are a resource-level injection surface. See
  [lua-host.md](lua-host.md).
- **App:** one stripped RISC-V binary, `init.axf` (2,081,328 bytes; three
  concatenated raw-LZMA1 blocks inside MINFS — see
  [format-minfs.md](format-minfs.md)), built against the Melis exported API.
- **DRAM:** 512 MiB DDR3 @ 528 MHz (base `0x4000_0000`); CPU @ ~1008 MHz. The
  full `[dram_para]` block is in `sys_config_nor.fex`, reusable for a D1s DRAM
  init if we ever rebuild the boot chain.
- **Boot integrity is a checksum only — no secure boot.** boot0 ships as a plain
  `eGON.BT0` image (not a secure `TOC0` container), so the secure-boot efuse is
  necessarily unburned; the only check over any partition is the 32-bit additive
  checksum our repacker already fixes. The sunxi verified-boot *code* is compiled
  into u-boot but dormant. No AVB/dm-verity/signature/pubkey anywhere. **Every
  resource mod we build flashes and boots once the checksum is corrected** — see
  [boot-and-recovery.md](boot-and-recovery.md).

> **The shipped DTB (`sunxi.fex`) is misleading** — it decompiles to a
> `sun8iw19 / Cortex-A7` (ARM V833) tree with wrong register addresses, because
> the F133 BSP was forked from the sun8iw19 tree and kept the old identity. The
> u-boot blob is unambiguously RISC-V (`sun20iw1p1`). Trust the RISC-V u-boot +
> `sys_config` pins + the D1s manual, **not** the DTB's CPU/GIC/reg nodes.

## Boot chain

BootROM → `boot0` (SPI-NOR) → u-boot → Melis. The image also ships card-boot
artifacts (`boot0_card.fex`, `cardtool.fex`, `cardscript.fex`), i.e. the
platform supports booting from SD. **FEL** hardware recovery is referenced
throughout (`jump fel`, `crashdump mode, jump fel`). See
[boot-and-recovery.md](boot-and-recovery.md).

Storage is SPI-NOR holding a GPT/MBR with `bootA` / `ROOTFS` / `UDISK`
partitions. NOR is small and fully used; the SD card is the expansion surface.

### Debug & probe interfaces (from `sys_config_nor.fex`)

The stock image's `sys_config_nor.fex` is plain-text INI and names the vendor's
pin wiring directly — no probing needed to find the ports:

- **Debug UART console:** `uart_debug_port=3`, **TX = PB06, RX = PB07**, at
  **576000 baud** 8N1 (note: *not* 115200 — the common mistake).
- **USB-OTG / FEL:** USB0 (`usbc0`, `usb_used=1`) — the `sunxi-fel` recovery/
  flashing port.
- **I²C:** touch GT911 on **TWI3** (PG10/PG11, addr `0x5d`); the default
  `twi_port=2` → **TWI2** (PE12/PE13) is the likely MFi/CarPlay-coprocessor bus.
- **JTAG:** present on PE4… but disabled (`jtag_enable=0`).

Caveat: PB06 is listed both as `uart_debug_tx` and `usbc0`'s `usb_id_gpio` (a
reused-template artifact); the debug UART takes it in practice. Full tiering of
how to use these — FEL, UART, custom app — is in
[boot-and-recovery.md](boot-and-recovery.md#hardware-probing--hal-access-talk-to-the-board-directly).

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

The region enum in `init.axf` is CHINA / AMERICA1 / AMERICA2 / JAPAN / RUSSIA /
MIDEAST / EUROPE / BRAZIL / AUSTRALIA / EASTEUROPE / KOREA, with FM/AM(+MW/LW/SW)
bands. **DAB** control code (`CDabCtrlW2`) and **FM-transmit** code
(`bRadioTransmit`) are also compiled in — both dormant on this model (see
"Dormant capabilities" below).

But: RadioText is never shown (no screen places `ViewShowStrRdsRt`), and the
default FM screen shows no RDS at all. Getting station name / track requires
(a) enabling RDS + setting the right region in Settings, (b) the specific
tuner fitted actually decoding RDS, and (c) for track text, surfacing the
control. See [format-datav1.md](format-datav1.md).

**RadioText is a strong resource-only candidate — both static halves confirmed.**
`ViewShowStrRdsRt` is referenced *by name* inside `init.axf` (vaddr `0xe9262348`,
directly between `ViewShowStrRdsPs` — which renders fine when placed — and
`ViewShowStrRdsEn`), and its screen `RadioRds.data` *is* shipped. These apps
update controls by name ("find control X on the active screen, set its text"), so
the only reason RadioText is blank is that **no screen instantiates the
control** — exactly what the MINFS repacker now lets us fix. The one thing static
analysis can't settle is whether the app auto-fills a placed-but-previously-absent
control from the RDS 2A/2B decode; that's the bench test,
`scripts/experiment_rds_radiotext.py` (it places a `ViewShowStrRdsRt` control on
`RadioRds.data` by renaming the expendable PTY-type label). If RadioText appears
with no binary patch, now-playing-on-radio is a pure resource edit. Pending
hardware confirmation.

### Weather band (NOAA) — not possible, hardware limit

NOAA weather radio is **162.400–162.550 MHz** (VHF), above the FM broadcast
band (≤108 MHz). Every tuner this firmware drives (TEF668x, Si47xx 473x/475x,
TDA770x, QN8035) is **broadcast-only** — the synthesizer and RF front-end
cover FM + AM/MW/LW/SW and cannot reach 162 MHz; none is the dedicated NOAA part
(Si4707). The `init.axf` band enum is FM / AM / MW / SW / LW with region
band-plans (China, America1/2, Japan, Europe, Korea…) and **no 162-MHz
frequencies anywhere**. (Correction to an earlier note: the stray `WEATHER`
string is **not** a base64 fragment — it's the **RDS PTY-16 program-type label**;
the full PTY enum NONE/NEWS/…/WEATHER/FINANCE/…/ALARM is present.) A firmware mod
can't add a band the silicon can't tune.

## Boot splash & startup video (animation is supported)

The early splash is a single static JPEG (`Logo/logo1.jpg` + `stalogo1.jpg`,
1280×720) — one image slot, not animatable. **But the firmware also has a
startup-*video* player.** `init.axf` carries `bPlayStartUpVideo` (an on/off
flag), `startUpVideoPath`, a `StartUpVideo.data` screen, `Animation total
time %d` / `animationType`, and `Play end,exit startup video`; it references
`.mp4`/`.avi`/`.gif` and ships the Allwinner **Cedar** hardware video decoder
(`cedar/psr_video.plg`, plus H.264 paths like `f:\dump-%d.h264`). Removable
media is probed at `/mnt/%c/%svideo` and `/mnt/%c/%spicture`, and the internal
startup logo lives at `e:\stalogo.jpg`.

So a **boot animation is feasible as a video clip** (not hand-animated frames) —
likely a config/resource feature (enable `bPlayStartUpVideo`, supply a clip at
`startUpVideoPath`) rather than a binary patch. Open questions for the bench
unit: the exact codec/container/resolution the player expects (Cedar + the
H.264 paths point at H.264), whether the path is internal (`e:`), removable
(`/mnt`), or inside the data partition, and whether it's exposed in a menu. See
the roadmap in [feature-ideas.md](feature-ideas.md).

## Power, audio, and radios (from DTB + sys_config)

- **PMU:** X-Powers **AXP2101** (DTB `x-powers,axp2101`); concrete regulator
  rails (dcdc1-5, aldo2/3) — relevant for power sequencing in any u-boot port.
  (`[pmu1_regu]`'s "axp28_*" names are stale template text.)
- **Audio:** `sys_config` marks audio `used=0`, but the DTB enables the SoC
  **internal codec** (`codec@0x05096000`) plus an **AC108 quad-mic ADC** on I²S
  (the hands-free mic array); the external **speaker amp enable is PH9**
  (active-high). Two analog audio-processor driver classes ship (`CAudioPT2312`,
  `CAudioBD3702`) and there's a hardware-DSP path that flashes `best1400_eq.bin`
  coefficients. A full **10-band graphic EQ** (30 Hz–16 kHz, presets
  Jazz/Classic/Rock/Pop/Flat/**User**) already ships — see
  [feature-ideas.md](feature-ideas.md); the EQ *screens* are re-skinnable but the
  preset curves live in `init.axf` + the DSP blob, so re-tuning the sound is not
  a resource edit.
- **WiFi:** SDIO on MMC1 (`sdc1`), `wifi_mod_sel=3`, power rail `aldo1` — but the
  **chip isn't named** in the fex; identify it from the physical module or the
  Melis driver. **Bluetooth:** `[bt_para]` is empty and the DTB configures no BT,
  yet the UI has full BT/phone/call screens — so BT is wired some other way
  (likely a combo WiFi/BT module). Both are **bench-unit questions**.
- **I²C map:** touch GT911 on **TWI3** (PG10/PG11, `0x5d`); the **MFi/CarPlay
  coprocessor is on TWI2** (PE12/PE13) — independently confirmed by `init.axf`
  (its only compiled TWI node is `/dev/twi2`), the DTB, and `sys_config`'s
  `twi_port=2`. The MFi 7-bit slave address isn't a literal in `init.axf` (the
  auth transaction lives in `carplay.mod`).

## Dormant capabilities (code present, hardware absent)

`init.axf` is a **shared multi-SKU codebase**, so it carries UI and control code
for features this cheap model doesn't physically support. The whole-image
control cross-reference ([format-datav1.md](format-datav1.md)) found these
**referenced but shipped on no screen**, and their enabling hardware isn't on the
board:

- **DAB digital radio** (`CDabCtrlW2`, 15 `…Dab…` controls, no `Dab.data`) — the
  TEF6686 family *can* do DAB, but needs the right tuner variant + antenna.
- **TPMS tire-pressure** (~21 `…Tpms…` controls, no screen) — needs tire sensors.
- **EV / vehicle telemetry** (`CarInfoBattery.data`, battery SoC/voltage/temp,
  engine temp) — needs a CAN feed.
- **FM transmit** (`bRadioTransmit`) and **EcLink/Mirror** phone-link variants.

These are the platform's ceiling markers, not easy wins: authoring their screens
would display static placeholders, not live data. The real resource-only
opportunities are *missing fields on shipped screens* (RadioText and the other
RDS extras), where the hardware is present and already feeding sibling controls.

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
