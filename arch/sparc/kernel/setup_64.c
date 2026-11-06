// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/arch/sparc64/kernel/setup.c
 *
 *  Copyright (C) 1995,1996  David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1997       Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <asm/smp.h>
#include <linux/user.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/kdev_t.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/inet.h>
#include <linux/console.h>
#include <linux/root_dev.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/initrd.h>
#include <linux/module.h>
#include <linux/start_kernel.h>
#include <linux/workqueue.h>
#include <linux/panic.h>
#include <linux/memblock.h>
#include <uapi/linux/mount.h>

#include <asm/io.h>
#include <asm/processor.h>
#include <asm/oplib.h>
#include <asm/page.h>
#include <asm/idprom.h>
#include <asm/head.h>
#include <asm/starfire.h>
#include <asm/mmu_context.h>
#include <asm/timer.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sparc64_v.h>
#include <asm/mmu.h>
#include <asm/ns87303.h>
#include <asm/btext.h>
#include <asm/elf.h>
#include <asm/mdesc.h>
#include <asm/cacheflush.h>
#include <asm/dma.h>
#include <asm/irq.h>

#ifdef CONFIG_IP_PNP
#include <net/ipconfig.h>
#endif

#include "entry.h"
#include "kernel.h"

/* Used to synchronize accesses to NatSemi SUPER I/O chip configure
 * operations in asm/ns87303.h
 */
DEFINE_SPINLOCK(ns87303_lock);
EXPORT_SYMBOL(ns87303_lock);

static bool prom_console_has_retired __read_mostly;

bool prom_console_retired(void)
{
	/* Pair with exit after the console core has drained its writers. */
	return smp_load_acquire(&prom_console_has_retired);
}

static int prom_console_exit(struct console *con)
{
	/* Console core has drained registered writers before calling exit. */
	smp_store_release(&prom_console_has_retired, true);
	return 0;
}

static void
prom_console_write(struct console *con, const char *s, unsigned int n)
{
	prom_write(s, n);
}

/* Exported for mm/init.c:paging_init. */
unsigned long cmdline_memory_size = 0;

static struct console prom_early_console = {
	.name =		"earlyprom",
	.write =	prom_console_write,
	.exit =		prom_console_exit,
	.flags =	CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index =	-1,
};

/*
 * Process kernel command line switches that are specific to the
 * SPARC or that require special low-level processing.
 */
static void __init process_switch(char c)
{
	switch (c) {
	case 'd':
	case 's':
		break;
	case 'h':
		prom_printf("boot_flags_init: Halt!\n");
		prom_halt();
		break;
	case 'p':
		prom_early_console.flags &= ~CON_BOOT;
		break;
	case 'P':
		/* Force UltraSPARC-III P-Cache on. */
		if (tlb_type != cheetah) {
			printk("BOOT: Ignoring P-Cache force option.\n");
			break;
		}
		cheetah_pcache_forced_on = 1;
		add_taint(TAINT_MACHINE_CHECK, LOCKDEP_NOW_UNRELIABLE);
		cheetah_enable_pcache();
		break;

	default:
		printk("Unknown boot switch (-%c)\n", c);
		break;
	}
}

static void __init boot_flags_init(char *commands)
{
	while (*commands) {
		/* Move to the start of the next "argument". */
		while (*commands == ' ')
			commands++;

		/* Process any command switches, otherwise skip it. */
		if (*commands == '\0')
			break;
		if (*commands == '-') {
			commands++;
			while (*commands && *commands != ' ')
				process_switch(*commands++);
			continue;
		}
		if (!strncmp(commands, "mem=", 4))
			cmdline_memory_size = memparse(commands + 4, &commands);

		while (*commands && *commands != ' ')
			commands++;
	}
}

extern unsigned short root_flags;
extern unsigned short root_dev;
extern unsigned short ram_flags;
#define RAMDISK_IMAGE_START_MASK	0x07FF
#define RAMDISK_PROMPT_FLAG		0x8000
#define RAMDISK_LOAD_FLAG		0x4000

extern int root_mountflags;

