#include <linux/kernel.h>

#include <asm/cputype.h>
#include <asm/idmap.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/sections.h>
#include <asm/system_info.h>


/** 20150620
 * 커널 이미지의 밖을 접근한다. 
 * Note: accesses outside of the kernel image and the identity map area
 * are not supported on any CPU using the idmap tables as its current
 * page tables.
 **/
pgd_t *idmap_pgd;

#ifdef CONFIG_ARM_LPAE
static void idmap_add_pmd(pud_t *pud, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pmd_t *pmd;
	unsigned long next;

	if (pud_none_or_clear_bad(pud) || (pud_val(*pud) & L_PGD_SWAPPER)) {
		pmd = pmd_alloc_one(&init_mm, addr);
		if (!pmd) {
			pr_warning("Failed to allocate identity pmd.\n");
			return;
		}
		pud_populate(&init_mm, pud, pmd);
		pmd += pmd_index(addr);
	} else
		pmd = pmd_offset(pud, addr);

	do {
		next = pmd_addr_end(addr, end);
		*pmd = __pmd((addr & PMD_MASK) | prot);
		flush_pmd_entry(pmd);
	} while (pmd++, addr = next, addr != end);
}
#else	/* !CONFIG_ARM_LPAE */
static void idmap_add_pmd(pud_t *pud, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pmd_t *pmd = pmd_offset(pud, addr);

	addr = (addr & PMD_MASK) | prot;
	pmd[0] = __pmd(addr);
	addr += SECTION_SIZE;
	pmd[1] = __pmd(addr);
	flush_pmd_entry(pmd);
}
#endif	/* CONFIG_ARM_LPAE */

static void idmap_add_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pud_t *pud = pud_offset(pgd, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		idmap_add_pmd(pud, addr, next, prot);
	} while (pud++, addr = next, addr != end);
}

/** 20150620    
 * addr ~ end 영역을 pgd 에 매핑시킨다.
 **/
static void identity_mapping_add(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	unsigned long prot, next;

	/** 20150620    
	 * SECTION, Read/Write Access 속성을 지정한다.
	 **/
	prot = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AF;
	/** 20150620    
	 * cpu_architecture는 ARMv7이다.
	 **/
	if (cpu_architecture() <= CPU_ARCH_ARMv5TEJ && !cpu_is_xscale())
		prot |= PMD_BIT4;

	/** 20150620    
	 * addr부터 end까지 영역을 mapping 시킨다. 
	 *
	 * pgd_index에 해당하는 address는 보통 가상메모리를 주고, 그에 해당하는
	 * entry의 위치를 찾아오기 위함인데, 여기서 넘어온 address는 physical이다.
	 * 물리주소를 가상주소와 동일하게 매핑하기 위한 것이다.
	 **/
	pgd += pgd_index(addr);
	do {
		next = pgd_addr_end(addr, end);
		idmap_add_pud(pgd, addr, next, prot);
	} while (pgd++, addr = next, addr != end);
}

/** 20150620    
 * section의 시작과 끝.
 *
 * __idmap으로 section을 지정하면 이 영역에 위치시킬 수 있다.
 * 또는 assembly에서 .pushsection .idmap.text로 직접 지정한다.
 **/
extern char  __idmap_text_start[], __idmap_text_end[];

/** 20150613    
 * idmap page table을 위한 초기화를 한다.
 *
 * page table을 새로 할당 받고, 기존의 page table에서 커널 영역에 해당하는 부분을 복사한다.
 * idmap_start ~ idmap_end 사이의 영역에 대해 page table에 mapping 시킨다.
 **/
static int __init init_static_idmap(void)
{
	phys_addr_t idmap_start, idmap_end;

	/** 20150620    
	 * idmap을 위한 L1 page table을 할당받는다.
	 **/
	idmap_pgd = pgd_alloc(&init_mm);
	if (!idmap_pgd)
		return -ENOMEM;

	/* Add an identity mapping for the physical address of the section. */
	/** 20150620    
	 * idmap 영역을 identity mapping으로 추가하기 위해 물리 주소를 받아온다.
	 **/
	idmap_start = virt_to_phys((void *)__idmap_text_start);
	idmap_end = virt_to_phys((void *)__idmap_text_end);

	pr_info("Setting up static identity map for 0x%llx - 0x%llx\n",
		(long long)idmap_start, (long long)idmap_end);
	/** 20150620    
	 * idmap_pgd에 idmap_start ~ idmap_end 영역에 대한 mapping을 추가한다.
	 * 물리주소와 가상주소를 동일하게 매핑시킨다.
	 * 
	 * kernel 빌드 후 System.map을 확인하면 __idmap_text_start ~ __idmap_text_end
	 * 사이의 심볼을 확인할 수 있다.
	 **/
	identity_mapping_add(idmap_pgd, idmap_start, idmap_end);

	return 0;
}
early_initcall(init_static_idmap);

/*
 * In order to soft-boot, we need to switch to a 1:1 mapping for the
 * cpu_reset functions. This will then ensure that we have predictable
 * results when turning off the mmu.
 */
void setup_mm_for_reboot(void)
{
	/* Clean and invalidate L1. */
	flush_cache_all();

	/* Switch to the identity mapping. */
	cpu_switch_mm(idmap_pgd, &init_mm);

	/* Flush the TLB. */
	local_flush_tlb_all();
}
