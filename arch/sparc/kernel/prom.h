/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PROM_H
#define __PROM_H

#include <linux/cpumask.h>
#include <linux/spinlock.h>
#include <asm/prom.h>

void of_console_init(void);

extern unsigned int prom_early_allocated;

#include "pp250_attach.h"

struct primepower_controller;
struct primepower_pbm;

enum pp250_containment {
	PP250_CONTAIN_NONE, PP250_CONTAIN_MAP_CHANGED,
	PP250_CONTAIN_MASK_UNCONFIRMED, PP250_CONTAIN_MASK_CONFIRMED,
};

enum pp250_return_phase {
	PP250_HOME, PP250_HOLD, PP250_PREPARED, PP250_UNCERTAIN, PP250_ROUTED,
};

/* This descriptor qualifies only the previously tested PCI#01 source. */
struct pp250_route_source {
	unsigned long imap, pending_offset, pending_mask;
	unsigned int map_index, devfn, vector, slot;
};

static const struct pp250_route_source pp250_pci01_source = {
	.imap = 0x10600000c20UL,
	.pending_offset = 0xa800,
	.pending_mask = 0xffUL << 32,
	.map_index = 4,
	.devfn = 3 << 3,
	.vector = 0xd0,
	.slot = 12,
};

struct primepower_imap {
	struct primepower_controller *controller;
	unsigned long addr, saved_route, startup_original;
	enum pp250_return_phase phase;
	unsigned long first, last_programmed;
	unsigned long before_prom, after_prom;
	unsigned long containment_readback;
	enum pp250_containment containment;
	unsigned int irq[4];
	struct cpumask *requested[4];
	unsigned int target_cpu, leased_slot, grant_epoch;
	unsigned int query_refs;
	u8 pins[2], described[2], quiesced_pins;
	u8 bridge_pbms, active_pins, enabled_pins, masked_pins;
	bool scf_source, scf_alias, timer_source;
	bool observed, programmed, quarantined;
	bool startup_pending, auto_owned;
};

struct pp250_u2u_slot {
	struct primepower_imap *map;
	u32 previous;
	u32 mode_status;
};

/* Built-in providers run under the controller lock and must not sleep or enter PROM. */
struct pp250_u2u_ops {
	/* Acquire excludes other users and returns the old target. Failure leaves no claim. */
	int (*acquire)(struct primepower_imap *map, unsigned int slot, u32 *target);
	/* An OS-lifetime provider assigns an exclusive slot without a restore target. */
	int (*assign)(struct primepower_imap *map, unsigned int slot);
	/* Release must establish safe handback or keep the slot exclusively held. */
	int (*release)(struct primepower_imap *map, unsigned int slot);
};

/* Both callbacks run under the controller lock with firmware entry excluded. */
struct pp250_interval_policy {
	/* Report independently authorized slots for this interval without claiming them. */
	u16 (*eligible)(struct primepower_controller *c);
	/* Confirm safe handback or fail with the slot still owned. */
	int (*handback)(struct primepower_imap *map, unsigned int slot);
};

struct pp250_u2u_io {
	/* Ordered 32-bit accesses must report bus faults to the caller. */
	int (*write32)(unsigned long addr, u32 value);
	int (*read32)(unsigned long addr, u32 *value);
};

int pp250_phys_write32(unsigned long addr, u32 value);
int pp250_phys_read32(unsigned long addr, u32 *value);
int pp250_phys_read64(unsigned long addr, u64 *value);

struct pp250_u2u_owner {
	u16 delegated;
	u64 authority_generation;
	unsigned int epoch;
	struct pp250_u2u_slot slots[16];
	const struct pp250_u2u_ops *ops;
};

struct primepower_controller {
	struct pp250_attach attach;
	const struct pp250_interval_policy *interval_policy;
	const struct pp250_u2u_io *io;
	bool fw_pending;
	bool query_active;
	struct primepower_imap *query_map;
	unsigned int query_cpu, query_entries, query_returns, query_closed;
	unsigned int query_canceled;
	unsigned int query_target, query_epoch;
	u64 query_generation;
	unsigned long query_route;
	int query_result;
	unsigned int fw_target, fw_handbacks, fw_restores;
	unsigned int cif_entries, cif_returns;
	int fw_result;
	struct list_head list;
	unsigned long window, base, window_size, base_size;
	raw_spinlock_t lock;
	unsigned int handoff_depth;
	u32 reserved_selectors;
	bool inventory_complete;
	bool pci01_alias;
	u8 pci01_pins[2];
	unsigned int pci01_rows[2];
	struct pp250_u2u_owner u2u;
	unsigned int refused, restored;
	unsigned int entries, returns;
	struct device_node *pbms[2];
	struct primepower_imap maps[8 + 32];
};

static inline struct primepower_imap *pp250_route_map(struct primepower_controller *c)
{
	return &c->maps[pp250_pci01_source.map_index];
}

int primepower_grant_pci01_slots(struct primepower_controller *c, u64 generation,
				u16 eligible);
int primepower_offer_pci01_slot(struct primepower_controller *c,
			       unsigned int slot, unsigned int generation,
			       u8 quiesced_pins, const struct pp250_u2u_ops *ops);

unsigned long primepower_observe_imap(struct primepower_controller *c,
				      unsigned long value);

#ifdef CONFIG_PCI
int primepower_route_request(struct primepower_imap *map, unsigned int irq);
void primepower_route_release(struct primepower_imap *map);
int primepower_route_startup(struct primepower_imap *map);
int primepower_affinity_target(struct primepower_imap *map, unsigned int irq,
			      const struct cpumask *mask);
#else
static inline int primepower_route_request(struct primepower_imap *map, unsigned int irq)
{
	return 0;
}

static inline void primepower_route_release(struct primepower_imap *map) { }

static inline int primepower_route_startup(struct primepower_imap *map)
{
	return -EOPNOTSUPP;
}

static inline int primepower_affinity_target(struct primepower_imap *map,
					     unsigned int irq, const struct cpumask *mask)
{
	return -EINVAL;
}
#endif

struct primepower_pbm *primepower_attach_pbm(struct device_node *dp,
				 const struct linux_prom64_registers *regs);
unsigned int primepower_irq_build(struct device_node *dp, unsigned int ino, void *data);

unsigned int primepower_build_irq(struct primepower_imap *map, unsigned int pbm,
				  int pin, unsigned long iclr);

#endif /* __PROM_H */
