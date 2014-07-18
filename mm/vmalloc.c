/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 *  Numa awareness, Christoph Lameter, SGI, June 2005
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/pfn.h>
#include <linux/kmemleak.h>
#include <linux/atomic.h>
#include <asm/uaccess.h>
#include <asm/tlbflush.h>
#include <asm/shmparam.h>

/*** Page table manipulation functions ***/

static void vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end)
{
	pte_t *pte;

	pte = pte_offset_kernel(pmd, addr);
	do {
		pte_t ptent = ptep_get_and_clear(&init_mm, addr, pte);
		WARN_ON(!pte_none(ptent) && !pte_present(ptent));
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none_or_clear_bad(pmd))
			continue;
		vunmap_pte_range(pmd, addr, next);
	} while (pmd++, addr = next, addr != end);
}

static void vunmap_pud_range(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		vunmap_pmd_range(pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

/** 20140405    
 * addr ~ end 사이의 영역을 vunmap 시킨다(page table을 clear 한다)
 * 세부적인 분석은 하지 않았음 ???
 **/
static void vunmap_page_range(unsigned long addr, unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		vunmap_pud_range(pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

/** 20140329    
 * addr ~ end까지 pmd에 해당하는 pte table의 정보를 구성한다.
 **/
static int vmap_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pte_t *pte;

	/*
	 * nr is a running index into the array which helps higher level
	 * callers keep track of where we're up to.
	 */

	/** 20140329    
	 * addr에 해당하는 pte의 위치를 받아온다.
	 **/
	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;
	/** 20140329    
	 * addr ~ end까지 page 단위로 pte entry를 기록한다.
	 **/
	do {
		/** 20140329    
		 * pages 중 *nr번째 포인터 하나를 가져온다.
		 **/
		struct page *page = pages[*nr];

		if (WARN_ON(!pte_none(*pte)))
			return -EBUSY;
		if (WARN_ON(!page))
			return -ENOMEM;
		/** 20140329    
		 * pte에 pte entry 정보(미리 할당받아온 page frame 주소)를 구성해 기록한다.
		 **/
		set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));
		(*nr)++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

/** 20140329    
 * addr ~ end 사이의 pmd entry를 구성한다.
 **/
static int vmap_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pmd_t *pmd;
	unsigned long next;

	/** 20140329    
	 * pud 내에서 addr에 해당하는 entry를 리턴한다.
	 * 2단계 페이징에서는 pgd = pud = pmd
	 **/
	pmd = pmd_alloc(&init_mm, pud, addr);
	if (!pmd)
		return -ENOMEM;
	/** 20140329    
	 * pmd(2MB) 단위로 addr ~ end사이의 주소에 대해 pte table을 구성한다.
	 **/
	do {
		next = pmd_addr_end(addr, end);
		if (vmap_pte_range(pmd, addr, next, prot, pages, nr))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

/** 20140329    
 * pud = pmd 이므로 vmap_pmd_range 단위로 수행
 *   (pages에는 미리 할당받은 page frame 정보가 들어 있다.
 *    VA -> PA address를 mapping 하는 과정)
 **/
static int vmap_pud_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pud_t *pud;
	unsigned long next;

	/** 20140329    
	 * addr에 해당하는 pud entry 위치를 받아온다.
	 **/
	pud = pud_alloc(&init_mm, pgd, addr);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);
		/** 20140301    
		 *
		 **/
		if (vmap_pmd_range(pud, addr, next, prot, pages, nr))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

/*
 * Set up page tables in kva (addr, end). The ptes shall have prot "prot", and
 * will have pfns corresponding to the "pages" array.
 *
 * Ie. pte at addr+N*PAGE_SIZE shall point to pfn corresponding to pages[N]
 */
/** 20140329    
 * start ~ end까지 pgd = pud = pmd, pte 단위로 page table entry를 기록한다.
 * start 와 end 는 상위에서 할당 받은 address space.
 * pages는 이미 할당 받은 물리 메모리.
 * 이 두 정보를 mapping 한다.
 **/
static int vmap_page_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages)
{
	pgd_t *pgd;
	unsigned long next;
	unsigned long addr = start;
	int err = 0;
	int nr = 0;

	BUG_ON(addr >= end);
	/** 20140301    
	 * addr (VA)에 해당하는 pgd entry의 위치를 가져온다.
	 **/
	pgd = pgd_offset_k(addr);
	do {
		/** 20140301    
		 * addr와 end 중 작은 값을 취해 end를 넘지 않도록 한다.
		 **/
		next = pgd_addr_end(addr, end);
		err = vmap_pud_range(pgd, addr, next, prot, pages, &nr);
		if (err)
			return err;
	} while (pgd++, addr = next, addr != end);

	return nr;
}

/** 20140329    
 * start ~ end 사이의 영역을 vmap 시킨다(page table에 mapping 시킨다)
 **/
static int vmap_page_range(unsigned long start, unsigned long end,
			   pgprot_t prot, struct page **pages)
{
	int ret;

	/** 20140329    
	 * start ~ end까지 pages로 할당받은 memory와 mapping 한다.
	 **/
	ret = vmap_page_range_noflush(start, end, prot, pages);
	/** 20140329    
	 * start ~ end까지 cache를 flush 해 memory에 반영시킨다.
	 **/
	flush_cache_vmap(start, end);
	return ret;
}

int is_vmalloc_or_module_addr(const void *x)
{
	/*
	 * ARM, x86-64 and sparc64 put modules in a special place,
	 * and fall back on vmalloc() if that fails. Others
	 * just put it in the vmalloc space.
	 */
#if defined(CONFIG_MODULES) && defined(MODULES_VADDR)
	unsigned long addr = (unsigned long)x;
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
#endif
	return is_vmalloc_addr(x);
}

/*
 * Walk a vmap address to the struct page it maps.
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	unsigned long addr = (unsigned long) vmalloc_addr;
	struct page *page = NULL;
	pgd_t *pgd = pgd_offset_k(addr);

	/*
	 * XXX we might need to change this if we add VIRTUAL_BUG_ON for
	 * architectures that do not vmalloc module space
	 */
	VIRTUAL_BUG_ON(!is_vmalloc_or_module_addr(vmalloc_addr));

	if (!pgd_none(*pgd)) {
		pud_t *pud = pud_offset(pgd, addr);
		if (!pud_none(*pud)) {
			pmd_t *pmd = pmd_offset(pud, addr);
			if (!pmd_none(*pmd)) {
				pte_t *ptep, pte;

				ptep = pte_offset_map(pmd, addr);
				pte = *ptep;
				if (pte_present(pte))
					page = pte_page(pte);
				pte_unmap(ptep);
			}
		}
	}
	return page;
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * Map a vmalloc()-space virtual address to the physical page frame number.
 */
unsigned long vmalloc_to_pfn(const void *vmalloc_addr)
{
	return page_to_pfn(vmalloc_to_page(vmalloc_addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);


/*** Global kva allocator ***/

/** 20140419    
 * vmap_area에 대해 lazy free를 구분하는 매크로.
 * VM_LAZY_FREE    : vmap_area가 lazy free 대상임을 의미
 * VM_LAZY_FREEING : vmap_area가 lazy free 중임을 의미
 **/
#define VM_LAZY_FREE	0x01
#define VM_LAZY_FREEING	0x02
#define VM_VM_AREA	0x04

/** 20140329    
 * alloc_vmap_area
 **/
struct vmap_area {
	unsigned long va_start;
	unsigned long va_end;
	unsigned long flags;
	/** 20140329    
	 * address를 기준으로 정렬되어 rbtree로 구성될 때 사용
	 **/
	struct rb_node rb_node;		/* address sorted rbtree */
	struct list_head list;		/* address sorted list */
	struct list_head purge_list;	/* "lazy purge" list */
	struct vm_struct *vm;
	struct rcu_head rcu_head;
};

/** 20140329    
 * vmap_area를 위한 spinlock 선언
 **/
static DEFINE_SPINLOCK(vmap_area_lock);
/** 20140329    
 * vmap_area를 list 자료구조로 묶기 위한 list head
 **/
static LIST_HEAD(vmap_area_list);
/** 20140322    
 * vmap_area에서 사용되는 rb tree
 **/
static struct rb_root vmap_area_root = RB_ROOT;

/* The vmap cache globals are protected by vmap_area_lock */
/** 20140419    
 * alloc_vmap_area 에서 새로 추가된 vmap_area를 free_vmap_cache로 지정.
 * 동일 함수에서 vmap_area를 찾을 때 이 cache를 이용할 수 있는지 먼저 검사한다.
 **/
static struct rb_node *free_vmap_cache;
static unsigned long cached_hole_size;
static unsigned long cached_vstart;
static unsigned long cached_align;

/** 20140322    
 * 초기값은 vmalloc_init에서 VMALLOC_END로 설정
 **/
static unsigned long vmap_area_pcpu_hole;

/** 20140405    
 * rb_tree에서 해당 addr로 시작하는 vmap_area를 찾아온다.
 **/
static struct vmap_area *__find_vmap_area(unsigned long addr)
{
	/** 20140405    
	 * vmap_area_root부터 rb_tree 순회
	 **/
	struct rb_node *n = vmap_area_root.rb_node;

	while (n) {
		struct vmap_area *va;

		/** 20140405    
		 * 현재 rb_node가 속한 vmap_area를 가져온다.
		 **/
		va = rb_entry(n, struct vmap_area, rb_node);
		/** 20140405    
		 * 찾을 addr가 현재 node의 va_start보다 작으면 왼쪽을 순회.
		 * 크면 오른쪽을 순회.
		 * 같다면 찾는 node.
		 **/
		if (addr < va->va_start)
			n = n->rb_left;
		else if (addr > va->va_start)
			n = n->rb_right;
		else
			return va;
	}

	return NULL;
}

/** 20140329    
 * rbtree, list에 새로운 vmap_area를 등록한다.
 **/
static void __insert_vmap_area(struct vmap_area *va)
{
	/** 20140322    
	 * rb tree에 대한 분석은 생략???
	 * vmap_area의 rb_node를 초기화 해 vmap_area_root에 등록한다.
	 **/
	struct rb_node **p = &vmap_area_root.rb_node;
	struct rb_node *parent = NULL;
	struct rb_node *tmp;

	/** 20140329    
	 * rbtree를 rbroot에서부터 순회하며 추가할 위치를 찾는다.
	 **/
	while (*p) {
		struct vmap_area *tmp_va;

		parent = *p;
		/** 20140329    
		 * parent의 주소와 비교해
		 *	parent의 va_end보다 작으면 rb_left
		 *	parent의 va_start보다 크면 rb_right로 이동
		 *
		 * NULL에 도달하면 추가할 자리를 찾은 것이다.
		 **/
		tmp_va = rb_entry(parent, struct vmap_area, rb_node);
		if (va->va_start < tmp_va->va_end)
			p = &(*p)->rb_left;
		else if (va->va_end > tmp_va->va_start)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	/** 20140329    
	 * rbtree에 노드를 추가한다.
	 **/
	rb_link_node(&va->rb_node, parent, p);
	rb_insert_color(&va->rb_node, &vmap_area_root);

	/* address-sort this list so it is usable like the vmlist */
	/** 20140322    
	 * rb_prev가 없는 경우 vmap_area_list로 등록
	 * 존재하는 경우 rb_entry를 rb_node 내에서 등록
	 *
	 * 전체적인 구조 파악 필요???
	 **/
	/** 20140329    
	 * 새로 추가한 vmap_area에 대해
	 * rbtree에서의 이전 rb_node를 구해와 tmp에 저장
	 **/
	tmp = rb_prev(&va->rb_node);
	/** 20140329    
	 * 이전 노드가 존재하는 경우, 그에 해당하는 struct vmap_area를 가져온다.
	 **/
	if (tmp) {
		struct vmap_area *prev;
		prev = rb_entry(tmp, struct vmap_area, rb_node);
		/** 20140329    
		 * rcu 매커니즘을 이용해 prev의 다음으로 va를 리스트에 추가한다.
		 * 즉 rbtree와 list 자료구조의 순서를 맞춰주기 위한 동작이다.
		 **/
		list_add_rcu(&va->list, &prev->list);
	} else
	/** 20140329    
	 * 이전 노드가 존재하지 않는 경우 vmap_area_list의 다음에 추가한다.
	 **/
		list_add_rcu(&va->list, &vmap_area_list);
}

static void purge_vmap_area_lazy(void);

/*
 * Allocate a region of KVA of the specified size and alignment, within the
 * vstart and vend.
 */
/** 20140329    
 * vmap_area를 할당받고,
 * 자료구조(rbtree)를 조회해 적합한 addr를 가져온뒤,
 * 할당받은 vmap_area에 채우고,
 * 자료구조에 등록시킨다.
 **/
static struct vmap_area *alloc_vmap_area(unsigned long size,
				unsigned long align,
				unsigned long vstart, unsigned long vend,
				int node, gfp_t gfp_mask)
{
	struct vmap_area *va;
	struct rb_node *n;
	unsigned long addr;
	int purged = 0;
	struct vmap_area *first;

	BUG_ON(!size);
	BUG_ON(size & ~PAGE_MASK);
	BUG_ON(!is_power_of_2(align));

	/** 20140329    
	 * kmalloc으로 vmap_area를 위한 메모리를 할당
	 **/
	va = kmalloc_node(sizeof(struct vmap_area),
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!va))
		return ERR_PTR(-ENOMEM);

retry:
	/** 20140329    
	 * vmap cache 전역변수는 spinlock으로 보호된다.
	 **/
	spin_lock(&vmap_area_lock);
	/*
	 * Invalidate cache if we have more permissive parameters.
	 * cached_hole_size notes the largest hole noticed _below_
	 * the vmap_area cached in free_vmap_cache: if size fits
	 * into that hole, we want to scan from vstart to reuse
	 * the hole instead of allocating above free_vmap_cache.
	 * Note that __free_vmap_area may update free_vmap_cache
	 * without updating cached_hole_size or cached_align.
	 */
	if (!free_vmap_cache ||
			size < cached_hole_size ||
			vstart < cached_vstart ||
			align < cached_align) {
nocache:
		cached_hole_size = 0;
		free_vmap_cache = NULL;
	}
	/* record if we encounter less permissive parameters */
	/** 20140405    
	 * va를 받아오도록 요청된 범위와 align정보를
	 * cached_vstart, cached_align에 각각 기록
	 **/
	cached_vstart = vstart;
	cached_align = align;

	/* find starting point for our search */
	/** 20140405    
	 * free_vmap_cache가 존재한다면 먼저 cached에서 받아올 수 있는지 검사한다.
	 **/
	if (free_vmap_cache) {
		/** 20140329    
		 * free_vmap_cache가 존재하면 vmap_area 를 가져온다.
		 **/
		first = rb_entry(free_vmap_cache, struct vmap_area, rb_node);
		/** 20140329    
		 * first에서 va_end를 가져와 align 해서 addr에 저장.
		 **/
		addr = ALIGN(first->va_end, align);
		/** 20140405    
		 * vstart는 허용되는 range의 시작값 보다 작다면 해당 cache는 사용하지 못한다.
		 **/
		if (addr < vstart)
			goto nocache;
		/** 20140405    
		 * cached address 보다 요청한 size를 더한 값이 크다면 overflow
		 **/
		if (addr + size - 1 < addr)
			goto overflow;

	} else {
		/** 20140329    
		 * free_vmap_cache가 존재하지 않으면 
		 *  vstart를 정렬해 addr에 저장.
		 **/
		addr = ALIGN(vstart, align);
		/** 20140329    
		 * 끝주소가 addr보다 작다면 size가 커서 overflow 발생
		 **/
		if (addr + size - 1 < addr)
			goto overflow;

		/** 20140329    
		 * vmap_area_root에서부터 search하기 위해 n으로 가져옴
		 * first는 NULL로 초기화 
		 **/
		n = vmap_area_root.rb_node;
		first = NULL;

		while (n) {
			struct vmap_area *tmp;
			/** 20140329    
			 * rbtree를 순회하며 현재 n이 가리키는 vmap_area를 가져와
			 **/
			tmp = rb_entry(n, struct vmap_area, rb_node);
			/** 20140329    
			 * sort된 rbtree에서 addr(정렬된 vstart)를 각 rb_entry와 비교하며
			 **/
			if (tmp->va_end >= addr) {
				/** 20140405    
				 * 현재 traverse 중인 rb_node의 va_end보다 작거나 같다면
				 **/
				first = tmp;
				/** 20140405    
				 * rb_node가 갖고 있는 va_start와 va_end사이에 addr이 포함되는 경우 break;
				 **/
				if (tmp->va_start <= addr)
					break;
				/** 20140405    
				 * 현재 비교하는 rb_node의 va_start보다 addr이 작은 경우 왼쪽 순회
				 **/
				n = n->rb_left;
			} else
				/** 20140405    
				 * 현재 traverse 중인 rb_node의 va_end보다 크다면
				 * 오른쪽 순회
				 **/
				n = n->rb_right;
		}

		/** 20140329    
		 * first가 NULL인 경우 found로 바로 이동해 다음 while block을 수행하지 않는다.
		 *		- rb_tree의 가장 오른쪽 끝 노드까지 내려온 경우
		 *
		 * 20140405
		 * first가 NULL이 아닌 경우 first는 비교를 시작할 위치를 갖고 있다.
		 *
		 * 즉, 이 routine은 현재 rb_tree 상의 node들이 갖고 있는 address range에서 홀을 찾는 과정과 같다.
		 **/
		if (!first)
			goto found;
	}

	/* from the starting point, walk areas until a suitable hole is found */
	while (addr + size > first->va_start && addr + size <= vend) {
		if (addr + cached_hole_size < first->va_start)
			cached_hole_size = first->va_start - addr;
		addr = ALIGN(first->va_end, align);
		if (addr + size - 1 < addr)
			goto overflow;

		/** 20140329    
		 * list로 연결된 vmap_area 중 first가 last entry인 경우
		 * found로 이동
		 **/
		if (list_is_last(&first->list, &vmap_area_list))
			goto found;

		/** 20140329    
		 * last가 아닌 경우, sort되어 있는 list에서 다음 entry를 first로 삼는다.
		 **/
		first = list_entry(first->list.next,
				struct vmap_area, list);
	}

found:
	/** 20140329    
	 * 범위값 vend를 벗어나면 overflow
	 **/
	if (addr + size > vend)
		goto overflow;

	/** 20140329    
	 * addr와 size를 바탕으로 vmap_area에 정보를 채운다.
	 **/
	va->va_start = addr;
	va->va_end = addr + size;
	va->flags = 0;
	/** 20140405    
	 * vmap_area를 자료구조에 추가한다.
	 **/
	__insert_vmap_area(va);
	/** 20140329    
	 * 새로 추가된 va의 rb_node를 free_vmap_cache로 삼는다.
	 **/
	free_vmap_cache = &va->rb_node;
	spin_unlock(&vmap_area_lock);

	BUG_ON(va->va_start & (align-1));
	BUG_ON(va->va_start < vstart);
	BUG_ON(va->va_end > vend);

	return va;

overflow:
	spin_unlock(&vmap_area_lock);
	/** 20140405    
	 * purged(추방)하지 않은상태라면 한 번은 purge_vmap_area_lazy 한 뒤 다시 시도.
	 **/
	if (!purged) {
		purge_vmap_area_lazy();
		purged = 1;
		goto retry;
	}
	if (printk_ratelimit())
		printk(KERN_WARNING
			"vmap allocation for size %lu failed: "
			"use vmalloc=<size> to increase size.\n", size);
	kfree(va);
	return ERR_PTR(-EBUSY);
}

/** 20140419    
 * 전달된 vmap_area를 자료구조(rb-tree, list)에서 제거하고,
 * rcu 제거함수를 이용해 해제.
 **/
static void __free_vmap_area(struct vmap_area *va)
{
	BUG_ON(RB_EMPTY_NODE(&va->rb_node));

	/** 20140419    
	 * vmap_area cache가 존재하면
	 **/
	if (free_vmap_cache) {
		/** 20140419    
		 * overlap되는 부분이 없는 경우 free_vmap_cache는 NULL.
		 **/
		if (va->va_end < cached_vstart) {
			free_vmap_cache = NULL;
		} else {
		/** 20140419    
		 * overlap되는 부분이 있는 경우 rb_tree에서 이전 vmap_area를 가져와
		 * 새로운 free_vmap_cache로 삼는다.
		 **/
			struct vmap_area *cache;
			cache = rb_entry(free_vmap_cache, struct vmap_area, rb_node);
			if (va->va_start <= cache->va_start) {
				free_vmap_cache = rb_prev(&va->rb_node);
				/*
				 * We don't try to update cached_hole_size or
				 * cached_align, but it won't go very wrong.
				 */
			}
		}
	}
	/** 20140419    
	 * rb tree(vmap_area_root)에서 vmap_area를 제거한다.
	 **/
	rb_erase(&va->rb_node, &vmap_area_root);
	RB_CLEAR_NODE(&va->rb_node);
	/** 20140419    
	 * list에서 vmap_area를 제거한다.
	 **/
	list_del_rcu(&va->list);

	/*
	 * Track the highest possible candidate for pcpu area
	 * allocation.  Areas outside of vmalloc area can be returned
	 * here too, consider only end addresses which fall inside
	 * vmalloc area proper.
	 */
	/** 20140419    
	 * 삭제하는 vmap_area의 end가 VMALLOC_START ~ VMALLOC_END 사이에 존재하면
	 * 현재의 hole과 vmap_end 중 큰 값을 vmap_area_pcpu_hole에 저장한다.
	 **/
	if (va->va_end > VMALLOC_START && va->va_end <= VMALLOC_END)
		vmap_area_pcpu_hole = max(vmap_area_pcpu_hole, va->va_end);

	/** 20140419    
	 * vmap_area free.
	 * 자세한 분석은 하지 않음???
	 **/
	kfree_rcu(va, rcu_head);
}

/*
 * Free a region of KVA allocated by alloc_vmap_area
 */
static void free_vmap_area(struct vmap_area *va)
{
	spin_lock(&vmap_area_lock);
	__free_vmap_area(va);
	spin_unlock(&vmap_area_lock);
}

/*
 * Clear the pagetable entries of a given vmap_area
 */
/** 20140405    
 * vmap_area를 page table에서 clear 한다.
 **/
static void unmap_vmap_area(struct vmap_area *va)
{
	/** 20140405    
	 * va_start ~ va_end 사이의 page table을 unmap 한다.
	 **/
	vunmap_page_range(va->va_start, va->va_end);
}

static void vmap_debug_free_range(unsigned long start, unsigned long end)
{
	/*
	 * Unmap page tables and force a TLB flush immediately if
	 * CONFIG_DEBUG_PAGEALLOC is set. This catches use after free
	 * bugs similarly to those in linear kernel virtual address
	 * space after a page has been freed.
	 *
	 * All the lazy freeing logic is still retained, in order to
	 * minimise intrusiveness of this debugging feature.
	 *
	 * This is going to be *slow* (linear kernel virtual address
	 * debugging doesn't do a broadcast TLB flush so it is a lot
	 * faster).
	 */
#ifdef CONFIG_DEBUG_PAGEALLOC
	vunmap_page_range(start, end);
	flush_tlb_kernel_range(start, end);
#endif
}

/*
 * lazy_max_pages is the maximum amount of virtual address space we gather up
 * before attempting to purge with a TLB flush.
 *
 * There is a tradeoff here: a larger number will cover more kernel page tables
 * and take slightly longer to purge, but it will linearly reduce the number of
 * global TLB flushes that must be performed. It would seem natural to scale
 * this number up linearly with the number of CPUs (because vmapping activity
 * could also scale linearly with the number of CPUs), however it is likely
 * that in practice, workloads might be constrained in other ways that mean
 * vmap activity will not scale linearly with CPUs. Also, I want to be
 * conservative and not introduce a big latency on huge systems, so go with
 * a less aggressive log scale. It will still be an improvement over the old
 * code, and it will be simple to change the scale factor if we find that it
 * becomes a problem on bigger systems.
 */
/** 20140405    
 * TLB flush를 통해 방출하기 전까지 virtual address space를 얼마만큼 보유할지
 * 결정하는 함수.
 * 여러가지 trade-off를 고려해 cpu의 log 값을 취해 계산함.
 **/
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	/** 20140405    
	 * online cpu의 수의 bit수를 구한다.
	 *   ex) fls(6) -> 3이 리턴.
	 **/
	log = fls(num_online_cpus());

	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
}

/** 20140405    
 * atomic 전역변수. lazy 해제할 vmap_area의 page 수를 기록한다.
 **/
static atomic_t vmap_lazy_nr = ATOMIC_INIT(0);

/* for per-CPU blocks */
static void purge_fragmented_blocks_allcpus(void);

/*
 * called before a call to iounmap() if the caller wants vm_area_struct's
 * immediately freed.
 */
void set_iounmap_nonlazy(void)
{
	atomic_set(&vmap_lazy_nr, lazy_max_pages()+1);
}

/*
 * Purges all lazily-freed vmap areas.
 *
 * If sync is 0 then don't purge if there is already a purge in progress.
 * If force_flush is 1, then flush kernel TLBs between *start and *end even
 * if we found no lazy vmap areas to unmap (callers can use this to optimise
 * their own TLB flushing).
 * Returns with *start = min(*start, lowest purged address)
 *              *end = max(*end, highest purged address)
 */
/** 20140405    
 * purge_vmap_area_lazy 에서 호출된 경우 sync 1, force_flush 0
 * try_purge_vmap_area_lazy 에서 호출된 경우 sync 0, force_flush 0
 *
 * 20140419
 * lazy로 vmap_area를 방출하는 함수.
 *		1. 미사용 중인 vmap_block을 순회하며 해제
 *		2. lazy_free로 mark된 vmap_area를 찾아 임시리스트에 등록해 두었다가
 *			순차적으로 해제
 **/
static void __purge_vmap_area_lazy(unsigned long *start, unsigned long *end,
					int sync, int force_flush)
{
	static DEFINE_SPINLOCK(purge_lock);
	LIST_HEAD(valist);
	struct vmap_area *va;
	struct vmap_area *n_va;
	int nr = 0;

	/*
	 * If sync is 0 but force_flush is 1, we'll go sync anyway but callers
	 * should not expect such behaviour. This just simplifies locking for
	 * the case that isn't actually used at the moment anyway.
	 */
	/** 20140419    
	 * sync가 아니고, force_flush 요청이 없는 경우 (try_purge_vmap_area_lazy)
	 * purge_lock을 얻지 못한 경우 바로 리턴.
	 **/
	if (!sync && !force_flush) {
		if (!spin_trylock(&purge_lock))
			return;
	} else
		spin_lock(&purge_lock);

	/** 20140405    
	 * sync 가 1로 호출된 경우
	 *   vmap_block 중 alloc 상태가 아닌 vmap_area로만 이뤄진 vmap_block을 찾아 해제
	 **/
	if (sync)
		purge_fragmented_blocks_allcpus();

	rcu_read_lock();
	list_for_each_entry_rcu(va, &vmap_area_list, list) {
		/** 20140419    
		 * free_vmap_area_noflush에서 마크해둔 lazy free 인 vmap_area인 경우
		 * valist에 달아주고, VM_LAZY_FREEING으로 상태 변경.
		 **/
		if (va->flags & VM_LAZY_FREE) {
			if (va->va_start < *start)
				*start = va->va_start;
			if (va->va_end > *end)
				*end = va->va_end;
			nr += (va->va_end - va->va_start) >> PAGE_SHIFT;
			list_add_tail(&va->purge_list, &valist);
			va->flags |= VM_LAZY_FREEING;
			va->flags &= ~VM_LAZY_FREE;
		}
	}
	rcu_read_unlock();

	/** 20140419    
	 * 해제할 vmap_lazy 수에서 빼준다.
	 **/
	if (nr)
		atomic_sub(nr, &vmap_lazy_nr);

	/** 20140419    
	 * 해제하는 vmap_area가 있거나 force_flush 요청이 들어온 경우 tlb flush.
	 **/
	if (nr || force_flush)
		flush_tlb_kernel_range(*start, *end);

	if (nr) {
		spin_lock(&vmap_area_lock);
		/** 20140419    
		 * valist에 등록해 둔 vmap_area들을 순회하며 __free_vmap_area로 해제
		 **/
		list_for_each_entry_safe(va, n_va, &valist, purge_list)
			__free_vmap_area(va);
		spin_unlock(&vmap_area_lock);
	}
	spin_unlock(&purge_lock);
}

/*
 * Kick off a purge of the outstanding lazy areas. Don't bother if somebody
 * is already purging.
 */
/** 20140419    
 * start ~ end 사이 주소에 lazy로 vmap_area를 방출하는 함수.
 *
 * purge_lock을 실행하고 있는 다른 task가 있다면 lock을 획득하지 못하고 바로 리턴된다.
 **/
static void try_purge_vmap_area_lazy(void)
{
	unsigned long start = ULONG_MAX, end = 0;

	__purge_vmap_area_lazy(&start, &end, 0, 0);
}

/*
 * Kick off a purge of the outstanding lazy areas.
 */
static void purge_vmap_area_lazy(void)
{
	unsigned long start = ULONG_MAX, end = 0;

	__purge_vmap_area_lazy(&start, &end, 1, 0);
}

/*
 * Free a vmap area, caller ensuring that the area has been unmapped
 * and flush_cache_vunmap had been called for the correct range
 * previously.
 */
/** 20140405    
 * vmap_area를 free하는 함수.
 * lazy 개념을 도입해 flags에 VM_LAZE_FREE만 설정해 두고,
 * 실제 해제는 __purge_vmap_area_lazy를 통해 이뤄진다.
 **/
static void free_vmap_area_noflush(struct vmap_area *va)
{
	/** 20140405    
	 * VM_LAZY_FREE로 flags를 설정한다.
	 * 실제 memory에서 vmap_area를 해제하는 동작은 __purge_vmap_area_lazy에서 이뤄진다.
	 *
	 **/
	va->flags |= VM_LAZY_FREE;
	/** 20140405    
	 * size를 구해 page 개수를 vmap_lazy_nr에 누적시킨다.
	 **/
	atomic_add((va->va_end - va->va_start) >> PAGE_SHIFT, &vmap_lazy_nr);
	/** 20140405    
	 * 그 숫자가 lazy_max_pages 보다 크다면 (일종의 작동 임계값)
	 * try_purge_vmap_area_lazy으로 해제 시도를 시작한다.
	 **/
	if (unlikely(atomic_read(&vmap_lazy_nr) > lazy_max_pages()))
		try_purge_vmap_area_lazy();
}

/*
 * Free and unmap a vmap area, caller ensuring flush_cache_vunmap had been
 * called for the correct range previously.
 */
/** 20140405    
 * vmap_area에 대해 unmap(page table에서 해제)하고 free(lazy)시킨다.
 **/
static void free_unmap_vmap_area_noflush(struct vmap_area *va)
{
	/** 20140405    
	 * va에 해당하는 영역의 page table을 clear.
	 **/
	unmap_vmap_area(va);
	/** 20140405    
	 * vmap_area를 lazy free. 즉, 실제 메모리 해제동작은 나중에 이뤄질 것이다.
	 **/
	free_vmap_area_noflush(va);
}

/*
 * Free and unmap a vmap area
 */
/** 20140405    
 * vmap_area를 해제하는 함수
 **/
static void free_unmap_vmap_area(struct vmap_area *va)
{
	/** 20140405    
	 * va_start ~ va_end 사이의 주소에 대해 cache를 flush 한다.
	 **/
	flush_cache_vunmap(va->va_start, va->va_end);
	/** 20140405    
	 * vmap_area를 free(lazy)하고 page table에서 해제한다.
	 **/
	free_unmap_vmap_area_noflush(va);
}

static struct vmap_area *find_vmap_area(unsigned long addr)
{
	struct vmap_area *va;

	spin_lock(&vmap_area_lock);
	va = __find_vmap_area(addr);
	spin_unlock(&vmap_area_lock);

	return va;
}

/** 20140405    
 * addr 영역에 대해 vmap_area를 찾아 해제한다.
 **/
static void free_unmap_vmap_area_addr(unsigned long addr)
{
	struct vmap_area *va;

	/** 20140405    
	 * addr로 rb_tree에서 vmap_area를 찾아온다.
	 **/
	va = find_vmap_area(addr);
	BUG_ON(!va);
	/** 20140405    
	 * 찾은 va를 해제한다 (실제 메모리 해제 과정은 lazy로 이뤄진다)
	 * unmap과 cache flush 과정은 동기적으로 수행됨.
	 **/
	free_unmap_vmap_area(va);
}


/*** Per cpu kva allocator ***/

/*
 * vmap space is limited especially on 32 bit architectures. Ensure there is
 * room for at least 16 percpu vmap blocks per CPU.
 */
/*
 * If we had a constant VMALLOC_START and VMALLOC_END, we'd like to be able
 * to #define VMALLOC_SPACE		(VMALLOC_END-VMALLOC_START). Guess
 * instead (we just need a rough idea)
 */
#if BITS_PER_LONG == 32
/** 20140405    
 * x86 시스템에서 3:1 split을 한 경우, VMALLOC_SPACE는 128MB
 **/
#define VMALLOC_SPACE		(128UL*1024*1024)
#else
#define VMALLOC_SPACE		(128UL*1024*1024*1024)
#endif

/** 20140405    
 * VMALLOC_SPACE를 PAGE_SIZE로 나눈 페이지 개수
 *   128MB / 4KB = 32 * 1024
 **/
#define VMALLOC_PAGES		(VMALLOC_SPACE / PAGE_SIZE)
#define VMAP_MAX_ALLOC		BITS_PER_LONG	/* 256K with 4K pages */
#define VMAP_BBMAP_BITS_MAX	1024	/* 4MB with 4K pages */
#define VMAP_BBMAP_BITS_MIN	(VMAP_MAX_ALLOC*2)
#define VMAP_MIN(x, y)		((x) < (y) ? (x) : (y)) /* can't use min() */
#define VMAP_MAX(x, y)		((x) > (y) ? (x) : (y)) /* can't use max() */
/** 20140405    
 * BBMAP_BITS를 계산. 최소값과 최대값을 넘지않는 선에서 BBMAP_BITS를 계산한다.
 * VMAP_BBMAP_BITS_MIN : 32 * 2 = 64
 * VMAP_BBMAP_BITS_MAX : 1024
 *
 * 64 <= VMAP_BBMAP_BITS <= 1024
 *	(32 * 1024) / 4 / 16 = 512
 **/
#define VMAP_BBMAP_BITS		\
		VMAP_MIN(VMAP_BBMAP_BITS_MAX,	\
		VMAP_MAX(VMAP_BBMAP_BITS_MIN,	\
			VMALLOC_PAGES / roundup_pow_of_two(NR_CPUS) / 16))

/** 20140412    
 * VMAP_BBMAP_BITS (512) * PAGE_SIZE (4096) = 2MB
 **/
#define VMAP_BLOCK_SIZE		(VMAP_BBMAP_BITS * PAGE_SIZE)

/** 20140322    
 * vmalloc_init이 호출 완료 여부를 표시
 **/
static bool vmap_initialized __read_mostly = false;

/** 20140412    
 * vmap_block_queue는 per-CPU 변수로, spin_lock으로 보호된다.
 * free는 사용 가능한 vmap_block 리스트이다.
 **/
struct vmap_block_queue {
	spinlock_t lock;
	struct list_head free;
};

/** 20140405    
 **/
struct vmap_block {
	spinlock_t lock;
	/** 20140412    
	 * vmap_block이 관리하는 page들 전체를 매핑할 vmap_area
	 **/
	struct vmap_area *va;
	/** 20140405    
	 * vmap_block이 속한 queue 정보
	 **/
	struct vmap_block_queue *vbq;
	/** 20140412    
	 * free는 사용하지 않은 page의 수 (vmap_area 할당이 가능한)
	 * dirty는 vmap_area는 해제했지만 실제로 메모리에 남아 있는 page의 수
	 **/
	unsigned long free, dirty;
	/** 20140405    
	 * VMAP_BBMAP_BITS를 표현할 수 있는 alloc_map, dirty_map을 선언
	 * VMAP_BBMAP_BITS (CPU 4개인 경우 512) / 32 (LONG 32BIT) 
	 *
	 * alloc_map을 설정하는 부분 : bitmap_find_free_region
	 * dirty_map을 설정하는 부분 : bitmap_allocate_region
	 **/
	DECLARE_BITMAP(alloc_map, VMAP_BBMAP_BITS);
	DECLARE_BITMAP(dirty_map, VMAP_BBMAP_BITS);
	/** 20140405    
	 * vmap_block_queue list에 연결할 때 사용.
	 **/
	struct list_head free_list;
	struct rcu_head rcu_head;
	struct list_head purge;
};

/* Queue of free and dirty vmap blocks, for allocation and flushing purposes */
/** 20140322    
 * 정적 per cpu 변수 선언.
 **/
static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queue);

/*
 * Radix tree of vmap blocks, indexed by address, to quickly find a vmap block
 * in the free path. Could get rid of this if we change the API to return a
 * "cookie" from alloc, to be passed to free. But no big deal yet.
 */
/** 20140412    
 * vmap_block 들의 radix tree에 대한 lock.
 **/
static DEFINE_SPINLOCK(vmap_block_tree_lock);
/** 20140412    
 * vmap_block 를 관리할 radix tree를 생성한다.
 **/
static RADIX_TREE(vmap_block_tree, GFP_ATOMIC);

/*
 * We should probably have a fallback mechanism to allocate virtual memory
 * out of partially filled vmap blocks. However vmap block sizing should be
 * fairly reasonable according to the vmalloc size, so it shouldn't be a
 * big problem.
 */

/** 20140405    
 * address가 속한 vmap block의 index를 구한다.
 **/
static unsigned long addr_to_vb_idx(unsigned long addr)
{
	/** 20140405    
	 * VMALLOC_START를 VMAP_BLOCK_SIZE 단위로 align 시킨 뒤,
	 * addr에서 빼준다.
	 **/
	addr -= VMALLOC_START & ~(VMAP_BLOCK_SIZE-1);
	/** 20140405    
	 * addr을 VMAP_BLOCK_SIZE로 나눈 몫을 구해 index로 삼는다.
	 **/
	addr /= VMAP_BLOCK_SIZE;
	return addr;
}

/** 20140412
 * 새로운 vmap_block 공간을 할당받고, vmap_block이 관리할 vmap_area를 할당 받는다.
 * vmap_block을 vmap_block_queue, vmap_block_tree에 추가한다.
 **/
static struct vmap_block *new_vmap_block(gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	struct vmap_area *va;
	unsigned long vb_idx;
	int node, err;

	/** 20140412    
	 * 현재 node의 id
	 **/
	node = numa_node_id();

	/** 20140412    
	 * vmap_block을 동적 메모리 할당. RECLAIM 대상에서 제외한 페이지를 요청한다.
	 **/
	vb = kmalloc_node(sizeof(struct vmap_block),
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!vb))
		return ERR_PTR(-ENOMEM);

	/** 20140412    
	 * vmap_block이 관리하는 페이지들을 매핑하기 위한 vmap_area를 할당받는다.
	 **/
	va = alloc_vmap_area(VMAP_BLOCK_SIZE, VMAP_BLOCK_SIZE,
					VMALLOC_START, VMALLOC_END,
					node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(vb);
		return ERR_CAST(va);
	}

	/** 20140412    
	 * radix_tree_preload를 사용해 per-cpu pool의 빈공간을 채운다.
	 * radix_tree_insert를 하기 전에 호출한다.
	 *
	 * 성공한다면 선점 불가능 상태로 리턴하므로 사용이 끝난 지점에
	 * radix_tree_preload_end를 호출해야 한다.
	 **/
	err = radix_tree_preload(gfp_mask);
	if (unlikely(err)) {
		kfree(vb);
		free_vmap_area(va);
		return ERR_PTR(err);
	}

	/** 20140412    
	 * vmap_block에 대한 spin_lock을 사용한다.
	 **/
	spin_lock_init(&vb->lock);
	/** 20140412    
	 * vmap_block에 할당 받은 vmap_area를 매핑한다.
	 **/
	vb->va = va;
	/** 20140412    
	 * 현재 vmap_block이 관리하는 전체 페이지가 free이며, dirty 상태가 아니다.
	 **/
	vb->free = VMAP_BBMAP_BITS;
	vb->dirty = 0;
	/** 20140412    
	 * bitmap을 0으로 채운다.
	 **/
	bitmap_zero(vb->alloc_map, VMAP_BBMAP_BITS);
	bitmap_zero(vb->dirty_map, VMAP_BBMAP_BITS);
	/** 20140412    
	 * vmap_block_queue에 연결할 때 사용하는 리스트 자료구조 초기화
	 **/
	INIT_LIST_HEAD(&vb->free_list);

	/** 20140412    
	 * 할당받은 va_start가 속한 vmap_block의 index를 구한다.
	 * vmap_block_tree는 radix tree이고, spin_lock으로 보호된다.
	 **/
	vb_idx = addr_to_vb_idx(va->va_start);
	spin_lock(&vmap_block_tree_lock);
	/** 20140412    
	 * 새로 할당한 vmap_block을 radix tree에 추가한다.
	 *
	 * vmap_block_tree는 radix tree이고, radix tree의 slot은 RCU이므로
	 * tree에 추가하는 동작은 lock에 의해 보호 받아야 한다.
	 **/
	err = radix_tree_insert(&vmap_block_tree, vb_idx, vb);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(err);
	/** 20140412    
	 * radix_tree_preload에서 막아둔 선점을 가능하게 한다.
	 **/
	radix_tree_preload_end();

	/** 20140412    
	 * 현재 cpu의 vmap_block_queue를 받아온다. (선점불가)
	 **/
	vbq = &get_cpu_var(vmap_block_queue);
	/** 20140412    
	 * vmap_block에 연결된 vmap_block_queue를 지정한다.
	 **/
	vb->vbq = vbq;
	spin_lock(&vbq->lock);
	/** 20140412    
	 * vmap_block을 vmap_block_queue의 free 리스트에 추가한다.
	 **/
	list_add_rcu(&vb->free_list, &vbq->free);
	spin_unlock(&vbq->lock);
	/** 20140412    
	 * get_cpu_var 에 대응되는 부분 (선점가능)
	 **/
	put_cpu_var(vmap_block_queue);

	return vb;
}

/** 20140412    
 * vmap_block의 tree에서 제거하고, mapping한 vmap_area를 해제한다.
 * vmap_block은 rcu 동기화되는 object이므로 rcu API를 사용해 메모리 해제한다.
 **/
static void free_vmap_block(struct vmap_block *vb)
{
	struct vmap_block *tmp;
	unsigned long vb_idx;

	/** 20140412    
	 * vmap_block에 mapping된 vmap_area의 시작 주소로 vmap_block의 index를 구한다. 
	 **/
	vb_idx = addr_to_vb_idx(vb->va->va_start);
	/** 20140412    
	 * vmap block tree는 spinlock으로 보호된다.
	 **/
	spin_lock(&vmap_block_tree_lock);
	/** 20140412    
	 * vmap_block_tree에서 vb_idx에 해당하는 vmap_block을 제거한다.
	 **/
	tmp = radix_tree_delete(&vmap_block_tree, vb_idx);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(tmp != vb);

	/** 20140412    
	 * 제거한 vmap_block의 va에 해당하는 vmap_area를 free한다.
	 *     flush는 별도로 수행한다.
	 **/
	free_vmap_area_noflush(vb->va);
	/** 20140412    
	 * vmap_block에 대한 접근은 rcu 동기화를 통해 관리된다.
	 * rcu로 관리되는 object의 해제이므로 kfree_rcu API를 사용한다.
	 **/
	kfree_rcu(vb, rcu_head);
}

/** 20140419   
 * 미사용 중인 (free + dirty 상태) vmap_block을 찾아 vmap_area를 제거하고 해제.
 * lazy free의 실제 동작
 **/
static void purge_fragmented_blocks(int cpu)
{
	LIST_HEAD(purge);
	struct vmap_block *vb;
	struct vmap_block *n_vb;
	/** 20140405    
	 * 현재 cpu의 vmap_block_queue를 가져온다.
	 **/
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);

	/** 20140412    
	 * vmap_block은 rcu 동기화 대상 object이므로 vmap_block을 순회할 때는
	 * rcu_read_lock 구간으로 보호되어야 한다.
	 **/
	rcu_read_lock();
	/** 20140405    
	 * vmap_block_queue의 free list를 순회하며 vmap_block를 vb로 가리킨다.
	 **/
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {

		/** 20140412    
		 * '현재 vmap_block이 모두 free(미사용)와 dirty(page_table에서만 해제한 상태)로만 이루어져 있고, 그 중 모두가 dirty 상태가 아닐 경우'
		 *		=> 이 경우일 때만 나머지 동작을 수행
		 **/
		if (!(vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS))
			continue;

		spin_lock(&vb->lock);
		/** 20140405    
		 * vmap_block이 관리하는 vmap_area 영역이
		 * free + dirty 로만 이루어져 있고, 모두 dirty는 아닐 때
		 **/
		if (vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS) {
			/** 20140412    
			 * vmap_block의 free page를 0으로 만들어 unlock 이후 할당을 막는다.
			 **/
			vb->free = 0; /* prevent further allocs after releasing lock */
			vb->dirty = VMAP_BBMAP_BITS; /* prevent purging it again */
			/** 20140405    
			 * vmap block의 alloc_map과 dirty_map을 모두 채운다.
			 **/
			bitmap_fill(vb->alloc_map, VMAP_BBMAP_BITS);
			bitmap_fill(vb->dirty_map, VMAP_BBMAP_BITS);
			spin_lock(&vbq->lock);
			/** 20140405    
			 * vmap_block을 vmap_block_queue에서 제거한다.
			 **/
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
			spin_unlock(&vb->lock);
			/** 20140405    
			 * vmap_block을 purge 리스트에 등록한다.
			 **/
			list_add_tail(&vb->purge, &purge);
		} else
			spin_unlock(&vb->lock);
	}
	rcu_read_unlock();

	/** 20140405    
	 * purge 리스트에 등록해 놓은 vmap_block을 리스트에서 제거하고
	 * free_vmap_block으로 해제한다.
	 **/
	list_for_each_entry_safe(vb, n_vb, &purge, purge) {
		list_del(&vb->purge);
		free_vmap_block(vb);
	}
}

