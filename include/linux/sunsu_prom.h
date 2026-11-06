/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SUNSU_PROM_H
#define _LINUX_SUNSU_PROM_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device_node;
struct sunsu_prom_handoff;

typedef int (*sunsu_prom_call_t)(struct sunsu_prom_handoff *state, void *arg);

#if defined(CONFIG_SPARC64) && IS_BUILTIN(CONFIG_SERIAL_SUNSU) && \
	defined(CONFIG_SERIAL_SUNSU_CONSOLE)
/* Process context only, without held spinlocks or disabled preemption/BHs. */
int sunsu_prom_handoff(struct device_node *input, struct device_node *output,
		       sunsu_prom_call_t call, void *arg);
int sunsu_prom_begin(struct sunsu_prom_handoff *state);
int sunsu_prom_end(struct sunsu_prom_handoff *state);
#else
static inline int sunsu_prom_handoff(struct device_node *input,
				     struct device_node *output,
				     sunsu_prom_call_t call, void *arg)
{
	return -EOPNOTSUPP;
}

static inline int sunsu_prom_begin(struct sunsu_prom_handoff *state)
{
	return -EOPNOTSUPP;
}

static inline int sunsu_prom_end(struct sunsu_prom_handoff *state)
{
	return -EOPNOTSUPP;
}
#endif
#endif
