/*
 *  arch/arm/include/asm/pgtable.h
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASMARM_PGTABLE_H
#define _ASMARM_PGTABLE_H

#include <linux/const.h>
#include <asm/proc-fns.h>

#ifndef CONFIG_MMU

#include <asm-generic/4level-fixup.h>
#include "pgtable-nommu.h"

#else

#include <asm-generic/pgtable-nopud.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>

#ifdef CONFIG_ARM_LPAE
#include <asm/pgtable-3level.h>
#else
#include <asm/pgtable-2level.h>
#endif

/*
 * Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */
/** 20130316
	VMALLOC_START : VA(마지막 물리주소)의 다음 8M 단위 주소 
**/
/** 20130810
참고 : http://www.iamroot.org/xe/Kernel_7_ARM/25433	
**/
#define VMALLOC_OFFSET		(8*1024*1024)
/** 20140329    
 * high_memory 이후 VMALLOC_OFFSET로 정렬된 위치
 * Documentation/arm/memory.txt 참고
 **/
#define VMALLOC_START		(((unsigned long)high_memory + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1))
/** 20140329    
 * VMALLOC_END : (4GB - 16MB)
 **/
#define VMALLOC_END		0xff000000UL

#define LIBRARY_TEXT_START	0x0c000000

#ifndef __ASSEMBLY__
extern void __pte_error(const char *file, int line, pte_t);
extern void __pmd_error(const char *file, int line, pmd_t);
extern void __pgd_error(const char *file, int line, pgd_t);

#define pte_ERROR(pte)		__pte_error(__FILE__, __LINE__, pte)
#define pmd_ERROR(pmd)		__pmd_error(__FILE__, __LINE__, pmd)
#define pgd_ERROR(pgd)		__pgd_error(__FILE__, __LINE__, pgd)

/*
 * This is the lowest virtual address we can permit any user space
 * mapping to be mapped at.  This is particularly important for
 * non-high vector CPUs.
 */
#define FIRST_USER_ADDRESS	PAGE_SIZE

/*
 * The pgprot_* and protection_map entries will be fixed up in runtime
 * to include the cachable and bufferable bits based on memory policy,
 * as well as any architecture dependent bits like global/ASID and SMP
 * shared mapping bits.
 */
#define _L_PTE_DEFAULT	L_PTE_PRESENT | L_PTE_YOUNG

extern pgprot_t		pgprot_user;
extern pgprot_t		pgprot_kernel;

/** 20150523    
 * (p) | (b)
 **/
#define _MOD_PROT(p, b)	__pgprot(pgprot_val(p) | (b))

#define PAGE_NONE		_MOD_PROT(pgprot_user, L_PTE_XN | L_PTE_RDONLY)
#define PAGE_SHARED		_MOD_PROT(pgprot_user, L_PTE_USER | L_PTE_XN)
#define PAGE_SHARED_EXEC	_MOD_PROT(pgprot_user, L_PTE_USER)
#define PAGE_COPY		_MOD_PROT(pgprot_user, L_PTE_USER | L_PTE_RDONLY | L_PTE_XN)
#define PAGE_COPY_EXEC		_MOD_PROT(pgprot_user, L_PTE_USER | L_PTE_RDONLY)
#define PAGE_READONLY		_MOD_PROT(pgprot_user, L_PTE_USER | L_PTE_RDONLY | L_PTE_XN)
#define PAGE_READONLY_EXEC	_MOD_PROT(pgprot_user, L_PTE_USER | L_PTE_RDONLY)
/** 20130511 
 * pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
 *			     L_PTE_DIRTY | kern_pgprot);
 * #define L_PTE_XN		(_AT(pteval_t, 1) << 9)
 * 이러한 속성으로 페이지 속성을 설정.
 **/
#define PAGE_KERNEL		_MOD_PROT(pgprot_kernel, L_PTE_XN)
#define PAGE_KERNEL_EXEC	pgprot_kernel

