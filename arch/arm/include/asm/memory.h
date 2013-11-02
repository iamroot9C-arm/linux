/*
 *  arch/arm/include/asm/memory.h
 *
 *  Copyright (C) 2000-2002 Russell King
 *  modification for nommu, Hyok S. Choi, 2004
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Note: this file should not be included by non-asm/.h files
 */
#ifndef __ASM_ARM_MEMORY_H
#define __ASM_ARM_MEMORY_H

#include <linux/compiler.h>
#include <linux/const.h>
#include <linux/types.h>
#include <linux/sizes.h>

#ifdef CONFIG_NEED_MACH_MEMORY_H
#include <mach/memory.h>
#endif

/*
 * Allow for constants defined here to be used from assembly code
 * by prepending the UL suffix only with actual C code compilation.
 */
#define UL(x) _AC(x, UL)

#ifdef CONFIG_MMU

/*
 * PAGE_OFFSET - the virtual address of the start of the kernel image
 * TASK_SIZE - the maximum size of a user space task.
 * TASK_UNMAPPED_BASE - the lower boundary of the mmap VM area
 */
#define PAGE_OFFSET		UL(CONFIG_PAGE_OFFSET)
/** 20131102    
 * TASK_SIZE는 user space mapping 주소 다음 위치.
 * Documentation/arm/memory.txt 를 참고
 **/
#define TASK_SIZE		(UL(CONFIG_PAGE_OFFSET) - UL(0x01000000))
#define TASK_UNMAPPED_BASE	(UL(CONFIG_PAGE_OFFSET) / 3)

/*
 * The maximum size of a 26-bit user space task.
 */
#define TASK_SIZE_26		UL(0x04000000)

/*
 * The module space lives between the addresses given by TASK_SIZE
 * and PAGE_OFFSET - it must be within 32MB of the kernel text.
 */
/** 20130810
vexpress 에서는 TASK_SIZE는 PAGE_OFFSET에서 16MB를 빼준값이므로  위치는 MODULES_VADDR과 같다.
여기서 MODULES_VADDR이 의미하는 바는???
**/
#ifndef CONFIG_THUMB2_KERNEL
#define MODULES_VADDR		(PAGE_OFFSET - 16*1024*1024)
#else
/* smaller range for Thumb-2 symbols relocation (2^24)*/
#define MODULES_VADDR		(PAGE_OFFSET - 8*1024*1024)
#endif

#if TASK_SIZE > MODULES_VADDR
#error Top of user space clashes with start of module space
#endif

/*
 * The highmem pkmap virtual space shares the end of the module area.
 */
#ifdef CONFIG_HIGHMEM
/** 20130824    
 * HIGHMEM일 경우 PKMAP_BASE 영역과 겹치지 않도록 하기 위해 PMD_SIZE를 빼준다
 **/
#define MODULES_END		(PAGE_OFFSET - PMD_SIZE)
#else
#define MODULES_END		(PAGE_OFFSET)
#endif

/*
 * The XIP kernel gets mapped at the bottom of the module vm area.
 * Since we use sections to map it, this macro replaces the physical address
 * with its virtual address while keeping offset from the base section.
 */
#define XIP_VIRT_ADDR(physaddr)  (MODULES_VADDR + ((physaddr) & 0x000fffff))

/*
 * Allow 16MB-aligned ioremap pages
 */
#define IOREMAP_MAX_ORDER	24

#define CONSISTENT_END		(0xffe00000UL)

#else /* CONFIG_MMU */

/*
 * The limitation of user task size can grow up to the end of free ram region.
 * It is difficult to define and perhaps will never meet the original meaning
 * of this define that was meant to.
 * Fortunately, there is no reference for this in noMMU mode, for now.
 */
#ifndef TASK_SIZE
#define TASK_SIZE		(CONFIG_DRAM_SIZE)
#endif

#ifndef TASK_UNMAPPED_BASE
#define TASK_UNMAPPED_BASE	UL(0x00000000)
#endif

#ifndef PHYS_OFFSET
#define PHYS_OFFSET 		UL(CONFIG_DRAM_BASE)
#endif

#ifndef END_MEM
#define END_MEM     		(UL(CONFIG_DRAM_BASE) + CONFIG_DRAM_SIZE)
#endif

#ifndef PAGE_OFFSET
#define PAGE_OFFSET		(PHYS_OFFSET)
#endif

/*
 * The module can be at any place in ram in nommu mode.
 */
