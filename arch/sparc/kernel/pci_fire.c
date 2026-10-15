// SPDX-License-Identifier: GPL-2.0
/* pci_fire.c: Sun4u platform PCI-E controller support.
 *
 * Copyright (C) 2007 David S. Miller (davem@davemloft.net)
 */
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/msi.h>
#include <linux/export.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/numa.h>
#include <linux/mutex.h>

#include <asm/prom.h>
#include <asm/irq.h>
#include <asm/upa.h>

#include "pci_impl.h"

#define DRIVER_NAME	"fire"
#define PFX		DRIVER_NAME ": "

#define FIRE_IOMMU_CONTROL	0x40000UL
#define FIRE_IOMMU_TSBBASE	0x40008UL
#define FIRE_IOMMU_FLUSH	0x40100UL
#define FIRE_IOMMU_FLUSHINV	0x40108UL

static int pci_fire_pbm_iommu_init(struct pci_pbm_info *pbm)
{
	struct iommu *iommu = pbm->iommu;
	u32 vdma[2], dma_mask;
	u64 control;
	int tsbsize, err;

	/* No virtual-dma property on these guys, use largest size.  */
	vdma[0] = 0xc0000000; /* base */
	vdma[1] = 0x40000000; /* size */
	dma_mask = 0xffffffff;
	tsbsize = 128;

	/* Register addresses. */
	iommu->iommu_control  = pbm->pbm_regs + FIRE_IOMMU_CONTROL;
	iommu->iommu_tsbbase  = pbm->pbm_regs + FIRE_IOMMU_TSBBASE;
	iommu->iommu_flush    = pbm->pbm_regs + FIRE_IOMMU_FLUSH;
	iommu->iommu_flushinv = pbm->pbm_regs + FIRE_IOMMU_FLUSHINV;
	if (pbm->is_oberon) {
		/*
		 * 0x40100 flushes an IOTSB cache line by physical address;
		 * 0x40010 flushes a translated DVMA page.
		 */
		iommu->iommu_flush = pbm->pbm_regs + 0x40010UL;
		iommu->iommu_cache_flush = pbm->pbm_regs + 0x40100UL;
		iommu->write_complete_reg = iommu->iommu_control;
	}

	/* We use the main control/status register of FIRE as the write
	 * completion register.
	 */
	if (!iommu->iommu_cache_flush)
		iommu->write_complete_reg = pbm->controller_regs + 0x410000UL;

	/*
	 * Invalidate TLB Entries.
	 */
	upa_writeq(~(u64)0, iommu->iommu_flushinv);

	err = iommu_table_init(iommu, tsbsize * 8 * 1024, vdma[0], dma_mask,
			       pbm->numa_node);
	if (err)
		return err;

	/* Make initialized IOPTEs visible before publishing the table base. */
	wmb();
	upa_writeq(__pa(iommu->page_table) | 0x7UL, iommu->iommu_tsbbase);

	control = upa_readq(iommu->iommu_control);
	control |= (0x00000400 /* TSB cache snoop enable */	|
		    0x00000300 /* Cache mode */			|
		    0x00000002 /* Bypass enable */		|
		    0x00000001 /* Translation enable */);
	upa_writeq(control, iommu->iommu_control);
	if (iommu->iommu_cache_flush) {
		upa_writeq(~(u64)0, iommu->iommu_flushinv);
		(void)upa_readq(iommu->iommu_control);
	}

	return 0;
}

#ifdef CONFIG_PCI_MSI
struct pci_msiq_entry {
	u64		word0;
#define MSIQ_WORD0_RESV			0x8000000000000000UL
#define MSIQ_WORD0_FMT_TYPE		0x7f00000000000000UL
#define MSIQ_WORD0_FMT_TYPE_SHIFT	56
#define MSIQ_WORD0_LEN			0x00ffc00000000000UL
#define MSIQ_WORD0_LEN_SHIFT		46
#define MSIQ_WORD0_ADDR0		0x00003fff00000000UL
#define MSIQ_WORD0_ADDR0_SHIFT		32
#define MSIQ_WORD0_RID			0x00000000ffff0000UL
#define MSIQ_WORD0_RID_SHIFT		16
#define MSIQ_WORD0_DATA0		0x000000000000ffffUL
#define MSIQ_WORD0_DATA0_SHIFT		0

#define MSIQ_TYPE_MSG			0x6
#define MSIQ_TYPE_MSI32			0xb
#define MSIQ_TYPE_MSI64			0xf

	u64		word1;
#define MSIQ_WORD1_ADDR1		0xffffffffffff0000UL
#define MSIQ_WORD1_ADDR1_SHIFT		16
#define MSIQ_WORD1_DATA1		0x000000000000ffffUL
#define MSIQ_WORD1_DATA1_SHIFT		0

	u64		resv[6];
};

/* All MSI registers are offset from pbm->pbm_regs */
#define EVENT_QUEUE_BASE_ADDR_REG	0x010000UL
#define  EVENT_QUEUE_BASE_ADDR_ALL_ONES	0xfffc000000000000UL

#define EVENT_QUEUE_CONTROL_SET(EQ)	(0x011000UL + (EQ) * 0x8UL)
#define  EVENT_QUEUE_CONTROL_SET_OFLOW	0x0200000000000000UL
#define  EVENT_QUEUE_CONTROL_SET_EN	0x0000100000000000UL

#define EVENT_QUEUE_CONTROL_CLEAR(EQ)	(0x011200UL + (EQ) * 0x8UL)
#define  EVENT_QUEUE_CONTROL_CLEAR_OF	0x0200000000000000UL
#define  EVENT_QUEUE_CONTROL_CLEAR_E2I	0x0000800000000000UL
#define  EVENT_QUEUE_CONTROL_CLEAR_DIS	0x0000100000000000UL

#define EVENT_QUEUE_STATE(EQ)		(0x011400UL + (EQ) * 0x8UL)
#define  EVENT_QUEUE_STATE_MASK		0x0000000000000007UL
#define  EVENT_QUEUE_STATE_IDLE		0x0000000000000001UL
#define  EVENT_QUEUE_STATE_ACTIVE	0x0000000000000002UL
#define  EVENT_QUEUE_STATE_ERROR	0x0000000000000004UL

