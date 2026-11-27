#!/usr/bin/env bash
# clip-wait.sh: poll the SOIC clip until flashrom sees the flash chip, then chime.
# Handy while reseating a fiddly clip. Read-only (chip ID probe only).
#   tools/fel/clip-wait.sh [seconds]   (default 300)
P="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
[ -n "$P" ] || { echo "No Pico at /dev/cu.usbmodem*"; exit 1; }
end=$(( $(date +%s) + ${1:-300} ))
while [ "$(date +%s)" -lt "$end" ]; do
  if flashrom -p "serprog:dev=$P:115200,spispeed=4M" 2>&1 | grep -q 'Found XMC'; then
    echo "chip found at $(date +%T)"; afplay /System/Library/Sounds/Glass.aiff 2>/dev/null; exit 0
  fi
  sleep 2
done
echo "no chip after ${1:-300} s"; exit 1
