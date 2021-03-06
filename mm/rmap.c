/*
 * mm/rmap.c - physical to virtual reverse mappings
 *
 * Copyright 2001, Rik van Riel <riel@conectiva.com.br>
 * Released under the General Public License (GPL).
 *
 * Simple, low overhead reverse mapping scheme.
 * Please try to keep this thing as modular as possible.
 *
 * Provides methods for unmapping each kind of mapped page:
 * the anon methods track anonymous pages, and
 * the file methods track pages belonging to an inode.
 *
 * Original design by Rik van Riel <riel@conectiva.com.br> 2001
 * File methods by Dave McCracken <dmccr@us.ibm.com> 2003, 2004
 * Anonymous methods by Andrea Arcangeli <andrea@suse.de> 2004
 * Contributions by Hugh Dickins 2003, 2004
 */

/*
 * Lock ordering in mm:
 *
 * inode->i_mutex	(while writing or truncating, not reading or faulting)
 *   mm->mmap_sem
 *     page->flags PG_locked (lock_page)
 *       mapping->i_mmap_mutex
 *         anon_vma->mutex
 *           mm->page_table_lock or pte_lock
 *             zone->lru_lock (in mark_page_accessed, isolate_lru_page)
 *             swap_lock (in swap_duplicate, swap_info_get)
 *               mmlist_lock (in mmput, drain_mmlist and others)
 *               mapping->private_lock (in __set_page_dirty_buffers)
 *               inode->i_lock (in set_page_dirty's __mark_inode_dirty)
 *               bdi.wb->list_lock (in set_page_dirty's __mark_inode_dirty)
 *                 sb_lock (within inode_lock in fs/fs-writeback.c)
 *                 mapping->tree_lock (widely used, in set_page_dirty,
 *                           in arch-dependent flush_dcache_mmap_lock,
 *                           within bdi.wb->list_lock in __sync_single_inode)
 *
 * anon_vma->mutex,mapping->i_mutex      (memory_failure, collect_procs_anon)
 *   ->tasklist_lock
 *     pte map lock
 */

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/rcupdate.h>
#include <linux/export.h>
#include <linux/memcontrol.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/hugetlb.h>

#include <asm/tlbflush.h>

#include "internal.h"

static struct kmem_cache *anon_vma_cachep;
static struct kmem_cache *anon_vma_chain_cachep;

static inline struct anon_vma *anon_vma_alloc(void)
{
	struct anon_vma *anon_vma;

	anon_vma = kmem_cache_alloc(anon_vma_cachep, GFP_KERNEL);
	if (anon_vma) {
		atomic_set(&anon_vma->refcount, 1);
		/*
		 * Initialise the anon_vma root to point to itself. If called
		 * from fork, the root will be reset to the parents anon_vma.
		 */
		anon_vma->root = anon_vma;
	}

	return anon_vma;
}

/** 20140524
 * anon_vma를 해제한다.
 **/
static inline void anon_vma_free(struct anon_vma *anon_vma)
{
	/** 20140524
	 * refcount는 free 전에 0이 되어야 한다.
	 **/
	VM_BUG_ON(atomic_read(&anon_vma->refcount));

	/*
	 * Synchronize against page_lock_anon_vma() such that
	 * we can safely hold the lock without the anon_vma getting
	 * freed.
	 *
	 * Relies on the full mb implied by the atomic_dec_and_test() from
	 * put_anon_vma() against the acquire barrier implied by
	 * mutex_trylock() from page_lock_anon_vma(). This orders:
	 *
	 * page_lock_anon_vma()		VS	put_anon_vma()
	 *   mutex_trylock()			  atomic_dec_and_test()
	 *   LOCK				  MB
	 *   atomic_read()			  mutex_is_locked()
	 *
	 * LOCK should suffice since the actual taking of the lock must
	 * happen _before_ what follows.
	 */
	/** 20140524
	 * root에 lock이 걸려 있는데, 왜 lock/unlock을 호출해 주는 것일까?
	 **/
	if (mutex_is_locked(&anon_vma->root->mutex)) {
		anon_vma_lock(anon_vma);
		anon_vma_unlock(anon_vma);
	}

	/** 20140524
	 * slab object를 반환한다.
	 **/
	kmem_cache_free(anon_vma_cachep, anon_vma);
}

/** 20160416
 * anon_vma_chain 인스턴스를 kmem_cache로부터 받아온다.
 **/
static inline struct anon_vma_chain *anon_vma_chain_alloc(gfp_t gfp)
{
	return kmem_cache_alloc(anon_vma_chain_cachep, gfp);
}

static void anon_vma_chain_free(struct anon_vma_chain *anon_vma_chain)
{
	kmem_cache_free(anon_vma_chain_cachep, anon_vma_chain);
}

/** 20160416
 * fork된 child를 위해 할당받은 vma를 avc와 연결하고,
 * avc를 anon_vma와 연결한다.
 *
 * [참고] https://lwn.net/Articles/383162/
 * [참고] http://www.2cto.com/os/201411/349997.html
 *
 * 매개변수 avc는 dst가 사용하기 위해 할당 받은 avc.
 * vma는 dst vma.
 * anon_vma는 src의 avc가 가리키던 anon_vma.
 **/
static void anon_vma_chain_link(struct vm_area_struct *vma,
				struct anon_vma_chain *avc,
				struct anon_vma *anon_vma)
{
	/** 20160416
	 * (dst용으로 할당받은)avc의 vma를 dst의 vma로 지정.
	 **/
	avc->vma = vma;
	/** 20160416
	 * (dst용으로 할당받은)avc의 anon_vma를 src의 anon_vma로 지정.
	 * 즉, src와 같은 anon_vma를 자신의 avc로 가리키게 된다.
	 **/
	avc->anon_vma = anon_vma;
	/** 20160416
	 * child vma의 anon_vma_chain 리스트에 (dst용으로 할당받은)avc를 등록한다.
	 **/
	list_add(&avc->same_vma, &vma->anon_vma_chain);

	/*
	 * It's critical to add new vmas to the tail of the anon_vma,
	 * see comment in huge_memory.c:__split_huge_page().
	 */
	/** 20160416
	 * 기존 anon_vma 리스트의 끝에 avc의 same_anon_vma 리스트 노드를 연결한다.
	 **/
	list_add_tail(&avc->same_anon_vma, &anon_vma->head);
}

/**
 * anon_vma_prepare - attach an anon_vma to a memory region
 * @vma: the memory region in question
 *
 * This makes sure the memory mapping described by 'vma' has
 * an 'anon_vma' attached to it, so that we can associate the
 * anonymous pages mapped into it with that anon_vma.
 *
 * The common case will be that we already have one, but if
 * not we either need to find an adjacent mapping that we
 * can re-use the anon_vma from (very common when the only
 * reason for splitting a vma has been mprotect()), or we
 * allocate a new one.
 *
 * Anon-vma allocations are very subtle, because we may have
 * optimistically looked up an anon_vma in page_lock_anon_vma()
 * and that may actually touch the spinlock even in the newly
 * allocated vma (it depends on RCU to make sure that the
 * anon_vma isn't actually destroyed).
 *
 * As a result, we need to do proper anon_vma locking even
 * for the new allocation. At the same time, we do not want
 * to do any locking for the common case of already having
 * an anon_vma.
 *
 * This must be called with the mmap_sem held for reading.
 */
int anon_vma_prepare(struct vm_area_struct *vma)
{
	struct anon_vma *anon_vma = vma->anon_vma;
	struct anon_vma_chain *avc;

	might_sleep();
	if (unlikely(!anon_vma)) {
		struct mm_struct *mm = vma->vm_mm;
		struct anon_vma *allocated;

		avc = anon_vma_chain_alloc(GFP_KERNEL);
		if (!avc)
			goto out_enomem;

		anon_vma = find_mergeable_anon_vma(vma);
		allocated = NULL;
		if (!anon_vma) {
			anon_vma = anon_vma_alloc();
			if (unlikely(!anon_vma))
				goto out_enomem_free_avc;
			allocated = anon_vma;
		}

		anon_vma_lock(anon_vma);
		/* page_table_lock to protect against threads */
		spin_lock(&mm->page_table_lock);
		if (likely(!vma->anon_vma)) {
			vma->anon_vma = anon_vma;
			anon_vma_chain_link(vma, avc, anon_vma);
			allocated = NULL;
			avc = NULL;
		}
		spin_unlock(&mm->page_table_lock);
		anon_vma_unlock(anon_vma);

		if (unlikely(allocated))
			put_anon_vma(allocated);
		if (unlikely(avc))
			anon_vma_chain_free(avc);
	}
	return 0;

 out_enomem_free_avc:
	anon_vma_chain_free(avc);
 out_enomem:
	return -ENOMEM;
}

/*
 * This is a useful helper function for locking the anon_vma root as
 * we traverse the vma->anon_vma_chain, looping over anon_vma's that
 * have the same vma.
 *
 * Such anon_vma's should have the same root, so you'd expect to see
 * just a single mutex_lock for the whole traversal.
 */
static inline struct anon_vma *lock_anon_vma_root(struct anon_vma *root, struct anon_vma *anon_vma)
{
	struct anon_vma *new_root = anon_vma->root;
	if (new_root != root) {
		if (WARN_ON_ONCE(root))
			mutex_unlock(&root->mutex);
		root = new_root;
		mutex_lock(&root->mutex);
	}
	return root;
}

static inline void unlock_anon_vma_root(struct anon_vma *root)
{
	if (root)
		mutex_unlock(&root->mutex);
}

