/*
 * High memory handling common code and variables.
 *
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with
 * 64-bit physical space. With current x86 CPUs this
 * means up to 64 Gigabytes physical RAM.
 *
 * Rewrote high memory support to move the page cache into
 * high memory. Implemented permanent (schedulable) kmaps
 * based on Linus' idea.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <linux/kgdb.h>
#include <asm/tlbflush.h>


#if defined(CONFIG_HIGHMEM) || defined(CONFIG_X86_32)
DEFINE_PER_CPU(int, __kmap_atomic_idx);
#endif

/*
 * Virtual_count is not a pure "count".
 *  0 means that it is not mapped, and has not been mapped
 *    since a TLB flush - it is usable.
 *  1 means that there are no users, but it has been mapped
 *    since the last TLB flush - so we can't use it.
 *  n means that there are (n-1) current users of it.
 */
#ifdef CONFIG_HIGHMEM

unsigned long totalhigh_pages __read_mostly;
EXPORT_SYMBOL(totalhigh_pages);


EXPORT_PER_CPU_SYMBOL(__kmap_atomic_idx);

unsigned int nr_free_highpages (void)
{
	pg_data_t *pgdat;
	unsigned int pages = 0;

	for_each_online_pgdat(pgdat) {
		pages += zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
		if (zone_movable_is_highmem())
			pages += zone_page_state(
					&pgdat->node_zones[ZONE_MOVABLE],
					NR_FREE_PAGES);
	}

	return pages;
}

static int pkmap_count[LAST_PKMAP];
static unsigned int last_pkmap_nr;
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(kmap_lock);

pte_t * pkmap_page_table;

static DECLARE_WAIT_QUEUE_HEAD(pkmap_map_wait);

/*
 * Most architectures have no use for kmap_high_get(), so let's abstract
 * the disabling of IRQ out of the locking in that case to save on a
 * potential useless overhead.
 */
/** 20131012
  1. lock_kmap_any(flags)에 대하여,
  * ARCH_NEEDS_KMAP_HIGH_GET이 define되어있으면, irq 상태 플래그를 저장한 뒤,disable하고 스핀락을 해준다.
  * ARCH_NEEDS_KMAP_HIGH_GET이 define되어있지 않으면, 스핀락만 해준다.
  * vexpress_defconfig에서는 ARCH_NEEDS_KMAP_HIGH_GET이 define된다
  2. unlock_kmap_any(flags)에 대하여,
  * ARCH_NEEDS_KMAP_HIGH_GET이 define되어있으면, irq 상태 플래그를 복구한 뒤 스핀락을 해제한다.
  * ARCH_NEEDS_KMAP_HIGH_GET이 define되어있지 않으면, 스핀락만 해제한다.
  * vexpress_defconfig에서는 ARCH_NEEDS_KMAP_HIGH_GET이 define된다

#define ARCH_NEEDS_KMAP_HIGH_GET
#if defined(CONFIG_SMP) && defined(CONFIG_CPU_TLB_V6)
#undef ARCH_NEEDS_KMAP_HIGH_GET

**/
#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/** 20131026    
 * irq disable 시킨 뒤 spin_lock 획득
 **/
#define lock_kmap()             spin_lock_irq(&kmap_lock)
#define unlock_kmap()           spin_unlock_irq(&kmap_lock)
#define lock_kmap_any(flags)    spin_lock_irqsave(&kmap_lock, flags)
#define unlock_kmap_any(flags)  spin_unlock_irqrestore(&kmap_lock, flags)
#else
#define lock_kmap()             spin_lock(&kmap_lock)
#define unlock_kmap()           spin_unlock(&kmap_lock)
#define lock_kmap_any(flags)    \
		do { spin_lock(&kmap_lock); (void)(flags); } while (0)
#define unlock_kmap_any(flags)  \
		do { spin_unlock(&kmap_lock); (void)(flags); } while (0)
#endif

struct page *kmap_to_page(void *vaddr)
{
	unsigned long addr = (unsigned long)vaddr;