/** 20140405    
 * 현재 cpu의 fragmented block들을 퇴출한다.
 **/
static void purge_fragmented_blocks_thiscpu(void)
{
	/** 20140405    
	 * 현재 cpu에 대해 fragmented block (free와 dirty만으로 이뤄진 경우)을
	 * 퇴출한다.
	 **/
	purge_fragmented_blocks(smp_processor_id());
}

/** 20140419    
 * cpu 별로 존재하는 vmap_block_queue에서 미사용 중인 vmap_block을 찾아 해제
 **/
static void purge_fragmented_blocks_allcpus(void)
{
	int cpu;

	/** 20140405    
	 * possible cpu 목록을 순회하면서 미사용 중인 vmap_block purge
	 **/
	for_each_possible_cpu(cpu)
		purge_fragmented_blocks(cpu);
}

/** 20140405    
 * vmap_block은 bitmap으로 vmap_area를 한 번에 할당받아 페이지 단위로 관리한다.
 * vmap_area가 vmap_block이 관리하는 크기 이하로 필요할 때, vmap_block에서 할당 받는다.
 *
 * 이 함수는 vmap_block에서 size만큼의 vmap_area (가상 주소 공간) 를 받아 리턴한다.
 * 만약 vmap_block_queue에 vmap_block이 남아 있지 않다면,
 * 새로 할당받아 queue에 추가한 뒤 다시 할당을 시도한다.
 **/
