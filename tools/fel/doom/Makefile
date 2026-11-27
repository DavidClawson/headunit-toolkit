# Doom on the head unit (bare metal, run over FEL). Run ./fetch.sh first.
#   make            -> doom.bin
#   ../felrun.py doom/doom.bin --load 0x43800000:doom/doom1.wad --timeout 3600
CROSS  = riscv64-elf-
DG     = doomgeneric/doomgeneric
DRAM   = ../dram
CFLAGS = -march=rv64gc_zicsr -mabi=lp64d -mcmodel=medany -O2 -ffreestanding -fno-builtin \
         -nostdlib -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
         -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -DFEATURE_SOUND
DOOMSRC = $(filter-out doomgeneric_xlib.c,$(patsubst %.o,%.c,$(shell sed -n 's/^SRC_DOOM = //p' $(DG)/Makefile)))
OURS    = libc.c dg_f133.c snd_f133.c
HW      = start.S lib.c dsi.c display.c gfx.c font8x16.c font16x32.c twi.c gt911.c uart.c mcu.c input.c \
          bbi2c.c tuner.c audio.c
OBJ     = $(addprefix build/dg_,$(DOOMSRC:.c=.o)) $(addprefix build/,$(OURS:.c=.o)) \
          $(addprefix build/hw_,$(patsubst %.S,%.o,$(HW:.c=.o)))

doom.bin: doom.elf
	$(CROSS)objcopy -O binary $< $@
	@echo "$@: $$(wc -c < $@) bytes"
doom.elf: $(OBJ) doom.ld
	$(CROSS)gcc $(CFLAGS) -T doom.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments -o $@ $(OBJ) -lgcc
build/dg_%.o: $(DG)/%.c | build
	$(CROSS)gcc $(CFLAGS) -isystem inc -w -c $< -o $@
build/%.o: %.c | build
	$(CROSS)gcc $(CFLAGS) -isystem inc -I$(DG) -I$(DRAM) -Wall -c $< -o $@
build/hw_%.o: $(DRAM)/%.c | build
	$(CROSS)gcc $(CFLAGS) -I$(DRAM) -Wall -c $< -o $@
build/hw_%.o: $(DRAM)/%.S | build
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
build:
	mkdir -p build
clean:
	rm -rf build doom.elf doom.bin
