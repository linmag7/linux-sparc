/* SPDX-License-Identifier: GPL-2.0-only */
	.align	32
	.globl	__flushw_user
	.type	__flushw_user,#function
__flushw_user:
	rdpr	%otherwin, %g1
	brz,pn	%g1, 2f
	 clr	%g2
1:	save	%sp, -128, %sp
	rdpr	%otherwin, %g1
	brnz,pt	%g1, 1b
	 add	%g2, 1, %g2
1:	sub	%g2, 1, %g2
	brnz,pt	%g2, 1b
	 restore %g0, %g0, %g0
2:	retl
	 nop
	.size	__flushw_user,.-__flushw_user
EXPORT_SYMBOL(__flushw_user)

	/* Flush %fp and %i7 to the stack for all register
	 * windows active inside of the cpu.  This allows
	 * show_stack_trace() to avoid using an expensive
	 * 'flushw'.
	 */
	.globl		stack_trace_flush
	.type		stack_trace_flush,#function
stack_trace_flush:
	rdpr		%pstate, %o0
	wrpr		%o0, PSTATE_IE, %pstate

	rdpr		%cwp, %g1
	rdpr		%canrestore, %g2
	sub		%g1, 1, %g3

1:	brz,pn		%g2, 2f
	 sub		%g2, 1, %g2
	wrpr		%g3, %cwp
	stx		%fp, [%sp + STACK_BIAS + RW_V9_I6]
	stx		%i7, [%sp + STACK_BIAS + RW_V9_I7]
	ba,pt		%xcc, 1b
	 sub		%g3, 1, %g3

2:	wrpr		%g1, %cwp
	wrpr		%o0, %pstate

	retl
	 nop
	.size		stack_trace_flush,.-stack_trace_flush

#ifdef CONFIG_SMP
	.globl		hard_smp_processor_id
	.type		hard_smp_processor_id,#function
hard_smp_processor_id:
#endif
	.globl		real_hard_smp_processor_id
	.type		real_hard_smp_processor_id,#function
real_hard_smp_processor_id:
	__GET_CPUID(%o0)
	retl
	 nop
#ifdef CONFIG_SMP
	.size		hard_smp_processor_id,.-hard_smp_processor_id
#endif
	.size		real_hard_smp_processor_id,.-real_hard_smp_processor_id
EXPORT_SYMBOL_GPL(real_hard_smp_processor_id)

	.align 32
	.globl sparc64_vii_ecc_trap
sparc64_vii_ecc_trap:
	/* VII Extensions P.7.1/P.7.4: latch and clear before restoring IE. */
	ldxa	[%g0] 0x4c, %g4
	brz,pn	%g4, .Lvii_empty_ecc
	 nop
	set	((1 << 12) | (1 << 11) | (1 << 10)), %g1
	andncc	%g4, %g1, %g0
	bne,pn	%xcc, sparc64_vii_fatal_trap
	 nop
	stxa	%g4, [%g0] 0x4c
	membar	#Sync
	rdpr	%pil, %g2
	wrpr	%g0, PIL_NORMAL_MAX, %pil
	ba,pt	%xcc, etrap_irq
	 rd	%pc, %g7
#ifdef CONFIG_TRACE_IRQFLAGS
	call	trace_hardirqs_off
	 nop
#endif
	mov	%l4, %o1
	call	sparc64_vii_ecc_error
	 add	%sp, PTREGS_OFF, %o0
	ba,a,pt	%xcc, rtrap_irq

.Lvii_empty_ecc:
	retry

	.globl sparc64_vii_ecc_trap_tl1
sparc64_vii_ecc_trap_tl1:
	/* Nested entry cannot assume either a safe stack or unused globals. */
	ba,pt	%xcc, sparc64_vii_fatal_trap
	 nop

	.align 32
	.globl sparc64_vii_fatal_trap
sparc64_vii_fatal_trap:
	wrpr %g0, 15, %pil
	sethi %hi(sparc64_vii_fault_record), %g1
	or %g1, %lo(sparc64_vii_fault_record), %g1
	rdpr %tpc, %g2
	stx %g2, [%g1 + 8]
	rdpr %tnpc, %g2
	stx %g2, [%g1 + 16]
	rdpr %tstate, %g2
	stx %g2, [%g1 + 24]
	rdpr %tl, %g2
	stx %g2, [%g1 + 32]
	rdpr %tt, %g2
	stx %g2, [%g1 + 40]
	/* VII Appendix P: state-change error information, read-only here. */
	mov 0x18, %g2
	ldxa [%g2] 0x4c, %g2
	stx %g2, [%g1 + 48]
	ldxa [%g0] 0x4c, %g2
	stx %g2, [%g1 + 56]
	set 0x56494900, %g2
	stx %g2, [%g1]
	membar #Sync
