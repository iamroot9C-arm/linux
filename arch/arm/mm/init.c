/*
 *  linux/arch/arm/mm/init.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/export.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/of_fdt.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/dma-contiguous.h>
#include <linux/sizes.h>

#include <asm/mach-types.h>
#include <asm/memblock.h>
#include <asm/prom.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "mm.h"

/** 20150124    
 * initrd 물리 시작 주소와 크기가 저장되는 변수.
 **/
static unsigned long phys_initrd_start __initdata = 0;
static unsigned long phys_initrd_size __initdata = 0;
/** 20130810
 * kernel param으로 initrd start, size세팅 가능
 **/
static int __init early_initrd(char *p)
{
	unsigned long start, size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		phys_initrd_start = start;
		phys_initrd_size = size;
	}
	return 0;
}
early_param("initrd", early_initrd);

static int __init parse_tag_initrd(const struct tag *tag)
{
	printk(KERN_WARNING "ATAG_INITRD is deprecated; "
		"please update your bootloader.\n");
	phys_initrd_start = __virt_to_phys(tag->u.initrd.start);
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD, parse_tag_initrd);

/** 20150411    
 * qemu에서 -initrd debug_with_qemu/busybox-1.20.2/rootfs.img를 준 경우
 * atags를 파싱해 initrd 정보를 채움.
 **/
static int __init parse_tag_initrd2(const struct tag *tag)
{
	phys_initrd_start = tag->u.initrd.start;
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD2, parse_tag_initrd2);

#ifdef CONFIG_OF_FLATTREE
void __init early_init_dt_setup_initrd_arch(unsigned long start, unsigned long end)
{
	phys_initrd_start = start;
	phys_initrd_size = end - start;
}
#endif /* CONFIG_OF_FLATTREE */

/*
 * This keeps memory configuration data used by a couple memory
 * initialization functions, as well as show_mem() for the skipping
 * of holes in the memory map.  It is populated by arm_add_memory().
 */
struct meminfo meminfo;

void show_mem(unsigned int filter)
{
	int free = 0, total = 0, reserved = 0;
	int shared = 0, cached = 0, slab = 0, i;
	struct meminfo * mi = &meminfo;

	printk("Mem-info:\n");
	show_free_areas(filter);

	for_each_bank (i, mi) {
		struct membank *bank = &mi->bank[i];
		unsigned int pfn1, pfn2;
		struct page *page, *end;

		pfn1 = bank_pfn_start(bank);
		pfn2 = bank_pfn_end(bank);

		page = pfn_to_page(pfn1);
		end  = pfn_to_page(pfn2 - 1) + 1;

		do {
			total++;
			if (PageReserved(page))
				reserved++;
			else if (PageSwapCache(page))
				cached++;
			else if (PageSlab(page))
				slab++;
			else if (!page_count(page))
				free++;
			else
				shared += page_count(page) - 1;
			page++;
		} while (page < end);
	}

	printk("%d pages of RAM\n", total);
	printk("%d free pages\n", free);
	printk("%d reserved pages\n", reserved);
	printk("%d slab pages\n", slab);
	printk("%d pages shared\n", shared);
	printk("%d pages swap cached\n", cached);
}

/** 20130330    
 * meminfo의 entry 중 가장 낮은 pfn을 min에,
 * 가장 높은 pfn을 max_low, max_high에 저장한다.
 **/
static void __init find_limits(unsigned long *min, unsigned long *max_low,
			       unsigned long *max_high)
{
	struct meminfo *mi = &meminfo;
	int i;

	/* This assumes the meminfo array is properly sorted */
	/** 20130330    
	 * 첫번째 bank의 start 주소에 대한 pfn.
	 **/
	*min = bank_pfn_start(&mi->bank[0]);
	/** 20130330    
	 * for (i = 0; i < (mi)->nr_banks; i++)
	 **/
	for_each_bank (i, mi)
		if (mi->bank[i].highmem)
				break;
	/** 20130330    
	 * highmem이 true로 되어 break될 경우
	 *     max_low는 highmem bank 이전의 bank의 마지막 주소에 대한 pfn
	 *     max_high는 마지막 bank의 마지막 주소에 대한 pfn
	 * 그렇지 않을 경우 max_low와 max_high는 같음
	 **/
	*max_low = bank_pfn_end(&mi->bank[i - 1]);
	*max_high = bank_pfn_end(&mi->bank[mi->nr_banks - 1]);
}

/** 20130406    
 * 해당 노드에 대한 bootmem을 초기화 하고, memblock의 memory와 reserved에 해당하는 비트를 설정한다.
 **/
