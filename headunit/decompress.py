"""Decompress the LZMA-packed payloads inside MINFS (`init.axf`, `*.mod`,
`*.drv`, `*.plg`).

These are **not** standard single LZMA streams. Allwinner's packer concatenates
one or more **raw LZMA1** streams, each introduced by a 5-byte header
``5d 00 80 00 00`` (properties ``0x5d`` → lc=3, lp=0, pb=2; dict ``0x8000``) and
carrying **no** 8-byte uncompressed-size field and (usually) no end-of-stream
marker. The decompressor therefore:

1. splits the stored bytes on the ``5d 00 80 00 00`` magic,
2. decodes each block as raw LZMA1, bounding its output to the bytes still owed
   toward ``original_len`` (so the final stream doesn't over-run into padding),
3. concatenates and truncates to ``original_len``.

This reproduces ``init.axf`` and ``auto.mod`` **exactly**. A few files
(``carplay.mod``, ``cedar/vdecoder.drv``, the tiny ``cedar/*.plg``) come back a
little short or fail a block — a false magic inside compressed data, or a
framing variant on the small plugins — so the result is returned with a
``complete`` flag rather than silently trusted. See
``docs/format-minfs.md``.

What a decoded payload *is*: not ELF. ``init.axf`` is a stripped Melis RISC-V
application; the ``.mod``/``.drv`` modules share a custom Melis container header
(``97 ?? 0? 00  93 87 …``) with internal ``MAGIC``/``VERSION``/``MOD_`` fields.
Producing a loadable module means emitting that container, not an ELF ``.so``.
"""
from __future__ import annotations

import lzma
import re
from dataclasses import dataclass

MELIS_LZMA_MAGIC = b"\x5d\x00\x80\x00\x00"
_RAW_FILTERS = [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 26,
                 "lc": 3, "lp": 0, "pb": 2}]


@dataclass
class Decoded:
    data: bytes
    original_len: int
    complete: bool          # True iff len(data) == original_len with no block error
    blocks: int             # LZMA streams detected
    error: str | None = None


def is_compressed(stored: bytes) -> bool:
    """A payload is LZMA-packed iff it begins with the Melis LZMA magic."""
    return stored[:5] == MELIS_LZMA_MAGIC


def decompress(stored: bytes, original_len: int) -> Decoded:
    """Decode a Melis LZMA payload. ``stored`` is the on-disk (compressed)
    bytes — i.e. ``partition[node.offset : node.offset + node.stored_len]``, not
    the over-reading ``MinFS.read``. ``original_len`` is ``node.length``.

    If ``stored`` isn't LZMA-magic'd it's returned verbatim (already
    uncompressed). The returned ``complete`` flag says whether the full
    ``original_len`` was recovered; callers should check it before trusting the
    bytes of the known-partial files."""
    if not is_compressed(stored):
        return Decoded(stored, original_len, len(stored) == original_len, 0)

    offs = [m.start() for m in re.finditer(re.escape(MELIS_LZMA_MAGIC), stored)]
    out = bytearray()
    err = None
    for i, o in enumerate(offs):
        if len(out) >= original_len:
            break
        end = offs[i + 1] if i + 1 < len(offs) else len(stored)
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=_RAW_FILTERS)
        try:
            out += dec.decompress(stored[o + 5:end],
                                  max_length=original_len - len(out))
        except lzma.LZMAError as e:
            err = f"block {i} @ {o}: {e}"
            break
    data = bytes(out[:original_len])
    return Decoded(data, original_len, len(data) == original_len and err is None,
                   len(offs), err)


def decompress_node(mfs, node_or_path) -> Decoded:
    """Decompress a MINFS node (or path) from its parsed ``MinFS``.

    Reads the node's *stored* bytes (``offset``..``offset+stored_len``) — unlike
    ``MinFS.read``, which uses ``length`` and over-reads compressed files."""
    n = mfs.find(node_or_path) if isinstance(node_or_path, str) else node_or_path
    stored = mfs.data[n.offset:n.offset + n.stored_len]
    return decompress(stored, n.length)
