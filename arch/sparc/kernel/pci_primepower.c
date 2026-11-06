// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2026 Magnus Lindholm */
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <asm/oplib.h>
#include <asm/setup.h>
#include <asm/prom.h>
#include <asm/irq.h>
#include <asm/spitfire.h>
#include <asm/upa.h>

#include "prom.h"

/* PSYCHO interrupt mapping support. */
#define PSYCHO_IMAP_A_SLOT0	0x0c00UL
#define PSYCHO_IMAP_B_SLOT0	0x0c20UL
static unsigned long psycho_pcislot_imap_offset(unsigned long ino)
{
	unsigned int bus =  (ino & 0x10) >> 4;
	unsigned int slot = (ino & 0x0c) >> 2;

	if (bus == 0)
		return PSYCHO_IMAP_A_SLOT0 + (slot * 8);
	else
		return PSYCHO_IMAP_B_SLOT0 + (slot * 8);
}

#define PSYCHO_OBIO_IMAP_BASE	0x1000UL

#define PSYCHO_ONBOARD_IRQ_BASE		0x20
#define psycho_onboard_imap_offset(__ino) \
	(PSYCHO_OBIO_IMAP_BASE + (((__ino) & 0x1f) << 3))

#define PSYCHO_ICLR_A_SLOT0	0x1400UL
#define PSYCHO_ICLR_SCSI	0x1800UL

#define psycho_iclr_offset(ino)					      \
	((ino & 0x20) ? (PSYCHO_ICLR_SCSI + (((ino) & 0x1f) << 3)) :  \
			(PSYCHO_ICLR_A_SLOT0 + (((ino) & 0x1f)<<3)))

struct primepower_pbm {
	struct primepower_controller *controller;
	unsigned int index;
};

static LIST_HEAD(primepower_controllers);
static bool primepower_maps_ready __ro_after_init;

static unsigned long psycho_imap_offset(unsigned int ino)
{
	if (ino < PSYCHO_ONBOARD_IRQ_BASE)
		return psycho_pcislot_imap_offset(ino);
	return psycho_onboard_imap_offset(ino);
}



static void __init pp250_identify_pbm(struct primepower_controller *c,
				      struct device_node *dp,
				      const struct linux_prom64_registers *regs)
{
	u32 portid = 0, u2u_id = 0;
	const u8 *range;
	int len;
	bool valid;

	range = of_get_property(dp, "u2u-tid-range", &len);
	valid = range && len == 2 && !range[0] && range[1] == 15 &&
		!of_property_read_u32(dp, "upa-portid", &portid) &&
		!of_property_read_u32(dp, "u2u#", &u2u_id) &&
		!(c->base & 7) && c->window >= c->base &&
		c->window - c->base == 0x800000 &&
		regs[0].phys_addr >= c->base && regs[0].reg_size >= 0x2000 &&
		c->window_size == regs[3].reg_size && c->base_size == regs[2].reg_size &&
		!pp250_ranges_overlap(c->base, c->base_size, c->window, c->window_size);
	pp250_record_pbm(&c->attach, regs[0].phys_addr - c->base, dp->phandle,
			 portid, u2u_id, valid);
}

static const struct pp250_u2u_io pp250_phys_io = {
	.write32 = pp250_phys_write32,
	.read32 = pp250_phys_read32,
};

struct primepower_pbm * __init
primepower_attach_pbm(struct device_node *dp, const struct linux_prom64_registers *regs)
{
	struct primepower_controller *controller;
	struct primepower_pbm *pbm;
	bool overlap = false;
	unsigned int i;

	list_for_each_entry(controller, &primepower_controllers, list) {
		if (controller->window == regs[3].phys_addr &&
		    controller->base == regs[2].phys_addr)
			goto attach;
		if (controller->window == regs[3].phys_addr ||
		    controller->base == regs[2].phys_addr)
			goto conflict;
		for (i = 2; i <= 3; i++) {
			if (pp250_ranges_overlap(regs[i].phys_addr, regs[i].reg_size,
						 controller->window, controller->window_size) ||
			    pp250_ranges_overlap(regs[i].phys_addr, regs[i].reg_size,
						 controller->base, controller->base_size)) {
				controller->attach.invalid = true;
				overlap = true;
			}
		}
	}

	controller = prom_early_alloc(sizeof(*controller));
	controller->attach.invalid = overlap;
	controller->io = &pp250_phys_io;
	controller->window_size = regs[3].reg_size;
	controller->base_size = regs[2].reg_size;
	controller->window = regs[3].phys_addr;
	controller->base = regs[2].phys_addr;
	controller->reserved_selectors = BIT(0);
	raw_spin_lock_init(&controller->lock);
	for (i = 0; i < ARRAY_SIZE(controller->maps); i++) {
		unsigned int ino = i < 8 ? i * 4 : 0x20 + i - 8;

		controller->maps[i].target_cpu = primepower_irq_boot_cpu;
		controller->maps[i].controller = controller;
		controller->maps[i].addr = controller->base + psycho_imap_offset(ino);
	}
	list_add(&controller->list, &primepower_controllers);

attach:
	for (i = 0; i < ARRAY_SIZE(controller->pbms); i++)
		if (!controller->pbms[i] || controller->pbms[i] == dp)
			break;
	if (i == ARRAY_SIZE(controller->pbms))
		goto conflict;
	pp250_identify_pbm(controller, dp, regs);
	controller->pbms[i] = dp;
	pbm = prom_early_alloc(sizeof(*pbm));
	pbm->controller = controller;
	pbm->index = i;
	return pbm;

conflict:
	prom_printf("PRIMEPOWER: conflicting Psycho register windows\n");
	prom_halt();
	return NULL;
}

static unsigned int primepower_map_index(unsigned int ino)
{
	return ino < 0x20 ? ino / 4 : 8 + ino - 0x20;
}

/* Psycho has six PCI slot maps and nineteen onboard maps, including timers. */
static bool pp250_catalog_index(unsigned int index)
{
	return index < 27 && index != 2 && index != 3;
}

static bool pp250_catalog_vector(struct primepower_controller *c, unsigned int vector)
{
	unsigned int group = vector & ~0x3fU;

	return (!group || group == c->attach.portid << 6) &&
	       pp250_catalog_index(primepower_map_index(vector & 0x3f));
}

#define PP250_TIMER0_INO	0x2c
#define PP250_TIMER1_INO	0x2d

static void __init primepower_describe(struct primepower_controller *c,
				       unsigned int pbm, unsigned int ino)
{
	unsigned int pin;

	if (!pp250_catalog_vector(c, ino)) {
		c->inventory_complete = false;
		return;
	}
	ino &= 0x3f;
	/* A conflicting PCI description cannot authorize timer MMIO. */
	if (ino == PP250_TIMER0_INO || ino == PP250_TIMER1_INO) {
		c->inventory_complete = false;
		return;
	}
	pin = ino < 0x20 ? ino & 3 : 0;
	c->maps[primepower_map_index(ino)].described[pbm] |= BIT(pin);
}

/* Only the four root-slot entries may select the PCI#01 physical map. */
static void __init pp250_record_pin(struct primepower_controller *c, unsigned int pbm,
				    struct device_node *bus, unsigned int na,
				    const __be32 *entry, unsigned int ino)
{
	const __be32 *mask;
	unsigned int pin = be32_to_cpu(entry[na]);
	bool root = bus == c->pbms[pbm];
	bool slot3 = root && na == 3 &&
		     (be32_to_cpu(entry[0]) & 0x00fff800) == (pp250_pci01_source.devfn << 8);
	int len;

	if (slot3)
		c->pci01_rows[pbm]++;
	if (primepower_map_index(ino & 0x3f) != pp250_pci01_source.map_index)
		return;
	mask = of_get_property(bus, "interrupt-map-mask", &len);
	if (!slot3 || !mask || len != 4 * sizeof(*mask) ||
	    be32_to_cpu(mask[0]) != 0x00fff800 || mask[1] || mask[2] ||
	    be32_to_cpu(mask[3]) != 7 || be32_to_cpu(entry[na + 1]) != bus->phandle ||
	    pin < 1 || pin > 4 || (ino & 0x3f) != 0x10 + pin - 1 ||
	    (c->pci01_pins[pbm] & BIT(pin - 1))) {
		c->pci01_alias = true;
		return;
	}
	c->pci01_pins[pbm] |= BIT(pin - 1);
}

static unsigned int pp250_source_pins(struct primepower_controller *c)
{
	unsigned int i;

	if (!c->inventory_complete || c->pci01_alias)
		return 0;
	for (i = 0; i < ARRAY_SIZE(c->pbms); i++)
		if (c->pci01_pins[i] == 0xf && c->pci01_rows[i] == 4 &&
		    !c->pci01_pins[i ^ 1])
			return 0xf;
	return 0;
}