static void *vb_alloc(unsigned long size, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	unsigned long addr = 0;
	unsigned int order;
	int purge = 0;

	BUG_ON(size & ~PAGE_MASK);
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);
	if (WARN_ON(size == 0)) {
		/*
		 * Allocating 0 bytes isn't what caller wants since
		 * get_order(0) returns funny result. Just warn and terminate
		 * early.
		 */
		return NULL;
	}
	/** 20140405    
	 * size의 지수값을 구함
	 **/
	order = get_order(size);

again:
	rcu_read_lock();
	/** 20140405    
	 * 현재 percpu의 vmap_block_queue를 가져온다.
	 **/
	vbq = &get_cpu_var(vmap_block_queue);
	/** 20140405    
	 * vmap_block_queue의 free가 가리키는 free_list를 포함하는 vmap_block을 가져온다.
	 *
	 * vmap_block_queue는 rcu를 사용해 관리한다.
	 **/
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		int i;

		/** 20140405    
		 * vmap_block에 lock을 건다.
		 **/
		spin_lock(&vb->lock);
		/** 20140405    
		 * 현재 vmap_block의 free가 1 ** order 만큼의 여유가 없을 경우
		 * 다음 vmap_block을 찾는다.
		 **/
		if (vb->free < 1UL << order)
			goto next;

		/** 20140405    
		 * alloc_map에서 free region을 찾아 설정(alloc)한다.
		 **/
		i = bitmap_find_free_region(vb->alloc_map,
						VMAP_BBMAP_BITS, order);

		/** 20140405    
		 * free region을 찾아 할당하지 못한 경우
		 **/
		if (i < 0) {
			/** 20140405    
			 * 현재 vmap_block이 free와 dirty 상태만 존재할 경우
			 **/
			if (vb->free + vb->dirty == VMAP_BBMAP_BITS) {
				/* fragmented and no outstanding allocations */
				BUG_ON(vb->dirty != VMAP_BBMAP_BITS);
				/** 20140405    
				 * purge 시킨다.
				 **/
				purge = 1;
			}
			/** 20140405    
			 * 동작은 next로 이동. vmap_block_queue의 다음 entry.
			 **/
			goto next;
		}
		/** 20140405    
		 * vmap_area의 va_start에 찾은 bit의 offset만큼을 구해 addr에 저장
		 **/
		addr = vb->va->va_start + (i << PAGE_SHIFT);
		BUG_ON(addr_to_vb_idx(addr) !=
				addr_to_vb_idx(vb->va->va_start));
		/** 20140405    
		 * vmap_block의 free page 수를 order만큼 감소시킨다.
		 **/
		vb->free -= 1UL << order;
		/** 20140405    
		 * vmap_block의 free page를 모두 사용하였다면
		 * vmap_block을 queue에서 제거한다.
		 **/
		if (vb->free == 0) {
			/** 20140405   
			 * vmap_block_queue는 per-CPU 변수로 spinlock으로 보호받는다.
			 * vmap_block은 rcu로 관리되는 object이므로 list 제거시 rcu API를 사용한다.
			 **/
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
		}
		spin_unlock(&vb->lock);
		/** 20140405    
		 * vmap_block에서 vmap_area를 받아온 상태이므로 break
		 **/
		break;
