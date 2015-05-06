#ifndef _LINUX_SLUB_DEF_H
#define _LINUX_SLUB_DEF_H

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter
 */
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>

#include <linux/kmemleak.h>

enum stat_item {
	ALLOC_FASTPATH,		/* Allocation from cpu slab */
	ALLOC_SLOWPATH,		/* Allocation by getting a new cpu slab */
	FREE_FASTPATH,		/* Free to cpu slub */
	FREE_SLOWPATH,		/* Freeing not to cpu slab */
	FREE_FROZEN,		/* Freeing to frozen slab */
	FREE_ADD_PARTIAL,	/* Freeing moves slab to partial list */
	FREE_REMOVE_PARTIAL,	/* Freeing removes last object */
	ALLOC_FROM_PARTIAL,	/* Cpu slab acquired from node partial list */
	ALLOC_SLAB,		/* Cpu slab acquired from page allocator */
	ALLOC_REFILL,		/* Refill cpu slab from slab freelist */
	ALLOC_NODE_MISMATCH,	/* Switching cpu slab */
	FREE_SLAB,		/* Slab freed to the page allocator */
	CPUSLAB_FLUSH,		/* Abandoning of the cpu slab */
	DEACTIVATE_FULL,	/* Cpu slab was full when deactivated */
	DEACTIVATE_EMPTY,	/* Cpu slab was empty when deactivated */
	DEACTIVATE_TO_HEAD,	/* Cpu slab was moved to the head of partials */
	DEACTIVATE_TO_TAIL,	/* Cpu slab was moved to the tail of partials */
	DEACTIVATE_REMOTE_FREES,/* Slab contained remotely freed objects */
	DEACTIVATE_BYPASS,	/* Implicit deactivation */
	ORDER_FALLBACK,		/* Number of times fallback was necessary */
	CMPXCHG_DOUBLE_CPU_FAIL,/* Failure of this_cpu_cmpxchg_double */
	CMPXCHG_DOUBLE_FAIL,	/* Number of times that cmpxchg double did not match */
	CPU_PARTIAL_ALLOC,	/* Used cpu partial on alloc */
	CPU_PARTIAL_FREE,	/* Refill cpu partial on free */
	CPU_PARTIAL_NODE,	/* Refill cpu partial from node partial */
	CPU_PARTIAL_DRAIN,	/* Drain cpu partial to node partial */
	NR_SLUB_STAT_ITEMS };

struct kmem_cache_cpu {
	/** 20140322    
	 * fastpath로 다음 사용 가능한 object를 찾기 위한 포인터
	 **/
	void **freelist;	/* Pointer to next available object */
	unsigned long tid;	/* Globally unique transaction id */
	/** 20140322    
	 * page : allocation 한 slab
	 **/
	struct page *page;	/* The slab from which we are allocating */
	struct page *partial;	/* Partially allocated frozen slabs */
#ifdef CONFIG_SLUB_STATS
	unsigned stat[NR_SLUB_STAT_ITEMS];
#endif
};

/** 20140222
 * kmem_cache_node는 partial object에 대해서만 관리한다.
 *
 * 20140510
 * node에서 slab용으로 사용 중인 page 들을 list로 관리한다.
 *
 * list_lock  : lock
 * nr_partial : partial 리스트에 추가된 항목 수
 *   add_partial, remove_partial로 페이지 추가 제거.
 * partial    : list head. struct page의 lru를 사용한다.
 **/
struct kmem_cache_node {
	spinlock_t list_lock;	/* Protect partial list and nr_partial */
	unsigned long nr_partial;
	struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;
#endif
};

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
/** 20140215
 * 하나의 slab에 대한 order와 object의 갯수에 대한 정보
 **/
struct kmem_cache_order_objects {
	unsigned long x;
};

/*
 * Slab cache management.
 */
/** 20150502    
 **/
struct kmem_cache {
	/** 20140510    
	 * kmem_cache_cpu가 percpu변수로 존재한다.
	 **/
	struct kmem_cache_cpu __percpu *cpu_slab;
	/* Used for retriving partial slabs etc */
	unsigned long flags;
	unsigned long min_partial;
	int size;		/* The size of an object including meta data */
	int object_size;	/* The size of an object without meta data */
	int offset;		/* Free pointer offset. */
	/** 20140215
	 * kmem_cache_open()에서 초기화 됨
	 **/
	int cpu_partial;	/* Number of per cpu partial objects to keep around */
	/** 20140215
	 * 하나의 slab에 대한 order와 object의 수를 나타냄 
	 **/
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	struct kmem_cache_order_objects max;
	struct kmem_cache_order_objects min;
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	int refcount;		/* Refcount for slab cache destroy */
	void (*ctor)(void *);
	/** 20140510    
	 * object에 의해 실제 사용되는 공간. 정렬된 크기를 가져야 한다.
	 **/
	int inuse;		/* Offset to metadata */
	int align;		/* Alignment */
	/** 20140510    
	 * rcu 등을 위해 slab에 예약된 크기(bytes 단위)
	 **/
	int reserved;		/* Reserved bytes at the end of slabs */
	const char *name;	/* Name (only for display!) */
	/** 20140322    
	 * slab_caches로 관리하기 위한 list_head
	 **/
	struct list_head list;	/* List of slab caches */
#ifdef CONFIG_SYSFS
	struct kobject kobj;	/* For sysfs */
#endif

#ifdef CONFIG_NUMA
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	int remote_node_defrag_ratio;
#endif
	struct kmem_cache_node *node[MAX_NUMNODES];
};