static void __init arm_bootmem_init(unsigned long start_pfn,
	unsigned long end_pfn)
{
	struct memblock_region *reg;
	unsigned int boot_pages;
	phys_addr_t bitmap;
	pg_data_t *pgdat;

	/*
	 * Allocate the bootmem bitmap page.  This must be in a region
	 * of memory which has already been mapped.
	 */
	/** 20130330    
	 * boot_pages는 pfn을 비트맵으로 표현하기 위해 필요한 페이지 수
	 **/
	boot_pages = bootmem_bootmap_pages(end_pfn - start_pfn);
	/** 20130330    
	 * 필요한 물리메모리의 크기 : boot_pages << PAGE_SHIFT
	 * 정렬 단위                : L1_CACHE_BYTES
	 * 할당 가능한 최대 물리 메모리 주소: __pfn_to_phys(end_pfn)
	 *
	 * bootmem을 비트맵으로 표현하기 위해 필요한 메모리를 할당 받아 저장
	 **/
	bitmap = memblock_alloc_base(boot_pages << PAGE_SHIFT, L1_CACHE_BYTES,
				__pfn_to_phys(end_pfn));

	/*
	 * Initialise the bootmem allocator, handing the
	 * memory banks over to bootmem.
	 */
	/** 20130330    
	 * vexpress는 null function
	 **/
	node_set_online(0);
	/** 20130330    
	 * pgdata <- &contig_page_data
	 **/
	pgdat = NODE_DATA(0);
	init_bootmem_node(pgdat, __phys_to_pfn(bitmap), start_pfn, end_pfn);

	/* Free the lowmem regions from memblock into bootmem. */
	/** 20130406    
	 * memblock 중 memory의 region을 순회
	 **/
	for_each_memblock(memory, reg) {
		/** 20130330    
		 * start는 round up한 시작 pfn, end는 round down한 끝 pfn.
		 * (정렬되지 않은 주소는 버려지는듯???)
		 **/
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		/** 20130330    
		 * 경계값 검사
		 **/
		if (end >= end_pfn)
			end = end_pfn;
		if (start >= end)
			break;

		/** 20130406
		 * 시작 주소와 크기(byte)를 전달.
		 * 해당 영역을 bootmem에서 usable 하게 설정
		 **/
		free_bootmem(__pfn_to_phys(start), (end - start) << PAGE_SHIFT);
	}

	/* Reserve the lowmem memblock reserved regions in bootmem. */
	/** 20130406    
	 * memblock 중 reserved의 region을 순회
	 **/
	for_each_memblock(reserved, reg) {
		unsigned long start = memblock_region_reserved_base_pfn(reg);
		unsigned long end = memblock_region_reserved_end_pfn(reg);

		if (end >= end_pfn)
			end = end_pfn;
		if (start >= end)
			break;

		/** 20130406    
		 * 해당 영역을 bootmem에서 reserve로 설정
		 **/
		reserve_bootmem(__pfn_to_phys(start),
			        (end - start) << PAGE_SHIFT, BOOTMEM_DEFAULT);
	}
}

#ifdef CONFIG_ZONE_DMA

unsigned long arm_dma_zone_size __read_mostly;
EXPORT_SYMBOL(arm_dma_zone_size);

/*
 * The DMA mask corresponding to the maximum bus address allocatable
 * using GFP_DMA.  The default here places no restriction on DMA
 * allocations.  This must be the smallest DMA mask in the system,
 * so a successful GFP_DMA allocation will always satisfy this.
 */
phys_addr_t arm_dma_limit;

/** 20130413
 * DMA 있는 경우, zone_size, zhole_size 를 조정한다.
 */
static void __init arm_adjust_dma_zone(unsigned long *size, unsigned long *hole,
	unsigned long dma_size)
{
	if (size[0] <= dma_size)
		return;

	size[ZONE_NORMAL] = size[0] - dma_size;
	size[ZONE_DMA] = dma_size;
	hole[ZONE_NORMAL] = hole[0];
	hole[ZONE_DMA] = 0;
}
#endif

void __init setup_dma_zone(struct machine_desc *mdesc)
{
#ifdef CONFIG_ZONE_DMA
	if (mdesc->dma_zone_size) {
		arm_dma_zone_size = mdesc->dma_zone_size;
		arm_dma_limit = PHYS_OFFSET + arm_dma_zone_size - 1;
	} else
		arm_dma_limit = 0xffffffff;
#endif
}

/** 20130511 
 * lowmem pfn의 최소, 최대, highmem pfn의 최대값을 받아
 * 각 zone의 크기(pfn수)를 계산하고 (hole을 포함한 것과 포함하지 않은 것 계산),
 * 0번 node에 대해서 free 상태로 초기화 작업을 수행한다.
 **/
