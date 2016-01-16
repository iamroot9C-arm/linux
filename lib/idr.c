/*
 * 2002-10-18  written by Jim Houston jim.houston@ccur.com
 *	Copyright (C) 2002 by Concurrent Computer Corporation
 *	Distributed under the GNU GPL license version 2.
 *
 * Modified by George Anzinger to reuse immediately and to use
 * find bit instructions.  Also removed _irq on spinlocks.
 *
 * Modified by Nadia Derbey to make it RCU safe.
 *
 * Small id to pointer translation service.
 *
 * It uses a radix tree like structure as a sparse array indexed
 * by the id to obtain the pointer.  The bitmap makes allocating
 * a new id quick.
 *
 * You call it to allocate an id (an int) an associate with that id a
 * pointer or what ever, we treat it as a (void *).  You can pass this
 * id to a user for him to pass back at a later time.  You then pass
 * that id to this code and it returns your pointer.

 * You can release ids at any time. When all ids are released, most of
 * the memory is returned (we keep IDR_FREE_MAX) in a local pool so we
 * don't need to go to the memory "store" during an id allocate, just
 * so you don't need to be too concerned about locking and conflicts
 * with the slab allocator.
 */

#ifndef TEST                        // to test in user space...
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/export.h>
#endif
#include <linux/err.h>
#include <linux/string.h>
#include <linux/idr.h>
#include <linux/spinlock.h>

/** 20140517    
 * idr_init_cache 에서 slab cache 생성
 **/
static struct kmem_cache *idr_layer_cache;
static DEFINE_SPINLOCK(simple_ida_lock);

/** 20140705
 * id_free 리스트로부터 idr_layer하나를 꺼내온다.
 */
static struct idr_layer *get_from_free_list(struct idr *idp)
{
	struct idr_layer *p;
	unsigned long flags;

	spin_lock_irqsave(&idp->lock, flags);
	if ((p = idp->id_free)) {
		idp->id_free = p->ary[0];
		idp->id_free_cnt--;
		p->ary[0] = NULL;
	}
	spin_unlock_irqrestore(&idp->lock, flags);
	return(p);
}

/** 20140712    
 * 제거할 rcu_head를 포함한 idr_layer를 가져와 kmem_cache_free로 해제한다.
 **/
static void idr_layer_rcu_free(struct rcu_head *head)
{
	struct idr_layer *layer;

	layer = container_of(head, struct idr_layer, rcu_head);
	kmem_cache_free(idr_layer_cache, layer);
}

/** 20140712    
 * idr_layer를 해제하기 위한 rcu callback으로 idr_layer_rcu_free를 지정.
 * (reclamation phase에 해당)
 **/
static inline void free_layer(struct idr_layer *p)
{
	call_rcu(&p->rcu_head, idr_layer_rcu_free);
}

/** 20140705
 * idp의 id_free에 idr_layer를 추가시킨다.
 *
 * .ary[0]을 이용해 single list로 연결시킨다.
 *
 * [ before ]
 * idr->id_free --> old idr_layer
 *
 * [ after ]
 * idr->id_free --> new idr_layer->ary[0] --> old idr_layer
 */

/* only called when idp->lock is held */
static void __move_to_free_list(struct idr *idp, struct idr_layer *p)
{
	p->ary[0] = idp->id_free;
	idp->id_free = p;
	idp->id_free_cnt++;
}

/** 20140705
 * idr의 id_free list에 idr_layer 엔트리를 추가.
 *
 * spin lock을 한번만 걸어주기 위해 __move_to_free_list와 별도로
 * 만들어준 함수.
 */
static void move_to_free_list(struct idr *idp, struct idr_layer *p)
{
	unsigned long flags;

	/*
	 * Depends on the return element being zeroed.
	 */
	spin_lock_irqsave(&idp->lock, flags);
	__move_to_free_list(idp, p);
	spin_unlock_irqrestore(&idp->lock, flags);
}

/** 20140705 
 * id값에 대한 idr_layer의 bitmap을 세팅하고,
 * 현재 레이어가 IDR_FULL인 경우 
 * 상위레이어의 루프를 계속 돌면서 상위 레이어의 bitmap을 세팅한다.
 * (leaf -> top으로 이동)
 */
static void idr_mark_full(struct idr_layer **pa, int id)
{
	struct idr_layer *p = pa[0];
	int l = 0;

	__set_bit(id & IDR_MASK, &p->bitmap);
	/*
	 * If this layer is full mark the bit in the layer above to
	 * show that this part of the radix tree is full.  This may
	 * complete the layer above and require walking up the radix
	 * tree.
	 */
	while (p->bitmap == IDR_FULL) {
		if (!(p = pa[++l]))
			break;
		id = id >> IDR_BITS;
		__set_bit((id & IDR_MASK), &p->bitmap);
	}
}

/**
 * idr_pre_get - reserve resources for idr allocation
 * @idp:	idr handle
 * @gfp_mask:	memory allocation flags
 *
 * This function should be called prior to calling the idr_get_new* functions.
 * It preallocates enough memory to satisfy the worst possible allocation. The
 * caller should pass in GFP_KERNEL if possible.  This of course requires that
 * no spinning locks be held.
 *
 * If the system is REALLY out of memory this function returns %0,
 * otherwise %1.
 */
/** 20140705
 * IDR_FREE_MAX갯수만큼 idr_layer를 할당받아서 idr의 id_free에 채워넣는다
 */
