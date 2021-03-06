/*
 *  linux/arch/arm/mm/pgd.c
 *
 *  Copyright (C) 1998-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#include <asm/cp15.h>
#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

#include "mm.h"

#ifdef CONFIG_ARM_LPAE
#define __pgd_alloc()	kmalloc(PTRS_PER_PGD * sizeof(pgd_t), GFP_KERNEL)
#define __pgd_free(pgd)	kfree(pgd)
#else
/** 20150613
 * 2 << 2개의 page를 KERNEL 속성으로 할당 받는다.
 **/
#define __pgd_alloc()	(pgd_t *)__get_free_pages(GFP_KERNEL, 2)
#define __pgd_free(pgd)	free_pages((unsigned long)pgd, 2)
#endif

/*
 * need to get a 16k page for level 1
 */
/** 20150613
 * Level 1 page table을 위한 page를 할당받고,
 * 초기화된 page table로부터 kernel 영역과 IO 영역의 page table을 복사한다.
 **/
pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *new_pgd, *init_pgd;
	pud_t *new_pud, *init_pud;
	pmd_t *new_pmd, *init_pmd;
	pte_t *new_pte, *init_pte;

	/** 20150613
	 * L1 page table 크기만큼 page를 할당받는다.
	 **/
	new_pgd = __pgd_alloc();
	if (!new_pgd)
		goto no_pgd;

	/** 20150613
	 * 할당받은 page를 0으로 초기화 한다.
	 **/
	memset(new_pgd, 0, USER_PTRS_PER_PGD * sizeof(pgd_t));

	/*
	 * Copy over the kernel and IO PGD entries
	 */
	/** 20150613
	 * 새로 할당받은 pgd 영역에 기존의 kernel 영역과 IO PGD entry를 복사한다.
	 **/
	init_pgd = pgd_offset_k(0);
	memcpy(new_pgd + USER_PTRS_PER_PGD, init_pgd + USER_PTRS_PER_PGD,
		       (PTRS_PER_PGD - USER_PTRS_PER_PGD) * sizeof(pgd_t));

	/** 20150620
	 * new_pgd 영역의 dcache를 클린시킨다.
	 **/
	clean_dcache_area(new_pgd, PTRS_PER_PGD * sizeof(pgd_t));

#ifdef CONFIG_ARM_LPAE
	/*
	 * Allocate PMD table for modules and pkmap mappings.
	 */
	new_pud = pud_alloc(mm, new_pgd + pgd_index(MODULES_VADDR),
			    MODULES_VADDR);
	if (!new_pud)
		goto no_pud;

	new_pmd = pmd_alloc(mm, new_pud, 0);
	if (!new_pmd)
		goto no_pmd;
#endif

	/** 20150620
	 * high vector가 아닌 경우 vector table에 대한 pte entry를 설정한다.
	 * low vector인 경우 추후분석???
	 **/
	if (!vectors_high()) {
		/*
		 * On ARM, first page must always be allocated since it
		 * contains the machine vectors. The vectors are always high
		 * with LPAE.
		 */
		new_pud = pud_alloc(mm, new_pgd, 0);
		if (!new_pud)
			goto no_pud;

		new_pmd = pmd_alloc(mm, new_pud, 0);
		if (!new_pmd)
			goto no_pmd;

		/** 20160416
		 **/
		new_pte = pte_alloc_map(mm, NULL, new_pmd, 0);
		if (!new_pte)
			goto no_pte;

		init_pud = pud_offset(init_pgd, 0);
		init_pmd = pmd_offset(init_pud, 0);
		init_pte = pte_offset_map(init_pmd, 0);
		set_pte_ext(new_pte, *init_pte, 0);
		pte_unmap(init_pte);
		pte_unmap(new_pte);
	}

	return new_pgd;

no_pte:
	pmd_free(mm, new_pmd);
no_pmd:
	pud_free(mm, new_pud);
no_pud:
	__pgd_free(new_pgd);
no_pgd:
	return NULL;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd_base)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pgtable_t pte;

	if (!pgd_base)
		return;

	/** 20160625
	 * index 0에 해당하는 pgd, pud, pmd를 받아온다.
	 **/
	pgd = pgd_base + pgd_index(0);
	if (pgd_none_or_clear_bad(pgd))
		goto no_pgd;

	pud = pud_offset(pgd, 0);
	if (pud_none_or_clear_bad(pud))
		goto no_pud;

	pmd = pmd_offset(pud, 0);
	if (pmd_none_or_clear_bad(pmd))
		goto no_pmd;

	/** 20160625
	 * pmd entry가 가리키는 page (pte table) 주소를 리턴.
	 *
	 * 20170228
	 * 모든 entry를 다 제거하지 않고 첫번째 주소에 해당하는 엔트리만 제거
	 **/
	pte = pmd_pgtable(*pmd);
	pmd_clear(pmd);
	pte_free(mm, pte);
no_pmd:
	pud_clear(pud);
	pmd_free(mm, pmd);
no_pud:
	pgd_clear(pgd);
	pud_free(mm, pud);
no_pgd:
#ifdef CONFIG_ARM_LPAE
	/*
	 * Free modules/pkmap or identity pmd tables.
	 */
	for (pgd = pgd_base; pgd < pgd_base + PTRS_PER_PGD; pgd++) {
		if (pgd_none_or_clear_bad(pgd))
			continue;
		if (pgd_val(*pgd) & L_PGD_SWAPPER)
			continue;
		pud = pud_offset(pgd, 0);
		if (pud_none_or_clear_bad(pud))
			continue;
		pmd = pmd_offset(pud, 0);
		pud_clear(pud);
		pmd_free(mm, pmd);
		pgd_clear(pgd);
		pud_free(mm, pud);
	}
#endif
	__pgd_free(pgd_base);
}
