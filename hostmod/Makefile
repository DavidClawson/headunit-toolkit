# Build custom Melis modules for the W022 head unit.
#   brew install riscv64-elf-gcc
#   make -C hostmod            -> hostmod/build/hello.mod
CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
PYTHON  ?= python3

# C906 core: rv64gc. medany = pc-relative addressing (module sits above 2 GiB).
# --no-relax: never emit gp-relative accesses (gp belongs to the kernel).
CFLAGS  := -march=rv64gc -mabi=lp64d -mcmodel=medany -O2 -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-pic -fno-common \
           -fno-asynchronous-unwind-tables -fno-stack-protector -Iinclude
LDFLAGS := -nostdlib -static -T module.ld -Wl,--no-relax -Wl,--build-id=none -Wl,--no-warn-rwx-segments

BUILD := build

.SECONDEXPANSION:

# module name -> slot type (load address = 0xe0000000 + type MiB)
hello_TYPE := 0x75

all: $(BUILD)/hello.mod

$(BUILD)/%.elf: $$*/$$*.c include/melis.h module.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	  -Wl,--defsym=MODULE_BASE=$(shell printf '0x%x' $$((0xe0000000 + ($($*_TYPE) << 20)))) \
	  -o $@ $<

$(BUILD)/%.mod: $(BUILD)/%.elf
	cd .. && $(PYTHON) -m headunit pack-module hostmod/$< hostmod/$@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
.PRECIOUS: $(BUILD)/%.elf
