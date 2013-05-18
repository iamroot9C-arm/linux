/*
 * Procedures for maintaining information about logical memory blocks.
 *
 * Peter Bergner, IBM Corp.	June 2001.
 * Copyright (C) 2001 Peter Bergner.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/poison.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>

static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;

struct memblock memblock __initdata_memblock = {
	.memory.regions		= memblock_memory_init_regions,
	.memory.cnt		= 1,	/* empty dummy entry */
	.memory.max		= INIT_MEMBLOCK_REGIONS,

	.reserved.regions	= memblock_reserved_init_regions,
	.reserved.cnt		= 1,	/* empty dummy entry */
	.reserved.max		= INIT_MEMBLOCK_REGIONS,

	.current_limit		= MEMBLOCK_ALLOC_ANYWHERE,
};

int memblock_debug __initdata_memblock;
/** 20130126    
 * memblock_can_resize 은 memblock_double_array 등에서 사용
 **/
static int memblock_can_resize __initdata_memblock;
static int memblock_memory_in_slab __initdata_memblock = 0;
static int memblock_reserved_in_slab __initdata_memblock = 0;

/* inline so we don't get a warning when pr_debug is compiled out */
static inline const char *memblock_type_name(struct memblock_type *type)
{
	if (type == &memblock.memory)
		return "memory";
	else if (type == &memblock.reserved)
		return "reserved";
	else
		return "unknown";
}

/* adjust *@size so that (@base + *@size) doesn't overflow, return new size */
/** 20130119
실제 사용할수 있는 사이즈를 다시 한번 조사해서 리턴한다
 **/
static inline phys_addr_t memblock_cap_size(phys_addr_t base, phys_addr_t *size)
{
	return *size = min(*size, (phys_addr_t)ULLONG_MAX - base);
}

/*
 * Address comparison utilities
 */
/** 20130126    
 * addr가 서로 겹치는지 검사하는 함수 (포함관계까지 검사됨)
 **/
static unsigned long __init_memblock memblock_addrs_overlap(phys_addr_t base1, phys_addr_t size1,
				       phys_addr_t base2, phys_addr_t size2)
{
	return ((base1 < (base2 + size2)) && (base2 < (base1 + size1)));
}

/** 20130126    
 * memblock의 regions를 돌면서 주어진 addr와 겹치는 부분이 있는지 검사하는 함수
 * 겹쳐져 있으면 겹친 영역의 index를 리턴
 * 아니면 -1 리턴
 **/
static long __init_memblock memblock_overlaps_region(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size)
{
	unsigned long i;

	for (i = 0; i < type->cnt; i++) {
		phys_addr_t rgnbase = type->regions[i].base;
		phys_addr_t rgnsize = type->regions[i].size;
		if (memblock_addrs_overlap(base, size, rgnbase, rgnsize))
			break;
	}

	return (i < type->cnt) ? i : -1;
}

/**
 * memblock_find_in_range_node - find free area in given range and node
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %MAX_NUMNODES for any node
 *
 * Find @size free area aligned to @align in the specified range and node.
 *
 * RETURNS:
 * Found address on success, %0 on failure.
 */
 /** 20130302 
  	주어진 범위와 노드에서 alloc 가능한 메모리 영역(aligned)을 찾는다.
  **/	
phys_addr_t __init_memblock memblock_find_in_range_node(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align, int nid)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	/* pump up @end */
	/** 20130223    
	 * end가 0으로 넘어왔다면 end를 memblock의 current_limit으로 잡음
	 **/
	if (end == MEMBLOCK_ALLOC_ACCESSIBLE)
		end = memblock.current_limit;

	/* avoid allocating the first page */
	/** 20130223    
	 * start와 PAGE_SIZE 중 큰 값을 취해 첫번째 page를 할당하는 것을 피한다. 왜???
	 **/
	start = max_t(phys_addr_t, start, PAGE_SIZE);
	end = max(start, end);
/** 20130302 
 
#define for_each_free_mem_range_reverse(i, nid, p_start, p_end, p_nid)	\
	for (i = (u64)ULLONG_MAX,					\
	     __next_free_mem_range_rev(&i, nid, p_start, p_end, p_nid);	\
	     i != (u64)ULLONG_MAX;					\
	     __next_free_mem_range_rev(&i, nid, p_start, p_end, p_nid))
 **/	
	for_each_free_mem_range_reverse(i, nid, &this_start, &this_end, NULL) {
		/** 20130302 
			사용가능한 메모리영역이 start 와 end사이의 주소로 보장됨
	 	**/	
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);
		/** 20130302 
			size : 요구 사이즈
			요구한 사이즈보다 실제 사용가능한 메모리 region의 끝주소가 작을 경우 다시 찾음
 		**/	
		if (this_end < size)
			continue;

		/** 20130302 
		 	메모리 최소 요구 사이즈를 충족시키기는 align된 메모리시작주소 candidate(...???)에 저장한다.
			
		 **/	
		cand = round_down(this_end - size, align);
		/** 20130302 
		 	현재 사용가능한 memblock memory region에 메모리 공간을 잡을 수 있다면 candidate를 리턴한다.
		 **/	
		if (cand >= this_start)
			return cand;
	}
	return 0;
}

