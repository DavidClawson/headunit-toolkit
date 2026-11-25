# MINFS filesystem + partition checksum

The `data_udisk.fex` and `melis_pkg_nor.fex` partitions are **MINFS**, the
read-only filesystem Allwinner's Melis RTOS uses. It is a simple directory
tree. All fields little-endian.

## Superblock

Starts with ASCII `MINFS`. The 512-byte header (`__minfs_hdr_t` in the SDK):

| off | type | field | stock `data_udisk.fex` |
|----:|------|-------|------|
| +0x00 | char[6] | magic `MINFS\0` | |
| +0x06 | u16 | version | `0x100` |
| +0x08 | u32 | root dir offset | 512 |
| +0x0c | u32 | root dir size | 164 |
| +0x10 | u32 | total dentry count | 174 |
| +0x14 | u32 | total dentry bytes | 16360 |
| +0x18 | u32 | file data area size | 12490476 |
| +0x1c | u32 | image size | 14614528 |

The reader still locates the root directory by scanning (it predates knowing
this header). Adding a *new* file means growing the dentry area and updating
these counts, which is why the repacker can only replace files so far.

## Directory entry

| off | type | meaning |
|----:|------|---------|
| +0x00 | u32 | data offset (absolute, within the partition) |
| +0x04 | u32 | stored length (compressed size; equals original if not compressed) |
| +0x08 | u32 | original length |
| +0x0c | u16 | record length (whole entry incl. name) |
| +0x0e | u16 | attributes: bit0 directory, bit1 **module**, bit2 compressed |
| +0x10 | u16 | name length |
| +0x12 | u16 | **extent length** (bytes of section headers after the name; modules only) |
| +0x14 | char[name_len] | name (ASCII), padded to 4 bytes, then the extent |

Directory records are laid out back-to-back inside the parent's data region;
a zero record length ends the list. (Field names follow Allwinner's
`__minfs_dentry_t` in the public D1s Melis SDK, `ekernel/filesystem/fs/minfs/minfs.h`.)
In the stock image, plain files are `attr=0` and never compressed; directories
are `attr=1`; and every executable (`init.axf`, `*.mod`, `*.drv`, `*.plg` — 41
files) is `attr=6` (module + compressed). Nothing else is compressed.

### Modules are stored as ELF sections

A module isn't stored as one blob. The build links an ordinary RISC-V ELF and
the packer keeps only its sections. The dentry's extent is an array of 32-byte
section headers (`__minfs_section_hdr_t`):

| Offset | Type | Field |
|---|---|---|
| +0x00 | u32 | offset of this section's record, relative to the file's data offset |
| +0x04 | u32 | record size as stored (compressed size if LZMA) |
| +0x08 | u32 | record size once unpacked |
| +0x0c | u32 | section size in memory |
| +0x10 | u32 | load address (vaddr) |
| +0x14 | u32 | ELF `sh_type` (1 PROGBITS, 8 NOBITS/bss) |
| +0x18 | u32 | ELF `sh_flags` (1 WRITE, 2 ALLOC, 4 EXEC) |
| +0x1c | u32 | attr: bit0 **magic section**, bit1 **LZMA-compressed** |

Each compressed section is an **independent** LZMA record: 5 bytes of LZMA
properties (`5d 00 80 00 00` in practice → lc=3 lp=0 pb=2, dict 32 KiB), then raw
LZMA1 data with no size field and no end marker. The unpacked size comes from the
header. bss sections store nothing. A typical module:

```
init.axf   0xe9100000  .text    1358556 B  lzma
           0xe924badc  .rodata   479468 B  lzma
           0xe92c0bc8  .data     242904 B  lzma
           0xe92fc0a0  .bss      104776 B
           0xffff0000  magic         80 B  raw   ("ePDK.mod" descriptor)
```

**All 170 sections across all 41 modules decode to exactly their declared size.**
`headunit decompress <img> <path> --out <file>` writes the flat memory image
(lowest vaddr to the end of the last PROGBITS section; the sections are
contiguous in every stock module). `--elf` writes an ELF with each section at
its real address, so Ghidra/objdump need no manual base. In Python:
`headunit.decompress_node(mfs, path)`, then `headunit.module_to_elf(decoded)`.

> **Correction (2026-11-24).** The earlier decoder split the stored bytes on the
> `5d 00 80 00 00` magic and decoded each piece with an open-ended output length.
> That read a few bytes past each section's end. For `init.axf` its output was
> shifted **+4 bytes** from the start of `.rodata` on (so `ViewShowStrRdsRt` is at
> image offset **1450824**, vaddr `0xe9262348`, not 1450828), and its `.data`
> section was garbled by a false magic match. Its "exact" length match was a
> coincidence of the dentry's `original_len` (which counts the magic record plus
> header padding). The "trailer / symbol-table tails" and the `charset.mod`
> "header variant" described in earlier notes were just the next section's
> bytes. Use addresses, not old offsets, when citing locations.

### What a module is: fixed-address code + a descriptor

- **Not PIC.** A system module is linked at, and loaded at,
  `0xe0000000 + type × 1 MiB` (`EPOS_smod_startaddr + id × EPOS_smod_segsize`).
  Stock examples: `carplay.mod` type `0x88` → `0xe8800000`, `auto.mod` `0x89`,
  `init.axf` `0x91`, `cedar.mod` `0x94`, the Cedar codecs `0x4c..0x5f`/`0x96`.
  Two modules may share a type only if they are never installed at the same time
  (e.g. the `adec_*.drv` codecs are all `0x52`).
