# Feature ideas — demand-backed roadmap

Candidate mods ranked by **appeal × feasibility**, each tied to a real user
complaint and to the firmware hooks we actually found. "Feasibility" is for
*this* Melis/F133 unit specifically.

## How this list was built

Two YouTube research passes with `ytscan` (transcripts + top comments):

- **Scan 1** — `wireless carplay android auto car stereo review problems`
  (12 videos, 641 comments). Skewed toward *portable screens / AI-box
  dongles*, a different product class; dongle-only gripes are excluded below.
- **Scan 2** — `atoto dasaita android double din head unit review problems
  settings` (12 videos, ~400 comments). Clean signal from *installed* units
  (ATOTO, Dasaita, Joying, Teyes, Xtrons).

Reddit was blocked (logged-out views return nothing), so this is YouTube-only.
Raw reports live in the ytscan repo (not committed here). Quotes below are
representative, not exhaustive.

## The biggest unmet need isn't a feature — it's docs

The loudest, most-upvoted theme across both scans was **confusion and lack of
support**, not missing capability:

- *"The worst user manual ever… every video on YouTube has a different menu, so
  looking up help is no fun."*
- An entire 500k-view video exists just to list **factory-settings access
  codes** (3368, 8888, 7890, 000000, 1617…), with hundreds of grateful "this
  saved me" comments.
- r/CarAV: *"half a dozen threads a week about no-name Android stuff… don't be
  surprised if you run into trouble that nobody can help you solve."*

→ The knowledge base this repo produces is itself the highest-value
deliverable. **Priority 0: find and document our W022 unit's factory/engineer
code and hidden menu** (hooks: `SetupPassword`, `loginPassword`,
`PasswordProtected`, `SetupFactory`, "Please enter password" — the default may
be extractable from `init.axf`).

## Tier A — do first (high appeal, resource/config-level, low risk)

| idea | demand (quotes) | firmware hooks | notes |
|---|---|---|---|
| **Custom boot splash** ✅ done | *"I can't even change the startup logo"* | `Logo/logo1.jpg`, `stalogo1.jpg` | shipped as `headunit logo` |
| **Volume-curve remap** | *"volume at 2 was too quiet and 3 was super loud"* (multiple) | `SetupGain`, `SetupAudio`, `carplayVolGain` | smooth the step curve; first real patch |
| **Key / volume beep toggle** | *"how do you get the beeping to stop when you turn the volume up?"* | `SetupBasic`, `TipBox`, `VolumeBar` | likely a flag or a resource swap |
| **Night brightness floor** | *"screen brightness doesn't go low enough for night driving"* | `SetupBasic`, display cfg | lower the minimum backlight |

## Tier B — worth doing (medium feasibility, some reverse engineering)

| idea | demand | firmware hooks | notes |
|---|---|---|---|
| **Mic / call-quality tuning** | call quality is a top recurring complaint; *"adjust the microphone volume during a phone call"*, *"adjust microphone sensitivity"* | `SetupAudio`/`SetupDebug`: `AGC Ctrl`, `Noise cancel`, `Noise Threshold`, `Echo cancel`, `Level`, `Max/Min` | likely behind the factory menu → ties to Priority 0 |
| **Clock persistence** | *"every 2 or 3 days I have to reset the time and date"* | `SetupTime` | sync from GPS/phone instead of relying on RTC |
| **RDS station name on main screen + RadioText** | *"Lyrics shown from Spotify?"*; general "show what's playing" | `Radio.data`, `RadioRds.data`, `ViewShowStrRdsPs`, (absent) `RdsRt` | needs the DATAV1 *editor*; see [format-datav1.md](format-datav1.md) |
| **Backup-cam layout / guide-line toggle** | backup cam is the single most-asked install topic | `Ccd.data`, `CcdBackLine.data` | reclaim screen, toggle guide lines |
| **Reskin / theming** | *"installed car launcher and it feels like new"*; people want to customize | all `*.data` + `WallPaper/` | validates the whole modding angle |

## Tier C — hard / uncertain (module internals or hardware-tied)

| idea | demand | why hard |
|---|---|---|
| CarPlay/AA reconnect reliability | *"fails to connect 1 in 10… cycle the key"*; *"screen just says connecting"* | inside `carplay.mod`/`auto.mod` (proprietary) |
| Projection video resolution / aspect | *"can't fill the screen 16:9"* | `carplayVideoWidth/Height` exist but behavior is in the module |
| Wi-Fi reliability (2.4 vs 5 GHz) | *"wifi flips to off… not in range"* | hardware + `wireless.mod` |

## Not firmware-addressable (hardware — don't over-promise)

Internal-amp hiss / weak amp, RAM-bound slowness, screen glare, AM/FM module
failures, 2.4 GHz-only Wi-Fi radios, power-cycle-on-engine-crank, data-privacy
concerns about the stock units phoning home (mitigate at the router, not in
firmware — though documenting *what* it connects to is a fit for the project).

## Suggested order

Priority 0 (factory code/menu docs) unblocks several Tier-B items, so it comes
first. Then **volume-curve remap** as the first real binary/resource patch —
small, high-demand, and a clean proof that config edits work on real hardware
(bench unit, after a NOR backup).
