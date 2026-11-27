#!/usr/bin/env bash
#
# backlight.sh: first hardware-mapping test over FEL. Drives the two backlight
# pins from sys_config_nor.fex [lcd0] as plain GPIOs:
#
#   lcd_bl_en  = PG15, output, active high       (backlight enable)
#   lcd_pwm_ch = 7 -> [pwm7] PD22 (func 5), pol 1 (active high)
#
# Instead of running the PWM, PD22 is set as a GPIO and held high (= 100% duty).
# The panel itself isn't initialised, so success is the screen glowing evenly
# (dark grey/white) rather than showing a picture.
#
#   tools/fel/backlight.sh status     # show the registers, change nothing
#   tools/fel/backlight.sh on
#   tools/fel/backlight.sh off
#   tools/fel/backlight.sh blink [n]  # n on/off cycles, 1 s each (default 5)
#
# Registers (D1/F133 GPIO at 0x02000000, 0x30 per port, PD = 3, PG = 6;
#   CFG0-3 at +0x00..0x0C, DAT at +0x10, DRV at +0x14):
#   PD_CFG2 0x02000098 (PD16-22, 4 bits/pin), PD_DAT 0x020000A0
#   PG_CFG1 0x02000124 (PG8-15),              PG_DAT 0x02000130
# Only the two pins' own bits are changed (read-modify-write).

set -euo pipefail
X="${XFEL:-$HOME/src/xfel/xfel}"

PD_CFG2=0x02000098; PD_DAT=0x020000A0; PD22_SHIFT=24; PD22_BIT=22
PG_CFG1=0x02000124; PG_DAT=0x02000130; PG15_SHIFT=28; PG15_BIT=15

rd()  { "$X" read32 "$1" | tr -d '[:space:]'; }
wr()  { "$X" write32 "$1" "$(printf '0x%08x' "$2")"; }
setfield() { # addr shift value(4 bits)
  local v; v=$(( $(rd "$1") )); v=$(( (v & ~(0xf << $2)) | ($3 << $2) )); wr "$1" "$v"; }
setbit() {   # addr bit 0|1
  local v; v=$(( $(rd "$1") ))
  if [ "$3" = 1 ]; then v=$(( v | (1 << $2) )); else v=$(( v & ~(1 << $2) )); fi
  wr "$1" "$v"; }

status() {
  local pdc pdd pgc pgd
  pdc=$(( $(rd $PD_CFG2) )); pdd=$(( $(rd $PD_DAT) ))
  pgc=$(( $(rd $PG_CFG1) )); pgd=$(( $(rd $PG_DAT) ))
  printf 'PD22 (PWM7): func %d  level %d   [PD_CFG2=0x%08x PD_DAT=0x%08x]\n' \
    $(( (pdc >> PD22_SHIFT) & 0xf )) $(( (pdd >> PD22_BIT) & 1 )) $pdc $pdd
  printf 'PG15 (BL_EN): func %d  level %d   [PG_CFG1=0x%08x PG_DAT=0x%08x]\n' \
    $(( (pgc >> PG15_SHIFT) & 0xf )) $(( (pgd >> PG15_BIT) & 1 )) $pgc $pgd
  echo "(func 0 = input, 1 = output, 15 = disabled)"
}

on()  { setbit $PD_DAT $PD22_BIT 1; setfield $PD_CFG2 $PD22_SHIFT 1
        setbit $PG_DAT $PG15_BIT 1; setfield $PG_CFG1 $PG15_SHIFT 1; }
off() { setbit $PD_DAT $PD22_BIT 0; setbit $PG_DAT $PG15_BIT 0; }

case "${1:-status}" in
  status) status ;;
  on)     on;  status ;;
  off)    off; status ;;
  blink)  for _ in $(seq 1 "${2:-5}"); do on >/dev/null; sleep 1; off >/dev/null; sleep 1; done; status ;;
  *) echo "usage: $0 status|on|off|blink [n]"; exit 1 ;;
esac