/**
 * memblock_find_in_range - find free area in given range
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 *
 * Find @size free area aligned to @align in the specified range.
 *
 * RETURNS:
 * Found address on success, %0 on failure.
 */
phys_addr_t __init_memblock memblock_find_in_range(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align)
{
	return memblock_find_in_range_node(start, end, size, align,
					   MAX_NUMNODES);
}

static void __init_memblock memblock_remove_region(struct memblock_type *type, unsigned long r)
{
	type->total_size -= type->regions[r].size;
	memmove(&type->regions[r], &type->regions[r + 1],
		(type->cnt - (r + 1)) * sizeof(type->regions[r]));
	type->cnt--;

	/* Special case for empty arrays */
	if (type->cnt == 0) {
		WARN_ON(type->total_size != 0);
		type->cnt = 1;
		type->regions[0].base = 0;
		type->regions[0].size = 0;
		memblock_set_region_node(&type->regions[0], MAX_NUMNODES);
	}
}

phys_addr_t __init_memblock get_allocated_memblock_reserved_regions_info(
					phys_addr_t *addr)
{
	if (memblock.reserved.regions == memblock_reserved_init_regions)
		return 0;

	*addr = __pa(memblock.reserved.regions);

	return PAGE_ALIGN(sizeof(struct memblock_region) *
			  memblock.reserved.max);
}

/**
 * memblock_double_array - double the size of the memblock regions array
 * @type: memblock type of the regions array being doubled
 * @new_area_start: starting address of memory range to avoid overlap with
 * @new_area_size: size of memory range to avoid overlap with
 *
 * Double the size of the @type regions array. If memblock is being used to
 * allocate memory for a new reserved regions array and there is a previously
 * allocated memory range [@new_area_start,@new_area_start+@new_area_size]
 * waiting to be reserved, ensure the memory used by the new array does
 * not overlap.
 *
 * RETURNS:
 * 0 on success, -1 on failure.
 */
static int __init_memblock memblock_double_array(struct memblock_type *type,
						phys_addr_t new_area_start,
						phys_addr_t new_area_size)
{
	struct memblock_region *new_array, *old_array;
	phys_addr_t old_alloc_size, new_alloc_size;
	phys_addr_t old_size, new_size, addr;
	/** 20130126    
	 * 현재 slab_state를 초기화하는 부분이 나오지 않았음.
	 * false라 가정하고 분석시작.
	 **/
	int use_slab = slab_is_available();
	int *in_slab;

	/* We don't allow resizing until we know about the reserved regions
	 * of memory that aren't suitable for allocation
	 */
	/** 20130126    
	 * memblock_can_resize는 static 전역변수.
	 * memblock_add_region에서 호출될 때는 초기값 상태로 들어와 return -1.
	 **/
	if (!memblock_can_resize)
		return -1;

	/* Calculate new doubled size */
	old_size = type->max * sizeof(struct memblock_region);
	new_size = old_size << 1;
	/*
	 * We need to allocated new one align to PAGE_SIZE,
	 *   so we can free them completely later.
	 */
	old_alloc_size = PAGE_ALIGN(old_size);
	new_alloc_size = PAGE_ALIGN(new_size);

	/* Retrieve the slab flag */
	if (type == &memblock.memory)
		in_slab = &memblock_memory_in_slab;
	else
		in_slab = &memblock_reserved_in_slab;

	/* Try to find some space for it.
	 *
	 * WARNING: We assume that either slab_is_available() and we use it or
	 * we use MEMBLOCK for allocations. That means that this is unsafe to
	 * use when bootmem is currently active (unless bootmem itself is
	 * implemented on top of MEMBLOCK which isn't the case yet)
	 *
	 * This should however not be an issue for now, as we currently only
	 * call into MEMBLOCK while it's still active, or much later when slab
	 * is active for memory hotplug operations
	 */
	if (use_slab) {
		new_array = kmalloc(new_size, GFP_KERNEL);
		addr = new_array ? __pa(new_array) : 0;
	} else {
		/* only exclude range when trying to double reserved.regions */
		if (type != &memblock.reserved)
			new_area_start = new_area_size = 0;

		addr = memblock_find_in_range(new_area_start + new_area_size,
						memblock.current_limit,
						new_alloc_size, PAGE_SIZE);
		if (!addr && new_area_size)
			addr = memblock_find_in_range(0,
				min(new_area_start, memblock.current_limit),
				new_alloc_size, PAGE_SIZE);

		new_array = addr ? __va(addr) : 0;
	}
	if (!addr) {
		pr_err("memblock: Failed to double %s array from %ld to %ld entries !\n",
		       memblock_type_name(type), type->max, type->max * 2);
		return -1;
	}

	memblock_dbg("memblock: %s is doubled to %ld at [%#010llx-%#010llx]",
			memblock_type_name(type), type->max * 2, (u64)addr,
			(u64)addr + new_size - 1);

	/*
	 * Found space, we now need to move the array over before we add the
	 * reserved region since it may be our reserved array itself that is
	 * full.
	 */
	memcpy(new_array, type->regions, old_size);
	memset(new_array + type->max, 0, old_size);
	old_array = type->regions;
	type->regions = new_array;
	type->max <<= 1;

	/* Free old array. We needn't free it if the array is the static one */
	if (*in_slab)
		kfree(old_array);
	else if (old_array != memblock_memory_init_regions &&
		 old_array != memblock_reserved_init_regions)
		memblock_free(__pa(old_array), old_alloc_size);

	/*
	 * Reserve the new array if that comes from the memblock.  Otherwise, we
	 * needn't do it
	 */
	if (!use_slab)
		BUG_ON(memblock_reserve(addr, new_alloc_size));

	/* Update slab flag */
	*in_slab = use_slab;

	return 0;
}