	if (addr >= PKMAP_ADDR(0) && addr <= PKMAP_ADDR(LAST_PKMAP)) {
		int i = (addr - PKMAP_ADDR(0)) >> PAGE_SHIFT;
		return pte_page(pkmap_page_table[i]);
	}

	return virt_to_page(addr);
}

static void flush_all_zero_pkmaps(void)
{
	int i;
	int need_flush = 0;

	flush_cache_kmaps();

	/** 20131026    
	 * 0 ~ LAST_PKMAP까지 loop
	 **/
	for (i = 0; i < LAST_PKMAP; i++) {
		struct page *page;

		/*
		 * zero means we don't have anything to do,
		 * >1 means that it is still in use. Only
		 * a count of 1 means that it is free but
		 * needs to be unmapped
		 */
		/** 20131026    
		 * pkmap_count == 0 이면 mapping 되어 있지 않은 entry.
		 * pkmap_count >  1 이면 다른 곳에서(e.g. kmap_atomic의 kmap_get_high)
		 *                 잡고 있는 경우.
		 **/
		if (pkmap_count[i] != 1)
			continue;
		/** 20131026    
		 * pkmap_count[i] 가 1이었는데 0으로 설정하겠다.
		 **/
		pkmap_count[i] = 0;

		/* sanity check */
		/** 20131026    
		 * 해당 영역의 pte entry가 들어있어야 하는데 pte_none이면 BUG.
		 **/
		BUG_ON(pte_none(pkmap_page_table[i]));

		/*
		 * Don't need an atomic fetch-and-clear op here;
		 * no-one has the page mapped, and cannot get at
		 * its virtual address (and hence PTE) without first
		 * getting the kmap_lock (which is held here).
		 * So no dangers, even with speculative execution.
		 */
		/** 20131026    
		 * pkmap_page_table에서 i번째 pte entry를 가져와
		 * 그에 해당하는 struct page *를 얻는다.
		 **/
		page = pte_page(pkmap_page_table[i]);
		/** 20131026    
		 * pkmap_page_table[i] pte entry를 지운다.
		 **/
		pte_clear(&init_mm, (unsigned long)page_address(page),
			  &pkmap_page_table[i]);

		/** 20131026    
		 * 해당 page를 htable에서 제거한다.
		 **/
		set_page_address(page, NULL);
		/** 20131026    
		 * page table entry가 변경되었으므로 tlb flush가 필요하다.
		 **/
		need_flush = 1;
	}
	if (need_flush)
		flush_tlb_kernel_range(PKMAP_ADDR(0), PKMAP_ADDR(LAST_PKMAP));
}

/**
 * kmap_flush_unused - flush all unused kmap mappings in order to remove stray mappings
 */
void kmap_flush_unused(void)
{
	lock_kmap();
	flush_all_zero_pkmaps();
	unlock_kmap();
}

static inline unsigned long map_new_virtual(struct page *page)
{
	unsigned long vaddr;
	int count;

start:
	/** 20131026    
	 * count = LAST_PKMAP; // 512
	 **/
	count = LAST_PKMAP;
	/* Find an empty entry */
	for (;;) {
		/** 20131026    
		 * last_pkmap_nr을 1 증가시키고, LAST_PKMAP_MASK로 만든다.
		 **/
		last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;
		/** 20131026    
		 * last_pkmap_nr가 0인 경우, 즉 LAST_PKMAP-1번째 수행시
		 **/
		if (!last_pkmap_nr) {
			flush_all_zero_pkmaps();
			count = LAST_PKMAP;
		}
		if (!pkmap_count[last_pkmap_nr])
			break;	/* Found a usable entry */
		if (--count)
			continue;

		/*
		 * Sleep for somebody else to unmap their entries
		 */
		{
			DECLARE_WAITQUEUE(wait, current);

			__set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&pkmap_map_wait, &wait);
			unlock_kmap();
			schedule();
			remove_wait_queue(&pkmap_map_wait, &wait);
			lock_kmap();

			/* Somebody else might have mapped it while we slept */
			if (page_address(page))
				return (unsigned long)page_address(page);

			/* Re-start */
			goto start;
		}
	}
	vaddr = PKMAP_ADDR(last_pkmap_nr);
	set_pte_at(&init_mm, vaddr,
		   &(pkmap_page_table[last_pkmap_nr]), mk_pte(page, kmap_prot));

	pkmap_count[last_pkmap_nr] = 1;
	set_page_address(page, (void *)vaddr);

	return vaddr;
}

