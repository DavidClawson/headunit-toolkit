# Hardware audit: what the config claims vs what's on the board

The vendor ships **one firmware for a family of boards** and tailors each model
with `Config.ini` (app level) and `sys_config` (board level). Both are hints,
not facts. Several lines are leftovers from templates or other board revisions.
This table is the ground truth for the bench unit (JOY-W022, mainboard "FR4
REV.09"). **Rule: nothing gets built on a feature until its row says ✅.**

Legend:
- ✅ confirmed on the board (probe, photo, or working test)
- ❌ checked and not there (or not as the config says)
- ❓ not checked yet
- ⚠️ partly confirmed, or contradicted

## Core

| Part | Config / code says | On the board | Evidence |
|---|---|---|---|
| SoC | F133 (D1s), RISC-V C906 | ✅ | FEL ID `0x00185900` |
| RAM | `sys_config` `[dram_para]` / hardware.md: 512 MiB DDR3 | ⚠️ **64 MB** (the F133's built-in DDR2) | `memtest` (62 MB tested, 0 errors); the 512 MiB figure is a template value |
| Flash | SPI NOR, 16 MB | ✅ XMC 25QH128 | photo, flashrom ID, `xfel spinor` |
| Display | MIPI-DSI panel | ✅ JD9365, 720×1280, 4 lanes | our firmware drives it ([display.md](display.md)) |
| Touch | `bCapacitiveScreen=0` | ⚠️ **capacitive** GT911 on TWI3 `0x5d` (the flag means something else, or is stale) | our firmware reads it |
| Backlight | PG15 enable + PWM7 (PD22) | ✅ | FEL test |
| Front-panel MCU | `mcuChip=2`, `/dev/uart1` | ✅ Bluetrum AB5301 (`AB5301_20260106`) | photo, version reply to `A0` ([mcu-protocol.md](mcu-protocol.md)) |
| Knob / keys | via MCU | ✅ knob, phone; ❓ Home | key frames ([issue #4](https://github.com/DavidClawson/headunit-toolkit/issues/4)) |

## Audio and radio

| Part | Config / code says | On the board | Evidence |
|---|---|---|---|
| Sound processor | `audioChip=0` (BD3702), `/dev/twi2` | ✅ answers at TWI2 `0x40` (write-only) | bus scan |
| Tuner | `bArmRadio=1`, `radioModule=0` (Si4730), `/dev/twi1`, reset `PB5` | ❌ **nothing answers on TWI1**, on either pin set; `ANT` module footprint empty | bus scans; photo |
| TWI1 pins | `sys_config`: PG14 / PB4 | ❌ PG14 can't be TWI1 (on the F133 it's TWI2-SCK in function 3); TWI1 can come out on **PB4/PB5 (fn 4, scanned: empty), PE0/PE1 (fn 4) or PG8/PG9 (fn 3)**, and the last two are **not scanned yet** | F133 datasheet pin table; Linux pin-mux table |
| FM radio | works in stock | ✅ 101.1 MHz from the rear RCA | iMic recording; **source unknown** ([issue #2](https://github.com/DavidClawson/headunit-toolkit/issues/2)) |
| RDS | `bHaveRds=0` | ❓ | test: set `bHaveRds=1` in Config.ini |
| AM | `bAmEnable=1` | ❓ | |
| Speaker amp | no amp pin anywhere | ❓ chip unknown | [issue #3](https://github.com/DavidClawson/headunit-toolkit/issues/3) |
| Rear RCA out | — | ✅ line level, stereo | iMic |
| Codec (internal) | `sys_config` `codec_used=0`, `gpio-spk=PH9` | ❌ unused; the F133 has no port H | `sys_config` |

## Connectivity and projection

| Part | Config / code says | On the board | Evidence |
|---|---|---|---|
| Apple MFi chip | `carplayI2cName=/dev/twi2` | ✅ TWI2 `0x10` | bus scan; `carplay.mod` probes 0x11 then 0x10 |
| CarPlay / AA | `usbIphoneLink=1`, `usbAndroidLink=2` | ✅ works in stock | owner's use |
| Wireless CarPlay/AA | `wireless.mod`, `wifiChannel=36` | ❓ | |
| WiFi | `sys_config`: SDIO on MMC1, `wifi_mod_sel=3` | ❓ chip unnamed (RTL8723-class guessed from the boot command line) | |
| Bluetooth | `btSerialName=/dev/uart4`, AT commands | ⚠️ BT status text arrives from the MCU (`81 D4`: "MMCAR BT"); UART4 silent from FEL | MCU traffic |
| USB (front) | USB0 OTG; FEL | ✅ FEL, host for sticks | daughterboard with a hub-like part ❓ |
| microSD | `card0_boot_para`; manual says there is a slot | ❌ no slot fitted | inspection |

## Vehicle inputs and video

| Part | Config / code says | On the board | Evidence |
|---|---|---|---|
| Camera / video in | TP2850 decoder | ✅ TWI2 `0x44`, chip ID `0x2850` | bus scan |
| Reverse, brake, FCCD | `brakeDetectGpio=-1`; MCU `A6` flags | ❓ likely via MCU | [mcu-protocol.md](mcu-protocol.md) |
| Steering-wheel control 1/2 | factory "Touch Learning" | ❓ likely MCU ADC | |
| Illumination (orange wire) | `illDetectGpio=-1` | ❓ likely via MCU | |
| IR remote | `[s_cir0] s_cir0_used=0` | ❓ receiver on the front panel; likely decoded by the MCU | |
| Mic | `[MIC]`, `bEchoCancel=1` | ❓ wired mic present | |
| DVD | `bArmDvd=1`, `bEnableDvd=1` | ❌ no drive | (config leftover) |
| CAN box | `bArmCan=0` | ❌ none | |
| TV tuner | `tvType=0` | ❌ none | |

## Notes

- **Contradictions found so far:** tuner, TWI1 pins, RAM size, `bCapacitiveScreen`,
  DVD, microSD.
- **`sourceValid=995757`** is a bitmask of the sources the UI offers. Decoding it
  against the app's source table would show which features the vendor meant
  this model to have.
- The car unit is a second W022. Its `Config.ini` may differ, and comparing the
  two would show what's per-unit.
