/*
 *  linux/arch/arm/mm/flush.c
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>

#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/highmem.h>
#include <asm/smp_plat.h>
#include <asm/tlbflush.h>

#include "mm.h"

#ifdef CONFIG_CPU_CACHE_VIPT

/** 20130518    
 * 캐시가 VIPT ALIAS 정책을 사용하는 경우 flush 해주는 함수.
 * 그러나 이것이 어떤 의미를 갖는지 모르겠음???
 **/
static void flush_pfn_alias(unsigned long pfn, unsigned long vaddr)
{
	/** 20130511 
	SHMLBA 마스킹한 하위 주소를 가져와서 pfn으로 변환. 
	FLUSH_ALIAS_START : 0xffff4000
	**/
	unsigned long to = FLUSH_ALIAS_START + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	const int zero = 0;
	/** 20130511
	pfn_pte: pfn에 해당하는 paddr와 page_kernel속성을 더해 pte 값을 구한다.
	set_top_pte
	**/
	set_top_pte(to, pfn_pte(pfn, PAGE_KERNEL));

	/** 20130518    
	 * r0 : to
	 * r1 : to + PAGE_SIZE - L1_CACHE_BYTES
	 * r2 : zero
	 *
	 * 첫번째 instruction은 
	 r0에서r1까지의 범위에 data cache를 clean하고 invalidate시킴.
	 -> Block transfer operation중의 하나임
 	 (c14 0 Clean and invalidate data cache range  Start address End address)
	 * 두번째 instruction은 DSB operation.  **/
	asm(	"mcrr	p15, 0, %1, %0, c14\n"
	"	mcr	p15, 0, %2, c7, c10, 4"
	    :
	    : "r" (to), "r" (to + PAGE_SIZE - L1_CACHE_BYTES), "r" (zero)
	    : "cc");
}

static void flush_icache_alias(unsigned long pfn, unsigned long vaddr, unsigned long len)
{
	unsigned long va = FLUSH_ALIAS_START + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	unsigned long offset = vaddr & (PAGE_SIZE - 1);
	unsigned long to;

	set_top_pte(va, pfn_pte(pfn, PAGE_KERNEL));
	to = va + offset;
	flush_icache_range(to, to + len);
}

void flush_cache_mm(struct mm_struct *mm)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_mm(mm);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		asm(	"mcr	p15, 0, %0, c7, c14, 0\n"
		"	mcr	p15, 0, %0, c7, c10, 4"
		    :
		    : "r" (0)
		    : "cc");
	}
}

void flush_cache_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_range(vma, start, end);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		asm(	"mcr	p15, 0, %0, c7, c14, 0\n"
		"	mcr	p15, 0, %0, c7, c10, 4"
		    :
		    : "r" (0)
		    : "cc");
	}

	if (vma->vm_flags & VM_EXEC)
		__flush_icache_all();
}

void flush_cache_page(struct vm_area_struct *vma, unsigned long user_addr, unsigned long pfn)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_page(vma, user_addr, pfn);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		flush_pfn_alias(pfn, user_addr);
		__flush_icache_all();
	}

	if (vma->vm_flags & VM_EXEC && icache_is_vivt_asid_tagged())
		__flush_icache_all();
}

#else
#define flush_pfn_alias(pfn,vaddr)		do { } while (0)
#define flush_icache_alias(pfn,vaddr,len)	do { } while (0)
#endif

static void flush_ptrace_access_other(void *args)
{
	__flush_icache_all();
}

static
void flush_ptrace_access(struct vm_area_struct *vma, struct page *page,
			 unsigned long uaddr, void *kaddr, unsigned long len)
{
	if (cache_is_vivt()) {
		if (cpumask_test_cpu(smp_processor_id(), mm_cpumask(vma->vm_mm))) {
			unsigned long addr = (unsigned long)kaddr;
			__cpuc_coherent_kern_range(addr, addr + len);
		}
		return;
	}

	if (cache_is_vipt_aliasing()) {
		flush_pfn_alias(page_to_pfn(page), uaddr);
		__flush_icache_all();
		return;
	}

	/* VIPT non-aliasing D-cache */
	if (vma->vm_flags & VM_EXEC) {
		unsigned long addr = (unsigned long)kaddr;
		if (icache_is_vipt_aliasing())
			flush_icache_alias(page_to_pfn(page), uaddr, len);
		else
			__cpuc_coherent_kern_range(addr, addr + len);
		if (cache_ops_need_broadcast())
			smp_call_function(flush_ptrace_access_other,
					  NULL, 1);
	}
}