/**
 * kmap_high - map a highmem page into memory
 * @page: &struct page to map
 *
 * Returns the page's virtual memory address.
 *
 * We cannot call this from interrupts, as it may block.
 */
void *kmap_high(struct page *page)
{
	unsigned long vaddr;

	/*
	 * For highmem pages, we can't trust "virtual" until
	 * after we have the lock.
	 */
	/** 20131026    
	 * irq disable 뒤 spinlock 한다.
	 **/
	lock_kmap();
	/** 20131026    
	 * lowmem일 경우 page에 대한 vaddr을 받아 온다.
	 **/
	vaddr = (unsigned long)page_address(page);
	/** 20131026    
	 * vaddr이 NULL인 경우
	 **/
	if (!vaddr)
		vaddr = map_new_virtual(page);
	pkmap_count[PKMAP_NR(vaddr)]++;
	BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 2);
	unlock_kmap();
	return (void*) vaddr;
}

EXPORT_SYMBOL(kmap_high);

#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/**
 * kmap_high_get - pin a highmem page into memory
 * @page: &struct page to pin
 *
 * Returns the page's current virtual memory address, or NULL if no mapping
 * exists.  If and only if a non null address is returned then a
 * matching call to kunmap_high() is necessary.
 *
 * This can be called from any context.
 */
/** 20131012
  * high memory에 대해 맵핑되어 있으면 virtual address를 리턴하고,
  * 그렇지 않으면 NULL을 리턴한다.
 **/
void *kmap_high_get(struct page *page)
{
	unsigned long vaddr, flags;
  /** 20131012
   * irq diable하고 spin_lock()함수를 호출한다.
   **/
	lock_kmap_any(flags);
    /** 20131012
      * page에 대한 virtual address를 가져온다
     **/
	vaddr = (unsigned long)page_address(page);
	/** 20131012
	 * vaddr이 존재할때 PKMAP_NR이 1보다 작으면 BUG_ON을 실행한다.
	 * BUG가 아니면 pkmap_count를 1 증가 시킨다.
	 **/
	if (vaddr) {
		BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 1);
		pkmap_count[PKMAP_NR(vaddr)]++;
	}
    /** 20131012
    * spin_unlock()을 호출하고 irq상태플래그를 복원한다.
     **/
	unlock_kmap_any(flags);
	return (void*) vaddr;
}
#endif

/**
 * kunmap_high - unmap a highmem page into memory
 * @page: &struct page to unmap
 *
 * If ARCH_NEEDS_KMAP_HIGH_GET is not defined then this may be called
 * only from user context.
 */
void kunmap_high(struct page *page)
{
	unsigned long vaddr;
	unsigned long nr;
	unsigned long flags;
	int need_wakeup;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	BUG_ON(!vaddr);
	nr = PKMAP_NR(vaddr);

	/*
	 * A count must never go down to zero
	 * without a TLB flush!
	 */
	need_wakeup = 0;
	switch (--pkmap_count[nr]) {
	case 0:
		BUG();
	case 1:
		/*
		 * Avoid an unnecessary wake_up() function call.
		 * The common case is pkmap_count[] == 1, but
		 * no waiters.
		 * The tasks queued in the wait-queue are guarded
		 * by both the lock in the wait-queue-head and by
		 * the kmap_lock.  As the kmap_lock is held here,
		 * no need for the wait-queue-head's lock.  Simply
		 * test if the queue is empty.
		 */
		need_wakeup = waitqueue_active(&pkmap_map_wait);
	}
	unlock_kmap_any(flags);

	/* do wake-up, if needed, race-free outside of the spin lock */
	if (need_wakeup)
		wake_up(&pkmap_map_wait);
}

