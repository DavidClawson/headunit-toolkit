/* Single-blob SRAM payload for FEL: entry first, everything in one region.
   0x40000-0x40EFF code/data; 0x40F00 is the request block (see i2c.c). */
ENTRY(_start)
MEMORY { sram (rwx) : org = 0x40000, len = 0xF00 }
SECTIONS
{
	.text : { *(.text.start) *(.text*) *(.rodata*) *(.srodata*) } > sram
	.data : { *(.data*) *(.sdata*) } > sram
	.bss  : { *(.bss*) *(.sbss*) *(COMMON) } > sram
	/DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
