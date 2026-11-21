"""
DATAV1.0 UI-resource **editor** — the writer side of :mod:`headunit.datav1`.

A ``*.data`` screen layout lives inside the ``data_udisk.fex`` MINFS partition
at ``node.offset``. This module performs **in-place** widget edits that never
change a file's byte length, so no MINFS offset or directory entry has to move:

* :func:`set_geometry` — rewrite a widget's ``(x, y, w, h)`` int32s.
* :func:`set_colors`   — rewrite a ``Str`` widget's primary / secondary color.
* :func:`set_value`    — rewrite a ``Str`` widget's sample/value text, provided
  the new UTF-16LE bytes fit the original value buffer (slack is zero-padded).

Every edit operates on an in-memory :class:`~headunit.imagewty.ImageWTY`:
it patches the partition bytes at ``node.offset + local_widget_offset`` and then
fixes the partition's additive checksum in ``Vdata_udisk.fex`` — exactly the
pattern :mod:`headunit.logo` uses. Call :meth:`ImageWTY.to_bytes` afterwards to
serialise the rebuilt image.

**Scope / caveats**

* Changing a file's *size* would require repacking MINFS (shifting every later
  offset and directory entry). That is out of scope; all helpers here preserve
  byte length and refuse edits that would not fit.
* Geometry at ``name-0x38`` is only reliable for ``Str`` (text) widgets.
  ``Button``/``Slider``/``Wnd`` records carry one extra leading word, so their
  geometry is shifted. :func:`set_geometry` refuses non-``Str`` kinds unless
  ``force=True`` (then it writes the raw slot and warns — you own the result).
* Color slots are only meaningful for ``Str`` widgets; :func:`set_colors` and
  :func:`set_value` refuse other kinds.
* Repositioning/restyling an *existing* widget is a pure resource edit and
  works. Making a control show **new runtime data** (e.g. populating a RadioText
  field the stock UI never fills) additionally requires changes to the app
  binary ``init.axf`` — a layout edit alone cannot add a functional field.
"""
from __future__ import annotations

import struct
import warnings
from dataclasses import dataclass

from .datav1 import DataV1, Widget
from .imagewty import ImageWTY, FileEntry
from .minfs import MAGIC as MINFS_MAGIC, MinFS, Node, checksum

# Local widget-record field offsets, relative to the start of the name buffer.
_GEOM_OFF = -0x38   # 4 x int32 : x, y, w, h   (Str widgets)
_FG_OFF = -0x24     # u32 0x00RRGGBB primary color (Str)
_ALT_OFF = -0x20    # u32 secondary / state color  (Str)
_VALUE_OFF = 0xCC   # utf-16z sample/value string


@dataclass
class Change:
    """One contiguous byte range modified by an edit, for reporting/verifying.

    ``image_offset`` is absolute within the rebuilt IMAGEWTY image; ``before``
    and ``after`` are the old and new bytes of that range.
    """
    what: str
    image_offset: int
    before: bytes
    after: bytes

    @property
    def length(self) -> int:
        return len(self.after)

    @property
    def end(self) -> int:
        return self.image_offset + self.length

    def __str__(self) -> str:  # pragma: no cover - cosmetic
        return (f"{self.what}: [{self.image_offset:#x}..{self.end:#x}) "
                f"{self.before.hex()} -> {self.after.hex()}")


# -- partition plumbing ----------------------------------------------------
def _vname(partition: str) -> str:
    """Name of the paired checksum ("V") partition for ``partition``."""
    return "V" + partition


def _open_partition(img: ImageWTY, partition: str):
    """Return ``(part_entry, v_entry, part_bytearray)`` after verifying the
    stored checksum. Mirrors the guard in :func:`headunit.logo.swap_logo`."""
    part_e = img.get(partition)
    v_e = img.get(_vname(partition))
    part = bytearray(img.read(part_e))
    if part[:5] != MINFS_MAGIC:
        raise ValueError(f"{partition} is not a MINFS partition")
    if struct.unpack_from("<I", img.data, v_e.offset)[0] != checksum(part):
        raise ValueError("input partition checksum mismatch; aborting")
    return part_e, v_e, part


def _locate(part: bytes, data_path: str, widget_name: str):
    """Resolve ``(node, widget)`` for ``widget_name`` inside ``data_path``."""
    fs = MinFS(part)
    node = fs.find(data_path)
    scr = DataV1(bytes(part[node.offset:node.offset + node.length]))
    return node, scr.by_name(widget_name)


def _commit(img: ImageWTY, part_e: FileEntry, v_e: FileEntry,
            part: bytearray, changes: list[Change]) -> list[Change]:
    """Write ``part`` back into the image, refresh the checksum, and translate
    each change's partition offset to an absolute image offset."""
    img.data[part_e.offset:part_e.offset + part_e.length] = part
    struct.pack_into("<I", img.data, v_e.offset, checksum(part))
    for ch in changes:
        ch.image_offset += part_e.offset
    return changes


