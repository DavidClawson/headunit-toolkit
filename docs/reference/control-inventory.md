# UI control inventory & cross-reference (stock LTTF133.img)

Reference data from the whole-image static analysis. These are **structural
identifiers** (control names) and counts — not vendor UI text. Everything here is
regenerable from a stock image with the toolkit (see [Regenerating](#regenerating)).

- [`controls-placed.txt`](controls-placed.txt) — **337** distinct `ViewShow*`
  names actually placed on a screen, across all 60 `*.data` files.
- [`controls-referenced.txt`](controls-referenced.txt) — **983** distinct
  `ViewShow*` names referenced by name inside `init.axf` (the app knows how to
  fill them).
- **Diff = 647** referenced-but-not-placed — the dormant set.

## Why this matters

The Melis app fills view controls **by name**. So for any "show X" idea the
decisive question is whether the control is already referenced in `init.axf`:

- **Referenced + placed** (337) → working, visible controls.
- **Referenced + not placed** (647) → the app can drive it, but no screen
  instantiates it. Splits into two very different effort classes (below).
- **Not referenced** → needs `init.axf` (RISC-V) work; a `*.data` edit alone only
  ever shows static sample text.

See [format-datav1.md](../format-datav1.md) for the by-name fill rule and the
RadioText worked case.

## The two classes of dormant control

| class | what it is | effort | examples |
|---|---|---|---|
| **Missing field on a shipped screen** | the `.data` exists; the control just isn't placed | **low** — place one widget + repack | RDS extras on `RadioRds.data`: `ViewShowStrRdsRt` (RadioText), `…RdsEn`, `…PtyEn`, `…AfEn`, `…TaEn` |
| **Whole dormant screen** | the app references a screen whose `.data` isn't shipped | **high** — author a new screen *and* the hardware must exist | DAB, TPMS, EV telemetry, EcLink (see below) |

Whole dormant screens referenced by the app but **not shipped** in the image:
`Dab.data`, `CarInfoBattery.data`, `EclinkWireless.data`, `MainLinkMirror.data`
(no TPMS/CarInfo screen exists at all).

## Dormant feature groups (referenced, not placed)

Counts over the 647-name diff. **These are multi-SKU code whose hardware isn't on
this cheap board** — authoring their screens would show static placeholders, not
live data (no tire sensors, no CAN feed, no DAB tuner variant). They mark the
platform's ceiling, not easy wins.

| group | count | notes |
|---|---:|---|
| TPMS tire-pressure | 20 | `…TpmsPressure/Temperature/Alarm/PairStatus{FL,FR,RL,RR}` + set-points — needs tire sensors |
| DAB digital radio | 15 | `…Dab{Name,Program,PTY,DLS,Freq,…}` — needs a DAB tuner variant/antenna |
| EV / vehicle telemetry | 24 | battery SoC/voltage/temp, engine temp, extra panel voltages — needs a CAN feed |
| Audio-output routing | 15 | `…AudioOutput{AUX,BT,CarBT,FM,SP}` — source-dependent, shown conditionally |
| Phone-link / mirror | 14 | EcLink, screen-mirror, QR codes, phone signal/battery |
| 360 / multi-camera | 9 | `…Camera{Back,MiddleDoor,Trunk,WholeCar}` — needs the extra cameras |
| Dashcam / recorder | 3 | `…CarRecorder` — needs a recorder module |

Also surfaced in the ungrouped remainder: extra EQ presets
(`…EqDance/Live/Soft`), `AnyKeyPowerUp`, `BackLightSensor`, `CarLife` (the
Chinese CarPlay-equivalent), analog-clock and animation toggles — alternate
controls for features that may or may not be wired here.

## The genuine resource-only wins

The real opportunities are the **missing fields on shipped screens**, where the
hardware is present and already feeding sibling controls:

- **RadioText** — `ViewShowStrRdsRt` is referenced (`init.axf` offset 1450828,
  between `RdsPs` and `RdsEn`) and `RadioRds.data` ships. Confirmed both-halves;
  only the app-auto-fill question remains (bench test:
  `scripts/experiment_rds_radiotext.py`).
- The other RDS extras (`AfEn`, `TaEn`, `PtyEn`) are the same class.

## Regenerating

These lists come from:
- **placed** — decode every `*.data` screen and collect widget names (the
  `headunit screen` decoder; the atlas script walks all screens).
- **referenced** — grep the **decompressed** `init.axf` for `ViewShow[A-Za-z0-9]+`
  (both ASCII and UTF-16LE). This needs the module/`init.axf` LZMA decompressor
  (see [format-minfs.md](../format-minfs.md) — `init.axf` is 3 raw-LZMA1 blocks).
- **diff** = referenced − placed.

The full per-screen widget atlas (names + kinds + geometry + sample text) is
regenerable the same way and is intentionally **not** committed here (it contains
vendor UI strings); generate it on demand from your own image.