char reboot_command[COMMAND_LINE_SIZE];

static void __init per_cpu_patch(void)
{
	struct cpuid_patch_entry *p;
	unsigned long ver;
	int is_jbus;

	if (tlb_type == sparc64_v ||
	    (tlb_type == spitfire && !this_is_starfire))
		return;

	is_jbus = 0;
	if (tlb_type != hypervisor) {
		__asm__ ("rdpr %%ver, %0" : "=r" (ver));
		is_jbus = ((ver >> 32UL) == __JALAPENO_ID ||
			   (ver >> 32UL) == __SERRANO_ID);
	}

	p = &__cpuid_patch;
	while (p < &__cpuid_patch_end) {
		unsigned long addr = p->addr;
		unsigned int *insns;

		switch (tlb_type) {
		case spitfire:
			insns = &p->starfire[0];
			break;
		case cheetah:
		case cheetah_plus:
			if (is_jbus)
				insns = &p->cheetah_jbus[0];
			else
				insns = &p->cheetah_safari[0];
			break;
		case sparc64_vii:
			insns = &p->vii[0];
			break;
		case hypervisor:
			insns = &p->sun4v[0];
			break;
		default:
			prom_printf("Unknown cpu type, halting.\n");
			prom_halt();
		}

		*(unsigned int *) (addr +  0) = insns[0];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  0));

		*(unsigned int *) (addr +  4) = insns[1];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  4));

		*(unsigned int *) (addr +  8) = insns[2];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  8));

		*(unsigned int *) (addr + 12) = insns[3];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr + 12));

		p++;
	}
}

void sun4v_patch_1insn_range(struct sun4v_1insn_patch_entry *start,
			     struct sun4v_1insn_patch_entry *end)
{
	while (start < end) {
		unsigned long addr = start->addr;

		*(unsigned int *) (addr +  0) = start->insn;
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  0));

		start++;
	}
}

void sun4v_patch_2insn_range(struct sun4v_2insn_patch_entry *start,
			     struct sun4v_2insn_patch_entry *end)
{
	while (start < end) {
		unsigned long addr = start->addr;

		*(unsigned int *) (addr +  0) = start->insns[0];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  0));

		*(unsigned int *) (addr +  4) = start->insns[1];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  4));

		start++;
	}
}

void sun_m7_patch_2insn_range(struct sun4v_2insn_patch_entry *start,
			     struct sun4v_2insn_patch_entry *end)
{
	while (start < end) {
		unsigned long addr = start->addr;

		*(unsigned int *) (addr +  0) = start->insns[0];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  0));

		*(unsigned int *) (addr +  4) = start->insns[1];
		wmb();
		__asm__ __volatile__("flush	%0" : : "r" (addr +  4));

		start++;
	}
}

static void __init sun4v_patch(void)
{
	extern void sun4v_hvapi_init(void);

	if (tlb_type != hypervisor)
		return;

	sun4v_patch_1insn_range(&__sun4v_1insn_patch,
				&__sun4v_1insn_patch_end);

	sun4v_patch_2insn_range(&__sun4v_2insn_patch,
				&__sun4v_2insn_patch_end);

	switch (sun4v_chip_type) {
	case SUN4V_CHIP_SPARC_M7:
	case SUN4V_CHIP_SPARC_M8:
	case SUN4V_CHIP_SPARC_SN:
		sun4v_patch_1insn_range(&__sun_m7_1insn_patch,
					&__sun_m7_1insn_patch_end);
		sun_m7_patch_2insn_range(&__sun_m7_2insn_patch,
					 &__sun_m7_2insn_patch_end);
		break;
	default:
		break;
	}

	if (sun4v_chip_type != SUN4V_CHIP_NIAGARA1) {
		sun4v_patch_1insn_range(&__fast_win_ctrl_1insn_patch,
					&__fast_win_ctrl_1insn_patch_end);
	}

	sun4v_hvapi_init();
}

