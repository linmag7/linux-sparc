// SPDX-License-Identifier: GPL-2.0
/* Legacy ta 0x11 ABI, sharing Kaloz's CASA/futex serialization backend. */
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <asm/ptrace.h>
#include <asm/cas_32.h>
#include <asm/futex.h>

/*
 * Set the carry bit to report an error, the way the syscall path does,
 * and step over the trapping instruction.
 */
static void cas_return(struct pt_regs *regs, unsigned long value, bool error)
{
	regs->u_regs[UREG_I0] = value;
	if (error)
		regs->psr |= PSR_C;
	else
		regs->psr &= ~PSR_C;

	regs->pc = regs->npc;
	regs->npc = regs->npc + 4;
}

/* Preserve the documented arguments, carry flag and errno return convention. */
asmlinkage void handle_sparc32_cas(struct pt_regs *regs, unsigned long pc,
                                 unsigned long npc, unsigned long psr)
{
	u32 __user *uaddr = (u32 __user *)regs->u_regs[UREG_I0];
	u32 oldval = regs->u_regs[UREG_I1];
	u32 newval = regs->u_regs[UREG_I2];
	u32 val;
	int err;

	if (unlikely((unsigned long)uaddr & 3)) {
		cas_return(regs, EINVAL, true);
		return;
	}
	if (unlikely(!access_ok(uaddr, sizeof(u32)))) {
		cas_return(regs, EFAULT, true);
		return;
	}
	for (;;) {
		pagefault_disable();
		err = futex_atomic_cmpxchg_inatomic(&val, uaddr, oldval, newval);
		pagefault_enable();
		if (!err) {
			cas_return(regs, val, false);
			return;
		}
		if (fault_in_safe_writeable((char __user *)uaddr, sizeof(u32))) {
			cas_return(regs, EFAULT, true);
			return;
		}
	}
}