/**
 * memblock_merge_regions - merge neighboring compatible regions
 * @type: memblock type to scan
 *
 * Scan @type and merge neighboring compatible regions.
 */
/** 20130126    
 * 인접하는 region을 하나의 region으로 통합
 * (함수 호출 전에 overlap된 정보를 새로운 region으로 등록시킨 상태)
 **/
static void __init_memblock memblock_merge_regions(struct memblock_type *type)
{
	int i = 0;

	/* cnt never goes below 1 */
	while (i < type->cnt - 1) {
		struct memblock_region *this = &type->regions[i];
		struct memblock_region *next = &type->regions[i + 1];

		/** 20130126    
		 * 현재 region의 마지막 address가 다음 region의 address와 인접하는지 검사
		 **/
		if (this->base + this->size != next->base ||
			/** 20130126
			 * memblock_get_region_node 은 현재 config 에서 return 0
			 **/
		    memblock_get_region_node(this) !=
		    memblock_get_region_node(next)) {
			BUG_ON(this->base + this->size > next->base);
			i++;
			/** 20130126    
			 * 인접하지 않을 경우 merge 대상에서 제외
			 **/
			continue;
		}

		/** 20130126    
		 * 다음 region의 size를 더해주고, 앞으로 하나씩 당김
		 **/
		this->size += next->size;
		memmove(next, next + 1, (type->cnt - (i + 1)) * sizeof(*next));
		type->cnt--;
	}
}

/**
 * memblock_insert_region - insert new memblock region
 * @type: memblock type to insert into
 * @idx: index for the insertion point
 * @base: base address of the new region
 * @size: size of the new region
 *
 * Insert new memblock region [@base,@base+@size) into @type at @idx.
 * @type must already have extra room to accomodate the new region.
 */
/** 20130126    
 * memblock에 새로운 region을 추가한다.
 **/
static void __init_memblock memblock_insert_region(struct memblock_type *type,
						   int idx, phys_addr_t base,
						   phys_addr_t size, int nid)
{
	struct memblock_region *rgn = &type->regions[idx];

	BUG_ON(type->cnt >= type->max);
	/** 20130126    
	 * idx부터 뒤의 모든 rgn을 밀어 insert 할 공간을 확보.
	 * 초기 type->cnt 1, idx 역시 1로 들어오면 실제로 공간 확보를 이뤄지지 않음.
	 **/
	memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));
	rgn->base = base;
	rgn->size = size;
	/** 20130126    
	 * 현재 vexpress 기준 config에서는 NULL 함수
	 **/
	memblock_set_region_node(rgn, nid);
	type->cnt++;
	type->total_size += size;
}

/**
 * memblock_add_region - add new memblock region
 * @type: memblock type to add new region into
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 *
 * Add new memblock region [@base,@base+@size) into @type.  The new region
 * is allowed to overlap with existing ones - overlaps don't affect already
 * existing regions.  @type is guaranteed to be minimal (all neighbouring
 * compatible regions are merged) after the addition.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */

/** 20130119

  <처리전>
              rbase  rend     rbase   rend
                +------+        +-------+
  memblock      |   0  |        |  1    |
                +------+        +-------+                      
          base            end
            +---------------+
  meminfo   |               |
            +---------------+

            
  <merge처리전>
            +---------------+   +-------+
  memblock  | 0 |   1   | 2 |   |  3    |
            +---------------+   +-------+

  <merge처리후>
            +---------------+   +-------+
  memblock  |       0       |   |   1   |
            +---------------+   +-------+ 
 **/

/** 20130126    
 *  memblock에 새로운 region을 추가한다.
 *  logical memory block 을 관리하는 memblock 변수의 자료구조를 채운다.
 **/