next:
		spin_unlock(&vb->lock);
	}

	/** 20140405    
	 * purge를 수행해야 한다고 기록해 두었으면
	 * 현재 cpu의 fragmented blocks을 퇴출한다.
	 **/
	if (purge)
		purge_fragmented_blocks_thiscpu();

	/** 20140405    
	 * 현재 cpu를 선점 가능 상태로 만든다.
	 **/
	put_cpu_var(vmap_block_queue);
	rcu_read_unlock();

	/** 20140405    
	 * vmap_block_queue의 vmap_block들을 순회했지만 addr를 받아오지 못했다면
	 **/
	if (!addr) {
		/** 20140405    
		 * 새로운 vmap_block을 할당받아 설정하고 다시 시도한다.
		 **/
		vb = new_vmap_block(gfp_mask);
		if (IS_ERR(vb))
			return vb;
		goto again;
	}

	/** 20140405    
	 * 받아온 vmap_block에 속하는 page 단위의 addr를 리턴한다.
	 **/
	return (void *)addr;
}

/** 20140405    
 * vmap_block에서 addr부터 size만큼의 vmap_area를 해제하는 함수.
 *
 * dirtymap에 표시만 하고, 실제 해제동작은 vmap_block이 모두 해제되었을 때 또는
 * purge_fragmented_blocks 등에서 이루어짐
 **/
