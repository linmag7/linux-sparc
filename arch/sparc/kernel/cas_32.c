// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel assisted compare-and-swap for pre-v9 sparc.
 *
 * v8 and earlier have no compare-and-swap instruction, so userspace can
 * only emulate one with a lock.  A lock private to a process cannot
 * serialise against another process operating on the same page, and a
 * lock kept inside the object costs value bits, which rules out the
 * full-width operations the compiler expects.  A lock owned by the kernel
 * has neither problem.  parisc does the same thing for the same reason.
 *
 * The array is shared with asm/futex_32.h so that a compare-and-swap from
 * userspace and one performed by the futex code exclude each other.
 *
 * Where the cpu does implement the instruction - LEON3 and later - the lock
 * is not used at all and the swap is performed directly.  That has to follow
 * the hardware rather than a build option: a binary built for such a cpu uses
 * casa inline and never comes here, so a kernel serialising on a lock would
 * exclude nothing against it.
 */

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>

#include <asm/ptrace.h>
#include <asm/cas_32.h>
#include <asm/cpu_type.h>

raw_spinlock_t sparc32_atomic_locks[SPARC32_ATOMIC_LOCKS] = {
	[0 ... (SPARC32_ATOMIC_LOCKS - 1)] =
		__RAW_SPIN_LOCK_UNLOCKED(sparc32_atomic_locks)
};

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

/*
 * ta 0x11, which is trap type SP_TRAP_CAS.  The interface is described in
 * Documentation/arch/sparc/cas-trap.rst.
 *
 *	%o0	word aligned user address
 *	%o1	expected value
 *	%o2	new value
 *
 * returns the value found at the address in %o0, so the caller can tell
 * whether the swap happened by comparing it with what it expected.  On
 * failure %o0 holds an errno and the carry bit is set.
 */
asmlinkage void handle_sparc32_cas(struct pt_regs *regs, unsigned long pc,
				   unsigned long npc, unsigned long psr)
{
	u32 __user *uaddr = (u32 __user *)regs->u_regs[UREG_I0];
	u32 oldval = regs->u_regs[UREG_I1];
	u32 newval = regs->u_regs[UREG_I2];
	raw_spinlock_t *lock;
	unsigned long flags;
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

	if (sparc32_has_casa) {
		/*
		 * No lock: the instruction is atomic against everything else
		 * touching the word, including a binary doing its own casa.
		 * A fault here is an ordinary one - we hold nothing - so the
		 * fault handler resolves it and the instruction is retried;
		 * the exception table entry only catches an address that
		 * cannot be made good.
		 */
		if (likely(!__sparc32_casa_user(uaddr, oldval, newval, &val)))
			cas_return(regs, val, false);
		else
			cas_return(regs, EFAULT, true);
		return;
	}

	lock = sparc32_atomic_lock(uaddr);

retry:
	/*
	 * pagefault_disable() turns a fault under the lock into -EFAULT
	 * instead of a trip through the mmap lock, which must not happen
	 * while holding a raw spinlock with interrupts off.  On failure the
	 * lock is dropped, the page is faulted in, and we start again.
	 */
	raw_spin_lock_irqsave(lock, flags);
	pagefault_disable();

	err = __get_user(val, uaddr);
	if (!err && val == oldval)
		err = __put_user(newval, uaddr);

	pagefault_enable();
	raw_spin_unlock_irqrestore(lock, flags);

	if (likely(!err)) {
		cas_return(regs, val, false);
		return;
	}

	/*
	 * fault_in_safe_writeable() rather than fault_in_writeable(): the
	 * latter zeroes the range it faults in, which here is the word
	 * being swapped.
	 */
	if (fault_in_safe_writeable((char __user *)uaddr, sizeof(u32))) {
		cas_return(regs, EFAULT, true);
		return;
	}

	goto retry;
}
