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
- `ViewShowStrRdsRt` (RadioText = song/artist) is **defined in the app binary
  but placed on no screen** (0 occurrences across all `*.data`). Surfacing
  now-playing text therefore needs both a new widget record *and* confirmation
  that the app fills the control from the tuner's RDS 2A/2B decode.

See [hardware.md](hardware.md) for the tuner/RDS side.

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
takes effect on the unit. But making a control display **new runtime data** does
**not** work from a layout edit alone. The Melis app only populates the view
controls it already knows about: the app binary `init.axf` holds the code that
pushes tuner/RDS/clock values into named controls. So, for example, adding a
functional RadioText (`ViewShowStrRdsRt`, song/artist) field the stock UI lacks
requires **both** a layout change here **and** corresponding changes to
`init.axf` so the app fills the new control. A `*.data` edit by itself will only
ever show the widget's static sample text — do not expect the editor to surface
data the firmware was not already wired to display.
