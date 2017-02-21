#ifndef MM_SLAB_H
#define MM_SLAB_H
/*
 * Internal slab definitions
 */

/*
 * State of the slab allocator.
 *
 * This is used to describe the states of the allocator during bootup.
 * Allocators use this to gradually bootstrap themselves. Most allocators
 * have the problem that the structures used for managing slab caches are
 * allocated from slab caches themselves.
 */
/** 20140510
 * DOWN    : 초기 상태
 * PARTIAL : kmem_cache_init 과정 중
 * FULL    : initcall에 의해 slab_sysfs_init이 호출되어 나머지 초기화를 수행한 뒤
 **/
enum slab_state {
	DOWN,			/* No slab functionality yet */
	PARTIAL,		/* SLUB: kmem_cache_node available */
	PARTIAL_ARRAYCACHE,	/* SLAB: kmalloc size for arraycache available */
	PARTIAL_L3,		/* SLAB: kmalloc size for l3 struct available */
	UP,			/* Slab caches usable but not all extras yet */
	FULL			/* Everything is working */
};

extern enum slab_state slab_state;

/* The slab cache mutex protects the management structures during changes */
extern struct mutex slab_mutex;
extern struct list_head slab_caches;

struct kmem_cache *__kmem_cache_create(const char *name, size_t size,
	size_t align, unsigned long flags, void (*ctor)(void *));

#endif