/*
 * Attach the anon_vmas from src to dst.
 * Returns 0 on success, -ENOMEM on failure.
 */
/** 20160423
 * src의 VMA에서 same_vma를 따라가며 각 avc마다
 * 연결하기 위한 dst avc를 할당받고 연결 한다.
 *
 * 1. vma는 fork된 경우일지라도 VM을 share하지 않는 경우 독자적인 vma를
 *    인스턴스를 갖는다. COW기법을 사용하므로 
 *
 *
 * [참고] http://www.2cto.com/os/201411/349997.html
 * [참고] https://lwn.net/Articles/383162/
 **/
int anon_vma_clone(struct vm_area_struct *dst, struct vm_area_struct *src)
{
	struct anon_vma_chain *avc, *pavc;
	struct anon_vma *root = NULL;

	/** 20160416
	 * src의 avc부터 same_vma 리스트를 순회한다.
	 **/
	list_for_each_entry_reverse(pavc, &src->anon_vma_chain, same_vma) {
		struct anon_vma *anon_vma;

		/** 20160416
		 * avc를 하나 할당 받는다.
		 **/
		avc = anon_vma_chain_alloc(GFP_NOWAIT | __GFP_NOWARN);
		if (unlikely(!avc)) {
			unlock_anon_vma_root(root);
			root = NULL;
			avc = anon_vma_chain_alloc(GFP_KERNEL);
			if (!avc)
				goto enomem_failure;
		}
		/** 20160416
		 * src의 same_vma 리스트의 현재 avc가 가리키는 anon_vma를 받아온다.
		 **/
		anon_vma = pavc->anon_vma;
		root = lock_anon_vma_root(root, anon_vma);
		/** 20160423
		 * src vma와 src avc, src avc와 dst anon_vma를 연결한다.
		 **/
		anon_vma_chain_link(dst, avc, anon_vma);
	}
	unlock_anon_vma_root(root);
	return 0;

 enomem_failure:
	unlink_anon_vmas(dst);
	return -ENOMEM;
}

/*
 * Some rmap walk that needs to find all ptes/hugepmds without false
 * negatives (like migrate and split_huge_page) running concurrent
 * with operations that copy or move pagetables (like mremap() and
 * fork()) to be safe. They depend on the anon_vma "same_anon_vma"
 * list to be in a certain order: the dst_vma must be placed after the
 * src_vma in the list. This is always guaranteed by fork() but
 * mremap() needs to call this function to enforce it in case the
 * dst_vma isn't newly allocated and chained with the anon_vma_clone()
 * function but just an extension of a pre-existing vma through
 * vma_merge.
 *
 * NOTE: the same_anon_vma list can still be changed by other
 * processes while mremap runs because mremap doesn't hold the
 * anon_vma mutex to prevent modifications to the list while it
 * runs. All we need to enforce is that the relative order of this
 * process vmas isn't changing (we don't care about other vmas
 * order). Each vma corresponds to an anon_vma_chain structure so
 * there's no risk that other processes calling anon_vma_moveto_tail()
 * and changing the same_anon_vma list under mremap() will screw with
 * the relative order of this process vmas in the list, because we
 * they can't alter the order of any vma that belongs to this
 * process. And there can't be another anon_vma_moveto_tail() running
 * concurrently with mremap() coming from this process because we hold
 * the mmap_sem for the whole mremap(). fork() ordering dependency
 * also shouldn't be affected because fork() only cares that the
 * parent vmas are placed in the list before the child vmas and
 * anon_vma_moveto_tail() won't reorder vmas from either the fork()
 * parent or child.
 */
void anon_vma_moveto_tail(struct vm_area_struct *dst)
{
	struct anon_vma_chain *pavc;
	struct anon_vma *root = NULL;

	list_for_each_entry_reverse(pavc, &dst->anon_vma_chain, same_vma) {
		struct anon_vma *anon_vma = pavc->anon_vma;
		VM_BUG_ON(pavc->vma != dst);
		root = lock_anon_vma_root(root, anon_vma);
		list_del(&pavc->same_anon_vma);
		list_add_tail(&pavc->same_anon_vma, &anon_vma->head);
	}
	unlock_anon_vma_root(root);
}

/*
 * Attach vma to its own anon_vma, as well as to the anon_vmas that
 * the corresponding VMA in the parent process is attached to.
 * Returns 0 on success, non-zero on failure.
 */
/** 20160430
 * 자식 프로세스를 위해 생성한  vma를 자신의 anon_vma에 연결한다.
 * 뿐만 아니라 vma를 부모 프로세스에 있는, 대응하는 vma가 붙어 있는
 * anon_vma들에도 붙인다.
 **/
int anon_vma_fork(struct vm_area_struct *vma, struct vm_area_struct *pvma)
{
	struct anon_vma_chain *avc;
	struct anon_vma *anon_vma;

	/* Don't bother if the parent process has no anon_vma here. */
	/** 20160514
	 * 복사할 parent의 vma가 anon_vma를 갖지 않으면 복사할 것이 없으므로 리턴.
	 **/
	if (!pvma->anon_vma)
		return 0;

	/*
	 * First, attach the new VMA to the parent VMA's anon_vmas,
	 * so rmap can find non-COWed pages in child processes.
	 */
	/** 20160423
	 * 할당 받은 child의 vma를 parent VMA's의 anon_vma들에 각각 연결한다.
	 * child용 avc 할당과 av 연결은 아래에서 이뤄진다.
	 *
	 * parent - forked child (1) - forked child (2) - forked child (3)인 경우
	 * forked child (3)은 각 avc마다 다른 AV를 지정하므로 anon_vma'들'을
	 * 연결하는 것이다.
	 **/
	if (anon_vma_clone(vma, pvma))
		return -ENOMEM;

	/* Then add our own anon_vma. */
	/** 20160416
	 * fork된 task를 위한 anon_vma를 할당 받는다.
	 **/
	anon_vma = anon_vma_alloc();
	if (!anon_vma)
		goto out_error;
	/** 20160416
	 * fork되는 task를 위한 avc를 할당.
	 **/
	avc = anon_vma_chain_alloc(GFP_KERNEL);
	if (!avc)
		goto out_error_free_anon_vma;

	/*
	 * The root anon_vma's spinlock is the lock actually used when we
	 * lock any of the anon_vmas in this anon_vma tree.
	 */
	/** 20160423
	 * fork된 task를 위해 할당한 AV의 root를 parent의 AV의 root로 한다.
	 **/
	anon_vma->root = pvma->anon_vma->root;
	/*
	 * With refcounts, an anon_vma can stay around longer than the
	 * process it belongs to. The root anon_vma needs to be pinned until
	 * this anon_vma is freed, because the lock lives in the root.
	 */
	/** 20160423
	 * anon_vma root의 레퍼런스 카운트를 증가시킨다.
	 **/
	get_anon_vma(anon_vma->root);
	/* Mark this anon_vma as the one where our new (COWed) pages go. */
	/** 20160423
	 * child의 vma가 child를 위해 생성한 anon_vma를 가리킨다.
	 **/
	vma->anon_vma = anon_vma;
	anon_vma_lock(anon_vma);
	/** 20160423
	 * child의 vma와 child의 avc, child의 avc와 child의 anon_vma를 연결한다.
	 **/
	anon_vma_chain_link(vma, avc, anon_vma);
	anon_vma_unlock(anon_vma);

	return 0;

 out_error_free_anon_vma:
	put_anon_vma(anon_vma);
 out_error:
	unlink_anon_vmas(vma);
	return -ENOMEM;
}

void unlink_anon_vmas(struct vm_area_struct *vma)
{
	struct anon_vma_chain *avc, *next;
	struct anon_vma *root = NULL;

	/*
	 * Unlink each anon_vma chained to the VMA.  This list is ordered
	 * from newest to oldest, ensuring the root anon_vma gets freed last.
	 */
	list_for_each_entry_safe(avc, next, &vma->anon_vma_chain, same_vma) {
		struct anon_vma *anon_vma = avc->anon_vma;

		root = lock_anon_vma_root(root, anon_vma);
		list_del(&avc->same_anon_vma);

		/*
		 * Leave empty anon_vmas on the list - we'll need
		 * to free them outside the lock.
		 */
		if (list_empty(&anon_vma->head))
			continue;

		list_del(&avc->same_vma);
		anon_vma_chain_free(avc);
	}
	unlock_anon_vma_root(root);

	/*
	 * Iterate the list once more, it now only contains empty and unlinked
	 * anon_vmas, destroy them. Could not do before due to __put_anon_vma()
	 * needing to acquire the anon_vma->root->mutex.
	 */
	list_for_each_entry_safe(avc, next, &vma->anon_vma_chain, same_vma) {
		struct anon_vma *anon_vma = avc->anon_vma;

		put_anon_vma(anon_vma);

		list_del(&avc->same_vma);
		anon_vma_chain_free(avc);
	}
}

/** 20150207
 * anon_vma object(slub object)를 만들 때마다 호출하는 콜백함수.
 *
 * object의 mutex, refcount, list head를 초기화 한다.
 **/
static void anon_vma_ctor(void *data)
{
	struct anon_vma *anon_vma = data;

	mutex_init(&anon_vma->mutex);
	atomic_set(&anon_vma->refcount, 0);
	INIT_LIST_HEAD(&anon_vma->head);
}

/** 20150207
 * anon_vma를 위한 kmem_cache를 생성한다.
 *   "anon_vma", "anon_vma_chain"
 **/