int idr_pre_get(struct idr *idp, gfp_t gfp_mask)
{
	while (idp->id_free_cnt < IDR_FREE_MAX) {
		struct idr_layer *new;
		new = kmem_cache_zalloc(idr_layer_cache, gfp_mask);
		if (new == NULL)
			return (0);
		move_to_free_list(idp, new);
	}
	return 1;
}
EXPORT_SYMBOL(idr_pre_get);

/** 20140705 
 * starting_id 이상의 할당 가능한 id를 찾아 idr_layer를 생성해 할당하고
 * id를 리턴하는 함수.
 * 
 * top부터 leaf idr_layer 까지 해당 경로상의 idr_layer가 모두 생성된다.
 * pa는 각 level의 idr_layer 포인터를 저장할 임시 구조체를 받는다.
 */
static int sub_alloc(struct idr *idp, int *starting_id, struct idr_layer **pa)
{
	int n, m, sh;
	struct idr_layer *p, *new;
	int l, id, oid;
	unsigned long bm;

	/** 20140712    
	 * id는 starting_id부터 시작.
	 **/
	id = *starting_id;
 restart:
	/** 20140712    
	 * top에서 id를 mapping할 leaf 까지 순회한다.
	 * pa	top에서 leaf까지 각 level의 idr_layer를 저장.
	 *		[layer 0][layer 1]...[layer Top-1][TOP][NULL]
	 **/
	/** 20140719
	 * m이 IDR_SIZE와 같은 경우(layer안의 비트맵이 꽉 차 있을 경우), 
	 * 		현재 layer와 동일한 레벨의 다음 layer를 찾고,
	 * m이 IDR_SIZE와 같지 않은 경우(layer안에서 비트맵이 차 있지 않는 경우),
	 * 루프문을 통해 TOP으로부터 하위레이어로 이동하면서 leaf노드까지 이동하여
	 * id를 리턴한다.
	 */
	p = idp->top;
	l = idp->layers;
	pa[l--] = NULL;
	while (1) {
		/*
		 * We run around this while until we reach the leaf node...
		 */
		/** 20140712    
		 * n: id를 shift 시켜 bitmap에서 index를 찾는다.
		 * bm: bitmap을 반전시켰으므로 가용 id를 보유한 bit가 1이 된다.
		 * m: bitmap에서 n <= m <= IDR_SIZE 인 최소 index를 찾는다.
		 **/
		n = (id >> (IDR_BITS*l)) & IDR_MASK;
		bm = ~p->bitmap;
		m = find_next_bit(&bm, IDR_SIZE, n);
		/** 20140712    
		 * 해당 level의 bitmap이 모두 차 있다.
		 * 해당 level에서 가질 수 있는 모두 id가 사용 중이다.
		 **/
		if (m == IDR_SIZE) {
			/* no space available go back to previous layer. */
			/** 20140712    
			 * 옆 idr_layer의 가장 작은 값으로 id를 변경한다.
			 **/
			l++;
			oid = id;
			id = (id | ((1 << (IDR_BITS * l)) - 1)) + 1;

			/* if already at the top layer, we need to grow */
			/** 20140712    
			 * id가 현재 layer 구조에서 가질 수 있는 가장 큰 값보다 크다면
			 * starting_id를 변경하고 확장이 필요하다는 의미를 리턴한다.
			 **/
			if (id >= 1 << (idp->layers * IDR_BITS)) {
				*starting_id = id;
				return IDR_NEED_TO_GROW;
			}
			/** 20140712    
			 * 다시 이전 level의 idr_layer를 가져온다.
			 **/
			p = pa[l];
			BUG_ON(!p);

			/* If we need to go up one layer, continue the
			 * loop; otherwise, restart from the top.
			 */
			/** 20140712    
			 * 동일한 상위 layer에 속한다면 상위 layer에서 다시 시작하고,
			 * 그렇지 않다면 top에서부터 다시 시작한다.
			 **/
			sh = IDR_BITS * (l + 1);
			if (oid >> sh == id >> sh)
				continue;
			else
				goto restart;
		}
		/** 20140712    
		 * 해당 layer의 index 값과 찾은 값이 다르면
		 * bit n~(m-1)에 해당되는 값들은 사용 중이다.
		 * (m << sh)부터의 값이 할당할 수 있는 id가 된다.
		 **/
		if (m != n) {
			sh = IDR_BITS*l;
			id = ((id >> sh) ^ n ^ m) << sh;
		}
		/** 20140712    
		 * 생성된 id값 검사.
		 **/
		if ((id >= MAX_ID_BIT) || (id < 0))
			return IDR_NOMORE_SPACE;
		/** 20140712    
		 * leaf까지 도달했다면 break.
		 **/
		if (l == 0)
			break;
		/*
		 * Create the layer below if it is missing.
		 */
		/** 20140712    
		 * id를 할당할 idr_layer이 만들어지지 않았다면
		 * free_list로부터 idp를 가져와 생성하고 값을 채운다.
		 *
		 * idr_get_empty_slot에서 중간 node(idr_layer)는 생성하지 않았으므로,
		 * 그 사이의 node를 채운다.
		 **/
		if (!p->ary[m]) {
			new = get_from_free_list(idp);
			if (!new)
				return -1;
			new->layer = l-1;
			rcu_assign_pointer(p->ary[m], new);
			p->count++;
		}
		/** 20140712    
		 * 현재 level에 해당하는 위치에 현재 가리키는 idr_layer 위치를 저장한다.
		 **/
		pa[l--] = p;
		/** 20140712    
		 * leaf 노드 방향으로 하나 이동한다.
		 **/
		p = p->ary[m];
	}

	/** 20140712    
	 * p가 해당 layer의 새로운 idr_layer가 된다.
	 * 찾은 id를 리턴한다.
	 **/
	pa[l] = p;
	return id;
}

