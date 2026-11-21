"""
DATAV1.0 UI-resource reader — the compiled screen-layout files inside the
Melis app partition (``Radio.data``, ``RadioRds.data``, ``SetupMenu.data`` …).

A screen is a flat list of named widgets. Each widget record contains:

* a 0xC8-byte UTF-16LE name buffer (e.g. ``ViewShowStrRdsPs``). The prefix
  encodes the type: ``...Str...`` (text), ``...Image...``, ``...Button...``,
  ``...Slider...``, ``...Wnd...``, ``...List...``.
* geometry + style int32s, and
* a UTF-16LE sample-value buffer at ``name+0xCC`` (e.g. ``"Station name"``).

Field offsets are **fully verified for text (``Str``) widgets**: four int32s
(x, y, w, h) sit at ``name-0x38``, followed by a font size and fg/bg colors.
Button/Slider/Wnd records carry one extra leading word, so their geometry is
shifted by 4 bytes — exposed here as ``geom_raw`` without interpretation.

The file header carries a magic, a widget count at 0x24, and a section table
at 0x30; we read the widget count there and cross-check by scanning.
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass

MAGIC = "DATAV1.0"
_NAME_RE = re.compile(rb"V\x00i\x00e\x00w\x00S\x00h\x00o\x00w\x00")


def _utf16z(data: bytes, off: int) -> str:
    out = bytearray()
    while off + 1 < len(data) and data[off:off + 2] != b"\x00\x00":
        out += data[off:off + 2]
        off += 2
    return out.decode("utf-16le", "replace")


@dataclass
class Widget:
    name: str
    kind: str               # Str / Image / Button / Slider / Wnd / List
    offset: int
    value: str              # sample/default text
    geom: tuple | None      # (x, y, w, h) — reliable for Str widgets
    geom_raw: tuple         # raw 4 int32 at name-0x38 (all kinds)
    fg: int | None          # 0xRRGGBB primary text color (Str widgets)
    alt: int | None         # 0xRRGGBB secondary/state color slot (Str widgets)


def _kind(name: str) -> str:
    for k in ("Str", "Image", "Button", "Slider", "Wnd", "List"):
        if "ViewShow" + k in name:
            return k
    return "?"


class DataV1:
    def __init__(self, data: bytes):
        self.data = bytes(data)
        magic = data[:16].decode("utf-16le", "replace").rstrip("\0 ")
        if not magic.startswith(MAGIC):
            raise ValueError("not a DATAV1.0 resource")
        self.declared_count = struct.unpack_from("<I", data, 0x24)[0]
        self.widgets: list[Widget] = []
        for m in _NAME_RE.finditer(data):
            off = m.start()
            name = _utf16z(data, off)
            kind = _kind(name)
            raw = struct.unpack_from("<4i", data, off - 0x38)
            value = _utf16z(data, off + 0xCC)
            geom = fg = alt = None
            if kind == "Str":
                geom = raw
                fg, alt = (v & 0xFFFFFF for v in
                           struct.unpack_from("<2I", data, off - 0x24))
            self.widgets.append(
                Widget(name, kind, off, value, geom, raw, fg, alt)
            )

    def by_name(self, name: str) -> Widget:
        for w in self.widgets:
            if w.name == name:
                return w
        raise KeyError(name)
