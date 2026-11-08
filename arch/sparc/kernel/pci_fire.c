// SPDX-License-Identifier: GPL-2.0
/* pci_fire.c: Sun4u platform PCI-E controller support.
 *
 * Copyright (C) 2007 David S. Miller (davem@davemloft.net)
 */
#include <linux/workqueue.h>
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
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/sizes.h>

#include <asm/prom.h>
#include <asm/irq.h>
#include <asm/upa.h>
#include <asm/trap_block.h>

#include "iommu_common.h"
#include "pci_impl.h"
#include "pci_oberon_msi.h"

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

#define OBERON_MSI_IMAP_MONDO		BIT_ULL(63)

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
	*head = upa_readq(pbm->pbm_regs + EVENT_QUEUE_HEAD(msiqid)) &
		EVENT_QUEUE_HEAD_VAL;
	if (*head >= pbm->msiq_ent_count)
		return -EINVAL;
	return 0;
}

static int pci_fire_dequeue_msi(struct pci_pbm_info *pbm, unsigned long msiqid,
				unsigned long *head, unsigned long *msi)
{
	unsigned long type_fmt, type, msi_num;
	u64 word;
	int result = MSIQ_DISPATCH;
	struct pci_msiq_entry *base, *ep;

	base = (pbm->msi_queues + ((msiqid - pbm->msiq_first) * 8192));
	ep = &base[*head];

	word = READ_ONCE(ep->word0);
	if (!(word & MSIQ_WORD0_FMT_TYPE))
		return 0;
	dma_rmb();

	type_fmt = ((word & MSIQ_WORD0_FMT_TYPE) >>
		    MSIQ_WORD0_FMT_TYPE_SHIFT);
	type = (type_fmt >> 3);
	if (unlikely(type != MSIQ_TYPE_MSI32 &&
		     type != MSIQ_TYPE_MSI64))
		return -EINVAL;

	*msi = msi_num = ((word & MSIQ_WORD0_DATA0) >>
			  MSIQ_WORD0_DATA0_SHIFT);
	if (msi_num < pbm->msi_first ||
	    msi_num - pbm->msi_first >= pbm->msi_num) {
		result = MSIQ_DISCARD;
		goto consume;
	}

	upa_writeq(MSI_CLEAR_EQWR_N, pbm->pbm_regs + MSI_CLEAR(msi_num));

consume:
	/* Clear the entry.  */
	WRITE_ONCE(ep->word0, word & ~MSIQ_WORD0_FMT_TYPE);

	/* Go to next entry in ring.  */
	(*head)++;
	if (*head >= pbm->msiq_ent_count)
		*head = 0;

	return result;
}

static int pci_fire_set_head(struct pci_pbm_info *pbm, unsigned long msiqid,
			     unsigned long head)
{
	dma_wmb();
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
	if (upa_readq(pbm->pbm_regs + MSI_MAP(msi)) & MSI_MAP_VALID)
		return -EIO;

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

	return irq;
}

static int pci_fire_msiq_enable(struct pci_pbm_info *pbm, unsigned long msiqid)
{
	upa_writeq(EVENT_QUEUE_CONTROL_SET_EN,
		   pbm->pbm_regs + EVENT_QUEUE_CONTROL_SET(msiqid));
	return 0;
}

static int pci_fire_msiq_disable(struct pci_pbm_info *pbm, unsigned long msiqid)
{
	unsigned long state;

	upa_writeq(EVENT_QUEUE_CONTROL_CLEAR_DIS,
		   pbm->pbm_regs + EVENT_QUEUE_CONTROL_CLEAR(msiqid));
	state = upa_readq(pbm->pbm_regs + EVENT_QUEUE_STATE(msiqid));
	/* The state readback does not establish completion of queue DMA. */
	return (state & EVENT_QUEUE_STATE_MASK) == EVENT_QUEUE_STATE_IDLE ?
		0 : -EBUSY;
}

static const struct sparc64_msiq_ops pci_fire_msiq_ops = {
	.get_head	=	pci_fire_get_head,
	.dequeue_msi	=	pci_fire_dequeue_msi,
	.set_head	=	pci_fire_set_head,
	.msi_setup	=	pci_fire_msi_setup,
	.msi_teardown	=	pci_fire_msi_teardown,
	.msiq_alloc	=	pci_fire_msiq_alloc,
	.msiq_enable	=	pci_fire_msiq_enable,
	.msiq_disable	=	pci_fire_msiq_disable,
	.msiq_build_irq	=	pci_fire_msiq_build_irq,
};