static void vb_free(const void *addr, unsigned long size)
{
	unsigned long offset;
	unsigned long vb_idx;
	unsigned int order;
	struct vmap_block *vb;

	BUG_ON(size & ~PAGE_MASK);
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);

	/** 20140412    
	 * HW cache flush.
	 **/
	flush_cache_vunmap((unsigned long)addr, (unsigned long)addr + size);

	/** 20140412    
	 * 요청한 크기로 order를 구해온다.
	 **/
	order = get_order(size);

	/** 20140412    
	 * address에서 vmap_block의 offset을 추출한다.
	 **/
	offset = (unsigned long)addr & (VMAP_BLOCK_SIZE - 1);

	/** 20140405    
	 * addr가 속하는 vmap_block의 index를 구한다.
	 **/
	vb_idx = addr_to_vb_idx((unsigned long)addr);
	/** 20140412    
	 * radix_tree의 node는 rcu를 이용해 동기화시킨다.
	 * rcu read operation은 read lock이 필요하다. (write lock은 존재하지 않음)
	 **/
	rcu_read_lock();
	/** 20140405    
	 * vmap_block_tree에서 해당 vb_idx로 vmap_block을 찾는다.
	 **/
	vb = radix_tree_lookup(&vmap_block_tree, vb_idx);
	rcu_read_unlock();
	/** 20140405    
	 * vmap_block이 존재하지 않으면 BUG.
	 **/
	BUG_ON(!vb);

	/** 20140405    
	 * addr ~ addr + size에 해당하는 부분을 page table에서 unmap 시킨다.
	 * (pte에 0을 기록하는 방식으로 clear)
	 **/
	vunmap_page_range((unsigned long)addr, (unsigned long)addr + size);

	spin_lock(&vb->lock);
	/** 20140405    
	 * allocate_region을 통해 free할 page들을 dirty_map에 set을 한다.
	 **/
	BUG_ON(bitmap_allocate_region(vb->dirty_map, offset >> PAGE_SHIFT, order));

	/** 20140405    
	 * 2 ** order 만큼의 페이지를 dirty로 표현
	 **/
	vb->dirty += 1UL << order;
	/** 20140405    
	 * 모든 VMAP_BBMAP_BITS가 다 해제되었다면 free_vmap_block을 호출해 vmap_block 자체를 free.
	 * 즉, 모든 page가 vunmap 되기 전에는 즉시 vmap_block을 해제하지 않는다.
	 **/
	if (vb->dirty == VMAP_BBMAP_BITS) {
		BUG_ON(vb->free);
		spin_unlock(&vb->lock);
		free_vmap_block(vb);
	} else
		spin_unlock(&vb->lock);
}

/**
 * vm_unmap_aliases - unmap outstanding lazy aliases in the vmap layer
 *
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB entries
 * still referencing that page (additional to the regular 1:1 kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we can
 * be sure that none of the pages we have control over will have any aliases
 * from the vmap layer.
 */
void vm_unmap_aliases(void)
{
	unsigned long start = ULONG_MAX, end = 0;
	int cpu;
	int flush = 0;

	if (unlikely(!vmap_initialized))
		return;

	for_each_possible_cpu(cpu) {
		struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);
		struct vmap_block *vb;

		rcu_read_lock();
		list_for_each_entry_rcu(vb, &vbq->free, free_list) {
			int i;

			spin_lock(&vb->lock);
			i = find_first_bit(vb->dirty_map, VMAP_BBMAP_BITS);
			while (i < VMAP_BBMAP_BITS) {
				unsigned long s, e;
				int j;
				j = find_next_zero_bit(vb->dirty_map,
					VMAP_BBMAP_BITS, i);

				s = vb->va->va_start + (i << PAGE_SHIFT);
				e = vb->va->va_start + (j << PAGE_SHIFT);
				flush = 1;

				if (s < start)
					start = s;
				if (e > end)
					end = e;

				i = j;
				i = find_next_bit(vb->dirty_map,
							VMAP_BBMAP_BITS, i);
			}
			spin_unlock(&vb->lock);
		}
		rcu_read_unlock();
	}

	__purge_vmap_area_lazy(&start, &end, 1, flush);
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/**
 * vm_unmap_ram - unmap linear kernel address space set up by vm_map_ram
 * @mem: the pointer returned by vm_map_ram
 * @count: the count passed to that vm_map_ram call (cannot unmap partial)
 */
/** 20140405    
 * memory mapping을 해제할 위치와 page의 수를 전달 받아
 * vmap_area를 해제하는 함수.
 * 
 * VMAP_MAX_ALLOC 이하면 vmap_block 으로 하당했으므로 vb_free 호출,
 * 그 이상이라면 vmap_area를 직접 찾아 호출.
 **/
void vm_unmap_ram(const void *mem, unsigned int count)
{
	unsigned long size = count << PAGE_SHIFT;
	unsigned long addr = (unsigned long)mem;

	BUG_ON(!addr);
	BUG_ON(addr < VMALLOC_START);
	BUG_ON(addr > VMALLOC_END);
	BUG_ON(addr & (PAGE_SIZE-1));

	debug_check_no_locks_freed(mem, size);
	vmap_debug_free_range(addr, addr+size);

	/** 20140405    
	 * vm_map_ram과 쌍을 이뤄 해제함수를 호출한다.
	 *
	 * count가  VMAP_MAX_ALLOC 이하일 경우
	 *   vb_free를 이용해 해제
	 * 그보다 큰 경우 
	 *   free_unmap_vmap_area_addr를 이용해 해제
	 **/
	if (likely(count <= VMAP_MAX_ALLOC))
		vb_free(mem, size);
	else
		free_unmap_vmap_area_addr(addr);
}
EXPORT_SYMBOL(vm_unmap_ram);

/**
 * vm_map_ram - map pages linearly into kernel virtual address (vmalloc space)
 * @pages: an array of pointers to the pages to be mapped
 * @count: number of pages
 * @node: prefer to allocate data structures on this node
 * @prot: memory protection to use. PAGE_KERNEL for regular RAM
 *
 * Returns: a pointer to the address that has been mapped, or %NULL on failure
 */
/** 20140412    
 * 물리 페이지를 count 개의 page만큼 vmap_area를 할당 받아 매핑하고,
 * page_table에 등록시킨다.
 **/
void *vm_map_ram(struct page **pages, unsigned int count, int node, pgprot_t prot)
{
	unsigned long size = count << PAGE_SHIFT;
	unsigned long addr;
	void *mem;

	/** 20140405    
	 * count(page의 수)가 VMAP_MAX_ALLOC (32) 이하이면 vmap_block 에서 할당.
	 **/
	if (likely(count <= VMAP_MAX_ALLOC)) {
		/** 20140412    
		 * vmap_block으로부터 size만큼의 가상주소 공간을 받아온다.
		 **/
		mem = vb_alloc(size, GFP_KERNEL);
		if (IS_ERR(mem))
			return NULL;
		addr = (unsigned long)mem;
	} else {
		/** 20140412    
		 * VMAP_MAX_ALLOC 보다 큰 사이즈를 요청한 경우, alloc_vmap_area로 직접
		 * vmap_area를 할당 받는다.
		 **/
		struct vmap_area *va;
		va = alloc_vmap_area(size, PAGE_SIZE,
				VMALLOC_START, VMALLOC_END, node, GFP_KERNEL);
		if (IS_ERR(va))
			return NULL;

		addr = va->va_start;
		mem = (void *)addr;
	}
	/** 20140412    
	 * 할당받은 vmap_area를 page table에 mapping 시킨다.
	 **/
	if (vmap_page_range(addr, addr + size, prot, pages) < 0) {
		/** 20140412    
		 * vmap이 실패했을 경우 vm_unmap_ram으로 공간을 반납한다.
		 **/
		vm_unmap_ram(mem, count);
		return NULL;
	}
	/** 20140412    
	 * mapping한 vmap_area 주소를 리턴한다.
	 **/
	return mem;
}
EXPORT_SYMBOL(vm_map_ram);

/**
 * vm_area_add_early - add vmap area early during boot
 * @vm: vm_struct to add
 *
 * This function is used to add fixed kernel vm area to vmlist before
 * vmalloc_init() is called.  @vm->addr, @vm->size, and @vm->flags
 * should contain proper values and the other fields should be zero.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
/** 20130323
*	최초 vmlist = vm, vm->next = NULL
*	vmalloc_init이 호출되기 전에 vm 구조체를 vmlist에 추가하는 함수
*
*	20130330
*   vmlist에 새로운 vm_struct을 오름차순으로 삽입
*/
void __init vm_area_add_early(struct vm_struct *vm)
{
	struct vm_struct *tmp, **p;

	BUG_ON(vmap_initialized);
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		/** 20130330    
		 * 기존 tmp의 주소와 추가할 vm의 주소를 비교
		 **/
		if (tmp->addr >= vm->addr) {
			/** 20130330    
			 * 만약 주소가 겹치면(시작주소+size와 주소 비교) BUG_ON.
			 */
			BUG_ON(tmp->addr < vm->addr + vm->size);
			break;
		} else
		/** 20130330    
		 *
		 **/
			BUG_ON(tmp->addr + tmp->size > vm->addr);
	}
	vm->next = *p;
	*p = vm;
}

/**
 * vm_area_register_early - register vmap area early during boot
 * @vm: vm_struct to register
 * @align: requested alignment
 *
 * This function is used to register kernel vm area before
 * vmalloc_init() is called.  @vm->size and @vm->flags should contain
 * proper values on entry and other fields should be zero.  On return,
 * vm->addr contains the allocated address.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_register_early(struct vm_struct *vm, size_t align)
{
	static size_t vm_init_off __initdata;
	unsigned long addr;

	addr = ALIGN(VMALLOC_START + vm_init_off, align);
	vm_init_off = PFN_ALIGN(addr + vm->size) - VMALLOC_START;

	vm->addr = (void *)addr;

	vm_area_add_early(vm);
}

/** 20140322    
 * vmlist에 등록되어 있던 entry를 vmap_area로 생성해 자료구조를 구성
 **/
void __init vmalloc_init(void)
{
	struct vmap_area *va;
	struct vm_struct *tmp;
	int i;

	/** 20140322    
	 * possible cpu를 순회하며
	 **/
	for_each_possible_cpu(i) {
		struct vmap_block_queue *vbq;

		/** 20140322    
		 * cpu별로 별도의 vmap_block_queue를 가져와 vbq에 저장
		 **/
		vbq = &per_cpu(vmap_block_queue, i);
		/** 20140322    
		 * vmap_block_queue 자료구조 초기화
		 **/
		spin_lock_init(&vbq->lock);
		INIT_LIST_HEAD(&vbq->free);
	}

	/* Import existing vmlist entries. */
	/** 20140322    
	 * 현재 vmlist에 등록되어 있는 entry를 순회하며 
	 * vmap_area를 생성해 정보를 할당하고 vmap_area 자료구조에 추가한다.
	 * 
	 * 20140419
	 * vmlist에 이미 추가되어 있는 entry에 대해 vmap_area 자료구조를 구성한다.
	 * iotable_init에서 등록한 PA/VA는 vmlist에만 존재하는 상태이다.
	 **/
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		/** 20140322    
		 * vmap_area 구조체를 하나 할당한다.
		 **/
		va = kzalloc(sizeof(struct vmap_area), GFP_NOWAIT);
		va->flags = VM_VM_AREA;
		va->va_start = (unsigned long)tmp->addr;
		va->va_end = va->va_start + tmp->size;
		va->vm = tmp;
		__insert_vmap_area(va);
	}

	/** 20140322    
	 * vmap_area_pcpu_hole 초기값을 VMALLOC_END로 설정
	 **/
	vmap_area_pcpu_hole = VMALLOC_END;

	/** 20140322
	 * vmap_initialized를 표시.
	 **/
	vmap_initialized = true;
}

/**
 * map_kernel_range_noflush - map kernel VM area with the specified pages
 * @addr: start of the VM area to map
 * @size: size of the VM area to map
 * @prot: page protection flags to use
 * @pages: pages to map
 *
 * Map PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vmap() on to-be-mapped areas
 * before calling this function.
 *
 * RETURNS:
 * The number of pages mapped on success, -errno on failure.
 */
/** 20140308    
 * percpu에서 allocator로부터 할당받은 pages를 vmap 구간에 mapping.
 * vmap_page_range_noflush은 percpu 분석 후 vmalloc 분석시 분석하기로 함.
 **/
int map_kernel_range_noflush(unsigned long addr, unsigned long size,
			     pgprot_t prot, struct page **pages)
{
	return vmap_page_range_noflush(addr, addr + size, prot, pages);
}

