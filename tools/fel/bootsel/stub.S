/*
 * stub.S: FEL window for stock boot0 (Allwinner F133).
 *
 * Patched into boot0's padding at 0x2b800 (past its .bss, 0x2b4c8-0x2b5ec).
 * boot0's main calls orig_check (0x24d38) once at 0x28c48 to ask "jump to
 * FEL?" (RTC GP2 == 0x5AA5A55A); that call is redirected here.
 *
 *   orig_check says yes            -> yes (stock behaviour kept)
 *   RTC GP3 == TRIED               -> clear GP3, no: boot stock normally
 *   otherwise                      -> GP3 = TRIED, arm the watchdog
 *                                     (WIN seconds), yes: go to FEL
 *
 * The BROM's FEL entry (0x20) disarms the watchdog (0x8a-0x9a) before
 * starting FEL, so boot0's final `jr 0x20` (at 0x28c66) is redirected to
 * fel_tramp, which repeats the BROM's setup from 0x22-0x86 and enters at
 * 0x9e, just past the disarm. Outside our window (stock "reboot efex"),
 * fel_tramp disarms the watchdog itself, as the BROM would.
 *
 * In FEL, a host that wants to stay disarms the watchdog and clears GP3
 * (tools/fel/fel-catch.sh). If nobody does, the watchdog resets the chip,
 * boot0 runs again, sees TRIED, and boots stock.
 *
 * sel (boot selector, docs/boot-and-recovery.md): boot0's call to load the
 * Melis package (0x28d3c: jal 0x284fa) is redirected here. DRAM is up and
 * boot0's flash driver is usable: flash_init 0x2819c(0), flash_read
 * 0x28080(sector, count, dst), both 0 = ok. Contract in fwboot.h.
 *
 *   GP5 == STOCK1 or TRYING        -> stock (TRYING = our last boot hung)
 *   no valid image at 0x120000     -> stock
 *   otherwise                      -> GP5 = TRYING, watchdog 8 s, D-cache
 *                                     written back and switched off (as in
 *                                     FEL), jump to FW_LOAD
 *   the program returns            -> GP5 = STOCK1, watchdog reset
 * GP5 is cleared on entry, so a fallback lasts one boot. "stock" = a tail
 * call to the original loader with boot0's own return address.
 */
#include "fwboot.h"
	.equ RTC_GP3,   0x0709010C
	.equ TRIED,     0xFE1C0DE5
	.equ WIN,       3            /* WDT_MODE interval: 3 = 3 s */

	.section .text
	.global stub
stub:
	addi	sp, sp, -16
	sd	ra, 8(sp)
	jal	ra, orig_check
	bnez	a0, out                 /* stock "jump to FEL" flag: keep it */

	li	t0, RTC_GP3
	lw	t1, 0(t0)
	li	t2, TRIED
	sext.w	t2, t2
	beq	t1, t2, normal

	sw	t2, 0(t0)               /* mark: FEL window used */
	li	t0, WDT_CFG
	li	t1, WDT_KEY | 1         /* on timeout: reset whole system */
	sw	t1, 0(t0)
	li	t0, WDT_MODE
	li	t1, WDT_KEY | (WIN << 4) | 1
	sw	t1, 0(t0)
	li	a0, 1                   /* caller clears GP2, restores clocks, jr 0x20 */
	j	out

normal:
	sw	zero, 0(t0)
	li	a0, 0
out:
	ld	ra, 8(sp)
	addi	sp, sp, 16
	ret

	.global sel
sel:
	addi	sp, sp, -32
	sd	ra, 24(sp)
	sd	s0, 16(sp)
	sd	s1, 8(sp)
	sd	s2, 0(sp)
	li	s0, RTC_GP5
	lw	s1, 0(s0)
	sw	zero, 0(s0)
	li	t0, GP5_STOCK1
	sext.w	t0, t0
	beq	s1, t0, stock
	li	t0, GP5_TRYING
	sext.w	t0, t0
	beq	s1, t0, stock

	li	a0, 0
	jal	ra, flash_init
	bnez	a0, stock
	li	a0, FW_SECTOR
	li	a1, 1
	li	a2, FW_HDR_BUF
	jal	ra, flash_read
	bnez	a0, stock
	li	s2, FW_HDR_BUF
	lw	t0, 0(s2)               /* magic */
	li	t1, FW_MAGIC
	sext.w	t1, t1
	bne	t0, t1, stock
	lw	t0, 4(s2)               /* version */
	li	t1, FW_VERSION
	bne	t0, t1, stock
	lw	t0, 8(s2)               /* load address */
	li	t1, FW_LOAD
	bne	t0, t1, stock
	lwu	s1, 12(s2)              /* size: 4..FW_MAX, multiple of 4 */
	beqz	s1, stock
	li	t1, FW_MAX
	bgtu	s1, t1, stock
	andi	t0, s1, 3
	bnez	t0, stock

	addi	a1, s1, 511             /* read the program */
	srli	a1, a1, 9
	li	a0, FW_SECTOR + 1
	li	a2, FW_LOAD
	jal	ra, flash_read
	bnez	a0, stock

	li	t0, FW_LOAD             /* checksum */
	add	t1, t0, s1
	li	t2, 0
1:	lwu	t3, 0(t0)
	addw	t2, t2, t3
	addi	t0, t0, 4
	bltu	t0, t1, 1b
	lw	t3, 16(s2)
	bne	t2, t3, stock

	li	t0, GP5_TRYING          /* commit */
	sw	t0, 0(s0)
	li	t0, WDT_CFG
	li	t1, WDT_KEY | 1
	sw	t1, 0(t0)
	li	t0, WDT_MODE
	li	t1, WDT_KEY | (FW_WDT_INTV << 4) | 1
	sw	t1, 0(t0)
	.word	0x0030000b              /* th.dcache.ciall: write back + invalidate */
	fence	rw, rw
	csrci	0x7c1, 2                /* mhcr.DE = 0: D-cache off, as under FEL */
	.word	0x0100000b              /* th.icache.iall */
	fence.i
	li	t0, FW_LOAD
	jalr	ra, t0

	li	t0, GP5_STOCK1          /* the program returned: stock once, reset */
	sw	t0, 0(s0)
	li	t0, WDT_CFG
	li	t1, WDT_KEY | 1
	sw	t1, 0(t0)
	li	t0, WDT_MODE
	li	t1, WDT_KEY | 1         /* 0.5 s */
	sw	t1, 0(t0)
2:	j	2b

stock:
	ld	ra, 24(sp)
	ld	s0, 16(sp)
	ld	s1, 8(sp)
	ld	s2, 0(sp)
	addi	sp, sp, 32
	j	orig_load

	.global fel_tramp
fel_tramp:
	li	s1, 92                  /* BROM 0x22: marks the FEL path (checked at 0xe4) */
	li	t1, 0x400000            /* BROM 0x60-0x86: mxstatus, mhcr, MIE off, mie, mtvec */
	csrs	0x7c0, t1
	li	t2, 0x70013
	csrs	0x7c2, t2
	csrci	mstatus, 8
	csrw	mie, zero
	li	t0, 0x3320
	csrw	mtvec, t0
	li	t0, RTC_GP3
	lw	t1, 0(t0)
	li	t2, TRIED
	sext.w	t2, t2
	beq	t1, t2, 1f
	li	t0, WDT_MODE            /* not our window: disarm like BROM 0x8a-0x9a */
	li	t1, WDT_KEY
	sw	t1, 0(t0)
1:	li	t0, 0x9e
	jr	t0