#define EVENT_QUEUE_TAIL(EQ)		(0x011600UL + (EQ) * 0x8UL)
#define  EVENT_QUEUE_TAIL_OFLOW		0x0200000000000000UL
#define  EVENT_QUEUE_TAIL_VAL		0x000000000000007fUL

#define EVENT_QUEUE_HEAD(EQ)		(0x011800UL + (EQ) * 0x8UL)
#define  EVENT_QUEUE_HEAD_VAL		0x000000000000007fUL

#define MSI_MAP(MSI)			(0x020000UL + (MSI) * 0x8UL)
#define  MSI_MAP_VALID			0x8000000000000000UL
#define  MSI_MAP_EQWR_N			0x4000000000000000UL
#define  MSI_MAP_EQNUM			0x000000000000003fUL

#define MSI_CLEAR(MSI)			(0x028000UL + (MSI) * 0x8UL)
#define  MSI_CLEAR_EQWR_N		0x4000000000000000UL

#define IMONDO_DATA0			0x02C000UL
#define  IMONDO_DATA0_DATA		0xffffffffffffffc0UL

#define IMONDO_DATA1			0x02C008UL
#define  IMONDO_DATA1_DATA		0xffffffffffffffffUL

#define MSI_32BIT_ADDR			0x034000UL
#define  MSI_32BIT_ADDR_VAL		0x00000000ffff0000UL

#define MSI_64BIT_ADDR			0x034008UL
#define  MSI_64BIT_ADDR_VAL		0xffffffffffff0000UL

static int pci_fire_get_head(struct pci_pbm_info *pbm, unsigned long msiqid,
			     unsigned long *head)
{
	*head = upa_readq(pbm->pbm_regs + EVENT_QUEUE_HEAD(msiqid));
	return 0;
}

static int pci_fire_dequeue_msi(struct pci_pbm_info *pbm, unsigned long msiqid,
				unsigned long *head, unsigned long *msi)
{
	unsigned long type_fmt, type, msi_num;
	struct pci_msiq_entry *base, *ep;

	base = (pbm->msi_queues + ((msiqid - pbm->msiq_first) * 8192));
	ep = &base[*head];

	if ((ep->word0 & MSIQ_WORD0_FMT_TYPE) == 0)
		return 0;

	type_fmt = ((ep->word0 & MSIQ_WORD0_FMT_TYPE) >>
		    MSIQ_WORD0_FMT_TYPE_SHIFT);
	type = (type_fmt >> 3);
	if (unlikely(type != MSIQ_TYPE_MSI32 &&
		     type != MSIQ_TYPE_MSI64))
		return -EINVAL;

	*msi = msi_num = ((ep->word0 & MSIQ_WORD0_DATA0) >>
			  MSIQ_WORD0_DATA0_SHIFT);

	upa_writeq(MSI_CLEAR_EQWR_N, pbm->pbm_regs + MSI_CLEAR(msi_num));

	/* Clear the entry.  */
	ep->word0 &= ~MSIQ_WORD0_FMT_TYPE;

	/* Go to next entry in ring.  */
	(*head)++;
	if (*head >= pbm->msiq_ent_count)
		*head = 0;

	return 1;
}

static int pci_fire_set_head(struct pci_pbm_info *pbm, unsigned long msiqid,
			     unsigned long head)
{
	upa_writeq(head, pbm->pbm_regs + EVENT_QUEUE_HEAD(msiqid));
	return 0;
}

static int pci_fire_msi_setup(struct pci_pbm_info *pbm, unsigned long msiqid,
			      unsigned long msi, int is_msi64)
{
	u64 val;

	val = upa_readq(pbm->pbm_regs + MSI_MAP(msi));
	val &= ~(MSI_MAP_EQNUM);
	val |= msiqid;
	upa_writeq(val, pbm->pbm_regs + MSI_MAP(msi));

	upa_writeq(MSI_CLEAR_EQWR_N, pbm->pbm_regs + MSI_CLEAR(msi));

	val = upa_readq(pbm->pbm_regs + MSI_MAP(msi));
	val |= MSI_MAP_VALID;
	upa_writeq(val, pbm->pbm_regs + MSI_MAP(msi));

	return 0;
}

static int pci_fire_msi_teardown(struct pci_pbm_info *pbm, unsigned long msi)
{
	u64 val;

	val = upa_readq(pbm->pbm_regs + MSI_MAP(msi));

	val &= ~MSI_MAP_VALID;

	upa_writeq(val, pbm->pbm_regs + MSI_MAP(msi));

	return 0;
}

static int pci_fire_msiq_alloc(struct pci_pbm_info *pbm)
{
	unsigned long pages, order, i;

	order = get_order(512 * 1024);
	pages = __get_free_pages(GFP_KERNEL | __GFP_COMP, order);
	if (pages == 0UL) {
		printk(KERN_ERR "MSI: Cannot allocate MSI queues (o=%lu).\n",
		       order);
		return -ENOMEM;
	}
	memset((char *)pages, 0, PAGE_SIZE << order);
	pbm->msi_queues = (void *) pages;

	upa_writeq((EVENT_QUEUE_BASE_ADDR_ALL_ONES |
		    __pa(pbm->msi_queues)),
		   pbm->pbm_regs + EVENT_QUEUE_BASE_ADDR_REG);

	upa_writeq(pbm->portid << 6, pbm->pbm_regs + IMONDO_DATA0);
	upa_writeq(0, pbm->pbm_regs + IMONDO_DATA1);

	upa_writeq(pbm->msi32_start, pbm->pbm_regs + MSI_32BIT_ADDR);
	upa_writeq(pbm->msi64_start, pbm->pbm_regs + MSI_64BIT_ADDR);

	for (i = 0; i < pbm->msiq_num; i++) {
		upa_writeq(0, pbm->pbm_regs + EVENT_QUEUE_HEAD(i));
		upa_writeq(0, pbm->pbm_regs + EVENT_QUEUE_TAIL(i));
	}

	return 0;
}