/* Identify the SCF row itself; another description of its map is an alias. */
static void __init pp250_describe_scf(struct primepower_controller *c,
				      struct device_node *bus, unsigned int na,
				      const __be32 *entry, unsigned int ino)
{
	struct primepower_imap *map = &c->maps[primepower_map_index(0x23)];
	struct device_node *scf;
	const __be32 *reg;
	u32 pin;
	int bytes;

	if ((ino & 0x3f) != 0x23)
		return;
	scf = of_find_node_by_path("/pci@83,4000/ebus@1/FJSV,scfc@14,210000");
	reg = scf ? of_get_property(scf, "reg", &bytes) : NULL;
	if (!map->scf_source && reg && bytes >= 3 * sizeof(*reg) &&
	    scf->parent == bus && bus->parent &&
	    be32_to_cpu(entry[na + 1]) == bus->parent->phandle && na == 2 &&
	    reg[0] == entry[0] && reg[1] == entry[1] &&
	    of_property_count_u32_elems(scf, "interrupts") == 1 &&
	    !of_property_read_u32(scf, "interrupts", &pin) &&
	    pin == be32_to_cpu(entry[na]))
		map->scf_source = true;
	else
		map->scf_alias = true;
	of_node_put(scf);
}

/* SPARC firmware uses na address cells, a pin, a phandle and a target INO. */
static bool __init primepower_describe_bus(struct primepower_controller *c,
					   unsigned int pbm, struct device_node *bus)
{
	const __be32 *entry;
	u32 na, ni;
	int bytes;

	entry = of_get_property(bus, "interrupt-map", &bytes);
	if (!entry)
		return !of_property_present(bus, "interrupt-map");
	if (of_property_read_u32(bus, "#address-cells", &na) || na > 3 ||
	    of_property_read_u32(bus, "#interrupt-cells", &ni) || ni != 1 ||
	    bytes <= 0 || bytes % ((na + 3) * sizeof(*entry))) {
		pr_warn("PRIMEPOWER: unsupported interrupt-map layout on %pOF\n", bus);
		return false;
	}
	while (bytes) {
		phandle parent = be32_to_cpu(entry[na + 1]);
		unsigned int ino = be32_to_cpu(entry[na + 2]);
		int j;

		for (j = 0; j < ARRAY_SIZE(c->pbms); j++)
			if (c->pbms[j] && c->pbms[j]->phandle == parent) {
				pp250_record_pin(c, pbm, bus, na, entry, ino);
				pp250_describe_scf(c, bus, na, entry, ino);
				primepower_describe(c, pbm, ino);
				break;
			}
		if (j == ARRAY_SIZE(c->pbms)) {
			pr_warn("PRIMEPOWER: unknown interrupt parent %x on %pOF\n", parent, bus);
			return false;
		}
		entry += na + 3;
		bytes -= (na + 3) * sizeof(*entry);
	}
	return true;
}

static bool __init primepower_describe_subtree(struct primepower_controller *c,
					       unsigned int pbm, struct device_node *bus)
{
	struct device_node *child;
	bool complete = primepower_describe_bus(c, pbm, bus);

	for_each_child_of_node(bus, child)
		if (!primepower_describe_subtree(c, pbm, child))
			complete = false;

	return complete;
}

static bool __init pp250_describe_timers(struct primepower_controller *c)
{
	struct device_node *dp __free(device_node) =
		of_get_child_by_name(of_root, "counter-timer");
	const struct linux_prom64_registers *regs;
	const __be32 *vectors;
	u32 first = (c->attach.portid << 6) | PP250_TIMER0_INO;
	int len;

	if (!dp || !of_device_is_available(dp))
		return false;
	regs = of_get_property(dp, "reg", &len);
	if (!regs || len != 3 * sizeof(*regs) || c->base_size < 0x1c20 ||
	    regs[0].phys_addr != c->base + 0x1c00 || regs[0].reg_size != 0x20 ||
	    regs[1].phys_addr != c->base + psycho_iclr_offset(PP250_TIMER0_INO) ||
	    regs[1].reg_size != 0x10 ||
	    regs[2].phys_addr != c->base + psycho_imap_offset(PP250_TIMER0_INO) ||
	    regs[2].reg_size != 0x10)
		return false;

	vectors = of_get_property(dp, "interrupts", &len);
	if (!vectors || len != 2 * sizeof(*vectors) ||
	    be32_to_cpu(vectors[0]) != first || be32_to_cpu(vectors[1]) != first + 1)
		return false;

	c->maps[primepower_map_index(PP250_TIMER0_INO)].timer_source = true;
	c->maps[primepower_map_index(PP250_TIMER1_INO)].timer_source = true;
	return true;
}

static int __init primepower_describe_maps(void)
{
	struct primepower_controller *c;
	int i;

	list_for_each_entry(c, &primepower_controllers, list) {
		c->inventory_complete = true;
		for (i = 0; i < ARRAY_SIZE(c->pbms); i++) {
			struct device_node *bus = c->pbms[i];
			const __be32 *vec;
			int bytes;

			if (!bus) {
				c->inventory_complete = false;
				continue;
			}
			if (!of_property_present(bus, "interrupt-map"))
				c->inventory_complete = false;
			if (!primepower_describe_subtree(c, i, bus))
				c->inventory_complete = false;
			vec = of_get_property(bus, "interrupts", &bytes);
			if (!vec || bytes <= 0 || bytes % sizeof(*vec))
				c->inventory_complete = false;
			while (vec && bytes >= (int)sizeof(*vec)) {
				unsigned int ino = be32_to_cpup(vec++);

				if (!pp250_catalog_vector(c, ino)) {
					c->inventory_complete = false;
					bytes -= sizeof(*vec);
					continue;
				}
				ino &= 0x3f;
				if (primepower_map_index(ino) == 4)
					c->pci01_alias = true;
				c->maps[primepower_map_index(ino)].bridge_pbms |= BIT(i);
				primepower_describe(c, i, ino);
				bytes -= sizeof(*vec);
			}
		}
		if (!pp250_describe_timers(c)) {
			c->inventory_complete = false;
			pr_warn("PRIMEPOWER: incomplete root counter-timer description\n");
		}
		pp250_finish_inventory(&c->attach, c->inventory_complete);
		for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
			struct primepower_imap *map = &c->maps[i];
			unsigned long flags, value;
			const char *reason;
			bool programmed;

			if (!c->inventory_complete)
				reason = "incomplete-inventory";
			else if (map->timer_source)
				reason = "counter-timer-selector-unread";
			else if (!map->described[0] && !map->described[1])
				reason = "undescribed";
			else
				reason = NULL;
			if (reason) {
				pr_info("pp250 pre-enumeration imap=%lx unknown=%s\n",
					map->addr, reason);
				continue;
			}
			raw_spin_lock_irqsave(&c->lock, flags);
			value = primepower_observe_imap(c, upa_readq(map->addr));
			programmed = map->programmed;
			raw_spin_unlock_irqrestore(&c->lock, flags);
			pr_info("pp250 pre-enumeration imap=%lx value=%lx valid=%u selector=%lu programmed=%u\n",
				map->addr, value, !!(value & IMAP_VALID),
				(value & IMAP_TID_UPA) >> IMAP_TID_SHIFT,
				programmed);
		}
	}
	return 0;
}
early_initcall(primepower_describe_maps);

static int __init primepower_enable_handoff(void)
{
	primepower_maps_ready = !list_empty(&primepower_controllers);
	return 0;
}
subsys_initcall(primepower_enable_handoff);

/* Reserve terminal entry before a route changes, or keep firmware out until handback. */
enum pp250_gate_state {
	PP250_GATE_IDLE = 0,
	PP250_GATE_ROUTING = BIT(0),
	PP250_GATE_TERMINAL = BIT(1),
	PP250_GATE_HANDOFF = BIT(2),
	PP250_GATE_PROM = BIT(3),
};

static atomic_t pp250_gate = ATOMIC_INIT(PP250_GATE_IDLE);
static int pp250_fw_owner = -1;

static bool pp250_routing_gate(void)
{
	int state = atomic_read_acquire(&pp250_gate);

	return state == PP250_GATE_ROUTING ||
	       (state == (PP250_GATE_ROUTING | PP250_GATE_HANDOFF) &&
		READ_ONCE(pp250_fw_owner) == raw_smp_processor_id());
}

static bool pp250_route_start(void)
{
	return atomic_cmpxchg(&pp250_gate, PP250_GATE_IDLE,
			      PP250_GATE_ROUTING) == PP250_GATE_IDLE;
}

static void pp250_route_done(void)
{
	atomic_fetch_andnot_release(PP250_GATE_ROUTING, &pp250_gate);
}

/* Count admitted calls, including those waiting for the PROM entry lock. */
int primepower_prom_begin(void)
{
	int state;

	if (!primepower_maps_ready)
		return 0;
	state = atomic_read_acquire(&pp250_gate);
	do {
		if (state & (PP250_GATE_ROUTING | PP250_GATE_TERMINAL))
			return -EBUSY;
		if ((state & PP250_GATE_HANDOFF) &&
		    READ_ONCE(pp250_fw_owner) != raw_smp_processor_id())
			return -EBUSY;
	} while (!atomic_try_cmpxchg(&pp250_gate, &state, state + PP250_GATE_PROM));
	return 1;
}

void primepower_prom_end(void)
{
	atomic_fetch_sub_release(PP250_GATE_PROM, &pp250_gate);
}

int primepower_terminal_begin(void)
{
	if (!primepower_maps_ready)
		return 0;
	if (atomic_fetch_or(PP250_GATE_TERMINAL, &pp250_gate) &
	    ~PP250_GATE_TERMINAL)
		return -EBUSY;
	return 1;
}

