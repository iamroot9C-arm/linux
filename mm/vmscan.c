/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/prefetch.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

struct scan_control {
	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	unsigned long hibernation_mode;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/** 20140524    
	 * reclaim 중 page가 writeout될 수 있는지 여부
	 **/
	int may_writepage;

	/* Can mapped pages be reclaimed? */
	/** 20140524    
	 * mapping 된 pages이 reclaim될 수 있는지 여부
	 **/
	int may_unmap;

	/* Can pages be swapped as part of reclaim? */
	int may_swap;

	int order;

	/* Scan (total_size >> priority) pages at once */
	int priority;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;
};

/** 20140111
 * lru 리스트의 맨 마지막 (가장 오래된) 페이지를 리턴
 **/
#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#ifdef ARCH_HAS_PREFETCHW
/** 20140111
 * lruvec이 가리키는 lru리스트의 이전 페이지의 필드를 prefetch한다.
 **/
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 100.  Higher means more swappy.
 */
/** 20131214    
 * 숫자가 높을수록 가능하면 swap을 사용한다.
 * [참고] http://ubuntu.or.kr/wiki/doku.php?id=목차:vm.swap_조정하기
 **/
int vm_swappiness = 60;
long vm_total_pages;	/* The total number of pages which the VM controls */

/** 20140517    
 * shrinker_list
 **/
static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_MEMCG
static bool global_reclaim(struct scan_control *sc)
{
	return !sc->target_mem_cgroup;
}
#else
/** 20131214    
 * 항상 true
 **/
static bool global_reclaim(struct scan_control *sc)
{
	return true;
}
#endif

/** 20131214    
 * zone의 stat 정보에서 lru에 해당하는 통계값을 리턴.
 **/
static unsigned long get_lru_size(struct lruvec *lruvec, enum lru_list lru)
{
	/** 20131214    
	 * mem_cgroup_disabled은 항상 true이므로 if문은 false
	 **/
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, lru);

	/** 20131214    
	 * lruvec을 포함하고 있는 zone의 vm stat에서 lru에 해당하는 값을 리턴.
	 **/
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru);
}

/*
 * Add a shrinker callback to be called from the vm
 */
/** 20140517    
 * shrinker를 전역변수 shrinker_list에 등록.
 **/
void register_shrinker(struct shrinker *shrinker)
{
	/** 20140517    
	 * shrinker의 nr_in_batch를 0으로 초기화.
	 * shrinker_list는 전역변수이므로 rw semaphore로 보호된 상태에서 저장.
	 **/
	atomic_long_set(&shrinker->nr_in_batch, 0);
	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	up_write(&shrinker_rwsem);
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	up_write(&shrinker_rwsem);
}
EXPORT_SYMBOL(unregister_shrinker);

/** 20140517    
 * shrinker callback의 shrink 동작 수행.
 **/
static inline int do_shrinker_shrink(struct shrinker *shrinker,
				     struct shrink_control *sc,
				     unsigned long nr_to_scan)
{
	/** 20140517    
	 * scan할 shrink control에 저장해 shrink 호출.
	 **/
	sc->nr_to_scan = nr_to_scan;
	return (*shrinker->shrink)(shrinker, sc);
}

#define SHRINK_BATCH 128
/*
 * Call the shrink functions to age shrinkable caches
 *
 * Here we assume it costs one seek to replace a lru page and that it also
 * takes a seek to recreate a cache object.  With this in mind we age equal
 * percentages of the lru and ageable caches.  This should balance the seeks
 * generated by these structures.
 *
 * If the vm encountered mapped pages on the LRU it increase the pressure on
 * slab to avoid swapping.
 *
 * We do weird things to avoid (scanned*seeks*entries) overflowing 32 bits.
 *
 * `lru_pages' represents the number of on-LRU pages in all the zones which
 * are eligible for the caller's allocation attempt.  It is used for balancing
 * slab reclaim versus page reclaim.
 *
 * Returns the number of slab objects which we shrunk.
 */
/** 20140517    
 * shrinker_list에 등록된 shrinker로 shrink하고, shrink 한 페이지 수를 리턴한다.
 **/
unsigned long shrink_slab(struct shrink_control *shrink,
			  unsigned long nr_pages_scanned,
			  unsigned long lru_pages)
{
	struct shrinker *shrinker;
	unsigned long ret = 0;

	if (nr_pages_scanned == 0)
		nr_pages_scanned = SWAP_CLUSTER_MAX;

	if (!down_read_trylock(&shrinker_rwsem)) {
		/* Assume we'll be able to shrink next time */
		ret = 1;
		goto out;
	}

	/** 20140517    
	 * shrinker_list에 등록된 각 shrinker에 대해 아래 동작 수행
	 **/
	list_for_each_entry(shrinker, &shrinker_list, list) {
		unsigned long long delta;
		long total_scan;
		long max_pass;
		int shrink_ret = 0;
		long nr;
		long new_nr;
		/** 20140517    
		 * shrinker가 batch 단위를 갖고 있다면 그것을 사용,
		 * 그렇지 않다면 default로 SHRINK_BATCH 값을 사용.
		 **/
		long batch_size = shrinker->batch ? shrinker->batch
						  : SHRINK_BATCH;

		/** 20140517    
		 * shrinker의 shrink 동작 수행. 
		 * nr_to_scan에 0을 주었으므로 cache size에 대한 query.
		 *   struct shrinker 선언부의 주석 참고.
		 **/
		max_pass = do_shrinker_shrink(shrinker, shrink, 0);
		if (max_pass <= 0)
			continue;

		/*
		 * copy the current shrinker scan count into a local variable
		 * and zero it so that other concurrent shrinker invocations
		 * don't also do this scanning work.
		 */
		/** 20140517    
		 * shrinker의 nr_in_batch를 0으로 설정하고 이전 값을 리턴.
		 **/
		nr = atomic_long_xchg(&shrinker->nr_in_batch, 0);

		/** 20140517    
		 * nr_in_batch에 scan 요구된 수를 기준으로 delta을 계산해 반영시킨다.
		 **/
		total_scan = nr;
		delta = (4 * nr_pages_scanned) / shrinker->seeks;
		delta *= max_pass;
		do_div(delta, lru_pages + 1);
		total_scan += delta;
		if (total_scan < 0) {
			printk(KERN_ERR "shrink_slab: %pF negative objects to "
			       "delete nr=%ld\n",
			       shrinker->shrink, total_scan);
			total_scan = max_pass;
		}

		/*
		 * We need to avoid excessive windup on filesystem shrinkers
		 * due to large numbers of GFP_NOFS allocations causing the
		 * shrinkers to return -1 all the time. This results in a large
		 * nr being built up so when a shrink that can do some work
		 * comes along it empties the entire cache due to nr >>>
		 * max_pass.  This is bad for sustaining a working set in
		 * memory.
		 *
		 * Hence only allow the shrinker to scan the entire cache when
		 * a large delta change is calculated directly.
		 */
		/** 20140517    
		 * delta가 max_pass의 1/4보다 작을 경우 
		 * total_scan을 최대 max_pass의 1/2로 한다.
		 **/
		if (delta < max_pass / 4)
			total_scan = min(total_scan, max_pass / 2);

		/*
		 * Avoid risking looping forever due to too large nr value:
		 * never try to free more than twice the estimate number of
		 * freeable entries.
		 */
		/** 20140517    
		 * total_scan을 최대 max_pass의 2배로 한다.
		 **/
		if (total_scan > max_pass * 2)
			total_scan = max_pass * 2;

		trace_mm_shrink_slab_start(shrinker, shrink, nr,
					nr_pages_scanned, lru_pages,
					max_pass, delta, total_scan);

		/** 20140517    
		 * total_scan을 batch_size 단위로 수행
		 **/
		while (total_scan >= batch_size) {
			int nr_before;

			/** 20140517    
			 * shrink 전 shrinker의 cache size를 조회.
			 * shrinker의 shrink 동작 수행 후 cache size를 저장
			 **/
			nr_before = do_shrinker_shrink(shrinker, shrink, 0);
			shrink_ret = do_shrinker_shrink(shrinker, shrink,
							batch_size);
			if (shrink_ret == -1)
				break;
			/** 20140517    
			 * shrink한 숫자를 ret에 누적.
			 **/
			if (shrink_ret < nr_before)
				ret += nr_before - shrink_ret;
			/** 20140517    
			 * SLABS_SCANNED에 반영
			 **/
			count_vm_events(SLABS_SCANNED, batch_size);
			total_scan -= batch_size;

			cond_resched();
		}

		/*
		 * move the unused scan count back into the shrinker in a
		 * manner that handles concurrent updates. If we exhausted the
		 * scan, there is no need to do an update.
		 */
		/** 20140517    
		 * total_scan이 batch_size로 나뉘어 떨어지지 않아 나머지가 생긴 경우
		 *   남은 total_scan에 nr_in_batch를 더해 리턴
		 * 나머지가 없는 경우
		 *   nr_in_batch를 리턴
		 **/
		if (total_scan > 0)
			new_nr = atomic_long_add_return(total_scan,
					&shrinker->nr_in_batch);
		else
			new_nr = atomic_long_read(&shrinker->nr_in_batch);

		trace_mm_shrink_slab_end(shrinker, shrink_ret, nr, new_nr);
	}
	up_read(&shrinker_rwsem);
out:
	cond_resched();
	return ret;
}

static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache radix tree and
	 * optional buffer heads at page->private.
	 */
	return page_count(page) - page_has_private(page) == 2;
}

static int may_write_to_queue(struct backing_dev_info *bdi,
			      struct scan_control *sc)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!bdi_write_congested(bdi))
		return 1;
	if (bdi == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */
static pageout_t pageout(struct page *page, struct address_space *mapping,
			 struct scan_control *sc)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in __generic_file_aio_write() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				printk("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_queue(mapping->backing_dev_info, sc))
		return PAGE_KEEP;

	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		trace_mm_vmscan_writepage(page, trace_reclaim_flags(page));
		inc_zone_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page)
{
	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	spin_lock_irq(&mapping->tree_lock);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_count.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under tree_lock, then this ordering is not required.
	 */
	if (!page_freeze_refs(page, 2))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_freeze_refs provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_unfreeze_refs(page, 2);
		goto cannot_free;
	}

	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };
		__delete_from_swap_cache(page);
		spin_unlock_irq(&mapping->tree_lock);
		swapcache_free(swap, page);
	} else {
		void (*freepage)(struct page *);

		freepage = mapping->a_ops->freepage;

		__delete_from_page_cache(page);
		spin_unlock_irq(&mapping->tree_lock);
		mem_cgroup_uncharge_cache_page(page);

		if (freepage != NULL)
			freepage(page);
	}

	return 1;

