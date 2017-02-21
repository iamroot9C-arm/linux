/*
 *  linux/arch/arm/mm/fault-armv.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *  Modifications for ARM processor (c) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bitops.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/gfp.h>

#include <asm/bugs.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "mm.h"

static pteval_t shared_pte_mask = L_PTE_MT_BUFFERABLE;

#if __LINUX_ARM_ARCH__ < 6
/*
 * We take the easy way out of this problem - we make the
 * PTE uncacheable.  However, we leave the write buffer on.
 *
 * Note that the pte lock held when calling update_mmu_cache must also
 * guard the pte (somewhere else in the same mm) that we modify here.
 * Therefore those configurations which might call adjust_pte (those
 * without CONFIG_CPU_CACHE_VIPT) cannot support split page_table_lock.
 */
static int do_adjust_pte(struct vm_area_struct *vma, unsigned long address,
	unsigned long pfn, pte_t *ptep)
{
	pte_t entry = *ptep;
	int ret;

	/*
	 * If this page is present, it's actually being shared.
	 */
	ret = pte_present(entry);

	/*
	 * If this page isn't present, or is already setup to
	 * fault (ie, is old), we can safely ignore any issues.
	 */
	if (ret && (pte_val(entry) & L_PTE_MT_MASK) != shared_pte_mask) {
		flush_cache_page(vma, address, pfn);
		outer_flush_range((pfn << PAGE_SHIFT),
				  (pfn << PAGE_SHIFT) + PAGE_SIZE);
		pte_val(entry) &= ~L_PTE_MT_MASK;
		pte_val(entry) |= shared_pte_mask;
		set_pte_at(vma->vm_mm, address, ptep, entry);
		flush_tlb_page(vma, address);
	}

	return ret;
}

#if USE_SPLIT_PTLOCKS
/*
 * If we are using split PTE locks, then we need to take the page
 * lock here.  Otherwise we are using shared mm->page_table_lock
 * which is already locked, thus cannot take it.
 */
static inline void do_pte_lock(spinlock_t *ptl)
{
	/*
	 * Use nested version here to indicate that we are already
	 * holding one similar spinlock.
	 */
	spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);
}

static inline void do_pte_unlock(spinlock_t *ptl)
{
	spin_unlock(ptl);
}
#else /* !USE_SPLIT_PTLOCKS */
static inline void do_pte_lock(spinlock_t *ptl) {}
static inline void do_pte_unlock(spinlock_t *ptl) {}
#endif /* USE_SPLIT_PTLOCKS */

static int adjust_pte(struct vm_area_struct *vma, unsigned long address,
	unsigned long pfn)
{
	spinlock_t *ptl;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int ret;

	pgd = pgd_offset(vma->vm_mm, address);
	if (pgd_none_or_clear_bad(pgd))
		return 0;

	pud = pud_offset(pgd, address);
	if (pud_none_or_clear_bad(pud))
		return 0;

	pmd = pmd_offset(pud, address);
	if (pmd_none_or_clear_bad(pmd))
		return 0;

	/*
	 * This is called while another page table is mapped, so we
	 * must use the nested version.  This also means we need to
	 * open-code the spin-locking.
	 */
	ptl = pte_lockptr(vma->vm_mm, pmd);
	pte = pte_offset_map(pmd, address);
	do_pte_lock(ptl);

	ret = do_adjust_pte(vma, address, pfn, pte);

	do_pte_unlock(ptl);
	pte_unmap(pte);

	return ret;
}

static void
make_coherent(struct address_space *mapping, struct vm_area_struct *vma,
	unsigned long addr, pte_t *ptep, unsigned long pfn)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *mpnt;
	struct prio_tree_iter iter;
	unsigned long offset;
	pgoff_t pgoff;
	int aliases = 0;

	pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);

	/*
	 * If we have any shared mappings that are in the same mm
	 * space, then we need to handle them specially to maintain
	 * cache coherency.
	 */
	flush_dcache_mmap_lock(mapping);
	vma_prio_tree_foreach(mpnt, &iter, &mapping->i_mmap, pgoff, pgoff) {
		/*
		 * If this VMA is not in our MM, we can ignore it.
		 * Note that we intentionally mask out the VMA
		 * that we are fixing up.
		 */
		if (mpnt->vm_mm != mm || mpnt == vma)
			continue;
		if (!(mpnt->vm_flags & VM_MAYSHARE))
			continue;
		offset = (pgoff - mpnt->vm_pgoff) << PAGE_SHIFT;
		aliases += adjust_pte(mpnt, mpnt->vm_start + offset, pfn);
	}
	flush_dcache_mmap_unlock(mapping);
	if (aliases)
		do_adjust_pte(vma, addr, pfn, ptep);
}

