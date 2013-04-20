/*
 *  bootmem - A boot-time physical memory allocator and configurator
 *
 *  Copyright (C) 1999 Ingo Molnar
 *                1999 Kanoj Sarcar, SGI
 *                2008 Johannes Weiner
 *
 * Access to this subsystem has to be serialized externally (which is true
 * for the boot process anyway).
 */
#include <linux/init.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/bootmem.h>
#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/range.h>
#include <linux/memblock.h>

#include <asm/bug.h>
#include <asm/io.h>
#include <asm/processor.h>

#include "internal.h"

/** 20130330    
 * vexpress에서는 not defined.
 * contig_page_data의 .bdata는 bootmem_node_data[0]의 시작 주소
 **/
#ifndef CONFIG_NEED_MULTIPLE_NODES
struct pglist_data __refdata contig_page_data = {
	.bdata = &bootmem_node_data[0]
};
EXPORT_SYMBOL(contig_page_data);
#endif

unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;

bootmem_data_t bootmem_node_data[MAX_NUMNODES] __initdata;

/** 20130330    
 * 초기화한 bdata_list를 static 전역 변수로 선언
 **/
static struct list_head bdata_list __initdata = LIST_HEAD_INIT(bdata_list);

static int bootmem_debug;

/** 20130406    
 * "bootmem_debug"가 command line으로 설정되었을 경우 호출.
 **/
static int __init bootmem_debug_setup(char *buf)
{
	bootmem_debug = 1;
	return 0;
}
early_param("bootmem_debug", bootmem_debug_setup);

#define bdebug(fmt, args...) ({				\
	if (unlikely(bootmem_debug))			\
		printk(KERN_INFO			\
			"bootmem::%s " fmt,		\
			__func__, ## args);		\
})

/** 20130330    
 * pages를 비트맵으로 표현하기 위한 바이트 수를 구한 뒤,
 * sizeof (long) 단위로 round up 해서 리턴.
 **/
static unsigned long __init bootmap_bytes(unsigned long pages)
{
	/** 20130330    
	 * pages를 표현하기 위해 몇 바이트가 필요한지 구함.
	 *   bitmap 표현방식
	 **/
	unsigned long bytes = DIV_ROUND_UP(pages, 8);

	/** 20130330    
	 * (((x) + (mask)) & ~(mask))
	 * mask: (typeof(x))(a) - 1
	 *       (unsigned long)(sizeof(long)) - 1   ==> 3
	 **/
	return ALIGN(bytes, sizeof(long));
}

/**
 * bootmem_bootmap_pages - calculate bitmap size in pages
 * @pages: number of pages the bitmap has to represent
 */
/** 20130330    
 * pages를 표현하기 위해 필요한 bytes의 수를 구한 뒤 정렬하고,
 * 이를 표현하는데 필요한 page의 수를 리턴.
 **/
unsigned long __init bootmem_bootmap_pages(unsigned long pages)
{
	unsigned long bytes = bootmap_bytes(pages);

	return PAGE_ALIGN(bytes) >> PAGE_SHIFT;
}

/*
 * link bdata in order
 */
/** 20130330    
 * bdata_list에 bdata를 삽입 (오름차순)
 **/
static void __init link_bootmem(bootmem_data_t *bdata)
{
	bootmem_data_t *ent;

	/** 20130330    
	#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

	#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

	#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, typeof(*pos), member);	\
	     &pos->member != (head); 	\
	     pos = list_entry(pos->member.next, typeof(*pos), member))

		__mptr ==> (bdata_list)->next, 자기 자신
	     &pos->member : &((__mptr - offsetof(ent, list))->list), 결국 자기 자신
		 head         : &bdata_list

		 초기 수행시 첫번째 test에서 fail로 빠져나간다.
	 **/
	list_for_each_entry(ent, &bdata_list, list) {
		/** 20130330    
		 * 추가할 bdata의 min_pfn(start)이 더 작다면 
		 * ent 이전에 삽입시킨다.
		 **/
		if (bdata->node_min_pfn < ent->node_min_pfn) {
			list_add_tail(&bdata->list, &ent->list);
			return;
		}
	}

	list_add_tail(&bdata->list, &bdata_list);
}

