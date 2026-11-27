#!/usr/bin/env python3
"""Offline smoke test for a packed Melis module (hostmod/build/*.mod).

Loads the module the way the kernel's plain-ELF loader does (ELF32-layout
headers, section named MAGIC, ALLOC sections mapped at their vaddr, bss
zeroed), then runs MInit, MOpen and any thread MOpen creates in the Unicorn
RISC-V emulator. Every `ecall` is answered by a fake kernel that logs the call,
so you can see exactly which syscalls the module makes, with which arguments.

    python3 scripts/emulate_module.py hostmod/build/hello.mod
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/scripts/", 1)[0])
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV64, UC_HOOK_INTR  # noqa: E402
from unicorn.riscv_const import (  # noqa: E402
    UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_A3,
    UC_RISCV_REG_A7, UC_RISCV_REG_PC, UC_RISCV_REG_RA, UC_RISCV_REG_SP)

from headunit.modpack import check_module, read_packed, MODULE_MAGIC  # noqa: E402

STACK, STACK_SZ = 0x7F000000, 0x100000
RET_TRAP = 0x7E000000          # MInit/MOpen "return here" -> stop
FAKE_FP = 0x7D000000

NAMES = {0x10000003: "esSIOS_putstr", 0x10000100: "esKRNL_TCreate",
         0x10000101: "esKRNL_TDel", 0x1000010C: "esKRNL_TimeDly",
         0x10000308: "esFSYS_fopen", 0x10000315: "esFSYS_fwrite",
         0x10000302: "esFSYS_fclose"}


def cstr(uc, addr, limit=256):
    b = bytes(uc.mem_read(addr, limit))
    return b[:b.index(b"\0")].decode("latin1") if b"\0" in b else b.decode("latin1")


def main(path, mount="F"):
    mod = open(path, "rb").read()
    secs = read_packed(mod)
    info = check_module(secs)
    magic = next(s for s in secs if s.name == "MAGIC").data
    assert magic[:8] == MODULE_MAGIC and len(magic) == 80
    mif = dict(zip(("MInit", "MExit", "MOpen", "MClose", "MRead", "MWrite",
                    "MIoctrl"), struct.unpack_from("<7Q", magic, 24)))

    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV64)
    uc.mem_map(info["base"], 1 << 20)
    for s in secs:
        if s.name == "MAGIC":
            continue
        uc.mem_write(s.addr, s.data if s.data else b"\0" * s.size)
    uc.mem_map(STACK, STACK_SZ)
    uc.mem_map(RET_TRAP, 0x1000)
    uc.mem_write(RET_TRAP, b"\x73\x00\x10\x00")   # ebreak

    state = {"threads": [], "files": {}, "log": [], "stop_after_sleeps": 3,
             "sleeps": 0}

    def on_intr(uc, intno, _):
        pc = uc.reg_read(UC_RISCV_REG_PC)
        if pc in (RET_TRAP, RET_TRAP + 4):       # returned to our trap
            uc.emu_stop()
            return
        a = [uc.reg_read(r) for r in (UC_RISCV_REG_A0, UC_RISCV_REG_A1,
                                      UC_RISCV_REG_A2, UC_RISCV_REG_A3)]
        n = uc.reg_read(UC_RISCV_REG_A7)
        name = NAMES.get(n, f"UNKNOWN({n:#x})")
        ret = 0
        if n == 0x10000003:
            detail = repr(cstr(uc, a[0]))
        elif n == 0x10000100:
            state["threads"].append((a[0], a[1]))
            detail = f"entry={a[0]:#x} stack={a[2]:#x} prio={a[3]:#06x}"
            ret = 1
        elif n == 0x1000010C:
            state["sleeps"] += 1
            detail = f"{a[0]} ticks"
            if state["sleeps"] >= state["stop_after_sleeps"]:
                uc.emu_stop()
        elif n == 0x10000308:
            p = cstr(uc, a[0])
            detail = f"{p!r}, {cstr(uc, a[1])!r}"
            if p[:1].upper() == mount:
                ret = FAKE_FP
                state["files"][p] = b""
                state["open"] = p
        elif n == 0x10000315:
            data = bytes(uc.mem_read(a[0], a[1] * a[2]))
            state["files"][state["open"]] += data
            detail = f"{a[1] * a[2]} bytes"
            ret = a[2]
        else:
            detail = " ".join(hex(x) for x in a)
        state["log"].append(f"  ecall {name}({detail}) -> {ret:#x}")
        uc.reg_write(UC_RISCV_REG_A0, ret)
        # Unicorn may report the PC at or after the ecall; resume just past it.
        if bytes(uc.mem_read(pc, 4)) == b"\x73\x00\x00\x00":
            uc.reg_write(UC_RISCV_REG_PC, pc + 4)

    uc.hook_add(UC_HOOK_INTR, on_intr)

    def call(fn, *args):
        uc.reg_write(UC_RISCV_REG_SP, STACK + STACK_SZ - 0x100)
        uc.reg_write(UC_RISCV_REG_RA, RET_TRAP)
        for reg, v in zip((UC_RISCV_REG_A0, UC_RISCV_REG_A1), args):
            uc.reg_write(reg, v)
        uc.emu_start(fn, RET_TRAP, count=200000)
        return uc.reg_read(UC_RISCV_REG_A0)

    print(f"{path}: type {info['type']:#04x} @ {info['base']:#x}, "
          f"{len(secs)} sections")
    mid = 96 + info["type"]
    r = call(mif["MInit"])
    print(f"MInit() -> {r:#x}")
    print("\n".join(state["log"])); state["log"].clear()
    r = call(mif["MOpen"], mid, 0)
    print(f"MOpen({mid}, 0) -> {r:#x}")
    print("\n".join(state["log"])); state["log"].clear()
    for entry, arg in state["threads"]:
        print(f"thread {entry:#x} (USB stick mounted as {mount}:)")
        call(entry, arg)
        print("\n".join(state["log"])); state["log"].clear()
    for p, data in state["files"].items():
        print(f"file {p}: {len(data)} bytes\n    " +
              data.decode("latin1").replace("\r\n", "\n    "))
    ok = r != 0 and bool(state["files"])
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "hostmod/build/hello.mod"))
