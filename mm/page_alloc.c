/*
 *  linux/mm/page_alloc.c
 *
 *  Manages the free list, the system allocates free pages here.
 *  Note that kmalloc() lives in slab.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *  Zone balancing, Kanoj Sarcar, SGI, Jan 2000
 *  Per cpu hot/cold page lists, bulk allocation, Martin J. Bligh, Sept 2002
 *          (lots of bits borrowed from Ingo Molnar & Andrew Morton)
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mempolicy.h>
#include <linux/stop_machine.h>
#include <linux/sort.h>
#include <linux/pfn.h>
#include <linux/backing-dev.h>
#include <linux/fault-inject.h>
#include <linux/page-isolation.h>
#include <linux/page_cgroup.h>
#include <linux/debugobjects.h>
#include <linux/kmemleak.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <linux/ftrace_event.h>
#include <linux/memcontrol.h>
#include <linux/prefetch.h>
#include <linux/migrate.h>
#include <linux/page-debug-flags.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>
#include "internal.h"

#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
DEFINE_PER_CPU(int, numa_node);
EXPORT_PER_CPU_SYMBOL(numa_node);
#endif

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * N.B., Do NOT reference the '_numa_mem_' per cpu variable directly.
 * It will not be defined when CONFIG_HAVE_MEMORYLESS_NODES is not defined.
 * Use the accessor functions set_numa_mem(), numa_mem_id() and cpu_to_mem()
 * defined in <linux/topology.h>.
 */
DEFINE_PER_CPU(int, _numa_mem_);		/* Kernel "local memory" node */
EXPORT_PER_CPU_SYMBOL(_numa_mem_);
#endif

/*
 * Array of node states.
 */
nodemask_t node_states[NR_NODE_STATES] __read_mostly = {
	[N_POSSIBLE] = NODE_MASK_ALL,
	[N_ONLINE] = { { [0] = 1UL } },
#ifndef CONFIG_NUMA
	[N_NORMAL_MEMORY] = { { [0] = 1UL } },
#ifdef CONFIG_HIGHMEM
	[N_HIGH_MEMORY] = { { [0] = 1UL } },
#endif
	[N_CPU] = { { [0] = 1UL } },
#endif	/* NUMA */
};
EXPORT_SYMBOL(node_states);

/** 20130803    
 * mem_init 에서 초기화
 *
 * highmem page까지 포함한다.
 **/
unsigned long totalram_pages __read_mostly;
unsigned long totalreserve_pages __read_mostly;
/*
 * When calculating the number of globally allowed dirty pages, there
 * is a certain number of per-zone reserves that should not be
 * considered dirtyable memory.  This is the sum of those reserves
 * over all existing zones that contribute dirtyable memory.
 */
unsigned long dirty_balance_reserve __read_mostly;

int percpu_pagelist_fraction;
/** 20130907    
 * gfp_allowed_mask는 gfp_mask로 사용할 수 있는 속성들을 정의.
 * 초기값은 BOOT 중 사용할 수 있는 속성.
 **/
/** 20130914
 * 부팅 중 sleep, IO나 filesystem에 대한 접근은 허용되지 않는다.
 * #define GFP_BOOT_MASK (__GFP_BITS_MASK & ~(__GFP_WAIT|__GFP_IO|__GFP_FS))
 *
 * 20150523
 * kernel_init에서 __GFP_BITS_MASK로 설정된다.
 **/
gfp_t gfp_allowed_mask __read_mostly = GFP_BOOT_MASK;

#ifdef CONFIG_PM_SLEEP
/*
 * The following functions are used by the suspend/hibernate code to temporarily
 * change gfp_allowed_mask in order to avoid using I/O during memory allocations
 * while devices are suspended.  To avoid races with the suspend/hibernate code,
 * they should always be called with pm_mutex held (gfp_allowed_mask also should
 * only be modified with pm_mutex held, unless the suspend/hibernate code is
 * guaranteed not to run in parallel with that modification).
 */

static gfp_t saved_gfp_mask;

void pm_restore_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	if (saved_gfp_mask) {
		gfp_allowed_mask = saved_gfp_mask;
		saved_gfp_mask = 0;
	}
}

void pm_restrict_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	WARN_ON(saved_gfp_mask);
	saved_gfp_mask = gfp_allowed_mask;
	gfp_allowed_mask &= ~GFP_IOFS;
}

/** 20140628    
 * 허용된 mask에 IOFS가 남아 있다면 suspended_storage 상태는 아니다.
 **/
bool pm_suspended_storage(void)
{
	if ((gfp_allowed_mask & GFP_IOFS) == GFP_IOFS)
		return false;
	return true;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE
int pageblock_order __read_mostly;
#endif

static void __free_pages_ok(struct page *page, unsigned int order);

/*
 * results with 256, 32 in the lowmem_reserve sysctl:
 *	1G machine -> (16M dma, 800M-16M normal, 1G-800M high)
 *	1G machine -> (16M dma, 784M normal, 224M high)
 *	NORMAL allocation will leave 784M/256 of ram reserved in the ZONE_DMA
 *	HIGHMEM allocation will leave 224M/32 of ram reserved in ZONE_NORMAL
 *	HIGHMEM allocation will (224M+784M)/256 of ram reserved in ZONE_DMA
 *
 * TBD: should special case ZONE_DMA32 machines here - in those we normally
 * don't need any ZONE_NORMAL reservation
 */
int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES-1] = {
#ifdef CONFIG_ZONE_DMA
	 256,
#endif
#ifdef CONFIG_ZONE_DMA32
	 256,
#endif
#ifdef CONFIG_HIGHMEM
	 32,
#endif
	 32,
};

/** 20130907    
 * mem_init에서 free시켜준 page 수를 이 변수에 누적시킴.
 * 이후 free_XXX 류의 함수들을 통해 해제된 page들의 수를 누적
 **/
EXPORT_SYMBOL(totalram_pages);

/** 20130427    
 * zone index에 따른 이름을 리턴하기 위한 자료구조.
 * (vexpress에서는 "Normal", "Movable")
 **/
static char * const zone_names[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	 "DMA",
#endif
#ifdef CONFIG_ZONE_DMA32
	 "DMA32",
#endif
	 "Normal",
#ifdef CONFIG_HIGHMEM
	 "HighMem",
#endif
	 "Movable",
};

int min_free_kbytes = 1024;

static unsigned long __meminitdata nr_kernel_pages;
static unsigned long __meminitdata nr_all_pages;
static unsigned long __meminitdata dma_reserve;

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
static unsigned long __meminitdata arch_zone_lowest_possible_pfn[MAX_NR_ZONES];
static unsigned long __meminitdata arch_zone_highest_possible_pfn[MAX_NR_ZONES];
static unsigned long __initdata required_kernelcore;
static unsigned long __initdata required_movablecore;
static unsigned long __meminitdata zone_movable_pfn[MAX_NUMNODES];

/* movable_zone is the "real" zone pages in ZONE_MOVABLE are taken from */
int movable_zone;
EXPORT_SYMBOL(movable_zone);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

#if MAX_NUMNODES > 1
int nr_node_ids __read_mostly = MAX_NUMNODES;
int nr_online_nodes __read_mostly = 1;
EXPORT_SYMBOL(nr_node_ids);
EXPORT_SYMBOL(nr_online_nodes);
#endif

/** 20140517    
 * build_all_zonelists 에서 설정
 **/
int page_group_by_mobility_disabled __read_mostly;

/*
 * NOTE:
 * Don't use set_pageblock_migratetype(page, MIGRATE_ISOLATE) directly.
 * Instead, use {un}set_pageblock_isolate.
 */
/** 20130504
 * 해당 page에 대한 pageblock의 migratetype을 설정
 **/
void set_pageblock_migratetype(struct page *page, int migratetype)
{

	if (unlikely(page_group_by_mobility_disabled))
		migratetype = MIGRATE_UNMOVABLE;

	set_pageblock_flags_group(page, (unsigned long)migratetype,
					PB_migrate, PB_migrate_end);
}

bool oom_killer_disabled __read_mostly;

#ifdef CONFIG_DEBUG_VM
static int page_outside_zone_boundaries(struct zone *zone, struct page *page)
{
	int ret = 0;
	unsigned seq;
	unsigned long pfn = page_to_pfn(page);

	do {
		seq = zone_span_seqbegin(zone);
		if (pfn >= zone->zone_start_pfn + zone->spanned_pages)
			ret = 1;
		else if (pfn < zone->zone_start_pfn)
			ret = 1;
	} while (zone_span_seqretry(zone, seq));

	return ret;
}

static int page_is_consistent(struct zone *zone, struct page *page)
{
	if (!pfn_valid_within(page_to_pfn(page)))
		return 0;
	if (zone != page_zone(page))
		return 0;

	return 1;
}
/*
 * Temporary debugging check for pages not lying within a given zone.
 */
static int bad_range(struct zone *zone, struct page *page)
{
	if (page_outside_zone_boundaries(zone, page))
		return 1;
	if (!page_is_consistent(zone, page))
		return 1;

	return 0;
}
#else
/** 20130921    
 * CONFIG_DEBUG_VM 가 정의되어 있지 않은 경우 거짓 리턴.
 **/
static inline int bad_range(struct zone *zone, struct page *page)
{
	return 0;
}
#endif

/** 20130824    
 * bad_page 정보를 출력한다.
 *  add_taint는 분석 안 함
 **/
static void bad_page(struct page *page)
{
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	/* Don't complain about poisoned pages */
	/** 20130824    
	 * HWPoison은 정의되어 있지 않아 NULL을 0을 리턴
	 **/
	if (PageHWPoison(page)) {
		reset_page_mapcount(page); /* remove PageBuddy */
		return;
	}

	/*
	 * Allow a burst of 60 reports, then keep quiet for that minute;
	 * or allow a steady drip of one report per second.
	 */
	/** 20130824    
	 * 
	 **/
	if (nr_shown == 60) {
		/** 20130824    
		 * jiffies가 resume보다 작으면, 즉 아직 resume에 도달하지 않았으면
		 * nr_unshown을 증가시키고, out으로 빠진다.
		 **/
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			goto out;
		}
		/** 20130824    
		 * nr_unshown이 0보다 크면 
		 * ALERT를 출력하고 un_unshown을 초기화.
		 **/
		if (nr_unshown) {
			printk(KERN_ALERT
			      "BUG: Bad page state: %lu messages suppressed\n",
				nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	/** 20130824    
	 * nr_shown이 0인 경우
	 *   resume에 현재 보다 (60 * HZ)인 값을 저장한다.
	 **/
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	/** 20130824    
	 * 현재 prcoess의 commmand와 pfn을 출력
	 **/
	printk(KERN_ALERT "BUG: Bad page state in process %s  pfn:%05lx\n",
		current->comm, page_to_pfn(page));
	dump_page(page);

	print_modules();
	dump_stack();
out:
	/* Leave bad fields for debug, except PageBuddy could make trouble */
	/** 20130824    
	 * mapcount를 reset (-1) 한다.
	 **/
	reset_page_mapcount(page); /* remove PageBuddy */
	add_taint(TAINT_BAD_PAGE);
}

/*
 * Higher-order pages are called "compound pages".  They are structured thusly:
 *
 * The first PAGE_SIZE page is called the "head page".
 *
 * The remaining PAGE_SIZE pages are called "tail pages".
 *
 * All pages have PG_compound set.  All tail pages have their ->first_page
 * pointing at the head page.
 *
 * The first tail page's ->lru.next holds the address of the compound page's
 * put_page() function.  Its ->lru.prev holds the order of allocation.
 * This usage means that zero-order pages may not be compound.
 */

/** 20140614    
 * compound page에 대한 default destructor.
 * 일반적인 page free 함수를 호출한다.
 **/
static void free_compound_page(struct page *page)
{
	__free_pages_ok(page, compound_order(page));
}

/** 20131116    
 * page부터 order 만큼의 page를 compound page로 묶어준다.
 **/
void prep_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	/** 20131116    
	 * compound page의 destructor 함수를 free_compound_page로 지정한다.
	 **/
	set_compound_page_dtor(page, free_compound_page);
	/** 20131116    
	 * compound order를 page[1].lru.prev에 저장한다.
	 **/
	set_compound_order(page, order);
	/** 20131116    
	 * struct page 구조체의 flags 필드에 PG_head 비트를 설정해 head page임을 나타낸다.
	 **/
	__SetPageHead(page);
	/** 20131116    
	 * 나머지 페이지들에 대해서 각각의 page에
	 *   - PG_tail 비트를 설정한다.
	 *   - _count를 0으로 설정한다.
	 *   - first_page를 head page로 설정한다.
	 *
	 * CONFIG_PAGEFLAGS_EXTENDED가 설정된 경우
	 * [PG_head][PG_tail][PG_tail]...[PG_tail]
	 **/
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;
		__SetPageTail(p);
		set_page_count(p, 0);
		p->first_page = page;
	}
}

/* update __split_huge_page_refcount if you change this function */
/** 20131116    
 * compound page를 위한 각 page의 정보를 초기화 해
 * compound page를 각각의 page로 관리되게 한다.
 **/
static int destroy_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	int bad = 0;

	/** 20131116    
	 * order로 전달된 값과 page에 저장된 compound order의 값이 다르거나,
	 * page가 PageHead가 아닐 경우
	 *   bad_page로 page 정보를 출력하고, bad count를 하나 증가시킨다.
	 **/
	if (unlikely(compound_order(page) != order) ||
	    unlikely(!PageHead(page))) {
		bad_page(page);
		bad++;
	}

	/** 20131116    
	 * PageHead flag를 지워준다.
	 **/
	__ClearPageHead(page);

	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;

		/** 20131116    
		 * PageTail flag가 설정되어 있지 않거나 first_page가 넘어온 page와 같지 않다면 bad page 처리
		 * 참고: prep_compound_page 의 설정과정
		 **/
		if (unlikely(!PageTail(p) || (p->first_page != page))) {
			bad_page(page);
			bad++;
		}
		/** 20131116    
		 * PG_tail 비트를 flags에서 삭제한다.
		 **/
		__ClearPageTail(p);
	}

	/** 20131116    
	 * bad의 수를 리턴
	 **/
	return bad;
}

/** 20131116    
 * page부터 (1<<order)개만큼의 page를 0으로 초기화 시킨다.
 **/
static inline void prep_zero_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	/*
	 * clear_highpage() will use KM_USER0, so it's a bug to use __GFP_ZERO
	 * and __GFP_HIGHMEM from hard or soft interrupt context.
	 */
	/** 20131005    
	 * VM_BUG_ON은 선언의 주석을 참고.
	 **/
	/** 20131012
	 * in_interrupt()는 0을 리턴.컴파일시에서만 expression검사를 수행한다.
	 **/
	VM_BUG_ON((gfp_flags & __GFP_HIGHMEM) && in_interrupt());
    /** 20131012
	 * struct page *page부터 order개만큼의 page를 clear 시키는 함수
     **/
	for (i = 0; i < (1 << order); i++)
		clear_highpage(page + i);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
unsigned int _debug_guardpage_minorder;

static int __init debug_guardpage_minorder_setup(char *buf)
{
	unsigned long res;

	if (kstrtoul(buf, 10, &res) < 0 ||  res > MAX_ORDER / 2) {
		printk(KERN_ERR "Bad debug_guardpage_minorder value\n");
		return 0;
	}
	_debug_guardpage_minorder = res;
	printk(KERN_INFO "Setting debug_guardpage_minorder to %lu\n", res);
	return 0;
}
__setup("debug_guardpage_minorder=", debug_guardpage_minorder_setup);

static inline void set_page_guard_flag(struct page *page)
{
	__set_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}

static inline void clear_page_guard_flag(struct page *page)
{
	__clear_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}
#else
static inline void set_page_guard_flag(struct page *page) { }
static inline void clear_page_guard_flag(struct page *page) { }
#endif

/** 20130921    
 * page의 private에 order를 설정하고, private 멤버를 설정해 buddy에서 order 단위로 관리됨을 설정한다.
 **/
static inline void set_page_order(struct page *page, int order)
{
	/** 20130921    
	 * struct page의 private 멤버에 order를 넣어준다.
	 **/
	set_page_private(page, order);
	/** 20130921    
	 * page가 Buddy로 관리됨을 설정한다.
	 **/
	__SetPageBuddy(page);
}

/** 20130921    
 * page의 buddy 정보와 order정보를 초기화 한다. 
 **/
static inline void rmv_page_order(struct page *page)
{
	/** 20130921    
	 * page를 Buddy에 의해 관리되지 않음을 나타낸다.
	 * (해당 order로 나누어 떨어지는 첫번째 page만 __SetPageBuddy를 해준다.)
	 **/
	__ClearPageBuddy(page);
	/** 20130921    
	 * page의 private를 0으로 해준다. (order의 첫번째 page의 private 에 order를 넣어준다)
	 **/
	set_page_private(page, 0);
}

/*
 * Locate the struct page for both the matching buddy in our
 * pair (buddy1) and the combined O(n+1) page they form (page).
 *
 * 1) Any buddy B1 will have an order O twin B2 which satisfies
 * the following equation:
 *     B2 = B1 ^ (1 << O)
 * For example, if the starting buddy (buddy2) is #8 its order
 * 1 buddy is #10:
 *     B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *
 * 2) Any buddy B will have an order O+1 parent P which
 * satisfies the following equation:
 *     P = B & ~(1 << O)
 *
 * Assumption: *_mem_map is contiguous at least up to MAX_ORDER
 */
/** 20130921    
 * buddy의 index를 찾는다.
 * page_idx가 4이고,
 *   order가 0인 경우     4 ^ (1<<0) = 4 ^ 1 = 5
 *     (0) (1)|(2) (3)|(4) (5)|(6) (7)
 *
 *   order가 1인 경우     4 ^ (1<<1) = 4 ^ 2 = 6
 *     (0 1) (2 3)|(4 5) (6 7)
 *
 *   order가 2인 경우     4 ^ (1<<2) = 4 ^ 4 = 0
 *     (0 1 2 3) (4 5 6 7)|(8 9 10 11) (12 13 14 15)
 **/
static inline unsigned long
__find_buddy_index(unsigned long page_idx, unsigned int order)
{
	return page_idx ^ (1 << order);
}

/*
 * This function checks whether a page is free && is the buddy
 * we can do coalesce a page and its buddy if
 * (a) the buddy is not in a hole &&
 * (b) the buddy is in the buddy system &&
 * (c) a page and its buddy have the same order &&
 * (d) a page and its buddy are in the same zone.
 *
 * For recording whether a page is in the buddy system, we set ->_mapcount -2.
 * Setting, clearing, and testing _mapcount -2 is serialized by zone->lock.
 *
 * For recording page's order, we use page_private(page).
 */
/** 20130921    
 * page와 buddy가 order 레벨에서 buddy인지 판단하는 함수
 **/
static inline int page_is_buddy(struct page *page, struct page *buddy,
								int order)
{
	/** 20130921    
	 * CONFIG_HOLES_IN_ZONE 가 정의되어 있지 않아 항상 valid.
	 **/
	if (!pfn_valid_within(page_to_pfn(buddy)))
		return 0;

	/** 20130921    
	 * page와 buddy의 zoneid가 같지 않으면 0 리턴.
	 **/
	if (page_zone_id(page) != page_zone_id(buddy))
		return 0;

	/** 20130921    
	 * DEBUG가 켜 있지 않아 page_is_guard는 항상 false 리턴.
	 **/
	if (page_is_guard(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}

	/** 20130921    
	 * free_all_bootmem에서 온 경우 buddy는 PageBuddy가 아닌 상태.
	 * 따라서 해당 안 됨.
	 *
	 * 일반적인 경우 buddy가 Buddy에 의해 관리되는 page이고,
	 * buddy의 order가 현재 order와 같은 경우 return 1.
	 * (page_order는 set_page_order에서만 설정됨.
	 *  따라서 buddy는 이미 Buddy Allocator에 의해 free page.)
	 **/
	if (PageBuddy(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}
	return 0;
}

/*
 * Freeing function for a buddy system allocator.
 *
 * The concept of a buddy system is to maintain direct-mapped table
 * (containing bit values) for memory blocks of various "orders".
 * The bottom level table contains the map for the smallest allocatable
 * units of memory (here, pages), and each level above it describes
 * pairs of units from the levels below, hence, "buddies".
 * At a high level, all that happens here is marking the table entry
 * at the bottom level available, and propagating the changes upward
 * as necessary, plus some accounting needed to play nicely with other
 * parts of the VM system.
 * At each level, we keep a list of pages, which are heads of continuous
 * free pages of length of (1 << order) and marked with _mapcount -2. Page's
 * order is recorded in page_private(page) field.
 * So when we are allocating or freeing one, we can derive the state of the
 * other.  That is, if we allocate a small block, and both were
 * free, the remainder of the region must be split into blocks.
 * If a block is freed, and its buddy is also free, then this
 * triggers coalescing into a block of larger size.
 *
 * -- wli
 */

/** 20130831    
 * __free_pages_bootmem 에서 불렸지만, depth가 깊어 추후 다시 분석하기로 함 ???
 * free_all_bootmem_core 에서 호출될 경우 buddy가 관리하는 free_list에까지 넣어주는 것인지 확인 필요
 **/
/** 20130921
 * buddy allocator를 이용해 page를 order개만큼 해제하는 함수.
 * 가능한 연속적인 묶음을 만들기 위해 buddy page가 free이면 상위 order로 올라가 free.
**/
static inline void __free_one_page(struct page *page,
		struct zone *zone, unsigned int order,
		int migratetype)
{
	unsigned long page_idx;
	unsigned long combined_idx;
	unsigned long uninitialized_var(buddy_idx);
	struct page *buddy;

	/** 20130907    
	 * Compound page라면 destroy_compound_page로 풀고, 실패한다면 리턴.
	 **/
	if (unlikely(PageCompound(page)))
		if (unlikely(destroy_compound_page(page, order)))
			return;

	/** 20130907    
	 * migratetype
	 **/
	VM_BUG_ON(migratetype == -1);

	/** 20130921    
	 * struct page *page가 가리키는 pfn에서,
	 * 1 << MAX_ORDER (11)개의 하위 비트만 취해 인덱스로 삼는다.
	 **/
	page_idx = page_to_pfn(page) & ((1 << MAX_ORDER) - 1);

	/** 20130921    
	 * 계산한 page_idx가 요청한 order 단위로 안 떨어질 경우 BUG.
	 *
	 * 예를 들어 4개의 page 해제를 요청할 때, page_idx는 첫번째 페이지가 넘어와야 한다.
	 * 정렬되지 않은 2, 3, 4번째 page가 넘어온 경우에는 BUG.
	 **/
	VM_BUG_ON(page_idx & ((1 << order) - 1));
	/** 20130921    
	 * page가 zone의 범위 내에 없다면 bad_range.
	 **/
	VM_BUG_ON(bad_range(zone, page));

	/** 20130921    
	 * 현재 order부터 MAX_ORDER-1까지 반복(현재 커널 버전에서 MAX_ORDER는 11) 
	 **/
	while (order < MAX_ORDER-1) {
		/** 20130921    
		 * page_idx의 order 단위 buddy의 index를 구해온다.
		 **/
		buddy_idx = __find_buddy_index(page_idx, order);
		/** 20130921    
		 * struct page *page를 기준으로 buddy의 struct page *를 구한다.
		 **/
		buddy = page + (buddy_idx - page_idx);
		/** 20130921    
		 * page와 buddy가 order 레벨에서 buddy가 아닐 경우 false로 break
		 * (buddy에 의해 관리되는 free page일 경우)
		 **/
		if (!page_is_buddy(page, buddy, order))
			break;
		/*
		 * Our buddy is free or it is CONFIG_DEBUG_PAGEALLOC guard page,
		 * merge with it and move up one order.
		 */
		/** 20130921    
		 * CONFIG_DEBUG_PAGEALLOC 가 아닐 경우 항상 false.
		 **/
		if (page_is_guard(buddy)) {
			clear_page_guard_flag(buddy);
			set_page_private(page, 0);
			__mod_zone_page_state(zone, NR_FREE_PAGES, 1 << order);
		} else {
			/** 20130921    
			 * page와 buddy가 해당 order에서 buddy이므로 free 가 되면서
			 * 상위 order로 관리하기 위해 현재 order에서 빼준다.
			 **/
			/** 20130921    
			 * buddy page를 lru list에서 빼준다.
			 **/
			list_del(&buddy->lru);
			/** 20130921    
			 * 현재 order의 nr_free를 감소시킨다.
			 **/
			zone->free_area[order].nr_free--;
			/** 20130921    
			 * buddy의 order나 buddy 관리 정보를 초기화 한다.
			 *
			 * order = 2에서 page_idx가 4이고, buddy_idx가 0일 경우
			 * buddy_idx가 combinded_idx가 되어 새로운 page_idx로 될텐데,
			 * 기존의 page_idx로 rmv_page_order를 해줘야 하지 않을까?
			 **/
			rmv_page_order(buddy);
		}
		/** 20130921    
		 * combined_idx를 merge 상태의 첫번째 page의 idx를 가리키기 위해 변경한다.
		 *
		 * page_idx가 4이고, buddy_idx는 order에 따라 결정되고
		 *   order가 0인 경우     4 & 5 = 4
		 *     (0) (1)|(2) (3)|(4) (5)|(6) (7)
		 *
		 *   order가 1인 경우     4 & 6 = 4
		 *     (0 1) (2 3)|(4 5) (6 7)
		 *
		 *   order가 2인 경우     4 & 0 = 0
		 *     (0 1 2 3) (4 5 6 7)|(8 9 10 11) (12 13 14 15)
		 **/
		combined_idx = buddy_idx & page_idx;
		/** 20130921    
		 * page 역시 새로 지정된 combined_idx가 가리키는 page로 변경
		 **/
		page = page + (combined_idx - page_idx);
		/** 20130921    
		 * combined_idx를 새로 page_idx로 삼는다.
		 **/
		page_idx = combined_idx;
		/** 20130921    
		 * 상위 order로 이동한다.
		 **/
		order++;
	}
	/** 20130921    
	 * page의 order를 설정해 buddy에서 해당 order 단위로 관리됨으로 나타낸다.
	 * 처음 bootmem에서 buddy 이관시에는 page_is_buddy에서 빠져나와 이곳을 바로 수행한다.
	 **/
	set_page_order(page, order);

	/*
	 * If this is not the largest possible page, check if the buddy
	 * of the next-highest order is free. If it is, it's possible
	 * that pages are being freed that will coalesce soon. In case,
	 * that is happening, add the free page to the tail of the list
	 * so it's less likely to be used soon and more likely to be merged
	 * as a higher order page
	 */
	/** 20130921    
	 * order가 MAX_ORDER-2까지 도달하지 않았을 경우 다음 order의 buddy가 free인지 파악해
	 * free라면 현재 page를 현재 order에 free_list의 마지막에 추가한다.
	 * 즉, 현재 order의 page의 buddy가 free가 아니더라도(현재 order의 page는 free 함수이므로 항상 free)
	 * 곧 free될 가능성이 높으므로(확인 필요함???) 미리 free_list의 끝에 추가해 두는 것이다.
	 **/
	if ((order < MAX_ORDER-2) && pfn_valid_within(page_to_pfn(buddy))) {
		struct page *higher_page, *higher_buddy;
		/** 20130921    
		 * page_idx와 buddy_idx로 combined_idx를 구한다.
		 **/
		combined_idx = buddy_idx & page_idx;
		/** 20130921    
		 * combined_idx에 해당하는 page를 구해 higher_page에 저장.
		 **/
		higher_page = page + (combined_idx - page_idx);
		/** 20130921    
		 * ex) order 2인 상태에서
		 * buddy_idx: 0, page_idx: 4인 경우 combined_idx: 0.
		 *     order 3일 때의 buddy_idx를 구했으므로
		 * buddy_idx: 8
		 **/
		buddy_idx = __find_buddy_index(combined_idx, order + 1);
		/** 20130921    
		 * higher_buddy는 higher buddy_idx에 대한 struct page *.
		 **/
		higher_buddy = page + (buddy_idx - combined_idx);
		/** 20130921    
		 * higher_page와 higher_buddy로 다시 두 page가 buddy 관계일 때
		 * 현재 order의 free_list의 'tail'에 추가한다.
		 **/
		if (page_is_buddy(higher_page, higher_buddy, order + 1)) {
			list_add_tail(&page->lru,
				&zone->free_area[order].free_list[migratetype]);
			goto out;
		}
	}

	/** 20130921    
	 * 위의 경우가 아니라면 현재 page를 현재 order의 free_list의 처음에 넣어준다.
	 **/
	list_add(&page->lru, &zone->free_area[order].free_list[migratetype]);
out:
	/** 20130921    
	 * 현재 order 의 nr_free를 증가시킨다.
	 **/
	zone->free_area[order].nr_free++;
}

/*
 * free_page_mlock() -- clean up attempts to free and mlocked() page.
 * Page should not be on lru, so no need to fix that up.
 * free_pages_check() will verify...
 */
static inline void free_page_mlock(struct page *page)
{
	__dec_zone_page_state(page, NR_MLOCK);
	__count_vm_event(UNEVICTABLE_MLOCKFREED);
}

/** 20130824    
 * page가 사용 중이라면 1을 리턴, 사용 중이 아니라면 flags를 초기화 하고 0을 리턴.
 **/
static inline int free_pages_check(struct page *page)
{
	/** 20130824    
	 * mapcount가 0이 아니거나 mapping되어 있는 등 사용 중이라면
	 * bad_page로 처리하는듯 ???
	 **/
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0) |
		(page->flags & PAGE_FLAGS_CHECK_AT_FREE) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}
	/** 20130824    
	 * page->flags에 어떤 비트라도 설정되어 있다면 flags를 모두 날려준다.
	 * 항상 초기화 해주면 안 되나???
	 **/
	if (page->flags & PAGE_FLAGS_CHECK_AT_PREP)
		page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	return 0;
}

/*
 * Frees a number of pages from the PCP lists
 * Assumes all pages on list are in same zone, and of same order.
 * count is the number of pages to free.
 *
 * If the zone was previously in an "all pages pinned" state then look to
 * see if this freeing clears that state.
 *
 * And clear the zone's pages_scanned counter, to hold off the "all pages are
 * pinned" detection logic.
 */
/** 20130831    
 * per_cpu_pages 리스트 중 하나를 가져와 count 만큼 free 시킨다.
 *   - list에서 삭제
 *   - __free_one_page에서 실제 free (20130928 분석)
 *   - states에 반영
 **/
static void free_pcppages_bulk(struct zone *zone, int count,
					struct per_cpu_pages *pcp)
{
	int migratetype = 0;
	int batch_free = 0;
	/** 20130831    
	 * free_hot_cold_page 에서 count는 pcp->batch 단위.
	 **/
	int to_free = count;

	/** 20130928    
	 * zone 자료구조를 변경하기 위해 spin_lock을 건다.
	 **/
	spin_lock(&zone->lock);
	/** 20130928    
	 * reclaim시에 관련된 자료구조를 0으로 초기화 한다.
	 **/
	zone->all_unreclaimable = 0;
	zone->pages_scanned = 0;

	/** 20130831    
	 * to_free 개만큼 수행할 때까지 반복.
	 *   list가 empty이지만 to_free가 남았을 경우에 해당
	 **/
	while (to_free) {
		struct page *page;
		struct list_head *list;

		/*
		 * Remove pages from lists in a round-robin fashion. A
		 * batch_free count is maintained that is incremented when an
		 * empty list is encountered.  This is so more pages are freed
		 * off fuller lists instead of spinning excessively around empty
		 * lists
		 */
		/** 20130831    
		 * pcp->lists에서 MIGRATE_UNMOVABLE부터 MIGRATE_PCPTYPES 전까지 돌며
		 * lists를 성공적으로 가져올 때까지 반복한다.
		 * batch_free는 반복하기 전마다 증가한다.
		 **/
		do {
			batch_free++;
			if (++migratetype == MIGRATE_PCPTYPES)
				migratetype = 0;
			list = &pcp->lists[migratetype];
		} while (list_empty(list));

		/* This is the only non-empty list. Free them all. */
		/** 20130831    
		 * batch_free가 MIGRATE_PCPTYPES이라면 
		 * batch_free를 to_free로 지정
		 **/
		if (batch_free == MIGRATE_PCPTYPES)
			batch_free = to_free;

		do {
			/** 20130831    
			 * list의 prev가 가리키는 (cold page가 추가되는 tail에 해당) page를 가져온다.
			 **/
			page = list_entry(list->prev, struct page, lru);
			/* must delete as __free_one_page list manipulates */
			/** 20130831    
			 * 해당 page를 list에서 제거.
			 **/
			list_del(&page->lru);
			/* MIGRATE_MOVABLE list may include MIGRATE_RESERVEs */
			/** 20130831    
			 * 이 함수가 호출될 때 page_private에 page의 migratetype을 저장해 뒀음.
			 **/
			__free_one_page(page, zone, 0, page_private(page));
			trace_mm_page_pcpu_drain(page, 0, page_private(page));
		/** 20130831    
		 * 하나의 page를 free 했으므로 감소시킬 page (to_free)를 감소시켜 0이 되거나,
		 * free 할 단위인 batch_free 역시 감소시켜 0이 되거나,
		 * list가 모두 빌 때까지 반복한다.
		 **/
		} while (--to_free && --batch_free && !list_empty(list));
	}
	/** 20130831    
	 * zone의 NR_FREE_PAGES 동작에 관한 states에 count만큼 추가.
	 **/
	__mod_zone_page_state(zone, NR_FREE_PAGES, count);
	spin_unlock(&zone->lock);
}

/** 20130831    
 * __free_one_page는 buddy에 대한 내용이 있으므로 추후 다시 분석하기로 함
 *
 * 20130928
 * __free_one_page로 page부터 order 만큼의 페이지를 해제하고,
 * zone 구조체에 회수에 관련된 멤버변수를 초기화 하고, state에 free한 개수를 반영한다.
 **/
