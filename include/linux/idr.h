/*
 * include/linux/idr.h
 * 
 * 2002-10-18  written by Jim Houston jim.houston@ccur.com
 *	Copyright (C) 2002 by Concurrent Computer Corporation
 *	Distributed under the GNU GPL license version 2.
 *
 * Small id to pointer translation service avoiding fixed sized
 * tables.
 */

#ifndef __IDR_H__
#define __IDR_H__

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/rcupdate.h>

#if BITS_PER_LONG == 32
/** 20140517
 **/
# define IDR_BITS 5
# define IDR_FULL 0xfffffffful
/* We can only use two of the bits in the top level because there is
   only one possible bit in the top level (5 bits * 7 levels = 35
   bits, but you only use 31 bits in the id). */
# define TOP_LEVEL_FULL (IDR_FULL >> 30)
#elif BITS_PER_LONG == 64
# define IDR_BITS 6
# define IDR_FULL 0xfffffffffffffffful
/* We can only use two of the bits in the top level because there is
   only one possible bit in the top level (6 bits * 6 levels = 36
   bits, but you only use 31 bits in the id). */
# define TOP_LEVEL_FULL (IDR_FULL >> 62)
#else
# error "BITS_PER_LONG is not 32 or 64"
#endif

#define IDR_SIZE (1 << IDR_BITS)
#define IDR_MASK ((1 << IDR_BITS)-1)

/** 20140517
 * MAX_ID_SHIFT : 31
 * MAX_ID_BIT   : 1U << 31
 * MAX_ID_MASK  : (1U << 31 - 1)
 **/
#define MAX_ID_SHIFT (sizeof(int)*8 - 1)
#define MAX_ID_BIT (1U << MAX_ID_SHIFT)
#define MAX_ID_MASK (MAX_ID_BIT - 1)

/* Leave the possibility of an incomplete final layer */
/** 20140517
 * MAX_LEVEL :  31 + 5 - 1 / 5 = 7
 **/
#define MAX_LEVEL (MAX_ID_SHIFT + IDR_BITS - 1) / IDR_BITS

/* Number of id_layer structs to leave in free list */
#define IDR_FREE_MAX MAX_LEVEL + MAX_LEVEL

/** 20140712
 * integer ID와 pointer를 저장하는 자료구조 (tree의 node에 해당)
 *
 *		bitmap		현재 layer에서 비어있는 곳은 0, 채워진 곳은 1.
 *					leaf node일 경우, 각 비트는 정수 ID되어 있는지 여부를 나타냄.
 *					non-leaf node일 경우, 각 비트는 하위 layer 공간이 모두 차 있는지 여부를 나타냄.
 *		ary			leaf node일 경우, 해당 ID에 대응하는 pointer 값.
 *					non-leaf node일 경우, 하위 idr_layer에 대한 pointer.
 *
 *					ida로 사용할 때, leaf node일 경우 ary의 의미는 독자적인 bitmap의 위치를 저장하기 위해 사용된다.
 *		count		leaf node일 경우, 현재 layer에서 할당된 idr의 수.
 *					non-leaf node일 경우, 할당된 하위 layer의 수.
 *		layer		leaf node일 경우 0부터 시작한 index.
 *		rcu_head	idr_layer 제거시 사용
 *
 *		[정리출처] http://studyfoss.egloos.com/5187192
 **/
struct idr_layer {
	unsigned long		 bitmap; /* A zero bit means "space here" */
	struct idr_layer __rcu	*ary[1<<IDR_BITS];
	int			 count;	 /* When zero, we can release it */
	int			 layer;	 /* distance from leaf */
	struct rcu_head		 rcu_head;
};

/** 20140517
 * object에 integer ID를 할당하고, id로 objects를 찾을 때 사용한다.
 *
 * idr_init으로 초기화.
 *		top		- IDR layer의 가장 윗단. tree의 root.
 *		id_free	- 예비용으로 보관 중인 여유 idr_layer의 list. (single list)
 *		layers	- IDR layer의 수. tree의 height.
 *		id_free_cnt - id_free 리스트에 연결된 idr_layer의 수.
 *		lock	- idr 구조체에 대한 lock.
 *
 *	[참고] http://studyfoss.egloos.com/5187192
 **/
struct idr {
	struct idr_layer __rcu *top;
	struct idr_layer *id_free;
	int		  layers; /* only valid without concurrent changes */
	int		  id_free_cnt;
	spinlock_t	  lock;
};