/*
 * Take care of architecture specific things when placing a new PTE into
 * a page table, or changing an existing PTE.  Basically, there are two
 * things that we need to take care of:
 *
 *  1. If PG_dcache_clean is not set for the page, we need to ensure
 *     that any cache entries for the kernels virtual memory
 *     range are written back to the page.
 *  2. If we have multiple shared mappings of the same space in
 *     an object, we need to deal with the cache aliasing issues.
 *
 * Note that the pte lock will be held.
 */
void update_mmu_cache(struct vm_area_struct *vma, unsigned long addr,
	pte_t *ptep)
{
	unsigned long pfn = pte_pfn(*ptep);
	struct address_space *mapping;
	struct page *page;

	if (!pfn_valid(pfn))
		return;

	/*
	 * The zero page is never written to, so never has any dirty
	 * cache lines, and therefore never needs to be flushed.
	 */
	page = pfn_to_page(pfn);
	if (page == ZERO_PAGE(0))
		return;

	mapping = page_mapping(page);
	if (!test_and_set_bit(PG_dcache_clean, &page->flags))
		__flush_dcache_page(mapping, page);
	if (mapping) {
		if (cache_is_vivt())
			make_coherent(mapping, vma, addr, ptep, pfn);
		else if (vma->vm_flags & VM_EXEC)
			__flush_icache_all();
	}
}
#endif	/* __LINUX_ARM_ARCH__ < 6 */

/*
 * Check whether the write buffer has physical address aliasing
 * issues.  If it has, we need to avoid them for the case where
 * we have several shared mappings of the same object in user
 * space.
 */
/** 20150523
 * write buffer가 물리주소 aliasing 이슈가 있는지 검사한다.
 * 이슈가 있다면 1이 리턴된다.
 **/
static int __init check_writebuffer(unsigned long *p1, unsigned long *p2)
{
	register unsigned long zero = 0, one = 1, val;

	/** 20150523
	 * 인터럽트를 금지한 상태에서,
	 * 다른 가상 주소에 각각 1을 쓰고, 0을 쓰고, 먼저 쓴 곳의 값을 읽어
	 * 나중에 쓴 값과 다른지 검사한다.
	 * write 순서를 보장하기 위해 mb를 사용한다.
	 **/
	local_irq_disable();
	mb();
	*p1 = one;
	mb();
	*p2 = zero;
	mb();
	val = *p1;
	mb();
	local_irq_enable();
	return val != zero;
}

/** 20150523
 * writebuffer일 경우 발생할 수 있는 bug(physical address aliasing)가 있는지
 * 검사하고 조치한다.
 **/
void __init check_writebuffer_bugs(void)
{
	struct page *page;
	const char *reason;
	unsigned long v = 1;

	printk(KERN_INFO "CPU: Testing write buffer coherency: ");

	/** 20150523
	 * KERNEL에서 사용할 페이지 하나 할당.
	 **/
	page = alloc_page(GFP_KERNEL);
	if (page) {
		unsigned long *p1, *p2;
		/** 20150523
		 * PAGE_KERNEL 속성에서 memory type을 지우고 BUFFERABLE로 설정.
		 **/
		pgprot_t prot = __pgprot_modify(PAGE_KERNEL,
					L_PTE_MT_MASK, L_PTE_MT_BUFFERABLE);

		/** 20150523
		 * 할당받은 page를 가상 주소 공간에 VM_IOREMAP로 각각 매핑한다.
		 **/
		p1 = vmap(&page, 1, VM_IOREMAP, prot);
		p2 = vmap(&page, 1, VM_IOREMAP, prot);

		/** 20150523
		 * 각각 매핑한 주소를 check_writebuffer로 검사해
		 * 같지 않은지 결과를 받아온다.
		 **/
		if (p1 && p2) {
			v = check_writebuffer(p1, p2);
			reason = "enabling work-around";
		} else {
			reason = "unable to map memory\n";
		}

		/** 20150523
		 * 매핑한 공간을 해제하고, 할당받은 페이지를 반환한다.
		 **/
		vunmap(p1);
		vunmap(p2);
		put_page(page);
	} else {
		reason = "unable to grab page\n";
	}

	/** 20150523
	 * write buffer로 인한 physical address aliasing 이슈가 있다면 failed.
	 * shared_pte_mask에 대해 L_PTE_MT_UNCACHED 속성을 지정한다.
	 **/
	if (v) {
		printk("failed, %s\n", reason);
		shared_pte_mask = L_PTE_MT_UNCACHED;
	} else {
		printk("ok\n");
	}
}