- **The magic section** (vaddr `0xffff0000`, 80 bytes) is the
  `__module_mgsec_t` descriptor: `char magic[8] = "ePDK.mod"`, `u32 version
  (0x01000000)`, `u8 type`, `u32 heapaddr (0xF0000)`, `u32 heapsize (0x400)`, then
  the `__mif_t` table of seven 64-bit function pointers `MInit, MExit, MOpen,
  MClose, MRead, MWrite, MIoctrl`. Every stock module uses these exact template
  values.
- **Loading** (`esMODS_MInstall` → `LDR_*` → the MINFS `ROMLDR_IOC_*` ioctls):
  find the magic section, take the type, map each ALLOC PROGBITS/NOBITS section at
  its vaddr, copy/decompress its data, zero bss, then call `MInit`. **There's no
  relocation table, no symbol table, and no signature.**
- **Kernel calls** are `ecall` with `a7 = 0x10000000 | (group << 8) | slot`, using
  the SDK's `SWIHandler_*_t` tables in `kapi.h` (the `CONFIG_KERNEL_USE_SBI`
  branch). Mapping every `ecall` in the stock `init.axf`/`auto.mod`/`carplay.mod`/
  `cedar.mod` through those tables gives coherent names (`esFSYS_fopen/fread/
  fseek/mkdir/remove`, `esCFG_GetKeyValue`, `esKRNL_TCreate`, `esMODS_*`,
  `esMSTUB_*`), so the public SDK's numbering matches this firmware.

> The `MAGIC`/`VERSION`/`MOD_` tokens first spotted in `auto.mod` are **OpenSSL**
> strings (`RC2_MAGIC_TO_METH`, `TLS_MAX_VERSION`, `RSA_NULL_MOD_EXP`);
> `auto.mod` statically links OpenSSL for Android Auto's TLS. A build-path leak
> inside it names the toolchain: `melis-4.x-release-1.2 … livedesk/beetles/CarApp`.

## Partition checksum (the only integrity check)

Each MINFS partition has a partner `V*` partition (`Vdata_udisk.fex`) holding a
single **32-bit additive checksum**: interpret the whole partition as
little-endian `u32` words (zero-padded to a multiple of 4) and sum them mod
2^32.

```python
from headunit import checksum
assert struct.unpack_from("<I", img.data, v_entry.offset)[0] == checksum(part)
```

There is **no signature and no encryption**. Any edit is valid as long as you
recompute this checksum. The logo builder does this automatically.

## Repacking (writing files back)

The length-preserving editors (`logo`, `edit-widget`) only cover edits that
keep a file's byte size. To change a file's **size** — a longer menu string, a
bigger wallpaper — use `MinFS.repack`, which exploits the format's one helpful
property: a file's location is just three fields in *its own* directory entry
(`offset`, `stored_len`, `original_len`), so moving or resizing one file never
cascades to the others.

`repack(edits)` takes `{path: new_bytes}` and returns a **new partition blob of
the exact same total size**, so it still drops straight into the IMAGEWTY slot:

- If the new content still fits the file's original slot, it's written **in
  place** (shorter content leaves harmless unreferenced tail bytes).
- If it **grows**, the new bytes are placed in the partition's *free tail
  arena* — the trailing run of zero bytes after the last file (≈2.1 MB on the
  stock `data_udisk.fex`) — and only that file's entry is repointed.
- Only the touched entries' `(offset, stored_len, original_len)` fields and the
  written slot(s) change; the superblock, directory structure, and every other
  file are byte-identical. `write_partition()` then fixes the V-checksum.

Constraints: targets must be **uncompressed** (`stored_len == original_len`) —
growing an LZMA file would need recompression — and total growth must fit the
free arena. Modules are refused outright (they're stored as sections). Two
spellings of one file (legacy `/Setup.txt` and the true
`/apps/Language/Setup.txt`) are deduplicated and updated once.

```python
from headunit import MinFS, write_partition
mfs = MinFS(img.read("data_udisk.fex"))
new_blob = mfs.repack({"/Setup.txt": patched_bytes})
write_partition(img, new_blob)      # writes partition + fixes checksum
```

CLI: `headunit replace-file <img> <path> <localfile> --out <dir>` (generic) and
`headunit fix-strings <img> --out <dir>` (the curated menu typo pass). The
invariant above is checked end-to-end by `scripts/repack_selftest.py`.

## Notable contents of `data_udisk.fex`

- `/Font30_4Bit.vft` — the 30px UI font. **Not TrueType** — a proprietary
  `VIFT` bitmap glyph atlas (header `VIFT` in UTF-16LE), 30px / 4-bit
  anti-aliased, with all ~33 languages' CJK+Latin glyphs baked in (3.63 MB).
  Changing the typeface means regenerating the whole atlas, not swapping a font
  file.
- Asset note: **every screen, image, text table and the font is uncompressed**
  (only `init.axf` + the Melis `.mod/.drv/.plg` modules are LZMA) — so all UI
  resources are directly repack-moddable. There are **no loose UI bitmaps**:
  button/slider art is embedded inside the compiled `*.data` screens. The only
  standalone rasters are the 4 logos, 4 wallpapers (`1024×600`, panel-native)
  and 3 CarPlay/AA store icons.
- `/init.axf` — the whole application (stripped RISC-V, LZMA-compressed).
- `/Language/*.txt` — UTF-16LE string tables (tab-separated, ~30 languages).
- `/Logo/logo1..4.jpg` + a second `stalogo1.jpg` — the boot/splash images.
- `/WallPaper/0..3.jpg` — home-screen wallpapers.
- `/*.data` — compiled UI screens (see [format-datav1.md](format-datav1.md)).
- `carplay.mod`, `auto.mod`, `wireless.mod`, codec `*.drv`/`*.plg` — the
  Melis loadable modules (proprietary; see [hardware.md](hardware.md)).