cannot_free:
	spin_unlock_irq(&mapping->tree_lock);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_unfreeze_refs(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */

/** 20140111
 * page가 evictable할 경우 percpu의 lru리스트에 page를 다시 등록시키고
 * unevictable할 경우 zone의 lru리스트에 page를 등록시킨다.
 **/
void putback_lru_page(struct page *page)
{
	int lru;
	int active = !!TestClearPageActive(page);
	int was_unevictable = PageUnevictable(page);

	VM_BUG_ON(PageLRU(page));

redo:
	ClearPageUnevictable(page);

	/*
	enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};
*/
	/** 20140111
	 * lru_list가 LRU_INACTIVE_ANON, LRU_ACTIVE_ANON, 
	 * LRU_INACTIVE_FILE, LRU_ACTIVE_FILE 일 경우 실행
	 **/
	if (page_evictable(page, NULL)) {
		/*
		 * For evictable pages, we can use the cache.
		 * In event of a race, worst case is we end up with an
		 * unevictable page on [in]active list.
		 * We know how to handle that.
		 */
		lru = active + page_lru_base_type(page);
		/** 20140111
		 * percpu의 lru리스트에 page를 등록시킨다.
		 **/
		lru_cache_add_lru(page, lru);
	/** 20140111
	 * lru_list가 LRU_UNEVICTABLE일 경우 실행
	 **/
	} else {
		/*
		 * Put unevictable pages directly on zone's unevictable
		 * list.
		 */
		lru = LRU_UNEVICTABLE;
		add_page_to_unevictable_list(page);
		/*
		 * When racing with an mlock or AS_UNEVICTABLE clearing
		 * (page is unlocked) make sure that if the other thread
		 * does not observe our setting of PG_lru and fails
		 * isolation/check_move_unevictable_pages,
		 * we see PG_mlocked/AS_UNEVICTABLE cleared below and move
		 * the page back to the evictable list.
		 *
		 * The other side is TestClearPageMlocked() or shmem_lock().
		 */
		smp_mb();
	}

	/*
	 * page's status can change while we move it among lru. If an evictable
	 * page is on unevictable list, it never be freed. To avoid that,
	 * check after we added it to the list, again.
	 */
	if (lru == LRU_UNEVICTABLE && page_evictable(page, NULL)) {
		if (!isolate_lru_page(page)) {
			put_page(page);
			goto redo;
		}
		/* This means someone else dropped this page from LRU
		 * So, it will be freed or putback to LRU again. There is
		 * nothing to do here.
		 */
	}

	if (was_unevictable && lru != LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGRESCUED);
	else if (!was_unevictable && lru == LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGCULLED);

	put_page(page);		/* drop ref from isolate */
}

enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

static enum page_references page_check_references(struct page *page,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	/** 20140524    
	 * page의 referenced된 수를 가져온다.
	 * vm_flags에는 referenced된 page의 상태가 채워진다.
	 * (page table에 등록되어 있고, rmapping을 가지고 있는 경우)
	 **/
	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	/** 20140524    
	 * page의 referenced flag 상태를 가져오고, clear 시킨다.
	 **/
	referenced_page = TestClearPageReferenced(page);

	/*
	 * Mlock lost the isolation race with us.  Let try_to_unmap()
	 * move the page to the unevictable list.
	 */
	/** 20140524    
	 * VM_LOCKED된 페이지인 경우 PAGEREF_RECLAIM 상태를 리턴.
	 **/
	if (vm_flags & VM_LOCKED)
		return PAGEREF_RECLAIM;

	/** 20140524    
	 * page table에 등록된 페이지이며 rmapping인 페이지라면
	 **/
	if (referenced_ptes) {
		/** 20140524    
		 * swap을 가지고 있는 경우 PAGEREF_ACTIVATE로 리턴
		 **/
		if (PageSwapBacked(page))
			return PAGEREF_ACTIVATE;
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		/** 20140524    
		 * pte에 등록된 page이며 swap backed가 아닌 경우,
		 * 다시 referenced된 상태로 설정한다.
		 **/
		SetPageReferenced(page);

		/** 20140524    
		 * swapbacked가 아닌 page.
		 * reference된 page이거나 ptes에 2이상 reference되어 있다면
		 * PAGEREF_ACTIVATE를 리턴.
		 **/
		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		/** 20140524    
		 * file-backed인 실행 가능한 페이지인 경우 PAGEREF_ACTIVATE 리턴
		 **/
		if (vm_flags & VM_EXEC)
			return PAGEREF_ACTIVATE;

		/** 20140524    
		 * 그 외의 referenced_ptes는 KEEP 한다.
		 **/
		return PAGEREF_KEEP;
	}

	/** 20140524    
	 * referenced_ptes 가 아닌 페이지 중
	 * referenced 된 페이지이면서 swapbacked가 아닌 페이지인 경우
	 * PAGEREF_RECLAIM_CLEAN 리턴
	 **/
	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
static unsigned long shrink_page_list(struct list_head *page_list,
				      struct zone *zone,
				      struct scan_control *sc,
				      unsigned long *ret_nr_dirty,
				      unsigned long *ret_nr_writeback)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	int pgactivate = 0;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_writeback = 0;

	cond_resched();

	mem_cgroup_uncharge_start();
	/** 20140524    
	 * page_list가 채워져 있는동안 수행한다.
	 **/
	while (!list_empty(page_list)) {
		enum page_references references;
		struct address_space *mapping;
		struct page *page;
		int may_enter_fs;

		cond_resched();

		/** 20140524    
		 * page_list의 page를 오래된 것부터 가져와 list에서 제거한다.
		 **/
		page = lru_to_page(page_list);
		list_del(&page->lru);

		/** 20140524    
		 * lock을 걸지 못한 경우에는 keep으로 이동
		 **/
		if (!trylock_page(page))
			goto keep;

		VM_BUG_ON(PageActive(page));
		VM_BUG_ON(page_zone(page) != zone);

		sc->nr_scanned++;

		/** 20140524    
		 * page가 evictable 하지 않다면 cull_mlocked로 이동시킨다.
		 **/
		if (unlikely(!page_evictable(page, NULL)))
			goto cull_mlocked;

		/** 20140524    
		 * reclaim 중 unmap이 가능하지 않도록 설정되었는데 page가 mapping 중이라면 keep_locked로 이동
		 **/
		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		/* Double the slab pressure for mapped and swapcache pages */
		if (page_mapped(page) || PageSwapCache(page))
			sc->nr_scanned++;

		/** 20140524    
		 * gfp에서 __GFP_FS가 설정되어 있거나
		 * page가 swapcache이며 GFP_IO가 가능할 경우, fs 함수로 진입이 가능하다.
		 **/
		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		/** 20140524    
		 * page가 writeback이라면 
		 **/
		if (PageWriteback(page)) {
			/*
			 * memcg doesn't have any dirty pages throttling so we
			 * could easily OOM just because too many pages are in
			 * writeback and there is nothing else to reclaim.
			 *
			 * Check __GFP_IO, certainly because a loop driver
			 * thread might enter reclaim, and deadlock if it waits
			 * on a page for which it is needed to do the write
			 * (loop masks off __GFP_IO|__GFP_FS for this reason);
			 * but more thought would probably show more reasons.
			 *
			 * Don't require __GFP_FS, since we're not going into
			 * the FS, just waiting on its writeback completion.
			 * Worryingly, ext4 gfs2 and xfs allocate pages with
			 * grab_cache_page_write_begin(,,AOP_FLAG_NOFS), so
			 * testing may_enter_fs here is liable to OOM on them.
			 */
			/** 20140524    
			 * global reclaim이거나 page에 reclaim이 설정되지 않았거나,
			 * IO가 허용되지 않은 경우 writeback 완료를 대기하지 않는다.
			 **/
			if (global_reclaim(sc) ||
			    !PageReclaim(page) || !(sc->gfp_mask & __GFP_IO)) {
				/*
				 * This is slightly racy - end_page_writeback()
				 * might have just cleared PageReclaim, then
				 * setting PageReclaim here end up interpreted
				 * as PageReadahead - but that does not matter
				 * enough to care.  What we do want is for this
				 * page to have PageReclaim set next time memcg
				 * reclaim reaches the tests above, so it will
				 * then wait_on_page_writeback() to avoid OOM;
				 * and it's also appropriate in global reclaim.
				 */
				/** 20140524    
				 * page가 writeback이므로 reclaim 대상으로 write를 대기 중임을 설정한다.
				 **/
				SetPageReclaim(page);
				nr_writeback++;
				goto keep_locked;
			}
			/** 20140524    
			 * writeback 완료를 대기한다.
			 **/
			wait_on_page_writeback(page);
		}

		/** 20140524    
		 * page의 reference type을 조회한다.
		 **/
		references = page_check_references(page, sc);
		switch (references) {
		/** 20140524    
		 * PAGEREF_ACTIVATE : reclaim을 하지 않고, activate 만 시킨다.
		 * PAGEREF_KEEP     : 
		 **/
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			goto keep_locked;
		/** 20140524    
		 * reclaim을 진행한다.
		 **/
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the page below */
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 */
		/** 20140524    
		 * page가 anon이고, swapcache로 사용 중이지 않은 경우
		 *   IO가 허용되지 않는다면 keep_locked로 간다.
		 * page를 위한 swap 공간을 할당해 swap cache에 추가한다.
		 * 실패한다면 activate_locked로 간다.
		 * 성공한다면 fs으로 접근할 가능성이 있음을 표시한다.
		 **/
		if (PageAnon(page) && !PageSwapCache(page)) {
			if (!(sc->gfp_mask & __GFP_IO))
				goto keep_locked;
			/** 20140524    
			 * page를 위한 swap space를 할당한다.
			 **/
			if (!add_to_swap(page))
				goto activate_locked;
			may_enter_fs = 1;
		}

		/** 20140524    
		 * page의 mapping 정보를 가져온다.
		 **/
		mapping = page_mapping(page);

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		/** 20140524    
		 * page가 page table에 mapping 되어 있고, address_space가 존재하면
		 **/
		if (page_mapped(page) && mapping) {
			switch (try_to_unmap(page, TTU_UNMAP)) {
			case SWAP_FAIL:
				goto activate_locked;
			case SWAP_AGAIN:
				goto keep_locked;
			case SWAP_MLOCK:
				goto cull_mlocked;
			case SWAP_SUCCESS:
				; /* try to free the page below */
			}
		}

		if (PageDirty(page)) {
			nr_dirty++;

			/*
			 * Only kswapd can writeback filesystem pages to
			 * avoid risk of stack overflow but do not writeback
			 * unless under significant pressure.
			 */
			if (page_is_file_cache(page) &&
					(!current_is_kswapd() ||
					 sc->priority >= DEF_PRIORITY - 2)) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to deactivate_page()
				 * except we already have the page isolated
				 * and know it's dirty
				 */
				inc_zone_page_state(page, NR_VMSCAN_IMMEDIATE);
				SetPageReclaim(page);

				goto keep_locked;
			}

			if (references == PAGEREF_RECLAIM_CLEAN)
				goto keep_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/* Page is dirty, try to write it out here */
			switch (pageout(page, mapping, sc)) {
			case PAGE_KEEP:
				nr_congested++;
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				if (PageWriteback(page))
					goto keep;
				if (PageDirty(page))
					goto keep;

				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_complete_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed++;
					continue;
				}
			}
		}

		if (!mapping || !__remove_mapping(mapping, page))
			goto keep_locked;

		/*
		 * At this point, we have no other references and there is
		 * no way to pick any more up (removed from LRU, removed
		 * from pagecache). Can use non-atomic bitops now (and
		 * we obviously don't have to worry about waking up a process
		 * waiting on the page lock, because there are no references.
		 */
		__clear_page_locked(page);
free_it:
		nr_reclaimed++;

		/*
		 * Is there need to periodically free_page_list? It would
		 * appear not as the counts should be low
		 */
		list_add(&page->lru, &free_pages);
		continue;

cull_mlocked:
		if (PageSwapCache(page))
			try_to_free_swap(page);
		unlock_page(page);
		putback_lru_page(page);
		continue;

activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		/** 20140524    
		 * page가 swapcache로 사용되는 경우면서 
		 **/
		if (PageSwapCache(page) && vm_swap_full())
			try_to_free_swap(page);
		VM_BUG_ON(PageActive(page));
		SetPageActive(page);
		pgactivate++;
keep_locked:
		unlock_page(page);
keep:
		/** 20140524    
		 * ret_pages에 page를 등록한다.
		 **/
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON(PageLRU(page) || PageUnevictable(page));
	}

	/*
	 * Tag a zone as congested if all the dirty pages encountered were
	 * backed by a congested BDI. In this case, reclaimers should just
	 * back off and wait for congestion to clear because further reclaim
	 * will encounter the same problem
	 */
	if (nr_dirty && nr_dirty == nr_congested && global_reclaim(sc))
		zone_set_flag(zone, ZONE_CONGESTED);

	free_hot_cold_page_list(&free_pages, 1);

	list_splice(&ret_pages, page_list);
	count_vm_events(PGACTIVATE, pgactivate);
	mem_cgroup_uncharge_end();
	*ret_nr_dirty += nr_dirty;
	*ret_nr_writeback += nr_writeback;
	return nr_reclaimed;
}

/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns 0 on success, -ve errno on failure.
 */
/** 20140111
 * lru리스트에서 page를 제거한다.
 **/

int __isolate_lru_page(struct page *page, isolate_mode_t mode)
{
	int ret = -EINVAL;

	/* Only take pages on the LRU. */
	/** 20140111
	 * page가 lru리스트에 속해있지 않으면 에러를 리턴
	 **/
	if (!PageLRU(page))
		return ret;
	/* Do not give back unevictable pages for compaction */
	/** 20140111
	 * page가 메모리에 고정되어 있으면(unevictable) 에러를 리턴
	 **/
	if (PageUnevictable(page))
		return ret;

	ret = -EBUSY;

	/*
	 * To minimise LRU disruption, the caller can indicate that it only
	 * wants to isolate pages it will be able to operate on without
	 * blocking - clean pages for the most part.
	 *
	 * ISOLATE_CLEAN means that only clean pages should be isolated. This
	 * is used by reclaim when it is cannot write to backing storage
	 *
	 * ISOLATE_ASYNC_MIGRATE is used to indicatedd that it only wants to pages
	 * that it is possible to migrate without blocking
	 */
	/** 20140111
	 * ISOLATE_CLEAN 및 ISOLATE_ASYNC_MIGRATE 모드에 따라 에러를 리턴한다
	**/
	if (mode & (ISOLATE_CLEAN|ISOLATE_ASYNC_MIGRATE)) {
		/* All the caller can do on PageWriteback is block */
		if (PageWriteback(page))
			return ret;

		if (PageDirty(page)) {
			struct address_space *mapping;

			/* ISOLATE_CLEAN means only clean pages */
			if (mode & ISOLATE_CLEAN)
				return ret;

			/*
			 * Only pages without mappings or that have a
			 * ->migratepage callback are possible to migrate
			 * without blocking
			 */
			/** 20140111
			 * page가 매핑되어 있고  migratepage 콜백 함수가 지정
			 * 되지 않은 경우 에러를 리턴한다.
			 **/
			mapping = page_mapping(page);
			if (mapping && !mapping->a_ops->migratepage)
				return ret;
		}
	}

	/** 20140111
	 * ISOLATE_UNMAPPED가 지정되어 있을 경우 page가 맵핑되어 있으면 
	 * 에러를 리턴한다.
	 **/
	if ((mode & ISOLATE_UNMAPPED) && page_mapped(page))
		return ret;

	/** 20140111
	 * page가 사용중인 경우 page를 가져오고 lru플래그를 클리어한다.
	 * 0을 리턴한다.
	 **/

	if (likely(get_page_unless_zero(page))) {
		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
		ClearPageLRU(page);
		ret = 0;
	}

	return ret;
}

