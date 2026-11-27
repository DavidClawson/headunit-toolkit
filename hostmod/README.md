# hostmod — custom native modules for the W022 head unit

Build your own Melis kernel modules and have the stock firmware load them at
boot, with no `init.axf` patch and CarPlay/Android Auto left untouched. This is
milestone 2 of the [Lua host](../docs/lua-host.md); `hello/` is the first module.

> **Status: the first hardware test hung the bench unit at the boot splash**
> (2026-11-24). It was recovered by rewriting the SPI flash with a clip
> ([boot-and-recovery.md](../docs/boot-and-recovery.md)). The cause is not yet
> known. **Don't install a module into `startup.sh` again** until we have a boot
> log (debug UART) and a way to skip the module at boot. The emulator passing
> did not predict this.

## Build

```sh
brew install riscv64-elf-gcc
make -C hostmod                 # -> hostmod/build/hello.mod
python3 scripts/emulate_module.py hostmod/build/hello.mod   # offline smoke test
```

## Install into a firmware image

```sh
python3 -m headunit install-module stock/LTTF133.img hostmod/build/hello.mod --out build/hello
# -> build/hello/Update/LTTF133.img   (flash like any other build)
```

`install-module` makes two plain-file edits in the data partition:

1. puts the module into an **empty placeholder file**, `/mod/willow/readme.txt`
   (0 bytes in stock; `--slot` picks another), and
2. adds `insmod d:\mod\willow\readme.txt` to **`/startup.sh`**, the boot script
   the kernel shell runs, after the stock `insmod d:\apps\desktop.mod` line.

Each flash is a complete image, so reflashing `stock/` removes the module.

## Writing a module

```c
#include "melis.h"

static int32_t my_init(void) { melis_puts("hi\r\n"); return EPDK_OK; }
/* ... exit/open/close/read/write/ioctl ... */
MELIS_MODULE(0x75, my_init, my_exit, my_open, my_close, my_read, my_write, my_ioctl);
```

- **Type = address.** A module is linked at `0xe0000000 + type MiB` and must
  claim a type no other module uses. `0x75` is free in the stock image. Add
  `<name>_TYPE := 0x..` to the Makefile for a new module.
- **Lifecycle.** The kernel calls `MInit` while installing; `insmod` then calls
  `MOpen(mid, 0)` and expects a non-NULL handle. `startup.sh` waits on both, so
  keep them short and do real work in a thread (`melis_thread_create`).
- **Kernel calls** go through `ecall` (see `include/melis.h`). Only calls whose
  numbers were confirmed against stock firmware are wrapped there; add more the
  same way.
- **Rules the toolchain enforces:** the descriptor (`MAGIC` section) is exactly
  80 bytes (the kernel copies it into an 80-byte stack struct); code is built
  `-mcmodel=medany --no-relax` (pc-relative, never `gp`-relative, because `gp`
  belongs to the kernel); no libc (freestanding).

## Why this works

All of this comes from Allwinner's public D1s Melis SDK and was verified against
the stock image; see [docs/format-minfs.md](../docs/format-minfs.md).

- The stock **kernel** (decompressed from `melis_pkg_nor.fex`) contains the
  SDK's plain-ELF loader. Disassembly shows it clears a 52-byte ELF32 header and
  40-byte section headers and matches the section name `MAGIC` and the
  `ePDK.mod` tag, the layout `headunit pack-module` writes.
- A MINFS file *without* the module flag makes the loader's MINFS-section ioctl
  fail, so it falls back to that plain-ELF path. That's why an ordinary file
  (the placeholder) can hold a module.
- Syscall numbers in `melis.h` are the ones every stock module uses (e.g. all 56
  stock modules call `esSIOS_putstr` as `0x10000003`).

## Bench test: first boot of `hello.mod`

What it does: prints to the debug UART (PB06/PB07, 576000 baud for Melis)
from `MInit`/`MOpen`, and starts a thread that, for 10 minutes after boot, tries
every 2 s to write `HELLO.TXT` to the root of drives `F:`–`K:` (`F:` is the USB
stick the factory menu exports to). Internal drives `d:`/`e:` are never written.

1. Build the image (above) and flash it with the usual `Update/` stick.
2. After it reboots, **remove the stick**, and on the Mac rename its `Update`
   folder (e.g. to `Update.done`) so reinserting it doesn't trigger another
   upgrade. A second blank FAT32 stick works too.
3. Within 10 minutes of boot, insert the stick, wait ~10 s, remove it.
4. On the Mac: **`HELLO.TXT` in the stick's root = our code ran on the unit.**
   Check that CarPlay/Android Auto still work, as with every flash.

Reading the result:

| Outcome | Meaning |
|---|---|
| `HELLO.TXT` present | Module loaded, `MInit`/`MOpen` ran, its thread ran with kernel file I/O. Milestone 2 done. |
| Boots normally, no file | Load failed gracefully, or the stick isn't `F:`–`K:`. A UART log says which (`install fail` vs our banner). |
| Doesn't boot / hangs | **What actually happened.** A stick can't fix it: the USB updater is in the app, which never starts. Recover with the SPI flash clip ([boot-and-recovery.md](../docs/boot-and-recovery.md)). |

**Risk: high without a flash backup.** This test hung the bench unit at the
splash, and neither reflashing from a USB stick nor any other software path
brought it back. Only rewriting the SPI flash chip did. Before any boot-path
test: back up the chip with the clip, keep the case open with the clip at hand,
and use the **bench unit only**, never the car.
