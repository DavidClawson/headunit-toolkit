#!/usr/bin/env bash
# fel-boot-stock.sh: leave FEL and boot the stock firmware, no power cycle.
# Marks the FEL window as used (RTC GP3) and resets via the watchdog, so the
# patched boot0 (tools/fel/bootsel) skips the window and boots normally.
XFEL="${XFEL:-$HOME/src/xfel/xfel}"
"$XFEL" write32 0x0709010c 0xfe1c0de5 && "$XFEL" write32 0x020500b4 0x16aa0001 &&
"$XFEL" write32 0x020500b8 0x16aa0001 && echo "resetting into stock firmware"