static bool primepower_map_known(const struct primepower_imap *map)
{
	/* The firmware timer bank is inventory-only, including across PROM calls. */
	return !map->timer_source && (map->observed || map->described[0] || map->described[1]);
}

static bool primepower_map_conflict(const struct primepower_imap *map)
{
	unsigned long mask = IMAP_VALID | IMAP_TID_UPA | IMAP_IGN | IMAP_INO;

	/* OBP masks and reenables its slot-zero SCF route across enter/go. */
	if (map->controller->inventory_complete && map->scf_source && !map->scf_alias &&
	    !map->bridge_pbms && !map->programmed && !map->leased_slot &&
	    (map->before_prom & IMAP_INO) == 0x23 &&
	    !((map->before_prom | map->after_prom) & IMAP_TID_UPA))
		mask &= ~IMAP_VALID;

	return ((map->before_prom ^ map->after_prom) & mask) ||
	       (map->programmed && ((map->after_prom ^ map->last_programmed) & mask)) ||
	       ((map->after_prom & IMAP_VALID) && (map->after_prom & IMAP_TID_UPA));
}

/* PCI#01 has four shared pins; keep the post-capture path non-sleeping. */
static bool pp250_pins_idle(struct primepower_controller *c)
{
	return !(upa_readq(c->base + pp250_pci01_source.pending_offset) &
		 pp250_pci01_source.pending_mask);
}

/* PCI#01 shares one map across all four pin state fields. */

static bool pp250_provider_valid(const struct pp250_u2u_ops *ops)
{
	return ops && ops->release && (!!ops->acquire != !!ops->assign);
}

/* The provider must establish slot authority and quiesce every listed pin before offering. */
int primepower_offer_pci01_slot(struct primepower_controller *c,
			       unsigned int slot, unsigned int generation,
			       u8 quiesced_pins, const struct pp250_u2u_ops *ops)
{
	struct primepower_imap *map = pp250_route_map(c);
	struct pp250_u2u_owner *owner = &c->u2u;
	u8 required = map->described[0] | map->described[1] |
		      map->pins[0] | map->pins[1];
	unsigned int i;

	lockdep_assert_held(&c->lock);
	if (!slot || slot >= ARRAY_SIZE(owner->slots) || !pp250_provider_valid(ops) ||
	    !c->io || !c->io->write32 || !c->io->read32)
		return -EINVAL;
	if (generation != c->returns)
		return -ESTALE;
	if (!c->inventory_complete || !c->pbms[0] || !c->pbms[1])
		return -ENODATA;
	if (map->phase != PP250_PREPARED || c->handoff_depth || map->quarantined ||
	    owner->delegated || map->leased_slot || (c->reserved_selectors & BIT(slot)) ||
	    !required || pp250_source_pins(c) != required || quiesced_pins != required ||
	    !pp250_routing_gate())
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(owner->slots); i++)
		if (owner->slots[i].map)
			return -EBUSY;
	if (map->bridge_pbms || map->scf_source || map->scf_alias ||
	    !pp250_authorized(&c->attach, ops, slot))
		return -EOPNOTSUPP;
	owner->authority_generation = c->attach.generation;
	owner->ops = ops;
	owner->epoch = generation;
	owner->delegated = BIT(slot);
	map->grant_epoch = generation;
	map->quiesced_pins = quiesced_pins;
	return 0;
}

/* Both PBMs use one canonical map per physical IMAP in this controller. */
static int pp250_find_slot(struct primepower_imap *map, u16 eligible)
{
	struct primepower_controller *c = map->controller;
	unsigned int slot;

	lockdep_assert_held(&c->lock);
	eligible &= ~(c->reserved_selectors | BIT(0));
	for (slot = 1; slot < ARRAY_SIZE(c->u2u.slots); slot++) {
		struct primepower_imap *used = c->u2u.slots[slot].map;

		if (used == map)
			return slot;
		if (used)
			eligible &= ~BIT(slot);
	}
	if (!eligible)
		return -ENOSPC;
	return __ffs(eligible);
}

static int pp250_assign_os_slot(struct primepower_imap *map, unsigned int slot)
{
	struct primepower_controller *c = map->controller;

	if (map->active_pins || c->u2u.slots[slot].map ||
	    pp250_find_slot(map, c->u2u.delegated) != slot)
		return -EBUSY;
	return 0;
}

static int pp250_release_os_slot(struct primepower_imap *map, unsigned int slot)
{
	if (map->active_pins || pp250_find_slot(map, 0) != slot)
		return -EBUSY;
	return 0;
}

static const struct pp250_u2u_ops pp250_os_provider = {
	.assign = pp250_assign_os_slot,
	.release = pp250_release_os_slot,
};

static int pp250_interval_release(struct primepower_imap *map, unsigned int slot)
{
	struct primepower_controller *c = map->controller;
	int err = pp250_release_os_slot(map, slot);

	if (err)
		return err;
	return c->interval_policy->handback(map, slot);
}

static const struct pp250_u2u_ops pp250_interval_provider = {
	.assign = pp250_assign_os_slot,
	.release = pp250_interval_release,
};

static int pp250_grant_slots(struct primepower_controller *c, u64 generation, u16 eligible,
			     const struct pp250_u2u_ops *ops)
{
	int slot;

	lockdep_assert_held(&c->lock);
	if (!eligible || c->handoff_depth ||
	    pp250_route_map(c)->quarantined || pp250_route_map(c)->leased_slot || c->u2u.delegated)
		return -EBUSY;
	slot = pp250_find_slot(pp250_route_map(c), eligible);
	if (slot < 0)
		return slot;
	if (!(eligible & BIT(slot)) || c->u2u.slots[slot].map)
		return -EBUSY;
	if (!pp250_publish_authority(&c->attach, generation, ops,
				     slot, c->reserved_selectors))
		return -EACCES;
	return 0;
}

/* The platform must guarantee exclusive first use and handback after drained release. */
int primepower_grant_pci01_slots(struct primepower_controller *c, u64 generation,
				u16 eligible)
{
	lockdep_assert_held(&c->lock);
	if (pp250_route_map(c)->phase != PP250_HOME || c->interval_policy)
		return -EBUSY;
	return pp250_grant_slots(c, generation, eligible, &pp250_os_provider);
}

/* Called only after the closed endpoint's IMAP is masked and all pins have drained. */
static int pp250_prepare_offer(struct primepower_controller *c)
{
	const struct pp250_interval_policy *policy = c->interval_policy;
	struct primepower_imap *map = pp250_route_map(c);
	int err;

	lockdep_assert_held(&c->lock);
	if (!policy) {
		if (c->attach.provider != &pp250_os_provider)
			return 0;
	} else {
		if (!policy->eligible || !policy->handback)
			return -EINVAL;
		if (map->phase != PP250_PREPARED || c->handoff_depth || map->active_pins ||
		    map->quarantined || !c->attach.ready || c->attach.provider ||
		    !pp250_routing_gate())
			return -EBUSY;
		if (map->saved_route != (IMAP_VALID | pp250_pci01_source.vector) ||
		    upa_readq(map->addr) != (map->saved_route & ~IMAP_VALID) ||
		    !pp250_pins_idle(c))
			return -EBUSY;
		err = pp250_grant_slots(c, c->attach.generation, policy->eligible(c),
					&pp250_interval_provider);
		if (err)
			return err;
	}
	err = primepower_offer_pci01_slot(c, c->attach.slot, c->returns,
					 pp250_source_pins(c), c->attach.provider);
	if (err && policy)
		pp250_revoke_authority(&c->attach);
	return err;
}

#define PP250_U2U_STATUS	0x00
#define PP250_U2U_DEST_BASE	0x10
#define PP250_U2U_DEST_STRIDE	0x08
#define PP250_U2U_MODE_BASE	0x40
#define PP250_U2U_UE_CONV	0x80

static bool pp250_mode_supported(u32 status)
{
	return (status & ~PP250_U2U_UE_CONV) == PP250_U2U_MODE_BASE;
}

static int pp250_verify_mode(struct primepower_controller *c, u32 expected)
{
	u32 status = ~0U;
	int err;

	err = c->io->read32(c->window + PP250_U2U_STATUS, &status);
	if (err)
		return err;
	return status == expected ? 0 : -EIO;
}

static const struct pp250_interval_policy pp250_catalog_policy;
static int pp250_catalog_check(struct primepower_controller *c, unsigned long source);
static int pp250_catalog_check_active(struct primepower_imap *map);

static int __pp250_set_slot_target(struct primepower_imap *map, unsigned int slot,
				   u32 target, bool check_catalog)
{
	struct primepower_controller *c = map->controller;
	const struct pp250_u2u_io *io = c->io;
	unsigned long offset;
	u32 expected;
	int err;

	lockdep_assert_held(&c->lock);
	if (!slot || slot >= ARRAY_SIZE(c->u2u.slots) || target > U8_MAX ||
	    map->leased_slot != slot || c->u2u.slots[slot].map != map)
		return -EINVAL;
	offset = PP250_U2U_DEST_BASE + slot * PP250_U2U_DEST_STRIDE;
	if (offset + sizeof(u32) > c->window_size ||
	    c->window > ~0UL - offset - (sizeof(u32) - 1))
		return -ERANGE;

	if (check_catalog && c->interval_policy == &pp250_catalog_policy) {
		err = pp250_catalog_check(c, map->last_programmed & ~IMAP_VALID);
		if (err)
			return err;
	}
	expected = c->u2u.slots[slot].mode_status;
	if (!pp250_mode_supported(expected))
		return -EINVAL;
	err = pp250_verify_mode(c, expected);
	if (err)
		return err;
	err = io->write32(c->window + offset, target);
	if (err)
		return err;
	return pp250_verify_mode(c, expected);
}

