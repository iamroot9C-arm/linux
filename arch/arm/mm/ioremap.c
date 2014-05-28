/*
 *  linux/arch/arm/mm/ioremap.c
 *
 * Re-map IO memory to kernel address space so that we can access it.
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 *
 * Hacked for ARM by Phil Blundell <philb@gnu.org>
 * Hacked to allow all architectures to build, and various cleanups
 * by Russell King
 *
 * This allows a driver to remap an arbitrary region of bus memory into
 * virtual space.  One should *only* use readl, writel, memcpy_toio and
 * so on with such remapped areas.
 *
 * Because the ARM only has a 32-bit address space we can't address the
 * whole of the (physical) PCI space at once.  PCI huge-mode addressing
 * allows us to circumvent this restriction by splitting PCI space into
 * two 2GB chunks and mapping only one at a time into processor memory.
 * We use MMU protection domains to trap any attempt to access the bank
 * that is not currently mapped.  (This isn't fully implemented yet.)
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/system_info.h>

#include <asm/mach/map.h>
#include "mm.h"

int ioremap_page(unsigned long virt, unsigned long phys,
		 const struct mem_type *mtype)
{
	return ioremap_page_range(virt, virt + PAGE_SIZE, phys,
				  __pgprot(mtype->prot_pte));
}
EXPORT_SYMBOL(ioremap_page);

void __check_kvm_seq(struct mm_struct *mm)
{
	unsigned int seq;

	do {
		seq = init_mm.context.kvm_seq;
		memcpy(pgd_offset(mm, VMALLOC_START),
		       pgd_offset_k(VMALLOC_START),
		       sizeof(pgd_t) * (pgd_index(VMALLOC_END) -
					pgd_index(VMALLOC_START)));
		mm->context.kvm_seq = seq;
	} while (seq != init_mm.context.kvm_seq);
}

#if !defined(CONFIG_SMP) && !defined(CONFIG_ARM_LPAE)
/*
 * Section support is unsafe on SMP - If you iounmap and ioremap a region,
 * the other CPUs will not see this change until their next context switch.
 * Meanwhile, (eg) if an interrupt comes in on one of those other CPUs
 * which requires the new ioremap'd region to be referenced, the CPU will
 * reference the _old_ region.
 *
 * Note that get_vm_area_caller() allocates a guard 4K page, so we need to
 * mask the size back to 1MB aligned or we will overflow in the loop below.
 */
static void unmap_area_sections(unsigned long virt, unsigned long size)
{
	unsigned long addr = virt, end = virt + (size & ~(SZ_1M - 1));
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmdp;

	flush_cache_vunmap(addr, end);
	pgd = pgd_offset_k(addr);
	pud = pud_offset(pgd, addr);
	pmdp = pmd_offset(pud, addr);
	do {
		pmd_t pmd = *pmdp;

		if (!pmd_none(pmd)) {
			/*
			 * Clear the PMD from the page table, and
			 * increment the kvm sequence so others
			 * notice this change.
			 *
			 * Note: this is still racy on SMP machines.
			 */
			pmd_clear(pmdp);
			init_mm.context.kvm_seq++;

			/*
			 * Free the page table, if there was one.
			 */
			if ((pmd_val(pmd) & PMD_TYPE_MASK) == PMD_TYPE_TABLE)
				pte_free_kernel(&init_mm, pmd_page_vaddr(pmd));
		}

		addr += PMD_SIZE;
		pmdp += 2;
	} while (addr < end);

	/*
	 * Ensure that the active_mm is up to date - we want to
	 * catch any use-after-iounmap cases.
	 */
	if (current->active_mm->context.kvm_seq != init_mm.context.kvm_seq)
		__check_kvm_seq(current->active_mm);

	flush_tlb_kernel_range(virt, end);
}

