#!/bin/bash
# boot0-write.sh: write a boot0 image (exactly 48 KB at NOR 0) over FEL, and
# verify. The unit must be caught in FEL. Used for the boot selector (step 3)
# and for rollback:
#
#   tools/fel/boot0-write.sh tools/fel/bootsel/boot0-fw.bin       # v2: FEL window + selector
#   tools/fel/boot0-write.sh stock/nor/boot0-felwin-v1.bin        # v1: FEL window only (rollback)
#
# Refuses unless the boot0 on the flash now is one we know (v1, v2, or the new
# image itself). Saves the current 0x0-0x10000 first. After writing, checks
# the 48 KB and that 0xC000-0x10000 (data, not ours) is unchanged.
set -e
cd "$(dirname "$0")"
X=~/src/xfel/xfel
ROOT=$(cd ../.. && pwd)
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
V1=$ROOT/stock/nor/boot0-felwin-v1.bin
V2=$ROOT/tools/fel/bootsel/boot0-fw.bin
SAVE=$ROOT/stock/nor/boot0-area-before-$(date +%Y%m%d-%H%M%S).bin

[ "$(wc -c < "$IMG")" -eq 49152 ] || { echo "image is not 48 KB"; exit 1; }
python3 - "$IMG" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert b[4:12] == b"eGON.BT0", "no eGON.BT0 header"
s = bytearray(b); s[12:16] = struct.pack("<I", 0x5F0A6C39)
assert sum(struct.unpack(f"<{len(s)//4}I", s)) & 0xFFFFFFFF == struct.unpack("<I", b[12:16])[0], "bad eGON checksum"
print("image: eGON.BT0, checksum ok")
PY
ok=0
for i in $(seq 20); do
  if $X version 2>/dev/null | grep -q AWUSBFEX; then ok=$((ok + 1)); [ $ok -ge 3 ] && break; else ok=0; fi
  sleep 0.5
done
[ $ok -ge 3 ] || { echo "FEL link not stable; not writing"; exit 1; }
$X spinor | grep -q "Found spi nor" || { sleep 1; $X spinor | grep -q "Found spi nor"; } || { echo "flash not detected"; exit 1; }

$X spinor read 0 0x10000 "$SAVE" >/dev/null
python3 - "$SAVE" "$IMG" "$V1" "$V2" <<'PY'
import sys
cur = open(sys.argv[1], "rb").read()[:0xC000]
names = {"v1 (FEL window)": sys.argv[3], "v2 (FEL window + selector)": sys.argv[4], "the new image": sys.argv[2]}
for n, p in names.items():
    try:
        if open(p, "rb").read() == cur:
            print(f"flash now holds: {n}")
            break
    except FileNotFoundError:
        pass
else:
    sys.exit("flash holds an unknown boot0: not writing (compare with stock/nor backups by hand)")
PY
echo "saved current 0x0-0x10000 to $SAVE"

$X spinor write 0 "$IMG"
T=$(mktemp); trap 'rm -f "$T"' EXIT
$X spinor read 0 0x10000 "$T" >/dev/null
python3 - "$T" "$IMG" "$SAVE" <<'PY'
import sys
got, img, before = (open(p, "rb").read() for p in sys.argv[1:4])
if got[:0xC000] != img: sys.exit("VERIFY FAILED: boot0 area does not match the image")
if got[0xC000:0x10000] != before[0xC000:0x10000]: sys.exit("VERIFY FAILED: 0xC000-0x10000 changed")
print("verified: 48 KB boot0 matches, 0xC000-0x10000 unchanged")
PY
