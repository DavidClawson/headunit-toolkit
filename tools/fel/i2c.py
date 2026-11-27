#!/usr/bin/env python3
"""i2c.py: talk I2C on the W022 over FEL, using the i2c.bin SRAM payload.

    tools/fel/i2c.py scan  --bus N
    tools/fel/i2c.py read  ADDR REG LEN --bus N [--reglen 2]
    tools/fel/i2c.py write ADDR REG BYTE... --bus N [--reglen 2]
    tools/fel/i2c.py gt911-id                     # Goodix product ID at 0x8140 (bus 3)
    tools/fel/i2c.py gt911-touch [--seconds 20]   # print touches live
    tools/fel/i2c.py gt911-reset [--addr 0x5d]    # reset sequence selecting the address

Buses (sys_config_nor.fex + Config.ini), all verified on the bench unit:
  TWI1  PG14 SCL / PB04 SDA (func 4)  radio tuner (radioI2cName); empty in FEL
  TWI2  PE12 SCL / PE13 SDA (func 2)  0x10 Apple MFi auth, 0x40 sound processor,
                                      0x44 Techpoint TP2850 camera decoder
  TWI3  PG10 SCL / PG11 SDA (func 3)  0x5d Goodix GT911 touch
Touch INT = PG07, touch reset/wakeup = PG08 (high = run).
--bus is required for scan/read/write: it's easy to probe the wrong bus.
Every step shells out to xfel, so it's slow (tens of ms per call) but simple.
"""
import argparse, os, struct, subprocess, sys, tempfile, time

XFEL = os.environ.get("XFEL", os.path.expanduser("~/src/xfel/xfel"))
HERE = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(HERE, "i2c.bin")
LOAD, REQ = 0x40000, 0x40F00
REQ_FMT = "<8I128s"
ERRORS = {0: "ok", 1: "timeout", 2: "no START", 3: "address NACK", 4: "data NACK", 5: "bad args"}

# TWI pin mux per bus: (port CFG reg, PULL reg, [(pin, cfg shift, pull shift)], function)
PG_CFG0, PG_CFG1, PG_DAT, PG_PULL0 = 0x02000120, 0x02000124, 0x02000130, 0x02000144
PE_CFG1, PE_PULL0 = 0x020000C4, 0x020000E4
PB_CFG0, PB_PULL0 = 0x02000030, 0x02000054
PINMUX = {
    1: [(PG_CFG1, 24, 4, PG_PULL0, 28), (PB_CFG0, 16, 4, PB_PULL0, 8)],   # PG14 SCL, PB04 SDA
    3: [(PG_CFG1, 8, 3, PG_PULL0, 20), (PG_CFG1, 12, 3, PG_PULL0, 22)],   # PG10, PG11
    2: [(PE_CFG1, 16, 2, PE_PULL0, 24), (PE_CFG1, 20, 2, PE_PULL0, 26)],  # PE12, PE13
}


def xfel(*args, capture=False):
    r = subprocess.run([XFEL, *map(str, args)], capture_output=True, text=True)
    if r.returncode != 0 or "ERROR" in r.stdout.upper():
        sys.exit(f"xfel {' '.join(map(str, args))} failed: {r.stdout}{r.stderr}")
    return r.stdout


def rd32(a):
    return int(xfel("read32", hex(a)).strip(), 16)


def wr32(a, v):
    xfel("write32", hex(a), hex(v & 0xFFFFFFFF))


def rmw(a, clear, set_):
    wr32(a, (rd32(a) & ~clear) | set_)


def pinmux(bus):
    for cfg, shift, func, pull, pshift in PINMUX.get(bus, []):
        rmw(cfg, 0xF << shift, func << shift)
        rmw(pull, 3 << pshift, 1 << pshift)


_loaded = False


def run(cmd, bus, addr=0, reg=0, reglen=0, length=0, data=b""):
    global _loaded
    if not _loaded:
        xfel("write", hex(LOAD), PAYLOAD)
        pinmux(bus)
        _loaded = True
    with tempfile.TemporaryDirectory() as t:
        f = os.path.join(t, "req.bin")
        open(f, "wb").write(struct.pack(REQ_FMT, cmd, bus, addr, reg, reglen, length,
                                        0xEE, 0, data.ljust(128, b"\0")))
        xfel("write", hex(REQ), f)
        xfel("exec", hex(LOAD))
        xfel("read", hex(REQ), struct.calcsize(REQ_FMT), f)
        *_, status, stat, out = struct.unpack(REQ_FMT, open(f, "rb").read())
    if status == 0xEE:
        sys.exit("payload did not run (status untouched)")
    return status, stat, out