/** 20140705
 * starting id값보다 큰, 비어있는 slot에 해당하는 id값을 하나 가져온다
 */
static int idr_get_empty_slot(struct idr *idp, int starting_id,
			      struct idr_layer **pa)
{
	struct idr_layer *p, *new;
	int layers, v, id;
	unsigned long flags;

	id = starting_id;
build_up:
	/** 20140705
	 * idr->top이 존재하지 않을 경우 id_free의 리스트로부터
	 * idr_layer하나를 가져와 idr->top으로 한다
	 */
	p = idp->top;
	layers = idp->layers;
	if (unlikely(!p)) {
		/** 20140712    
		 * free list로 idr_layer를 받아오지 못한 경우 -1 리턴.
		 * _idr_rc_to_errno에 EAGAIN으로 판단.
		 **/
		if (!(p = get_from_free_list(idp)))
			return -1;
		p->layer = 0;
		layers = 1;
	}
	/*
	 * Add a new layer to the top of the tree if the requested
	 * id is larger than the currently allocated space.
	 */
	/** 20130705 
	 * id값이 현재 레이어의 구성된 값보다 클경우 새로운 레이어를 추가한다
	 */
	while ((layers < (MAX_LEVEL - 1)) && (id >= (1 << (layers*IDR_BITS)))) {
		layers++;
		/** 20140705
		 * idr_layer->layer : leaf로부터의 거리
		 * idr->layers : idr_layer의 계층수
		 * idr_layer가 비어있을 경우 idr_layer->layer 값을 1증가시켜 
		 * 레이어를 상위로 올려준다
		 */
		if (!p->count) {
			/* special case: if the tree is currently empty,
			 * then we grow the tree by moving the top node
			 * upwards.
			 */
			p->layer++;
			continue;
		}

		/** 20140705 
		 * id_free 리스트에서 idr_layer를 가져오는 데 실패했을 경우
		 * idr_layer의 p가 가리키고있는 id_layer부터 leaf까지의 계층의 
		 * ary[0] 을 id_free에 넣어주고 -1을 리턴한다. 
		 */
		if (!(new = get_from_free_list(idp))) {
			/*
			 * The allocation failed.  If we built part of
			 * the structure tear it down.
			 */
			spin_lock_irqsave(&idp->lock, flags);
			for (new = p; p && p != idp->top; new = p) {
				p = p->ary[0];
				new->ary[0] = NULL;
				new->bitmap = new->count = 0;
				__move_to_free_list(idp, new);
			}
			spin_unlock_irqrestore(&idp->lock, flags);
			return -1;
		}
		/** 20140705
		 * id_free 리스트에서 idr_layer를 가져온뒤 최상위 레이어로 한다.
		 * p->bitmap(ary[0]에 해당됨)이 FULL인 경우 0번 비트를 켜준다.
		 */
		new->ary[0] = p;
		new->count = 1;
		new->layer = layers-1;
		if (p->bitmap == IDR_FULL)
			__set_bit(0, &new->bitmap);
		p = new;
	}
	/** 20140705
	 * idp->top이 p를 가리키도록 rcu포인터 값을 할당한다.
	 */
	rcu_assign_pointer(idp->top, p);
	/** 20140705
	 * idp->layers를 갱신한다.
	 */
	idp->layers = layers;
	/** 20140705
	 * 요청된 id값 이후의 가용한 id값을 할당받아 v에 저장한다.
	 *
	 * IDR_NEED_TO_GROW가 리턴되었을 경우, id에는 가용한 starting_id가 저장된다.
	 */
	v = sub_alloc(idp, &id, pa);

	/** 20140705
	 * id값이 현재 idr_layer로 제공할 수 있는 값보다 크다면 
	 * build_up으로 가서 idr_layer를 추가한다.
	 */
	if (v == IDR_NEED_TO_GROW)
		goto build_up;
	return(v);
}

/** 20140705
 * 비어있는 id값을 정상적으로 가져왔다면, 
 * ptr을 pa[0]->ary[id & IDR_MASK]에 rcu포인터로 등록시킨다. 
 */
static int idr_get_new_above_int(struct idr *idp, void *ptr, int starting_id)
{
	/** 20140712    
	 * pa의 0은 leaf.
	 **/
	struct idr_layer *pa[MAX_LEVEL];
	int id;

	/** 20140712    
	 * starting_id 이상의 id값을 받아온다.
	 **/
	id = idr_get_empty_slot(idp, starting_id, pa);
	if (id >= 0) {
		/*
		 * Successfully found an empty slot.  Install the user
		 * pointer and mark the slot full.
		 */
		rcu_assign_pointer(pa[0]->ary[id & IDR_MASK],
				(struct idr_layer *)ptr);
		pa[0]->count++;
		/** 20140719
		 * layer가 full일때 id값에 대한 bitmap값을 설정한다
		 */
		idr_mark_full(pa, id);
	}

	return id;
}

