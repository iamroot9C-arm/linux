/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/sysctl/vm.txt.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mm_inline.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/backing-dev.h>
#include <linux/memcontrol.h>
#include <linux/gfp.h>

#include "internal.h"

/* How many pages do we try to swap or page in/out together? */
int page_cluster;

/** 20140104
 * lru_add_pvecs는 LRU 종류의 수만큼 pagevec이 배열로 percpu로 존재한다.
 * lru_add_pvecs : lru cache
 **/
static DEFINE_PER_CPU(struct pagevec[NR_LRU_LISTS], lru_add_pvecs);
static DEFINE_PER_CPU(struct pagevec, lru_rotate_pvecs);
static DEFINE_PER_CPU(struct pagevec, lru_deactivate_pvecs);

/*
 * This path almost never happens for VM activity - pages are normally
 * freed via pagevecs.  But it gets used by networking.
 */
/** 20140111
 * page에 lru플래그가 설정되어 있으면, lru플래그를 클리어하고
 * page를 lru리스트로부터 제거한다.
**/
static void __page_cache_release(struct page *page)
{
	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);
		struct lruvec *lruvec;
		unsigned long flags;

		spin_lock_irqsave(&zone->lru_lock, flags);
		lruvec = mem_cgroup_page_lruvec(page, zone);
		VM_BUG_ON(!PageLRU(page));
		__ClearPageLRU(page);
		del_page_from_lru_list(page, lruvec, page_off_lru(page));
		spin_unlock_irqrestore(&zone->lru_lock, flags);
	}
}

/** 20140111
 * order가 0인(single page)를 lru리스트에서 제거하고,
 * free_hot_cold_page로 해제한다.
 **/
static void __put_single_page(struct page *page)
{
	__page_cache_release(page);
	free_hot_cold_page(page, 0);
}

static void __put_compound_page(struct page *page)
{
	compound_page_dtor *dtor;

	__page_cache_release(page);
	dtor = get_compound_page_dtor(page);
	(*dtor)(page);
}

static void put_compound_page(struct page *page)
{
	if (unlikely(PageTail(page))) {
		/* __split_huge_page_refcount can run under us */
		struct page *page_head = compound_trans_head(page);

		if (likely(page != page_head &&
			   get_page_unless_zero(page_head))) {
			unsigned long flags;

			/*
			 * THP can not break up slab pages so avoid taking
			 * compound_lock().  Slab performs non-atomic bit ops
			 * on page->flags for better performance.  In particular
			 * slab_unlock() in slub used to be a hot path.  It is
			 * still hot on arches that do not support
			 * this_cpu_cmpxchg_double().
			 */
			if (PageSlab(page_head)) {
				if (PageTail(page)) {
					if (put_page_testzero(page_head))
						VM_BUG_ON(1);

					atomic_dec(&page->_mapcount);
					goto skip_lock_tail;
				} else
					goto skip_lock;
			}
			/*
			 * page_head wasn't a dangling pointer but it
			 * may not be a head page anymore by the time
			 * we obtain the lock. That is ok as long as it
			 * can't be freed from under us.
			 */
			flags = compound_lock_irqsave(page_head);
			if (unlikely(!PageTail(page))) {
				/* __split_huge_page_refcount run before us */
				compound_unlock_irqrestore(page_head, flags);
skip_lock:
				if (put_page_testzero(page_head))
					__put_single_page(page_head);
out_put_single:
				if (put_page_testzero(page))
					__put_single_page(page);
				return;
			}
			VM_BUG_ON(page_head != page->first_page);
			/*
			 * We can release the refcount taken by
			 * get_page_unless_zero() now that
			 * __split_huge_page_refcount() is blocked on
			 * the compound_lock.
			 */
			if (put_page_testzero(page_head))
				VM_BUG_ON(1);
			/* __split_huge_page_refcount will wait now */
			VM_BUG_ON(page_mapcount(page) <= 0);
			atomic_dec(&page->_mapcount);
			VM_BUG_ON(atomic_read(&page_head->_count) <= 0);
			VM_BUG_ON(atomic_read(&page->_count) != 0);
			compound_unlock_irqrestore(page_head, flags);

skip_lock_tail:
			if (put_page_testzero(page_head)) {
				if (PageHead(page_head))
					__put_compound_page(page_head);
				else
					__put_single_page(page_head);
			}
		} else {
			/* page_head is a dangling pointer */
			VM_BUG_ON(PageTail(page));
			goto out_put_single;
		}
	} else if (put_page_testzero(page)) {
		if (PageHead(page))
			__put_compound_page(page);
		else
			__put_single_page(page);
	}
}