static void pci_fire_msi_init(struct pci_pbm_info *pbm)
{
	sparc64_pbm_msi_init(pbm, &pci_fire_msiq_ops);
}

static int oberon_msi_cells(struct device_node *dp, const char *name,
			    u32 *cells, size_t count)
{
	if (of_property_count_u32_elems(dp, name) != count)
		return -EINVAL;
	return of_property_read_u32_array(dp, name, cells, count);
}

static int oberon_msi_geometry(struct pci_pbm_info *pbm,
			       struct oberon_msi_geometry *g)
{
	struct device_node *dp = pbm->op->dev.of_node;
	const struct linux_prom64_registers *regs;
	const char *range = "msi-eq-to-devino";
	int len;

	regs = of_get_property(dp, "reg", &len);
	if (!regs || len < sizeof(*regs) || regs[0].reg_size < 0x34010 ||
	    pbm->pbm_regs > ~0UL - 0x34010)
		return -EINVAL;
	if (!of_find_property(dp, range, NULL))
		range = "msi-eq-devino";
	if (oberon_msi_cells(dp, "#msi-eqs", &g->queues, 1) ||
	    oberon_msi_cells(dp, "msi-eq-size", &g->entries, 1) ||
	    oberon_msi_cells(dp, range, g->queue_range, 3) ||
	    oberon_msi_cells(dp, "#msi", &g->messages, 1) ||
	    oberon_msi_cells(dp, "msi-ranges", g->message_range, 2) ||
	    oberon_msi_cells(dp, "msi-data-mask", &g->data_mask, 1) ||
	    oberon_msi_cells(dp, "msix-data-width", &g->msix_width, 1) ||
	    oberon_msi_cells(dp, "msi-address-ranges", g->addresses, 6) ||
	    oberon_msi_cells(dp, "ino-bitmap", g->ino_bitmap, 2))
		return -EINVAL;
	return oberon_msi_validate(g);
}

enum oberon_message_state {
	OBERON_MSG_UNUSED,
	OBERON_MSG_RESERVED,
	OBERON_MSG_LIVE,
	OBERON_MSG_RETIRED,
	OBERON_MSG_QUARANTINED,
};

struct oberon_msi_peer {
	struct list_head node;
	struct pci_dev *pdev;
	unsigned int vectors;
	bool blocked;
	bool notified;
};

struct oberon_msi_message {
	struct oberon_msi_peer *peer;
	unsigned int irq;
	u8 state;
};

/* Published queue storage remains owned by this bridge until reboot. */
struct oberon_msi {
	struct list_head node;
	struct list_head peers;
	struct work_struct fault_work;
	struct pci_pbm_info *pbm;
	struct pci_dev *pdev;
	void *allocation;
	dma_addr_t dma;
	struct pci_msiq_entry *queue;
	unsigned long imap, iclr;
	unsigned int irq, next, head;
	struct oberon_msi_message messages[256];
	unsigned int endpoint_failures, invalidation_failures;
	unsigned int blocked_publications;
	atomic64_t records;
	atomic64_t unbound_records;
	u64 release_state, release_tail;
	unsigned int releases, release_head;
	bool failed;
	raw_spinlock_t publish_lock; /* Order map publication against fault stop. */
	struct mutex lock; /* Serialize vector allocation and teardown. */
};

/* Peer references cover allocated vectors; blocked peers stay pinned. */
static struct oberon_msi_peer *oberon_msi_peer_get(struct oberon_msi *t,
						   struct pci_dev *pdev)
{
	struct oberon_msi_peer *peer;

	lockdep_assert_held(&t->lock);
	if (READ_ONCE(t->failed) ||
	    pdev->dev.archdata.host_controller != t->pbm)
		return ERR_PTR(-EIO);
	list_for_each_entry(peer, &t->peers, node) {
		if (peer->pdev != pdev)
			continue;
		if (peer->blocked)
			return ERR_PTR(-EIO);
		peer->vectors++;
		return peer;
	}
	peer = kzalloc_obj(*peer);
	if (!peer)
		return ERR_PTR(-ENOMEM);
	peer->pdev = pci_dev_get(pdev);
	peer->vectors = 1;
	list_add_tail(&peer->node, &t->peers);
	return peer;
}

