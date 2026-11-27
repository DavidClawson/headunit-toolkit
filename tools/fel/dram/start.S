/*
 * start.S: entry for DRAM programs run over FEL (xfel exec 0x40000000).
 * Saves the BootROM's registers, switches to our own stack in DRAM, clears
 * .bss, calls main(), then restores everything and returns to the FEL loop.
 */
	.section .text.start
	.global _start
_start:
	la	t0, saved
	sd	ra, 0(t0)
	sd	sp, 8(t0)
	sd	gp, 16(t0)
	sd	tp, 24(t0)
	sd	s0, 32(t0)
	sd	s1, 40(t0)
	li	t0, 0x2000                 /* mstatus.FS = initial: the FPU on */
	csrs	mstatus, t0
	fscsr	zero
	la	sp, __stack_top
	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop
	la	t0, __bss_start
	la	t1, __bss_end
1:	bgeu	t0, t1, 2f
	sd	zero, 0(t0)
	addi	t0, t0, 8
	j	1b
2:	call	lib_init
	call	main
	la	t0, saved
	ld	ra, 0(t0)
	ld	sp, 8(t0)
	ld	gp, 16(t0)
	ld	tp, 24(t0)
	ld	s0, 32(t0)
	ld	s1, 40(t0)
	ret

	.section .data
	.balign 8
saved:	.fill 6, 8, 0
