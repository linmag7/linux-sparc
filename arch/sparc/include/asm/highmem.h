/* SPDX-License-Identifier: GPL-2.0 */
/*
 * highmem.h: virtual kernel memory mappings for high memory
 *
 * Used in CONFIG_HIGHMEM systems for memory pages which
 * are not addressable by direct kernel virtual addresses.
 *
 * Copyright (C) 1999 Gerhard Wichert, Siemens AG
 *		      Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with 
 * up to 16 Terrabyte physical memory. With current x86 CPUs
 * we now support up to 64 Gigabytes physical RAM.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#ifndef _ASM_HIGHMEM_H
#define _ASM_HIGHMEM_H

#ifdef __KERNEL__

#include <linux/interrupt.h>
#include <linux/pgtable.h>
#include <asm/vaddrs.h>
#include <asm/pgtsrmmu.h>

/* Reconcile a pinned persistent mapping after kmap_lock is released. */
#define ARCH_HAS_KMAP_FLUSH_TLB
void kmap_flush_tlb(unsigned long addr);

/* Pin PKMAP aliases, but use fixmap slots so the post-map hook runs. */
#define ARCH_NEEDS_KMAP_HIGH_GET
void *kmap_high_get(const struct page *page);

static inline void *arch_kmap_local_high_get(const struct page *page)
{
	return NULL;
}

#define arch_kmap_local_high_get arch_kmap_local_high_get

void sparc32_kmap_local_cache_flush(unsigned long vaddr);
void sparc32_kmap_local_cache_flush_unmap(unsigned long vaddr);
void sparc32_kunmap_local_copy_source(const void *vaddr);
void sparc32_kmap_local_tlb_flush(unsigned long vaddr);
void sparc32_kmap_local_post_map(unsigned long vaddr, pte_t pteval);

/*
 * Kernel kmap mappings do not need hardware dirty/accessed tracking.
 * Match PAGE_KERNEL and avoid M/R updates during translated accesses.
 */
#define kmap_prot __pgprot(SRMMU_ET_PTE | SRMMU_PRIV | SRMMU_CACHE | \
			   SRMMU_DIRTY | SRMMU_REF)
extern pte_t *pkmap_page_table;

/*
 * Right now we initialize only a single pte table. It can be extended
 * easily, subsequent pte tables have to be allocated in one physical
 * chunk of RAM.  Currently the simplest way to do this is to align the
 * pkmap region on a pagetable boundary (4MB).
 */
#define LAST_PKMAP 1024
#define PKMAP_SIZE (LAST_PKMAP << PAGE_SHIFT)
#define PKMAP_BASE PMD_ALIGN(SRMMU_NOCACHE_VADDR + (SRMMU_MAX_NOCACHE_PAGES << PAGE_SHIFT))

#define LAST_PKMAP_MASK (LAST_PKMAP - 1)
#define PKMAP_NR(virt)  ((virt - PKMAP_BASE) >> PAGE_SHIFT)
#define PKMAP_ADDR(nr)  (PKMAP_BASE + ((nr) << PAGE_SHIFT))

#define PKMAP_END (PKMAP_ADDR(LAST_PKMAP))

#define flush_cache_kmaps()	flush_cache_all()

/* FIXME: Use __flush_*_one(vaddr) instead of flush_*_all() -- Anton */
#define arch_kmap_local_pre_unmap(vaddr) \
	sparc32_kmap_local_cache_flush_unmap(vaddr)
#define arch_kmap_local_post_map(vaddr, pteval) \
	sparc32_kmap_local_post_map(vaddr, pteval)
#define arch_kmap_local_post_unmap(vaddr) \
	sparc32_kmap_local_tlb_flush(vaddr)

#endif /* __KERNEL__ */

#endif /* _ASM_HIGHMEM_H */