static void free_one_page(struct zone *zone, struct page *page, int order,
				int migratetype)
{
	/** 20130928    
	 * free_one_page 전에 local irq를 disable 한 상태.
	 *
	 * spin lock을 건다.
	 **/
	spin_lock(&zone->lock);
	/** 20130928    
	 * page 회수 불가를 0으로 설정한다.
	 **/
	zone->all_unreclaimable = 0;
	/** 20130928    
	 * reclaim 이 일어나므로 pages_scanned를 0으로 설정한다.
	 **/
	zone->pages_scanned = 0;

	/** 20130928    
	 * page부터 order 만큼을 해제한다.
	 **/
	__free_one_page(page, zone, order, migratetype);
	/** 20130928    
	 * NR_FREE_PAGES state에 free한 개수만큼 반영시킨다.
	 **/
	__mod_zone_page_state(zone, NR_FREE_PAGES, 1 << order);
	/** 20130928    
	 * spin lock을 해제한다.
	 **/
	spin_unlock(&zone->lock);
}

/** 20130810
 * page부터 1 << order 개수의 pages들을 free 하기 전에
 * free가 가능한지 검사하는 함수
 **/
static bool free_pages_prepare(struct page *page, unsigned int order)
{
	int i;
	int bad = 0;

	/** 20130803    
	 * Trace Point 생성
	 **/
	trace_mm_page_free(page, order);
	/** 20130803    
	 * CONFIG_KMEMCHECK가 정의되어 있지 않으므로 NULL 함수
	 **/
	kmemcheck_free_shadow(page, order);

	/** 20130824    
	 * page가 ANONYMOUS page인 경우 mapping을 NULL로 설정.
	 *   file로부터 생성된 page인 경우 non anonymous page. 그 외 anonymous page.
	 **/
	if (PageAnon(page))
		page->mapping = NULL;
	/** 20130824    
	 * order 만큼 돌면서 free_pages_check이 실패한 개수만큼 bad에 누적한다
	 **/
	for (i = 0; i < (1 << order); i++)
		bad += free_pages_check(page + i);
	/** 20130824    
	 * bad가 있다면 free 해줄 수 없다.
	 **/
	if (bad)
		return false;

	/** 20130824    
	 * page가 HighMem이 아닌 경우라면 (CONFIG_HIGHMEM이 꺼진 경우 포함)
	 * 
	 **/
	if (!PageHighMem(page)) {
		debug_check_no_locks_freed(page_address(page),PAGE_SIZE<<order);
		debug_check_no_obj_freed(page_address(page),
					   PAGE_SIZE << order);
	}
	/** 20130824    
	 * 두 함수 모두 NULL 함수
	 **/
	arch_free_page(page, order);
	kernel_map_pages(page, 1 << order, 0);

	return true;
}

/** 20130831    
 * struct page * page 부터 order 개의 page들을 free 하는 함수.
 * prepare가 실패하면 바로 리턴. 그 외 interrupt disable을 걸고 free를 수행.
 **/
static void __free_pages_ok(struct page *page, unsigned int order)
{
	unsigned long flags;
	/** 20130803    
	 * MACRO로 생성된 __TestClearPageMlocked 호출.
	 *   Mlocked bit를 clear 해주고, 이전 상태를 wasMlocked에 저장
	 **/
	int wasMlocked = __TestClearPageMlocked(page);

	/** 20130824    
	 * page부터  1<< order만큼 free를 위한 준비 과정에서 실패하면 바로 리턴.
	 **/
	if (!free_pages_prepare(page, order))
		return;

	/** 20130928    
	 * local cpu의 interrupt를 disable 한다.
	 **/
	local_irq_save(flags);
	/** 20130824    
	 * page의 속성이 mlocked인 경우 (mlock syscall 등으로 memory에 lock 된 경우)
	 **/
	if (unlikely(wasMlocked))
		free_page_mlock(page);
	__count_vm_events(PGFREE, 1 << order);
	free_one_page(page_zone(page), page, order,
					get_pageblock_migratetype(page));
	/** 20130928    
	 * local cpu의 interrupt를 복원한다.
	 **/
	local_irq_restore(flags);
}

/** 20130831    
 * page부터 order만큼의 pages를 해제하는 함수
 *   order 개의 pages에 대해
 *   - struct page flags에서 reserved를 클리어
 *   - page의 _count를 0으로 설정
 *   첫번째 page의 _count를 1로 만들어 order와 함께 __free_pages 호출
 **/
void __meminit __free_pages_bootmem(struct page *page, unsigned int order)
{
	unsigned int nr_pages = 1 << order;
	unsigned int loop;

	/** 20130803    
	 * page 영역의 데이터를 prefetch 시킨다.
	 **/
	prefetchw(page);
	/** 20130803    
	 * order에 해당하는 pages 개수만큼 순회하며
	 * 각 page에 해당하는 struct page의 flags에서 reserved를 클리어 해준다.
	 **/
	for (loop = 0; loop < nr_pages; loop++) {
		/** 20130803    
		 * 순회하는 page의 (struct page *) 주소를 p에 저장
		 **/
		struct page *p = &page[loop];

		/** 20130803    
		 * 마지막 page가 아니라면 다음 page를 prefetch 시킨다.
		 **/
		if (loop + 1 < nr_pages)
			prefetchw(p + 1);
		/** 20130803    
		 * p에 해당하는 struct page의 flags에서 PG_reserved 비트를 클리어.
		 **/
		__ClearPageReserved(p);
		/** 20130803    
		 * page의 _count를 0으로 설정함
		 **/
		set_page_count(p, 0);
	}

	/** 20130803    
	 * 첫번째 page에 대해서만 _count를 1로 설정한다.
	 * 20130907 _count를 하나 감소시키고 0이 되어야 실제 free를 하는 __free_pages의 구현방식을 맞춰주기 위해.
	 *
	 * 20130831
	 * __free_pages에서 다시 감소시키므로. 이 안에서 이전 값이 0이라면 VM_BUG_ON.
	 **/
	set_page_refcounted(page);
	/** 20130831    
	 * 실제 page를 free 하는 함수
	 **/
	__free_pages(page, order);
}

#ifdef CONFIG_CMA
/* Free whole pageblock and set it's migration type to MIGRATE_CMA. */
void __init init_cma_reserved_pageblock(struct page *page)
{
	unsigned i = pageblock_nr_pages;
	struct page *p = page;

	do {
		__ClearPageReserved(p);
		set_page_count(p, 0);
	} while (++p, --i);

	set_page_refcounted(page);
	set_pageblock_migratetype(page, MIGRATE_CMA);
	__free_pages(page, pageblock_order);
	totalram_pages += pageblock_nr_pages;
}
#endif

/*
 * The order of subdivision here is critical for the IO subsystem.
 * Please do not alter this order without good reasons and regression
 * testing. Specifically, as large blocks of memory are subdivided,
 * the order in which smaller blocks are delivered depends on the order
 * they're subdivided in this function. This is the primary factor
 * influencing the order in which pages are delivered to the IO
 * subsystem according to empirical testing, and this is also justified
 * by considering the behavior of a buddy system containing a single
 * large block of memory acted on by a series of small allocations.
 * This behavior is a critical factor in sglist merging's success.
 *
 * -- wli
 */
/** 20130928    
 * buddy allocation에서 상위 order의 free_list를 가져와 그 다음 하위 order의 lru 리스트에 추가시킨다.
 * high order에서 free pages를 가져와 low order까지 오면서 nr_free를 하나씩 증가.
 *
 * area : current_order에 해당하는 struct area *
 * order : 요청한 order
 * current_order : 사용 가능한 free pages 가져온(제공된) order
 * expand(zone, page, order, current_order, area, migratetype);
 **/
static inline void expand(struct zone *zone, struct page *page,
	int low, int high, struct free_area *area,
	int migratetype)
{
	/** 20130928    
	 * high order에 해당하는 page의 개수
	 **/
	unsigned long size = 1 << high;

	/** 20130928    
	 * high부터 low까지 돌면서...
	 *
	 *        |<-- struct page (loop를 돌아도 위치는 변하지 않음)
	 * high   [][][][][][][][]
	 *                ^ 이 page frame을 하위 order의 free_list에 추가.
	 *        [][][][]
	 *            ^ 이 page frame을 하위 order의 free_list에 추가.
	 *        [][]
	 *          ^ 이 page frame을 하위 order의 free_list에 추가.
	 * low    []
	 **/
	while (high > low) {
		/** 20130928    
		 * struct free_area와 high를 하나 감소시키고 수행.
		 * 여기부터 expand 할 order의 자료구조를 나타낸다.
		 **/
		area--;
		high--;
		/** 20130928    
		 * size는 1 감소
		 **/
		size >>= 1;
		/** 20130928    
		 * CONFIG_DEBUG_VM이 선언되어 있지 않는 경우에는 판단하지 않음
		 **/
		VM_BUG_ON(bad_range(zone, &page[size]));

		/** 20130928    
		 * DEBUG용
		 **/
#ifdef CONFIG_DEBUG_PAGEALLOC
		if (high < debug_guardpage_minorder()) {
			/*
			 * Mark as guard pages (or page), that will allow to
			 * merge back to allocator when buddy will be freed.
			 * Corresponding page table entries will not be touched,
			 * pages will stay not present in virtual address space
			 */
			INIT_LIST_HEAD(&page[size].lru);
			set_page_guard_flag(&page[size]);
			set_page_private(&page[size], high);
			/* Guard pages are not available for any usage */
			__mod_zone_page_state(zone, NR_FREE_PAGES, -(1 << high));
			continue;
		}
#endif
		/** 20130928    
		 * area의 free_list 중 migratetype에 해당하는 리스트에 (현재 order의 free_list)
		 * page를 추가시킨다.
		 * 이 때 추가할 페이지는 처음 매개변수로 넘어온 page에서 size만큼 떨어진 struct page이다.
		 *
		 * lru형식으로 관리되는 리스트인듯???
		 **/
		list_add(&page[size].lru, &area->free_list[migratetype]);
		/** 20130928    
		 * list에 entry가 추가되었으므로 nr_free를 증가시킨다.
		 **/
		area->nr_free++;
		/** 20130928    
		 * 하위 order에 추가할 page의 order를 현재 order로 설정한다.
		 **/
		set_page_order(&page[size], high);
	}
}

/*
 * This page is about to be returned from the page allocator
 */
/** 20131005    
 * new page라면 0, 사용 중이라면 1을 리턴
 **/
static inline int check_new_page(struct page *page)
{
	/** 20131005    
	 *  page_mapcount(page)					: page_mapcount가 0이 아니다.
		(page->mapping != NULL)				: page->mapping이 NULL이 아니다.
		(atomic_read(&page->_count) != 0)	: page->_count가 0이 아니다. (reference count)
		(page->flags & PAGE_FLAGS_CHECK_AT_PREP) : flags 속성 중 하나 이상이 설정되어 있다.
		(mem_cgroup_bad_page_check(page))	: cgroup bad page check 는 항상 false.

		즉 page가 사용된 정보가 하나라도 있다면 1을 리턴.
	 **/
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0)  |
		(page->flags & PAGE_FLAGS_CHECK_AT_PREP) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}
	return 0;
}

/** 20131116    
 * gfp_flags에 따라 새로 받아온 page들에 대해 관련된 자료구조를 초기화 해준다.
 *
 * 첫번째 page에만 private = 0, _count = 1로 설정한다.
 **/
static int prep_new_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	/** 20131005    
	 * page부터 1<<order만큼 loop를 수행해 new page인지 검사
	 **/
	for (i = 0; i < (1 << order); i++) {
		/** 20131005    
		 * 하나의 struct page *를 가져옴
		 **/
		struct page *p = page + i;
		/** 20131005    
		 * page가 사용 중이라면 check_new_page가 true가 되어 1로 리턴함.
		 **/
		if (unlikely(check_new_page(p)))
			return 1;
	}

	/** 20131005    
	 * page 구조체에 private에 0을 설정.
	 **/
	set_page_private(page, 0);
	/** 20131005    
	 * page의 _count 변수를 설정해 reference 상태로 만든다.
	 **/
	set_page_refcounted(page);

	/** 20131005    
	 * 둘 모두 NULL 함수
	 **/
	arch_alloc_page(page, order);
	kernel_map_pages(page, 1 << order, 1);

	/** 20131005    
	 * __GFP_ZERO 속성이 gfp_flags로 요청되었다면
	 * page의 내용을 0으로 초기화 한다.
	 **/
	if (gfp_flags & __GFP_ZERO)
		prep_zero_page(page, order, gfp_flags);

	/** 20131116    
	 * __GFP_COMP 속성이 gfp_flags로 요청되어 있고, order가 주어졌다면
	 * compound page로 묶어준다.
	 **/
	if (order && (gfp_flags & __GFP_COMP))
		prep_compound_page(page, order);

	return 0;
}

/*
 * Go through the free lists for the given migratetype and remove
 * the smallest available page from the freelists
 */
/** 20130928    
 * 요구된 order를 만족시키는 가장 낮은 order를 찾아 free_area 구조를 재구성한다.
 *   (재구성: order를 만족시키는 가장 낮은 order에서 뺀 뒤 그보다 낮은 order에 추가)
 * 작업이 일어난 page를 리턴한다.
 **/
static inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
						int migratetype)
{
	unsigned int current_order;
	struct free_area * area;
	struct page *page;

	/* Find a page of the appropriate size in the preferred list */
	/** 20130928    
	 * 전달받은 order부터 MAX_ORDER전까지 돌면서 free_list가 존재하는 가장 낮은 차수를 찾고,
	 * 찾는다면 바로 expand를 해주고 리턴한다.
	 **/
	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		/** 20130928    
		 * 현재 order에 대한 free_area를 가져온다.
		 **/
		area = &(zone->free_area[current_order]);
		/** 20130928    
		 * free_list 중 넘어온 migratetype에 해당하는 list를 가져와
		 *   empty라면 다음 order에 대해 수행
		 **/
		if (list_empty(&area->free_list[migratetype]))
			continue;

		/** 20130928    
		 * struct page의 lru를 가리키는 area->free_list[migratetype].next가 포함된
		 * struct page를 하나 가져온다.
		 *
		 * 여기부터 expand 호출 전까지
		 *   현재 order의 free_list에서 page를 제거하고 관련 정보를 초기화 하는 부분.
		 **/
		page = list_entry(area->free_list[migratetype].next,
							struct page, lru);
		/** 20130928    
		 * page의 lru에서 제거한다.
		 **/
		list_del(&page->lru);
		/** 20130928    
		 * page를 buddy에서 제거하고 관련된 자료구조를 초기화 한다.
		 **/
		rmv_page_order(page);
		/** 20130928    
		 * area에서 nr_free를 줄여준다.
		 **/
		area->nr_free--;
		/** 20130928    
		 * curret_order에서 order까지 order를 하나씩 감소시키며 free_list에 추가한다.
		 **/
		expand(zone, page, order, current_order, area, migratetype);
		/** 20130928    
		 * expand가 일어난 page를 바로 리턴한다.
		 **/
		return page;
	}

	/** 20130928    
	 * MAX_ORDER까지 돌았음에도 expand를 하지 못했다면 NULL을 리턴
	 **/
	return NULL;
}


/*
 * This array describes the order lists are fallen back to when
 * the free lists for the desirable migrate type are depleted
 */
/** 20131005    
 * 요청한 migratetype이 고갈되어 페이지를 가져올 수 없을 경우 대신
 * free lists를 찾을 migratetype의 순서를 정의한 array.
 **/
static int fallbacks[MIGRATE_TYPES][4] = {
	[MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE,     MIGRATE_RESERVE },
	[MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE,     MIGRATE_RESERVE },
#ifdef CONFIG_CMA
	[MIGRATE_MOVABLE]     = { MIGRATE_CMA,         MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_RESERVE },
	[MIGRATE_CMA]         = { MIGRATE_RESERVE }, /* Never used */
#else
	[MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE,   MIGRATE_RESERVE },
#endif
	[MIGRATE_RESERVE]     = { MIGRATE_RESERVE }, /* Never used */
	[MIGRATE_ISOLATE]     = { MIGRATE_RESERVE }, /* Never used */
};

/*
 * Move the free pages in a range to the free lists of the requested type.
 * Note that start_page and end_pages are not aligned on a pageblock
 * boundary. If alignment is required, use move_freepages_block()
 */
/** 20131005    
 * start_page에서 end_page까지 page를 옮길 수 있는 경우
 * zone의 free_area의 free_list[migratetype]로 옮기고
 * 이동시킨 page의 개수를 리턴한다.
 **/
static int move_freepages(struct zone *zone,
			  struct page *start_page, struct page *end_page,
			  int migratetype)
{
	struct page *page;
	unsigned long order;
	int pages_moved = 0;

#ifndef CONFIG_HOLES_IN_ZONE
	/*
	 * page_zone is not safe to call in this context when
	 * CONFIG_HOLES_IN_ZONE is set. This bug check is probably redundant
	 * anyway as we check zone boundaries in move_freepages_block().
	 * Remove at a later date when no bug reports exist related to
	 * grouping pages by mobility
	 */
	/** 20131005    
	 * start_page와 end_page가 같은 zone에 있지 않은 경우 BUG.
	 **/
	BUG_ON(page_zone(start_page) != page_zone(end_page));
#endif

	for (page = start_page; page <= end_page;) {
		/* Make sure we are not inadvertently changing nodes */
		/** 20131005    
		 * page와 zone이 같은 node에 있어야 한다.
		 **/
		VM_BUG_ON(page_to_nid(page) != zone_to_nid(zone));

		/** 20131005    
		 * pfn_valid_within은 1이므로 if문은 false.
		 **/
		if (!pfn_valid_within(page_to_pfn(page))) {
			page++;
			continue;
		}

		/** 20131005    
		 * page가 Buddy Allocator에 의해 관리되지 않는 경우
		 * 다음 page로 계속 시도
		 **/
		if (!PageBuddy(page)) {
			page++;
			continue;
		}

		/** 20131005    
		 * private에 설정한 order값을 가져온다.
		 **/
		order = page_order(page);
		/** 20131005    
		 * page를 lru에서 제거해 zone의 free_area의 free_list에 달아준다.
		 **/
		list_move(&page->lru,
			  &zone->free_area[order].free_list[migratetype]);
		/** 20131005    
		 * order 만큼 건너뛰어 다음 처리할 page를 가리킨다.
		 **/
		page += 1 << order;
		/** 20131005    
		 * 몇 개의 page를 옮겼는지 기록.
		 **/
		pages_moved += 1 << order;
	}

	return pages_moved;
}

/** 20131005    
 * page를 pageblock 단위로 새로운 zone의 free_area의 free_list[migratetype]으로 옮긴다.
 **/
int move_freepages_block(struct zone *zone, struct page *page,
				int migratetype)
{
	unsigned long start_pfn, end_pfn;
	struct page *start_page, *end_page;

	start_pfn = page_to_pfn(page);
	/** 20130928    
	 * start_pfn을 pageblock의 개수 단위로 정렬
	 **/
	start_pfn = start_pfn & ~(pageblock_nr_pages-1);
	/** 20130928    
	 * 정렬한 pfn에 해당하는 struct page*를 가져온다.
	 **/
	start_page = pfn_to_page(start_pfn);
	/** 20131005    
	 * end_page는 pageblock 개수만큼 떨어진 page.
	 **/
	end_page = start_page + pageblock_nr_pages - 1;
	end_pfn = start_pfn + pageblock_nr_pages - 1;

	/* Do not cross zone boundaries */
	/** 20131005
	 * 정렬로 인해 start_pfn이 zone의 첫번째 pfn보다 작으면
	 * page를 start_page로 설정한다.
	 **/
	if (start_pfn < zone->zone_start_pfn)
		start_page = page;
	/** 20131005    
	 * end_pfn이 zone의 마지막 페이지를 벗어난다면 pageblock 단위로
	 * 이동시키지 못하기 때문에 0을 리턴
	 **/
	if (end_pfn >= zone->zone_start_pfn + zone->spanned_pages)
		return 0;

	return move_freepages(zone, start_page, end_page, migratetype);
}

/** 20131005    
 * start_order의 page들을 pageblock 단위로 새로운 'migratetype'으로 옮겨준다.
 **/
static void change_pageblock_range(struct page *pageblock_page,
					int start_order, int migratetype)
{
	/** 20131005    
	 * pageblocks 단위로 몇 blocks까지 수행할지 연산.
	 **/
	int nr_pageblocks = 1 << (start_order - pageblock_order);

	/** 20131005    
	 * nr_pageblocks 번만큼 수행
	 **/
	while (nr_pageblocks--) {
		/** 20131005    
		 * pageblock_page의 migratetype을 'migratetype'으로 변경.
		 **/
		set_pageblock_migratetype(pageblock_page, migratetype);
		/** 20131005    
		 * 다음 pageblock 단위의 시작 위치를 나타내가 위해 변경.
		 **/
		pageblock_page += pageblock_nr_pages;
	}
}

/* Remove an element from the buddy allocator from the fallback list */
/** 20131005    
 * fallback을 시작하기 전의 migratetype이 start_migratetype이다.
 * start_migratetype의 fallbacks를 순회하며 free list에서 order 단위의 page를 가져온다.
 * 성공한다면 가져온 page를 리턴하고, 실패한다면 NULL을 리턴한다.
 **/
static inline struct page *
__rmqueue_fallback(struct zone *zone, int order, int start_migratetype)
{
	struct free_area * area;
	int current_order;
	struct page *page;
	int migratetype, i;

	/* Find the largest possible block of pages in the other list */
	/** 20130928    
	 * 최대 order부터 요청된 order까지 반복
	 **/
	for (current_order = MAX_ORDER-1; current_order >= order;
						--current_order) {
		for (i = 0;; i++) {
			/** 20130928    
			 * fallbacks 배열에서 start_migratetype에 해당하는 배열을 순회하며
			 * migratetype을 가져온다.
			 **/
			migratetype = fallbacks[start_migratetype][i];

			/* MIGRATE_RESERVE handled later if necessary */
			/** 20130928    
			 * fallbacks에서 가져온 migratetype이 MIGRATE_RESERVE일 경우는
			 * 다음 order에서 찾기 위해 loop을 빠져나간다.
			 **/
			if (migratetype == MIGRATE_RESERVE)
				break;

			/** 20130928    
			 * current_order의 area를 가져온다.
			 * 바깥 loop를 같이 놓고 보면, fallbacks의 migratetype을 순회하며
			 * current_order에 해당하는 area를 가져오게 된다.
			 **/
			area = &(zone->free_area[current_order]);
			/** 20130928    
			 * area의 migratetype에 해당하는 free_list를 가져오는데,
			 * 비어 있다면 다음 loop를 수행한다.
			 **/
			if (list_empty(&area->free_list[migratetype]))
				continue;

			/** 20130928    
			 * free_list[migratetype].next가 가리키는 lru를 포함하는 struct page *를 가져온다.
			 **/
			page = list_entry(area->free_list[migratetype].next,
					struct page, lru);
			/** 20130928    
			 * free의 개수 감소
			 **/
			area->nr_free--;

			/*
			 * If breaking a large block of pages, move all free
			 * pages to the preferred allocation list. If falling
			 * back for a reclaimable kernel allocation, be more
			 * aggressive about taking ownership of free pages
			 *
			 * On the other hand, never change migration
			 * type of MIGRATE_CMA pageblocks nor move CMA
			 * pages on different free lists. We don't
			 * want unmovable pages to be allocated from
			 * MIGRATE_CMA areas.
			 */
			/** 20130928    
			 * migratetype이 cma가 아니고,
			 *   current_order가 pageblock_order의 절반 이상이거나 (vexpress는 MAX_ORDER-1)
			 *   start_migratetype(원래 migratetype)이  MIGRATE_RECLAIMABLE이거나
			 *   page_group_by_mobility_disabled 이라면 (vexpress에 1기가를 할당한 경우 0)
			 * 실행
			 **/
			if (!is_migrate_cma(migratetype) &&
			    (unlikely(current_order >= pageblock_order / 2) ||
			     start_migratetype == MIGRATE_RECLAIMABLE ||
			     page_group_by_mobility_disabled)) {
				int pages;
				/** 20131005    
				 * page를 pageblock 단위로 start_migratetype이 가리키는
				 * free_list로 옮기고, 실제 옮겨진 개수를 pages에 저장한다.
				 **/
				pages = move_freepages_block(zone, page,
								start_migratetype);

				/* Claim the whole block if over half of it is free */
				/** 20131005    
				 * 옮겨진 page의 수가 1 << (pageblock_order-1) (512) 이상인 경우이거나
				 * page_group_by_mobility_disabled인 경우
				 **/
				if (pages >= (1 << (pageblock_order-1)) ||
						page_group_by_mobility_disabled)
					/** 20131005    
					 * page의 start_migratetype을 새롭게 지정한다.
					 **/
					set_pageblock_migratetype(page,
								start_migratetype);

				/** 20131005    
				 * page가 start_migratetype가 가리키는 free_list로 옮겨졌으므로
				 * 이후 migratetype은 start_migratetype으로 한다.
				 **/
				migratetype = start_migratetype;
			}

			/* Remove the page from the freelists */
			/** 20131005    
			 * page를 현재 lru가 가리키는 list에서 제거하고,
			 * buddy 정보와 order 정보를 초기화 한다.
			 **/
			list_del(&page->lru);
			rmv_page_order(page);

			/* Take ownership for orders >= pageblock_order */
			/** 20131005    
			 * current_order가 pageblock_order 이상이고,
			 * migratetype이 CMA가 아니라면 
			 *   pageblock_order 단위로 옮겨주고 start_migratetype로 변경한다.
			 **/
			if (current_order >= pageblock_order &&
			    !is_migrate_cma(migratetype))
				change_pageblock_range(page, current_order,
							start_migratetype);

			/** 20131005    
			 * current_order(high)에서 order(low)까지 순회하며
			 * buddy의 free_list의 하위 order의 free list에 추가한다.
			 *
			 * migrate가 CMA일 경우 기존의 migratetype에 해당하는 free list에 추가하고,
			 * CMA가 아닌 경우 start_migratetype에 해당하는 free list에 추가한다.
			 **/
			expand(zone, page, order, current_order, area,
			       is_migrate_cma(migratetype)
			     ? migratetype : start_migratetype);

			/** 20131005    
			 * TRACE 관련 함수는 분석하지 않음
			 **/
			trace_mm_page_alloc_extfrag(page, order, current_order,
				start_migratetype, migratetype);

			/** 20131005    
			 * 가져온 page를 리턴한다.
			 **/
			return page;
		}
	}

	return NULL;
}

/*
 * Do the hard work of removing an element from the buddy allocator.
 * Call me with the zone->lock already held.
 */
/** 20131005    
 * zone에서 migratetype에 해당하는 free list에서 최하 1<<order만큼의 page를 받아와
 * 리턴한다.
 * 첫번째 시도에서 실패했을 경우 __rmqueue_fallback으로 다른 migratetype으로 시도하고,
 * 다음 시도에서 역시 실패했을 경우 MIGRATE_RESERVE로 변경해서 __rmqueue_smallest를 다시 시도한다.
 **/
static struct page *__rmqueue(struct zone *zone, unsigned int order,
						int migratetype)
{
	struct page *page;

retry_reserve:
	/** 20130928    
	 * 해당 zone으로부터 order를 제공가능한 차수를 찾아 migratetype의 리스트에서
	 * page를 가져와 하위 차수에 추가하고, 그 order의 첫번째 page를 리턴 받는다.
	 **/
	page = __rmqueue_smallest(zone, order, migratetype);

	/** 20130928    
	 * page를 가져오는데 실패했고, 요청한 migratetype이 MIGRATE_RESERVE이 아니라면
	 **/
	if (unlikely(!page) && migratetype != MIGRATE_RESERVE) {
		/** 20131005    
		 * __rmqueue_fallback으로 가져온 page를 저장한다.
		 **/
		page = __rmqueue_fallback(zone, order, migratetype);

		/*
		 * Use MIGRATE_RESERVE rather than fail an allocation. goto
		 * is used because __rmqueue_smallest is an inline function
		 * and we want just one call site
		 */
		/** 20131005    
		 * page를 가져오는데 실패했다면
		 * migratetype을 MIGRATE_RESERVE로 변경한 뒤 다시 시도한다.
		 **/
		if (!page) {
			migratetype = MIGRATE_RESERVE;
			goto retry_reserve;
		}
	}

	/** 20130928    
	 * trace 관련 함수
	 **/
	trace_mm_page_alloc_zone_locked(page, order, migratetype);
	return page;
}

/*
 * Obtain a specified number of elements from the buddy allocator, all under
 * a single hold of the lock, for efficiency.  Add them to the supplied list.
 * Returns the number of new pages which were placed at *list.
 */
/** 20131005    
 * zone에 해당하는 buddy allocator로부터 (1<<order) * count 만큼의 page를 받아온다.
 * 성공한다면 'list'에 'cold' 여부에 따라 cold/hot으로 추가시킨다.
 *
 * page를 받아올 migratetype은 요청되어진 migratetype을 우선하고,
 * 실패했을 경우 약속된 다른 migratetype에서 받아올 수 있다.
 * 
 * buddy에서 빼왔으므로 받아온 page의 private영역에 migratetype을 저장하고 state를 조정한다.
 *
 * list : zone -> pageset -> pcp -> migratetype 별 list
  **/
static int rmqueue_bulk(struct zone *zone, unsigned int order,
			unsigned long count, struct list_head *list,
			int migratetype, int cold)
{
	int mt = migratetype, i;

	/** 20130928    
	 * zone 자료구조를 작업하기 전에 spin lock을 걸어준다.
	 **/
	spin_lock(&zone->lock);
	/** 20130928    
	 * count 개수(pcp->bulk)만큼 반복한다.
	 **/
	for (i = 0; i < count; ++i) {
		/** 20131005    
		 * zone에서 migratetype의 free list에서 1 << order 이상의 page를 가져온다.
		 * (현재 rmqueue_bulk를 호출하는 곳은 buffered_rmqueue이고, 이 때 order는 0)
		 **/
		struct page *page = __rmqueue(zone, order, migratetype);
		/** 20131005    
		 * __rmqueue가 실패했을 경우 중단.
		 **/
		if (unlikely(page == NULL))
			break;

		/*
		 * Split buddy pages returned by expand() are received here
		 * in physical page order. The page is added to the callers and
		 * list and the list head then moves forward. From the callers
		 * perspective, the linked list is ordered by page number in
		 * some conditions. This is useful for IO devices that can
		 * merge IO requests if the physical pages are ordered
		 * properly.
		 */
		/** 20131005    
		 * cold가 아닌 경우 'list'의 앞에 page를 추가한다.
		 * rmqueue_bulk가 처음 호출되었을 때 list는 pcp->lists[migratetype]이고,
		 *  이 반복문이 한 번 수행되면 등록될 list가 &page->lru로 변경된다.
		 * 
		 * cold인 경우 'list'의 마지막에 page를 추가한다.
		 **/
		if (likely(cold == 0))
			list_add(&page->lru, list);
		else
			list_add_tail(&page->lru, list);
		/** 20131005    
		 * CONFIG_CMA이 정의되어 있지 않으므로 false.
		 **/
		if (IS_ENABLED(CONFIG_CMA)) {
			mt = get_pageblock_migratetype(page);
			if (!is_migrate_cma(mt) && mt != MIGRATE_ISOLATE)
				mt = migratetype;
		}
		/** 20131005    
		 * pcp로 관리되는 page의 private에는 migratetype을 저장한다.
		 **/
		set_page_private(page, mt);
		/** 20131005    
		 * list의 기준위치를 가져온 page->lru로 다시 설정한다.
		 **/
		list = &page->lru;
	}
	/** 20131005    
	 * zone의 NR_FREE_PAGES state를 i<<order개만큼 뺀다.
	 **/
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(i << order));
	/** 20131005    
	 * spinlock을 푼다.
	 **/
	spin_unlock(&zone->lock);
	/** 20131005    
	 * i를 리턴
	 **/
	return i;
}

#ifdef CONFIG_NUMA
/*
 * Called from the vmstat counter updater to drain pagesets of this
 * currently executing processor on remote nodes after they have
 * expired.
 *
 * Note that this function must be called with the thread pinned to
 * a single processor.
 */
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp)
{
	unsigned long flags;
	int to_drain;

	local_irq_save(flags);
	if (pcp->count >= pcp->batch)
		to_drain = pcp->batch;
	else
		to_drain = pcp->count;
	if (to_drain > 0) {
		free_pcppages_bulk(zone, to_drain, pcp);
		pcp->count -= to_drain;
	}
	local_irq_restore(flags);
}
#endif

/*
 * Drain pages of the indicated processor.
 *
 * The processor must either be the current processor and the
 * thread pinned to the current processor or a processor that
 * is not online.
 */
/** 20140622    
 * 해당 cpu가 보유 중인 percpu pages를 버디 할당자로 되돌린다.
 **/
static void drain_pages(unsigned int cpu)
{
	unsigned long flags;
	struct zone *zone;

	/** 20140622    
	 * page를 보유한 zone들을 순회하며
	 **/
	for_each_populated_zone(zone) {
		struct per_cpu_pageset *pset;
		struct per_cpu_pages *pcp;

		/** 20140622    
		 * 인터럽트를 막은 상태에서 zone의 percpu 변수 pageset에 접근해
		 * percpu pages를 보유 중이라면 percpu 해제 함수를 호출한다.
		 **/
		local_irq_save(flags);
		pset = per_cpu_ptr(zone->pageset, cpu);

		pcp = &pset->pcp;
		if (pcp->count) {
			free_pcppages_bulk(zone, pcp->count, pcp);
			pcp->count = 0;
		}
		local_irq_restore(flags);
	}
}

/*
 * Spill all of this CPU's per-cpu pages back into the buddy allocator.
 */
/** 20140622    
 * 현재 cpu가 per-cpu용으로 가지고 있는 페이지들을 buddy allocator로 이관한다.
 **/
void drain_local_pages(void *arg)
{
	drain_pages(smp_processor_id());
}

/*
 * Spill all the per-cpu pages from all CPUs back into the buddy allocator.
 *
 * Note that this code is protected against sending an IPI to an offline
 * CPU but does not guarantee sending an IPI to newly hotplugged CPUs:
 * on_each_cpu_mask() blocks hotplug and won't talk to offlined CPUs but
 * nothing keeps CPUs from showing up after we populated the cpumask and
 * before the call to on_each_cpu_mask().
 */
/** 20140622    
 * cpu가 zone에 대해 cpu캐시용으로 별도의 페이지를 보유하고 있다면
 * drain_local_pages를 호출해 per-cpu 페이지들을 buddy로 반납하도록 한다.
 **/
