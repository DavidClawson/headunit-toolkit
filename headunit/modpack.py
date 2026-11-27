"""Package and install custom Melis modules (see ``hostmod/``).

``pack_module`` turns the RV64 ELF64 that ``riscv64-elf-gcc`` links into the
file layout the Melis kernel's **plain-ELF loader** reads. That loader
(``LDR_LoadELFFile`` in the SDK; present in the stock kernel, which
``memset``s a 52-byte header and 40-byte section headers and compares names of
length 5 / 8) parses **ELF32-layout** headers regardless of the code's ISA:

* 52-byte ELF header; ``e_shentsize`` 40; ``e_shstrndx`` names the string table;
* one 40-byte section header per loadable section (``addr`` is 32-bit — modules
  live at ``0xe0000000 + type MiB``, so it fits), PROGBITS/NOBITS with ALLOC;
* the descriptor section must be **named ``MAGIC``**, start with ``ePDK.mod``,
  and be **exactly 80 bytes**: ``LDR_LoadELFFile`` peeks 128 bytes into its own
  buffer (a short read is fine), but ``esMODS_MInstall`` then copies the *whole*
  section into an 80-byte stack struct, so anything larger smashes the stack.

The loader maps every ALLOC section at its address, zeroes NOBITS, skips the
MAGIC section, then calls ``MInit``; ``insmod`` then calls ``MOpen``.

``install_module`` places the packed module into the data partition by
replacing an existing *empty placeholder* file (default
``/mod/willow/readme.txt``, 0 bytes) and adds an ``insmod`` line for it to
``/startup.sh``, the boot script the kernel shell runs. Both are plain-file
edits the MINFS repacker already supports. A file not flagged as a MINFS module
makes the loader's ROMLDR ioctl fail, so it falls back to the plain-ELF path.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .minfs import MinFS, write_partition

ELF_MAGIC = b"\x7fELF"
EM_RISCV = 243
SHT_PROGBITS, SHT_NOBITS, SHT_STRTAB = 1, 8, 3
SHF_ALLOC = 2
MODULE_MAGIC = b"ePDK.mod"
MAGIC_SECTION = "MAGIC"
MAGIC_LEN = 80
MODULE_SPACE = 0xE0000000

DEFAULT_SLOT = "/mod/willow/readme.txt"
STARTUP = "/startup.sh"


@dataclass
class ElfSection:
    name: str
    type: int
    flags: int
    addr: int
    data: bytes      # empty for NOBITS
    size: int


def _read_elf64(elf: bytes) -> tuple[int, list[ElfSection]]:
    if elf[:4] != ELF_MAGIC or elf[4] != 2 or elf[5] != 1:
        raise ValueError("expected a little-endian ELF64 file")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize,
     e_shnum, e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", elf, 16)
    if e_machine != EM_RISCV:
        raise ValueError(f"not a RISC-V ELF (machine {e_machine})")
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", elf, e_shoff + i * e_shentsize)
            for i in range(e_shnum)]
    strtab = hdrs[e_shstrndx]
    names = elf[strtab[4]:strtab[4] + strtab[5]]

    def name(off):
        return names[off:names.index(b"\0", off)].decode()

    secs = []
    for sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, *_ in hdrs:
        if not sh_flags & SHF_ALLOC or sh_type not in (SHT_PROGBITS, SHT_NOBITS):
            continue
        if sh_size == 0:
            continue
        data = b"" if sh_type == SHT_NOBITS else elf[sh_off:sh_off + sh_size]
        secs.append(ElfSection(name(sh_name), sh_type, sh_flags, sh_addr,
                               data, sh_size))
    return e_entry, secs


def check_module(secs: list[ElfSection]) -> dict:
    """Validate a module's sections and descriptor; return a summary."""
    magic = [s for s in secs if s.name == MAGIC_SECTION]
    if len(magic) != 1:
        raise ValueError("module needs exactly one section named 'MAGIC'")
    m = magic[0].data
    if m[:8] != MODULE_MAGIC:
        raise ValueError("MAGIC section must start with 'ePDK.mod'")
    if len(m) != MAGIC_LEN:
        raise ValueError(f"MAGIC section is {len(m)} bytes; it must be exactly "
                         f"{MAGIC_LEN} (the kernel copies it into an 80-byte struct)")
    mtype = m[12]
    mif = struct.unpack_from("<7Q", m, 24)
    base = MODULE_SPACE + (mtype << 20)
    load = [s for s in secs if s is not magic[0]]
    if not load:
        raise ValueError("module has no loadable sections")
    lo = min(s.addr for s in load)
    hi = max(s.addr + s.size for s in load)
    if lo != base:
        raise ValueError(f"module type {mtype:#x} must be linked at {base:#x}, "
                         f"but its first section is at {lo:#x}")
    if hi - base > 1 << 20:
        raise ValueError(f"module is {hi - base} bytes; a slot is 1 MiB")
    for nm, p in zip(("MInit", "MExit", "MOpen", "MClose", "MRead", "MWrite",
                      "MIoctrl"), mif):
        if not lo <= p < hi:
            raise ValueError(f"{nm} pointer {p:#x} is outside the module")
    for s in load:
        if s.addr + s.size > 0xFFFFFFFF:
            raise ValueError(f"section {s.name} doesn't fit a 32-bit address")
    return {"type": mtype, "base": base, "size": hi - base,
            "MInit": mif[0], "MOpen": mif[2]}


