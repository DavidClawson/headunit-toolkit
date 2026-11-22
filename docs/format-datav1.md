# DATAV1.0 UI screen-layout format

The `*.data` files in `data_udisk.fex` are compiled screen layouts
(`Radio.data`, `RadioRds.data`, `SetupMenu.data`, …). Each is a flat list of
named widgets. All strings UTF-16LE.

## Header

| off | type | meaning |
|----:|------|---------|
| 0x00 | char[16] (utf-16) | magic `DATAV1.0` |
| 0x24 | u32 | widget count |
| 0x30 | — | section table: `(offset, count)` pairs pointing at image/font/string blocks |

## Widget record

Widgets are found by their UTF-16 name, which always begins `ViewShow…`. The
name prefix encodes the type: `ViewShowStr…` (text), `…Image…`, `…Button…`,
`…Slider…`, `…Wnd…`, `…List…`.

Relative to the start of the name field:

| off | type | meaning |
|----:|------|---------|
| name-0x38 | 4×i32 | geometry `(x, y, w, h)` — **verified for `Str` (text) widgets** |
| name-0x28 | i32 | font size / flag |
| name-0x24 | u32 | primary color `0x00RRGGBB` (Str) |
| name-0x20 | u32 | secondary / state color (Str) |
| name-0x1c | `"<<<"` | sentinel |
| name+0x00 | utf-16z | widget name |
| name+0xcc | utf-16z | sample / default value (`"Station name"`, `"108.00"`) |

> **Alignment caveat.** The `(x,y,w,h)` at `name-0x38` is correct for `Str`
> widgets (verified against raw bytes). `Button`/`Slider`/`Wnd` records carry
> one extra leading word, so their geometry is shifted by 4 bytes — the API
> exposes `geom_raw` for those and marks them in the CLI with `*`. Nailing the
> non-text geometry is a TODO.

The screen is **720×1280 portrait rotated to 1280×720 landscape** (panel
config in `sys_config_nor.fex`: `lcd_x=720`, `lcd_y=1280`), so x can run to
1280 and y to 720.

## Worked example: the RDS screen

`RadioRds.data` decodes to 37 widgets. The text fields (coords reliable):

| widget | meaning | x,y | w×h |
|---|---|---|---|
| `ViewShowStrRdsPs` | **station name (RDS PS, 8 char)** | 908, 83 | 100×30 |
| `ViewShowStrPtyType` | program type (NEWS/ROCK/…) | 480, 150 | 200×30 |
| `ViewShowStrTaInd` | traffic-announcement flag | 655, 150 | 200×30 |
| `ViewShowStrTpInd` | traffic-program flag | 376, 150 | 50×30 |
| `ViewShowStrMainFreqNum` | tuned frequency | — | — |
| buttons `Af` / `Ta` / `Pty` / `RdsEn` | AF / TA / PTY / RDS-enable | right side | — |
| `WndProgram` / `ListProgram` / `StrProgram` | station program list | — | — |

### Key finding
- The plain FM screen (`Radio.data`) has **no RDS fields** — only frequency,
  band slider and 6 presets. That's why a stock unit shows just the frequency.
- `ViewShowStrRdsPs` (station name) exists on the RDS sub-screen.
- `ViewShowStrRdsRt` (RadioText = song/artist) is **referenced by the app binary
  but placed on no screen**. Confirmed by cross-reference (below): the name sits
  in `init.axf` at offset 1450828, directly between `ViewShowStrRdsPs` and
  `ViewShowStrRdsEn` — and `RadioRds.data` is shipped. So RadioText is a
  **missing field on an existing screen**, the cheapest class of mod.

See [hardware.md](hardware.md) for the tuner/RDS side.

## Referenced-vs-placed: finding the moddable controls

A whole-image analysis cross-references two sets: every `ViewShow*` control name
**referenced by name in `init.axf`** (the app knows how to fill it) against every
name **placed on a screen** across all 60 `*.data` files.

- **983** distinct control names referenced in `init.axf`.
- **337** distinct names actually placed on a screen.
- **647** referenced-but-not-placed — the dormant set.

The 647 split into two very different categories, and the distinction is what
decides effort:

| category | what it is | effort | examples |
|---|---|---|---|
| **Missing field on a shipped screen** | the screen `.data` exists; the control just isn't placed on it | **low** — place one widget + repack | RDS extras: `ViewShowStrRdsRt` (RadioText), `…RdsEn`, `…PtyEn`, `…AfEn`, `…TaEn` on `RadioRds.data` |
| **Whole dormant screen** | the app references a screen whose `.data` isn't shipped at all | **high** — author a new screen *and* the hardware must exist | `Dab.data` (DAB radio), `CarInfoBattery.data` (EV/battery telemetry), TPMS tire-pressure (~21 controls, no screen), `EclinkWireless.data`, `MainLinkMirror.data` |