/*
 * zone->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Appropriate locks must be held before calling this function.
 *
 * @nr_to_scan:	The number of pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @mode:	One of the LRU isolation modes
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
/** 20140111
 * nr_to_scan개의 page중 실제 스캔한 페이지의 갯수를  nr_scanned에 저장한다.
 * lru list로부터 isolate시켜 dst에 등록하고,
 * isolate 한 page의 수(nr_taken)를 리턴한다.
 **/
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long scan;

	/** 20140524    
	 * 최대 nr_to_scan만큼의 page를 스캔한다.
	 * 그 전에 src list가 빌 경우 scan은 중단된다.
	 **/
	for (scan = 0; scan < nr_to_scan && !list_empty(src); scan++) {
		struct page *page;
		int nr_pages;
		/** 20140111
		 * src 리스트로부터 맨 마지막 페이지를 page에 저장
		 **/
		page = lru_to_page(src);
		/** 2014011
		 * lru리스트의 이전페이지의 flags를 prefetch한다.
		 **/
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON(!PageLRU(page));

		switch (__isolate_lru_page(page, mode)) {
		/** 20140111
		 * page가 isolate된 경우 lru리스트에서 dst으로 옮기고 옮긴 
		 * page의 갯수를 nr_taken에 누적시킨다.
		 **/
		case 0:
			nr_pages = hpage_nr_pages(page);
			mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
			list_move(&page->lru, dst);
			nr_taken += nr_pages;
			break;
		/** 20140111
		 * page가 isolate되지 않은 경우 lru리스트의 앞으로 옮겨준다.
		 **/
		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			continue;

		default:
			BUG();
		}
	}

	*nr_scanned = scan;
	trace_mm_vmscan_lru_isolate(sc->order, nr_to_scan, scan,
				    nr_taken, mode, is_file_lru(lru));
	return nr_taken;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamentnal difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	VM_BUG_ON(!page_count(page));

	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);
		struct lruvec *lruvec;

		spin_lock_irq(&zone->lru_lock);
		lruvec = mem_cgroup_page_lruvec(page, zone);
		if (PageLRU(page)) {
			int lru = page_lru(page);
			get_page(page);
			ClearPageLRU(page);
			del_page_from_lru_list(page, lruvec, lru);
			ret = 0;
		}
		spin_unlock_irq(&zone->lru_lock);
	}
	return ret;
}

/*
 * Are there way too many processes in the direct reclaim path already?
 */
static int too_many_isolated(struct zone *zone, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	/** 20140524    
	 * direct reclaim인 경우에만 0 리턴.
	 **/
	if (current_is_kswapd())
		return 0;

	/** 20140524    
	 * global reclaim이 아닌 경우 0 리턴.
	 **/
	if (!global_reclaim(sc))
		return 0;

	/** 20140524    
	 * file일 때와 아닐 때를 구분히 inactive, isolated state를 조회한다.
	 **/
	if (file) {
		inactive = zone_page_state(zone, NR_INACTIVE_FILE);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	return isolated > inactive;
}

static noinline_for_stack void
putback_inactive_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	struct zone *zone = lruvec_zone(lruvec);
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page(page_list);
		int lru;

		VM_BUG_ON(PageLRU(page));
		list_del(&page->lru);
		if (unlikely(!page_evictable(page, NULL))) {
			spin_unlock_irq(&zone->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&zone->lru_lock);
			continue;
		}

		lruvec = mem_cgroup_page_lruvec(page, zone);

		SetPageLRU(page);
		lru = page_lru(page);
		add_page_to_lru_list(page, lruvec, lru);

		if (is_active_lru(lru)) {
			int file = is_file_lru(lru);
			int numpages = hpage_nr_pages(page);
			reclaim_stat->recent_rotated[file] += numpages;
		}
		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);
		}
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, page_list);
}

/*
 * shrink_inactive_list() is a helper for shrink_zone().  It returns the number
 * of reclaimed pages
 */
static noinline_for_stack unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_writeback = 0;
	isolate_mode_t isolate_mode = 0;
	/** 20140524    
	 * lru가 file 속성인지 여부를 저장한다.
	 **/
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

	/** 20140524    
	 * isolated가 inactive보다 많다면 
	 * BLK_RW_ASYNC에 대해 1/10초만큼 기다린다.
	 **/
	while (unlikely(too_many_isolated(zone, file, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		/** 20140524    
		 * 현재 task가 SIGKIL을 받았다면 SWAP_CLUSTER_MAX를 리턴한다.
		 * (nr_to_scan의 최대치)
		 **/
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	/** 20140524    
	 * cpu의 lru list에 있던 page들을 zone으로 옮긴다.
	 **/
	lru_add_drain();

	/** 20140524    
	 * scan control에 따라 isolate mode를 설정한다.
	 *
	 * may_unmap : mapping 된 pages이 reclaim될 수 있는지 여부
	 * may_writepage : reclaim 중 page가 writeout될 수 있는지 여부
	 **/
	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);

	/** 20140524    
	 * lruvec의 lru list에서 nr_to_scan개만큼을 scan하고, isolate한 page들을
	 * page_list에 등록한다. scan 한 개수는 nr_scanned, nr_taken은 실제 옮긴 개수.
	 **/
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, isolate_mode, lru);

	/** 20140524    
	 * isolate한 수만큼 lru의 수를 감소시키고, isolated의 수를 증가시킨다.
	 **/
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);

	/** 20140524    
	 * scan결과를 vm events에 반영.
	 **/
	if (global_reclaim(sc)) {
		zone->pages_scanned += nr_scanned;
		if (current_is_kswapd())
			__count_zone_vm_events(PGSCAN_KSWAPD, zone, nr_scanned);
		else
			__count_zone_vm_events(PGSCAN_DIRECT, zone, nr_scanned);
	}
	spin_unlock_irq(&zone->lru_lock);

	/** 20140524    
	 * isolate된 페이지가 없다면 여기서 리턴.
	 **/
	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_page_list(&page_list, zone, sc,
						&nr_dirty, &nr_writeback);

	spin_lock_irq(&zone->lru_lock);

	reclaim_stat->recent_scanned[file] += nr_taken;

	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_zone_vm_events(PGSTEAL_KSWAPD, zone,
					       nr_reclaimed);
		else
			__count_zone_vm_events(PGSTEAL_DIRECT, zone,
					       nr_reclaimed);
	}

	putback_inactive_pages(lruvec, &page_list);

	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);

	spin_unlock_irq(&zone->lru_lock);

	free_hot_cold_page_list(&page_list, 1);

	/*
	 * If reclaim is isolating dirty pages under writeback, it implies
	 * that the long-lived page allocation rate is exceeding the page
	 * laundering rate. Either the global limits are not being effective
	 * at throttling processes due to the page distribution throughout
	 * zones or there is heavy usage of a slow backing device. The
	 * only option is to throttle from reclaim context which is not ideal
	 * as there is no guarantee the dirtying process is throttled in the
	 * same way balance_dirty_pages() manages.
	 *
	 * This scales the number of dirty pages that must be under writeback
	 * before throttling depending on priority. It is a simple backoff
	 * function that has the most effect in the range DEF_PRIORITY to
	 * DEF_PRIORITY-2 which is the priority reclaim is considered to be
	 * in trouble and reclaim is considered to be in trouble.
	 *
	 * DEF_PRIORITY   100% isolated pages must be PageWriteback to throttle
	 * DEF_PRIORITY-1  50% must be PageWriteback
	 * DEF_PRIORITY-2  25% must be PageWriteback, kswapd in trouble
	 * ...
	 * DEF_PRIORITY-6 For SWAP_CLUSTER_MAX isolated pages, throttle if any
	 *                     isolated page is PageWriteback
	 */
	if (nr_writeback && nr_writeback >=
			(nr_taken >> (DEF_PRIORITY - sc->priority)))
		wait_iff_congested(zone, BLK_RW_ASYNC, HZ/10);

	trace_mm_vmscan_lru_shrink_inactive(zone->zone_pgdat->node_id,
		zone_idx(zone),
		nr_scanned, nr_reclaimed,
		sc->priority,
		trace_shrink_flags(file));
	return nr_reclaimed;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone->lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone->lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_count against each page.
 * But we had to alter page->flags anyway.
 */

/** 20140524    
 * list에 등록된 active page를 zone의 lru list에 등록시킨다.
 * 만약 page의 reference가 0이 되다면 pages_to_free 리스트에 등록시킨다.
 **/
static void move_active_pages_to_lru(struct lruvec *lruvec,
				     struct list_head *list,
				     struct list_head *pages_to_free,
				     enum lru_list lru)
{
	/** 20140524    
	 * 현재 lruvec이 속해 있는 zone을 가져온다.
	 **/
	struct zone *zone = lruvec_zone(lruvec);
	unsigned long pgmoved = 0;
	struct page *page;
	int nr_pages;

	/** 20140524    
	 * 주어진 list가 빌 때까지 아래 작업을 수행
	 **/
	while (!list_empty(list)) {
		/** 20140524    
		 * 제공된 list에서 가장 오래된 page를 가져온다.
		 **/
		page = lru_to_page(list);
		lruvec = mem_cgroup_page_lruvec(page, zone);

		VM_BUG_ON(PageLRU(page));
		/** 20140524    
		 * page를 LRU 리스트에 추가한다고 표시한다.
		 **/
		SetPageLRU(page);

		nr_pages = hpage_nr_pages(page);
		mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
		/** 20140524    
		 * page를 zone lruvec의 지정된 lru list에 등록한다.
		 * pgmoved를 증가시킨다.
		 **/
		list_move(&page->lru, &lruvec->lists[lru]);
		pgmoved += nr_pages;

		/** 20140524    
		 * page의 reference count를 감소시켰을 때 비사용 상태가 되었다면
		 **/
		if (put_page_testzero(page)) {
			/** 20140524    
			 * lru, active flags를 제거하고, lruvec에서 제거한다.
			 **/
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);

			/** 20140524    
			 * 해당 page가 compound page라면, dtor를 호출해 해제하고,
			 * 아니라면 pages_to_free에 등록한다.
			 **/
			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, pages_to_free);
		}
	}
	/** 20140524    
	 * lru의 state를 갱신시킨다.
	 **/
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, pgmoved);
	if (!is_active_lru(lru))
		__count_vm_events(PGDEACTIVATE, pgmoved);
}

