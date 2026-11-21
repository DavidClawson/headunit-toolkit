# First flash on the bench unit

A short, safe checklist for the **very first** USB firmware-update test on a
fresh bench unit (a second W022 with no speakers attached, driven by USB-stick
updates). The ideal first test is flashing a `fix-strings` build and visually
confirming the menu-typo fixes landed and the unit still boots. This touches
only the MINFS resource partition — low risk, reversible by reflashing stock
(see [`modding-workflow.md`](modding-workflow.md) risk tiers).

## 1. Pre-flight

1. **Stash the restore point.** Copy the vendor's original `LTTF133.img`
   (the one the unit shipped with) to `stock/LTTF133.img` and never edit it —
   it's the base for every build and your reflash-to-stock image.
2. **Build the test image:**
   ```sh
   headunit fix-strings stock/LTTF133.img --out build
   ```
   This writes `build/Update/LTTF133.img` (curated menu-typo pass: `botton`→
   `button`, `RDS swich`→`RDS switch`, and the stray non-breaking space on
   "R-CAM/F-CAM Mirroring").
3. **Copy the whole folder.** Put the entire `build/Update/` folder onto a
   freshly **FAT32**-formatted USB stick (keep the `Update/` directory name and
   structure — the vendor flow looks for it).

## 2. Flash

1. Follow the vendor's update procedure as documented in
   [`modding-workflow.md`](modding-workflow.md) (copy `Update/` to the stick,
   insert, trigger the update).
2. The update is **software-triggered** from the running firmware, not a
   hardware button combo — see
   [`boot-and-recovery.md`](boot-and-recovery.md). Let it run to completion and
   reboot on its own; don't pull power mid-write.

## 3. Verify after boot (visual — no speakers needed)

1. **Unit boots normally** to the home screen.
2. **Touch-calibration prompt reads "button"** (not "botton"): "Click panel
   button, then click screen button".
3. **RDS screen shows "RDS switch"** (not "RDS swich") on the RDS radio screen.
4. **CarPlay / Android Auto still present** in the source list (confirms the
   resource repack didn't disturb the projection modules).
5. The `RdsSw` / RDS-switch label also appears in the engineer menu
   (`SetupConfig`); if you want to check it there, enter factory code
   **113266** (see [`factory-menu.md`](factory-menu.md)) — optional, the radio
   screen is the simpler check.

## 4. Recovery & safety

- This first test is a **Tier-A resource edit** — reversible by reflashing
  `stock/LTTF133.img` the same way. If anything looks wrong, reflash stock.
- The real **hardware safety net** for riskier future work (boot-chain edits)
  is **FEL + a full NOR backup**, not USB updates — see
  [`boot-and-recovery.md`](boot-and-recovery.md).
- **Take a NOR backup early**, while the bench unit is fresh and known-good:
  a ~$5 CH341A clip dump of the SPI-NOR into `nor_backup/` means any later
  flash is recoverable byte-for-byte. Do it once and experimentation is cheap.

## 5. If it won't boot / the update was rejected

- **Update not detected:** confirm the stick is **FAT32** (not exFAT/NTFS) and
  the `Update/` folder (with `LTTF133.img` inside) sits at the stick's root.
- **Update rejected / checksum error:** rebuild with `fix-strings` from a
  pristine `stock/LTTF133.img`; the repacker fixes the MINFS checksum
  automatically, so a bad image usually means the source was already modified.
- **Boots to stock behaviour (typos still present):** the flash likely didn't
  apply — re-trigger the vendor update and confirm it ran to completion.
- **Won't boot / blank screen:** reflash `stock/LTTF133.img`. If the unit is
  unresponsive to USB updates, that's what the FEL + NOR-backup path in
  [`boot-and-recovery.md`](boot-and-recovery.md) exists to recover — another
  reason to take the NOR dump first.