/** 20140111
 * page의 usage count를 감소.
 * 감소 결과 0이 되면 page를 해제한다.
 **/
void put_page(struct page *page)
{
	/** 20140607
	 * compound page인 경우 put_compound_page로 해제 (분석 생략???)
	 * put page를 해 usage count가 0인 경우 __put_single_page로 해제
	 **/
	if (unlikely(PageCompound(page)))
		put_compound_page(page);
	else if (put_page_testzero(page))
		__put_single_page(page);
}
EXPORT_SYMBOL(put_page);

/*
 * This function is exported but must not be called by anything other
 * than get_page(). It implements the slow path of get_page().
 */
/** 20140111
 * compound 된 page로 부터 page_tail여부를 판단하고 page를 가져올수 있으면 
 * true를 리턴한다???
 * 자세한 분석은 생략???
 **/
bool __get_page_tail(struct page *page)
{
	/*
	 * This takes care of get_page() if run on a tail page
	 * returned by one of the get_user_pages/follow_page variants.
	 * get_user_pages/follow_page itself doesn't need the compound
	 * lock because it runs __get_page_tail_foll() under the
	 * proper PT lock that already serializes against
	 * split_huge_page().
	 */
	unsigned long flags;
	bool got = false;
	struct page *page_head = compound_trans_head(page);

	if (likely(page != page_head && get_page_unless_zero(page_head))) {

		/* Ref to put_compound_page() comment. */
		if (PageSlab(page_head)) {
			if (likely(PageTail(page))) {
				__get_page_tail_foll(page, false);
				return true;
			} else {
				put_page(page_head);
				return false;
			}
		}

		/*
		 * page_head wasn't a dangling pointer but it
		 * may not be a head page anymore by the time
		 * we obtain the lock. That is ok as long as it
		 * can't be freed from under us.
		 */
		flags = compound_lock_irqsave(page_head);
		/* here __split_huge_page_refcount won't run anymore */
		if (likely(PageTail(page))) {
			__get_page_tail_foll(page, false);
			got = true;
		}
		compound_unlock_irqrestore(page_head, flags);
		if (unlikely(!got))
			put_page(page_head);
	}
	return got;
}
EXPORT_SYMBOL(__get_page_tail);

/**
 * put_pages_list() - release a list of pages
 * @pages: list of pages threaded on page->lru
 *
 * Release a list of pages which are strung together on page.lru.  Currently
 * used by read_cache_pages() and related error recovery code.
 */
void put_pages_list(struct list_head *pages)
{
	while (!list_empty(pages)) {
		struct page *victim;

		victim = list_entry(pages->prev, struct page, lru);
		list_del(&victim->lru);
		page_cache_release(victim);
	}
}
EXPORT_SYMBOL(put_pages_list);

/*
 * get_kernel_pages() - pin kernel pages in memory
 * @kiov:	An array of struct kvec structures
 * @nr_segs:	number of segments to pin
 * @write:	pinning for read/write, currently ignored
 * @pages:	array that receives pointers to the pages pinned.
 *		Should be at least nr_segs long.
 *
 * Returns number of pages pinned. This may be fewer than the number
 * requested. If nr_pages is 0 or negative, returns 0. If no pages
 * were pinned, returns -errno. Each page returned must be released
 * with a put_page() call when it is finished with.
 */
int get_kernel_pages(const struct kvec *kiov, int nr_segs, int write,
		struct page **pages)
{
	int seg;

	for (seg = 0; seg < nr_segs; seg++) {
		if (WARN_ON(kiov[seg].iov_len != PAGE_SIZE))
			return seg;

		pages[seg] = kmap_to_page(kiov[seg].iov_base);
		page_cache_get(pages[seg]);
	}

	return seg;
}
EXPORT_SYMBOL_GPL(get_kernel_pages);

/*
 * get_kernel_page() - pin a kernel page in memory
 * @start:	starting kernel address
 * @write:	pinning for read/write, currently ignored
 * @pages:	array that receives pointer to the page pinned.
 *		Must be at least nr_segs long.
 *
 * Returns 1 if page is pinned. If the page was not pinned, returns
 * -errno. The page returned must be released with a put_page() call
 * when it is finished with.
 */
