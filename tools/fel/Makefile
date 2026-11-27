# FEL test payloads for the W022. Needs riscv64-elf-gcc (brew install riscv64-elf-gcc).
CROSS ?= riscv64-elf-
LOAD  ?= 0x40000
CFLAGS = -march=rv64gc_zicsr -mabi=lp64d -mcmodel=medany -Os -ffreestanding \
         -fno-builtin -nostdlib -nostartfiles -Wall -Wextra

.PRECIOUS: %.elf

all: probe.bin i2c.bin

probe.elf: probe.S
	$(CROSS)gcc $(CFLAGS) -Wl,-Ttext=$(LOAD) -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@ $<

%.elf: %.c payload.ld
	$(CROSS)gcc $(CFLAGS) -T payload.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@ $<

%.bin: %.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes, load at $(LOAD)"

clean:
	rm -f *.elf *.bin

.PHONY: all clean
