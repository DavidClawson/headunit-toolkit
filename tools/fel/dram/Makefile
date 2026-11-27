# DRAM programs for FEL (load/run with tools/fel/felrun.py). Needs riscv64-elf-gcc.
CROSS ?= riscv64-elf-
CFLAGS = -march=rv64gc_zicsr -mabi=lp64d -mcmodel=medany -O2 -ffreestanding \
         -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -fno-tree-loop-distribute-patterns
LDFLAGS = -T dram.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments
PROGS = stage1 memtest clock dsi1 dsi2 dsion dsi3 dsi4 ui touch touchrate vsync app hang radio twiscan bdtest bbscan bttest calltest pbtest spitest bltest ledtest audtest dmatest mixtest adctest usbtest ncmtest iphonetest carplaytest

all: $(PROGS:=.bin)

.PRECIOUS: %.elf
COMMON = start.S lib.c dsi.c display.c gfx.c font8x16.c font16x32.c home.c twi.c gt911.c uart.c mcu.c input.c fw.c bbi2c.c tuner.c radio_ui.c bt.c bt_ui.c call_ui.c spinor.c settings.c settings_ui.c audio.c musb.c
%.elf: %.c $(COMMON) lib.h dsi.h display.h gfx.h home.h twi.h gt911.h uart.h mcu.h input.h fw.h tuner.h radio_ui.h bt.h bt_ui.h call_ui.h spinor.h settings.h settings_ui.h audio.h musb.h sine256.h bbi2c.h ../bootsel/fwboot.h panel_init.h dram.ld
	$(CROSS)gcc $(CFLAGS) -ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections -o $@ $(COMMON) $<

%.bin: %.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes at $$($(CROSS)nm $< | sed -n 's/^0*\([0-9a-f]*\) T _start$$/0x\1/p')"

clean:
	rm -f *.elf *.bin *.s2
.PHONY: all clean

# stage two: the full firmware linked at 0x40100000, for NOR S2_ADDR (see ../bootsel/fwboot.h)
app2.elf: app.c $(COMMON) stage2.ld
	$(CROSS)gcc $(CFLAGS) -ffunction-sections -fdata-sections -T stage2.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections -o $@ $(COMMON) app.c
app2.s2: app2.bin
	./mkfw.py --stage2 $< -o $@
stage1.fw: stage1.bin
	./mkfw.py $< -o $@

# the role-switch transition tested against the Mac (no iPhone)
carplaymac.elf: carplaytest.c $(COMMON) lib.h display.h gfx.h musb.h dram.ld
	$(CROSS)gcc $(CFLAGS) -DMACTEST -ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections -o $@ $(COMMON) $<