void drain_all_pages(void)
{
	int cpu;
	struct per_cpu_pageset *pcp;
	struct zone *zone;

	/*
	 * Allocate in the BSS so we wont require allocation in
	 * direct reclaim path for CONFIG_CPUMASK_OFFSTACK=y
	 */
	static cpumask_t cpus_with_pcps;

	/*
	 * We don't care about racing with CPU hotplug event
	 * as offline notification will cause the notified
	 * cpu to drain that CPU pcps and on_each_cpu_mask
	 * disables preemption as part of its processing
	 */
	/** 20140621    
	 * 각 cpu를 순회하며 cpu가 percpu로 페이지를 보유 중이라면
	 * cpu mask를 설정한다.
	 **/
	for_each_online_cpu(cpu) {
		bool has_pcps = false;
		/** 20140621    
		 * cpu별로 보유 중인 zone의 pageset 값을 가져와
		 **/
		for_each_populated_zone(zone) {
			pcp = per_cpu_ptr(zone->pageset, cpu);
			/** 20140621    
			 * 해당 zone에 대해 cpu가 percpu로 페이지를 보유 중이면
			 * true로 설정하고 빠져 나간다.
			 **/
			if (pcp->pcp.count) {
				has_pcps = true;
				break;
			}
		}
		/** 20140621    
		 * cpus_with_pcps 에 cpu가 pcps를 보유 중임을 설정한다.
		 **/
		if (has_pcps)
			cpumask_set_cpu(cpu, &cpus_with_pcps);
		else
			cpumask_clear_cpu(cpu, &cpus_with_pcps);
	}
	/** 20140622    
	 * 위에서 설정한 마스크에 속한 cpu들이 drain_local_pages 함수를 실행하도록 한다. 다른 cpu가 함수 호출을 하도록 기다린 뒤에 리턴된다.
	 **/
	on_each_cpu_mask(&cpus_with_pcps, drain_local_pages, NULL, 1);
}

#ifdef CONFIG_HIBERNATION

void mark_free_pages(struct zone *zone)
{
	unsigned long pfn, max_zone_pfn;
	unsigned long flags;
	int order, t;
	struct list_head *curr;

	if (!zone->spanned_pages)
		return;

	spin_lock_irqsave(&zone->lock, flags);

	max_zone_pfn = zone->zone_start_pfn + zone->spanned_pages;
	for (pfn = zone->zone_start_pfn; pfn < max_zone_pfn; pfn++)
		if (pfn_valid(pfn)) {
			struct page *page = pfn_to_page(pfn);

			if (!swsusp_page_is_forbidden(page))
				swsusp_unset_page_free(page);
		}

	for_each_migratetype_order(order, t) {
		list_for_each(curr, &zone->free_area[order].free_list[t]) {
			unsigned long i;

			pfn = page_to_pfn(list_entry(curr, struct page, lru));
			for (i = 0; i < (1UL << order); i++)
				swsusp_set_page_free(pfn_to_page(pfn + i));
		}
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif /* CONFIG_PM */

/*
 * Free a 0-order page
 * cold == 1 ? free a cold page : free a hot page
 */
/** 20130831    
 * 0-order page를 free 한다.
 *
 * 해제시 바로 buddy로 이관하지 않고, percpu의 리스트의 해당 type에 달아준다.
 * percpu의 list에 high watermark 이상의 페이지가 등록된 경우 buddy로 이관한다.
 *
 * hot page : 최근 access된 page의 경우, hw cache에 남아 있을 가능성이 높다.
 * hot page를 free 해줄 때 list의 앞에 넣고, 그렇지 않은 경우 list의 뒤에 넣는다.
 * [참고] http://lwn.net/Articles/14768/
 **/
void free_hot_cold_page(struct page *page, int cold)
{
	/** 20130831    
	 * page가 속한 zone 정보를 가져온다.
	 **/
	struct zone *zone = page_zone(page);
	struct per_cpu_pages *pcp;
	unsigned long flags;
	int migratetype;
	/** 20130831    
	 *   MACRO로 생성된 __TestClearPageMlocked 호출.
	 *   Mlocked bit를 clear 해주고, 이전 상태를 wasMlocked에 저장
	 **/

	int wasMlocked = __TestClearPageMlocked(page);

	/** 20130831    
	 * page부터  1<< order만큼 free를 위한 준비 과정에서 실패하면 바로 리턴.
	 **/
	if (!free_pages_prepare(page, 0))
		return;

	/** 20130831    
	 * page가 속한 pageblock의 migratetype을 리턴한다.
	 **/
	migratetype = get_pageblock_migratetype(page);
	/** 20130831    
	 * struct page의 private에 migratetype을 저장한다.
	 **/
	set_page_private(page, migratetype);
	/** 20130928    
	 * 여기부터 local cpu의 interrupt를 금지시킨다.
	 **/
	local_irq_save(flags);
	/** 20130831    
	 * 아래 두 함수는 추후 보기로 함 ???
	 **/
	if (unlikely(wasMlocked))
		free_page_mlock(page);
	__count_vm_event(PGFREE);

	/*
	 * We only track unmovable, reclaimable and movable on pcp lists.
	 * Free ISOLATE pages back to the allocator because they are being
	 * offlined but treat RESERVE as movable pages so we can get those
	 * areas back if necessary. Otherwise, we may have to free
	 * excessively into the page allocator
	 */
	/** 20130831    
	 * migratetype이 MIGRATE_PCPTYPES 이상인 경우 MIGRATE_MOVABLE로 설정.
	 * MIGRATE_ISOLATE인 경우 page만 free_one_page로 free 한 뒤 out.
	 **/
	if (migratetype >= MIGRATE_PCPTYPES) {
		if (unlikely(migratetype == MIGRATE_ISOLATE)) {
			free_one_page(zone, page, 0, migratetype);
			goto out;
		}
		migratetype = MIGRATE_MOVABLE;
	}

	/** 20130831    
	 * pcpu 변수 zone->pageset에서 현재 cpu에 해당하는 메모리 주소를 가져오고,
	 * 그 주소가 가리키는 구조체의 pcp 값을 가져온다.
	 * zone->pageset은 zone_pcp_init에서 할당된다.
	 **/
	pcp = &this_cpu_ptr(zone->pageset)->pcp;
	/** 20130831    
	 * pcp의 lists 중 migratetype에 해당하는 list에 추가
	 *     cold인 경우 tail에, hot인 경우 앞에 추가
	 **/
	if (cold)
		list_add_tail(&page->lru, &pcp->lists[migratetype]);
	else
		list_add(&page->lru, &pcp->lists[migratetype]);
	/** 20130831    
	 * list에 추가했으므로 count를 증가.
	 **/
	pcp->count++;
	/** 20130831    
	 * count의 수가 high watermark 이상이라면 (너무 많은 수의 page들이 리스트에 등록되어 있을 경우)
	 *   pcp->batch 수만큼 free 하고, pcp->batch 단위만큼 count에 감소시킨다.
	 **/
	if (pcp->count >= pcp->high) {
		free_pcppages_bulk(zone, pcp->batch, pcp);
		pcp->count -= pcp->batch;
	}

out:
	/** 20130928    
	 * local cpu의 상태를 복원한다.
	 **/
	local_irq_restore(flags);
}

/*
 * Free a list of 0-order pages
 */
/** 20140104    
 * list에 등록된 page들을 순회하며 cold 여부에 따라 page를 free한다.
 * 실제 buddy로 이관은 bulk 단위로 이루어진다.
 **/
void free_hot_cold_page_list(struct list_head *list, int cold)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru) {
		trace_mm_page_free_batched(page, cold);
		free_hot_cold_page(page, cold);
	}
}

/*
 * split_page takes a non-compound higher-order page, and splits it into
 * n (1<<order) sub-pages: page[0..n]
 * Each sub-page must be freed individually.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
/** 20150103    
 * (compound가 아닌) order만큼의 연속적인 page 각각을 독립적인 page로 분할한다.
 **/
void split_page(struct page *page, unsigned int order)
{
	int i;

	VM_BUG_ON(PageCompound(page));
	VM_BUG_ON(!page_count(page));

#ifdef CONFIG_KMEMCHECK
	/*
	 * Split shadow pages too, because free(page[0]) would
	 * otherwise free the whole shadow.
	 */
	if (kmemcheck_page_is_tracked(page))
		split_page(virt_to_page(page[0].shadow), order);
#endif

	/** 20150103    
	 * 첫번째 page를 제외한 page들 각각을 referenced 상태로 만들어 준다.
	 **/
	for (i = 1; i < (1 << order); i++)
		set_page_refcounted(page + i);
}

/*
 * Similar to split_page except the page is already free. As this is only
 * being used for migration, the migratetype of the block also changes.
 * As this is called with interrupts disabled, the caller is responsible
 * for calling arch_alloc_page() and kernel_map_page() after interrupts
 * are enabled.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
int split_free_page(struct page *page)
{
	unsigned int order;
	unsigned long watermark;
	struct zone *zone;

	BUG_ON(!PageBuddy(page));

	zone = page_zone(page);
	order = page_order(page);

	/* Obey watermarks as if the page was being allocated */
	watermark = low_wmark_pages(zone) + (1 << order);
	if (!zone_watermark_ok(zone, 0, watermark, 0, 0))
		return 0;

	/* Remove page from free list */
	list_del(&page->lru);
	zone->free_area[order].nr_free--;
	rmv_page_order(page);
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(1UL << order));

	/* Split into individual pages */
	set_page_refcounted(page);
	split_page(page, order);

	if (order >= pageblock_order - 1) {
		struct page *endpage = page + (1 << order) - 1;
		for (; page < endpage; page += pageblock_nr_pages) {
			int mt = get_pageblock_migratetype(page);
			if (mt != MIGRATE_ISOLATE && !is_migrate_cma(mt))
				set_pageblock_migratetype(page,
							  MIGRATE_MOVABLE);
		}
	}

	return 1 << order;
}

/*
 * Really, prep_compound_page() should be called from __rmqueue_bulk().  But
 * we cheat by calling it from here, in the order > 0 path.  Saves a branch
 * or two.
 */
/** 20131116    
 * order == 0인 경우
 *     해당 cpu의 pcp 에 접근해 migratetype에 해당하는 list에서 page를 가져온다.
 *     list가 비어 있다면 rmqueue_bulk를 호출해 buddy로부터 할당받아 채운다.
 * order > 0인 경우
 *     __rmqueue를 이용해 free list에서 (1<<order)개의 page를 가져온다.
 *     __rmqueue는 요청된 migratetype에 대해 충분한 page들을 확보하지 않았다면 
 *       fallback 정책을 사용해 다른 migratetype으로 다시 시도한다.
 *
 * page를 얻어온 뒤에 prep_new_page를 호출하여 관련된 자료구조를 설정한다.
 **/
static inline
struct page *buffered_rmqueue(struct zone *preferred_zone,
			struct zone *zone, int order, gfp_t gfp_flags,
			int migratetype)
{
	unsigned long flags;
	struct page *page;
	/** 20130928    
	 * gfp_flags에서 __GFP_COLD가 있는지 여부를 저장한다.
	 **/
	int cold = !!(gfp_flags & __GFP_COLD);

again:
	/** 20130928    
	 * order가 0인 경우 한 페이지만 별도로 처리한다.
	 **/
	if (likely(order == 0)) {
		struct per_cpu_pages *pcp;
		struct list_head *list;

		/** 20130928    
		 * local cpu interrupt disable.
		 **/
		local_irq_save(flags);
		/** 20130928    
		 * 현재 cpu에 해당하는 'pageset의 메모리 주소'를 가지고 pcp 변수의 값을 가져온다.
		 **/
		pcp = &this_cpu_ptr(zone->pageset)->pcp;
		/** 20130928    
		 * per_cpu_pages의 migratetype에 해당하는 list를 가져온다.
		 **/
		list = &pcp->lists[migratetype];
		/** 20130928    
		 * pcp의 list가 비어 있다면
		 **/
		if (list_empty(list)) {
			/** 20131005    
			 * zone에 해당하는 buddy allocator에서 pcp->batch개 만큼 page를 받아와 list에 추가한다.
			 * 추가한 개수만큼 pcp->count에 더한다.
			 *
			 *  pcp->batch는 setup_pageset() 에서 설정. 
			 **/
			pcp->count += rmqueue_bulk(zone, 0,
					pcp->batch, list,
					migratetype, cold);
			/** 20131005    
			 * 여전히 list가 비어있다면, failed로 이동.
			 **/
			if (unlikely(list_empty(list)))
				goto failed;
		}

		/** 20131005    
		 * cold가 주어졌다면 page는 list->prev가 가리키는 page (pcp->lists[migratetype]의 마지막)
		 * cold가 없다면 page는 list->next가 가리키는 page (pcp->lists[migratetype]의 처음)
		 **/
		if (cold)
			page = list_entry(list->prev, struct page, lru);
		else
			page = list_entry(list->next, struct page, lru);

		/** 20131005    
		 * pcp->lists[migratetype]이 가리키는 list에서 제거.
		 **/
		list_del(&page->lru);
		/** 20131005    
		 * pcp로 관리되는 page 수를 감소
		 **/
		pcp->count--;
	} else {
		/** 20131005    
		 * order가 0이 아닌 경우
		 **/

		/** 20131005    
		 * gfp_flags에 __GFP_NOFAIL이 있다면 order가 1보다 크면 WARN.
		 **/
		if (unlikely(gfp_flags & __GFP_NOFAIL)) {
			/*
			 * __GFP_NOFAIL is not to be used in new code.
			 *
			 * All __GFP_NOFAIL callers should be fixed so that they
			 * properly detect and handle allocation failures.
			 *
			 * We most definitely don't want callers attempting to
			 * allocate greater than order-1 page units with
			 * __GFP_NOFAIL.
			 */
			WARN_ON_ONCE(order > 1);
		}
		/** 20131005    
		 * zone의 spinlock을 걸고, 현재 정보를 flags에 저장한다.
		 **/
		spin_lock_irqsave(&zone->lock, flags);
		/** 20131005    
		 * zone의 migratetype에 해당하는 free list에서 (1<<order)개의 page를 가져온다.
		 **/
		page = __rmqueue(zone, order, migratetype);
		/** 20131005    
		 * lock만 해제한다.
		 **/
		spin_unlock(&zone->lock);
		/** 20131005    
		 * page를 받아오지 못했다면 failed로 이동
		 **/
		if (!page)
			goto failed;
		/** 20131005    
		 * NR_FREE_PAGES의 state를 감소시킨다.
		 **/
		__mod_zone_page_state(zone, NR_FREE_PAGES, -(1 << order));
	}

	/** 20131005    
	 * __count_vm_events(PGALLOC_NORMAL - ZONE_NORMAL + ((zone) - (zone)->zone_pgdat->node_zones), 1 << order);
	 **/
	__count_zone_vm_events(PGALLOC, zone, 1 << order);
	/** 20131005    
	 * NUMA가 아닐 때는 NULL.
	 **/
	zone_statistics(preferred_zone, zone, gfp_flags);
	/** 20131005    
	 * 저장해둔  irq 복원
	 **/
	local_irq_restore(flags);

	/** 20131005    
	 * DEBUG용 함수
	 **/
	VM_BUG_ON(bad_range(zone, page));
	/** 20131005    
	 * gfp_flags에 따라 관련된 자료구조를 설정하고, 실패한다면 again부터 다시 실행
	 **/
	if (prep_new_page(page, order, gfp_flags))
		goto again;
	/** 20131116    
	 * 첫번째 page를 리턴한다.
	 **/
	return page;

failed:
	local_irq_restore(flags);
	return NULL;
}

/* The ALLOC_WMARK bits are used as an index to zone->watermark */
#define ALLOC_WMARK_MIN		WMARK_MIN
#define ALLOC_WMARK_LOW		WMARK_LOW
#define ALLOC_WMARK_HIGH	WMARK_HIGH
#define ALLOC_NO_WATERMARKS	0x04 /* don't check watermarks at all */

/* Mask to get the watermark bits */
#define ALLOC_WMARK_MASK	(ALLOC_NO_WATERMARKS-1)

#define ALLOC_HARDER		0x10 /* try to alloc harder */
#define ALLOC_HIGH		0x20 /* __GFP_HIGH set */
#define ALLOC_CPUSET		0x40 /* check for correct cpuset */

#ifdef CONFIG_FAIL_PAGE_ALLOC

static struct {
	struct fault_attr attr;

	u32 ignore_gfp_highmem;
	u32 ignore_gfp_wait;
	u32 min_order;
} fail_page_alloc = {
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_wait = 1,
	.ignore_gfp_highmem = 1,
	.min_order = 1,
};

static int __init setup_fail_page_alloc(char *str)
{
	return setup_fault_attr(&fail_page_alloc.attr, str);
}
__setup("fail_page_alloc=", setup_fail_page_alloc);

static bool should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	if (order < fail_page_alloc.min_order)
		return false;
	if (gfp_mask & __GFP_NOFAIL)
		return false;
	if (fail_page_alloc.ignore_gfp_highmem && (gfp_mask & __GFP_HIGHMEM))
		return false;
	if (fail_page_alloc.ignore_gfp_wait && (gfp_mask & __GFP_WAIT))
		return false;

	return should_fail(&fail_page_alloc.attr, 1 << order);
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS

static int __init fail_page_alloc_debugfs(void)
{
	umode_t mode = S_IFREG | S_IRUSR | S_IWUSR;
	struct dentry *dir;

	dir = fault_create_debugfs_attr("fail_page_alloc", NULL,
					&fail_page_alloc.attr);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	if (!debugfs_create_bool("ignore-gfp-wait", mode, dir,
				&fail_page_alloc.ignore_gfp_wait))
		goto fail;
	if (!debugfs_create_bool("ignore-gfp-highmem", mode, dir,
				&fail_page_alloc.ignore_gfp_highmem))
		goto fail;
	if (!debugfs_create_u32("min-order", mode, dir,
				&fail_page_alloc.min_order))
		goto fail;

	return 0;
fail:
	debugfs_remove_recursive(dir);

	return -ENOMEM;
}

late_initcall(fail_page_alloc_debugfs);

#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */

#else /* CONFIG_FAIL_PAGE_ALLOC */

/** 20130907    
 * CONFIG_FAIL_PAGE_ALLOC 이 CONFIG 되어 있지 않아 false 리턴.
 *
 * fault-injection은 특정 기능이 실패했을 경우 적당한 절차로 처리되는지 검사하기 위한 기법
 * [참고] http://en.wikipedia.org/wiki/Fault_injection
 *        Documentation/fault-injection/
 **/
static inline bool should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	return false;
}

#endif /* CONFIG_FAIL_PAGE_ALLOC */

/*
 * Return true if free pages are above 'mark'. This takes into account the order
 * of the allocation.
 */
/** 20130914
__zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
					zone_page_state(z, NR_FREE_PAGES));
watermark test를 통과하면 true, 실패하면 false; 리턴.
다음주에 다시 확인???
**/
static bool __zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags, long free_pages)
{
	/* free_pages my go negative - that's OK */
	long min = mark;
	/** 20130928    
	 * classzone_idx는 preferrend_zone의 zone idx.
	 * lowmem_reserve 배열 중에 classzone_idx에 해당하는 값을 가져온다.
	 **/
	long lowmem_reserve = z->lowmem_reserve[classzone_idx];
	int o;

	/** 20130914
	 * -1 을 왜하는건지???
	 *
	 * 20131214
	 * 아래서 min값과 equal로 비교해 같으면 실패로 판정했으므로 -1을 미리 빼준듯.
	 **/
	free_pages -= (1 << order) - 1;

	/** 20130914
	|             |
	|  |free page |
	|++|+++++++++ |-> WATER_MARK 
	|  |          |
	|  |          |
	|++|+++++++++ |-> ALLOC_HARDER
	|  |          |   min-min/4
	|  |          |
	|++|+++++++++ |-> ALLOC_HIGH
	|  |	      |   min-min/2
	|			  |

	free_pages 가 watermark 이하로 떨어지면 return false
	단, 위와 같이 alloc_flags에따라(ALLOC_HIGH,ALLOC_HARDER 일경우)
	비교되는 watermark(min)은 조정된다.

	HARDER가 HIGH보다 큰 값이 되므로 더욱 엄격한 기준이 된다.
	**/
	if (alloc_flags & ALLOC_HIGH)
		min -= min / 2;
	if (alloc_flags & ALLOC_HARDER)
		min -= min / 4;

	/** 20130928    
	 * free_pages가 최소값과 lowmem_reserve을 더한 값 이하라면 바로 false를 리턴한다.
	 **/
	if (free_pages <= min + lowmem_reserve)
		return false;
	
	/** 20130914
		각 order를 순회하면서
		for(o=0;o<order;o++)
		{
			1. zone 의 총 free page(free_pages) 에서  각 order의 free page 를 구해서 빼준다.
			2. min을 2로 나눈다.

			3. free_pages가  min보다 작거나 같다면 return  false
			   아니면  다음 for문 실행
		}

		이게 맞나???
			- min 이 이렇게 조정이 되는이유는???

		 20131214    
		  각 단계에서 WMARK_MIN / (2 ^ o) 보다 많은 free page의 개수를 갖지 못한다.
		    (연속된 page라면 이미 그 상위 order로 merge가 될 것이기 때문에)
		  이 때 free_pages가 min 이하라면 watermark 테스트를 실패했다고 할 수 있다.
		 
		    -> lazy merge에서는 이렇게 단계별로 비교하는 것이 무의미 하지 않나???
	**/
	for (o = 0; o < order; o++) {
		/* At the next order, this order's pages become unavailable */
		free_pages -= z->free_area[o].nr_free << o;

		/* Require fewer higher order pages to be free */
		min >>= 1;

		if (free_pages <= min)
			return false;
	}
	return true;
}

#ifdef CONFIG_MEMORY_ISOLATION
static inline unsigned long nr_zone_isolate_freepages(struct zone *zone)
{
	if (unlikely(zone->nr_pageblock_isolate))
		return zone->nr_pageblock_isolate * pageblock_nr_pages;
	return 0;
}
#else
/** 20131116    
 * CONFIG_MEMORY_ISOLATION가 설정되어 있지 않아 0을 리턴.
 **/
static inline unsigned long nr_zone_isolate_freepages(struct zone *zone)
{
	return 0;
}
#endif

/** 20130928    
 * zone z의 NR_FREE_PAGES를 가져와 __zone_watermark_ok에 넘겨
 * watermark 내에 위치하는지 판단하는 함수
 **/
bool zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
					zone_page_state(z, NR_FREE_PAGES));
}

/** 20131116    
 * freepages 값에 percpu와 isolate 값을 반영해 watermark 기준을 통과하는지 결과를 리턴한다.
 **/
bool zone_watermark_ok_safe(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	/** 20131116    
	 * zone state에서 NR_FREE_PAGES의 값을 free_pages에 저장한다.
	 **/
	long free_pages = zone_page_state(z, NR_FREE_PAGES);

	/** 20131116    
	 * zone의 percpu_drift_mark가 설정되어 있고, 이 값이 free_pages보다 클 때만
	 * 보다 정확한 값을 읽어온다.
	 **/
	if (z->percpu_drift_mark && free_pages < z->percpu_drift_mark)
		free_pages = zone_page_state_snapshot(z, NR_FREE_PAGES);

	/*
	 * If the zone has MIGRATE_ISOLATE type free pages, we should consider
	 * it.  nr_zone_isolate_freepages is never accurate so kswapd might not
	 * sleep although it could do so.  But this is more desirable for memory
	 * hotplug than sleeping which can cause a livelock in the direct
	 * reclaim path.
	 */
	/** 20131116    
	 * isolate된 freepages 값을 free_pages에 빼준다.
	 **/
	free_pages -= nr_zone_isolate_freepages(z);
	/** 20131116    
	 * 계산된 free_pages 정보로 __zone_watermark_ok 에 전달해 결과를 리턴한다.
	 **/
	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
								free_pages);
}

#ifdef CONFIG_NUMA
/*
 * zlc_setup - Setup for "zonelist cache".  Uses cached zone data to
 * skip over zones that are not allowed by the cpuset, or that have
 * been recently (in last second) found to be nearly full.  See further
 * comments in mmzone.h.  Reduces cache footprint of zonelist scans
 * that have to skip over a lot of full or unallowed zones.
 *
 * If the zonelist cache is present in the passed in zonelist, then
 * returns a pointer to the allowed node mask (either the current
 * tasks mems_allowed, or node_states[N_HIGH_MEMORY].)
 *
 * If the zonelist cache is not available for this zonelist, does
 * nothing and returns NULL.
 *
 * If the fullzones BITMAP in the zonelist cache is stale (more than
 * a second since last zap'd) then we zap it out (clear its bits.)
 *
 * We hold off even calling zlc_setup, until after we've checked the
 * first zone in the zonelist, on the theory that most allocations will
 * be satisfied from that first zone, so best to examine that zone as
 * quickly as we can.
 */
static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	nodemask_t *allowednodes;	/* zonelist_cache approximation */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return NULL;

	if (time_after(jiffies, zlc->last_full_zap + HZ)) {
		bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
		zlc->last_full_zap = jiffies;
	}

	allowednodes = !in_interrupt() && (alloc_flags & ALLOC_CPUSET) ?
					&cpuset_current_mems_allowed :
					&node_states[N_HIGH_MEMORY];
	return allowednodes;
}

/*
 * Given 'z' scanning a zonelist, run a couple of quick checks to see
 * if it is worth looking at further for free memory:
 *  1) Check that the zone isn't thought to be full (doesn't have its
 *     bit set in the zonelist_cache fullzones BITMAP).
 *  2) Check that the zones node (obtained from the zonelist_cache
 *     z_to_n[] mapping) is allowed in the passed in allowednodes mask.
 * Return true (non-zero) if zone is worth looking at further, or
 * else return false (zero) if it is not.
 *
 * This check -ignores- the distinction between various watermarks,
 * such as GFP_HIGH, GFP_ATOMIC, PF_MEMALLOC, ...  If a zone is
 * found to be full for any variation of these watermarks, it will
 * be considered full for up to one second by all requests, unless
 * we are so low on memory on all allowed nodes that we are forced
 * into the second scan of the zonelist.
 *
 * In the second scan we ignore this zonelist cache and exactly
 * apply the watermarks to all zones, even it is slower to do so.
 * We are low on memory in the second scan, and should leave no stone
 * unturned looking for a free page.
 */
static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
						nodemask_t *allowednodes)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	int i;				/* index of *z in zonelist zones */
	int n;				/* node that zone *z is on */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return 1;

	i = z - zonelist->_zonerefs;
	n = zlc->z_to_n[i];

	/* This zone is worth trying if it is allowed but not full */
	return node_isset(n, *allowednodes) && !test_bit(i, zlc->fullzones);
}

/*
 * Given 'z' scanning a zonelist, set the corresponding bit in
 * zlc->fullzones, so that subsequent attempts to allocate a page
 * from that zone don't waste time re-examining it.
 */
static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	int i;				/* index of *z in zonelist zones */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	i = z - zonelist->_zonerefs;

	set_bit(i, zlc->fullzones);
}

/*
 * clear all zones full, called after direct reclaim makes progress so that
 * a zone that was recently full is not skipped over for up to a second
 */
static void zlc_clear_zones_full(struct zonelist *zonelist)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
}

#else	/* CONFIG_NUMA */

static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	return NULL;
}
/** 20130914
vexpress는 NUMA가 아니므로 리턴 1
**/
static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
				nodemask_t *allowednodes)
{
	return 1;
}

static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
}

static void zlc_clear_zones_full(struct zonelist *zonelist)
{
}
#endif	/* CONFIG_NUMA */

/*
 * get_page_from_freelist goes through the zonelist trying to allocate
 * a page.
 */
/** 20131116    
 *
 * 선호하는 zone의 watermark를 검사해 watermark를 통과하면
 * 해당 zone에서 (1<<order)만큼의 page를 받아 온다.
 *
 * 실패했을 경우 zonelist를 순회하며 다른 zone으로부터 page를 얻어온다.
 **/
static struct page *
get_page_from_freelist(gfp_t gfp_mask, nodemask_t *nodemask, unsigned int order,
		struct zonelist *zonelist, int high_zoneidx, int alloc_flags,
		struct zone *preferred_zone, int migratetype)
{
	struct zoneref *z;
	struct page *page = NULL;
	int classzone_idx;
	struct zone *zone;
	nodemask_t *allowednodes = NULL;/* zonelist_cache approximation */
	int zlc_active = 0;		/* set if using zonelist_cache */
	int did_zlc_setup = 0;		/* just call zlc_setup() one time */

