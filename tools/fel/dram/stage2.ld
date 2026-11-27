/* Stage two (and FEL runs of it): code at 0x40100000, stack 0x43E00000..0x43EF0000 (stage one keeps its own), log at 0x43FF0000, stack below it.
   Run `xfel ddr f133` first (tools/fel/felrun.py does). */
ENTRY(_start)
MEMORY {
	ram (rwx) : org = 0x40100000, len = 0x03D00000
	stk (rw)  : org = 0x43E00000, len = 0x000F0000
	log (rw)  : org = 0x43FF0000, len = 0x00010000
}
SECTIONS
{
	.text : { *(.text.start) *(.text*) } > ram
	.rodata : { *(.rodata*) *(.srodata*) } > ram
	.data : { *(.data*) *(.sdata*) __global_pointer$ = . + 0x800; } > ram
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(8); __bss_end = .; } > ram
	__image_end = .;
	__stack_top = ORIGIN(stk) + LENGTH(stk);
	__log = ORIGIN(log);
	__log_size = LENGTH(log);
	/DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