int get_kernel_page(unsigned long start, int write, struct page **pages)
{
	const struct kvec kiov = {
		.iov_base = (void *)start,
		.iov_len = PAGE_SIZE
	};

	return get_kernel_pages(&kiov, 1, write, pages);
}
EXPORT_SYMBOL_GPL(get_kernel_page);

/** 20140104
 * pagevec으로 참조되는 page들을 arg(분석한 path에서는 lru)로 이동시키는 함수.
 *   이동하는 동작을 수행할 함수는 move_fn으로 전달 받는다.
 * lru에서 move 시킨 뒤에 release_pages를 호출.
 **/
static void pagevec_lru_move_fn(struct pagevec *pvec,
	void (*move_fn)(struct page *page, struct lruvec *lruvec, void *arg),
	void *arg)
{
	int i;
	struct zone *zone = NULL;
	struct lruvec *lruvec;
	unsigned long flags = 0;
	/** 20131221
	 * pvec의 nr갯수만큼 루프를 돈다.
	 * pvec의 pages[i]가 포함된 zone에 
	 **/
	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];
		struct zone *pagezone = page_zone(page);
		/** 20131221
		 * 현재 page가 loop문 이전에 옮겨준 page와 다른 zone에 있으면
		 * 이전 zone의 lock을 풀고 현재 zone의 lock을 건다.
		 **/
		if (pagezone != zone) {
			if (zone)
				spin_unlock_irqrestore(&zone->lru_lock, flags);
			zone = pagezone;
			spin_lock_irqsave(&zone->lru_lock, flags);
		}

		/** 20140104
		 * mem cgroup을 사용하지 않을 경우 일반적인 zone의 lruvec을 가져온다.
		 **/
		lruvec = mem_cgroup_page_lruvec(page, zone);
		/** 20131221
		 * 각 page를 move_fn 함수를 호출해 처리한다.
		 **/
		(*move_fn)(page, lruvec, arg);
	}
	if (zone)
		spin_unlock_irqrestore(&zone->lru_lock, flags);

	/** 20140104
	 * move_fn로 이동되었으므로
	 *   pvec->nr 개수만큼의 page들을 각각 release 한다.
	 **/
	release_pages(pvec->pages, pvec->nr, pvec->cold);
	pagevec_reinit(pvec);
}

/** 20140104
 * page를 page type에 따라 새로운 INACTIVE lru list에 등록.
 **/
static void pagevec_move_tail_fn(struct page *page, struct lruvec *lruvec,
				 void *arg)
{
	int *pgmoved = arg;

	/** 20140104
	 * page가 LRU에 등록되어 있고, Active 상태가 아니고, evictable이라면
	 **/
	if (PageLRU(page) && !PageActive(page) && !PageUnevictable(page)) {
		/** 20140104
		 * page의 type에 해당하는 INACTIVE LRU를 가져온다. 
		 **/
		enum lru_list lru = page_lru_base_type(page);
		list_move_tail(&page->lru, &lruvec->lists[lru]);
		/** 20140104
		 * pgmoved를 하나 증가
		 **/
		(*pgmoved)++;
	}
}

/*
 * pagevec_move_tail() must be called with IRQ disabled.
 * Otherwise this may cause nasty races.
 */
/** 20140104
 * pvec에 들어있는 page들을 pagevec_move_tail_fn을 통해
 * 새로운 lru list의 tail에 추가시킨다.
 **/
static void pagevec_move_tail(struct pagevec *pvec)
{
	int pgmoved = 0;

	pagevec_lru_move_fn(pvec, pagevec_move_tail_fn, &pgmoved);
	__count_vm_events(PGROTATED, pgmoved);
}

/*
 * Writeback is about to end against a page which has been marked for immediate
 * reclaim.  If it still appears to be reclaimable, move it to the tail of the
 * inactive list.
 */
void rotate_reclaimable_page(struct page *page)
{
	if (!PageLocked(page) && !PageDirty(page) && !PageActive(page) &&
	    !PageUnevictable(page) && PageLRU(page)) {
		struct pagevec *pvec;
		unsigned long flags;

		page_cache_get(page);
		local_irq_save(flags);
		pvec = &__get_cpu_var(lru_rotate_pvecs);
		if (!pagevec_add(pvec, page))
			pagevec_move_tail(pvec);
		local_irq_restore(flags);
	}
}

