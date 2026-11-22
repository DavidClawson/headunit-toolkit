# Feature ideas — demand-backed roadmap

Candidate mods ranked by **appeal × feasibility**, each tied to a real user
complaint and to the firmware hooks we actually found. "Feasibility" is for
*this* Melis/F133 unit specifically.

## How this list was built

Four YouTube research passes with `ytscan` (transcripts + top comments). The
first two were *complaint*-oriented; the second two were *wishlist*-oriented
(what people say they want / wish it could do):

- **Scan 1** — `wireless carplay android auto car stereo review problems`
  (12 videos, 641 comments). Skewed toward *portable screens / AI-box
  dongles*, a different product class; dongle-only gripes are excluded below.
- **Scan 2** — `atoto dasaita android double din head unit review problems
  settings` (12 videos, ~400 comments). Clean signal from *installed* units
  (ATOTO, Dasaita, Joying, Teyes, Xtrons).
- **Scan 3** — `car stereo android auto head unit features I wish it had`
  (12 videos). Wishlist framing; fed the *Customization & discoverability*
  section below.
- **Scan 4** — `double din head unit missing features suggestions mods
  customization` (12 videos). Same framing, mods/customization angle.

Scans 3–4 again skewed toward the pricier Android class (Kenwood, JVC, ATOTO)
and most transcripts were IP-blocked, so their signal is **comment-based and
representative, not a clean sample of our tier**. Reddit stayed blocked
(logged-out views return nothing) across all four. Raw reports live in the
ytscan repo (not committed here). Quotes below are representative, not
exhaustive.

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
deliverable. **Priority 0 — DONE:** the factory/engineer code and full menu map
are documented in [`factory-menu.md`](factory-menu.md) (factory menu = **113266**,
boot-logo screen = **112233**; extracted from `init.axf` and independently
verified — the firmware's own key names are `factoryPaswword`/`logoPassword`,
their typo. There's also a boot-login code `0000` gated by `bStartUpEnterLogin`,
and **five undocumented hard-coded 6-digit codes** near the menu/calib logic —
`001106, 112345, 230762, 123579, 666666` — purpose unknown, a bench-probe item). Several Tier-A/B items below turn out to be plain in-menu config
changes, no flashing — see that doc's roadmap-mapping table.

## Tier A — do first (high appeal, resource/config-level, low risk)

| idea | demand (quotes) | firmware hooks | notes |
|---|---|---|---|
| **Custom boot splash** ✅ done | *"I can't even change the startup logo"* | `Logo/logo1.jpg`, `stalogo1.jpg` | shipped as `headunit logo` |
| **Shareable config profiles (`factory1.bin`)** | *"every video shows a different menu"*; whole videos just listing codes | `SetupFactory` export/import → `F:\factory1.bin`; all `SetupConfig`/`SetupGain` fields | no flashing — a curated preset of engineer settings, see below |
| **Volume-curve remap** | *"volume at 2 was too quiet and 3 was super loud"* (multiple) | `SetupGain`, `SetupAudio`, `carplayVolGain` | smooth the step curve; first real patch |
| **Key / volume beep toggle** | *"how do you get the beeping to stop when you turn the volume up?"* | `SetupBasic`, `TipBox`, `VolumeBar` | likely a flag or a resource swap |
| **Night brightness floor** | *"screen brightness doesn't go low enough for night driving"* | `SetupBasic`, display cfg | lower the minimum backlight |

### Config profiles via `factory1.bin` — the no-flash distribution vehicle

The factory/engineer menu ([`factory-menu.md`](factory-menu.md)) has a **save /
export / import** function that writes and reads `factory1.bin` on an SD card
(the firmware references `F:\factory1.bin`). That one file is the whole set of
engineer parameters — every `SetupConfig` field (mic/AGC/echo/noise, radio
region + RDS, power-on volume caps, display voltages) plus the `SetupGain`
per-source gains. This makes it the **highest-leverage, lowest-risk deliverable
in the repo**: a complete tuning change ships as a small binary + a one-page
"enter 113266 → Import" instruction, no image flashing, no bricking risk, and a
one-tap **Export-first = backup** before anyone touches a setting.

Why it's compelling:

- **Directly answers the loudest complaint** — "every video shows a different
  menu, worst manual ever." A named, documented profile (e.g. *"balanced
  volume curve + hotter mic for hands-free calls"*) is exactly the support
  these units lack.
