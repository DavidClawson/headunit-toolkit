#!/bin/bash
# fw-write.sh: put a firmware image (dram/mkfw.py output) into its NOR slot
# at 0x120000 over FEL, and verify it. The unit must be caught in FEL.
#
#   tools/fel/fw-write.sh dram/app.fw
#
# Checks the image as the boot0 selector will, shows what the slot held
# before, writes, then reads the whole 64 KB slot back: the image, then 0xFF.
set -e
cd "$(dirname "$0")"
X=~/src/xfel/xfel
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
SLOT=0x120000
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "image: $(./dram/mkfw.py --check "$IMG")"
# the link can drop for a moment right after a catch: wait for 3 good answers in a row
ok=0
for i in $(seq 20); do
  if $X version 2>/dev/null | grep -q AWUSBFEX; then ok=$((ok + 1)); [ $ok -ge 3 ] && break; else ok=0; fi
  sleep 0.5
done
[ $ok -ge 3 ] || { echo "FEL link not stable; not writing"; exit 1; }
$X spinor | grep -q "Found spi nor" || { sleep 1; $X spinor | grep -q "Found spi nor"; } || { echo "flash not detected; not writing"; exit 1; }
echo "flash: $($X spinor)"
$X spinor read $SLOT 0x10000 "$T/before.bin" >/dev/null
python3 - "$T/before.bin" <<'PY'
import sys
b = open(sys.argv[1], "rb").read()
n = len(b.rstrip(b"\xff"))
print("slot before: blank" if n == 0 else f"slot before: {n} bytes in use, magic {b[:4]!r}")
PY
$X spinor write $SLOT "$IMG"
$X spinor read $SLOT 0x10000 "$T/after.bin" >/dev/null
python3 - "$IMG" "$T/after.bin" <<'PY'
import sys
img, got = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
if got[:len(img)] != img:
    bad = next(i for i in range(len(img)) if got[i] != img[i])
    sys.exit(f"VERIFY FAILED: first difference at slot offset {bad:#x}")
if got[len(img):] != b"\xff" * (len(got) - len(img)):
    sys.exit("VERIFY FAILED: data after the image (slot not blank)")
print(f"verified: {len(img)} bytes at 0x120000, rest of the slot blank")
PY