	/** 20131214    
	 * 선호하는 zone의 idx를 구해온다.
	 **/
	classzone_idx = zone_idx(preferred_zone);
zonelist_scan:
	/*
	 * Scan zonelist, looking for a zone with enough free.
	 * See also cpuset_zone_allowed() comment in kernel/cpuset.c.
	 */
	/** 20131123    
	 * nodemask를 적용해 zonelist의 각 zone을 순회하며 page를 할당 받아온다.
	 * 만약 zonelist에서 적합한 zone을 찾지 못한다면 page는 NULL인 상태로 종료.
	 **/
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
						high_zoneidx, nodemask) {
		/** 20130914
		 * NUMA가 아니므로 다음 if는 거짓
		 **/
		if (NUMA_BUILD && zlc_active &&
			!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;
		/** 20140628    
		 * NUMA인 경우, ALLOC_CPUSET이 지정되면
		 * process에게 허용된 CPUs에 할당된 memory로부터만 page를 받아올 수 있다.
		 **/
		if ((alloc_flags & ALLOC_CPUSET) &&
			!cpuset_zone_allowed_softwall(zone, gfp_mask))
				continue;
		/*
		 * When allocating a page cache page for writing, we
		 * want to get it from a zone that is within its dirty
		 * limit, such that no single zone holds more than its
		 * proportional share of globally allowed dirty pages.
		 * The dirty limits take into account the zone's
		 * lowmem reserves and high watermark so that kswapd
		 * should be able to balance it without having to
		 * write pages from its LRU list.
		 *
		 * This may look like it could increase pressure on
		 * lower zones by failing allocations in higher zones
		 * before they are full.  But the pages that do spill
		 * over are limited as the lower zones are protected
		 * by this very same mechanism.  It should not become
		 * a practical burden to them.
		 *
		 * XXX: For now, allow allocations to potentially
		 * exceed the per-zone dirty limit in the slowpath
		 * (ALLOC_WMARK_LOW unset) before going into reclaim,
		 * which is important when on a NUMA setup the allowed
		 * zones are together not big enough to reach the
		 * global limit.  The proper fix for these situations
		 * will require awareness of zones in the
		 * dirty-throttling and the flusher threads.
		 */
		/** 20130914
		ALLOC_WMARK_LOW가 설정되어 있고 요청 페이지의 타입이 Write일경우 
		그리고 dirty limit을 초과 했을 경우 this_zone_full로 goto. 
		**/
		if ((alloc_flags & ALLOC_WMARK_LOW) &&
		    (gfp_mask & __GFP_WRITE) && !zone_dirty_ok(zone))
			goto this_zone_full;

		/** 20130914
		#define ALLOC_NO_WATERMARKS	0x04 // don't check watermarks at all
		**/
		BUILD_BUG_ON(ALLOC_NO_WATERMARKS < NR_WMARK);
		/** 20131116    
		 * ALLOC_NO_WATERMARKS가 설정되지 않았다면 watermark를 검사한다.
		 **/
		if (!(alloc_flags & ALLOC_NO_WATERMARKS)) {
			unsigned long mark;
			int ret;

			/** 20131214    
			 * alloc_flags의 WAMRK_MASK에 해당하는 비트는
			 *    gfp_to_alloc_flags에서 default로 ALLOC_WMARK_MIN이거나
			 *    목적에 따라 get_page_from_freelist 호출시 명시적으로 지정한 값이다.
			 **/
			mark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
			/** 20130928    
			 * classzone_idx는 preferred_zone의 zone idx.
			 * zone의 watermark 값을 통과하면 현재 zone에서 메모리 할당을 계속한다.
			 **/
			if (zone_watermark_ok(zone, order, mark,
				    classzone_idx, alloc_flags))
				goto try_this_zone;

			/** 20130928    
			 * NUMA가 아닐 경우 false
			 **/
			if (NUMA_BUILD && !did_zlc_setup && nr_online_nodes > 1) {
				/*
				 * we do zlc_setup if there are multiple nodes
				 * and before considering the first zone allowed
				 * by the cpuset.
				 */
				allowednodes = zlc_setup(zonelist, alloc_flags);
				zlc_active = 1;
				did_zlc_setup = 1;
			}

			/** 20130928    
			 * NUMA가 아닌 경우 항상 0으로 this_zone_full로 이동
			 *
			 * 20131214
			 * zone_reclaim_mode는 NUMA인 경우 build_zonelists에서 초기값이 1로 설정되거나
			 * sysctl에 의해서 바뀔 수 있다.
			 * UMA인 경우 초기값 0이기 때문에 watermark false이므로 항상 this_zone_full로 이동.
			 **/
			if (zone_reclaim_mode == 0)
				goto this_zone_full;

			/*
			 * As we may have just activated ZLC, check if the first
			 * eligible zone has failed zone_reclaim recently.
			 */
			if (NUMA_BUILD && zlc_active &&
				!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;

			ret = zone_reclaim(zone, gfp_mask, order);
			switch (ret) {
			case ZONE_RECLAIM_NOSCAN:
				/* did not scan */
				continue;
			case ZONE_RECLAIM_FULL:
				/* scanned but unreclaimable */
				continue;
			default:
				/* did we reclaim enough */
				if (!zone_watermark_ok(zone, order, mark,
						classzone_idx, alloc_flags))
					goto this_zone_full;
			}
		}

try_this_zone:
		/** 20131116    
		 * zone에서 (1<<order)만큼의 page들을 할당 받는다.
		 **/
		page = buffered_rmqueue(preferred_zone, zone, order,
						gfp_mask, migratetype);
		/** 20131116    
		 * 성공적으로 페이지를 할당받았다면 break.
		 **/
		if (page)
			break;
this_zone_full:
		/** 20130928    
		 * NUMA가 아닐 경우 거짓이 되고,
		 * loop의 끝에 도달해 다음 zone에 대해 반복.
		 **/
		if (NUMA_BUILD)
			zlc_mark_zone_full(zonelist, z);
	}

	/** 20131116    
	 * CONFIG_NUMA가 설정되지 않았으므로 NUMA_BUILD 는 0.
	 **/
	if (unlikely(NUMA_BUILD && page == NULL && zlc_active)) {
		/* Disable zlc cache for second zonelist scan */
		zlc_active = 0;
		goto zonelist_scan;
	}
	/** 20131116    
	 * 할당 받은 page를 리턴. 할당받지 못한 상태라면 page는 NULL.
	 **/
	return page;
}

/*
 * Large machines with many possible nodes should not always dump per-node
 * meminfo in irq context.
 */
static inline bool should_suppress_show_mem(void)
{
	bool ret = false;

#if NODES_SHIFT > 8
	ret = in_interrupt();
#endif
	return ret;
}

static DEFINE_RATELIMIT_STATE(nopage_rs,
		DEFAULT_RATELIMIT_INTERVAL,
		DEFAULT_RATELIMIT_BURST);

void warn_alloc_failed(gfp_t gfp_mask, int order, const char *fmt, ...)
{
	unsigned int filter = SHOW_MEM_FILTER_NODES;

	if ((gfp_mask & __GFP_NOWARN) || !__ratelimit(&nopage_rs) ||
	    debug_guardpage_minorder() > 0)
		return;

	/*
	 * This documents exceptions given to allocations in certain
	 * contexts that are allowed to allocate outside current's set
	 * of allowed nodes.
	 */
	if (!(gfp_mask & __GFP_NOMEMALLOC))
		if (test_thread_flag(TIF_MEMDIE) ||
		    (current->flags & (PF_MEMALLOC | PF_EXITING)))
			filter &= ~SHOW_MEM_FILTER_NODES;
	if (in_interrupt() || !(gfp_mask & __GFP_WAIT))
		filter &= ~SHOW_MEM_FILTER_NODES;

	if (fmt) {
		struct va_format vaf;
		va_list args;

		va_start(args, fmt);

		vaf.fmt = fmt;
		vaf.va = &args;

		pr_warn("%pV", &vaf);

		va_end(args);
	}

	pr_warn("%s: page allocation failure: order:%d, mode:0x%x\n",
		current->comm, order, gfp_mask);

	dump_stack();
	if (!should_suppress_show_mem())
		show_mem(filter);
}

/** 20140628    
 * alloc을 재시도 해야 하는지 검사한다.
 **/
static inline int
should_alloc_retry(gfp_t gfp_mask, unsigned int order,
				unsigned long did_some_progress,
				unsigned long pages_reclaimed)
{
	/* Do not loop if specifically requested */
	/** 20140628    
	 * retry 시도 금지인 경우 retry 하지 않는다.
	 **/
	if (gfp_mask & __GFP_NORETRY)
		return 0;

	/* Always retry if specifically requested */
	/** 20140628    
	 * NOFAIL인 경우 retry.
	 **/
	if (gfp_mask & __GFP_NOFAIL)
		return 1;

	/*
	 * Suspend converts GFP_KERNEL to __GFP_WAIT which can prevent reclaim
	 * making forward progress without invoking OOM. Suspend also disables
	 * storage devices so kswapd will not help. Bail if we are suspending.
	 */
	/** 20140628    
	 * 마지막 시도에서 reclaim 한 page가 없고, suspended_storage 상태라면 retry 하지 않는다.
	 **/
	if (!did_some_progress && pm_suspended_storage())
		return 0;

	/*
	 * In this implementation, order <= PAGE_ALLOC_COSTLY_ORDER
	 * means __GFP_NOFAIL, but that may not be true in other
	 * implementations.
	 */
	/** 20140628    
	 * order가 PAGE_ALLOC_COSTLY_ORDER 보다 작다면 재시도.
	 **/
	if (order <= PAGE_ALLOC_COSTLY_ORDER)
		return 1;

	/*
	 * For order > PAGE_ALLOC_COSTLY_ORDER, if __GFP_REPEAT is
	 * specified, then we retry until we no longer reclaim any pages
	 * (above), or we've reclaimed an order of pages at least as
	 * large as the allocation's order. In both cases, if the
	 * allocation still fails, we stop retrying.
	 */
	/** 20140628    
	 * __GFP_REPEAT가 설정되어 있고,
	 * 지금까지 회수한 page가 요구한 order 보다 작은 경우 재시도.
	 **/
	if (gfp_mask & __GFP_REPEAT && pages_reclaimed < (1 << order))
		return 1;

	return 0;
}

/** 20140628    
 * order만큼 high watermark로 page alloc을 시도하고,
 * 실패했을 경우 oom kill을 동작 시킨다.
 **/
static inline struct page *
__alloc_pages_may_oom(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	/* Acquire the OOM killer lock for the zones in zonelist */
	/** 20140628    
	 * zonelist의 zone들에 대해 oom killer lock 획득을 시도하고,
	 * 실패하면 1 jiffie 만큼 sleep하여 schedule 한다.
	 **/
	if (!try_set_zonelist_oom(zonelist, gfp_mask)) {
		schedule_timeout_uninterruptible(1);
		return NULL;
	}

	/*
	 * Go through the zonelist yet one more time, keep very high watermark
	 * here, this is only to catch a parallel oom killing, we must fail if
	 * we're still under heavy pressure.
	 */
	/** 20140628    
	 * high watermark로 page 할당을 시도한다.
	 * oom killing 동시에 진행 중이라면 page 할당이 성공해 out으로 빠져나갈 것이다.
	 **/
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask,
		order, zonelist, high_zoneidx,
		ALLOC_WMARK_HIGH|ALLOC_CPUSET,
		preferred_zone, migratetype);
	if (page)
		goto out;

	/** 20140628    
	 * 메모리 할당 실패불가가 아니라면 몇 가지 실패 조건을 검사한다.
	 *		- PAGE_ALLOC_COSTLY_ORDER 이상의 order에 대한 요청인 경우.
	 *		- ZONE_NORMAL 이하 영역에 대한 요청인 경우.
	 *		- NUMA에서 현재 NODE에서의 할당 요청인 경우.
	 **/
	if (!(gfp_mask & __GFP_NOFAIL)) {
		/* The OOM killer will not help higher order allocs */
		if (order > PAGE_ALLOC_COSTLY_ORDER)
			goto out;
		/* The OOM killer does not needlessly kill tasks for lowmem */
		if (high_zoneidx < ZONE_NORMAL)
			goto out;
		/*
		 * GFP_THISNODE contains __GFP_NORETRY and we never hit this.
		 * Sanity check for bare calls of __GFP_THISNODE, not real OOM.
		 * The caller should handle page allocation failure by itself if
		 * it specifies __GFP_THISNODE.
		 * Note: Hugepage uses it but will hit PAGE_ALLOC_COSTLY_ORDER.
		 */
		if (gfp_mask & __GFP_THISNODE)
			goto out;
	}
	/* Exhausted what can be done so it's blamo time */
	out_of_memory(zonelist, gfp_mask, order, nodemask, false);

out:
	clear_zonelist_oom(zonelist, gfp_mask);
	return page;
}

#ifdef CONFIG_COMPACTION
/** 20131207
 * vexpress에서는 undefined
 * page migrate와 관련된 구현인듯..? 추후 분석???
 **/
/* Try memory compaction for high-order allocations before reclaim */
static struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	struct page *page;

	if (!order)
		return NULL;

	if (compaction_deferred(preferred_zone, order)) {
		*deferred_compaction = true;
		return NULL;
	}

	current->flags |= PF_MEMALLOC;
	*did_some_progress = try_to_compact_pages(zonelist, order, gfp_mask,
						nodemask, sync_migration);
	current->flags &= ~PF_MEMALLOC;
	if (*did_some_progress != COMPACT_SKIPPED) {

		/* Page migration frees to the PCP lists but we want merging */
		drain_pages(get_cpu());
		put_cpu();

		page = get_page_from_freelist(gfp_mask, nodemask,
				order, zonelist, high_zoneidx,
				alloc_flags & ~ALLOC_NO_WATERMARKS,
				preferred_zone, migratetype);
		if (page) {
			preferred_zone->compact_considered = 0;
			preferred_zone->compact_defer_shift = 0;
			if (order >= preferred_zone->compact_order_failed)
				preferred_zone->compact_order_failed = order + 1;
			count_vm_event(COMPACTSUCCESS);
			return page;
		}

		/*
		 * It's bad if compaction run occurs and fails.
		 * The most likely reason is that pages exist,
		 * but not enough to satisfy watermarks.
		 */
		count_vm_event(COMPACTFAIL);

		/*
		 * As async compaction considers a subset of pageblocks, only
		 * defer if the failure was a sync compaction failure.
		 */
		if (sync_migration)
			defer_compaction(preferred_zone, order);

		cond_resched();
	}

	return NULL;
}
#else
/** 20140517    
 * CONFIG_COMPACTION이 정의되어 있지 않아 NULL
 **/
static inline struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	return NULL;
}
#endif /* CONFIG_COMPACTION */

/* Perform direct synchronous page reclaim */
/** 20140621    
 * 메모리 부족시에 확보를 위해 page 회수를 동기적으로 실행한다.
 **/
static int
__perform_reclaim(gfp_t gfp_mask, unsigned int order, struct zonelist *zonelist,
		  nodemask_t *nodemask)
{
	struct reclaim_state reclaim_state;
	int progress;

	/** 20140517    
	 * 스케쥴링 포인트 제공
	 **/
	cond_resched();

	/* We now go into synchronous reclaim */
	cpuset_memory_pressure_bump();
	/** 20131207
	 * current task의 flag에서 PF_MEMALLOC 속성을 더함
	 * 원하는 작업이 끝나면 다시 속성을 빼준다. 
	 * current->flags &= ~PF_MEMALLOC; 참고
	 ***/
	current->flags |= PF_MEMALLOC;
	/** 20131207
	* vexpress NULL
	**/
	lockdep_set_current_reclaim_state(gfp_mask);
	/** 20131207
	 *current task의 reclaim_state 설정(초기화)
	 **/
	reclaim_state.reclaimed_slab = 0;
	current->reclaim_state = &reclaim_state;

	/** 20140621    
	 * order만큼의 page회수를 시도하고, 회수된 페이지의 수가 리턴된다.
	 *
	 * 만약 회수된 페이지가 존재하지 않거나, zonelist의 zone들이 메모리 회수가
	 * 불가능한 상황이라면 0이 리턴.
	 **/
	progress = try_to_free_pages(zonelist, order, gfp_mask, nodemask);

	/** 20140621    
	 * 호출이 끝나고
	 *		현재 task의 reclaim 진행 중이 아님을 표시.
	 *		PF_MEMALLOC를 지운다.
	 **/
	current->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	current->flags &= ~PF_MEMALLOC;

	/** 20140621    
	 * page 확보가 끝나고 리스케줄링 포인트를 확보한다.
	 **/
	cond_resched();

	return progress;
}

/* The really slow allocator path where we enter direct reclaim */
/** 20140622    
 * direct reclaim으로 호출되어 메모리 확보를 시도하고 재시도 한다.
 * 만약 또다시 실패한 경우에는 per-cpu용 pages까지 한 번더 비운다.
 **/
static inline struct page *
__alloc_pages_direct_reclaim(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, unsigned long *did_some_progress)
{
	struct page *page = NULL;
	bool drained = false;

	/** 20140621    
	 * order 만큼의 메모리 확보를 시도하고, 메모리가 회수되지 못했다면
	 * 0이 리턴된다.
	 **/
	*did_some_progress = __perform_reclaim(gfp_mask, order, zonelist,
					       nodemask);
	if (unlikely(!(*did_some_progress)))
		return NULL;

	/* After successful reclaim, reconsider all zones for allocation */
	if (NUMA_BUILD)
		zlc_clear_zones_full(zonelist);

retry:
	/** 20140621    
	 * reclaim이 완료됐으므로 ALLOC_NO_WATERMARKS 를 제거하고(watermark 체크 안 함)
	 * 다시 페이지를 요청한다.
	 **/
	page = get_page_from_freelist(gfp_mask, nodemask, order,
					zonelist, high_zoneidx,
					alloc_flags & ~ALLOC_NO_WATERMARKS,
					preferred_zone, migratetype);

	/*
	 * If an allocation failed after direct reclaim, it could be because
	 * pages are pinned on the per-cpu lists. Drain them and try again
	 */
	/** 20140621    
	 * 만약 page 확보 후 다시 시도한 뒤에도 메모리를 확보하지 못했다면
	 * per-cpu용으로 보유 중인 page를 해제하고 한 번 더 시도한다.
	 **/
	if (!page && !drained) {
		drain_all_pages();
		drained = true;
		goto retry;
	}

	/** 20140628    
	 * 받아온 페이지를 리턴.
	 **/
	return page;
}

/*
 * This is called in the allocator slow-path if the allocation request is of
 * sufficient urgency to ignore watermarks and take other desperate measures
 */
/** 20131130    
 * slow-path에서도 page를 할당받지 못한 경우,
 * 극단적인 방법으로 watermarks를 무시한 상태에서 페이지 할당을 시도한다.
 *   WATERMARKS를 무시한 상태로 페이지 할당을 요청하며,
 *   GPF_NO_FAIL인 경우 congested가 해제되도록 한다.
 **/
static inline struct page *
__alloc_pages_high_priority(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	/** 20131130    
	 * page를 못 받아온 상황에서 __GFP_NOFAIL mask가 설정되어 있다면
	 * page를 얻어올 때까지 get_page_from_freelist를 NO_WATERMARKS로 시도한다.
	 **/
	do {
		/** 20131123    
		 * alloc_flags를 ALLOC_NO_WATERMARKS로 변경해서 다시 시도한다.
		 **/
		page = get_page_from_freelist(gfp_mask, nodemask, order,
			zonelist, high_zoneidx, ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);

		/** 20131123    
		 * 마찬가지로 page를 할당받는데 실패하고, __GFP_NOFAIL인 경우
		 *   wait_iff_congested를 호출한다.
		 **/
		if (!page && gfp_mask & __GFP_NOFAIL)
			wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
	} while (!page && (gfp_mask & __GFP_NOFAIL));

	return page;
}

/** 20131123    
 * zonelist를 순회하며 kswapd를 깨울지 판단하고, 깨워야 한다면 wake시킨다.
 **/
static inline
void wake_all_kswapd(unsigned int order, struct zonelist *zonelist,
						enum zone_type high_zoneidx,
						enum zone_type classzone_idx)
{
	struct zoneref *z;
	struct zone *zone;

	/** 20131116    
	 * zonelist에서 high_zoneidx보다 작은 zone들에 대해 순회하며
	 * kswapd를 깨울지 판단해 low memory 상태일 경우 깨운다.
	 **/
	for_each_zone_zonelist(zone, z, zonelist, high_zoneidx)
		wakeup_kswapd(zone, order, classzone_idx);
}

/** 20131123    
 * gfp_mask로부터 alloc_flags를 생성한다.
 **/
static inline int
gfp_to_alloc_flags(gfp_t gfp_mask)
{
	/** 20131116    
	 * alloc_flags의 초기값은 ALLOC_WMARK_MIN과 ALLOC_CPUSET을 포함한다.
	 **/
	int alloc_flags = ALLOC_WMARK_MIN | ALLOC_CPUSET;
	/** 20131116    
	 * gfp_mask가 __GFP_WAIT 속성을 포함하는지 여부를 wait에 저장한다.
	 **/
	const gfp_t wait = gfp_mask & __GFP_WAIT;

	/* __GFP_HIGH is assumed to be the same as ALLOC_HIGH to save a branch. */
	BUILD_BUG_ON(__GFP_HIGH != (__force gfp_t) ALLOC_HIGH);

	/*
	 * The caller may dip into page reserves a bit more if the caller
	 * cannot run direct reclaim, or if the caller has realtime scheduling
	 * policy or is asking for __GFP_HIGH memory.  GFP_ATOMIC requests will
	 * set both ALLOC_HARDER (!wait) and ALLOC_HIGH (__GFP_HIGH).
	 */
	/** 20131116    
	 * gfp_mask에 __GFP_HIGH가 포함되어 있다면 alloc_flags 에도 표시한다.
	 * ALLOC_HIGH와 __GFP_HIGH가 같은 값으로 define되어 있음
	 **/
	alloc_flags |= (__force int) (gfp_mask & __GFP_HIGH);

	if (!wait) {
		/** 20131123    
		 * 먼저 wait이 가능하지 않다면 (GFP_ATOMIC의 경우)
		 **/
		/*
		 * Not worth trying to allocate harder for
		 * __GFP_NOMEMALLOC even if it can't schedule.
		 */
		/** 20131123    
		 * __GFP_NOMEMALLOC이 주어져 있다면 ALLOC_HARDER 사용하지 않는다.
		 *
		 * ALLOC_HARDER는 zone_watermark_ok () 에서 WMARK min을 더 높인다.
		 * 즉, reclaim pages 등에 대한 요구사항을 높이는 것이다.
		 **/
		if  (!(gfp_mask & __GFP_NOMEMALLOC))
			alloc_flags |= ALLOC_HARDER;
		/*
		 * Ignore cpuset if GFP_ATOMIC (!wait) rather than fail alloc.
		 * See also cpuset_zone_allowed() comment in kernel/cpuset.c.
		 */
		/** 20131123    
		 * alloc_flags에서 ALLOC_CPUSET 속성을 삭제한다.
		 * ALLOC_CPUSET은 check for correct cpuset. cpuset 검사를 하지 않는다.
		 **/
		alloc_flags &= ~ALLOC_CPUSET;
	} else if (unlikely(rt_task(current)) && !in_interrupt())
		/** 20131123    
		 * !wait이 아닌 경우
		 * 현재 task가 rt_task이면서 interrupt context에 있지 않은 경우
		 * ALLOC_HARDER를 속성에 준다.
		 * 즉, 
		 **/
		alloc_flags |= ALLOC_HARDER;

	/** 20131123    
	 *  __GFP_NOMEMALLOC 속성이 없을 경우
	 **/
	if (likely(!(gfp_mask & __GFP_NOMEMALLOC))) {
		/** 20131123    
		 * __GFP_MEMALLOC 속성이 있다면 watermarks 체크를 하지 않는다.
		 **/
		if (gfp_mask & __GFP_MEMALLOC)
			alloc_flags |= ALLOC_NO_WATERMARKS;
		/** 20131123    
		 * softirq가 처리 중이고, 현재 task의 flags에 PF_MEMALLOC이 설정되어 있다면 watermarks 체크를 하지 않는다.
		 *
		 **/
		else if (in_serving_softirq() && (current->flags & PF_MEMALLOC))
			alloc_flags |= ALLOC_NO_WATERMARKS;
		/** 20131123    
		 * interrupt context가 아니고,
		 *   현재 task의 flags에 PF_MEMALLOC이 설정되어 있거나 TIF_MEMDIE가 thread flag에 포함되어 있다면 watermarks 체크를 하지 않는다.
		 **/
		else if (!in_interrupt() &&
				((current->flags & PF_MEMALLOC) ||
				 unlikely(test_thread_flag(TIF_MEMDIE))))
			alloc_flags |= ALLOC_NO_WATERMARKS;
	}

	/** 20131123    
	 * alloc_flags를 리턴
	 **/
	return alloc_flags;
}

bool gfp_pfmemalloc_allowed(gfp_t gfp_mask)
{
	return !!(gfp_to_alloc_flags(gfp_mask) & ALLOC_NO_WATERMARKS);
}

/** 20140628    
 * slowpath로 메모리 할당을 시도한다.
 * disk IO가 이뤄질 수 있으므로 block 될 수 있다.
 *
 * 1. kswapd를 깨운다.
 * 2. get_page_from_freelist (alloc_flags & ~ALLOC_NO_WATERMARKS)로 시도.
 * 3. if (alloc_flags & ALLOC_NO_WATERMARKS)
 *		 __alloc_pages_high_priority
 *			get_page_from_freelist (ALLOC_NO_WATERMARKS)로 시도.
 *			(NOFAIL인 경우 받아올 때까지 시도)
 * 4. __alloc_pages_direct_reclaim
 *		__perform_reclaim() 후
 *		get_page_from_freelist (~ALLOC_NO_WATERMARKS)로 시도.
 * 5. NORETRY인 경우 __alloc_pages_may_oom
 *		get_page_from_freelist (ALLOC_WMARK_HIGH|ALLOC_CPUSET)로 시도.
 *		out_of_memory().
 * 6. should retry?
 **/
static inline struct page *
__alloc_pages_slowpath(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	/** 20131214    
	 * __GFP_WAIT 여부를 wait에 저장
	 **/
	const gfp_t wait = gfp_mask & __GFP_WAIT;
	struct page *page = NULL;
	int alloc_flags;
	unsigned long pages_reclaimed = 0;
	unsigned long did_some_progress;
	bool sync_migration = false;
	bool deferred_compaction = false;

	/*
	 * In the slowpath, we sanity check order to avoid ever trying to
	 * reclaim >= MAX_ORDER areas which will never succeed. Callers may
	 * be using allocators in order of preference for an area that is
	 * too large.
	 */
	/** 20131116    
	 * order는 MAX_ORDER 이상이면 실패한다.
	 * 현재 MAX_ORDER는 2^10, 즉 buddy로부터 한 번에 할당할 수 있는 최대 크기는
	 * 4MB.
	 *
	 * 20131214
	 * __GFP_NO_WARN 옵션이 주어지면 메모리 할당실패시 경고 메시지를 발생시키지 않는다.
	 **/
	if (order >= MAX_ORDER) {
		WARN_ON_ONCE(!(gfp_mask & __GFP_NOWARN));
		return NULL;
	}

	/*
	 * GFP_THISNODE (meaning __GFP_THISNODE, __GFP_NORETRY and
	 * __GFP_NOWARN set) should not cause reclaim since the subsystem
	 * (f.e. slab) using GFP_THISNODE may choose to trigger reclaim
	 * using a larger set of nodes after it has established that the
	 * allowed per node queues are empty and that nodes are
	 * over allocated.
	 */
	/** 20131116    
	 * vexpress는 NUMA가 아니므로 NUMA_BUILD가 항상 0.
	 **/
	if (NUMA_BUILD && (gfp_mask & GFP_THISNODE) == GFP_THISNODE)
		goto nopage;

restart:
	/** 20131116    
	 * __GFP_NO_KSWAPD가 속성으로 지정되어 있지 않은 경우
	 * zonelist를 순회하며 watermark low 보다 free page가 낮은 zone에 대해 kswap을 실행시킨다.
	 *
	 * kswapd는 node별로 생성되므로, NUMA가 아닌 경우 하나만 생성된다.
	 **/
	if (!(gfp_mask & __GFP_NO_KSWAPD))
		wake_all_kswapd(order, zonelist, high_zoneidx,
						zone_idx(preferred_zone));

	/*
	 * OK, we're below the kswapd watermark and have kicked background
	 * reclaim. Now things get more complex, so set up alloc_flags according
	 * to how we want to proceed.
	 */
	/** 20131123    
	 * gfp_mask를 바탕으로 alloc_flags를 생성한다.
	 **/
	alloc_flags = gfp_to_alloc_flags(gfp_mask);

	/*
	 * Find the true preferred zone if the allocation is unconstrained by
	 * cpusets.
	 */
	/** 20131123    
	 * alloc_flags에 ALLOC_CPUSET이 존재하지 않고 (CPUSET 검사 확인을 하지 않고)
	 *   -> CPUSET을 참고해야 하므로 first_zones_zonelist를 바로 호출하지 않음
	 * nodemask가 없다면,
	 *   first_zones_zonelist으로 high_zoneidx보다 작은 첫번째 zone을
	 *   preferred_zone을 가져온다.
	 **/
	if (!(alloc_flags & ALLOC_CPUSET) && !nodemask)
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
					&preferred_zone);

rebalance:
	/* This is the last chance, in general, before the goto nopage. */
	/** 20131123    
	 * gfp_to_alloc_flags에서 받아온 alloc_flags에서 ALLOC_NO_WATERMARKS 제외한  속성
	 * get_page_from_freelist를 다시 실행한다.
	 **/
	page = get_page_from_freelist(gfp_mask, nodemask, order, zonelist,
			high_zoneidx, alloc_flags & ~ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);
	/** 20131123    
	 * freelist로부터 page를 받았다면 got_pg로 바로 이동.
	 **/
	if (page)
		goto got_pg;

	/* Allocate without watermarks if the context allows */
	/** 20131123    
	 * ~ALLOC_NO_WATERMARKS로 get_page_from_freelist가 실패한 경우
	 *
	 * ALLOC_NO_WATERMARKS 인 경우, (WATERMARKS check를 하지 않는다)
	 *   gfp_to_alloc_flags에서
	 *   - gfp_mask & __GFP_MEMALLOC인 경우
	 *   - (in_serving_softirq() && (current->flags & PF_MEMALLOC))
	 *   - (!in_interrupt() &&
				((current->flags & PF_MEMALLOC) ||
				 unlikely(test_thread_flag(TIF_MEMDIE))))인 경우
	 **/
	if (alloc_flags & ALLOC_NO_WATERMARKS) {
		/*
		 * Ignore mempolicies if ALLOC_NO_WATERMARKS on the grounds
		 * the allocation is high priority and these type of
		 * allocations are system rather than user orientated
		 */
		/** 20131123    
		 * node id 와 gfp_mask 로 적합한 zonelist를 반환한다.
		 **/
		zonelist = node_zonelist(numa_node_id(), gfp_mask);

		/** 20131130    
		 * page를 여전히 할당받지 못한 경우 watermarks 체크를 하지 않고 page 할당한다.
		 * 여전히 할당받지 못한 경우 GFP_NO_FAIL 속성이 주어져 있다면 page를 할당받을 때까지 반복해 시도한다.
		 *
		 * 20131214
		 * WATERMARK_MIN 이하의 메모리를 사용해서라도 메모리를 할당해 줘야 하는 경우
		 * high priority로 수행한다.
		 **/
		page = __alloc_pages_high_priority(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);
		/** 20131130    
		 * 할당 받아 온 경우
		 **/
		if (page) {
			/*
			 * page->pfmemalloc is set when ALLOC_NO_WATERMARKS was
			 * necessary to allocate the page. The expectation is
			 * that the caller is taking steps that will free more
			 * memory. The caller should avoid the page being used
			 * for !PFMEMALLOC purposes.
			 */
			/** 20131130    
			 * page를 할당받기 위해 ALLOC_NO_WATERMARKS가 필요했을 경우
			 * page->pfmemalloc이 설정된다.
			 * 호출자에게 좀더 많은 메모리를 해제하기 위해 추가적인 작업을 수행하도록 기대된다.
			 * 호출자는 !pfmemalloc가 아닌 목적을 위해 page를 사용하는 것을 피해야 한다.
			 *   --> GFP에 __GFP_MEMALLOC가 포함된 경우에 해당.
			 **/
			page->pfmemalloc = true;
			/** 20131130    
			 * got_pg로 이동.
			 **/
			goto got_pg;
		}
	}

	/* Atomic allocations - we can't balance anything */
	/** 20131130    
	 * page를 할당받지 못한 상태이므로
	 * gfp_mask에 __GFP_WAIT이 없는 경우 바로 nopage로 이동
	 **/
	if (!wait)
		goto nopage;

	/* Avoid recursion of direct reclaim */
	/** 20131130    
	 * 현재 task의 flags가 PF_MEMALLOC이 설정되어 있는 경우 바로 nopage로 이동.
	 *
	 * PF_MEMALLOC를 검사해 현재 다른 메모리 할당 요청에 의해 메모리 해제가 진행 중이라면 nopage로 빠져나간다.
	 *
	 *   kswapd, __perform_reclaim, shrink_all_memory 인 경우 속성이 주어진다.
	 *   현재 task가 위 세 가지처럼 메모리 할당을 처리하기 위해 메모리를 요청한 경우, 더 이상 반복적인 과정을 수행하지 않도록 실패로 처리.
	 **/
	if (current->flags & PF_MEMALLOC)
		goto nopage;

	/* Avoid allocations with no watermarks from looping endlessly */
	/** 20131130    
	 * thread_info 의 flag(low level flag)  TIF_MEMDIE이고 (OOM Killer에 의해 제거되는 task),
	 * '실패 없는 할당' 요청이 아닌 경우 nopage.
	 **/
	if (test_thread_flag(TIF_MEMDIE) && !(gfp_mask & __GFP_NOFAIL))
		goto nopage;

	/*
	 * Try direct compaction. The first pass is asynchronous. Subsequent
	 * attempts after direct reclaim are synchronous
	 */
	/** 20131207
	 * vexpress에서는 null 리턴
	 * direct compaction(???)을 비동기로 하고, 다음 코드(__alloc_pages_direct_reclaim
	 * )에서direct reclaim후 동기로 두번째 direct compaction 시도.
	 ***/
	page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&deferred_compaction,
					&did_some_progress);
	/** 20140517    
	 * compaction에서 page 할당이 성공한 경우 got_pg.
	 **/
	if (page)
		goto got_pg;
	sync_migration = true;

	/*
	 * If compaction is deferred for high-order allocations, it is because
	 * sync compaction recently failed. In this is the case and the caller
	 * has requested the system not be heavily disrupted, fail the
	 * allocation now instead of entering direct reclaim
	 */
	/** 20131207
	 * deferred_compaction은 false 이므로 다음코드 진행
	 ***/
	if (deferred_compaction && (gfp_mask & __GFP_NO_KSWAPD))
		goto nopage;

	/* Try direct reclaim and then allocating */
	/** 20140629    
	 * direct reclaim을 수행하고, 할당이 성공하면 page를 리턴받는다.
	 **/
	page = __alloc_pages_direct_reclaim(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, &did_some_progress);
	if (page)
		goto got_pg;

	/*
	 * If we failed to make any progress reclaiming, then we are
	 * running out of options and have to consider going OOM
	 */
	/** 20140628    
	 * __perform_reclaim으로 회수된 페이지가 없다면
	 **/
	if (!did_some_progress) {
		/** 20140628    
		 * low-level FS 접근이 허용되었고, 재시도 금지가 아닐 경우
		 **/
		if ((gfp_mask & __GFP_FS) && !(gfp_mask & __GFP_NORETRY)) {
			/** 20140628    
			 * oom killer가 비활성화 되었다면 nopage. (hibernate 중)
			 **/
			if (oom_killer_disabled)
				goto nopage;
			/* Coredumps can quickly deplete all memory reserves */
			/** 20140628    
			 * 현재 coredump 되었다면 memory를 사용해 버릴 것이므로 (???)
			 * NOFAIL이 아니라면 nopage로 이동한다.
			 **/
			if ((current->flags & PF_DUMPCORE) &&
			    !(gfp_mask & __GFP_NOFAIL))
				goto nopage;
			/** 20140628    
			 * page 할당을 시도하고, 실패시 oom kill을 동작시킨다.
			 * 마지막으로 page 할당이 성공한 경우 got_pg로 이동.
			 **/
			page = __alloc_pages_may_oom(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask, preferred_zone,
					migratetype);
			if (page)
				goto got_pg;

			/** 20140628    
			 * page 할당 실패. oom이 동작한 상태.
			 * NOFAIL이 아닌 경우 order가 PAGE_ALLOC_COSTLY_ORDER 이상이거나
			 * ZONE_NORMAL 이하에 대한 요청인 경우 할당실패로 이동.
			 **/
			if (!(gfp_mask & __GFP_NOFAIL)) {
				/*
				 * The oom killer is not called for high-order
				 * allocations that may fail, so if no progress
				 * is being made, there are no other options and
				 * retrying is unlikely to help.
				 */
				if (order > PAGE_ALLOC_COSTLY_ORDER)
					goto nopage;
				/*
				 * The oom killer is not called for lowmem
				 * allocations to prevent needlessly killing
				 * innocent tasks.
				 */
				if (high_zoneidx < ZONE_NORMAL)
					goto nopage;
			}

			/** 20140628    
			 * slowpath를 처음부터 재시도 한다.
			 **/
			goto restart;
		}
	}

	/* Check if we should retry the allocation */
	/** 20140628    
	 * 회수된 페이지를 pages_reclaimed에 누적하고,
	 * 재시도 해야할지 여부를 검사한다.
	 **/
	pages_reclaimed += did_some_progress;
	if (should_alloc_retry(gfp_mask, order, did_some_progress,
						pages_reclaimed)) {
		/* Wait for some write requests to complete then retry */
		/** 20140628    
		 * io 완료를 BLK_RW_ASYNC로 HZ/50동안 기다린다.
		 * rebalance 부터 다시 시작한다.
		 **/
		wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
		goto rebalance;
	} else {
		/*
		 * High-order allocations do not necessarily loop after
		 * direct reclaim and reclaim/compaction depends on compaction
		 * being called after reclaim so call directly if necessary
		 */
		/** 20140628    
		 * direct compaction으로 통해 page를 확보하면 got_pg로 이동.
		 **/
		page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&deferred_compaction,
					&did_some_progress);
		if (page)
			goto got_pg;
	}

nopage:
	/** 20131207
	 * nopage 상태에서 경고 메시지와 dump stack, memory 정보를 보여준다. 
	 ***/
	warn_alloc_failed(gfp_mask, order, NULL);
	return page;
got_pg:
	if (kmemcheck_enabled)
		kmemcheck_pagealloc_alloc(page, order, gfp_mask);

	return page;
}

/*
 * This is the 'heart' of the zoned buddy allocator.
 */
/** 20140705 
 * "buddy allocator"
 *
 * sequence락을 걸고 cpu_mems_allowed값을 읽어서 선호하는 존을 설정하고
 * 선호하는 존으로부터 watermark를 low 설정하여 freepage를 얻어온다.
 * 처음에는 watermark low로 설정하여 dirty page가 limit을 초과하지 않는
 * zone에서 할당을 시도한다
 * 
 * get_page_from_freelist로 부터 freepage를 얻어오는데 실패하면
 * slowpath를 통해 page할당을 재시도한다.
 */
struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
			struct zonelist *zonelist, nodemask_t *nodemask)
{
	/** 20131116    
	 * gfp_mask로부터 zone type을 받아 high_zoneidx로 삼는다.
	 **/
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	struct zone *preferred_zone;
	struct page *page = NULL;
	int migratetype = allocflags_to_migratetype(gfp_mask);
	unsigned int cpuset_mems_cookie;

	/** 20130907    
	 * gfp_allowed_mask 로 제한된 gfp_mask 속성만 사용할 수 있다.
	 **/
	gfp_mask &= gfp_allowed_mask;

	lockdep_trace_alloc(gfp_mask);