EXPORT_SYMBOL(kunmap_high);
#endif

#if defined(HASHED_PAGE_VIRTUAL)

#define PA_HASH_ORDER	7

/*
 * Describes one page->virtual association
 */
struct page_address_map {
	struct page *page;
	void *virtual;
	struct list_head list;
};

/*
 * page_address_map freelist, allocated from page_address_maps.
 */
/** 20131026    
 * page_address_map의 freelist를 나타내는 head.
 **/
static struct list_head page_address_pool;	/* freelist */
static spinlock_t pool_lock;			/* protects page_address_pool */

/*
 * Hash table bucket
 */
/** 20131026    
 * page_address_htable는 1 << PA_HASH_ORDER개의 slot으로 이뤄져 있음.
 * 각 htable의 slot 하나마다 lock을 사용한다.
 **/
static struct page_address_slot {
	struct list_head lh;			/* List of page_address_maps */
	spinlock_t lock;			/* Protect this bucket's list */
} ____cacheline_aligned_in_smp page_address_htable[1<<PA_HASH_ORDER];

/** 20131026    
 * page에 대해 hash값을 구해 page_address_htable의 특정 slot의 주소를 가져온다.
 **/
static struct page_address_slot *page_slot(const struct page *page)
{
	/** 20131026    
	 * PA_HASH_ORDER은 7
	 **/
	return &page_address_htable[hash_ptr(page, PA_HASH_ORDER)];
}

/**
 * page_address - get the mapped virtual address of a page
 * @page: &struct page to get the virtual address of
 *
 * Returns the page's virtual address.
 */
/** 20131012
 * CONFIG_HIGHMEM이 define되어 있을 경우 실행된다.
 * page에 해당하는 해쉬 테이블 슬롯을 가져와서 pas->lh를 순회하며 
 * page_address_map과 page가 같은것을 찾으면 그 page의 virtual address를 리턴한다
 * =>상세한 코드 분석은 생략하고 넘어감 ???
 **/
void *page_address(const struct page *page)
{
	unsigned long flags;
	void *ret;
	struct page_address_slot *pas;

	if (!PageHighMem(page))
		return lowmem_page_address(page);

	pas = page_slot(page);
	ret = NULL;
	spin_lock_irqsave(&pas->lock, flags);
	if (!list_empty(&pas->lh)) {
		struct page_address_map *pam;

		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				ret = pam->virtual;
				goto done;
			}
		}
	}
done:
	spin_unlock_irqrestore(&pas->lock, flags);
	return ret;
}

EXPORT_SYMBOL(page_address);

/**
 * set_page_address - set a page's virtual address
 * @page: &struct page to set
 * @virtual: virtual address to use
 */
/** 20131026    
 * virtual이 주어졌을 경우 page_address mapping table에 추가,
 * virtual이 NULL일 경우 page_address mapping table에서 제거.
 **/