/**
 * idr_get_new_above - allocate new idr entry above or equal to a start id
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @starting_id: id to start search at
 * @id: pointer to the allocated handle
 *
 * This is the allocate id function.  It should be called with any
 * required locks.
 *
 * If allocation from IDR's private freelist fails, idr_get_new_above() will
 * return %-EAGAIN.  The caller should retry the idr_pre_get() call to refill
 * IDR's preallocation and then retry the idr_get_new_above() call.
 *
 * If the idr is full idr_get_new_above() will return %-ENOSPC.
 *
 * @id returns a value in the range @starting_id ... %0x7fffffff
 */
/** 20140712    
 * ptr를 starting_id 이상의 정수값에 mapping 하고,
 * mapping된 handle을 id에 채워 리턴.
 **/
int idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id)
{
	int rv;

	/** 20140712    
	 * ptr를 mapping할 starting_id 이상의 integer handle을 받아온다.
	 **/
	rv = idr_get_new_above_int(idp, ptr, starting_id);
	/*
	 * This is a cheap hack until the IDR code can be fixed to
	 * return proper error values.
	 */
	/** 20140712    
	 * error return value에 따라 errno로 변환해 리턴.
	 **/
	if (rv < 0)
		return _idr_rc_to_errno(rv);
	/** 20140712    
	 * 성공적으로 id를 받아온 경우 매개변수에 채워 리턴.
	 **/
	*id = rv;
	return 0;
}
EXPORT_SYMBOL(idr_get_new_above);

/**
 * idr_get_new - allocate new idr entry
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @id: pointer to the allocated handle
 *
 * If allocation from IDR's private freelist fails, idr_get_new_above() will
 * return %-EAGAIN.  The caller should retry the idr_pre_get() call to refill
 * IDR's preallocation and then retry the idr_get_new_above() call.
 *
 * If the idr is full idr_get_new_above() will return %-ENOSPC.
 *
 * @id returns a value in the range %0 ... %0x7fffffff
 */
/** 20160109    
 * idr에서 ptr에 대한 핸들을 받아 리턴한다.
 *
 * starting id, 즉 최저 id 값을 지정하지 않는 버전.
 **/
int idr_get_new(struct idr *idp, void *ptr, int *id)
{
	int rv;

	rv = idr_get_new_above_int(idp, ptr, 0);
	/*
	 * This is a cheap hack until the IDR code can be fixed to
	 * return proper error values.
	 */
	if (rv < 0)
		return _idr_rc_to_errno(rv);
	*id = rv;
	return 0;
}
EXPORT_SYMBOL(idr_get_new);

/** 20140712    
 * 생성하지 않은 id에 대한 잘못된 제거 요청.
 **/
static void idr_remove_warning(int id)
{
	printk(KERN_WARNING
		"idr_remove called for id=%d which is not allocated.\n", id);
	dump_stack();
}

/** 20140712    
 * id를 idr_layer에서 제거한다.
 *
 * top에서 제거할 id를 보유한 leaf node를 찾아 내려간다.
 * leaf node에서 id를 제거하고 필요하면 찾아 내려온 path상의 idr_layer를 free한다.
 **/
static void sub_remove(struct idr *idp, int shift, int id)
{
	struct idr_layer *p = idp->top;
	struct idr_layer **pa[MAX_LEVEL];
	struct idr_layer ***paa = &pa[0];
	struct idr_layer *to_free;
	int n;

	/** 20140712    
	 * pa	top에서 leaf로 내려가며 순회한 각 level의 idr_layer를 저장.
	 *		[NULL][TOP ][layer N-2]...[layer 1][layer 0]
	 * paa  pa 배열의 멤버를 가리키는 pointer.
	 **/
	*paa = NULL;
	*++paa = &idp->top;

	/** 20140712    
	 * 전달받은 shift 크기 내에서 top부터 leaf까지 loop.
	 *
	 * bitmap에서 제거할 id에 해당하는 위치를 찾아 bit를 clear하고,
	 * paa를 통해 pa에 제거할 id를 가진 idr_layer 까지
	 * ary[n] 포인터를 저장한다.
	 **/
	while ((shift > 0) && p) {
		n = (id >> shift) & IDR_MASK;
		__clear_bit(n, &p->bitmap);
		*++paa = &p->ary[n];
		p = p->ary[n];
		shift -= IDR_BITS;
	}
	/** 20140712    
	 * id에 해당하는 index값을 계산하고,
	 * idr_layer에서 index에 해당하는 bit를 통해 mapping 여부를 확인한다.
	 **/
	n = id & IDR_MASK;
	if (likely(p != NULL && test_bit(n, &p->bitmap))){
		/** 20140712    
		 * integer mapping 정보를 제거한다.
		 **/
		__clear_bit(n, &p->bitmap);
		rcu_assign_pointer(p->ary[n], NULL);
		to_free = NULL;
		/** 20140712    
		 * pa에 채웠던 path를 역순회(leaf->top) 하며
		 * count를 감소시켜 0인 idr_layer를 free시킨다.
		 *
		 * 즉, 제거할 id까지 layer당 idr_layer가 하나씩만 존재했다면
		 * 하위 node가 사라진 상태의 node는 존재할 이유가 없으므로 제거한다.
		 **/
		while(*paa && ! --((**paa)->count)){
			if (to_free)
				free_layer(to_free);
			to_free = **paa;
			**paa-- = NULL;
		}
		/** 20140712    
		 * top까지 모두 제거된 상태.
		 **/
		if (!*paa)
			idp->layers = 0;
		if (to_free)
			free_layer(to_free);
	} else
		idr_remove_warning(id);
}