/*
 * Called once to set up the allocator itself.
 */
/** 20130330    
 * bootmem data를 bdata_list에 추가하고, bdata->node_bootmem_map를 초기화
 **/
static unsigned long __init init_bootmem_core(bootmem_data_t *bdata,
	unsigned long mapstart, unsigned long start, unsigned long end)
{
	unsigned long mapsize;

	/** 20130330    
	 * vexpress에서 null function
	 **/
	mminit_validate_memmodel_limits(&start, &end);
	/** 20130330    
	 * bdata는 include/linux/bootmem.h에 선언된 struct bootmem_data
	 * node_bootmem_map : 할당받은 mapstart 영역에 대한 VA를 저장
	 * node_min_pfn     : 물리 메모리의 시작 주소에 대한 pfn
	 * node_low_pfn     : 물리 메모리의 끝 주소에 대한 pfn
	 *
	 * bdata를 bdata_list에 추가
	 **/
	bdata->node_bootmem_map = phys_to_virt(PFN_PHYS(mapstart));
	bdata->node_min_pfn = start;
	bdata->node_low_pfn = end;
	link_bootmem(bdata);

	/*
	 * Initially all pages are reserved - setup_arch() has to
	 * register free RAM areas explicitly.
	 */
	/** 20130330    
	 * start ~ end까지 표현 가능한 비트맵의 메모리 크기를 저장
	 **/
	mapsize = bootmap_bytes(end - start);
	/** 20130330    
	 * bdata->node_bootmem_map 영역을 0xff로 초기화
	 * (사용 가능하다는 의미로 각 pfn에 해당하는 비트를 1로 설정하는듯???)
	 **/
	memset(bdata->node_bootmem_map, 0xff, mapsize);

	bdebug("nid=%td start=%lx map=%lx end=%lx mapsize=%lx\n",
		bdata - bootmem_node_data, start, mapstart, end, mapsize);

	return mapsize;
}

/**
 * init_bootmem_node - register a node as boot memory
 * @pgdat: node to register
 * @freepfn: pfn where the bitmap for this node is to be placed
 * @startpfn: first pfn on the node
 * @endpfn: first pfn after the node
 *
 * Returns the number of bytes needed to hold the bitmap for this node.
 */
/** 20130330    
 * 특정 노드에 대한 bootmem을 초기화
 **/
unsigned long __init init_bootmem_node(pg_data_t *pgdat, unsigned long freepfn,
				unsigned long startpfn, unsigned long endpfn)
{
	return init_bootmem_core(pgdat->bdata, freepfn, startpfn, endpfn);
}

/**
 * init_bootmem - register boot memory
 * @start: pfn where the bitmap is to be placed
 * @pages: number of available physical pages
 *
 * Returns the number of bytes needed to hold the bitmap.
 */
unsigned long __init init_bootmem(unsigned long start, unsigned long pages)
{
	max_low_pfn = pages;
	min_low_pfn = start;
	return init_bootmem_core(NODE_DATA(0)->bdata, start, 0, pages);
}

/*
 * free_bootmem_late - free bootmem pages directly to page allocator
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * This is only useful when the bootmem allocator has already been torn
 * down, but we are still initializing the system.  Pages are given directly
 * to the page allocator, no bootmem metadata is updated because it is gone.
 */
void __init free_bootmem_late(unsigned long addr, unsigned long size)
{
	unsigned long cursor, end;

	kmemleak_free_part(__va(addr), size);

	cursor = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	for (; cursor < end; cursor++) {
		__free_pages_bootmem(pfn_to_page(cursor), 0);
		totalram_pages++;
	}
}