static void __init arm_bootmem_free(unsigned long min, unsigned long max_low,
	unsigned long max_high)
{
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	struct memblock_region *reg;

	/*
	 * initialise the zones.
	 */
	memset(zone_size, 0, sizeof(zone_size));

	/*
	 * The memory size has already been determined.  If we need
	 * to do anything fancy with the allocation of this memory
	 * to the zones, now is the time to do it.
	 */
	/** 20130413
	 * ZONE_NORMAL (0) 에 pfn의 수를 저장 (lowmem의 최대값 - lowmem의 최소값)
	 * vexperss 에서는 2개 ZONE 존재 (ZONE_NORMAL, ZONE_MOVABLE)
	 */
	zone_size[0] = max_low - min;
#ifdef CONFIG_HIGHMEM
	zone_size[ZONE_HIGHMEM] = max_high - max_low;
#endif

	/*
	 * Calculate the size of the holes.
	 *  holes = node_size - sum(bank_sizes)
	 */
	memcpy(zhole_size, zone_size, sizeof(zhole_size));
	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		/** 20130413
		 * zhole_size 는 zone_normal size 에서 memory region 의 size를 뺀 것.
		 */
		if (start < max_low) {
			unsigned long low_end = min(end, max_low);
			zhole_size[0] -= low_end - start;
		}
#ifdef CONFIG_HIGHMEM
		if (end > max_low) {
			unsigned long high_start = max(start, max_low);
			zhole_size[ZONE_HIGHMEM] -= end - high_start;
		}
#endif
	}

#ifdef CONFIG_ZONE_DMA
	/*
	 * Adjust the sizes according to any special requirements for
	 * this machine type.
	 */
	if (arm_dma_zone_size)
		arm_adjust_dma_zone(zone_size, zhole_size,
			arm_dma_zone_size >> PAGE_SHIFT);
#endif

	/** 20140511
	 * node 0의 zone 정보를 채우고, page 구조체 정보를 초기화하고 예약상태로 표시한다.
	 **/
	free_area_init_node(0, zone_size, min, zhole_size);
}

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
/** 20130518    
 * pfn이 물리 메모리 내에 속하는지 검사하는 루틴
 **/
int pfn_valid(unsigned long pfn)
{
	return memblock_is_memory(__pfn_to_phys(pfn));
}
EXPORT_SYMBOL(pfn_valid);
#endif

#ifndef CONFIG_SPARSEMEM
static void __init arm_memory_present(void)
{
}
#else
static void __init arm_memory_present(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg)
		memory_present(0, memblock_region_memory_base_pfn(reg),
			       memblock_region_memory_end_pfn(reg));
}
#endif

static bool arm_memblock_steal_permitted = true;

phys_addr_t __init arm_memblock_steal(phys_addr_t size, phys_addr_t align)
{
	phys_addr_t phys;

	BUG_ON(!arm_memblock_steal_permitted);

	phys = memblock_alloc(size, align);
	memblock_free(phys, size);
	memblock_remove(phys, size);

	return phys;
}

/** 20130126    
 * meminfo  : cmdline에서 전달받은 물리적 메모리 구성 정보
 * memblock : kernel이 관리하는 logical memory blocks
 *
 * 1. meminfo의 정보를 memblock.memory에 등록
 * 2. 커널 실행코드 영역, initrd, swapper_pg_dir, machine specific 한 영역을 memblock.reserved에 등록
 * 3. 전역변수 arm_memblock_steal_permitted, memblock_can_resize 를 설정
 **/