static int pp250_set_slot_target(struct primepower_imap *map, unsigned int slot,
				 u32 target)
{
	return __pp250_set_slot_target(map, slot, target, true);
}

static int pp250_release_slot(struct primepower_imap *map)
{
	struct pp250_u2u_owner *owner = &map->controller->u2u;
	unsigned int slot = map->leased_slot;

	if (map->query_refs)
		return -EBUSY;
	if (owner->ops->release(map, slot))
		return -EIO;
	owner->slots[slot] = (struct pp250_u2u_slot){};
	map->leased_slot = 0;
	return 0;
}

static bool pp250_claim_unused(struct primepower_controller *c, unsigned int slot)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
		struct primepower_imap *map = &c->maps[i];
		unsigned long live;

		if (!primepower_map_known(map))
			continue;
		live = primepower_observe_imap(c, upa_readq(map->addr));
		if (map == pp250_route_map(c) &&
		    live != (pp250_route_map(c)->saved_route & ~IMAP_VALID))
			return false;
	}
	return !(c->reserved_selectors & BIT(slot));
}

/* Solaris pcipsy checks the shared mode and port ID at controller attach. */
static int pp250_check_controller(struct primepower_controller *c, u32 *mode_status)
{
	u32 status = ~0U, portid = ~0U;
	int err;

	if (c->window_size < 0xc || c->window > ~0UL - 0xb)
		return -ERANGE;
	err = c->io->read32(c->window + 8, &portid);
	if (err)
		return err;
	if (portid != c->attach.portid)
		return -ENODEV;
	err = c->io->read32(c->window + PP250_U2U_STATUS, &status);
	if (err)
		return err;
	if (!pp250_mode_supported(status))
		return -EOPNOTSUPP;
	*mode_status = status;
	return 0;
}

/* A verified mask prevents new delivery, but does not drain in-flight pins. */
static int pp250_enable_route(struct primepower_imap *map, unsigned long route)
{
	struct primepower_controller *c = map->controller;
	unsigned long live;

	lockdep_assert_held(&c->lock);
	map->containment = PP250_CONTAIN_NONE;
	upa_writeq(route, map->addr);
	live = upa_readq(map->addr);
	if (live == route && pp250_pins_idle(c))
		return 0;

	map->quarantined = true;
	pp250_route_map(c)->phase = PP250_UNCERTAIN;
	map->containment = PP250_CONTAIN_MAP_CHANGED;
	map->containment_readback = live;
	/* Do not overwrite an unexpected route, including a late selector change. */
	if (live != route)
		return -EIO;
	live = upa_readq(map->addr);
	map->containment_readback = live;
	if (live != route)
		return -EIO;

	map->containment = PP250_CONTAIN_MASK_UNCONFIRMED;
	upa_writeq(route & ~IMAP_VALID, map->addr);
	live = upa_readq(map->addr);
	map->containment_readback = live;
	if (live == (route & ~IMAP_VALID))
		map->containment = PP250_CONTAIN_MASK_CONFIRMED;
	return -EIO;
}

static int pp250_publish_route(struct primepower_imap *map, unsigned int slot,
			       unsigned int cpu)
{
	unsigned long route = map->saved_route | ((unsigned long)slot << IMAP_TID_SHIFT);

	if (map->startup_pending)
		route &= ~IMAP_VALID;
	if (pp250_enable_route(map, route))
		return -EIO;
	map->last_programmed = route;
	map->programmed = true;
	map->target_cpu = cpu;
	map->phase = PP250_ROUTED;
	return 0;
}

/* Resource setup leaves the route masked until IRQ-core startup owns the descriptor. */
int primepower_route_startup(struct primepower_imap *map)
{
	struct primepower_controller *c = map->controller;
	unsigned long route = map->last_programmed, live;

	lockdep_assert_held(&c->lock);
	if (upa_readq(map->addr) != route)
		goto uncertain;
	if (!map->startup_pending)
		return 0;
	if (!pp250_routing_gate() || c->query_active || c->fw_pending ||
	    !map->leased_slot || c->u2u.slots[map->leased_slot].map != map ||
	    c->u2u.epoch != c->returns || map->grant_epoch != c->returns)
		goto uncertain;
	route |= IMAP_VALID;
	upa_writeq(route, map->addr);
	live = upa_readq(map->addr);
	if (live != route)
		goto uncertain;
	map->last_programmed = route;
	map->startup_pending = false;
	return 0;
uncertain:
	map->quarantined = true;
	map->phase = PP250_UNCERTAIN;
	return -EIO;
}

/* Called under the controller lock with PCI#01 closed and its four pins drained. */
static int pp250_forward_route(struct primepower_controller *c, unsigned int cpu)
{
	struct primepower_imap *map = pp250_route_map(c);
	struct pp250_u2u_owner *owner = &c->u2u;
	unsigned int i, slot;
	u16 available = owner->delegated & ~BIT(0);
	u32 mode_status, previous = U8_MAX + 1;
	int err;
	u8 bound = map->pins[0] | map->pins[1];
	u8 required = bound | map->described[0] | map->described[1];

	if (!available || !pp250_provider_valid(owner->ops) ||
	    !c->io || !c->io->write32 || !c->io->read32)
		return -EOPNOTSUPP;
	/* The Fujitsu dispatch-slot layout places the target in the low byte. */
	if (cpu > U8_MAX)
		return -ERANGE;
	if (IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    (cpu == primepower_irq_boot_cpu && !c->fw_pending && !map->startup_pending))
		return -EOPNOTSUPP;
	if (!c->inventory_complete)
		return -ENODATA;
	available &= ~c->reserved_selectors;
	if (!available)
		return -EBUSY;
	if (owner->epoch != c->returns || map->grant_epoch != c->returns)
		return -ESTALE;
	/* Only pin A is synchronized here. The owner must cover all advertised pins. */
	if (!map->irq[0] || bound != BIT(0) || (required & ~map->quiesced_pins))
		return -EBUSY;
	if (!c->pbms[0] || !c->pbms[1] || cpu >= nr_cpu_ids || !cpu_online(cpu) ||
	    map->phase != PP250_PREPARED || c->handoff_depth || map->leased_slot)
		return -EBUSY;
	if (map->saved_route != (IMAP_VALID | pp250_pci01_source.vector) ||
	    upa_readq(map->addr) != (map->saved_route & ~IMAP_VALID) ||
	    !pp250_pins_idle(c))
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
		unsigned long live;

		if (c->maps[i].quarantined)
			return -EBUSY;
		if (!primepower_map_known(&c->maps[i]))
			continue;
		live = upa_readq(c->maps[i].addr);
		primepower_observe_imap(c, live);
	}
	available &= ~c->reserved_selectors;
	for (i = 1; i < ARRAY_SIZE(owner->slots); i++) {
		if (owner->slots[i].map == map)
			goto uncertain;
		if (owner->slots[i].map)
			available &= ~BIT(i);
	}
	if (!available)
		return -EBUSY;
	slot = __ffs(available);
	if (owner->authority_generation != c->attach.generation)
		return -ESTALE;
	if (!pp250_authorized(&c->attach, owner->ops, slot))
		return -EOPNOTSUPP;
	if (!pp250_routing_gate())
		return -EBUSY;
	err = pp250_check_controller(c, &mode_status);
	if (err)
		return err;
	if (owner->ops->assign)
		err = owner->ops->assign(map, slot);
	else
		err = owner->ops->acquire(map, slot, &previous);
	if (err)
		return err;
	/* Only reversible loans have an inherited target to restore. */
	if (owner->ops->acquire)
		owner->slots[slot].previous = previous;
	owner->slots[slot].mode_status = mode_status;
	owner->slots[slot].map = map;
	map->leased_slot = slot;
	/* Verify both PBMs again under the provider's exclusive claim. */
	err = -ERANGE;
	if (owner->ops->assign || previous <= U8_MAX)
		err = pp250_claim_unused(c, slot) && pp250_pins_idle(c) &&
		      pp250_routing_gate() ? 0 : -EBUSY;
	if (err) {
		if (pp250_release_slot(map))
			goto uncertain;
		if (owner->ops == &pp250_interval_provider) {
			pp250_revoke_authority(&c->attach);
			owner->delegated = 0;
			owner->ops = NULL;
		}
		return err;
	}
	if (pp250_set_slot_target(map, slot, cpu))
		goto uncertain;
	/* Keep the source masked if quiescence was lost during programming. */
	if (!pp250_pins_idle(c))
		goto uncertain;
	if (!pp250_routing_gate()) {
		if (owner->ops->assign || pp250_set_slot_target(map, slot, previous) ||
		    pp250_release_slot(map))
			goto uncertain;
		return -EBUSY;
	}
	return pp250_publish_route(map, slot, cpu);

