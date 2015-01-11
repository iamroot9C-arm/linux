#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is page cache page backed by a regular filesystem,
 * or 0 if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
/** 20140104    
 * page가 file cache인지 아닌지 여부를 리턴.
 *
 * page가 regular filesystem에 근거한 page cache page라면 1,
 * 그렇지 않고 anonymous, tmpfs나 ram/swap backed에 근거한 page라면 0을 리턴.
 **/
static inline int page_is_file_cache(struct page *page)
{
	/** 20140104
	 * swap backed라면 anonymous page이므로 file cache가 아니다.
	 * 이에 따라 속하는 LRU가 달라진다.
	 **/
	return !PageSwapBacked(page);
}
/** 20131221
 * lruvec->list에 page를 추가시킨다.
 *
 * 20140118    
 * 추가되는 위치는 head와 head->next 사이, 즉 처음 위치이다.
 *
 * n: 새로 추가되는 페이지
 * o: 이전에 추가된 페이지
 *
 * lru head -> n ->  o  ->  o  ->  o  ->  o
 *   최근 추가된 페이지 -> 오래전 추가된 페이지 순서로 전재
 **/
static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	/** 20131221
	 * nr_pages = 1
	 **/
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
	/** 20131221
	 * lruvec->list에 page를 등록시킨다
	 **/
	list_add(&page->lru, &lruvec->lists[lru]);
	/** 20131221
	 * lruvec로 부터 zone을 구하고 해당 zone의 page state에 nr_pages만큼을 증감시킨다.
	 **/
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, nr_pages);
}

/** 20140104    
 * page를 lru list에서 제거하고 state를 update한다.
 **/
static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	/** 20140104    
	 * page로 대표되는 page들의 수를 가져온다.
	 **/
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
	/** 20140104    
	 * lru list에서 page를 제거.
	 **/
	list_del(&page->lru);
	/** 20140104    
	 * zone의 page state에서 해당 lru에 속해 있는 page들의 수를 감소.
	 **/
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, -nr_pages);
}

/**
 * page_lru_base_type - which LRU list type should a page be on?
 * @page: the page to test
 *
 * Used for LRU list index arithmetic.
 *
 * Returns the base LRU type - file or anon - @page should be on.
 */
/** 20140104    
 * page의 lru base 속성을 리턴한다.
 *
 * page가 file cache라면 LRU_INACTIVE_FILE, 아니라면 LRU_INACTIVE_ANON이다.
 * 이후 active 속성을 추가로 설정하는 방법이 사용됨.
 **/
static inline enum lru_list page_lru_base_type(struct page *page)
{
	if (page_is_file_cache(page))
		return LRU_INACTIVE_FILE;
	return LRU_INACTIVE_ANON;
}

/**
 * page_off_lru - which LRU list was page on? clearing its lru flags.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of LRU
 * lists; and clears its Unevictable or Active flags, ready for freeing.
 */
static __always_inline enum lru_list page_off_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page)) {
		__ClearPageUnevictable(page);
		lru = LRU_UNEVICTABLE;
	} else {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			__ClearPageActive(page);
			lru += LRU_ACTIVE;
		}
	}
	return lru;
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
/** 20140607    
 * page의 flag을 보고 page가 속해야 할 lru type을 리턴.
 **/
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	/** 20140607    
	 * unevictable인 경우 LRU_UNEVICTABLE 리턴.
	 **/
	if (PageUnevictable(page))
		lru = LRU_UNEVICTABLE;
	else {
		/** 20140607    
		 * file, anon 속성을 가져와 active 여부를 추가한다.
		 **/
		lru = page_lru_base_type(page);
		if (PageActive(page))
			lru += LRU_ACTIVE;
	}
	return lru;
}

#endif