void __init anon_vma_init(void)
{
	anon_vma_cachep = kmem_cache_create("anon_vma", sizeof(struct anon_vma),
			0, SLAB_DESTROY_BY_RCU|SLAB_PANIC, anon_vma_ctor);
	anon_vma_chain_cachep = KMEM_CACHE(anon_vma_chain, SLAB_PANIC);
}

/*
 * Getting a lock on a stable anon_vma from a page off the LRU is tricky!
 *
 * Since there is no serialization what so ever against page_remove_rmap()
 * the best this function can do is return a locked anon_vma that might
 * have been relevant to this page.
 *
 * The page might have been remapped to a different anon_vma or the anon_vma
 * returned may already be freed (and even reused).
 *
 * In case it was remapped to a different anon_vma, the new anon_vma will be a
 * child of the old anon_vma, and the anon_vma lifetime rules will therefore
 * ensure that any anon_vma obtained from the page will still be valid for as
 * long as we observe page_mapped() [ hence all those page_mapped() tests ].
 *
 * All users of this function must be very careful when walking the anon_vma
 * chain and verify that the page in question is indeed mapped in it
 * [ something equivalent to page_mapped_in_vma() ].
 *
 * Since anon_vma's slab is DESTROY_BY_RCU and we know from page_remove_rmap()
 * that the anon_vma pointer from page->mapping is valid if there is a
 * mapcount, we can dereference the anon_vma after observing those.
 */
struct anon_vma *page_get_anon_vma(struct page *page)
{
	struct anon_vma *anon_vma = NULL;
	unsigned long anon_mapping;

	rcu_read_lock();
	anon_mapping = (unsigned long) ACCESS_ONCE(page->mapping);
	if ((anon_mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON)
		goto out;
	if (!page_mapped(page))
		goto out;

	anon_vma = (struct anon_vma *) (anon_mapping - PAGE_MAPPING_ANON);
	if (!atomic_inc_not_zero(&anon_vma->refcount)) {
		anon_vma = NULL;
		goto out;
	}

	/*
	 * If this page is still mapped, then its anon_vma cannot have been
	 * freed.  But if it has been unmapped, we have no security against the
	 * anon_vma structure being freed and reused (for another anon_vma:
	 * SLAB_DESTROY_BY_RCU guarantees that - so the atomic_inc_not_zero()
	 * above cannot corrupt).
	 */
	if (!page_mapped(page)) {
		put_anon_vma(anon_vma);
		anon_vma = NULL;
	}
out:
	rcu_read_unlock();

	return anon_vma;
}

/*
 * Similar to page_get_anon_vma() except it locks the anon_vma.
 *
 * Its a little more complex as it tries to keep the fast path to a single
 * atomic op -- the trylock. If we fail the trylock, we fall back to getting a
 * reference like with page_get_anon_vma() and then block on the mutex.
 */
/** 20140524
 * page의 anon_vma에 lock을 걸고 anon_vma를 리턴한다.
 * 실패했다면 NULL이 리턴된다.
 *
 * atomic op으로 fast path로 trylock하고,
 * 실패했을 경우 mutex lock을 시도한다.
 **/
struct anon_vma *page_lock_anon_vma(struct page *page)
{
	struct anon_vma *anon_vma = NULL;
	struct anon_vma *root_anon_vma;
	unsigned long anon_mapping;

	rcu_read_lock();
	/** 20140524
	 * 현재 page의 mapping 정보를 읽어온다.
	 **/
	anon_mapping = (unsigned long) ACCESS_ONCE(page->mapping);
	/** 20140524
	 * PAGE_MAPPING_FLAGS가 PAGE_MAPPING_ANON 이 아닌 경우 out.
	 * page가 mapping table에 등록되지 않은 경우 out.
	 **/
	if ((anon_mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON)
		goto out;
	if (!page_mapped(page))
		goto out;

	/** 20140524
	 * flag를 빼고 anon_vma 주소만 가져온다.
	 **/
	anon_vma = (struct anon_vma *) (anon_mapping - PAGE_MAPPING_ANON);
	/** 20140524
	 * anon_vma의 root를 가져온다.
	 **/
	root_anon_vma = ACCESS_ONCE(anon_vma->root);
	/** 20140524
	 * root anon_vma에 lock을 시도한다.
	 **/
	if (mutex_trylock(&root_anon_vma->mutex)) {
		/*
		 * If the page is still mapped, then this anon_vma is still
		 * its anon_vma, and holding the mutex ensures that it will
		 * not go away, see anon_vma_free().
		 */
		/** 20140524
		 * lock을 획득한 경우 goto out.
		 * page가 mapping이 되어 있지 않다면 lock을 해제하고 anon_vma는 NULL
		 **/
		if (!page_mapped(page)) {
			mutex_unlock(&root_anon_vma->mutex);
			anon_vma = NULL;
		}
		goto out;
	}

	/* trylock failed, we got to sleep */
	/** 20140524
	 * root lock을 획득하지 못한 경우 - refcount를 증가하는데, 실패한 경우
	 * anon_vma = NULL로 out.
	 **/
	if (!atomic_inc_not_zero(&anon_vma->refcount)) {
		anon_vma = NULL;
		goto out;
	}

	/** 20140524
	 * root lock을 획득하지 못한 경우 - page가 mapping 되지 않은 경우
	 **/
	if (!page_mapped(page)) {
		/** 20140524
		 * 바로 위에서 refcount를 증가시켰으므로 put으로 refcount를 감소시킨다.
		 **/
		put_anon_vma(anon_vma);
		anon_vma = NULL;
		goto out;
	}

	/* we pinned the anon_vma, its safe to sleep */
	rcu_read_unlock();
	/** 20140524
	 * root lock을 획득하지 못한 경우 - anon_vma에 lock을 건다.
	 *   그러나 내부적으로 root에 mutex lock을 거는 함수를 호출한다.
	 **/
	anon_vma_lock(anon_vma);

	/** 20140524
	 * anon_vma에 refcount를 감소시켜 0이 되었다면 
	 * unlock, put(refcount가 0이 되었을 때 memory 해제), anon_vma는 NULL.
	 **/
	if (atomic_dec_and_test(&anon_vma->refcount)) {
		/*
		 * Oops, we held the last refcount, release the lock
		 * and bail -- can't simply use put_anon_vma() because
		 * we'll deadlock on the anon_vma_lock() recursion.
		 */
		anon_vma_unlock(anon_vma);
		__put_anon_vma(anon_vma);
		anon_vma = NULL;
	}

	/** 20140524
	 * anon_vam를 리턴한다.
	 **/
	return anon_vma;

out:
	rcu_read_unlock();
	return anon_vma;
}

void page_unlock_anon_vma(struct anon_vma *anon_vma)
{
	anon_vma_unlock(anon_vma);
}

/*
 * At what user virtual address is page expected in @vma?
 * Returns virtual address or -EFAULT if page's index/offset is not
 * within the range mapped the @vma.
 */
/** 20140531
 * page(Page Frame)가 주어진 vm_area_struct에 매핑된 가상 주소를 가져온다.
 **/
inline unsigned long
vma_address(struct page *page, struct vm_area_struct *vma)
{
	/** 20140531
	 * page의 mapping index 정보를 가져온다.
	 **/
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	unsigned long address;

	/** 20140531
	 * hugetlb page인 경우 pgoff을 보정한다.
	 **/
	if (unlikely(is_vm_hugetlb_page(vma)))
		pgoff = page->index << huge_page_order(page_hstate(page));
	/** 20140531
	 * vm area의 시작 주소에서 page의 offset을 더해 page에 해당하는 VA를 리턴한다.
	 **/
	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	if (unlikely(address < vma->vm_start || address >= vma->vm_end)) {
		/* page should be within @vma mapping range */
		return -EFAULT;
	}
	return address;
}

/*
 * At what user virtual address is page expected in vma?
 * Caller should check the page is actually part of the vma.
 */
unsigned long page_address_in_vma(struct page *page, struct vm_area_struct *vma)
{
	if (PageAnon(page)) {
		struct anon_vma *page__anon_vma = page_anon_vma(page);
		/*
		 * Note: swapoff's unuse_vma() is more efficient with this
		 * check, and needs it to match anon_vma when KSM is active.
		 */
		if (!vma->anon_vma || !page__anon_vma ||
		    vma->anon_vma->root != page__anon_vma->root)
			return -EFAULT;
	} else if (page->mapping && !(vma->vm_flags & VM_NONLINEAR)) {
		if (!vma->vm_file ||
		    vma->vm_file->f_mapping != page->mapping)
			return -EFAULT;
	} else
		return -EFAULT;
	return vma_address(page, vma);
}

/*
 * Check that @page is mapped at @address into @mm.
 *
 * If @sync is false, page_check_address may perform a racy check to avoid
 * the page table lock when the pte is not present (helpful when reclaiming
 * highly shared pages).
 *
 * On success returns with pte mapped and locked.
 */
/** 20140531
 * page descriptor가 가리키는 page frame 번호와
 * address에 대한 pte entry가 가리키는 page frame 번호가 같은지 검사해,
 * 일치할 경우 pte에 대한 lock을 획득한 상태에서 pte entry를 리턴하는 함수.
 **/
pte_t *__page_check_address(struct page *page, struct mm_struct *mm,
			  unsigned long address, spinlock_t **ptlp, int sync)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;

	/** 20140531
	 * hugepage는 설정되어 있지 않아 분석 제외
	 **/
	if (unlikely(PageHuge(page))) {
		pte = huge_pte_offset(mm, address);
		ptl = &mm->page_table_lock;
		goto check;
	}

	/** 20140531
	 * mm_struct을 참고하여 process영역 address가 mapping 된 pagetable entry 를 찾는다.
	 **/
	pgd = pgd_offset(mm, address);
	if (!pgd_present(*pgd))
		return NULL;