	/** 20130907    
	 * gfp_mask에 __GFP_WAIT 속성이 있다면 might_sleep() 함수 호출.
	 *   resched 가능성을 둔다.
	 * boot time시에는 __GFP_WAIT이 allowed가 아니므로 sleep 가능하지 않다.
	 **/
	might_sleep_if(gfp_mask & __GFP_WAIT);

	if (should_fail_alloc_page(gfp_mask, order))
		return NULL;

	/*
	 * Check the zones suitable for the gfp_mask contain at least one
	 * valid zone. It's possible to have an empty zonelist as a result
	 * of GFP_THISNODE and a memoryless node
	 */
	/** 20130907    
	 * 넘어온 zonelist에 실제 zone이 존재하지 않으면 NULL 리턴.
	 **/
	if (unlikely(!zonelist->_zonerefs->zone))
		return NULL;

retry_cpuset:
	/** 20130928    
	 * seqlock의 시작
	 **/
	cpuset_mems_cookie = get_mems_allowed();

	/* The preferred zone is used for statistics later */
	/** 20130914
	nodemask의 값에따라 
	zonelist에서 첫번째 zoneref가 가리키는 struct zone의 주소를
	cpuset_current_mems_allowed 나 preferred_zone에 채워 준다.
	**/
	first_zones_zonelist(zonelist, high_zoneidx,
				nodemask ? : &cpuset_current_mems_allowed,
				&preferred_zone);
	
	/** 20130928    
	 * 선호하는 zone이 없으면 바로 out.
	 **/
	if (!preferred_zone)
		goto out;

	/* First allocation attempt */
	/** 20131123    
	 * 선호하는 zone이 채우진 경우 freelist로부터 page를 받아온다.
	 *   gfp_mask에 __GFP_HARDWALL로 추가,
	 *   alloc_flags에 ALLOC_WMARK_LOW | ALLOC_CPUSET 지정
	 **/
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask, order,
			zonelist, high_zoneidx, ALLOC_WMARK_LOW|ALLOC_CPUSET,
			preferred_zone, migratetype);
	/** 20131116    
	 * get_page_from_freelist로 페이지를 가져오지 못했을 경우
	 * __alloc_pages_slowpath로 시도한다.
	 */
	if (unlikely(!page))
		page = __alloc_pages_slowpath(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);
	else
		page->pfmemalloc = false;

	trace_mm_page_alloc(page, order, gfp_mask, migratetype);

out:
	/*
	 * When updating a task's mems_allowed, it is possible to race with
	 * parallel threads in such a way that an allocation can fail while
	 * the mask is being updated. If a page allocation is about to fail,
	 * check if the cpuset changed during allocation and if so, retry.
	 */
	/** 20140705
	 * get_mems_allowed로 가져온 값과 seqcount현재 값이 다르고, 
	 * page할당에 실패하면 다시한번 retry_cpuset으로 돌아가서 메모리 할당을 다시 시도한다. 
	 **/
	if (unlikely(!put_mems_allowed(cpuset_mems_cookie) && !page))
		goto retry_cpuset;

	return page;
}
EXPORT_SYMBOL(__alloc_pages_nodemask);

/*
 * Common helper functions.
 */
/** 20140705
 * 1. 메모리 할당을 하기 위한 인터페이스로 사용됨 (kmalloc으로도 호출됨)
 * 2. buddy로부터 물리적으로 연속적으로 2**order만큼 할당하여
 * 가상메모리주소를 반환한다
 */
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	struct page *page;

	/*
	 * __get_free_pages() returns a 32-bit address, which cannot represent
	 * a highmem page
	 */
	/** 20130907    
	 * highmem 에서는 할당 받아 올 수 없는 함수.
	 **/
	VM_BUG_ON((gfp_mask & __GFP_HIGHMEM) != 0);

	page = alloc_pages(gfp_mask, order);
	if (!page)
		return 0;
	return (unsigned long) page_address(page);
}
EXPORT_SYMBOL(__get_free_pages);

unsigned long get_zeroed_page(gfp_t gfp_mask)
{
	return __get_free_pages(gfp_mask | __GFP_ZERO, 0);
}
EXPORT_SYMBOL(get_zeroed_page);

/** 20130831    
 * page 디스크립터를 받아 reference count를 감소시키고,
 * 0이 되면 order만큼 page들을 free 하는 함수
 **/
void __free_pages(struct page *page, unsigned int order)
{
	/** 20130803    
	 * page의 _count를 하나 감소시키고, 그 결과 0이 되었다면
	 * 더 이상 이 페이지를 참조하지 않으므로 free 한다.
	 **/
	if (put_page_testzero(page)) {
		if (order == 0)
			free_hot_cold_page(page, 0);
		else
			__free_pages_ok(page, order);
	}
}

EXPORT_SYMBOL(__free_pages);

/** 20140322    
 * va와 order를 받아 page를 free 하는 함수
 **/
void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_pages(virt_to_page((void *)addr), order);
	}
}

EXPORT_SYMBOL(free_pages);

/** 20150111    
 * 사용할 페이지 다음 페이지부터 할당받은 마지막 페이지까지 해제한다.
 **/
static void *make_alloc_exact(unsigned long addr, unsigned order, size_t size)
{
	if (addr) {
		unsigned long alloc_end = addr + (PAGE_SIZE << order);
		unsigned long used = addr + PAGE_ALIGN(size);

		split_page(virt_to_page((void *)addr), order);
		while (used < alloc_end) {
			free_page(used);
			used += PAGE_SIZE;
		}
	}
	return (void *)addr;
}

/**
 * alloc_pages_exact - allocate an exact number physically-contiguous pages.
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation
 *
 * This function is similar to alloc_pages(), except that it allocates the
 * minimum number of pages to satisfy the request.  alloc_pages() can only
 * allocate memory in power-of-two pages.
 *
 * This function is also limited by MAX_ORDER.
 *
 * Memory allocated by this function must be released by free_pages_exact().
 */
/** 20150103    
 * size만큼 order로 연속적인 물리페이지를 할당받은 뒤,
 * 남은 크기의 페이지들을 해제한다.
 *
 * alloc_pages_exact로 할당받은 페이지는 free_pages_exact로 해제해야 한다.
 * split되어 각 page별로 referenced 되기 때문이다.
 **/
void *alloc_pages_exact(size_t size, gfp_t gfp_mask)
{
	/** 20150103    
	 * 할당받을 크기에 해당하는 order를 구한다.
	 **/
	unsigned int order = get_order(size);
	unsigned long addr;

	/** 20150103    
	 * buddy로부터 2**order개의 페이지를 할당받는다.
	 * 물리적으로 연속적인 메모리가 할당된다.
	 **/
	addr = __get_free_pages(gfp_mask, order);
	/** 20150103    
	 * 할당받은 page에서 size를 제외한 페이지들을 free시킨다.
	 **/
	return make_alloc_exact(addr, order, size);
}
EXPORT_SYMBOL(alloc_pages_exact);

/**
 * alloc_pages_exact_nid - allocate an exact number of physically-contiguous
 *			   pages on a node.
 * @nid: the preferred node ID where memory should be allocated
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation
 *
 * Like alloc_pages_exact(), but try to allocate on node nid first before falling
 * back.
 * Note this is not alloc_pages_exact_node() which allocates on a specific node,
 * but is not exact.
 */
void *alloc_pages_exact_nid(int nid, size_t size, gfp_t gfp_mask)
{
	unsigned order = get_order(size);
	struct page *p = alloc_pages_node(nid, gfp_mask, order);
	if (!p)
		return NULL;
	return make_alloc_exact((unsigned long)page_address(p), order, size);
}
EXPORT_SYMBOL(alloc_pages_exact_nid);

/**
 * free_pages_exact - release memory allocated via alloc_pages_exact()
 * @virt: the value returned by alloc_pages_exact.
 * @size: size of allocation, same value as passed to alloc_pages_exact().
 *
 * Release the memory allocated by a previous call to alloc_pages_exact.
 */
void free_pages_exact(void *virt, size_t size)
{
	unsigned long addr = (unsigned long)virt;
	unsigned long end = addr + PAGE_ALIGN(size);

	while (addr < end) {
		free_page(addr);
		addr += PAGE_SIZE;
	}
}
EXPORT_SYMBOL(free_pages_exact);

/** 20130727    
 * zonelist를 순회하며 조건에 해당하는 free pages들의 개수를 리턴한다.
 **/
static unsigned int nr_free_zone_pages(int offset)
{
	struct zoneref *z;
	struct zone *zone;

	/* Just pick one node, since fallback list is circular */
	unsigned int sum = 0;

	/** 20130727    
	 * node id와 flags로 해당하는 zonelist를 가져온다.
	 **/
	struct zonelist *zonelist = node_zonelist(numa_node_id(), GFP_KERNEL);

	/** 20130727    
	 * zonelist를 순회 (offset은 허용되는 highest zone_type)
	 *   sum에 사용가능한 page의 개수를 누적한다.
	 **/
	for_each_zone_zonelist(zone, z, zonelist, offset) {
		/** 20130727    
		 * 물리 메모리 상에서 제공되는 pages의 개수를 size에 저장
		 * zone의 watermark에 대한 설정이 이루어지지 않았으므로 high는 0.
		 **/
		unsigned long size = zone->present_pages;
		unsigned long high = high_wmark_pages(zone);
		if (size > high)
			sum += size - high;
	}

	return sum;
}

/*
 * Amount of free RAM allocatable within ZONE_DMA and ZONE_NORMAL
 */
/** 20150214    
 * buffer 용으로 사용할 free pages의 수를 계산해 리턴한다.
 *
 * 구현상으로 ZONE_DMA와 ZONE_NORMAL의 free pages의 수가 리턴된다.
 * (HIGHMEM은 포함되지 않음)
 **/
unsigned int nr_free_buffer_pages(void)
{
	/** 20150214    
	 * GFP_USER용으로 메모리 할당이 가능한 zone 리스트를 받아와
	 * free pages의 개수를 합해 리턴한다.
	 **/
	return nr_free_zone_pages(gfp_zone(GFP_USER));
}
EXPORT_SYMBOL_GPL(nr_free_buffer_pages);

/*
 * Amount of free RAM allocatable within all zones
 */
/** 20130727    
 * zonelist를 돌며 gfp_zone(GFP_HIGHUSER_MOVABLE)을 만족하는 pages 들의 개수를
 * 더해 리턴한다.
 **/
unsigned int nr_free_pagecache_pages(void)
{
	/** 20130727    
	 * gfp_zone(GFP_HIGHUSER_MOVABLE)에 의해 ZONE_MOVABLE을 리턴
	 **/
	return nr_free_zone_pages(gfp_zone(GFP_HIGHUSER_MOVABLE));
}

static inline void show_node(struct zone *zone)
{
	if (NUMA_BUILD)
		printk("Node %d ", zone_to_nid(zone));
}

void si_meminfo(struct sysinfo *val)
{
	val->totalram = totalram_pages;
	val->sharedram = 0;
	val->freeram = global_page_state(NR_FREE_PAGES);
	val->bufferram = nr_blockdev_pages();
	val->totalhigh = totalhigh_pages;
	val->freehigh = nr_free_highpages();
	val->mem_unit = PAGE_SIZE;
}

EXPORT_SYMBOL(si_meminfo);

#ifdef CONFIG_NUMA
void si_meminfo_node(struct sysinfo *val, int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	val->totalram = pgdat->node_present_pages;
	val->freeram = node_page_state(nid, NR_FREE_PAGES);
#ifdef CONFIG_HIGHMEM
	val->totalhigh = pgdat->node_zones[ZONE_HIGHMEM].present_pages;
	val->freehigh = zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
#else
	val->totalhigh = 0;
	val->freehigh = 0;
#endif
	val->mem_unit = PAGE_SIZE;
}
#endif

/*
 * Determine whether the node should be displayed or not, depending on whether
 * SHOW_MEM_FILTER_NODES was passed to show_free_areas().
 */
bool skip_free_areas_node(unsigned int flags, int nid)
{
	bool ret = false;
	unsigned int cpuset_mems_cookie;

	if (!(flags & SHOW_MEM_FILTER_NODES))
		goto out;

	do {
		cpuset_mems_cookie = get_mems_allowed();
		ret = !node_isset(nid, cpuset_current_mems_allowed);
	} while (!put_mems_allowed(cpuset_mems_cookie));
out:
	return ret;
}

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
 * Show free area list (used inside shift_scroll-lock stuff)
 * We also calculate the percentage fragmentation. We do this by counting the
 * memory on each free list with the exception of the first item on the list.
 * Suppresses nodes that are not allowed by current's cpuset if
 * SHOW_MEM_FILTER_NODES is passed.
 */
void show_free_areas(unsigned int filter)
{
	int cpu;
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s per-cpu:\n", zone->name);

		for_each_online_cpu(cpu) {
			struct per_cpu_pageset *pageset;

			pageset = per_cpu_ptr(zone->pageset, cpu);

			printk("CPU %4d: hi:%5d, btch:%4d usd:%4d\n",
			       cpu, pageset->pcp.high,
			       pageset->pcp.batch, pageset->pcp.count);
		}
	}

	printk("active_anon:%lu inactive_anon:%lu isolated_anon:%lu\n"
		" active_file:%lu inactive_file:%lu isolated_file:%lu\n"
		" unevictable:%lu"
		" dirty:%lu writeback:%lu unstable:%lu\n"
		" free:%lu slab_reclaimable:%lu slab_unreclaimable:%lu\n"
		" mapped:%lu shmem:%lu pagetables:%lu bounce:%lu\n",
		global_page_state(NR_ACTIVE_ANON),
		global_page_state(NR_INACTIVE_ANON),
		global_page_state(NR_ISOLATED_ANON),
		global_page_state(NR_ACTIVE_FILE),
		global_page_state(NR_INACTIVE_FILE),
		global_page_state(NR_ISOLATED_FILE),
		global_page_state(NR_UNEVICTABLE),
		global_page_state(NR_FILE_DIRTY),
		global_page_state(NR_WRITEBACK),
		global_page_state(NR_UNSTABLE_NFS),
		global_page_state(NR_FREE_PAGES),
		global_page_state(NR_SLAB_RECLAIMABLE),
		global_page_state(NR_SLAB_UNRECLAIMABLE),
		global_page_state(NR_FILE_MAPPED),
		global_page_state(NR_SHMEM),
		global_page_state(NR_PAGETABLE),
		global_page_state(NR_BOUNCE));

	for_each_populated_zone(zone) {
		int i;

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s"
			" free:%lukB"
			" min:%lukB"
			" low:%lukB"
			" high:%lukB"
			" active_anon:%lukB"
			" inactive_anon:%lukB"
			" active_file:%lukB"
			" inactive_file:%lukB"
			" unevictable:%lukB"
			" isolated(anon):%lukB"
			" isolated(file):%lukB"
			" present:%lukB"
			" mlocked:%lukB"
			" dirty:%lukB"
			" writeback:%lukB"
			" mapped:%lukB"
			" shmem:%lukB"
			" slab_reclaimable:%lukB"
			" slab_unreclaimable:%lukB"
			" kernel_stack:%lukB"
			" pagetables:%lukB"
			" unstable:%lukB"
			" bounce:%lukB"
			" writeback_tmp:%lukB"
			" pages_scanned:%lu"
			" all_unreclaimable? %s"
			"\n",
			zone->name,
			K(zone_page_state(zone, NR_FREE_PAGES)),
			K(min_wmark_pages(zone)),
			K(low_wmark_pages(zone)),
			K(high_wmark_pages(zone)),
			K(zone_page_state(zone, NR_ACTIVE_ANON)),
			K(zone_page_state(zone, NR_INACTIVE_ANON)),
			K(zone_page_state(zone, NR_ACTIVE_FILE)),
			K(zone_page_state(zone, NR_INACTIVE_FILE)),
			K(zone_page_state(zone, NR_UNEVICTABLE)),
			K(zone_page_state(zone, NR_ISOLATED_ANON)),
			K(zone_page_state(zone, NR_ISOLATED_FILE)),
			K(zone->present_pages),
			K(zone_page_state(zone, NR_MLOCK)),
			K(zone_page_state(zone, NR_FILE_DIRTY)),
			K(zone_page_state(zone, NR_WRITEBACK)),
			K(zone_page_state(zone, NR_FILE_MAPPED)),
			K(zone_page_state(zone, NR_SHMEM)),
			K(zone_page_state(zone, NR_SLAB_RECLAIMABLE)),
			K(zone_page_state(zone, NR_SLAB_UNRECLAIMABLE)),
			zone_page_state(zone, NR_KERNEL_STACK) *
				THREAD_SIZE / 1024,
			K(zone_page_state(zone, NR_PAGETABLE)),
			K(zone_page_state(zone, NR_UNSTABLE_NFS)),
			K(zone_page_state(zone, NR_BOUNCE)),
			K(zone_page_state(zone, NR_WRITEBACK_TEMP)),
			zone->pages_scanned,
			(zone->all_unreclaimable ? "yes" : "no")
			);
		printk("lowmem_reserve[]:");
		for (i = 0; i < MAX_NR_ZONES; i++)
			printk(" %lu", zone->lowmem_reserve[i]);
		printk("\n");
	}

	for_each_populated_zone(zone) {
 		unsigned long nr[MAX_ORDER], flags, order, total = 0;

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s: ", zone->name);

		spin_lock_irqsave(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++) {
			nr[order] = zone->free_area[order].nr_free;
			total += nr[order] << order;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++)
			printk("%lu*%lukB ", nr[order], K(1UL) << order);
		printk("= %lukB\n", K(total));
	}

	printk("%ld total pagecache pages\n", global_page_state(NR_FILE_PAGES));

	show_swap_cache_info();
}

/** 20130629    
 * 매개변수로 전달받은 zoneref 위치에 zoneref 자료구조 설정
 **/
static void zoneref_set_zone(struct zone *zone, struct zoneref *zoneref)
{
	/** 20130629    
	 * zoneref자료구조 설정
	 *   - zone의 위치를 저장
	 *   - node에서의 zone의 index를 저장
	 **/
	zoneref->zone = zone;
	zoneref->zone_idx = zone_idx(zone);
}

/*
 * Builds allocation fallback zone lists.
 *
 * Add all populated zones of a node to the zonelist.
 */
/** 20130629    
 *  pgdat를 참조해 설정된 zone들을 zonelist의 zone_refs에 추가한다.
 *    : pgdat->node_zones 에서 zone을 가져와 zonelist에 추가한다.
 **/
static int build_zonelists_node(pg_data_t *pgdat, struct zonelist *zonelist,
				int nr_zones, enum zone_type zone_type)
{
	struct zone *zone;

	/** 20130629    
	 * parameter zone_type의 sanity check.
	 **/
	BUG_ON(zone_type >= MAX_NR_ZONES);
	/** 20130629    
	 * zone_type을 하나 증가. ZONE_MOVABLE이 넘어왔을 경우 __MAX_NR_ZONES.
	 **/
	zone_type++;

	do {
		/** 20130629    
		 * zone_type을 하나 감소.
		 **/
		zone_type--;
		/** 20130629    
		 * 해당 node의 zone을 뒤에서부터 하나 가져온다.
		 **/
		zone = pgdat->node_zones + zone_type;
		/** 20130629    
		 * zone->present_pages가 설정된 zone에 대해서 다음 동작 수행
		 **/
		if (populated_zone(zone)) {
			/** 20130629
			 *  nr_zones에 해당하는 zonelist의 _zonerefs에 zone 정보를 설정
			 **/
			zoneref_set_zone(zone,
				&zonelist->_zonerefs[nr_zones++]);
			/** 20130629    
			 * UMA에서는 NULL 함수
			 **/
			check_highest_zone(zone_type);
		}

	} while (zone_type);
	/** 20130629    
	 * 설정한 nr_zones의 개수를 리턴 (매개변수로 0이 넘어왔을 경우)
	 **/
	return nr_zones;
}


/*
 *  zonelist_order:
 *  0 = automatic detection of better ordering.
 *  1 = order by ([node] distance, -zonetype)
 *  2 = order by (-zonetype, [node] distance)
 *
 *  If not NUMA, ZONELIST_ORDER_ZONE and ZONELIST_ORDER_NODE will create
 *  the same zonelist. So only NUMA can configure this param.
 */
#define ZONELIST_ORDER_DEFAULT  0
#define ZONELIST_ORDER_NODE     1
#define ZONELIST_ORDER_ZONE     2

/* zonelist order in the kernel.
 * set_zonelist_order() will set this to NODE or ZONE.
 */
static int current_zonelist_order = ZONELIST_ORDER_DEFAULT;
static char zonelist_order_name[3][8] = {"Default", "Node", "Zone"};


#ifdef CONFIG_NUMA
/* The value user specified ....changed by config */
static int user_zonelist_order = ZONELIST_ORDER_DEFAULT;
/* string for sysctl */
#define NUMA_ZONELIST_ORDER_LEN	16
char numa_zonelist_order[16] = "default";

/*
 * interface for configure zonelist ordering.
 * command line option "numa_zonelist_order"
 *	= "[dD]efault	- default, automatic configuration.
 *	= "[nN]ode 	- order by node locality, then by zone within node
 *	= "[zZ]one      - order by zone, then by locality within zone
 */

static int __parse_numa_zonelist_order(char *s)
{
	if (*s == 'd' || *s == 'D') {
		user_zonelist_order = ZONELIST_ORDER_DEFAULT;
	} else if (*s == 'n' || *s == 'N') {
		user_zonelist_order = ZONELIST_ORDER_NODE;
	} else if (*s == 'z' || *s == 'Z') {
		user_zonelist_order = ZONELIST_ORDER_ZONE;
	} else {
		printk(KERN_WARNING
			"Ignoring invalid numa_zonelist_order value:  "
			"%s\n", s);
		return -EINVAL;
	}
	return 0;
}

static __init int setup_numa_zonelist_order(char *s)
{
	int ret;

	if (!s)
		return 0;

	ret = __parse_numa_zonelist_order(s);
	if (ret == 0)
		strlcpy(numa_zonelist_order, s, NUMA_ZONELIST_ORDER_LEN);

	return ret;
}
early_param("numa_zonelist_order", setup_numa_zonelist_order);

/*
 * sysctl handler for numa_zonelist_order
 */
int numa_zonelist_order_handler(ctl_table *table, int write,
		void __user *buffer, size_t *length,
		loff_t *ppos)
{
	char saved_string[NUMA_ZONELIST_ORDER_LEN];
	int ret;
	static DEFINE_MUTEX(zl_order_mutex);

	mutex_lock(&zl_order_mutex);
	if (write)
		strcpy(saved_string, (char*)table->data);
	ret = proc_dostring(table, write, buffer, length, ppos);
	if (ret)
		goto out;
	if (write) {
		int oldval = user_zonelist_order;
		if (__parse_numa_zonelist_order((char*)table->data)) {
			/*
			 * bogus value.  restore saved string
			 */
			strncpy((char*)table->data, saved_string,
				NUMA_ZONELIST_ORDER_LEN);
			user_zonelist_order = oldval;
		} else if (oldval != user_zonelist_order) {
			mutex_lock(&zonelists_mutex);
			build_all_zonelists(NULL, NULL);
			mutex_unlock(&zonelists_mutex);
		}
	}
out:
	mutex_unlock(&zl_order_mutex);
	return ret;
}


#define MAX_NODE_LOAD (nr_online_nodes)
static int node_load[MAX_NUMNODES];

/**
 * find_next_best_node - find the next node that should appear in a given node's fallback list
 * @node: node whose fallback list we're appending
 * @used_node_mask: nodemask_t of already used nodes
 *
 * We use a number of factors to determine which is the next node that should
 * appear on a given node's fallback list.  The node should not have appeared
 * already in @node's fallback list, and it should be the next closest node
 * according to the distance array (which contains arbitrary distance values
 * from each node to each node in the system), and should also prefer nodes
 * with no CPUs, since presumably they'll have very little allocation pressure
 * on them otherwise.
 * It returns -1 if no node is found.
 */
static int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	int n, val;
	int min_val = INT_MAX;
	int best_node = -1;
	const struct cpumask *tmp = cpumask_of_node(0);

	/* Use the local node if we haven't already */
	if (!node_isset(node, *used_node_mask)) {
		node_set(node, *used_node_mask);
		return node;
	}

	for_each_node_state(n, N_HIGH_MEMORY) {

		/* Don't want a node to appear more than once */
		if (node_isset(n, *used_node_mask))
			continue;

		/* Use the distance array to find the distance */
		val = node_distance(node, n);

		/* Penalize nodes under us ("prefer the next node") */
		val += (n < node);

		/* Give preference to headless and unused nodes */
		tmp = cpumask_of_node(n);
		if (!cpumask_empty(tmp))
			val += PENALTY_FOR_NODE_WITH_CPUS;

		/* Slight preference for less loaded node */
		val *= (MAX_NODE_LOAD*MAX_NUMNODES);
		val += node_load[n];

		if (val < min_val) {
			min_val = val;
			best_node = n;
		}
	}

	if (best_node >= 0)
		node_set(best_node, *used_node_mask);

	return best_node;
}


/*
 * Build zonelists ordered by node and zones within node.
 * This results in maximum locality--normal zone overflows into local
 * DMA zone, if any--but risks exhausting DMA zone.
 */
static void build_zonelists_in_node_order(pg_data_t *pgdat, int node)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	for (j = 0; zonelist->_zonerefs[j].zone != NULL; j++)
		;
	j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 * Build gfp_thisnode zonelists
 */
static void build_thisnode_zonelists(pg_data_t *pgdat)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[1];
	j = build_zonelists_node(pgdat, zonelist, 0, MAX_NR_ZONES - 1);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 * Build zonelists ordered by zone and nodes within zones.
 * This results in conserving DMA zone[s] until all Normal memory is
 * exhausted, but results in overflowing to remote node while memory
 * may still exist in local DMA zone.
 */
static int node_order[MAX_NUMNODES];

static void build_zonelists_in_zone_order(pg_data_t *pgdat, int nr_nodes)
{
	int pos, j, node;
	int zone_type;		/* needs to be signed */
	struct zone *z;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	pos = 0;
	for (zone_type = MAX_NR_ZONES - 1; zone_type >= 0; zone_type--) {
		for (j = 0; j < nr_nodes; j++) {
			node = node_order[j];
			z = &NODE_DATA(node)->node_zones[zone_type];
			if (populated_zone(z)) {
				zoneref_set_zone(z,
					&zonelist->_zonerefs[pos++]);
				check_highest_zone(zone_type);
			}
		}
	}
	zonelist->_zonerefs[pos].zone = NULL;
	zonelist->_zonerefs[pos].zone_idx = 0;
}

static int default_zonelist_order(void)
{
	int nid, zone_type;
	unsigned long low_kmem_size,total_size;
	struct zone *z;
	int average_size;
	/*
         * ZONE_DMA and ZONE_DMA32 can be very small area in the system.
	 * If they are really small and used heavily, the system can fall
	 * into OOM very easily.
	 * This function detect ZONE_DMA/DMA32 size and configures zone order.
	 */
	/* Is there ZONE_NORMAL ? (ex. ppc has only DMA zone..) */
	low_kmem_size = 0;
	total_size = 0;
	for_each_online_node(nid) {
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->present_pages;
				total_size += z->present_pages;
			} else if (zone_type == ZONE_NORMAL) {
				/*
				 * If any node has only lowmem, then node order
				 * is preferred to allow kernel allocations
				 * locally; otherwise, they can easily infringe
				 * on other nodes when there is an abundance of
				 * lowmem available to allocate from.
				 */
				return ZONELIST_ORDER_NODE;
			}
		}
	}
	if (!low_kmem_size ||  /* there are no DMA area. */
	    low_kmem_size > total_size/2) /* DMA/DMA32 is big. */
		return ZONELIST_ORDER_NODE;
	/*
	 * look into each node's config.
  	 * If there is a node whose DMA/DMA32 memory is very big area on
 	 * local memory, NODE_ORDER may be suitable.
         */
	average_size = total_size /
				(nodes_weight(node_states[N_HIGH_MEMORY]) + 1);
	for_each_online_node(nid) {
		low_kmem_size = 0;
		total_size = 0;
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->present_pages;
				total_size += z->present_pages;
			}
		}
		if (low_kmem_size &&
		    total_size > average_size && /* ignore small node */
		    low_kmem_size > total_size * 70/100)
			return ZONELIST_ORDER_NODE;
	}
	return ZONELIST_ORDER_ZONE;
}

static void set_zonelist_order(void)
{
	if (user_zonelist_order == ZONELIST_ORDER_DEFAULT)
		current_zonelist_order = default_zonelist_order();
	else
		current_zonelist_order = user_zonelist_order;
}

static void build_zonelists(pg_data_t *pgdat)
{
	int j, node, load;
	enum zone_type i;
	nodemask_t used_mask;
	int local_node, prev_node;
	struct zonelist *zonelist;
	int order = current_zonelist_order;

	/* initialize zonelists */
	for (i = 0; i < MAX_ZONELISTS; i++) {
		zonelist = pgdat->node_zonelists + i;
		zonelist->_zonerefs[0].zone = NULL;
		zonelist->_zonerefs[0].zone_idx = 0;
	}

	/* NUMA-aware ordering of nodes */
	local_node = pgdat->node_id;
	load = nr_online_nodes;
	prev_node = local_node;
	nodes_clear(used_mask);

	memset(node_order, 0, sizeof(node_order));
	j = 0;

	while ((node = find_next_best_node(local_node, &used_mask)) >= 0) {
		int distance = node_distance(local_node, node);

		/*
		 * If another node is sufficiently far away then it is better
		 * to reclaim pages in a zone before going off node.
		 */
		if (distance > RECLAIM_DISTANCE)
			zone_reclaim_mode = 1;

		/*
		 * We don't want to pressure a particular node.
		 * So adding penalty to the first node in same
		 * distance group to make it round-robin.
		 */
		if (distance != node_distance(local_node, prev_node))
			node_load[node] = load;

		prev_node = node;
		load--;
		if (order == ZONELIST_ORDER_NODE)
			build_zonelists_in_node_order(pgdat, node);
		else
			node_order[j++] = node;	/* remember order */
	}

	if (order == ZONELIST_ORDER_ZONE) {
		/* calculate node order -- i.e., DMA last! */
		build_zonelists_in_zone_order(pgdat, j);
	}

	build_thisnode_zonelists(pgdat);
}

/* Construct the zonelist performance cache - see further mmzone.h */
static void build_zonelist_cache(pg_data_t *pgdat)
{
	struct zonelist *zonelist;
	struct zonelist_cache *zlc;
	struct zoneref *z;

	zonelist = &pgdat->node_zonelists[0];
	zonelist->zlcache_ptr = zlc = &zonelist->zlcache;
	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
	for (z = zonelist->_zonerefs; z->zone; z++)
		zlc->z_to_n[z - zonelist->_zonerefs] = zonelist_node_idx(z);
}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * Return node id of node used for "local" allocations.
 * I.e., first node id of first zone in arg node's generic zonelist.
 * Used for initializing percpu 'numa_mem', which is used primarily
 * for kernel allocations, so use GFP_KERNEL flags to locate zonelist.
 */
int local_memory_node(int node)
{
	struct zone *zone;

	(void)first_zones_zonelist(node_zonelist(node, GFP_KERNEL),
				   gfp_zone(GFP_KERNEL),
				   NULL,
				   &zone);
	return zone->node;
}
#endif

#else	/* CONFIG_NUMA */

/** 20130629    
 * current_zonelist_order에 zonelist_order를 지정
 **/
static void set_zonelist_order(void)
{
	/** 20130629    
	 * zonelist의 order를 ZONE을 기준으로 한다.
	 **/
	current_zonelist_order = ZONELIST_ORDER_ZONE;
}

/** 20130629    
 * pgdat의 zonelists 의 값을 초기화함.
 **/
static void build_zonelists(pg_data_t *pgdat)
{
	int node, local_node;
	enum zone_type j;
	struct zonelist *zonelist;

	/** 20130629    
	 * pgdat의 node_id 는 0
	 **/
	local_node = pgdat->node_id;

	zonelist = &pgdat->node_zonelists[0];
	/** 20130629    
	 * nr_zones : 0
	 * zone_type: ZONE_MOVABLE
	 *
	 * 첫번째 node의 zonelists 를 설정한다.
	 * zonelist에 _zone_ref 멤버를 설정.
	 **/
	j = build_zonelists_node(pgdat, zonelist, 0, MAX_NR_ZONES - 1);

	/*
	 * Now we build the zonelist so that it contains the zones
	 * of all the other nodes.
	 * We don't want to pressure a particular node, so when
	 * building the zones for node N, we make sure that the
	 * zones coming right after the local ones are those from
	 * node N+1 (modulo N)
	 */
	/** 20130629    
	 * 이후의 node들에 대해 zonelists를 설정한다.
	 * vexpress에서 node가 1개이므로 수행되지 않는다.
	 *   NUMA에 해당하는 부분인데 왜 이후 NODE에 대한 내용도 들어가 있을까???
	 **/
	for (node = local_node + 1; node < MAX_NUMNODES; node++) {
		/** 20130629    
		 * online인 node에 대해서만 수행한다.
		 **/
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	}
	/** 20130629    
	 * local_node가 0이므로 수행되지 않음. 
	 * non-NUMA 에 포함된 함수인데 왜 이 for가 필요한 것일까???
	 **/
	for (node = 0; node < local_node; node++) {
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j,
							MAX_NR_ZONES - 1);
	}

	/** 20130629    
	 * 마지막 _zonerefs를 NULL로 설정함.
	 * j는 build_zonelists_node 에서 _zonerefs를 설정하고 ++ 해 리턴한 값.
	 **/
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/* non-NUMA variant of zonelist performance cache - just NULL zlcache_ptr */
/** 20130629    
 * NUMA에서는 cache를 사용할 이유가 없으므로 NULL로 설정.
 **/
static void build_zonelist_cache(pg_data_t *pgdat)
{
	pgdat->node_zonelists[0].zlcache_ptr = NULL;
}

#endif	/* CONFIG_NUMA */

/*
 * Boot pageset table. One per cpu which is going to be used for all
 * zones and all nodes. The parameters will be set in such a way
 * that an item put on a list will immediately be handed over to
 * the buddy list. This is safe since pageset manipulation is done
 * with interrupts disabled.
 *
 * The boot_pagesets must be kept even after bootup is complete for
 * unused processors and/or zones. They do play a role for bootstrapping
 * hotplugged processors.
 *
 * zoneinfo_show() and maybe other functions do
 * not check if the processor is online before following the pageset pointer.
 * Other parts of the kernel may not check if the zone is available.
 */