static void pci_fire_msiq_free(struct pci_pbm_info *pbm)
{
	unsigned long pages, order;

	order = get_order(512 * 1024);
	pages = (unsigned long) pbm->msi_queues;

	free_pages(pages, order);

	pbm->msi_queues = NULL;
}

static int pci_fire_msiq_build_irq(struct pci_pbm_info *pbm,
				   unsigned long msiqid,
				   unsigned long devino)
{
	unsigned long cregs = (unsigned long) pbm->pbm_regs;
	unsigned long imap_reg, iclr_reg, int_ctrlr;
	unsigned int irq;
	int fixup;
	u64 val;

	imap_reg = cregs + (0x001000UL + (devino * 0x08UL));
	iclr_reg = cregs + (0x001400UL + (devino * 0x08UL));

	/* XXX iterate amongst the 4 IRQ controllers XXX */
	int_ctrlr = (1UL << 6);

	val = upa_readq(imap_reg);
	val |= (1UL << 63) | int_ctrlr;
	upa_writeq(val, imap_reg);

	fixup = ((pbm->portid << 6) | devino) - int_ctrlr;

	irq = build_irq(fixup, iclr_reg, imap_reg);
	if (!irq)
		return -ENOMEM;

	upa_writeq(EVENT_QUEUE_CONTROL_SET_EN,
		   pbm->pbm_regs + EVENT_QUEUE_CONTROL_SET(msiqid));

	return irq;
}

static const struct sparc64_msiq_ops pci_fire_msiq_ops = {
	.get_head	=	pci_fire_get_head,
	.dequeue_msi	=	pci_fire_dequeue_msi,
	.set_head	=	pci_fire_set_head,
	.msi_setup	=	pci_fire_msi_setup,
	.msi_teardown	=	pci_fire_msi_teardown,
	.msiq_alloc	=	pci_fire_msiq_alloc,
	.msiq_free	=	pci_fire_msiq_free,
	.msiq_build_irq	=	pci_fire_msiq_build_irq,
};

static void pci_fire_msi_init(struct pci_pbm_info *pbm)
{
	sparc64_pbm_msi_init(pbm, &pci_fire_msiq_ops);
}
#else /* CONFIG_PCI_MSI */
static void pci_fire_msi_init(struct pci_pbm_info *pbm)
{
}
#endif /* !(CONFIG_PCI_MSI) */

/* Based at pbm->controller_regs */
#define FIRE_PARITY_CONTROL	0x470010UL
#define  FIRE_PARITY_ENAB	0x8000000000000000UL
#define FIRE_FATAL_RESET_CTL	0x471028UL
#define  FIRE_FATAL_RESET_SPARE	0x0000000004000000UL
#define  FIRE_FATAL_RESET_MB	0x0000000002000000UL
#define  FIRE_FATAL_RESET_CPE	0x0000000000008000UL
#define  FIRE_FATAL_RESET_APE	0x0000000000004000UL
#define  FIRE_FATAL_RESET_PIO	0x0000000000000040UL
#define  FIRE_FATAL_RESET_JW	0x0000000000000004UL
#define  FIRE_FATAL_RESET_JI	0x0000000000000002UL
#define  FIRE_FATAL_RESET_JR	0x0000000000000001UL
#define FIRE_CORE_INTR_ENABLE	0x471800UL

/* Based at pbm->pbm_regs */
#define FIRE_TLU_CTRL		0x80000UL
#define  FIRE_TLU_CTRL_TIM	0x00000000da000000UL
#define  FIRE_TLU_CTRL_QDET	0x0000000000000100UL
#define  FIRE_TLU_CTRL_CFG	0x0000000000000001UL
#define FIRE_TLU_DEV_CTRL	0x90008UL
#define FIRE_TLU_LINK_CTRL	0x90020UL
#define FIRE_TLU_LINK_CTRL_CLK	0x0000000000000040UL
#define FIRE_LPU_RESET		0xe2008UL
#define FIRE_LPU_LLCFG		0xe2200UL
#define  FIRE_LPU_LLCFG_VC0	0x0000000000000100UL
#define FIRE_LPU_FCTRL_UCTRL	0xe2240UL
#define  FIRE_LPU_FCTRL_UCTRL_N	0x0000000000000002UL
#define  FIRE_LPU_FCTRL_UCTRL_P	0x0000000000000001UL
#define FIRE_LPU_TXL_FIFOP	0xe2430UL
#define FIRE_LPU_LTSSM_CFG2	0xe2788UL
#define FIRE_LPU_LTSSM_CFG3	0xe2790UL
#define FIRE_LPU_LTSSM_CFG4	0xe2798UL
#define FIRE_LPU_LTSSM_CFG5	0xe27a0UL
#define FIRE_DMC_IENAB		0x31800UL
#define FIRE_DMC_DBG_SEL_A	0x53000UL
#define FIRE_DMC_DBG_SEL_B	0x53008UL
#define FIRE_PEC_IENAB		0x51800UL

