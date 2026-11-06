// SPDX-License-Identifier: GPL-2.0
/*
 * p1275.c: Sun IEEE 1275 PROM low level interface routines
 *
 * Copyright (C) 1996,1997 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/irqflags.h>
#include <linux/interrupt.h>
#include <linux/panic.h>

#include <asm/irq.h>
#include <asm/setup.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/spitfire.h>
#include <asm/pstate.h>
#include <asm/ldc.h>

struct {
	long prom_callback;			/* 0x00 */
	void (*prom_cif_handler)(long *);	/* 0x08 */
} p1275buf;

extern void prom_world(int);

extern void prom_cif_direct(unsigned long *args);
extern void prom_cif_callback(void);

/*
 * This provides SMP safety on the p1275buf.
 */
DEFINE_RAW_SPINLOCK(prom_entry_lock);

static bool primepower_cif_allowed(unsigned long *args, unsigned long flags,
				   bool terminal)
{
	if (!primepower_irq_supported() || !prom_console_retired() ||
	    system_state < SYSTEM_RUNNING)
		return true;
	if (terminal)
		return primepower_uart_handoff_active();
	/* Only this returning read has been qualified with the native UART active. */
	return !flags && !in_interrupt() && !preempt_count() &&
	       args[1] == 2 && args[2] == 1 &&
	       !strcmp((char *)args[0], "getproplen") &&
	       args[3] == (unsigned int)prom_root_node &&
	       !strcmp((char *)args[4], "name");
}

static int p1275_call(unsigned long *args, bool terminal)
{
	unsigned long flags;
	int admission = 0;

	/* Exceptional entry cannot transfer the native UART or IRQ ownership. */
	if (primepower_irq_supported() && prom_console_retired() &&
	    (in_nmi() || READ_ONCE(oops_in_progress) || panic_in_progress()))
		return -EOPNOTSUPP;
	local_save_flags(flags);
	if (!primepower_cif_allowed(args, flags, terminal))
		return -EOPNOTSUPP;
	local_irq_restore((unsigned long)PIL_NMI);
	if (terminal) {
		if (!raw_spin_trylock(&prom_entry_lock)) {
			admission = -EBUSY;
			goto restore;
		}
	} else {
		admission = primepower_cif_query_begin(args, flags);
		if (!admission)
			admission = primepower_cif_begin();
		if (admission < 0)
			goto restore;
		if (admission == PP250_CIF_RETAINED) {
			if (!raw_spin_trylock(&prom_entry_lock)) {
				int err = primepower_cif_query_cancel();

				admission = err ?: -EBUSY;
				goto restore;
			}
		} else {
			raw_spin_lock(&prom_entry_lock);
		}
	}

	prom_world(1);
	prom_cif_direct(args);
	prom_world(0);

	raw_spin_unlock(&prom_entry_lock);
	if (admission)
		admission = primepower_cif_end(admission);
restore:
	local_irq_restore(flags);
	return admission < 0 ? admission : 0;
}

int p1275_cmd_direct(unsigned long *args)
{
	return p1275_call(args, false);
}

int p1275_cmd_try_direct(unsigned long *args)
{
	return p1275_call(args, true);
}

void prom_cif_init(void *cif_handler)
{
	p1275buf.prom_cif_handler = (void (*)(long *))cif_handler;
}
