/*
 * mm/percpu-vm.c - vmalloc area based chunk allocation
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This file is released under the GPLv2.
 *
 * Chunks are mapped into vmalloc areas and populated page by page.
 * This is the default chunk allocator.
 */

static struct page *pcpu_chunk_page(struct pcpu_chunk *chunk,
				    unsigned int cpu, int page_idx)
{
	/* must not be used on pre-mapped chunk */
	WARN_ON(chunk->immutable);

	return vmalloc_to_page((void *)pcpu_chunk_addr(chunk, cpu, page_idx));
}

/**
 * pcpu_get_pages_and_bitmap - get temp pages array and bitmap
 * @chunk: chunk of interest
 * @bitmapp: output parameter for bitmap
 * @may_alloc: may allocate the array
 *
 * Returns pointer to array of pointers to struct page and bitmap,
 * both of which can be indexed with pcpu_page_idx().  The returned
 * array is cleared to zero and *@bitmapp is copied from
 * @chunk->populated.  Note that there is only one array and bitmap
 * and access exclusion is the caller's responsibility.
 *
 * CONTEXT:
 * pcpu_alloc_mutex and does GFP_KERNEL allocation if @may_alloc.
 * Otherwise, don't care.
 *
 * RETURNS:
 * Pointer to temp pages array on success, NULL on failure.
 */
static struct page **pcpu_get_pages_and_bitmap(struct pcpu_chunk *chunk,
					       unsigned long **bitmapp,
					       bool may_alloc)
{
	static struct page **pages;
	static unsigned long *bitmap;
	size_t pages_size = pcpu_nr_units * pcpu_unit_pages * sizeof(pages[0]);
	size_t bitmap_size = BITS_TO_LONGS(pcpu_unit_pages) *
			     sizeof(unsigned long);

	/** 20140301    
	 * pages와 bitmap이 할당되지 않은 경우
	 **/
	if (!pages || !bitmap) {
		/** 20140301    
		 * pages가 할당되지 않은 경우 pages_size만큼 할당해준다.
		 * bitmap이 할당되지 않은 경우bitmap_size만큼 할당해준다.
		 **/
		if (may_alloc && !pages)
			pages = pcpu_mem_zalloc(pages_size);
		if (may_alloc && !bitmap)
			bitmap = pcpu_mem_zalloc(bitmap_size);
		/** 20140301    
		 * 실패했다면 NULL 리턴
		 **/
		if (!pages || !bitmap)
			return NULL;
	}

	/** 20140301    
	 * 현재 chunk의 populated 값을 bitmap에 복사.
	 **/
	bitmap_copy(bitmap, chunk->populated, pcpu_unit_pages);

	/** 20140301    
	 * 복사한 bitmap의 위치를 argument로 전달된 bitmapp에 저장
	 **/
	*bitmapp = bitmap;
	/** 20140301    
	 * 임시로 할당된 pages 주소를 리턴
	 **/
	return pages;
}

/**
 * pcpu_free_pages - free pages which were allocated for @chunk
 * @chunk: chunk pages were allocated for
 * @pages: array of pages to be freed, indexed by pcpu_page_idx()
 * @populated: populated bitmap
 * @page_start: page index of the first page to be freed
 * @page_end: page index of the last page to be freed + 1
 *
 * Free pages [@page_start and @page_end) in @pages for all units.
 * The pages were allocated for @chunk.
 */
static void pcpu_free_pages(struct pcpu_chunk *chunk,
			    struct page **pages, unsigned long *populated,
			    int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page = pages[pcpu_page_idx(cpu, i)];

			if (page)
				__free_page(page);
		}
	}
}

/**
 * pcpu_alloc_pages - allocates pages for @chunk
 * @chunk: target chunk
 * @pages: array to put the allocated pages into, indexed by pcpu_page_idx()
 * @populated: populated bitmap
 * @page_start: page index of the first page to be allocated
 * @page_end: page index of the last page to be allocated + 1
 *
 * Allocate pages [@page_start,@page_end) into @pages for all units.
 * The allocation is for @chunk.  Percpu core doesn't care about the
 * content of @pages and will pass it verbatim to pcpu_map_pages().
 */
/** 20140301    
 * cpu들을 순회하며
 * page_start ~ page_end 까지의 page 각각을 할당받아 pages의 해당 위치에 저장.
 **/