static int
remap_area_sections(unsigned long virt, unsigned long pfn,
		    size_t size, const struct mem_type *type)
{
	unsigned long addr = virt, end = virt + size;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	/*
	 * Remove and free any PTE-based mapping, and
	 * sync the current kernel mapping.
	 */
	unmap_area_sections(virt, size);

	pgd = pgd_offset_k(addr);
	pud = pud_offset(pgd, addr);
	pmd = pmd_offset(pud, addr);
	do {
		pmd[0] = __pmd(__pfn_to_phys(pfn) | type->prot_sect);
		pfn += SZ_1M >> PAGE_SHIFT;
		pmd[1] = __pmd(__pfn_to_phys(pfn) | type->prot_sect);
		pfn += SZ_1M >> PAGE_SHIFT;
		flush_pmd_entry(pmd);

		addr += PMD_SIZE;
		pmd += 2;
	} while (addr < end);

	return 0;
}

static int
remap_area_supersections(unsigned long virt, unsigned long pfn,
			 size_t size, const struct mem_type *type)
{
	unsigned long addr = virt, end = virt + size;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	/*
	 * Remove and free any PTE-based mapping, and
	 * sync the current kernel mapping.
	 */
	unmap_area_sections(virt, size);

	pgd = pgd_offset_k(virt);
	pud = pud_offset(pgd, addr);
	pmd = pmd_offset(pud, addr);
	do {
		unsigned long super_pmd_val, i;

		super_pmd_val = __pfn_to_phys(pfn) | type->prot_sect |
				PMD_SECT_SUPER;
		super_pmd_val |= ((pfn >> (32 - PAGE_SHIFT)) & 0xf) << 20;

		for (i = 0; i < 8; i++) {
			pmd[0] = __pmd(super_pmd_val);
			pmd[1] = __pmd(super_pmd_val);
			flush_pmd_entry(pmd);

			addr += PMD_SIZE;
			pmd += 2;
		}

		pfn += SUPERSECTION_SIZE >> PAGE_SHIFT;
	} while (addr < end);

	return 0;
}
#endif

/** 20140419    
 * page frame을 mapping할 vm_struct, vmap_area를 할당 받고 (VA),
 * page table에 MT_DEVICE에 해당하는 속성으로 등록시키는 함수
 **/