static void pci_fire_hw_init(struct pci_pbm_info *pbm)
{
	u64 val;

	upa_writeq(FIRE_PARITY_ENAB,
		   pbm->controller_regs + FIRE_PARITY_CONTROL);

	upa_writeq((FIRE_FATAL_RESET_SPARE |
		    FIRE_FATAL_RESET_MB |
		    FIRE_FATAL_RESET_CPE |
		    FIRE_FATAL_RESET_APE |
		    FIRE_FATAL_RESET_PIO |
		    FIRE_FATAL_RESET_JW |
		    FIRE_FATAL_RESET_JI |
		    FIRE_FATAL_RESET_JR),
		   pbm->controller_regs + FIRE_FATAL_RESET_CTL);

	upa_writeq(~(u64)0, pbm->controller_regs + FIRE_CORE_INTR_ENABLE);

	val = upa_readq(pbm->pbm_regs + FIRE_TLU_CTRL);
	val |= (FIRE_TLU_CTRL_TIM |
		FIRE_TLU_CTRL_QDET |
		FIRE_TLU_CTRL_CFG);
	upa_writeq(val, pbm->pbm_regs + FIRE_TLU_CTRL);
	upa_writeq(0, pbm->pbm_regs + FIRE_TLU_DEV_CTRL);
	upa_writeq(FIRE_TLU_LINK_CTRL_CLK,
		   pbm->pbm_regs + FIRE_TLU_LINK_CTRL);

	upa_writeq(0, pbm->pbm_regs + FIRE_LPU_RESET);
	upa_writeq(FIRE_LPU_LLCFG_VC0, pbm->pbm_regs + FIRE_LPU_LLCFG);
	upa_writeq((FIRE_LPU_FCTRL_UCTRL_N | FIRE_LPU_FCTRL_UCTRL_P),
		   pbm->pbm_regs + FIRE_LPU_FCTRL_UCTRL);
	upa_writeq(((0xffff << 16) | (0x0000 << 0)),
		   pbm->pbm_regs + FIRE_LPU_TXL_FIFOP);
	upa_writeq(3000000, pbm->pbm_regs + FIRE_LPU_LTSSM_CFG2);
	upa_writeq(500000, pbm->pbm_regs + FIRE_LPU_LTSSM_CFG3);
	upa_writeq((2 << 16) | (140 << 8),
		   pbm->pbm_regs + FIRE_LPU_LTSSM_CFG4);
	upa_writeq(0, pbm->pbm_regs + FIRE_LPU_LTSSM_CFG5);

	upa_writeq(~(u64)0, pbm->pbm_regs + FIRE_DMC_IENAB);
	upa_writeq(0, pbm->pbm_regs + FIRE_DMC_DBG_SEL_A);
	upa_writeq(0, pbm->pbm_regs + FIRE_DMC_DBG_SEL_B);

	upa_writeq(~(u64)0, pbm->pbm_regs + FIRE_PEC_IENAB);
}

static bool oberon_diag_enabled;
static DEFINE_MUTEX(oberon_diag_lock);
static void oberon_diag_snapshot(struct pci_pbm_info *pbm, const char *phase);

static ssize_t oberon_snapshot_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct pci_pbm_info *pbm = dev_get_drvdata(dev);
	bool trigger;

	if (!oberon_diag_enabled || !pbm || !pbm->is_oberon)
		return -ENODEV;
	if (kstrtobool(buf, &trigger) || !trigger)
		return -EINVAL;
	oberon_diag_snapshot(pbm, "runtime");
	return count;
}
static DEVICE_ATTR_WO(oberon_snapshot);

static int pci_fire_pbm_init(struct pci_pbm_info *pbm,
			     struct platform_device *op, u32 portid)
{
	const struct linux_prom64_registers *regs;
	struct device_node *dp = op->dev.of_node;
	int err, len;

	regs = of_get_property(dp, "reg", &len);
	if (!regs || len < 2 * sizeof(*regs)) {
		dev_err(&op->dev, "missing or short reg property\n");
		return -EINVAL;
	}

	pbm->numa_node = NUMA_NO_NODE;
	pbm->is_oberon = of_device_is_compatible(dp, "pciex108e,80f8");

	pbm->pci_ops = &sun4u_pci_ops;
	pbm->config_space_reg_bits = 12;

	pbm->index = pci_num_pbms++;

	pbm->portid = portid;
	pbm->op = op;
	pbm->name = dp->full_name;

	pbm->pbm_regs = regs[0].phys_addr;
	pbm->controller_regs = regs[1].phys_addr - 0x410000UL;
	if (pbm->is_oberon && oberon_diag_enabled)
		oberon_diag_snapshot(pbm, "pre-pbm");

	printk("%s: SUN4U PCIE Bus Module\n", pbm->name);

	pci_determine_mem_io_space(pbm);

	pci_get_pbm_props(pbm);

	if (!pbm->is_oberon)
		pci_fire_hw_init(pbm);

	err = pci_fire_pbm_iommu_init(pbm);
	if (err)
		return err;

	if (!pbm->is_oberon)
		pci_fire_msi_init(pbm);

	pbm->pci_bus = pci_scan_one_pbm(pbm, &op->dev);

	/* XXX register error interrupt handlers XXX */

	pbm->next = pci_pbm_root;
	pci_pbm_root = pbm;

	return 0;
}

static int fire_probe(struct platform_device *op)
{
	struct device_node *dp = op->dev.of_node;
	struct pci_pbm_info *pbm;
	struct iommu *iommu;
	u32 portid;
	int err;

	portid = of_getintprop_default(dp, "portid", 0xff);

	err = -ENOMEM;
	pbm = kzalloc_obj(*pbm);
	if (!pbm) {
		printk(KERN_ERR PFX "Cannot allocate pci_pbminfo.\n");
		goto out_err;
	}

	iommu = kzalloc_obj(struct iommu);
	if (!iommu) {
		printk(KERN_ERR PFX "Cannot allocate PBM iommu.\n");
		goto out_free_controller;
	}

	pbm->iommu = iommu;

	err = pci_fire_pbm_init(pbm, op, portid);
	if (err)
		goto out_free_iommu;

	dev_set_drvdata(&op->dev, pbm);
	if (pbm->is_oberon && oberon_diag_enabled) {
		err = device_create_file(&op->dev, &dev_attr_oberon_snapshot);
		if (err)
			dev_warn(&op->dev, "passive snapshot trigger unavailable: %d\n", err);
	}

	return 0;

out_free_iommu:
	kfree(pbm->iommu);
			
out_free_controller:
	kfree(pbm);

out_err:
	return err;
}

static const struct of_device_id fire_match[] = {
	{
		.name = "pci",
		.compatible = "pciex108e,80f0",
	},
	{ .name = "pci", .compatible = "pciex108e,80f8" },
	{},
};

static struct platform_driver fire_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = fire_match,
	},
	.probe		= fire_probe,
};

/* This diagnostic is opt-in and never acknowledges or enables errors. */

static int __init oberon_diag_setup(char *str)
{
	return kstrtobool(str, &oberon_diag_enabled);
}
early_param("oberon_diag", oberon_diag_setup);

