#!/usr/bin/env bash
# fel-catch.sh: catch the boot0 FEL window (tools/fel/bootsel) and stay in FEL.
#
# Start this, then power the unit on (or run fel-boot-stock.sh / reset it).
# With the patched boot0, every power-up sits in FEL for ~3 s with the
# watchdog armed. When the FEL device appears this disarms the watchdog and
# clears the "window used" flag, so the unit stays in FEL.
#   tools/fel/fel-catch.sh [timeout-seconds]    (default 120)
XFEL="${XFEL:-$HOME/src/xfel/xfel}"
end=$(( $(date +%s) + ${1:-120} ))
while [ "$(date +%s)" -lt "$end" ]; do
  if ioreg -p IOUSB -l -w0 | grep -qiE '"idVendor" = 7994'; then
    [ -z "$seen" ] && { seen=1; echo "FEL device seen at $(date +%T)"; }
    for i in 1 2 3 4 5 6 7 8 9 10; do
      if "$XFEL" write32 0x020500b8 0x16aa0000 >/dev/null 2>&1; then
        "$XFEL" write32 0x0709010c 0 >/dev/null 2>&1
        "$XFEL" write32 0x40ff0 0 >/dev/null 2>&1     # felrun.py "DRAM ready" marker: fresh boot
        echo "caught FEL at $(date +%T); watchdog off: $("$XFEL" read32 0x020500b8)"
        afplay /System/Library/Sounds/Glass.aiff 2>/dev/null &
        exit 0
      fi
      sleep 0.05
    done
    echo "  xfel could not disarm in time ($(date +%T)); still trying"
  else
    [ -n "$seen" ] && { echo "FEL device gone at $(date +%T) (window closed?)"; seen=; }
  fi
  sleep 0.05
done
echo "no FEL device within ${1:-120} s"; exit 1
