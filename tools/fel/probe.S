/*
 * probe.S: first FEL test payload for the W022 (Allwinner F133 / T-Head C906).
 *
 * Proves the load -> run -> read-back loop over USB:
 *   xfel write 0x40000 probe.bin && xfel exec 0x40000 && xfel hexdump 0x40100 0x40
 *
 * Runs in M-mode from SRAM, uses no stack and no RAM outside this blob, and
 * returns to the BootROM's FEL loop with `ret`, the same convention as xfel's
 * own payloads. Nothing here touches the flash or any peripheral.
 *
 * Result block at 0x40100 (all 64-bit little-endian):
 *   +0x00  magic      0x600DF00D once the probe has run to the end
 *   +0x08  runs       incremented on every exec (proves repeated execution)
 *   +0x10  mvendorid  expect 0x5b7 (T-Head)
 *   +0x18  marchid
 *   +0x20  mimpid
 *   +0x28  mhartid    expect 0 (single core)
 *   +0x30  misa       RV64 + extension bits
 *   +0x38  cycles     mcycle delta across a 100000-iteration loop
 */
	.section .text
	.global _start
_start:
	la	t0, results

	csrr	t1, mvendorid
	sd	t1, 0x10(t0)
	csrr	t1, marchid
	sd	t1, 0x18(t0)
	csrr	t1, mimpid
	sd	t1, 0x20(t0)
	csrr	t1, mhartid
	sd	t1, 0x28(t0)
	csrr	t1, misa
	sd	t1, 0x30(t0)

	csrr	t2, mcycle
	li	t3, 100000
1:	addi	t3, t3, -1
	bnez	t3, 1b
	csrr	t1, mcycle
	sub	t1, t1, t2
	sd	t1, 0x38(t0)

	ld	t1, 0x08(t0)
	addi	t1, t1, 1
	sd	t1, 0x08(t0)

	fence
	li	t1, 0x600DF00D
	sd	t1, 0x00(t0)
	fence
	ret

	.org	0x100
results:
	.fill	8, 8, 0