	pud = pud_offset(pgd, address);
	if (!pud_present(*pud))
		return NULL;

	pmd = pmd_offset(pud, address);
	if (!pmd_present(*pmd))
		return NULL;
	if (pmd_trans_huge(*pmd))
		return NULL;

	/** 20140531
	 * 위 단계를 거쳐 최종적으로 pte entry의 주소를 가져온다.
	 **/
	pte = pte_offset_map(pmd, address);
	/* Make a quick check before getting the lock */
	/** 20140531
	 * pte entry가 가리키는 page가 메모리 상에 존재하지 않을 경우 NULL을 리턴.
	 **/
	if (!sync && !pte_present(*pte)) {
		pte_unmap(pte);
		return NULL;
	}

	/** 20140531
	 * pmd가 가리키는 페이지에 대한 descriptor의 lock  변수 주소를 가져온다.
	 **/
	ptl = pte_lockptr(mm, pmd);
check:
	/** 20140531
	 **/
	spin_lock(ptl);
	/** 20140531
	 * pte entry가 가리키는 page가 메모리 상에 위치해 있고,
	 * 전달된 page의 pfn과 pte entry에 저장된 pfn이 같은지 검사.
	 *
	 * pte lock을 획득한 상태로, pte entry의 위치를 넘겨준다.
	 **/
	if (pte_present(*pte) && page_to_pfn(page) == pte_pfn(*pte)) {
		*ptlp = ptl;
		return pte;
	}
	pte_unmap_unlock(pte, ptl);
	return NULL;
}

/**
 * page_mapped_in_vma - check whether a page is really mapped in a VMA
 * @page: the page to test
 * @vma: the VMA to test
 *
 * Returns 1 if the page is mapped into the page tables of the VMA, 0
 * if the page is not mapped into the page tables of this VMA.  Only
 * valid for normal file or anonymous VMAs.
 */
int page_mapped_in_vma(struct page *page, struct vm_area_struct *vma)
{
	unsigned long address;
	pte_t *pte;
	spinlock_t *ptl;

	address = vma_address(page, vma);
	if (address == -EFAULT)		/* out of vma range */
		return 0;
	pte = page_check_address(page, vma->vm_mm, address, &ptl, 1);
	if (!pte)			/* the page is not in this mm */
		return 0;
	pte_unmap_unlock(pte, ptl);

	return 1;
}

/*
 * Subfunctions of page_referenced: page_referenced_one called
 * repeatedly from either page_referenced_anon or page_referenced_file.
 */
int page_referenced_one(struct page *page, struct vm_area_struct *vma,
			unsigned long address, unsigned int *mapcount,
			unsigned long *vm_flags)
{
	struct mm_struct *mm = vma->vm_mm;
	int referenced = 0;

	if (unlikely(PageTransHuge(page))) {
		pmd_t *pmd;

		spin_lock(&mm->page_table_lock);
		/*
		 * rmap might return false positives; we must filter
		 * these out using page_check_address_pmd().
		 */
		pmd = page_check_address_pmd(page, mm, address,
					     PAGE_CHECK_ADDRESS_PMD_FLAG);
		if (!pmd) {
			spin_unlock(&mm->page_table_lock);
			goto out;
		}

		if (vma->vm_flags & VM_LOCKED) {
			spin_unlock(&mm->page_table_lock);
			*mapcount = 0;	/* break early from loop */
			*vm_flags |= VM_LOCKED;
			goto out;
		}

		/* go ahead even if the pmd is pmd_trans_splitting() */
		if (pmdp_clear_flush_young_notify(vma, address, pmd))
			referenced++;
		spin_unlock(&mm->page_table_lock);
	} else {
		pte_t *pte;
		spinlock_t *ptl;

		/*
		 * rmap might return false positives; we must filter
		 * these out using page_check_address().
		 */
		pte = page_check_address(page, mm, address, &ptl, 0);
		if (!pte)
			goto out;

		if (vma->vm_flags & VM_LOCKED) {
			pte_unmap_unlock(pte, ptl);
			*mapcount = 0;	/* break early from loop */
			*vm_flags |= VM_LOCKED;
			goto out;
		}

		if (ptep_clear_flush_young_notify(vma, address, pte)) {
			/*
			 * Don't treat a reference through a sequentially read
			 * mapping as such.  If the page has been used in
			 * another mapping, we will catch it; if this other
			 * mapping is already gone, the unmap path will have
			 * set PG_referenced or activated the page.
			 */
			if (likely(!VM_SequentialReadHint(vma)))
				referenced++;
		}
		pte_unmap_unlock(pte, ptl);
	}

	(*mapcount)--;

	if (referenced)
		*vm_flags |= vma->vm_flags;
out:
	return referenced;
}

static int page_referenced_anon(struct page *page,
				struct mem_cgroup *memcg,
				unsigned long *vm_flags)
{
	unsigned int mapcount;
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	int referenced = 0;

	anon_vma = page_lock_anon_vma(page);
	if (!anon_vma)
		return referenced;

	mapcount = page_mapcount(page);
	list_for_each_entry(avc, &anon_vma->head, same_anon_vma) {
		struct vm_area_struct *vma = avc->vma;
		unsigned long address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		/*
		 * If we are reclaiming on behalf of a cgroup, skip
		 * counting on behalf of references from different
		 * cgroups
		 */
		if (memcg && !mm_match_cgroup(vma->vm_mm, memcg))
			continue;
		referenced += page_referenced_one(page, vma, address,
						  &mapcount, vm_flags);
		if (!mapcount)
			break;
	}

	page_unlock_anon_vma(anon_vma);
	return referenced;
}

/**
 * page_referenced_file - referenced check for object-based rmap
 * @page: the page we're checking references on.
 * @memcg: target memory control group
 * @vm_flags: collect encountered vma->vm_flags who actually referenced the page
 *
 * For an object-based mapped page, find all the places it is mapped and
 * check/clear the referenced flag.  This is done by following the page->mapping
 * pointer, then walking the chain of vmas it holds.  It returns the number
 * of references it found.
 *
 * This function is only called from page_referenced for object-based pages.
 */
static int page_referenced_file(struct page *page,
				struct mem_cgroup *memcg,
				unsigned long *vm_flags)
{
	unsigned int mapcount;
	struct address_space *mapping = page->mapping;
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	struct vm_area_struct *vma;
	struct prio_tree_iter iter;
	int referenced = 0;

	/*
	 * The caller's checks on page->mapping and !PageAnon have made
	 * sure that this is a file page: the check for page->mapping
	 * excludes the case just before it gets set on an anon page.
	 */
	BUG_ON(PageAnon(page));

	/*
	 * The page lock not only makes sure that page->mapping cannot
	 * suddenly be NULLified by truncation, it makes sure that the
	 * structure at mapping cannot be freed and reused yet,
	 * so we can safely take mapping->i_mmap_mutex.
	 */
	BUG_ON(!PageLocked(page));

	mutex_lock(&mapping->i_mmap_mutex);

	/*
	 * i_mmap_mutex does not stabilize mapcount at all, but mapcount
	 * is more likely to be accurate if we note it after spinning.
	 */
	mapcount = page_mapcount(page);

	vma_prio_tree_foreach(vma, &iter, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		/*
		 * If we are reclaiming on behalf of a cgroup, skip
		 * counting on behalf of references from different
		 * cgroups
		 */
		if (memcg && !mm_match_cgroup(vma->vm_mm, memcg))
			continue;
		referenced += page_referenced_one(page, vma, address,
						  &mapcount, vm_flags);
		if (!mapcount)
			break;
	}

	mutex_unlock(&mapping->i_mmap_mutex);
	return referenced;
}

/**
 * page_referenced - test if the page was referenced
 * @page: the page to test
 * @is_locked: caller holds lock on the page
 * @memcg: target memory cgroup
 * @vm_flags: collect encountered vma->vm_flags who actually referenced the page
 *
 * Quick test_and_clear_referenced for all mappings to a page,
 * returns the number of ptes which referenced the page.
 */
/** 20140111
 * 추후 분석하기로 함 ???
 * page의 referenced된 수를 누적시켜 리턴.
 **/
int page_referenced(struct page *page,
		    int is_locked,
		    struct mem_cgroup *memcg,
		    unsigned long *vm_flags)
{
	int referenced = 0;
	int we_locked = 0;

	*vm_flags = 0;
	/** 20140524
	 * page가 page table에 mapping되어 있고, rmapping을 가지고 있다면
	 **/
	if (page_mapped(page) && page_rmapping(page)) {
		/** 20140524
		 * lock이 결리지 않고, page anon이 아니라면
		 * page의 lock을 시도하고, lock을 걸지 못했다면 referenced를 증가시키고 빠져나간다.
		 **/
		if (!is_locked && (!PageAnon(page) || PageKsm(page))) {
			we_locked = trylock_page(page);
			if (!we_locked) {
				referenced++;
				goto out;
			}
		}
		if (unlikely(PageKsm(page)))
			referenced += page_referenced_ksm(page, memcg,
								vm_flags);
		else if (PageAnon(page))
			referenced += page_referenced_anon(page, memcg,
								vm_flags);
		else if (page->mapping)
			referenced += page_referenced_file(page, memcg,
								vm_flags);
		if (we_locked)
			unlock_page(page);

		if (page_test_and_clear_young(page_to_pfn(page)))
			referenced++;
	}
out:
	return referenced;
}

static int page_mkclean_one(struct page *page, struct vm_area_struct *vma,
			    unsigned long address)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	spinlock_t *ptl;
	int ret = 0;