static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch);
/** 20130427    
 * static __attribute__((section(".data..percpu"))) __typeof__(struct per_cpu_pageset) boot_pageset;
 **/
static DEFINE_PER_CPU(struct per_cpu_pageset, boot_pageset);
static void setup_zone_pageset(struct zone *zone);

/*
 * Global mutex to protect against size modification of zonelists
 * as well as to serialize pageset setup for the new populated zone.
 */
DEFINE_MUTEX(zonelists_mutex);

/* return values int ....just for stop_machine() */
/** 20130629    
 * 1. node마다 존재하는 zonelist를 생성.
 * 2. percpu로 setup_pageset 함수 호출해 자료구조 초기화.
 **/
static int __build_all_zonelists(void *data)
{
	int nid;
	int cpu;
	/** 20130629    
	 * build_all_zonelists 에서 호출 될 때
	 * SYSTEM_BOOTING 상태에서 data가 NULL로 넘어옴.
	 **/
	pg_data_t *self = data;

#ifdef CONFIG_NUMA
	memset(node_load, 0, sizeof(node_load));
#endif

	/** 20130629    
	 * self가 NULL일 경우 실행 안 됨
	 **/
	if (self && !node_online(self->node_id)) {
		build_zonelists(self);
		build_zonelist_cache(self);
	}

	for_each_online_node(nid) {
		/** 20130629    
		 * contig_page_data의 주소를 얻어옴
		 **/
		pg_data_t *pgdat = NODE_DATA(nid);

		/** 20130629    
		 * pgdat의 zonelist 를 생성함.
		 **/
		build_zonelists(pgdat);
		build_zonelist_cache(pgdat);
	}

	/*
	 * Initialize the boot_pagesets that are going to be used
	 * for bootstrapping processors. The real pagesets for
	 * each zone will be allocated later when the per cpu
	 * allocator is available.
	 *
	 * boot_pagesets are used also for bootstrapping offline
	 * cpus if the system is already booted because the pagesets
	 * are needed to initialize allocators on a specific cpu too.
	 * F.e. the percpu allocator needs the page allocator which
	 * needs the percpu allocator in order to allocate its pagesets
	 * (a chicken-egg dilemma).
	 */
	for_each_possible_cpu(cpu) {
		/** 20130629    
		 * boot_pageset은 PERCPU 변수
		 * static DEFINE_PER_CPU(struct per_cpu_pageset, boot_pageset);
		 *
		 * 각 cpu마다 boot_pageset을 생성한다.
		 **/
		setup_pageset(&per_cpu(boot_pageset, cpu), 0);

/** 20130629    
 * vexpress에서 정의되지 않음
 **/
#ifdef CONFIG_HAVE_MEMORYLESS_NODES
		/*
		 * We now know the "local memory node" for each node--
		 * i.e., the node of the first zone in the generic zonelist.
		 * Set up numa_mem percpu variable for on-line cpus.  During
		 * boot, only the boot cpu should be on-line;  we'll init the
		 * secondary cpus' numa_mem as they come on-line.  During
		 * node/memory hotplug, we'll fixup all on-line cpus.
		 */
		if (cpu_online(cpu))
			set_cpu_numa_mem(cpu, local_memory_node(cpu_to_node(cpu)));
#endif
	}

	return 0;
}

/*
 * Called with zonelists_mutex held always
 * unless system_state == SYSTEM_BOOTING.
 */
/** 20130727    
 * pgdat의 node마다 zonelists를 생성하고,
 * free pages의 수를 더해 vm_total_pages에 저장하고 page_group_by_mobility_disabled 상태를 결정
 **/
void __ref build_all_zonelists(pg_data_t *pgdat, struct zone *zone)
{
	set_zonelist_order();

	if (system_state == SYSTEM_BOOTING) {
		/** 20130629    
		 * SYSTEM_BOOTING 중일 경우 zonelists를 setup한다.
		 **/
		__build_all_zonelists(NULL);
		/** 20130629    
		 * 정의되어 있음. 하지만 early_param이 설정되어 있지 않아 출력되지 않음.
		 **/
		mminit_verify_zonelist();
		cpuset_init_current_mems_allowed();
	} else {
		/* we have to stop all cpus to guarantee there is no user
		   of zonelist */
#ifdef CONFIG_MEMORY_HOTPLUG
		if (zone)
			setup_zone_pageset(zone);
#endif
		/** 20130720 stop_machine 추후 분석하기로 함 ???  
		 **/
		stop_machine(__build_all_zonelists, pgdat, NULL);
		/* cpuset refresh routine should be here */
	}
	/** 20130727
	 * 유효한 최대 page의 수를 저장
	 **/
	vm_total_pages = nr_free_pagecache_pages();
	/*
	 * Disable grouping by mobility if the number of pages in the
	 * system is too low to allow the mechanism to work. It would be
	 * more accurate, but expensive to check per-zone. This check is
	 * made on memory-hotadd so a system can start with mobility
	 * disabled and enable it later
	 */
	/** 20130727    
	 * 유효한 최대 page 수가
	 * pageblock 단위의 migrate을 위해 필요한 갯수보다 작다면 
	 * page_group_by_mobility_disabled을 1로 설정,
	 * 그렇지 않다면 0으로 설정.
	 **/
	if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES))
		page_group_by_mobility_disabled = 1;
	else
		page_group_by_mobility_disabled = 0;

	/** 20130727    
	 * booting 시 information 메시지 출력
	 *
	 *  vexpress qemu에서 실행시
	 *  Built 1 zonelists in Zone order, mobility grouping on.  Total pages: 32448
	 **/
	printk("Built %i zonelists in %s order, mobility grouping %s.  "
		"Total pages: %ld\n",
			nr_online_nodes,
			zonelist_order_name[current_zonelist_order],
			page_group_by_mobility_disabled ? "off" : "on",
			vm_total_pages);
#ifdef CONFIG_NUMA
	/** 20130727    
	 * NUMA일 때만 출력
	 **/
	printk("Policy zone: %s\n", zone_names[policy_zone]);
#endif
}

/*
 * Helper functions to size the waitqueue hash table.
 * Essentially these want to choose hash table sizes sufficiently
 * large so that collisions trying to wait on pages are rare.
 * But in fact, the number of active page waitqueues on typical
 * systems is ridiculously low, less than 200. So this is even
 * conservative, even though it seems large.
 *
 * The constant PAGES_PER_WAITQUEUE specifies the ratio of pages to
 * waitqueues, i.e. the size of the waitq table given the number of pages.
 */
#define PAGES_PER_WAITQUEUE	256

#ifndef CONFIG_MEMORY_HOTPLUG

/** 20130427    
 * wait table에서 hash bucket으로 사용할 queue의 entry 개수를 구한다.
 **/
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	unsigned long size = 1;

	/** 20130427    
	 * 전체 pages(spanned)를 WAITQUEUE 당 pages 수의 ratio로 나눈다.
	 *
	 * ex) pages가 약 32500개일 경우 /= 256으로 126.
	 **/
	pages /= PAGES_PER_WAITQUEUE;

	/** 20130427    
	 * size를 pages 보다 큰 최소 2의 지수승으로 만든다.
	 * ex) size = 128
	 **/
	while (size < pages)
		size <<= 1;

	/*
	 * Once we have dozens or even hundreds of threads sleeping
	 * on IO we've got bigger problems than wait queue collision.
	 * Limit the size of the wait table to a reasonable size.
	 */
	/** 20130427    
	 * size의 최대값은 4096
	 **/
	size = min(size, 4096UL);

	/** 20130427    
	 * size의 최소값은 4
	 **/
	return max(size, 4UL);
}
#else
/*
 * A zone's size might be changed by hot-add, so it is not possible to determine
 * a suitable size for its wait_table.  So we use the maximum size now.
 *
 * The max wait table size = 4096 x sizeof(wait_queue_head_t).   ie:
 *
 *    i386 (preemption config)    : 4096 x 16 = 64Kbyte.
 *    ia64, x86-64 (no preemption): 4096 x 20 = 80Kbyte.
 *    ia64, x86-64 (preemption)   : 4096 x 24 = 96Kbyte.
 *
 * The maximum entries are prepared when a zone's memory is (512K + 256) pages
 * or more by the traditional way. (See above).  It equals:
 *
 *    i386, x86-64, powerpc(4K page size) : =  ( 2G + 1M)byte.
 *    ia64(16K page size)                 : =  ( 8G + 4M)byte.
 *    powerpc (64K page size)             : =  (32G +16M)byte.
 */
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	return 4096UL;
}
#endif

/*
 * This is an integer logarithm so that shifts can be used later
 * to extract the more random high bits from the multiplicative
 * hash function before the remainder is taken.
 */
/** 20130427    
 * size 개수를 표현하기 위해 필요한 비트수를 구함
 **/
static inline unsigned long wait_table_bits(unsigned long size)
{
	/** 20130427    
	 * ffz: find first zero
	 * ex)  size : 128 (0x80)
	 *     ~size :     (0xffffff7f)
	 * ffz(~size):   7
	 **/
	return ffz(~size);
}

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

/*
 * Check if a pageblock contains reserved pages
 */
static int pageblock_is_reserved(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!pfn_valid_within(pfn) || PageReserved(pfn_to_page(pfn)))
			return 1;
	}
	return 0;
}

/*
 * Mark a number of pageblocks as MIGRATE_RESERVE. The number
 * of blocks reserved is based on min_wmark_pages(zone). The memory within
 * the reserve will tend to store contiguous free pages. Setting min_free_kbytes
 * higher will lead to a bigger reserve which will get freed as contiguous
 * blocks as reclaim kicks in
 */
static void setup_zone_migrate_reserve(struct zone *zone)
{
	unsigned long start_pfn, pfn, end_pfn, block_end_pfn;
	struct page *page;
	unsigned long block_migratetype;
	int reserve;

	/*
	 * Get the start pfn, end pfn and the number of blocks to reserve
	 * We have to be careful to be aligned to pageblock_nr_pages to
	 * make sure that we always check pfn_valid for the first page in
	 * the block.
	 */
	start_pfn = zone->zone_start_pfn;
	end_pfn = start_pfn + zone->spanned_pages;
	start_pfn = roundup(start_pfn, pageblock_nr_pages);
	reserve = roundup(min_wmark_pages(zone), pageblock_nr_pages) >>
							pageblock_order;

	/*
	 * Reserve blocks are generally in place to help high-order atomic
	 * allocations that are short-lived. A min_free_kbytes value that
	 * would result in more than 2 reserve blocks for atomic allocations
	 * is assumed to be in place to help anti-fragmentation for the
	 * future allocation of hugepages at runtime.
	 */
	reserve = min(2, reserve);

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		if (!pfn_valid(pfn))
			continue;
		page = pfn_to_page(pfn);

		/* Watch out for overlapping nodes */
		if (page_to_nid(page) != zone_to_nid(zone))
			continue;

		block_migratetype = get_pageblock_migratetype(page);

		/* Only test what is necessary when the reserves are not met */
		if (reserve > 0) {
			/*
			 * Blocks with reserved pages will never free, skip
			 * them.
			 */
			block_end_pfn = min(pfn + pageblock_nr_pages, end_pfn);
			if (pageblock_is_reserved(pfn, block_end_pfn))
				continue;

			/* If this block is reserved, account for it */
			if (block_migratetype == MIGRATE_RESERVE) {
				reserve--;
				continue;
			}

			/* Suitable for reserving if this block is movable */
			if (block_migratetype == MIGRATE_MOVABLE) {
				set_pageblock_migratetype(page,
							MIGRATE_RESERVE);
				move_freepages_block(zone, page,
							MIGRATE_RESERVE);
				reserve--;
				continue;
			}
		}

		/*
		 * If the reserve is met and this is a previous reserved block,
		 * take it back
		 */
		if (block_migratetype == MIGRATE_RESERVE) {
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
			move_freepages_block(zone, page, MIGRATE_MOVABLE);
		}
	}
}

/*
 * Initially all pages are reserved - free ones are freed
 * up by free_all_bootmem() once the early boot process is
 * done. Non-atomic initialization, single-pass.
 */
/** 20130504
 * zone에 속한 각 page frame들을 순회하며 struct page 구조체를 초기화하고 reserved 상태로 만든다.
 *
 * 20130907    
 * 여기서 reserved 된 page들은 free_all_bootmem()에서 PG_reserved flags를 해제 해준다.
 **/
void __meminit memmap_init_zone(unsigned long size, int nid, unsigned long zone,
		unsigned long start_pfn, enum memmap_context context)
{
	struct page *page;
	unsigned long end_pfn = start_pfn + size;
	unsigned long pfn;
	struct zone *z;

	/** 20130504
	 * highest_memmap_pfn 전역 변수 초기화
	 **/
	if (highest_memmap_pfn < end_pfn - 1)
		highest_memmap_pfn = end_pfn - 1;
	/** 20130504
	 * node의 pglist_data에서 zone에 해당하는 struct zone을 가져온다.
	 **/
	z = &NODE_DATA(nid)->node_zones[zone];

	/** 20130504
	 * start_pfn ~ end_pfn을 순회하며, 각 page에 대해
	 * 1. flags 설정 (zone과 node 정보 저장, reserved 표시)
	 * 2. usage count, mapping count 초기화
	 * 3. PFN이 1024의 배수인 page에 대해서 migrate type을 MIGRATE_MOVABLE 설정
	 * 4. lru list_head 초기화
	 **/
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		/*
		 * There can be holes in boot-time mem_map[]s
		 * handed to this function.  They do not
		 * exist on hotplugged memory.
		 */
		 
		if (context == MEMMAP_EARLY) {
			/** 20130504
			 * early_pfn_valid, early_pfn_in_nid  모두 1 로 define 되어 있어 있음
			 **/
			if (!early_pfn_valid(pfn))
				continue;
			if (!early_pfn_in_nid(pfn, nid))
				continue;
		}
		page = pfn_to_page(pfn);
		/** 20130504
		 * page struct 의 flags에 해당 zone과 node 정보 저장.
		 **/
		set_page_links(page, zone, nid, pfn);
		mminit_verify_page_links(page, zone, nid, pfn);
		/** 20130504
		 * page의 _count (usage count), _mapcount (page table mapping count) 초기화.
		 **/
		init_page_count(page);
		reset_page_mapcount(page);
		/** 20130504
		 * page struct 의 flags에서 PG_reserved 번째 비트를 셋한다. 	
		 **/
		SetPageReserved(page);
		/*
		 * Mark the block movable so that blocks are reserved for
		 * movable at startup. This will force kernel allocations
		 * to reserve their blocks rather than leaking throughout
		 * the address space during boot when many long-lived
		 * kernel allocations are made. Later some blocks near
		 * the start are marked MIGRATE_RESERVE by
		 * setup_zone_migrate_reserve()
		 *
		 * bitmap is created for zone's valid pfn range. but memmap
		 * can be created for invalid pages (for alignment)
		 * check here not to call set_pageblock_migratetype() against
		 * pfn out of zone.
		 */
		 /** 20130504
		  * PFN이 1024의 배수인 page에 대해서 migrate type을 MIGRATE_MOVABLE 설정
		  **/
		if ((z->zone_start_pfn <= pfn)
		    && (pfn < z->zone_start_pfn + z->spanned_pages)
		    && !(pfn & (pageblock_nr_pages - 1)))
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);

		/** 20140504
		 * lru list head 초기화
		 **/
		INIT_LIST_HEAD(&page->lru);
#ifdef WANT_PAGE_VIRTUAL
		/* The shift won't overflow because ZONE_NORMAL is below 4G. */
		if (!is_highmem_idx(zone))
			set_page_address(page, __va(pfn << PAGE_SHIFT));
#endif
	}
}

/** 20130504
 * zoned buddy allocator를 위한 구조체 초기화.
 *
 * free area 를 MAX_ORDER만큼 돌면서 list를 초기화 
 **/
static void __meminit zone_init_free_lists(struct zone *zone)
{
	int order, t;
	/** 20130504
	 * #define for_each_migratetype_order(order, type) \
	 * for (order = 0; order < MAX_ORDER; order++) \
	 *	for (type = 0; type < MIGRATE_TYPES; type++)
	 *
	 * MAX_ORDER : 11
	 * MIGRATE_TYPE : enum MIGRATE_TYPE의 마지막(총갯수)
	 *
	 * free list 초기화 
	 **/
	for_each_migratetype_order(order, t) {
		INIT_LIST_HEAD(&zone->free_area[order].free_list[t]);
		zone->free_area[order].nr_free = 0;
	}
}
/** 20130504
**/
#ifndef __HAVE_ARCH_MEMMAP_INIT
#define memmap_init(size, nid, zone, start_pfn) \
	memmap_init_zone((size), (nid), (zone), (start_pfn), MEMMAP_EARLY)
#endif

/** 20150124    
 * zone의 batchsize를 계산한다.
 **/
static int __meminit zone_batchsize(struct zone *zone)
{
#ifdef CONFIG_MMU
	int batch;

	/*
	 * The per-cpu-pages pools are set to around 1000th of the
	 * size of the zone.  But no more than 1/2 of a meg.
	 *
	 * OK, so we don't know how big the cache is.  So guess.
	 */
	/** 20150124    
	 * batch를 계산한다. (1 <= batch <= 32)
	 **/
	batch = zone->present_pages / 1024;
	if (batch * PAGE_SIZE > 512 * 1024)
		batch = (512 * 1024) / PAGE_SIZE;
	batch /= 4;		/* We effectively *= 4 below */
	if (batch < 1)
		batch = 1;

	/*
	 * Clamp the batch to a 2^n - 1 value. Having a power
	 * of 2 value was found to be more likely to have
	 * suboptimal cache aliasing properties in some cases.
	 *
	 * For example if 2 tasks are alternately allocating
	 * batches of pages, one task can end up with a lot
	 * of pages of one half of the possible page colors
	 * and the other with pages of the other colors.
	 */
	/** 20150124    
	 * cache aliasing 속성 때문에 2**n으로 설정하는 것은
	 * 오히려 성능을 약화시킬 수 있어 2**n - 1로 설정한다.
	 **/
	batch = rounddown_pow_of_two(batch + batch/2) - 1;

	return batch;

#else
	/* The deferral and batching of frees should be suppressed under NOMMU
	 * conditions.
	 *
	 * The problem is that NOMMU needs to be able to allocate large chunks
	 * of contiguous memory as there's no hardware page translation to
	 * assemble apparent contiguous memory from discontiguous pages.
	 *
	 * Queueing large contiguous runs of pages for batching, however,
	 * causes the pages to actually be freed in smaller chunks.  As there
	 * can be a significant delay between the individual batches being
	 * recycled, this leads to the once large chunks of space being
	 * fragmented and becoming unavailable for high-order allocations.
	 */
	return 0;
#endif
}

/** 20130629    
 * per_cpu_pageset 구조체의 주소를 받아 pageset 을 초기화 하는 함수
 **/
static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch)
{
	struct per_cpu_pages *pcp;
	int migratetype;

	memset(p, 0, sizeof(*p));

	/** 20130629    
	 * 매개변수로 받은 p의 element인 per_cpu_pages 구조체의 위치를 pcp에 저장
	 **/
	pcp = &p->pcp;
	/** 20130629    
	 * 초기화
	 **/
	pcp->count = 0;
	pcp->high = 6 * batch;
	pcp->batch = max(1UL, 1 * batch);
	/** 20130629    
	 * MIGRATE_PCPTYPES 이전까지 loop을 돌면서 list초기화
	 *   type별로 list가 구성되는 것으로 보임
	 **/
	for (migratetype = 0; migratetype < MIGRATE_PCPTYPES; migratetype++)
		INIT_LIST_HEAD(&pcp->lists[migratetype]);
}

/*
 * setup_pagelist_highmark() sets the high water mark for hot per_cpu_pagelist
 * to the value high for the pageset p.
 */

/** 20150124    
 * pcp의 highmark 를 설정한다.
 * batch는 최소 1, high/4 사이의 값으로 설정된다.
 **/
static void setup_pagelist_highmark(struct per_cpu_pageset *p,
				unsigned long high)
{
	struct per_cpu_pages *pcp;

	pcp = &p->pcp;
	pcp->high = high;
	pcp->batch = max(1UL, high/4);
	if ((high/4) > (PAGE_SHIFT * 8))
		pcp->batch = PAGE_SHIFT * 8;
}

/** 20150124    
 * zone의 pageset을 percpu로 동적할당 받아 설정한다.
 **/
static void __meminit setup_zone_pageset(struct zone *zone)
{
	int cpu;

	/** 20150124    
	 * cpu 개수만큼 사용할 per_cpu_pageset을 저장한 공간을 동적으로 할당 받는다.
	 **/
	zone->pageset = alloc_percpu(struct per_cpu_pageset);

	/** 20150124    
	 * possible cpu를 순회하며
	 *   zone->pageset에서 해당 cpu용으로 할당된 메모리를 가져와
	 *   zone batchsize를 계산해 pcp를 설정한다.
	 *
	 * zone_pcp_init은 bootstrapping 중 percpu서브시스템이 초기화 안 되었을 때,
	 * 또는 hotplug cpu가 올라오며 percpu 메모리를 요청할 때
	 * pageset을 제공하기 위한 역할이다.
	 *
	 * 자세한 내용은 zone_pcp_init, __build_all_zonelists의 주석을 참고한다.
	 **/
	for_each_possible_cpu(cpu) {
		struct per_cpu_pageset *pcp = per_cpu_ptr(zone->pageset, cpu);

		setup_pageset(pcp, zone_batchsize(zone));

		/** 20150124    
		 * pagelist fraction(분수)이 설정되어 있다면
		 * zone에 존재하는 pages를 해당값으로 나눠 highmark를 설정한다.
		 **/
		if (percpu_pagelist_fraction)
			setup_pagelist_highmark(pcp,
				(zone->present_pages /
					percpu_pagelist_fraction));
	}
}

/*
 * Allocate per cpu pagesets and initialize them.
 * Before this call only boot pagesets were available.
 */
void __init setup_per_cpu_pageset(void)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		setup_zone_pageset(zone);
}

/** 20130427    
 * zone_size_pages 만큼의 page들을 관리하는 zone wait queue의 hash table 초기화
 **/
static noinline __init_refok
int zone_wait_table_init(struct zone *zone, unsigned long zone_size_pages)
{
	int i;
	struct pglist_data *pgdat = zone->zone_pgdat;
	size_t alloc_size;

	/*
	 * The per-page waitqueue mechanism uses hashed waitqueues
	 * per zone.
	 */
	/** 20130427    
	 * wait queue hash table 관련 설정값 초기화
	 **/
	zone->wait_table_hash_nr_entries =
		 wait_table_hash_nr_entries(zone_size_pages);
	zone->wait_table_bits =
		wait_table_bits(zone->wait_table_hash_nr_entries);
	/** 20130427    
	 * wait queue table에 필요한 메모리의 크기를 구한다.
	 **/
	alloc_size = zone->wait_table_hash_nr_entries
					* sizeof(wait_queue_head_t);

	/** 20130427    
	 * 초기화 과정에서 slab은 아직 unavailable.
	 **/
	if (!slab_is_available()) {
		/** 20130427    
		 * 실제 메모리를 할당해 wait_table로 가리킴
		 **/
		zone->wait_table = (wait_queue_head_t *)
			alloc_bootmem_node_nopanic(pgdat, alloc_size);
	} else {
		/*
		 * This case means that a zone whose size was 0 gets new memory
		 * via memory hot-add.
		 * But it may be the case that a new node was hot-added.  In
		 * this case vmalloc() will not be able to use this new node's
		 * memory - this wait_table must be initialized to use this new
		 * node itself as well.
		 * To use this new node's memory, further consideration will be
		 * necessary.
		 */
		/** 20130427    
		 * slab이 사용 가능하면 vmalloc
		 **/
		zone->wait_table = vmalloc(alloc_size);
	}
	/** 20130427    
	 * 설정이 안 되었다면 에러
	 **/
	if (!zone->wait_table)
		return -ENOMEM;

	/** 20130427    
	 * waitqueue head 초기화
	 **/
	for(i = 0; i < zone->wait_table_hash_nr_entries; ++i)
		init_waitqueue_head(zone->wait_table + i);

	return 0;
}

/** 20130427    
 * zone의 pageset 에 boot_pageset 변수의 주소를 저장한다.
 **/
static __meminit void zone_pcp_init(struct zone *zone)
{
	/*
	 * per cpu subsystem is not up at this point. The following code
	 * relies on the ability of the linker to provide the
	 * offset of a (static) per cpu variable into the per cpu area.
	 */
	/** 20130427    
	 * static 전역변수 boot_pageset의 주소를 pageset에 저장.
	 * (boot_pageset은 .data..percpu 섹션에 존재)
	 *
	 * per cpu 서브시스템이 아직 동작하지 않기 때문에,
	 * static per cpu 변수 영역에 위치한 값을 사용한다.
	 **/
	zone->pageset = &boot_pageset;

	if (zone->present_pages)
		printk(KERN_DEBUG "  %s zone: %lu pages, LIFO batch:%u\n",
			zone->name, zone->present_pages,
					 zone_batchsize(zone));
}

/** 20130504
 * 1. zone wait hash table 초기화
 * 2. zone 의 free_area 구조체 초기화
 * 왜 이름이 currently_empty_zone인지???
 *    => 아직 zone의 free_area[o].free_list[t]가 채워지지 않았다.
 **/
int __meminit init_currently_empty_zone(struct zone *zone,
					unsigned long zone_start_pfn,
					unsigned long size,
					enum memmap_context context)
{
	struct pglist_data *pgdat = zone->zone_pgdat;
	int ret;
	/** 20130427    
	 * zone wait table init은 성공시 0 리턴
	 **/
	ret = zone_wait_table_init(zone, size);
	if (ret)
		return ret;
	/** 20130504
	 * zone 의 총 갯수를 저장
	 **/
	pgdat->nr_zones = zone_idx(zone) + 1;
	zone->zone_start_pfn = zone_start_pfn;

	mminit_dprintk(MMINIT_TRACE, "memmap_init",
			"Initialising map node %d zone %lu pfns %lu -> %lu\n",
			pgdat->node_id,
			(unsigned long)zone_idx(zone),
			zone_start_pfn, (zone_start_pfn + size));

	zone_init_free_lists(zone);

	return 0;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
#ifndef CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID
/*
 * Required by SPARSEMEM. Given a PFN, return what node the PFN is on.
 * Architectures may implement their own version but if add_active_range()
 * was used and there are no special requirements, this is a convenient
 * alternative
 */
int __meminit __early_pfn_to_nid(unsigned long pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)
		if (start_pfn <= pfn && pfn < end_pfn)
			return nid;
	/* This is a memory hole */
	return -1;
}
#endif /* CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID */

int __meminit early_pfn_to_nid(unsigned long pfn)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0)
		return nid;
	/* just returns 0 */
	return 0;
}

#ifdef CONFIG_NODES_SPAN_OTHER_NODES
bool __meminit early_pfn_in_nid(unsigned long pfn, int node)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0 && nid != node)
		return false;
	return true;
}
#endif

/**
 * free_bootmem_with_active_regions - Call free_bootmem_node for each active range
 * @nid: The node to free memory on. If MAX_NUMNODES, all nodes are freed.
 * @max_low_pfn: The highest PFN that will be passed to free_bootmem_node
 *
 * If an architecture guarantees that all ranges registered with
 * add_active_ranges() contain no holes and may be freed, this
 * this function may be used instead of calling free_bootmem() manually.
 */
void __init free_bootmem_with_active_regions(int nid, unsigned long max_low_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid) {
		start_pfn = min(start_pfn, max_low_pfn);
		end_pfn = min(end_pfn, max_low_pfn);

		if (start_pfn < end_pfn)
			free_bootmem_node(NODE_DATA(this_nid),
					  PFN_PHYS(start_pfn),
					  (end_pfn - start_pfn) << PAGE_SHIFT);
	}
}

/**
 * sparse_memory_present_with_active_regions - Call memory_present for each active range
 * @nid: The node to call memory_present for. If MAX_NUMNODES, all nodes will be used.
 *
 * If an architecture guarantees that all ranges registered with
 * add_active_ranges() contain no holes and may be freed, this
 * function may be used instead of calling memory_present() manually.
 */
void __init sparse_memory_present_with_active_regions(int nid)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid)
		memory_present(this_nid, start_pfn, end_pfn);
}

/**
 * get_pfn_range_for_nid - Return the start and end page frames for a node
 * @nid: The nid to return the range for. If MAX_NUMNODES, the min and max PFN are returned.
 * @start_pfn: Passed by reference. On return, it will have the node start_pfn.
 * @end_pfn: Passed by reference. On return, it will have the node end_pfn.
 *
 * It returns the start and end page frame of a node based on information
 * provided by an arch calling add_active_range(). If called for a node
 * with no available memory, a warning is printed and the start and end
 * PFNs will be 0.
 */
void __meminit get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn)
{
	unsigned long this_start_pfn, this_end_pfn;
	int i;

	*start_pfn = -1UL;
	*end_pfn = 0;

	for_each_mem_pfn_range(i, nid, &this_start_pfn, &this_end_pfn, NULL) {
		*start_pfn = min(*start_pfn, this_start_pfn);
		*end_pfn = max(*end_pfn, this_end_pfn);
	}

	if (*start_pfn == -1UL)
		*start_pfn = 0;
}

/*
 * This finds a zone that can be used for ZONE_MOVABLE pages. The
 * assumption is made that zones within a node are ordered in monotonic
 * increasing memory addresses so that the "highest" populated zone is used
 */
static void __init find_usable_zone_for_movable(void)
{
	int zone_index;
	for (zone_index = MAX_NR_ZONES - 1; zone_index >= 0; zone_index--) {
		if (zone_index == ZONE_MOVABLE)
			continue;

		if (arch_zone_highest_possible_pfn[zone_index] >
				arch_zone_lowest_possible_pfn[zone_index])
			break;
	}

	VM_BUG_ON(zone_index == -1);
	movable_zone = zone_index;
}

/*
 * The zone ranges provided by the architecture do not include ZONE_MOVABLE
 * because it is sized independent of architecture. Unlike the other zones,
 * the starting point for ZONE_MOVABLE is not fixed. It may be different
 * in each node depending on the size of each node and how evenly kernelcore
 * is distributed. This helper function adjusts the zone ranges
 * provided by the architecture for a given node by using the end of the
 * highest usable zone for ZONE_MOVABLE. This preserves the assumption that
 * zones within a node are in order of monotonic increases memory addresses
 */
static void __meminit adjust_zone_range_for_zone_movable(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	/* Only adjust if ZONE_MOVABLE is on this node */
	if (zone_movable_pfn[nid]) {
		/* Size ZONE_MOVABLE */
		if (zone_type == ZONE_MOVABLE) {
			*zone_start_pfn = zone_movable_pfn[nid];
			*zone_end_pfn = min(node_end_pfn,
				arch_zone_highest_possible_pfn[movable_zone]);

		/* Adjust for ZONE_MOVABLE starting within this range */
		} else if (*zone_start_pfn < zone_movable_pfn[nid] &&
				*zone_end_pfn > zone_movable_pfn[nid]) {
			*zone_end_pfn = zone_movable_pfn[nid];

		/* Check if this whole range is within ZONE_MOVABLE */
		} else if (*zone_start_pfn >= zone_movable_pfn[nid])
			*zone_start_pfn = *zone_end_pfn;
	}
}

/*
 * Return the number of pages a zone spans in a node, including holes
 * present_pages = zone_spanned_pages_in_node() - zone_absent_pages_in_node()
 */
static unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *ignored)
{
	unsigned long node_start_pfn, node_end_pfn;
	unsigned long zone_start_pfn, zone_end_pfn;

	/* Get the start and end of the node and zone */
	get_pfn_range_for_nid(nid, &node_start_pfn, &node_end_pfn);
	zone_start_pfn = arch_zone_lowest_possible_pfn[zone_type];
	zone_end_pfn = arch_zone_highest_possible_pfn[zone_type];
	adjust_zone_range_for_zone_movable(nid, zone_type,
				node_start_pfn, node_end_pfn,
				&zone_start_pfn, &zone_end_pfn);

	/* Check that this node has pages within the zone's required range */
	if (zone_end_pfn < node_start_pfn || zone_start_pfn > node_end_pfn)
		return 0;

	/* Move the zone boundaries inside the node if necessary */
	zone_end_pfn = min(zone_end_pfn, node_end_pfn);
	zone_start_pfn = max(zone_start_pfn, node_start_pfn);

	/* Return the spanned pages */
	return zone_end_pfn - zone_start_pfn;
}

/*
 * Return the number of holes in a range on a node. If nid is MAX_NUMNODES,
 * then all holes in the requested range will be accounted for.
 */
unsigned long __meminit __absent_pages_in_range(int nid,
				unsigned long range_start_pfn,
				unsigned long range_end_pfn)
{
	unsigned long nr_absent = range_end_pfn - range_start_pfn;
	unsigned long start_pfn, end_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
		start_pfn = clamp(start_pfn, range_start_pfn, range_end_pfn);
		end_pfn = clamp(end_pfn, range_start_pfn, range_end_pfn);
		nr_absent -= end_pfn - start_pfn;
	}
	return nr_absent;
}

/**
 * absent_pages_in_range - Return number of page frames in holes within a range
 * @start_pfn: The start PFN to start searching for holes
 * @end_pfn: The end PFN to stop searching for holes
 *
 * It returns the number of pages frames in memory holes within a range.
 */
unsigned long __init absent_pages_in_range(unsigned long start_pfn,
							unsigned long end_pfn)
{
	return __absent_pages_in_range(MAX_NUMNODES, start_pfn, end_pfn);
}

/* Return the number of page frames in holes in a zone on a node */
static unsigned long __meminit zone_absent_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *ignored)
{
	unsigned long zone_low = arch_zone_lowest_possible_pfn[zone_type];
	unsigned long zone_high = arch_zone_highest_possible_pfn[zone_type];
	unsigned long node_start_pfn, node_end_pfn;
	unsigned long zone_start_pfn, zone_end_pfn;

	get_pfn_range_for_nid(nid, &node_start_pfn, &node_end_pfn);
	zone_start_pfn = clamp(node_start_pfn, zone_low, zone_high);
	zone_end_pfn = clamp(node_end_pfn, zone_low, zone_high);

	adjust_zone_range_for_zone_movable(nid, zone_type,
			node_start_pfn, node_end_pfn,
			&zone_start_pfn, &zone_end_pfn);
	return __absent_pages_in_range(nid, zone_start_pfn, zone_end_pfn);
}