/** 20150221
 * name이라는 IDR의 생성 및 초기화.
 **/
#define IDR_INIT(name)						\
{								\
	.top		= NULL,					\
	.id_free	= NULL,					\
	.layers 	= 0,					\
	.id_free_cnt	= 0,					\
	.lock		= __SPIN_LOCK_UNLOCKED(name.lock),	\
}
#define DEFINE_IDR(name)	struct idr name = IDR_INIT(name)

/* Actions to be taken after a call to _idr_sub_alloc */
#define IDR_NEED_TO_GROW -2
#define IDR_NOMORE_SPACE -3

/** 20140712
 * idr code를 errno 로 변환.
 * 위의 error code는 _idr_sub_alloc가 리턴하는 경우.
 **/
#define _idr_rc_to_errno(rc) ((rc) == -1 ? -EAGAIN : -ENOSPC)

/**
 * DOC: idr sync
 * idr synchronization (stolen from radix-tree.h)
 *
 * idr_find() is able to be called locklessly, using RCU. The caller must
 * ensure calls to this function are made within rcu_read_lock() regions.
 * Other readers (lock-free or otherwise) and modifications may be running
 * concurrently.
 *
 * It is still required that the caller manage the synchronization and
 * lifetimes of the items. So if RCU lock-free lookups are used, typically
 * this would mean that the items have their own locks, or are amenable to
 * lock-free access; and that the items are freed by RCU (or only freed after
 * having been deleted from the idr tree *and* a synchronize_rcu() grace
 * period).
 */

/*
 * This is what we export.
 */

void *idr_find(struct idr *idp, int id);
int idr_pre_get(struct idr *idp, gfp_t gfp_mask);
int idr_get_new(struct idr *idp, void *ptr, int *id);
int idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id);
int idr_for_each(struct idr *idp,
		 int (*fn)(int id, void *p, void *data), void *data);
void *idr_get_next(struct idr *idp, int *nextid);
void *idr_replace(struct idr *idp, void *ptr, int id);
void idr_remove(struct idr *idp, int id);
void idr_remove_all(struct idr *idp);
void idr_destroy(struct idr *idp);
void idr_init(struct idr *idp);


/*
 * IDA - IDR based id allocator, use when translation from id to
 * pointer isn't necessary.
 *
 * IDA_BITMAP_LONGS is calculated to be one less to accommodate
 * ida_bitmap->nr_busy so that the whole struct fits in 128 bytes.
 */
/** 20150228
 * IDA chunk 크기는 128 바이트.
 * IDA 비트맵을 LONGS로 31개.
 * IDA 비트맵 LONGS를 BITS 크기로 계산. (31 * 4 * 8)
 **/
#define IDA_CHUNK_SIZE		128	/* 128 bytes per chunk */
#define IDA_BITMAP_LONGS	(IDA_CHUNK_SIZE / sizeof(long) - 1)
#define IDA_BITMAP_BITS 	(IDA_BITMAP_LONGS * sizeof(long) * 8)

/** 20150228
 * nr_busy는 정수값을 하나 할당 받을 때마다 증가시킨다.
 **/
struct ida_bitmap {
	long			nr_busy;
	unsigned long		bitmap[IDA_BITMAP_LONGS];
};

/** 20150221
 * IDR의 구조를 이용하는 ID 할당자.
 * id에 따른 pointer를 저장하지 않고 ID만을 할당한다.
 *
 * idr의 구조는 http://studyfoss.egloos.com/5187192 참고.
 **/
struct ida {
	struct idr		idr;
	struct ida_bitmap	*free_bitmap;
};

/** 20150221
 * name이라는 IDA를 생성 및 초기화.
 **/
#define IDA_INIT(name)		{ .idr = IDR_INIT(name), .free_bitmap = NULL, }
#define DEFINE_IDA(name)	struct ida name = IDA_INIT(name)

int ida_pre_get(struct ida *ida, gfp_t gfp_mask);
int ida_get_new_above(struct ida *ida, int starting_id, int *p_id);
int ida_get_new(struct ida *ida, int *p_id);
void ida_remove(struct ida *ida, int id);
void ida_destroy(struct ida *ida);
void ida_init(struct ida *ida);

int ida_simple_get(struct ida *ida, unsigned int start, unsigned int end,
		   gfp_t gfp_mask);
void ida_simple_remove(struct ida *ida, unsigned int id);

void __init idr_init_cache(void);

#endif /* __IDR_H__ */
