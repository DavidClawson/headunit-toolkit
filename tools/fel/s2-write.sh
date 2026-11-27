#!/bin/bash
# s2-write.sh: put a stage-two image (dram/mkfw.py --stage2 output) at NOR
# S2_ADDR (0xD20000) over FEL, and verify it. The unit must be caught in FEL.
#
#   tools/fel/s2-write.sh dram/app2.s2
#   ALLOW_BAD=1 tools/fel/s2-write.sh bad.s2      # deliberately invalid, to test the fallback
#
# The area 0xD20000..0xF20000 is the unused tail of stock's MINFS partition
# (zero in every stock dump; stock's files end at 0xD1D8E9). Refuses unless the
# area holds only zeros, erased flash, or an earlier stage-two image, so a
# different stock layout is never overwritten. Erases and writes only as far as
# needed, never past 0xF20000 (stock's UDISK), then reads back and compares.
set -e
cd "$(dirname "$0")"
X=~/src/xfel/xfel
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
A=0xD20000; END=0xF20000
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

if CHK=$(./dram/mkfw.py --check "$IMG"); then :
elif [ "$ALLOW_BAD" = 1 ]; then echo "image fails stage one's checks ($CHK) -- writing anyway (ALLOW_BAD=1, fallback test)"
else echo "image fails stage one's checks ($CHK): not writing"; exit 1; fi
python3 -c "import struct,sys; sys.exit(struct.unpack('<I',open('$IMG','rb').read(4))[0]!=0x32535548)" || { echo "not a stage-two image: not writing"; exit 1; }
echo "image: $CHK"
ok=0
for i in $(seq 20); do
  if $X version 2>/dev/null | grep -q AWUSBFEX; then ok=$((ok + 1)); [ $ok -ge 3 ] && break; else ok=0; fi
  sleep 0.5
done
[ $ok -ge 3 ] || { echo "FEL link not stable; not writing"; exit 1; }
$X spinor | grep -q "Found spi nor" || { sleep 1; $X spinor | grep -q "Found spi nor"; } || { echo "flash not detected; not writing"; exit 1; }
echo "flash: $($X spinor)"
$X spinor read 0x120000 0x10000 "$T/slot.bin" >/dev/null           # stage one + settings, to prove they stay put
$X spinor read $A $((END - A)) "$T/before.bin" >/dev/null
# erase length: the larger of the new image and what's there now, rounded up to 4 KB
LEN=$(python3 - "$IMG" "$T/before.bin" <<'PY'
import struct, sys
img, b = (open(p, "rb").read() for p in sys.argv[1:3])
used = max(len(b.rstrip(b"\0")), len(b.rstrip(b"\xff")) if b[:4] != b"\0" * 4 else 0)
magic = struct.unpack("<I", b[:4])[0]
if magic == 0x32535548:
    old = 512 + struct.unpack("<I", b[12:16])[0]
    rest = b[old:].lstrip(b"\xff")             # erased up to the 4 KB boundary, then stock's zeros
    if rest.strip(b"\0"):
        sys.exit(f"REFUSING: data after the old stage-two image (+{old:#x})")
    print(f"area before: stage-two image, {old} bytes", file=sys.stderr)
elif b.strip(b"\0") and b.strip(b"\xff"):
    n = len(b.rstrip(b"\0").rstrip(b"\xff"))
    sys.exit(f"REFUSING: the area holds unknown data (up to +{n:#x}); expected zeros, blank or a stage-two image")
else:
    old = 0
    print("area before: " + ("zeros (stock)" if not b.strip(b"\0") else "blank"), file=sys.stderr)
n = max(len(img), old)
print((n + 0xFFF) & ~0xFFF)
PY
)
[ $((A + LEN)) -le $((END)) ] || { echo "would pass 0xF20000; not writing"; exit 1; }
$X spinor erase $A $LEN >/dev/null
$X spinor write $A "$IMG"
$X spinor read $A $((END - A)) "$T/after.bin" >/dev/null
$X spinor read 0x120000 0x10000 "$T/slot2.bin" >/dev/null
python3 - "$IMG" "$T/after.bin" "$T/before.bin" "$T/slot.bin" "$T/slot2.bin" $LEN <<'PY'
import sys
img, got, before, s1, s2 = (open(p, "rb").read() for p in sys.argv[1:6])
L = int(sys.argv[6])
if s1 != s2: sys.exit("VERIFY FAILED: 0x120000 slot (stage one / settings) changed")
if got[:len(img)] != img:
    bad = next(i for i in range(len(img)) if got[i] != img[i])
    sys.exit(f"VERIFY FAILED: first difference at +{bad:#x}")
if got[len(img):L] != b"\xff" * (L - len(img)): sys.exit("VERIFY FAILED: erased tail not blank")
if got[L:] != before[L:]: sys.exit("VERIFY FAILED: flash beyond the erased range changed")
print(f"verified: {len(img)} bytes at 0xD20000; erased to +{L:#x}; everything else unchanged")
PY
