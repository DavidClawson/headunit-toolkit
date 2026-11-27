#!/usr/bin/env bash
#
# fel-exit.sh: undo fel-enter.sh. Writes the first 4 KB (boot0 header) back from
# the backup fel-enter.sh made, then reads the chip and compares it to that backup.
#
#   tools/fel/fel-exit.sh                      # uses stock/nor/fel-backup-latest.bin
#   tools/fel/fel-exit.sh stock/nor/pre-fel-YYYYmmdd-HHMMSS.bin
#
# 12 V MUST BE DISCONNECTED while the clip is on.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NOR="$ROOT/stock/nor"
B="${1:-$NOR/fel-backup-latest.bin}"
[ -f "$B" ] || { echo "Backup not found: $B"; exit 1; }
[ "$(dd if="$B" bs=1 skip=4 count=8 2>/dev/null)" = "eGON.BT0" ] || { echo "$B has no boot0 header; refusing."; exit 1; }

P="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
[ -n "$P" ] || { echo "No Pico found at /dev/cu.usbmodem*"; exit 1; }
F=(flashrom -p "serprog:dev=$P:115200,spispeed=4M" -c "XM25QH128C/XM25QH128D")

read -r -p "12 V unplugged and clip seated (red = pin 1)? [y/N] " ok
[[ "$ok" == [yY] ]] || exit 1

printf '00000000:00000fff hdr\n00001000:00ffffff rest\n' > "$NOR/fel-layout.txt"
echo "== Restoring boot0 header from $B"
"${F[@]}" -l "$NOR/fel-layout.txt" -i hdr -w "$B"

echo "== Reading back the whole chip to compare"
"${F[@]}" -r "$NOR/post-fel.bin"
if cmp "$NOR/post-fel.bin" "$B"; then
  echo "MATCHES the backup. Take the clip off and power on."
else
  echo "Chip differs from the backup. Do not power on; check the clip and rerun."
  exit 1
fi