	pte = page_check_address(page, mm, address, &ptl, 1);
	if (!pte)
		goto out;

	if (pte_dirty(*pte) || pte_write(*pte)) {
		pte_t entry;

		flush_cache_page(vma, address, pte_pfn(*pte));
		entry = ptep_clear_flush_notify(vma, address, pte);
		entry = pte_wrprotect(entry);
		entry = pte_mkclean(entry);
		set_pte_at(mm, address, pte, entry);
		ret = 1;
	}

	pte_unmap_unlock(pte, ptl);
out:
	return ret;
}

static int page_mkclean_file(struct address_space *mapping, struct page *page)
{
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	struct vm_area_struct *vma;
	struct prio_tree_iter iter;
	int ret = 0;

	BUG_ON(PageAnon(page));

	mutex_lock(&mapping->i_mmap_mutex);
	vma_prio_tree_foreach(vma, &iter, &mapping->i_mmap, pgoff, pgoff) {
		if (vma->vm_flags & VM_SHARED) {
			unsigned long address = vma_address(page, vma);
			if (address == -EFAULT)
				continue;
			ret += page_mkclean_one(page, vma, address);
		}
	}
	mutex_unlock(&mapping->i_mmap_mutex);
	return ret;
}

int page_mkclean(struct page *page)
{
	int ret = 0;

	BUG_ON(!PageLocked(page));

	if (page_mapped(page)) {
		struct address_space *mapping = page_mapping(page);
		if (mapping) {
			ret = page_mkclean_file(mapping, page);
			if (page_test_and_clear_dirty(page_to_pfn(page), 1))
				ret = 1;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(page_mkclean);

/**
 * page_move_anon_rmap - move a page to our anon_vma
 * @page:	the page to move to our anon_vma
 * @vma:	the vma the page belongs to
 * @address:	the user virtual address mapped
 *
 * When a page belongs exclusively to one process after a COW event,
 * that page can be moved into the anon_vma that belongs to just that
 * process, so the rmap code will not search the parent or sibling
 * processes.
 */
void page_move_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address)
{
	struct anon_vma *anon_vma = vma->anon_vma;

	VM_BUG_ON(!PageLocked(page));
	VM_BUG_ON(!anon_vma);
	VM_BUG_ON(page->index != linear_page_index(vma, address));

	anon_vma = (void *) anon_vma + PAGE_MAPPING_ANON;
	page->mapping = (struct address_space *) anon_vma;
}

/**
 * __page_set_anon_rmap - set up new anonymous rmap
 * @page:	Page to add to rmap	
 * @vma:	VM area to add page to.
 * @address:	User virtual address of the mapping	
 * @exclusive:	the page is exclusively owned by the current process
 */
static void __page_set_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address, int exclusive)
{
	struct anon_vma *anon_vma = vma->anon_vma;

	BUG_ON(!anon_vma);

	if (PageAnon(page))
		return;

	/*
	 * If the page isn't exclusively mapped into this vma,
	 * we must use the _oldest_ possible anon_vma for the
	 * page mapping!
	 */
	if (!exclusive)
		anon_vma = anon_vma->root;

	anon_vma = (void *) anon_vma + PAGE_MAPPING_ANON;
	page->mapping = (struct address_space *) anon_vma;
	page->index = linear_page_index(vma, address);
}

/**
 * __page_check_anon_rmap - sanity check anonymous rmap addition
 * @page:	the page to add the mapping to
 * @vma:	the vm area in which the mapping is added
 * @address:	the user virtual address mapped
 */
static void __page_check_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address)
{
#ifdef CONFIG_DEBUG_VM
	/*
	 * The page's anon-rmap details (mapping and index) are guaranteed to
	 * be set up correctly at this point.
	 *
	 * We have exclusion against page_add_anon_rmap because the caller
	 * always holds the page locked, except if called from page_dup_rmap,
	 * in which case the page is already known to be setup.
	 *
	 * We have exclusion against page_add_new_anon_rmap because those pages
	 * are initially only visible via the pagetables, and the pte is locked
	 * over the call to page_add_new_anon_rmap.
	 */
	BUG_ON(page_anon_vma(page)->root != vma->anon_vma->root);
	BUG_ON(page->index != linear_page_index(vma, address));
#endif
}

/**
 * page_add_anon_rmap - add pte mapping to an anonymous page
 * @page:	the page to add the mapping to
 * @vma:	the vm area in which the mapping is added
 * @address:	the user virtual address mapped
 *
 * The caller needs to hold the pte lock, and the page must be locked in
 * the anon_vma case: to serialize mapping,index checking after setting,
 * and to ensure that PageAnon is not being upgraded racily to PageKsm
 * (but PageKsm is never downgraded to PageAnon).
 */
void page_add_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address)
{
	do_page_add_anon_rmap(page, vma, address, 0);
}

/*
 * Special version of the above for do_swap_page, which often runs
 * into pages that are exclusively owned by the current process.
 * Everybody else should continue to use page_add_anon_rmap above.
 */
void do_page_add_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address, int exclusive)
{
	int first = atomic_inc_and_test(&page->_mapcount);
	if (first) {
		if (!PageTransHuge(page))
			__inc_zone_page_state(page, NR_ANON_PAGES);
		else
			__inc_zone_page_state(page,
					      NR_ANON_TRANSPARENT_HUGEPAGES);
	}
	if (unlikely(PageKsm(page)))
		return;

	VM_BUG_ON(!PageLocked(page));
	/* address might be in next vma when migration races vma_adjust */
	if (first)
		__page_set_anon_rmap(page, vma, address, exclusive);
	else
		__page_check_anon_rmap(page, vma, address);
}

/**
 * page_add_new_anon_rmap - add pte mapping to a new anonymous page
 * @page:	the page to add the mapping to
 * @vma:	the vm area in which the mapping is added
 * @address:	the user virtual address mapped
 *
 * Same as page_add_anon_rmap but must only be called on *new* pages.
 * This means the inc-and-test can be bypassed.
 * Page does not have to be locked.
 */
void page_add_new_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address)
{
	VM_BUG_ON(address < vma->vm_start || address >= vma->vm_end);
	SetPageSwapBacked(page);
	atomic_set(&page->_mapcount, 0); /* increment count (starts at -1) */
	if (!PageTransHuge(page))
		__inc_zone_page_state(page, NR_ANON_PAGES);
	else
		__inc_zone_page_state(page, NR_ANON_TRANSPARENT_HUGEPAGES);
	__page_set_anon_rmap(page, vma, address, 1);
	if (page_evictable(page, vma))
		lru_cache_add_lru(page, LRU_ACTIVE_ANON);
	else
		add_page_to_unevictable_list(page);
}

/**
 * page_add_file_rmap - add pte mapping to a file page
 * @page: the page to add the mapping to
 *
 * The caller needs to hold the pte lock.
 */
void page_add_file_rmap(struct page *page)
{
	bool locked;
	unsigned long flags;

	mem_cgroup_begin_update_page_stat(page, &locked, &flags);
	if (atomic_inc_and_test(&page->_mapcount)) {
		__inc_zone_page_state(page, NR_FILE_MAPPED);
		mem_cgroup_inc_page_stat(page, MEMCG_NR_FILE_MAPPED);
	}
	mem_cgroup_end_update_page_stat(page, &locked, &flags);
}

/**
 * page_remove_rmap - take down pte mapping from a page
 * @page: page to remove mapping from
 *
 * The caller needs to hold the pte lock.
 */
/** 20140531
 * page의 _mapcount을 하나 감소시켜
 * -1이 되었다면 zone stat에 반영한다.
 **/
void page_remove_rmap(struct page *page)
{
	bool anon = PageAnon(page);
	bool locked;
	unsigned long flags;

	/*
	 * The anon case has no mem_cgroup page_stat to update; but may
	 * uncharge_page() below, where the lock ordering can deadlock if
	 * we hold the lock against page_stat move: so avoid it on anon.
	 */
	if (!anon)
		/** 20140531
		 * MEM_CG를 설정하지 않아 NULL 함수
		 **/
		mem_cgroup_begin_update_page_stat(page, &locked, &flags);

	/* page still mapped by someone else? */
	/** 20140531
	 * _mapcount를 하나 감소시키고, 그 결과 0보다 작지 않다면
	 * 다른 곳에서 mapping 되어 있으므로 out.
	 **/
	if (!atomic_add_negative(-1, &page->_mapcount))
		goto out;

	/*
	 * Now that the last pte has gone, s390 must transfer dirty
	 * flag from storage key to struct page.  We can usually skip
	 * this if the page is anon, so about to be freed; but perhaps
	 * not if it's in swapcache - there might be another pte slot
	 * containing the swap entry, but page not yet written to swap.
	 */
	/** 20140531
	 * page_test_and_clear_dirty가 항상 0을 리턴.
	 **/
	if ((!anon || PageSwapCache(page)) &&
	    page_test_and_clear_dirty(page_to_pfn(page), 1))
		set_page_dirty(page);
	/*
	 * Hugepages are not counted in NR_ANON_PAGES nor NR_FILE_MAPPED
	 * and not charged by memcg for now.
	 */
	/** 20140531
	 * page가 huge page인 경우 out.
	 **/
	if (unlikely(PageHuge(page)))
		goto out;
	/** 20140531
	 * page가 hugepage가 아니고 anon인 경우
	 **/
	if (anon) {
		mem_cgroup_uncharge_page(page);
		/** 20140531
		 * transparent huge가 아닐 경우 NR_ANON_PAGES 갱신.
		 * transparent huge일 경우 NR_ANON_TRANSPARENT_HUGEPAGES 갱신.
		 **/
		if (!PageTransHuge(page))
			__dec_zone_page_state(page, NR_ANON_PAGES);
		else
			__dec_zone_page_state(page,
					      NR_ANON_TRANSPARENT_HUGEPAGES);
	} else {
		/** 20140531
		 * page가 hugepage가 아니고 file인 경우
		 * zone state의 NR_FILE_MAPPED를 감소
		 **/
		__dec_zone_page_state(page, NR_FILE_MAPPED);
		mem_cgroup_dec_page_stat(page, MEMCG_NR_FILE_MAPPED);
	}
	/*
	 * It would be tidy to reset the PageAnon mapping here,
	 * but that might overwrite a racing page_add_anon_rmap
	 * which increments mapcount after us but sets mapping
	 * before us: so leave the reset to free_hot_cold_page,
	 * and remember that it's only reliable while mapped.
	 * Leaving it set also helps swapoff to reinstate ptes
	 * faster for those pages still in swapcache.
	 */
out:
	if (!anon)
		mem_cgroup_end_update_page_stat(page, &locked, &flags);
}

