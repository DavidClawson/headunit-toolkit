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