static void oberon_msi_peer_put(struct oberon_msi *t,
				struct oberon_msi_peer *peer)
{
	lockdep_assert_held(&t->lock);
	if (WARN_ON_ONCE(!peer->vectors))
		return;
	peer->vectors--;
	if (READ_ONCE(t->failed))
		peer->blocked = true;
	if (peer->vectors || peer->blocked)
		return;
	list_del(&peer->node);
	pci_dev_put(peer->pdev);
	kfree(peer);
}

/* Notify every affected device without holding controller locks. */
static void oberon_msi_fault_work(struct work_struct *work)
{
	struct oberon_msi *t = container_of(work, struct oberon_msi,
						fault_work);
	struct oberon_msi_peer *peer;
	unsigned int i;

	scoped_guard(mutex, &t->lock) {
		list_for_each_entry(peer, &t->peers, node)
			peer->blocked = true;
		for (i = 0; i < t->next; i++)
			if (t->messages[i].state == OBERON_MSG_LIVE)
				t->messages[i].state = OBERON_MSG_QUARANTINED;
	}
	for (;;) {
		enum pci_msi_loss_outcome outcome = PCI_MSI_LOSS_UNRESOLVED;
		struct pci_dev *pdev = NULL;
		int ret;

		scoped_guard(mutex, &t->lock) {
			list_for_each_entry(peer, &t->peers, node) {
				if (peer->notified)
					continue;
				peer->notified = true;
				pdev = pci_dev_get(peer->pdev);
				break;
			}
		}
		if (!pdev)
			break;
		/* Driver teardown re-enters this owner; blocked peers stay pinned. */
		ret = pci_msi_notify_lost(pdev, &outcome);
		scoped_guard(mutex, &t->lock) {
			list_for_each_entry(peer, &t->peers, node)
				if (peer->pdev == pdev && peer->vectors && ret >= 0) {
					ret = -EBUSY;
					outcome = PCI_MSI_LOSS_UNRESOLVED;
				}
		}
		if (ret < 0) {
			int failure = pci_msi_report_failure(pdev);

			if (failure < 0)
				dev_err(&pdev->dev,
					"Oberon MSI software failure handoff unresolved (%d)\n",
					failure);
			dev_err(&pdev->dev,
				"Oberon MSI recovery unresolved: outcome %u, error %d; controller remains blocked\n",
				outcome, ret);
		} else {
			dev_info(&pdev->dev,
				 "Oberon MSI notification complete: outcome %u; controller remains blocked\n",
				 outcome);
		}
		pci_dev_put(pdev);
	}
}

static DEFINE_MUTEX(oberon_msi_init_lock);
static LIST_HEAD(oberon_msi_owners);
static DEFINE_MUTEX(oberon_msi_owners_lock);

static struct oberon_msi *oberon_msi_owner(struct pci_dev *pdev)
{
	struct oberon_msi *t;

	guard(mutex)(&oberon_msi_owners_lock);
	list_for_each_entry(t, &oberon_msi_owners, node)
		if (t->pbm == pdev->dev.archdata.host_controller)
			return t;
	return NULL;
}

static int oberon_msi_claim(struct oberon_msi *t)
{
	struct oberon_msi *owner;

	guard(mutex)(&oberon_msi_owners_lock);
	list_for_each_entry(owner, &oberon_msi_owners, node)
		if (owner->pbm == t->pbm)
			return -EBUSY;
	list_add_tail(&t->node, &oberon_msi_owners);
	return 0;
}

static void oberon_msi_unclaim(struct oberon_msi *t)
{
	guard(mutex)(&oberon_msi_owners_lock);
	list_del_init(&t->node);
}