**Caveat that matters:** `init.axf` is a **shared multi-SKU codebase**. The
dormant whole-screen features (DAB, TPMS, EV/battery telemetry, EcLink) are UI
for hardware that **isn't fitted on this cheap model** — no tire sensors, no CAN
feed, no DAB-capable tuner variant wired. Authoring their screens would show
static placeholders, not live data. They mark the platform's ceiling, not easy
wins. The genuine opportunities are the *missing-field* rows, where the hardware
(FM tuner + RDS) is present and already feeding the sibling controls.

## Editing (in-place writer)

`headunit.datav1_edit` adds the writer side. A `*.data` file lives inside the
`data_udisk.fex` MINFS partition at `node.offset`. These editors keep the file's
byte length unchanged — nothing moves, and the single additive partition
checksum in `Vdata_udisk.fex` is recomputed exactly as `headunit.logo` does.
Edits that would **grow** a `*.data` file (e.g. adding a widget record) are not
done here; route those through `MinFS.repack` instead (see
[format-minfs.md](format-minfs.md) → *Repacking*), which relocates a grown file
into the partition's free arena without shifting the others.

Three edits are supported, all operating on an in-memory `ImageWTY`:

| function | writes | constraint |
|---|---|---|
| `set_geometry(img, data_path, widget, x, y, w, h)` | 4×i32 at `name-0x38` | `Str` widgets only (pass `force=True` to write the raw, shifted slot of other kinds) |
| `set_colors(img, data_path, widget, fg=, alt=)` | u32 at `name-0x24` / `name-0x20` | `Str` only; low 24 bits written, high-byte flags preserved |
| `set_value(img, data_path, widget, text)` | utf-16z at `name+0xCC` | `Str` only; must fit the original value buffer (zero-padded; refused if too long) |

Each returns a list of `Change` records (image-absolute offset, before/after
bytes) for verification. The value buffer capacity is the current string plus
its trailing zero padding, up to the next non-zero field — 200 bytes (0xC8) for
`ViewShowStrRdsPs`, for example.

CLI:

```
headunit edit-widget LTTF133.img RadioRds.data ViewShowStrRdsPs \
    --xywh 100 200 120 40 --fg ff0000 --alt 00ff00 --value "FM 101.1" \
    --out ./build
```

This writes `./build/Update/LTTF133.img` and prints the changed byte ranges.

**Verified end-to-end** against the stock image: editing `ViewShowStrRdsPs` in
`RadioRds.data` (move to `100,200,120×40`, fg→red, value→`"HELLO FM"`) changes
only the intended widget bytes plus the 4-byte checksum word in `Vdata_udisk.fex`;
the output is byte-identical elsewhere and the same size, the IMAGEWTY structure
and the partition checksum both re-validate, and a re-parse reads the new values
back.

### Crucial caveat — layout edits cannot add functional fields

Repositioning or restyling an **existing** widget is a pure resource edit and
takes effect on the unit. But making a control display **new runtime data**
depends on whether the app already knows how to fill that control. The Melis app
populates view controls **by name** (`init.axf` holds the code that pushes
tuner/RDS/clock values into named controls). So the real question for any "show
X" idea is: *does the app already reference that control name?*

- If **yes** (the name is in `init.axf` but just isn't placed on a screen),
  then simply **placing** the control may be enough — a pure resource edit, now
  that [the repacker](format-minfs.md#repacking-writing-files-back) can grow a
  `*.data` file to add a widget record.
- If **no**, the control also needs `init.axf` work (RISC-V patching) so the app
  fills it. A `*.data` edit alone will only ever show the widget's static sample
  text.

**Worked case — RadioText (`ViewShowStrRdsRt`, song/artist):** both halves of
the "resource-only?" test now check out from static analysis — the name *is*
referenced in `init.axf` (offset 1450828, between `ViewShowStrRdsPs`, which
renders fine when placed, and `ViewShowStrRdsEn`), and its screen `RadioRds.data`
*is* shipped. So this is a **missing field on an existing screen** — the most
optimistic case, plausibly pure resource. `scripts/experiment_rds_radiotext.py`
builds the test image (it places a `ViewShowStrRdsRt` control on `RadioRds.data`
by renaming the expendable PTY-type label); the one thing static analysis can't
settle is whether the app auto-fills a placed-but-previously-absent control from
the tuner's RDS 2A/2B decode — that's the bench test. See
[hardware.md](hardware.md) → FM tuner & RDS.