static void __init popc_patch(void)
{
	struct popc_3insn_patch_entry *p3;
	struct popc_6insn_patch_entry *p6;

	p3 = &__popc_3insn_patch;
	while (p3 < &__popc_3insn_patch_end) {
		unsigned long i, addr = p3->addr;

		for (i = 0; i < 3; i++) {
			*(unsigned int *) (addr +  (i * 4)) = p3->insns[i];
			wmb();
			__asm__ __volatile__("flush	%0"
					     : : "r" (addr +  (i * 4)));
		}

		p3++;
	}

	p6 = &__popc_6insn_patch;
	while (p6 < &__popc_6insn_patch_end) {
		unsigned long i, addr = p6->addr;

		for (i = 0; i < 6; i++) {
			*(unsigned int *) (addr +  (i * 4)) = p6->insns[i];
			wmb();
			__asm__ __volatile__("flush	%0"
					     : : "r" (addr +  (i * 4)));
		}

		p6++;
	}
}

static void __init pause_patch(void)
{
	struct pause_patch_entry *p;

	p = &__pause_3insn_patch;
	while (p < &__pause_3insn_patch_end) {
		unsigned long i, addr = p->addr;

		for (i = 0; i < 3; i++) {
			*(unsigned int *) (addr +  (i * 4)) = p->insns[i];
			wmb();
			__asm__ __volatile__("flush	%0"
					     : : "r" (addr +  (i * 4)));
		}

		p++;
	}
}

/*
 * Fujitsu restrainable ECC uses CPU-specific C handlers; other errors halt.
 * Avoid Spitfire error registers on both Fujitsu implementations.
 */
static void __init fujitsu_patch_error_traps(void (*handler)(void))
{
	static const unsigned int traps[] = { 0x0a, 0x32, 0x40, 0x63 };
	unsigned int *tables[] = { sparc64_ttable_tl0, sparc64_ttable_tl1 };
	int i, j;

	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		for (j = 0; j < ARRAY_SIZE(traps); j++) {
			unsigned int *slot = tables[i] + traps[j] * 8;
			void (*target)(void) = handler;
			long delta;

			if (tlb_type == sparc64_vii && traps[j] == 0x63)
				target = i ? sparc64_vii_ecc_trap_tl1 :
					     sparc64_vii_ecc_trap;
			if (tlb_type == sparc64_v && traps[j] == 0x63)
				target = i ? sparc64_v_ecc_trap_tl1 : sparc64_v_ecc_trap;
#ifdef CONFIG_PCI
			if (tlb_type == sparc64_v && traps[j] == 0x32 && !i)
				target = pp250_u2u_dae;
#endif
			delta = (long)target - (long)slot;

			/* ba (disp22), followed by nop. */
			if (delta < -(1L << 23) || delta >= (1L << 23)) {
				prom_printf("VII: error-trap handler is out of branch range.\n");
				prom_halt();
			}
			slot[1] = 0x01000000;
			slot[0] = 0x10800000 | ((delta >> 2) & 0x3fffff);
			/* Publish the trap instructions before synchronizing fetch. */
			wmb();
			__asm__ __volatile__("flush %0" : : "r" (slot));
		}
	}
}

