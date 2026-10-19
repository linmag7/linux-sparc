/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC_CAS_32_H
#define _SPARC_CAS_32_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct pt_regs;

asmlinkage void handle_sparc32_cas(struct pt_regs *regs, unsigned long pc,
				   unsigned long npc, unsigned long psr);

/* Returns 0 and stores the value found in *prev, or -EFAULT. */
int __sparc32_casa(u32 *addr, u32 oldval, u32 newval, u32 *prev);

/* The same on a user address, using the user data ASI. */
int __sparc32_casa_user(u32 __user *addr, u32 oldval, u32 newval, u32 *prev);

#endif /* _SPARC_CAS_32_H */
