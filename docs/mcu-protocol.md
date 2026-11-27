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

## On the bench (FEL)

`tools/fel/uart_sniff.py` heard nothing on UART1 or UART4 for 5 s each; all
four lines are held high externally. Consistent with the MCU only answering
the F133. A safe first transmit test would be a command the app sends at init
(for example `0xA0`, LEN 0: `02 80 A0 00 20`), but which MCU commands have side
effects is not mapped yet.