#define OBERON_DIAG_DMC_ENABLE	0x31800UL
#define OBERON_DIAG_DMC_STATUS	0x31808UL
#define OBERON_DIAG_MULTI_STATUS	0x31810UL
#define OBERON_DIAG_MMU_LOG	0x41000UL
#define OBERON_DIAG_MMU_ENABLE	0x41008UL
#define OBERON_DIAG_MMU_STATUS	0x41010UL
#define OBERON_DIAG_TFAR		0x41028UL
#define OBERON_DIAG_TFSR		0x41030UL
#define OBERON_DIAG_ILU_STATUS	0x51010UL
#define OBERON_DIAG_PEC_STATUS	0x51808UL
#define OBERON_DIAG_OE_STATUS	0x81010UL
#define OBERON_DIAG_OE_RX1	0x81028UL
#define OBERON_DIAG_OE_RX2	0x81030UL
#define OBERON_DIAG_OE_TX1	0x81038UL
#define OBERON_DIAG_OE_TX2	0x81040UL
#define OBERON_DIAG_OE_RUC_WUC	(BIT_ULL(16) | BIT_ULL(17))
#define OBERON_DIAG_UE_STATUS	0x91010UL
#define OBERON_DIAG_CE_STATUS	0xa1010UL

static void oberon_diag_snapshot(struct pci_pbm_info *pbm, const char *phase)
{
	struct device_node *dp = pbm->op->dev.of_node;
	const struct linux_prom64_registers *regs;
	const __be32 *intr;
	unsigned long base = pbm->pbm_regs, imap_offset;
	unsigned int ino;
	u64 multi, dmc, status, tfar = 0, tfsr = 0;
	u64 multi_after, dmc_after, status_after;
	u64 pec = 0, pec_after = 0, ilu = 0, ilu_after = 0;
	u64 oe = 0, oe_after = 0, ue = 0, ue_after = 0;
	u64 ce = 0, ce_after = 0;
	u64 rx1 = 0, rx2 = 0, tx1 = 0, tx2 = 0;
	u64 rx1_after = 0, rx2_after = 0, tx1_after = 0, tx2_after = 0;
	bool headers_read = false;
	bool pec_read;
	u64 dmc_enable, mmu_enable, log_enable, imap;
	bool logs_read;
	int len;

	regs = of_get_property(dp, "reg", &len);
	if (!regs || len < sizeof(*regs) || regs[0].phys_addr != base ||
	    regs[0].reg_size < OBERON_DIAG_CE_STATUS + sizeof(u64) ||
	    base > ~0UL - OBERON_DIAG_CE_STATUS - sizeof(u64)) {
		pr_warn("Oberon diag %pOF: invalid CSR range; skipped\n", dp);
		return;
	}
	intr = of_get_property(dp, "interrupts", &len);
	if (!intr || len < 2 * sizeof(*intr)) {
		pr_warn("Oberon diag %pOF: missing error IRQ property; skipped\n", dp);
		return;
	}
	ino = be32_to_cpup(intr + 1);
	if (ino > 0x3f) {
		pr_warn("Oberon diag %pOF: invalid error INO; skipped\n", dp);
		return;
	}
	imap_offset = 0x1000UL + 8 * ino;
	mutex_lock(&oberon_diag_lock);
	dmc_enable = upa_readq(base + OBERON_DIAG_DMC_ENABLE);
	log_enable = upa_readq(base + OBERON_DIAG_MMU_LOG);
	mmu_enable = upa_readq(base + OBERON_DIAG_MMU_ENABLE);
	imap = upa_readq(base + imap_offset);
	multi = upa_readq(base + OBERON_DIAG_MULTI_STATUS);
	dmc = upa_readq(base + OBERON_DIAG_DMC_STATUS);
	status = upa_readq(base + OBERON_DIAG_MMU_STATUS);
	/* Fault logs are candidates only; status equality cannot prove coherence. */
	logs_read = status != 0;
	if (logs_read) {
		tfar = upa_readq(base + OBERON_DIAG_TFAR);
		tfsr = upa_readq(base + OBERON_DIAG_TFSR);
	}
	/* Preserve both samples; equal values do not establish an atomic snapshot. */
	pec_read = !!(multi & BIT_ULL(1));
	if (pec_read) {
		pec = upa_readq(base + OBERON_DIAG_PEC_STATUS);
		oe = upa_readq(base + OBERON_DIAG_OE_STATUS);
		ce = upa_readq(base + OBERON_DIAG_CE_STATUS);
		ue = upa_readq(base + OBERON_DIAG_UE_STATUS);
		ilu = upa_readq(base + OBERON_DIAG_ILU_STATUS);
		/* Primary RUC/WUC gates candidate logs; secondary events lack attribution. */
		headers_read = !!(oe & OBERON_DIAG_OE_RUC_WUC);
		if (headers_read) {
			rx1 = upa_readq(base + OBERON_DIAG_OE_RX1);
			rx2 = upa_readq(base + OBERON_DIAG_OE_RX2);
			tx1 = upa_readq(base + OBERON_DIAG_OE_TX1);
			tx2 = upa_readq(base + OBERON_DIAG_OE_TX2);
			tx2_after = upa_readq(base + OBERON_DIAG_OE_TX2);
			tx1_after = upa_readq(base + OBERON_DIAG_OE_TX1);
			rx2_after = upa_readq(base + OBERON_DIAG_OE_RX2);
			rx1_after = upa_readq(base + OBERON_DIAG_OE_RX1);
		}
		ilu_after = upa_readq(base + OBERON_DIAG_ILU_STATUS);
		ue_after = upa_readq(base + OBERON_DIAG_UE_STATUS);
		ce_after = upa_readq(base + OBERON_DIAG_CE_STATUS);
		oe_after = upa_readq(base + OBERON_DIAG_OE_STATUS);
		pec_after = upa_readq(base + OBERON_DIAG_PEC_STATUS);
	}
	status_after = upa_readq(base + OBERON_DIAG_MMU_STATUS);
	dmc_after = upa_readq(base + OBERON_DIAG_DMC_STATUS);
	multi_after = upa_readq(base + OBERON_DIAG_MULTI_STATUS);

	/* Finish all register reads before printk can enter the firmware console. */
	pr_info("Oberon diag %pOF: phase=%s\n", dp, phase);
	pr_info("Oberon diag %pOF: portid=%u ino=%x imap=%016llx route_valid=%u\n",
		dp, pbm->portid, ino, imap, !!(imap & BIT_ULL(31)));
	pr_info("Oberon diag %pOF: dmc_enable=%016llx mmu_log=%016llx mmu_enable=%016llx\n",
		dp, dmc_enable, log_enable, mmu_enable);
	pr_info("Oberon diag %pOF: multi=%016llx dmc=%016llx mmu=%016llx\n",
		dp, multi, dmc, status);
	pr_info("Oberon diag %pOF: logs_read=%u tfar_raw=%016llx tfsr_raw=%016llx\n",
		dp, logs_read, tfar, tfsr);
	pr_info("Oberon diag %pOF: multi_after=%016llx dmc_after=%016llx mmu_after=%016llx changed=%u\n",
		dp, multi_after, dmc_after, status_after,
		multi != multi_after || dmc != dmc_after || status != status_after);
	pr_info("Oberon diag %pOF: pec_read=%u pec=%016llx pec_after=%016llx\n",
		dp, pec_read, pec, pec_after);
	pr_info("Oberon diag %pOF: oe=%016llx ce=%016llx ue=%016llx ilu=%016llx\n",
		dp, oe, ce, ue, ilu);
	pr_info("Oberon diag %pOF: oe_after=%016llx ce_after=%016llx\n",
		dp, oe_after, ce_after);
	pr_info("Oberon diag %pOF: ue_after=%016llx ilu_after=%016llx pec_changed=%u\n",
		dp, ue_after, ilu_after,
		pec != pec_after || oe != oe_after || ce != ce_after ||
		ue != ue_after || ilu != ilu_after);
	pr_info("Oberon diag %pOF: headers_read=%u headers_changed=%u\n",
		dp, headers_read, rx1 != rx1_after || rx2 != rx2_after ||
		tx1 != tx1_after || tx2 != tx2_after);
	pr_info("Oberon diag %pOF: rx1=%016llx rx2=%016llx\n", dp, rx1, rx2);
	pr_info("Oberon diag %pOF: tx1=%016llx tx2=%016llx\n", dp, tx1, tx2);
	pr_info("Oberon diag %pOF: rx1_after=%016llx rx2_after=%016llx\n",
		dp, rx1_after, rx2_after);
	pr_info("Oberon diag %pOF: tx1_after=%016llx tx2_after=%016llx\n",
		dp, tx1_after, tx2_after);
	mutex_unlock(&oberon_diag_lock);
}

