# DRAM programs for FEL (load/run with tools/fel/felrun.py). Needs riscv64-elf-gcc.
CROSS ?= riscv64-elf-
CFLAGS = -march=rv64gc_zicsr -mabi=lp64d -mcmodel=medany -O2 -ffreestanding \
         -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -fno-tree-loop-distribute-patterns
LDFLAGS = -T dram.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments
PROGS = memtest clock dsi1 dsi2 dsion dsi3

all: $(PROGS:=.bin)

.PRECIOUS: %.elf
COMMON = start.S lib.c dsi.c
%.elf: %.c $(COMMON) lib.h dsi.h panel_init.h dram.ld
	$(CROSS)gcc $(CFLAGS) -ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections -o $@ $(COMMON) $<

%.bin: %.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes at 0x40000000"

clean:
	rm -f *.elf *.bin
.PHONY: all clean
