/*
 * arch/arm/mm/highmem.c -- ARM highmem support
 *
 * Author:	Nicolas Pitre
 * Created:	september 8, 2008
 * Copyright:	Marvell Semiconductors Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <asm/fixmap.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include "mm.h"

void *kmap(struct page *page)
{
	might_sleep();
	if (!PageHighMem(page))
		return page_address(page);
	return kmap_high(page);
}
EXPORT_SYMBOL(kmap);

void kunmap(struct page *page)
{
	BUG_ON(in_interrupt());
	if (!PageHighMem(page))
		return;
	kunmap_high(page);
}
EXPORT_SYMBOL(kunmap);

void *kmap_atomic(struct page *page)
{
	unsigned int idx;
	unsigned long vaddr;
	void *kmap;
	int type;
/** 20131012
 * pagefault handler를 disable시킨다.
 **/
	pagefault_disable();
    /** 20131012
    * CONFIG_HIGHMEM이 define되어있지 않으므로 page_address(page)를 리턴 
     **/

	if (!PageHighMem(page))
      /** 20131012
        * page에 대한 virtual address를 리턴한다.
       **/
		return page_address(page);

#ifdef CONFIG_DEBUG_HIGHMEM
	/*
	 * There is no cache coherency issue when non VIVT, so force the
	 * dedicated kmap usage for better debugging purposes in that case.
	 */
	if (!cache_is_vivt())
		kmap = NULL;
	else
#endif
		kmap = kmap_high_get(page);
	if (kmap)
		return kmap;

    /** 20131012
    * 현재 __kmap_atomic_idx값을 type에 저장하고, 1 증가시킨다. 
     **/
	type = kmap_atomic_idx_push();
    /** 20131019
    idx               smp_id_processor
    0    +-------------+       0
    1    |     type 0  |                   
    2    |     type 1  |
    .    |     ......  |
    15   |     type 15 |
    16   +-------------+       1
    .    |     type 0  |                 
    .    |     type 1  |
    .    |     ......  |
    .    |     type 15 |
    .    +-------------+       2
    .    |     type 0  |               
    .    |     type 1  |
    .    |     ......  |
    .    |     type 15 |
    .    +-------------+       ...
    .    |     type 0  |             
    .    |     type 1  |
              ......

     **/
	idx = type + KM_TYPE_NR * smp_processor_id();
    /** 20131019
        idx를 통해서 가상주소를 얻어온다.
     **/
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	/*
	 * With debugging enabled, kunmap_atomic forces that entry to 0.
	 * Make sure it was indeed properly unmapped.
	 */
	BUG_ON(!pte_none(get_top_pte(vaddr)));
#endif
	/*
	 * When debugging is off, kunmap_atomic leaves the previous mapping
	 * in place, so the contained TLB flush ensures the TLB is updated
	 * with the new mapping.
	 */
    /** 20131019
    * top_pmd에서 vaddr에 대한 pte entry의 위치를 구하고, 
    * mk_pte로 pte값을 생성해서 넣어준다.
     **/
	set_top_pte(vaddr, mk_pte(page, kmap_prot));
  /** 20131026
      다음주 진행예정
**/

	return (void *)vaddr;
}
EXPORT_SYMBOL(kmap_atomic);

void __kunmap_atomic(void *kvaddr)
{
	unsigned long vaddr = (unsigned long) kvaddr & PAGE_MASK;
	int idx, type;

	if (kvaddr >= (void *)FIXADDR_START) {
		type = kmap_atomic_idx();
		idx = type + KM_TYPE_NR * smp_processor_id();

		if (cache_is_vivt())
			__cpuc_flush_dcache_area((void *)vaddr, PAGE_SIZE);
#ifdef CONFIG_DEBUG_HIGHMEM
		BUG_ON(vaddr != __fix_to_virt(FIX_KMAP_BEGIN + idx));
		set_top_pte(vaddr, __pte(0));
#else
		(void) idx;  /* to kill a warning */
#endif
		kmap_atomic_idx_pop();
	} else if (vaddr >= PKMAP_ADDR(0) && vaddr < PKMAP_ADDR(LAST_PKMAP)) {
		/* this address was obtained through kmap_high_get() */
		kunmap_high(pte_page(pkmap_page_table[PKMAP_NR(vaddr)]));
	}
	pagefault_enable();
}
EXPORT_SYMBOL(__kunmap_atomic);

void *kmap_atomic_pfn(unsigned long pfn)
{
	unsigned long vaddr;
	int idx, type;

	pagefault_disable();

	type = kmap_atomic_idx_push();
	idx = type + KM_TYPE_NR * smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	BUG_ON(!pte_none(get_top_pte(vaddr)));
#endif
	set_top_pte(vaddr, pfn_pte(pfn, kmap_prot));

	return (void *)vaddr;
}

struct page *kmap_atomic_to_page(const void *ptr)
{
	unsigned long vaddr = (unsigned long)ptr;

	if (vaddr < FIXADDR_START)
		return virt_to_page(ptr);

	return pte_page(get_top_pte(vaddr));
}