/*
 * Kmalloc subsystem.
 */
/** 20140405    
 * KMALLOC_MIN_SIZE는 DMA_MINALIGN (L1 CACHE SIZE => vexpress는 64)
 **/
#if defined(ARCH_DMA_MINALIGN) && ARCH_DMA_MINALIGN > 8
#define KMALLOC_MIN_SIZE ARCH_DMA_MINALIGN
#else
#define KMALLOC_MIN_SIZE 8
#endif

/** 20140322    
 * KMALLOC_MIN_SIZE의 지수값을 KMALLOC_SHIFT_LOW로 사용
 * (2^6 = 64이므로 6)
 **/
#define KMALLOC_SHIFT_LOW ilog2(KMALLOC_MIN_SIZE)

/*
 * Maximum kmalloc object size handled by SLUB. Larger object allocations
 * are passed through to the page allocator. The page allocator "fastpath"
 * is relatively slow so we need this value sufficiently high so that
 * performance critical objects are allocated through the SLUB fastpath.
 *
 * This should be dropped to PAGE_SIZE / 2 once the page allocator
 * "fastpath" becomes competitive with the slab allocator fastpaths.
 */
/** 20140405    
 * SLUB_MAX_SIZE는 페이지 2개 크기. 현재 8KB.
 **/
#define SLUB_MAX_SIZE (2 * PAGE_SIZE)

/** 20140322    
 * PAGE_SHIFT는 현재 12.
 * SLUB_PAGE_SHIFT는 14.
 **/
#define SLUB_PAGE_SHIFT (PAGE_SHIFT + 2)

#ifdef CONFIG_ZONE_DMA
#define SLUB_DMA __GFP_DMA
#else
/* Disable DMA functionality */
#define SLUB_DMA (__force gfp_t)0
#endif

/*
 * We keep the general caches in an array of slab caches that are used for
 * 2^x bytes of allocations.
 */
extern struct kmem_cache *kmalloc_caches[SLUB_PAGE_SHIFT];

/*
 * Sorry that the following has to be that ugly but some versions of GCC
 * have trouble with constant propagation and loops.
 */
/** 20140405    
 * 요청한 size에 따라 적절한 index를 리턴.
 *
 * index가 SLAB MAX 이상까지 지정되어 있는 것은 어떤 경우를 고려한 것인지???
 **/
static __always_inline int kmalloc_index(size_t size)
{
	if (!size)
		return 0;

	/** 20140405    
	 * MIN SIZE보다 작으면 SHIFT_LOW로 index를 가장 작은 크기로 지정
	 **/
	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;

	if (KMALLOC_MIN_SIZE <= 32 && size > 64 && size <= 96)
		return 1;
	/** 20140405    
	 * 128 < size <= 192인 경우 고정된 index를 리턴.
	 **/
	if (KMALLOC_MIN_SIZE <= 64 && size > 128 && size <= 192)
		return 2;
	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
/*
 * The following is only needed to support architectures with a larger page
 * size than 4k. We need to support 2 * PAGE_SIZE here. So for a 64k page
 * size we would have to go up to 128k.
 */
	if (size <=   8 * 1024) return 13;
	if (size <=  16 * 1024) return 14;
	if (size <=  32 * 1024) return 15;
	if (size <=  64 * 1024) return 16;
	if (size <= 128 * 1024) return 17;
	if (size <= 256 * 1024) return 18;
	if (size <= 512 * 1024) return 19;
	if (size <= 1024 * 1024) return 20;
	if (size <=  2 * 1024 * 1024) return 21;
	BUG();
	return -1; /* Will never be reached */

/*
 * What we really wanted to do and cannot do because of compiler issues is:
 *	int i;
 *	for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++)
 *		if (size <= (1 << i))
 *			return i;
 */
}

/*
 * Find the slab cache for a given combination of allocation flags and size.
 *
 * This ought to end up with a global pointer to the right cache
 * in kmalloc_caches.
 */
/** 20140405    
 * 요청한 size에 따라 kmalloc_caches에서 적합한 kmem_cache를 찾아 리턴하는 함수
 **/