static unsigned long __init free_all_bootmem_core(bootmem_data_t *bdata)
{
	struct page *page;
	unsigned long start, end, pages, count = 0;

	if (!bdata->node_bootmem_map)
		return 0;

	start = bdata->node_min_pfn;
	end = bdata->node_low_pfn;

	bdebug("nid=%td start=%lx end=%lx\n",
		bdata - bootmem_node_data, start, end);

	while (start < end) {
		unsigned long *map, idx, vec;

		map = bdata->node_bootmem_map;
		idx = start - bdata->node_min_pfn;
		vec = ~map[idx / BITS_PER_LONG];
		/*
		 * If we have a properly aligned and fully unreserved
		 * BITS_PER_LONG block of pages in front of us, free
		 * it in one go.
		 */
		if (IS_ALIGNED(start, BITS_PER_LONG) && vec == ~0UL) {
			int order = ilog2(BITS_PER_LONG);

			__free_pages_bootmem(pfn_to_page(start), order);
			count += BITS_PER_LONG;
			start += BITS_PER_LONG;
		} else {
			unsigned long off = 0;

			vec >>= start & (BITS_PER_LONG - 1);
			while (vec) {
				if (vec & 1) {
					page = pfn_to_page(start + off);
					__free_pages_bootmem(page, 0);
					count++;
				}
				vec >>= 1;
				off++;
			}
			start = ALIGN(start + 1, BITS_PER_LONG);
		}
	}

	page = virt_to_page(bdata->node_bootmem_map);
	pages = bdata->node_low_pfn - bdata->node_min_pfn;
	pages = bootmem_bootmap_pages(pages);
	count += pages;
	while (pages--)
		__free_pages_bootmem(page++, 0);

	bdebug("nid=%td released=%lx\n", bdata - bootmem_node_data, count);

	return count;
}

/**
 * free_all_bootmem_node - release a node's free pages to the buddy allocator
 * @pgdat: node to be released
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem_node(pg_data_t *pgdat)
{
	register_page_bootmem_info_node(pgdat);
	return free_all_bootmem_core(pgdat->bdata);
}

/**
 * free_all_bootmem - release free pages to the buddy allocator
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem(void)
{
	unsigned long total_pages = 0;
	bootmem_data_t *bdata;

	list_for_each_entry(bdata, &bdata_list, list)
		total_pages += free_all_bootmem_core(bdata);

	return total_pages;
}

/** 20130406    
 * sidx~eidx에 해당하는 비트를 bootmem bitmap에서 클리어시킴
 **/
static void __init __free(bootmem_data_t *bdata,
			unsigned long sidx, unsigned long eidx)
{
	unsigned long idx;

	bdebug("nid=%td start=%lx end=%lx\n", bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn);

	/** 20130406    
	 * 초기화 하지 않은 전역 구조체의 멤버이므로 초기값은 0
	 * hint_idx의 역할은???
	 **/
	if (bdata->hint_idx > sidx)
		bdata->hint_idx = sidx;

	for (idx = sidx; idx < eidx; idx++)
		/** 20130406    
		 * sidx ~ eidx 영역에 해당하는 bit를 clear.
		 * 이전 비트값이 0일 때 BUG() 호출
		 **/
		if (!test_and_clear_bit(idx, bdata->node_bootmem_map))
			BUG();
}

/** 20130406    
 * sidx ~ eidx 사이의 pfn에 대해 1로 설정하는 함수
 **/
static int __init __reserve(bootmem_data_t *bdata, unsigned long sidx,
			unsigned long eidx, int flags)
{
	unsigned long idx;
	int exclusive = flags & BOOTMEM_EXCLUSIVE;

	bdebug("nid=%td start=%lx end=%lx flags=%x\n",
		bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn,
		flags);

	for (idx = sidx; idx < eidx; idx++)
		/** 20130406    
		 * idx에 해당하는 bit를 1로 설정하고, 이전 상태를 리턴
		 **/
		if (test_and_set_bit(idx, bdata->node_bootmem_map)) {
			/** 20130406
			 * 이전 상태가 1이고, exclusive가 flags에 의해 설정되었다면
			 * __free 호출
			 **/
			if (exclusive) {
				__free(bdata, sidx, idx);
				return -EBUSY;
			}
			bdebug("silent double reserve of PFN %lx\n",
				idx + bdata->node_min_pfn);
		}
	return 0;
}