def pack_module(elf64: bytes) -> bytes:
    """Convert a linked RV64 ELF64 module into the Melis plain-ELF layout."""
    _, secs = _read_elf64(elf64)
    check_module(secs)

    shstr = bytearray(b"\0")
    name_off = []
    for nm in [s.name for s in secs] + [".shstrtab"]:
        name_off.append(len(shstr))
        shstr += nm.encode() + b"\0"

    body = bytearray()
    pos = 52
    offsets = []
    for s in secs:
        pad = (-pos) % 8
        body += b"\0" * pad
        pos += pad
        offsets.append(pos if s.data else 0)
        body += s.data
        pos += len(s.data)
    shstr_off = pos
    body += shstr
    pos += len(shstr)
    pad = (-pos) % 4
    body += b"\0" * pad
    shoff = pos + pad

    shnum = len(secs) + 2  # null + sections + shstrtab
    sh = bytearray(40)
    for s, off, no in zip(secs, offsets, name_off):
        sh += struct.pack("<10I", no, s.type, s.flags, s.addr, off, s.size,
                          0, 0, 8, 0)
    sh += struct.pack("<10I", name_off[-1], SHT_STRTAB, 0, 0, shstr_off,
                      len(shstr), 0, 0, 1, 0)

    magic = next(s for s in secs if s.name == MAGIC_SECTION)
    entry = struct.unpack_from("<Q", magic.data, 24)[0]
    hdr = ELF_MAGIC + bytes([1, 1, 1, 0]) + b"\0" * 8
    hdr += struct.pack("<HHIIIIIHHHHHH", 2, EM_RISCV, 1, entry, 0, shoff, 0x5,
                       52, 0, 0, 40, shnum, shnum - 1)
    out = bytes(hdr + body + sh)
    assert len(hdr) == 52
    return out


def read_packed(mod: bytes) -> list[ElfSection]:
    """Parse a packed module the way the Melis loader does (for verification)."""
    if mod[:4] != ELF_MAGIC:
        raise ValueError("not an ELF file")
    shoff, = struct.unpack_from("<I", mod, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", mod, 46)
    hdrs = [struct.unpack_from("<10I", mod, shoff + i * shentsize)
            for i in range(shnum)]
    st = hdrs[shstrndx]
    names = mod[st[4]:st[4] + st[5]]
    out = []
    for h in hdrs:
        nm = names[h[0]:names.index(b"\0", h[0])].decode()
        if h[1] in (SHT_PROGBITS, SHT_NOBITS):
            data = b"" if h[1] == SHT_NOBITS else mod[h[4]:h[4] + h[5]]
            out.append(ElfSection(nm, h[1], h[2], h[3], data, h[5]))
    return out


def install_module(img, mod: bytes, slot: str = DEFAULT_SLOT,
                   partition: str = "data_udisk.fex") -> dict:
    """Put a packed module into ``img`` (an ``ImageWTY``, edited in place):
    replace the placeholder ``slot`` with it and make ``/startup.sh`` insmod it.

    Idempotent: an existing insmod line for the slot isn't duplicated."""
    info = check_module(read_packed(mod))
    fs = MinFS(img.read(partition))
    slot_node = fs.find(slot)
    real_slot = next(p for p, n in fs.walk() if n is slot_node)
    if slot_node.is_dir or slot_node.is_module:
        raise ValueError(f"{slot} is not a plain file")
    if real_slot == DEFAULT_SLOT and slot_node.length != 0:
        raise ValueError(f"{slot} is not the empty placeholder it should be "
                         f"({slot_node.length} bytes); refusing to overwrite")

    script = bytes(fs.read(fs.find(STARTUP)))
    eol = b"\r\n" if b"\r\n" in script else b"\n"
    line = b"insmod d:" + real_slot.replace("/", "\\").encode()
    lines = script.split(eol)
    if line not in (l.strip() for l in lines):
        # after the last insmod, so the stock desktop module loads first
        last = max((i for i, l in enumerate(lines)
                    if l.strip().startswith(b"insmod")), default=-1)
        lines.insert(last + 1, line)
    new_script = eol.join(lines)

    blob = fs.repack({real_slot: mod, STARTUP: new_script})
    write_partition(img, blob, partition, "V" + partition)
    return {**info, "slot": real_slot, "insmod": line.decode(),
            "startup": new_script.decode("latin1")}
