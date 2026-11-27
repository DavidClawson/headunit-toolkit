/*
 * Link a Melis system module at its fixed slot address.
 * Pass the base with: -Wl,--defsym=MODULE_BASE=0xe7500000
 * (= 0xe0000000 + type * 1 MiB; must match the type in MELIS_MODULE()).
 */
OUTPUT_ARCH(riscv)
ENTRY(melis_module_desc)

SECTIONS
{
    . = MODULE_BASE;
    .text   : { *(.text.entry) *(.text .text.*) }
    .rodata : ALIGN(8) { *(.rodata .rodata.* .srodata .srodata.*) }
    .data   : ALIGN(8) { *(.data .data.* .sdata .sdata.*) }
    .bss    : ALIGN(8) { *(.bss .bss.* .sbss .sbss.* COMMON) }
    __module_end = .;

    /* The loader finds the descriptor by this section's NAME ("MAGIC"). */
    MAGIC 0xffff0000 : { KEEP(*(.magic)) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