/** 20131221
 * lruvec의 reclaim_stat의 scanned값을 증가시킨다.
 * lru가 active일 경우 reclaim_stat의 rotated값도 같이 증가시킨다.
 **/
static void update_page_reclaim_stat(struct lruvec *lruvec,
				     int file, int rotated)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

	reclaim_stat->recent_scanned[file]++;
	if (rotated)
		reclaim_stat->recent_rotated[file]++;
}

/** 20140104
 * page를 기존의 lru list에서 제거하고 (보통 cpu lru list),
 * active 시켜 zone의 해당 타입에 맞는 active lru list에 등록한다.
 **/
static void __activate_page(struct page *page, struct lruvec *lruvec,
			    void *arg)
{
	/** 20140104
	 * page가 LRU list에 속해 있고, Active 하지 않고, evictable 하다면
	 **/
	if (PageLRU(page) && !PageActive(page) && !PageUnevictable(page)) {
		/** 20140104
		 * lru의 flie/anon 여부, base type 을 가져옴
		 **/
		int file = page_is_file_cache(page);
		int lru = page_lru_base_type(page);

		/** 20140104
		 * page를 현재 lru에서 제거한다.
		 **/
		del_page_from_lru_list(page, lruvec, lru);
		/** 20140104
		 * page의 속성을 active로 만든다.
		 **/
		SetPageActive(page);
		/** 20140104
		 * inactive lru -> active lru로 옮긴다.
		 **/
		lru += LRU_ACTIVE;
		add_page_to_lru_list(page, lruvec, lru);

		/** 20140104
		 * PGACTIVATE event를 업데이트한다.
		 **/
		__count_vm_event(PGACTIVATE);
		/** 20140104
		 * reclaim stat를 업데이트 한다.
		 **/
		update_page_reclaim_stat(lruvec, file, 1);
	}
}

#ifdef CONFIG_SMP
static DEFINE_PER_CPU(struct pagevec, activate_page_pvecs);

/** 20140104
 * cpu의 lru list에서 제거하고, active시켜 zone의 lru list에 등록시킨다.
 **/
static void activate_page_drain(int cpu)
{
	/** 20140104
	 * cpu에 해당하는 activate_page_pvecs pagevec을 가져옴
	 **/
	struct pagevec *pvec = &per_cpu(activate_page_pvecs, cpu);

	/** 20140104
	 * pvec이 비어 있지 않다면 activate_page로 move 한다.
	 **/
	if (pagevec_count(pvec))
		pagevec_lru_move_fn(pvec, __activate_page, NULL);
}

void activate_page(struct page *page)
{
	if (PageLRU(page) && !PageActive(page) && !PageUnevictable(page)) {
		struct pagevec *pvec = &get_cpu_var(activate_page_pvecs);

		page_cache_get(page);
		if (!pagevec_add(pvec, page))
			pagevec_lru_move_fn(pvec, __activate_page, NULL);
		put_cpu_var(activate_page_pvecs);
	}
}

#else
static inline void activate_page_drain(int cpu)
{
}

void activate_page(struct page *page)
{
	struct zone *zone = page_zone(page);

	spin_lock_irq(&zone->lru_lock);
	__activate_page(page, mem_cgroup_page_lruvec(page, zone), NULL);
	spin_unlock_irq(&zone->lru_lock);
}
#endif

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 */
void mark_page_accessed(struct page *page)
{
	if (!PageActive(page) && !PageUnevictable(page) &&
			PageReferenced(page) && PageLRU(page)) {
		activate_page(page);
		ClearPageReferenced(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
}
EXPORT_SYMBOL(mark_page_accessed);

/** 20140111
 * page를 lru cache (percpu로 존재하는 pagevec)에 추가한다.
 *
 * page를 percpu의 lru pagevec에 추가시키고,
 * lru pagevec이 다 찼으면 pagevec을 zone에 등록시킨다.
 **/
void __lru_cache_add(struct page *page, enum lru_list lru)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_pvecs)[lru];

	page_cache_get(page);
	if (!pagevec_add(pvec, page))
		__pagevec_lru_add(pvec, lru);
	put_cpu_var(lru_add_pvecs);
}
EXPORT_SYMBOL(__lru_cache_add);

/**
 * lru_cache_add_lru - add a page to a page list
 * @page: the page to be added to the LRU.
 * @lru: the LRU list to which the page is added.
 */