void __iomem * __arm_ioremap_pfn_caller(unsigned long pfn,
	unsigned long offset, size_t size, unsigned int mtype, void *caller)
{
	const struct mem_type *type;
	int err;
	unsigned long addr;
 	struct vm_struct * area;

#ifndef CONFIG_ARM_LPAE
	/*
	 * High mappings must be supersection aligned
	 */
	/** 20130323
	*	pfn이 1M 보다 크고(4G 이상이고),물리주소가 16M 단위로 정렬되어 있지 않으면 널로 리턴.
	*/
	if (pfn >= 0x100000 && (__pfn_to_phys(pfn) & ~SUPERSECTION_MASK))
		return NULL;
#endif
	/** 20130323
	* ioremap은 mtype이 MT_DEVICE. 이에 해당하는 mem_type을 리턴
	* mem_types에서 mtype을 index로 조회
	*/
	type = get_mem_type(mtype);
	if (!type)
		return NULL;

	/*
	 * Page align the mapping size, taking account of any offset.
	 */
	/** 20140419    
	 * off과 마찬가지로 size 역시 page align
	 **/
	size = PAGE_ALIGN(offset + size);

	/*
	 * Try to reuse one of the static mapping whenever possible.
	 */
	read_lock(&vmlist_lock);
	/** 20140419    
	 * vmlist에 등록된 vm_struct 중 STATIC MAPPING은
	 * 매핑할 VA를 이미 가지고 있다.
	 * (architecture 초기화 과정에서 iotable_init 호출)
	 *
	 * 이 주소를 찾아 리턴한다.
	 **/
	for (area = vmlist; area; area = area->next) {
		if (!size || (sizeof(phys_addr_t) == 4 && pfn >= 0x100000))
			break;
		/** 20140419    
		 * VM_ARM_STATIC_MAPPING 매핑인 경우 vmlist의 VA를 리턴한다.
		 * 
		 * vexpress의 경우,
		 * mach-vexpress/v2m.c, platsmp.c의 장치가 등록되어 있다.
		 **/
		if (!(area->flags & VM_ARM_STATIC_MAPPING))
			continue;
		if ((area->flags & VM_ARM_MTYPE_MASK) != VM_ARM_MTYPE(mtype))
			continue;
		/** 20130518    
		 * vmlist에서 가져온 entry의 phys_addr (start) 보다 작거나
		 *                                     (end)보다 크면
		 * vmlist에서 다음 entry를 찾음 (continue)
		 **/
		if (__phys_to_pfn(area->phys_addr) > pfn ||
		    __pfn_to_phys(pfn) + size-1 > area->phys_addr + area->size-1)
			continue;
		/* we can drop the lock here as we know *area is static */
		/** 20130518    
		 * 새로운 pfn이 vmlist에 등록한 entry의 주소 범위 내에 있으면
		 * offset을 map_desc의 .virtual에 더해 리턴. (vexpress의 경우 V2T_PERIPH)
		 **/
		read_unlock(&vmlist_lock);
		addr = (unsigned long)area->addr;
		addr += __pfn_to_phys(pfn) - area->phys_addr;
		return (void __iomem *) (offset + addr);
	}
	read_unlock(&vmlist_lock);

/** 20130323
*	이하는 다음 진입 시 분석 (SLUB ....)
*	20130518
*	vmlist에 등록한 entry의 주소 범위 밖에 있는 경우 아래 루틴 수행.
*/

	/*
	 * Don't allow RAM to be mapped - this causes problems with ARMv6+
	 */
	/** 20130518    
	 * pfn이 물리 메모리 영역에 속한다면 WARN()을 호출하고 NULL을 리턴.
	 **/
	if (WARN_ON(pfn_valid(pfn)))
		return NULL;

	/** 20140419    
	 * vm_struct와 vmap_area(VA 할당을 의미)를 받아온다.
	 **/
	area = get_vm_area_caller(size, VM_IOREMAP, caller);
 	if (!area)
 		return NULL;
	/** 20140419    
	 * vm_struct의 가상 주소 할당
	 **/
 	addr = (unsigned long)area->addr;

#if !defined(CONFIG_SMP) && !defined(CONFIG_ARM_LPAE)
	if (DOMAIN_IO == 0 &&
	    (((cpu_architecture() >= CPU_ARCH_ARMv6) && (get_cr() & CR_XP)) ||
	       cpu_is_xsc3()) && pfn >= 0x100000 &&
	       !((__pfn_to_phys(pfn) | size | addr) & ~SUPERSECTION_MASK)) {
		area->flags |= VM_ARM_SECTION_MAPPING;
		err = remap_area_supersections(addr, pfn, size, type);
	} else if (!((__pfn_to_phys(pfn) | size | addr) & ~PMD_MASK)) {
		area->flags |= VM_ARM_SECTION_MAPPING;
		err = remap_area_sections(addr, pfn, size, type);
	} else
#endif
		/** 20140419    
		 * 할당받은 VA와 PA를 매핑 (page table에 등록)
		 * prot는 MT_DEVICE type에 대한 protection 설정.
		 **/
		err = ioremap_page_range(addr, addr + size, __pfn_to_phys(pfn),
					 __pgprot(type->prot_pte));

	if (err) {
 		vunmap((void *)addr);
 		return NULL;
 	}

	/** 20140419    
	 * addr ~ addr + size에 대한 cache flush
	 * if/else에 따라 ioremap_page_range가 호출되지 않았다면 cache flush 되지 않을 수 있음
	 **/
	flush_cache_vmap(addr, addr + size);
	/** 20140419    
	 * page단위로 mapping 하였으므로 PA의 정렬되지 않은 주소에 
	 * 매핑한 addr(PA)을 더해 리턴
	 **/
	return (void __iomem *) (offset + addr);
}

/** 20140419    
 * architecture specific ioremap 함수.
 * pfn을 매핑할 VA를 할당 받아 page table에 매핑한다.
 **/
