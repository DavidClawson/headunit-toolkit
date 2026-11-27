#!/bin/bash
# fw-write.sh: put a firmware image (dram/mkfw.py output) into its NOR slot
# at 0x120000 over FEL, and verify it. The unit must be caught in FEL.
#
#   tools/fel/fw-write.sh dram/app.fw
#   ALLOW_BAD=1 tools/fel/fw-write.sh bad.fw     # deliberately invalid, to test the fallback
#
# Checks the image as the boot0 selector will, shows what the slot held
# before, writes, then reads the whole 64 KB slot back: the image, then 0xFF up
# to 0x12F000. The slot's last 4 KB (0x12F000) holds our settings: it is never
# erased here, and the write is refused if that sector changed.
set -e
cd "$(dirname "$0")"
X=~/src/xfel/xfel
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
SLOT=0x120000
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

if CHK=$(./dram/mkfw.py --check "$IMG"); then
  echo "image: $CHK"
elif [ "$ALLOW_BAD" = 1 ]; then
  echo "image: $CHK -- writing anyway (ALLOW_BAD=1, fallback test)"
else
  echo "image fails the selector's checks ($CHK): not writing"; exit 1
fi
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
[ $(stat -f%z "$IMG") -le $((0xF000)) ] || { echo "image overlaps the settings sector; not writing"; exit 1; }
$X spinor erase $SLOT 0xF000 >/dev/null      # the program area only (no leftovers from a bigger image); settings stay
$X spinor write $SLOT "$IMG"
$X spinor read $SLOT 0x10000 "$T/after.bin" >/dev/null
python3 - "$IMG" "$T/after.bin" "$T/before.bin" <<'PY'
import sys
img, got, before = (open(p, "rb").read() for p in sys.argv[1:4])
if got[0xF000:] != before[0xF000:]:
    sys.exit("VERIFY FAILED: the settings sector (0x12F000) changed")
if got[:len(img)] != img:
    bad = next(i for i in range(len(img)) if got[i] != img[i])
    sys.exit(f"VERIFY FAILED: first difference at slot offset {bad:#x}")
if got[len(img):0xF000] != b"\xff" * (0xF000 - len(img)):
    sys.exit("VERIFY FAILED: data after the image (program area not blank)")
s = "blank" if got[0xF000:] == b"\xff" * 0x1000 else "kept"
print(f"verified: {len(img)} bytes at 0x120000, rest of the program area blank, settings sector {s}")
PY