#define __PAGE_NONE		__pgprot(_L_PTE_DEFAULT | L_PTE_RDONLY | L_PTE_XN)
#define __PAGE_SHARED		__pgprot(_L_PTE_DEFAULT | L_PTE_USER | L_PTE_XN)
#define __PAGE_SHARED_EXEC	__pgprot(_L_PTE_DEFAULT | L_PTE_USER)
#define __PAGE_COPY		__pgprot(_L_PTE_DEFAULT | L_PTE_USER | L_PTE_RDONLY | L_PTE_XN)
#define __PAGE_COPY_EXEC	__pgprot(_L_PTE_DEFAULT | L_PTE_USER | L_PTE_RDONLY)
#define __PAGE_READONLY		__pgprot(_L_PTE_DEFAULT | L_PTE_USER | L_PTE_RDONLY | L_PTE_XN)
#define __PAGE_READONLY_EXEC	__pgprot(_L_PTE_DEFAULT | L_PTE_USER | L_PTE_RDONLY)

/** 20151017    
 * 기존 protection 값을 복사해 mask부분을 날리고 bits으로 새로 정의한다.
 **/
#define __pgprot_modify(prot,mask,bits)		\
	__pgprot((pgprot_val(prot) & ~(mask)) | (bits))

#define pgprot_noncached(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_UNCACHED)

#define pgprot_writecombine(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_BUFFERABLE)

#define pgprot_stronglyordered(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_UNCACHED)

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
/** 20151017    
 * dmacoherent를 위한 pgprot 정의 매크로.
 * CONFIG_ARM_DMA_MEM_BUFFERABLE가 정의되어 있으므로 BUFFERABLE 속성이 추가된다.
 **/
#define pgprot_dmacoherent(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_BUFFERABLE | L_PTE_XN)
#define __HAVE_PHYS_MEM_ACCESS_PROT
struct file;
extern pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
				     unsigned long size, pgprot_t vma_prot);
#else
#define pgprot_dmacoherent(prot) \
	__pgprot_modify(prot, L_PTE_MT_MASK, L_PTE_MT_UNCACHED | L_PTE_XN)
#endif

#endif /* __ASSEMBLY__ */

/*
 * The table below defines the page protection levels that we insert into our
 * Linux page table version.  These get translated into the best that the
 * architecture can perform.  Note that on most ARM hardware:
 *  1) We cannot do execute protection
 *  2) If we could do execute protection, then read is implied
 *  3) write implies read permissions
 */
#define __P000  __PAGE_NONE
#define __P001  __PAGE_READONLY
#define __P010  __PAGE_COPY
#define __P011  __PAGE_COPY
#define __P100  __PAGE_READONLY_EXEC
#define __P101  __PAGE_READONLY_EXEC
#define __P110  __PAGE_COPY_EXEC
#define __P111  __PAGE_COPY_EXEC

#define __S000  __PAGE_NONE
#define __S001  __PAGE_READONLY
#define __S010  __PAGE_SHARED
#define __S011  __PAGE_SHARED
#define __S100  __PAGE_READONLY_EXEC
#define __S101  __PAGE_READONLY_EXEC
#define __S110  __PAGE_SHARED_EXEC
#define __S111  __PAGE_SHARED_EXEC

#ifndef __ASSEMBLY__
/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
/** 20151010    
 * ZERO_PAGE() 매크로는 항상empty_zero_page을 가리킨다.
 **/
extern struct page *empty_zero_page;
#define ZERO_PAGE(vaddr)	(empty_zero_page)


extern pgd_t swapper_pg_dir[PTRS_PER_PGD];

/* to find an entry in a page-table-directory */
/** 20130216
 * page table directory의 entry를 찾기 위한 인덱스를 구한다.
 * 2MB(PGDIR_SHIFT) 단위.
 * */
#define pgd_index(addr)		((addr) >> PGDIR_SHIFT)

