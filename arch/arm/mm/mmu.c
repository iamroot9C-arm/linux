/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/sections.h>
#include <asm/cachetype.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/tlb.h>
#include <asm/highmem.h>
#include <asm/system_info.h>
#include <asm/traps.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "mm.h"

/*
 * empty_zero_page is a special page that is used for
 * zero-initialized data and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

/*
 * The pmd table for the upper-most set of pages.
 */
pmd_t *top_pmd;

#define CPOLICY_UNCACHED	0
#define CPOLICY_BUFFERED	1
#define CPOLICY_WRITETHROUGH	2
#define CPOLICY_WRITEBACK	3
#define CPOLICY_WRITEALLOC	4

static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	const char	policy[16];
	unsigned int	cr_mask;
	pmdval_t	pmd;
	pteval_t	pte;
};

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
	}
};

/*
 * These are useful for identifying cache coherency
 * problems by allowing the cache or the cache and
 * writebuffer to be turned off.  (Note: the write
 * buffer should not be on and the cache off).
 */
static int __init early_cachepolicy(char *p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0) {
			cachepolicy = i;
			cr_alignment &= ~cache_policies[i].cr_mask;
			cr_no_alignment &= ~cache_policies[i].cr_mask;
			break;
		}
	}
	if (i == ARRAY_SIZE(cache_policies))
		printk(KERN_ERR "ERROR: unknown or unsupported cache policy\n");
	/*
	 * This restriction is partly to do with the way we boot; it is
	 * unpredictable to have memory mapped using two different sets of
	 * memory attributes (shared, type, and cache attribs).  We can not
	 * change these attributes once the initial assembly has setup the
	 * page tables.
	 */
	if (cpu_architecture() >= CPU_ARCH_ARMv6) {
		printk(KERN_WARNING "Only cachepolicy=writeback supported on ARMv6 and later\n");
		cachepolicy = CPOLICY_WRITEBACK;
	}
	flush_cache_all();
	set_cr(cr_alignment);
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

static int __init early_nocache(char *__unused)
{
	char *p = "buffered";
	printk(KERN_WARNING "nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nocache", early_nocache);

static int __init early_nowrite(char *__unused)
{
	char *p = "uncached";
	printk(KERN_WARNING "nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nowb", early_nowrite);

#ifndef CONFIG_ARM_LPAE
static int __init early_ecc(char *p)
{
	if (memcmp(p, "on", 2) == 0)
		ecc_mask = PMD_PROTECTION;
	else if (memcmp(p, "off", 3) == 0)
		ecc_mask = 0;
	return 0;
}
early_param("ecc", early_ecc);
#endif

static int __init noalign_setup(char *__unused)
{
	cr_alignment &= ~CR_A;
	cr_no_alignment &= ~CR_A;
	set_cr(cr_alignment);
	return 1;
}
__setup("noalign", noalign_setup);

#ifndef CONFIG_SMP
void adjust_cr(unsigned long mask, unsigned long set)
{
	unsigned long flags;

	mask &= ~CR_A;

	set &= mask;

	local_irq_save(flags);

	cr_no_alignment = (cr_no_alignment & ~mask) | set;
	cr_alignment = (cr_alignment & ~mask) | set;

	set_cr((get_cr() & ~mask) | set);

	local_irq_restore(flags);
}
#endif

#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_XN
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

/** 20130202
* arm linux page table 은 2개로 관리되는데, Hardware용과 Linux용 2가지이다.
* L_PTE_xxx 매크로의 경우 리눅스용 페이지 테이블의 설정값이다.
* PMD_xxx 매크로의 경우 하드웨어용 페이지 테이블의 설정값이다.
* .prot_pte 필드는 리눅스용 pte값을 저장하는 곳이다.
* .prot_l1, .prot_sect 는 하드웨어용 pte값을 저장하는 곳이다.
* 	20130216
* 		ARM CortexA PG. Figure 10-3 Level 1 page table entry format
* 		.prot_l1 : Pointer to 2nd level page table
* 		.prot_sect : Section
* ???
*/
static struct mem_type mem_types[] = {
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cached */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},	
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	[MT_CACHECLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
#ifndef CONFIG_ARM_LPAE
	[MT_MINICLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
#endif
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
/** 20130202
* prot_pte = 0x43 
* #define L_PTE_PRESENT		(_AT(pteval_t, 1) << 0)
* #define L_PTE_YOUNG		(_AT(pteval_t, 1) << 1)
* #define L_PTE_DIRTY		(_AT(pteval_t, 1) << 6)
* prot_l1 = 0x1
* #define PMD_TYPE_TABLE		(_AT(pmdval_t, 1) << 0)
* prot_sect = 0x402
* #define PMD_TYPE_SECT		(_AT(pmdval_t, 2) << 0)
* #define PMD_SECT_AP_WRITE	(_AT(pmdval_t, 1) << 10)
* domain = 0
*/
	[MT_MEMORY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_ROM] = {
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_NONCACHED] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_BUFFERABLE,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DTCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_ITCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_SO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_UNCACHED,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_S |
				PMD_SECT_UNCACHED | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DMA_READY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
};

const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}
EXPORT_SYMBOL(get_mem_type);

/*
 * Adjust the PMD section entries according to the CPU in use.
 */
/** 20130216
 * mem_types 에 architecture에 따른 기본 속성을 추가. 
 * */
static void __init build_mem_type_table(void)
{
	struct cachepolicy *cp;
/** 20130202
* cr in vexpers  = 0x10c53c7d
*/  
	unsigned int cr = get_cr();
/**
*
*/
	pteval_t user_pgprot, kern_pgprot, vecs_pgprot;
/** 20130202  
* CPU_ARCH_ARMv7
*/
	int cpu_arch = cpu_architecture();
	int i;

/** 20130202  
* cachepolicy : default is  CPOLICY_WRITEBACK
*/
	if (cpu_arch < CPU_ARCH_ARMv6) {
#if defined(CONFIG_CPU_DCACHE_DISABLE)
		if (cachepolicy > CPOLICY_BUFFERED)
			cachepolicy = CPOLICY_BUFFERED;
#elif defined(CONFIG_CPU_DCACHE_WRITETHROUGH)
		if (cachepolicy > CPOLICY_WRITETHROUGH)
			cachepolicy = CPOLICY_WRITETHROUGH;
#endif
	}
	if (cpu_arch < CPU_ARCH_ARMv5) {
		if (cachepolicy >= CPOLICY_WRITEALLOC)
			cachepolicy = CPOLICY_WRITEBACK;
		ecc_mask = 0;
	}
/** 20130202  
* cachepolicy : smp이므로   CPOLICY_WRITEBACK -> CPOLICY_WRITEALLOC
*/
	if (is_smp())
		cachepolicy = CPOLICY_WRITEALLOC;

	/*
	 * Strip out features not present on earlier architectures.
	 * Pre-ARMv5 CPUs don't have TEX bits.  Pre-ARMv6 CPUs or those
	 * without extended page tables don't have the 'Shared' bit.
	 */
	if (cpu_arch < CPU_ARCH_ARMv5)
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_TEX(7);
/** 20130202
*  해당사항 없음
*/ 
	if ((cpu_arch < CPU_ARCH_ARMv6 || !(cr & CR_XP)) && !cpu_is_xsc3())
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_S;
	/*
	 * ARMv5 and lower, bit 4 must be set for page tables (was: cache
	 * "update-able on write" bit on ARM610).  However, Xscale and
	 * Xscale3 require this bit to be cleared.
	*/
	if (cpu_is_xscale() || cpu_is_xsc3()) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			mem_types[i].prot_sect &= ~PMD_BIT4;
			mem_types[i].prot_l1 &= ~PMD_BIT4;
		}
	} else if (cpu_arch < CPU_ARCH_ARMv6) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			if (mem_types[i].prot_l1)
				mem_types[i].prot_l1 |= PMD_BIT4;
			if (mem_types[i].prot_sect)
				mem_types[i].prot_sect |= PMD_BIT4;
		}
	}

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
/** 20130202
* XP : Extended page tables, default 1	
*/ 
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
/** 20130202
* XN : Excecute Never , determines if the region is Executable (0) or Not-executable (1)
* MT_DEVICE 영역은 instruction이 fetch되지 않게 Not-executable 영역으로 선언.
*/ 
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;
		}