static int __init_memblock memblock_add_region(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size, int nid)
{
	bool insert = false;
	phys_addr_t obase = base;
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int i, nr_new;

	if (!size)
		return 0;

	/* special case for empty array */
	if (type->regions[0].size == 0) {
        /** 20130119
          type->regions가 memblock_memory_init_regions[128]로 초기화 되어 있음
          최초에는 이 블록으로 들어옴
        **/
		WARN_ON(type->cnt != 1 || type->total_size);
		type->regions[0].base = base;
		type->regions[0].size = size;
		memblock_set_region_node(&type->regions[0], nid);
		type->total_size = size;
		return 0;
	}
repeat:
	/*
	 * The following is executed twice.  Once with %false @insert and
	 * then with %true.  The first counts the number of regions needed
	 * to accomodate the new area.  The second actually inserts them.
	 */
	base = obase;
	/** 20130126    
	 * insert를 하려고 하는 memblock region의 수
	 **/
	nr_new = 0;


	for (i = 0; i < type->cnt; i++) {
		struct memblock_region *rgn = &type->regions[i];
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;
/** 20130119
  rbase : memblock region의 base 값
  rend  : memblock region의 end 값
  base  : 추가할 meminfo의 base 값 
  end   : 추가할 meminfo의 end 값
 **/
		if (rbase >= end)
			break;
		if (rend <= base)
			continue;
		/*
		 * @rgn overlaps.  If it separates the lower part of new
		 * area, insert that portion.
		 */
		if (rbase > base) {
			nr_new++;
			if (insert)
                /** 20130119
                  memblock의 region을 새롭게 추가한다. 
                  type->cnt, type->total_size값을 갱신한다 
                 **/
				memblock_insert_region(type, i++, base,
						       rbase - base, nid);
		}
		/* area below @rend is dealt with, forget about it */

		/** 20130119
         만약 새로 추가할 meminfo의 end값이 기존memblock의 rend값보다 크다면
         meminfo의 나머지 메모리 영역을 insert하기 위해 rend값을 base 값으로 설정한다
         **/
        base = min(rend, end);
	}
    
	/* insert the remaining portion */
	if (base < end) {
		nr_new++;
		if (insert)
			memblock_insert_region(type, i, base, end - base, nid);
	}

	/*
	 * If this was the first round, resize array and repeat for actual
	 * insertions; otherwise, merge and return.
	 */
	if (!insert) {
		/** 20130126    
		 * type->cnt 현재 memblock에 존재하는 region의 수 
		 * nr_new    새롭게 insert할 region의 수
		 *
		 * type->max는 128로 define.
		 **/
		while (type->cnt + nr_new > type->max)
			if (memblock_double_array(type, obase, size) < 0)
				return -ENOMEM;
		insert = true;
		goto repeat;
	} else {
		memblock_merge_regions(type);
		return 0;
	}
}

int __init_memblock memblock_add_node(phys_addr_t base, phys_addr_t size,
				       int nid)
{
	return memblock_add_region(&memblock.memory, base, size, nid);
}

/** 20130126
 * memblock_add 함수를 호출하면 static 함수인 memblock_add_region을 호출
 * memblock.memory에 등록
 **/
int __init_memblock memblock_add(phys_addr_t base, phys_addr_t size)
{
	return memblock_add_region(&memblock.memory, base, size, MAX_NUMNODES);
}

/**
 * memblock_isolate_range - isolate given range into disjoint memblocks
 * @type: memblock type to isolate range for
 * @base: base of range to isolate
 * @size: size of range to isolate
 * @start_rgn: out parameter for the start of isolated region
 * @end_rgn: out parameter for the end of isolated region
 *
 * Walk @type and ensure that regions don't cross the boundaries defined by
 * [@base,@base+@size).  Crossing regions are split at the boundaries,
 * which may create at most two more regions.  The index of the first
 * region inside the range is returned in *@start_rgn and end in *@end_rgn.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
static int __init_memblock memblock_isolate_range(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size,
					int *start_rgn, int *end_rgn)
{
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int i;

	*start_rgn = *end_rgn = 0;

	if (!size)
		return 0;

	/* we'll create at most two more regions */
	while (type->cnt + 2 > type->max)
		if (memblock_double_array(type, base, size) < 0)
			return -ENOMEM;

	for (i = 0; i < type->cnt; i++) {
		struct memblock_region *rgn = &type->regions[i];
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		if (rbase >= end)
			break;
		if (rend <= base)
			continue;

		if (rbase < base) {
			/*
			 * @rgn intersects from below.  Split and continue
			 * to process the next region - the new top half.
			 */
			rgn->base = base;
			rgn->size -= base - rbase;
			type->total_size -= base - rbase;
			memblock_insert_region(type, i, rbase, base - rbase,
					       memblock_get_region_node(rgn));
		} else if (rend > end) {
			/*
			 * @rgn intersects from above.  Split and redo the
			 * current region - the new bottom half.
			 */
			rgn->base = end;
			rgn->size -= end - rbase;
			type->total_size -= end - rbase;
			memblock_insert_region(type, i--, rbase, end - rbase,
					       memblock_get_region_node(rgn));
		} else {
			/* @rgn is fully contained, record it */
			if (!*end_rgn)
				*start_rgn = i;
			*end_rgn = i + 1;
		}
	}

	return 0;
}