#else /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */
/** 20130413
 * zone_type에 해당하는 zone 의 size를 리턴.
 **/
static inline unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long *zones_size)
{
	return zones_size[zone_type];
}

/** 20130413
 * zone_type에 해당하는 hole size를 리턴.
 **/
static inline unsigned long __meminit zone_absent_pages_in_node(int nid,
						unsigned long zone_type,
						unsigned long *zholes_size)
{
	if (!zholes_size)
		return 0;

	return zholes_size[zone_type];
}

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

/** 20130413
 * totalpages (*pgdat->node_spanned_pages), realtotalpages ( pgdat->node_present_pages) 를 구한다. 
 */
static void __meminit calculate_node_totalpages(struct pglist_data *pgdat,
		unsigned long *zones_size, unsigned long *zholes_size)
{
	/** 20130413
	 * totalpages 는 zone size를 합친 것.
	 * realtotalpages 는 hole size를 뺀 것.  
	 */
	unsigned long realtotalpages, totalpages = 0;
	enum zone_type i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		totalpages += zone_spanned_pages_in_node(pgdat->node_id, i,
								zones_size);
	/** 20130413
	 * total size of physical page range, including holes
	 */
	pgdat->node_spanned_pages = totalpages;

	realtotalpages = totalpages;
	for (i = 0; i < MAX_NR_ZONES; i++)
		realtotalpages -=
			zone_absent_pages_in_node(pgdat->node_id, i,
								zholes_size);
	/** 20130413
	 * total number of physical pages (hole을 뺀 것)
	 */
	pgdat->node_present_pages = realtotalpages;
	printk(KERN_DEBUG "On node %d totalpages: %lu\n", pgdat->node_id,
							realtotalpages);
}

#ifndef CONFIG_SPARSEMEM
/*
 * Calculate the size of the zone->blockflags rounded to an unsigned long
 * Start by making sure zonesize is a multiple of pageblock_order by rounding
 * up. Then use 1 NR_PAGEBLOCK_BITS worth of bits per pageblock, finally
 * round what is now in bits to nearest long in bits, then return it in
 * bytes.
 */
/** 20130504
 * zonesize에 해당하는 pageblock을 비트맵으로 표현할때
 * 필요한 바이트단위 크기
 **/
static unsigned long __init usemap_size(unsigned long zonesize)
{
	unsigned long usemapsize;
	
	/** 20130504
	 * zonesize를 pageblock_nr_pages(1024)단위로 올림
	 **/
	usemapsize = roundup(zonesize, pageblock_nr_pages);
	/** 20130504
	 * pageblock_order로 나눈다.
	 **/
	usemapsize = usemapsize >> pageblock_order;
	/** 20130504
	 * 3으로 곱한다.
	 * pageblock 을 나타내기 위한 비트수가 3개 인듯???
	 * 그래서pageblock를 표현하기위해서 필요한 총 비트수를 구한다???
	 **/
	usemapsize *= NR_PAGEBLOCK_BITS;
	/** 20130504
	 * unsigned long단위로 올림한다.
	 **/
	usemapsize = roundup(usemapsize, 8 * sizeof(unsigned long));

	/** 20130504
	 * 바이트 단위로 usemap size를 리턴
	 **/
	return usemapsize / 8;
}
/** 20130504
 * zonesize에 해당하는 pageblock의 크기를 bootmem으로 할당하여
 * pageblock_flags에 저장한다.
 **/
static void __init setup_usemap(struct pglist_data *pgdat,
				struct zone *zone, unsigned long zonesize)
{
	unsigned long usemapsize = usemap_size(zonesize);
	zone->pageblock_flags = NULL;
	if (usemapsize)
		zone->pageblock_flags = alloc_bootmem_node_nopanic(pgdat,
								   usemapsize);
}
#else
static inline void setup_usemap(struct pglist_data *pgdat,
				struct zone *zone, unsigned long zonesize) {}
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE

/* Initialise the number of pages represented by NR_PAGEBLOCK_BITS */
void __init set_pageblock_order(void)
{
	unsigned int order;

	/* Check that pageblock_nr_pages has not already been setup */
	if (pageblock_order)
		return;

	if (HPAGE_SHIFT > PAGE_SHIFT)
		order = HUGETLB_PAGE_ORDER;
	else
		order = MAX_ORDER - 1;

	/*
	 * Assume the largest contiguous order of interest is a huge page.
	 * This value may be variable depending on boot parameters on IA64 and
	 * powerpc.
	 */
	pageblock_order = order;
}
#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * When CONFIG_HUGETLB_PAGE_SIZE_VARIABLE is not set, set_pageblock_order()
 * is unused as pageblock_order is set at compile-time. See
 * include/linux/pageblock-flags.h for the values of pageblock_order based on
 * the kernel config
 */
void __init set_pageblock_order(void)
{
}

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * Set up the zone data structures:
 *   - mark all pages reserved
 *   - mark all memory queues empty
 *   - clear the memory bitmaps
 *
 * NOTE: pgdat should get zeroed by caller.
 */

/** 20130511 
 * 각 zone 별 구조체를 초기화한다.
 *  - 모든 페이지들을 reserved로 설정한다.
 *  - waitqueue 초기화???
 *  - 메모리 비트맵을 클리어한다.???	
 **/
static void __paginginit free_area_init_core(struct pglist_data *pgdat,
		unsigned long *zones_size, unsigned long *zholes_size)
{
	enum zone_type j;
	int nid = pgdat->node_id;
	unsigned long zone_start_pfn = pgdat->node_start_pfn;
	int ret;

	/** 20130427    
	 * CONFIG_MEMORY_HOTPLUG 가 정의되어 있지 않아 NULL 함수
	 **/
	pgdat_resize_init(pgdat);
	/** 20130427    
	 * pgdat의 wait_queue_head_t 자료구조 초기화
	 *
	 * 20131116
	 * wakeup_kswapd 에서 깨운다.
	 **/
	init_waitqueue_head(&pgdat->kswapd_wait);
	init_waitqueue_head(&pgdat->pfmemalloc_wait);
	/** 20130427    
	 * vexpress에서는 NULL 함수
	 **/
	pgdat_page_cgroup_init(pgdat);

	/** 20130511 
	 * zone의 모든 영역을 순회하면서 zone 구조체의 내용을 채운다. 
	 *
	 * 20131214
	 * node에 속한 zone들을 순회하면서 zone 자료구조를 채운다. node_zones는 array.
	 **/
	for (j = 0; j < MAX_NR_ZONES; j++) {
		struct zone *zone = pgdat->node_zones + j;
		unsigned long size, realsize, memmap_pages;

		/** 20130427    
		 * zones_size는 매개변수로 받은 zone들의 size 배열.
		 *   ZONE_NORMAL은 max_low - min (단위 pfn)
		 **/
		size = zone_spanned_pages_in_node(nid, j, zones_size);
		/** 20130427    
		 * spanned_pages - absent_pages로 hole을 제외한 실제 크기를 구한다.
		 **/
		realsize = size - zone_absent_pages_in_node(nid, j,
								zholes_size);

		/*
		 * Adjust realsize so that it accounts for how much memory
		 * is used by this zone for memmap. This affects the watermark
		 * and per-cpu initialisations
		 */
		/** 20130427    
		 * 전체 (spanned) 메모리 관리에 필요한 struct page의 메모리 크기를 구해,
		 * 필요한 PAGE FRAME의 개수를 구한다.
		 **/	
		memmap_pages =
			PAGE_ALIGN(size * sizeof(struct page)) >> PAGE_SHIFT;
		/** 20130427    
		 * 실제 사용가능한 page frame의 개수가 메모리 관리를 위해 사용되는 page frame 보다 커야함 
		 **/
		if (realsize >= memmap_pages) {
			/** 20130427    
			 * realsize를 줄임
			 **/
			realsize -= memmap_pages;
			if (memmap_pages)
				printk(KERN_DEBUG
				       "  %s zone: %lu pages used for memmap\n",
				       zone_names[j], memmap_pages);
		} else
			printk(KERN_WARNING
				"  %s zone: %lu pages exceeds realsize %lu\n",
				zone_names[j], memmap_pages, realsize);

		/* Account for reserved pages */
		/** 20130427    
		 * 첫번째 zone에 대해 realsize가 dma_reserve보다 크면 realsize에서 뺀다.
		 * (ZONE_DMA로 사용해야 할 공간을 realsize에서 뺀다)
		 **/
		if (j == 0 && realsize > dma_reserve) {
			realsize -= dma_reserve;
			printk(KERN_DEBUG "  %s zone: %lu pages reserved\n",
					zone_names[0], dma_reserve);
		}

		/** 20130427    
		 * j가 highmem index가 아닐 경우 nr_kernel_pages에 realsize를 더함
		 **/
		if (!is_highmem_idx(j))
			nr_kernel_pages += realsize;
		/** 20130427    
		 * nr_all_pages에 realsize를 더함 
		 **/
		nr_all_pages += realsize;

		/** 20130427    
		 * spanned_pages와 presend_pages 에 구한 크기를 저장
		 **/
		zone->spanned_pages = size;
		zone->present_pages = realsize;
		/** 20130427    
		 * vexpress 에서는 정의되어 있지 않음
		 **/
#if defined CONFIG_COMPACTION || defined CONFIG_CMA
		zone->compact_cached_free_pfn = zone->zone_start_pfn +
						zone->spanned_pages;
		zone->compact_cached_free_pfn &= ~(pageblock_nr_pages-1);
#endif
		/** 20130427    
		 * vexpress 에서는 CONFIG_NUMA가 정의되어 있지 않음
		 **/
#ifdef CONFIG_NUMA
		zone->node = nid;
		zone->min_unmapped_pages = (realsize*sysctl_min_unmapped_ratio)
						/ 100;
		zone->min_slab_pages = (realsize * sysctl_min_slab_ratio) / 100;
#endif
		zone->name = zone_names[j];
		/** 20130427    
		 * spin_lock 변수 초기화
		 **/
		spin_lock_init(&zone->lock);
		spin_lock_init(&zone->lru_lock);
		/** 20130427    
		 * vepxress 에서는 NULL 함수
		 **/
		zone_seqlock_init(zone);
		zone->zone_pgdat = pgdat;

		/** 20130427    
		 * zone_pcp, lruvec 자료구조 초기화
		 **/
		zone_pcp_init(zone);
		lruvec_init(&zone->lruvec, zone);
		if (!size)
			continue;

		/** 20130427    
		 * vexpress에서는 CONFIG_HUGETLB_PAGE_SIZE_VARIABLE 가 정의되어 있지 않아 NULL 함수
		 **/
		set_pageblock_order();	
		/** 20130504
		 * zone에 해당하는 크기를 비트맵메모리로(bootmem) 할당하여
		 * pageblock_flags에 저장한다.
		 **/
		/** 20130511 
		 * 해당 zone을 페이지블록 단위로 표현하기 위한 byte를 구하고 시작 주소를
		 * pageblock_flags에 저장한다.	
		 **/
		setup_usemap(pgdat, zone, size);
		ret = init_currently_empty_zone(zone, zone_start_pfn,
						size, MEMMAP_EARLY);
		BUG_ON(ret);
		/** 20130504
		 * zone의 각 page frame 각각에 대해 struct page 구조체를 초기화하고 reserved 상태로 만든다.
		 **/
		memmap_init(size, nid, j, zone_start_pfn);
		/** 20130504
		 * 다음 zone의 시작 pfn을 설정한다.
		 **/
		zone_start_pfn += size;

		/** 20130511
		 * zones_size의 MOVABLE 영역은 size가 0이므로 초기화하거나 할당되어지는 부분이 없다.
		 **/
	}
}

/** 20130427    
 * node에 속한 물리 메모리를 struct page로 관리하기 위한 공간을 할당받는 함수.
 *
 * 전역변수 mem_map을 NODE_DATA(0)->node_mem_map로 설정.
 **/
static void __init_refok alloc_node_mem_map(struct pglist_data *pgdat)
{
	/* Skip empty nodes */
	if (!pgdat->node_spanned_pages)
		return;

#ifdef CONFIG_FLAT_NODE_MEM_MAP
	/* ia64 gets its own node_mem_map, before this, without bootmem */
	/** 20130420    
	 * node_mem_map은 초기에 0이므로 수행
	 **/
	if (!pgdat->node_mem_map) {
		unsigned long size, start, end;
		struct page *map;

		/*
		 * The zone's endpoints aren't required to be MAX_ORDER
		 * aligned but the node_mem_map endpoints must be in order
		 * for the buddy allocator to function correctly.
		 */
		/** 20130413 
		 * zone의 끝은 MAX_ORDER로 align될 필요가 없지만,
		 * node_mem_map의 끝은 buddy allocator가 정상적으로 동작하기 위해 order 단위로 align되어야 한다.
		 * 
		 * start pfn은 round down, end pfn은 hole을 포함한 크기를 계산하여 round up.
		 **/
		start = pgdat->node_start_pfn & ~(MAX_ORDER_NR_PAGES - 1);
		end = pgdat->node_start_pfn + pgdat->node_spanned_pages;
		end = ALIGN(end, MAX_ORDER_NR_PAGES);
		/** 20130420    
		 * size는 pfn 수만큼 struct page를 저장하기 위한 크기 계산.
		 **/
		size =  (end - start) * sizeof(struct page);
		/** 20130413
		 * alloc_remap 은 NULL 리턴.
		 * size만큼의 메모리를 bootmem을 통해 할당.
		 */
		map = alloc_remap(pgdat->node_id, size);
		if (!map)
			map = alloc_bootmem_node_nopanic(pgdat, size);
		/** 20130420    
		 * map은 정렬된 크기로 할당받은 메모리의 시작 위치.
		 * 정렬시킨 위치에서 정렬되지 않은 영역까지는 실제 사용 가능한
		 * page frame들이 존재하지 않으므로 node_mem_map에 저장되는 주소를 조정.
		 **/
		pgdat->node_mem_map = map + (pgdat->node_start_pfn - start);
	}
#ifndef CONFIG_NEED_MULTIPLE_NODES
	/*
	 * With no DISCONTIG, the global mem_map is just set as node 0's
	 */
	/** 20130420
	 * 전역변수 mem_map 설정.
	 * memory model이 DISCONTIG가 아닐 경우(FLATMEM), 단순히 NODE 0의 node_mem_map이 설정.
	 *
	 * __page_to_pfn, __pfn_to_page 에서 사용.
	 **/
	if (pgdat == NODE_DATA(0)) {
		mem_map = NODE_DATA(0)->node_mem_map;
		/** 20130420    
		 * vexpress에서 정의되어 있지 않음
		 **/
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
		if (page_to_pfn(mem_map) != pgdat->node_start_pfn)
			mem_map -= (pgdat->node_start_pfn - ARCH_PFN_OFFSET);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */
	}
#endif
#endif /* CONFIG_FLAT_NODE_MEM_MAP */
}

/** 20130511
 * node의 free_area를 초기화 한다.
 *
 * 1. nid해당하는 pgdat를 가져와서 해당 node에 필요한 총 페이지수를 구한다.
 * 2. 그 페이지를 관리하기 위한 구조체페이지에 공간(bitmap)을 할당하고 그 시작주소를 저장한다.
 * 3. 각 node에 해당되는 zone(zone 구조체와 zone에 속하는 page 구조체 정보)을 초기화한다. 
 **/
void __paginginit free_area_init_node(int nid, unsigned long *zones_size,
		unsigned long node_start_pfn, unsigned long *zholes_size)
{
	/** 20130413
	 * pgdat 는 UMA에서 contig_page_data의 주소
	 **/
	pg_data_t *pgdat = NODE_DATA(nid);

	/* pg_data_t should be reset to zero when it's allocated */
	/** 20130413
	 * nr_zones, classzone_idx 는 아직 설정되지 않은 상태로 초기값 0 으로 추측. ???
	 **/
	WARN_ON(pgdat->nr_zones || pgdat->classzone_idx);

	/** 20130413
	 * nid = 0
	 **/
	pgdat->node_id = nid;
	pgdat->node_start_pfn = node_start_pfn;

	
	/** 20130413
	 * totalpages (*pgdat->node_spanned_pages), realtotalpages ( pgdat->node_present_pages) 를 구한다. 
	 **/
	calculate_node_totalpages(pgdat, zones_size, zholes_size);

	/** 20130427    
	 * node에 속하는 물리 메모리에 대한 descriptor인 struct page들을 저장할 공간을 할당 받는다.
	 **/
	alloc_node_mem_map(pgdat);
#ifdef CONFIG_FLAT_NODE_MEM_MAP
	printk(KERN_DEBUG "free_area_init_node: node %d, pgdat %08lx, node_mem_map %08lx\n",
		nid, (unsigned long)pgdat,
		(unsigned long)pgdat->node_mem_map);
#endif

	/** 20130511 
	 * 각 zone을 초기화 시켜준다.
	 **/
	free_area_init_core(pgdat, zones_size, zholes_size);
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP

#if MAX_NUMNODES > 1
/*
 * Figure out the number of possible node ids.
 */
static void __init setup_nr_node_ids(void)
{
	unsigned int node;
	unsigned int highest = 0;

	for_each_node_mask(node, node_possible_map)
		highest = node;
	nr_node_ids = highest + 1;
}
#else
static inline void setup_nr_node_ids(void)
{
}
#endif

/**
 * node_map_pfn_alignment - determine the maximum internode alignment
 *
 * This function should be called after node map is populated and sorted.
 * It calculates the maximum power of two alignment which can distinguish
 * all the nodes.
 *
 * For example, if all nodes are 1GiB and aligned to 1GiB, the return value
 * would indicate 1GiB alignment with (1 << (30 - PAGE_SHIFT)).  If the
 * nodes are shifted by 256MiB, 256MiB.  Note that if only the last node is
 * shifted, 1GiB is enough and this function will indicate so.
 *
 * This is used to test whether pfn -> nid mapping of the chosen memory
 * model has fine enough granularity to avoid incorrect mapping for the
 * populated node map.
 *
 * Returns the determined alignment in pfn's.  0 if there is no alignment
 * requirement (single node).
 */
unsigned long __init node_map_pfn_alignment(void)
{
	unsigned long accl_mask = 0, last_end = 0;
	unsigned long start, end, mask;
	int last_nid = -1;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		if (!start || last_nid < 0 || last_nid == nid) {
			last_nid = nid;
			last_end = end;
			continue;
		}

		/*
		 * Start with a mask granular enough to pin-point to the
		 * start pfn and tick off bits one-by-one until it becomes
		 * too coarse to separate the current node from the last.
		 */
		mask = ~((1 << __ffs(start)) - 1);
		while (mask && last_end <= (start & (mask << 1)))
			mask <<= 1;

		/* accumulate all internode masks */
		accl_mask |= mask;
	}

	/* convert mask to number of pages */
	return ~accl_mask + 1;
}

/* Find the lowest pfn for a node */
static unsigned long __init find_min_pfn_for_node(int nid)
{
	unsigned long min_pfn = ULONG_MAX;
	unsigned long start_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, NULL, NULL)
		min_pfn = min(min_pfn, start_pfn);

	if (min_pfn == ULONG_MAX) {
		printk(KERN_WARNING
			"Could not find start_pfn for node %d\n", nid);
		return 0;
	}

	return min_pfn;
}

/**
 * find_min_pfn_with_active_regions - Find the minimum PFN registered
 *
 * It returns the minimum PFN based on information provided via
 * add_active_range().
 */
unsigned long __init find_min_pfn_with_active_regions(void)
{
	return find_min_pfn_for_node(MAX_NUMNODES);
}

/*
 * early_calculate_totalpages()
 * Sum pages in active regions for movable zone.
 * Populate N_HIGH_MEMORY for calculating usable_nodes.
 */
static unsigned long __init early_calculate_totalpages(void)
{
	unsigned long totalpages = 0;
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		unsigned long pages = end_pfn - start_pfn;

		totalpages += pages;
		if (pages)
			node_set_state(nid, N_HIGH_MEMORY);
	}
  	return totalpages;
}

/*
 * Find the PFN the Movable zone begins in each node. Kernel memory
 * is spread evenly between nodes as long as the nodes have enough
 * memory. When they don't, some nodes will have more kernelcore than
 * others
 */
static void __init find_zone_movable_pfns_for_nodes(void)
{
	int i, nid;
	unsigned long usable_startpfn;
	unsigned long kernelcore_node, kernelcore_remaining;
	/* save the state before borrow the nodemask */
	nodemask_t saved_node_state = node_states[N_HIGH_MEMORY];
	unsigned long totalpages = early_calculate_totalpages();
	int usable_nodes = nodes_weight(node_states[N_HIGH_MEMORY]);

	/*
	 * If movablecore was specified, calculate what size of
	 * kernelcore that corresponds so that memory usable for
	 * any allocation type is evenly spread. If both kernelcore
	 * and movablecore are specified, then the value of kernelcore
	 * will be used for required_kernelcore if it's greater than
	 * what movablecore would have allowed.
	 */
	if (required_movablecore) {
		unsigned long corepages;

		/*
		 * Round-up so that ZONE_MOVABLE is at least as large as what
		 * was requested by the user
		 */
		required_movablecore =
			roundup(required_movablecore, MAX_ORDER_NR_PAGES);
		corepages = totalpages - required_movablecore;

		required_kernelcore = max(required_kernelcore, corepages);
	}

	/* If kernelcore was not specified, there is no ZONE_MOVABLE */
	if (!required_kernelcore)
		goto out;

	/* usable_startpfn is the lowest possible pfn ZONE_MOVABLE can be at */
	find_usable_zone_for_movable();
	usable_startpfn = arch_zone_lowest_possible_pfn[movable_zone];

restart:
	/* Spread kernelcore memory as evenly as possible throughout nodes */
	kernelcore_node = required_kernelcore / usable_nodes;
	for_each_node_state(nid, N_HIGH_MEMORY) {
		unsigned long start_pfn, end_pfn;

		/*
		 * Recalculate kernelcore_node if the division per node
		 * now exceeds what is necessary to satisfy the requested
		 * amount of memory for the kernel
		 */
		if (required_kernelcore < kernelcore_node)
			kernelcore_node = required_kernelcore / usable_nodes;

		/*
		 * As the map is walked, we track how much memory is usable
		 * by the kernel using kernelcore_remaining. When it is
		 * 0, the rest of the node is usable by ZONE_MOVABLE
		 */
		kernelcore_remaining = kernelcore_node;

		/* Go through each range of PFNs within this node */
		for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
			unsigned long size_pages;

			start_pfn = max(start_pfn, zone_movable_pfn[nid]);
			if (start_pfn >= end_pfn)
				continue;

			/* Account for what is only usable for kernelcore */
			if (start_pfn < usable_startpfn) {
				unsigned long kernel_pages;
				kernel_pages = min(end_pfn, usable_startpfn)
								- start_pfn;

				kernelcore_remaining -= min(kernel_pages,
							kernelcore_remaining);
				required_kernelcore -= min(kernel_pages,
							required_kernelcore);

				/* Continue if range is now fully accounted */
				if (end_pfn <= usable_startpfn) {

					/*
					 * Push zone_movable_pfn to the end so
					 * that if we have to rebalance
					 * kernelcore across nodes, we will
					 * not double account here
					 */
					zone_movable_pfn[nid] = end_pfn;
					continue;
				}
				start_pfn = usable_startpfn;
			}

			/*
			 * The usable PFN range for ZONE_MOVABLE is from
			 * start_pfn->end_pfn. Calculate size_pages as the
			 * number of pages used as kernelcore
			 */
			size_pages = end_pfn - start_pfn;
			if (size_pages > kernelcore_remaining)
				size_pages = kernelcore_remaining;
			zone_movable_pfn[nid] = start_pfn + size_pages;

			/*
			 * Some kernelcore has been met, update counts and
			 * break if the kernelcore for this node has been
			 * satisified
			 */
			required_kernelcore -= min(required_kernelcore,
								size_pages);
			kernelcore_remaining -= size_pages;
			if (!kernelcore_remaining)
				break;
		}
	}

	/*
	 * If there is still required_kernelcore, we do another pass with one
	 * less node in the count. This will push zone_movable_pfn[nid] further
	 * along on the nodes that still have memory until kernelcore is
	 * satisified
	 */
	usable_nodes--;
	if (usable_nodes && required_kernelcore > usable_nodes)
		goto restart;

	/* Align start of ZONE_MOVABLE on all nids to MAX_ORDER_NR_PAGES */
	for (nid = 0; nid < MAX_NUMNODES; nid++)
		zone_movable_pfn[nid] =
			roundup(zone_movable_pfn[nid], MAX_ORDER_NR_PAGES);

out:
	/* restore the node_state */
	node_states[N_HIGH_MEMORY] = saved_node_state;
}

/* Any regular memory on that node ? */
static void __init check_for_regular_memory(pg_data_t *pgdat)
{
#ifdef CONFIG_HIGHMEM
	enum zone_type zone_type;

	for (zone_type = 0; zone_type <= ZONE_NORMAL; zone_type++) {
		struct zone *zone = &pgdat->node_zones[zone_type];
		if (zone->present_pages) {
			node_set_state(zone_to_nid(zone), N_NORMAL_MEMORY);
			break;
		}
	}
#endif
}

/**
 * free_area_init_nodes - Initialise all pg_data_t and zone data
 * @max_zone_pfn: an array of max PFNs for each zone
 *
 * This will call free_area_init_node() for each active node in the system.
 * Using the page ranges provided by add_active_range(), the size of each
 * zone in each node and their holes is calculated. If the maximum PFN
 * between two adjacent zones match, it is assumed that the zone is empty.
 * For example, if arch_max_dma_pfn == arch_max_dma32_pfn, it is assumed
 * that arch_max_dma32_pfn has no pages. It is also assumed that a zone
 * starts where the previous one ended. For example, ZONE_DMA32 starts
 * at arch_max_dma_pfn.
 */
void __init free_area_init_nodes(unsigned long *max_zone_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, nid;

	/* Record where the zone boundaries are */
	memset(arch_zone_lowest_possible_pfn, 0,
				sizeof(arch_zone_lowest_possible_pfn));
	memset(arch_zone_highest_possible_pfn, 0,
				sizeof(arch_zone_highest_possible_pfn));
	arch_zone_lowest_possible_pfn[0] = find_min_pfn_with_active_regions();
	arch_zone_highest_possible_pfn[0] = max_zone_pfn[0];
	for (i = 1; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		arch_zone_lowest_possible_pfn[i] =
			arch_zone_highest_possible_pfn[i-1];
		arch_zone_highest_possible_pfn[i] =
			max(max_zone_pfn[i], arch_zone_lowest_possible_pfn[i]);
	}
	arch_zone_lowest_possible_pfn[ZONE_MOVABLE] = 0;
	arch_zone_highest_possible_pfn[ZONE_MOVABLE] = 0;

	/* Find the PFNs that ZONE_MOVABLE begins at in each node */
	memset(zone_movable_pfn, 0, sizeof(zone_movable_pfn));
	find_zone_movable_pfns_for_nodes();

	/* Print out the zone ranges */
	printk("Zone ranges:\n");
	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		printk(KERN_CONT "  %-8s ", zone_names[i]);
		if (arch_zone_lowest_possible_pfn[i] ==
				arch_zone_highest_possible_pfn[i])
			printk(KERN_CONT "empty\n");
		else
			printk(KERN_CONT "[mem %0#10lx-%0#10lx]\n",
				arch_zone_lowest_possible_pfn[i] << PAGE_SHIFT,
				(arch_zone_highest_possible_pfn[i]
					<< PAGE_SHIFT) - 1);
	}

	/* Print out the PFNs ZONE_MOVABLE begins at in each node */
	printk("Movable zone start for each node\n");
	for (i = 0; i < MAX_NUMNODES; i++) {
		if (zone_movable_pfn[i])
			printk("  Node %d: %#010lx\n", i,
			       zone_movable_pfn[i] << PAGE_SHIFT);
	}

	/* Print out the early_node_map[] */
	printk("Early memory node ranges\n");
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)
		printk("  node %3d: [mem %#010lx-%#010lx]\n", nid,
		       start_pfn << PAGE_SHIFT, (end_pfn << PAGE_SHIFT) - 1);

	/* Initialise every node */
	mminit_verify_pageflags_layout();
	setup_nr_node_ids();
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		free_area_init_node(nid, NULL,
				find_min_pfn_for_node(nid), NULL);

		/* Any memory on that node */
		if (pgdat->node_present_pages)
			node_set_state(nid, N_HIGH_MEMORY);
		check_for_regular_memory(pgdat);
	}
}

static int __init cmdline_parse_core(char *p, unsigned long *core)
{
	unsigned long long coremem;
	if (!p)
		return -EINVAL;

	coremem = memparse(p, &p);
	*core = coremem >> PAGE_SHIFT;

	/* Paranoid check that UL is enough for the coremem value */
	WARN_ON((coremem >> PAGE_SHIFT) > ULONG_MAX);

	return 0;
}

/*
 * kernelcore=size sets the amount of memory for use for allocations that
 * cannot be reclaimed or migrated.
 */
static int __init cmdline_parse_kernelcore(char *p)
{
	return cmdline_parse_core(p, &required_kernelcore);
}

/*
 * movablecore=size sets the amount of memory for use for allocations that
 * can be reclaimed or migrated.
 */
static int __init cmdline_parse_movablecore(char *p)
{
	return cmdline_parse_core(p, &required_movablecore);
}

early_param("kernelcore", cmdline_parse_kernelcore);
early_param("movablecore", cmdline_parse_movablecore);

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

/**
 * set_dma_reserve - set the specified number of pages reserved in the first zone
 * @new_dma_reserve: The number of pages to mark reserved
 *
 * The per-cpu batchsize and zone watermarks are determined by present_pages.
 * In the DMA zone, a significant percentage may be consumed by kernel image
 * and other unfreeable allocations which can skew the watermarks badly. This
 * function may optionally be used to account for unfreeable pages in the
 * first zone (e.g., ZONE_DMA). The effect will be lower watermarks and
 * smaller per-cpu batchsize.
 */
void __init set_dma_reserve(unsigned long new_dma_reserve)
{
	dma_reserve = new_dma_reserve;
}

void __init free_area_init(unsigned long *zones_size)
{
	free_area_init_node(0, zones_size,
			__pa(PAGE_OFFSET) >> PAGE_SHIFT, NULL);
}

/** 20150111    
 * page_alloc 관련 cpu notify를 처리한다.
 **/
static int page_alloc_cpu_notify(struct notifier_block *self,
				 unsigned long action, void *hcpu)
{
	int cpu = (unsigned long)hcpu;

	/** 20150111    
	 * CPU_DEAD, CPU_DEAD_FROZEN action일 때
	 *
	 * - cpu의 pagevecs 속 pages들을 zone의 lru list로 옮긴다.
	 * - cpu가 보유 중인 percpu pages를 버디 할당자로 되돌린다. 
	 * - vm_events, vm_stats 관련 작업 처리
	 **/
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		lru_add_drain_cpu(cpu);
		drain_pages(cpu);

		/*
		 * Spill the event counters of the dead processor
		 * into the current processors event counters.
		 * This artificially elevates the count of the current
		 * processor.
		 */
		vm_events_fold_cpu(cpu);

		/*
		 * Zero the differential counters of the dead processor
		 * so that the vm statistics are consistent.
		 *
		 * This is only okay since the processor is dead and cannot
		 * race with what we are doing.
		 */
		refresh_cpu_vm_stats(cpu);
	}
	return NOTIFY_OK;
}

/** 20130727    
 * page alloc 을 사용하기 전에 초기화 해준다.
 **/
void __init page_alloc_init(void)
{
	/** 20130727    
	 * page_alloc_cpu_notify를 cpu notifier chain에 priority 0으로  등록한다.
	 **/
	hotcpu_notifier(page_alloc_cpu_notify, 0);
}

/*
 * calculate_totalreserve_pages - called when sysctl_lower_zone_reserve_ratio
 *	or min_free_kbytes changes.
 */
static void calculate_totalreserve_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long reserve_pages = 0;
	enum zone_type i, j;

	for_each_online_pgdat(pgdat) {
		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = pgdat->node_zones + i;
			unsigned long max = 0;

			/* Find valid and maximum lowmem_reserve in the zone */
			for (j = i; j < MAX_NR_ZONES; j++) {
				if (zone->lowmem_reserve[j] > max)
					max = zone->lowmem_reserve[j];
			}

			/* we treat the high watermark as reserved pages. */
			max += high_wmark_pages(zone);

			if (max > zone->present_pages)
				max = zone->present_pages;
			reserve_pages += max;
			/*
			 * Lowmem reserves are not available to
			 * GFP_HIGHUSER page cache allocations and
			 * kswapd tries to balance zones to their high
			 * watermark.  As a result, neither should be
			 * regarded as dirtyable memory, to prevent a
			 * situation where reclaim has to clean pages
			 * in order to balance the zones.
			 */
			zone->dirty_balance_reserve = max;
		}
	}
	dirty_balance_reserve = reserve_pages;
	totalreserve_pages = reserve_pages;
}

/*
 * setup_per_zone_lowmem_reserve - called whenever
 *	sysctl_lower_zone_reserve_ratio changes.  Ensures that each zone
 *	has a correct pages reserved value, so an adequate number of
 *	pages are left in the zone after a successful __alloc_pages().
 */