/** 20130202 ??? CR_TRE가 어디서 설정되었는지 확인 필요..
* #define CR_TRE	(1 << 28)	 TEX remap enable		
* TEX : Type extension, refer to ARM Doc page 1688
*/
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
/** 20130202 ??? 이 부분은 다시 정리 필요..
* PMD_SECT_TEX(1) ->
*   #define PMD_SECT_TEX(x)		(_AT(pmdval_t, (x)) << 12)	// v5 
*   TEX 14.13.12 -> 001
* PMD_SECT_BUFFERABLE -> B bit
*/
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
/* 20130202
* 여기들어오지 않음...
*/
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
/** 20130202
*	cachepolicy =  WRITEALLOC, 4
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC, (0x1C) -> XCB = 111
*/
	cp = &cache_policies[cachepolicy];
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;

	/*
	 * Enable CPU-specific coherency if supported.
	 * (Only available on XSC3 at the moment.)
	 */
	if (arch_is_coherent() && cpu_is_xsc3()) {
		mem_types[MT_MEMORY].prot_sect |= PMD_SECT_S;
		mem_types[MT_MEMORY].prot_pte |= L_PTE_SHARED;
		mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
		mem_types[MT_MEMORY_NONCACHED].prot_sect |= PMD_SECT_S;
		mem_types[MT_MEMORY_NONCACHED].prot_pte |= L_PTE_SHARED;
	}
	/*
	 * ARMv6 and above have extended page tables.
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
/** 20130202
* Large Page Table Entry is not set.
*/
#ifndef CONFIG_ARM_LPAE
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
/** 20130202
* PMD_SECT_APX 15 bit
* PMD_SECT_AP_WRITE 10 bit
* L1 page table entry format 에서 APX, AP(o1) 를 설정.
* Kernel mode에서만 접근가능하게 설정. 
*/
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
#endif

		if (is_smp()) {
			/*
			 * Mark memory with the "shared" attribute
			 * for SMP systems
			 */
/** 20130202
* XXX_SHARED 의미는 프로세서(core)간에 공유로 해석되고,
* 따라서 SMP 에서는 관련된 메모리를 모두 XXX_SHARED로 선언 하는 듯 ???
* #define L_PTE_SHARED		(_AT(pteval_t, 1) << 10)	// shared(v6), coherent(xsc3)/
*/
			user_pgprot |= L_PTE_SHARED;
			kern_pgprot |= L_PTE_SHARED;
			vecs_pgprot |= L_PTE_SHARED;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_NONCACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_NONCACHED].prot_pte |= L_PTE_SHARED;
		}
	}

	/*
	 * Non-cacheable Normal - intended for memory areas that must
	 * not cause dirty cache line writebacks when used
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6) {
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/* Non-cacheable Normal is XCB = 001 */
/** 20130202
* cache 사용하지 않지만 write 시 cpu -> buffer -> physical memmory 순으로 동작하도록 설정.
* 실제 sram 영역이 해당 됨. ???
*/
			mem_types[MT_MEMORY_NONCACHED].prot_sect |=
				PMD_SECT_BUFFERED;
		} else {
			/* For both ARMv6 and non-TEX-remapping ARMv7 */
			mem_types[MT_MEMORY_NONCACHED].prot_sect |=
				PMD_SECT_TEX(1);
		}
	} else {
		mem_types[MT_MEMORY_NONCACHED].prot_sect |= PMD_SECT_BUFFERABLE;
	}