static __always_inline struct kmem_cache *kmalloc_slab(size_t size)
{
	/** 20140405    
	 * 요청한 size로 kmalloc index를 찾아온다.
	 * kmem_cache_init 부분에서 kmalloc size별 kmem_cache를 생성해 두었다.
	 **/
	int index = kmalloc_index(size);

	if (index == 0)
		return NULL;

	/** 20140405    
	 * index로 kmalloc kmem_cache 하나를 찾아 리턴
	 **/
	return kmalloc_caches[index];
}

void *kmem_cache_alloc(struct kmem_cache *, gfp_t);
void *__kmalloc(size_t size, gfp_t flags);

/** 20140705
 * buddy로부터 1 << order 만큼의 page를 받아온다.
 */
static __always_inline void *
kmalloc_order(size_t size, gfp_t flags, unsigned int order)
{
	void *ret = (void *) __get_free_pages(flags | __GFP_COMP, order);
	kmemleak_alloc(ret, size, 1, flags);
	return ret;
}

/**
 * Calling this on allocated memory will check that the memory
 * is expected to be in use, and print warnings if not.
 */
#ifdef CONFIG_SLUB_DEBUG
extern bool verify_mem_not_deleted(const void *x);
#else
static inline bool verify_mem_not_deleted(const void *x)
{
	return true;
}
#endif

#ifdef CONFIG_TRACING
extern void *
kmem_cache_alloc_trace(struct kmem_cache *s, gfp_t gfpflags, size_t size);
extern void *kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order);
#else
/** 20140517    
 * tracing을 사용하지 않을 경우 kmem_cache_alloc만 호출.
 * object를 하나 할당받아 리턴한다.
 **/
static __always_inline void *
kmem_cache_alloc_trace(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	return kmem_cache_alloc(s, gfpflags);
}

/** 20140705
 * kmalloc_order를 바로 호출 
 */
static __always_inline void *
kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order)
{
	return kmalloc_order(size, flags, order);
}
#endif

/** 20140705
 * size를 page의 order로 계산하여 kmalloc_order를 호출한다.
 */
static __always_inline void *kmalloc_large(size_t size, gfp_t flags)
{
	unsigned int order = get_order(size);
	return kmalloc_order_trace(size, flags, order);
}

/** 20140705
 *
 * kmalloc : 물리적으로 연속적인 메모리 할당을 시도하는 함수
 *
 * size가 compile시에 결정되는 상수일 경우
 * 1. size가 SLUB_MAX_SIZE보다 클 경우 kmalloc_large를 통해 buddy에서 page를
 *    order만큼 받아오고
 * 2. 그렇지 않고 SLUB_DMA가 설정되어 있지 않은 경우 
 *    slab으로부터 page를 받아온다.
 * =>
 *
 * size가 compile시 결정되지 않는 상수이면 __kmalloc을 호출한다
 * => get_slab함수 호출시 size를 지정하여 kmem_cache로부터 
 *    적함한 object를 할당한다.
 */
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	if (__builtin_constant_p(size)) {
		if (size > SLUB_MAX_SIZE)
			return kmalloc_large(size, flags);

		if (!(flags & SLUB_DMA)) {
			struct kmem_cache *s = kmalloc_slab(size);

			if (!s)
				return ZERO_SIZE_PTR;

			return kmem_cache_alloc_trace(s, flags, size);
		}
	}
	return __kmalloc(size, flags);
}

#ifdef CONFIG_NUMA
void *__kmalloc_node(size_t size, gfp_t flags, int node);
void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node);

#ifdef CONFIG_TRACING
extern void *kmem_cache_alloc_node_trace(struct kmem_cache *s,
					   gfp_t gfpflags,
					   int node, size_t size);
#else
/** 20140405    
 * CONFIG_TRACING을 정의하지 않은 경우
 * 
 * kmem_cache object를 지정한 노드로부터 할당 받는다.
 **/
static __always_inline void *
kmem_cache_alloc_node_trace(struct kmem_cache *s,
			      gfp_t gfpflags,
			      int node, size_t size)
{
	return kmem_cache_alloc_node(s, gfpflags, node);
}
#endif

static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	/** 20140405    
	 * size가 compile time에 상수로 확인가능한 경우이며,
	 * SLUB_MAX_SIZE보다 작거나 같고,
	 * SLUB_DMA 요청이 아닌 경우
	 * kmalloc slub으로부터 메모리를 할당받는다.
	 **/
	if (__builtin_constant_p(size) &&
		size <= SLUB_MAX_SIZE && !(flags & SLUB_DMA)) {
			struct kmem_cache *s = kmalloc_slab(size);

		if (!s)
			return ZERO_SIZE_PTR;

		return kmem_cache_alloc_node_trace(s, flags, node, size);
	}
	return __kmalloc_node(size, flags, node);
}
#endif

#endif /* _LINUX_SLUB_DEF_H */
