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
/** 20131109
 * page가 가리키는 페이지 프레임을 매핑하고 VA를 리턴한다.
 *
 * page에 대한 virtual adddress를 리턴한다.
 * 단, mapping할 슬롯이 꽉차 VA를 할당받지 못할 때 sleep 하기 때문에
 * interrupt context에서 호출되어서는 안된다.
 *
 * kmap
 *	kmap_high
 *		map_new_virtual		// PKMAP_BASE부터 PMD 크기만큼 영역
 **/
void *kmap(struct page *page)
{
	/** 20131026
	 * vexpress의 경우 default로 CONFIG_PREEMPT_VOLUNTARY 선언되어 있지 않음.
	 * might_sleep()은 NULL 함수.
	 *
	 * 선언되어 있다면 resched 대기 중인지 검사해 scheduler를 호출함.
	 * 따라서 kmap은 interrupt context에서 호출할 수 없다.
	 **/
	might_sleep();
	/** 20131026
	 * page가 highmem이 아니라면 page_address로 VA를 바로 리턴.
	 **/
	if (!PageHighMem(page))
		return page_address(page);
	/** 20131026
	 * highmem이라면 page에 대한 virtual address를 리턴한다.
	 **/
	return kmap_high(page);
}
EXPORT_SYMBOL(kmap);

/** 20131109
 * page가 highmem영역이면 virtual address를 unmapping하고
 * 휴면 상태에 있는 태스크를 깨운다
 **/
void kunmap(struct page *page)
{
	BUG_ON(in_interrupt());
	/** 20131109
	 * page가 highmem영역에 없으면 바로 리턴
	 **/
	if (!PageHighMem(page))
		return;

	kunmap_high(page);
}
EXPORT_SYMBOL(kunmap);

/** 20131026
 * page가 가리키는 페이지 프레임을 매핑하고 VA를 리턴한다.
 *
 * 1) highmem 영역이 아닌 page에 대한 요청일 경우
 *     이미 매핑된 VA를 리턴.
 * 2) highmem 영역인 page에 대한 요청일 경우
 *     2-1) 이미 kmap_high로 매핑된 주소인 경우
 *       이미 매핑된 VA를 리턴.
 *     2-2) 이미 매핑된 주소가 아닌 경우
 *       cpu별로 준비된 fixmap address 영역에서 va를 할당 받아 매핑한 뒤,
 *       매핑된 주소를 리턴 (전역 lock이 필요 없다)
 *       
 *  - 물리 메모리까지 할당받는 함수는 아니다.
 *
 *  kmap_atomic   (pagefault_disable 부터 수행)
 *  ...			// Fixmap 영역에 매핑
 *  kunmap_atomic (동작 후 pagefault_enable 수행)
 *
 * Documentation/vm/highmem.txt 참고.
 **/
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
	 * page가 highmem 영역에 속하지 않는다면 page가 매핑된 VA를 바로 리턴한다.
	 **/
	if (!PageHighMem(page))
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
		/** 20131026
		 * page에 대해 mapping 된 VA가 있다면 해당 vaddr을,
		 * 없다면 NULL을 리턴한다.
		 * 즉, kmap_high를 통해 이미 mapping된 페이지라면 매핑된 VA를 받아온다.
		 **/
		kmap = kmap_high_get(page);
	/** 20131026
	 * kmap이 NULL이 아니라면 이미 매핑된 가상주소를 얻어왔으므로 리턴
	 **/
	if (kmap)
		return kmap;

	/** 20131012
	 * 현재 __kmap_atomic_idx (percpu 선언)값을 type에 저장하고, 1 증가시킨다. 
	 **/
	type = kmap_atomic_idx_push();
	/** 20131019
	 * idx               smp_id_processor
	 * 0    +-------------+       0
	 * 1    |     type 0  |
	 * 2    |     type 1  |
	 * .    |     ......  |
	 * 15   |     type 15 |
	 * 16   +-------------+       1
	 * .    |     type 0  |
	 * .    |     type 1  |
	 * .    |     ......  |
	 * .    |     type 15 |
	 * .    +-------------+       2
	 * .    |     type 0  |
	 * .    |     type 1  |
	 * .    |     ......  |
	 * .    |     type 15 |
	 * .    +-------------+       ...
	 * .    |     type 0  |
	 * .    |     type 1  |
	 * ......
	 **/
	idx = type + KM_TYPE_NR * smp_processor_id();
	/** 20131019
	 * idx를 통해서 fixaddr 구간에 해당하는 가상주소를 얻어온다.
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
	 * pte에 등록한 vaddr를 리턴.
	 **/
	return (void *)vaddr;
}
EXPORT_SYMBOL(kmap_atomic);

void __kunmap_atomic(void *kvaddr)
{
	/** 20131026
	 * kvaddr를 PAGE 단위로 정렬시킨다.
	 **/
	unsigned long vaddr = (unsigned long) kvaddr & PAGE_MASK;
	int idx, type;

	/** 20131026
	 * kvaddr >= (void *)FIXADDR_START 라면
	 *   kmap_atomic으로 매핑된 경우
	 **/
	if (kvaddr >= (void *)FIXADDR_START) {
		/** 20131026
		 * percpu변수에서 마지막으로 mapping된 값을 type으로 가져온다.
		 **/
		type = kmap_atomic_idx();
		/** 20131026
		 * fixmap에서의 idx를 계산한다.
		 **/
		idx = type + KM_TYPE_NR * smp_processor_id();

		/** 20131026
		 * vexpress 의 경우 CACHEID_VIVT가 __CACHEID_NEVER 에 속해 false.
		 **/
		if (cache_is_vivt())
			/** 20131026
			 * vaddr에 대해 하나의 PAGE만큼 data cache를 flush한다.
			 * cache가 vipt인 경우에는 flush 하지 않아도 되나???
			 **/
			__cpuc_flush_dcache_area((void *)vaddr, PAGE_SIZE);
#ifdef CONFIG_DEBUG_HIGHMEM
		BUG_ON(vaddr != __fix_to_virt(FIX_KMAP_BEGIN + idx));
		/** 20131026
		 * top_pte에 0 번지 값을 넣어준다.
		 **/
		set_top_pte(vaddr, __pte(0));
#else
		/** 20131026
		 * idx가 사용되지 않으면 compiler가 warning을 발생시키므로
		 * debug가 아닐 경우에도 사용해 준다.
		 **/
		(void) idx;  /* to kill a warning */
#endif
		/** 20131026
		 * fixmap 용으로 사용하는 index를 감소시킨다.
		 * 즉, 직접적으로 unmap하는 부분은 존재 않는다.
		 *
		 * CONFIG_DEBUG_HIGHMEM을 사용하지 않는 경우에는
		 * kunmap한 이후에 다시 접근해도 BUG가 출력되지는 않을 듯???
		 **/
		kmap_atomic_idx_pop();
	/** 20131026
	 * PKMAP_ADDR(0) <= vaddr < PKMAP_ADDR(LAST_PKMAP) 라면
	 *   kmap으로 매핑된 경우
	 **/
	} else if (vaddr >= PKMAP_ADDR(0) && vaddr < PKMAP_ADDR(LAST_PKMAP)) {
		/* this address was obtained through kmap_high_get() */
		/** 20131109
		 * vaddr에 해당되는 PKMP_Nr을 구하고, pkmap_page_table에서
		 * 해당 인덱스에 대한 pte를 가져온다.
		 * 그리고 pte엔트리에 대한 page를 가져와서 unmapping한다
		 **/
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