static void __init sparc64_v_check_boot(void)
{
	char name[sizeof("FJSV,GPUZC-M") + 1] = {};
	char model[sizeof("250") + 1] = {};
	unsigned long ver, dcu, mcntl, upa, ext;
	unsigned int offset;

	/* The supported firmware identifies this board by root name and model. */
	prom_getstring(prom_root_node, "name", name, sizeof(name) - 1);
	prom_getstring(prom_root_node, "model", model, sizeof(model) - 1);
	if (strcmp(prom_root_compatible, "sun4us") ||
	    strcmp(name, "FJSV,GPUZC-M") || strcmp(model, "250")) {
		prom_printf("SPARC64 V: unsupported platform; expected PRIMEPOWER 250\n");
		prom_halt();
	}

	__asm__ __volatile__("rdpr %%ver, %0" : "=r" (ver));
	__asm__ __volatile__("ldxa [%%g0] 0x45, %0" : "=r" (dcu));
	__asm__ __volatile__("ldxa [%1] 0x45, %0"
			     : "=r" (mcntl) : "r" (8UL));
	__asm__ __volatile__("ldxa [%%g0] 0x4a, %0" : "=r" (upa));

	/* V F.10.1: preserve supported firmware MMU modes. */
	if ((mcntl & SPARC64_V_MCNTL_UNSUPPORTED) ||
	    (mcntl & SPARC64_V_MCNTL_RMD_MASK) != SPARC64_V_MCNTL_RMD_2 ||
	    (dcu & SPARC64_V_DCU_MMU_ENABLE) != SPARC64_V_DCU_MMU_ENABLE ||
	    (dcu & SPARC64_V_DCU_WEAK_SPCA) ||
	    ((upa >> SPARC64_V_UPA_AM_SHIFT) & 3) > 1 ||
	    ((upa >> SPARC64_V_UPA_MID_SHIFT) & 0x1f)) {
		prom_printf("SPARC64-V: VER=%lx DCUCR=%lx MCNTL=%lx UPA=%lx\n",
			    ver, dcu, mcntl, upa);
		prom_printf("SPARC64-V: unsupported controls or nonzero boot MID\n");
		prom_halt();
	}
	/*
	 * Physical TSB bases must not be XORed with inherited extensions.
	 * JPS1 Commonality F.10.7 defines extensions for both MMUs.
	 */
	for (offset = TSB_EXTENSION_P; offset <= TSB_EXTENSION_N; offset += 8) {
		__asm__ __volatile__("ldxa [%1] %2, %0"
				     : "=r" (ext) : "r" (offset), "i" (ASI_DMMU));
		if (ext) {
			prom_printf("SPARC64-V: D TSB extension %x=%lx unsupported\n",
				    offset, ext);
			prom_halt();
		}
		if (offset == TSB_EXTENSION_S)
			continue;
		__asm__ __volatile__("ldxa [%1] %2, %0"
				     : "=r" (ext) : "r" (offset), "i" (ASI_IMMU));
		if (ext) {
			prom_printf("SPARC64-V: I TSB extension %x=%lx unsupported\n",
				    offset, ext);
			prom_halt();
		}
	}
	/* V F.5 and Appendix P: IAE, DAE, ADE and ECC error. */
	fujitsu_patch_error_traps(sparc64_v_fatal_trap);
}

void __init start_early_boot(void)
{
	int cpu;

	if (tlb_type == sparc64_v)
		sparc64_v_check_boot();
	check_if_starfire();
	per_cpu_patch();
	sun4v_patch();
	smp_init_cpu_poke();

	if (tlb_type == sparc64_vii) {
		unsigned long mcntl;

		fujitsu_patch_error_traps(sparc64_vii_fatal_trap);

		__asm__ __volatile__("ldxa [%1] 0x45, %0"
				     : "=r" (mcntl) : "r" (8UL));
		/*
		 * Reject context hashing, forced instruction caching of noncacheable
		 * addresses and multiple-page-size contexts (VII Table F-4).
		 * Preserve firmware state; stop if a mode transition is required.
		 */
		if (mcntl & 0x101c0UL) {
			prom_printf("VII-BOOT: incompatible inherited MCNTL; halted\n");
			prom_halt();
		}
	}

	cpu = hard_smp_processor_id();
	if (cpu >= NR_CPUS) {
		prom_printf("Serious problem, boot cpu id (%d) >= NR_CPUS (%d)\n",
			    cpu, NR_CPUS);
		prom_halt();
	}
	current_thread_info()->cpu = cpu;

	time_init_early();
	prom_init_report();
	start_kernel();
}

/* On Ultra, we support all of the v8 capabilities. */
unsigned long sparc64_elf_hwcap = (HWCAP_SPARC_FLUSH | HWCAP_SPARC_STBAR |
				   HWCAP_SPARC_SWAP | HWCAP_SPARC_MULDIV |
				   HWCAP_SPARC_V9);
EXPORT_SYMBOL(sparc64_elf_hwcap);

