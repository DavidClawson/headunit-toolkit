"""Unpack Melis modules stored in MINFS (``init.axf``, ``*.mod``, ``*.drv``,
``*.plg``) back into memory images, plus an ELF wrapper for disassemblers.

How MINFS stores a module (from Allwinner's Melis ``minfs.h`` / ``loader.c``,
verified byte-exact against all 41 modules / 170 sections in the stock image):

* The build produces an ordinary RISC-V ELF linked at a **fixed address**
  (``0xe0000000 + type * 1 MiB`` for system modules — they are *not*
  position-independent). The packer keeps only its loadable sections.
* The file's dentry carries one 32-byte header per section
  (``minfs.Section``: offset, stored size, unpacked size, memory size, vaddr,
  ELF type/flags, attr).
* Each section's bytes are LZMA-compressed **independently** when the header
  says so: 5 bytes of LZMA properties followed by raw LZMA1 data with no size
  field and no end marker (the unpacked size comes from the header). bss
  sections store nothing.
* One section is the **magic section** at ``0xffff0000``: the 80-byte
  ``__module_mgsec_t`` descriptor (``"ePDK.mod"``, version, type, heap,
  and the ``MInit..MIoctrl`` entry points). ``esMODS_MInstall`` maps every other
  ALLOC section at its vaddr, zeroes bss, then calls ``MInit``.

``decompress_node`` returns the flat memory image (lowest loadable vaddr up to
the end of the last PROGBITS section; gaps zero-filled). In every stock module
the PROGBITS sections are contiguous, so this equals the plain concatenation of
the sections. ``module_to_elf`` rebuilds an ELF so Ghidra/objdump load each
section at its true address with no manual base.
"""
from __future__ import annotations

import lzma
import struct
from dataclasses import dataclass, field

from .minfs import SHT_NOBITS, SHT_PROGBITS, Section

MODULE_MAGIC = b"ePDK.mod"
MAGIC_VADDR = 0xFFFF0000
MODULE_SPACE = 0xE0000000       # EPOS_smod_startaddr
MODULE_SEGMENT = 1 << 20        # EPOS_smod_segsize
MIF_NAMES = ("MInit", "MExit", "MOpen", "MClose", "MRead", "MWrite", "MIoctrl")


def module_vaddr(mod_type: int) -> int:
    """Link/load address of a system module of the given type ID."""
    return MODULE_SPACE + mod_type * MODULE_SEGMENT


def is_compressed(stored: bytes) -> bool:
    """True iff ``stored`` starts with plausible LZMA properties (lc/lp/pb
    byte < 225). Kept for API compatibility; prefer ``Section.compressed``."""
    return len(stored) >= 5 and stored[0] < 9 * 5 * 5


def decompress(stored: bytes, original_len: int) -> bytes:
    """Decode one MINFS LZMA record: 5-byte properties + raw LZMA1 data.
    ``original_len`` is the unpacked size (from the section header); the
    stream has no end marker, so decoding stops there."""
    props = stored[:5]
    lc, rem = props[0] % 9, props[0] // 9
    dict_size = struct.unpack_from("<I", props, 1)[0]
    filt = {"id": lzma.FILTER_LZMA1, "lc": lc, "lp": rem % 5, "pb": rem // 5,
            "dict_size": max(dict_size, 4096)}
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[filt])
    return dec.decompress(stored[5:], max_length=original_len)


@dataclass
class Descriptor:
    """The module's ``__module_mgsec_t`` (from its magic section)."""
    magic: bytes
    version: int
    type: int
    heapaddr: int
    heapsize: int
    mif: dict[str, int]

    @classmethod
    def parse(cls, raw: bytes) -> "Descriptor":
        magic = raw[:8]
        version, mtype = struct.unpack_from("<IB", raw, 8)
        heapaddr, heapsize = struct.unpack_from("<II", raw, 16)
        mif = dict(zip(MIF_NAMES, struct.unpack_from("<7Q", raw, 24)))
        return cls(magic, version, mtype, heapaddr, heapsize, mif)


@dataclass
class Decoded:
    data: bytes                 # flat memory image starting at ``base``
    original_len: int           # node.length (sum of unpacked section records)
    complete: bool              # every section decoded to its declared size
    blocks: int                 # number of sections
    error: str | None = None
    base: int = 0               # vaddr of data[0]
    sections: list[tuple[Section, bytes]] = field(default_factory=list)
    descriptor: Descriptor | None = None


def _node(mfs, node_or_path):
    return mfs.find(node_or_path) if isinstance(node_or_path, str) else node_or_path


