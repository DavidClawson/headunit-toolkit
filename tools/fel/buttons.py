#!/usr/bin/env python3
"""buttons.py: find out how the front-panel buttons are read, over FEL.

    tools/fel/buttons.py watch [--seconds 60] [--no-touch] [--all-inputs]

--all-inputs: pins still in the reset "I/O disabled" state (CFG 0xF) read 0 in
DAT whatever the pin does, so a button there is invisible. This switches every
such pin (ports B, D, E, G; not C = SPI flash, not F = JTAG/debug) to input
with pull-up for the watch, and restores CFG and PULL afterwards.

Polls, and prints every change with a timestamp:
  - LRADC (the F133 "KEYADC" pin, 0x02009800): 6-bit level; resistor-ladder keys
  - GPIO input levels of ports B-G (DAT registers): direct-wired keys
  - GT911 touch (TWI3 0x5d): touches, and its HaveKey flag (bit 4 of 0x814E)
    with the key byte at 0x8093, for touch keys outside the display
Read-only apart from enabling the LRADC clock and clearing the GT911 status.
"""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i2c   # noqa: E402  (xfel helpers + GT911 over TWI3)

LRADC_BGR, LRADC = 0x02001A9C, 0x02009800
PORTS = {"B": 1, "C": 2, "D": 3, "E": 4, "F": 5, "G": 6}   # no port A on F133


def lradc_init():
    i2c.wr32(LRADC_BGR, (1 << 16) | 1)                       # de-assert reset, gate on
    # first-convert delay 2, hold enable, 250 Hz sample rate, enable
    i2c.wr32(LRADC + 0x00, (2 << 24) | (1 << 6) | (0 << 2) | 1)


SCAN_PORTS = "BDEG"


def open_inputs():
    """Disabled pins -> input + pull-up. Returns what to restore."""
    saved, opened = [], {}
    for p in SCAN_PORTS:
        base = 0x02000000 + 0x30 * PORTS[p]
        for r in range(4):
            cfg = i2c.rd32(base + 4 * r)
            new = cfg
            for k in range(8):
                if (cfg >> (4 * k)) & 0xF == 0xF:
                    new &= ~(0xF << (4 * k))
                    opened.setdefault(p, []).append(8 * r + k)
            if new != cfg:
                saved.append((base + 4 * r, cfg))
                i2c.wr32(base + 4 * r, new)
        for r in range(2):
            pull = i2c.rd32(base + 0x24 + 4 * r)
            new = pull
            for pin in opened.get(p, []):
                if pin // 16 == r:
                    new = (new & ~(3 << (2 * (pin % 16)))) | (1 << (2 * (pin % 16)))
            if new != pull:
                saved.append((base + 0x24 + 4 * r, pull))
                i2c.wr32(base + 0x24 + 4 * r, new)
    print("opened as inputs: " + "  ".join(f"P{p}{','.join(map(str, v))}" for p, v in opened.items()))
    return saved


def snapshot():
    s = {"LRADC": i2c.rd32(LRADC + 0x0C) & 0x3F}
    for p, n in PORTS.items():
        s["P" + p] = i2c.rd32(0x02000000 + 0x30 * n + 0x10)
    return s


def touch_events():
    st = i2c.read(3, 0x5D, 0x814E, 1)[0]
    out = []
    if st & 0x80:
        k = st & 0x0F
        if st & 0x10:
            out.append(f"GT911 key flag, 0x8093 = {i2c.read(3, 0x5D, 0x8093, 1)[0]:#04x}")
        if k:
            pts = i2c.read(3, 0x5D, 0x814F, 8 * k)
            for i in range(k):
                tid, x, y, size = struct.unpack("<BHHH", pts[8 * i: 8 * i + 7])
                out.append(f"touch id {tid}: x={x} y={y} size={size}")
        i2c.write(3, 0x5D, 0x814E, [0])
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["watch"])
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--no-touch", action="store_true")
    p.add_argument("--all-inputs", action="store_true")
    a = p.parse_args()
    saved = open_inputs() if a.all_inputs else []
    try:
        watch(a)
    finally:
        for reg, val in reversed(saved):
            i2c.wr32(reg, val)
        if saved:
            print("pin config restored")


def watch(a):

    lradc_init()
    if not a.no_touch:
        i2c.gt911_reset(0x5D)                                # PG08 high = touch running
        i2c.scan(3)
    base = snapshot()
    print("baseline: " + "  ".join(f"{k}={v:#x}" for k, v in base.items()), flush=True)
    t0 = time.time()
    while time.time() - t0 < a.seconds:
        now = snapshot()
        ts = f"{time.time() - t0:6.2f}s"
        for k, v in now.items():
            if v != base[k]:
                if k == "LRADC":
                    print(f"{ts} LRADC {base[k]:2d} -> {v:2d}", flush=True)
                else:
                    ch = v ^ base[k]
                    pins = ", ".join(f"{k}{b}={(v >> b) & 1}" for b in range(32) if ch >> b & 1)
                    print(f"{ts} {pins}", flush=True)
        base = now
        if not a.no_touch:
            for e in touch_events():
                print(f"{ts} {e}", flush=True)


if __name__ == "__main__":
    main()
