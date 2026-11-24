# Dev log

Dated, honest notes from real work on the bench — what we tried, what worked,
what didn't, and the lessons. Newest entries first. Nothing here is firmware;
it's reverse-engineering facts and test results from a unit we own.

---

## 2026-11-24 — First bench session: pipeline + two mod lanes validated on hardware

Unit: a second **W022** (badge **JOY-W022**), no speakers, driven entirely by
USB-stick firmware updates. Draws ~0.75 A @ 12.8 V idle.

### ✅ What works (validated on the live unit)

- **The whole USB-update pipeline.** build → MINFS repack → checksum fix →
  copy `Update/` to a FAT32 stick → unit auto-enters upgrade on insert → ~2 min →
  reboots itself. CarPlay/Android Auto stayed intact (phone paired, projection
  app worked) after flashing — confirming resource repacks don't disturb the
  licensed modules.
- **Custom boot splash (image lane).** Replaced the boot `logo1.jpg` with a
  generated "MODDED ✓" image; it showed on power-on. `logo1` is confirmed the
  boot-splash slot.
- **Menu relabels / translations (string lane).** Renamed the launcher "Logos"
  tile → "HELLO" (same length, in place) and "F-CAM" → "Backup Cam" (longer,
  forced a relocation). Both showed. So the repacker's **in-place *and*
  relocation** paths are both honored on-device.

### ⚠️ Lessons (the hard-won part)

- **The firmware code-draws its prominent, dynamic UI.** Editing `DATAV1` widget
  attributes (color/geometry) on dynamic widgets does **not** change the screen —
  the app (`init.axf`) repaints them at runtime. Confirmed by moving+recoloring
  the radio frequency (`Radio.data/ViewShowStrMainFreqNum`): the bytes changed,
  the screen didn't. The launcher is the same story — `MainApp.data` has **zero**
  editable widgets; the tiles' icons and labels are painted in code. **Re-skins
  therefore go through images, not widget edits.**
- **A label string lives in several files — edit the one the shown screen reads.**
  The launcher grid reads `Main.txt` / `SystemBar.txt`, **not** `Setup.txt`
  (editing Setup.txt's copy did nothing). Files are 34-col UTF-16LE TSV, BOM +
  CRLF, English = column index 2; a label often repeats across the language
  columns of its row (edit only col 2).
- **Labels clip to a fixed field width.** "Backup Cam" rendered as "Backup…".
  The clip width is a code constant (the tile text box is code-drawn), so
  longer translations must fit the space — widening the field would need an
  `init.axf` patch (code-tier, not a resource edit).
- **Each flash is a complete image, not a patch.** Every build starts from
  pristine `stock/` and applies only that build's edits; a prior relabel reverts
  unless re-included. Stack edits into one build to keep several at once.
- **The reset-hole step is unnecessary** on this unit — it auto-restarts after a
  flash. A 123 GB stick worked despite the vendor's "≤32 GB" guidance.

### Factory menu + resilience

- **Factory menu is installer-locked.** Settings → Factory Set → the stock
  default password **113266** is rejected ("invalid") — the installer changed it
  and stored it persistently (USB resource-flashes don't touch the config area).
- Of the five hardcoded 6-digit codes clustered in `init.axf`
  (`001106 / 112345 / 230762 / 123579 / 666666`), **`666666` opens a hidden
  "在线升级 / Online Upgrade" (OTA) screen.** With no network it blocks on a server
  call and freezes the UI (touch + buttons dead). **A power-cycle fully recovered
  it — no brick.** Worst case from a whole session of resource/menu experiments
  was a 5-second reboot: good evidence the flash-and-go path is low-risk.
- **Side-finding:** the firmware has a built-in **networked OTA** client. Relevant
  to the Bluetooth/WiFi network discussion and the common "I wish it had OTA"
  complaint — worth a future look (what server, is it configurable?).

### Takeaways for the project

- The two **flash-and-go** lanes that matter most — **custom images** and
  **menu relabels/translations** — are proven. They directly answer the loudest
  user complaints (ugly splash, confusing/poorly-translated menus).
- Teardown / NOR backup is **not** needed for any of this; it's a developer
  safety net for the *riskier* code-tier R&D (native modules, boot chain), so
  end users never open the case.
- The browser configurator (`web/`) can now ship these lanes with confidence —
  they're hardware-validated, not hypothetical.
