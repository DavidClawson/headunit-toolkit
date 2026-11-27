#!/usr/bin/env python3
"""mkboot0.py: build stock boot0 + FEL window + boot selector (see stub.S).

    tools/fel/bootsel/mkboot0.py <nor-backup.bin>                 # -> boot0-fw.bin
    tools/fel/bootsel/mkboot0.py <nor-backup.bin> --no-selector  # FEL window only

Takes boot0 (first 48 KB) from a full NOR backup, checks it is the stock
image this patch was written against, inserts stub.bin at 0x2b800, redirects
the FEL check (0x28c48), the FEL jump (0x28c66) and, unless --no-selector, the
Melis load (0x28d3c) to the boot selector, and fixes the eGON checksum.
The image on the bench before the selector (FEL window only, older stub
layout) is kept as stock/nor/boot0-felwin-v1.bin: that is the rollback.
"""
import argparse, hashlib, struct, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
LOAD = 0x20000
STUB_AT = 0x2b800            # inside boot0's random padding (0x2b4c0-0x2c000), past .bss
CALL_AT = 0x28c48            # jal ra,0x24d38 in boot0 main
ORIG_CALL = 0x8f0fc0ef       # jal ra,0x24d38 from 0x28c48
JUMP_AT = 0x28c66            # addi a0,zero,32 (then jalr a0 -> BROM FEL entry 0x20)
ORIG_JUMP = 0x02000513
HOOK_AT = 0x28d3c            # jal ra,0x284fa: load the Melis package
ORIG_HOOK = 0xfbeff0ef


def jal(frm, to, rd=1):
    off = to - frm
    assert -(1 << 20) <= off < (1 << 20) and off % 2 == 0
    o = off & 0x1FFFFF
    imm = ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3FF) << 21 | ((o >> 11) & 1) << 20 | ((o >> 12) & 0xFF) << 12
    return imm | rd << 7 | 0x6F


def egon_sum(img):
    img = bytearray(img)
    img[12:16] = struct.pack("<I", 0x5F0A6C39)
    return sum(struct.unpack(f"<{len(img)//4}I", img)) & 0xFFFFFFFF


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("backup")
    p.add_argument("-o", "--out")
    p.add_argument("--no-selector", action="store_true", help="FEL window only (no 0x28d3c hook)")
    a = p.parse_args()
    if not a.out:
        a.out = os.path.join(HERE, "boot0-felwin.bin" if a.no_selector else "boot0-fw.bin")

    subprocess.run(["make", "-s", "-C", HERE, "stub.bin"], check=True)
    stub = open(os.path.join(HERE, "stub.bin"), "rb").read()

    img = bytearray(open(a.backup, "rb").read()[:0xC000])
    if img[4:12] != b"eGON.BT0":
        sys.exit("no eGON.BT0 header at offset 4 (header erased? use a pre-fel backup)")
    length = struct.unpack("<I", img[16:20])[0]
    if length != 0xC000 or struct.unpack("<I", img[12:16])[0] != egon_sum(img):
        sys.exit("boot0 length/checksum not as expected")
    have = struct.unpack("<I", img[CALL_AT - LOAD:CALL_AT - LOAD + 4])[0]
    if have != ORIG_CALL:
        sys.exit(f"unexpected instruction at {CALL_AT:#x}: {have:#010x} (different boot0?)")
    have = struct.unpack("<I", img[JUMP_AT - LOAD:JUMP_AT - LOAD + 4])[0]
    if have != ORIG_JUMP:
        sys.exit(f"unexpected instruction at {JUMP_AT:#x}: {have:#010x}")
    have = struct.unpack("<I", img[HOOK_AT - LOAD:HOOK_AT - LOAD + 4])[0]
    if have != ORIG_HOOK:
        sys.exit(f"unexpected instruction at {HOOK_AT:#x}: {have:#010x}")
    print(f"stock boot0 sha256 {hashlib.sha256(img).hexdigest()[:16]}")

    s = STUB_AT - LOAD
    assert s + len(stub) <= 0xC000
    img[s:s + len(stub)] = stub
    img[CALL_AT - LOAD:CALL_AT - LOAD + 4] = struct.pack("<I", jal(CALL_AT, STUB_AT))
    syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(
        ["riscv64-elf-nm", os.path.join(HERE, "stub.elf")], capture_output=True, text=True, check=True).stdout.splitlines()
        if len(l.split()) == 3}
    assert syms["stub"] == STUB_AT
    tramp = syms["fel_tramp"]
    img[JUMP_AT - LOAD:JUMP_AT - LOAD + 4] = struct.pack("<I", jal(JUMP_AT, tramp, rd=0))
    if not a.no_selector:
        img[HOOK_AT - LOAD:HOOK_AT - LOAD + 4] = struct.pack("<I", jal(HOOK_AT, syms["sel"]))
    img[12:16] = struct.pack("<I", egon_sum(img))
    open(a.out, "wb").write(img)
    print(f"wrote {a.out}: stub {len(stub)} bytes at {STUB_AT:#x}; FEL check {CALL_AT:#x} -> {STUB_AT:#x}, "
          f"FEL jump {JUMP_AT:#x} -> {tramp:#x}"
          + ("" if a.no_selector else f", Melis load {HOOK_AT:#x} -> sel {syms['sel']:#x}")
          + f"; checksum {struct.unpack('<I', img[12:16])[0]:#010x}")


if __name__ == "__main__":
    main()