/*
 * Subfunctions of try_to_unmap: try_to_unmap_one called
 * repeatedly from try_to_unmap_ksm, try_to_unmap_anon or try_to_unmap_file.
 */
/** 20140607
 * unmap 할 page table entry를 flush하고, dirty page로 세팅한다.
 * page anon일 경우 swap entry값을 pte entry에 넣어 unmap 한다.
 * rmap을 감소(_mapcount)시키고, page_cache_release(put_page)를 호출한다.
 *
 * put_page에서 _count를 감소시켜 0이 된 경우 page를 해제한다.
 **/
int try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
		     unsigned long address, enum ttu_flags flags)
{
	/** 20140531
	 * vma로부터 mm 정보를 가져온다.
	 **/
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;
	int ret = SWAP_AGAIN;

	/** 20140531
	 * 전달받은 page와 address의 정보로 찾은 page가 일치할 경우
	 * lock을 걸어 mm의 pte entry를 받아온다.
	 **/
	pte = page_check_address(page, mm, address, &ptl, 0);
	if (!pte)
		goto out;

	/*
	 * If the page is mlock()d, we cannot swap it out.
	 * If it's recently referenced (perhaps page_referenced
	 * skipped over this mm) then we should reactivate it.
	 */
	/** 20140531
	 * mlock을 무시하도로 지정되지 않은 경우
	 **/
	if (!(flags & TTU_IGNORE_MLOCK)) {
		/** 20140531
		 * vma에 LOCK이 걸려 있다면 unmap 하지 못한다.
		 **/
		if (vma->vm_flags & VM_LOCKED)
			goto out_mlock;

		/** 20140531
		 * TTU_MUNLOCK 속성이 요청된 경우 out_unmap 로 바로 이동한다.
		 **/
		if (TTU_ACTION(flags) == TTU_MUNLOCK)
			goto out_unmap;
	}
	/** 20140531
	 * TTU_IGNORE_ACCESS (YOUNG)요청이 없었다면
	 * pte 값에 YOUNG이 셋되어 있었다면(accessed) 클리어하고 tlb를 flush 한다.
	 * YOUNG이 설정되어 있었다면 SWAP_FAIL로 out_unmap으로 이동한다.
	 **/
	if (!(flags & TTU_IGNORE_ACCESS)) {
		if (ptep_clear_flush_young_notify(vma, address, pte)) {
			ret = SWAP_FAIL;
			goto out_unmap;
		}
  	}

	/* Nuke the page table entry. */
	/** 20140607
	 * page table에서 address에 해당하는 cache 영역을 flush한다.
	 **/
	flush_cache_page(vma, address, page_to_pfn(page));
	/** 20140531
	 * pte를 clear시킨다.
	 * 해당되는 tlb를 flush 하고, 이전 값을 가져온다.
	 **/
	pteval = ptep_clear_flush_notify(vma, address, pte);

	/* Move the dirty bit to the physical page now the pte is gone. */
	/** 20140531
	 * pte entry를 날렸으므로 pte 값이 dirty였다면,
	 * struct page에 dirty를 설정한다.
	 **/
	if (pte_dirty(pteval))
		set_page_dirty(page);

	/* Update high watermark before we lower rss */
	/** 20140531
	 * rss hiwatermark 갱신.
	 **/
	update_hiwater_rss(mm);

	/** 20140531
	 * CONFIG_MEMORY_FAILURE가 정의되지 않아 HWPoison은 항상 0을 리턴한다.
	 **/
	if (PageHWPoison(page) && !(flags & TTU_IGNORE_HWPOISON)) {
		if (PageAnon(page))
			dec_mm_counter(mm, MM_ANONPAGES);
		else
			dec_mm_counter(mm, MM_FILEPAGES);
		set_pte_at(mm, address, pte,
				swp_entry_to_pte(make_hwpoison_entry(page)));
	/** 20140531
	 * page가 anon인 경우 set_pte_at으로 swap entry를 넣어준다.
	 **/
	} else if (PageAnon(page)) {
		/** 20140531
		 * page의 private 값을 저장하는 entry를 선언.
		 **/
		swp_entry_t entry = { .val = page_private(page) };

		/** 20140531
		 * page가 swapcache인 경우 (add_to_swap된 경우)
		 **/
		if (PageSwapCache(page)) {
			/*
			 * Store the swap location in the pte.
			 * See handle_pte_fault() ...
			 */
			/** 20140531
			 * swap entry의 reference count를 하나 증가시킨다.
			 * 실패할 경우 다시 page table에 mapping 시키고, out_unmap으로 이동.
			 **/
			if (swap_duplicate(entry) < 0) {
				set_pte_at(mm, address, pte, pteval);
				ret = SWAP_FAIL;
				goto out_unmap;
			}
			/** 20140531
			 * 현재 mm의 mmlist가 비어있다면, init_mm의 mmlist에 추가한다.
			 **/
			if (list_empty(&mm->mmlist)) {
				spin_lock(&mmlist_lock);
				if (list_empty(&mm->mmlist))
					list_add(&mm->mmlist, &init_mm.mmlist);
				spin_unlock(&mmlist_lock);
			}
			/** 20140531
			 * mm의 anonpages를 감소시키고, swapents는 증가시킨다.
			 **/
			dec_mm_counter(mm, MM_ANONPAGES);
			inc_mm_counter(mm, MM_SWAPENTS);
		/** 20140531
		 * CONFIG_MIGRATION 정의되지 않음.
		 **/
		} else if (IS_ENABLED(CONFIG_MIGRATION)) {
			/*
			 * Store the pfn of the page in a special migration
			 * pte. do_swap_page() will wait until the migration
			 * pte is removed and then restart fault handling.
			 */
			BUG_ON(TTU_ACTION(flags) != TTU_MIGRATION);
			entry = make_migration_entry(page, pte_write(pteval));
		}
		/** 20140607
		 * pte 위치에 swap entry로 pte 값을 채운다.
		 * 즉, swap될 page인 경우 pte entry에는 swap entry(swap된 위치)가 들어간다.
		 **/
		set_pte_at(mm, address, pte, swp_entry_to_pte(entry));
		BUG_ON(pte_file(*pte));
	/** 20140531
	 * CONFIG_MIGRATION 정의되지 않음.
	 **/
	} else if (IS_ENABLED(CONFIG_MIGRATION) &&
		   (TTU_ACTION(flags) == TTU_MIGRATION)) {
		/* Establish migration entry for a file page */
		swp_entry_t entry;
		entry = make_migration_entry(page, pte_write(pteval));
		set_pte_at(mm, address, pte, swp_entry_to_pte(entry));
	} else
	/** 20140531
	 * 그 외에는 filepages에 해당한다.
	 * mm counter만 감소시킨다.
	 **/
		dec_mm_counter(mm, MM_FILEPAGES);

	/** 20140531
	 * rmap을 감소시킨다.
	 **/
	page_remove_rmap(page);
	/** 20140531
	 * page cache의 usage count를 하나 감소시킨다.
	 * usage count가 0이 된 경우 해제한다.
	 **/
	page_cache_release(page);

out_unmap:
	/** 20140531
	 * pte의 lock을 해제한다. (unmap에 해당하는 부분은 NULL)
	 **/
	pte_unmap_unlock(pte, ptl);
out:
	return ret;

out_mlock:
	/** 20140531
	 * mlock 되어 unmap하지 못하는 경우, lock을 해제한다.
	 **/
	pte_unmap_unlock(pte, ptl);


	/*
	 * We need mmap_sem locking, Otherwise VM_LOCKED check makes
	 * unstable result and race. Plus, We can't wait here because
	 * we now hold anon_vma->mutex or mapping->i_mmap_mutex.
	 * if trylock failed, the page remain in evictable lru and later
	 * vmscan could retry to move the page to unevictable lru if the
	 * page is actually mlocked.
	 */
	/** 20140531
	 * vma가 속한 mm_struct의 lock 획득을 시도한다.
	 * lock을 획득한다면 vm_flags의 LOCKED 상태를 보고
	 **/
	if (down_read_trylock(&vma->vm_mm->mmap_sem)) {
		if (vma->vm_flags & VM_LOCKED) {
			mlock_vma_page(page);
			ret = SWAP_MLOCK;
		}
		up_read(&vma->vm_mm->mmap_sem);
	}
	return ret;
}