static int __init oberon_diag_init(void)
{
	struct pci_pbm_info *pbm;

	if (!oberon_diag_enabled)
		return 0;
	for (pbm = pci_pbm_root; pbm; pbm = pbm->next)
		if (pbm->is_oberon)
			oberon_diag_snapshot(pbm, "late");
	return 0;
}
late_initcall(oberon_diag_init);

static int __init fire_init(void)
{
	return platform_driver_register(&fire_driver);
}

subsys_initcall(fire_init);

struct oberon_irq_capture {
	struct work_struct report;
	struct timer_list deadline;
	unsigned int irq;
	struct pci_pbm_info *pbm;
	u64 status[8], status_after[8];
	u64 fault[2], header[4], header_after[4];
	bool fault_read, headers_read;
	bool clear_attempted;
	int clear_result;
	u64 clear_after[2][8];
	struct oberon_ack_result ack;
	u64 ack_status[2][2][8];
	int ack_result, ack_checks;
	bool ack_requested;
};

static int oberon_capture_arm_port = -1;

static int __init oberon_capture_arm_setup(char *str)
{
	int port, ret = kstrtoint(str, 0, &port);

	if (ret || port < 0 || port > 1)
		return -EINVAL;
	oberon_capture_arm_port = port;
	return 0;
}
early_param("oberon_capture_arm_port", oberon_capture_arm_setup);

static void oberon_capture_deadline(struct timer_list *timer)
{
	struct oberon_irq_capture *cap = timer_container_of(cap, timer, deadline);

	oberon_disarm_capture(cap->irq);
}

static struct oberon_irq_capture *oberon_live_capture;

static ssize_t oberon_capture_arm_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct oberon_irq_capture *cap = oberon_live_capture;
	int ret;

	if (!cap || dev != &cap->pbm->op->dev || !sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = oberon_arm_capture(cap->irq);
	if (ret)
		return ret;
	mod_timer(&cap->deadline, jiffies + 5 * HZ);
	return count;
}
static DEVICE_ATTR_WO(oberon_capture_arm);

static bool oberon_capture_prepare;

static int __init oberon_capture_setup(char *str)
{
	return kstrtobool(str, &oberon_capture_prepare);
}
early_param("oberon_capture_prepare", oberon_capture_setup);

static const unsigned long oberon_capture_status_regs[] = {
	OBERON_DIAG_MULTI_STATUS, OBERON_DIAG_DMC_STATUS,
	OBERON_DIAG_MMU_STATUS, OBERON_DIAG_PEC_STATUS,
	OBERON_DIAG_OE_STATUS, OBERON_DIAG_UE_STATUS,
	OBERON_DIAG_CE_STATUS, OBERON_DIAG_ILU_STATUS,
};

static const unsigned long oberon_capture_header_regs[] = {
	OBERON_DIAG_OE_RX1, OBERON_DIAG_OE_RX2,
	OBERON_DIAG_OE_TX1, OBERON_DIAG_OE_TX2,
};

