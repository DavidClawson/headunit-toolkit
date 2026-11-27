# MCU link and radio control (from `init.axf`)

Static analysis of the stock app (`/apps/init.axf`, decompressed with
`headunit decompress --elf`; addresses are its virtual addresses). Not yet
exercised on hardware. T-Head custom instructions (`th.lrbu`, `th.extu`, ...)
were decoded by hand where objdump shows `.insn`.

## Which chip does what: "ARM" means the F133

`Config.ini` flags named `bArm*` (`bArmRadio`, `bArmCan`, `bArmDvd`, `bArmBeep`)
choose whether **the F133 itself** ("ARM", the vendor's name for the main SoC)
handles a feature, or delegates it to the **external MCU** on `mcuSerialName`.
The radio facade at `0xe91a2cf2`–`0xe91a2ea2` shows this directly: with
`bArmRadio=1` it calls the F133's own tuner drivers (`CRadioInterface`); with
`bArmRadio=0` it sends MCU commands instead (for example `B0 60 hi lo` to set a
frequency, `0xe918cc86`).

**This unit has `bArmRadio=1`: the F133 drives the tuner over I2C**
(`radioI2cName=/dev/twi1`, speed from `radioI2cFreq`), with reset on PB5
(`radioResetGpio=105`, active-low). An earlier note in this repo said FM ran on
the Bluetrum chip over UART; that was a misreading of `bArmRadio` and is wrong.

## Tuner drivers in `CRadioInterface`

Log strings in `0xe91dc000`–`0xe91e8000` show drivers for Si470x, Si473x,
Si4754, TDA7786, TEF6621, TEF665x, QN8035 (`8035ID=`) and a **"ca96xx" / 9636**
part. The ca96xx driver writes to I2C address `0x20` (7-bit **`0x10`**) via
`0xe91e4716`, and compares `radioI2cName` with `"/dev/twi2"`, so on some builds
that tuner sits on TWI2. RDS support (AF, TA, PI/PTY search, EON) is in the same
region. `radioModule=0` (module/area logged by `CRadioInterface Module:%d,Area %d`)
presumably means auto-detect; not confirmed.

**Bench status:** from FEL, TWI1 has no devices, even with PB5 released. Likely
the tuner's supply is off until the stock firmware (or the MCU) enables it; the
tuner IC itself may sit inside the antenna shield can. TWI2 `0x10` reads all
zeros; it could be the MFi chip (as `carplayI2cName=/dev/twi2` suggests) or a
powered-down ca96xx/QN8035-class tuner. Unresolved.

## MCU UART

- Port: `mcuSerialName=/dev/uart1` (PG12 TX / PG13 RX). Object `CMcuCtrl`.
- Baud: **115200** when `mcuChip` is 1 or 2 (this unit: 2), 57600 when 0
  (`0xe918be78`).

### Frame format (normal mode)

```
02 | B1 | CMD | LEN | DATA[LEN] | XOR
```

- Total length `LEN + 5`. `XOR` = XOR of every byte from `B1` through the
  last `DATA` byte. Bad checksums are logged (`mcu data check sum error`) and
  dropped (`0xe918bb7a`).
- **F133 → MCU:** `B1 = 0x80`. Built by `McuSend(cmd, len, data)` at
  `0xe918c1e6` (logs `Tx %s`).
- **MCU → F133:** `B1 = 0x81` (events) or `0x80`; dispatched by `CMD` at
  `0xe918b0fa`. `0x81` uses a 23-entry handler table at `0xe9274478`
  (CMD `a6 a8 ad ae af b2 b4 b6 b9 bb be c0 c2 c8 cb d4 d6 d5 bf da db de e1`).

### F133 → MCU commands seen (constant CMD at the call site)

| CMD | LEN | Context |
|---|---|---|
| `0xA0` | 0 | init |
| `0xAA` | 10 | radio settings block built from config getters + 3 flag bits (`0xe918cb98`) |
| `0xB0` | 5 | generic `[code, arg, 0, hi, lo]` (`0xe918cc5a`); codes seen: `0x60` set frequency (16-bit), `0x63`, `0x0B`, `0xA0`, `0xA6`, `0x96` on/off, `0xCE`, `0x1D` (screen on), `0xFE` |
| `0xB1`, `0xE6`, `0xD4`, `0xD6` | 5, 2, 2, var | init / timeout handling |
| `0xB3`, `0xA7`, `0xCF`, `0xBD`, `0xE0`, `0xB8` | 1, 2, 8, 1, 5, 12 | small wrappers near `0xe918c330`–`0xe918c650` |
| `0xB4`, `0xC4`, `0xC6`, `0xC8`, `0xC9`, `0xCB` | 6, 1, 1, 1, 11, 2–4 | Setup screen |
| `0xD8` | 60 | after `CMcuCtrl_Init Success` |
| `0xDA`, `0xDF`, `0xA5` | 1, 1, 4 | misc (`0xDF` near "Player") |

### MCU firmware-update mode (don't trigger)

When an update is in progress the receive path switches to `0xe91bba46`
(`McuUpdate`): the MCU bootloader announces itself with ASCII `RECEIVESTART`,
then frames are `AA 55 TYPE(1-3) + 12 bytes`, with firmware sent in 512-byte
blocks. The pre-dispatch hook `0xe91bb9bc` routes `81/C5`, `80/C4`, `80/C6` to it.

## Start-up handshake (verified on the bench, 2026-11-26)

Sending `A0` (`02 80 A0 00 20`, `tools/fel/mcu.py send A0`) from FEL makes the
MCU answer within 15 ms:

| Frame | Content |
|---|---|
| `80 A0 01 00` | acknowledges `A0`, status 0 |
| `81 DB 01 01` | ? |
| `81 C3` (20 bytes) | ASCII `AB5301_20260106_2125`: **MCU firmware; the "MCU" is a Bluetrum AB5301** (the LQFP48 "AB" chip) |
| `81 A6 09 00 04 00…` | ? |
| `81 BF 05` | ASCII `LTT-H` |
| `81 D4` × 6 | BT status lines: `IS`, `II`, `MMCAR BT` (device name), `MN0000` (likely PIN), `MZ414288002B52` (likely BT address), `IA` |
| `81 B2 … FE 00 00 05 00` | key frame with code `FE` |
| `81 B9 07 03 00…` | ? |
| `81 B2 00 CE` every 3 s | heartbeat |

**Unanswered heartbeats seem to make the MCU reset the F133:** about 2.5 minutes
after `A0`, with no replies, the unit reset (and booted stock through the FEL
window). Before sending `A0`, FEL ran for long periods without resets. The reply
the app sends is not yet identified (the app does send `B0` code `CE`).

## Front-panel keys (verified on the bench, 2026-11-26)

One frame per press, sent on the press. Holding a key sends nothing more: a
3 s hold of the phone key gives exactly the frame a short press gives, with no
repeats and no release frame. So the F133 can't see long-presses.

The front-panel buttons are read by the **MCU**, not the F133: in FEL, the
key ADC and every GPIO (with disabled pins opened as pulled-up inputs) stayed
still while buttons were pressed. The MCU sends one frame per press on UART1:

```
02 81 B2 06 | 00 KK 00 00 00 00 | XOR
```

The app's `B2` handler (`0xe918b43c`) maps `KK` through a jump table at
`0xe9273c34` to an internal key ID:

| Control | `KK` (seen) | App key ID |
|---|---|---|
| Knob push | `01` | 70 (presumably mute) |
| Knob clockwise | `04` | 65 (presumably volume up) |
| Knob counter-clockwise | `05` | 66 (presumably volume down) |
| Phone | `06` | special handler (call state check, `0xe91803ae`) |
| Home | — | **nothing seen anywhere**: not on UART1 (even after the `A0` handshake), UART4, LRADC, or any GPIO checked (ports B, D, E, G opened as inputs; PF0–PF6, PC0–PC1) |

Other `KK` in the table (not on this panel, likely steering-wheel/IR paths):
`02→64`, `03→67`, `07→365`, `08→71`, `09→17`, `0A→73`, `0C→101`, `1D→233`;
`0x10`–`0x15` → `KK+112`. Codes `0x87`–`0x98` take a separate path. Holding a
key sends nothing extra. How home reaches the app is still open: perhaps the MCU
forwards it only after more of the app's init sequence than `A0`, or it is
wired somewhere not yet checked.

## On the bench (FEL)

With no buttons pressed, `tools/fel/uart_sniff.py` hears nothing on UART1 or
UART4; the MCU only sends key events (above) unprompted. The capture shows
115200 baud, matching the app. A safe first transmit test would be a command the app sends at init
(for example `0xA0`, LEN 0: `02 80 A0 00 20`), but which MCU commands have side
effects is not mapped yet.

## Vehicle inputs on the harness: where each one probably lands (2026-11-27)

The bench unit's harness has an input bundle: steering-wheel control 1 and 2,
brake detect, `FCCD`, ground, and "back car control" (reverse trigger).
The unit also has an IR remote and a small photosensor on the front panel, a
wired hands-free microphone, and a video input. From `sys_config`, `Config.ini`
and the app's MCU frame handlers, most of them likely reach the F133 through the
MCU. **All of this is inferred and untested.**

| Input | Likely path | Evidence |
|---|---|---|
| IR remote | MCU decodes it and sends key frames (`81 B2`) | `sys_config` `[s_cir0] s_cir0_used = 0`, `cir_pin` commented out (the F133's IR receiver is unused); an `[ir_code]` table exists |
| Steering-wheel control 1/2 | MCU ADC (resistor ladder), sent as key frames | the factory menu's "Touch Learning" (key remap) |
| Brake detect | MCU, `81 A6` flags | `brakeDetectGpio=-1` (not an F133 pin); `A6` carries vehicle-input bits |
| FCCD (front camera trigger) | MCU, `81 A6` data[1] bit `0x01` | app log string "Fccd" in the `A6` handler |
| Back car control (reverse) | MCU, `81 A6` flags | the `A6` handler's "开始倒车" ("start reversing") path |
| Photosensor | MCU, maybe `81 B9` subtype 4 (a 0–100 level the app applies as brightness) | `B9` subtype 4 handling |
| Camera / video input | Techpoint TP2850 on TWI2 `0x44` (AHD/CVBS → BT656) into the F133 | chip ID read over I2C |
| Hands-free mic | probably the Bluetrum (BT hands-free), or the F133's ADC | `[MIC]` section, `micDetectGpio=-1` |

The `A6` frame seen so far (`00 04 …`, then `00 44 …` after source = Radio)
fits this. A live MCU-frame monitor in our firmware would let each input be
checked by hand (press a remote key, ground the brake wire, cover the sensor)
without FEL.