void __init arm_memblock_init(struct meminfo *mi, struct machine_desc *mdesc)
{
	int i;
	/** 20130126    
	 * meminfo의 bank 정보를 memblock의 memory 부분에 채워넣음
	 **/
	for (i = 0; i < mi->nr_banks; i++)
		memblock_add(mi->bank[i].start, mi->bank[i].size);

	/* Register the kernel text, kernel data and initrd with memblock. */
#ifdef CONFIG_XIP_KERNEL
	memblock_reserve(__pa(_sdata), _end - _sdata);
#else
	/** 20130126    
	 * kernel의 실행코드 (text~bss까지)를 memblock의 reserved 영역에 등록
	 **/
	memblock_reserve(__pa(_stext), _end - _stext);
#endif
#ifdef CONFIG_BLK_DEV_INITRD
	/** 20130126    
	 * initrd로 주어진 메모리 공간이 memblock.memory 영역 안에 없다면 initrd를 무시
	 **/
	/** 20130810
	  initrd지정된 영역이 region memory에 존재 하지 않을경우
	  유효하지 않는 주소로 판단하고 무시
	 **/
	if (phys_initrd_size &&
	    !memblock_is_region_memory(phys_initrd_start, phys_initrd_size)) {
		pr_err("INITRD: 0x%08lx+0x%08lx is not a memory region - disabling initrd\n",
		       phys_initrd_start, phys_initrd_size);
		phys_initrd_start = phys_initrd_size = 0;
	}
	/** 20130126    
	 * initrd로 주어진 메모리 공간이 memblock.reserved 영역과 겹쳐진다면 initrd를 무시
	 **/
	if (phys_initrd_size &&
	    memblock_is_region_reserved(phys_initrd_start, phys_initrd_size)) {
		pr_err("INITRD: 0x%08lx+0x%08lx overlaps in-use memory region - disabling initrd\n",
		       phys_initrd_start, phys_initrd_size);
		phys_initrd_start = phys_initrd_size = 0;
	}
	/** 20130810
	 * 그리고 initrd_size가 있다면.. 
	 **/
	if (phys_initrd_size) {
		/** 20130126    
		 * initrd 메모리 영역을 memblock.reserved에 등록
		 **/
		memblock_reserve(phys_initrd_start, phys_initrd_size);

		/* Now convert initrd to virtual addresses */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}
#endif

	/** 20130126    
	 * swapper_pg_dir 영역을 memblock.reseved에 추가
	 **/
	arm_mm_memblock_reserve();
	/** 20130126    
	 * device tree를 memblock.reserved에 추가하는 듯.
	 * vexpress에서는 NULL 함수
	 **/
	arm_dt_memblock_reserve();

	/* reserve any platform specific memblock areas */
	/** 20130126
	 * vexpress는 지정되어 있지 않지만,
	 * machine에 따라 각각 따로 처리해야 할 메모리 영역을 예약하는 함수를 호출한다.
	 *
	 * 예를 들어 exynos 코어를 사용하는 ORIGEN 머신의 경우
	 * MACHINE_START(ORIGEN, "ORIGEN")
	 * .reserve    = &origen_reserve,
	 *
	 * s5p_mfc_reserve_mem(0x43000000, 8 << 20, 0x51000000, 8 << 20);
	 * mfc (영상 코덱 ip)용으로 사용되는 버퍼 영역을 reserve 함수로 처리한다.
	 **/
	if (mdesc->reserve)
		mdesc->reserve();

	/*
	 * reserve memory for DMA contigouos allocations,
	 * must come from DMA area inside low memory
	 */
	/** 20130309    
	 * CONFIG_CMA가 not defined이므로 NULL을 리턴
	 **/
	dma_contiguous_reserve(min(arm_dma_limit, arm_lowmem_limit));

	/** 20130126    
	 * memblock steal을 불가능하게 처리
	 **/
	arm_memblock_steal_permitted = false;
	/** 20130126    
	 * memblock resize 허용
	 **/
	memblock_allow_resize();
	memblock_dump_all();
}

/** 20130511 
 * 1. meminfo로부터 pfn 구간을 구한다.
 * 2. 물리 영역에 대해 bootmem bitmap을 초기화하고 설정한다.
 *    페이징이 시작되기 전에 boot time에서 메모리를 사용하기 위한 초기화 작업.
 **/
void __init bootmem_init(void)
{
	unsigned long min, max_low, max_high;

	max_low = max_high = 0;

	/** 20130330    
	 * meminfo로부터 min (첫 pfn), max_low (마지막 pfn), max_high를 채움
	 **/
	find_limits(&min, &max_low, &max_high);

	/** 20130406    
	 * find_limits에서 가져온 물리 영역에 대해 bootmem bitmap을 초기화하고 설정.
	 **/
	arm_bootmem_init(min, max_low);

	/*
	 * Sparsemem tries to allocate bootmem in memory_present(),
	 * so must be done after the fixed reservations
	 */
	/** 20130406    
	 * SPARSEMEM이 설정되지 않았을 경우 NULL 함수
	 **/
	arm_memory_present();

	/*
	 * sparse_init() needs the bootmem allocator up and running.
	 */
	/** 20130406    
	 * SPARSEMEM이 설정되지 않았을 경우 NULL 함수
	 **/
	sparse_init();

	/*
	 * Now free the memory - free_area_init_node needs
	 * the sparse mem_map arrays initialized by sparse_init()
	 * for memmap_init_zone(), otherwise all PFNs are invalid.
	 */
	/** 20130511
	 * zone size 계산하고 저장한 후 0번 node에 대해서 초기화 작업을 수행한다.
	 * (zone과 zone에 속하는 page frame의 page 구조체 등 초기화)
	 **/
	arm_bootmem_free(min, max_low, max_high);

	/*
	 * This doesn't seem to be used by the Linux memory manager any
	 * more, but is used by ll_rw_block.  If we can get rid of it, we
	 * also get rid of some of the stuff above as well.
	 *
	 * Note: max_low_pfn and max_pfn reflect the number of _pages_ in
	 * the system, not the maximum PFN.
	 */

	/** 20130511 
	 * 커널이 사용하는 물리 주소의 pfn 수
	 * max_low_pfn : 커널이 사용하는 (low memory 내에서의)pfn의 수
	 * max_pfn : max_low_pfn+highmem pfn의 수
	 *   (만약 highmem이 없을 경우는 max_low_pfn과 max_pfn는 같다. )
	 **/	
	max_low_pfn = max_low - PHYS_PFN_OFFSET;
	max_pfn = max_high - PHYS_PFN_OFFSET;
}