#ifdef CONFIG_ARM_LPAE
	/*
	 * Do not generate access flag faults for the kernel mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		mem_types[i].prot_pte |= PTE_EXT_AF;
		if (mem_types[i].prot_sect)
			mem_types[i].prot_sect |= PMD_SECT_AF;
	}
	kern_pgprot |= PTE_EXT_AF;
	vecs_pgprot |= PTE_EXT_AF;
#endif

	for (i = 0; i < 16; i++) {
		unsigned long v = pgprot_val(protection_map[i]);
/** 20130202
* user_pgprot는 현재 = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
* vecs_pgprot는 현재 = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
* protection_map에 user_pgprot값을 추가
*/
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

/** 20130216 여기서부터
*/
	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | kern_pgprot);

	/** 20130216
	 * ecc_mask		= 0
	 * cp->pmd		= PMD_SECT_WBWA,
	 * kern_pgprot	= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * */
	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_MEMORY].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_DMA_READY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_NONCACHED].prot_sect |= ecc_mask;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	printk("Memory policy: ECC %sabled, Data cache %s\n",
		ecc_mask ? "en" : "dis", cp->policy);

	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
}

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);
#endif

/** 20130216
 * cr in vexpress  = 0x10c53c7d
 * vexpress에서는 high vector로 default 세팅되어 있어서 0xffff0000 
 **/
#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)
/** 20130302 
 	memblock_alloc된 영역의 시작주소를 align된 가상주소로 변환하여 초기화 하고 리턴한다.
 **/	
static void __init *early_alloc_aligned(unsigned long sz, unsigned long align)
{
	void *ptr = __va(memblock_alloc(sz, align));
	memset(ptr, 0, sz);
	return ptr;
}
/** 20130302 
 	size만큼 메모리를 alloc한다.
 **/	
static void __init *early_alloc(unsigned long sz)
{
	return early_alloc_aligned(sz, sz);
}

/** 20130309    
 * pte table을 위한 공간을 생성하고 해당 pmd에 속성과 함께 저장.
 * 할당한 pte 중 addr에 해당하는 index의 주소를 리턴.
 **/
static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr, unsigned long prot)
{
	/** 20130302 
	 (*pmd)가 NULL일 경우
	 **/	
	if (pmd_none(*pmd)) {
	/** 20130302 	 
 	*
 	*    pgd             pte
 	* |        |
 	* +--------+
 	* |        |       +------------+ +0
 	* +- - - - +       | Linux pt 0 |
 	* |        |       +------------+ +1024
 	* +--------+ +0    | Linux pt 1 |
 	* |        |-----> +------------+ +2048
 	* +- - - - + +4    |  h/w pt 0  |
 	* |        |-----> +------------+ +3072
 	* +--------+ +8    |  h/w pt 1  |
 	* |        |       +------------+ +4096
 	*
	**/	
		pte_t *pte = early_alloc(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE);
		__pmd_populate(pmd, __pa(pte), prot);
	}
	/** 20130309    
	 * 방금 채운 pmd의 값이 정상인지 검사하는 함수
	 **/
	BUG_ON(pmd_bad(*pmd));
	/** 20130309    
	 * alloc한 pte에서 addr에 해당하는 pte entry의 주소를 리턴
	 **/
	return pte_offset_kernel(pmd, addr);
}

/** 20130309    
 * pte table을 할당하고 초기화
 **/
static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type)
{
	/** 20130309    
	 * pte를 alloc 하고, addr에 해당하는 pte entry의 주소가 리턴됨
	 **/
	pte_t *pte = early_pte_alloc(pmd, addr, type->prot_l1);
	/** 20130309    
	 * PAGE_SIZE 단위로 pte table의 각 entry (pte 속성이 적용)를 채움.
	 *   early_pte_alloc이 리턴한 주소부터 end까지 순회하며 값을 채운다.
	 **/
	do {
			set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)), 0);
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

/** 20130309    
 * section 단위로 pmd entry를 채운다.
 * 정렬되지 않은 영역에 대해 pmd에 l2 table을 저장하고, pte entry를 생성하고 채움
 **/