/** 20140111
 * page의 active 속성과 unevictable 속성을 제거하여
 * percpu인 lru cache에 추가한다.
 **/
void lru_cache_add_lru(struct page *page, enum lru_list lru)
{
	if (PageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		ClearPageActive(page);
	} else if (PageUnevictable(page)) {
		VM_BUG_ON(PageActive(page));
		ClearPageUnevictable(page);
	}

	VM_BUG_ON(PageLRU(page) || PageActive(page) || PageUnevictable(page));
	__lru_cache_add(page, lru);
}

/**
 * add_page_to_unevictable_list - add a page to the unevictable list
 * @page:  the page to be added to the unevictable list
 *
 * Add page directly to its zone's unevictable list.  To avoid races with
 * tasks that might be making the page evictable, through eg. munlock,
 * munmap or exit, while it's not on the lru, we want to add the page
 * while it's locked or otherwise "invisible" to other tasks.  This is
 * difficult to do when using the pagevec cache, so bypass that.
 */
/** 20140111
 * page의 unevictable 플래그 및 lru 플래그를 set하고, 
 * lruvec의 LRU_UNEVICTABLE리스트에 page를 추가한다.
 **/
void add_page_to_unevictable_list(struct page *page)
{
	struct zone *zone = page_zone(page);
	struct lruvec *lruvec;

	spin_lock_irq(&zone->lru_lock);
	lruvec = mem_cgroup_page_lruvec(page, zone);
	SetPageUnevictable(page);
	SetPageLRU(page);
	add_page_to_lru_list(page, lruvec, LRU_UNEVICTABLE);
	spin_unlock_irq(&zone->lru_lock);
}

/*
 * If the page can not be invalidated, it is moved to the
 * inactive list to speed up its reclaim.  It is moved to the
 * head of the list, rather than the tail, to give the flusher
 * threads some time to write it out, as this is much more
 * effective than the single-page writeout from reclaim.
 *
 * If the page isn't page_mapped and dirty/writeback, the page
 * could reclaim asap using PG_reclaim.
 *
 * 1. active, mapped page -> none
 * 2. active, dirty/writeback page -> inactive, head, PG_reclaim
 * 3. inactive, mapped page -> none
 * 4. inactive, dirty/writeback page -> inactive, head, PG_reclaim
 * 5. inactive, clean -> inactive, tail
 * 6. Others -> none
 *
 * In 4, why it moves inactive's head, the VM expects the page would
 * be write it out by flusher threads as this is much more effective
 * than the single-page writeout from reclaim.
 */
/** 20140104
 * active lru list에서 zone lruvec의 inactive lru list로 이동시킨다.
 **/
static void lru_deactivate_fn(struct page *page, struct lruvec *lruvec,
			      void *arg)
{
	int lru, file;
	bool active;

	/** 20140104
	 * page가 LRU에 속해 있지 않다면 return
	 **/
	if (!PageLRU(page))
		return;

	/** 20140104
	 * page가 Unevictable 하다면 return
	 **/
	if (PageUnevictable(page))
		return;

	/* Some processes are using the page */
	/** 20140104
	 * page가 page_mapped라면 리턴
	 **/
	if (page_mapped(page))
		return;

	/** 20140104
	 * page의 현재 active 여부, file_cache 여부, lru base type을 가져옴
	 **/
	active = PageActive(page);
	file = page_is_file_cache(page);
	lru = page_lru_base_type(page);

	/** 20140104
	 * page를 lru list에서 제거한다.
	 **/
	del_page_from_lru_list(page, lruvec, lru + active);
	/** 20140104
	 * flags에서 Active 속성을 제거
	 **/
	ClearPageActive(page);
	/** 20140104
	 * flags에서 refereced 속성을 제거
	 **/
	ClearPageReferenced(page);
	/** 20140104
	 * active 속성만 제거하고 zone의 inactive lru list에 추가한다.
	 **/
	add_page_to_lru_list(page, lruvec, lru);

	/** 20140104
	 * page가 page_mapped가 아니면서 writeback이거나 dirty라면
	 **/
	if (PageWriteback(page) || PageDirty(page)) {
		/*
		 * PG_reclaim could be raced with end_page_writeback
		 * It can make readahead confusing.  But race window
		 * is _really_ small and  it's non-critical problem.
		 */
		/** 20140104
		 * page에 PG_reclaim으로 표시
		 **/
		SetPageReclaim(page);
	} else {
		/** 20140104
		 * 그렇지 않다면 새로운 inactive lru list로 옮긴다.
		 **/
		/*
		 * The page's writeback ends up during pagevec
		 * We moves tha page into tail of inactive.
		 */
		list_move_tail(&page->lru, &lruvec->lists[lru]);
		/** 20140104
		 * ROTATED 이벤트를 갱신
		 **/
		__count_vm_event(PGROTATED);
	}

	/** 20140104
	 * page가 기존에 active 상태였다면 deactivate 정보를 갱신한다.
	 **/
	if (active)
		__count_vm_event(PGDEACTIVATE);
	
	/** 20140104
	 * reclaim stat을 증가시킨다.
	 *   scanned는 증가시키고, rotate는 증가시키지 않는다.
	 **/
	update_page_reclaim_stat(lruvec, file, 0);
}