uncertain:
	map->quarantined = true;
	map->phase = PP250_UNCERTAIN;
	return -EIO;
}

/* The map is masked and synchronize_irq() has completed before PREPARED. */
static int pp250_route_target(struct primepower_controller *c, unsigned int cpu)
{
	struct primepower_imap *map = pp250_route_map(c);
	struct pp250_u2u_owner *owner = &c->u2u;
	unsigned int i, slot = map->leased_slot;
	unsigned long route;

	if (map->active_pins && !map->startup_pending)
		return -EBUSY;
	if (!slot || !owner->ops || !owner->ops->assign)
		return pp250_forward_route(c, cpu);
	if ((map->phase != PP250_PREPARED &&
	     !(map->startup_pending && map->phase == PP250_ROUTED)) ||
	    c->handoff_depth || map->quarantined ||
	    !pp250_routing_gate())
		return -EBUSY;
	if (cpu >= nr_cpu_ids || cpu > U8_MAX || !cpu_online(cpu))
		return -EINVAL;
	if (owner->authority_generation != c->attach.generation ||
	    owner->epoch != c->returns || map->grant_epoch != c->returns ||
	    !pp250_authorized(&c->attach, owner->ops, slot))
		goto uncertain;
	if (slot >= ARRAY_SIZE(owner->slots) || owner->slots[slot].map != map ||
	    ((owner->ops == &pp250_os_provider || owner->ops == &pp250_interval_provider) &&
	     pp250_find_slot(map, 0) != slot))
		goto uncertain;
	route = map->saved_route | ((unsigned long)slot << IMAP_TID_SHIFT);
	if ((map->startup_pending ? route & ~IMAP_VALID : route) != map->last_programmed ||
	    upa_readq(map->addr) != (route & ~IMAP_VALID))
		goto uncertain;
	for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
		struct primepower_imap *other = &c->maps[i];

		if (other->quarantined)
			goto uncertain;
		if (other == map || !primepower_map_known(other))
			continue;
		if (((upa_readq(other->addr) & IMAP_TID_UPA) >> IMAP_TID_SHIFT) == slot)
			goto uncertain;
	}
	if (!pp250_pins_idle(c))
		return -EBUSY;
	if (pp250_set_slot_target(map, slot, cpu) || !pp250_pins_idle(c) ||
	    !pp250_routing_gate())
		goto uncertain;
	return pp250_publish_route(map, slot, cpu);

uncertain:
	map->quarantined = true;
	map->phase = PP250_UNCERTAIN;
	return -EIO;
}

/* Keep an existing assignment; acquisition and release still require a closed source. */
static bool pp250_active_assignment(struct primepower_imap *map)
{
	struct primepower_controller *c = map->controller;
	struct pp250_u2u_owner *owner = &c->u2u;
	unsigned int slot = map->leased_slot;

	return !IS_ENABLED(CONFIG_HOTPLUG_CPU) && map == pp250_route_map(c) &&
	       map->phase == PP250_ROUTED && !c->handoff_depth && !c->fw_pending &&
	       !c->query_active && !map->startup_pending && !map->quarantined && map->programmed &&
	       map->active_pins == BIT(0) && map->irq[0] &&
	       !map->irq[1] && !map->irq[2] && !map->irq[3] &&
	       (map->pins[0] | map->pins[1]) == BIT(0) &&
	       pp250_source_pins(c) == 0xf && !map->bridge_pbms &&
	       !map->scf_source && !map->scf_alias && !map->timer_source &&
	       slot && slot < ARRAY_SIZE(owner->slots) &&
	       owner->ops == &pp250_interval_provider &&
	       c->interval_policy == &pp250_catalog_policy &&
	       owner->slots[slot].map == map && pp250_find_slot(map, 0) == slot &&
	       owner->authority_generation == c->attach.generation &&
	       owner->epoch == c->returns && map->grant_epoch == c->returns &&
	       pp250_authorized(&c->attach, owner->ops, slot) &&
	       pp250_routing_gate();
}

/* The IRQ descriptor lock excludes EOI; never wait for a handler under that lock. */
static int pp250_active_affinity(struct primepower_imap *map, unsigned int irq,
				 unsigned int cpu)
{
	struct primepower_controller *c = map->controller;
	unsigned long route = map->last_programmed, live;
	int err;

	if (irq != map->irq[0] || cpu >= nr_cpu_ids || cpu > U8_MAX || !cpu_online(cpu))
		return -EINVAL;
	if (!pp250_active_assignment(map))
		return -EBUSY;
	if (!(route & IMAP_VALID) ||
	    route != (map->saved_route | ((unsigned long)map->leased_slot << IMAP_TID_SHIFT)))
		goto uncertain;
	if (upa_readq(map->addr) != route)
		goto uncertain;
	if (!pp250_pins_idle(c))
		return -EBUSY;

	upa_writeq(route & ~IMAP_VALID, map->addr);
	if (upa_readq(map->addr) != (route & ~IMAP_VALID))
		goto uncertain;
	err = pp250_catalog_check_active(map);
	if (err == -EAGAIN) {
		err = -EBUSY;
		goto restore;
	}
	if (err)
		goto uncertain;
	if (!pp250_pins_idle(c) || !pp250_routing_gate()) {
		err = -EBUSY;
		goto restore;
	}
	/* The masked, drained map retains its slot while the destination changes. */
	if (__pp250_set_slot_target(map, map->leased_slot, cpu, false))
		goto uncertain;
	if (!pp250_routing_gate())
		goto uncertain;
	upa_writeq(route, map->addr);
	if (upa_readq(map->addr) != route)
		goto uncertain;
	map->target_cpu = cpu;
	return cpu;

restore:
	/* A refused transition before the destination write leaves the old route usable. */
	if (upa_readq(map->addr) != (route & ~IMAP_VALID))
		goto uncertain;
	upa_writeq(route, map->addr);
	if (upa_readq(map->addr) == route)
		return err;
uncertain:
	map->quarantined = true;
	map->phase = PP250_UNCERTAIN;
	map->containment = PP250_CONTAIN_MAP_CHANGED;
	live = upa_readq(map->addr);
	map->containment_readback = live;
	if (live == (route & ~IMAP_VALID)) {
		map->containment = PP250_CONTAIN_MASK_CONFIRMED;
	} else if (live == route) {
		map->containment = PP250_CONTAIN_MASK_UNCONFIRMED;
		upa_writeq(route & ~IMAP_VALID, map->addr);
		live = upa_readq(map->addr);
		map->containment_readback = live;
		if (live == (route & ~IMAP_VALID))
			map->containment = PP250_CONTAIN_MASK_CONFIRMED;
	}
	return -EIO;
}

int primepower_affinity_target(struct primepower_imap *map, unsigned int irq,
			      const struct cpumask *mask)
{
	struct primepower_controller *c = map->controller;
	int ret;

	lockdep_assert_held(&c->lock);
	if (c->handoff_depth || c->fw_pending || c->query_active)
		return -EAGAIN;
	if (map->quarantined)
		return -EIO;
	if (map->startup_pending && map->leased_slot &&
	    !cpumask_test_cpu(map->target_cpu, mask)) {
		unsigned int cpu = cpumask_first_and(mask, cpu_online_mask);
		int err;

		if (map->active_pins != BIT(0) || irq != map->irq[0])
			return -EBUSY;
		err = pp250_route_target(c, cpu);
		return err ? err : cpu;
	}
	if (map->active_pins && map->leased_slot &&
	    !cpumask_test_cpu(map->target_cpu, mask))
		return pp250_active_affinity(map, irq, cpumask_first_and(mask, cpu_online_mask));
	if (map == pp250_route_map(c) && map->phase == PP250_PREPARED) {
		unsigned int cpu;

		if (irq != map->irq[0])
			return -EINVAL;
		if (cpumask_test_cpu(map->target_cpu, mask) &&
		    !(map->leased_slot && c->u2u.ops && c->u2u.ops->assign))
			return -EAGAIN;
		cpu = cpumask_first_and(mask, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			return -EINVAL;
		ret = pp250_route_target(c, cpu);
		if (ret)
			return ret;
	}
	if (map == pp250_route_map(c) && map->phase != PP250_HOME &&
	    map->phase != PP250_ROUTED)
		return -EAGAIN;
	return cpumask_test_cpu(map->target_cpu, mask) ? map->target_cpu : -EINVAL;
}

static int pp250_end_route(struct primepower_controller *c, bool explicit_release)
{
	struct primepower_imap *map = pp250_route_map(c);
	unsigned long masked;
	unsigned int i, slot = map->leased_slot;

	if (map->query_refs || (slot && map->active_pins))
		return -EBUSY;
	if (slot && c->u2u.ops && c->u2u.ops->assign && !explicit_release)
		return -EBUSY;
	if (map->phase == PP250_HOME)
		return slot ? -EIO : 0;
	if (map->phase == PP250_HOLD || map->phase == PP250_ROUTED)
		return -EAGAIN;
	if (map->phase != PP250_PREPARED)
		return -EIO;
	if (!pp250_pins_idle(c))
		return -EAGAIN;
	if ((!(map->saved_route & IMAP_VALID) && !map->startup_pending) ||
	    (map->saved_route & IMAP_TID_UPA))
		goto uncertain;
	if (slot && (slot >= ARRAY_SIZE(c->u2u.slots) ||
		     c->u2u.slots[slot].map != map || !c->u2u.ops || !c->io ||
		     !c->io->write32 || !c->io->read32 ||
		     !c->u2u.ops->release))
		goto uncertain;
	masked = (map->saved_route & ~(IMAP_VALID | IMAP_TID_UPA)) |
		 ((unsigned long)slot << IMAP_TID_SHIFT);
	if (upa_readq(map->addr) != masked)
		goto uncertain;
	if (pp250_enable_route(map, map->saved_route))
		return -EIO;
	if (slot) {
		for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
			unsigned long live;

			if (!primepower_map_known(&c->maps[i]))
				continue;
			live = upa_readq(c->maps[i].addr);
			if (((live & IMAP_TID_UPA) >> IMAP_TID_SHIFT) == slot)
				goto uncertain;
		}
		/* An OS-owned slot has no inherited target to restore. */
		if (!c->u2u.ops->assign &&
		    pp250_set_slot_target(map, slot, c->u2u.slots[slot].previous))
			goto uncertain;
		if (pp250_release_slot(map))
			goto uncertain;
	}
	if ((slot && c->u2u.ops->assign) || c->attach.provider == &pp250_interval_provider)
		pp250_revoke_authority(&c->attach);
	c->u2u.delegated = 0;
	c->u2u.ops = NULL;
	map->quiesced_pins = 0;
	map->target_cpu = primepower_irq_boot_cpu;
	map->last_programmed = map->saved_route;
	map->programmed = true;
	map->phase = PP250_HOME;
	map->startup_pending = false;
	map->auto_owned = false;
	c->restored++;
	pp250_route_done();
	return 0;

uncertain:
	map->phase = PP250_UNCERTAIN;
	map->quarantined = true;
	return -EIO;
}