static void __oberon_msi_stop(struct oberon_msi *t)
{
	unsigned long regs = t->pbm->pbm_regs;
	unsigned int i;
	bool first = !READ_ONCE(t->failed);

	lockdep_assert_held(&t->publish_lock);
	WRITE_ONCE(t->failed, true);
	upa_writeq(upa_readq(t->imap) & ~IMAP_VALID, t->imap);
	upa_readq(t->imap);
	for (i = 0; i < 256; i++)
		upa_writeq(0, regs + MSI_MAP(i));
	upa_readq(regs + MSI_MAP(255));
	upa_writeq(EVENT_QUEUE_CONTROL_CLEAR_DIS,
		   regs + EVENT_QUEUE_CONTROL_CLEAR(0));
	upa_readq(regs + EVENT_QUEUE_STATE(0));
	/* No drain guarantee is assumed: storage and the handler remain pinned. */
	if (first)
		schedule_work(&t->fault_work);
}

static void oberon_msi_stop(struct oberon_msi *t)
{
	guard(raw_spinlock_irqsave)(&t->publish_lock);
	__oberon_msi_stop(t);
}

static bool oberon_msi_publish(struct oberon_msi *t, unsigned int msi)
{
	unsigned long regs = t->pbm->pbm_regs;

	guard(raw_spinlock_irqsave)(&t->publish_lock);
	if (READ_ONCE(t->failed)) {
		t->blocked_publications++;
		return false;
	}
	/* Publish dispatch state before making the hardware message map valid. */
	wmb();
	upa_writeq(MSI_CLEAR_EQWR_N, regs + MSI_CLEAR(msi));
	upa_writeq(MSI_MAP_VALID, regs + MSI_MAP(msi));
	if ((upa_readq(regs + MSI_MAP(msi)) &
	     (MSI_MAP_VALID | MSI_MAP_EQNUM)) != MSI_MAP_VALID) {
		__oberon_msi_stop(t);
		return false;
	}
	return true;
}

static irqreturn_t oberon_msi_interrupt(int irq, void *cookie)
{
	struct oberon_msi *t = cookie;
	unsigned long regs = t->pbm->pbm_regs;
	unsigned int n;

	if (READ_ONCE(t->failed))
		return IRQ_HANDLED;
	if (upa_readq(regs + EVENT_QUEUE_STATE(0)) != EVENT_QUEUE_STATE_ACTIVE ||
	    upa_readq(regs + EVENT_QUEUE_TAIL(0)) & EVENT_QUEUE_TAIL_OFLOW)
		goto fault;
	for (n = 0; n < 128; n++) {
		struct pci_msiq_entry *e = &t->queue[t->head];
		u64 word = READ_ONCE(e->word0);
		unsigned int msi, vector, type;

		if (!(word & MSIQ_WORD0_FMT_TYPE))
			break;
		dma_rmb();
		type = (word & MSIQ_WORD0_FMT_TYPE) >>
			(MSIQ_WORD0_FMT_TYPE_SHIFT + 3);
		msi = word & MSIQ_WORD0_DATA0;
		if ((type != MSIQ_TYPE_MSI32 && type != MSIQ_TYPE_MSI64) ||
		    msi >= 256)
			goto fault;
		upa_writeq(MSI_CLEAR_EQWR_N, regs + MSI_CLEAR(msi));
		vector = READ_ONCE(t->messages[msi].irq);
		if (vector)
			generic_handle_irq(vector);
		else
			atomic64_inc(&t->unbound_records);
		WRITE_ONCE(e->word0, word & ~MSIQ_WORD0_FMT_TYPE);
		t->head = (t->head + 1) & 127;
		atomic64_inc(&t->records);
	}
	/* Publish consumed records before advancing the hardware head. */
	dma_wmb();
	upa_writeq(t->head, regs + EVENT_QUEUE_HEAD(0));
	upa_readq(regs + EVENT_QUEUE_HEAD(0));
	return IRQ_HANDLED;

fault:
	oberon_msi_stop(t);
	dev_err(&t->pdev->dev, "Oberon MSI queue fault; delivery stopped, storage retained\n");
	return IRQ_HANDLED;
}

static struct irq_chip oberon_msi_chip = {
	.name = "Oberon-MSI",
	.irq_mask = pci_msi_mask_irq,
	.irq_unmask = pci_msi_unmask_irq,
	.irq_enable = pci_msi_unmask_irq,
	.irq_disable = pci_msi_mask_irq,
};

static int oberon_msi_owner_init(struct pci_dev *pdev);