/**
 * unmap_kernel_range_noflush - unmap kernel VM area
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Unmap PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vunmap() on to-be-mapped areas
 * before calling this function and flush_tlb_kernel_range() after.
 */
void unmap_kernel_range_noflush(unsigned long addr, unsigned long size)
{
	vunmap_page_range(addr, addr + size);
}
EXPORT_SYMBOL_GPL(unmap_kernel_range_noflush);

/**
 * unmap_kernel_range - unmap kernel VM area and flush cache and TLB
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Similar to unmap_kernel_range_noflush() but flushes vcache before
 * the unmapping and tlb after.
 */
void unmap_kernel_range(unsigned long addr, unsigned long size)
{
	unsigned long end = addr + size;

	flush_cache_vunmap(addr, end);
	vunmap_page_range(addr, end);
	flush_tlb_kernel_range(addr, end);
}

/** 20140329    
 * vm area를 pages와 mapping한다.
 **/
int map_vm_area(struct vm_struct *area, pgprot_t prot, struct page ***pages)
{
	unsigned long addr = (unsigned long)area->addr;
	unsigned long end = addr + area->size - PAGE_SIZE;
	int err;

	/** 20140329    
	 * virtual address와 pages 사이 mapping (page table에 기록)
	 **/
	err = vmap_page_range(addr, end, prot, *pages);
	if (err > 0) {
		*pages += err;
		err = 0;
	}

	return err;
}
EXPORT_SYMBOL_GPL(map_vm_area);

/*** Old vmalloc interfaces ***/
DEFINE_RWLOCK(vmlist_lock);

/** 20140322    
 *
 * vmalloc_init 전 vmlist에 추가하는 부분
 *		vm_area_add_early
 **/
struct vm_struct *vmlist;

/** 20140329    
 * vm_struct에 매개변수로 넘어온 값을 설정한다.
 * 
 * va가 vm을 찾을 수 있도록 연결하는 과정을 포함한다.
 **/
static void setup_vmalloc_vm(struct vm_struct *vm, struct vmap_area *va,
			      unsigned long flags, const void *caller)
{
	vm->flags = flags;
	/** 20140329    
	 * vm_start의 addr과 size정보는 vmap_area의 내용을 기록한다.
	 **/
	vm->addr = (void *)va->va_start;
	vm->size = va->va_end - va->va_start;
	vm->caller = caller;
	/** 20140329    
	 * vmap_area가 어떤 vm_struct에 속한 정보인지 찾기 위해 연결한다.
	 **/
	va->vm = vm;
	/** 20140329    
	 * flag에 VM_AREA가 연결되었음을 표시한다.
	 **/
	va->flags |= VM_VM_AREA;
}

/** 20140329    
 * vm_struct을 vmlist에 추가한다.
 **/
static void insert_vmalloc_vmlist(struct vm_struct *vm)
{
	struct vm_struct *tmp, **p;

	/** 20140329    
	 * flags에서 VM_UNLIST를 삭제
	 **/
	vm->flags &= ~VM_UNLIST;
	/** 20140329    
	 * vmlist에 대한 동작은 rw lock으로 보호된다.
	 * vmlist에 write 동작을 수행하는 부분이므로 write_lock을 걸고 사용한다.
	 **/
	write_lock(&vmlist_lock);
	/** 20140329    
	 * vmlist에서 정렬된 주소를 바탕으로 vm을 추가할 위치를 찾는다.
	 **/
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr >= vm->addr)
			break;
	}
	/** 20140329    
	 * 추가할 위치에 vm을 추가한다.
	 **/
	vm->next = *p;
	*p = vm;
	write_unlock(&vmlist_lock);
}

static void insert_vmalloc_vm(struct vm_struct *vm, struct vmap_area *va,
			      unsigned long flags, const void *caller)
{
	/** 20140329    
	 * vm_struct을 vmap_area 정보로 채우고, vmap_area가 vm_struct을 가리키게 한다.
	 **/
	setup_vmalloc_vm(vm, va, flags, caller);
	/** 20140329    
	 * vm_struct을 vmlist에 추가한다.
	 **/
	insert_vmalloc_vmlist(vm);
}
/** 20130323
 * ioremap에서 호출한 경우
 * flags : VM_IOREMAP
 *
 * return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
 *					-1, GFP_KERNEL, caller);
 *
 * 20140329    
 * vmalloc에서 호출한 경우
 * area = __get_vm_area_node(size, align, VM_ALLOC | VM_UNLIST,
 *				  start, end, node, gfp_mask, caller);
 *
 * vm_struct와 vmap_area를 할당받고 자료구조에 등록한다.
 * vmalloc에서 호출된 경우 vmlist에 등록하는 과정은 수행하지 않는다.
*/
static struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long flags, unsigned long start,
		unsigned long end, int node, gfp_t gfp_mask, const void *caller)
{
	struct vmap_area *va;
	struct vm_struct *area;

	/** 20140329    
	 * interrupt handler 내에서는 vm area를 받아올 수 없다.
	 **/
	BUG_ON(in_interrupt());
	/** 20140329    
	 * ioremap에서 호출된 경우 align을 IOREMAP ORDER 범위 사이로 조정
	 **/
	if (flags & VM_IOREMAP) {
		int bit = fls(size);
		/** 20130323
		* IOREMAP_MAX_ORDER = 24
		* 12 (4K) <= bit <= 24 (16M)
		*/
		if (bit > IOREMAP_MAX_ORDER)
			bit = IOREMAP_MAX_ORDER;
		else if (bit < PAGE_SHIFT)
			bit = PAGE_SHIFT;

		align = 1ul << bit;
	}

	/** 20140329    
	 * page align
	 **/
	size = PAGE_ALIGN(size);
	if (unlikely(!size))
		return NULL;

	/** 20140329    
	 * struct vm_struct를 kmalloc을 통해 할당
	 **/
	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!area))
		return NULL;

	/*
	 * We always allocate a guard page.
	 */
	/** 20140329    
	 * PAGE_SIZE만큼을 메모리 침범을 보호하기 위한 공간으로 더한다.
	 **/
	size += PAGE_SIZE;

	/** 20140329    
	 * vmap_area를 할당받고 자료구조에 등록시킨다.
	 **/
	va = alloc_vmap_area(size, align, start, end, node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(area);
		return NULL;
	}

	/*
	 * When this function is called from __vmalloc_node_range,
	 * we do not add vm_struct to vmlist here to avoid
	 * accessing uninitialized members of vm_struct such as
	 * pages and nr_pages fields. They will be set later.
	 * To distinguish it from others, we use a VM_UNLIST flag.
	 */
	/** 20140329    
	 * VM_UNLIST인 경우 vm을 setup만 한다.
	 * 그렇지 않다면 vmlist에 추가한다.
	 * 
	 * vmalloc에서 호출된 경우 flags에 VM_UNLIST가 추가된다.
	 **/
	if (flags & VM_UNLIST)
		setup_vmalloc_vm(area, va, flags, caller);
	else
		insert_vmalloc_vm(area, va, flags, caller);

	/** 20140329    
	 * 설정이 끝난 vm_struct를 리턴한다.
	 **/
	return area;
}

struct vm_struct *__get_vm_area(unsigned long size, unsigned long flags,
				unsigned long start, unsigned long end)
{
	return __get_vm_area_node(size, 1, flags, start, end, -1, GFP_KERNEL,
						__builtin_return_address(0));
}
EXPORT_SYMBOL_GPL(__get_vm_area);

struct vm_struct *__get_vm_area_caller(unsigned long size, unsigned long flags,
				       unsigned long start, unsigned long end,
				       const void *caller)
{
	return __get_vm_area_node(size, 1, flags, start, end, -1, GFP_KERNEL,
				  caller);
}

/**
 *	get_vm_area  -  reserve a contiguous kernel virtual area
 *	@size:		size of the area
 *	@flags:		%VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 *	Search an area of @size in the kernel virtual mapping area,
 *	and reserved it for out purposes.  Returns the area descriptor
 *	on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
				-1, GFP_KERNEL, __builtin_return_address(0));
}

/** 20140419    
 * VMALLOC_START ~ VMALLOC_END 사이에서 vm_struct와 vmap_area를 할당 받고 자료구조에 등록한다.
 *
 * mapping은 하지 않은 상태.
 **/
struct vm_struct *get_vm_area_caller(unsigned long size, unsigned long flags,
				const void *caller)
{
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
						-1, GFP_KERNEL, caller);
}

/**
 *	find_vm_area  -  find a continuous kernel virtual area
 *	@addr:		base address
 *
 *	Search for the kernel VM area starting at @addr, and return it.
 *	It is up to the caller to do all required locking to keep the returned
 *	pointer valid.
 */
struct vm_struct *find_vm_area(const void *addr)
{
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (va && va->flags & VM_VM_AREA)
		return va->vm;

	return NULL;
}

/**
 *	remove_vm_area  -  find and remove a continuous kernel virtual area
 *	@addr:		base address
 *
 *	Search for the kernel VM area starting at @addr, and remove it.
 *	This function returns the found VM area, but using it is NOT safe
 *	on SMP machines, except for its size or flags.
 */
struct vm_struct *remove_vm_area(const void *addr)
{
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (va && va->flags & VM_VM_AREA) {
		struct vm_struct *vm = va->vm;

		if (!(vm->flags & VM_UNLIST)) {
			struct vm_struct *tmp, **p;
			/*
			 * remove from list and disallow access to
			 * this vm_struct before unmap. (address range
			 * confliction is maintained by vmap.)
			 */
			write_lock(&vmlist_lock);
			for (p = &vmlist; (tmp = *p) != vm; p = &tmp->next)
				;
			*p = tmp->next;
			write_unlock(&vmlist_lock);
		}

		vmap_debug_free_range(va->va_start, va->va_end);
		free_unmap_vmap_area(va);
		vm->size -= PAGE_SIZE;

		return vm;
	}
	return NULL;
}

static void __vunmap(const void *addr, int deallocate_pages)
{
	struct vm_struct *area;

	if (!addr)
		return;

	if ((PAGE_SIZE-1) & (unsigned long)addr) {
		WARN(1, KERN_ERR "Trying to vfree() bad address (%p)\n", addr);
		return;
	}

	area = remove_vm_area(addr);
	if (unlikely(!area)) {
		WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}

	debug_check_no_locks_freed(addr, area->size);
	debug_check_no_obj_freed(addr, area->size);

	if (deallocate_pages) {
		int i;

		for (i = 0; i < area->nr_pages; i++) {
			struct page *page = area->pages[i];

			BUG_ON(!page);
			__free_page(page);
		}

		if (area->flags & VM_VPAGES)
			vfree(area->pages);
		else
			kfree(area->pages);
	}

	kfree(area);
	return;
}

/**
 *	vfree  -  release memory allocated by vmalloc()
 *	@addr:		memory base address
 *
 *	Free the virtually continuous memory area starting at @addr, as
 *	obtained from vmalloc(), vmalloc_32() or __vmalloc(). If @addr is
 *	NULL, no operation is performed.
 *
 *	Must not be called in interrupt context.
 */
void vfree(const void *addr)
{
	BUG_ON(in_interrupt());

	kmemleak_free(addr);

	__vunmap(addr, 1);
}
EXPORT_SYMBOL(vfree);

/**
 *	vunmap  -  release virtual mapping obtained by vmap()
 *	@addr:		memory base address
 *
 *	Free the virtually contiguous memory area starting at @addr,
 *	which was created from the page array passed to vmap().
 *
 *	Must not be called in interrupt context.
 */
void vunmap(const void *addr)
{
	BUG_ON(in_interrupt());
	might_sleep();
	__vunmap(addr, 0);
}
EXPORT_SYMBOL(vunmap);

/**
 *	vmap  -  map an array of pages into virtually contiguous space
 *	@pages:		array of page pointers
 *	@count:		number of pages to map
 *	@flags:		vm_area->flags
 *	@prot:		page protection for the mapping
 *
 *	Maps @count pages from @pages into contiguous kernel virtual
 *	space.
 */
void *vmap(struct page **pages, unsigned int count,
		unsigned long flags, pgprot_t prot)
{
	struct vm_struct *area;

	might_sleep();

	if (count > totalram_pages)
		return NULL;

	area = get_vm_area_caller((count << PAGE_SHIFT), flags,
					__builtin_return_address(0));
	if (!area)
		return NULL;

	if (map_vm_area(area, prot, &pages)) {
		vunmap(area->addr);
		return NULL;
	}

	return area->addr;
}
EXPORT_SYMBOL(vmap);