/* A request which never reached startup must restore its exact inherited valid bit. */
static int pp250_abort_startup(struct primepower_imap *map)
{
	struct primepower_controller *c = map->controller;

	if (map->active_pins || map->query_refs || map->quarantined ||
	    c->query_active || c->handoff_depth ||
	    !map->startup_pending || !pp250_routing_gate() || !pp250_pins_idle(c))
		return -EBUSY;
	map->saved_route = map->startup_original;
	map->phase = PP250_PREPARED;
	return pp250_end_route(c, true);
}

void primepower_route_release(struct primepower_imap *map)
{
	lockdep_assert_held(&map->controller->lock);
	if (map->auto_owned && map->startup_pending && !map->active_pins &&
	    pp250_abort_startup(map)) {
		map->quarantined = true;
		map->phase = PP250_UNCERTAIN;
	}
}

/* Stable assignments survive close/open; only unpublished startup is unwound here. */
static int pp250_request_owned(struct primepower_imap *map, unsigned int irq)
{
	struct primepower_controller *c = map->controller;
	unsigned long original;
	int err;

	lockdep_assert_held(&c->lock);
	if (irq != map->irq[0] || map->irq[1] || map->irq[2] || map->irq[3] ||
	    map->active_pins || map->query_refs || map->quarantined || c->handoff_depth ||
	    c->fw_pending || c->query_active || !c->attach.ready ||
	    c->interval_policy != &pp250_catalog_policy)
		return -EBUSY;
	if (map->leased_slot) {
		unsigned int slot = map->leased_slot;

		if (map->phase != PP250_ROUTED || slot >= ARRAY_SIZE(c->u2u.slots) ||
		    c->u2u.slots[slot].map != map || c->u2u.epoch != c->returns ||
		    map->grant_epoch != c->returns ||
		    c->u2u.authority_generation != c->attach.generation ||
		    !pp250_authorized(&c->attach, c->u2u.ops, slot) ||
		    !pp250_routing_gate() || upa_readq(map->addr) != map->last_programmed)
			return -EIO;
		return 0;
	}
	if (map->phase != PP250_HOME || !pp250_route_start())
		return -EBUSY;
	original = upa_readq(map->addr);
	if ((original & ~IMAP_VALID) != pp250_pci01_source.vector || !pp250_pins_idle(c)) {
		pp250_route_done();
		return -EBUSY;
	}
	map->startup_original = original;
	map->saved_route = original | IMAP_VALID;
	map->startup_pending = true;
	map->auto_owned = true;
	map->phase = PP250_HOLD;
	upa_writeq(original & ~IMAP_VALID, map->addr);
	if (upa_readq(map->addr) != (original & ~IMAP_VALID))
		goto uncertain;
	map->last_programmed = original & ~IMAP_VALID;
	map->programmed = true;
	map->phase = PP250_PREPARED;
	err = pp250_prepare_offer(c);
	if (!err)
		err = pp250_forward_route(c, primepower_irq_boot_cpu);
	if (!err)
		return 0;
	if (!map->quarantined && !pp250_abort_startup(map))
		return err;
uncertain:
	map->quarantined = true;
	map->phase = PP250_UNCERTAIN;
	return -EIO;
}

static int pp250_return_route(struct primepower_controller *c)
{
	return pp250_end_route(c, false);
}

static int pp250_owner_handoff(struct primepower_controller *c)
{
	unsigned int i;

	lockdep_assert_held(&c->lock);
	for (i = 0; i < ARRAY_SIZE(c->u2u.slots); i++)
		if (c->u2u.slots[i].map)
			return -EBUSY;
	pp250_revoke_authority(&c->attach);
	c->u2u.delegated = 0;
	c->u2u.ops = NULL;
	return 0;
}


/* No other CPU may enter CIF or acquire a route until this owner finishes. */
static void pp250_fw_drop_token(void)
{
	WRITE_ONCE(pp250_fw_owner, -1);
	atomic_fetch_andnot_release(PP250_GATE_HANDOFF, &pp250_gate);
}

static int pp250_fw_failed(struct primepower_controller *c, int err)
{
	pp250_route_map(c)->quarantined = true;
	pp250_route_map(c)->phase = PP250_UNCERTAIN;
	c->fw_pending = false;
	c->fw_result = err;
	atomic_fetch_or(PP250_GATE_ROUTING, &pp250_gate);
	pp250_fw_drop_token();
	return err;
}

/* IRQ resource release follows free_irq() synchronization; do not wait here. */
static int pp250_mask_closed(struct primepower_controller *c)
{
	struct primepower_imap *map = pp250_route_map(c);
	unsigned long route = map->last_programmed;

	if (map->active_pins || map->query_refs || !pp250_pins_idle(c))
		return -EBUSY;
	if (!(route & IMAP_VALID) || upa_readq(map->addr) != route)
		return -EUCLEAN;
	upa_writeq(route & ~IMAP_VALID, map->addr);
	if (upa_readq(map->addr) != (route & ~IMAP_VALID) || !pp250_pins_idle(c))
		return -EIO;
	map->phase = PP250_PREPARED;
	return 0;
}

static int pp250_auto_handback(struct primepower_controller *c)
{
	struct primepower_imap *map = pp250_route_map(c);
	int err;

	if (c->fw_pending)
		return -EBUSY;
	if (!map->leased_slot)
		return 0;
	if (c->interval_policy != &pp250_catalog_policy ||
	    c->u2u.ops != &pp250_interval_provider || c->handoff_depth ||
	    c->fw_pending || map->phase != PP250_ROUTED || map->quarantined ||
	    map->startup_pending || map->active_pins || map->query_refs || !pp250_pins_idle(c))
		return -EBUSY;
	if (atomic_cmpxchg(&pp250_gate, PP250_GATE_ROUTING,
			   PP250_GATE_ROUTING | PP250_GATE_HANDOFF) != PP250_GATE_ROUTING)
		return -EBUSY;
	WRITE_ONCE(pp250_fw_owner, raw_smp_processor_id());
	c->fw_target = map->target_cpu;
	c->fw_pending = true;
	err = pp250_mask_closed(c);
	if (!err)
		err = pp250_end_route(c, true);
	if (err)
		return pp250_fw_failed(c, err);
	c->fw_handbacks++;
	c->fw_result = 0;
	return 0;
}

/* Firmware owns the interval just completed; obtain new authority before writing. */
static void pp250_auto_restore(struct primepower_controller *c)
{
	struct primepower_imap *map = pp250_route_map(c);
	int err = -EBUSY;

	if (!c->fw_pending)
		return;
	if (READ_ONCE(pp250_fw_owner) != raw_smp_processor_id()) {
		c->fw_result = -EACCES;
		return;
	}
	if (c->handoff_depth || map->phase != PP250_HOME || map->active_pins ||
	    map->leased_slot || map->quarantined || c->attach.provider ||
	    c->u2u.delegated || atomic_cmpxchg(&pp250_gate, PP250_GATE_HANDOFF,
			PP250_GATE_HANDOFF | PP250_GATE_ROUTING) != PP250_GATE_HANDOFF)
		goto failed;
	err = pp250_mask_closed(c);
	if (!err)
		err = pp250_prepare_offer(c);
	if (!err)
		err = pp250_route_target(c, c->fw_target);
	if (err)
		goto failed;
	c->fw_pending = false;
	c->fw_restores++;
	c->fw_result = 0;
	pp250_fw_drop_token();
	return;
failed:
	pp250_fw_failed(c, err);
}