# -- edits -----------------------------------------------------------------
def set_geometry(img: ImageWTY, data_path: str, widget_name: str,
                 x: int, y: int, w: int, h: int,
                 partition: str = "data_udisk.fex",
                 force: bool = False) -> list[Change]:
    """Set a widget's geometry ``(x, y, w, h)`` (four int32 at ``name-0x38``).

    Reliable for ``Str`` widgets only. For other kinds the slot is shifted by
    one word, so the call refuses unless ``force=True`` (which writes the raw
    slot and emits a warning).
    """
    part_e, v_e, part = _open_partition(img, partition)
    node, widget = _locate(part, data_path, widget_name)
    if widget.kind != "Str":
        if not force:
            raise ValueError(
                f"{widget_name!r} is a {widget.kind!r} widget: geometry at "
                f"name-0x38 is only verified for Str widgets. Pass force=True "
                f"to write the raw slot anyway."
            )
        warnings.warn(
            f"{widget_name!r} is {widget.kind!r}; its geometry slot is shifted "
            f"by one word — writing name-0x38 raw as requested.",
            stacklevel=2,
        )
    for label, v in (("x", x), ("y", y), ("w", w), ("h", h)):
        if not -0x8000_0000 <= v <= 0x7FFF_FFFF:
            raise ValueError(f"{label}={v} out of int32 range")

    pos = node.offset + widget.offset + _GEOM_OFF
    before = bytes(part[pos:pos + 16])
    after = struct.pack("<4i", x, y, w, h)
    part[pos:pos + 16] = after
    changes = [Change("geometry", node.offset + widget.offset + _GEOM_OFF,
                      before, after)]
    return _commit(img, part_e, v_e, part, changes)


def set_colors(img: ImageWTY, data_path: str, widget_name: str,
               fg: int | None = None, alt: int | None = None,
               partition: str = "data_udisk.fex") -> list[Change]:
    """Set a ``Str`` widget's primary (``fg``) and/or secondary (``alt``) color.

    Colors are ``0xRRGGBB``; only the low 24 bits are written, the stored high
    byte (flags) is preserved. Pass either or both; at least one is required.
    """
    if fg is None and alt is None:
        raise ValueError("nothing to do: pass fg and/or alt")
    part_e, v_e, part = _open_partition(img, partition)
    node, widget = _locate(part, data_path, widget_name)
    if widget.kind != "Str":
        raise ValueError(
            f"{widget_name!r} is a {widget.kind!r} widget: color slots are "
            f"only defined for Str widgets."
        )

    changes: list[Change] = []
    for name, value, off in (("fg", fg, _FG_OFF), ("alt", alt, _ALT_OFF)):
        if value is None:
            continue
        if not 0 <= value <= 0xFFFFFF:
            raise ValueError(f"{name}={value:#x} out of 0xRRGGBB range")
        pos = node.offset + widget.offset + off
        old = struct.unpack_from("<I", part, pos)[0]
        new = (old & 0xFF000000) | value           # preserve high-byte flags
        before = bytes(part[pos:pos + 4])
        after = struct.pack("<I", new)
        part[pos:pos + 4] = after
        changes.append(Change(f"color.{name}", pos, before, after))
    return _commit(img, part_e, v_e, part, changes)


def _value_capacity(part: bytes, value_pos: int) -> int:
    """Bytes available for a value string at ``value_pos``: the current string
    plus its trailing zero padding, up to the next non-zero field."""
    p = value_pos
    while p + 1 < len(part) and part[p:p + 2] != b"\x00\x00":
        p += 2                                      # skip current string
    while p < len(part) and part[p] == 0:
        p += 1                                      # skip zero padding
    return p - value_pos


def set_value(img: ImageWTY, data_path: str, widget_name: str, text: str,
              partition: str = "data_udisk.fex") -> list[Change]:
    """Set a ``Str`` widget's sample/value text (UTF-16LE at ``name+0xCC``).

    The new string plus a NUL terminator must fit the original value buffer
    (bounded by the next non-zero field); the whole buffer is zero-padded so
    the file size is unchanged. Refuses if the text is too long.
    """
    part_e, v_e, part = _open_partition(img, partition)
    node, widget = _locate(part, data_path, widget_name)
    if widget.kind != "Str":
        raise ValueError(
            f"{widget_name!r} is a {widget.kind!r} widget: the value buffer is "
            f"only defined for Str widgets."
        )

    pos = node.offset + widget.offset + _VALUE_OFF
    cap = _value_capacity(part, pos)
    encoded = text.encode("utf-16le")
    if len(encoded) + 2 > cap:                      # +2 for NUL terminator
        raise ValueError(
            f"value {text!r} needs {len(encoded) + 2} bytes but the buffer "
            f"holds {cap}; shorten it (in-place edits cannot grow the file)."
        )
    before = bytes(part[pos:pos + cap])
    after = encoded + b"\0" * (cap - len(encoded))  # terminator + slack
    part[pos:pos + cap] = after
    changes = [Change("value", pos, before, after)]
    return _commit(img, part_e, v_e, part, changes)