/**
 * idr_remove - remove the given id and free its slot
 * @idp: idr handle
 * @id: unique key
 */
/** 20140712    
 * id의 mapping을 해제하고, 빈 idr_layer는 free한다.
 **/
void idr_remove(struct idr *idp, int id)
{
	struct idr_layer *p;
	struct idr_layer *to_free;

	/* Mask off upper bits we don't use for the search. */
	/** 20140712    
	 * id는 MAX_ID_MASK를 적용시켜 처리할 수 있는 값으로 만든다.
	 **/
	id &= MAX_ID_MASK;

	/** 20140712    
	 * idp에서 id를 제거한다.
	 **/
	sub_remove(idp, (idp->layers - 1) * IDR_BITS, id);
	/** 20140712    
	 * top과 ary[0] 하나로만 이루어진 경우,
	 * ary[0]을 새로운 top으로 지정하고, top이었던 idr_layer를 해제한다.
	 **/
	if (idp->top && idp->top->count == 1 && (idp->layers > 1) &&
	    idp->top->ary[0]) {
		/*
		 * Single child at leftmost slot: we can shrink the tree.
		 * This level is not needed anymore since when layers are
		 * inserted, they are inserted at the top of the existing
		 * tree.
		 */
		to_free = idp->top;
		p = idp->top->ary[0];
		rcu_assign_pointer(idp->top, p);
		--idp->layers;
		to_free->bitmap = to_free->count = 0;
		free_layer(to_free);
	}
	/** 20140712    
	 * 해제한 결과, free_list에 등록된 idr_layer가 IDR_FREE_MAX 이상이 되면
	 * free_list에서 idr_layer를 하나 가져와 메모리를 해제한다.
	 **/
	while (idp->id_free_cnt >= IDR_FREE_MAX) {
		p = get_from_free_list(idp);
		/*
		 * Note: we don't call the rcu callback here, since the only
		 * layers that fall into the freelist are those that have been
		 * preallocated.
		 */
		kmem_cache_free(idr_layer_cache, p);
	}
	return;
}
EXPORT_SYMBOL(idr_remove);

/**
 * idr_remove_all - remove all ids from the given idr tree
 * @idp: idr handle
 *
 * idr_destroy() only frees up unused, cached idp_layers, but this
 * function will remove all id mappings and leave all idp_layers
 * unused.
 *
 * A typical clean-up sequence for objects stored in an idr tree will
 * use idr_for_each() to free all objects, if necessay, then
 * idr_remove_all() to remove all ids, and idr_destroy() to free
 * up the cached idr_layers.
 */
void idr_remove_all(struct idr *idp)
{
	int n, id, max;
	int bt_mask;
	struct idr_layer *p;
	struct idr_layer *pa[MAX_LEVEL];
	struct idr_layer **paa = &pa[0];

	n = idp->layers * IDR_BITS;
	p = idp->top;
	rcu_assign_pointer(idp->top, NULL);
	max = 1 << n;

	id = 0;
	while (id < max) {
		while (n > IDR_BITS && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = p->ary[(id >> n) & IDR_MASK];
		}

		bt_mask = id;
		id += 1 << n;
		/* Get the highest bit that the above add changed from 0->1. */
		while (n < fls(id ^ bt_mask)) {
			if (p)
				free_layer(p);
			n += IDR_BITS;
			p = *--paa;
		}
	}
	idp->layers = 0;
}
EXPORT_SYMBOL(idr_remove_all);

/**
 * idr_destroy - release all cached layers within an idr tree
 * @idp: idr handle
 */
void idr_destroy(struct idr *idp)
{
	while (idp->id_free_cnt) {
		struct idr_layer *p = get_from_free_list(idp);
		kmem_cache_free(idr_layer_cache, p);
	}
}
EXPORT_SYMBOL(idr_destroy);

/**
 * idr_find - return pointer for given id
 * @idp: idr handle
 * @id: lookup key
 *
 * Return the pointer given the id it has been registered with.  A %NULL
 * return indicates that @id is not valid or you passed %NULL in
 * idr_get_new().
 *
 * This function can be called under rcu_read_lock(), given that the leaf
 * pointers lifetimes are correctly managed.
 */
void *idr_find(struct idr *idp, int id)
{
	int n;
	struct idr_layer *p;

	p = rcu_dereference_raw(idp->top);
	if (!p)
		return NULL;
	n = (p->layer+1) * IDR_BITS;

	/* Mask off upper bits we don't use for the search. */
	id &= MAX_ID_MASK;

	if (id >= (1 << n))
		return NULL;
	BUG_ON(n == 0);

	while (n > 0 && p) {
		n -= IDR_BITS;
		BUG_ON(n != p->layer*IDR_BITS);
		p = rcu_dereference_raw(p->ary[(id >> n) & IDR_MASK]);
	}
	return((void *)p);
}
EXPORT_SYMBOL(idr_find);

/**
 * idr_for_each - iterate through all stored pointers
 * @idp: idr handle
 * @fn: function to be called for each pointer
 * @data: data passed back to callback function
 *
 * Iterate over the pointers registered with the given idr.  The
 * callback function will be called for each pointer currently
 * registered, passing the id, the pointer and the data pointer passed
 * to this function.  It is not safe to modify the idr tree while in
 * the callback, so functions such as idr_get_new and idr_remove are
 * not allowed.
 *
 * We check the return of @fn each time. If it returns anything other
 * than %0, we break out and return that value.
 *
 * The caller must serialize idr_for_each() vs idr_get_new() and idr_remove().
 */