/*
 * objrmap doesn't work for nonlinear VMAs because the assumption that
 * offset-into-file correlates with offset-into-virtual-addresses does not hold.
 * Consequently, given a particular page and its ->index, we cannot locate the
 * ptes which are mapping that page without an exhaustive linear search.
 *
 * So what this code does is a mini "virtual scan" of each nonlinear VMA which
 * maps the file to which the target page belongs.  The ->vm_private_data field
 * holds the current cursor into that scan.  Successive searches will circulate
 * around the vma's virtual address space.
 *
 * So as more replacement pressure is applied to the pages in a nonlinear VMA,
 * more scanning pressure is placed against them as well.   Eventually pages
 * will become fully unmapped and are eligible for eviction.
 *
 * For very sparsely populated VMAs this is a little inefficient - chances are
 * there there won't be many ptes located within the scan cluster.  In this case
 * maybe we could scan further - to the end of the pte page, perhaps.
 *
 * Mlocked pages:  check VM_LOCKED under mmap_sem held for read, if we can
 * acquire it without blocking.  If vma locked, mlock the pages in the cluster,
 * rather than unmapping them.  If we encounter the "check_page" that vmscan is
 * trying to unmap, return SWAP_MLOCK, else default SWAP_AGAIN.
 */
#define CLUSTER_SIZE	min(32*PAGE_SIZE, PMD_SIZE)
#define CLUSTER_MASK	(~(CLUSTER_SIZE - 1))

static int try_to_unmap_cluster(unsigned long cursor, unsigned int *mapcount,
		struct vm_area_struct *vma, struct page *check_page)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;
	struct page *page;
	unsigned long address;
	unsigned long end;
	int ret = SWAP_AGAIN;
	int locked_vma = 0;

	address = (vma->vm_start + cursor) & CLUSTER_MASK;
	end = address + CLUSTER_SIZE;
	if (address < vma->vm_start)
		address = vma->vm_start;
	if (end > vma->vm_end)
		end = vma->vm_end;

	pgd = pgd_offset(mm, address);
	if (!pgd_present(*pgd))
		return ret;

	pud = pud_offset(pgd, address);
	if (!pud_present(*pud))
		return ret;

	pmd = pmd_offset(pud, address);
	if (!pmd_present(*pmd))
		return ret;

	/*
	 * If we can acquire the mmap_sem for read, and vma is VM_LOCKED,
	 * keep the sem while scanning the cluster for mlocking pages.
	 */
	if (down_read_trylock(&vma->vm_mm->mmap_sem)) {
		locked_vma = (vma->vm_flags & VM_LOCKED);
		if (!locked_vma)
			up_read(&vma->vm_mm->mmap_sem); /* don't need it */
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);

	/* Update high watermark before we lower rss */
	update_hiwater_rss(mm);

	for (; address < end; pte++, address += PAGE_SIZE) {
		if (!pte_present(*pte))
			continue;
		page = vm_normal_page(vma, address, *pte);
		BUG_ON(!page || PageAnon(page));

		if (locked_vma) {
			mlock_vma_page(page);   /* no-op if already mlocked */
			if (page == check_page)
				ret = SWAP_MLOCK;
			continue;	/* don't unmap */
		}

		if (ptep_clear_flush_young_notify(vma, address, pte))
			continue;

		/* Nuke the page table entry. */
		flush_cache_page(vma, address, pte_pfn(*pte));
		pteval = ptep_clear_flush_notify(vma, address, pte);

		/* If nonlinear, store the file page offset in the pte. */
		if (page->index != linear_page_index(vma, address))
			set_pte_at(mm, address, pte, pgoff_to_pte(page->index));

		/* Move the dirty bit to the physical page now the pte is gone. */
		if (pte_dirty(pteval))
			set_page_dirty(page);

		page_remove_rmap(page);
		page_cache_release(page);
		dec_mm_counter(mm, MM_FILEPAGES);
		(*mapcount)--;
	}
	pte_unmap_unlock(pte - 1, ptl);
	if (locked_vma)
		up_read(&vma->vm_mm->mmap_sem);
	return ret;
}

bool is_vma_temporary_stack(struct vm_area_struct *vma)
{
	int maybe_stack = vma->vm_flags & (VM_GROWSDOWN | VM_GROWSUP);

	if (!maybe_stack)
		return false;

	if ((vma->vm_flags & VM_STACK_INCOMPLETE_SETUP) ==
						VM_STACK_INCOMPLETE_SETUP)
		return true;

	return false;
}

/**
 * try_to_unmap_anon - unmap or unlock anonymous page using the object-based
 * rmap method
 * @page: the page to unmap/unlock
 * @flags: action and flags
 *
 * Find all the mappings of a page using the mapping pointer and the vma chains
 * contained in the anon_vma struct it points to.
 *
 * This function is only called from try_to_unmap/try_to_munlock for
 * anonymous pages.
 * When called from try_to_munlock(), the mmap_sem of the mm containing the vma
 * where the page was found will be held for write.  So, we won't recheck
 * vm_flags for that VMA.  That should be OK, because that vma shouldn't be
 * 'LOCKED.
 */
/** 20140607
 * anon page에 대해 unmap을 위해 호출되는 함수.
 *
 * page의 mapping를 통해 anon_vma를 가져와 anon_vma_chain을 순회하며
 * unmap을 호출한다.
 **/
static int try_to_unmap_anon(struct page *page, enum ttu_flags flags)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	int ret = SWAP_AGAIN;

	/** 20140531
	 * page에 lock을 걸고, page->mapping에서 anon_vma 주소만 가져온다.
	 * anon_vma가 아닌 경우 리턴한다.
	 **/
	anon_vma = page_lock_anon_vma(page);
	if (!anon_vma)
		return ret;

	/** 20140531
	 * anon_vma의 head부터 same_anon_vma를 순회하며 page를 unmap 시킨다.
	 **/
	list_for_each_entry(avc, &anon_vma->head, same_anon_vma) {
		struct vm_area_struct *vma = avc->vma;
		unsigned long address;

		/*
		 * During exec, a temporary VMA is setup and later moved.
		 * The VMA is moved under the anon_vma lock but not the
		 * page tables leading to a race where migration cannot
		 * find the migration ptes. Rather than increasing the
		 * locking requirements of exec(), migration skips
		 * temporary VMAs until after exec() completes.
		 */
		/** 20140531
		 * migration이 활성화 되어 있고, migration mode로 unmap_anon이 호출된 경우
		 **/
		if (IS_ENABLED(CONFIG_MIGRATION) && (flags & TTU_MIGRATION) &&
				is_vma_temporary_stack(vma))
			continue;

		/** 20140531
		 * page가 mapping된 process 영역의 VA를 가져온다.
		 **/
		address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		/** 20140607
		 * file/anon 에서 호출할 수 있는 try_to_unmap_one로 unmap시킨다.
		 * usage count (_count)가 0이 되면 실제 page free 동작이 수행된다.
		 **/
		ret = try_to_unmap_one(page, vma, address, flags);
		/** 20140607
		 * SWAP_AGAIN이 아니거나(SWAP_FAIL) page가 모두 unmap 된 경우 break.
		 **/
		if (ret != SWAP_AGAIN || !page_mapped(page))
			break;
	}

	page_unlock_anon_vma(anon_vma);
	return ret;
}

/**
 * try_to_unmap_file - unmap/unlock file page using the object-based rmap method
 * @page: the page to unmap/unlock
 * @flags: action and flags
 *
 * Find all the mappings of a page using the mapping pointer and the vma chains
 * contained in the address_space struct it points to.
 *
 * This function is only called from try_to_unmap/try_to_munlock for
 * object-based pages.
 * When called from try_to_munlock(), the mmap_sem of the mm containing the vma
 * where the page was found will be held for write.  So, we won't recheck
 * vm_flags for that VMA.  That should be OK, because that vma shouldn't be
 * 'LOCKED.
 */
/** 20140607
 * 추후 분석 ???
 **/
static int try_to_unmap_file(struct page *page, enum ttu_flags flags)
{
	struct address_space *mapping = page->mapping;
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	struct vm_area_struct *vma;
	struct prio_tree_iter iter;
	int ret = SWAP_AGAIN;
	unsigned long cursor;
	unsigned long max_nl_cursor = 0;
	unsigned long max_nl_size = 0;
	unsigned int mapcount;

	mutex_lock(&mapping->i_mmap_mutex);
	vma_prio_tree_foreach(vma, &iter, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		ret = try_to_unmap_one(page, vma, address, flags);
		if (ret != SWAP_AGAIN || !page_mapped(page))
			goto out;
	}

	if (list_empty(&mapping->i_mmap_nonlinear))
		goto out;

	/*
	 * We don't bother to try to find the munlocked page in nonlinears.
	 * It's costly. Instead, later, page reclaim logic may call
	 * try_to_unmap(TTU_MUNLOCK) and recover PG_mlocked lazily.
	 */
	if (TTU_ACTION(flags) == TTU_MUNLOCK)
		goto out;

	list_for_each_entry(vma, &mapping->i_mmap_nonlinear,
						shared.vm_set.list) {
		cursor = (unsigned long) vma->vm_private_data;
		if (cursor > max_nl_cursor)
			max_nl_cursor = cursor;
		cursor = vma->vm_end - vma->vm_start;
		if (cursor > max_nl_size)
			max_nl_size = cursor;
	}

	if (max_nl_size == 0) {	/* all nonlinears locked or reserved ? */
		ret = SWAP_FAIL;
		goto out;
	}

	/*
	 * We don't try to search for this page in the nonlinear vmas,
	 * and page_referenced wouldn't have found it anyway.  Instead
	 * just walk the nonlinear vmas trying to age and unmap some.
	 * The mapcount of the page we came in with is irrelevant,
	 * but even so use it as a guide to how hard we should try?
	 */
	mapcount = page_mapcount(page);
	if (!mapcount)
		goto out;
	cond_resched();

	max_nl_size = (max_nl_size + CLUSTER_SIZE - 1) & CLUSTER_MASK;
	if (max_nl_cursor == 0)
		max_nl_cursor = CLUSTER_SIZE;

	do {
		list_for_each_entry(vma, &mapping->i_mmap_nonlinear,
						shared.vm_set.list) {
			cursor = (unsigned long) vma->vm_private_data;
			while ( cursor < max_nl_cursor &&
				cursor < vma->vm_end - vma->vm_start) {
				if (try_to_unmap_cluster(cursor, &mapcount,
						vma, page) == SWAP_MLOCK)
					ret = SWAP_MLOCK;
				cursor += CLUSTER_SIZE;
				vma->vm_private_data = (void *) cursor;
				if ((int)mapcount <= 0)
					goto out;
			}
			vma->vm_private_data = (void *) max_nl_cursor;
		}
		cond_resched();
		max_nl_cursor += CLUSTER_SIZE;
	} while (max_nl_cursor <= max_nl_size);

	/*
	 * Don't loop forever (perhaps all the remaining pages are
	 * in locked vmas).  Reset cursor on all unreserved nonlinear
	 * vmas, now forgetting on which ones it had fallen behind.
	 */
	list_for_each_entry(vma, &mapping->i_mmap_nonlinear, shared.vm_set.list)
		vma->vm_private_data = NULL;
out:
	mutex_unlock(&mapping->i_mmap_mutex);
	return ret;
}