/* A matching snapshot permits review of a clear, not automatic recovery. */
static int oberon_capture_recovery_check(struct oberon_irq_capture *cap)
{
	const u64 expected[] = { 2, 0, 0, 1, BIT_ULL(16) | BIT_ULL(48), 0, 0, 0 };
	unsigned long base = cap->pbm->pbm_regs;
	int pass, i;

	if (!oberon_capture_quarantined(cap->irq))
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(expected); i++)
		if (cap->status[i] != expected[i] ||
		    cap->status_after[i] != expected[i])
			return -EOPNOTSUPP;
	if (!cap->headers_read)
		return -EOPNOTSUPP;
	for (i = 0; i < ARRAY_SIZE(cap->header); i++)
		if (cap->header[i] != cap->header_after[i])
			return -EAGAIN;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ARRAY_SIZE(expected); i++)
			if (upa_readq(base + oberon_capture_status_regs[i]) != expected[i])
				return -EAGAIN;
		for (i = 0; i < ARRAY_SIZE(cap->header); i++)
			if (upa_readq(base + oberon_capture_header_regs[i]) != cap->header[i])
				return -EAGAIN;
	}
	return oberon_capture_quarantined(cap->irq) ? 0 : -EBUSY;
}

static ssize_t oberon_capture_recovery_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct oberon_irq_capture *cap = oberon_live_capture;
	int ret;

	if (!cap || dev != &cap->pbm->op->dev)
		return -ENODEV;
	guard(mutex)(&oberon_diag_lock);
	ret = oberon_capture_recovery_check(cap);
	return sysfs_emit(buf, "check=%d offset=0x81018 mask=0x0001000000010000 writes=0\n",
			  ret);
}
static DEVICE_ATTR(oberon_capture_recovery, 0400, oberon_capture_recovery_show, NULL);

static int oberon_capture_clear_ruc(struct oberon_irq_capture *cap)
{
	unsigned long base = cap->pbm->pbm_regs;
	int pass, i, ret;

	if (cap->clear_attempted)
		return -EALREADY;
	ret = oberon_capture_recovery_check(cap);
	if (ret)
		return ret;
	cap->clear_attempted = true;
	upa_writeq(BIT_ULL(16) | BIT_ULL(48), base + 0x81018);
	ret = 0;
	/* Preserve both samples even when a source remains set; never retry. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++) {
			cap->clear_after[pass][i] =
				upa_readq(base + oberon_capture_status_regs[i]);
			if (cap->clear_after[pass][i])
				ret = -EAGAIN;
		}
	}
	if (!oberon_capture_quarantined(cap->irq))
		ret = -EBUSY;
	cap->clear_result = ret;
	return ret;
}

static ssize_t oberon_capture_clear_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct oberon_irq_capture *cap = oberon_live_capture;
	int ret;

	if (!cap || dev != &cap->pbm->op->dev || !sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!oberon_capture_quarantined(cap->irq))
		return -EBUSY;
	timer_delete_sync(&cap->deadline);
	flush_work(&cap->report);
	guard(mutex)(&oberon_diag_lock);
	ret = oberon_capture_clear_ruc(cap);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(oberon_capture_clear);

static ssize_t oberon_capture_clear_result_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct oberon_irq_capture *cap = oberon_live_capture;
	int len, i;

	if (!cap || dev != &cap->pbm->op->dev)
		return -ENODEV;
	guard(mutex)(&oberon_diag_lock);
	len = sysfs_emit(buf, "attempted=%u result=%d ack=%u rearm=0\n",
			 cap->clear_attempted, cap->clear_result, cap->ack.written);
	for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++)
		len += sysfs_emit_at(buf, len, "reg=%lx first=%016llx second=%016llx\n",
				     oberon_capture_status_regs[i],
				     cap->clear_after[0][i], cap->clear_after[1][i]);
	return len;
}
static DEVICE_ATTR(oberon_capture_clear_result, 0400,
		   oberon_capture_clear_result_show, NULL);

static int oberon_ack_sources(void *arg)
{
	struct oberon_irq_capture *cap = arg;
	int stage = cap->ack_checks++, pass, i, ret = 0;

	if (stage >= ARRAY_SIZE(cap->ack_status))
		return -EINVAL;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++) {
			u64 val = upa_readq(cap->pbm->pbm_regs +
					    oberon_capture_status_regs[i]);

			cap->ack_status[stage][pass][i] = val;
			if (val)
				ret = -EAGAIN;
		}
	}
	return ret;
}

static ssize_t oberon_capture_ack_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct oberon_irq_capture *cap = oberon_live_capture;

	if (!cap || dev != &cap->pbm->op->dev || !sysfs_streq(buf, "1"))
		return -EINVAL;
	guard(mutex)(&oberon_diag_lock);
	if (!cap->clear_attempted || cap->clear_result || cap->ack_requested)
		return -EBUSY;
	cap->ack_requested = true;
	cap->ack_result = oberon_ack_capture(cap->irq, oberon_ack_sources,
					     cap, &cap->ack);
	return cap->ack_result ? cap->ack_result : count;
}
static DEVICE_ATTR_WO(oberon_capture_ack);

static ssize_t oberon_capture_ack_result_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct oberon_irq_capture *cap = oberon_live_capture;
	int len, stage, i;

	if (!cap || dev != &cap->pbm->op->dev)
		return -ENODEV;
	guard(mutex)(&oberon_diag_lock);
	len = sysfs_emit(buf,
		"requested=%u written=%u result=%d before=%lx after=%lx checks=%d rearm=0\n",
			 cap->ack_requested, cap->ack.written, cap->ack_result,
			 cap->ack.before, cap->ack.after, cap->ack_checks);
	for (stage = 0; stage < cap->ack_checks && stage < 2; stage++)
		for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++)
			len += sysfs_emit_at(buf, len,
				"stage=%d reg=%lx first=%016llx second=%016llx\n",
				stage, oberon_capture_status_regs[i],
				cap->ack_status[stage][0][i], cap->ack_status[stage][1][i]);
	return len;
}
static DEVICE_ATTR(oberon_capture_ack_result, 0400,
		   oberon_capture_ack_result_show, NULL);

static void oberon_capture_report(struct work_struct *work)
{
	struct oberon_irq_capture *cap = container_of(work,
		struct oberon_irq_capture, report);
	int i;

	pr_warn("Oberon capture %s: candidate samples; no ack or rearm\n",
		cap->pbm->name);
	for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++)
		pr_warn("Oberon capture %s: reg=%lx before=%016llx after=%016llx\n",
			cap->pbm->name, oberon_capture_status_regs[i],
			cap->status[i], cap->status_after[i]);
	pr_warn("Oberon capture %s: fault_read=%u tfar=%016llx tfsr=%016llx headers_read=%u\n",
		cap->pbm->name, cap->fault_read, cap->fault[0],
		cap->fault[1], cap->headers_read);
	if (cap->headers_read)
		for (i = 0; i < ARRAY_SIZE(oberon_capture_header_regs); i++)
			pr_warn("Oberon capture %s: header=%lx before=%016llx after=%016llx\n",
				cap->pbm->name, oberon_capture_header_regs[i],
				cap->header[i], cap->header_after[i]);
}

static void oberon_capture_event(void *arg)
{
	struct oberon_irq_capture *cap = arg;
	unsigned long base = cap->pbm->pbm_regs;
	int i;

	/* At most 26 reads; equal samples do not prove an atomic snapshot. */
	for (i = 0; i < ARRAY_SIZE(oberon_capture_status_regs); i++)
		cap->status[i] = upa_readq(base + oberon_capture_status_regs[i]);
	cap->fault_read = cap->status[2] != 0;
	if (cap->fault_read) {
		cap->fault[0] = upa_readq(base + OBERON_DIAG_TFAR);
		cap->fault[1] = upa_readq(base + OBERON_DIAG_TFSR);
	}
	/* Secondary-only events do not identify the candidate header logs. */
	cap->headers_read = !!(cap->status[4] & OBERON_DIAG_OE_RUC_WUC);
	if (cap->headers_read) {
		for (i = 0; i < ARRAY_SIZE(oberon_capture_header_regs); i++)
			cap->header[i] = upa_readq(base + oberon_capture_header_regs[i]);
		for (i = ARRAY_SIZE(oberon_capture_header_regs) - 1; i >= 0; i--)
			cap->header_after[i] = upa_readq(base + oberon_capture_header_regs[i]);
	}
	for (i = ARRAY_SIZE(oberon_capture_status_regs) - 1; i >= 0; i--)
		cap->status_after[i] = upa_readq(base + oberon_capture_status_regs[i]);
	schedule_work(&cap->report);
}

