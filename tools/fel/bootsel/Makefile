CROSS ?= riscv64-elf-
stub.bin: stub.S
	$(CROSS)gcc -march=rv64gc -mabi=lp64d -nostdlib -nostartfiles -Wl,-Ttext=0x2b800 \
	  -Wl,--defsym=orig_check=0x24d38 -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o stub.elf $<
	$(CROSS)objcopy -O binary -j .text stub.elf $@
clean:
	rm -f stub.elf stub.bin boot0-felwin.bin
