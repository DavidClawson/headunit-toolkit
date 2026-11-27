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
MARK, MARK_VAL = 0x40FF0, 0xD0D0F133   # NOT near 0x44FFC: that is the BROM FEL stack


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
    p.add_argument("--usb-serial", type=float, metavar="SECONDS",
                   help="the program takes over USB as a serial port (dram/usbtest.c): talk to it "
                        "for SECONDS instead of reading the log over FEL (FEL is gone afterwards)")
    p.add_argument("--detach", action="store_true",
                   help="start the program and exit (it takes over USB; results show on the unit's screen)")
    p.add_argument("--usb-net", type=float, metavar="SECONDS",
                   help="the program takes over USB as a network adapter (dram/ncmtest.c): find the "
                        "Mac's new interface, wait for its address and ping the unit")
    p.add_argument("--load", action="append", default=[], metavar="ADDR:FILE",
                   help="write FILE to DRAM at ADDR before running (repeatable)")
    a = p.parse_args()

    print(xfel("version").strip())
    if a.ddr or int(xfel("read32", hex(MARK)).strip(), 16) != MARK_VAL:
        print("DRAM init (xfel ddr f133)...")
        out = xfel("ddr", "f133")
        print("  " + (out.strip().replace("\n", "\n  ") if out.strip() else "done"))
        xfel("write32", hex(MARK), hex(MARK_VAL))

    for spec in a.load:
        addr, path = spec.split(":", 1)
        print(f"loading {path} ({os.path.getsize(path)} bytes) at {addr}")
        xfel("write", addr, path)
    xfel("write", hex(LOAD), a.program)
    t0 = time.perf_counter()
    before = subprocess.run(["ifconfig", "-l"], capture_output=True, text=True).stdout.split()
    xfel("exec", hex(LOAD))
    if a.detach:
        print("started; FEL is no longer needed (the program owns USB now)")
        return
    if a.usb_net:
        return usb_net(a.usb_net, before)
    if a.usb_serial:
        return usb_serial(a.usb_serial)
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


def usb_net(seconds, before, unit="192.168.133.1"):
    """Wait for the program's USB network interface and its DHCP address, then ping the unit."""
    t0 = time.perf_counter()
    iface = None
    while time.perf_counter() - t0 < 25 and not iface:
        new = [i for i in subprocess.run(["ifconfig", "-l"], capture_output=True, text=True).stdout.split() if i not in before]
        iface = new[0] if new else None
        time.sleep(0.3)
    if not iface:
        r = subprocess.run(["ioreg", "-p", "IOUSB", "-w0"], capture_output=True, text=True).stdout
        print("no new network interface after 25 s. USB devices (ioreg):")
        print("\n".join(l for l in r.splitlines() if "@" in l)[:3000])
        sys.exit(1)
    print(f"new interface {iface} after {time.perf_counter() - t0:.1f} s")
    addr = None
    while time.perf_counter() - t0 < 60 and not addr:
        out = subprocess.run(["ifconfig", iface], capture_output=True, text=True).stdout
        for l in out.splitlines():
            l = l.strip()
            if l.startswith("inet "):
                addr = l.split()[1]
        time.sleep(0.5)
    print(subprocess.run(["ifconfig", iface], capture_output=True, text=True).stdout.rstrip())
    if not addr:
        sys.exit(f"{iface} got no IPv4 address in 60 s (DHCP failed?)")
    print(f"{iface} has {addr} after {time.perf_counter() - t0:.1f} s; pinging the unit at {unit}")
    r = subprocess.run(["ping", "-c", "10", "-i", "0.5", "-b", iface, unit], capture_output=True, text=True)
    print(r.stdout.rstrip() or r.stderr.rstrip())
    for size in (1000, 1400, 1472):                     # 1472 = a full 1500-byte IP packet
        r = subprocess.run(["ping", "-c", "3", "-s", str(size), "-b", iface, unit], capture_output=True, text=True)
        print(f"{size}-byte pings:", (r.stdout.strip().splitlines() or ["?"])[-2])


def usb_serial(seconds):
    """Wait for the program's USB serial port, then show what it says and echo a test line."""
    import glob, serial
    t0 = time.perf_counter()
    port = None
    while time.perf_counter() - t0 < 20:
        ports = glob.glob("/dev/cu.usbmodem*")
        if ports:
            port = ports[0]
            break
        time.sleep(0.2)
    if not port:
        r = subprocess.run(["ioreg", "-p", "IOUSB", "-w0"], capture_output=True, text=True).stdout
        print("no /dev/cu.usbmodem* after 20 s. USB devices the Mac sees (ioreg):")
        print(r.strip()[:3000])
        sys.exit(1)
    print(f"serial port {port} appeared after {time.perf_counter() - t0:.1f} s")
    r = subprocess.run(["ioreg", "-p", "IOUSB", "-w0"], capture_output=True, text=True).stdout
    print("\n".join(l for l in r.splitlines() if "Head Unit" in l or "1209" in l))
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True
    end = time.perf_counter() + seconds
    sent = False
    while time.perf_counter() < end:
        line = s.readline()
        if line:
            print("  <", line.decode(errors="replace").rstrip())
        if not sent and time.perf_counter() > end - seconds + 3:
            s.write(b"ping from the mac\r\n")
            print("  > ping from the mac")
            sent = True
    s.close()


if __name__ == "__main__":
    main()
