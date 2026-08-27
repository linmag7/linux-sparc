/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CPU_TYPE_H
#define __ASM_CPU_TYPE_H

#include <linux/types.h>

/*
 * Sparc (general) CPU types
 */
enum sparc_cpu {
  sun4m       = 0x00,
  sun4d       = 0x01,
  sun4e       = 0x02,
  sun4u       = 0x03, /* V8 ploos ploos */
  sun_unknown = 0x04,
  ap1000      = 0x05, /* almost a sun4m */
  sparc_leon  = 0x06, /* Leon SoC */
};

#ifdef CONFIG_SPARC32
extern enum sparc_cpu sparc_cpu_model;

/*
 * Some 32-bit sparc implementations provide the v9 compare-and-swap
 * instruction as an extension; LEON3 does, plain v8 does not.  Whether it is
 * present decides how a user word can be made atomic, and the kernel has to
 * make the same choice userspace made: a binary built for such a cpu uses
 * casa inline, one built for v8 cannot and asks the kernel instead.  Both
 * follow from the hardware, so deciding on the hardware keeps them in step.
 *
 * Probed once on the boot cpu: an SMP machine whose cpus disagree about this
 * is not something the port supports.
 */
extern bool sparc32_has_casa;

#define SUN4M_NCPUS            4              /* Architectural limit of sun4m. */

#else

#define sparc_cpu_model sun4u

#endif

#endif /* __ASM_CPU_TYPE_H */