int idr_for_each(struct idr *idp,
		 int (*fn)(int id, void *p, void *data), void *data)
{
	int n, id, max, error = 0;
	struct idr_layer *p;
	struct idr_layer *pa[MAX_LEVEL];
	struct idr_layer **paa = &pa[0];

	n = idp->layers * IDR_BITS;
	p = rcu_dereference_raw(idp->top);
	max = 1 << n;

	id = 0;
	while (id < max) {
		while (n > 0 && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = rcu_dereference_raw(p->ary[(id >> n) & IDR_MASK]);
		}

		if (p) {
			error = fn(id, (void *)p, data);
			if (error)
				break;
		}

		id += 1 << n;
		while (n < fls(id)) {
			n += IDR_BITS;
			p = *--paa;
		}
	}

	return error;
}
EXPORT_SYMBOL(idr_for_each);

/**
 * idr_get_next - lookup next object of id to given id.
 * @idp: idr handle
 * @nextidp:  pointer to lookup key
 *
 * Returns pointer to registered object with id, which is next number to
 * given id. After being looked up, *@nextidp will be updated for the next
 * iteration.
 *
 * This function can be called under rcu_read_lock(), given that the leaf
 * pointers lifetimes are correctly managed.
 */
void *idr_get_next(struct idr *idp, int *nextidp)
{
	struct idr_layer *p, *pa[MAX_LEVEL];
	struct idr_layer **paa = &pa[0];
	int id = *nextidp;
	int n, max;

	/* find first ent */
	p = rcu_dereference_raw(idp->top);
	if (!p)
		return NULL;
	n = (p->layer + 1) * IDR_BITS;
	max = 1 << n;

	while (id < max) {
		while (n > 0 && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = rcu_dereference_raw(p->ary[(id >> n) & IDR_MASK]);
		}

		if (p) {
			*nextidp = id;
			return p;
		}

		id += 1 << n;
		while (n < fls(id)) {
			n += IDR_BITS;
			p = *--paa;
		}
	}
	return NULL;
}
EXPORT_SYMBOL(idr_get_next);


/**
 * idr_replace - replace pointer for given id
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @id: lookup key
 *
 * Replace the pointer registered with an id and return the old value.
 * A %-ENOENT return indicates that @id was not found.
 * A %-EINVAL return indicates that @id was not within valid constraints.
 *
 * The caller must serialize with writers.
 */
void *idr_replace(struct idr *idp, void *ptr, int id)
{
	int n;
	struct idr_layer *p, *old_p;

	p = idp->top;
	if (!p)
		return ERR_PTR(-EINVAL);

	n = (p->layer+1) * IDR_BITS;

	id &= MAX_ID_MASK;

	if (id >= (1 << n))
		return ERR_PTR(-EINVAL);

	n -= IDR_BITS;
	while ((n > 0) && p) {
		p = p->ary[(id >> n) & IDR_MASK];
		n -= IDR_BITS;
	}

	n = id & IDR_MASK;
	if (unlikely(p == NULL || !test_bit(n, &p->bitmap)))
		return ERR_PTR(-ENOENT);

	old_p = p->ary[n];
	rcu_assign_pointer(p->ary[n], ptr);

	return old_p;
}
EXPORT_SYMBOL(idr_replace);

/** 20140510    
 * struct idr_layer용 kmem_cache를 생성한다.
 **/
void __init idr_init_cache(void)
{
	idr_layer_cache = kmem_cache_create("idr_layer_cache",
				sizeof(struct idr_layer), 0, SLAB_PANIC, NULL);
}

/**
 * idr_init - initialize idr handle
 * @idp:	idr handle
 *
 * This function is use to set up the handle (@idp) that you will pass
 * to the rest of the functions.
 */
/** 20140517    
 * idr handle을 초기화 하는 함수
 **/
void idr_init(struct idr *idp)
{
	memset(idp, 0, sizeof(struct idr));
	spin_lock_init(&idp->lock);
}
EXPORT_SYMBOL(idr_init);


/**
 * DOC: IDA description
 * IDA - IDR based ID allocator
 *
 * This is id allocator without id -> pointer translation.  Memory
 * usage is much lower than full blown idr because each id only
 * occupies a bit.  ida uses a custom leaf node which contains
 * IDA_BITMAP_BITS slots.
 *
 * 2007-04-25  written by Tejun Heo <htejun@gmail.com>
 */

/** 20150228    
 * 전달받은 bitmap을 ida의 free_bitmap으로 지정한다.
 **/
static void free_bitmap(struct ida *ida, struct ida_bitmap *bitmap)
{
	unsigned long flags;

	if (!ida->free_bitmap) {
		spin_lock_irqsave(&ida->idr.lock, flags);
		if (!ida->free_bitmap) {
			ida->free_bitmap = bitmap;
			bitmap = NULL;
		}
		spin_unlock_irqrestore(&ida->idr.lock, flags);
	}

	kfree(bitmap);
}

/**
 * ida_pre_get - reserve resources for ida allocation
 * @ida:	ida handle
 * @gfp_mask:	memory allocation flag
 *
 * This function should be called prior to locking and calling the
 * following function.  It preallocates enough memory to satisfy the
 * worst possible allocation.
 *
 * If the system is REALLY out of memory this function returns %0,
 * otherwise %1.
 */