/** 20130406    
 * reserve에 따라 start와 end 사이에 해당하는 영역을 bdata bitmap에서 변경함
 **/
static int __init mark_bootmem_node(bootmem_data_t *bdata,
				unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long sidx, eidx;

	/** 20130406    
	 * bootmem debug 용 출력 함수
	 **/
	bdebug("nid=%td start=%lx end=%lx reserve=%d flags=%x\n",
		bdata - bootmem_node_data, start, end, reserve, flags);

	/** 20130406    
	 * 매개변수에 대한 sanity check.
	 **/
	BUG_ON(start < bdata->node_min_pfn);
	BUG_ON(end > bdata->node_low_pfn);

	/** 20130406    
	 * 물리 메모리의 시작 주소에 대한 offset 개념으로 sidx와 eidx를 구함
	 **/
	sidx = start - bdata->node_min_pfn;
	eidx = end - bdata->node_min_pfn;

	/** 20130406    
	 * reserve 값에 따라
	 * true면 __reserve (set)
	 * false면 __free   (clear)
	 **/
	if (reserve)
		return __reserve(bdata, sidx, eidx, flags);
	else
		__free(bdata, sidx, eidx);
	return 0;
}

/** 20130406    
 * start ~ end 영역에 대해 reserve 값에 따라 bootmem을 설정하는 함수
 **/
static int __init mark_bootmem(unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long pos;
	bootmem_data_t *bdata;

	pos = start;
	/** 20130406    
	 * bdata_list를 순회. UMA의 경우 하나의 entry만 존재한다.
	 **/
	list_for_each_entry(bdata, &bdata_list, list) {
		int err;
		unsigned long max;

		/** 20130406    
		 * bdata에 저장된 값과 비교해 사용 가능 영역인지 검사
		 **/
		if (pos < bdata->node_min_pfn ||
		    pos >= bdata->node_low_pfn) {
			BUG_ON(pos != start);
			continue;
		}

		/** 20130406    
		 * bootmem data의 node_low_pfn와 end 중 작은 값을 취함
		 * NUMA일 경우, 각 노드의 끝(node_low_pfn)과 전체 메모리 영역의 끝(end) 중에 작은 값
		 **/
		max = min(bdata->node_low_pfn, end);

		/** 20130406    
		 * free_bootmem에서 호출될 때 reserve와 flags는 0, 0
		 * reserve_bootmem에서 호출될 때 reserve와 flags는 1, 0
		 **/
		err = mark_bootmem_node(bdata, pos, max, reserve, flags);
		if (reserve && err) {
			/** 20130406    
			 * reserve가 실패했을 경우 start ~ pos가지 수행 후
			 * err 리턴
			 **/
			mark_bootmem(start, pos, 0, 0);
			return err;
		}

		/** 20130406    
		 * 끝까지 수행했을 경우 정상 리턴
		 **/
		if (max == end)
			return 0;
		pos = bdata->node_low_pfn;
	}
	/** 20130406    
	 * 비정상적으로 list를 모두 순회했을 경우 BUG()
	 **/
	BUG();
}

/**
 * free_bootmem_node - mark a page range as usable
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must reside completely on the specified node.
 */
void __init free_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
			      unsigned long size)
{
	unsigned long start, end;

	kmemleak_free_part(__va(physaddr), size);

	start = PFN_UP(physaddr);
	end = PFN_DOWN(physaddr + size);

	mark_bootmem_node(pgdat->bdata, start, end, 0, 0);
}

/**
 * free_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must be contiguous but may span node boundaries.
 */