static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller);
/** 20140329    
 * 특정 node에서 memory를 할당(page 단위로 할당 받는다) 받아 가상 메모리 공간에
 * mapping 한다.
 **/
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, int node, const void *caller)
{
	const int order = 0;
	struct page **pages;
	unsigned int nr_pages, array_size, i;
	gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;

	/** 20140329    
	 * area의 size를 바탕으로 실제 요구되는 size에 해당하는 page의 수를 반환
	 **/
	nr_pages = (area->size - PAGE_SIZE) >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *));

	/** 20140329    
	 * vm_struct에 nr_pages를 설정한다.
	 **/
	area->nr_pages = nr_pages;
	/* Please note that the recursion is strictly bounded. */
	/** 20140329    
	 * struct page * 배열을 할당하기 위해 page크기 이상이 필요하다면
	 * vmalloc으로 할당.
	 *	(이 경우 flags에 array 역시 vmalloc으로 할당되었음을 나타내가 위해 VM_VPAGES를 추가)
	 * 그렇지 않다면 kmalloc으로 할당
	 **/
	if (array_size > PAGE_SIZE) {
		pages = __vmalloc_node(array_size, 1, nested_gfp|__GFP_HIGHMEM,
				PAGE_KERNEL, node, caller);
		area->flags |= VM_VPAGES;
	} else {
		pages = kmalloc_node(array_size, nested_gfp, node);
	}
	/** 20140329    
	 * '페이지 프레임을 나타내는 struct page *' 배열의 주소 저장
	 **/
	area->pages = pages;
	area->caller = caller;
	if (!area->pages) {
		remove_vm_area(area->addr);
		kfree(area);
		return NULL;
	}

	/** 20140329    
	 * nr_pages만큼 한 페이지씩 페이지프레임을 할당 받는다.
	 * pages에 저장한다.
	 **/
	for (i = 0; i < area->nr_pages; i++) {
		struct page *page;
		gfp_t tmp_mask = gfp_mask | __GFP_NOWARN;

		/** 20140329    
		 * node가 -1일 경우 특정 node를 지정하지 않고 전체 node에서 받아올 수 있다.
		 * 물리 메모리를 할당받는다.
		 **/
		if (node < 0)
			page = alloc_page(tmp_mask);
		else
			page = alloc_pages_node(node, tmp_mask, order);

		if (unlikely(!page)) {
			/* Successfully allocated i pages, free them in __vunmap() */
			area->nr_pages = i;
			goto fail;
		}
		area->pages[i] = page;
	}

	/** 20140329    
	 * alloc 받아온 pages를 vm_struct에 저장된 va 공간과 mapping 한다.
	 **/
	if (map_vm_area(area, prot, &pages))
		goto fail;
	return area->addr;

fail:
	warn_alloc_failed(gfp_mask, order,
			  "vmalloc: allocation failure, allocated %ld of %ld bytes\n",
			  (area->nr_pages*PAGE_SIZE), area->size);
	vfree(area->addr);
	return NULL;
}

/**
 *	__vmalloc_node_range  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	@align:		desired alignment
 *	@start:		vm area range start
 *	@end:		vm area range end
 *	@gfp_mask:	flags for the page level allocator
 *	@prot:		protection mask for the allocated pages
 *	@node:		node to use for allocation or -1
 *	@caller:	caller's return address
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator with @gfp_mask flags.  Map them into contiguous
 *	kernel virtual space, using a pagetable protection of @prot.
 */
/** 20140329    
 * start ~ end 사이에 해당하는 페이지를 할당 받아 가상 주소 공간에 mapping 하는 함수
 **/
void *__vmalloc_node_range(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, int node, const void *caller)
{
	struct vm_struct *area;
	void *addr;
	unsigned long real_size = size;

	/** 20140329    
	 * 요청 받은 크기를 page 단위로 align 한다.
	 **/
	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > totalram_pages)
		goto fail;

	/** 20140329    
	 * flags : VM_ALLOC | VM_UNLIST
	 * vmalloc에서 호출되는 경우 vmlist에 등록시키지 않는다.
	 *
	 * vm_struct와 vmap_area를 할당받아 vmap_area 정보로 vm_struct을 설정해 리턴한다.
	 **/
	area = __get_vm_area_node(size, align, VM_ALLOC | VM_UNLIST,
				  start, end, node, gfp_mask, caller);
	if (!area)
		goto fail;

	/** 20140329    
	 * vmalloc으로 실제 페이지를 할당 받아 매핑한다.
	 **/
	addr = __vmalloc_area_node(area, gfp_mask, prot, node, caller);
	if (!addr)
		return NULL;

	/*
	 * In this function, newly allocated vm_struct is not added
	 * to vmlist at __get_vm_area_node(). so, it is added here.
	 */
	/** 20140329    
	 * vmalloc으로 받아온 vm_struct을 vmlist에 등록한다.
	 **/
	insert_vmalloc_vmlist(area);

	/*
	 * A ref_count = 3 is needed because the vm_struct and vmap_area
	 * structures allocated in the __get_vm_area_node() function contain
	 * references to the virtual address of the vmalloc'ed block.
	 */
	kmemleak_alloc(addr, real_size, 3, gfp_mask);

	return addr;

fail:
	warn_alloc_failed(gfp_mask, 0,
			  "vmalloc: allocation failure: %lu bytes\n",
			  real_size);
	return NULL;
}

/**
 *	__vmalloc_node  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	@align:		desired alignment
 *	@gfp_mask:	flags for the page level allocator
 *	@prot:		protection mask for the allocated pages
 *	@node:		node to use for allocation or -1
 *	@caller:	caller's return address
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator with @gfp_mask flags.  Map them into contiguous
 *	kernel virtual space, using a pagetable protection of @prot.
 */
static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller)
{
	return __vmalloc_node_range(size, align, VMALLOC_START, VMALLOC_END,
				gfp_mask, prot, node, caller);
}

void *__vmalloc(unsigned long size, gfp_t gfp_mask, pgprot_t prot)
{
	return __vmalloc_node(size, 1, gfp_mask, prot, -1,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc);

static inline void *__vmalloc_node_flags(unsigned long size,
					int node, gfp_t flags)
{
	return __vmalloc_node(size, 1, flags, PAGE_KERNEL,
					node, __builtin_return_address(0));
}

/**
 *	vmalloc  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, -1, GFP_KERNEL | __GFP_HIGHMEM);
}
EXPORT_SYMBOL(vmalloc);

/**
 *	vzalloc - allocate virtually contiguous memory with zero fill
 *	@size:	allocation size
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *	The memory allocated is set to zero.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vzalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, -1,
				GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc);

/**
 * vmalloc_user - allocate zeroed virtually contiguous memory for userspace
 * @size: allocation size
 *
 * The resulting memory area is zeroed so it can be mapped to userspace
 * without leaking data.
 */
void *vmalloc_user(unsigned long size)
{
	struct vm_struct *area;
	void *ret;

	ret = __vmalloc_node(size, SHMLBA,
			     GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO,
			     PAGE_KERNEL, -1, __builtin_return_address(0));
	if (ret) {
		area = find_vm_area(ret);
		area->flags |= VM_USERMAP;
	}
	return ret;
}
EXPORT_SYMBOL(vmalloc_user);

/**
 *	vmalloc_node  -  allocate memory on a specific node
 *	@size:		allocation size
 *	@node:		numa node
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc_node(unsigned long size, int node)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL,
					node, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_node);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc_node() instead.
 */
void *vzalloc_node(unsigned long size, int node)
{
	return __vmalloc_node_flags(size, node,
			 GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc_node);

#ifndef PAGE_KERNEL_EXEC
# define PAGE_KERNEL_EXEC PAGE_KERNEL
#endif

/**
 *	vmalloc_exec  -  allocate virtually contiguous, executable memory
 *	@size:		allocation size
 *
 *	Kernel-internal function to allocate enough pages to cover @size
 *	the page level allocator and map them into contiguous and
 *	executable kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */

void *vmalloc_exec(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL_EXEC,
			      -1, __builtin_return_address(0));
}

#if defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA32)
#define GFP_VMALLOC32 GFP_DMA32 | GFP_KERNEL
#elif defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA)
#define GFP_VMALLOC32 GFP_DMA | GFP_KERNEL
#else
#define GFP_VMALLOC32 GFP_KERNEL
#endif

/**
 *	vmalloc_32  -  allocate virtually contiguous memory (32bit addressable)
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into contiguous kernel virtual space.
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_VMALLOC32, PAGE_KERNEL,
			      -1, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 *	@size:		allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 */
void *vmalloc_32_user(unsigned long size)
{
	struct vm_struct *area;
	void *ret;

	ret = __vmalloc_node(size, 1, GFP_VMALLOC32 | __GFP_ZERO, PAGE_KERNEL,
			     -1, __builtin_return_address(0));
	if (ret) {
		area = find_vm_area(ret);
		area->flags |= VM_USERMAP;
	}
	return ret;
}
EXPORT_SYMBOL(vmalloc_32_user);

/*
 * small helper routine , copy contents to buf from addr.
 * If the page is not present, fill zero.
 */

static int aligned_vread(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = (unsigned long)addr & ~PAGE_MASK;
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(buf, map + offset, length);
			kunmap_atomic(map);
		} else
			memset(buf, 0, length);

		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

static int aligned_vwrite(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = (unsigned long)addr & ~PAGE_MASK;
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(map + offset, buf, length);
			kunmap_atomic(map);
		}
		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

/**
 *	vread() -  read vmalloc area in a safe way.
 *	@buf:		buffer for reading data
 *	@addr:		vm address.
 *	@count:		number of bytes to be read.
 *
 *	Returns # of bytes which addr and buf should be increased.
 *	(same number to @count). Returns 0 if [addr...addr+count) doesn't
 *	includes any intersect with alive vmalloc area.
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	copy data from that area to a given buffer. If the given memory range
 *	of [addr...addr+count) includes some valid address, data is copied to
 *	proper area of @buf. If there are memory holes, they'll be zero-filled.
 *	IOREMAP area is treated as memory hole and no copy is done.
 *
 *	If [addr...addr+count) doesn't includes any intersects with alive
 *	vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 *	Note: In usual ops, vread() is never necessary because the caller
 *	should know vmalloc() area is valid and can use memcpy().
 *	This is for routines which have to access vmalloc area without
 *	any informaion, as /dev/kmem.
 *
 */

long vread(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long buflen = count;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; count && tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		if (n > count)
			n = count;
		if (!(tmp->flags & VM_IOREMAP))
			aligned_vread(buf, addr, n);
		else /* IOREMAP area is treated as memory hole */
			memset(buf, 0, n);
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	read_unlock(&vmlist_lock);

	if (buf == buf_start)
		return 0;
	/* zero-fill memory holes */
	if (buf != buf_start + buflen)
		memset(buf, 0, buflen - (buf - buf_start));

	return buflen;
}

/**
 *	vwrite() -  write vmalloc area in a safe way.
 *	@buf:		buffer for source data
 *	@addr:		vm address.
 *	@count:		number of bytes to be read.
 *
 *	Returns # of bytes which addr and buf should be incresed.
 *	(same number to @count).
 *	If [addr...addr+count) doesn't includes any intersect with valid
 *	vmalloc area, returns 0.
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	copy data from a buffer to the given addr. If specified range of
 *	[addr...addr+count) includes some valid address, data is copied from
 *	proper area of @buf. If there are memory holes, no copy to hole.
 *	IOREMAP area is treated as memory hole and no copy is done.
 *
 *	If [addr...addr+count) doesn't includes any intersects with alive
 *	vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 *	Note: In usual ops, vwrite() is never necessary because the caller
 *	should know vmalloc() area is valid and can use memcpy().
 *	This is for routines which have to access vmalloc area without
 *	any informaion, as /dev/kmem.
 */

long vwrite(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr;
	unsigned long n, buflen;
	int copied = 0;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;
	buflen = count;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; count && tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		if (n > count)
			n = count;
		if (!(tmp->flags & VM_IOREMAP)) {
			aligned_vwrite(buf, addr, n);
			copied++;
		}
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	read_unlock(&vmlist_lock);
	if (!copied)
		return 0;
	return buflen;
}

/**
 *	remap_vmalloc_range  -  map vmalloc pages to userspace
 *	@vma:		vma to cover (map full range of vma)
 *	@addr:		vmalloc memory
 *	@pgoff:		number of pages into addr before first page to map
 *
 *	Returns:	0 for success, -Exxx on failure
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	that it is big enough to cover the vma. Will return failure if
 *	that criteria isn't met.
 *
 *	Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
						unsigned long pgoff)
{
	struct vm_struct *area;
	unsigned long uaddr = vma->vm_start;
	unsigned long usize = vma->vm_end - vma->vm_start;

	if ((PAGE_SIZE-1) & (unsigned long)addr)
		return -EINVAL;

	area = find_vm_area(addr);
	if (!area)
		return -EINVAL;

	if (!(area->flags & VM_USERMAP))
		return -EINVAL;

	if (usize + (pgoff << PAGE_SHIFT) > area->size - PAGE_SIZE)
		return -EINVAL;

	addr += pgoff << PAGE_SHIFT;
	do {
		struct page *page = vmalloc_to_page(addr);
		int ret;

		ret = vm_insert_page(vma, uaddr, page);
		if (ret)
			return ret;

		uaddr += PAGE_SIZE;
		addr += PAGE_SIZE;
		usize -= PAGE_SIZE;
	} while (usize > 0);

	/* Prevent "things" like memory migration? VM_flags need a cleanup... */
	vma->vm_flags |= VM_RESERVED;

	return 0;
}
EXPORT_SYMBOL(remap_vmalloc_range);

/*
 * Implement a stub for vmalloc_sync_all() if the architecture chose not to
 * have one.
 */
