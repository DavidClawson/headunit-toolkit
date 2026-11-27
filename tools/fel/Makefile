# FEL test payloads for the W022. Needs riscv64-elf-gcc (brew install riscv64-elf-gcc).
CROSS ?= riscv64-elf-
LOAD  ?= 0x40000

.PRECIOUS: %.elf

all: probe.bin

%.elf: %.S
	$(CROSS)gcc -march=rv64gc_zicsr -mabi=lp64d -nostdlib -nostartfiles \
	  -Wl,-Ttext=$(LOAD) -Wl,--build-id=none -o $@ $<

%.bin: %.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes, load at $(LOAD)"

clean:
	rm -f *.elf *.bin

.PHONY: all clean
