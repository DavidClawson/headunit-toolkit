"""
IMAGEWTY firmware image reader/writer (Allwinner "awimage" container).

This is the outer container used by Allwinner's PhoenixSuit/LiveSuit update
images. On these F133 car stereos the file is distributed as ``LTTF133.img``.
The variant seen here is the v0x0403 header with **plaintext** (unencrypted)
file headers, so everything is directly readable.

Layout (little-endian)::

    0x00  char   magic[8]      "IMAGEWTY"
    0x08  u32    header_version  (0x0403 on this unit)
    0x0c  u32    header_size     (0x60)
    0x20  u32    file_header_size(0x400 — size of each file-table entry)
    0x3c  u32    num_files       (verified: matches the real table)
    0x400 ...    file table: num_files entries, 0x400 bytes each

Each file-table entry::

    +0x00  u32    filename_len   (0x100)
    +0x04  u32    entry_size     (0x400)
    +0x08  char   main_type[8]   e.g. "COMMON  ", "RFSFAT16"
    +0x10  char   sub_type[16]   e.g. "DATA_UDISK_FEX00"
    +0x24  char   filename[256]  e.g. "data_udisk.fex"
    +0x124 u32    stored_len     (content length padded up to 0x200)
    +0x128 u32    (stored_len high / reserved)
    +0x12c u32    original_len   (true content length — use this)
    +0x130 u32    (original_len high / reserved)
    +0x134 u32    data_offset    (absolute offset into the image)

Only the fields we rely on are interpreted; the rest of each entry is
preserved verbatim on repack so we never disturb unknown metadata.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"IMAGEWTY"
ENTRY_SIZE = 0x400
TABLE_START = 0x400


@dataclass
class FileEntry:
    index: int
    main_type: str
    sub_type: str
    name: str
    offset: int        # absolute byte offset of the content in the image
    length: int        # true (original) content length
    stored_len: int    # padded on-disk length
    _entry_off: int    # offset of this entry's header record in the image


class ImageWTY:
    """Parsed view over an IMAGEWTY image held in memory."""

    def __init__(self, data: bytes):
        if data[:8] != MAGIC:
            raise ValueError("not an IMAGEWTY image (bad magic)")
        self.data = bytearray(data)
        self.header_version = struct.unpack_from("<I", data, 0x08)[0]
        self.num_files = struct.unpack_from("<I", data, 0x3C)[0]
        self.files: list[FileEntry] = []
        for i in range(self.num_files):
            h = TABLE_START + i * ENTRY_SIZE
            main = data[h + 0x08:h + 0x10].rstrip(b"\0 ").decode("latin1")
            sub = data[h + 0x10:h + 0x20].rstrip(b"\0 ").decode("latin1")
            name = data[h + 0x24:h + 0x124].split(b"\0")[0].decode("latin1")
            stored, _, orig, _, off = struct.unpack_from("<5I", data, h + 0x124)
            self.files.append(
                FileEntry(i, main, sub, name, off, orig, stored, h)
            )

    @classmethod
    def from_path(cls, path: str) -> "ImageWTY":
        with open(path, "rb") as fh:
            return cls(fh.read())

    def get(self, name: str) -> FileEntry:
        for f in self.files:
            if f.name == name:
                return f
        raise KeyError(name)

    def read(self, entry: FileEntry | str) -> bytes:
        """Return the raw content bytes of a sub-file."""
        f = self.get(entry) if isinstance(entry, str) else entry
        return bytes(self.data[f.offset:f.offset + f.length])

    def replace(self, entry: FileEntry | str, new: bytes) -> None:
        """Overwrite a sub-file *in place*.

        ``new`` must fit within the entry's padded ``stored_len`` so that no
        other file has to move. The length fields are updated; trailing slack
        inside the slot is zero-filled.
        """
        f = self.get(entry) if isinstance(entry, str) else entry
        if len(new) > f.stored_len:
            raise ValueError(
                f"{f.name}: new content {len(new)} > slot {f.stored_len}"
            )
        self.data[f.offset:f.offset + f.stored_len] = (
            new + b"\0" * (f.stored_len - len(new))
        )
        struct.pack_into("<I", self.data, f._entry_off + 0x124, len(new))
        struct.pack_into("<I", self.data, f._entry_off + 0x12C, len(new))
        f.length = len(new)

    def to_bytes(self) -> bytes:
        return bytes(self.data)