static void setup_per_zone_lowmem_reserve(void)
{
	struct pglist_data *pgdat;
	enum zone_type j, idx;

	for_each_online_pgdat(pgdat) {
		for (j = 0; j < MAX_NR_ZONES; j++) {
			struct zone *zone = pgdat->node_zones + j;
			unsigned long present_pages = zone->present_pages;

			zone->lowmem_reserve[j] = 0;

			idx = j;
			while (idx) {
				struct zone *lower_zone;

				idx--;

				if (sysctl_lowmem_reserve_ratio[idx] < 1)
					sysctl_lowmem_reserve_ratio[idx] = 1;

				lower_zone = pgdat->node_zones + idx;
				lower_zone->lowmem_reserve[j] = present_pages /
					sysctl_lowmem_reserve_ratio[idx];
				present_pages += lower_zone->present_pages;
			}
		}
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

static void __setup_per_zone_wmarks(void)
{
	unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);
	unsigned long lowmem_pages = 0;
	struct zone *zone;
	unsigned long flags;

	/* Calculate total number of !ZONE_HIGHMEM pages */
	for_each_zone(zone) {
		if (!is_highmem(zone))
			lowmem_pages += zone->present_pages;
	}

	for_each_zone(zone) {
		u64 tmp;

		spin_lock_irqsave(&zone->lock, flags);
		tmp = (u64)pages_min * zone->present_pages;
		do_div(tmp, lowmem_pages);
		if (is_highmem(zone)) {
			/*
			 * __GFP_HIGH and PF_MEMALLOC allocations usually don't
			 * need highmem pages, so cap pages_min to a small
			 * value here.
			 *
			 * The WMARK_HIGH-WMARK_LOW and (WMARK_LOW-WMARK_MIN)
			 * deltas controls asynch page reclaim, and so should
			 * not be capped for highmem.
			 */
			int min_pages;

			min_pages = zone->present_pages / 1024;
			if (min_pages < SWAP_CLUSTER_MAX)
				min_pages = SWAP_CLUSTER_MAX;
			if (min_pages > 128)
				min_pages = 128;
			zone->watermark[WMARK_MIN] = min_pages;
		} else {
			/*
			 * If it's a lowmem zone, reserve a number of pages
			 * proportionate to the zone's size.
			 */
			zone->watermark[WMARK_MIN] = tmp;
		}

		zone->watermark[WMARK_LOW]  = min_wmark_pages(zone) + (tmp >> 2);
		zone->watermark[WMARK_HIGH] = min_wmark_pages(zone) + (tmp >> 1);

		zone->watermark[WMARK_MIN] += cma_wmark_pages(zone);
		zone->watermark[WMARK_LOW] += cma_wmark_pages(zone);
		zone->watermark[WMARK_HIGH] += cma_wmark_pages(zone);

		setup_zone_migrate_reserve(zone);
		spin_unlock_irqrestore(&zone->lock, flags);
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

/**
 * setup_per_zone_wmarks - called when min_free_kbytes changes
 * or when memory is hot-{added|removed}
 *
 * Ensures that the watermark[min,low,high] values for each zone are set
 * correctly with respect to min_free_kbytes.
 */
void setup_per_zone_wmarks(void)
{
	mutex_lock(&zonelists_mutex);
	__setup_per_zone_wmarks();
	mutex_unlock(&zonelists_mutex);
}

/*
 * The inactive anon list should be small enough that the VM never has to
 * do too much work, but large enough that each inactive page has a chance
 * to be referenced again before it is swapped out.
 *
 * The inactive_anon ratio is the target ratio of ACTIVE_ANON to
 * INACTIVE_ANON pages on this zone's LRU, maintained by the
 * pageout code. A zone->inactive_ratio of 3 means 3:1 or 25% of
 * the anonymous pages are kept on the inactive list.
 *
 * total     target    max
 * memory    ratio     inactive anon
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */
static void __meminit calculate_zone_inactive_ratio(struct zone *zone)
{
	unsigned int gb, ratio;

	/* Zone size in gigabytes */
	gb = zone->present_pages >> (30 - PAGE_SHIFT);
	if (gb)
		ratio = int_sqrt(10 * gb);
	else
		ratio = 1;

	zone->inactive_ratio = ratio;
}

static void __meminit setup_per_zone_inactive_ratio(void)
{
	struct zone *zone;

	for_each_zone(zone)
		calculate_zone_inactive_ratio(zone);
}

/*
 * Initialise min_free_kbytes.
 *
 * For small machines we want it small (128k min).  For large machines
 * we want it large (64MB max).  But it is not linear, because network
 * bandwidth does not increase linearly with machine size.  We use
 *
 * 	min_free_kbytes = 4 * sqrt(lowmem_kbytes), for better accuracy:
 *	min_free_kbytes = sqrt(lowmem_kbytes * 16)
 *
 * which yields
 *
 * 16MB:	512k
 * 32MB:	724k
 * 64MB:	1024k
 * 128MB:	1448k
 * 256MB:	2048k
 * 512MB:	2896k
 * 1024MB:	4096k
 * 2048MB:	5792k
 * 4096MB:	8192k
 * 8192MB:	11584k
 * 16384MB:	16384k
 */
int __meminit init_per_zone_wmark_min(void)
{
	unsigned long lowmem_kbytes;

	lowmem_kbytes = nr_free_buffer_pages() * (PAGE_SIZE >> 10);

	min_free_kbytes = int_sqrt(lowmem_kbytes * 16);
	if (min_free_kbytes < 128)
		min_free_kbytes = 128;
	if (min_free_kbytes > 65536)
		min_free_kbytes = 65536;
	setup_per_zone_wmarks();
	refresh_zone_stat_thresholds();
	setup_per_zone_lowmem_reserve();
	setup_per_zone_inactive_ratio();
	return 0;
}
module_init(init_per_zone_wmark_min)

/*
 * min_free_kbytes_sysctl_handler - just a wrapper around proc_dointvec() so 
 *	that we can call two helper functions whenever min_free_kbytes
 *	changes.
 */
int min_free_kbytes_sysctl_handler(ctl_table *table, int write, 
	void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec(table, write, buffer, length, ppos);
	if (write)
		setup_per_zone_wmarks();
	return 0;
}

#ifdef CONFIG_NUMA
int sysctl_min_unmapped_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_unmapped_pages = (zone->present_pages *
				sysctl_min_unmapped_ratio) / 100;
	return 0;
}

int sysctl_min_slab_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_slab_pages = (zone->present_pages *
				sysctl_min_slab_ratio) / 100;
	return 0;
}
#endif

/*
 * lowmem_reserve_ratio_sysctl_handler - just a wrapper around
 *	proc_dointvec() so that we can call setup_per_zone_lowmem_reserve()
 *	whenever sysctl_lowmem_reserve_ratio changes.
 *
 * The reserve ratio obviously has absolutely no relation with the
 * minimum watermarks. The lowmem reserve ratio can only make sense
 * if in function of the boot time zone sizes.
 */
int lowmem_reserve_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec_minmax(table, write, buffer, length, ppos);
	setup_per_zone_lowmem_reserve();
	return 0;
}

/*
 * percpu_pagelist_fraction - changes the pcp->high for each zone on each
 * cpu.  It is the fraction of total pages in each zone that a hot per cpu pagelist
 * can have before it gets flushed back to buddy allocator.
 */

int percpu_pagelist_fraction_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	unsigned int cpu;
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (!write || (ret < 0))
		return ret;
	for_each_populated_zone(zone) {
		for_each_possible_cpu(cpu) {
			unsigned long  high;
			high = zone->present_pages / percpu_pagelist_fraction;
			setup_pagelist_highmark(
				per_cpu_ptr(zone->pageset, cpu), high);
		}
	}
	return 0;
}

/** 20130727    
 * NUMA면 1, 아니면 0
 **/
int hashdist = HASHDIST_DEFAULT;

#ifdef CONFIG_NUMA
static int __init set_hashdist(char *str)
{
	if (!str)
		return 0;
	hashdist = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("hashdist=", set_hashdist);
#endif

/*
 * allocate a large system hash table from bootmem
 * - it is assumed that the hash table must contain an exact power-of-2
 *   quantity of entries
 * - limit is the number of hash buckets, not the total allocation size
 */
/** 20130727    
 * 대형 system hash table을 할당 받아 사용한다.
 * flags에 HASH_EARLY가 있는 경우 bootmem에서 할당.
 *
 * 예를 들어 pidhash_init
 * tablename : "PID"
 * bucketsize : 4
 * numentries : 0
 * scale      : 18
 * flags      : HASH_EARLY | HASH_SMALL
 * _hash_shift : pidhash_shift
 * _hash_mask  : NULL
 * low_limit   : 0
 * high_limit  : 4096
 * 
 * 1. nr_kernel_pages를 MB 단위로 올려 numentries에 저장한다.
 * 2. 1 bucket이 2^scale 단위로 cover할 수 있도록 bucket의 수를 구한 뒤,
 * 그만큼 할당할 수 없다면 절반씩 줄인다.
 * 3. bucketsize * bucket 수만큼 hash table을 할당한다.
 * 4. hash table을 생성할 때 결정된 log2qty 값을 _hash_shift에 저장한다.
 *
 * 부팅시 출력물
 * PID hash table entries: 4096 (order: 2, 16384 bytes)
 * Dentry cache hash table entries: 131072 (order: 7, 524288 bytes)
 * Inode-cache hash table entries: 65536 (order: 6, 262144 bytes)
 * ...
 **/
void *__init alloc_large_system_hash(const char *tablename,
				     unsigned long bucketsize,
				     unsigned long numentries,
				     int scale,
				     int flags,
				     unsigned int *_hash_shift,
				     unsigned int *_hash_mask,
				     unsigned long low_limit,
				     unsigned long high_limit)
{
	unsigned long long max = high_limit;
	unsigned long log2qty, size;
	void *table = NULL;

	/* allow the kernel cmdline to have a say */
	/** 20130727    
	 * numentries가 지정되어 있지 않으면
	 **/
	if (!numentries) {
		/** 20130727    
		 * nr_kernel_pages를 가져와 megabyte 단위로 만들어 준다.
		 * (20 - PAGE_SHIFT)는 page 단위의 숫자를 megabyte 단위로 처리하기 위함
		 **/
		/* round applicable memory size up to nearest megabyte */
		numentries = nr_kernel_pages;
		numentries += (1UL << (20 - PAGE_SHIFT)) - 1;
		numentries >>= 20 - PAGE_SHIFT;
		numentries <<= 20 - PAGE_SHIFT;

		/* limit to 1 bucket per 2^scale bytes of low memory */
		/** 20130727    
		 * scale은 low memory의 2^scale bytes 당 1개의 bucket이 나오도록 결정하는
		 * 값이다.
		 *
		 * scale > PAGE_SHIFT 일 때
		 *   numentries = numentries/scale/4096;
		 **/
		if (scale > PAGE_SHIFT)
			numentries >>= (scale - PAGE_SHIFT);
		else
			numentries <<= (PAGE_SHIFT - scale);

		/* Make sure we've got at least a 0-order allocation.. */
		/** 20130727    
		 * HASH_SMALL flag가 설정되어 있다면
		 **/
		if (unlikely(flags & HASH_SMALL)) {
			/* Makes no sense without HASH_EARLY */
			WARN_ON(!(flags & HASH_EARLY));
			/** 20130727    
			 * _hash_shift shift 해도 0이면,
			 * 즉 16보다 작을 경우 최소 16으로 설정하고, 그래도 0일 경우 BUG.
			 **/
			if (!(numentries >> *_hash_shift)) {
				numentries = 1UL << *_hash_shift;
				BUG_ON(!numentries);
			}
		/** 20130727    
		 * bucket의 수와 크기가 PAGE_SIZE보다 작다면 (최소값)
		 * numentries를 PAGE_SIZE / bucketsize로 설정한다.
		 **/
		} else if (unlikely((numentries * bucketsize) < PAGE_SIZE))
			numentries = PAGE_SIZE / bucketsize;
	}
	/** 20130727    
	 * 2의 제곱수로 올림.
	 **/
	numentries = roundup_pow_of_two(numentries);

	/* limit allocation size to 1/16 total memory by default */
	/** 20130727    
	 * max값이 0일 때 nr_all_pages에 해당하는 바이트 수를 16으로 나눠 max에 저장.
	 * max = max / bucketsize;
	 **/
	if (max == 0) {
		max = ((unsigned long long)nr_all_pages << PAGE_SHIFT) >> 4;
		do_div(max, bucketsize);
	}
	/** 20130727    
	 * max의 최대값은 0x80000000ULL.
	 **/
	max = min(max, 0x80000000ULL);

	/** 20130727    
	 * numentries이 최소, 최대값 사이에 위치하도록 보장
	 **/
	if (numentries < low_limit)
		numentries = low_limit;
	if (numentries > max)
		numentries = max;

	/** 20130727    
	 * numentries <= 2의 제곱값이 되는 최소 2의 제곱값을 log2qty에 저장
	 **/
	log2qty = ilog2(numentries);

	do {
		size = bucketsize << log2qty;
		/** 20130727    
		 * flags에 HASH_EARLY가 설정되어 있으므로
		 * bucketsize * ilog2(numentries) 개수만큼 할당받아 table 생성
		 **/
		if (flags & HASH_EARLY)
			table = alloc_bootmem_nopanic(size);
		else if (hashdist)
			table = __vmalloc(size, GFP_ATOMIC, PAGE_KERNEL);
		else {
			/*
			 * If bucketsize is not a power-of-two, we may free
			 * some pages at the end of hash table which
			 * alloc_pages_exact() automatically does
			 */
			if (get_order(size) < MAX_ORDER) {
				table = alloc_pages_exact(size, GFP_ATOMIC);
				kmemleak_alloc(table, size, 1, GFP_ATOMIC);
			}
		}
	/** 20130727    
	 * table이 생성되지 않았고, size가 PAGE_SIZE 보다 큰 동안 반복.
	 * 반복할 때마다 --log2qty로 size를 감소시킨다.
	 **/
	} while (!table && size > PAGE_SIZE && --log2qty);

	/** 20130727    
	 * hash table을 결국 생성하지 못했을 경우 panic.
	 **/
	if (!table)
		panic("Failed to allocate %s hash table\n", tablename);

	/** 20130727    
	 * kernel log 출력
	 * 예) PID hash table entries: 4096 (order: 2, 16384 bytes)
	 **/
	printk(KERN_INFO "%s hash table entries: %ld (order: %d, %lu bytes)\n",
	       tablename,
	       (1UL << log2qty),
	       ilog2(size) - PAGE_SHIFT,
	       size);

	/** 20130727    
	 * _hash_shift 포인터가 존재하면 log2qty값을 저장
	 **/
	if (_hash_shift)
		*_hash_shift = log2qty;
	/** 20130727    
	 * _hash_mask 포인터가 존재하면 log2qty 값으로 마스크를 만들어 저장
	 **/
	if (_hash_mask)
		*_hash_mask = (1 << log2qty) - 1;

	return table;
}

/* Return a pointer to the bitmap storing bits affecting a block of pages */
/** 20130504
 * zone 구조체의 pageblock_flags를 반환 
 **/
static inline unsigned long *get_pageblock_bitmap(struct zone *zone,
							unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM
	return __pfn_to_section(pfn)->pageblock_flags;
#else
	return zone->pageblock_flags;
#endif /* CONFIG_SPARSEMEM */
}
/** 20130504
 * pfn이 속한 pageblock_flags의 인덱스를 반환 
 **/
static inline int pfn_to_bitidx(struct zone *zone, unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM
	pfn &= (PAGES_PER_SECTION-1);
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
#else
	pfn = pfn - zone->zone_start_pfn;
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
#endif /* CONFIG_SPARSEMEM */
}

/**
 * get_pageblock_flags_group - Return the requested group of flags for the pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @start_bitidx: The first bit of interest to retrieve
 * @end_bitidx: The last bit of interest
 * returns pageblock_bits flags
 */
/** 20130831    
 * page가 속한 pageblock 에 대한 bitmap 속성을 검사해 flags 값을 리턴한다.
 **/
unsigned long get_pageblock_flags_group(struct page *page,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long flags = 0;
	unsigned long value = 1;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	bitmap = get_pageblock_bitmap(zone, pfn);
	/** 20130831    
	 * pfn에 해당하는 bitmap에서의 index.
	 **/
	bitidx = pfn_to_bitidx(zone, pfn);

	/** 20130831    
	 * migrate type을 표시하기 위한 3개의 비트를 각각 검사한다.
	 * 해당 bit가 set 되어 있다면 flags 에 value를 or-ing (누적) 한다.
	 * isolate type 까지 가능하다.
	 **/
	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		if (test_bit(bitidx + start_bitidx, bitmap))
			flags |= value;

	return flags;
}

/**
 * set_pageblock_flags_group - Set the requested group of flags for a pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @start_bitidx: The first bit of interest
 * @end_bitidx: The last bit of interest
 * @flags: The flags to set
 */
/** 20130504
 * pageblock bitmap 중 pfn에 해당하는 bits들 중, flags에 해당하는 비트를 설정한다.
 **/
void set_pageblock_flags_group(struct page *page, unsigned long flags,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long value = 1;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	/** 20130504
	 * zone의 pageblock_flags를 pageblock bitmap을 가져옴.
	 **/
	bitmap = get_pageblock_bitmap(zone, pfn);
	/** 20130504
	 * pfn이 속한 pageblock_flags의 인덱스를 bitidx를 저장
	 **/
	bitidx = pfn_to_bitidx(zone, pfn);
	VM_BUG_ON(pfn < zone->zone_start_pfn);
	VM_BUG_ON(pfn >= zone->zone_start_pfn + zone->spanned_pages);
	/** 20130504
	 * 예를 들어  PB_migrate ~ PB_migrate_end 사이의 3개의 비트 중, 
	 * MIGRATE_MOVABLE에 해당하는 bit만 1로 설정해 type을 지정한다.
	 *
	 * flag에 해당하는 영역을 1로 세팅을 하고 나머지는 0으로 clear한다.
	 * /linux/include/linux/mmzone.h 참조
	 * MIGRATE_UNMOVABLE = 0,
	 * MIGRATE_RECLAIMABLE = 1,
	 * MIGRATE_MOVABLE = 2,
	 * MIGRATE_CMA = 4
	 *
	 * value에 값에 따라
	 * MIGRATE_UNMOVABLE을 제외한 다른 MIGRATE들은
	 * flags의 값에 따라 set_bit이 된다.
	 * 모든 비트가 clear되었을 경우 MIGRATE_UNMOVABLE로 판단하는듯??? 
	 *
	 * 20130511 
	 * init_page_count에서 page 구조체의 _count를 1로 set했기 때문에 
	 * not-atomic 버전의 set_bit, clear_bit 사용한듯 ???
	 * 같은 페이지 구조체 안에 있는 필드내의 값에 대한 동시성을 지키기 위해
	 * not-atomic으로 충분하다는 판단하에 사용됨???
	 **/
	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		if (flags & value)
			__set_bit(bitidx + start_bitidx, bitmap);
		else
			__clear_bit(bitidx + start_bitidx, bitmap);
}

/*
 * This function checks whether pageblock includes unmovable pages or not.
 * If @count is not zero, it is okay to include less @count unmovable pages
 *
 * PageLRU check wihtout isolation or lru_lock could race so that
 * MIGRATE_MOVABLE block might include unmovable pages. It means you can't
 * expect this function should be exact.
 */
bool has_unmovable_pages(struct zone *zone, struct page *page, int count)
{
	unsigned long pfn, iter, found;
	int mt;

	/*
	 * For avoiding noise data, lru_add_drain_all() should be called
	 * If ZONE_MOVABLE, the zone never contains unmovable pages
	 */
	if (zone_idx(zone) == ZONE_MOVABLE)
		return false;
	mt = get_pageblock_migratetype(page);
	if (mt == MIGRATE_MOVABLE || is_migrate_cma(mt))
		return false;

	pfn = page_to_pfn(page);
	for (found = 0, iter = 0; iter < pageblock_nr_pages; iter++) {
		unsigned long check = pfn + iter;

		if (!pfn_valid_within(check))
			continue;

		page = pfn_to_page(check);
		/*
		 * We can't use page_count without pin a page
		 * because another CPU can free compound page.
		 * This check already skips compound tails of THP
		 * because their page->_count is zero at all time.
		 */
		if (!atomic_read(&page->_count)) {
			if (PageBuddy(page))
				iter += (1 << page_order(page)) - 1;
			continue;
		}

		if (!PageLRU(page))
			found++;
		/*
		 * If there are RECLAIMABLE pages, we need to check it.
		 * But now, memory offline itself doesn't call shrink_slab()
		 * and it still to be fixed.
		 */
		/*
		 * If the page is not RAM, page_count()should be 0.
		 * we don't need more check. This is an _used_ not-movable page.
		 *
		 * The problematic thing here is PG_reserved pages. PG_reserved
		 * is set to both of a memory hole page and a _used_ kernel
		 * page at boot.
		 */
		if (found > count)
			return true;
	}
	return false;
}

bool is_pageblock_removable_nolock(struct page *page)
{
	struct zone *zone;
	unsigned long pfn;

	/*
	 * We have to be careful here because we are iterating over memory
	 * sections which are not zone aware so we might end up outside of
	 * the zone but still within the section.
	 * We have to take care about the node as well. If the node is offline
	 * its NODE_DATA will be NULL - see page_zone.
	 */
	if (!node_online(page_to_nid(page)))
		return false;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	if (zone->zone_start_pfn > pfn ||
			zone->zone_start_pfn + zone->spanned_pages <= pfn)
		return false;

	return !has_unmovable_pages(zone, page, 0);
}

#ifdef CONFIG_CMA

static unsigned long pfn_max_align_down(unsigned long pfn)
{
	return pfn & ~(max_t(unsigned long, MAX_ORDER_NR_PAGES,
			     pageblock_nr_pages) - 1);
}

static unsigned long pfn_max_align_up(unsigned long pfn)
{
	return ALIGN(pfn, max_t(unsigned long, MAX_ORDER_NR_PAGES,
				pageblock_nr_pages));
}

static struct page *
__alloc_contig_migrate_alloc(struct page *page, unsigned long private,
			     int **resultp)
{
	gfp_t gfp_mask = GFP_USER | __GFP_MOVABLE;

	if (PageHighMem(page))
		gfp_mask |= __GFP_HIGHMEM;

	return alloc_page(gfp_mask);
}

/* [start, end) must belong to a single zone. */
static int __alloc_contig_migrate_range(unsigned long start, unsigned long end)
{
	/* This function is based on compact_zone() from compaction.c. */

	unsigned long pfn = start;
	unsigned int tries = 0;
	int ret = 0;

	struct compact_control cc = {
		.nr_migratepages = 0,
		.order = -1,
		.zone = page_zone(pfn_to_page(start)),
		.sync = true,
	};
	INIT_LIST_HEAD(&cc.migratepages);

	migrate_prep_local();

	while (pfn < end || !list_empty(&cc.migratepages)) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		if (list_empty(&cc.migratepages)) {
			cc.nr_migratepages = 0;
			pfn = isolate_migratepages_range(cc.zone, &cc,
							 pfn, end);
			if (!pfn) {
				ret = -EINTR;
				break;
			}
			tries = 0;
		} else if (++tries == 5) {
			ret = ret < 0 ? ret : -EBUSY;
			break;
		}

		ret = migrate_pages(&cc.migratepages,
				    __alloc_contig_migrate_alloc,
				    0, false, MIGRATE_SYNC);
	}

	putback_lru_pages(&cc.migratepages);
	return ret > 0 ? 0 : ret;
}

/*
 * Update zone's cma pages counter used for watermark level calculation.
 */
static inline void __update_cma_watermarks(struct zone *zone, int count)
{
	unsigned long flags;
	spin_lock_irqsave(&zone->lock, flags);
	zone->min_cma_pages += count;
	spin_unlock_irqrestore(&zone->lock, flags);
	setup_per_zone_wmarks();
}

/*
 * Trigger memory pressure bump to reclaim some pages in order to be able to
 * allocate 'count' pages in single page units. Does similar work as
 *__alloc_pages_slowpath() function.
 */
static int __reclaim_pages(struct zone *zone, gfp_t gfp_mask, int count)
{
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	struct zonelist *zonelist = node_zonelist(0, gfp_mask);
	int did_some_progress = 0;
	int order = 1;

	/*
	 * Increase level of watermarks to force kswapd do his job
	 * to stabilise at new watermark level.
	 */
	__update_cma_watermarks(zone, count);

	/* Obey watermarks as if the page was being allocated */
	while (!zone_watermark_ok(zone, 0, low_wmark_pages(zone), 0, 0)) {
		wake_all_kswapd(order, zonelist, high_zoneidx, zone_idx(zone));

		did_some_progress = __perform_reclaim(gfp_mask, order, zonelist,
						      NULL);
		if (!did_some_progress) {
			/* Exhausted what can be done so it's blamo time */
			out_of_memory(zonelist, gfp_mask, order, NULL, false);
		}
	}

	/* Restore original watermark levels. */
	__update_cma_watermarks(zone, -count);

	return count;
}

/**
 * alloc_contig_range() -- tries to allocate given range of pages
 * @start:	start PFN to allocate
 * @end:	one-past-the-last PFN to allocate
 * @migratetype:	migratetype of the underlaying pageblocks (either
 *			#MIGRATE_MOVABLE or #MIGRATE_CMA).  All pageblocks
 *			in range must have the same migratetype and it must
 *			be either of the two.
 *
 * The PFN range does not have to be pageblock or MAX_ORDER_NR_PAGES
 * aligned, however it's the caller's responsibility to guarantee that
 * we are the only thread that changes migrate type of pageblocks the
 * pages fall in.
 *
 * The PFN range must belong to a single zone.
 *
 * Returns zero on success or negative error code.  On success all
 * pages which PFN is in [start, end) are allocated for the caller and
 * need to be freed with free_contig_range().
 */
int alloc_contig_range(unsigned long start, unsigned long end,
		       unsigned migratetype)
{
	struct zone *zone = page_zone(pfn_to_page(start));
	unsigned long outer_start, outer_end;
	int ret = 0, order;

	/*
	 * What we do here is we mark all pageblocks in range as
	 * MIGRATE_ISOLATE.  Because pageblock and max order pages may
	 * have different sizes, and due to the way page allocator
	 * work, we align the range to biggest of the two pages so
	 * that page allocator won't try to merge buddies from
	 * different pageblocks and change MIGRATE_ISOLATE to some
	 * other migration type.
	 *
	 * Once the pageblocks are marked as MIGRATE_ISOLATE, we
	 * migrate the pages from an unaligned range (ie. pages that
	 * we are interested in).  This will put all the pages in
	 * range back to page allocator as MIGRATE_ISOLATE.
	 *
	 * When this is done, we take the pages in range from page
	 * allocator removing them from the buddy system.  This way
	 * page allocator will never consider using them.
	 *
	 * This lets us mark the pageblocks back as
	 * MIGRATE_CMA/MIGRATE_MOVABLE so that free pages in the
	 * aligned range but not in the unaligned, original range are
	 * put back to page allocator so that buddy can use them.
	 */

	ret = start_isolate_page_range(pfn_max_align_down(start),
				       pfn_max_align_up(end), migratetype);
	if (ret)
		goto done;

	ret = __alloc_contig_migrate_range(start, end);
	if (ret)
		goto done;

	/*
	 * Pages from [start, end) are within a MAX_ORDER_NR_PAGES
	 * aligned blocks that are marked as MIGRATE_ISOLATE.  What's
	 * more, all pages in [start, end) are free in page allocator.
	 * What we are going to do is to allocate all pages from
	 * [start, end) (that is remove them from page allocator).
	 *
	 * The only problem is that pages at the beginning and at the
	 * end of interesting range may be not aligned with pages that
	 * page allocator holds, ie. they can be part of higher order
	 * pages.  Because of this, we reserve the bigger range and
	 * once this is done free the pages we are not interested in.
	 *
	 * We don't have to hold zone->lock here because the pages are
	 * isolated thus they won't get removed from buddy.
	 */

	lru_add_drain_all();
	drain_all_pages();

	order = 0;
	outer_start = start;
	while (!PageBuddy(pfn_to_page(outer_start))) {
		if (++order >= MAX_ORDER) {
			ret = -EBUSY;
			goto done;
		}
		outer_start &= ~0UL << order;
	}

	/* Make sure the range is really isolated. */
	if (test_pages_isolated(outer_start, end)) {
		pr_warn("alloc_contig_range test_pages_isolated(%lx, %lx) failed\n",
		       outer_start, end);
		ret = -EBUSY;
		goto done;
	}

	/*
	 * Reclaim enough pages to make sure that contiguous allocation
	 * will not starve the system.
	 */
	__reclaim_pages(zone, GFP_HIGHUSER_MOVABLE, end-start);

	/* Grab isolated pages from freelists. */
	outer_end = isolate_freepages_range(outer_start, end);
	if (!outer_end) {
		ret = -EBUSY;
		goto done;
	}

	/* Free head and tail (if any) */
	if (start != outer_start)
		free_contig_range(outer_start, start - outer_start);
	if (end != outer_end)
		free_contig_range(end, outer_end - end);

done:
	undo_isolate_page_range(pfn_max_align_down(start),
				pfn_max_align_up(end), migratetype);
	return ret;
}

void free_contig_range(unsigned long pfn, unsigned nr_pages)
{
	for (; nr_pages--; ++pfn)
		__free_page(pfn_to_page(pfn));
}
#endif

#ifdef CONFIG_MEMORY_HOTPLUG
static int __meminit __zone_pcp_update(void *data)
{
	struct zone *zone = data;
	int cpu;
	unsigned long batch = zone_batchsize(zone), flags;

	for_each_possible_cpu(cpu) {
		struct per_cpu_pageset *pset;
		struct per_cpu_pages *pcp;

		pset = per_cpu_ptr(zone->pageset, cpu);
		pcp = &pset->pcp;

		local_irq_save(flags);
		if (pcp->count > 0)
			free_pcppages_bulk(zone, pcp->count, pcp);
		setup_pageset(pset, batch);
		local_irq_restore(flags);
	}
	return 0;
}

void __meminit zone_pcp_update(struct zone *zone)
{
	stop_machine(__zone_pcp_update, zone, NULL);
}
#endif

#ifdef CONFIG_MEMORY_HOTREMOVE
void zone_pcp_reset(struct zone *zone)
{
	unsigned long flags;

	/* avoid races with drain_pages()  */
	local_irq_save(flags);
	if (zone->pageset != &boot_pageset) {
		free_percpu(zone->pageset);
		zone->pageset = &boot_pageset;
	}
	local_irq_restore(flags);
}

/*
 * All pages in the range must be isolated before calling this.
 */
void
__offline_isolated_pages(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *page;
	struct zone *zone;
	int order, i;
	unsigned long pfn;
	unsigned long flags;
	/* find the first valid pfn */
	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		if (pfn_valid(pfn))
			break;
	if (pfn == end_pfn)
		return;
	zone = page_zone(pfn_to_page(pfn));
	spin_lock_irqsave(&zone->lock, flags);
	pfn = start_pfn;
	while (pfn < end_pfn) {
		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		page = pfn_to_page(pfn);
		BUG_ON(page_count(page));
		BUG_ON(!PageBuddy(page));
		order = page_order(page);
#ifdef CONFIG_DEBUG_VM
		printk(KERN_INFO "remove from free list %lx %d %lx\n",
		       pfn, 1 << order, end_pfn);
#endif
		list_del(&page->lru);
		rmv_page_order(page);
		zone->free_area[order].nr_free--;
		__mod_zone_page_state(zone, NR_FREE_PAGES,
				      - (1UL << order));
		for (i = 0; i < (1 << order); i++)
			SetPageReserved((page+i));
		pfn += (1 << order);
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif

#ifdef CONFIG_MEMORY_FAILURE
bool is_free_buddy_page(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long pfn = page_to_pfn(page);
	unsigned long flags;
	int order;

	spin_lock_irqsave(&zone->lock, flags);
	for (order = 0; order < MAX_ORDER; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(page_head) && page_order(page_head) >= order)
			break;
	}
	spin_unlock_irqrestore(&zone->lock, flags);

	return order < MAX_ORDER;
}
#endif

static const struct trace_print_flags pageflag_names[] = {
	{1UL << PG_locked,		"locked"	},
	{1UL << PG_error,		"error"		},
	{1UL << PG_referenced,		"referenced"	},
	{1UL << PG_uptodate,		"uptodate"	},
	{1UL << PG_dirty,		"dirty"		},
	{1UL << PG_lru,			"lru"		},
	{1UL << PG_active,		"active"	},
	{1UL << PG_slab,		"slab"		},
	{1UL << PG_owner_priv_1,	"owner_priv_1"	},
	{1UL << PG_arch_1,		"arch_1"	},
	{1UL << PG_reserved,		"reserved"	},
	{1UL << PG_private,		"private"	},
	{1UL << PG_private_2,		"private_2"	},
	{1UL << PG_writeback,		"writeback"	},
#ifdef CONFIG_PAGEFLAGS_EXTENDED
	{1UL << PG_head,		"head"		},
	{1UL << PG_tail,		"tail"		},
#else
	{1UL << PG_compound,		"compound"	},
#endif
	{1UL << PG_swapcache,		"swapcache"	},
	{1UL << PG_mappedtodisk,	"mappedtodisk"	},
	{1UL << PG_reclaim,		"reclaim"	},
	{1UL << PG_swapbacked,		"swapbacked"	},
	{1UL << PG_unevictable,		"unevictable"	},
#ifdef CONFIG_MMU
	{1UL << PG_mlocked,		"mlocked"	},
#endif
#ifdef CONFIG_ARCH_USES_PG_UNCACHED
	{1UL << PG_uncached,		"uncached"	},
#endif
#ifdef CONFIG_MEMORY_FAILURE
	{1UL << PG_hwpoison,		"hwpoison"	},
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	{1UL << PG_compound_lock,	"compound_lock"	},
#endif
};

static void dump_page_flags(unsigned long flags)
{
	const char *delim = "";
	unsigned long mask;
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(pageflag_names) != __NR_PAGEFLAGS);

	printk(KERN_ALERT "page flags: %#lx(", flags);

	/* remove zone id */
	flags &= (1UL << NR_PAGEFLAGS) - 1;

	for (i = 0; i < ARRAY_SIZE(pageflag_names) && flags; i++) {

		mask = pageflag_names[i].mask;
		if ((flags & mask) != mask)
			continue;

		flags &= ~mask;
		printk("%s%s", delim, pageflag_names[i].name);
		delim = "|";
	}

	/* check for left over flags */
	if (flags)
		printk("%s%#lx", delim, flags);

	printk(")\n");
}

void dump_page(struct page *page)
{
	printk(KERN_ALERT
	       "page:%p count:%d mapcount:%d mapping:%p index:%#lx\n",
		page, atomic_read(&page->_count), page_mapcount(page),
		page->mapping, page->index);
	dump_page_flags(page->flags);
	mem_cgroup_print_bad_page(page);
}