/** 20140111
 * isolate 실행한다.
 *   - isolate된 페이지들 중 evictable하지 않은 page를 putback한다.
 *   - file cache인 페이지이면 l_active리스트로 등록시키고,
 *   - file cache가 아니면서 evictable한 page는 l_inactive 리스트로 등록시킨다.
 * 이후 zone의 각 lru 리스트로 옮기고, reference가 0인 페이지는 해제한다.
**/
static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	unsigned long nr_rotated = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);

	/** 20140104    
	 * cpu lru list에 있던 page를 zone에 반영시킨다.
	 **/
	lru_add_drain();

	/** 20140104    
	 * try_to_free_pages의 지역변수 sc를 참고.
	 **/
	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	/** 20140104    
	 * zone의 lru lock.
	 **/
	spin_lock_irq(&zone->lru_lock);

	/** 20140111
	 * isolate를 실행하고, isolate된 페이지의 갯수를 리턴한다.
	 **/
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);

	/** 20140111
	 * scan된 page의 갯수를 누적시킨다.
 	**/
	if (global_reclaim(sc))
		zone->pages_scanned += nr_scanned;

	/** 20140111
	 * reclaim될 page에 대한 recent_scanned값을 누적시킨다.
	 **/
	reclaim_stat->recent_scanned[file] += nr_taken;

	/** 20140111
	 * zone에 발생한 event를 stat에 반영한다.
	 **/
	__count_zone_vm_events(PGREFILL, zone, nr_scanned);
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	/** 20140111
	 * isolate한 page리스트(l_hold)에서 unevictable한 페이지를 putback하고
	 * 남은 page리스트를 inactive시키고 l_inactive리스트에 추가한다.
	 **/
	while (!list_empty(&l_hold)) {
		/** 20140111
		 * condition에 따라 rescheduling됨, sleep도 가능
		 **/
		cond_resched();
		/** 20140111
		* l_hold리스트의 제일 마지막 page를 제거한다.
		**/
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		/** 20140111
		 * isolate한 page가 evictable하지 않다면 lru에 다시 등록시킨다.
		 **/
		if (unlikely(!page_evictable(page, NULL))) {
			putback_lru_page(page);
			continue;
		}

		/** 20140111
		 * buffer_heads_over_limit는 blkdev에 관련된 내용이므로
		 * 추후 분석하기로함 ???
		 **/
		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		/** 20140111
		 * rmap관련 내용이므로 추후 분석하기로 함 ???
		 **/
		if (page_referenced(page, 0, sc->target_mem_cgroup,
				    &vm_flags)) {
			nr_rotated += hpage_nr_pages(page);
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			/** 20140111
			 * 실행 가능하고 file cache인 page에 대해서는 
			 * l_active리스트에 등록시킨다.
			 **/
			if ((vm_flags & VM_EXEC) && page_is_file_cache(page)) {
				list_add(&page->lru, &l_active);
				continue;
			}
		}

		/** 20140111
		 * 위 조건에서 vm_flags와 file_cache인 경우를 제외한 page들은
		 * page의 active flag를 clear하고 l_inactive에 page를 등록시킨다. 
		 **/
		ClearPageActive(page);	/* we are de-activating */

		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&zone->lru_lock);
	/*
	 * Count referenced pages from currently used mappings as rotated,
	 * even though only some of them are actually re-activated.  This
	 * helps balance scan pressure between file and anonymous pages in
	 * get_scan_ratio.
	 */
	/** 20140111
	 * isolate된 page중 referrend되어 있으며, inactive된 page의 수(nr_rotated)
	 * 를 reclaim_stat의 rotated 변수에 업데이트 시킨다.
	 **/
	reclaim_stat->recent_rotated[file] += nr_rotated;

	/** 20140111
	 * l_active 및 l_inactive의 page를 lru 리스트로 옮기고,
	 * 그 과정에서 reference count가 0이 된 page는 l_hold로 옮긴 뒤
	 * percpu의 cold page로서 free시킨다.
	 **/
	move_active_pages_to_lru(lruvec, &l_active, &l_hold, lru);
	move_active_pages_to_lru(lruvec, &l_inactive, &l_hold, lru - LRU_ACTIVE);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	free_hot_cold_page_list(&l_hold, 1);
}

#ifdef CONFIG_SWAP
/** 20140118    
 * zone 내에서 inactive ratio를 감안한 inactive anon 페이지의 수가 
 * active anon 페이지의 수보다 적은지 검사해 리턴
 **/
static int inactive_anon_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	/** 20140118    
	 * zone 상의 ACTIVE ANON과 INACTIVE ANON 페이지의 수를 구한다.
	 **/
	active = zone_page_state(zone, NR_ACTIVE_ANON);
	inactive = zone_page_state(zone, NR_INACTIVE_ANON);

	/** 20140118    
	 * inactive에 ratio를 적용한 것이 active 페이지의 수보다 작다면 1을 리턴
	 **/
	if (inactive * zone->inactive_ratio < active)
		return 1;

	return 0;
}

/**
 * inactive_anon_is_low - check if anonymous pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * Returns true if the zone does not have enough inactive anon pages,
 * meaning some active anon pages need to be deactivated.
 */
/** 20131221
 * lruvec에 해당하는 zone에서
 * inactive anon 페이지가 적으면 true를 리턴한다
 **/
static int inactive_anon_is_low(struct lruvec *lruvec)
{
	/*
	 * If we don't have swap space, anonymous page deactivation
	 * is pointless.
	 */
	/** 20131221
	  swap으로 사용할 page가 없으면 0을 리턴한다.
	 **/
	if (!total_swap_pages)
		return 0;

	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_anon_is_low(lruvec);

	return inactive_anon_is_low_global(lruvec_zone(lruvec));
}
#else
static inline int inactive_anon_is_low(struct lruvec *lruvec)
{
	return 0;
}
#endif

/** 20131221
 * 해당 zone에서 active file페이지가 크면 true를 리턴한다.
 **/
static int inactive_file_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_FILE);
	inactive = zone_page_state(zone, NR_INACTIVE_FILE);

	return (active > inactive);
}

/**
 * inactive_file_is_low - check if file pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * When the system is doing streaming IO, memory pressure here
 * ensures that active file pages get deactivated, until more
 * than half of the file pages are on the inactive list.
 *
 * Once we get to that situation, protect the system's working
 * set from being evicted by disabling active file page aging.
 *
 * This uses a different ratio than the anonymous pages, because
 * the page cache uses a use-once replacement algorithm.
 */
/** 20131221
 * inactive file페이지가 적으면 true를 리턴한다.
 **/
static int inactive_file_is_low(struct lruvec *lruvec)
{
		/** 20131221
		 * mem_cgroup_disabled에서 항상 true를 리턴한다
		 **/
	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_file_is_low(lruvec);

	return inactive_file_is_low_global(lruvec_zone(lruvec));
}
/** 20131221
 * 해당 lru에서 inactive된 페이지의 갯수가 적으면 true를 리턴한다.
 **/
static int inactive_list_is_low(struct lruvec *lruvec, enum lru_list lru)
{
	if (is_file_lru(lru))
		return inactive_file_is_low(lruvec);
	else
		return inactive_anon_is_low(lruvec);
}

/** 20140118    
 * 주어진 lru가 active lru인 경우,
 *   inactive lru의 target ratio 아래로 page수가 떨어졌을 경우 shrink 수행
 * 주어진 lru가 inactive lru인 경우,
 *   inactive shrink 수행
 *
 * active lru인 경우 reclaim 여부와 상관없이 항상 0을 리턴,
 * inactive lru인 경우 reclaim한 페이지 수를 리턴 (왜???)
 **/
static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	/** 20140104    
	 * lru가 active인 경우 shrink_active_list를 호출하고 0을 리턴.
	 **/
	if (is_active_lru(lru)) {
		/** 20140104    
		 * 해당 lru의 type(file, anon)에서 inactive list의 수가 적을 때
		 * shrink_active_list 호출
		 **/
		if (inactive_list_is_low(lruvec, lru))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		return 0;
	}

	/** 20140104    
	 * lru가 inactive인 경우 shrink_inactive_list를 호출
	 **/
	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

static int vmscan_swappiness(struct scan_control *sc)
{
	if (global_reclaim(sc))
		return vm_swappiness;
	return mem_cgroup_swappiness(sc->target_mem_cgroup);
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
/** 20131221
 * 각각의 lru에서 스캔해야 할 페이지의 수(nr)를 리턴한다.
 * 자세한 분석은 생략함???
 **/

static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
			   unsigned long *nr)
{
	unsigned long anon, file, free;
	unsigned long anon_prio, file_prio;
	unsigned long ap, fp;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	u64 fraction[2], denominator;
	enum lru_list lru;
	int noswap = 0;
	bool force_scan = false;
	struct zone *zone = lruvec_zone(lruvec);

	/*
	 * If the zone or memcg is small, nr[l] can be 0.  This
	 * results in no scanning on this priority and a potential
	 * priority drop.  Global direct reclaim can go to the next
	 * zone and tends to have no problems. Global kswapd is for
	 * zone balancing and it needs to scan a minimum amount. When
	 * reclaiming for a memcg, a priority drop can cause high
	 * latencies, so it's better to scan a minimum amount there as
	 * well.
	 */
	/** 20131214    
	 * 이 함수가 kswapd에 의해 수행되며
	 * zone의 page들이 (고정되어) 회수되지 못하는 경우
	 *   force_scan.
	 **/
	if (current_is_kswapd() && zone->all_unreclaimable)
		force_scan = true;
	/** 20131214    
	 * global_reclaim는 항상 true이므로 false.
	 **/
	if (!global_reclaim(sc))
		force_scan = true;

	/* If we have no swap space, do not bother scanning anon pages. */
	/** 20131214    
	 * scan control 정보를 읽어 swap 가능하지 않거나
	 * swap page의 크기가 0보다 작다면
	 *
	 * noswap.
	 **/
	if (!sc->may_swap || (nr_swap_pages <= 0)) {
		noswap = 1;
		fraction[0] = 0;
		fraction[1] = 1;
		/** 20131214    
		 * 분모
		 **/
		denominator = 1;
		goto out;
	}

	/** 20131214    
	 * anon = active anon + inactive anon
	 * file = active file + inactive file
	 **/
	anon  = get_lru_size(lruvec, LRU_ACTIVE_ANON) +
		get_lru_size(lruvec, LRU_INACTIVE_ANON);
	file  = get_lru_size(lruvec, LRU_ACTIVE_FILE) +
		get_lru_size(lruvec, LRU_INACTIVE_FILE);

	/** 20131214    
	 * 항상 true.
	 **/
	if (global_reclaim(sc)) {
		/** 20131214    
		 * zone의 free pages 값을 읽어온다.
		 **/
		free  = zone_page_state(zone, NR_FREE_PAGES);
		/* If we have very few page cache pages,
		   force-scan anon pages. */
		/** 20131214    
		 * file로 mapping 된 page수와 free page수를 더한 값이
		 *   high watermark를 넘지 않았다면
		 *   ??? goto out.
		 **/
		if (unlikely(file + free <= high_wmark_pages(zone))) {
			fraction[0] = 1;
			fraction[1] = 0;
			denominator = 1;
			goto out;
		}
	}

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	/** 20131214    
	 * vmscan의 swappiness를 가져온다.
	 * sysctl로 변경하지 않는다면 초기값 60 (0~100)
	 *
	 * swappiness가 100인 경우 anonymous와 file이 같은 priority를 갖는다.
	 * 이 scanning
	 **/
	anon_prio = vmscan_swappiness(sc);
	file_prio = 200 - anon_prio;

	/** 20131221
	
**/

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */
	spin_lock_irq(&zone->lru_lock);
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
	}

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = anon_prio * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = file_prio * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;
	spin_unlock_irq(&zone->lru_lock);

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp + 1;
out:
	for_each_evictable_lru(lru) {
		int file = is_file_lru(lru);
		unsigned long scan;

		/** 20140517    
		 * lruvec에서 lru에 해당하는 stat값을 리턴한다.
		 **/
		scan = get_lru_size(lruvec, lru);
		if (sc->priority || noswap || !vmscan_swappiness(sc)) {
			scan >>= sc->priority;
			if (!scan && force_scan)
				scan = SWAP_CLUSTER_MAX;
			scan = div64_u64(scan * fraction[file], denominator);
		}
		nr[lru] = scan;
	}
}