1:	ba,pt %xcc, 1b
	 nop

	.data
	.align 64
	.globl sparc64_vii_fault_record
sparc64_vii_fault_record:
	.xword 0, 0, 0, 0, 0, 0, 0, 0
	.previous

/*
 * Require disabled shared-context/prefetch valid bits (VII F.10.2/F.10.13).
 * Failures populate sparc64_vii_mmu_fault; a locked PROM cannot report them.
 */
	.align	32
	.globl	sparc64_vii_check_mmu_state
	.type	sparc64_vii_check_mmu_state,#function
sparc64_vii_check_mmu_state:
	/* Preserve %o0, which carries the secondary startup cookie. */
	mov	0x68, %o1
	ldxa	[%o1] 0x58, %o2
	sethi	%hi(0x8000), %o3
	sllx	%o3, 32, %o4
	or	%o3, %o4, %o3
	andcc	%o2, %o3, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	1, %o1
	mov	0x0, %o1
	ldxa	[%o1] 0x61, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	2, %o1
	mov	0x8, %o1
	ldxa	[%o1] 0x61, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	3, %o1
	mov	0x40, %o1
	ldxa	[%o1] 0x61, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	4, %o1
	mov	0x48, %o1
	ldxa	[%o1] 0x61, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	5, %o1
	mov	0x0, %o1
	ldxa	[%o1] 0x62, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	6, %o1
	mov	0x8, %o1
	ldxa	[%o1] 0x62, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	7, %o1
	mov	0x40, %o1
	ldxa	[%o1] 0x62, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	8, %o1
	mov	0x48, %o1
	ldxa	[%o1] 0x62, %o2
	andcc	%o2, 0x100, %g0
	bne,pn	%xcc, .Lvii_mmu_bad
	 mov	9, %o1
	retl
	 nop
.Lvii_mmu_bad:
	/* PROM may already be locked; record the failure without reentering it. */
	wrpr	%g0, 15, %pil
	rdpr	%pstate, %o3
	andn	%o3, PSTATE_IE, %o3
	wrpr	%o3, 0, %pstate
	sethi	%hi(sparc64_vii_mmu_fault), %o3
	or	%o3, %lo(sparc64_vii_mmu_fault), %o3
	stx	%o1, [%o3 + 8]
	stx	%o2, [%o3 + 16]
	stx	%o7, [%o3 + 24]
	set	0x56494d4d, %o2
	stx	%o2, [%o3]
	membar	#Sync
1:	ba,pt	%xcc, 1b
	 nop
	.size	sparc64_vii_check_mmu_state,.-sparc64_vii_check_mmu_state

	.data
	.align	32
/* Fault record: magic, register (1 shared, 2-5 I, 6-9 D), value, caller. */
	.globl	sparc64_vii_mmu_fault
sparc64_vii_mmu_fault:
	.xword	0, 0, 0, 0
	.previous

	/* V Supplement 5.2.12, F.10.1/F.10.7 and R.3: retain the qualified MMU modes. */
	.align 32
	.globl sparc64_v_check_mmu_state
	.type sparc64_v_check_mmu_state,#function
sparc64_v_check_mmu_state:
	ldxa	[%g0] 0x45, %o2
	and	%o2, 0xc, %o3
	cmp	%o3, 0xc
	bne,pn	%xcc, .Lv_mmu_bad
	 mov	1, %o1
	srlx	%o2, 41, %o3
	andcc	%o3, 1, %g0
	bne,pn	%xcc, .Lv_mmu_bad
	 nop
	mov	8, %o1
	ldxa	[%o1] 0x45, %o2
	set	0x1f100, %o3
	and	%o2, %o3, %o3
	set	0x2000, %o4
	cmp	%o3, %o4
	bne,pn	%xcc, .Lv_mmu_bad
	 mov	2, %o1
	ldxa	[%g0] 0x4a, %o2
	srlx	%o2, 39, %o3
	and	%o3, 3, %o3
	cmp	%o3, 1
	bgu,pn	%xcc, .Lv_mmu_bad
	 mov	3, %o1
	mov	0x48, %o5