static const char *hwcaps[] = {
	"flush", "stbar", "swap", "muldiv", "v9",
	"ultra3", "blkinit", "n2",

	/* These strings are as they appear in the machine description
	 * 'hwcap-list' property for cpu nodes.
	 */
	"mul32", "div32", "fsmuld", "v8plus", "popc", "vis", "vis2",
	"ASIBlkInit", "fmaf", "vis3", "hpc", "random", "trans", "fjfmau",
	"ima", "cspare", "pause", "cbcond", NULL /*reserved for crypto */,
	"adp",
};

static const char *crypto_hwcaps[] = {
	"aes", "des", "kasumi", "camellia", "md5", "sha1", "sha256",
	"sha512", "mpmul", "montmul", "montsqr", "crc32c",
};

void cpucap_info(struct seq_file *m)
{
	unsigned long caps = sparc64_elf_hwcap;
	int i, printed = 0;

	seq_puts(m, "cpucaps\t\t: ");
	for (i = 0; i < ARRAY_SIZE(hwcaps); i++) {
		unsigned long bit = 1UL << i;
		if (hwcaps[i] && (caps & bit)) {
			seq_printf(m, "%s%s",
				   printed ? "," : "", hwcaps[i]);
			printed++;
		}
	}
	if (caps & HWCAP_SPARC_CRYPTO) {
		unsigned long cfr;

		__asm__ __volatile__("rd %%asr26, %0" : "=r" (cfr));
		for (i = 0; i < ARRAY_SIZE(crypto_hwcaps); i++) {
			unsigned long bit = 1UL << i;
			if (cfr & bit) {
				seq_printf(m, "%s%s",
					   printed ? "," : "", crypto_hwcaps[i]);
				printed++;
			}
		}
	}
	seq_putc(m, '\n');
}

static void __init report_one_hwcap(int *printed, const char *name)
{
	if ((*printed) == 0)
		printk(KERN_INFO "CPU CAPS: [");
	printk(KERN_CONT "%s%s",
	       (*printed) ? "," : "", name);
	if (++(*printed) == 8) {
		printk(KERN_CONT "]\n");
		*printed = 0;
	}
}

static void __init report_crypto_hwcaps(int *printed)
{
	unsigned long cfr;
	int i;

	__asm__ __volatile__("rd %%asr26, %0" : "=r" (cfr));

	for (i = 0; i < ARRAY_SIZE(crypto_hwcaps); i++) {
		unsigned long bit = 1UL << i;
		if (cfr & bit)
			report_one_hwcap(printed, crypto_hwcaps[i]);
	}
}

static void __init report_hwcaps(unsigned long caps)
{
	int i, printed = 0;

	for (i = 0; i < ARRAY_SIZE(hwcaps); i++) {
		unsigned long bit = 1UL << i;
		if (hwcaps[i] && (caps & bit))
			report_one_hwcap(&printed, hwcaps[i]);
	}
	if (caps & HWCAP_SPARC_CRYPTO)
		report_crypto_hwcaps(&printed);
	if (printed != 0)
		printk(KERN_CONT "]\n");
}

static unsigned long __init mdesc_cpu_hwcap_list(void)
{
	struct mdesc_handle *hp;
	unsigned long caps = 0;
	const char *prop;
	int len;
	u64 pn;

	hp = mdesc_grab();
	if (!hp)
		return 0;

	pn = mdesc_node_by_name(hp, MDESC_NODE_NULL, "cpu");
	if (pn == MDESC_NODE_NULL)
		goto out;

	prop = mdesc_get_property(hp, pn, "hwcap-list", &len);
	if (!prop)
		goto out;

	while (len) {
		int i, plen;

		for (i = 0; i < ARRAY_SIZE(hwcaps); i++) {
			unsigned long bit = 1UL << i;

			if (hwcaps[i] && !strcmp(prop, hwcaps[i])) {
				caps |= bit;
				break;
			}
		}
		for (i = 0; i < ARRAY_SIZE(crypto_hwcaps); i++) {
			if (!strcmp(prop, crypto_hwcaps[i]))
				caps |= HWCAP_SPARC_CRYPTO;
		}

		plen = strlen(prop) + 1;
		prop += plen;
		len -= plen;
	}

out:
	mdesc_release(hp);
	return caps;
}