/* Use reclaim/compaction for costly allocs or under memory pressure */
/** 20140118    
 * COMPACTION_BUILD를 사용하지 않아 항상 false 리턴.
 **/
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (COMPACTION_BUILD && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_zone() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
/** 20140118    
 *     nr_reclaimed : 앞서 reclaimed 된 페이지의 수
 *     nr_scanned   : 앞서 scanned 된 페이지의 수
 *
 *  CONFIG_COMPACTION이 정의되어 있지 않아 바로 false 리턴.
 *  분석 안함???
 **/
static inline bool should_continue_reclaim(struct lruvec *lruvec,
					unsigned long nr_reclaimed,
					unsigned long nr_scanned,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;

	/* If not in reclaim/compaction mode, stop */
	/** 20140118    
	 * compaction을 사용하지 않도록 설정하였으므로 바로 false 리턴
	 **/
	if (!in_reclaim_compaction(sc))
		return false;

	/* Consider stopping depending on scan and reclaim activity */
	if (sc->gfp_mask & __GFP_REPEAT) {
		/*
		 * For __GFP_REPEAT allocations, stop reclaiming if the
		 * full LRU list has been scanned and we are still failing
		 * to reclaim pages. This full LRU scan is potentially
		 * expensive but a __GFP_REPEAT caller really wants to succeed
		 */
		if (!nr_reclaimed && !nr_scanned)
			return false;
	} else {
		/*
		 * For non-__GFP_REPEAT allocations which can presumably
		 * fail without consequence, stop if we failed to reclaim
		 * any pages from the last SWAP_CLUSTER_MAX number of
		 * pages that were scanned. This will return to the
		 * caller faster at the risk reclaim/compaction and
		 * the resulting allocation attempt fails
		 */
		if (!nr_reclaimed)
			return false;
	}

	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = (2UL << sc->order);
	inactive_lru_pages = get_lru_size(lruvec, LRU_INACTIVE_FILE);
	if (nr_swap_pages > 0)
		inactive_lru_pages += get_lru_size(lruvec, LRU_INACTIVE_ANON);
	if (sc->nr_reclaimed < pages_for_compaction &&
			inactive_lru_pages > pages_for_compaction)
		return true;

	/* If compaction would go ahead or the allocation would succeed, stop */
	switch (compaction_suitable(lruvec_zone(lruvec), sc->order)) {
	case COMPACT_PARTIAL:
	case COMPACT_CONTINUE:
		return false;
	default:
		return true;
	}
}

/*
 * This is a basic per-zone page freer.  Used by both kswapd and direct reclaim.
 */
/** 20140118    
 * 특정 zone의 lru list에 대한 정보를 담고 있는 lruvec과
 * scan control로부터 scanning할 페이지의 속성과 reclaim할 갯수 등을 받아와
 * 각 lru list에 대해 shrink를 수행한다.
 **/
static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	/** 20131214    
	 * LRU LIST마다 nr 값을 저장하기 위한 array.
	 **/
	unsigned long nr[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed, nr_scanned;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	struct blk_plug plug;

restart:
	/** 20131214    
	 * nr_reclaimed, nr_scanned 초기화
	 **/
	nr_reclaimed = 0;
	nr_scanned = sc->nr_scanned;
	/** 20131221
	 * 특정 lru알고리즘을 통해서 scan할 페이지 수를 얻어와 nr에 저장한다.
	 **/
	get_scan_count(lruvec, sc, nr);
	/** 20131221
	 * blk_plug를 초기화하는 함수 
	 **/
	blk_start_plug(&plug);
	/** 20131221
	 * active_anon을 제외한 lru리스트들이 존재하는 동안
	 * shrink_list 함수를 수행한다
	 **/
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		/** 20140118    
		 * evictable한 lru list에 대해 shrink_list를 수행
		 **/
		for_each_evictable_lru(lru) {
			if (nr[lru]) {
				/** 20140104    
				 * nr_to_scan의 최대치는 SWAP_CLUSTER_MAX
				 **/
				nr_to_scan = min_t(unsigned long,
						   nr[lru], SWAP_CLUSTER_MAX);
				/** 20140104    
				 * nr_to_scan만큼 nr[lru]를 미리 뺀다.
				 **/
				nr[lru] -= nr_to_scan;

				/** 20140118    
				 * lru list의 entry들을 nr_to_scan만큼 scan해 shrink하고,
				 * reclaimed 된 페이지 수를 누적시킨다.
				 **/
				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}
		/*
		 * On large memory systems, scan >> priority can become
		 * really large. This is fine for the starting priority;
		 * we want to put equal scanning pressure on each zone.
		 * However, if the VM has a harder time of freeing pages,
		 * with multiple processes reclaiming pages, the total
		 * freeing target can get unreasonably large.
		 */
		/** 20140118    
		 * scan control로 전달된 목표치 이상을 reclaimed 했을 경우
		 * scan할 page의 수가 남아 있어도 break 한다.
		 **/
		if (nr_reclaimed >= nr_to_reclaim &&
		    sc->priority < DEF_PRIORITY)
			break;
	}
	/** 20131221
	 * blk_plug구조체를 참조한 뒤에 해제함
	 **/
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	/** 20140118    
	 * inactive anon 페이지가 적다면
	 *   shrink active list를 통해 inactive anon 페이지를 확보한다.
	 **/
	if (inactive_anon_is_low(lruvec))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);

	/* reclaim/compaction might need reclaim to continue */
	if (should_continue_reclaim(lruvec, nr_reclaimed,
				    sc->nr_scanned - nr_scanned, sc))
		goto restart;

	throttle_vm_writeout(sc->gfp_mask);
}

/** 20140118    
 * zone에 대해 shrink를 수행한다.
 *   MEMCG를 사용하지 않기에 zone에 대해 한 번만 수행된다.
 **/
static void shrink_zone(struct zone *zone, struct scan_control *sc)
{
	struct mem_cgroup *root = sc->target_mem_cgroup;
	struct mem_cgroup_reclaim_cookie reclaim = {
		.zone = zone,
		.priority = sc->priority,
	};
	struct mem_cgroup *memcg;

	memcg = mem_cgroup_iter(root, NULL, &reclaim);
	do {
		/** 20131214    
		 * memcg을 사용하지 않아 zone에 해당하는 lruvec을 받아옴.
		 * lruvec에 속한 lru list들에 대해 shrink를 수행한다.
		 **/
		struct lruvec *lruvec = mem_cgroup_zone_lruvec(zone, memcg);

		shrink_lruvec(lruvec, sc);

		/*
		 * Limit reclaim has historically picked one memcg and
		 * scanned it with decreasing priority levels until
		 * nr_to_reclaim had been reclaimed.  This priority
		 * cycle is thus over after a single memcg.
		 *
		 * Direct reclaim and kswapd, on the other hand, have
		 * to scan all memory cgroups to fulfill the overall
		 * scan target for the zone.
		 */
		/** 20140118    
		 * MEMCG를 사용하지 않아 global reclaim 사용
		 **/
		if (!global_reclaim(sc)) {
			mem_cgroup_iter_break(root, memcg);
			break;
		}
		memcg = mem_cgroup_iter(root, memcg, &reclaim);
	} while (memcg);
}

