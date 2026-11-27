CROSS ?= riscv64-elf-
stub.bin: stub.S fwboot.h
	$(CROSS)gcc -march=rv64gc -mabi=lp64d -nostdlib -nostartfiles -Wl,-Ttext=0x2b800 \
	  -Wl,--defsym=orig_check=0x24d38 \
	  -Wl,--defsym=orig_load=0x284fa -Wl,--defsym=flash_init=0x2819c -Wl,--defsym=flash_read=0x28080 -I. -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o stub.elf $<
	$(CROSS)objcopy -O binary -j .text stub.elf $@
clean:
	rm -f stub.elf stub.bin boot0-felwin.bin boot0-fw.bin