/*
 * Drain pages out of the cpu's pagevecs.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
/** 20140104
 * cpu pagevecs에 있던 pages 들을 zone의 lruvec의 해당하는 lru list로 옮긴다.
 **/
void lru_add_drain_cpu(int cpu)
{
	/** 20131221
	 * cpu번호에 해당하는 per cpu에 lru_add_pvecs의 위치를 받아온다.
	 **/
	struct pagevec *pvecs = per_cpu(lru_add_pvecs, cpu);
	struct pagevec *pvec;
	int lru;

	/** 20140104
	 * percpu lru_add_pvecs의 lru list 별 pagevec에 대해
	 **/
	for_each_lru(lru) {
		pvec = &pvecs[lru - LRU_BASE];
		/** 20140104
		 * pagevec에 등록된 page가 있다면, 해당 page들을 lru에 추가한다.
		 **/
		if (pagevec_count(pvec))
			__pagevec_lru_add(pvec, lru);
	}

	/** 20140104
	 * 현재 cpu에 해당하는 lru_rotate_pvecs 변수를 가져온다.
	 **/
	pvec = &per_cpu(lru_rotate_pvecs, cpu);
	/** 20140104
	 * 해당 cpu의 lru_rotate_pvecs에 page가 존재하면
	 **/
	if (pagevec_count(pvec)) {
		unsigned long flags;

		/* No harm done if a racing interrupt already did this */
		local_irq_save(flags);
		/** 20140104
		 * percpu lru list에서 제거해
		 * zone의 INACTIVE lru list에 등록한다.
		 **/
		pagevec_move_tail(pvec);
		local_irq_restore(flags);
	}

	/** 20140104
	 * 현재 cpu에 해당하는 lru_deactivate_pvecs 변수를 가져온다.
	 **/
	pvec = &per_cpu(lru_deactivate_pvecs, cpu);
	/** 20140104
	 * 해당 cpu의 lru_deactivate_pvecs에 page가 존재하면
	 **/
	if (pagevec_count(pvec))
		/** 20140104
		 * percpu lru list에서 제거해
		 * zone lruvec의 inactive lru list에 등록한다.
		 **/
		pagevec_lru_move_fn(pvec, lru_deactivate_fn, NULL);

	/** 20140104
	 * cpu의 activate_page_pvecs의 lru에서 제거하고,
	 *   zone lruvec의 active lru list에 추가한다.
	 **/
	activate_page_drain(cpu);
}

/**
 * deactivate_page - forcefully deactivate a page
 * @page: page to deactivate
 *
 * This function hints the VM that @page is a good reclaim candidate,
 * for example if its invalidation fails due to the page being dirty
 * or under writeback.
 */
void deactivate_page(struct page *page)
{
	/*
	 * In a workload with many unevictable page such as mprotect, unevictable
	 * page deactivation for accelerating reclaim is pointless.
	 */
	if (PageUnevictable(page))
		return;

	if (likely(get_page_unless_zero(page))) {
		struct pagevec *pvec = &get_cpu_var(lru_deactivate_pvecs);

		if (!pagevec_add(pvec, page))
			pagevec_lru_move_fn(pvec, lru_deactivate_fn, NULL);
		put_cpu_var(lru_deactivate_pvecs);
	}
}

/** 20140104
 * 해당 cpu의 lru list에 머물러 있던 page들을 zone의 lru list로 옮긴다.
 **/
