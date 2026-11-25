# A Lua scripting host — turning the unit into an open platform

> **Status: design / north-star.** Nothing here is built yet. The keystone's
> research half is **done** (module format, loader ABI, and a data-driven
> injection point are all mapped, as of 2026-11-24); what remains is building and
> bench-testing a first module. Open questions are flagged below. This doc captures the architecture and the evidence so we
> don't lose it.

## The idea in one line

Ship **one** native host module once; after that, every new feature is a
**folder of Lua scripts + screens dropped on the SD card** — no compiler, no
reflashing, no risk to the stock firmware or the licensed CarPlay/Android Auto.
The dashboard becomes hackable like a Pico or Arduino, and the community writes
"apps" as data.

## Why this shape (the reframe)

The platform has one hard wall: producing native code for Melis (we have the
exported-API symbol list and now a confirmed loader, but no SDK/toolchain — see
[the dependency chain](#dependency-chain--milestones)). The SD card does **not**
remove that wall. What it does is change *what you spend the one hard investment
on*: instead of hard-patching a single feature into `init.axf`, you build a
**general loader + interpreter once**, and then all future features are
drop-on-SD artifacts. That is the maximal version of this project's "patches +
guides, bring your own agent" goal — now "bring your own *app*."

It also inverts the usual risk. Resource edits are safe but limited; `init.axf`
patches are powerful but brittle and per-feature. A scripting host is a *third*
option: powerful **and** safe **and** reusable, because the dangerous native
part is written once and the per-feature part is sandboxed script.

## What the platform already gives us (evidence)

All confirmed by static analysis of the stock `LTTF133.img` (see
[hardware.md](hardware.md), [format-minfs.md](format-minfs.md)):

- **A real module loader.** `init.axf` links the Melis `esMODS_*` family
  (`esMODS_MClose`, `esMODS_MUninstall`, …). The OS loads code modules at
  runtime — that is exactly how `carplay.mod`, `auto.mod`, and the Cedar
  `*.drv`/`*.plg` plugins are brought in. We build on a real mechanism, not an
  invented one.
- **Removable media is already scanned.** The app probes `/mnt/%c/%svideo` and
  `/mnt/%c/%spicture` and exposes two SD/USB mounts (`SdCardName`,
  `SdCard2Name`). SD-as-*data* works today; SD-as-*code* is what we'd add.
- **No interpreter ships.** Grepping the decompressed `init.axf` for
  Lua/JS/Python/VM/bytecode found nothing (only unrelated "scan script" /
  PostScript hits). The interpreter is ours to embed.
- **Boot is open.** No secure boot, checksum-only
  ([boot-and-recovery.md](boot-and-recovery.md)) — a host module placed in the
  image flashes and boots once the checksum is fixed.
- **Memory budget:** 512 MiB DRAM, single RISC-V C906 @ ~1 GHz, shared with
  Melis + CarPlay. Fine for Lua-scale logic and simple UIs; not for heavy apps.

## Two flavors (and why Lua)

- **A — native modules from SD.** Reverse the `.mod` ABI + `esMODS_Install`
  interface, build modules with the (missing) Melis toolchain, host scans SD and
  installs them. Powerful, but every module is a native compile **and** a bad
  native module can corrupt the whole system.
- **B — embedded interpreter (the "browser"), recommended.** Build *one* native
  host module that embeds a small VM; "SD modules" are then **scripts + data**,
  not binaries. The native barrier is paid once.

**Lua** is the natural VM: ~200 KB, clean RISC-V, designed for embedding in C,
and — crucially — designed to be **sandboxed and resource-bounded** (see below).
MicroPython is an alternative but heavier. Flavor B is recommended because it is
the only one that makes the ecosystem both *accessible* (no compiler) and *safe*
(a script can't take down CarPlay).

## The HAL surface a script can reach

Architecture rule: **the host owns every hardware handle; Lua only ever sees a
curated, sandboxed API table.** Scripts never call `esDEV_*` directly. This is
how NodeMCU / LÖVE / OpenComputers work — safer and easier than raw bindings.
Mapped to our confirmed device map:

| Subsystem | Feasibility | How the host exposes it |
|---|---|---|
| **Display** | strong | `disp` compositor supports layers; give a script its own layer + a canvas (text/shapes/images via `g2d`) |
| **Touch** | strong | host gets GT911 events (TWI3) → forwards `onTouch(x,y)` to the active script |
| **Sound playback** | feasible | host wraps audio-out (internal codec); script plays tones/clips; polite mixing with the main path needs care |
| **Microphone** | maybe / contended | AC108 mic array as buffer reads for light uses (VU meter, level trigger); the call/CarPlay subsystem likely owns this path, and heavy DSP won't fit one core |
| **Car telemetry (read-only)** | bonus | host surfaces FM freq / RDS text / clock / volume from app state so scripts can *react* |
| **Bluetooth serial (SPP/RFCOMM)** | promising, gated | the firmware already has RFCOMM machinery (`RecCmd_Scan/RemoteAddr/UUID/RfcommData`, see [re-notes.md](re-notes.md)); if that client is reusable, the host exposes `bt.open(addr, uuid)` → byte stream — enough to talk to an **ELM327 OBD-II dongle**. Open RE: general-purpose vs. vendor-bound |
| **Network (WiFi)** | feasible | the unit has WiFi (RTL8723AS combo); host exposes sockets/HTTP to scripts that joined the phone hotspot or home AP. **Not** Bluetooth — there's no BT-PAN/tether stack ([re-notes.md](re-notes.md)) |
| **I²C / GPIO** | gate behind a capability | possible but lock it so a script can't wedge a bus the system needs (e.g. MFi on TWI2) |

## The sandbox / watchdog model (the reason to pick an interpreter)

The requirement "a script can't hang or break the other features" is essentially
*free* with an embedded Lua VM, and nearly impossible with native modules. The
mechanisms:

- **Own Melis task, below UI/CarPlay priority.** The scheduler preempts a
  spinning script; critical tasks never starve. (`esKRNL_*` gives us tasks and
  priorities.)
- **Instruction-count hook** (`lua_sethook` with `LUA_MASKCOUNT`) — abort a
  script after N bytecode ops. No infinite loop can hang the host.
- **Memory cap** via a custom Lua allocator — a script can't OOM the 512 MiB;
  allocation past its cap fails gracefully.
- **Per-frame time budget** — a script that overruns its slice is killed and the
  error surfaced ("script X exceeded its budget").
- **Capability sandbox** — scripts see only the vetted API table; strip
  `os.execute` / `io` / raw filesystem; each script gets a sandboxed storage dir.
- **Crash isolation via `pcall`** — a Lua error unloads *only that script*;
  everything else keeps running.

The decisive contrast: a bad **native** SD module can corrupt the system; a bad
**Lua** script gets caught and unloaded. That is why Flavor B is the safe one.

## Publishing it as a clean open ecosystem

The legal story is clean, and keeps the repo's "code + knowledge, not firmware"
rule intact:

- **Everything shippable is ours or permissive:** the host is our code, Lua is
  MIT, the API bindings are ours, scripts are user content. **Nothing touches
  `carplay.mod` / `auto.mod`** — they stay intact, licensed, and loaded by stock
  Melis. See [legal-and-scope.md](legal-and-scope.md).
- **Caveat:** build the host *without* bundling Allwinner's SDK headers/vendor
  code — ship our clean-room ABI knowledge + build instructions, not their SDK.
- **The deliverable:** a documented API, example scripts (clock faces, gauges, a
  VU meter, a g-meter, mini-games), a gallery, and "drop a folder on the SD card"
  install. An open app ecosystem for a dashboard, with a how-to-write-a-script
  guide as the on-ramp.

## Candidate flagship apps

Two concrete first-wave apps that ride the HAL surface above and double as the
"why this platform is cool" demos. Both are **clean-room** (no proprietary code)
and ride radios the unit already has ([re-notes.md](re-notes.md)).

### 1. OBD-II diagnostics (the flagship)

A Bluetooth **ELM327** dongle speaks SPP/RFCOMM — the exact transport the
firmware already contains. Vehicle data over an open serial link, no CAN tap, no
internet, no licensed anything.

- **Design it as a "Diagnostic mode," not a live overlay.** The user enters it
  while parked: the host connects the dongle, does its work, disconnects. This
  **sidesteps the big open question** — whether the Melis BT stack can hold the
  OBD dongle *and* the phone (CarPlay) connected at once. BT Classic is
  multipoint and wireless CarPlay's heavy data is on WiFi, so concurrency *may*
  work, but it isn't proven; diagnostic mode doesn't need it. A live background
  gauge cluster is a **stretch goal** that ships only if the bench proves the
  stack tolerates a second device + WiFi/BT coexistence.
- **Read codes** (OBD mode `03`) → show stored/pending DTCs.
- **Clear codes** (OBD mode `04`) → one tap. **UX caveat to surface:** clearing
  also resets emissions-readiness monitors, which can mask a real fault or fail
  an inspection if done right before a smog test. Warn, don't block.
- **Auto-detect the car** (OBD mode `09`, PID `02`) → read the **VIN**, decode
  the WMI prefix to make/model/year *offline* — labels the session and targets
  the help links with no user input.
- **Offline diagnosis pack** — a JSON file on the SD card mapping generic P-codes
  → plain-English cause + common fixes; optional per-make packs for `P1xxx`.
- **QR → YouTube (offline-first).** The head unit needs no internet: it renders a
  QR pointing at a YouTube search for `<code> <make> <model>`; the *phone* scans
  it and does the browsing. Live gauges drawn with DATAV1 screens, data from the
  dongle — a complete flow that works with zero connectivity.
- **Open RE before building:** is the `RecCmd_*` SPP client general-purpose or
  vendor-bound (see [re-notes.md](re-notes.md))? That gates the transport.

### 2. WiFi network API

For apps that genuinely want the internet (weather, now-playing art, a web
dashboard), the path is **WiFi**, not Bluetooth — the unit has a WiFi+BT combo
chip but **no BT-PAN/tether** stack. The host exposes sockets/HTTP to scripts
once the unit has joined the phone's hotspot or a home AP. Lower priority than
OBD (needs the user to manage a WiFi join), but it's the clean answer to "can a
script make a network request?" — yes, over WiFi.

## Dependency chain / milestones

Everything rides on the keystone — **produce a loadable Melis module and get it
loaded.** Ordered:

1. **RE the module format + loader ABI. ✅ Done (2026-11-24).** Allwinner's
   public D1s Melis SDK documents it, and every claim was verified against the
   stock image: modules are fixed-address RISC-V ELF sections with an `ePDK.mod`
   descriptor (`MInit..MIoctrl`); no relocations, no signature; kernel calls are
   `ecall` with the SDK's numbering. Full spec in
   [format-minfs.md](format-minfs.md#what-a-module-is-fixed-address-code--a-descriptor);
   `headunit decompress --elf` unpacks all 63 stock modules exactly.
2. **Hello-world module that loads**: the real proof. **Built and emulator-tested;
   awaiting the bench boot**: see [hostmod/](../hostmod/README.md). Build with
   `riscv64-elf-gcc`, link at `0xe0000000 + type × 1 MiB` (test type `0x75`, unused
   by every stock module), and load it with **one line in `/startup.sh`** (see
   injection below). It prints over the UART. Converges with the
   [HAL-explorer probe](boot-and-recovery.md#hardware-probing--hal-access-talk-to-the-board-directly):
   the first module you load is naturally a diagnostic REPL.
3. **Host + embedded Lua** — the VM, the sandbox, the API bindings.
4. **Design & document the script API** — the HAL surface above, versioned.
5. **Publish** — guide, examples, gallery.

## Open research questions

- **Module binary format: answered** (milestone 1 above). The earlier "raw PIC
  image + trailer tables" reading was a decoder artifact; see the correction note
  in [format-minfs.md](format-minfs.md#modules-are-stored-as-elf-sections).
  Toolchain target: **Melis 4.x-release-1.2**, framework `livedesk/beetles`.
- **Module injection: answered, via `/startup.sh`.** The data partition ships a
  boot script that the kernel shell runs (`msh_exec_script`, per the SDK):

  ```
  #insmod d:\mod\display.mod
  #insmod d:\mod\orange.mod
  insmod d:\apps\desktop.mod
  ```

  `insmod` = `esMODS_MInstall` + `esMODS_MOpen`, so the load list is **data-driven**.
  Adding one `insmod` line is a plain text edit the repacker already supports.
  The host then needs only `MInit`/`MOpen`, with no app-facing slot interface to
  emulate. Two places the module file can live:
  - **In MINFS** (e.g. `d:\mod\luahost.mod`): needs a new "add file" feature in the
    repacker (dentry-area growth); reflash to update the host.
  - **On removable media** (e.g. `e:\luahost.mod`): the SDK loader falls back to
    parsing a **plain ELF** from any path when the file isn't MINFS-packed. It reads
    **ELF32-layout** headers, so we'd emit that layout around RV64 code. Updating
    the host is then a file copy. Open: whether the vendor's compiled loader
    matches the SDK here, and whether the SD/USB is mounted when `startup.sh` runs
    (bench test).
- **Fallback: the absent `d:\mod\` slots.** Of the 13 slots `init.axf` tries, 8
  ship and 5 are absent (`oak`, `pace`, `miracast`, `carlife`, `eclink`). (An earlier
  parser bug hid half of `/mod/`, so older notes said 10 were absent.) Using a slot
  means satisfying whatever the app calls on it, so `startup.sh` is preferred.
- **Audio/mic ownership** — can the host share the codec/mic with the call and
  CarPlay subsystems, or only when they're idle?

## See also

- [hardware.md](hardware.md) — SoC, the `esMODS` evidence, device map, dormant
  multi-SKU features.
- [boot-and-recovery.md](boot-and-recovery.md) — open boot chain, FEL, the
  HAL-probing tiers the first module converges with.
- [format-minfs.md](format-minfs.md) — module compression framing + the
  decompressor TODO that gates milestone 1.
- [format-datav1.md](format-datav1.md) — the screen format a Lua app would draw
  into or compose beside.