void  __attribute__((weak)) vmalloc_sync_all(void)
{
}


static int f(pte_t *pte, pgtable_t table, unsigned long addr, void *data)
{
	pte_t ***p = data;

	if (p) {
		*(*p) = pte;
		(*p)++;
	}
	return 0;
}

/**
 *	alloc_vm_area - allocate a range of kernel address space
 *	@size:		size of the area
 *	@ptes:		returns the PTEs for the address space
 *
 *	Returns:	NULL on failure, vm_struct on success
 *
 *	This function reserves a range of kernel address space, and
 *	allocates pagetables to map that range.  No actual mappings
 *	are created.
 *
 *	If @ptes is non-NULL, pointers to the PTEs (in init_mm)
 *	allocated for the VM area are returned.
 */
struct vm_struct *alloc_vm_area(size_t size, pte_t **ptes)
{
	struct vm_struct *area;

	area = get_vm_area_caller(size, VM_IOREMAP,
				__builtin_return_address(0));
	if (area == NULL)
		return NULL;

	/*
	 * This ensures that page tables are constructed for this region
	 * of kernel virtual address space and mapped into init_mm.
	 */
	if (apply_to_page_range(&init_mm, (unsigned long)area->addr,
				size, f, ptes ? &ptes : NULL)) {
		free_vm_area(area);
		return NULL;
	}

	return area;
}
EXPORT_SYMBOL_GPL(alloc_vm_area);

void free_vm_area(struct vm_struct *area)
{
	struct vm_struct *ret;
	ret = remove_vm_area(area->addr);
	BUG_ON(ret != area);
	kfree(area);
}
EXPORT_SYMBOL_GPL(free_vm_area);

#ifdef CONFIG_SMP
static struct vmap_area *node_to_va(struct rb_node *n)
{
	return n ? rb_entry(n, struct vmap_area, rb_node) : NULL;
}

/**
 * pvm_find_next_prev - find the next and prev vmap_area surrounding @end
 * @end: target address
 * @pnext: out arg for the next vmap_area
 * @pprev: out arg for the previous vmap_area
 *
 * Returns: %true if either or both of next and prev are found,
 *	    %false if no vmap_area exists
 *
 * Find vmap_areas end addresses of which enclose @end.  ie. if not
 * NULL, *pnext->va_end > @end and *pprev->va_end <= @end.
 */
static bool pvm_find_next_prev(unsigned long end,
			       struct vmap_area **pnext,
			       struct vmap_area **pprev)
{
	struct rb_node *n = vmap_area_root.rb_node;
	struct vmap_area *va = NULL;

	while (n) {
		va = rb_entry(n, struct vmap_area, rb_node);
		if (end < va->va_end)
			n = n->rb_left;
		else if (end > va->va_end)
			n = n->rb_right;
		else
			break;
	}

	if (!va)
		return false;

	if (va->va_end > end) {
		*pnext = va;
		*pprev = node_to_va(rb_prev(&(*pnext)->rb_node));
	} else {
		*pprev = va;
		*pnext = node_to_va(rb_next(&(*pprev)->rb_node));
	}
	return true;
}

/**
 * pvm_determine_end - find the highest aligned address between two vmap_areas
 * @pnext: in/out arg for the next vmap_area
 * @pprev: in/out arg for the previous vmap_area
 * @align: alignment
 *
 * Returns: determined end address
 *
 * Find the highest aligned address between *@pnext and *@pprev below
 * VMALLOC_END.  *@pnext and *@pprev are adjusted so that the aligned
 * down address is between the end addresses of the two vmap_areas.
 *
 * Please note that the address returned by this function may fall
 * inside *@pnext vmap_area.  The caller is responsible for checking
 * that.
 */
static unsigned long pvm_determine_end(struct vmap_area **pnext,
				       struct vmap_area **pprev,
				       unsigned long align)
{
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	unsigned long addr;

	if (*pnext)
		addr = min((*pnext)->va_start & ~(align - 1), vmalloc_end);
	else
		addr = vmalloc_end;

	while (*pprev && (*pprev)->va_end > addr) {
		*pnext = *pprev;
		*pprev = node_to_va(rb_prev(&(*pnext)->rb_node));
	}

	return addr;
}

/**
 * pcpu_get_vm_areas - allocate vmalloc areas for percpu allocator
 * @offsets: array containing offset of each area
 * @sizes: array containing size of each area
 * @nr_vms: the number of areas to allocate
 * @align: alignment, all entries in @offsets and @sizes must be aligned to this
 *
 * Returns: kmalloc'd vm_struct pointer array pointing to allocated
 *	    vm_structs on success, %NULL on failure
 *
 * Percpu allocator wants to use congruent vm areas so that it can
 * maintain the offsets among percpu areas.  This function allocates
 * congruent vmalloc areas for it with GFP_KERNEL.  These areas tend to
 * be scattered pretty far, distance between two areas easily going up
 * to gigabytes.  To avoid interacting with regular vmallocs, these
 * areas are allocated from top.
 *
 * Despite its complicated look, this allocator is rather simple.  It
 * does everything top-down and scans areas from the end looking for
 * matching slot.  While scanning, if any of the areas overlaps with
 * existing vmap_area, the base address is pulled down to fit the
 * area.  Scanning is repeated till all the areas fit and then all
 * necessary data structres are inserted and the result is returned.
 */
/** 20140322    
 * 호출 예
 * pcpu_get_vm_areas(pcpu_group_offsets, pcpu_group_sizes, pcpu_nr_groups, pcpu_atom_size)
 **/
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align)
{
	const unsigned long vmalloc_start = ALIGN(VMALLOC_START, align);
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	struct vmap_area **vas, *prev, *next;
	struct vm_struct **vms;
	int area, area2, last_area, term_area;
	unsigned long base, start, end, last_end;
	bool purged = false;

	/* verify parameters and allocate data structures */
	BUG_ON(align & ~PAGE_MASK || !is_power_of_2(align));
	for (last_area = 0, area = 0; area < nr_vms; area++) {
		start = offsets[area];
		end = start + sizes[area];

		/* is everything aligned properly? */
		BUG_ON(!IS_ALIGNED(offsets[area], align));
		BUG_ON(!IS_ALIGNED(sizes[area], align));

		/* detect the area with the highest address */
		if (start > offsets[last_area])
			last_area = area;

		for (area2 = 0; area2 < nr_vms; area2++) {
			unsigned long start2 = offsets[area2];
			unsigned long end2 = start2 + sizes[area2];

			if (area2 == area)
				continue;

			BUG_ON(start2 >= start && start2 < end);
			BUG_ON(end2 <= end && end2 > start);
		}
	}
	last_end = offsets[last_area] + sizes[last_area];

	if (vmalloc_end - vmalloc_start < last_end) {
		WARN_ON(true);
		return NULL;
	}

	vms = kcalloc(nr_vms, sizeof(vms[0]), GFP_KERNEL);
	vas = kcalloc(nr_vms, sizeof(vas[0]), GFP_KERNEL);
	if (!vas || !vms)
		goto err_free2;

	for (area = 0; area < nr_vms; area++) {
		vas[area] = kzalloc(sizeof(struct vmap_area), GFP_KERNEL);
		vms[area] = kzalloc(sizeof(struct vm_struct), GFP_KERNEL);
		if (!vas[area] || !vms[area])
			goto err_free;
	}
retry:
	spin_lock(&vmap_area_lock);

	/* start scanning - we scan from the top, begin with the last area */
	area = term_area = last_area;
	start = offsets[area];
	end = start + sizes[area];

	if (!pvm_find_next_prev(vmap_area_pcpu_hole, &next, &prev)) {
		base = vmalloc_end - last_end;
		goto found;
	}
	base = pvm_determine_end(&next, &prev, align) - end;

	while (true) {
		BUG_ON(next && next->va_end <= base + end);
		BUG_ON(prev && prev->va_end > base + end);

		/*
		 * base might have underflowed, add last_end before
		 * comparing.
		 */
		if (base + last_end < vmalloc_start + last_end) {
			spin_unlock(&vmap_area_lock);
			if (!purged) {
				purge_vmap_area_lazy();
				purged = true;
				goto retry;
			}
			goto err_free;
		}

		/*
		 * If next overlaps, move base downwards so that it's
		 * right below next and then recheck.
		 */
		if (next && next->va_start < base + end) {
			base = pvm_determine_end(&next, &prev, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * If prev overlaps, shift down next and prev and move
		 * base so that it's right below new next and then
		 * recheck.
		 */
		if (prev && prev->va_end > base + start)  {
			next = prev;
			prev = node_to_va(rb_prev(&next->rb_node));
			base = pvm_determine_end(&next, &prev, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * This area fits, move on to the previous one.  If
		 * the previous one is the terminal one, we're done.
		 */
		area = (area + nr_vms - 1) % nr_vms;
		if (area == term_area)
			break;
		start = offsets[area];
		end = start + sizes[area];
		pvm_find_next_prev(base + end, &next, &prev);
	}
found:
	/* we've found a fitting base, insert all va's */
	for (area = 0; area < nr_vms; area++) {
		struct vmap_area *va = vas[area];

		va->va_start = base + offsets[area];
		va->va_end = va->va_start + sizes[area];
		__insert_vmap_area(va);
	}

	vmap_area_pcpu_hole = base + offsets[last_area];

	spin_unlock(&vmap_area_lock);

	/* insert all vm's */
	for (area = 0; area < nr_vms; area++)
		insert_vmalloc_vm(vms[area], vas[area], VM_ALLOC,
				  pcpu_get_vm_areas);

	kfree(vas);
	return vms;

err_free:
	for (area = 0; area < nr_vms; area++) {
		kfree(vas[area]);
		kfree(vms[area]);
	}
err_free2:
	kfree(vas);
	kfree(vms);
	return NULL;
}

/**
 * pcpu_free_vm_areas - free vmalloc areas for percpu allocator
 * @vms: vm_struct pointer array returned by pcpu_get_vm_areas()
 * @nr_vms: the number of allocated areas
 *
 * Free vm_structs and the array allocated by pcpu_get_vm_areas().
 */
void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
	int i;

	for (i = 0; i < nr_vms; i++)
		free_vm_area(vms[i]);
	kfree(vms);
}
#endif	/* CONFIG_SMP */

#ifdef CONFIG_PROC_FS
static void *s_start(struct seq_file *m, loff_t *pos)
	__acquires(&vmlist_lock)
{
	loff_t n = *pos;
	struct vm_struct *v;

	read_lock(&vmlist_lock);
	v = vmlist;
	while (n > 0 && v) {
		n--;
		v = v->next;
	}
	if (!n)
		return v;

	return NULL;

}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct vm_struct *v = p;

	++*pos;
	return v->next;
}

static void s_stop(struct seq_file *m, void *p)
	__releases(&vmlist_lock)
{
	read_unlock(&vmlist_lock);
}

static void show_numa_info(struct seq_file *m, struct vm_struct *v)
{
	if (NUMA_BUILD) {
		unsigned int nr, *counters = m->private;

		if (!counters)
			return;

		memset(counters, 0, nr_node_ids * sizeof(unsigned int));

		for (nr = 0; nr < v->nr_pages; nr++)
			counters[page_to_nid(v->pages[nr])]++;

		for_each_node_state(nr, N_HIGH_MEMORY)
			if (counters[nr])
				seq_printf(m, " N%u=%u", nr, counters[nr]);
	}
}

static int s_show(struct seq_file *m, void *p)
{
	struct vm_struct *v = p;

	seq_printf(m, "0x%p-0x%p %7ld",
		v->addr, v->addr + v->size, v->size);

	if (v->caller)
		seq_printf(m, " %pS", v->caller);

	if (v->nr_pages)
		seq_printf(m, " pages=%d", v->nr_pages);

	if (v->phys_addr)
		seq_printf(m, " phys=%llx", (unsigned long long)v->phys_addr);

	if (v->flags & VM_IOREMAP)
		seq_printf(m, " ioremap");

	if (v->flags & VM_ALLOC)
		seq_printf(m, " vmalloc");

	if (v->flags & VM_MAP)
		seq_printf(m, " vmap");

	if (v->flags & VM_USERMAP)
		seq_printf(m, " user");

	if (v->flags & VM_VPAGES)
		seq_printf(m, " vpages");

	show_numa_info(m, v);
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations vmalloc_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

static int vmalloc_open(struct inode *inode, struct file *file)
{
	unsigned int *ptr = NULL;
	int ret;

	if (NUMA_BUILD) {
		ptr = kmalloc(nr_node_ids * sizeof(unsigned int), GFP_KERNEL);
		if (ptr == NULL)
			return -ENOMEM;
	}
	ret = seq_open(file, &vmalloc_op);
	if (!ret) {
		struct seq_file *m = file->private_data;
		m->private = ptr;
	} else
		kfree(ptr);
	return ret;
}

static const struct file_operations proc_vmalloc_operations = {
	.open		= vmalloc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

static int __init proc_vmalloc_init(void)
{
	proc_create("vmallocinfo", S_IRUSR, NULL, &proc_vmalloc_operations);
	return 0;
}
module_init(proc_vmalloc_init);
#endif