/**
 * try_to_unmap - try to remove all page table mappings to a page
 * @page: the page to get unmapped
 * @flags: action and flags
 *
 * Tries to remove all the page table entries which are mapping this
 * page, used in the pageout path.  Caller must hold the page lock.
 * Return values are:
 *
 * SWAP_SUCCESS	- we succeeded in removing all mappings
 * SWAP_AGAIN	- we missed a mapping, try again later
 * SWAP_FAIL	- the page is unswappable
 * SWAP_MLOCK	- page is mlocked.
 */
/** 20140607
 * page가 mapping된 모든 page table mapping 정보를 제거한다.
 * page의 속성에 따라 ksm, anon, file용 unmap 함수를 호출한다.
 *
 * 리턴값은 다음과 같다.
 * SWAP_SUCCESS	- page에 대한 모든 매핑 정보를 제거한 경우
 * SWAP_AGAIN	- 일부 mapping 정보가 남은 경우, 나중에 다시 시도
 * SWAP_FAIL	- page가 unswappable한 경우
 * SWAP_MLOCK	- mlocked로 lock이 걸린 경우
 **/
int try_to_unmap(struct page *page, enum ttu_flags flags)
{
	int ret;

	/** 20140531
	 * 페이지는 잠금 상태여야 한다.
	 **/
	BUG_ON(!PageLocked(page));
	VM_BUG_ON(!PageHuge(page) && PageTransHuge(page));

	/** 20140524
	 * ksm인 경우 try_to_unmap_ksm,
	 * anon인 경우 try_to_unmap_anon,
	 * 그 외 try_to_unmap_file로 unmap.
	 **/
	if (unlikely(PageKsm(page)))
		ret = try_to_unmap_ksm(page, flags);
	else if (PageAnon(page))
		ret = try_to_unmap_anon(page, flags);
	else
		ret = try_to_unmap_file(page, flags);
	/** 20140607
	 * SWAP_MLOCK되지 않고 page가 unmap된 상태일 경우 성공적으로 SWAP되었다.
	 **/
	if (ret != SWAP_MLOCK && !page_mapped(page))
		ret = SWAP_SUCCESS;
	return ret;
}

/**
 * try_to_munlock - try to munlock a page
 * @page: the page to be munlocked
 *
 * Called from munlock code.  Checks all of the VMAs mapping the page
 * to make sure nobody else has this page mlocked. The page will be
 * returned with PG_mlocked cleared if no other vmas have it mlocked.
 *
 * Return values are:
 *
 * SWAP_AGAIN	- no vma is holding page mlocked, or,
 * SWAP_AGAIN	- page mapped in mlocked vma -- couldn't acquire mmap sem
 * SWAP_FAIL	- page cannot be located at present
 * SWAP_MLOCK	- page is now mlocked.
 */
int try_to_munlock(struct page *page)
{
	VM_BUG_ON(!PageLocked(page) || PageLRU(page));

	if (unlikely(PageKsm(page)))
		return try_to_unmap_ksm(page, TTU_MUNLOCK);
	else if (PageAnon(page))
		return try_to_unmap_anon(page, TTU_MUNLOCK);
	else
		return try_to_unmap_file(page, TTU_MUNLOCK);
}

/** 20140524
 * anon_vma 메모리를 해제한다.
 **/
void __put_anon_vma(struct anon_vma *anon_vma)
{
	/** 20140524
	 * root anon_vma를 가져온다.
	 **/
	struct anon_vma *root = anon_vma->root;

	/** 20140524
	 * anon_vma가 root가 아니고, root의 refcount를 감소시켜 0이라면
	 * root를 해제한다.
	 **/
	if (root != anon_vma && atomic_dec_and_test(&root->refcount))
		anon_vma_free(root);

	/** 20140524
	 * anon_vma를 해제한다.
	 **/
	anon_vma_free(anon_vma);
}

#ifdef CONFIG_MIGRATION
/*
 * rmap_walk() and its helpers rmap_walk_anon() and rmap_walk_file():
 * Called by migrate.c to remove migration ptes, but might be used more later.
 */
static int rmap_walk_anon(struct page *page, int (*rmap_one)(struct page *,
		struct vm_area_struct *, unsigned long, void *), void *arg)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	int ret = SWAP_AGAIN;

	/*
	 * Note: remove_migration_ptes() cannot use page_lock_anon_vma()
	 * because that depends on page_mapped(); but not all its usages
	 * are holding mmap_sem. Users without mmap_sem are required to
	 * take a reference count to prevent the anon_vma disappearing
	 */
	anon_vma = page_anon_vma(page);
	if (!anon_vma)
		return ret;
	anon_vma_lock(anon_vma);
	list_for_each_entry(avc, &anon_vma->head, same_anon_vma) {
		struct vm_area_struct *vma = avc->vma;
		unsigned long address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		ret = rmap_one(page, vma, address, arg);
		if (ret != SWAP_AGAIN)
			break;
	}
	anon_vma_unlock(anon_vma);
	return ret;
}

static int rmap_walk_file(struct page *page, int (*rmap_one)(struct page *,
		struct vm_area_struct *, unsigned long, void *), void *arg)
{
	struct address_space *mapping = page->mapping;
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	struct vm_area_struct *vma;
	struct prio_tree_iter iter;
	int ret = SWAP_AGAIN;

	if (!mapping)
		return ret;
	mutex_lock(&mapping->i_mmap_mutex);
	vma_prio_tree_foreach(vma, &iter, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long address = vma_address(page, vma);
		if (address == -EFAULT)
			continue;
		ret = rmap_one(page, vma, address, arg);
		if (ret != SWAP_AGAIN)
			break;
	}
	/*
	 * No nonlinear handling: being always shared, nonlinear vmas
	 * never contain migration ptes.  Decide what to do about this
	 * limitation to linear when we need rmap_walk() on nonlinear.
	 */
	mutex_unlock(&mapping->i_mmap_mutex);
	return ret;
}

int rmap_walk(struct page *page, int (*rmap_one)(struct page *,
		struct vm_area_struct *, unsigned long, void *), void *arg)
{
	VM_BUG_ON(!PageLocked(page));

	if (unlikely(PageKsm(page)))
		return rmap_walk_ksm(page, rmap_one, arg);
	else if (PageAnon(page))
		return rmap_walk_anon(page, rmap_one, arg);
	else
		return rmap_walk_file(page, rmap_one, arg);
}
#endif /* CONFIG_MIGRATION */

#ifdef CONFIG_HUGETLB_PAGE
/*
 * The following three functions are for anonymous (private mapped) hugepages.
 * Unlike common anonymous pages, anonymous hugepages have no accounting code
 * and no lru code, because we handle hugepages differently from common pages.
 */
static void __hugepage_set_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address, int exclusive)
{
	struct anon_vma *anon_vma = vma->anon_vma;

	BUG_ON(!anon_vma);

	if (PageAnon(page))
		return;
	if (!exclusive)
		anon_vma = anon_vma->root;

	anon_vma = (void *) anon_vma + PAGE_MAPPING_ANON;
	page->mapping = (struct address_space *) anon_vma;
	page->index = linear_page_index(vma, address);
}

void hugepage_add_anon_rmap(struct page *page,
			    struct vm_area_struct *vma, unsigned long address)
{
	struct anon_vma *anon_vma = vma->anon_vma;
	int first;

	BUG_ON(!PageLocked(page));
	BUG_ON(!anon_vma);
	/* address might be in next vma when migration races vma_adjust */
	first = atomic_inc_and_test(&page->_mapcount);
	if (first)
		__hugepage_set_anon_rmap(page, vma, address, 0);
}

void hugepage_add_new_anon_rmap(struct page *page,
			struct vm_area_struct *vma, unsigned long address)
{
	BUG_ON(address < vma->vm_start || address >= vma->vm_end);
	atomic_set(&page->_mapcount, 0);
	__hugepage_set_anon_rmap(page, vma, address, 1);
}
#endif /* CONFIG_HUGETLB_PAGE */