static int __init_memblock __memblock_remove(struct memblock_type *type,
					     phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = end_rgn - 1; i >= start_rgn; i--)
		memblock_remove_region(type, i);
	return 0;
}

int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
	return __memblock_remove(&memblock.memory, base, size);
}

int __init_memblock memblock_free(phys_addr_t base, phys_addr_t size)
{
	memblock_dbg("   memblock_free: [%#016llx-%#016llx] %pF\n",
		     (unsigned long long)base,
		     (unsigned long long)base + size,
		     (void *)_RET_IP_);

	return __memblock_remove(&memblock.reserved, base, size);
}

/** 20130126    
 * memblock 구조체의 reserved 영역에 등록
 **/
int __init_memblock memblock_reserve(phys_addr_t base, phys_addr_t size)
{
	struct memblock_type *_rgn = &memblock.reserved;

	/** 20130126    
	 * memblock=debug를 주면 early_memblock에서 debug 정보를 출력하도록 설정
	 **/
	memblock_dbg("memblock_reserve: [%#016llx-%#016llx] %pF\n",
		     (unsigned long long)base,
		     (unsigned long long)base + size,
		     (void *)_RET_IP_);

	/** 20130126    
	 * memblock.reserved에 등록
	 **/
	return memblock_add_region(_rgn, base, size, MAX_NUMNODES);
}

/**
 * __next_free_mem_range - next function for for_each_free_mem_range()
 * @idx: pointer to u64 loop variable
 * @nid: nid: node selector, %MAX_NUMNODES for all nodes
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Find the first free area from *@idx which matches @nid, fill the out
 * parameters, and update *@idx for the next iteration.  The lower 32bit of
 * *@idx contains index into memory region and the upper 32bit indexes the
 * areas before each reserved region.  For example, if reserved regions
 * look like the following,
 *
 *	0:[0-16), 1:[32-48), 2:[128-130)
 *
 * The upper 32bit indexes the following regions.
 *
 *	0:[0-0), 1:[16-32), 2:[48-128), 3:[130-MAX)
 *
 * As both region arrays are sorted, the function advances the two indices
 * in lockstep and returns each intersection.
 */
void __init_memblock __next_free_mem_range(u64 *idx, int nid,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end, int *out_nid)
{
	struct memblock_type *mem = &memblock.memory;
	struct memblock_type *rsv = &memblock.reserved;
	int mi = *idx & 0xffffffff;
	int ri = *idx >> 32;

	for ( ; mi < mem->cnt; mi++) {
		struct memblock_region *m = &mem->regions[mi];
		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;

		/* only memory regions are associated with nodes, check it */
		if (nid != MAX_NUMNODES && nid != memblock_get_region_node(m))
			continue;

		/* scan areas before each reservation for intersection */
		for ( ; ri < rsv->cnt + 1; ri++) {
			struct memblock_region *r = &rsv->regions[ri];
			phys_addr_t r_start = ri ? r[-1].base + r[-1].size : 0;
			phys_addr_t r_end = ri < rsv->cnt ? r->base : ULLONG_MAX;

			/* if ri advanced past mi, break out to advance mi */
			if (r_start >= m_end)
				break;
			/* if the two regions intersect, we're done */
			if (m_start < r_end) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = memblock_get_region_node(m);
				/*
				 * The region which ends first is advanced
				 * for the next iteration.
				 */
				if (m_end <= r_end)
					mi++;
				else
					ri++;
				*idx = (u32)mi | (u64)ri << 32;
				return;
			}
		}
	}

	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/**
 * __next_free_mem_range_rev - next function for for_each_free_mem_range_reverse()
 * @idx: pointer to u64 loop variable
 * @nid: nid: node selector, %MAX_NUMNODES for all nodes
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Reverse of __next_free_mem_range().
 */
 /** 20130302 
   idx : &i(ULLONG_MAX)
   nid : 1
   out_start : &this_start 
   end_start : &this_end
   out_nid   : null

	memblock memory region과 reserved region을 역순으로 돌면서 memory region영역에서 사용가능한 reserved region영역을 찾아서 리턴한다.
	
  **/	
