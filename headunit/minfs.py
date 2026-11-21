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
    +0x0e  u16   attr             (bit0 set => directory)
    +0x10  u16   name_len
    +0x12  u16   (reserved)
    +0x14  char  name[name_len]

The superblock starts with "MINFS"; the root directory record lives just
after it. Rather than chase the exact superblock layout across versions, we
locate the root directory empirically (the first valid entry block) and walk
from there — robust across the images seen so far.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"MINFS"


def checksum(blob: bytes) -> int:
    """32-bit sum of the partition as little-endian u32 words (zero-padded)."""
    if len(blob) % 4:
        blob = blob + b"\0" * (-len(blob) % 4)
    total = 0
    for (w,) in struct.iter_unpack("<I", blob):
        total += w
    return total & 0xFFFFFFFF


@dataclass
class Node:
    name: str
    offset: int
    stored_len: int
    length: int
    is_dir: bool
    entry_off: int
    children: list["Node"] = field(default_factory=list)


class MinFS:
    def __init__(self, data: bytes):
        if data[:5] != MAGIC:
            raise ValueError("not a MINFS partition")
        self.data = bytes(data)
        self.root = Node("", 0, 0, 0, True, -1)
        root_off, root_len = self._find_root()
        self._walk(root_off, root_len, self.root, set())

    # -- parsing -----------------------------------------------------------
    def _entry(self, p: int):
        off, slen, olen, rlen, attr, nlen, _ = struct.unpack_from(
            "<IIIHHHH", self.data, p
        )
        name = self.data[p + 0x14:p + 0x14 + nlen]
        return off, slen, olen, rlen, attr, nlen, name

    def _find_root(self) -> tuple[int, int]:
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
                name.decode("latin1"), o, slen, olen, bool(attr & 1), p
            )
            parent.children.append(node)
            if node.is_dir and o not in seen and 0x400 <= o < len(self.data):
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
        parts = [x for x in path.split("/") if x]
        node = self.root
        for part in parts:
            node = next(c for c in node.children if c.name == part)
        return node

    def read(self, node: Node | str) -> bytes:
        n = self.find(node) if isinstance(node, str) else node
        return self.data[n.offset:n.offset + n.length]
