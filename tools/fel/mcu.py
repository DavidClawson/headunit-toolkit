#!/usr/bin/env python3
"""mcu.py: talk to the MCU over UART1 from FEL (frames per docs/mcu-protocol.md).

    tools/fel/mcu.py listen [--seconds 10]
    tools/fel/mcu.py send A0 [DATA...] [--seconds 10]   # sends 02 80 CMD LEN DATA XOR

Frames are 02 | 80 | CMD | LEN | DATA | XOR(80..DATA). Received bytes are
split into frames and printed with their arrival time.
Pins: PG12 TX / PG13 RX, function 7 (sys_config [uart1]); 115200 baud.
"""
import argparse, os, struct, subprocess, sys, tempfile

XFEL = os.environ.get("XFEL", os.path.expanduser("~/src/xfel/xfel"))
HERE = os.path.dirname(os.path.abspath(__file__))
LOAD, REQ, RX = 0x40000, 0x40F00, 0x41000
PG_CFG1, PG_PULL0 = 0x02000124, 0x02000144
FMT = "<8I64s"


def xfel(*args):
    r = subprocess.run([XFEL, *map(str, args)], capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or "error" in (r.stdout + r.stderr).lower():
        sys.exit(f"xfel {' '.join(map(str, args))} failed: {r.stdout}{r.stderr}")
    return r.stdout


def rmw(a, clear, set_):
    v = int(xfel("read32", hex(a)).strip(), 16)
    xfel("write32", hex(a), hex((v & ~clear) | set_))


def frame(cmd, data=b""):
    body = bytes([0x80, cmd, len(data)]) + bytes(data)
    x = 0
    for b in body:
        x ^= b
    return b"\x02" + body + bytes([x])


def split(stream):
    """[(ms, byte)] -> [(ms, frame bytes, ok)] using 02 B1 CMD LEN ... XOR."""
    out, i = [], 0
    while i < len(stream):
        if stream[i][1] != 0x02 or i + 4 > len(stream):
            out.append((stream[i][0], bytes([stream[i][1]]), None)); i += 1; continue
        n = stream[i + 3][1] + 5
        fr = bytes(b for _, b in stream[i:i + n])
        x = 0
        for b in fr[1:-1]:
            x ^= b
        out.append((stream[i][0], fr, len(fr) == n and x == fr[-1]))
        i += n
    return out


def run(tx, seconds):
    rmw(PG_CFG1, 0xFF << 16, 0x77 << 16)                     # PG12/PG13 -> UART1
    rmw(PG_PULL0, 0xF << 24, 0x5 << 24)                       # pull-ups
    xfel("write", hex(LOAD), os.path.join(HERE, "uart.bin"))
    with tempfile.TemporaryDirectory() as t:
        f = os.path.join(t, "r.bin")
        open(f, "wb").write(struct.pack(FMT, 1, 13, len(tx), int(seconds * 1000), 0, 0xEE, 0, 0, tx.ljust(64, b"\0")))
        xfel("write", hex(REQ), f)
        xfel("exec", hex(LOAD))
        # FEL answers only once the payload returns; long listens outlast
        # xfel's USB timeout, so retry the read until it succeeds.
        import time
        end = time.time() + seconds + 30
        while True:
            r = subprocess.run([XFEL, "read", hex(REQ), str(struct.calcsize(FMT)), f], capture_output=True, text=True)
            if r.returncode == 0 and "error" not in (r.stdout + r.stderr).lower():
                break
            if time.time() > end:
                sys.exit("no answer from FEL after the listen: unit reset or hung?")
            time.sleep(0.5)
        _, _, _, _, n, status, *_ = struct.unpack(FMT, open(f, "rb").read())
        if status == 0xEE:
            sys.exit("payload did not run")
        ent = []
        if n:
            xfel("read", hex(RX), 4 * n, f)
            ent = [(e >> 8, e & 0xFF) for e in struct.unpack(f"<{n}I", open(f, "rb").read())]
    return ent


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["listen", "send"])
    p.add_argument("args", nargs="*")
    p.add_argument("--seconds", type=float, default=10)
    a = p.parse_args()
    tx = b""
    if a.cmd == "send":
        b = [int(x, 16) for x in a.args]
        tx = frame(b[0], bytes(b[1:]))
        print("TX " + tx.hex(" "))
    ent = run(tx, a.seconds)
    print(f"RX {len(ent)} bytes in {a.seconds:.0f} s")
    for ms, fr, ok in split(ent):
        tag = "" if ok else ("  (bad checksum/length)" if ok is False else "  (stray)")
        print(f"  {ms:6d} ms  {fr.hex(' ')}{tag}")


if __name__ == "__main__":
    main()
