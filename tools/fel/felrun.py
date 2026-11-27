#!/usr/bin/env python3
"""felrun.py: run a DRAM program (tools/fel/dram/*.bin) over FEL and print its log.

    tools/fel/felrun.py dram/memtest.bin
    tools/fel/felrun.py dram/memtest.bin --ddr     # force DRAM re-init

DRAM is set up with `xfel ddr f133` once per power-up (the board's sys_config
[dram_para] matches xfel's F133 table exactly). A marker in SRAM records that
it's been done; it disappears when the unit loses power.
The program's log (lib.h logf) lives at 0x43FF0000: "FLOG", u32 length, text.
"""
import argparse, os, struct, subprocess, sys, tempfile, time

XFEL = os.environ.get("XFEL", os.path.expanduser("~/src/xfel/xfel"))
LOAD, LOG = 0x40000000, 0x43FF0000
MARK, MARK_VAL = 0x44F80, 0xD0D0F133


def xfel(*args, check=True):
    r = subprocess.run([XFEL, *map(str, args)], capture_output=True, text=True)
    bad = r.returncode != 0 or "error" in (r.stdout + r.stderr).lower()
    if bad and check:
        sys.exit(f"xfel {' '.join(map(str, args))} failed: {r.stdout}{r.stderr}")
    return None if bad else r.stdout


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("program")
    p.add_argument("--ddr", action="store_true", help="re-run DRAM init even if marked done")
    p.add_argument("--timeout", type=float, default=120)
    a = p.parse_args()

    print(xfel("version").strip())
    if a.ddr or int(xfel("read32", hex(MARK)).strip(), 16) != MARK_VAL:
        print("DRAM init (xfel ddr f133)...")
        out = xfel("ddr", "f133")
        print("  " + (out.strip().replace("\n", "\n  ") if out.strip() else "done"))
        xfel("write32", hex(MARK), hex(MARK_VAL))

    xfel("write", hex(LOAD), a.program)
    t0 = time.perf_counter()
    xfel("exec", hex(LOAD))
    with tempfile.TemporaryDirectory() as t:
        f = os.path.join(t, "log.bin")
        # FEL answers again only once the program returns; keep retrying the read.
        while xfel("read", hex(LOG), 8, f, check=False) is None:
            if time.perf_counter() - t0 > a.timeout:
                sys.exit("no answer from FEL: program hung? power-cycle the unit")
            time.sleep(0.5)
        magic, n = struct.unpack("<II", open(f, "rb").read())
        dt = time.perf_counter() - t0
        if magic != 0x474F4C46:
            sys.exit(f"program returned after {dt:.1f} s but wrote no log (magic {magic:#x})")
        text = b""
        if n:
            xfel("read", hex(LOG + 8), n, f)
            text = open(f, "rb").read()
    print(f"--- log ({n} bytes, {dt:.1f} s) ---")
    print(text.decode(errors="replace"), end="")


if __name__ == "__main__":
    main()