void lru_add_drain(void)
{
	/** 20140104
	 * cpu의 lru list에 있던 page들을 zone의 해당 lru list로 옮긴다.
	 **/
	lru_add_drain_cpu(get_cpu());
	/** 20140104
	 * 선점 가능 여부를 리턴한다.
	 **/
	put_cpu();
}

static void lru_add_drain_per_cpu(struct work_struct *dummy)
{
	lru_add_drain();
}

/*
 * Returns 0 for success
 */
int lru_add_drain_all(void)
{
	return schedule_on_each_cpu(lru_add_drain_per_cpu);
}

/*
 * Batched page_cache_release().  Decrement the reference count on all the
 * passed pages.  If it fell to zero then remove the page from the LRU and
 * free it.
 *
 * Avoid taking zone->lru_lock if possible, but if it is taken, retain it
 * for the remainder of the operation.
 *
 * The locking in this function is against shrink_inactive_list(): we recheck
 * the page count inside the lock to see whether shrink_inactive_list()
 * grabbed the page via the LRU.  If it did, give up: shrink_inactive_list()
 * will free it.
 */
/** 20140104
 * page의 usage count를 감소시키고, 그 결과 0가 되었다면
 *   page를 lru에서 제거하고, free 한다.
 **/
void release_pages(struct page **pages, int nr, int cold)
{
	int i;
	LIST_HEAD(pages_to_free);
	struct zone *zone = NULL;
	struct lruvec *lruvec;
	unsigned long uninitialized_var(flags);

	for (i = 0; i < nr; i++) {
		struct page *page = pages[i];

		/** 20140104
		 * Compound Page라면 수행
		 *   - lock을 풀고, zone은 NULL
		 **/
		if (unlikely(PageCompound(page))) {
			if (zone) {
				spin_unlock_irqrestore(&zone->lru_lock, flags);
				zone = NULL;
			}
			put_compound_page(page);
			continue;
		}

		/** 20140104
		 * page의 usage count를 감소시키고, 그 결과 0이면 이후 라인 진행
		 **/
		if (!put_page_testzero(page))
			continue;

		/** 20140104
		 * LRU에 등록된 page라면
		 **/
		if (PageLRU(page)) {
			/** 20140104
			 * page의 zone 정보를 얻어와서
			 **/
			struct zone *pagezone = page_zone(page);

			/** 20140104
			 * page가 속한 zone과 zone 정보가 다르면
			 **/
			if (pagezone != zone) {
				if (zone)
					spin_unlock_irqrestore(&zone->lru_lock,
									flags);
				/** 20140104
				 * zone을 page가 속한 zone에 대한 포인터로 만든다.
				 **/
				zone = pagezone;
				spin_lock_irqsave(&zone->lru_lock, flags);
			}

			lruvec = mem_cgroup_page_lruvec(page, zone);
			VM_BUG_ON(!PageLRU(page));
			/** 20140104
			 * page의 flags에서 LRU에 해당하는 bit속성을 clear.
			 **/
			__ClearPageLRU(page);
			del_page_from_lru_list(page, lruvec, page_off_lru(page));
		}

		/** 20140104
		 * 한 번에 등록해 주기 위해 함수 내 지역변수에 우선 등록해 둔다.
		 **/
		list_add(&page->lru, &pages_to_free);
	}
	if (zone)
		spin_unlock_irqrestore(&zone->lru_lock, flags);

	/** 20140104
	 * hot/cold 여부에 따라 pages_to_free 개수만큼 page를 해제 한다.
	 **/
	free_hot_cold_page_list(&pages_to_free, cold);
}
EXPORT_SYMBOL(release_pages);

/*
 * The pages which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those pages may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __pagevec_release() will drain those queues here.  __pagevec_lru_add()
 * and __pagevec_lru_add_active() call release_pages() directly to avoid
 * mutual recursion.
 */
void __pagevec_release(struct pagevec *pvec)
{
	lru_add_drain();
	release_pages(pvec->pages, pagevec_count(pvec), pvec->cold);
	pagevec_reinit(pvec);
}
EXPORT_SYMBOL(__pagevec_release);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* used by __split_huge_page_refcount() */
void lru_add_page_tail(struct page *page, struct page *page_tail,
		       struct lruvec *lruvec)
{
	int uninitialized_var(active);
	enum lru_list lru;
	const int file = 0;