/** 20130907    
 * pfn부터 end 사이 영역을 free시키고 해제된 페이지 수를 리턴하는 함수
 **/
static inline int free_area(unsigned long pfn, unsigned long end, char *s)
{
	/** 20130907    
	 * PAGE_SHIFT - 10을 한 이유는 크기를 K 단위로 출력해 주기 위함
	 **/
	unsigned int pages = 0, size = (end - pfn) << (PAGE_SHIFT - 10);

	for (; pfn < end; pfn++) {
		/** 20130907    
		 * pfn에 해당하는 struct page *를 가져옴
		 **/
		struct page *page = pfn_to_page(pfn);
		/** 20130907    
		 * page->flags에서 PG_reserved를 clear
		 **/
		ClearPageReserved(page);
		/** 20130907    
		 * reference count를 초기화하고,
		 * page를 해제한다.
		 **/
		init_page_count(page);
		__free_page(page);
		/** 20130907    
		 * 초기화한 page 수 counting
		 **/
		pages++;
	}

	if (size && s)
		printk(KERN_INFO "Freeing %s memory: %dK\n", s, size);

	return pages;
}

/*
 * Poison init memory with an undefined instruction (ARM) or a branch to an
 * undefined instruction (Thumb).
 */
static inline void poison_init_mem(void *s, size_t count)
{
	u32 *p = (u32 *)s;
	for (; count != 0; count -= 4)
		*p++ = 0xe7fddef0;
}

/** 20130803    
 * start_pfn과 end_pfn 사이 물리 영역을 bootmem에서 초기화 한다.
 **/
static inline void
free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	/** 20130803    
	 * start_pfn, end_pfn에 대한 struct page *를 구한다.
	 *
	 * CONFIG_FLATMEM에서는 연속적이므로 -1을 해 구한 뒤 +1을 해줄 필요가 없지만,
	 * 다른 메모리 타입일 경우 -1을 해 변환한 뒤 +1을 해야 정확한 위치를 찾는듯???
	 **/
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and
	 * round start upwards and end downwards.
	 */
	/** 20130803    
	 * start_pg를 물리 주소로 변환하여 PAGE 단위로 ALIGN(올림)을 맞춘다.
	 * end_pg를 물리 주소로 변환하여 PAGE 단위로 내림을 수행한다.
	 **/
	pg = (unsigned long)PAGE_ALIGN(__pa(start_pg));
	pgend = (unsigned long)__pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these,
	 * free the section of the memmap array.
	 */
	/** 20130803    
	 * 실제 physical address로 정렬해 비교했을 때도 pgend가 pg보다 크다면
	 *   bootmem bitmap 영역에서 free시킨다.
	 **/
	if (pg < pgend)
		free_bootmem(pg, pgend - pg);
}

/*
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
/** 20130803    
 * meminfo 를 순회하며 각 bank 사이의 연속적이지 않은 공간에 대해 free.
 **/
