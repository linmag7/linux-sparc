/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PCI_OBERON_MSI_H
#define PCI_OBERON_MSI_H

#include <linux/errno.h>
#include <linux/types.h>

#define OBERON_EQ_ALIGNMENT (512U * 1024)
#define OBERON_EQ_BYTES (128U * 64)

struct oberon_msi_geometry {
	u32 queues;
	u32 entries;
	u32 queue_range[3];
	u32 messages;
	u32 message_range[2];
	u32 data_mask;
	u32 msix_width;
	u32 addresses[6];
	u32 ino_bitmap[2];
};

static inline int oberon_msi_validate(const struct oberon_msi_geometry *g)
{
	u64 bitmap = ((u64)g->ino_bitmap[1] << 32) | g->ino_bitmap[0];
	u32 first = g->queue_range[2];
	u32 i;

	/* Initially support the zero-based queue layout observed on M3000. */
	if (!g->queues || g->queues > 36 || g->entries != 128 ||
	    g->queue_range[0] || g->queue_range[1] != g->queues ||
	    first < 0x18 || first > 0x3b || g->queues > 0x3c - first)
		return -EINVAL;
	if (!g->messages || g->messages > 256 ||
	    g->message_range[0] > 255 ||
	    g->messages > 256 - g->message_range[0] ||
	    g->message_range[1] != g->messages ||
	    g->data_mask != 0xff || g->msix_width < 8 || g->msix_width > 32)
		return -EINVAL;
	if (g->addresses[0] || (g->addresses[1] & 0xffff) ||
	    (g->addresses[4] & 0xffff) ||
	    g->addresses[2] != 0x10000 || g->addresses[5] != 0x10000)
		return -EINVAL;
	for (i = 0; i < g->queues; i++)
		if (!(bitmap & (1ULL << (first + i))))
			return -EINVAL;
	return 0;
}

#endif /* PCI_OBERON_MSI_H */