void __iomem *__arm_ioremap_caller(unsigned long phys_addr, size_t size,
	unsigned int mtype, void *caller)
{
	unsigned long last_addr;
	/** 20140419    
	 * offset은 phys_addr의 page의 정렬되지 않은 주소
	 **/
 	unsigned long offset = phys_addr & ~PAGE_MASK;
	/** 20140419    
	 * phys_addr로 pfn 을 구함
	 **/
 	unsigned long pfn = __phys_to_pfn(phys_addr);

 	/*
 	 * Don't allow wraparound or zero size
	 */
	/** 20140419    
	 * last_addr에 대한 체크
	 **/
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr)
		return NULL;

	return __arm_ioremap_pfn_caller(pfn, offset, size, mtype,
			caller);
}

/*
 * Remap an arbitrary physical address space into the kernel virtual
 * address space. Needed when the kernel wants to access high addresses
 * directly.
 *
 * NOTE! We need to allow non-page-aligned mappings too: we will obviously
 * have to convert them into an offset in a page-aligned mapping, but the
 * caller shouldn't need to know that small detail.
 */
void __iomem *
__arm_ioremap_pfn(unsigned long pfn, unsigned long offset, size_t size,
		  unsigned int mtype)
{
	return __arm_ioremap_pfn_caller(pfn, offset, size, mtype,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(__arm_ioremap_pfn);

/** 20140419    
 * func pointer. __arm_ioremap_caller를 지정
 **/
void __iomem * (*arch_ioremap_caller)(unsigned long, size_t,
				      unsigned int, void *) =
	__arm_ioremap_caller;

/** 20130323
 *  arch_ioremap_caller 일부 분석
 *
 *  20140419
 *  ioremap 부분 분석 완료.
 *  caller는 vm_struct에 등록된다.
 *
 *  phys_addr를 매핑할 VA를 할당 받아 page table에 매핑한다.
 */
void __iomem *
__arm_ioremap(unsigned long phys_addr, size_t size, unsigned int mtype)
{
	return arch_ioremap_caller(phys_addr, size, mtype,
		__builtin_return_address(0));
}
EXPORT_SYMBOL(__arm_ioremap);

/*
 * Remap an arbitrary physical address space into the kernel virtual
 * address space as memory. Needed when the kernel wants to execute
 * code in external memory. This is needed for reprogramming source
 * clocks that would affect normal memory for example. Please see
 * CONFIG_GENERIC_ALLOCATOR for allocating external memory.
 */
void __iomem *
__arm_ioremap_exec(unsigned long phys_addr, size_t size, bool cached)
{
	unsigned int mtype;

	if (cached)
		mtype = MT_MEMORY;
	else
		mtype = MT_MEMORY_NONCACHED;

	return __arm_ioremap_caller(phys_addr, size, mtype,
			__builtin_return_address(0));
}

void __iounmap(volatile void __iomem *io_addr)
{
	void *addr = (void *)(PAGE_MASK & (unsigned long)io_addr);
	struct vm_struct *vm;

	read_lock(&vmlist_lock);
	for (vm = vmlist; vm; vm = vm->next) {
		if (vm->addr > addr)
			break;
		if (!(vm->flags & VM_IOREMAP))
			continue;
		/* If this is a static mapping we must leave it alone */
		if ((vm->flags & VM_ARM_STATIC_MAPPING) &&
		    (vm->addr <= addr) && (vm->addr + vm->size > addr)) {
			read_unlock(&vmlist_lock);
			return;
		}
#if !defined(CONFIG_SMP) && !defined(CONFIG_ARM_LPAE)
		/*
		 * If this is a section based mapping we need to handle it
		 * specially as the VM subsystem does not know how to handle
		 * such a beast.
		 */
		if ((vm->addr == addr) &&
		    (vm->flags & VM_ARM_SECTION_MAPPING)) {
			unmap_area_sections((unsigned long)vm->addr, vm->size);
			break;
		}
#endif
	}
	read_unlock(&vmlist_lock);

	vunmap(addr);
}

void (*arch_iounmap)(volatile void __iomem *) = __iounmap;

void __arm_iounmap(volatile void __iomem *io_addr)
{
	arch_iounmap(io_addr);
}
EXPORT_SYMBOL(__arm_iounmap);
