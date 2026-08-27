/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compare-and-swap for the 32-bit sparc implementations that have it.
 *
 * The instruction cannot appear in inline asm: the kernel builds sparc32 with
 * -Wa,-Av8 and the assembler rejects casa outright, so it lives here in an
 * object assembled -Wa,-Aleon instead.  A cpu without the instruction raises
 * illegal_instruction, which the exception table entry below turns into
 * -EFAULT, so the same entry serves both the probe and a faulting address.
 *
 * Both helpers are laid out for the LEON atomic errata: GRLIB-TN-0011 wants
 * the atomic 16-byte aligned, or an instruction TLB miss can release the bus
 * lock before the store, and GRLIB-TN-0010 forbids reaching one directly from
 * a load or a control transfer - which a call with a load in its delay slot
 * does.  A nop at the entry and nop padding up to the boundary cover both, for
 * less than runtime detection would cost.
 *
 * The padding has to be spelled .balignl so the fill is nops whatever the
 * assembler is aimed at.  A plain .align is nops under -Aleon but a branch over
 * the gap under -Av9, and that branch lands on the atomic - the very sequence
 * TN-0010 asks us to avoid.
 */

#include <linux/linkage.h>
#include <asm/errno.h>

	.text
	.align	4

/*
 * int __sparc32_casa(u32 *addr, u32 oldval, u32 newval, u32 *prev)
 *
 * Stores the value found at addr in *prev and returns 0, or returns -EFAULT
 * if the access faulted or the cpu does not implement the instruction.
 * ASI 0x0b is supervisor data.
 */
ENTRY(__sparc32_casa)
	nop
	.balignl 16, 0x01000000
1:	casa	[%o0] 0x0b, %o1, %o2
	st	%o2, [%o3]
	retl
	 clr	%o0
ENDPROC(__sparc32_casa)

/*
 * The same on a user address.  ASI 0x0a is user data, which is what a binary
 * built for such a cpu uses itself, so the kernel and userspace operate on the
 * word the same way.
 */
ENTRY(__sparc32_casa_user)
	nop
	.balignl 16, 0x01000000
4:	casa	[%o0] 0x0a, %o1, %o2
	st	%o2, [%o3]
	retl
	 clr	%o0
ENDPROC(__sparc32_casa_user)

	.section .fixup,#alloc,#execinstr
	.align	4
2:	retl
	 mov	-EFAULT, %o0
	.previous

	.section __ex_table,#alloc
	.align	4
	.word	1b, 2b
	.word	4b, 2b
	.previous