/*
 * Copy user data from/to a page which is mapped into a different
 * processes address space.  Really, we want to allow our "user
 * space" model to handle this.
 *
 * Note that this code needs to run on the current CPU.
 */
void copy_to_user_page(struct vm_area_struct *vma, struct page *page,
		       unsigned long uaddr, void *dst, const void *src,
		       unsigned long len)
{
#ifdef CONFIG_SMP
	preempt_disable();
#endif
	memcpy(dst, src, len);
	flush_ptrace_access(vma, page, uaddr, dst, len);
#ifdef CONFIG_SMP
	preempt_enable();
#endif
}
/** 20130511 
**/

/** 20130518    
 * page를 받아 해당 페이지가 가리키는 메모리 영역에 대해 flush를 수행하는 함수
 **/
void __flush_dcache_page(struct address_space *mapping, struct page *page)
{
	/*
	 * Writeback any data associated with the kernel mapping of this
	 * page.  This ensures that data in the physical page is mutually
	 * coherent with the kernels mapping.
	 */
	if (!PageHighMem(page)) {
		/** 20130511 
		v7_flush_kern_dcache_area
		**/
		__cpuc_flush_dcache_area(page_address(page), PAGE_SIZE);
	} else {
		void *addr = 0; /*kmap_high_get(page); */
		if (addr) {
			__cpuc_flush_dcache_area(addr, PAGE_SIZE);
			0; /*kunmap_high(page); */
		} else if (cache_is_vipt()) {
			/* unmapped pages might still be cached */
			addr = kmap_atomic(page);
			__cpuc_flush_dcache_area(addr, PAGE_SIZE);
			kunmap_atomic(addr);
		}
	}

	/*
	 * If this is a page cache page, and we have an aliasing VIPT cache,
	 * we only need to do one flush - which would be at the relevant
	 * userspace colour, which is congruent with page->index.
	 */

	/** 20131109
	 * mapping이 존재하고 현재 시스템의 cache형태가 vipt aliasing인 경우
	 * 해당 페이지 구간을 플러시한다.
	 **/
	if (mapping && cache_is_vipt_aliasing())
		flush_pfn_alias(page_to_pfn(page),
				page->index << PAGE_CACHE_SHIFT);
}

static void __flush_dcache_aliases(struct address_space *mapping, struct page *page)
{
	struct mm_struct *mm = current->active_mm;
	struct vm_area_struct *mpnt;
	struct prio_tree_iter iter;
	pgoff_t pgoff;

	/*
	 * There are possible user space mappings of this page:
	 * - VIVT cache: we need to also write back and invalidate all user
	 *   data in the current VM view associated with this page.
	 * - aliasing VIPT: we only need to find one mapping of this page.
	 */
	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);

	flush_dcache_mmap_lock(mapping);
	vma_prio_tree_foreach(mpnt, &iter, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long offset;

		/*
		 * If this VMA is not in our MM, we can ignore it.
		 */
		if (mpnt->vm_mm != mm)
			continue;
		if (!(mpnt->vm_flags & VM_MAYSHARE))
			continue;
		offset = (pgoff - mpnt->vm_pgoff) << PAGE_SHIFT;
		flush_cache_page(mpnt, mpnt->vm_start + offset, page_to_pfn(page));
	}
	flush_dcache_mmap_unlock(mapping);
}

/** 20131102    
 * __LINUX_ARM_ARCH__ 가 7이므로 이 함수 호출.
 **/
/** 20131109
 * pteval에 해당되는 영역의 icache와 dcache를 flush 해준다
 **/