/* This yields a mask that user programs can use to figure out what
 * instruction set this cpu supports.
 */
static void __init init_sparc64_elf_hwcap(void)
{
	unsigned long cap = sparc64_elf_hwcap;
	unsigned long mdesc_caps;

	if (tlb_type == cheetah || tlb_type == cheetah_plus)
		cap |= HWCAP_SPARC_ULTRA3;
	else if (tlb_type == hypervisor) {
		if (sun4v_chip_type == SUN4V_CHIP_NIAGARA1 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA2 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA3 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA4 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA5 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M6 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M7 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M8 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_SN ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC64X)
			cap |= HWCAP_SPARC_BLKINIT;
		if (sun4v_chip_type == SUN4V_CHIP_NIAGARA2 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA3 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA4 ||
		    sun4v_chip_type == SUN4V_CHIP_NIAGARA5 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M6 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M7 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_M8 ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC_SN ||
		    sun4v_chip_type == SUN4V_CHIP_SPARC64X)
			cap |= HWCAP_SPARC_N2;
	}

	cap |= (AV_SPARC_MUL32 | AV_SPARC_DIV32 | AV_SPARC_V8PLUS);

	mdesc_caps = mdesc_cpu_hwcap_list();
	if (!mdesc_caps) {
		if (tlb_type == spitfire)
			cap |= AV_SPARC_VIS;
		if (tlb_type == cheetah || tlb_type == cheetah_plus)
			cap |= AV_SPARC_VIS | AV_SPARC_VIS2;
		if (tlb_type == cheetah_plus) {
			unsigned long impl, ver;

			__asm__ __volatile__("rdpr %%ver, %0" : "=r" (ver));
			impl = ((ver >> 32) & 0xffff);
			if (impl == PANTHER_IMPL)
				cap |= AV_SPARC_POPC;
		}
		if (tlb_type == hypervisor) {
			if (sun4v_chip_type == SUN4V_CHIP_NIAGARA1)
				cap |= AV_SPARC_ASI_BLK_INIT;
			if (sun4v_chip_type == SUN4V_CHIP_NIAGARA2 ||
			    sun4v_chip_type == SUN4V_CHIP_NIAGARA3 ||
			    sun4v_chip_type == SUN4V_CHIP_NIAGARA4 ||
			    sun4v_chip_type == SUN4V_CHIP_NIAGARA5 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M6 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M7 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M8 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_SN ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC64X)
				cap |= (AV_SPARC_VIS | AV_SPARC_VIS2 |
					AV_SPARC_ASI_BLK_INIT |
					AV_SPARC_POPC);
			if (sun4v_chip_type == SUN4V_CHIP_NIAGARA3 ||
			    sun4v_chip_type == SUN4V_CHIP_NIAGARA4 ||
			    sun4v_chip_type == SUN4V_CHIP_NIAGARA5 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M6 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M7 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_M8 ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC_SN ||
			    sun4v_chip_type == SUN4V_CHIP_SPARC64X)
				cap |= (AV_SPARC_VIS3 | AV_SPARC_HPC |
					AV_SPARC_FMAF);
		}
	}
	sparc64_elf_hwcap = cap | mdesc_caps;

	report_hwcaps(sparc64_elf_hwcap);

	if (sparc64_elf_hwcap & AV_SPARC_POPC)
		popc_patch();
	if (sparc64_elf_hwcap & AV_SPARC_PAUSE)
		pause_patch();
}

static void __init alloc_irqstack_bootmem(void)
{
	unsigned int i, node;

	for_each_possible_cpu(i) {
		node = cpu_to_node(i);

		softirq_stack[i] = memblock_alloc_node(THREAD_SIZE,
						       THREAD_SIZE, node);
		if (!softirq_stack[i])
			panic("%s: Failed to allocate %lu bytes align=%lx nid=%d\n",
			      __func__, THREAD_SIZE, THREAD_SIZE, node);
		hardirq_stack[i] = memblock_alloc_node(THREAD_SIZE,
						       THREAD_SIZE, node);
		if (!hardirq_stack[i])
			panic("%s: Failed to allocate %lu bytes align=%lx nid=%d\n",
			      __func__, THREAD_SIZE, THREAD_SIZE, node);
	}
}