- **Bundles several Tier-A/B mods into one artifact** — volume-curve remap, mic
  /AGC tuning, and radio-region/RDS all live in the same file, so they ship
  together instead of as separate fiddly menu walkthroughs.
- **Reversible and portable** — stock `factory1.bin` is the restore point;
  profiles move between units of the same model by SD card.

What has to be nailed down before shipping one (open work, bench-unit tasks):

1. **Decode the `factory1.bin` format** — field layout, order, and whether it's
   a flat dump of the config table (likely, given the `<value>\0…\0<key>`
   descriptor table we already decompressed from `init.axf`) or a packed
   struct. Confirm whether it carries a checksum/version guard.
2. **Confirm portability scope** — same model/panel only? A profile that sets
   `VcomVoltage` or panel-drive values for a different LCD could blank a screen,
   so profiles must be tagged with the exact model they were captured on.
3. **Round-trip on the bench unit** — export stock, import unchanged, verify
   no-op; then export → edit one field offline → import → confirm the change
   took and nothing else moved. (This is the `factory1.bin` analogue of the
   `headunit logo` / `edit-widget` byte-diff verification.)

Toolkit angle: once the format is known, a `headunit profile` command could
read/diff/merge `factory1.bin` files offline — turning hand-tuned settings into
version-controlled, shareable presets, which is the "patches + guides, bring
your own agent" distribution model this project is aiming for.

## Customization & discoverability (from the wishlist scans)

Scans 3–4 (wishlist framing) didn't surface new *compute*-heavy asks — this
tier can't do those anyway — they hardened one cluster: people want to **make it
theirs** and **find the setting that already exists**. Both are squarely in our
safe-edit lane (strings, `*.data`, `WallPaper/`, logos) and several double as
documentation wins ("the option exists — here's where"). Ranked by
delight-to-risk:

| idea | demand (scan 3–4 quotes) | firmware hooks | effort | new? |
|---|---|---|---|---|
| **English / typo polish** ✅ started | the recurring *"I can't find / understand the menu"* theme | menu `*.txt` strings (`Setup.txt`, …) | low | `headunit fix-strings` ships the curated typo pass (now **6 fixes**: `botton`→button, `RDS swich`→switch, stray NBSP, + three fullwidth-colon `：`→`:` labels); English is otherwise good and 0 English cells are untranslated |
| **Theme / wallpaper packs** ✅ wallpaper swap shipped | *"I cannot… change my wallpaper to mimic my phone… I really wish I could"* (+6) | `WallPaper/`, color fields in `*.data` | low | `headunit wallpaper` swaps any/all of the four home wallpapers (dimension-matched, repacked); theme color packs still open |
| **Boot animation (startup video)** | "make it feel custom"; the splash/logo requests generally | `bPlayStartUpVideo`, `startUpVideoPath`, `StartUpVideo.data`, `cedar/psr_video.plg` | med | **new** — built-in startup-video player (Cedar HW decode). **Format:** supply an **H.264 `.mp4` (or raw `.h264`) ≤1280×720**, or a `.gif` for simple animation; path can be internal `e:\…` or a USB/SD mount. Config+file, not a binary patch — bench-gated on the path toggle (set via factory menu). See [hardware.md](hardware.md) |
| **Home-screen layout & shortcut remap** | *"can you change the layout? I don't see that menu option… my screen is split in 3 parts… I want to change it"* (+2) | `SetupMainKey` (factory menu), home `*.data` | low–med | **new** |
| **Boot-to-source** (auto-enter last source) | boot-speed gripes (*"takes a solid minute to boot… worst part"*); our RTOS boots fast — turn it into a feature | likely a config flag or small `init.axf` nudge | med | **new** |
| **Now-playing from BT metadata** | *"the only thing I want is an icon to show what's playing"* (+8) | BT AVRCP track/artist → a `*.data` screen + wire-up | med | adjacent to RDS RadioText (Tier B) |
| **Safer update flow + image validator** | *"the only thing I miss is OTA… using a USB drive is so ancient"* | offline: a `headunit validate` check before copy | low | **new** — can't do true OTA, but can de-risk the USB/SD path |