void __init_memblock __next_free_mem_range_rev(u64 *idx, int nid,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end, int *out_nid)
{
	/** 20130223    
	 * memblock.memory와 memblock.reserved 각각을 mem, rsv로 가리킴
	 **/
	struct memblock_type *mem = &memblock.memory;
	struct memblock_type *rsv = &memblock.reserved;
	/** 20130223    
	 * mi: *idx의 하위 32bit 값
	 * ri: *idx의 상위 32bit 값
	 */
	int mi = *idx & 0xffffffff;
	int ri = *idx >> 32;

	/** 20130223
	 * 최초 값에는 UULONG_MAX로 같음
	 **/
	if (*idx == (u64)ULLONG_MAX) {
		/** 20130223    
		 * mi를 memblock.memory region의 count - 1,
		 * ri를 memblock.reserved region의 count로 설정
		 **/
		mi = mem->cnt - 1;
		ri = rsv->cnt;
	}
/** 20130302 

memblock memory region이 여러개인 상황을 테스트

qemu-system-arm -M vexpress-a9    -kernel ./arch/arm/boot/zImage  -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug mem=64M@0x60000000 mem=32M@0x66000000 memblock=debug"

Machine: ARM-Versatile Express
memblock_reserve: [0x000000600081c0-0x000000608e66d0] arm_memblock_init+0xd8/0x318
memblock_reserve: [0x00000060004000-0x00000060008000] arm_mm_memblock_reserve+0x30/0x38
MEMBLOCK configuration:
 memory size = 0x6000000 reserved size = 0x8e2510
 memory.cnt  = 0x2
 memory[0x0]    [0x00000060000000-0x00000063ffffff], 0x4000000 bytes
 memory[0x1]    [0x00000066000000-0x00000067ffffff], 0x2000000 bytes
 reserved.cnt  = 0x2
 reserved[0x0]  [0x00000060004000-0x00000060007fff], 0x4000 bytes
 reserved[0x1]  [0x000000600081c0-0x000000608e66cf], 0x8de510 bytes
Memory policy: ECC disabled, Data cache writealloc
memblock_reserve: [0x00000067fff000-0x00000068000000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffe000-0x00000067fff000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffdfe0-0x00000067ffe000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffc000-0x00000067ffd000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffdfc0-0x00000067ffdfe0] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffb000-0x00000067ffc000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ffa000-0x00000067ffb000] memblock_alloc_base_nid+0x6c/0x90
memblock_reserve: [0x00000067ff9000-0x00000067ffa000] memblock_alloc_base_nid+0x6c/0x90
 **/	
	for ( ; mi >= 0; mi--) {
		struct memblock_region *m = &mem->regions[mi];
		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;

		/* only memory regions are associated with nodes, check it */
		/** 20130223    
		 * nid가 1로 넘어오면 MAX_NUMNODES와 같으므로 flase 
		 **/
		if (nid != MAX_NUMNODES && nid != memblock_get_region_node(m))
			continue;
		/** 20130302 
 		vexpress 에서 reserved 영역은 text~bss영역, initrd가 있을경우 initrd, swapper_pg_dir영역이 존재한다.
 		**/	
		/* scan areas before each reservation for intersection */
		for ( ; ri >= 0; ri--) {
			struct memblock_region *r = &rsv->regions[ri];
			/** 20130223    
			 * memblock.reserved의 개수가 1 이상이면 마지막 region의 base + size
			 **/
			phys_addr_t r_start = ri ? r[-1].base + r[-1].size : 0;
			/** 20130223
			 * for문 최초 수행시에는 ri와 rsv->cnt 같음
			 *  r_end <- ULLONG_MAX
			 * 다음 수행시에는 ri가 감소되므로 true
			 *  r_end <- r->base
			 **/
			phys_addr_t r_end = ri < rsv->cnt ? r->base : ULLONG_MAX;

			/** 20130302 
			 현재 memory region 이 현재reserved region과 겹치지 않으면 break 
			 **/ 
			/* if ri advanced past mi, break out to advance mi */
			if (r_end <= m_start)
				break;
			/* if the two regions intersect, we're done */
			/** 20130302 
			 현재 memory region에서 reserved region으로 사용될 수 있는 영역을 찾아서 리턴한다
			 **/	
			if (m_end > r_start) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
					/** 20130302 
					 	vexpress일경우 memblock_get_region_node 는 empty function
					 **/ 
				if (out_nid)
					*out_nid = memblock_get_region_node(m);

				if (m_start >= r_start)	
					/** 20130302 
					 현제 reserved영역이 물리메모리 영역이 아닐 경우 mi를 감소한다
					 **/	
					mi--;
				else
					/** 20130302 
					 현제 물리메모리에서 다른 reserved영역을 가리키게함
					 **/ 
					ri--;
				*idx = (u32)mi | (u64)ri << 32;
				return;
			}
		}
	}

	*idx = ULLONG_MAX;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
/*
 * Common iterator interface used to define for_each_mem_range().
 */
void __init_memblock __next_mem_pfn_range(int *idx, int nid,
				unsigned long *out_start_pfn,
				unsigned long *out_end_pfn, int *out_nid)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *r;

	while (++*idx < type->cnt) {
		r = &type->regions[*idx];

		if (PFN_UP(r->base) >= PFN_DOWN(r->base + r->size))
			continue;
		if (nid == MAX_NUMNODES || nid == r->nid)
			break;
	}
	if (*idx >= type->cnt) {
		*idx = -1;
		return;
	}

	if (out_start_pfn)
		*out_start_pfn = PFN_UP(r->base);
	if (out_end_pfn)
		*out_end_pfn = PFN_DOWN(r->base + r->size);
	if (out_nid)
		*out_nid = r->nid;
}