/** 20130216
 * addr에 해당하는 page table entry의 주소를 구한다. 
 * */
#define pgd_offset(mm, addr)	((mm)->pgd + pgd_index(addr))

/* to find an entry in a kernel page-table-directory */
/** 20131019
 * init_mm이 가리키는 pgd에서 index만큼을 더한 주소를 리턴한다.
 *
 * 20140301
 * 커널의 virtual address를 가리키는 특정 pgd entry의 주소가 리턴된다.
 **/
#define pgd_offset_k(addr)	pgd_offset(&init_mm, addr)

/** 20130330    
 * pmd_val은 pmd값을 리턴.
 *
 * pmd_none은 pmd entry가 NULL일 때,
 * pmd_present는 pmd entry가 NULL이 아닐 때
 **/
#define pmd_none(pmd)		(!pmd_val(pmd))
#define pmd_present(pmd)	(pmd_val(pmd))

/** 20130309    
 * pmd entry에 들어있는 pte table의 주소(PA)에 대한 커널 VA를 리턴하는 함수
 **/
static inline pte_t *pmd_page_vaddr(pmd_t pmd)
{
	/** 20130309    
	 * pmd_val를 mask한 값을 취한다.
	 *   PHYS_MASK 0xffffffff
	 *   PAGE_MASK 0xfffff000
	 * pte를 할당할 때 PAGE_SIZE 단위로 align된 주소를 받아왔으므로
	 *   PAGE_MASK를 씌워도 pte 주소값이 손상되지 않음
	 *   (PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE)
	 **/
	return __va(pmd_val(pmd) & PHYS_MASK & (s32)PAGE_MASK);
}

/** 20140531    
 * pmd entry값이 가리키는(pte table 주소) 페이지 프레임에 대한
 * descriptor (struct page)의 주소를 리턴
 *
 * pmd_val(pmd) & PHYS_MASK	: pmd 의 값을 physical 주소로 mask.
 * __phys_to_pfn(x)			: pmd 의 값에 해당하는 page frame number (속성값은 사라짐)
 * pfn_to_page(x)			: pfn에 해당하는 page descriptor의 주소
 **/
#define pmd_page(pmd)		pfn_to_page(__phys_to_pfn(pmd_val(pmd) & PHYS_MASK))

#ifndef CONFIG_HIGHPTE
/** 20140531    
 * __pte_map	: pmd에 들어있는 pte table의 가상 주소를 구한다.
 * __pte_unmap	: NULL.
 **/
#define __pte_map(pmd)		pmd_page_vaddr(*(pmd))
#define __pte_unmap(pte)	do { } while (0)
#else
#define __pte_map(pmd)		(pte_t *)kmap_atomic(pmd_page(*(pmd)))
#define __pte_unmap(pte)	kunmap_atomic(pte)
#endif

/** 20130309    
 * addr을 가리키는  pte의 index를 구한다.
 **/
#define pte_index(addr)		(((addr) >> PAGE_SHIFT) & (PTRS_PER_PTE - 1))

/** 20130309    
 * addr에 대한 해당 pte entry 주소를 리턴
 *   pmd_page_vaddr(*(pmd)) : 해당 pmd가 가리키는 pte table의 주소를 추출
 *   pte_index             : pte table에서의 index
 **/
/** 20131019
* 해당 pmd에서 index(addr를 변환)에 해당되는 pte entry주소를 추출
 **/
#define pte_offset_kernel(pmd,addr)	(pmd_page_vaddr(*(pmd)) + pte_index(addr))

/** 20140531    
 * pte_offset_map	: pmd에서 addr에 해당하는 pte entry의 주소를 리턴한다.
 * pte_unmap		: pte entry unmap. CONFIG에 따라 NULL.
 **/
#define pte_offset_map(pmd,addr)	(__pte_map(pmd) + pte_index(addr))
#define pte_unmap(pte)			__pte_unmap(pte)

