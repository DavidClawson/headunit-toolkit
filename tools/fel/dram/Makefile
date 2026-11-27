# DRAM programs for FEL (load/run with tools/fel/felrun.py). Needs riscv64-elf-gcc.
CROSS ?= riscv64-elf-
CFLAGS = -march=rv64gc_zicsr -mabi=lp64d -mcmodel=medany -O2 -ffreestanding \
         -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -fno-tree-loop-distribute-patterns
LDFLAGS = -T dram.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments
PROGS = memtest clock

all: $(PROGS:=.bin)

.PRECIOUS: %.elf
%.elf: %.c start.S lib.c lib.h dram.ld
	$(CROSS)gcc $(CFLAGS) $(LDFLAGS) -o $@ start.S lib.c $<

%.bin: %.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes at 0x40000000"

clean:
	rm -f *.elf *.bin
.PHONY: all clean