	VM_BUG_ON(!PageHead(page));
	VM_BUG_ON(PageCompound(page_tail));
	VM_BUG_ON(PageLRU(page_tail));
	VM_BUG_ON(NR_CPUS != 1 &&
		  !spin_is_locked(&lruvec_zone(lruvec)->lru_lock));

	SetPageLRU(page_tail);

	if (page_evictable(page_tail, NULL)) {
		if (PageActive(page)) {
			SetPageActive(page_tail);
			active = 1;
			lru = LRU_ACTIVE_ANON;
		} else {
			active = 0;
			lru = LRU_INACTIVE_ANON;
		}
	} else {
		SetPageUnevictable(page_tail);
		lru = LRU_UNEVICTABLE;
	}

	if (likely(PageLRU(page)))
		list_add_tail(&page_tail->lru, &page->lru);
	else {
		struct list_head *list_head;
		/*
		 * Head page has not yet been counted, as an hpage,
		 * so we must account for each subpage individually.
		 *
		 * Use the standard add function to put page_tail on the list,
		 * but then correct its position so they all end up in order.
		 */
		add_page_to_lru_list(page_tail, lruvec, lru);
		list_head = page_tail->lru.prev;
		list_move_tail(&page_tail->lru, list_head);
	}

	if (!PageUnevictable(page))
		update_page_reclaim_stat(lruvec, file, active);
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/** 20131221
 * page를 지정된 lruvec의 '인자(arg)로 넘어온 lru리스트'에 등록시켜주고 reclaim_stat을 갱신한다.
 **/
static void __pagevec_lru_add_fn(struct page *page, struct lruvec *lruvec,
				 void *arg)
{
	enum lru_list lru = (enum lru_list)arg;
	int file = is_file_lru(lru);
	int active = is_active_lru(lru);

	VM_BUG_ON(PageActive(page));
	VM_BUG_ON(PageUnevictable(page));
	VM_BUG_ON(PageLRU(page));
	/** 20131221
	 * page의 flag에서 LRU에 해당하는 bit를 Set해준다.
	 * page가 LRU리스트에 속한다는 의미임
	 **/
	SetPageLRU(page);
	/** 20131221
	 * 등록할 lru가 active일경우 page의 flag속성에 active bit를 set해준다
	 **/
	if (active)
		SetPageActive(page);

	/** 20140104
	 * lru list에 page를 추가한다.
	 **/
	add_page_to_lru_list(page, lruvec, lru);
	update_page_reclaim_stat(lruvec, file, active);
}

/*
 * Add the passed pages to the LRU, then drop the caller's refcount
 * on them.  Reinitialises the caller's pagevec.
 */
/** 20140104
 * pvec으로 가리키던 page들을 zone의 lru list로 이동시킨다.
 *   move 동작을 수행할 함수는 __pagevec_lru_add_fn를 지정한다.
 **/
void __pagevec_lru_add(struct pagevec *pvec, enum lru_list lru)
{
	/** 20140104
	 * lru가 unevictable_lru이면 BUG.
	 **/
	VM_BUG_ON(is_unevictable_lru(lru));

	/** 20140104
	 **/
	pagevec_lru_move_fn(pvec, __pagevec_lru_add_fn, (void *)lru);
}
EXPORT_SYMBOL(__pagevec_lru_add);

/**
 * pagevec_lookup - gang pagecache lookup
 * @pvec:	Where the resulting pages are placed
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @nr_pages:	The maximum number of pages
 *
 * pagevec_lookup() will search for and return a group of up to @nr_pages pages
 * in the mapping.  The pages are placed in @pvec.  pagevec_lookup() takes a
 * reference against the pages in @pvec.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 *
 * pagevec_lookup() returns the number of pages which were found.
 */
unsigned pagevec_lookup(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t start, unsigned nr_pages)
{
	pvec->nr = find_get_pages(mapping, start, nr_pages, pvec->pages);
	return pagevec_count(pvec);
}
EXPORT_SYMBOL(pagevec_lookup);

unsigned pagevec_lookup_tag(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t *index, int tag, unsigned nr_pages)
{
	pvec->nr = find_get_pages_tag(mapping, index, tag,
					nr_pages, pvec->pages);
	return pagevec_count(pvec);
}
EXPORT_SYMBOL(pagevec_lookup_tag);

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = totalram_pages >> (20 - PAGE_SHIFT);

#ifdef CONFIG_SWAP
	bdi_init(swapper_space.backing_dev_info);
#endif

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */
}