/** 20130309    
 * pte entry가 가리키는 물리주소의 pfn을 구한다.
 **/
#define pte_pfn(pte)		((pte_val(pte) & PHYS_MASK) >> PAGE_SHIFT)
/** 20130309    
 * 'pfn에 해당하는 물리주소 | 속성'으로 pte 데이터를 구한다.
 **/
#define pfn_pte(pfn,prot)	__pte(__pfn_to_phys(pfn) | pgprot_val(prot))

/** 20131026    
 * pte -> pfn -> page로 struct page *를 가져온다.
 **/
#define pte_page(pte)		pfn_to_page(pte_pfn(pte))
/** 20131019
* page를 pfn으로 변환 후, prot과 OR하여 얻어진 값으로 pte 데이터를 구하고 리턴함
 **/
#define mk_pte(page,prot)	pfn_pte(page_to_pfn(page), prot)

/** 20131026    
 * pte entry를 주소 0, 속성 0으로 채워 clear 시킨다.
 **/
#define pte_clear(mm,addr,ptep)	set_pte_ext(ptep, __pte(0), 0)

#if __LINUX_ARM_ARCH__ < 6
static inline void __sync_icache_dcache(pte_t pteval)
{
}
#else
extern void __sync_icache_dcache(pte_t pteval);
#endif

/** 20131109
 * addr가 커널영역이면 ptep에 pte만 채우고, 
 * 유저영역이면 pte에 해당되는 영역을 flush하고 
 * flag에 PTE_EXT_NG를 세팅한다.
 **/
static inline void set_pte_at(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, pte_t pteval)
{
	/** 20131102    
	 * addr가 TASK_SIZE 이후일 경우 
	 * set_pte_ext로 ptep가 가리키는 pte entry의 값을 pteval로 채움.
	 * hw 속성은 0.
	 **/
	if (addr >= TASK_SIZE)
		set_pte_ext(ptep, pteval, 0);
	/** 20131102    
	 * user space address라면 icache, dcache를 sync해주고
	 * ptep가 가리키는 pte entry의 값을 pteval로 채움
	 * PTE_EXT_NG : 특정 process용으로 사용되는 page를 의미함 (user 영역)
	 **/
	else {
		__sync_icache_dcache(pteval);
		set_pte_ext(ptep, pteval, PTE_EXT_NG);
	}
}

/** 20131026    
 * pte - pte entry의 주소
 *
 * pte_none	: pte entry가 비어 있다.
 * pte_present	: pte entry에 값이 들어 있고, 매핑된 페이지가 메모리에 존재한다.
 **/
#define pte_none(pte)		(!pte_val(pte))
#define pte_present(pte)	(pte_val(pte) & L_PTE_PRESENT)
#define pte_write(pte)		(!(pte_val(pte) & L_PTE_RDONLY))
#define pte_dirty(pte)		(pte_val(pte) & L_PTE_DIRTY)
#define pte_young(pte)		(pte_val(pte) & L_PTE_YOUNG)
/** 20131102    
 * pte의 속성에 L_PTE_XN이면 실행할 수 없는 address range.
 * 그렇지 않을 경우 true.
 **/
#define pte_exec(pte)		(!(pte_val(pte) & L_PTE_XN))
#define pte_special(pte)	(0)

/** 20131102    
 * pte는 linux용 pte value.
 * pteval에 L_PTE_PRESENT 또는 L_PTE_USER이 지정되어 있는지 검사
 **/
#define pte_present_user(pte) \
	((pte_val(pte) & (L_PTE_PRESENT | L_PTE_USER)) == \
	 (L_PTE_PRESENT | L_PTE_USER))

/** 20140531    
 * pte bit 조작 함수 생성 매크로.
 *	- pte_wrprotect ; copy-on-write시 사용
 *	- pte_mkold 등
 **/
