#ifndef _LINUX_LIST_BL_H
#define _LINUX_LIST_BL_H

#include <linux/list.h>
#include <linux/bit_spinlock.h>

/*
 * Special version of lists, where head of the list has a lock in the lowest
 * bit. This is useful for scalable hash tables without increasing memory
 * footprint overhead.
 *
 * For modification operations, the 0 bit of hlist_bl_head->first
 * pointer must be set.
 *
 * With some small modifications, this can easily be adapted to store several
 * arbitrary bits (not just a single lock bit), if the need arises to store
 * some fast and compact auxiliary data.
 */

#if defined(CONFIG_SMP) || defined(CONFIG_DEBUG_SPINLOCK)
#define LIST_BL_LOCKMASK	1UL
#else
#define LIST_BL_LOCKMASK	0UL
#endif

#ifdef CONFIG_DEBUG_LIST
#define LIST_BL_BUG_ON(x) BUG_ON(x)
#else
#define LIST_BL_BUG_ON(x)
#endif


/** 20150328
 * hlist 'bit lock' head
 **/
struct hlist_bl_head {
	struct hlist_bl_node *first;
};

/** 20150328
 * hlist node
 **/
struct hlist_bl_node {
	struct hlist_bl_node *next, **pprev;
};
/** 20130803
 * hlist의 first를 NULL로 초기화 한다.
 **/
#define INIT_HLIST_BL_HEAD(ptr) \
	((ptr)->first = NULL)

/** 20150328
 * hlist_bl_node 초기화 함수.
 **/
static inline void INIT_HLIST_BL_NODE(struct hlist_bl_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

/** 20150328
 * ptr를 member로 포함하고 있는 type인 struct가 지칭된다.
 **/
#define hlist_bl_entry(ptr, type, member) container_of(ptr,type,member)

/** 20150404
 * hlist node의 pprev가 NULL이라면 현재 hashlist에 등록되지 않는 노드이다.
 * 따라서 unhashed이다.
 **/
static inline int hlist_bl_unhashed(const struct hlist_bl_node *h)
{
	return !h->pprev;
}

static inline struct hlist_bl_node *hlist_bl_first(struct hlist_bl_head *h)
{
	return (struct hlist_bl_node *)
		((unsigned long)h->first & ~LIST_BL_LOCKMASK);
}

static inline void hlist_bl_set_first(struct hlist_bl_head *h,
					struct hlist_bl_node *n)
{
	LIST_BL_BUG_ON((unsigned long)n & LIST_BL_LOCKMASK);
	LIST_BL_BUG_ON(((unsigned long)h->first & LIST_BL_LOCKMASK) !=
							LIST_BL_LOCKMASK);
	h->first = (struct hlist_bl_node *)((unsigned long)n | LIST_BL_LOCKMASK);
}

static inline int hlist_bl_empty(const struct hlist_bl_head *h)
{
	return !((unsigned long)h->first & ~LIST_BL_LOCKMASK);
}

static inline void hlist_bl_add_head(struct hlist_bl_node *n,
					struct hlist_bl_head *h)
{
	struct hlist_bl_node *first = hlist_bl_first(h);

	n->next = first;
	if (first)
		first->pprev = &n->next;
	n->pprev = &h->first;
	hlist_bl_set_first(h, n);
}

static inline void __hlist_bl_del(struct hlist_bl_node *n)
{
	struct hlist_bl_node *next = n->next;
	struct hlist_bl_node **pprev = n->pprev;

	LIST_BL_BUG_ON((unsigned long)n & LIST_BL_LOCKMASK);

	/* pprev may be `first`, so be careful not to lose the lock bit */
	/** 20130803
	 * next의 LIST_BL_LOCKMASK 비트는 pprev의 LIST_BL_LOCKMASK의 속성을 계승받는다.
	 * LIST_BL_LOCKMASK는 first 노드에 표시한다.
	 * 만약 삭제할 노드가 first의 다음 노드라면, first의 *pprev를 갱신할 때
	 *   LIST_BL_LOCKMASK 속성을 유지하도록 한다.
	 **/
	*pprev = (struct hlist_bl_node *)
			((unsigned long)next |
			 ((unsigned long)*pprev & LIST_BL_LOCKMASK));
	if (next)
		next->pprev = pprev;
}

/** 20130803
 * hlist에서 노드를 삭제하고, 기존 노드의 n->next와 pprev를 POISON 처리한다.
 **/
static inline void hlist_bl_del(struct hlist_bl_node *n)
{
	__hlist_bl_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

static inline void hlist_bl_del_init(struct hlist_bl_node *n)
{
	if (!hlist_bl_unhashed(n)) {
		__hlist_bl_del(n);
		INIT_HLIST_BL_NODE(n);
	}
}

static inline void hlist_bl_lock(struct hlist_bl_head *b)
{
	bit_spin_lock(0, (unsigned long *)b);
}

static inline void hlist_bl_unlock(struct hlist_bl_head *b)
{
	__bit_spin_unlock(0, (unsigned long *)b);
}

/**
 * hlist_bl_for_each_entry	- iterate over list of given type
 * @tpos:	the type * to use as a loop cursor.
 * @pos:	the &struct hlist_node to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 *
 */
#define hlist_bl_for_each_entry(tpos, pos, head, member)		\
	for (pos = hlist_bl_first(head);				\
	     pos &&							\
		({ tpos = hlist_bl_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)

/**
 * hlist_bl_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @tpos:	the type * to use as a loop cursor.
 * @pos:	the &struct hlist_node to use as a loop cursor.
 * @n:		another &struct hlist_node to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_bl_for_each_entry_safe(tpos, pos, n, head, member)	 \
	for (pos = hlist_bl_first(head);				 \
	     pos && ({ n = pos->next; 1; }) && 				 \
		({ tpos = hlist_bl_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = n)

#endif
