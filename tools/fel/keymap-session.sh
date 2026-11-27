#!/usr/bin/env bash
# keymap-session.sh: spoken prompts + one UART1 capture per button.
cd "$(dirname "$0")"
step() {
  echo "=== $1"; say "$1"; sleep 0.3
  ./uart_sniff.py listen uart1 --seconds 8 --baud 115200 | grep -E "^  [0-9a-f]{4}:|no traffic"
  say "stop"; sleep 2
}
say "Key mapping. Get ready."; sleep 4
step "Press home, three times"
step "Press phone, three times"
step "Turn the knob clockwise, three clicks"
step "Turn the knob counter clockwise, three clicks"
step "Push the knob, three times"
step "Hold home for two seconds"
say "All done"