/** 20130406    
 * addr와 size에 해당하는 pfn을 bootmem bitmap 영역에서 free (clear) 시킴
 **/
void __init free_bootmem(unsigned long addr, unsigned long size)
{
	unsigned long start, end;

	/** 20130406    
	 * vexpress에서는 NULL 함수
	 **/
	kmemleak_free_part(__va(addr), size);

	/** 20130406    
	 * 시작 주소에 대해 round up 한 PFN, 끝 주소에 대해 round down한 PFN을 저장
	 **/
	start = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	/** 20130406    
	 * free 함수이므로 reserve에 0을 전달
	 **/
	mark_bootmem(start, end, 0, 0);
}

/**
 * reserve_bootmem_node - mark a page range as reserved
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 * @flags: reservation flags (see linux/bootmem.h)
 *
 * Partial pages will be reserved.
 *
 * The range must reside completely on the specified node.
 */
int __init reserve_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
				 unsigned long size, int flags)
{
	unsigned long start, end;

	start = PFN_DOWN(physaddr);
	end = PFN_UP(physaddr + size);

	return mark_bootmem_node(pgdat->bdata, start, end, 1, flags);
}

/**
 * reserve_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 * @flags: reservation flags (see linux/bootmem.h)
 *
 * Partial pages will be reserved.
 *
 * The range must be contiguous but may span node boundaries.
 */
/** 20130406    
 * addr와 size에 해당하는 pfn을 bootmem bitmap 영역에서 reserve (set) 시킴
 **/
int __init reserve_bootmem(unsigned long addr, unsigned long size,
			    int flags)
{
	unsigned long start, end;

	start = PFN_DOWN(addr);
	end = PFN_UP(addr + size);

	/** 20130406    
	 * reserve 를 1로 전달
	 **/
	return mark_bootmem(start, end, 1, flags);
}

int __weak __init reserve_bootmem_generic(unsigned long phys, unsigned long len,
				   int flags)
{
	return reserve_bootmem(phys, len, flags);
}

/** 20130420    
 * idx를 step 단위로 ALIGN 시켜 리턴
 **/
static unsigned long __init align_idx(struct bootmem_data *bdata,
				      unsigned long idx, unsigned long step)
{
	unsigned long base = bdata->node_min_pfn;

	/*
	 * Align the index with respect to the node start so that the
	 * combination of both satisfies the requested alignment.
	 */

/** 20130420    
 * base+idx를 step 단위로 ALIGN 시켜 base와의 offset을 리턴.
 **/
	return ALIGN(base + idx, step) - base;
}

static unsigned long __init align_off(struct bootmem_data *bdata,
				      unsigned long off, unsigned long align)
{
	unsigned long base = PFN_PHYS(bdata->node_min_pfn);

	/* Same as align_idx for byte offsets */

	return ALIGN(base + off, align) - base;
}

/** 20130420    
 * page frame을 관리하는 struct page 들을 저장하는 영역을
 * binary map 부분에서 할당
 **/
