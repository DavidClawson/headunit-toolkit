#!/usr/bin/env bash
#
# pwm-backlight.sh: backlight dimming over FEL with the F133's hardware PWM.
#
# From sys_config_nor.fex [lcd0]: lcd_pwm_ch = 7, lcd_pwm_freq = 50000,
# lcd_pwm_pol = 1; [pwm7]: PD22 function 5. Enable pin PG15 (lcd_bl_en) is held high.
#
#   tools/fel/pwm-backlight.sh init         # clocks, 50 kHz, PD22 -> PWM7, 50% duty
#   tools/fel/pwm-backlight.sh duty <0-100>
#   tools/fel/pwm-backlight.sh ramp [n]     # n slow sweeps 0 -> 100 -> 0 (default 2)
#   tools/fel/pwm-backlight.sh status
#
# Register layout as in Linux drivers/pwm/pwm-sun20i.c (D1/T113/F133):
#   CCU PWM_BGR        0x020017AC  bit0 bus gate, bit16 reset de-assert
#   PWM base           0x02000C00
#     PCCR67  +0x2C    clock for ch6/7: [8:7] src (0 = 24 MHz HOSC), [3:0] div 2^M
#     PCGR    +0x40    bit ch = clock gate, bit ch+16 = bypass
#     PER     +0x80    bit ch = enable
#     PCR(ch) +0x100+0x20*ch   [7:0] prescale K (/K+1), bit8 active state (1 = high)
#     PPR(ch) +0x104+0x20*ch   [31:16] entire cycles - 1, [15:0] active cycles
# 24 MHz / 480 = 50 kHz, so duty d% = d*480/100 active cycles.

set -euo pipefail
X="${XFEL:-$HOME/src/xfel/xfel}"

CCU_PWM_BGR=0x020017AC
B=0x02000C00
PCCR67=$((B + 0x2C)); PCGR=$((B + 0x40)); PER=$((B + 0x80))
PCR7=$((B + 0x100 + 0x20 * 7)); PPR7=$((B + 0x104 + 0x20 * 7))
PERIOD=480
PD_CFG2=0x02000098; PG_CFG1=0x02000124; PG_DAT=0x02000130

rd() { "$X" read32 "$(printf '0x%08x' "$1")" | tr -d '[:space:]'; }
wr() { "$X" write32 "$(printf '0x%08x' "$1")" "$(printf '0x%08x' "$2")"; }
rmw() { local v; v=$(( $(rd "$1") )); wr "$1" $(( (v & ~$2) | $3 )); }   # addr clear set

duty() {
  local d=$1 act; (( d < 0 )) && d=0; (( d > 100 )) && d=100
  act=$(( d * PERIOD / 100 ))
  wr $PPR7 $(( ((PERIOD - 1) << 16) | act ))
}

init() {
  rmw $CCU_PWM_BGR 0 $(( (1 << 16) | 1 ))        # de-assert reset, open bus gate
  rmw $PCCR67 $(( (3 << 7) | 0xf )) 0             # 24 MHz HOSC, no divide
  rmw $PCGR $(( 1 << 23 )) $(( 1 << 7 ))          # gate ch7 clock on, no bypass
  wr  $PCR7 $(( 1 << 8 ))                         # prescale /1, active high
  duty 50
  rmw $PER 0 $(( 1 << 7 ))                        # enable ch7
  rmw $PG_DAT 0 $(( 1 << 15 )); rmw $PG_CFG1 $(( 0xf << 28 )) $(( 1 << 28 ))   # PG15 out high
  rmw $PD_CFG2 $(( 0xf << 24 )) $(( 5 << 24 ))    # PD22 -> PWM7
}

status() {
  local ppr; ppr=$(( $(rd $PPR7) ))
  printf 'PWM_BGR=%s PCCR67=%s PCGR=%s PER=%s PCR7=%s PPR7=%s\n' \
    "$(rd $CCU_PWM_BGR)" "$(rd $PCCR67)" "$(rd $PCGR)" "$(rd $PER)" "$(rd $PCR7)" "$(printf '0x%08x' $ppr)"
  printf 'PD22 func %d (5 = PWM7)   period %d cycles, active %d (%d%%)\n' \
    $(( ($(rd $PD_CFG2) >> 24) & 0xf )) $(( (ppr >> 16) + 1 )) $(( ppr & 0xffff )) \
    $(( (ppr & 0xffff) * 100 / ((ppr >> 16) + 1) ))
}

case "${1:-status}" in
  init)   init; status ;;
  duty)   duty "${2:?duty 0-100}"; status ;;
  ramp)   for _ in $(seq 1 "${2:-2}"); do
            for d in 0 2 5 10 20 35 50 70 100; do duty $d; sleep 0.4; done
            for d in 70 50 35 20 10 5 2 0; do duty $d; sleep 0.4; done
          done; duty 50; status ;;
  status) status ;;
  *) echo "usage: $0 init|duty <0-100>|ramp [n]|status"; exit 1 ;;
esac