static int oberon_msi_setup(unsigned int *irq_p, struct pci_dev *pdev,
			    struct msi_desc *desc)
{
	struct oberon_msi *t;
	struct pci_driver *driver;
	struct msi_msg msg = {};
	struct oberon_msi_peer *peer;
	unsigned int msi, irq;
	int err = -ENOSPC;

	/* Allocation runs within the requesting driver's device lifetime. */
	driver = READ_ONCE(pdev->driver);
	if (!driver || !driver->msi_lost || !driver->msi_lost_failed ||
	    !pdev->irq || IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    IS_ENABLED(CONFIG_KEXEC_CORE) || IS_ENABLED(CONFIG_PM_SLEEP))
		return -EOPNOTSUPP;
	/* Do not expose an owner to allocation before initialization finishes. */
	scoped_guard(mutex, &oberon_msi_init_lock) {
		t = oberon_msi_owner(pdev);
		if (!t) {
			err = oberon_msi_owner_init(pdev);
			if (err)
				return err;
			t = oberon_msi_owner(pdev);
		}
	}
	if (!t)
		return -ENOSPC;
	mutex_lock(&t->lock);
	if (READ_ONCE(t->failed) || t->next == 256) {
		err = -ENOSPC;
		goto unlock;
	}
	peer = oberon_msi_peer_get(t, pdev);
	if (IS_ERR(peer)) {
		err = PTR_ERR(peer);
		goto unlock;
	}
	irq = irq_alloc(0, 0);
	if (!irq) {
		err = -ENOMEM;
		oberon_msi_peer_put(t, peer);
		goto unlock;
	}
	/* Retire each message number permanently so late records cannot alias. */
	msi = t->next++;
	t->messages[msi].peer = peer;
	t->messages[msi].state = OBERON_MSG_RESERVED;
	irq_set_chip_and_handler_name(irq, &oberon_msi_chip,
				      handle_simple_irq, "MSI");
	WRITE_ONCE(t->messages[msi].irq, irq);
	if (!oberon_msi_publish(t, msi)) {
		WRITE_ONCE(t->messages[msi].irq, 0);
		synchronize_irq(t->irq);
		irq_free(irq);
		t->messages[msi].state = OBERON_MSG_QUARANTINED;
		t->messages[msi].peer = NULL;
		oberon_msi_peer_put(t, peer);
		err = -EIO;
		goto unlock;
	}
	t->messages[msi].state = OBERON_MSG_LIVE;
	if (desc->pci.msi_attrib.is_64) {
		msg.address_hi = t->pbm->msi64_start >> 32;
		msg.address_lo = t->pbm->msi64_start;
	} else {
		msg.address_lo = t->pbm->msi32_start;
	}
	msg.data = msi;
	irq_set_msi_desc(irq, desc);
	pci_write_msi_msg(irq, &msg);
	*irq_p = irq;
	err = 0;
unlock:
	mutex_unlock(&t->lock);
	return err;
}

static bool oberon_msi_endpoint_masked(struct pci_dev *pdev,
				       const struct msi_desc *desc)
{
	u16 flags;
	int cap, offset, enable;

	if (pci_dev_is_disconnected(pdev))
		return false;
	if (desc->pci.msi_attrib.is_msix) {
		cap = pdev->msix_cap;
		offset = PCI_MSIX_FLAGS;
		enable = PCI_MSIX_FLAGS_ENABLE;
	} else {
		cap = pdev->msi_cap;
		offset = PCI_MSI_FLAGS;
		enable = PCI_MSI_FLAGS_ENABLE;
	}
	/* MSI-X allocation rollback runs enabled with all vectors masked. */
	return cap && !pci_read_config_word(pdev, cap + offset, &flags) &&
		(!(flags & enable) ||
		 (desc->pci.msi_attrib.is_msix && flags & PCI_MSIX_FLAGS_MASKALL));
}

