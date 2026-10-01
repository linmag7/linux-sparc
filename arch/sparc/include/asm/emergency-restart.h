/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC_EMERGENCY_RESTART_H
#define _SPARC_EMERGENCY_RESTART_H

#ifdef CONFIG_SPARC64
void machine_emergency_restart(void);
#else
#include <asm-generic/emergency-restart.h>
#endif

#endif
