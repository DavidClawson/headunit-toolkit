# Factory / engineer menu — access codes & map

The single biggest pain point for these units (per the YouTube research in
[`feature-ideas.md`](feature-ideas.md)) is **reaching the hidden
factory/engineering menu** — whole videos exist just to list access codes
(3368, 8888, 7890, 000000, 1617…). This documents **our W022 / LTTF133 unit's
actual codes and menu**, recovered by analysing `init.axf` and the `*.data`
screens. Nothing here requires flashing — the menu is reachable from the normal
on-screen Settings UI once you have the code.

> Read-only investigation. Codes below are the firmware's built-in **defaults**,
> extracted from the image; see [Confidence](#confidence--caveats).

## The codes

| Code | Unlocks | Config key | Confidence |
|---|---|---|---|
| **113266** | Factory / engineer menu (`SetupFactory` → `SetupConfig`) | `factoryPaswword` | High |
| **112233** | Boot-logo / card-update screen (`SetupLogo`) | `logoPassword` | High |

### Evidence

`init.axf` is LZMA-compressed inside the MINFS app partition (three
`LZMA_ALONE` streams, props byte `0x5d`, 32 KB dict, **no** length field and
**no** end marker — each stream is decoded by injecting `ff×8` as the size and
feeding it only its own bytes, sliced at the next `5d 00 80 00 00` header).
The decompressed blob carries the firmware's config-descriptor table, laid out
as repeating `<default-value>\0…\0<key-name>` records. The two password keys
sit right next to each other:

```
… colorLampGpioB\0\0 112233 \0\0 logoPassword \0\0\0\0 113266 \0\0 factoryPaswword \0 logoCardName …
```

The same value-before-key pattern holds across the whole table and is
independently verifiable on neighbours — e.g. `0.jpg`→`wallPaper`,
`20220101`→`defaultDateTime`, `LTT-CAR`→`productModel`, `LTTF133.img`→
`appUpdateFile`. So `factoryPaswword = 113266` and `logoPassword = 112233`
are read with the same confidence as those.

(`factoryPaswword` is the firmware's own spelling. A third key,
`loginPassword`, pairs with a `bStartUpEnterLogin` flag — an optional
"enter a password at every boot" lock, off by default — and reuses the same
password UI.)

## How to reach it (no flashing)

1. From the home screen open **Settings** (the normal gear/settings app — this
   is `SetupMenu`, see the map below).
2. Tap **Factory Set** (the menu item backed by `SetupPassword` →
   `SetupFactory`). Some builds label the tile *Factory* / *厂家设置*.
3. At **"Please enter password"** / **"Input password："** type **113266** and
   confirm. That opens the factory menu; its **Config Setting** button opens
   the full engineer parameter editor (`SetupConfig`).
4. To change the boot logo instead, use the logo/update tile and enter
   **112233** (`SetupLogo`).

Safe-use notes:

- **Change one setting at a time and note the original value.** The engineer
  editor exposes LCD voltages (`vcomVoltage`) and panel-drive values that can
  blank the screen if set wrong.
- The factory menu has **Restore Factory** and a **save / export / import
  config file** function (writes/reads `factory1.bin` on an SD card — the
  firmware references `F:\factory1.bin`). Export first = a one-tap backup of
  every engineer parameter before you touch anything.
- If 113266 is rejected on your physical unit, the installer may have changed
  it; the value is a config default, not a hardware constant. The tiles still
  exist — only the number would differ.

## Menu map

```
SetupMenu  (normal Settings — no code)
├─ SetupBasic ........ Basic Setup
├─ SetupRadioArea .... Tuner Region        ← region presets, NO code needed
├─ SetupWheel ........ steering-wheel keys
├─ SetupPassword ..... ─── password gate ───┐
├─ SetupEQ/SetupAudio  EQ Setup             │ enter 113266
├─ SetupVideo ........ Display settings     │
├─ SetupWallPaper .... wallpaper            ▼
├─ SetupScreenSaver .. screen saver     SetupFactory  (engineer menu)
├─ SetupTime ......... Clock              ├─ Config Setting → SetupConfig  (full editor, below)
├─ SetupLanguage ..... Language           ├─ Touch-Key Learning → SetupScreenLearn
└─ SetupVersion ...... Version            ├─ Main-Interface Button → SetupMainKey
                                          ├─ Gain Settings → SetupGain
                                          ├─ UI / Interface selection, License (Mp5+Mirror+CarPlay)
                                          └─ Wheel pull-up resistor
```

### `SetupConfig` — the engineer parameter editor (32 fields)

The high-value screen. Grouped by what it touches:

- **Radio / region:** `DefaultRadioArea`, `RadioModule`, `FmStopValue`,
  `AmStopValue` (seek thresholds), `StOpenValue` (ST stereo light value),
  `RdsSw` (**RDS switch**).
- **Mic / voice (BT call & voice assistant):** `MicGain`, `EchoCancel` +
  `EchoCancelMode` + `EchoDelayTime`, `AutoGainCtrl` + `MaxAgcGain` +
  `MinAgcGain` (**AGC**), `NoiseCancel` + `NoiseCancelMode` + `NoiseThreshold`.
- **Volume:** `StartUpMaxVolume`, `StartUpMaxTalkVolume` (power-on volume caps).
- **Video / camera:** `VideoOutputFormat`, `FrontCameraSw`, `BackDump`.
- **Display:** `VcomVoltage` (+ slider), `BackLight` (+ slider).
- **Links / misc:** `DefaultUsbIphoneLink`, `DefaultUsbAndroidLink` (CarPlay /
  Android Auto USB mode), `WifiFreq`, `DefaultColorLampMode`,
  `DefaultLanguage`, `DefaultBrakeWarn`, `CarType`.

### `SetupGain` — per-source volume gain (8 sliders)

`RadioVolGain`, `MediaVolGain`, `BtmVolGain` (BT music), `BtVolGain` (BT call),
`AuxVolGain`, `CarplayVolGain`, `GpsVolGain`, `LinkVolGain`. English labels in
`Setup.txt`: *Radio gain / Multimedia gain / BT music gain / BT call gain /
AUX gain / Navigation gain / PhoneLink Gain*, plus *Bluetooth MIC gain* and
*Navigation Mix Attenuation*.

### `SetupRadioArea` — region presets (reachable WITHOUT the code)

Ten regions: China, America1, America2, Japan, Russia, MidEast, Europe,
Brazil, Australia, EastEurope. This tile lives in the **normal** menu, so
switching FM/AM band plan is a no-code Tier-A change.

### `SetupAudio` — user EQ

10-band graphic EQ (30 / 60 / 125 / 250 / 500 / 1K / 2K / 4K / 8K / 16K Hz)
with presets Jazz / Classic / Rock / Pop / Flat / User.

### `SetupScreenLearn` — IR/panel key learning

Learns each function to a physical/steering key: Pre, Play, Next, Band, Vol+,
Vol−, Mute, Back, Main, Gps, Power, Mode, Dial, HangUp, Power-Screen,
Power-Mute, Siri.

### `SetupDebug` — developer toggles

`ScreenShotSw` (screenshot capture — pairs with `bEnableCapture`), `UsbMode`,
`PrintTouch` (log touch coords), `PanelVoltage1/2`. Not linked from the stock
menu tree we mapped; likely reached via a debug build flag (`bPrintLog`,
`DEBUG`).

## Settings → feature-roadmap mapping

Everything the [roadmap](feature-ideas.md) flagged as Tier-A/B is reachable
through this menu — several are plain config edits, no patching:

| Roadmap idea | Reachable via | No-flash? |
|---|---|---|
| **Volume-curve remap** | `SetupGain` (8 source gains) + `StartUpMaxVolume` / `StartUpMaxTalkVolume` in `SetupConfig` | Yes, in-menu |
| **Mic / AGC / echo / noise tuning** | `SetupConfig`: `MicGain`, `AutoGainCtrl`/`Max`/`MinAgcGain`, `Echo*`, `Noise*` | Yes, in-menu |
| **Radio region + RDS** | `SetupRadioArea` (no code) and `SetupConfig`: `DefaultRadioArea`, `RdsSw`, `Fm/AmStopValue`, `StOpenValue` | Yes, in-menu |
| **Key/volume beep, night brightness** | `SetupBasic` (beep) + `SetupConfig` `BackLight`/`VcomVoltage` | Yes, in-menu |
| **EQ presets / curve** | `SetupAudio` 10-band + presets | Yes, no code |
| **Custom boot splash** (shipped) | `SetupLogo` (code 112233) or `headunit logo` offline | Either |

Implication for the toolkit: the gain/mic/radio mods can ship first as a
**documented in-menu procedure** (enter 113266 → set these fields → export
`factory1.bin` backup), ahead of any binary patch. That export/import file is
also the obvious vehicle for a repeatable, shareable config profile.

## Confidence & caveats

- **High confidence** the *firmware defaults* are `113266` (factory) and
  `112233` (logo): same extraction method, same table, as fully-verified
  neighbouring key/value pairs.
- These are **defaults baked into the image**. A unit whose installer wrote a
  custom config could differ; the codes are values, not immutable constants.
- Menu **structure** (screen names, widget lists) is read directly from the
  `*.data` resources and is reliable. The exact **tile labels/paths** a user
  taps come from `Setup.txt` (English = column 3: *Factory Set*, *Configure
  Setting*, *Gain Settings*, *Tuner Region*, *RDS swich*, *Echo cancel*,
  *AGC Ctrl*, *Noise cancel*, etc.); a given build may arrange tiles slightly
  differently, which is exactly the "every video shows a different menu"
  complaint this doc exists to kill.
- The password **check logic** (whether `SetupPassword` compares against
  `factoryPaswword` vs `logoPassword` per entry point) is inferred from the
  two distinct keys and the `SetupFactory` / `SetupLogo` split, not from
  decompiled RISC-V control flow. The code values themselves are not inferred.

## Reproduce

```python
# from the repo dir, with LTTF133.img extracted to a temp dir
from headunit.imagewty import ImageWTY
from headunit.minfs import MinFS
import lzma

img  = ImageWTY.from_path(IMG)
mfs  = MinFS(img.read("data_udisk.fex"))
n    = mfs.find("init.axf")
raw  = img.read("data_udisk.fex")[n.offset:n.offset + n.stored_len]

HDR = b"\x5d\x00\x80\x00\x00"                      # props 0x5d, 32 KB dict
bounds = []
i = 0
while (j := raw.find(HDR, i)) >= 0:                # locate each stream
    bounds.append(j); i = j + 5
bounds.append(len(raw))

out = bytearray()
for a, b in zip(bounds, bounds[1:]):
    c = raw[a:b]
    d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
    out += d.decompress(c[:5] + b"\xff" * 8 + c[5:], 1 << 28)   # inject size

# out now contains the config table:  …112233\0\0logoPassword…113266\0\0factoryPaswword…
```