static void __init alloc_init_section(pud_t *pud, unsigned long addr,
				      unsigned long end, phys_addr_t phys,
				      const struct mem_type *type)
{
	/** 20130223    
	 * pgtable-2level에서는 pud이 그대로 나옴
	 **/
	pmd_t *pmd = pmd_offset(pud, addr);

	/*
	 * Try a section mapping - end, addr and phys must all be aligned
	 * to a section boundary.  Note that PMDs refer to the individual
	 * L1 entries, whereas PGDs refer to a group of L1 entries making
	 * up one logical pointer to an L2 table.
	 */
	/** 20130223    
	 * prot_sect 가 0이 아니고, addr, end, phys 모두 SECTION 단위로 정렬되어 있으면 true
	 **/
	if (type->prot_sect && ((addr | end | phys) & ~SECTION_MASK) == 0) {
		pmd_t *p = pmd;

#ifndef CONFIG_ARM_LPAE
		/** 20130223    
		 * addr이 1MB보다 크고 2MB보다 작을 경우 다음 pmd부터 page table에 entry를 생성하기 위해
		 * 경계를 검사하는 부분. (pgd = pud 는 2MB 단위의 주소. pgd_t는 2개의 pmd_t로 이루어짐)
		 **/
		if (addr & SECTION_SIZE)
			pmd++;
#endif

		/** 20130223    
		 * addr부터 end까지 pmd entry를 채운다.
		 **/
		do {
			*pmd = __pmd(phys | type->prot_sect);
			phys += SECTION_SIZE;
		} while (pmd++, addr += SECTION_SIZE, addr != end);

		/** 20130223    
		 * 최초 pmd 값을 기준으로 flush 수행
		 **/
		flush_pmd_entry(p);
	} else {
		/*
		 * No need to loop; pte's aren't interested in the
		 * individual L1 entries.
		 */
		/** 20130223    
		 * type->prot_sect가 0이거나, prot_sect여도 정렬되어 있지 않다면 수행
		 **/
		alloc_init_pte(pmd, addr, end, __phys_to_pfn(phys), type);
	}
}

/** 20130309    
 * addr에서 end까지 pud entry를 초기화
 **/
