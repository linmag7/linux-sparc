// SPDX-License-Identifier: GPL-2.0
/* reboot.c: reboot/shutdown/halt/poweroff handling
 *
 * Copyright (C) 2008 David S. Miller <davem@davemloft.net>
 */
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/export.h>
#include <linux/pm.h>
#include <linux/of.h>

#include <asm/emergency-restart.h>
#include <asm/oplib.h>
#include <asm/prom.h>
#include <asm/setup.h>

/* sysctl - toggle power-off restriction for serial console
 * systems in machine_power_off()
 */
int scons_pwroff = 1;

/* This isn't actually used, it exists merely to satisfy the
 * reference in kernel/sys.c
 */
void (*pm_power_off)(void) = machine_power_off;
EXPORT_SYMBOL(pm_power_off);

void machine_power_off(void)
{
	if (!of_node_is_type(of_console_device, "serial") || scons_pwroff)
		prom_halt_power_off();

	prom_halt();
}

void machine_halt(void)
{
	prom_halt();
	panic("Halt failed!");
}

static void sparc_restart(char *cmd, bool capture)
{
	char *p;

	p = strchr(reboot_command, '\n');
	if (p)
		*p = 0;
	if ((cmd && prom_reboot(cmd, capture)) ||
	    (*reboot_command && prom_reboot(reboot_command, capture)) ||
	    prom_reboot("", capture)) {
		if (capture)
			pr_emerg("Firmware reboot refused: interrupt handback incomplete\n");
		return;
	}
	panic("Reboot failed!");
}

void machine_restart(char *cmd)
{
	sparc_restart(cmd, true);
}

void machine_emergency_restart(void)
{
	/* Emergency restart cannot wait for other CPUs to respond. */
	sparc_restart(NULL, false);
}