.Lv_mmu_tsb:
	ldxa	[%o5] 0x58, %o2
	brnz,pn	%o2, .Lv_mmu_bad
	 mov	%o5, %o1
	cmp	%o5, 0x50
	be,pt	%xcc, .Lv_mmu_next
	 nop
	ldxa	[%o5] 0x50, %o2
	brnz,pn	%o2, .Lv_mmu_bad
	 or	%o5, 0x100, %o1
.Lv_mmu_next:
	add	%o5, 8, %o5
	cmp	%o5, 0x58
	bleu,pt	%xcc, .Lv_mmu_tsb
	 nop
	retl
	 nop
.Lv_mmu_bad:
	/* The firmware lock may be held; record the mismatch without calling PROM. */
	wrpr	%g0, 15, %pil
	rdpr	%pstate, %o3
	andn	%o3, PSTATE_IE, %o3
	wrpr	%o3, 0, %pstate
	sethi	%hi(sparc64_v_mmu_fault), %o3
	or	%o3, %lo(sparc64_v_mmu_fault), %o3
	stx	%o1, [%o3 + 8]
	stx	%o2, [%o3 + 16]
	stx	%o7, [%o3 + 24]
	set	0x56534d4d, %o2
	stx	%o2, [%o3]
	membar	#Sync
1:	ba,pt	%xcc, 1b
	 nop
	.size sparc64_v_check_mmu_state,.-sparc64_v_check_mmu_state

	.data
	.align 32
	/* Fault record: magic, register (1 DCU, 2 MCNTL, 3 UPA, or TSB offset), value, caller. */
	.globl sparc64_v_mmu_fault
sparc64_v_mmu_fault:
	.xword 0, 0, 0, 0
	.previous

	.align 32
	.globl sparc64_v_ecc_trap
sparc64_v_ecc_trap:
	/* V Supplement P.1.4/P.7.1: clear pending degradation before enabling IE. */
	ldxa	[%g0] 0x4c, %g4
	brz,pn	%g4, .Lv_empty_ecc
	 nop
	andncc	%g4, (1 << 10), %g0
	bne,pn	%xcc, .Lv_unhandled_ecc
	 nop
	stxa	%g4, [%g0] 0x4c
	membar	#Sync
	rdpr	%pil, %g2
	wrpr	%g0, PIL_NORMAL_MAX, %pil
	ba,pt	%xcc, etrap_irq
	 rd	%pc, %g7
#ifdef CONFIG_TRACE_IRQFLAGS
	call	trace_hardirqs_off
	 nop
#endif
	mov	%l4, %o1
	call	sparc64_v_ecc_error
	 add	%sp, PTREGS_OFF, %o0
	ba,a,pt	%xcc, rtrap_irq
.Lv_empty_ecc:
	retry
.Lv_unhandled_ecc:
	sethi	%hi(sparc64_v_fault_record), %g1
	or	%g1, %lo(sparc64_v_fault_record), %g1
	stx	%g4, [%g1 + 48]
	mov	8, %g2
	ldxa	[%g2] 0x4d, %g2
	stx	%g2, [%g1 + 56]
	ba,pt	%xcc, sparc64_v_fatal_trap
	 nop

	.globl sparc64_v_ecc_trap_tl1
sparc64_v_ecc_trap_tl1:
	/* Nested entry cannot assume a usable stack or unused trap globals. */
	ba,pt	%xcc, sparc64_v_fatal_trap
	 nop

	.align 32
	.globl sparc64_v_fatal_trap
	.type sparc64_v_fatal_trap, #function
sparc64_v_fatal_trap:
	wrpr %g0, 15, %pil
	sethi %hi(sparc64_v_fault_record), %g1
	or %g1, %lo(sparc64_v_fault_record), %g1
	rdpr %tpc, %g2
	stx %g2, [%g1 + 8]
	rdpr %tnpc, %g2
	stx %g2, [%g1 + 16]
	rdpr %tstate, %g2
	stx %g2, [%g1 + 24]
	rdpr %tl, %g2
	stx %g2, [%g1 + 32]
	rdpr %tt, %g2
	stx %g2, [%g1 + 40]
	/* Best-effort record in the locked image; no recovery or firmware. */
	sethi %hi(0x53505600), %g2
	or %g2, %lo(0x53505600), %g2
	stx %g2, [%g1]
	membar #Sync
1:	ba,pt %xcc, 1b
	 nop

	.size sparc64_v_fatal_trap, .-sparc64_v_fatal_trap

	.data
	.align 64
	.globl sparc64_v_fault_record
sparc64_v_fault_record:
	.xword 0, 0, 0, 0, 0, 0, 0, 0
	.previous