static int pcpu_alloc_pages(struct pcpu_chunk *chunk,
			    struct page **pages, unsigned long *populated,
			    int page_start, int page_end)
{
	/** 20140301    
	 * __GFP_COLD : cache-cold page로 할당한다.
	 **/
	const gfp_t gfp = GFP_KERNEL | __GFP_HIGHMEM | __GFP_COLD;
	unsigned int cpu;
	int i;

	/** 20140301    
	 * possible cpu들을 순회하며
	 **/
	for_each_possible_cpu(cpu) {
		/** 20140301    
		 * page_start ~ page_end 사이의 각 페이지들에 대해
		 **/
		for (i = page_start; i < page_end; i++) {
			/** 20140301    
			 * pages에서 cpu의 i번째 page에 대한 주소를 저장할 곳을 가져와
			 * alloc_pages_node로 할당한 page의 주소를 저장한다.
			 **/
			struct page **pagep = &pages[pcpu_page_idx(cpu, i)];

			/** 20140301    
			 * 특정 node에서 gfp 속성으로 page 하나를 할당 받아
			 * 특정 cpu의 특정 page 위치에 저장한다.
			 **/
			*pagep = alloc_pages_node(cpu_to_node(cpu), gfp, 0);
			/** 20140301    
			 * 실패했을 경우 앞서 할당 받은 메모리들을 해제한다.
			 **/
			if (!*pagep) {
				pcpu_free_pages(chunk, pages, populated,
						page_start, page_end);
				return -ENOMEM;
			}
		}
	}
	return 0;
}

/**
 * pcpu_pre_unmap_flush - flush cache prior to unmapping
 * @chunk: chunk the regions to be flushed belongs to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages in [@page_start,@page_end) of @chunk are about to be
 * unmapped.  Flush cache.  As each flushing trial can be very
 * expensive, issue flush on the whole region at once rather than
 * doing it for each cpu.  This could be an overkill but is more
 * scalable.
 */
static void pcpu_pre_unmap_flush(struct pcpu_chunk *chunk,
				 int page_start, int page_end)
{
	flush_cache_vunmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

static void __pcpu_unmap_pages(unsigned long addr, int nr_pages)
{
	unmap_kernel_range_noflush(addr, nr_pages << PAGE_SHIFT);
}

/**
 * pcpu_unmap_pages - unmap pages out of a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array which can be used to pass information to free
 * @populated: populated bitmap
 * @page_start: page index of the first page to unmap
 * @page_end: page index of the last page to unmap + 1
 *
 * For each cpu, unmap pages [@page_start,@page_end) out of @chunk.
 * Corresponding elements in @pages were cleared by the caller and can
 * be used to carry information to pcpu_free_pages() which will be
 * called after all unmaps are finished.  The caller should call
 * proper pre/post flush functions.
 */
static void pcpu_unmap_pages(struct pcpu_chunk *chunk,
			     struct page **pages, unsigned long *populated,
			     int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page;

			page = pcpu_chunk_page(chunk, cpu, i);
			WARN_ON(!page);
			pages[pcpu_page_idx(cpu, i)] = page;
		}
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				   page_end - page_start);
	}

	bitmap_clear(populated, page_start, page_end - page_start);
}

/**
 * pcpu_post_unmap_tlb_flush - flush TLB after unmapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been unmapped.  Flush
 * TLB for the regions.  This can be skipped if the area is to be
 * returned to vmalloc as vmalloc will handle TLB flushing lazily.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end)
{
	flush_tlb_kernel_range(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/** 20140301    
 * 할당받은 물리 메모리인 pages을 가상 주소 addr로 mapping 하는 함수
 **/
static int __pcpu_map_pages(unsigned long addr, struct page **pages,
			    int nr_pages)
{
	return map_kernel_range_noflush(addr, nr_pages << PAGE_SHIFT,
					PAGE_KERNEL, pages);
}

/**
 * pcpu_map_pages - map pages into a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array containing pages to be mapped
 * @populated: populated bitmap
 * @page_start: page index of the first page to map
 * @page_end: page index of the last page to map + 1
 *
 * For each cpu, map pages [@page_start,@page_end) into @chunk.  The
 * caller is responsible for calling pcpu_post_map_flush() after all
 * mappings are complete.
 *
 * This function is responsible for setting corresponding bits in
 * @chunk->populated bitmap and whatever is necessary for reverse
 * lookup (addr -> chunk).
 */
/** 20140308    
 * chunk 내의 pages들을 chunk에 해당하는 addresss에 mapping 시킨다.
 * 
 * 하위 함수의 vmap 관련 부분은 추후 분석하기로 함 ???
 **/
