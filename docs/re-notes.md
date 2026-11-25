# Reverse-engineering working notes

Durable notes for the deeper static RE (loader ABI, module format). Nothing here
is firmware — just offsets, addresses, and the recipe to reproduce the analysis
from your own `stock/LTTF133.img`.

## Unpacking modules

`init.axf` and every `*.mod/.drv/.plg` are stored in MINFS as separately
LZMA-compressed ELF sections at fixed addresses ([format-minfs.md](format-minfs.md)).
All 63 modules (331 sections) unpack exactly:

```sh
headunit decompress stock/LTTF133.img /apps/init.axf --elf --out init.elf
headunit decompress stock/LTTF133.img /mod/auto.mod  --elf --out auto.elf
headunit decompress stock/LTTF133.img /apps/init.axf --out init.bin   # flat image @ 0xe9100000
```

The `--elf` output carries every section at its true vaddr, so disassemblers
need no base address. `init.axf` is `.text 0xe9100000`, `.rodata 0xe924badc`,
`.data 0xe92c0bc8`, `.bss 0xe92fc0a0`.

> **Retired:** the earlier "link base `0xe90e2e04`" (recovered by pointer voting
> against the old, slightly mis-decoded flat output) and the "position-independent"
> framing. Modules are linked at `0xe0000000 + type × 1 MiB`.

## Ghidra setup (headless)

Installed via Homebrew formula `ghidra` (12.1.2);
`analyzeHeadless` at `/opt/homebrew/Cellar/ghidra/<ver>/libexec/support/`.
Python scripts need PyGhidra on 12.x — **use Java `GhidraScript`s** in headless.

```sh
analyzeHeadless <proj_dir> initaxf \
  -import init.elf \
  -processor RISCV:LE:64:default \
  -scriptPath <scripts> -postScript YourScript.java
```

(Import the `--elf` output with Ghidra's ELF loader, which places every section at
its real address, so absolute pointers resolve without manual setup.)

Use absolute paths (Ghidra rejects `./`). Drop `-deleteProject` to keep the
project for fast `-process` re-runs without re-analyzing.

## Module ABI — what's known vs open

**Known (2026-11-24, from the public D1s Melis SDK, verified on the stock image):**
- Container, descriptor, load procedure, address scheme, and syscall numbering
  are all documented in [format-minfs.md](format-minfs.md#what-a-module-is-fixed-address-code--a-descriptor).
- Of the 13 `d:\mod\*.mod` slots the app tries, 8 ship and **5 are absent**
  (`oak`, `pace`, `miracast`, `carlife`, `eclink`), giving a resource-level
  injection point ([lua-host.md](lua-host.md)).

**Open:**
- What `init.axf` does with each slot after `esMODS_MInstall` (which
  `MOpen`/`MIoctrl` calls it makes), to pick the quietest absent slot.
- MINFS "add file" support in the repacker (dentry area growth).

The first is interactive-RE shaped (follow the slot-install xrefs in Ghidra), and
is cheaply confirmed by loading a test module on the **bench unit**.

## Radios & Bluetooth transport (the OBD-II / network question)

Grepping the decompressed `init.axf` + the whole image for the radio stack:

- **Combo WiFi+BT chip.** The `[wifi_para]` config block has `wifi_used = 1`
  and `wifi_mod_sel = 3`, which the in-file legend maps to **`rtl8723as`
  (wifi+bt)** on SDIO (`wifi_sdc_id = 1`); the kernel cmdline carries both
  `wifi_mac` and `bt_mac`. So the unit has **both WiFi and Bluetooth Classic**
  at the silicon level (one combo module). This is the radio behind wireless
  CarPlay/AA.
- **RFCOMM/SPP machinery is present in `init.axf`.** There's a Bluetooth serial
  command protocol: `RecCmd_Scan`, `RecCmd_RemoteAddr %s`, `RecCmd_UUID %s`,
  `RecCmd_RfcommData error %d,%d`, plus `open_file_bt`, `RecCmd_ProgramInfo` /
  `RecCmd_AllProgramInfo` / `RecCmd_Picture …checksum`. Shape = a **vendor
  companion-app channel** (scan → connect to a remote address on a UUID →
  exchange raw RFCOMM bytes; also pushes radio "program" info + album-art
  pictures to a phone app). RFCOMM/SPP is exactly the transport a cheap
  **Bluetooth ELM327 OBD-II dongle** speaks.
- **No BNEP / PAN.** Zero `bnep`/`pan`/`tether` strings anywhere → **no
  Bluetooth tethering stack**. Routing internet *through the phone over BT* is
  not a firmware path (and iPhones dropped BT-PAN regardless). The sane network
  path for a host/script is **WiFi** (join the phone's hotspot or home AP), not
  Bluetooth.
- **No OBD/CAN built in.** No `obd`/`elm327`/`can`/`dtc` strings — expected; this
  is a pure aftermarket head unit with no CAN tap. A BT ELM327 dongle is
  therefore *the* way to get live vehicle data, and all of it goes over the SPP
  transport above.
- **Module surface is larger than the 13-slot list.** `insmod d:\mod\display.mod`
  and `insmod d:\mod\orange.mod` show up in the image beyond the catalogued
  `d:\mod\*.mod` slots — the loadable-module set is bigger than we'd mapped.

**Open RE (gates an OBD app):** is that `RecCmd_*` SPP client **general-purpose**
(point it at an arbitrary device address + UUID and stream bytes — what an
ELM327 app needs) or **hardwired** to the vendor companion-app protocol? If
reusable, a host module wraps it; if not, drive the RTL8723AS BT stack a layer
lower. Also open: does this stack hold a **second** simultaneous BT device (the
OBD dongle) alongside the phone, and how does WiFi/BT coexistence behave — both
bench tests. See the two candidate apps in [lua-host.md](lua-host.md).