#define MODULES_END		(END_MEM)
#define MODULES_VADDR		(PHYS_OFFSET)

#define XIP_VIRT_ADDR(physaddr)  (physaddr)

#endif /* !CONFIG_MMU */

/*
 * We fix the TCM memories max 32 KiB ITCM resp DTCM at these
 * locations
 */
#ifdef CONFIG_HAVE_TCM
#define ITCM_OFFSET	UL(0xfffe0000)
#define DTCM_OFFSET	UL(0xfffe8000)
#endif

/*
 * Convert a physical address to a Page Frame Number and back
 */
/** 20130112
	pfn : page frame number
	__phys_to_pfn 물리주소에 대한 페이지 프레임 인덱스를 구한다.
	__pfn_to_phys 페이지 프레임 인덱스에 대한 물리 주소를 구한다.
**/
#define	__phys_to_pfn(paddr)	((unsigned long)((paddr) >> PAGE_SHIFT))
#define	__pfn_to_phys(pfn)	((phys_addr_t)(pfn) << PAGE_SHIFT)

/*
 * Convert a page to/from a physical address
 */
#define page_to_phys(page)	(__pfn_to_phys(page_to_pfn(page)))
#define phys_to_page(phys)	(pfn_to_page(__phys_to_pfn(phys)))

#ifndef __ASSEMBLY__

/*
 * Physical vs virtual RAM address space conversion.  These are
 * private definitions which should NOT be used outside memory.h
 * files.  Use virt_to_phys/phys_to_virt/__pa/__va instead.
 */
#ifndef __virt_to_phys
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT

/*
 * Constants used to force the right instruction encodings and shifts
 * so that all we need to do is modify the 8-bit constant field.
 */
#define __PV_BITS_31_24	0x81000000

extern unsigned long __pv_phys_offset;
#define PHYS_OFFSET __pv_phys_offset

/** 20120922
	현재 섹션 정보를 섹션스택에 저장하고 .pv_table섹션에 .long타입의 
	데이터(어드레스정보)를 저장후 섹션스택을 다시 Pop한다
	(%0 : output, %1,%2 : input)
**/
/** 20121006
     인라인 변환 예 : add t,x,__PV_BITS_31_24
	 __virt_to_phys 실행시 
	inst 는 text 섹션 어딘가에 저장이 되지만
	pv_table은 inst의 주소만을 가지고 있다.

	vmlinux dump
		283800 Contents of section .init.pv_table:
 		283801  8045cd04 24b54380 18b74380 f4f00080 <b8de4380>  $.C...C.......C.	

		b8de4380 -> 8043deb8
		5755429 static inline unsigned long __phys_to_virt(unsigned long x)
		5755430 <8043deb8>:   e2455481    sub r5, r5, #-2130706432    ; 0x81000000
		5755431 8043debc:   ea000003    b   8043ded0 <setup_arch+0x388>

**/
/** 20121215
	__PV_BITS_31_24 는 __fixup_pv_table 에서 계산된 offset으로 변경됨.
	1. inline assembly 함수로 선언했기 때문에
		virt_to_phys(), phys_to_virt()를 호출한 부분마다 다음 코드가 삽입된다.
	2. instruction은 함수를 호출한 부분마다 들어가고, 각 instruction의 주소는
		.pv_table section에 일괄 저장된다.
	3. 이렇게 하는 이유는 offset 값을 memory에 넣고 add, sub를 하기 위해서는 load 과정이 필요한데,
		부팅시에 이 과정을 한 번에 수정해 수행속도의 향상을 얻기 위함이다.
	20130126
	4. P <-> V간 address 변환을 한 사이클로 수행해 속도 향상 뿐만 아니라 atomic 연산의 효과도 얻을 수 있을듯 ???
	5. add, sub의 instruction encoding을 보면 8비트만 immediate 로 사용해야 하나의 instruction으로 처리된다.
 **/
#define __pv_stub(from,to,instr,type)			\
	__asm__("@ __pv_stub\n"				\
	"1:	" instr "	%0, %1, %2\n"		\
	"	.pushsection .pv_table,\"a\"\n"		\
	"	.long	1b\n"				\
	"	.popsection\n"				\
	: "=r" (to)					\
	: "r" (from), "I" (type))

static inline unsigned long __virt_to_phys(unsigned long x)
{
	unsigned long t;
	__pv_stub(x, t, "add", __PV_BITS_31_24);
	return t;
}