#define PTE_BIT_FUNC(fn,op) \
static inline pte_t pte_##fn(pte_t pte) { pte_val(pte) op; return pte; }

PTE_BIT_FUNC(wrprotect, |= L_PTE_RDONLY);
PTE_BIT_FUNC(mkwrite,   &= ~L_PTE_RDONLY);
PTE_BIT_FUNC(mkclean,   &= ~L_PTE_DIRTY);
PTE_BIT_FUNC(mkdirty,   |= L_PTE_DIRTY);
PTE_BIT_FUNC(mkold,     &= ~L_PTE_YOUNG);
PTE_BIT_FUNC(mkyoung,   |= L_PTE_YOUNG);

static inline pte_t pte_mkspecial(pte_t pte) { return pte; }

static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	const pteval_t mask = L_PTE_XN | L_PTE_RDONLY | L_PTE_USER;
	pte_val(pte) = (pte_val(pte) & ~mask) | (pgprot_val(newprot) & mask);
	return pte;
}

/*
 * Encode and decode a swap entry.  Swap entries are stored in the Linux
 * page tables as follows:
 *
 *   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
 *   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *   <--------------- offset --------------------> <- type --> 0 0 0
 *
 * This gives us up to 63 swap files and 32GB per swap file.  Note that
 * the offset field is always non-zero.
 */
#define __SWP_TYPE_SHIFT	3
#define __SWP_TYPE_BITS		6
#define __SWP_TYPE_MASK		((1 << __SWP_TYPE_BITS) - 1)
#define __SWP_OFFSET_SHIFT	(__SWP_TYPE_BITS + __SWP_TYPE_SHIFT)

/** 20140607    
 * swap type과 offset을 추출해 swap entry를 생성한다.
 **/
#define __swp_type(x)		(((x).val >> __SWP_TYPE_SHIFT) & __SWP_TYPE_MASK)
#define __swp_offset(x)		((x).val >> __SWP_OFFSET_SHIFT)
#define __swp_entry(type,offset) ((swp_entry_t) { ((type) << __SWP_TYPE_SHIFT) | ((offset) << __SWP_OFFSET_SHIFT) })

#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(swp)	((pte_t) { (swp).val })

/*
 * It is an error for the kernel to have more swap files than we can
 * encode in the PTEs.  This ensures that we know when MAX_SWAPFILES
 * is increased beyond what we presently support.
 */
#define MAX_SWAPFILES_CHECK() BUILD_BUG_ON(MAX_SWAPFILES_SHIFT > __SWP_TYPE_BITS)

/*
 * Encode and decode a file entry.  File entries are stored in the Linux
 * page tables as follows:
 *
 *   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
 *   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *   <----------------------- offset ------------------------> 1 0 0
 */
#define pte_file(pte)		(pte_val(pte) & L_PTE_FILE)
#define pte_to_pgoff(x)		(pte_val(x) >> 3)
#define pgoff_to_pte(x)		__pte(((x) << 3) | L_PTE_FILE)

#define PTE_FILE_MAX_BITS	29

/* Needs to be defined here and not in linux/mm.h, as it is arch dependent */
/* FIXME: this is not correct */
#define kern_addr_valid(addr)	(1)

#include <asm-generic/pgtable.h>

/*
 * We provide our own arch_get_unmapped_area to cope with VIPT caches.
 */
#define HAVE_ARCH_UNMAPPED_AREA
#define HAVE_ARCH_UNMAPPED_AREA_TOPDOWN

/*
 * remap a physical page `pfn' of size `size' with page protection `prot'
 * into virtual address `from'
 */
#define io_remap_pfn_range(vma,from,pfn,size,prot) \
		remap_pfn_range(vma, from, pfn, size, prot)

#define pgtable_cache_init() do { } while (0)

#endif /* !__ASSEMBLY__ */

#endif /* CONFIG_MMU */

#endif /* _ASMARM_PGTABLE_H */