The honest finding from pulling the actual strings: the stock English is
**mostly fine** ("Basic Setup", "Factory Set", "Contrast:") — a full
retranslation isn't warranted. The real defects were a handful of typos
(`botton`→button ×2, `RDS swich`→switch) and stray whitespace, now fixed by
`headunit fix-strings`. More importantly, doing it built the **MINFS repacker**
(`replace-file` / `MinFS.repack`) — the keystone that lets *any* resource be
replaced at a different size (arbitrary-size logos, wallpapers, themes, future
translations), checksum-correct, verified by `scripts/repack_selftest.py`.

Notes and honest limits:

- The **logo tool already solves a named pain** from these scans — *"please add
  the Mitsubishi logo, can't seem to get the size right"* (+3) is exactly what
  `headunit logo`'s auto-fit (`encode_to_fit`) does. Good validation, not a new
  item.
- The single most-upvoted wish in the set was a **home-automation widget**
  (*"the one widget I want is to open/close the garage door"*, +69). True home
  automation is **out of scope** — no app runtime on this platform — but the
  underlying want ("a custom button that does a thing I choose") is partially
  addressable as a home-screen **shortcut** to a source/screen. Noted for the
  demand signal; scoped down.
- **Mic quality** came up again (*"upgrade the mic… cheap omnidirectional picks
  up background noise"*). The hardware mic is a hardware fix; our contribution is
  the AGC/noise/echo **tuning** already listed in Tier B and the factory menu.

## Tier B — worth doing (medium feasibility, some reverse engineering)

| idea | demand | firmware hooks | notes |
|---|---|---|---|
| **Mic / call-quality tuning** | call quality is a top recurring complaint; *"adjust the microphone volume during a phone call"*, *"adjust microphone sensitivity"* | `SetupAudio`/`SetupDebug`: `AGC Ctrl`, `Noise cancel`, `Noise Threshold`, `Echo cancel`, `Level`, `Max/Min` | likely behind the factory menu → ties to Priority 0 |
| **Clock persistence** | *"every 2 or 3 days I have to reset the time and date"* | `SetupTime` | sync from GPS/phone instead of relying on RTC |
| **RDS RadioText (song/artist) on the RDS screen** | *"Lyrics shown from Spotify?"*; general "show what's playing" | `RadioRds.data`, `ViewShowStrRdsRt` (referenced in `init.axf` @1450828, placed on no screen) | **strong resource-only candidate — both static halves confirmed** (name referenced next to `RdsPs`; its screen is shipped → a *missing field*, not a dormant screen). Only the app-auto-fill question is left; bench test ready in `scripts/experiment_rds_radiotext.py` (see [hardware.md](hardware.md)) |
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

**NOAA weather radio** belongs here too: it's at 162 MHz (VHF), and the fitted
broadcast tuner (FM/AM/MW/SW/LW only) physically can't tune it — a firmware mod
can't add a band the silicon can't receive. See [hardware.md](hardware.md).

**Dormant multi-SKU features (code present, hardware absent).** `init.axf` is a
shared codebase across many head-unit models, so it references whole feature
suites this cheap unit can't drive: **DAB digital radio**, **TPMS tire-pressure**
(~21 controls), **EV/battery & engine telemetry** (needs a CAN feed),
**FM-transmit**, and **EcLink/Mirror** phone-link variants. The control cross-
reference ([format-datav1.md](format-datav1.md)) shows these are referenced but
placed on **no screen** (their `.data` isn't even shipped) — and the enabling
hardware (tire sensors, CAN bus, DAB tuner variant) isn't on the board. Authoring
their screens would show static placeholders, not live data. Worth knowing so
nobody chases them as quick wins; the genuine resource-only wins are *missing
fields on shipped screens* (RadioText, RDS extras) where the hardware is present.

**Custom EQ sound (not the UI).** A full 10-band graphic EQ already ships
(`SetupAudio.data`, presets incl. a **User** slot); its *screen* is re-skinnable,
but the preset **curves/band frequencies live in `init.axf` + a hardware-DSP
`best1400_eq.bin`**, so actually re-voicing the sound is a binary/DSP patch, not
a resource edit. The moddable opportunity here is cosmetic (relabel/restyle),
not acoustic.

## Suggested order

Priority 0 (factory code/menu docs) unblocks several Tier-B items, so it comes
first. In parallel, the **Customization & discoverability** items need no bench
unit and no flashing — the **English/translation polish pack** and **theme/
wallpaper packs** are pure resource edits that can ship immediately and deliver
the highest delight-to-risk. Then **volume-curve remap** as the first real
binary/resource patch — small, high-demand, and a clean proof that config edits
work on real hardware (bench unit, after a NOR backup).
