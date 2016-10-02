/*
 *  arch/arm/include/asm/pgalloc.h
 *
 *  Copyright (C) 2000-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASMARM_PGALLOC_H
#define _ASMARM_PGALLOC_H

#include <linux/pagemap.h>

#include <asm/domain.h>
#include <asm/pgtable-hwdef.h>
#include <asm/processor.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#define check_pgt_cache()		do { } while (0)

#ifdef CONFIG_MMU

#define _PAGE_USER_TABLE	(PMD_TYPE_TABLE | PMD_BIT4 | PMD_DOMAIN(DOMAIN_USER))
/** 20140329    
 * prot로 kernel에 해당하는 PMD table 정보를 기록한다.
 **/
#define _PAGE_KERNEL_TABLE	(PMD_TYPE_TABLE | PMD_BIT4 | PMD_DOMAIN(DOMAIN_KERNEL))

#ifdef CONFIG_ARM_LPAE

static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	return (pmd_t *)get_zeroed_page(GFP_KERNEL | __GFP_REPEAT);
}

static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
	free_page((unsigned long)pmd);
}

static inline void pud_populate(struct mm_struct *mm, pud_t *pud, pmd_t *pmd)
{
	set_pud(pud, __pud(__pa(pmd) | PMD_TYPE_TABLE));
}

#else	/* !CONFIG_ARM_LPAE */

/*
 * Since we have only two-level page tables, these are trivial
 */
#define pmd_alloc_one(mm,addr)		({ BUG(); ((pmd_t *)2); })
#define pmd_free(mm, pmd)		do { } while (0)
#define pud_populate(mm,pmd,pte)	BUG()

#endif	/* CONFIG_ARM_LPAE */

extern pgd_t *pgd_alloc(struct mm_struct *mm);
extern void pgd_free(struct mm_struct *mm, pgd_t *pgd);

#define PGALLOC_GFP	(GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO)

/** 20140329    
 * 할당받아온 pte에 해당하는 data cache를 clean (dirty cache를 메모리에 반영한다)한다.
 **/
static inline void clean_pte_table(pte_t *pte)
{
	clean_dcache_area(pte + PTE_HWTABLE_PTRS, PTE_HWTABLE_SIZE);
}

/*
 * Allocate one PTE table.
 *
 * This actually allocates two hardware PTE tables, but we wrap this up
 * into one table thus:
 *
 *  +------------+
 *  | Linux pt 0 |
 *  +------------+
 *  | Linux pt 1 |
 *  +------------+
 *  |  h/w pt 0  |
 *  +------------+
 *  |  h/w pt 1  |
 *  +------------+
 */
/** 20140329    
 * pte table용 페이지를 하나 할당 받는다.
 * 페이지는 위에 있는 그림대로 나누어 사용한다.
 **/
static inline pte_t *
pte_alloc_one_kernel(struct mm_struct *mm, unsigned long addr)
{
	pte_t *pte;

	/** 20140329    
	 * PGALLOC_GFP 속성에 해당하는 페이지 하나를 할당 받는다.
	 **/
	pte = (pte_t *)__get_free_page(PGALLOC_GFP);
	/** 20140329    
	 * 할당 받아온 주소에 해당하는 메모리를 반영시킨다.
	 **/
	if (pte)
		clean_pte_table(pte);

	return pte;
}

/** 20160416    
 * pte용 page를 할당받아 초기화하고 리턴한다.
 **/
static inline pgtable_t
pte_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	struct page *pte;

#ifdef CONFIG_HIGHPTE
	pte = alloc_pages(PGALLOC_GFP | __GFP_HIGHMEM, 0);
#else
	/** 20160416    
	 * pte용 page를 할당 받는다.
	 **/
	pte = alloc_pages(PGALLOC_GFP, 0);
#endif
	/** 20160416    
	 * 할당이 성공했으면 
	 *   highmem zone이 아닌 영역을 받아온 경우 pte에 해당하는 dcache를 flush.
	 *   page table용 page를 초기화.
	 **/
	if (pte) {
		if (!PageHighMem(pte))
			clean_pte_table(page_address(pte));
		pgtable_page_ctor(pte);
	}

	/** 20160416    
	 * 할당 받은 페이지 리턴.
	 **/
	return pte;
}

/*
 * Free one PTE table.
 */
static inline void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
	if (pte)
		free_page((unsigned long)pte);
}

static inline void pte_free(struct mm_struct *mm, pgtable_t pte)
{
	pgtable_page_dtor(pte);
	__free_page(pte);
}

/** 20130309    
 * pmd에 pte 주소를 포함한 value를 채운다.
 **/
/** 20131019
  * LPAE가 아닐경우 pmdp[0],pmdp[1]에 pte table 각각의 시작주소와 
  * prot속성으로 엔트리를 채운다.
 **/
static inline void __pmd_populate(pmd_t *pmdp, phys_addr_t pte,
				  pmdval_t prot)
{
/** 20130302 
	prot : 0x01 
 	HW 1단계 PAGE table entry를 사용하겠다는 의미
	build_mem_type_table 함수 에서 prot속성에 domain 이 세팅 되어있음 
 **/	
 /** 20130309
  * 할당받은 pte 주소(PA)에 HWTABLE offset을 더하고 prot (0x1)를 더해 pdmval을 구한다.
  **/	
	pmdval_t pmdval = (pte + PTE_HWTABLE_OFF) | prot;
	/** 20130309    
	 * pmd에 h/w  pt 0 값을 넣어주고, LPAE가 아닐 경우 다음 pmd에 h/w pt 1의 값을 채운다.
	 **/
	pmdp[0] = __pmd(pmdval);
#ifndef CONFIG_ARM_LPAE
	pmdp[1] = __pmd(pmdval + 256 * sizeof(pte_t));
#endif
	/** 20130309    
	 * pmd 내용이 변경되었으므로 flush를 수행한다.
	 **/
	flush_pmd_entry(pmdp);
}

/*
 * Populate the pmdp entry with a pointer to the pte.  This pmd is part
 * of the mm address space.
 *
 * Ensure that we always set both PMD entries.
 */
/** 20140329    
 * pmdp (pmd entry)에
 *	ptep의 위치(pte table)와 _PAGE_KERNEL_TABLE 속성을 결합시켜 저장한다.
 **/
static inline void
pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmdp, pte_t *ptep)
{
	/*
	 * The pmd must be loaded with the physical address of the PTE table
	 */
	__pmd_populate(pmdp, __pa(ptep), _PAGE_KERNEL_TABLE);
}

/** 20160416    
 * pmd entry에 pte주소와 속성을 포함한 값을 채운다.
 **/
static inline void
pmd_populate(struct mm_struct *mm, pmd_t *pmdp, pgtable_t ptep)
{
	__pmd_populate(pmdp, page_to_phys(ptep), _PAGE_USER_TABLE);
}
/** 20160625
 * pmd entry가 가리키는 page 구조체를 리턴한다.
 **/
#define pmd_pgtable(pmd) pmd_page(pmd)

#endif /* CONFIG_MMU */

#endif