static unsigned long pp250_query_tba(void)
{
	unsigned long tba;

	asm volatile("rdpr %%tba, %0" : "=r" (tba));
	return tba;
}

static bool pp250_query_authority(struct primepower_controller *c)
{
	struct primepower_imap *map = pp250_route_map(c);

	return map->phase == PP250_ROUTED && !map->quarantined &&
	       map->programmed && !map->startup_pending &&
	       !c->fw_pending && !c->handoff_depth && map->leased_slot == pp250_pci01_source.slot &&
	       !(map->active_pins & ~BIT(0)) && map->irq[0] &&
	       !map->irq[1] && !map->irq[2] && !map->irq[3] &&
	       map->addr == pp250_pci01_source.imap &&
	       map->last_programmed == (IMAP_VALID | ((unsigned long)pp250_pci01_source.slot <<
			IMAP_TID_SHIFT) | pp250_pci01_source.vector) &&
	       c->interval_policy == &pp250_catalog_policy &&
	       c->u2u.ops == &pp250_interval_provider &&
	       c->u2u.slots[pp250_pci01_source.slot].map == map &&
	       c->u2u.epoch == c->returns && map->grant_epoch == c->returns &&
	       c->u2u.authority_generation == c->attach.generation &&
	       pp250_authorized(&c->attach, c->u2u.ops, 12);
}

/* Admit only the qualified service; the reference, not a caller lock, pins its route. */
int primepower_cif_query_begin(unsigned long *args, unsigned long flags)
{
	struct primepower_controller *c;
	struct primepower_imap *map;
	int ret = -EBUSY;

	if (!primepower_maps_ready ||
	    !list_is_singular(&primepower_controllers) ||
	    !(atomic_read_acquire(&pp250_gate) & PP250_GATE_ROUTING) ||
	    (args[1] != 2 || args[2] != 1 ||
	     strcmp((char *)args[0], "getproplen") ||
	     args[3] != (unsigned int)prom_root_node ||
	     strcmp((char *)args[4], "name")))
		return 0;
	c = list_first_entry(&primepower_controllers, struct primepower_controller, list);
	if (!raw_spin_trylock(&c->lock))
		return -EBUSY;
	map = pp250_route_map(c);
	if (!map->leased_slot || !map->active_pins) {
		ret = 0;
		goto out;
	}
	if (flags || preempt_count()) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (pp250_query_tba() != (unsigned long)sparc64_ttable_tl0) {
		ret = -EIO;
		goto out;
	}
	if (c->query_active || map->query_refs || map->active_pins != BIT(0) ||
	    !pp250_query_authority(c) ||
	    upa_readq(map->addr) != map->last_programmed)
		goto out;
	if (atomic_cmpxchg(&pp250_gate, PP250_GATE_ROUTING,
			   PP250_GATE_ROUTING | PP250_GATE_HANDOFF) != PP250_GATE_ROUTING)
		goto out;
	map->query_refs++;
	c->query_map = map;
	c->query_cpu = raw_smp_processor_id();
	c->query_target = map->target_cpu;
	c->query_epoch = c->u2u.epoch;
	c->query_generation = c->attach.generation;
	c->query_route = map->last_programmed;
	c->query_active = true;
	c->query_entries++;
	c->query_result = 0;
	ret = PP250_CIF_RETAINED;
out:
	raw_spin_unlock(&c->lock);
	return ret;
}

/* Retention never releases or rewrites a destination, even when return validation fails. */
static int pp250_query_end(bool executed)
{
	struct primepower_controller *c;
	struct primepower_imap *map;
	int ret = -EIO;

	c = list_first_entry(&primepower_controllers, struct primepower_controller, list);
	raw_spin_lock(&c->lock);
	map = pp250_route_map(c);
	if (!c->query_active || c->query_map != map || map->query_refs != 1 ||
	    c->query_cpu != raw_smp_processor_id())
		goto failed;
	if (!pp250_query_authority(c) || c->query_epoch != c->u2u.epoch ||
	    c->query_generation != c->attach.generation ||
	    c->query_target != map->target_cpu || c->query_route != map->last_programmed ||
	    upa_readq(map->addr) != c->query_route ||
	    pp250_query_tba() != (unsigned long)sparc64_ttable_tl0)
		goto failed;
	if (atomic_cmpxchg(&pp250_gate, PP250_GATE_ROUTING | PP250_GATE_HANDOFF,
			   PP250_GATE_ROUTING) != (PP250_GATE_ROUTING | PP250_GATE_HANDOFF))
		goto failed;
	if (executed && !map->active_pins)
		c->query_closed++;
	map->query_refs--;
	c->query_map = NULL;
	c->query_active = false;
	if (executed)
		c->query_returns++;
	else
		c->query_canceled++;
	c->query_result = 0;
	ret = 0;
	goto out;
failed:
	map->quarantined = true;
	pp250_route_map(c)->phase = PP250_UNCERTAIN;
	c->query_result = ret;
	/* Keep the exclusion token and assignment; an unknown route is not safe to rewrite. */
out:
	raw_spin_unlock(&c->lock);
	return ret;
}

enum pp250_cif_admission {
	PP250_CIF_READER = 1,
	PP250_CIF_HANDBACK,
};

/* Called at PIL_NMI, before prom_entry_lock; never wait on an interrupted owner. */
int primepower_cif_begin(void)
{
	struct primepower_controller *c;
	struct primepower_imap *map;
	int ret = primepower_prom_begin();

	if (ret != -EBUSY ||
	    !list_is_singular(&primepower_controllers))
		return ret;
	c = list_first_entry(&primepower_controllers, struct primepower_controller, list);
	if (!raw_spin_trylock(&c->lock))
		return -EBUSY;
	map = pp250_route_map(c);
	if (!map->leased_slot || c->fw_pending || c->query_active || c->handoff_depth)
		goto out;
	ret = pp250_auto_handback(c);
	if (ret)
		goto out;
	ret = pp250_owner_handoff(c);
	if (ret)
		goto failed;
	map->before_prom = upa_readq(map->addr);
	ret = primepower_prom_begin();
	if (ret < 0)
		goto failed;
	c->cif_entries++;
	c->entries++;
	ret = PP250_CIF_HANDBACK;
	goto out;
failed:
	pp250_fw_failed(c, ret);
out:
	raw_spin_unlock(&c->lock);
	return ret;
}

/* A failed PROM trylock cancels admission before the service has run. */
int primepower_cif_query_cancel(void)
{
	return pp250_query_end(false);
}

/* HANDOFF excludes new routes and CIF callers until fresh authority is installed. */
int primepower_cif_end(int admission)
{
	struct primepower_controller *c;
	struct primepower_imap *map;
	int ret;

	if (admission == PP250_CIF_RETAINED)
		return pp250_query_end(true);
	primepower_prom_end();
	if (admission == PP250_CIF_READER)
		return 0;
	c = list_first_entry(&primepower_controllers, struct primepower_controller, list);
	raw_spin_lock(&c->lock);
	map = pp250_route_map(c);
	map->after_prom = upa_readq(map->addr);
	if (primepower_map_conflict(map))
		map->quarantined = true;
	c->cif_returns++;
	c->returns++;
	pp250_auto_restore(c);
	ret = c->fw_result;
	raw_spin_unlock(&c->lock);
	return ret;
}

int primepower_irq_handoff_begin(void)
{
	struct primepower_controller *c;
	unsigned long flags;
	unsigned int i;

	int err;

	if (!primepower_maps_ready)
		return 0;
	list_for_each_entry(c, &primepower_controllers, list) {
		raw_spin_lock_irqsave(&c->lock, flags);
		err = pp250_auto_handback(c);
		if (!err)
			err = pp250_return_route(c);
		if (!err)
			err = pp250_owner_handoff(c);
		if (err) {
			c->refused++;
			raw_spin_unlock_irqrestore(&c->lock, flags);
			list_for_each_entry_continue_reverse(c, &primepower_controllers, list) {
				raw_spin_lock_irqsave(&c->lock, flags);
				c->handoff_depth--;
				raw_spin_unlock_irqrestore(&c->lock, flags);
			}
			return err;
		}
		if (!c->handoff_depth++)
			for (i = 0; i < ARRAY_SIZE(c->maps); i++)
				if (primepower_map_known(&c->maps[i]))
					c->maps[i].before_prom = primepower_observe_imap(c,
							upa_readq(c->maps[i].addr));
		c->entries++;
		raw_spin_unlock_irqrestore(&c->lock, flags);

	}
	return 0;
}

void primepower_irq_handoff_end(void)
{
	struct primepower_controller *c;
	unsigned long flags;
	unsigned int i;

	if (!primepower_maps_ready)
		return;
	list_for_each_entry(c, &primepower_controllers, list) {
		raw_spin_lock_irqsave(&c->lock, flags);
		if (WARN_ON_ONCE(!c->handoff_depth) || --c->handoff_depth)
			goto unlock;
		for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
			struct primepower_imap *map = &c->maps[i];

			if (!primepower_map_known(map))
				continue;
			map->after_prom = primepower_observe_imap(c, upa_readq(map->addr));
			/* Preserve firmware changes until ownership can be re-established. */
			if (!map->quarantined && primepower_map_conflict(map)) {
				map->quarantined = true;
				printk_deferred(KERN_WARNING "PRIMEPOWER: IMAP %lx quarantined across PROM (%lx -> %lx, programmed %lx)\n",
						map->addr, map->before_prom, map->after_prom,
						map->last_programmed);
			}
		}
unlock:
		c->returns++;
		if (!c->handoff_depth)
			pp250_auto_restore(c);
		raw_spin_unlock_irqrestore(&c->lock, flags);
	}
}