static void * __init alloc_bootmem_bdata(struct bootmem_data *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	unsigned long fallback = 0;
	unsigned long min, max, start, sidx, midx, step;

	bdebug("nid=%td size=%lx [%lu pages] align=%lx goal=%lx limit=%lx\n",
		bdata - bootmem_node_data, size, PAGE_ALIGN(size) >> PAGE_SHIFT,
		align, goal, limit);

	/** 20130420    
	 * argument sanity check.
	 **/
	BUG_ON(!size);
	BUG_ON(align & (align - 1));
	BUG_ON(limit && goal + size > limit);

	if (!bdata->node_bootmem_map)
		return NULL;

	min = bdata->node_min_pfn;
	max = bdata->node_low_pfn;

	/** 20130420    
	 * goal과 limit을 pfn 으로 변환
	 **/
	goal >>= PAGE_SHIFT;
	limit >>= PAGE_SHIFT;

	/** 20130420    
	 * 변수값 재조정
	 **/
	if (limit && max > limit)
		max = limit;
	if (max <= min)
		return NULL;

	/** 20130420    
	 * 현재 align이 L1_CACHE_SIZE로 넘어왔으므로 >> PAGE_SHIFT를 하면 0이 됨.
	 * step의 최소값은 1. 
	 **/
	step = max(align >> PAGE_SHIFT, 1UL);

	/** 20130420    
	 * min < goal < max 일 경우 goal을 step 단위로 정렬해 start에 저장.
	 * 그렇지 않을 경우 min을 step  단위로 정렬해 start에 저장.
	 **/
	if (goal && min < goal && goal < max)
		start = ALIGN(goal, step);
	else
		start = ALIGN(min, step);

	/** 20130420    
	 * start와 max의 node_min_pfn에 대한 offset (index)을 구한다.
	 **/
	sidx = start - bdata->node_min_pfn;
	midx = max - bdata->node_min_pfn;

	/** 20130420    
	 * hint_idx가 0이면 수행되지 않음.
	 **/
	if (bdata->hint_idx > sidx) {
		/*
		 * Handle the valid case of sidx being zero and still
		 * catch the fallback below.
		 */
		fallback = sidx + 1;
		sidx = align_idx(bdata, bdata->hint_idx, step);
	}

	while (1) {
		int merge;
		void *region;
		unsigned long eidx, i, start_off, end_off;
find_block:
		sidx = find_next_zero_bit(bdata->node_bootmem_map, midx, sidx);
		/** 20130420    
		 * sidx는 위에서 찾은 sidx를 step 단위로 round up한 결과.
		 *    step이 1일 경우 sidx가 그대로 리턴
		 * eidx는 page frame을 관리하기 위한 메모리의 마지막 page frame index.
		 **/
		sidx = align_idx(bdata, sidx, step);
		eidx = sidx + PFN_UP(size);

		/** 20130420    
		 * zero bit를 못 찾았을 경우 sidx == midx이므로 break
		 * eidx가 midx보다 크면 break
		 **/
		if (sidx >= midx || eidx > midx)
			break;

		/** 20130420    
		 * 사용할 sidx ~ eidx까지 사용 중인 page frame이 있다면 find_block으로 이동
		 **/
		for (i = sidx; i < eidx; i++)
			if (test_bit(i, bdata->node_bootmem_map)) {
				/** 20130420    
				 * sidx를 사용 중인 page frame idx의 다음 step으로 변경.
				 * step이 1인 경우 sidx == i, sidx를 step만큼 증가
				 * step이 1이 아닌 경우 align_idx에서 이미 다음 step으로 변경되어 있음.
				 **/
				sidx = align_idx(bdata, i, step);
				if (sidx == i)
					sidx += step;
				goto find_block;
			}

		/** 20130420    
		 * 사용하지 않는 sidx ~ edix를 찾은 경우
		 **/

		/** 20130420    
		 * 처음 실행시 last_end_off는 설정되지 않은 상태이므로 0
		 * 따라서 else 실행
		 **/
		if (bdata->last_end_off & (PAGE_SIZE - 1) &&
				PFN_DOWN(bdata->last_end_off) + 1 == sidx)
			start_off = align_off(bdata, bdata->last_end_off, align);
		else
			/** 20130420    
			 * sidx(offset)에 해당하는 물리 주소를 저장
			 **/
			start_off = PFN_PHYS(sidx);

		/** 20130420    
		 * start_off에 대한 PFN을 구해 sidx와 비교.
		 * else에서 왔을 경우 merge는 false
		 **/
		merge = PFN_DOWN(start_off) < sidx;
		/** 20130420    
		 * page frame을 관리하기 위해 사용되는 공간의 마지막 물리 offset
		 * (from node_min_pfn)
		 **/
		end_off = start_off + size;

		/** 20130420    
		 * bdata의 last_end_off과 hint_idx에 저장
		 **/
		bdata->last_end_off = end_off;
		bdata->hint_idx = PFN_UP(end_off);

		/*
		 * Reserve the area now:
		 */
		/** 20130420    
		 * pfn (start_off) ~ pfn (end_off) 영역에 대해 사용 중임을 설정.
		 * BOOTMEM_EXCLUSIVE 옵션에 의해 해당 영역이 이미 사용 중일 경우 BUG.
		 **/
		if (__reserve(bdata, PFN_DOWN(start_off) + merge,
				PFN_UP(end_off), BOOTMEM_EXCLUSIVE))
			BUG();

		/** 20130420    
		 * region은 page frame을 관리하는 (struct page) 배열 시작 주소 (va)
		 * 해당 영역을 0으로 초기화
		 **/
		region = phys_to_virt(PFN_PHYS(bdata->node_min_pfn) +
				start_off);
		memset(region, 0, size);
		/*
		 * The min_count is set to 0 so that bootmem allocated blocks
		 * are never reported as leaks.
		 */
		/** 20130420    
		 * vexpress에서는 NULL. (memory leak을 관리하기 위한 debug용 옵션인듯)
		 **/
		kmemleak_alloc(region, size, 0, 0);
		return region;
	}

	/** 20130420    
	 * fallback이 존재하는 경우 sidx를 다음 step(fallback-1)으로 조정한 뒤
	 * 다시 find_block을 호출
	 **/
	if (fallback) {
		sidx = align_idx(bdata, fallback - 1, step);
		fallback = 0;
		goto find_block;
	}

	return NULL;
}