static void __init alloc_init_pud(pgd_t *pgd, unsigned long addr,
	unsigned long end, unsigned long phys, const struct mem_type *type)
{
	/** 20130223    
	 * pgtable-nopud에서는 pgd이 그대로 나옴
	 **/
	pud_t *pud = pud_offset(pgd, addr);
	unsigned long next;

	do {
		/** 20130223    
		 * pgtable-nopud에서는 end가 그대로 나옴
		 **/
		 /** 20130309    
		  * addr과 end 사이에 작은 값을 취해 next로 삼는다.
		  **/
		next = pud_addr_end(addr, end);
		/** 20130309    
		 * section 단위로 pud(pmd)를 채운다.
		 *   (section 크기보다 작은 단위는 pte를 생성)
		 **/
		alloc_init_section(pud, addr, next, phys, type);
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

#ifndef CONFIG_ARM_LPAE
static void __init create_36bit_mapping(struct map_desc *md,
					const struct mem_type *type)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	pgd_t *pgd;

	addr = md->virtual;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		printk(KERN_ERR "MM: CPU does not support supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	if (type->domain) {
		printk(KERN_ERR "MM: invalid domain in supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		printk(KERN_ERR "MM: cannot create mapping for 0x%08llx"
		       " at 0x%08lx invalid alignment\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	pgd = pgd_offset_k(addr);
	end = addr + length;
	do {
		pud_t *pud = pud_offset(pgd, addr);
		pmd_t *pmd = pmd_offset(pud, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER);

		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}
#endif	/* !CONFIG_ARM_LPAE */

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
/** 20130309    
 * map_desc 영역에 대한 page table을 채우는 함수
 **/
static void __init create_mapping(struct map_desc *md)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	const struct mem_type *type;
	pgd_t *pgd;

	/** 20130223
	 * md->virtual이 low vector가 아니면서 TASK_SIZE보다 작은 영역으로 변환된 virtual 주소일 경우
	 * md->virtual이 high vector가 아니면서 TASK_SIZE보다 작은 영역으로 변환된 virtual 주소일 경우
	 * mapping table을 생성하지 않음. ???
	 *
	 * vectors_base() : vector tables의 시작 주소
	 * TASK_SIZE : user space task의 최대 크기 (0x80000000 - 0x01000000 in vexpress)
	 **/
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		printk(KERN_WARNING "BUG: not creating mapping for 0x%08llx"
		       " at 0x%08lx in user region\n",
		       (long long)__pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

	/** 20130223    
	 * map_lowmem에서 넘어온 경우는 MT_MEMORY로 들어와 false
	 **/
	if ((md->type == MT_DEVICE || md->type == MT_ROM) &&
	    md->virtual >= PAGE_OFFSET &&
	    (md->virtual < VMALLOC_START || md->virtual >= VMALLOC_END)) {
		printk(KERN_WARNING "BUG: mapping for 0x%08llx"
		       " at 0x%08lx out of vmalloc space\n",
		       (long long)__pfn_to_phys((u64)md->pfn), md->virtual);
	}

	type = &mem_types[md->type];

#ifndef CONFIG_ARM_LPAE
	/*
	 * Catch 36-bit addresses
	 */
	/** 20130223    
	 * LPAE가 아닌 경우 pfn은 0x100000 보다 작음 (4G / 4K)
	 **/
	if (md->pfn >= 0x100000) {
		create_36bit_mapping(md, type);
		return;
	}
#endif

	/** 20130223
	 * md->virtual을 PAGE 단위로 align. (round down)
	 **/
	addr = md->virtual & PAGE_MASK;
	phys = __pfn_to_phys(md->pfn);
	/** 20130223
	 * 'md->length (size) + md->virtual의 하위 12비트'의 주소를 PAGE_ALIGN(round up) 시킴
	 **/
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));

	/** 20130223
	 * map_lowmem에서 넘어온 경우는 MT_MEMORY로 false
	 *	prot_l1이 0 -> fault의 의미
	 *	~SECTION_MASK는 0xfffff
	 *
	 * (addr | phys | length) 을 해서 1MB 단위로 안 떨어지면 ignore ???
	 **/
	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		printk(KERN_WARNING "BUG: map for 0x%08llx at 0x%08lx can not "
		       "be mapped using pages, ignoring.\n",
		       (long long)__pfn_to_phys(md->pfn), addr);
		return;
	}

	/** 20130223    
	 * addr를 포함하는 page table entry의 주소
	 **/
	pgd = pgd_offset_k(addr);
	end = addr + length;
	do {
		/** 20130223    
		 * addr을 PGDIR_SIZE로 round-up 한 값과 end 값 중에 작은 값을 next에 저장
		 * 즉 next는 2MB 단위
		 **/
		unsigned long next = pgd_addr_end(addr, end);

		/** 20130309    
		 * addr, next 영역에 대한 pud entry를 채움
		 **/
		alloc_init_pud(pgd, addr, next, phys, type);

		phys += next - addr;
		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the architecture specific mappings
 */
/** 20130323
*	nr 개수 만큼 vm_struct 메모리를 할당 받아 페이지테이블에 등록하고 vmlist 에 추가함. 
*/
void __init iotable_init(struct map_desc *io_desc, int nr)
{
	struct map_desc *md;
	struct vm_struct *vm;

	if (!nr)
		return;

	vm = early_alloc_aligned(sizeof(*vm) * nr, __alignof__(*vm));

	for (md = io_desc; nr; md++, nr--) {
		/** 20130330    
		 * md 영역에 대한 page table entry를 등록
		 **/
		create_mapping(md);
		/** 20130330    
		 * 정렬되지 않은 경우 addr은 PAGE_MASK로 정렬시키고(round down),
		 * 그 크기를 length에 더해 size에 저장
		 **/
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn); 
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING; 
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		/** 20130330    
		 * vm_area를 추가하고, 여러 개일 경우 vm++로 다음 entry를 가리킴
		 **/
		vm_area_add_early(vm++);
	}
}

#ifndef CONFIG_ARM_LPAE

/*
 * The Linux PMD is made of two consecutive section entries covering 2MB
 * (see definition in include/asm/pgtable-2level.h).  However a call to
 * create_mapping() may optimize static mappings by using individual
 * 1MB section mappings.  This leaves the actual PMD potentially half
 * initialized if the top or bottom section entry isn't used, leaving it
 * open to problems if a subsequent ioremap() or vmalloc() tries to use
 * the virtual space left free by that unused section entry.
 *
 * Let's avoid the issue by inserting dummy vm entries covering the unused
 * PMD halves once the static mappings are in place.
 */

/** 20130330    
 * 홀수 section인 경우 create_mapping 없이 vmlist에 추가
 **/
static void __init pmd_empty_section_gap(unsigned long addr)
{
	struct vm_struct *vm;

	vm = early_alloc_aligned(sizeof(*vm), __alignof__(*vm));
	vm->addr = (void *)addr;
	vm->size = SECTION_SIZE;
	vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
	vm->caller = pmd_empty_section_gap;
	vm_area_add_early(vm);
}

/** 20130330    
 * vmlist를 순회하며 각 node에서 odd section 단위로 할당되어 있고,
 * pmd가 free인 경우 ioremap()이나 vmalloc()에서 사용하지 못하게 vmlist에 추가
 **/
static void __init fill_pmd_gaps(void)
{
	struct vm_struct *vm;
	unsigned long addr, next = 0;
	pmd_t *pmd;

	/* we're still single threaded hence no lock needed here */
	for (vm = vmlist; vm; vm = vm->next) {
		if (!(vm->flags & VM_ARM_STATIC_MAPPING))
			continue;
		addr = (unsigned long)vm->addr;
		/** 20130330    
		 * addr이 이전에 구한 pmd 단위 내에 속하면 continue
		 **/
		if (addr < next)
			continue;

		/*
		 * Check if this vm starts on an odd section boundary.
		 * If so and the first section entry for this PMD is free
		 * then we block the corresponding virtual address.
		 */
		/** 20130330    
		 * 하위 21비트를 추출해 SECTION_SIZE인지 검사 (홀수번째 섹션)
		 **/
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			/** 20130330    
			 * addr에 해당하는 pmd entry를 구함
			 **/
			pmd = pmd_off_k(addr);
			/** 20130330    
			 * pmd가 NULL일 때.
			 * 즉 create_mapping이 안 되어 있을 경우 vm_list에 추가.
			 * vm_list에 추가하는 것이 block의 의미를 가지는지???
			 **/
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr & PMD_MASK);
		}

		/*
		 * Then check if this vm ends on an odd section boundary.
		 * If so and the second section entry for this PMD is empty
		 * then we block the corresponding virtual address.
		 */
		addr += vm->size;
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr) + 1;
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr);
		}

		/* no need to look at any vm entry until we hit the next PMD */
		/** 20130330    
		 * next는 'addr += vm->size'의 다음 PMD 단위의 주소
		 * (PMD_SIZE - 1을 더해 MASK로 상위 주소만 추출)
		 **/
		next = (addr + PMD_SIZE - 1) & PMD_MASK;
	}
}

#else
#define fill_pmd_gaps() do { } while (0)
#endif

static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);

/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 240m.
 */