/** 20150228    
 * ida 할당을 위해 필요한 자원을 할당받는다.
 *   - idr_layer를 할당받아 id_free 리스트에 채운다.
 *   - ida_bitmap을 할당받아 free_bitmap에 채운다.
 **/
int ida_pre_get(struct ida *ida, gfp_t gfp_mask)
{
	/* allocate idr_layers */
	/** 20150228    
	 * idr의 id_free 리스트에 object를 할당받아 채워넣는다.
	 **/
	if (!idr_pre_get(&ida->idr, gfp_mask))
		return 0;

	/* allocate free_bitmap */
	/** 20150228    
	 * free_bitmap을 할당받는다.
	 **/
	if (!ida->free_bitmap) {
		struct ida_bitmap *bitmap;

		/** 20150228    
		 * ida_bitmap을 할당받는다.
		 **/
		bitmap = kmalloc(sizeof(struct ida_bitmap), gfp_mask);
		if (!bitmap)
			return 0;

		free_bitmap(ida, bitmap);
	}

	return 1;
}
EXPORT_SYMBOL(ida_pre_get);

/**
 * ida_get_new_above - allocate new ID above or equal to a start id
 * @ida:	ida handle
 * @starting_id: id to start search at
 * @p_id:	pointer to the allocated handle
 *
 * Allocate new ID above or equal to @starting_id.  It should be called
 * with any required locks.
 *
 * If memory is required, it will return %-EAGAIN, you should unlock
 * and go back to the ida_pre_get() call.  If the ida is full, it will
 * return %-ENOSPC.
 *
 * @p_id returns a value in the range @starting_id ... %0x7fffffff.
 */
/** 20150228    
 * starting_id 이후의 새로운 id를 할당 받아 p_id에 채운다.
 **/
int ida_get_new_above(struct ida *ida, int starting_id, int *p_id)
{
	/** 20150228    
	 * 각 레벨당 idr_layer의 위치를 나타내는 임시 구조체.
	 **/
	struct idr_layer *pa[MAX_LEVEL];
	struct ida_bitmap *bitmap;
	unsigned long flags;
	/** 20150228    
	 * idr_id:
	 *   IDA BITMAP 크기로 계산했을 때, starting id를 포함하는 idr_layer의 위치.
	 * offset:
	 *   idr_id에서의 offset
	 **/
	int idr_id = starting_id / IDA_BITMAP_BITS;
	int offset = starting_id % IDA_BITMAP_BITS;
	int t, id;

 restart:
	/* get vacant slot */
	/** 20150228    
	 * idr 함수를 사용해 id를 하나 할당받아 온다.
	 **/
	t = idr_get_empty_slot(&ida->idr, idr_id, pa);
	if (t < 0)
		return _idr_rc_to_errno(t);

	/** 20150228    
	 * BITS로 계산해 최대치 이상이 리턴되었다면 해당 ida로 할당받을 수 있는
	 * 핸들이 모두 사용 중이다.
	 **/
	if (t * IDA_BITMAP_BITS >= MAX_ID_BIT)
		return -ENOSPC;

	if (t != idr_id)
		offset = 0;
	idr_id = t;

	/* if bitmap isn't there, create a new one */
	/** 20150228    
	 **/
	bitmap = (void *)pa[0]->ary[idr_id & IDR_MASK];
	if (!bitmap) {
		spin_lock_irqsave(&ida->idr.lock, flags);
		bitmap = ida->free_bitmap;
		ida->free_bitmap = NULL;
		spin_unlock_irqrestore(&ida->idr.lock, flags);

		if (!bitmap)
			return -EAGAIN;

		memset(bitmap, 0, sizeof(struct ida_bitmap));
		rcu_assign_pointer(pa[0]->ary[idr_id & IDR_MASK],
				(void *)bitmap);
		pa[0]->count++;
	}

	/* lookup for empty slot */
	/** 20150228    
	 * bitmap에서 비어있는 비트의 위치를 받아온다.
	 **/
	t = find_next_zero_bit(bitmap->bitmap, IDA_BITMAP_BITS, offset);
	if (t == IDA_BITMAP_BITS) {
		/** 20150228    
		 * offset 이후에 비어 있는 비트가 없으면 다음 chunk로 이동한다.
		 **/
		/* no empty slot after offset, continue to the next chunk */
		idr_id++;
		offset = 0;
		goto restart;
	}

	/** 20150228    
	 * 할당받은 id를 재계산한다.
	 **/
	id = idr_id * IDA_BITMAP_BITS + t;
	if (id >= MAX_ID_BIT)
		return -ENOSPC;

	/** 20150228    
	 * bitmap에 사용된 값을 표시한다.
	 **/
	__set_bit(t, bitmap->bitmap);
	/** 20150228    
	 * bitmap의 nr_busy는 id 사용 카운터이다.
	 * IDA_BITMAP_BITS에 도달했다면 모두 사용했다고 표시한다.
	 **/
	if (++bitmap->nr_busy == IDA_BITMAP_BITS)
		idr_mark_full(pa, idr_id);

	/** 20150228    
	 * 매개변수에 id를 채운다.
	 **/
	*p_id = id;

	/* Each leaf node can handle nearly a thousand slots and the
	 * whole idea of ida is to have small memory foot print.
	 * Throw away extra resources one by one after each successful
	 * allocation.
	 */
	/** 20150228    
	 * 많은 슬롯을 관리하므로 idr_layer에 불필요하게 많은 객체가 있다면 해제한다.
	 **/
	if (ida->idr.id_free_cnt || ida->free_bitmap) {
		/** 20150228    
		 * idr_layer를 하나 꺼내와 해제한다.
		 **/
		struct idr_layer *p = get_from_free_list(&ida->idr);
		if (p)
			kmem_cache_free(idr_layer_cache, p);
	}

	return 0;
}
EXPORT_SYMBOL(ida_get_new_above);