static int pp250_check_source(struct primepower_controller *c, struct pci_dev *pdev)
{
	struct device_node *node = pci_device_to_OF_node(pdev);
	u8 header, pin;
	unsigned int i;

	if (!node || !pp250_source_pins(c))
		return -ENODATA;
	for (i = 0; i < ARRAY_SIZE(c->pbms); i++)
		if (c->pci01_pins[i] && node->parent == c->pbms[i])
			break;
	if (i == ARRAY_SIZE(c->pbms))
		return -ENODEV;
	if (pci_read_config_byte(pdev, PCI_HEADER_TYPE, &header) ||
	    pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin))
		return -EIO;
	if (header != PCI_HEADER_TYPE_NORMAL || pin != 1)
		return -EOPNOTSUPP;
	return 0;
}

/* Validate the registered endpoint rather than a Linux IRQ number or interface name. */
int primepower_route_request(struct primepower_imap *map, unsigned int irq)
{
	struct primepower_controller *c = map->controller;
	struct pci_dev *pdev = NULL, *other;
	unsigned long flags;
	unsigned int i;
	int err = -ENODEV;

	if (map != pp250_route_map(c))
		return 0;
	/* Keep early boot and a retained PROM console on the firmware-compatible CPU0 route. */
	if (!prom_console_retired() || system_state < SYSTEM_RUNNING)
		return 0;
	if (IS_ENABLED(CONFIG_HOTPLUG_CPU) || IS_ENABLED(CONFIG_PM_SLEEP) ||
	    map->addr != pp250_pci01_source.imap)
		return -EOPNOTSUPP;
	for_each_pci_dev(pdev) {
		if (pdev->irq == irq && pdev->devfn == pp250_pci01_source.devfn &&
		    !pp250_check_source(c, pdev))
			break;
	}
	if (!pdev)
		return err;
	for (i = 1; i < 8; i++) {
		other = pci_get_slot(pdev->bus, pp250_pci01_source.devfn | i);
		if (other) {
			pci_dev_put(other);
			err = -EOPNOTSUPP;
			goto out;
		}
	}
	raw_spin_lock_irqsave(&c->lock, flags);
	err = pp250_request_owned(map, irq);
	raw_spin_unlock_irqrestore(&c->lock, flags);
out:
	pci_dev_put(pdev);
	return err;
}

static bool pp250_platform_layout(struct primepower_controller *c)
{
	u32 proxies;
	int i;

	if (!c->attach.ready || !c->inventory_complete ||
	    c->base != 0x10600000000UL || c->base_size != 0xd000 ||
	    c->window != 0x10600800000UL || c->window_size != 0x90 ||
	    !c->maps[primepower_map_index(PP250_TIMER0_INO)].timer_source ||
	    !c->maps[primepower_map_index(PP250_TIMER1_INO)].timer_source)
		return false;
	for (i = 0; i < ARRAY_SIZE(c->pbms); i++) {
		u32 version, impl;

		if (!c->pbms[i] || !of_device_is_available(c->pbms[i]) ||
		    !of_device_is_compatible(c->pbms[i], "pci108e,8000") ||
		    of_property_read_u32(c->pbms[i], "version#", &version) || version != 4 ||
		    of_property_read_u32(c->pbms[i], "implementation#", &impl) || impl)
			return false;
	}
	for (i = 0; i < ARRAY_SIZE(c->pbms); i++)
		if (!of_property_read_u32(c->pbms[i], "#upa-interrupt-proxies", &proxies) &&
		    proxies == 2)
			return true;
	return false;
}

/* Every grant, destination write and release uses fresh reads under the CIF gate. */
static int pp250_catalog_scan(struct primepower_controller *c, unsigned long source,
			      bool active)
{
	unsigned long allowed = IMAP_VALID | IMAP_TID_UPA | IMAP_IGN | IMAP_INO;
	unsigned int i;
	u32 status;
	int err = -EBUSY;

	lockdep_assert_held(&c->lock);
	if (!c->attach.ready || !c->inventory_complete || c->handoff_depth ||
	    (pp250_route_map(c)->active_pins && !active &&
	     !pp250_route_map(c)->startup_pending) || !pp250_routing_gate())
		goto done;
	err = pp250_check_controller(c, &status);
	if (err)
		goto done;
	for (i = 0; i < ARRAY_SIZE(c->maps); i++) {
		struct primepower_imap *map = &c->maps[i];
		unsigned int ino = i < 8 ? i * 4 : 0x20 + i - 8;
		unsigned long vector = ((c->attach.portid << 6) & IMAP_IGN) | ino;
		unsigned int selector;
		u64 value;

		if (map->quarantined) {
			err = -EBUSY;
			goto done;
		}
		if (!pp250_catalog_index(i)) {
			if (map->observed || map->described[0] || map->described[1]) {
				err = -ENODATA;
				goto done;
			}
			continue;
		}
		err = pp250_phys_read64(map->addr, &value);
		if (err)
			goto done;
		if ((value & ~allowed) || (value & (IMAP_IGN | IMAP_INO)) != vector) {
			err = -EUCLEAN;
			goto done;
		}
		selector = (value & IMAP_TID_UPA) >> IMAP_TID_SHIFT;
		if (map == pp250_route_map(c)) {
			if (value != source) {
				err = -EUCLEAN;
				goto done;
			}
		} else {
			c->reserved_selectors |= BIT(selector);
			if (selector == pp250_pci01_source.slot) {
				err = -EBUSY;
				goto done;
			}
		}
	}
	/* Proxy target fields are direct CPU IDs, not U2U selectors. */
	for (i = 0; i < 2; i++) {
		u64 value;

		err = pp250_phys_read64(c->base + 0x6000 + i * 0x2000, &value);
		if (err)
			goto done;
		if (value & IMAP_VALID) {
			err = -EBUSY;
			goto done;
		}
	}
	err = pp250_pins_idle(c) ? 0 : active ? -EAGAIN : -EBUSY;
done:
	return err;
}

static int pp250_catalog_check(struct primepower_controller *c, unsigned long source)
{
	return pp250_catalog_scan(c, source, false);
}

static int pp250_catalog_check_active(struct primepower_imap *map)
{
	if (!pp250_active_assignment(map))
		return -EBUSY;
	return pp250_catalog_scan(map->controller, map->last_programmed & ~IMAP_VALID, true);
}

static u16 pp250_catalog_eligible(struct primepower_controller *c)
{
	if (pp250_catalog_check(c, pp250_route_map(c)->saved_route & ~IMAP_VALID))
		return 0;
	return BIT(pp250_pci01_source.slot) & ~c->reserved_selectors;
}

static int pp250_catalog_handback(struct primepower_imap *map, unsigned int slot)
{
	struct primepower_controller *c = map->controller;
	u64 source;
	int err;

	if (slot != pp250_pci01_source.slot ||
	    (map->saved_route & ~IMAP_VALID) != pp250_pci01_source.vector ||
	    (!(map->saved_route & IMAP_VALID) && !map->startup_pending))
		return -EINVAL;
	err = pp250_phys_read64(map->addr, &source);
	if (err)
		return err;
	if (source != map->saved_route && source != (map->saved_route & ~IMAP_VALID))
		return -EBUSY;
	return pp250_catalog_check(c, source);
}

static const struct pp250_interval_policy pp250_catalog_policy = {
	.eligible = pp250_catalog_eligible,
	.handback = pp250_catalog_handback,
};

static int __init primepower_route_init(void)
{
	struct primepower_controller *c;

	if (!list_is_singular(&primepower_controllers))
		return 0;
	c = list_first_entry(&primepower_controllers, struct primepower_controller, list);
	if (pp250_platform_layout(c) && c->attach.portid == 0x83 &&
	    primepower_irq_boot_cpu == 0 && !IS_ENABLED(CONFIG_HOTPLUG_CPU) &&
	    !IS_ENABLED(CONFIG_PM_SLEEP))
		c->interval_policy = &pp250_catalog_policy;
	return 0;
}
late_initcall(primepower_route_init);

unsigned int primepower_irq_build(struct device_node *dp, unsigned int ino,
					 void *data)
{
	struct primepower_pbm *pbm = data;
	struct primepower_controller *controller = pbm->controller;
	unsigned int index;

	if (!pp250_catalog_vector(controller, ino)) {
		controller->attach.invalid = true;
		pp250_finish_inventory(&controller->attach, false);
		return 0;
	}
	ino &= 0x3f;
	if (ino == PP250_TIMER0_INO || ino == PP250_TIMER1_INO)
		return 0;
	index = primepower_map_index(ino);
	return primepower_build_irq(&controller->maps[index], pbm->index,
				    ino < 0x20 ? ino & 3 : 0,
				    controller->base + psycho_iclr_offset(ino));
}