/** 20130420    
 **/
static void * __init alloc_arch_preferred_bootmem(bootmem_data_t *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	/** 20130420    
	 * 초기화 하지 않은 상태이므로 slab이 사용 가능하다면 비정상임.
	 * 그럴 경우 경고를 출력
	 **/
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

/** 20130420    
 * vexpress 에서 정의되어 있지 않음
 **/
#ifdef CONFIG_HAVE_ARCH_BOOTMEM
	{
		bootmem_data_t *p_bdata;

		p_bdata = bootmem_arch_preferred_node(bdata, size, align,
							goal, limit);
		if (p_bdata)
			return alloc_bootmem_bdata(p_bdata, size, align,
							goal, limit);
	}
#endif
	return NULL;
}

/** 20130420    
 * bdata_list를 순회하며 alloc_bootmem_bdata를 수행하는 함수
 **/
static void * __init alloc_bootmem_core(unsigned long size,
					unsigned long align,
					unsigned long goal,
					unsigned long limit)
{
	bootmem_data_t *bdata;
	void *region;

	/** 20130420    
	 * vexpress 에서 NULL을 리턴
	 **/
	region = alloc_arch_preferred_bootmem(NULL, size, align, goal, limit);
	if (region)
		return region;

	/** 20130420    
	 * NUMA인 경우 bdata_list를 순회하며 alloc_bootmem_bdata를 시도
	 **/
	list_for_each_entry(bdata, &bdata_list, list) {
		/** 20130420    
		 * goal이 node_low_pfn보다 클 경우 continue
		 **/
		if (goal && bdata->node_low_pfn <= PFN_DOWN(goal))
			continue;
		/** 20130420    
		 * limit이 node_min_pfn보다 작을 경우 break
		 **/
		if (limit && bdata->node_min_pfn >= PFN_DOWN(limit))
			break;

		region = alloc_bootmem_bdata(bdata, size, align, goal, limit);
		if (region)
			return region;
	}

	return NULL;
}

static void * __init ___alloc_bootmem_nopanic(unsigned long size,
					      unsigned long align,
					      unsigned long goal,
					      unsigned long limit)
{
	void *ptr;

restart:
	ptr = alloc_bootmem_core(size, align, goal, limit);
	if (ptr)
		return ptr;
	if (goal) {
		goal = 0;
		goto restart;
	}

	return NULL;
}

/**
 * __alloc_bootmem_nopanic - allocate boot memory without panicking
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * Returns NULL on failure.
 */
