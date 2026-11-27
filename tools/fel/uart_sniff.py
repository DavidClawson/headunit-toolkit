#!/usr/bin/env python3
"""uart_sniff.py: listen to a UART RX line over FEL without knowing the baud rate.

Loads edges.bin (a GPIO edge timestamper), sets the RX pin to a plain input with
pull-up, records every level change for a couple of seconds, then estimates the
baud rate from the shortest pulses and decodes 8N1 bytes on the Mac. Read-only:
nothing is driven onto the line.

    tools/fel/uart_sniff.py calibrate          # measure the CPU clock in FEL
    tools/fel/uart_sniff.py listen uart1       # MCU link   (RX = PG13)
    tools/fel/uart_sniff.py listen uart4       # BT link    (RX = PB03)
    tools/fel/uart_sniff.py listen uart1 --seconds 5 --baud 115200

Pins from sys_config_nor.fex [uart1]/[uart4]; ports from Config.ini
(mcuSerialName=/dev/uart1, btSerialName=/dev/uart4).
"""
import argparse, bisect, os, struct, subprocess, sys, tempfile, time

XFEL = os.environ.get("XFEL", os.path.expanduser("~/src/xfel/xfel"))
HERE = os.path.dirname(os.path.abspath(__file__))
LOAD, REQ, EDGES = 0x40000, 0x40F00, 0x41000
GPIO = 0x02000000
PORTS = {"B": 1, "G": 6}
# name: (port, pin) of the SoC's RX pin (= the other chip's TX)
LINES = {"uart1": ("G", 13), "uart4": ("B", 3)}
BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000]
CAL_FILE = os.path.join(HERE, ".cpu_hz")


def xfel(*args):
    r = subprocess.run([XFEL, *map(str, args)], capture_output=True, text=True)
    if r.returncode != 0 or "error" in (r.stdout + r.stderr).lower():
        sys.exit(f"xfel {' '.join(map(str, args))} failed: {r.stdout}{r.stderr}")
    return r.stdout


def rd32(a):
    return int(xfel("read32", hex(a)).strip(), 16)


def wr32(a, v):
    xfel("write32", hex(a), hex(v & 0xFFFFFFFF))


def regs(port, pin):
    base = GPIO + 0x30 * PORTS[port]
    return (base + 4 * (pin // 8), (pin % 8) * 4,      # CFG reg, shift
            base + 0x10,                               # DAT
            base + 0x24 + 4 * (pin // 16), (pin % 16) * 2)  # PULL reg, shift


def as_input(port, pin):
    cfg, sh, dat, pull, psh = regs(port, pin)
    wr32(cfg, rd32(cfg) & ~(0xF << sh))                      # function 0 = input
    wr32(pull, (rd32(pull) & ~(3 << psh)) | (1 << psh))      # pull-up
    return dat


def capture(dat, bit, cycles, max_edges=4096):
    with tempfile.TemporaryDirectory() as t:
        f = os.path.join(t, "r.bin")
        open(f, "wb").write(struct.pack("<8I", dat, bit, max_edges, cycles, 0xEEEE, 0, 0, 0))
        xfel("write", hex(REQ), f)
        t0 = time.perf_counter()
        xfel("exec", hex(LOAD))
        xfel("read", hex(REQ), 32, f)      # FEL answers only once the payload returns
        wall = time.perf_counter() - t0
        _, _, _, _, n, lvl, elapsed, polls = struct.unpack("<8I", open(f, "rb").read())
        if n == 0xEEEE:
            sys.exit("payload did not run")
        edges = []
        if n:
            xfel("read", hex(EDGES), 4 * n, f)
            edges = list(struct.unpack(f"<{n}I", open(f, "rb").read()))
    return lvl, edges, elapsed, polls, wall


def load():
    xfel("write", hex(LOAD), os.path.join(HERE, "edges.bin"))


def calibrate():
    load()
    dat = as_input("G", 13)
    _, _, _, _, w0 = capture(dat, 13, 1000)                  # USB/exec overhead
    big = 1_000_000_000
    _, _, el, polls, w1 = capture(dat, 13, big, max_edges=0xFFFFFFFF)
    hz = el / max(w1 - w0, 1e-3)
    open(CAL_FILE, "w").write(f"{hz:.0f}\n")
    print(f"CPU ≈ {hz/1e6:.1f} MHz  ({el} cycles in {w1 - w0:.2f} s; "
          f"one poll ≈ {el/polls:.0f} cycles = {el/polls/hz*1e6:.2f} µs)")
    return hz


def decode(lvl0, edges, cyc_per_bit):
    """8N1 decode from edge timestamps (cycles)."""
    def level(t):
        i = bisect.bisect_right(edges, t)
        return lvl0 ^ (i & 1)
    out, i = [], 0
    while i < len(edges):
        t = edges[i]
        if level(t) != 0 or level(t - 1) != 1:    # need a falling edge from idle high
            i += 1
            continue
        bits = [level(t + cyc_per_bit * (1.5 + k)) for k in range(8)]
        stop = level(t + cyc_per_bit * 9.5)
        out.append((sum(b << k for k, b in enumerate(bits)), stop == 1))
        end = t + cyc_per_bit * 9.6
        i = bisect.bisect_right(edges, end)
    return out


def listen(name, seconds, baud):
    hz = float(open(CAL_FILE).read()) if os.path.exists(CAL_FILE) else calibrate()
    load()
    port, pin = LINES[name]
    dat = as_input(port, pin)
    cycles = min(int(seconds * hz), 0xFFFFFFF0)
    lvl, edges, el, polls, _ = capture(dat, pin, cycles)
    res_us = el / max(polls, 1) / hz * 1e6
    print(f"{name} RX = P{port}{pin}: idle level {lvl}, {len(edges)} edges in "
          f"{el/hz:.2f} s (sampling every ~{res_us:.2f} µs)")
    if len(edges) < 4:
        print("  no traffic" + (" (line held low: chip off or in reset?)" if lvl == 0 and not edges else ""))
        return
    widths = sorted(b - a for a, b in zip(edges, edges[1:]))
    floor = widths[max(0, len(widths) // 50)]                 # ~2nd percentile, ignores glitches
    est = hz / floor
    if not baud:
        baud = min(BAUDS, key=lambda b: abs(b - est) / b)
    print(f"  shortest pulses ≈ {floor/hz*1e6:.1f} µs → ~{est:.0f} baud; decoding at {baud}")
    data = decode(lvl, edges, hz / baud)
    bad = sum(1 for _, ok in data if not ok)
    raw = bytes(b for b, _ in data)
    print(f"  {len(raw)} bytes, {bad} framing errors")
    for off in range(0, min(len(raw), 512), 16):
        chunk = raw[off:off + 16]
        print(f"  {off:04x}: {chunk.hex(' '):<48} {''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["calibrate", "listen"])
    p.add_argument("line", nargs="?", choices=sorted(LINES))
    p.add_argument("--seconds", type=float, default=3)
    p.add_argument("--baud", type=int)
    a = p.parse_args()
    if a.cmd == "calibrate":
        calibrate()
    else:
        if not a.line:
            p.error("which line? uart1 or uart4")
        listen(a.line, a.seconds, a.baud)


if __name__ == "__main__":
    main()