static inline unsigned long __phys_to_virt(unsigned long x)
{
	unsigned long t;
	__pv_stub(x, t, "sub", __PV_BITS_31_24);
	return t;
}
#else
#define __virt_to_phys(x)	((x) - PAGE_OFFSET + PHYS_OFFSET)
#define __phys_to_virt(x)	((x) - PHYS_OFFSET + PAGE_OFFSET)
#endif
#endif

#ifndef PHYS_OFFSET
#ifdef PLAT_PHYS_OFFSET
#define PHYS_OFFSET	PLAT_PHYS_OFFSET
#else
#define PHYS_OFFSET	UL(CONFIG_PHYS_OFFSET)
#endif
#endif

/*
 * PFNs are used to describe any physical page; this means
 * PFN 0 == physical address 0.
 *
 * This is the PFN of the first RAM page in the kernel
 * direct-mapped view.  We assume this is the first page
 * of RAM in the mem_map as well.
 */
/** 20130511 
PHYS_OFFSET : (__pv_phys_offset) = 0x6000 0000 (vexpress에서 커널 페이지의 offset)
PAGE_SHIFT  : 12 
커널 시작 주소에 대한 PFN
**/
#define PHYS_PFN_OFFSET	(PHYS_OFFSET >> PAGE_SHIFT)

/*
 * These are *only* valid on the kernel direct mapped RAM memory.
 * Note: Drivers should NOT use these.  They are the wrong
 * translation for translating DMA addresses.  Use the driver
 * DMA support - see dma-mapping.h.
 */
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
	return __virt_to_phys((unsigned long)(x));
}

static inline void *phys_to_virt(phys_addr_t x)
{
	return (void *)(__phys_to_virt((unsigned long)(x)));
}

/*
 * Drivers should NOT use these either.
 */
/** 20130803    
 * __pa : virtual 주소를 physical 주소로 변환
 * __va : physical 주소를 virtual 주소로 변환
 **/
#define __pa(x)			__virt_to_phys((unsigned long)(x))
#define __va(x)			((void *)__phys_to_virt((unsigned long)(x)))
#define pfn_to_kaddr(pfn)	__va((pfn) << PAGE_SHIFT)

/*
 * Virtual <-> DMA view memory address translations
 * Again, these are *only* valid on the kernel direct mapped RAM
 * memory.  Use of these is *deprecated* (and that doesn't mean
 * use the __ prefixed forms instead.)  See dma-mapping.h.
 */
#ifndef __virt_to_bus
#define __virt_to_bus	__virt_to_phys
#define __bus_to_virt	__phys_to_virt
#define __pfn_to_bus(x)	__pfn_to_phys(x)
#define __bus_to_pfn(x)	__phys_to_pfn(x)
#endif

static inline __deprecated unsigned long virt_to_bus(void *x)
{
	return __virt_to_bus((unsigned long)x);
}

static inline __deprecated void *bus_to_virt(unsigned long x)
{
	return (void *)__bus_to_virt(x);
}

/*
 * Conversion between a struct page and a physical address.
 *
 * Note: when converting an unknown physical address to a
 * struct page, the resulting pointer must be validated
 * using VALID_PAGE().  It must return an invalid struct page
 * for any physical address not corresponding to a system
 * RAM address.
 *
 *  page_to_pfn(page)	convert a struct page * to a PFN number
 *  pfn_to_page(pfn)	convert a _valid_ PFN number to struct page *
 *
 *  virt_to_page(k)	convert a _valid_ virtual address to struct page *
 *  virt_addr_valid(k)	indicates whether a virtual address is valid
 */
/** 20130803    
 * ARCH_PFN_OFFSET은 Kernel 물리 시작 주소에 대한 PFN
 **/
#define ARCH_PFN_OFFSET		PHYS_PFN_OFFSET

/** 20130511
kaddr에 해당하는 물리주소를 구해서 pfn로 변환한 다음 pfn_to_page로 페이지의 위치를 반환하는 매크로  
**/
#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
#define virt_addr_valid(kaddr)	((unsigned long)(kaddr) >= PAGE_OFFSET && (unsigned long)(kaddr) < (unsigned long)high_memory)

/*
 * Optional coherency support.  Currently used only by selected
 * Intel XSC3-based systems.
 */
#ifndef arch_is_coherent
#define arch_is_coherent()		0
#endif

#endif

#include <asm-generic/memory_model.h>

#endif
