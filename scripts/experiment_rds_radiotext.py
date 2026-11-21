#!/usr/bin/env python3
"""
Bench experiment: try to surface RDS **RadioText** (song/artist) with a
resource-only edit — no init.axf patching.

Hypothesis (see docs/format-datav1.md + docs/hardware.md): the Melis app
decodes RadioText and updates view controls *by name*. The control
``ViewShowStrRdsRt`` is referenced inside ``init.axf`` but is placed on no
screen, so it never renders. If the app fills controls purely by name, then
simply **placing** a ``ViewShowStrRdsRt`` control on the RDS screen should make
RadioText appear — with no binary patch.

This builds that test image the cheapest, lowest-risk way: it **renames** an
existing, expendable ``Str`` widget on ``RadioRds.data`` —
``ViewShowStrPtyType`` (the PTY-type label) — to ``ViewShowStrRdsRt``, keeping
its geometry, and sets a distinctive placeholder value so the three outcomes
are easy to tell apart on the unit:

  * placeholder replaced by changing song/artist text  -> SUCCESS (app fills RT
    by name; RadioText is a pure resource mod)
  * placeholder stays put (static "RT?")               -> app does NOT fill
    unplaced controls; RadioText needs init.axf work
  * field blank                                         -> app cleared it but no
    RT is being broadcast / decoded; retry on a station with RadioText

It is a same-length, in-place edit committed through ``MinFS.repack`` (so the
partition size and checksum stay correct). Fully reversible by reflashing stock.
The PTY-type label is temporarily sacrificed for the test only.

Usage::

    python3 scripts/experiment_rds_radiotext.py stock/LTTF133.img build-rdsrt
    # -> build-rdsrt/Update/LTTF133.img  (copy Update/ to a FAT32 stick)

On the unit: enable RDS + a region in Settings, tune an FM station that
broadcasts RadioText, open the RDS screen, and watch the old PTY-type slot
(mid-screen, around x=480 y=150).
"""
from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from headunit.imagewty import ImageWTY            # noqa: E402
from headunit.minfs import MinFS, write_partition  # noqa: E402
from headunit.datav1 import DataV1                 # noqa: E402

SCREEN = "/RadioRds.data"
TARGET = "ViewShowStrPtyType"      # expendable Str widget to repurpose
NEW_NAME = "ViewShowStrRdsRt"      # the control init.axf knows but never places
PLACEHOLDER = "RT?"                # distinctive "not filled" marker


def _u16z(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\x00\x00"


def build(stock_path: str, out_dir: str) -> str:
    img = ImageWTY.from_path(stock_path)
    mfs = MinFS(img.read("data_udisk.fex"))
    data = bytearray(mfs.read(SCREEN))

    scr = DataV1(bytes(data))
    w = scr.by_name(TARGET)
    off = w.offset

    # 1) rename: zero the old name's byte span, then write the new name.
    old_name_bytes = len(TARGET) * 2
    for i in range(off, off + old_name_bytes + 2):
        data[i] = 0
    nb = _u16z(NEW_NAME)
    data[off:off + len(nb)] = nb

    # 2) distinctive placeholder value at name+0xCC (kept within the old value's
    #    footprint so nothing downstream moves).
    voff = off + 0xCC
    old_val_bytes = len(w.value) * 2
    for i in range(voff, voff + max(old_val_bytes, len(_u16z(PLACEHOLDER)))):
        data[i] = 0
    pv = _u16z(PLACEHOLDER)
    data[voff:voff + len(pv)] = pv

    # commit (same length -> in place) and fix checksum
    new_blob = mfs.repack({SCREEN: bytes(data)})
    write_partition(img, new_blob)

    out_img = os.path.join(out_dir, "Update", "LTTF133.img")
    os.makedirs(os.path.dirname(out_img), exist_ok=True)
    with open(out_img, "wb") as fh:
        fh.write(img.to_bytes())
    return out_img


def verify(out_img: str) -> None:
    img = ImageWTY.from_path(out_img)
    mfs = MinFS(img.read("data_udisk.fex"))
    from headunit.minfs import checksum
    blob = img.read("data_udisk.fex")
    v = img.get("Vdata_udisk.fex")
    scr = DataV1(mfs.read(SCREEN))
    names = {w.name for w in scr.widgets}
    rt = next((w for w in scr.widgets if w.name == NEW_NAME), None)
    print("  RdsRt present      :", NEW_NAME in names)
    print("  PtyType removed    :", TARGET not in names)
    print("  RdsRt kind/geom/val:", rt and (rt.kind, rt.geom, rt.value))
    print("  partition size OK  :", len(blob) == len(ImageWTY.from_path(sys.argv[1]).read("data_udisk.fex")))
    print("  V-checksum OK      :",
          struct.unpack_from("<I", img.data, v.offset)[0] == checksum(blob))
    print("  widget count       :", len(scr.widgets))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: experiment_rds_radiotext.py <stock LTTF133.img> <out_dir>")
    out = build(sys.argv[1], sys.argv[2])
    print(f"wrote {out}")
    print("verification:")
    verify(out)
    print("Copy the whole 'Update' folder to a FAT32 USB stick.")
