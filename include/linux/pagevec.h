/*
 * include/linux/pagevec.h
 *
 * In many places it is efficient to batch an operation up against multiple
 * pages.  A pagevec is a multipage container which is used for that.
 */

#ifndef _LINUX_PAGEVEC_H
#define _LINUX_PAGEVEC_H

/* 14 pointers + two long's align the pagevec structure to a power of two */
/** 20140104    
 * struct pagevec의 크기를 2의 n 제곱으로 맞춰주기 위한 값.
 *
 * 4 + 4 + (4 * 14) = 64
 **/
#define PAGEVEC_SIZE	14

struct page;
struct address_space;

/** 20140104    
 * pagevec은 page를 vector(덩어리)로 관리하기 위한 자료구조.
 *   cold는 lruvec에서만 사용하는 필드이다.
 *
 * pagevec의 nr개의 page들을 처리하는 동안 spinlock으로 동기화 한다.
 **/
struct pagevec {
	unsigned long nr;
	unsigned long cold;
	struct page *pages[PAGEVEC_SIZE];
};

void __pagevec_release(struct pagevec *pvec);
void __pagevec_lru_add(struct pagevec *pvec, enum lru_list lru);
unsigned pagevec_lookup(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t start, unsigned nr_pages);
unsigned pagevec_lookup_tag(struct pagevec *pvec,
		struct address_space *mapping, pgoff_t *index, int tag,
		unsigned nr_pages);

/** 20140104    
 * pagevec 구조체를 초기화 한다.
 **/
static inline void pagevec_init(struct pagevec *pvec, int cold)
{
	pvec->nr = 0;
	pvec->cold = cold;
}

/** 20140104    
 * pagevec 구조체를 초기화 한다.
 * 왜 cold는 해주지 않은 것일까???
 **/
static inline void pagevec_reinit(struct pagevec *pvec)
{
	pvec->nr = 0;
}
/** 20131221
 * pvec의 nr멤버를 리턴한다.
 **/
static inline unsigned pagevec_count(struct pagevec *pvec)
{
	return pvec->nr;
}

/** 20150111    
 * 하나의 pagevec에서 가리킬 수 있는 page가 몇 개인지 리턴한다.
 **/
static inline unsigned pagevec_space(struct pagevec *pvec)
{
	return PAGEVEC_SIZE - pvec->nr;
}

/*
 * Add a page to a pagevec.  Returns the number of slots still available.
 */
/** 20140111
 * page를 pagevec에 등록시키고, pvec의 남은 slot의 갯수를 리턴한다.
 **/
static inline unsigned pagevec_add(struct pagevec *pvec, struct page *page)
{
	pvec->pages[pvec->nr++] = page;
	return pagevec_space(pvec);
}

static inline void pagevec_release(struct pagevec *pvec)
{
	if (pagevec_count(pvec))
		__pagevec_release(pvec);
}

static inline void __pagevec_lru_add_anon(struct pagevec *pvec)
{
	__pagevec_lru_add(pvec, LRU_INACTIVE_ANON);
}

static inline void __pagevec_lru_add_active_anon(struct pagevec *pvec)
{
	__pagevec_lru_add(pvec, LRU_ACTIVE_ANON);
}

static inline void __pagevec_lru_add_file(struct pagevec *pvec)
{
	__pagevec_lru_add(pvec, LRU_INACTIVE_FILE);
}

static inline void __pagevec_lru_add_active_file(struct pagevec *pvec)
{
	__pagevec_lru_add(pvec, LRU_ACTIVE_FILE);
}

static inline void pagevec_lru_add_file(struct pagevec *pvec)
{
	if (pagevec_count(pvec))
		__pagevec_lru_add_file(pvec);
}

static inline void pagevec_lru_add_anon(struct pagevec *pvec)
{
	if (pagevec_count(pvec))
		__pagevec_lru_add_anon(pvec);
}

#endif /* _LINUX_PAGEVEC_H */