static void __init free_unused_memmap(struct meminfo *mi)
{
	unsigned long bank_start, prev_bank_end = 0;
	unsigned int i;

	/*
	 * This relies on each bank being in address order.
	 * The banks are sorted previously in bootmem_init().
	 */
	for_each_bank(i, mi) {
		struct membank *bank = &mi->bank[i];

		/** 20130803    
		 * 각 bank의 시작주소에 해당하는 PFN를 구한다.
		 **/
		bank_start = bank_pfn_start(bank);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist
		 * due to SPARSEMEM sections which aren't present.
		 */
		bank_start = min(bank_start,
				 ALIGN(prev_bank_end, PAGES_PER_SECTION));
#else
		/*
		 * Align down here since the VM subsystem insists that the
		 * memmap entries are valid from the bank start aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		/** 20130803    
		 * bank_start를 MAX_ORDER_NR_PAGES 단위로 내림한다.
		 *   올림을 수행해야 하는 것이 아닐까???
		 **/
		bank_start = round_down(bank_start, MAX_ORDER_NR_PAGES);
#endif
		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		/** 20130803    
		 * 처음 loop 수행시 prev_bank_end는 0.
		 * 다음 수행시 prev_bank_end가 현재 bank_start 보다 작으면
		 *   해당 영역을 free로 표시(사용 가능) 한다.
		 **/
		if (prev_bank_end && prev_bank_end < bank_start)
			free_memmap(prev_bank_end, bank_start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		/** 20130803    
		 * 현재 bank의 끝 pfn을 MAX_ORDER_NR_PAGES 단위로 정렬한다. 
		 **/
		prev_bank_end = ALIGN(bank_pfn_end(bank), MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_bank_end, PAGES_PER_SECTION))
		free_memmap(prev_bank_end,
			    ALIGN(prev_bank_end, PAGES_PER_SECTION));
#endif
}

/** 20130907    
 * memblock의 memory 영역을 돌면서 1:1 매핑이 되어 있지 않은 high memory 영역에 대해
 * reserved 되어 있지 않은 영역에 대해 free_page를 호출.
 **/
static void __init free_highpages(void)
{
#ifdef CONFIG_HIGHMEM
	/** 20130907    
	 * max_low_pfn : 커널이 사용하는 pfn의 수
	 * PHYS_PFN_OFFSET : 커널 영역 물리 메모리 시작 주소에 해당하는 pfn
	 * highmem이 시작하는 pfn
	 **/
	unsigned long max_low = max_low_pfn + PHYS_PFN_OFFSET;
	struct memblock_region *mem, *res;

	/* set highmem page free */
	/** 20130907    
	 * memblock 의 memory 각 region에 대해 루프를 수행한다.
	 **/
	for_each_memblock(memory, mem) {
		unsigned long start = memblock_region_memory_base_pfn(mem);
		unsigned long end = memblock_region_memory_end_pfn(mem);

		/* Ignore complete lowmem entries */
		/** 20130907    
		 * 1:1 매핑 영역 안에 속한 region은 지나감
		 **/
		if (end <= max_low)
			continue;

		/* Truncate partial highmem entries */
		/** 20130907    
		 * 1:1 매핑 된 영역을 제외한 부분부터 시작 주소로 설정
		 **/
		if (start < max_low)
			start = max_low;

		/* Find and exclude any reserved regions */
		/** 20130907    
		 * reserved 영역이 포함되어 있는 경우, 그것을 제외한 영역에 대해 free_area.
		 *
		 * 빗금친 부분 free
		 *
		 * case 1)
		 * memblock.memory
		 * start                                  end
		 *  +---------------------------------------+
		 *  |     res_start                         |  res_end
		 *  |////////+-----------------------------------+
		 *  |////////|          memblock.reserve         |
		 *  |////////+-----------------------------------+
		 *  |                                       |
		 *  +---------------------------------------+
		 *
		 * case 2)
		 * memblock.memory
		 * start                                  end
		 *  +---------------------------------------+
		 *  |     res_start                         |
		 *  |////////+----------+////+--------+/////|
		 *  |////////|          |////|        +/////|
		 *  |////////+----------+////+--------+/////|
		 *  |                                       |
		 *  +---------------------------------------+
		 *
		 * ...
		 **/
		for_each_memblock(reserved, res) {
			unsigned long res_start, res_end;

			res_start = memblock_region_reserved_base_pfn(res);
			res_end = memblock_region_reserved_end_pfn(res);

			/** 20130907    
			 * reserved 영역이 모두 lowmem 영역에 속해 있다면 지나감
			 **/
			if (res_end < start)
				continue;
			/** 20130907    
			 * 걸쳐 있는 경우 start 위치 조정
			 **/
			if (res_start < start)
				res_start = start;
			if (res_start > end)
				res_start = end;
			if (res_end > end)
				res_end = end;
			if (res_start != start)
				totalhigh_pages += free_area(start, res_start,
							     NULL);
			start = res_end;
			if (start == end)
				break;
		}

		/** 20130907    
		 * reserved가 아닌 영역에 대해 free_area.
		 **/
		/* And now free anything which remains */
		if (start < end)
			totalhigh_pages += free_area(start, end, NULL);
	}
	/** 20130907    
	 * highmem에서 free시킨 페이지 수를 totalram_pages에 누적시킨다.
	 **/
	totalram_pages += totalhigh_pages;
#endif
}

/*
 * mem_init() marks the free areas in the mem_map and tells us how much
 * memory is free.  This is done after various parts of the system have
 * claimed their memory after the kernel image.
 */
/** 20130907    
 * bootmem에서 사용 중이지 않은 공간과 bootmem 비트맵이 사용하던 공간을 free시키고, buddy에서 사용할 free_list에 추가한다.
 * CONFIG_HIGHMEM이 정의되어 있는 경우 highmem 영역에 속한 영역 중 memblock 의 memory region 중 reserved 되지 않은 영역을 free.
 * totalram_pages가 free한 pages를 저장한다.
 **/
void __init mem_init(void)
{
	unsigned long reserved_pages, free_pages;
	struct memblock_region *reg;
	int i;
	/** 20130803    
	 * vexpress에서 정의되어 있지 않음
	 **/
#ifdef CONFIG_HAVE_TCM
	/* These pointers are filled in on TCM detection */
	extern u32 dtcm_end;
	extern u32 itcm_end;
#endif

	/** 20130803    
	 * PFN으로 해당하는 page 구조체를 찾는다.
	 *   max_pfn         : 최대 pfn의 수
	 *   PHYS_PFN_OFFSET : 커널 시작 주소에 대한 PFN
	 *
	 *   mem_map         : struct page 들의 시작 위치
	 *
	 * 시작 struct page *에서 마지막 struct page *를 빼서
	 * page의 개수를 max_mapnr에 저장
	 **/
	max_mapnr   = pfn_to_page(max_pfn + PHYS_PFN_OFFSET) - mem_map;

	/* this will put all unused low memory onto the freelists */
	free_unused_memmap(&meminfo);

	/** 20130907
	 * bitmap이 위치한 struct page 구조체를 free.
	 * bitmap 자체를 정리한 것은 아님.
	 **/
	totalram_pages += free_all_bootmem();

#ifdef CONFIG_SA1111
	/* now that our DMA memory is actually so designated, we can free it */
	totalram_pages += free_area(PHYS_PFN_OFFSET,
				    __phys_to_pfn(__pa(swapper_pg_dir)), NULL);
#endif

	free_highpages();

	/** 20130907    
	 * reserved_pages와 free_pages를 0으로 초기화 
	 **/
	reserved_pages = free_pages = 0;

	for_each_bank(i, &meminfo) {
		struct membank *bank = &meminfo.bank[i];
		unsigned int pfn1, pfn2;
		struct page *page, *end;

		/** 20130907    
		 * bank의 시작 pfn과 마지막 pfn을 가져옴
		 **/
		pfn1 = bank_pfn_start(bank);
		pfn2 = bank_pfn_end(bank);

		/** 20130907    
		 * pfn을 struct page * 주소값으로 변환
		 * pfn2는 bank를 넘어선 주소이므로 마지막 bank에 대한 page 값을 가져와 1을 더한다.
		 **/
		page = pfn_to_page(pfn1);
		end  = pfn_to_page(pfn2 - 1) + 1;

		/** 20130907    
		 * bank의 각 pfn을 돌면서 free_pages와 reserved_pages를 각각 counting.
		 **/
		do {
			/** 20130907    
			 * reserved된 page의 수를 counting.
			 **/
			if (PageReserved(page))
				reserved_pages++;
			/** 20130907    
			 * page의 _count가 0이라면 사용되지 않는 page이다.
			 **/
			else if (!page_count(page))
				free_pages++;
			/** 20130907    
			 * 다음 page를 가리킴
			 **/
			page++;
		} while (page < end);
	}

	/*
	 * Since our memory may not be contiguous, calculate the
	 * real number of pages we have in this system
	 */
	/** 20130907    
	 * vexpress의 출력 예
	 * 
	Memory: 1024MB = 1024MB total
	Memory: 1032552k/1032552k available, 16024k reserved, 0K highmem
	Virtual kernel memory layout:
		vector  : 0xffff0000 - 0xffff1000   (   4 kB)
		fixmap  : 0xfff00000 - 0xfffe0000   ( 896 kB)
		vmalloc : 0xc0800000 - 0xff000000   (1000 MB)
		lowmem  : 0x80000000 - 0xc0000000   (1024 MB)
		modules : 0x7f000000 - 0x80000000   (  16 MB)
		  .text : 0x80008000 - 0x804450f0   (4341 kB)
		  .init : 0x80446000 - 0x804729c0   ( 179 kB)
		  .data : 0x80474000 - 0x804a2be0   ( 187 kB)
		   .bss : 0x804a2c04 - 0x804c13b8   ( 122 kB)
	 *
	 **/
	printk(KERN_INFO "Memory:");
	num_physpages = 0;
	/** 20130907    
	 * memblock 의 memory region에 속한 pages의 수를 구해 num_physpages에 누적시킨다.
	 **/
	for_each_memblock(memory, reg) {
		unsigned long pages = memblock_region_memory_end_pfn(reg) -
			memblock_region_memory_base_pfn(reg);
		num_physpages += pages;
		printk(" %ldMB", pages >> (20 - PAGE_SHIFT));
	}
	printk(" = %luMB total\n", num_physpages >> (20 - PAGE_SHIFT));

	printk(KERN_NOTICE "Memory: %luk/%luk available, %luk reserved, %luK highmem\n",
		nr_free_pages() << (PAGE_SHIFT-10),
		free_pages << (PAGE_SHIFT-10),
		reserved_pages << (PAGE_SHIFT-10),
		totalhigh_pages << (PAGE_SHIFT-10));

#define MLK(b, t) b, t, ((t) - (b)) >> 10
#define MLM(b, t) b, t, ((t) - (b)) >> 20
#define MLK_ROUNDUP(b, t) b, t, DIV_ROUND_UP(((t) - (b)), SZ_1K)

	printk(KERN_NOTICE "Virtual kernel memory layout:\n"
			"    vector  : 0x%08lx - 0x%08lx   (%4ld kB)\n"
#ifdef CONFIG_HAVE_TCM
			"    DTCM    : 0x%08lx - 0x%08lx   (%4ld kB)\n"
			"    ITCM    : 0x%08lx - 0x%08lx   (%4ld kB)\n"
#endif
			"    fixmap  : 0x%08lx - 0x%08lx   (%4ld kB)\n"
			"    vmalloc : 0x%08lx - 0x%08lx   (%4ld MB)\n"
			"    lowmem  : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#ifdef CONFIG_HIGHMEM
			"    pkmap   : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#endif
#ifdef CONFIG_MODULES
			"    modules : 0x%08lx - 0x%08lx   (%4ld MB)\n"
#endif
			"      .text : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"      .init : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"      .data : 0x%p" " - 0x%p" "   (%4d kB)\n"
			"       .bss : 0x%p" " - 0x%p" "   (%4d kB)\n",

			MLK(UL(CONFIG_VECTORS_BASE), UL(CONFIG_VECTORS_BASE) +
				(PAGE_SIZE)),
#ifdef CONFIG_HAVE_TCM
			MLK(DTCM_OFFSET, (unsigned long) dtcm_end),
			MLK(ITCM_OFFSET, (unsigned long) itcm_end),
#endif
			MLK(FIXADDR_START, FIXADDR_TOP),
			MLM(VMALLOC_START, VMALLOC_END),
			MLM(PAGE_OFFSET, (unsigned long)high_memory),
#ifdef CONFIG_HIGHMEM
			MLM(PKMAP_BASE, (PKMAP_BASE) + (LAST_PKMAP) *
				(PAGE_SIZE)),
#endif
#ifdef CONFIG_MODULES
			MLM(MODULES_VADDR, MODULES_END),
#endif

			MLK_ROUNDUP(_text, _etext),
			MLK_ROUNDUP(__init_begin, __init_end),
			MLK_ROUNDUP(_sdata, _edata),
			MLK_ROUNDUP(__bss_start, __bss_stop));

#undef MLK
#undef MLM
#undef MLK_ROUNDUP

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can
	 * be detected at build time already.
	 */
#ifdef CONFIG_MMU
	/** 20130907    
	 * TASK_SIZE가 MODULES_VADDR보다 크면 BUG
	 **/
	BUILD_BUG_ON(TASK_SIZE				> MODULES_VADDR);
	BUG_ON(TASK_SIZE 				> MODULES_VADDR);
#endif

#ifdef CONFIG_HIGHMEM
	BUILD_BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE > PAGE_OFFSET);
	BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE	> PAGE_OFFSET);
#endif

	/** 20130907    
	 * PAGE_SIZE가 16KB 이상이고 num_physpages가 128 이하라면 OVERCOMMIT_ALWAYS 속성으로 지정한다.
	 **/
	if (PAGE_SIZE >= 16384 && num_physpages <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get
		 * anywhere without overcommit, so turn
		 * it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
#ifdef CONFIG_HAVE_TCM
	extern char __tcm_start, __tcm_end;

	poison_init_mem(&__tcm_start, &__tcm_end - &__tcm_start);
	totalram_pages += free_area(__phys_to_pfn(__pa(&__tcm_start)),
				    __phys_to_pfn(__pa(&__tcm_end)),
				    "TCM link");
#endif

	poison_init_mem(__init_begin, __init_end - __init_begin);
	if (!machine_is_integrator() && !machine_is_cintegrator())
		totalram_pages += free_area(__phys_to_pfn(__pa(__init_begin)),
					    __phys_to_pfn(__pa(__init_end)),
					    "init");
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd;

void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd) {
		poison_init_mem((void *)start, PAGE_ALIGN(end) - start);
		totalram_pages += free_area(__phys_to_pfn(__pa(start)),
					    __phys_to_pfn(__pa(end)),
					    "initrd");
	}
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif
