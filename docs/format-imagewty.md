# IMAGEWTY container format

`LTTF133.img` is an Allwinner **IMAGEWTY** image (the "awimage" format used by
PhoenixSuit / LiveSuit). The variant on this unit uses header version
`0x0403` with **plaintext** file headers (no encryption), so it parses
directly. All fields little-endian.

Verified against a real 26-partition image (17,901,568 bytes).

## Main header (offset 0)

| off | type | value seen | meaning |
|----:|------|-----------|---------|
| 0x00 | char[8] | `IMAGEWTY` | magic |
| 0x08 | u32 | `0x0403` | header version |
| 0x0c | u32 | `0x60` | header size |
| 0x20 | u32 | `0x400` | file-header entry size |
| 0x38 | u32 | `0x400` | (entry size / alignment; mirrors 0x20) |
| 0x3c | u32 | `0x1a` (26) | **number of files** — this is the count that matches reality |
| 0x400 | — | — | start of the file table |

> Note: some IMAGEWTY variants put the file count at 0x38. On this image 0x38
> holds `0x400` and the real count is at **0x3c**. The reader validates by
> parsing all entries.

## File-table entry (0x400 bytes each, starting at 0x400)

| off | type | meaning |
|----:|------|---------|
| +0x00 | u32 | filename length (`0x100`) |
| +0x04 | u32 | entry size (`0x400`) |
| +0x08 | char[8] | main type, e.g. `COMMON  `, `RFSFAT16`, `12345678` |
| +0x10 | char[16] | sub type, e.g. `DATA_UDISK_FEX00` |
| +0x24 | char[256] | filename, e.g. `data_udisk.fex` |
| +0x124 | u32 | stored length (content padded up to 0x200) |
| +0x128 | u32 | (high word / reserved, 0) |
| +0x12c | u32 | **original length** — the true content size |
| +0x130 | u32 | (high word / reserved, 0) |
| +0x134 | u32 | **data offset** (absolute into the image) |

The toolkit interprets only these fields and preserves the rest of each entry
verbatim, so repacking never disturbs unknown metadata.

## The 26 partitions (this image)

Boot chain and tools:
`sys_config_nor.fex`, `config_nor.fex`, `sunxi.fex` (DTB), `boot0_nor.fex`,
`boot0_card.fex`, `boot_pkg_uboot_nor.fex`, `u-boot_nor.fex`, `fes1.fex`,
`usbtool.fex`, `cardtool.fex`, `cardscript.fex`, `sunxi_gpt.fex`,
`sunxi_mbr_nor.fex`, `dlinfo.fex`, `arisc.fex`.

Payload:
- `melis_pkg_nor.fex` (+ `Vmelis_pkg_nor.fex`) — the Melis OS package.
- `data_udisk.fex` (+ `Vdata_udisk.fex`) — the **MINFS app/data partition**
  (UI, fonts, logos, modules). This is where most mods live.

The `V*` partner partitions hold a 32-bit additive checksum of their payload —
see [format-minfs.md](format-minfs.md).

## In-place edit rule

`ImageWTY.replace(name, new_bytes)` overwrites a sub-file only if it fits the
entry's padded `stored_len`, keeping every offset fixed. Larger replacements
would require rebuilding the whole table + offsets (not yet implemented, and
rarely needed — most edits are inside MINFS, which has its own slack).
