# Display: panel facts and bring-up plan

Goal: light the LCD from our own code over FEL (no stock firmware), as the
base for a clean-sheet UI. Status: **desk work done, nothing run yet.**

## The panel (from `sys_config_nor.fex` `[lcd0]` / `[mipi_reg]`)

| | |
|---|---|
| Driver name | `he0801a068` (Melis kernel panel driver of that name) |
| Controller | **Jadard JD9365** (init opens with the `E0 00 / E1 93 / E2 65 / E3 F8` password) |
| Interface | MIPI-DSI (`lcd_if = 4`), **4 lanes**, video mode (`lcd_dsi_if = 0`), RGB888 (`lcd_dsi_format = 0`), no TE |
| Resolution | 720 × 1280 (portrait; the UI rotates 90°, `rotate_angle=90`) |
| Pixel clock | 70 MHz (`lcd_dclk_freq`) |
| Horizontal | total 908, back porch 48, sync 8 → front porch 132 |
| Vertical | total 1317, back porch 15, sync 6 → front porch 16 |
| Refresh | 70e6 / (908 × 1317) ≈ 58.5 Hz |
| Pins | PD0–PD9 function 4 = DSI lanes (`lcd_gpio_0..9`) |
| Backlight | PG15 enable + PWM7 on PD22 (verified, `tools/fel/pwm-backlight.sh`) |
| Power | `lcd_power = power2` (a regulator, not a GPIO); **no `lcd_reset` pin** |

**Init sequence:** `[mipi_reg]` in `sys_config`: 212 entries of `"RRVV"` (one
register, one value), across JD9365 pages selected by `E0 nn` (pages 0–5:
power, gamma, GIP timing), ending `E0 00`, `11 00` (sleep out), `delayms120`,
`29 00` (display on), `delayms5`, `35 00` (TE on). The Melis kernel's
`he0801a068 lcd_panel_init` reads this list (strings `mipi_reg`, `delayms`) and
optionally `lcd_reset`/`lcd_reset_time`, which this board doesn't set.

## The F133 side (mainline Linux as the register reference)

| Block | Address | Linux driver |
|---|---|---|
| DE2 clocks | `0x05000000` | `sun8i_mixer.c` (`sun20i-d1-de2-clk`) |
| Mixer 0 | `0x05100000` | `sun8i_mixer.c`, `sun8i_ui_layer.c` |
| MIPI-DSI | `0x05450000` | `sun6i_mipi_dsi.c` (A100 variant) |
| D-PHY | `0x05451000` | `phy-sun6i-mipi-dphy.c` (`sun50i_a100_mipi_dphy_tx_power_on`) |
| TCON TOP | `0x05460000` | `sun8i_tcon_top.c` (routes mixer 0 → TCON LCD0, gates the DSI clock) |
| TCON LCD0 | `0x05461000` | `sun4i_tcon.c` (`sun20i_d1_lcd_quirks`) |
| CCU | `0x02001000` | `ccu-sun20i-d1.c` (PLL_VIDEO0 `0x040`, DE `0x600`, MIPI_DSI, TCON_LCD0) |

Addresses from `sunxi-d1s-t113.dtsi`.

## Bring-up plan (each stage a DRAM program run with `tools/fel/felrun.py`)

1. **DSI command path.** Clocks and resets (bus DSI, MIPI_DSI module clock,
   TCON TOP DSI gate), D-PHY power-on in low-power mode, DSI controller in
   command mode. Success test: **read the panel's ID over DSI** (DCS `0x04` /
   `0xDA`–`0xDC`, needs bus turnaround). No video needed, so a failure here
   means the link, not the timing.
2. **Panel init.** Send the `[mipi_reg]` list (DCS/generic short writes), sleep
   out, display on, backlight on. A panel that's on but gets no video often
   shows a grey or noisy screen: the first visible sign.
3. **TCON test pattern over DSI video.** TCON LCD0 timing from the table above,
   DSI switched to video mode, and the TCON's own pattern generator (colour
   check / grey / grid, `TCON0_CTL` source select). **First real image, no
   display engine needed.**
4. **Framebuffer.** DE2 mixer 0 with one UI layer reading a 720×1280 buffer in
   DRAM, routed through TCON TOP. Then we can draw anything.

Risks: getting the D-PHY timing wrong gives a silent failure (stage 1's ID read
catches that); `power2` may need enabling if it's a switched rail (it may
simply be always on: check the panel answers in stage 1 first).