static int __init early_vmalloc(char *arg)
{
	unsigned long vmalloc_reserve = memparse(arg, NULL);

	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		printk(KERN_WARNING
			"vmalloc area too small, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	if (vmalloc_reserve > VMALLOC_END - (PAGE_OFFSET + SZ_32M)) {
		vmalloc_reserve = VMALLOC_END - (PAGE_OFFSET + SZ_32M);
		printk(KERN_WARNING
			"vmalloc area is too big, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	vmalloc_min = (void *)(VMALLOC_END - vmalloc_reserve);
	return 0;
}
early_param("vmalloc", early_vmalloc);

/** 20130126    
 * arm_lowmem_limit은 ZONE_NORMAL에서의 physical memory 마지막 주소
 **/
phys_addr_t arm_lowmem_limit __initdata = 0;

/** 20130119
  메모리 뱅크들에 대한 적정한 설정이 되어 있는지 조사하고 수정한다
  1. highmem세팅
  2. memory bank에 대한 재조정
  3. highmem세팅시 캐쉬 aliasing을 검사하고
  4. high_memory(가상주소)를 세팅하고, ZONE_NORMAL에서의 memblock.current_limit를 세팅
 **/
void __init sanity_check_meminfo(void)
{
	int i, j, highmem = 0;
/** 20130119
 * meminfo.nr_banks 값은 8까지 가능.
 * CONFIG_HIGHMEM이 꺼져 있을 때, highmem이 1일 경우 해당 뱅크를 삭제한다.
 * j : 앞으로 설정이 될 뱅크의 인덱스
 * i : 앞으로 처리할 뱅크의 인덱스
 **/
	for (i = 0, j = 0; i < meminfo.nr_banks; i++) {
		struct membank *bank = &meminfo.bank[j];
		*bank = meminfo.bank[i];
/** 20130112
	bank의 start가 4기가 이상이면 highmemory를 설정한다.
**/
		if (bank->start > ULONG_MAX)
			highmem = 1;

#ifdef CONFIG_HIGHMEM
/** 20130112
	static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);
    early_vmalloc이 실행되지 않으면 default값으로 240M가 세팅된다.
	
	bank의 start가 vamalloc_min보다 같거나 크면
	PAGE_OFFSET(0x8000000)이 bank의 start보다 크다면 
	highmem 설정한다.
 	
	커널은 vexpress의 경우 PAGE_OFFSET(0x80000000)을 시작으로
    VMALLOC_END (0xff000000)까지의 메모리만 접근가능하다
	커널 입장에서는 이 영역을 제외한 영역은 High Memory로 인식하다.
	__va(bank->start) < (void *)PAGE_OFFSET)
	이 조건문은 뱅크의 Start가 PAGE_OFFSET보다 작을 경우 High Memory로 설정된다.
**/
		if (__va(bank->start) >= vmalloc_min ||
		    __va(bank->start) < (void *)PAGE_OFFSET)
			highmem = 1;

		bank->highmem = highmem;

		/*
		 * Split those memory banks which are partially overlapping
		 * the vmalloc area greatly simplifying things later.
		 */
/** 20130112
		bank의 address range와 vmalloc의 영역이 겹치는지 확인
**/	
		if (!highmem && __va(bank->start) < vmalloc_min &&
		    bank->size > (vmalloc_min - __va(bank->start))) {
/** 20130112
		사용 가능한 메모리를 나누어서 뱅크로 추가할수 없으면 에러를 출력하고 커널패닉에 빠진다. 
**/
		
			if (meminfo.nr_banks >= NR_BANKS) {
				printk(KERN_CRIT "NR_BANKS too low, "
						 "ignoring high memory\n");
			} else {
/** 20130112
	1. 현재 뱅크에서 vmalloc영역과 겹치는 부분이 있는지 조사
	2. 겹치는 부분이 있으면 뱅크를 추가하기 위해 뒤의 뱅크를 하나씩 미룬다.
	3. 겹치는 부분의 데이터(물리 주소 start, 사이즈 size) 를 계산해서 신규뱅크(bank[1])에 저장한다.
	4. 현재 뱅크의 사이즈를 크기를 조정한다. 
**/
				memmove(bank + 1, bank,
					(meminfo.nr_banks - i) * sizeof(*bank));
				meminfo.nr_banks++;
				i++;
				bank[1].size -= vmalloc_min - __va(bank->start);
				bank[1].start = __pa(vmalloc_min - 1) + 1;
				bank[1].highmem = highmem = 1;
				j++;
			}
			bank->size = vmalloc_min - __va(bank->start);
		}
#else
		bank->highmem = highmem;

		/*
		 * Highmem banks not allowed with !CONFIG_HIGHMEM.
		 */
/** 20130112
		CONFIG_HIGHMEM이 꺼져있고 highmem 변수 1인 경우 현 뱅크의 시작주소가 4기가를 
		넘어서면  다음 뱅크로 진행한다.
**/
		if (highmem) {
			printk(KERN_NOTICE "Ignoring RAM at %.8llx-%.8llx "
			       "(!CONFIG_HIGHMEM).\n",
			       (unsigned long long)bank->start,
			       (unsigned long long)bank->start + bank->size - 1);
			continue;
		}

		/*
		 * Check whether this memory bank would entirely overlap
		 * the vmalloc area.
		 */
/** 20130112
		CONFIG_HIGHMEM꺼져 있는 상태에서
		현 뱅크의 address영역이 vmalloc영역 안에 있는 경우
		또는 커널의 1기가 영역외에 현재 뱅크가 있는 경우
		다음 뱅크로 진행하다.
**/
		if (__va(bank->start) >= vmalloc_min ||
		    __va(bank->start) < (void *)PAGE_OFFSET) {
			printk(KERN_NOTICE "Ignoring RAM at %.8llx-%.8llx "
			       "(vmalloc region overlap).\n",
			       (unsigned long long)bank->start,
			       (unsigned long long)bank->start + bank->size - 1);
			continue;
		}

		/*
		 * Check whether this memory bank would partially overlap
		 * the vmalloc area.
		 */
/** 20130112
		CONFIG_HIGHMEM꺼져 있는 상태에서
		현 뱅크의 address영역이 vmalloc 영역과 겹치는 경우 
		현 뱅크의 사이즈를 조정한다.
**/
        if (__va(bank->start + bank->size) > vmalloc_min ||
		    __va(bank->start + bank->size) < __va(bank->start)) {
			unsigned long newsize = vmalloc_min - __va(bank->start);
			printk(KERN_NOTICE "Truncating RAM at %.8llx-%.8llx "
			       "to -%.8llx (vmalloc region overlap).\n",
			       (unsigned long long)bank->start,
			       (unsigned long long)bank->start + bank->size - 1,
			       (unsigned long long)bank->start + newsize - 1);
			bank->size = newsize;
		}
#endif
/** 20130112
	high memory 가 설정되어 있을 경우 low memory limit을 의미가 없고
		- 커널이 4기가의 모든 영역을 쓸수 있으므로...
	high memory 가 설정이 안되어 있으면 커널이 사용하는 최상위 주소(arm_lowmem_limit)를 지정해줘야 한다. 
**/
		if ((!bank->highmem) && (bank->start + bank->size > arm_lowmem_limit))
			arm_lowmem_limit = bank->start + bank->size;
		/** 20130119
         각각의 뱅크에 대해서 arm_lowmem_limit를 설정한다
         **/
        j++;
	}
#ifdef CONFIG_HIGHMEM
	if (highmem) {
		const char *reason = NULL;
		/** 20130119
        cacheid는 CACHEID_VIPT_NONALIASING, 
        mask는 CACHEID_VIPT_ALIASING으로 설정 되어 있으므로 
        cache_is_vipt_aliasing()의 값은 0이 된다
         **/
        if (cache_is_vipt_aliasing()) {
			/*
			 * Interactions between kmap and other mappings
			 * make highmem support with aliasing VIPT caches
			 * rather difficult.
			 */
			reason = "with VIPT aliasing cache";
		}
		if (reason) {
			printk(KERN_CRIT "HIGHMEM is not supported %s, ignoring high memory\n",
				reason);
            /** 20130119
            ZONE_NORMAL인 경우 물리주소와 가상주소가 1대1 매핑되므로 
            ALIASING이 발생하지 않는다고 가정한다.
            ZONE_HIGHMEM인 경우에는 ALIASING이 발생할 수 있고 
            ALIASING_CACHE면 HIGHMEM을 지원하지 않으려고 하는 듯 ???
             **/

            /** 20130119
            highmem이 셋팅되고, 캐쉬 정책이 VIPT_ALIASING이면 
            캐쉬 ALIASING문제로 highmem을 지원하는 뱅크를 카운트에서 제외한다.
             **/
			while (j > 0 && meminfo.bank[j - 1].highmem)
				j--;
		}
	}
#endif
	meminfo.nr_banks = j;
	high_memory = __va(arm_lowmem_limit - 1) + 1;
    /** 20130119
    arm_lowmem_limit을 memblock.current_limit값으로 지정
    (arm_lowmem_limit은 ZONE_NORMAL에서의 physical memory 마지막 주소)
     **/
    memblock_set_current_limit(arm_lowmem_limit);
}

/** 20130216
 * VMALLOC_START 이전에서 커널이 실행되고 있는 메모리를 제외한 영역의 pmd를 clear
 **/
static inline void prepare_page_table(void)
{
	unsigned long addr;
	phys_addr_t end;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
	/** 20130216
	 * MODULES_VADDR : PAGE_OFFSET - 16MB : 0x8000_0000 - 16MB(vexpress)
	 * PMD_SIZE : 2MB
	 *
	 * 왜 16MB를 뺐는지???
	 * */
	for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

#ifdef CONFIG_XIP_KERNEL
	/* The XIP kernel is mapped in the module area -- skip over it */
	addr = ((unsigned long)_etext + PMD_SIZE - 1) & PMD_MASK;
#endif
	for ( ; addr < PAGE_OFFSET; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Find the end of the first block of lowmem.
	 */
	/** 20130216
	 * memblock의 첫 번째 region에는 커널이 로딩되어 수행중이므로, pmd_clear에서 제외하기 위함.
	 **/
	end = memblock.memory.regions[0].base + memblock.memory.regions[0].size;
	if (end >= arm_lowmem_limit)
		end = arm_lowmem_limit;

	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the vmalloc region.
	 */
	/** 20130216
	 * memblock의 첫 번째 region의 마지막부터 VMALLOC_START까지 pmd_clear
	 * VMALLOC_START는 마지막 VA(물리메모리 주소) + 8MB 
		#define VMALLOC_START		(((unsigned long)high_memory + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1))
	 **/
	for (addr = __phys_to_virt(end);
	     addr < VMALLOC_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
}

/** 20130126    
 * vexpress는 LPAE 사용 안 함
 **/
#ifdef CONFIG_ARM_LPAE
/* the first page is reserved for pgd */
#define SWAPPER_PG_DIR_SIZE	(PAGE_SIZE + \
				 PTRS_PER_PGD * PTRS_PER_PMD * sizeof(pmd_t))
#else
#define SWAPPER_PG_DIR_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#endif

/*
 * Reserve the special regions of memory
 */
/** 20130126    
 * memory management 관련된 영역을 memblock.reserved에 등록
 **/
void __init arm_mm_memblock_reserve(void)
{
	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	/** 20130126    
	 * swapper_pg_dir 이름의 의미는???
	 * swapper_pg_dir 메모리 공간을 memblock.reserved에 등록
	 **/
	memblock_reserve(__pa(swapper_pg_dir), SWAPPER_PG_DIR_SIZE);

#ifdef CONFIG_SA1111
	/*
	 * Because of the SA1111 DMA bug, we want to preserve our
	 * precious DMA-able memory...
	 */
	memblock_reserve(PHYS_OFFSET, __pa(swapper_pg_dir) - PHYS_OFFSET);
#endif
}

/*
 * Set up the device mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_START, we will remove any debug device mappings.
 * This means you have to be careful how you debug this function, or any
 * called function.  This means you can't use any function or debugging
 * method which may touch any device, otherwise the kernel _will_ crash.
 */
/** 20130330    
 * vector table, mdesc->map_io (peripheral) 영역 등에 대한 mapping 생성 후,
 * tlb와 cache를 flush 함.
 **/
static void __init devicemaps_init(struct machine_desc *mdesc)
{
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
	/** 20130309    
	 * PAGE_SIZE만큼 메모리 공간을 할당 받아 vectors에 저장
	 **/
	vectors = early_alloc(PAGE_SIZE);

	/** 20130316
		vectors 영역을 채움.	
	**/
	early_trap_init(vectors);

	/** 20130316
		VMALLOC_START ~ 0Xffff ffff (가상주소의 끝)까지 pmd clear 시킴.
	**/
	for (addr = VMALLOC_START; addr; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)_etext - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#endif

	/*
	 * Map the cache flushing regions.
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

/** 20130323 여기부터..
**/
	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
	map.type = MT_HIGH_VECTORS;
	create_mapping(&map);

	if (!vectors_high()) {
		map.virtual = 0;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */
/** 20130323
* In the case of vexpress, map_io = v2m_map_io from v2m.c
*/
	if (mdesc->map_io)
		mdesc->map_io();
	fill_pmd_gaps();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	local_flush_tlb_all();
	/** 20130330    
	 * cpu_cache.flush_kern_all => v7_flush_kern_cache_all
	 **/
	flush_cache_all();
}

/** 20130330    
 *  PKMAP_BASE 영역에 대한 pte table을 생성하고, 전역변수 초기화
 **/
static void __init kmap_init(void)
{
#ifdef CONFIG_HIGHMEM
	/** 20130330    
	 * pmd_off_k(PKMAP_BASE)
	 *	-> PAGE_OFFSET - PMD_SIZE (0x80000000 - 0x200000)에 대한 pmd entry의 주소
	 **/
	pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
		PKMAP_BASE, _PAGE_KERNEL_TABLE);
#endif
}

/** 20130309    
 * lowmem 영역에 대해 mapping table (page table)을 생성하는 함수
 *   (lowmem : 물리적으로 존재하는 메모리의 끝주소)
 **/
static void __init map_lowmem(void)
{
	struct memblock_region *reg;

	/* Map all the lowmem memory banks. */
	/** 20130223    
	 * for_each_memblock은 memblock의 memory type의 region의 개수만큼 순회하는 매크로
	 * #define for_each_memblock(memblock_type, region)					\
			for (region = memblock.memblock_type.regions;				\
				 region < (memblock.memblock_type.regions + memblock.memblock_type.cnt);	\
				 region++)
	 **/
	for_each_memblock(memory, reg) {
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;
		struct map_desc map;

		/** 20130223
		 * end가 arm_lowmem_limit 보다 크면 end를 arm_lowmem_limit으로 조정
		 **/
		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		/** 20130223    
		 * 비정상인 경우 for 종료
		 **/
		if (start >= end)
			break;

		map.pfn = __phys_to_pfn(start);
		map.virtual = __phys_to_virt(start);
		map.length = end - start;
		map.type = MT_MEMORY;

		/** 20130309    
		 * 해당 memblock 영역에 대한 page table을 채운다.
		 **/
		create_mapping(&map);
	}
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
void __init paging_init(struct machine_desc *mdesc)
{
	void *zero_page;

	memblock_set_current_limit(arm_lowmem_limit);

	build_mem_type_table();
	prepare_page_table();
	map_lowmem();
	dma_contiguous_remap();
	devicemaps_init(mdesc);
	kmap_init();

	/** 20130330    
	 * high vector 영역에 대한 pmd entry의 주소
	 **/
	top_pmd = pmd_off_k(0xffff0000);

	/* allocate the zero page. */
	/** 20130330    
	 * 한 페이지를 할당 받아 zero_page에 저장
	 **/
	zero_page = early_alloc(PAGE_SIZE);
	/** 20130511 
	부팅시 사용할 메모리 초기화
	**/
	bootmem_init();

	/** 20130511 
	위에서 할당한 zero_page를 관리하는 struct page의 위치를 가져온다.
	**/	
	empty_zero_page = virt_to_page(zero_page);
	__flush_dcache_page(NULL, empty_zero_page);
}
