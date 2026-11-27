"""
MINFS reader — the read-only filesystem Allwinner's Melis RTOS uses for the
application/data partition (``data_udisk.fex``, ``melis_pkg_nor.fex``).

It is a simple directory tree of (offset, length, name) records. Each MINFS
partition in the firmware is paired with a tiny "V" partition
(``Vdata_udisk.fex``) that stores a single 32-bit additive checksum of the
whole MINFS blob — the only integrity check in the image.

Directory entry (little-endian)::

    +0x00  u32   data_offset      (absolute offset within the partition)
    +0x04  u32   stored_len       (compressed size; == orig if not compressed)
    +0x08  u32   original_len
    +0x0c  u16   record_len       (total bytes of this entry incl. name)
    +0x0e  u16   attr             (bit0 directory, bit1 module, bit2 compressed)
    +0x10  u16   name_len
    +0x12  u16   extent_len       (bytes of section headers after the name)
    +0x14  char  name[name_len], padded to 4 bytes, then the extent

A **module** (``init.axf``, ``*.mod``, ``*.drv``, ``*.plg``; attr bit1) is not
stored as one blob: it is an ELF split into sections. Its extent is an array of
32-byte section headers (see ``Section``); each section's bytes live at
``data_offset + Section.offset`` and are LZMA-compressed *independently* when
``Section.compressed``. ``decompress.py`` turns them back into memory images.
Layout from Allwinner's Melis ``minfs.h`` (``__minfs_section_hdr_t``), verified
against every module in the stock image.

The superblock starts with "MINFS"; the root directory record lives just
after it. Rather than chase the exact superblock layout across versions, we
locate the root directory empirically (the first valid entry block) and walk
from there — robust across the images seen so far.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"MINFS"

ATTR_DIR = 1
ATTR_MODULE = 2
ATTR_COMPRESS = 4

SECTION_HDR = struct.Struct("<8I")
SEC_ATTR_MAGIC = 1      # the ``ePDK.mod`` descriptor section
SEC_ATTR_COMPRESS = 2   # section bytes are LZMA (5-byte props + raw LZMA1)
SHT_PROGBITS = 1
SHT_NOBITS = 8


def checksum(blob: bytes) -> int:
    """32-bit sum of the partition as little-endian u32 words (zero-padded)."""
    if len(blob) % 4:
        blob = blob + b"\0" * (-len(blob) % 4)
    total = 0
    for (w,) in struct.iter_unpack("<I", blob):
        total += w
    return total & 0xFFFFFFFF


@dataclass
class Section:
    """One ELF section of a MINFS module (``__minfs_section_hdr_t``)."""
    offset: int         # byte offset of the record, relative to the file's data
    rec_size: int       # bytes stored in the image (compressed size if LZMA)
    rec_unpack: int     # bytes once decompressed (0 for bss)
    size: int           # section size in memory
    vaddr: int          # load address (system modules are fixed-address)
    type: int           # ELF sh_type: 1 PROGBITS, 8 NOBITS (bss)
    flags: int          # ELF sh_flags: 1 WRITE, 2 ALLOC, 4 EXECINSTR
    attr: int           # bit0 magic section, bit1 LZMA-compressed

    @property
    def is_magic(self) -> bool:
        return bool(self.attr & SEC_ATTR_MAGIC)

    @property
    def compressed(self) -> bool:
        return bool(self.attr & SEC_ATTR_COMPRESS)

    @property
    def is_bss(self) -> bool:
        return self.type == SHT_NOBITS


@dataclass
class Node:
    name: str
    offset: int
    stored_len: int
    length: int
    is_dir: bool
    entry_off: int
    attr: int = 0
    sections: list[Section] = field(default_factory=list)
    children: list["Node"] = field(default_factory=list)

    @property
    def is_module(self) -> bool:
        return bool(self.attr & ATTR_MODULE)


class MinFS:
    def __init__(self, data: bytes):
        if data[:5] != MAGIC:
            raise ValueError("not a MINFS partition")
        self.data = bytes(data)
        self.root = Node("", 0, 0, 0, True, -1)
        root_off, root_len = self._root_from_header() or self._find_root()
        self._walk(root_off, root_len, self.root, set())

    # -- parsing -----------------------------------------------------------
    def _entry(self, p: int):
        off, slen, olen, rlen, attr, nlen, _ = struct.unpack_from(
            "<IIIHHHH", self.data, p
        )
        name = self.data[p + 0x14:p + 0x14 + nlen]
        return off, slen, olen, rlen, attr, nlen, name

    def _sections(self, p: int) -> list[Section]:
        """Parse the section-header extent of the dentry at ``p``."""
        _, _, _, rlen, attr, nlen, ext = struct.unpack_from(
            "<IIIHHHH", self.data, p
        )
        if not attr & ATTR_MODULE or not ext:
            return []
        start = p + 0x14 + ((nlen + 3) & ~3)
        if ext % SECTION_HDR.size or start + ext > p + rlen:
            raise ValueError(f"bad module extent at dentry {p:#x}")
        return [Section(*SECTION_HDR.unpack_from(self.data, start + i))
                for i in range(0, ext, SECTION_HDR.size)]

    def _root_from_header(self) -> tuple[int, int] | None:
        """Root directory (offset, size) from the 512-byte superblock
        (``__minfs_hdr_t``: magic[6], u16 version, u32 root_off, u32 root_size,
        ...). Returns None if the fields look implausible."""
        if len(self.data) < 0x20:
            return None
        root_off, root_size = struct.unpack_from("<II", self.data, 8)
        if 0x200 <= root_off < len(self.data) and 0 < root_size <= 0x10000:
            return root_off, root_size
        return None

    def _find_root(self) -> tuple[int, int]:
        # Fallback for images whose superblock doesn't parse. NOTE: this can
        # land mid-directory and merge sibling directories (it did on the stock
        # W022 image before the header was decoded); prefer the header.
        # The root directory's first child record is the first place where a
        # plausible entry (sane lengths + printable name) appears. Scan a small
        # window after the superblock.
        for p in range(0x400, 0x600, 2):
            try:
                off, slen, olen, rlen, attr, nlen, name = self._entry(p)
            except struct.error:
                break
            if (
                0 < nlen < 64
                and 0x14 + nlen <= rlen <= 0x400
                and 0x400 <= off < len(self.data)
                and name[:nlen].isascii()
                and all(0x20 <= b < 0x7F for b in name[:nlen])
            ):
                return p, 0x1000  # length is a generous upper bound; walk stops at rlen==0
        raise ValueError("could not locate MINFS root directory")

    def _walk(self, off: int, size: int, parent: Node, seen: set):
        p, end = off, off + size
        while p < end:
            try:
                o, slen, olen, rlen, attr, nlen, name = self._entry(p)
            except struct.error:
                break
            if rlen == 0 or nlen == 0 or nlen > 200:
                break
            node = Node(
                name.decode("latin1"), o, slen, olen, bool(attr & ATTR_DIR), p,
                attr, self._sections(p),
            )
            parent.children.append(node)
            if node.is_dir and o not in seen and 0x200 <= o < len(self.data):
                seen.add(o)
                self._walk(o, slen or 0x1000, node, seen)
            p += rlen

    # -- access ------------------------------------------------------------
    def walk(self):
        """Yield (path, Node) for every node, depth-first."""
        def rec(node, path):
            for c in node.children:
                full = f"{path}/{c.name}"
                yield full, c
                if c.is_dir:
                    yield from rec(c, full)
        yield from rec(self.root, "")

    def find(self, path: str) -> Node:
        """Look up a node by path. True paths look like
        ``/apps/Language/Setup.txt`` or ``/mod/carplay.mod``.

        For compatibility with paths written before the real root was decoded
        (``/Setup.txt``, ``/Logo/logo1.jpg``, ``/carplay.mod``), a path that
        doesn't resolve exactly falls back to the **unique** node whose full
        path ends with it. Ambiguous or missing paths raise ``KeyError``."""
        parts = [x for x in path.split("/") if x]
        node = self.root
        for part in parts:
            node = next((c for c in node.children if c.name == part), None)
            if node is None:
                break
        else:
            return node
        tail = "/" + "/".join(parts)
        hits = [n for full, n in self.walk() if full.endswith(tail)]
        if len(hits) == 1:
            return hits[0]
        if not hits:
            raise KeyError(f"no such MINFS path: {path}")
        raise KeyError(f"ambiguous MINFS path {path!r}: "
                       + ", ".join(f for f, n in self.walk() if n in hits))

    def read(self, node: Node | str) -> bytes:
        n = self.find(node) if isinstance(node, str) else node
        return self.data[n.offset:n.offset + n.length]

    # -- repack ------------------------------------------------------------
    def repack(self, edits: dict, *, align: int = 4) -> bytes:
        """Return a new partition blob — **the same total size as the
        original** — with the named files replaced by new content.

        ``edits`` maps a path (or a ``Node``) to the new ``bytes`` for that
        file. Files whose new content still fits their original slot are
        rewritten in place; files that grow are relocated into the partition's
        free *tail arena* (the trailing run of zero bytes). Only the affected
        directory entries' ``(offset, stored_len, original_len)`` fields are
        changed — the superblock, the directory structure, and every other
        file's bytes are preserved exactly.

        Edits are keyed by physical directory entry, so two spellings of the
        same file (e.g. legacy ``/Setup.txt`` and ``/apps/Language/Setup.txt``)
        collapse to a single update.

        Raises ``ValueError`` if a target is a directory, is LZMA-compressed
        (``stored_len != length`` — repacking those would need recompression),
        or if the free arena cannot hold the growth.
        """
        part = bytearray(self.data)
        total = len(part)

        # Free arena = the trailing run of zero bytes, 4-aligned.
        tail_zero = total - len(part.rstrip(b"\0"))
        cursor = (total - tail_zero + align - 1) & ~(align - 1)

        # Resolve paths -> nodes and deduplicate by physical entry offset.
        resolved: dict[int, tuple[Node, bytes]] = {}
        for key, content in edits.items():
            n = self.find(key) if isinstance(key, str) else key
            if n.is_dir:
                raise ValueError(f"{key!r} is a directory, not a file")
            if n.is_module:
                raise ValueError(
                    f"{key!r} is a Melis module (stored as ELF sections); "
                    f"replacing it as a flat file would corrupt it"
                )
            if n.stored_len != n.length:
                raise ValueError(
                    f"{key!r} is compressed (stored {n.stored_len} != "
                    f"len {n.length}); compressed repack is unsupported"
                )
            resolved[n.entry_off] = (n, bytes(content))

        for entry_off, (n, new) in resolved.items():
            size = len(new)
            if size <= n.stored_len:
                dst = n.offset            # fits the original slot: write in place
            else:
                dst = cursor              # grew: relocate into the free arena
                if dst + size > total:
                    raise ValueError(
                        f"free arena exhausted writing {n.name!r}: need "
                        f"{size} bytes, only {total - dst} left"
                    )
                cursor = (dst + size + align - 1) & ~(align - 1)
            part[dst:dst + size] = new
            # entry layout: +0x00 offset, +0x04 stored_len, +0x08 original_len
            struct.pack_into("<III", part, entry_off, dst, size, size)

        if len(part) != total:  # must never change the partition's footprint
            raise AssertionError("repack changed the partition size")
        return bytes(part)


def write_partition(img, blob: bytes, part_name: str = "data_udisk.fex",
                    v_name: str = "Vdata_udisk.fex") -> None:
    """Write a repacked MINFS ``blob`` into ``img`` (an ``ImageWTY``) in place
    and fix the paired "V" partition's additive checksum. ``blob`` must be the
    partition's exact stored length."""
    part_e = img.get(part_name)
    v_e = img.get(v_name)
    if len(blob) != part_e.length:
        raise ValueError(
            f"blob is {len(blob)} bytes; partition {part_name} holds "
            f"{part_e.length}"
        )
    img.data[part_e.offset:part_e.offset + part_e.length] = blob
    struct.pack_into("<I", img.data, v_e.offset, checksum(blob))
