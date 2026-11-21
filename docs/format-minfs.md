# MINFS filesystem + partition checksum

The `data_udisk.fex` and `melis_pkg_nor.fex` partitions are **MINFS**, the
read-only filesystem Allwinner's Melis RTOS uses. It is a simple directory
tree. All fields little-endian.

## Superblock

Starts with ASCII `MINFS`. Rather than depend on the exact superblock layout
(which varies by version), the reader locates the root directory empirically:
it scans the region just after the superblock for the first record with sane
lengths and a printable name, then walks the tree from there. This has been
robust across the images seen.

## Directory entry

| off | type | meaning |
|----:|------|---------|
| +0x00 | u32 | data offset (absolute, within the partition) |
| +0x04 | u32 | stored length (compressed size; equals original if not compressed) |
| +0x08 | u32 | original length |
| +0x0c | u16 | record length (whole entry incl. name) |
| +0x0e | u16 | attributes (bit 0 set → directory) |
| +0x10 | u16 | name length |
| +0x12 | u16 | (reserved) |
| +0x14 | char[name_len] | name (ASCII) |

Directory records are laid out back-to-back inside the parent's data region;
a zero record length ends the list. Some payloads are LZMA-compressed in place
(`stored < original`, e.g. `init.axf`, `carplay.mod`, `cedar/*.drv`); the
toolkit currently reads file bytes raw — decompression helpers are a TODO. Note
that for a compressed file `length` is the *decompressed* size, so a raw
`read()` returns `length` bytes and over-reads past the `stored_len` on disk;
the repacker refuses compressed targets for this reason.

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
free arena. Aliased paths that share one entry (e.g. `/Setup.txt` and
`/Language/Setup.txt`) are deduplicated and updated once.

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

- `/Font30_4Bit.vft` — the 30px UI font.
- `/init.axf` — the whole application (stripped RISC-V, LZMA-compressed).
- `/Language/*.txt` — UTF-16LE string tables (tab-separated, ~30 languages).
- `/Logo/logo1..4.jpg` + a second `stalogo1.jpg` — the boot/splash images.
- `/WallPaper/0..3.jpg` — home-screen wallpapers.
- `/*.data` — compiled UI screens (see [format-datav1.md](format-datav1.md)).
- `carplay.mod`, `auto.mod`, `wireless.mod`, codec `*.drv`/`*.plg` — the
  Melis loadable modules (proprietary; see [hardware.md](hardware.md)).