static int pcpu_map_pages(struct pcpu_chunk *chunk,
			  struct page **pages, unsigned long *populated,
			  int page_start, int page_end)
{
	unsigned int cpu, tcpu;
	int i, err;

	/** 20140301    
	 * 각 cpu를 순회하며 해당 cpu가 사용할 page들에 대해 virtual address를 매핑
	 **/
	for_each_possible_cpu(cpu) {
		/** 20140301    
		 * pcpu_chunk_addr(chunk, cpu, page_start)
		 *     chunk에서 각 cpu가 사용할 page의 시작 위치
		 *
		 * pcpu_page_idx(cpu, page_start)
		 *     cpu에 해당하는 page들 중 page_start의 page index를 받아옴
		 * &pages[pcpu_page_idx(cpu, page_start)]
		 *     cpu에 해당하는 pages의 시작위치
		 *
		 * page_end - page_start
		 *     mapping할 page 개수
		 **/
		err = __pcpu_map_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				       &pages[pcpu_page_idx(cpu, page_start)],
				       page_end - page_start);
		if (err < 0)
			goto err;
	}

	/* mapping successful, link chunk and mark populated */
	/** 20140308    
	 * mapping 후 각 struct page의 index에 chunk 주소를 기록한다.
	 * populated 에 page가 populate 되었음을 기록한다.
	 **/
	for (i = page_start; i < page_end; i++) {
		for_each_possible_cpu(cpu)
			pcpu_set_page_chunk(pages[pcpu_page_idx(cpu, i)],
					    chunk);
		/** 20140308    
		 * page가 alloc되어 address에 mapping까지 되어있는 경우 populate 되었다고 표시한다.
		 **/
		__set_bit(i, populated);
	}

	return 0;

err:
	for_each_possible_cpu(tcpu) {
		if (tcpu == cpu)
			break;
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, tcpu, page_start),
				   page_end - page_start);
	}
	return err;
}

/**
 * pcpu_post_map_flush - flush cache after mapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been mapped.  Flush
 * cache.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
/** 20140308    
 * percpu의 mapping 이후 수행하는 flush 작업
 **/
