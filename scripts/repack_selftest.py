#!/usr/bin/env python3
"""
Self-test for the MINFS repacker (``headunit.minfs.MinFS.repack``).

Exercises all three edit paths against a real ``LTTF133.img`` and checks the
core invariant: a repack changes the partition **only** inside the edited
file's slot (or its new arena slot) plus its 12-byte directory entry, keeps the
partition's exact size, keeps the paired V-partition checksum correct, and
leaves every other partition in the image byte-identical.

Usage::

    python3 scripts/repack_selftest.py /path/to/LTTF133.img

The image is yours to supply; it is never committed. Exits non-zero on failure.
"""
from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from headunit.imagewty import ImageWTY            # noqa: E402
from headunit.minfs import MinFS, checksum, write_partition  # noqa: E402

u16 = lambda s: s.encode("utf-16-le")  # noqa: E731


def diff_ranges(a: bytes, b: bytes, block: int = 1 << 16):
    """Half-open [start, end) ranges where a and b differ (dependency-free)."""
    out: list[list[int]] = []
    n = len(a)
    i = 0
    while i < n:
        j = min(i + block, n)
        if a[i:j] != b[i:j]:
            k = i
            while k < j:
                if a[k] != b[k]:
                    s = k
                    while k < j and a[k] != b[k]:
                        k += 1
                    if out and out[-1][1] == s:
                        out[-1][1] = k
                    else:
                        out.append([s, k])
                else:
                    k += 1
        i = j
    return [(s, e) for s, e in out]


def main(img_path: str) -> int:
    failures = 0

    def check(label, cond):
        nonlocal failures
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")
        if not cond:
            failures += 1

    img0 = ImageWTY.from_path(img_path)
    blob0 = img0.read("data_udisk.fex")
    part_len = len(blob0)
    mfs0 = MinFS(blob0)
    snode = mfs0.find("/Setup.txt")
    off0, eoff = snode.offset, snode.entry_off
    raw0 = open(img_path, "rb").read()

    orig = mfs0.read("/Setup.txt")
    A = orig.replace(u16("botton"), u16("button"))            # net 0  -> in place
    B = A.replace(u16("RDS swich"), u16("RDS switch"))        # net +2 -> relocate
    C = B.replace(u16("Mirroring\xa0"), u16("Mirroring"))     # ship   -> in place (shorter)

    def case(label, newtxt, expect_relocate):
        print(f"\n=== {label}  (len {len(orig)} -> {len(newtxt)}) ===")
        nb = mfs0.repack({"/Setup.txt": newtxt})
        check("partition size preserved", len(nb) == part_len)
        m2 = MinFS(nb)
        check("/Setup.txt reads new bytes", m2.read("/Setup.txt") == newtxt)
        check("/Language/Setup.txt alias updated",
              m2.read("/Language/Setup.txt") == newtxt)
        woff = m2.find("/Setup.txt").offset
        check("relocated" if expect_relocate else "in place",
              (woff != off0) if expect_relocate else (woff == off0))
        allowed = [(eoff, eoff + 12), (woff, woff + len(newtxt))]
        ranges = diff_ranges(blob0, nb)
        bad = [r for r in ranges if not any(r[0] >= a and r[1] <= b
                                            for a, b in allowed)]
        for r in bad:
            print("     unexpected change at", r)
        check("changes confined to entry record + write slot", not bad)
        check("something changed", len(ranges) > 0)
        img = ImageWTY.from_path(img_path)
        write_partition(img, nb)
        v = img.get("Vdata_udisk.fex")
        check("V-checksum == checksum(new blob)",
              struct.unpack_from("<I", img.data, v.offset)[0] == checksum(nb))
        others = all(
            raw0[f.offset:f.offset + f.length]
            == bytes(img.data[f.offset:f.offset + f.length])
            for f in img.files
            if f.name not in ("data_udisk.fex", "Vdata_udisk.fex")
        )
        check("every non-MINFS partition byte-identical", others)

    case("in-place, same length (botton->button x2)", A, False)
    case("relocate, grow (+swich->switch)", B, True)
    case("SHIP set (botton + swich + strip nbsp)", C, False)

    print("\n" + ("ALL TESTS PASSED" if not failures
                  else f"{failures} CHECK(S) FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: repack_selftest.py /path/to/LTTF133.img")
    sys.exit(main(sys.argv[1]))