static void oberon_msi_teardown(unsigned int irq, struct pci_dev *pdev)
{
	struct oberon_msi *t = oberon_msi_owner(pdev);
	struct msi_desc *desc = irq_get_msi_desc(irq);
	unsigned int i;

	if (!t)
		return;
	mutex_lock(&t->lock);
	for (i = 0; i < t->next; i++) {
		if (!t->messages[i].peer ||
		    t->messages[i].peer->pdev != pdev ||
		    READ_ONCE(t->messages[i].irq) != irq)
			continue;
		if (!desc || !oberon_msi_endpoint_masked(pdev, desc)) {
			t->endpoint_failures++;
			oberon_msi_stop(t);
		}
		upa_writeq(0, t->pbm->pbm_regs + MSI_MAP(i));
		if (upa_readq(t->pbm->pbm_regs + MSI_MAP(i)) & MSI_MAP_VALID) {
			t->invalidation_failures++;
			oberon_msi_stop(t);
		}
		WRITE_ONCE(t->messages[i].irq, 0);
		synchronize_irq(t->irq);
		/* Samples describe teardown; they do not establish DMA completion. */
		t->release_state = upa_readq(t->pbm->pbm_regs +
					     EVENT_QUEUE_STATE(0));
		t->release_tail = upa_readq(t->pbm->pbm_regs +
					    EVENT_QUEUE_TAIL(0));
		t->release_head = READ_ONCE(t->head);
		t->releases++;
		t->messages[i].state = READ_ONCE(t->failed) ?
			OBERON_MSG_QUARANTINED : OBERON_MSG_RETIRED;
		irq_set_msi_desc(irq, NULL);
		if (desc)
			desc->irq = 0;
		irq_set_chip(irq, NULL);
		irq_free(irq);
		oberon_msi_peer_put(t, t->messages[i].peer);
		t->messages[i].peer = NULL;
		break;
	}
	mutex_unlock(&t->lock);
}

