#!/usr/bin/env bash
#
# fel-enter.sh: make the W022 boot into FEL (USB recovery) by erasing only the
# first 4 KB of the SPI flash (boot0's eGON.BT0 header). With no valid boot0
# the F133 BootROM falls through to FEL on USB0.
#
# Uses the Pico + SOIC-8 clip (pico-serprog) exactly as in docs/boot-and-recovery.md.
# Steps:
#   1. read the whole chip twice and require the reads to match (backup)
#   2. write a copy of that backup with bytes 0x0000-0x0fff erased (0xff),
#      touching only that 4 KB region, and verify it
# Undo with fel-exit.sh, which writes the 4 KB back from the same backup.
#
# 12 V MUST BE DISCONNECTED while the clip is on.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NOR="$ROOT/stock/nor"          # gitignored: never commit flash dumps
mkdir -p "$NOR"

P="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
[ -n "$P" ] || { echo "No Pico found at /dev/cu.usbmodem* (is pico-serprog plugged in?)"; exit 1; }
F=(flashrom -p "serprog:dev=$P:115200,spispeed=4M" -c "XM25QH128C/XM25QH128D")

read -r -p "12 V unplugged and clip seated (red = pin 1)? [y/N] " ok
[[ "$ok" == [yY] ]] || exit 1

TS="$(date +%Y%m%d-%H%M%S)"
B1="$NOR/pre-fel-$TS.bin"; B2="$NOR/pre-fel-$TS.check.bin"
echo "== Reading chip (1/2) -> $B1"; "${F[@]}" -r "$B1"
echo "== Reading chip (2/2)";        "${F[@]}" -r "$B2"
cmp "$B1" "$B2" || { echo "Reads differ: reseat the clip and run again. Nothing was written."; exit 1; }
rm "$B2"

magic="$(dd if="$B1" bs=1 skip=4 count=8 2>/dev/null)"
[ "$magic" = "eGON.BT0" ] || { echo "No eGON.BT0 at offset 4 (got '$magic'). Already in FEL mode? Not writing."; exit 1; }

printf '00000000:00000fff hdr\n00001000:00ffffff rest\n' > "$NOR/fel-layout.txt"
python3 - "$B1" "$NOR/fel-noboot0.bin" <<'PY'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
d[0:0x1000] = b"\xff" * 0x1000
open(sys.argv[2], "wb").write(d)
PY
ln -sf "$(basename "$B1")" "$NOR/fel-backup-latest.bin"

echo "== Erasing boot0 header (0x0000-0x0fff only)"
"${F[@]}" -l "$NOR/fel-layout.txt" -i hdr -w "$NOR/fel-noboot0.bin"

echo
echo "Done. Backup: $B1"
echo "Next: remove the clip, connect the front USB to the Mac (VBUS cut), power on, then:"
echo "  ~/src/xfel/xfel version"
