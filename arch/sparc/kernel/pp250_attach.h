/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PP250_ATTACH_H
#define _PP250_ATTACH_H

struct pp250_u2u_ops;

/* The PBM letters follow their control-bank offsets, independent of probe order. */
struct pp250_attach {
	u32 node[2], portid, u2u_id;
	bool invalid, ready;
	u8 suspended;
	u64 generation;
	const struct pp250_u2u_ops *provider;
	unsigned int slot;
};

static inline void pp250_revoke_authority(struct pp250_attach *a)
{
	a->provider = NULL;
	a->slot = 0;
	a->generation++;
}

static inline bool pp250_ranges_overlap(unsigned long a, unsigned long an,
					unsigned long b, unsigned long bn)
{
	return a <= b ? b - a < an : a - b < bn;
}

static inline void pp250_record_pbm(struct pp250_attach *a, unsigned long offset,
				    u32 node, u32 portid, u32 u2u_id, bool valid)
{
	unsigned int bank = offset == 0x4000;

	if (!valid || !node || (offset != 0x2000 && offset != 0x4000) ||
	    (a->node[bank] && a->node[bank] != node) ||
	    a->node[bank ^ 1] == node ||
	    ((a->node[0] || a->node[1]) &&
	     (a->portid != portid || a->u2u_id != u2u_id))) {
		a->invalid = true;
		a->ready = false;
		pp250_revoke_authority(a);
		return;
	}
	if (a->node[bank] == node)
		return;
	a->node[bank] = node;
	a->portid = portid;
	a->u2u_id = u2u_id;
	a->ready = false;
	pp250_revoke_authority(a);
}

static inline void pp250_finish_inventory(struct pp250_attach *a, bool complete)
{
	a->ready = complete && !a->invalid && !a->suspended && a->node[0] && a->node[1];
	if (!a->ready)
		pp250_revoke_authority(a);
}

/* A lifecycle transition may revoke an idle loan, never forget a held slot. */
static inline bool pp250_offline_pbm(struct pp250_attach *a, unsigned int bank,
				     bool detach, bool held)
{
	if (bank >= 2 || !a->node[bank] || held)
		return false;
	a->ready = false;
	pp250_revoke_authority(a);
	if (detach) {
		a->node[bank] = 0;
		a->suspended &= ~BIT(bank);
	} else {
		a->suspended |= BIT(bank);
	}
	return true;
}

static inline bool pp250_resume_pbm(struct pp250_attach *a, unsigned int bank)
{
	if (bank >= 2 || !(a->suspended & BIT(bank)))
		return false;
	a->suspended &= ~BIT(bank);
	return true;
}

/* A trusted platform provider must establish authority outside this state machine. */
static inline bool pp250_publish_authority(struct pp250_attach *a, u64 generation,
				     const struct pp250_u2u_ops *ops,
				     unsigned int slot, u32 reserved)
{
	if (!a->ready || a->provider || generation != a->generation || !ops ||
	    !slot || slot >= 16 || (reserved & BIT(slot)))
		return false;
	a->slot = slot;
	a->provider = ops;
	return true;
}

static inline bool pp250_authorized(const struct pp250_attach *a,
				    const struct pp250_u2u_ops *ops,
				    unsigned int slot)
{
	return a->ready && ops && ops == a->provider && slot > 0 &&
	       slot < 16 && slot == a->slot;
}
#endif