def decompress_node(mfs, node_or_path) -> Decoded:
    """Unpack a MINFS module node (or path) into its sections + memory image.

    Non-module files are returned verbatim (they are never compressed)."""
    n = _node(mfs, node_or_path)
    if not n.sections:
        raw = mfs.data[n.offset:n.offset + n.stored_len]
        return Decoded(raw, n.length, len(raw) == n.length, 0)

    secs: list[tuple[Section, bytes]] = []
    err = None
    for i, s in enumerate(n.sections):
        if s.is_bss or not s.rec_size:
            secs.append((s, b""))
            continue
        raw = mfs.data[n.offset + s.offset:n.offset + s.offset + s.rec_size]
        try:
            body = decompress(raw, s.rec_unpack) if s.compressed else raw
        except lzma.LZMAError as e:
            err = err or f"section {i} @ {s.vaddr:#x}: {e}"
            body = b""
        if len(body) != s.rec_unpack and err is None:
            err = f"section {i} @ {s.vaddr:#x}: {len(body)}/{s.rec_unpack} bytes"
        secs.append((s, body))

    desc = None
    for s, body in secs:
        if s.is_magic and body[:8] == MODULE_MAGIC:
            desc = Descriptor.parse(body)

    load = [(s, b) for s, b in secs if s.type == SHT_PROGBITS and not s.is_magic]
    if load:
        base = min(s.vaddr for s, _ in load)
        end = max(s.vaddr + len(b) for s, b in load)
        image = bytearray(end - base)
        for s, b in load:
            image[s.vaddr - base:s.vaddr - base + len(b)] = b
    else:
        base, image = 0, bytearray()
    return Decoded(bytes(image), n.length, err is None, len(secs), err,
                   base, secs, desc)


# -- ELF reconstruction ----------------------------------------------------

def _section_name(s: Section) -> str:
    if s.is_magic:
        return ".magic"
    if s.type == SHT_NOBITS:
        return ".bss"
    if s.flags & 4:
        return ".text"
    if s.flags & 1:
        return ".data"
    return ".rodata"


def module_to_elf(d: Decoded) -> bytes:
    """Rebuild an ELF64 (RISC-V, little-endian) from a decoded module: one
    PT_LOAD + section header per MINFS section at its true vaddr, entry =
    ``MInit``. Names are inferred from flags (the originals aren't stored)."""
    if not d.sections:
        raise ValueError("not a module (no sections)")
    names, used = [], {}
    for s, _ in d.sections:
        nm = _section_name(s)
        used[nm] = used.get(nm, 0) + 1
        names.append(nm if used[nm] == 1 else f"{nm}.{used[nm] - 1}")
    shstr = bytearray(b"\0")
    name_off = []
    for nm in names + [".shstrtab"]:
        name_off.append(len(shstr))
        shstr += nm.encode() + b"\0"

    ehsize, phentsize, shentsize = 64, 56, 64
    nload = len(d.sections)
    pos = ehsize + phentsize * nload
    file_off = []
    blob = bytearray()
    for s, body in d.sections:
        pos = (pos + 15) & ~15
        file_off.append(pos)
        blob += b"\0" * (pos - ehsize - phentsize * nload - len(blob)) + body
        pos += len(body)
    shstr_off = pos
    blob += shstr
    shoff = (shstr_off + len(shstr) + 7) & ~7
    blob += b"\0" * (shoff - shstr_off - len(shstr))

    entry = d.descriptor.mif["MInit"] if d.descriptor else d.base
    hdr = bytearray(b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8)
    hdr += struct.pack("<HHIQQQIHHHHHH", 2, 243, 1, entry, ehsize, shoff,
                       0x5,  # EF_RISCV_RVC | float-ABI double
                       ehsize, phentsize, nload, shentsize,
                       len(d.sections) + 2, len(d.sections) + 1)
    ph = bytearray()
    for (s, body), off in zip(d.sections, file_off):
        pflags = 4 | (1 if s.flags & 4 else 0) | (2 if s.flags & 1 else 0)
        ph += struct.pack("<IIQQQQQQ", 1, pflags, off, s.vaddr, s.vaddr,
                          len(body), s.size, 4)
    sh = bytearray(b"\0" * shentsize)
    for i, ((s, body), off) in enumerate(zip(d.sections, file_off)):
        sh += struct.pack("<IIQQQQIIQQ", name_off[i], s.type, s.flags, s.vaddr,
                          off, s.size, 0, 0, 4, 0)
    sh += struct.pack("<IIQQQQIIQQ", name_off[-1], 3, 0, 0, shstr_off,
                      len(shstr), 0, 0, 1, 0)
    return bytes(hdr + ph + blob + sh)
