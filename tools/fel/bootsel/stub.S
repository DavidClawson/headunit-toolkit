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
 */
	.equ RTC_GP3,   0x0709010C
	.equ WDT_CFG,   0x020500B4
	.equ WDT_MODE,  0x020500B8
	.equ WDT_KEY,   0x16AA0000
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