def scan(bus):
    st, stat, out = run(1, bus)
    found = [a for a in range(128) if out[a]]
    print(f"TWI{bus}: {ERRORS.get(st, st)} (STAT=0x{stat:02x}); devices: "
          + (" ".join(f"0x{a:02x}" for a in found) or "none"))
    return found


def read(bus, addr, reg, length, reglen=2):
    st, stat, out = run(2, bus, addr, reg, reglen, length)
    if st:
        raise IOError(f"read 0x{addr:02x}/0x{reg:x}: {ERRORS.get(st, st)} (STAT=0x{stat:02x})")
    return out[:length]


def write(bus, addr, reg, data, reglen=2):
    st, stat, _ = run(3, bus, addr, reg, reglen, len(data), bytes(data))
    if st:
        raise IOError(f"write 0x{addr:02x}/0x{reg:x}: {ERRORS.get(st, st)} (STAT=0x{stat:02x})")


def gt911_reset(addr):
    """Goodix reset: INT level while RESET rises selects 0x5d (low) or 0x14 (high)."""
    int_level = 0 if addr == 0x5D else 1
    rmw(PG_CFG0, 0xF << 28, 1 << 28)            # PG07 output
    rmw(PG_CFG1, 0xF << 0, 1 << 0)              # PG08 output
    rmw(PG_DAT, (1 << 8) | (1 << 7), int_level << 7)   # RESET low, INT = level
    time.sleep(0.02)
    rmw(PG_DAT, 0, 1 << 8)                      # RESET high
    time.sleep(0.01)
    rmw(PG_CFG0, 0xF << 28, 0)                  # PG07 back to input
    time.sleep(0.06)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd")
    p.add_argument("args", nargs="*")
    p.add_argument("--bus", type=int, choices=sorted(PINMUX))
    p.add_argument("--reglen", type=int, default=2)
    p.add_argument("--addr", type=lambda s: int(s, 0), default=0x5D)
    p.add_argument("--seconds", type=float, default=20)
    a = p.parse_args()
    n = [int(x, 0) for x in a.args]
    if a.bus is None:
        if a.cmd.startswith("gt911"):
            a.bus = 3
        else:
            p.error("--bus is required (1 = tuner, 2 = MFi/audio/camera, 3 = touch)")

    if a.cmd == "scan":
        scan(a.bus)
    elif a.cmd == "read":
        print(read(a.bus, n[0], n[1], n[2], a.reglen).hex(" "))
    elif a.cmd == "write":
        write(a.bus, n[0], n[1], n[2:], a.reglen)
        print("ok")
    elif a.cmd == "gt911-reset":
        gt911_reset(a.addr)
        scan(a.bus)
    elif a.cmd == "gt911-id":
        d = read(a.bus, a.addr, 0x8140, 11)
        pid = d[0:4].rstrip(b"\0").decode("ascii", "replace")
        fw, xr, yr = struct.unpack("<HHH", d[4:10])
        print(f"product '{pid}', firmware 0x{fw:04x}, resolution {xr} x {yr}, vendor 0x{d[10]:02x}")
    elif a.cmd == "gt911-touch":
        end = time.time() + a.seconds
        print(f"touch the screen ({a.seconds:.0f} s)...")
        while time.time() < end:
            st = read(a.bus, a.addr, 0x814E, 1)[0]
            if st & 0x80:
                k = st & 0x0F
                if k:
                    pts = read(a.bus, a.addr, 0x814F, 8 * k)
                    for i in range(k):
                        tid, x, y, size = struct.unpack("<BHHH", pts[8 * i: 8 * i + 7])
                        print(f"  id {tid}: x={x:4d} y={y:4d} size={size}")
                write(a.bus, a.addr, 0x814E, [0])
    else:
        p.error(f"unknown command {a.cmd}")


if __name__ == "__main__":
    main()