void set_page_address(struct page *page, void *virtual)
{
	unsigned long flags;
	struct page_address_slot *pas;
	struct page_address_map *pam;

	/** 20131026    
	 * page가 highmem이 아닌 경우 BUG.
	 **/
	BUG_ON(!PageHighMem(page));

	/** 20131026    
	 * struct page에 대한 hash table의 슬롯을 가져온다.
	 **/
	pas = page_slot(page);
	/** 20131026    
	 * virtual이 NULL로 넘어온 경우 제거,
	 * virtual이 아닌 주소가 넘어온 경우 추가.
	 *
	 * add
	 *   pool에서 map을 제거하고,
	 *   mapping 정보를 채우고 hash table 의 해당 slot list에 추가
	 *
	 * remove
	 *   hash table 의 해당 slot list에서 찾아 제거하고,
	 *   pool의 끝에 추가한다.
	 **/
	if (virtual) {		/* Add */
		/** 20131026    
		 * page_address_pool은 page_address_init에서 초기화 되면
		 * list에 다 추가된 상태다.
		 *
		 * 혹시 list_del로 pool이 다 비어 버리는 경우는 없는 것일까???
		 **/
		BUG_ON(list_empty(&page_address_pool));

		/** 20131026    
		 * 현재 irq 상태를 포함한 정보를 flags에 저장하고  pool_lock을 건다.
		 **/
		spin_lock_irqsave(&pool_lock, flags);
		/** 20131026    
		 * pool의 다음 entry를 가져와 struct page_address_map 변수에 저장한다.
		 **/
		pam = list_entry(page_address_pool.next,
				struct page_address_map, list);
		/** 20131026    
		 * pam->list에서 일단 제거한다.
		 **/
		list_del(&pam->list);
		/** 20131026    
		 * pool_lock을 해제하고, 백업해둔 flags를 복원한다.
		 **/
		spin_unlock_irqrestore(&pool_lock, flags);

		/** 20131026    
		 * pam 구조체에 page와 virtual 정보, 즉 mapping 정보를 채운다.
		 **/
		pam->page = page;
		pam->virtual = virtual;

		/** 20131026    
		 * pool에 list entry를 추가하기 위해 spinlock을 사용해 보호한다.
		 * mapping한 struct page_address_map을 찾아놓은 hash slot의 list의 끝에 추가한다.
		 **/
		spin_lock_irqsave(&pas->lock, flags);
		list_add_tail(&pam->list, &pas->lh);
		spin_unlock_irqrestore(&pas->lock, flags);
	} else {		/* Remove */
		/** 20131026    
		 * spin_lock_irq로 hash table의 slot을 pas를 보호.
		 **/
		spin_lock_irqsave(&pas->lock, flags);
		/** 20131026    
		 * hash table의 해당 slot list의 각 entry를 순회하며
		 **/
		list_for_each_entry(pam, &pas->lh, list) {
			/** 20131026    
			 * 찾는 page를 mapping 정보로 가지고 있는 경우
			 **/
			if (pam->page == page) {
				/** 20131026    
				 * 현재 list (slot list)에서 제거.
				 **/
				list_del(&pam->list);
				/** 20131026    
				 * 해당 slot lock을 해제하고,
				 * pool lock 구간 내에서 map을 보호한다.
				 **/
				spin_unlock_irqrestore(&pas->lock, flags);
				spin_lock_irqsave(&pool_lock, flags);
				/** 20131026    
				 * page_address_pool의 끝에 page address map을 다시 건다.
				 **/
				list_add_tail(&pam->list, &page_address_pool);
				spin_unlock_irqrestore(&pool_lock, flags);
				goto done;
			}
		}
		spin_unlock_irqrestore(&pas->lock, flags);
	}
done:
	return;
}

/** 20131026    
 * page_address_maps는 PKMAP이 가능한 수만큼의 array.
 **/
static struct page_address_map page_address_maps[LAST_PKMAP];

/** 20131026    
 * CONFIG_HIGHMEM을 사용하는 경우
 * page_address 관련 자료구조를 초기화 한다.
 *
 * page_address_htable과 page_address_pool, 두 가지 자료구조를 초기화 한다.
 *
 * page_address_htable의 slot의 list가 일반 list여야
 * page_address_maps[i]가 자유롭게 추가되고 삭제될 수 있다.
 **/
void __init page_address_init(void)
{
	int i;

	/** 20131026    
	 * page_address_pool을 list head로 초기화 시킨다.
	 * page_address_maps 각각을 page_address_pool에 넣어 list로 구성한다 (freelist)
	 **/
	INIT_LIST_HEAD(&page_address_pool);
	for (i = 0; i < ARRAY_SIZE(page_address_maps); i++)
		list_add(&page_address_maps[i].list, &page_address_pool);
	/** 20131026    
	 * htable의 각 slot의 list head를 초기화 하고, 각 list에 대한 spinlock 초기화
	 **/
	for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
		INIT_LIST_HEAD(&page_address_htable[i].lh);
		spin_lock_init(&page_address_htable[i].lock);
	}
	/** 20131026    
	 * pool에 대한 전체 spinlock 초기화
	 **/
	spin_lock_init(&pool_lock);
}

#endif	/* defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL) */