/**
 * memblock_set_node - set node ID on memblock regions
 * @base: base of area to set node ID for
 * @size: size of area to set node ID for
 * @nid: node ID to set
 *
 * Set the nid of memblock memory regions in [@base,@base+@size) to @nid.
 * Regions which cross the area boundaries are split as necessary.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_set_node(phys_addr_t base, phys_addr_t size,
				      int nid)
{
	struct memblock_type *type = &memblock.memory;
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		type->regions[i].nid = nid;

	memblock_merge_regions(type);
	return 0;
}
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

static phys_addr_t __init memblock_alloc_base_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t max_addr,
					int nid)
{
	phys_addr_t found;

	/* align @size to avoid excessive fragmentation on reserved array */
	/** 20130302 
	 	요구 사이즈가 align된 사이즈보다 작을 경우 최소 align된 사이즈로 맞추기 위해 round_up을 한다.
	 **/	
	size = round_up(size, align);

	/** 20130302 
 		max_addr 보다 작은 범위에서 memblock alloc 가능한 영역을 찾는다.
 	**/	
	found = memblock_find_in_range_node(0, max_addr, size, align, nid);
	/** 20130302 
	 	alloc 가능한 영역을 찾으면 memblock reserved에 등록한다.
	 **/	
	if (found && !memblock_reserve(found, size))
		return found;

	return 0;
}

phys_addr_t __init memblock_alloc_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	return memblock_alloc_base_nid(size, align, MEMBLOCK_ALLOC_ACCESSIBLE, nid);
}

/** 20130302 
	max_addr보다 작은 범위에서만 alloc을 수행함. 
 **/	
phys_addr_t __init __memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
/** 20130302 
 	vexpress의 경우에는 MAX_NUMNODES 가 1이다.
 **/	
	return memblock_alloc_base_nid(size, align, max_addr, MAX_NUMNODES);
}
/** 20130302 
 	memblock의 memory alloc이 실패했을 경우 패닉정보 출력
	max_addr : alloc 가능한 memblock의 최대 주소. memblock_alloc에서 호출했을 경우는 0.
 **/	
phys_addr_t __init memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
	phys_addr_t alloc;

	alloc = __memblock_alloc_base(size, align, max_addr);

	if (alloc == 0)
		panic("ERROR: Failed to allocate 0x%llx bytes below 0x%llx.\n",
		      (unsigned long long) size, (unsigned long long) max_addr);

	return alloc;
}

/** 20130223    
 * vexpress에서 이 함수 호출
 **/
/** 20130302 
	memblock 영역에서 사용가능한 align된 공간을 확보하는 함수
**/	
phys_addr_t __init memblock_alloc(phys_addr_t size, phys_addr_t align)
{
/** 20130302 
 	alloc가능한 memblock의 최대 주소를 MEMBLOCK_ALLOC_ACCESSIBLE 로 지정함
 **/	
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}

phys_addr_t __init memblock_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	phys_addr_t res = memblock_alloc_nid(size, align, nid);

	if (res)
		return res;
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}


/*
 * Remaining API functions
 */

phys_addr_t __init memblock_phys_mem_size(void)
{
	return memblock.memory.total_size;
}

/* lowest address */
phys_addr_t __init_memblock memblock_start_of_DRAM(void)
{
	return memblock.memory.regions[0].base;
}

phys_addr_t __init_memblock memblock_end_of_DRAM(void)
{
	int idx = memblock.memory.cnt - 1;

	return (memblock.memory.regions[idx].base + memblock.memory.regions[idx].size);
}

void __init memblock_enforce_memory_limit(phys_addr_t limit)
{
	unsigned long i;
	phys_addr_t max_addr = (phys_addr_t)ULLONG_MAX;

	if (!limit)
		return;

	/* find out max address */
	for (i = 0; i < memblock.memory.cnt; i++) {
		struct memblock_region *r = &memblock.memory.regions[i];

		if (limit <= r->size) {
			max_addr = r->base + limit;
			break;
		}
		limit -= r->size;
	}

	/* truncate both memory and reserved regions */
	__memblock_remove(&memblock.memory, max_addr, (phys_addr_t)ULLONG_MAX);
	__memblock_remove(&memblock.reserved, max_addr, (phys_addr_t)ULLONG_MAX);
}

/** 20130126    
 * memblock에서 addr을 포함하는 region을 검사하는 함수
 **/
static int __init_memblock memblock_search(struct memblock_type *type, phys_addr_t addr)
{
	/** 20130126    
	 * 0부터 type의 개수에 대해 binary search
	 * addr을 포함하는 regions의 index를 리턴. 못 찾으면 -1 리턴.
	 **/ 
	unsigned int left = 0, right = type->cnt;

	do {
		unsigned int mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else
			return mid;
	} while (left < right);
	return -1;
}

int __init memblock_is_reserved(phys_addr_t addr)
{
	return memblock_search(&memblock.reserved, addr) != -1;
}

/** 20130518    
 * addr이 memblock.memory에 포함된다면 1을 리턴.
 * 그렇지 않다면 0을 리턴.
 **/
