#!/usr/bin/env python3
"""mkboot0.py: build stock boot0 + FEL-window stub (see stub.S).

    tools/fel/bootsel/mkboot0.py <nor-backup.bin> [-o boot0-felwin.bin]

Takes boot0 (first 48 KB) from a full NOR backup, checks it is the stock
image this patch was written against, inserts stub.bin at 0x2b800, redirects
the one call at 0x28c48, and fixes the eGON checksum.
"""
import argparse, hashlib, struct, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
LOAD = 0x20000
STUB_AT = 0x2b800            # inside boot0's random padding (0x2b4c0-0x2c000), past .bss
CALL_AT = 0x28c48            # jal ra,0x24d38 in boot0 main
ORIG_CALL = 0x8f0fc0ef       # jal ra,0x24d38 from 0x28c48
JUMP_AT = 0x28c66            # addi a0,zero,32 (then jalr a0 -> BROM FEL entry 0x20)
ORIG_JUMP = 0x02000513


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
    p.add_argument("-o", "--out", default=os.path.join(HERE, "boot0-felwin.bin"))
    a = p.parse_args()

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
    print(f"stock boot0 sha256 {hashlib.sha256(img).hexdigest()[:16]}")

    s = STUB_AT - LOAD
    assert s + len(stub) <= 0xC000
    img[s:s + len(stub)] = stub
    img[CALL_AT - LOAD:CALL_AT - LOAD + 4] = struct.pack("<I", jal(CALL_AT, STUB_AT))
    tramp = STUB_AT + int(subprocess.run(["riscv64-elf-nm", os.path.join(HERE, "stub.elf")], capture_output=True,
                          text=True, check=True).stdout.split(" T fel_tramp")[0].split()[-1], 16) - STUB_AT
    img[JUMP_AT - LOAD:JUMP_AT - LOAD + 4] = struct.pack("<I", jal(JUMP_AT, tramp, rd=0))
    img[12:16] = struct.pack("<I", egon_sum(img))
    open(a.out, "wb").write(img)
    print(f"wrote {a.out}: stub {len(stub)} bytes at {STUB_AT:#x}, call at {CALL_AT:#x} and FEL jump at {JUMP_AT:#x} redirected, "
          f"checksum {struct.unpack('<I', img[12:16])[0]:#010x}")


if __name__ == "__main__":
    main()