void __init setup_arch(char **cmdline_p)
{
	/* Initialize PROM console and command line. */
	*cmdline_p = prom_getbootargs();
	strscpy(boot_command_line, *cmdline_p, COMMAND_LINE_SIZE);
	parse_early_param();

	boot_flags_init(*cmdline_p);
#ifdef CONFIG_EARLYFB
	if (btext_find_display())
#endif
		register_console(&prom_early_console);

	if (tlb_type == hypervisor)
		pr_info("ARCH: SUN4V\n");
	else
		pr_info("ARCH: SUN4U\n");

	idprom_init();

	if (!root_flags)
		root_mountflags &= ~MS_RDONLY;
	ROOT_DEV = old_decode_dev(root_dev);
#ifdef CONFIG_BLK_DEV_RAM
	rd_image_start = ram_flags & RAMDISK_IMAGE_START_MASK;
#endif

#ifdef CONFIG_IP_PNP
	if (!ic_set_manually) {
		phandle chosen = prom_finddevice("/chosen");
		u32 cl, sv, gw;

		cl = prom_getintdefault (chosen, "client-ip", 0);
		sv = prom_getintdefault (chosen, "server-ip", 0);
		gw = prom_getintdefault (chosen, "gateway-ip", 0);
		if (cl && sv) {
			ic_myaddr = cl;
			ic_servaddr = sv;
			if (gw)
				ic_gateway = gw;
#if defined(CONFIG_IP_PNP_BOOTP) || defined(CONFIG_IP_PNP_RARP)
			ic_proto_enabled = 0;
#endif
		}
	}
#endif

	/* Get boot processor trap_block[] setup.  */
	init_cur_cpu_trap(current_thread_info());

	paging_init();
	init_sparc64_elf_hwcap();
	/*
	 * Once the OF device tree and MDESC have been setup and nr_cpus has
	 * been parsed, we know the list of possible cpus.  Therefore we can
	 * allocate the IRQ stacks.
	 */
	alloc_irqstack_bootmem();
}

extern int stop_a_enabled;

static atomic_t primepower_break_pending = ATOMIC_INIT(0);

static void primepower_break_workfn(struct work_struct *work)
{
	int ret;

	if (READ_ONCE(stop_a_enabled) && system_state == SYSTEM_RUNNING &&
	    !oops_in_progress && !panic_in_progress()) {
		flush_user_windows();
		ret = prom_cmdline();
		if (ret)
			pr_warn_ratelimited("PRIMEPOWER: firmware BREAK refused (%d)\n", ret);
	}
	atomic_set(&primepower_break_pending, 0);
}

static DECLARE_WORK(primepower_break_work, primepower_break_workfn);

void sun_do_break(void)
{
	if (!READ_ONCE(stop_a_enabled))
		return;
	if (primepower_irq_supported() && prom_console_retired()) {
		if (in_nmi() || system_state != SYSTEM_RUNNING ||
		    oops_in_progress || panic_in_progress())
			return;
		/* The serial interrupt still owns its port lock at this point. */
		if (!atomic_cmpxchg(&primepower_break_pending, 0, 1))
			schedule_work(&primepower_break_work);
		return;
	}

	flush_user_windows();

	prom_cmdline();
}
EXPORT_SYMBOL(sun_do_break);

void sparc_panic_exit(void)
{
	if (primepower_irq_supported() && prom_console_retired()) {
		WRITE_ONCE(stop_a_enabled, 0);
		pr_emerg("PRIMEPOWER: firmware recovery unavailable; use an external reset\n");
		return;
	}
	stop_a_enabled = 1;
	pr_emerg("Press Stop-A (L1-A) from sun keyboard or send break\n"
		 "twice on console to return to the boot prom\n");
}

int stop_a_enabled = 1;
EXPORT_SYMBOL(stop_a_enabled);
