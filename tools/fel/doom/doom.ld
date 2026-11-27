/* Doom over FEL: code + data + bss below the framebuffers (0x41000000), the heap
   above them (libc.c HEAP 0x41800000, 32 MB), the WAD at 0x43800000, the stack
   and log at the top as for the other DRAM programs. */
ENTRY(_start)
MEMORY {
	ram (rwx) : org = 0x40000000, len = 0x01000000
	stk (rw)  : org = 0x43F00000, len = 0x000F0000
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