static int __init oberon_capture_init(void)
{
	struct pci_pbm_info *pbm;
	struct oberon_irq_capture *selected = NULL;
	unsigned int matches = 0;

	if (!oberon_capture_prepare)
		return 0;
	for (pbm = pci_pbm_root; pbm; pbm = pbm->next) {
		const struct linux_prom64_registers *regs;
		const __be32 *intr;
		struct oberon_irq_capture *cap;
		int len, irq, ret;

		if (!pbm->is_oberon)
			continue;
		regs = of_get_property(pbm->op->dev.of_node, "reg", &len);
		if (!regs || len < sizeof(*regs) ||
		    regs[0].phys_addr != pbm->pbm_regs ||
		    regs[0].reg_size < OBERON_DIAG_CE_STATUS + sizeof(u64) ||
		    pbm->pbm_regs > ~0UL - OBERON_DIAG_CE_STATUS - sizeof(u64))
			continue;
		intr = of_get_property(pbm->op->dev.of_node, "interrupts", &len);
		if (!intr || len < 2 * sizeof(*intr) ||
		    be32_to_cpup(intr + 1) != 0x3e)
			continue;
		irq = platform_get_irq(pbm->op, 1);
		if (irq <= 0)
			continue;
		cap = kzalloc_obj(*cap, GFP_KERNEL);
		if (!cap)
			return -ENOMEM;
		cap->pbm = pbm;
		cap->irq = irq;
		timer_setup(&cap->deadline, oberon_capture_deadline, 0);
		INIT_WORK(&cap->report, oberon_capture_report);
		ret = oberon_prepare_capture(irq, pbm->pbm_regs + 0x11f0,
					     oberon_capture_event, cap);
		if (ret) {
			kfree(cap);
			pr_warn("Oberon capture %s: preparation rejected: %d\n",
				pbm->name, ret);
		} else {
			if (pbm->portid == oberon_capture_arm_port) {
				selected = cap;
				matches++;
			}
			pr_info("Oberon capture %s: prepared only, IRQ remains unarmed\n",
				pbm->name);
		}
	}
	if (matches == 1) {
		int ret;

		oberon_live_capture = selected;
		ret = device_create_file(&selected->pbm->op->dev,
					 &dev_attr_oberon_capture_arm);
		if (!ret)
			ret = device_create_file(&selected->pbm->op->dev,
						 &dev_attr_oberon_capture_recovery);
		if (!ret)
			ret = device_create_file(&selected->pbm->op->dev,
						 &dev_attr_oberon_capture_clear);
		if (!ret)
			ret = device_create_file(&selected->pbm->op->dev,
						 &dev_attr_oberon_capture_clear_result);
		if (!ret)
			ret = device_create_file(&selected->pbm->op->dev,
						 &dev_attr_oberon_capture_ack);
		if (!ret)
			ret = device_create_file(&selected->pbm->op->dev,
						 &dev_attr_oberon_capture_ack_result);
		if (ret) {
			device_remove_file(&selected->pbm->op->dev,
					   &dev_attr_oberon_capture_ack);
			device_remove_file(&selected->pbm->op->dev,
					   &dev_attr_oberon_capture_clear_result);
			device_remove_file(&selected->pbm->op->dev,
					   &dev_attr_oberon_capture_clear);
			device_remove_file(&selected->pbm->op->dev,
					   &dev_attr_oberon_capture_recovery);
			device_remove_file(&selected->pbm->op->dev,
					   &dev_attr_oberon_capture_arm);
			oberon_live_capture = NULL;
		}
		pr_warn("Oberon capture %s: arm interface result=%d; ownership unverified, route unarmed\n",
			selected->pbm->name, ret);
	}
	return 0;
}
late_initcall(oberon_capture_init);