/**
 * ida_get_new - allocate new ID
 * @ida:	idr handle
 * @p_id:	pointer to the allocated handle
 *
 * Allocate new ID.  It should be called with any required locks.
 *
 * If memory is required, it will return %-EAGAIN, you should unlock
 * and go back to the idr_pre_get() call.  If the idr is full, it will
 * return %-ENOSPC.
 *
 * @p_id returns a value in the range %0 ... %0x7fffffff.
 */
/** 20150509    
 * 0 다음의 새로운 id를 할당 받아 p_id에 저장한다.
 **/
int ida_get_new(struct ida *ida, int *p_id)
{
	return ida_get_new_above(ida, 0, p_id);
}
EXPORT_SYMBOL(ida_get_new);

/**
 * ida_remove - remove the given ID
 * @ida:	ida handle
 * @id:		ID to free
 */
void ida_remove(struct ida *ida, int id)
{
	struct idr_layer *p = ida->idr.top;
	int shift = (ida->idr.layers - 1) * IDR_BITS;
	int idr_id = id / IDA_BITMAP_BITS;
	int offset = id % IDA_BITMAP_BITS;
	int n;
	struct ida_bitmap *bitmap;

	/* clear full bits while looking up the leaf idr_layer */
	while ((shift > 0) && p) {
		n = (idr_id >> shift) & IDR_MASK;
		__clear_bit(n, &p->bitmap);
		p = p->ary[n];
		shift -= IDR_BITS;
	}

	if (p == NULL)
		goto err;

	n = idr_id & IDR_MASK;
	__clear_bit(n, &p->bitmap);

	bitmap = (void *)p->ary[n];
	if (!test_bit(offset, bitmap->bitmap))
		goto err;

	/* update bitmap and remove it if empty */
	__clear_bit(offset, bitmap->bitmap);
	if (--bitmap->nr_busy == 0) {
		__set_bit(n, &p->bitmap);	/* to please idr_remove() */
		idr_remove(&ida->idr, idr_id);
		free_bitmap(ida, bitmap);
	}

	return;

 err:
	printk(KERN_WARNING
	       "ida_remove called for id=%d which is not allocated.\n", id);
}
EXPORT_SYMBOL(ida_remove);

/**
 * ida_destroy - release all cached layers within an ida tree
 * @ida:		ida handle
 */
void ida_destroy(struct ida *ida)
{
	idr_destroy(&ida->idr);
	kfree(ida->free_bitmap);
}
EXPORT_SYMBOL(ida_destroy);

/**
 * ida_simple_get - get a new id.
 * @ida: the (initialized) ida.
 * @start: the minimum id (inclusive, < 0x8000000)
 * @end: the maximum id (exclusive, < 0x8000000 or 0)
 * @gfp_mask: memory allocation flags
 *
 * Allocates an id in the range start <= id < end, or returns -ENOSPC.
 * On memory allocation failure, returns -ENOMEM.
 *
 * Use ida_simple_remove() to get rid of an id.
 */
int ida_simple_get(struct ida *ida, unsigned int start, unsigned int end,
		   gfp_t gfp_mask)
{
	int ret, id;
	unsigned int max;
	unsigned long flags;

	BUG_ON((int)start < 0);
	BUG_ON((int)end < 0);

	if (end == 0)
		max = 0x80000000;
	else {
		BUG_ON(end < start);
		max = end - 1;
	}

again:
	if (!ida_pre_get(ida, gfp_mask))
		return -ENOMEM;

	spin_lock_irqsave(&simple_ida_lock, flags);
	ret = ida_get_new_above(ida, start, &id);
	if (!ret) {
		if (id > max) {
			ida_remove(ida, id);
			ret = -ENOSPC;
		} else {
			ret = id;
		}
	}
	spin_unlock_irqrestore(&simple_ida_lock, flags);

	if (unlikely(ret == -EAGAIN))
		goto again;

	return ret;
}
EXPORT_SYMBOL(ida_simple_get);

/**
 * ida_simple_remove - remove an allocated id.
 * @ida: the (initialized) ida.
 * @id: the id returned by ida_simple_get.
 */
void ida_simple_remove(struct ida *ida, unsigned int id)
{
	unsigned long flags;

	BUG_ON((int)id < 0);
	spin_lock_irqsave(&simple_ida_lock, flags);
	ida_remove(ida, id);
	spin_unlock_irqrestore(&simple_ida_lock, flags);
}
EXPORT_SYMBOL(ida_simple_remove);

/**
 * ida_init - initialize ida handle
 * @ida:	ida handle
 *
 * This function is use to set up the handle (@ida) that you will pass
 * to the rest of the functions.
 */
/** 20150704    
 * IDA 핸들을 초기화 한다.
 **/
void ida_init(struct ida *ida)
{
	memset(ida, 0, sizeof(struct ida));
	idr_init(&ida->idr);

}
EXPORT_SYMBOL(ida_init);