/* Returns true if compaction should go ahead for a high-order request */
static inline bool compaction_ready(struct zone *zone, struct scan_control *sc)
{
	unsigned long balance_gap, watermark;
	bool watermark_ok;

	/* Do not consider compaction for orders reclaim is meant to satisfy */
	if (sc->order <= PAGE_ALLOC_COSTLY_ORDER)
		return false;

	/*
	 * Compaction takes time to run and there are potentially other
	 * callers using the pages just freed. Continue reclaiming until
	 * there is a buffer of free pages available to give compaction
	 * a reasonable chance of completing and allocating the page
	 */
	balance_gap = min(low_wmark_pages(zone),
		(zone->present_pages + KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
			KSWAPD_ZONE_BALANCE_GAP_RATIO);
	watermark = high_wmark_pages(zone) + balance_gap + (2UL << sc->order);
	watermark_ok = zone_watermark_ok_safe(zone, 0, watermark, 0, 0);

	/*
	 * If compaction is deferred, reclaim up to a point where
	 * compaction will have a chance of success when re-enabled
	 */
	if (compaction_deferred(zone, sc->order))
		return watermark_ok;

	/* If compaction is not ready to start, keep reclaiming */
	if (!compaction_suitable(zone, sc->order))
		return false;

	return watermark_ok;
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * We reclaim from a zone even if that zone is over high_wmark_pages(zone).
 * Because:
 * a) The caller may be trying to free *extra* pages to satisfy a higher-order
 *    allocation or
 * b) The target zone may be at high_wmark_pages(zone) but the lower zones
 *    must go *over* high_wmark_pages(zone) to satisfy the `incremental min'
 *    zone defense algorithm.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 *
 * This function returns true if a zone is being reclaimed for a costly
 * high-order allocation and compaction is ready to begin. This indicates to
 * the caller that it should consider retrying the allocation instead of
 * further reclaim.
 */
/** 20140118    
 * zonelist에 속하는 zone들에 대해 shrink_zone를 수행한다.
 * aborted_reclaim 여부를 리턴한다.
 **/
static bool shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	bool aborted_reclaim = false;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	/** 20131214    
	 * buffer heads가 허용된 수치를 초과할 경우
	 * scan control의 gfp_mask에 GFP_HIGHMEM 속성을 추가한다.
	 * 주석은 이해 안 됨???
	 **/
	if (buffer_heads_over_limit)
		sc->gfp_mask |= __GFP_HIGHMEM;

	/** 20131214    
	 * 전체 zone list들 중 gfp_zone 이하의 존들에 대해 shrink_zone 수행
	 **/
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(sc->gfp_mask), sc->nodemask) {
		/** 20131214    
		 * zone이 비어 있으면 다음 zone.
		 **/
		if (!populated_zone(zone))
			continue;
		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		/** 20131214    
		 * global_reclaim는 항상 true.
		 **/
		if (global_reclaim(sc)) {
			/** 20131214    
			 * hardwall 기준을 통과하는 zone이 아니라면 다음 zone으로.
			 **/
			if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
				continue;
			/** 20131214    
			 * zone의 page들이 unreclaimable (pinned 되어 있다) 하고,
			 * 초기값 DEF_PRIORITY가 아닌 경우 다음 zone으로.
			 **/
			if (zone->all_unreclaimable &&
					sc->priority != DEF_PRIORITY)
				continue;	/* Let kswapd poll it */
			/** 20131214    
			 * huge page를 위한 memory compaction을 하도록 빌드 되어 있는지 검사.
			 * default로 설정되어 있지 않아 false.
			 **/
			if (COMPACTION_BUILD) {
				/*
				 * If we already have plenty of memory free for
				 * compaction in this zone, don't free any more.
				 * Even though compaction is invoked for any
				 * non-zero order, only frequent costly order
				 * reclamation is disruptive enough to become a
				 * noticeable problem, like transparent huge
				 * page allocations.
				 */
				if (compaction_ready(zone, sc)) {
					aborted_reclaim = true;
					continue;
				}
			}
			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			/** 20131214    
			 * MEM CGROUP을 사용하지 않아 0을 리턴한다.
			 **/
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
						sc->order, sc->gfp_mask,
						&nr_soft_scanned);
			/** 20131214    
			 * 정의되어 있지 않아 누적해도 0.
			 **/
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			/* need some check for avoid more shrink_zone() */
		}

		shrink_zone(zone, sc);
	}

	return aborted_reclaim;
}

static bool zone_reclaimable(struct zone *zone)
{
	return zone->pages_scanned < zone_reclaimable_pages(zone) * 6;
}

/* All zones in zonelist are unreclaimable? */
static bool all_unreclaimable(struct zonelist *zonelist,
		struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
			gfp_zone(sc->gfp_mask), sc->nodemask) {
		if (!populated_zone(zone))
			continue;
		if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
			continue;
		if (!zone->all_unreclaimable)
			return false;
	}

	return true;
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
/** 20140524
 * 
 **/
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					struct scan_control *sc,
					struct shrink_control *shrink)
{
	unsigned long total_scanned = 0;
	struct reclaim_state *reclaim_state = current->reclaim_state;
	struct zoneref *z;
	struct zone *zone;
	unsigned long writeback_threshold;
	bool aborted_reclaim;

	/** 20131214    
	 * CONFIG_TASK_DELAY_ACCT 설정되어 있지 않아 NULL 함수
	 **/
	delayacct_freepages_start();

	/** 20131214    
	 * CONFIG_MEMCG가 설정되어 있지 않은 경우
	 * ALLOCSTALL 이벤트에 대한 count를 증가시킨다.
	 **/
	if (global_reclaim(sc))
		count_vm_event(ALLOCSTALL);

	do {
		/** 20131214    
		 * scan된 inactive pages 수를 초기화.
		 **/
		sc->nr_scanned = 0;
		/** 20140118    
		 * sc에 따라 zonelist의 해당 zone에 대해 shrink를 수행한다.
		 * shrink_zones의 리턴값에 따라 reclaim을 중단할지 계속 수행할지 결정된다.
		 **/
		aborted_reclaim = shrink_zones(zonelist, sc);

		/*
		 * Don't shrink slabs when reclaiming memory from
		 * over limit cgroups
		 */
		if (global_reclaim(sc)) {
			unsigned long lru_pages = 0;
			/** 20140517    
			 * zonelist의 각 zone에 대해 회수 가능한 lru_pages의 수를 계산.
			 **/
			for_each_zone_zonelist(zone, z, zonelist,
					gfp_zone(sc->gfp_mask)) {
				/** 20140517    
				 * 특정 zone에 대한 hardwall 기준을 통과하지 못한 경우 다음 zone에 대해 수행
				 **/
				if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
					continue;

				/** 20140517    
				 * zone stat을 조회해 회수 가능한 pages의 수를 합산
				 **/
				lru_pages += zone_reclaimable_pages(zone);
			}

			/** 20140517    
			 * shrinker를 shrink를 수행하고,
			 * scan control에 회수된 slab의 수를 합산한다.
			 **/
			shrink_slab(shrink, sc->nr_scanned, lru_pages);
			if (reclaim_state) {
				sc->nr_reclaimed += reclaim_state->reclaimed_slab;
				reclaim_state->reclaimed_slab = 0;
			}
		}
		/** 20140517    
		 * total_scanned에 nr_scanned 된 페이지 수를 누적시킨다.
		 **/
		total_scanned += sc->nr_scanned;
		/** 20140517    
		 * 회수된 페이지 수가 reclaim할 페이지 수 이상이면 빠져나간다.
		 **/
		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			goto out;

		/*
		 * Try to write back as many pages as we just scanned.  This
		 * tends to cause slow streaming writers to write data to the
		 * disk smoothly, at the dirtying rate, which is nice.   But
		 * that's undesirable in laptop mode, where we *want* lumpy
		 * writeout.  So in laptop mode, write out the whole world.
		 */
		/** 20140517    
		 * writeback_threshold를 nr_to_reclaim의 1.5배로 한다.
		 **/
		writeback_threshold = sc->nr_to_reclaim + sc->nr_to_reclaim / 2;
		/** 20140517    
		 * 현재까지 scan된 페이지가 threshold 이상이면 flusher thread를 깨운다.
		 **/
		if (total_scanned > writeback_threshold) {
			wakeup_flusher_threads(laptop_mode ? 0 : total_scanned,
						WB_REASON_TRY_TO_FREE_PAGES);
			sc->may_writepage = 1;
		}

		/* Take a nap, wait for some writeback to complete */
		if (!sc->hibernation_mode && sc->nr_scanned &&
		    sc->priority < DEF_PRIORITY - 2) {
			struct zone *preferred_zone;

			first_zones_zonelist(zonelist, gfp_zone(sc->gfp_mask),
						&cpuset_current_mems_allowed,
						&preferred_zone);
			wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/10);
		}
	} while (--sc->priority >= 0);

out:
	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/*
	 * As hibernation is going on, kswapd is freezed so that it can't mark
	 * the zone into all_unreclaimable. Thus bypassing all_unreclaimable
	 * check.
	 */
	if (oom_killer_disabled)
		return 0;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (aborted_reclaim)
		return 1;

	/* top priority shrink_zones still had more to do? don't OOM, then */
	if (global_reclaim(sc) && !all_unreclaimable(zonelist, sc))
		return 1;

	return 0;
}

/** 20131207
 * pfmemalloc 의 최소 필요 사이즈에 대한 water mark를 체크하고 결과는 리턴
 * - 해당 노드의 free_pages와 water mark min/2 비교하여 
 * free pages가 부족하면 swapd를 깨운다.
 *
 * 20131214
 * wmark min 이하의 메모리들은 pfmemalloc용으로 사용하기 위한 용도의
 * watermark 하한이다. 이 공간에 대해 watermark test를 한다.
 */
static bool pfmemalloc_watermark_ok(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	/** 20131207
	 * ZONE_NORMAL 이하의 zone을 순회하며
	 * min water mark 페이지 수와 free 페이지수를 각각 누적한다.
	 */
	for (i = 0; i <= ZONE_NORMAL; i++) {
		zone = &pgdat->node_zones[i];
		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	/** 20131207
	 * free_pages 수가 pfmemalloc_reserve/2 이상이면 watermark test ok.
	 */
	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	/** 20131207
	 * watermark test 실패이고, kswapd가 waitqueue들어가 있으면
	 * pgdat->classzone_idx의 상한을 ZONE_NORMAL로 하고, swapd를 깨운다.
	 */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		pgdat->classzone_idx = min(pgdat->classzone_idx,
						(enum zone_type)ZONE_NORMAL);
		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached
 */
/** 20131214    
 * direct reclaim 전에 PFMEMALLOC watermark 테스트를 만족할 때까지 기다린다.
 **/
static void throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zone *zone;
	int high_zoneidx = gfp_zone(gfp_mask);
	pg_data_t *pgdat;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
	/** 20131207
	 * current task의 flags가 PF_KTHREAD(kernel thread)라면 리턴!
	 * 이유는 위 주석인데...???
	 * 
	 * 20140517
	 * INIT_TASK 는 PF_KTHREAD 설정되어 있음
	 */
	if (current->flags & PF_KTHREAD)
		return;

	/* Check if the pfmemalloc reserves are ok */
	first_zones_zonelist(zonelist, high_zoneidx, NULL, &zone);
	/** 20131207
	 * zone이pgdata를 가져와서 watermark 검사를 한다.
	 * true이면 pfmemalloc을 위한 direct reclaim에 throttle을 할 필요없다!
	 */
	pgdat = zone->zone_pgdat;
	if (pfmemalloc_watermark_ok(pgdat))
		return;

	/* Account for the throttling */
	/** 20131207
	 * cat /proc/vmstat  | grep -i throttle 로 확인 가능
	 */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
	/** 20131207
	 * filesystem을 사용할수 없는 경우(FS lock holding, journal transaction)
	 * HZ시간 동안 기다리거나 pfmemalloc_watermark_ok조건이 참이 될때까지 기다린다(sleep)
	 *
	 * 20131214
	 * __GFP_FS
	 *   If clear, the kernel is not allowed to perform filesystem-dependent operations.
	 * 메모리 할당 과정 중 file sytem에 대한 접근을 안하도록 요청된 경우
	 * pfmemalloc watermark test가 통과할 때까지 HZ동안(1초) 기다린다.
	 */
	if (!(gfp_mask & __GFP_FS)) {
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			pfmemalloc_watermark_ok(pgdat), HZ);
		return;
	}

	/* Throttle until kswapd wakes the process */
	/** 20131214    
	 * 여기까지 왔다면 kernel thread가 아니고,
	 * FileSystem 사용 불가 상태도 아니다.
	 *
	 * kswapd가 메모리를 확보해 pfmemalloc_watermark 테스트를 통과할 때까지
	 * TASK_KILLABLE 상태로 sleep 한다.
	 * 또는 FATAL SIGNAL이 pending 되어 있을 경우 리턴한다.
	 **/
	wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
		pfmemalloc_watermark_ok(pgdat));
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.gfp_mask = gfp_mask,
		.may_writepage = !laptop_mode,
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.may_unmap = 1,
		.may_swap = 1,
		.order = order,
		.priority = DEF_PRIORITY,
		.target_mem_cgroup = NULL,
		.nodemask = nodemask,
	};
	struct shrink_control shrink = {
		.gfp_mask = sc.gfp_mask,
	};

	/** 20131214    
	 * reclaim 전에 throttle을 준다.
	 * (kswapd가 메모리를 확보할 때까지 sleep할 수도 있다)
	 **/
	throttle_direct_reclaim(gfp_mask, zonelist, nodemask);

	/*
	 * Do not enter reclaim if fatal signal is pending. 1 is returned so
	 * that the page allocator does not consider triggering OOM
	 */
	/** 20131214    
	 * fatal signal이 대기 중일 경우 reclaim을 하지 않고 리턴한다.
	 **/
	if (fatal_signal_pending(current))
		return 1;

	trace_mm_vmscan_direct_reclaim_begin(order,
				sc.may_writepage,
				gfp_mask);

	/** 20140517    
	 * direct reclaim.
	 **/
	nr_reclaimed = do_try_to_free_pages(zonelist, &sc, &shrink);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}

#ifdef CONFIG_MEMCG

unsigned long mem_cgroup_shrink_node_zone(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						struct zone *zone,
						unsigned long *nr_scanned)
{
	struct scan_control sc = {
		.nr_scanned = 0,
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = !noswap,
		.order = 0,
		.priority = 0,
		.target_mem_cgroup = memcg,
	};
	struct lruvec *lruvec = mem_cgroup_zone_lruvec(zone, memcg);

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.may_writepage,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_zone from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_lruvec(lruvec, &sc);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;
	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   gfp_t gfp_mask,
					   bool noswap)
{
	struct zonelist *zonelist;
	unsigned long nr_reclaimed;
	int nid;
	struct scan_control sc = {
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = !noswap,
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.order = 0,
		.priority = DEF_PRIORITY,
		.target_mem_cgroup = memcg,
		.nodemask = NULL, /* we don't care the placement */
		.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
	};
	struct shrink_control shrink = {
		.gfp_mask = sc.gfp_mask,
	};

	/*
	 * Unlike direct reclaim via alloc_pages(), memcg's reclaim doesn't
	 * take care of from where we get pages. So the node where we start the
	 * scan does not need to be the current node.
	 */
	nid = mem_cgroup_select_victim_node(memcg);

	zonelist = NODE_DATA(nid)->node_zonelists;

	trace_mm_vmscan_memcg_reclaim_begin(0,
					    sc.may_writepage,
					    sc.gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc, &shrink);

	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}
#endif

static void age_active_anon(struct zone *zone, struct scan_control *sc)
{
	struct mem_cgroup *memcg;

	if (!total_swap_pages)
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_zone_lruvec(zone, memcg);

		if (inactive_anon_is_low(lruvec))
			shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
					   sc, LRU_ACTIVE_ANON);

		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}

/*
 * pgdat_balanced is used when checking if a node is balanced for high-order
 * allocations. Only zones that meet watermarks and are in a zone allowed
 * by the callers classzone_idx are added to balanced_pages. The total of
 * balanced pages must be at least 25% of the zones allowed by classzone_idx
 * for the node to be considered balanced. Forcing all zones to be balanced
 * for high orders can cause excessive reclaim when there are imbalanced zones.
 * The choice of 25% is due to
 *   o a 16M DMA zone that is balanced will not balance a zone on any
 *     reasonable sized machine
 *   o On all other machines, the top zone must be at least a reasonable
 *     percentage of the middle zones. For example, on 32-bit x86, highmem
 *     would need to be at least 256M for it to be balance a whole node.
 *     Similarly, on x86-64 the Normal zone would need to be at least 1G
 *     to balance a node on its own. These seemed like reasonable ratios.
 */
static bool pgdat_balanced(pg_data_t *pgdat, unsigned long balanced_pages,
						int classzone_idx)
{
	unsigned long present_pages = 0;
	int i;

	for (i = 0; i <= classzone_idx; i++)
		present_pages += pgdat->node_zones[i].present_pages;

	/* A special case here: if zone has no page, we think it's balanced */
	return balanced_pages >= (present_pages >> 2);
}

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order, long remaining,
					int classzone_idx)
{
	int i;
	unsigned long balanced = 0;
	bool all_zones_ok = true;

	/* If a direct reclaimer woke kswapd within HZ/10, it's premature */
	if (remaining)
		return false;

	/*
	 * There is a potential race between when kswapd checks its watermarks
	 * and a process gets throttled. There is also a potential race if
	 * processes get throttled, kswapd wakes, a large process exits therby
	 * balancing the zones that causes kswapd to miss a wakeup. If kswapd
	 * is going to sleep, no process should be sleeping on pfmemalloc_wait
	 * so wake them now if necessary. If necessary, processes will wake
	 * kswapd and get throttled again
	 */
	if (waitqueue_active(&pgdat->pfmemalloc_wait)) {
		wake_up(&pgdat->pfmemalloc_wait);
		return false;
	}

	/* Check the watermark levels */
	for (i = 0; i <= classzone_idx; i++) {
		struct zone *zone = pgdat->node_zones + i;

		if (!populated_zone(zone))
			continue;

		/*
		 * balance_pgdat() skips over all_unreclaimable after
		 * DEF_PRIORITY. Effectively, it considers them balanced so
		 * they must be considered balanced here as well if kswapd
		 * is to sleep
		 */
		if (zone->all_unreclaimable) {
			balanced += zone->present_pages;
			continue;
		}

		if (!zone_watermark_ok_safe(zone, order, high_wmark_pages(zone),
							i, 0))
			all_zones_ok = false;
		else
			balanced += zone->present_pages;
	}

	/*
	 * For high-order requests, the balanced zones must contain at least
	 * 25% of the nodes pages for kswapd to sleep. For order-0, all zones
	 * must be balanced
	 */
	if (order)
		return pgdat_balanced(pgdat, balanced, classzone_idx);
	else
		return all_zones_ok;
}

/*
 * For kswapd, balance_pgdat() will work across all this node's zones until
 * they are all at high_wmark_pages(zone).
 *
 * Returns the final order kswapd was reclaiming at
 *
 * There is special handling here for zones which are full of pinned pages.
 * This can happen if the pages are all mlocked, or if they are all used by
 * device drivers (say, ZONE_DMA).  Or if they are all in use by hugetlb.
 * What we do is to detect the case where all pages in the zone have been
 * scanned twice and there has been zero successful reclaim.  Mark the zone as
 * dead and from now on, only perform a short scan.  Basically we're polling
 * the zone for when the problem goes away.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), we scan that zone and the
 * lower zones regardless of the number of free pages in the lower zones. This
 * interoperates with the page allocator fallback scheme to ensure that aging
 * of pages is balanced across the zones.
 */
static unsigned long balance_pgdat(pg_data_t *pgdat, int order,
							int *classzone_idx)
{
	int all_zones_ok;
	unsigned long balanced;
	int i;
	int end_zone = 0;	/* Inclusive.  0 = ZONE_DMA */
	unsigned long total_scanned;
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 1,
		.may_swap = 1,
		/*
		 * kswapd doesn't want to be bailed out while reclaim. because
		 * we want to put equal scanning pressure on each zone.
		 */
		.nr_to_reclaim = ULONG_MAX,
		.order = order,
		.target_mem_cgroup = NULL,
	};
	struct shrink_control shrink = {
		.gfp_mask = sc.gfp_mask,
	};
loop_again:
	total_scanned = 0;
	sc.priority = DEF_PRIORITY;
	sc.nr_reclaimed = 0;
	sc.may_writepage = !laptop_mode;
	count_vm_event(PAGEOUTRUN);

	do {
		unsigned long lru_pages = 0;
		int has_under_min_watermark_zone = 0;

		all_zones_ok = 1;
		balanced = 0;

		/*
		 * Scan in the highmem->dma direction for the highest
		 * zone which needs scanning
		 */
		for (i = pgdat->nr_zones - 1; i >= 0; i--) {
			struct zone *zone = pgdat->node_zones + i;

			if (!populated_zone(zone))
				continue;

			if (zone->all_unreclaimable &&
			    sc.priority != DEF_PRIORITY)
				continue;

			/*
			 * Do some background aging of the anon list, to give
			 * pages a chance to be referenced before reclaiming.
			 */
			age_active_anon(zone, &sc);

			/*
			 * If the number of buffer_heads in the machine
			 * exceeds the maximum allowed level and this node
			 * has a highmem zone, force kswapd to reclaim from
			 * it to relieve lowmem pressure.
			 */
			if (buffer_heads_over_limit && is_highmem_idx(i)) {
				end_zone = i;
				break;
			}

			if (!zone_watermark_ok_safe(zone, order,
					high_wmark_pages(zone), 0, 0)) {
				end_zone = i;
				break;
			} else {
				/* If balanced, clear the congested flag */
				zone_clear_flag(zone, ZONE_CONGESTED);
			}
		}
		if (i < 0)
			goto out;

		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;

			lru_pages += zone_reclaimable_pages(zone);
		}

		/*
		 * Now scan the zone in the dma->highmem direction, stopping
		 * at the last zone which needs scanning.
		 *
		 * We do this because the page allocator works in the opposite
		 * direction.  This prevents the page allocator from allocating
		 * pages behind kswapd's direction of progress, which would
		 * cause too much scanning of the lower zones.
		 */
		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;
			int nr_slab, testorder;
			unsigned long balance_gap;

			if (!populated_zone(zone))
				continue;

			if (zone->all_unreclaimable &&
			    sc.priority != DEF_PRIORITY)
				continue;

			sc.nr_scanned = 0;

			nr_soft_scanned = 0;
			/*
			 * Call soft limit reclaim before calling shrink_zone.
			 */
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
							order, sc.gfp_mask,
							&nr_soft_scanned);
			sc.nr_reclaimed += nr_soft_reclaimed;
			total_scanned += nr_soft_scanned;

			/*
			 * We put equal pressure on every zone, unless
			 * one zone has way too many pages free
			 * already. The "too many pages" is defined
			 * as the high wmark plus a "gap" where the
			 * gap is either the low watermark or 1%
			 * of the zone, whichever is smaller.
			 */
			balance_gap = min(low_wmark_pages(zone),
				(zone->present_pages +
					KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
				KSWAPD_ZONE_BALANCE_GAP_RATIO);
			/*
			 * Kswapd reclaims only single pages with compaction
			 * enabled. Trying too hard to reclaim until contiguous
			 * free pages have become available can hurt performance
			 * by evicting too much useful data from memory.
			 * Do not reclaim more than needed for compaction.
			 */
			testorder = order;
			if (COMPACTION_BUILD && order &&
					compaction_suitable(zone, order) !=
						COMPACT_SKIPPED)
				testorder = 0;

			if ((buffer_heads_over_limit && is_highmem_idx(i)) ||
				    !zone_watermark_ok_safe(zone, testorder,
					high_wmark_pages(zone) + balance_gap,
					end_zone, 0)) {
				shrink_zone(zone, &sc);

				reclaim_state->reclaimed_slab = 0;
				nr_slab = shrink_slab(&shrink, sc.nr_scanned, lru_pages);
				sc.nr_reclaimed += reclaim_state->reclaimed_slab;
				total_scanned += sc.nr_scanned;

				if (nr_slab == 0 && !zone_reclaimable(zone))
					zone->all_unreclaimable = 1;
			}

			/*
			 * If we've done a decent amount of scanning and
			 * the reclaim ratio is low, start doing writepage
			 * even in laptop mode
			 */
			if (total_scanned > SWAP_CLUSTER_MAX * 2 &&
			    total_scanned > sc.nr_reclaimed + sc.nr_reclaimed / 2)
				sc.may_writepage = 1;

			if (zone->all_unreclaimable) {
				if (end_zone && end_zone == i)
					end_zone--;
				continue;
			}

			if (!zone_watermark_ok_safe(zone, testorder,
					high_wmark_pages(zone), end_zone, 0)) {
				all_zones_ok = 0;
				/*
				 * We are still under min water mark.  This
				 * means that we have a GFP_ATOMIC allocation
				 * failure risk. Hurry up!
				 */
				if (!zone_watermark_ok_safe(zone, order,
					    min_wmark_pages(zone), end_zone, 0))
					has_under_min_watermark_zone = 1;
			} else {
				/*
				 * If a zone reaches its high watermark,
				 * consider it to be no longer congested. It's
				 * possible there are dirty pages backed by
				 * congested BDIs but as pressure is relieved,
				 * speculatively avoid congestion waits
				 */
				zone_clear_flag(zone, ZONE_CONGESTED);
				if (i <= *classzone_idx)
					balanced += zone->present_pages;
			}

		}

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				pfmemalloc_watermark_ok(pgdat))
			wake_up(&pgdat->pfmemalloc_wait);

		if (all_zones_ok || (order && pgdat_balanced(pgdat, balanced, *classzone_idx)))
			break;		/* kswapd: all done */
		/*
		 * OK, kswapd is getting into trouble.  Take a nap, then take
		 * another pass across the zones.
		 */
		if (total_scanned && (sc.priority < DEF_PRIORITY - 2)) {
			if (has_under_min_watermark_zone)
				count_vm_event(KSWAPD_SKIP_CONGESTION_WAIT);
			else
				congestion_wait(BLK_RW_ASYNC, HZ/10);
		}

		/*
		 * We do this so kswapd doesn't build up large priorities for
		 * example when it is freeing in parallel with allocators. It
		 * matches the direct reclaim path behaviour in terms of impact
		 * on zone->*_priority.
		 */
		if (sc.nr_reclaimed >= SWAP_CLUSTER_MAX)
			break;
	} while (--sc.priority >= 0);
out:

	/*
	 * order-0: All zones must meet high watermark for a balanced node
	 * high-order: Balanced zones must make up at least 25% of the node
	 *             for the node to be balanced
	 */
	if (!(all_zones_ok || (order && pgdat_balanced(pgdat, balanced, *classzone_idx)))) {
		cond_resched();

		try_to_freeze();

		/*
		 * Fragmentation may mean that the system cannot be
		 * rebalanced for high-order allocations in all zones.
		 * At this point, if nr_reclaimed < SWAP_CLUSTER_MAX,
		 * it means the zones have been fully scanned and are still
		 * not balanced. For high-order allocations, there is
		 * little point trying all over again as kswapd may
		 * infinite loop.
		 *
		 * Instead, recheck all watermarks at order-0 as they
		 * are the most important. If watermarks are ok, kswapd will go
		 * back to sleep. High-order users can still perform direct
		 * reclaim if they wish.
		 */
		if (sc.nr_reclaimed < SWAP_CLUSTER_MAX)
			order = sc.order = 0;

		goto loop_again;
	}

	/*
	 * If kswapd was reclaiming at a higher order, it has the option of
	 * sleeping without all zones being balanced. Before it does, it must
	 * ensure that the watermarks for order-0 on *all* zones are met and
	 * that the congestion flags are cleared. The congestion flag must
	 * be cleared as kswapd is the only mechanism that clears the flag
	 * and it is potentially going to sleep here.
	 */
	if (order) {
		int zones_need_compaction = 1;

		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;

			if (!populated_zone(zone))
				continue;

			if (zone->all_unreclaimable &&
			    sc.priority != DEF_PRIORITY)
				continue;

			/* Would compaction fail due to lack of free memory? */
			if (COMPACTION_BUILD &&
			    compaction_suitable(zone, order) == COMPACT_SKIPPED)
				goto loop_again;

			/* Confirm the zone is balanced for order-0 */
			if (!zone_watermark_ok(zone, 0,
					high_wmark_pages(zone), 0, 0)) {
				order = sc.order = 0;
				goto loop_again;
			}

			/* Check if the memory needs to be defragmented. */
			if (zone_watermark_ok(zone, order,
				    low_wmark_pages(zone), *classzone_idx, 0))
				zones_need_compaction = 0;

			/* If balanced, clear the congested flag */
			zone_clear_flag(zone, ZONE_CONGESTED);
		}

		if (zones_need_compaction)
			compact_pgdat(pgdat, order);
	}

	/*
	 * Return the order we were reclaiming at so prepare_kswapd_sleep()
	 * makes a decision on the order we were last reclaiming at. However,
	 * if another caller entered the allocator slow path while kswapd
	 * was awake, order will remain at the higher level
	 */
	*classzone_idx = end_zone;
	return order;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int order, int classzone_idx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/* Try to sleep for a short interval */
	if (prepare_kswapd_sleep(pgdat, order, remaining, classzone_idx)) {
		remaining = schedule_timeout(HZ/10);
		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (prepare_kswapd_sleep(pgdat, order, remaining, classzone_idx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned long order, new_order;
	unsigned balanced_order;
	int classzone_idx, new_classzone_idx;
	int balanced_classzone_idx;
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;

	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	lockdep_set_current_reclaim_state(GFP_KERNEL);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	order = new_order = 0;
	balanced_order = 0;
	classzone_idx = new_classzone_idx = pgdat->nr_zones - 1;
	balanced_classzone_idx = classzone_idx;
	for ( ; ; ) {
		int ret;

		/*
		 * If the last balance_pgdat was unsuccessful it's unlikely a
		 * new request of a similar or harder type will succeed soon
		 * so consider going to sleep on the basis we reclaimed at
		 */
		if (balanced_classzone_idx >= new_classzone_idx &&
					balanced_order == new_order) {
			new_order = pgdat->kswapd_max_order;
			new_classzone_idx = pgdat->classzone_idx;
			pgdat->kswapd_max_order =  0;
			pgdat->classzone_idx = pgdat->nr_zones - 1;
		}

		if (order < new_order || classzone_idx > new_classzone_idx) {
			/*
			 * Don't sleep if someone wants a larger 'order'
			 * allocation or has tigher zone constraints
			 */
			order = new_order;
			classzone_idx = new_classzone_idx;
		} else {
			kswapd_try_to_sleep(pgdat, balanced_order,
						balanced_classzone_idx);
			order = pgdat->kswapd_max_order;
			classzone_idx = pgdat->classzone_idx;
			new_order = order;
			new_classzone_idx = classzone_idx;
			pgdat->kswapd_max_order = 0;
			pgdat->classzone_idx = pgdat->nr_zones - 1;
		}

		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (!ret) {
			trace_mm_vmscan_kswapd_wake(pgdat->node_id, order);
			balanced_classzone_idx = classzone_idx;
			balanced_order = balance_pgdat(pgdat, order,
						&balanced_classzone_idx);
		}
	}
	return 0;
}

/*
 * A zone is low on free memory, so wake its kswapd task to service it.
 */
/** 20131116    
 * 해당 zone의 free pages의 수가 WMARK_LOW 아래로 떨어진다면 kswapd를 실행시킨다.
 * 
 * 20131214
 * wakeup_kswapd가 호출되더라도 zone_watermark_ok_safe 라면 바로 리턴한다.
 **/
void wakeup_kswapd(struct zone *zone, int order, enum zone_type classzone_idx)
{
	pg_data_t *pgdat;

	/** 20131116    
	 * present_pages이 설정되어 있지 않다면 바로 리턴
	 **/
	if (!populated_zone(zone))
		return;

	/** 20131116    
	 * zone에서 메모리를 할당받는 게 허용되는지 hardwall 기준으로 검사해
	 * 허용되어 있지 않을 경우 바로 리턴.
	 **/
	if (!cpuset_zone_allowed_hardwall(zone, GFP_KERNEL))
		return;
	/** 20131116    
	 * zone_pgdat를 가져온다.
	 **/
	pgdat = zone->zone_pgdat;
	/** 20131116    
	 * kswapd_max_order 보다 요청 받은 order가 크다면 
	 * order를 새로운 kswapd_max_order로 설정한다. (늘려준다)
	 **/
	if (pgdat->kswapd_max_order < order) {
		pgdat->kswapd_max_order = order;
		/** 20131116    
		 * classzone_idx는 wake_all_swapd인 경우 preferred_zone의 zone_idx값.
		 * pgdat에 저장된 값 중에 더 작은 값을 pgdat에 갱신한다.
		 **/
		pgdat->classzone_idx = min(pgdat->classzone_idx, classzone_idx);
	}
	/** 20131116    
	 * kswapd_wait waitqueue에 대기 중인 task가 없다면 바로 리턴.
	 **/
	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;
	/** 20131116    
	 * low wmark 값보다 free_pages 값이 낮지 않다면 true가 되어 바로 리턴.
	 **/
	if (zone_watermark_ok_safe(zone, order, low_wmark_pages(zone), 0, 0))
		return;

	/** 20131116    
	 * trace용 함수. 분석 생략
	 **/
	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, zone_idx(zone), order);
	/** 20131116    
	 * pgdat->kwapd_wait 에서 sleep 중인 kswapd task를 깨운다.
	 *
	 * 20131214
	 * kswapd는 TASK_INTERRUPTIBLE로 sleep 중이다.
	 **/
	wake_up_interruptible(&pgdat->kswapd_wait);
}

/*
 * The reclaimable count would be mostly accurate.
 * The less reclaimable pages may be
 * - mlocked pages, which will be moved to unevictable list when encountered
 * - mapped pages, which may require several travels to be reclaimed
 * - dirty pages, which is not "instantly" reclaimable
 */
/** 20130914
전역적으로 회수가능한 page수를 리턴 
**/
unsigned long global_reclaimable_pages(void)
{
	int nr;

	nr = global_page_state(NR_ACTIVE_FILE) +
	     global_page_state(NR_INACTIVE_FILE);

	if (nr_swap_pages > 0)
		nr += global_page_state(NR_ACTIVE_ANON) +
		      global_page_state(NR_INACTIVE_ANON);

	return nr;
}

/** 20130914
 * zone에서 회수가능한 page의 수를 리턴.
 **/
unsigned long zone_reclaimable_pages(struct zone *zone)
{
	int nr;

	/** 20130914
	해당 zone의 NR_ACTIVE_FILE, NR_INACTIVE_FILE의 vm_stat을 가져와 
	nr에 더한다.
	**/
	nr = zone_page_state(zone, NR_ACTIVE_FILE) +
	     zone_page_state(zone, NR_INACTIVE_FILE);
	/** 20130914
	nr_swap_pages가 0보다 클 경우
	해당 zone의 NR_ACTIVE_ANON, NR_INACTIVE_ANON의 vm_stat을 가져와 
	nr에 더한다.

	nr_swap_pages 와 ANONYMOUS 타입의 관계는???
	**/
	if (nr_swap_pages > 0)
		nr += zone_page_state(zone, NR_ACTIVE_ANON) +
		      zone_page_state(zone, NR_INACTIVE_ANON);

	return nr;
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct reclaim_state reclaim_state;
	struct scan_control sc = {
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.may_swap = 1,
		.may_unmap = 1,
		.may_writepage = 1,
		.nr_to_reclaim = nr_to_reclaim,
		.hibernation_mode = 1,
		.order = 0,
		.priority = DEF_PRIORITY,
	};
	struct shrink_control shrink = {
		.gfp_mask = sc.gfp_mask,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	struct task_struct *p = current;
	unsigned long nr_reclaimed;

	p->flags |= PF_MEMALLOC;
	lockdep_set_current_reclaim_state(sc.gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc, &shrink);

	p->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	p->flags &= ~PF_MEMALLOC;

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/* It's optimal to keep kswapds on the same CPUs as their memory, but
   not required for correctness.  So if the last cpu in a node goes
   away, we get changed to run anywhere: as the first one comes back,
   restore their cpu bindings. */
static int __devinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action, void *hcpu)
{
	int nid;

	if (action == CPU_ONLINE || action == CPU_ONLINE_FROZEN) {
		for_each_node_state(nid, N_HIGH_MEMORY) {
			pg_data_t *pgdat = NODE_DATA(nid);
			const struct cpumask *mask;

			mask = cpumask_of_node(pgdat->node_id);

			if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
				/* One of our CPUs online: restore mask */
				set_cpus_allowed_ptr(pgdat->kswapd, mask);
		}
	}
	return NOTIFY_OK;
}

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
int kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kswapd)
		return 0;

	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state == SYSTEM_BOOTING);
		printk("Failed to start kswapd on node %d\n",nid);
		ret = -1;
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold lock_memory_hotplug().
 */
void kswapd_stop(int nid)
{
	struct task_struct *kswapd = NODE_DATA(nid)->kswapd;

	if (kswapd) {
		kthread_stop(kswapd);
		NODE_DATA(nid)->kswapd = NULL;
	}
}

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_HIGH_MEMORY)
 		kswapd_run(nid);
	hotcpu_notifier(cpu_callback, 0);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Zone reclaim mode
 *
 * If non-zero call zone_reclaim when the number of free pages falls below
 * the watermarks.
 */
int zone_reclaim_mode __read_mostly;

#define RECLAIM_OFF 0
#define RECLAIM_ZONE (1<<0)	/* Run shrink_inactive_list on the zone */
#define RECLAIM_WRITE (1<<1)	/* Writeout pages during reclaim */
#define RECLAIM_SWAP (1<<2)	/* Swap pages out during reclaim */

/*
 * Priority for ZONE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define ZONE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for zone_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long zone_unmapped_file_pages(struct zone *zone)
{
	unsigned long file_mapped = zone_page_state(zone, NR_FILE_MAPPED);
	unsigned long file_lru = zone_page_state(zone, NR_INACTIVE_FILE) +
		zone_page_state(zone, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static long zone_pagecache_reclaimable(struct zone *zone)
{
	long nr_pagecache_reclaimable;
	long delta = 0;

	/*
	 * If RECLAIM_SWAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and zone_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (zone_reclaim_mode & RECLAIM_SWAP)
		nr_pagecache_reclaimable = zone_page_state(zone, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = zone_unmapped_file_pages(zone);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(zone_reclaim_mode & RECLAIM_WRITE))
		delta += zone_page_state(zone, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this zone through reclaim.
 */
static int __zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	struct reclaim_state reclaim_state;
	struct scan_control sc = {
		.may_writepage = !!(zone_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(zone_reclaim_mode & RECLAIM_SWAP),
		.may_swap = 1,
		.nr_to_reclaim = max_t(unsigned long, nr_pages,
				       SWAP_CLUSTER_MAX),
		.gfp_mask = gfp_mask,
		.order = order,
		.priority = ZONE_RECLAIM_PRIORITY,
	};
	struct shrink_control shrink = {
		.gfp_mask = sc.gfp_mask,
	};
	unsigned long nr_slab_pages0, nr_slab_pages1;

	cond_resched();
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_SWAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_SWAP.
	 */
	p->flags |= PF_MEMALLOC | PF_SWAPWRITE;
	lockdep_set_current_reclaim_state(gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	if (zone_pagecache_reclaimable(zone) > zone->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink zone with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_zone(zone, &sc);
		} while (sc.nr_reclaimed < nr_pages && --sc.priority >= 0);
	}

	nr_slab_pages0 = zone_page_state(zone, NR_SLAB_RECLAIMABLE);
	if (nr_slab_pages0 > zone->min_slab_pages) {
		/*
		 * shrink_slab() does not currently allow us to determine how
		 * many pages were freed in this zone. So we take the current
		 * number of slab pages and shake the slab until it is reduced
		 * by the same nr_pages that we used for reclaiming unmapped
		 * pages.
		 *
		 * Note that shrink_slab will free memory on all zones and may
		 * take a long time.
		 */
		for (;;) {
			unsigned long lru_pages = zone_reclaimable_pages(zone);

			/* No reclaimable slab or very low memory pressure */
			if (!shrink_slab(&shrink, sc.nr_scanned, lru_pages))
				break;

			/* Freed enough memory */
			nr_slab_pages1 = zone_page_state(zone,
							NR_SLAB_RECLAIMABLE);
			if (nr_slab_pages1 + nr_pages <= nr_slab_pages0)
				break;
		}

		/*
		 * Update nr_reclaimed by the number of slab pages we
		 * reclaimed from this zone.
		 */
		nr_slab_pages1 = zone_page_state(zone, NR_SLAB_RECLAIMABLE);
		if (nr_slab_pages1 < nr_slab_pages0)
			sc.nr_reclaimed += nr_slab_pages0 - nr_slab_pages1;
	}

	p->reclaim_state = NULL;
	current->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE);
	lockdep_clear_current_reclaim_state();
	return sc.nr_reclaimed >= nr_pages;
}

int zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	int node_id;
	int ret;

	/*
	 * Zone reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the zone is overallocated. So we do not reclaim
	 * if less than a specified percentage of the zone is used by
	 * unmapped file backed pages.
	 */
	if (zone_pagecache_reclaimable(zone) <= zone->min_unmapped_pages &&
	    zone_page_state(zone, NR_SLAB_RECLAIMABLE) <= zone->min_slab_pages)
		return ZONE_RECLAIM_FULL;

	if (zone->all_unreclaimable)
		return ZONE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!(gfp_mask & __GFP_WAIT) || (current->flags & PF_MEMALLOC))
		return ZONE_RECLAIM_NOSCAN;

	/*
	 * Only run zone reclaim on the local zone or on zones that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	node_id = zone_to_nid(zone);
	if (node_state(node_id, N_CPU) && node_id != numa_node_id())
		return ZONE_RECLAIM_NOSCAN;

	if (zone_test_and_set_flag(zone, ZONE_RECLAIM_LOCKED))
		return ZONE_RECLAIM_NOSCAN;

	ret = __zone_reclaim(zone, gfp_mask, order);
	zone_clear_flag(zone, ZONE_RECLAIM_LOCKED);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/*
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 * @vma: the VMA in which the page is or will be mapped, may be NULL
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.  The vma argument is !NULL when called from the
 * fault path to determine how to instantate a new page.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
/** 20140111
 * mapping되어있는 page가 unevictable하거나, 
 * page가 mlocked설정되어 있다면 0을 리턴한다.
 **/
int page_evictable(struct page *page, struct vm_area_struct *vma)
{

	if (mapping_unevictable(page_mapping(page)))
		return 0;

	if (PageMlocked(page) || (vma && mlocked_vma_newpage(vma, page)))
		return 0;

	return 1;
}

#ifdef CONFIG_SHMEM
/**
 * check_move_unevictable_pages - check pages for evictability and move to appropriate zone lru list
 * @pages:	array of pages to check
 * @nr_pages:	number of pages to check
 *
 * Checks pages for evictability and moves them to the appropriate lru list.
 *
 * This function is only used for SysV IPC SHM_UNLOCK.
 */
void check_move_unevictable_pages(struct page **pages, int nr_pages)
{
	struct lruvec *lruvec;
	struct zone *zone = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = pages[i];
		struct zone *pagezone;

		pgscanned++;
		pagezone = page_zone(page);
		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
		lruvec = mem_cgroup_page_lruvec(page, zone);

		if (!PageLRU(page) || !PageUnevictable(page))
			continue;

		if (page_evictable(page, NULL)) {
			enum lru_list lru = page_lru_base_type(page);

			VM_BUG_ON(PageActive(page));
			ClearPageUnevictable(page);
			del_page_from_lru_list(page, lruvec, LRU_UNEVICTABLE);
			add_page_to_lru_list(page, lruvec, lru);
			pgrescued++;
		}
	}

	if (zone) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		spin_unlock_irq(&zone->lru_lock);
	}
}
#endif /* CONFIG_SHMEM */

static void warn_scan_unevictable_pages(void)
{
	printk_once(KERN_WARNING
		    "%s: The scan_unevictable_pages sysctl/node-interface has been "
		    "disabled for lack of a legitimate use case.  If you have "
		    "one, please send an email to linux-mm@kvack.org.\n",
		    current->comm);
}

/*
 * scan_unevictable_pages [vm] sysctl handler.  On demand re-scan of
 * all nodes' unevictable lists for evictable pages
 */
unsigned long scan_unevictable_pages;

int scan_unevictable_handler(struct ctl_table *table, int write,
			   void __user *buffer,
			   size_t *length, loff_t *ppos)
{
	warn_scan_unevictable_pages();
	proc_doulongvec_minmax(table, write, buffer, length, ppos);
	scan_unevictable_pages = 0;
	return 0;
}

#ifdef CONFIG_NUMA
/*
 * per node 'scan_unevictable_pages' attribute.  On demand re-scan of
 * a specified node's per zone unevictable lists for evictable pages.
 */

static ssize_t read_scan_unevictable_node(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	warn_scan_unevictable_pages();
	return sprintf(buf, "0\n");	/* always zero; should fit... */
}

static ssize_t write_scan_unevictable_node(struct device *dev,
					   struct device_attribute *attr,
					const char *buf, size_t count)
{
	warn_scan_unevictable_pages();
	return 1;
}


static DEVICE_ATTR(scan_unevictable_pages, S_IRUGO | S_IWUSR,
			read_scan_unevictable_node,
			write_scan_unevictable_node);

int scan_unevictable_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_scan_unevictable_pages);
}

void scan_unevictable_unregister_node(struct node *node)
{
	device_remove_file(&node->dev, &dev_attr_scan_unevictable_pages);
}
#endif