static void pcpu_post_map_flush(struct pcpu_chunk *chunk,
				int page_start, int page_end)
{
	/** 20140308    
	 * chunk 내에서
	 *	가장 낮은 cpu가 사용하는 페이지부터
	 *	가장 높은 cpu가 사용하는 페이지까지 flush 한다.
	 **/
	flush_cache_vmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/**
 * pcpu_populate_chunk - populate and map an area of a pcpu_chunk
 * @chunk: chunk of interest
 * @off: offset to the area to populate
 * @size: size of the area to populate in bytes
 *
 * For each cpu, populate and map pages [@page_start,@page_end) into
 * @chunk.  The area is cleared on return.
 *
 * CONTEXT:
 * pcpu_alloc_mutex, does GFP_KERNEL allocation.
 */
/** 20140308    
 * chunk에서 unpop 되어 있는 영역을 mapping시키고 populated 되었음을 표시한다.
 **/
static int pcpu_populate_chunk(struct pcpu_chunk *chunk, int off, int size)
{
	/** 20140301    
	 * off이 속하는 pfn을 가져온다.
	 **/
	int page_start = PFN_DOWN(off);
	/** 20140301    
	 * size를 더한 pfn을 가져온다.
	 **/
	int page_end = PFN_UP(off + size);
	int free_end = page_start, unmap_end = page_start;
	struct page **pages;
	unsigned long *populated;
	unsigned int cpu;
	int rs, re, rc;

	/* quick path, check whether all pages are already there */
	rs = page_start;
	/** 20140301    
	 * rs에서부터 첫번째 할당된 위치(pfn)가 rs에 저장,
	 * 첫번째 할당되지 않은 위치(pfn)이 re에 저장.
	 **/
	pcpu_next_pop(chunk, &rs, &re, page_end);
	/** 20140301    
	 * off이 위치하는 page_start와 page_end가
	 * next pop으로 가져온 pfn과 같다면 clear로 이동
	 *
	 * off에 해당하는 page의 처음부터 끝까지 모두 populate 되어 있다면 바로 clear로 간다.
	 **/
	if (rs == page_start && re == page_end)
		goto clear;

	/* need to allocate and map pages, this chunk can't be immutable */
	WARN_ON(chunk->immutable);

	/** 20140301    
	 * chunk의 unit에 해당하는 pages의 수만큼 공간을 할당하고 pages로 리턴.
	 * 현재 chunk의 populated 를 복사한 populated를 리턴.
	 **/
	pages = pcpu_get_pages_and_bitmap(chunk, &populated, true);
	if (!pages)
		return -ENOMEM;

	/* alloc and map */
	/** 20140301    
	 * page_start에서 page_end까지 unpop region을 순회하며 
	 *   pcpu_alloc_pages 로 page를 할당 받고,
	 **/
	pcpu_for_each_unpop_region(chunk, rs, re, page_start, page_end) {
		rc = pcpu_alloc_pages(chunk, pages, populated, rs, re);
		if (rc)
			goto err_free;
		free_end = re;
	}

	/** 20140308    
	 * page_start에서 page_end까지 unpop region을 순회하며
	 *   pcpu_map_pages 로 page를 mapping 한다.
	 **/
	pcpu_for_each_unpop_region(chunk, rs, re, page_start, page_end) {
		rc = pcpu_map_pages(chunk, pages, populated, rs, re);
		if (rc)
			goto err_unmap;
		unmap_end = re;
	}
	/** 20140308    
	 * vmap 이후 chunk의 데이터를 flush 한다.
	 **/
	pcpu_post_map_flush(chunk, page_start, page_end);

	/* commit new bitmap */
	/** 20140308    
	 * 새로 map한 populated 정보를 chunk로 복사
	 **/
	bitmap_copy(chunk->populated, populated, pcpu_unit_pages);
clear:
	/** 20140301    
	 * cpu들을 순회하며 각 cpu에 해당하는 page의 위치에서 off만큼 떨어진 위치부터
	 * size만큼 0으로 초기화.
	 * 즉, cpu가 사용할 영역을 0으로 클리어시킨다.
	 **/
	for_each_possible_cpu(cpu)
		memset((void *)pcpu_chunk_addr(chunk, cpu, 0) + off, 0, size);
	return 0;

err_unmap:
	pcpu_pre_unmap_flush(chunk, page_start, unmap_end);
	pcpu_for_each_unpop_region(chunk, rs, re, page_start, unmap_end)
		pcpu_unmap_pages(chunk, pages, populated, rs, re);
	pcpu_post_unmap_tlb_flush(chunk, page_start, unmap_end);
err_free:
	pcpu_for_each_unpop_region(chunk, rs, re, page_start, free_end)
		pcpu_free_pages(chunk, pages, populated, rs, re);
	return rc;
}

/**
 * pcpu_depopulate_chunk - depopulate and unmap an area of a pcpu_chunk
 * @chunk: chunk to depopulate
 * @off: offset to the area to depopulate
 * @size: size of the area to depopulate in bytes
 *
 * For each cpu, depopulate and unmap pages [@page_start,@page_end)
 * from @chunk.  If @flush is true, vcache is flushed before unmapping
 * and tlb after.
 *
 * CONTEXT:
 * pcpu_alloc_mutex.
 */
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk, int off, int size)
{
	int page_start = PFN_DOWN(off);
	int page_end = PFN_UP(off + size);
	struct page **pages;
	unsigned long *populated;
	int rs, re;

	/* quick path, check whether it's empty already */
	rs = page_start;
	pcpu_next_unpop(chunk, &rs, &re, page_end);
	if (rs == page_start && re == page_end)
		return;

	/* immutable chunks can't be depopulated */
	WARN_ON(chunk->immutable);

	/*
	 * If control reaches here, there must have been at least one
	 * successful population attempt so the temp pages array must
	 * be available now.
	 */
	pages = pcpu_get_pages_and_bitmap(chunk, &populated, false);
	BUG_ON(!pages);

	/* unmap and free */
	pcpu_pre_unmap_flush(chunk, page_start, page_end);

	pcpu_for_each_pop_region(chunk, rs, re, page_start, page_end)
		pcpu_unmap_pages(chunk, pages, populated, rs, re);

	/* no need to flush tlb, vmalloc will handle it lazily */

	pcpu_for_each_pop_region(chunk, rs, re, page_start, page_end)
		pcpu_free_pages(chunk, pages, populated, rs, re);

	/* commit new bitmap */
	bitmap_copy(chunk->populated, populated, pcpu_unit_pages);
}

static struct pcpu_chunk *pcpu_create_chunk(void)
{
	struct pcpu_chunk *chunk;
	struct vm_struct **vms;

	/** 20140322    
	 * struct pcpu_chunk를 할당받는다.
	 **/
	chunk = pcpu_alloc_chunk();
	if (!chunk)
		return NULL;

	vms = pcpu_get_vm_areas(pcpu_group_offsets, pcpu_group_sizes,
				pcpu_nr_groups, pcpu_atom_size);
	if (!vms) {
		pcpu_free_chunk(chunk);
		return NULL;
	}

	chunk->data = vms;
	chunk->base_addr = vms[0]->addr - pcpu_group_offsets[0];
	return chunk;
}

static void pcpu_destroy_chunk(struct pcpu_chunk *chunk)
{
	if (chunk && chunk->data)
		pcpu_free_vm_areas(chunk->data, pcpu_nr_groups);
	pcpu_free_chunk(chunk);
}

static struct page *pcpu_addr_to_page(void *addr)
{
	return vmalloc_to_page(addr);
}

static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai)
{
	/* no extra restriction */
	return 0;
}