#if __LINUX_ARM_ARCH__ >= 6
void __sync_icache_dcache(pte_t pteval)
{
	unsigned long pfn;
	struct page *page;
	struct address_space *mapping;

	/** 20131102    
	 * L_PTE_PRESENT와 L_PTE_USER 속성이 모두 지정되어 있는 않은 경우
	 * 바로 리턴.
	 **/
	if (!pte_present_user(pteval))
		return;
	/** 20131102    
	 * vipt_nonaliasing이면서 실행할 수 없는 address range인 경우 바로 리턴.
	 * (실행할 수 없는 instruction인 경우) 왜 ???
	 **/
	if (cache_is_vipt_nonaliasing() && !pte_exec(pteval))
		/* only flush non-aliasing VIPT caches for exec mappings */
		return;
	/** 20131102    
	 * pteval에서 주소에 해당하는 부분을 추출해 pfn을 구한다.
	 **/
	pfn = pte_pfn(pteval);
	/** 20131102    
	 * 물리 메모리 내에 속하지 않는 pfn인 경우 바로 리턴.
	 **/
	if (!pfn_valid(pfn))
		return;

	/** 20131102    
	 * pfn에 해당하는 page 구조체를 구해온다.
	 **/
	page = pfn_to_page(pfn);
	/** 20131102    
	 * cache_is_vipt_aliasing인 경우 mapping 주소를 받아온다.
	 * 그렇지 않은 경우 mapping은 NULL.
	 *
	**/
	if (cache_is_vipt_aliasing())
		mapping = page_mapping(page);
	else
		mapping = NULL;

	/** 20131109
	 * page->flags중 PG_dcache_clean이 세팅 되어 있지 않으면 
	 * PG_dcache_clean을 세팅하고 page의 dcache를 flush한다.
	 **/
	if (!test_and_set_bit(PG_dcache_clean, &page->flags))
		__flush_dcache_page(mapping, page);
	/** 20131109
	  pteval가 가리키는 page가 실행 가능한 영역이면 icache를 flush한다.
 	**/
	if (pte_exec(pteval))
		__flush_icache_all();
}
#endif

/*
 * Ensure cache coherency between kernel mapping and userspace mapping
 * of this page.
 *
 * We have three cases to consider:
 *  - VIPT non-aliasing cache: fully coherent so nothing required.
 *  - VIVT: fully aliasing, so we need to handle every alias in our
 *          current VM view.
 *  - VIPT aliasing: need to handle one alias in our current VM view.
 *
 * If we need to handle aliasing:
 *  If the page only exists in the page cache and there are no user
 *  space mappings, we can be lazy and remember that we may have dirty
 *  kernel cache lines for later.  Otherwise, we assume we have
 *  aliasing mappings.
 *
 * Note that we disable the lazy flush for SMP configurations where
 * the cache maintenance operations are not automatically broadcasted.
 */
void flush_dcache_page(struct page *page)
{
	struct address_space *mapping;

	/*
	 * The zero page is never written to, so never has any dirty
	 * cache lines, and therefore never needs to be flushed.
	 */
	if (page == ZERO_PAGE(0))
		return;

	mapping = page_mapping(page);

	if (!cache_ops_need_broadcast() &&
	    mapping && !mapping_mapped(mapping))
		clear_bit(PG_dcache_clean, &page->flags);
	else {
		__flush_dcache_page(mapping, page);
		if (mapping && cache_is_vivt())
			__flush_dcache_aliases(mapping, page);
		else if (mapping)
			__flush_icache_all();
		set_bit(PG_dcache_clean, &page->flags);
	}
}
EXPORT_SYMBOL(flush_dcache_page);

/*
 * Flush an anonymous page so that users of get_user_pages()
 * can safely access the data.  The expected sequence is:
 *
 *  get_user_pages()
 *    -> flush_anon_page
 *  memcpy() to/from page
 *  if written to page, flush_dcache_page()
 */
void __flush_anon_page(struct vm_area_struct *vma, struct page *page, unsigned long vmaddr)
{
	unsigned long pfn;

	/* VIPT non-aliasing caches need do nothing */
	if (cache_is_vipt_nonaliasing())
		return;

	/*
	 * Write back and invalidate userspace mapping.
	 */
	pfn = page_to_pfn(page);
	if (cache_is_vivt()) {
		flush_cache_page(vma, vmaddr, pfn);
	} else {
		/*
		 * For aliasing VIPT, we can flush an alias of the
		 * userspace address only.
		 */
		flush_pfn_alias(pfn, vmaddr);
		__flush_icache_all();
	}

	/*
	 * Invalidate kernel mapping.  No data should be contained
	 * in this mapping of the page.  FIXME: this is overkill
	 * since we actually ask for a write-back and invalidate.
	 */
	__cpuc_flush_dcache_area(page_address(page), PAGE_SIZE);
}