static int oberon_msi_owner_init(struct pci_dev *pdev)
{
	struct oberon_msi_geometry g;
	struct oberon_msi *t;
	struct pci_pbm_info *pbm;
	unsigned long regs, slot, i;
	dma_addr_t aligned;
	int err;

	pci_dev_get(pdev);
	pbm = pdev->dev.archdata.host_controller;
	err = -EINVAL;
	if (!pbm || !pbm->is_oberon ||
	    (pdev->class >> 8) != PCI_CLASS_NETWORK_ETHERNET)
		goto put;
	err = oberon_msi_geometry(pbm, &g);
	if (err)
		goto put;
	if (g.message_range[0] || g.messages != 256)
		goto invalid;
	regs = pbm->pbm_regs;
	if (upa_readq(regs + EVENT_QUEUE_BASE_ADDR_REG) ||
	    upa_readq(regs + MSI_32BIT_ADDR) || upa_readq(regs + MSI_64BIT_ADDR))
		goto busy;
	for (i = 0; i < 5; i++)
		if (upa_readq(regs + 0x30000 + i * 8))
			goto busy;
	for (i = 0; i < 256; i++)
		if (upa_readq(regs + MSI_MAP(i)))
			goto busy;
	for (i = 0; i < g.queues; i++)
		if (upa_readq(regs + EVENT_QUEUE_STATE(i)) != EVENT_QUEUE_STATE_IDLE ||
		    upa_readq(regs + EVENT_QUEUE_HEAD(i)) ||
		    upa_readq(regs + EVENT_QUEUE_TAIL(i)) ||
		    upa_readq(regs + 0x1000 + (g.queue_range[2] + i) * 8) ||
		    upa_readq(regs + 0x1400 + (g.queue_range[2] + i) * 8))
			goto busy;
	t = kzalloc_obj(*t);
	if (!t) {
		err = -ENOMEM;
		goto put;
	}
	INIT_LIST_HEAD(&t->node);
	INIT_LIST_HEAD(&t->peers);
	INIT_WORK(&t->fault_work, oberon_msi_fault_work);
	t->pbm = pbm;
	/* The retained owner holds the enumeration reference to this device. */
	t->pdev = pdev;
	mutex_init(&t->lock);
	raw_spin_lock_init(&t->publish_lock);
	err = oberon_msi_claim(t);
	if (err)
		goto free;
	t->allocation = dma_alloc_coherent(&pdev->dev, SZ_1M, &t->dma, GFP_KERNEL);
	if (!t->allocation) {
		err = -ENOMEM;
		goto free;
	}
	err = -EINVAL;
	if (t->dma < pbm->iommu->tbl.table_map_base ||
	    t->dma > (u64)pbm->iommu->dma_addr_mask + 1 - SZ_1M ||
	    !IS_ALIGNED(t->dma, IO_PAGE_SIZE))
		goto free_dma;
	aligned = ALIGN(t->dma, OBERON_EQ_ALIGNMENT);
	if (aligned - t->dma > SZ_1M - g.queues * OBERON_EQ_BYTES)
		goto free_dma;
	slot = (t->dma - pbm->iommu->tbl.table_map_base) >> IO_PAGE_SHIFT;
	for (i = 0; i < SZ_1M / IO_PAGE_SIZE; i++) {
		u64 expected = IOPTE_VALID | IOPTE_CACHE | IOPTE_WRITE |
			(__pa(t->allocation + (i << IO_PAGE_SHIFT)) & IOPTE_PAGE);

		if (iopte_val(pbm->iommu->page_table[slot + i]) != expected)
			goto free_dma;
	}
	t->queue = t->allocation + aligned - t->dma;
	t->imap = regs + 0x1000 + g.queue_range[2] * 8;
	t->iclr = regs + 0x1400 + g.queue_range[2] * 8;
	upa_writeq(OBERON_MSI_IMAP_MONDO | BIT_ULL(6), t->imap);
	t->irq = oberon_build_irq((pbm->portid << 6) + g.queue_range[2] - BIT(6),
				  t->iclr, t->imap);
	if (!t->irq) {
		err = -ENOMEM;
		goto restore_imap;
	}
	err = request_irq(t->irq, oberon_msi_interrupt,
			  IRQF_NO_AUTOEN | IRQF_NO_THREAD | IRQF_NOBALANCING,
			  "Oberon-MSIQ", t);
	if (err)
		goto restore_imap;
	/* From publication onward, neither the DMA mapping nor handler is freed. */
	dma_wmb();
	upa_writeq(aligned, regs + EVENT_QUEUE_BASE_ADDR_REG);
	upa_writeq(pbm->portid << 6, regs + IMONDO_DATA0);
	upa_writeq(0, regs + IMONDO_DATA1);
	pbm->msi32_start = g.addresses[1];
	pbm->msi64_start = (u64)g.addresses[3] << 32 | g.addresses[4];
	upa_writeq(pbm->msi32_start, regs + MSI_32BIT_ADDR);
	upa_writeq(pbm->msi64_start, regs + MSI_64BIT_ADDR);
	if (upa_readq(regs + EVENT_QUEUE_BASE_ADDR_REG) != aligned ||
	    upa_readq(regs + IMONDO_DATA0) != ((u64)pbm->portid << 6) ||
	    upa_readq(regs + IMONDO_DATA1) ||
	    upa_readq(regs + MSI_32BIT_ADDR) != pbm->msi32_start ||
	    upa_readq(regs + MSI_64BIT_ADDR) != pbm->msi64_start)
		goto retained;
	upa_writeq(EVENT_QUEUE_CONTROL_SET_EN, regs + EVENT_QUEUE_CONTROL_SET(0));
	if (upa_readq(regs + EVENT_QUEUE_STATE(0)) != EVENT_QUEUE_STATE_ACTIVE)
		goto retained;
	atomic64_set(&t->records, 0);
	atomic64_set(&t->unbound_records, 0);
	enable_irq(t->irq);
	dev_info(&pdev->dev, "Oberon MSI ready: queue 0, IRQ %u, DVMA %pad; storage pinned\n",
		 t->irq, &aligned);
	return 0;

retained:
	oberon_msi_stop(t);
	dev_err(&pdev->dev, "Oberon MSI initialization failed; storage and handler retained\n");
	return -EIO;
restore_imap:
	upa_writeq(0, t->imap);
free_dma:
	dma_free_coherent(&pdev->dev, SZ_1M, t->allocation, t->dma);
free:
	oberon_msi_unclaim(t);
	kfree(t);
put:
	pci_dev_put(pdev);
	return err;
busy:
	err = -EBUSY;
	goto put;
invalid:
	err = -EINVAL;
	goto put;
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
	else if (IS_ENABLED(CONFIG_PCI_MSI)) {
#ifdef CONFIG_PCI_MSI
		pbm->setup_msi_irq = oberon_msi_setup;
		pbm->teardown_msi_irq = oberon_msi_teardown;
#endif
	}

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

static int __init fire_init(void)
{
	return platform_driver_register(&fire_driver);
}

subsys_initcall(fire_init);