int __init_memblock memblock_is_memory(phys_addr_t addr)
{
	/** 20130518    
	 * addr가 memblock.memory의 region에 속하는지 검사
	 **/
	return memblock_search(&memblock.memory, addr) != -1;
}

/**
 * memblock_is_region_memory - check if a region is a subset of memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base+@size) is a subset of a memory block.
 *
 * RETURNS:
 * 0 if false, non-zero if true
 */
int __init_memblock memblock_is_region_memory(phys_addr_t base, phys_addr_t size)
{
	int idx = memblock_search(&memblock.memory, base);
	phys_addr_t end = base + memblock_cap_size(base, &size);

	if (idx == -1)
		return 0;
	/** 20130126    
	 * 찾은 region의 index의 영역 안에 base와 end가 
	 * reg[idx].base <= base ~ base+size <= reg[idx].end 이면 true
	 **/
	return memblock.memory.regions[idx].base <= base &&
		(memblock.memory.regions[idx].base +
		 memblock.memory.regions[idx].size) >= end;
}

/**
 * memblock_is_region_reserved - check if a region intersects reserved memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base+@size) intersects a reserved memory block.
 *
 * RETURNS:
 * 0 if false, non-zero if true
 */
/** 20130126    
 * memblock.reserved 영역과 주어진 주소 공간이 겹치는지 검사하는 함수
 **/
int __init_memblock memblock_is_region_reserved(phys_addr_t base, phys_addr_t size)
{
	memblock_cap_size(base, &size);
	/** 20130126    
	 * index 이므로 0과 비교
	 **/
	return memblock_overlaps_region(&memblock.reserved, base, size) >= 0;
}

/** 20130119
memblock.current_limit값을 인자로 넘어온 limit값으로 세팅
 **/
void __init_memblock memblock_set_current_limit(phys_addr_t limit)
{
	memblock.current_limit = limit;
}

/** 20130126    
 * memblock_type으로 주어진 배열을 순회하며 region 정보를 출력
 **/
static void __init_memblock memblock_dump(struct memblock_type *type, char *name)
{
	unsigned long long base, size;
	int i;

	pr_info(" %s.cnt  = 0x%lx\n", name, type->cnt);

	for (i = 0; i < type->cnt; i++) {
		struct memblock_region *rgn = &type->regions[i];
		char nid_buf[32] = "";

		base = rgn->base;
		size = rgn->size;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
		if (memblock_get_region_node(rgn) != MAX_NUMNODES)
			snprintf(nid_buf, sizeof(nid_buf), " on node %d",
				 memblock_get_region_node(rgn));
#endif
		pr_info(" %s[%#x]\t[%#016llx-%#016llx], %#llx bytes%s\n",
			name, i, base, base + size - 1, size, nid_buf);
	}
}

/** 20130126    
 * memblock의 모든 내용을 출력
 **/
void __init_memblock __memblock_dump_all(void)
{
	pr_info("MEMBLOCK configuration:\n");
	pr_info(" memory size = %#llx reserved size = %#llx\n",
		(unsigned long long)memblock.memory.total_size,
		(unsigned long long)memblock.reserved.total_size);

	/** 20130126    
	 * memblock의 각 region 정보를 출력
	 **/
	memblock_dump(&memblock.memory, "memory");
	memblock_dump(&memblock.reserved, "reserved");
}

/** 20130126    
 * memblock resize 를 허용
 **/
void __init memblock_allow_resize(void)
{
	memblock_can_resize = 1;
}

static int __init early_memblock(char *p)
{
	if (p && strstr(p, "debug"))
		memblock_debug = 1;
	return 0;
}
early_param("memblock", early_memblock);

#if defined(CONFIG_DEBUG_FS) && !defined(CONFIG_ARCH_DISCARD_MEMBLOCK)

static int memblock_debug_show(struct seq_file *m, void *private)
{
	struct memblock_type *type = m->private;
	struct memblock_region *reg;
	int i;

	for (i = 0; i < type->cnt; i++) {
		reg = &type->regions[i];
		seq_printf(m, "%4d: ", i);
		if (sizeof(phys_addr_t) == 4)
			seq_printf(m, "0x%08lx..0x%08lx\n",
				   (unsigned long)reg->base,
				   (unsigned long)(reg->base + reg->size - 1));
		else
			seq_printf(m, "0x%016llx..0x%016llx\n",
				   (unsigned long long)reg->base,
				   (unsigned long long)(reg->base + reg->size - 1));

	}
	return 0;
}

static int memblock_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, memblock_debug_show, inode->i_private);
}

static const struct file_operations memblock_debug_fops = {
	.open = memblock_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init memblock_init_debugfs(void)
{
	struct dentry *root = debugfs_create_dir("memblock", NULL);
	if (!root)
		return -ENXIO;
	debugfs_create_file("memory", S_IRUGO, root, &memblock.memory, &memblock_debug_fops);
	debugfs_create_file("reserved", S_IRUGO, root, &memblock.reserved, &memblock_debug_fops);

	return 0;
}
__initcall(memblock_init_debugfs);

#endif /* CONFIG_DEBUG_FS */










