void * __init __alloc_bootmem_nopanic(unsigned long size, unsigned long align,
					unsigned long goal)
{
	unsigned long limit = 0;

	return ___alloc_bootmem_nopanic(size, align, goal, limit);
}

static void * __init ___alloc_bootmem(unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	void *mem = ___alloc_bootmem_nopanic(size, align, goal, limit);

	if (mem)
		return mem;
	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem - allocate boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align,
			      unsigned long goal)
{
	unsigned long limit = 0;

	return ___alloc_bootmem(size, align, goal, limit);
}

/** 20130420    
 * node 상에 존재하는 page frame 들만큼을 관리하기 위한 page영역을 할당하는 함수
 **/
void * __init ___alloc_bootmem_node_nopanic(pg_data_t *pgdat,
				unsigned long size, unsigned long align,
				unsigned long goal, unsigned long limit)
{
	void *ptr;

again:
	ptr = alloc_arch_preferred_bootmem(pgdat->bdata, size,
					   align, goal, limit);
	if (ptr)
		return ptr;

	/* do not panic in alloc_bootmem_bdata() */
	/** 20130420    
	 * 현재 limit은 0으로 호출.
	 * 만약 limit 이 있지만 goal + size보다 작다면 limit을 무효화시킴
	 **/
	if (limit && goal + size > limit)
		limit = 0;

	/** 20130420    
	 * 영역을 성공적으로 할당했다면 ptr을 리턴
	 **/
	ptr = alloc_bootmem_bdata(pgdat->bdata, size, align, goal, limit);
	if (ptr)
		return ptr;

	ptr = alloc_bootmem_core(size, align, goal, limit);
	if (ptr)
		return ptr;

	/** 20130420    
	 * alloc_bootmem_core에서도 실패했을 경우 goal을 0으로 변경해 다시 시도
	 **/
	if (goal) {
		goal = 0;
		goto again;
	}

	return NULL;
}

/** 20130420    
 * bdata_list를 순회하며 alloc_bootmem_bdata를 수행하는 함수
 **/
void * __init __alloc_bootmem_node_nopanic(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	/** 20130413
	 * slab 초기화 안되어 있으므로 false 
	 */
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, 0);
}

void * __init ___alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				    unsigned long align, unsigned long goal,
				    unsigned long limit)
{
	void *ptr;

	ptr = ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, 0);
	if (ptr)
		return ptr;

	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem_node - allocate boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return  ___alloc_bootmem_node(pgdat, size, align, goal, 0);
}

void * __init __alloc_bootmem_node_high(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
#ifdef MAX_DMA32_PFN
	unsigned long end_pfn;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	/* update goal according ...MAX_DMA32_PFN */
	end_pfn = pgdat->node_start_pfn + pgdat->node_spanned_pages;

	if (end_pfn > MAX_DMA32_PFN + (128 >> (20 - PAGE_SHIFT)) &&
	    (goal >> PAGE_SHIFT) < MAX_DMA32_PFN) {
		void *ptr;
		unsigned long new_goal;

		new_goal = MAX_DMA32_PFN << PAGE_SHIFT;
		ptr = alloc_bootmem_bdata(pgdat->bdata, size, align,
						 new_goal, 0);
		if (ptr)
			return ptr;
	}
#endif

	return __alloc_bootmem_node(pgdat, size, align, goal);

}

#ifndef ARCH_LOW_ADDRESS_LIMIT
#define ARCH_LOW_ADDRESS_LIMIT	0xffffffffUL
#endif

/**
 * __alloc_bootmem_low - allocate low boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low(unsigned long size, unsigned long align,
				  unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, ARCH_LOW_ADDRESS_LIMIT);
}

/**
 * __alloc_bootmem_low_node - allocate low boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low_node(pg_data_t *pgdat, unsigned long size,
				       unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat, size, align,
				     goal, ARCH_LOW_ADDRESS_LIMIT);
}
