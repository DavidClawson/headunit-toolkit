#!/usr/bin/env python3
"""mkfw.py: wrap a DRAM program as a NOR boot image for the boot0 selector.

    tools/fel/dram/mkfw.py app.bin [-o app.fw]        # build
    tools/fel/dram/mkfw.py --check app.fw             # verify, exactly as sel does
    tools/fel/dram/mkfw.py --stage2 app2.bin -o app2.s2   # stage two (NOR S2_ADDR)
    tools/fel/dram/mkfw.py --check app2.s2            # verify, exactly as stage1.c does

Image = one 512-byte header sector + the program padded to 4 bytes. It goes
to NOR 0x120000 (sector 0x900). Layout and constants: ../bootsel/fwboot.h.
"""
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def constants():
    src = open(os.path.join(HERE, "..", "bootsel", "fwboot.h")).read()
    c = {}
    for name, val in re.findall(r"^#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$", src, re.M):
        c[name] = eval(val, {}, c)
    return c


def checksum(prog):
    return sum(struct.unpack(f"<{len(prog) // 4}I", prog)) & 0xFFFFFFFF


def kind(img, c):
    """'s2' for a stage-two image, 'fw' otherwise."""
    return "s2" if struct.unpack("<I", img[:4])[0] == c["S2_MAGIC"] else "fw"


def check(img, c):
    """The selector's (or for stage two, stage1.c's) checks, in its order. Returns (ok, reason)."""
    magic, ver, load, size, s = struct.unpack("<5I", img[:20])
    P = "S2" if kind(img, c) == "s2" else "FW"
    if magic != c[f"{P}_MAGIC"]: return False, f"magic {magic:#x}"
    if ver != c[f"{P}_VERSION"]: return False, f"version {ver}"
    if load != c[f"{P}_LOAD"]: return False, f"load {load:#x}"
    if size == 0 or size > c[f"{P}_MAX"] or size % 4: return False, f"size {size}"
    prog = img[512:512 + size]
    if len(prog) != size: return False, "truncated"
    if checksum(prog) != s: return False, f"checksum {checksum(prog):#x} != {s:#x}"
    return True, f"ok: {size} bytes, checksum {s:#010x}, loads at {load:#x}"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input")
    p.add_argument("-o", "--out")
    p.add_argument("--check", action="store_true")
    p.add_argument("--stage2", action="store_true", help="build a stage-two image (program linked at S2_LOAD)")
    a = p.parse_args()
    c = constants()
    if a.check:
        ok, why = check(open(a.input, "rb").read(), c)
        print(why)
        sys.exit(0 if ok else 1)
    prog = open(a.input, "rb").read()
    prog += b"\0" * (-len(prog) % 4)
    if a.stage2:
        if len(prog) > c["S2_MAX"]:
            sys.exit(f"{len(prog)} bytes: too big for stage two ({c['S2_MAX']} max)")
        hdr = struct.pack("<5I", c["S2_MAGIC"], c["S2_VERSION"], c["S2_LOAD"], len(prog), checksum(prog))
    else:
        if len(prog) > c["FW_APP_MAX"]:
            sys.exit(f"{len(prog)} bytes: too big ({c['FW_APP_MAX']} max: the slot's last 4 KB holds settings)")
        hdr = struct.pack("<5I", c["FW_MAGIC"], c["FW_VERSION"], c["FW_LOAD"], len(prog), checksum(prog))
    img = hdr.ljust(512, b"\0") + prog
    out = a.out or os.path.splitext(a.input)[0] + (".s2" if a.stage2 else ".fw")
    open(out, "wb").write(img)
    ok, why = check(img, c)
    assert ok, why
    print(f"wrote {out}: {len(img)} bytes for NOR {c['S2_ADDR'] if a.stage2 else c['FW_SECTOR'] * 512:#x}; {why}")


main()
