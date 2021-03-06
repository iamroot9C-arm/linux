#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/auxvec.h>
#include <linux/types.h>
#include <linux/threads.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/prio_tree.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/page-debug-flags.h>
#include <linux/uprobes.h>
#include <asm/page.h>
#include <asm/mmu.h>

#ifndef AT_VECTOR_SIZE_ARCH
#define AT_VECTOR_SIZE_ARCH 0
#endif
#define AT_VECTOR_SIZE (2*(AT_VECTOR_SIZE_ARCH + AT_VECTOR_SIZE_BASE + 1))

struct address_space;

/** 20140531
 * USE_SPLIT_PTLOCKS은 참.
 **/
#define USE_SPLIT_PTLOCKS	(NR_CPUS >= CONFIG_SPLIT_PTLOCK_CPUS)

/*
 * Each physical page in the system has a struct page associated with
 * it to keep track of whatever it is we are using the page for at the
 * moment. Note that we have no way to track which tasks are using
 * a page, though if it is a pagecache page, rmap structures can tell us
 * who is mapping it.
 *
 * The objects in struct page are organized in double word blocks in
 * order to allows us to use atomic double word operations on portions
 * of struct page. That is currently only used by slub but the arrangement
 * allows the use of atomic double word operations on the flags/mapping
 * and lru list pointers also.
 */
struct page {
	/* First double word block */
	/** 20130831
	 * page-flags.h에 flags의 enum type 선언.
	 *
	 * memmap_init_zone 에서
	 * set_page_section으로 flags에 section 정보를 기록,
	 * set_page_zone으로 flags에 zone 정보를 기록,
	 * set_page_node으로 flags에 node id를 기록
	 *
	 * 20130907
	 * memmap_init_zone 에서 flags에 대해 PG_reserved 설정
	 **/
	unsigned long flags;		/* Atomic flags, some possibly
					 * updated asynchronously */
	/** 20140531
	 * anonymous page가 user VMA에 매핑되었을 때,
	 * anon_vma 구조체 주소와 PAGE_MAPPING_ANON 비트로 구성된 값이 저장된다.
	 **/
	struct address_space *mapping;	/* If low bit clear, points to
					 * inode address_space, or NULL.
					 * If page mapped as anonymous
					 * memory, low bit is set, and
					 * it points to anon_vma object:
					 * see PAGE_MAPPING_ANON below.
					 */
	/* Second double word */
	struct {
		union {
			/** 20140308
			 * percpu으로 사용될 경우 pcpu_set_page_chunk에서 chunk 주소를 지정
			 **/
			pgoff_t index;		/* Our offset within mapping. */
			/** 20140308
			 * slub/slob으로 사용될 경우 new_slab에서 첫번째 free object를 지정
			 **/
			void *freelist;		/* slub/slob first free object */
			/** 20140517
			 * ALLOC_NO_WATERMARKS가 설정되어 __alloc_pages_high_priority()로 받은 경우 설정
			 **/
			bool pfmemalloc;	/* If set by the page allocator,
						 * ALLOC_NO_WATERMARKS was set
						 * and the low watermark was not
						 * met implying that the system
						 * is under some pressure. The
						 * caller should try ensure
						 * this page is only used to
						 * free other pages.
						 */
		};

		union {
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
	defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
			/* Used for cmpxchg_double in slub */
			unsigned long counters;
#else
			/*
			 * Keep _count separate from slub cmpxchg_double data.
			 * As the rest of the double word is protected by
			 * slab_lock but _count is not.
			 */
			unsigned counters;
#endif

			struct {

				union {
					/*
					 * Count of ptes mapped in
					 * mms, to show when page is
					 * mapped & limit reverse map
					 * searches.
					 *
					 * Used also for tail pages
					 * refcounting instead of
					 * _count. Tail pages cannot
					 * be mapped and keeping the
					 * tail page _count zero at
					 * all times guarantees
					 * get_page_unless_zero() will
					 * never succeed on tail
					 * pages.
					 */
					/** 20140104
					 * page tables에 mapping될 때마다 1씩 증가 (공유 count).
					 * reset value -1.
					 *
					 * 아래 함수에서 increment.
					 * page_add_new_anon_rmap,
					 * page_add_file_rmap
					 * [참고] http://studyfoss.egloos.com/5512112
					 *
					 * buddy에 free상태로 존재할 때는
					 * PAGE_BUDDY_MAPCOUNT_VALUE 값을 가진다.
					 **/
					atomic_t _mapcount;

					struct { /* SLUB */
						unsigned inuse:16;
						/** 20140215
						 * allocate_slab함수에서 설정
						 **/
						/** 20140222
						 * object의 갯수를 나타냄
						 **/
						unsigned objects:15;
						unsigned frozen:1;
					};
					int units;	/* SLOB */
				};
				/** 20140607
				 * usage count.
				 *
				 * 이 페이지가 reference된 횟수를 나타낸다.
				 * get_pageXXX에 의해 증가된다.
				 * put_pageXXX에 의해 감소된다.
				 * page_count에 의해 현재값이 리턴된다.
				 **/
				atomic_t _count;		/* Usage count, see below. */
			};
		};
	};

	/* Third double word block */
	union {
		/** 20140322
		 * buddy에서 사용할 때 : free_area[order]의 free_list에 대한 list head.
		 * slub에서 사용할 때  : partial slab에 대한 list head.
		 * zone에서 사용할 때  : zone lruvec의 lru[NR_LRU_LISTS]에 대한 list head.
		 *
		 * compound page인 경우, 두번째 page의 .lru.next에 destructor를 등록시킨다.
		 **/
		struct list_head lru;	/* Pageout list, eg. active_list
					 * protected by zone->lru_lock !
					 */
		struct {		/* slub per cpu partial pages */
			struct page *next;	/* Next partial slab */
#ifdef CONFIG_64BIT
			int pages;	/* Nr of partial slabs left */
			int pobjects;	/* Approximate # of objects */
#else
			short int pages;
			short int pobjects;
#endif
		};

		struct list_head list;	/* slobs list of pages */
		struct {		/* slab fields */
			struct kmem_cache *slab_cache;
			struct slab *slab_page;
		};
	};

	/* Remainder is not double word aligned */
	union {
		/** 20140531
		 * page flags에 따라 용도가 달라진다.
		 * private이 설정된 경우, block device의 buffer_heads 포인터가 저장.
		 * swapcache가 설정된 경우, swap entry의 value가 들어간다.
		 * buddy에서 사용될 경우, buddy의 order가 들어간다.
		 **/
		unsigned long private;		/* Mapping-private opaque data:
					 	 * usually used for buffer_heads
						 * if PagePrivate set; used for
						 * swp_entry_t if PageSwapCache;
						 * indicates order in the buddy
						 * system if PG_buddy is set.
						 */
#if USE_SPLIT_PTLOCKS
		/** 20140531
		 * SPLIT PTLOCK을 사용하므로 spinlock을 포함함.
		 **/
		spinlock_t ptl;
#endif
		/** 20140322
		 * page가 SLUB에서 사용될 때 struct kmem_cache를 가리킨다.
		 **/
		struct kmem_cache *slab;	/* SLUB: Pointer to slab */
		struct page *first_page;	/* Compound tail pages */
	};

	/*
	 * On machines where all RAM is mapped into kernel address space,
	 * we can simply calculate the virtual address. On machines with
	 * highmem some memory is mapped into kernel virtual memory
	 * dynamically, so we need a place to store that address.
	 * Note that this field could be 16 bits on x86 ... ;)
	 *
	 * Architectures with slow multiplication can define
	 * WANT_PAGE_VIRTUAL in asm/page.h
	 */
#if defined(WANT_PAGE_VIRTUAL)
	void *virtual;			/* Kernel virtual address (NULL if
					   not kmapped, ie. highmem) */
#endif /* WANT_PAGE_VIRTUAL */
#ifdef CONFIG_WANT_PAGE_DEBUG_FLAGS
	unsigned long debug_flags;	/* Use atomic bitops on this */
#endif

#ifdef CONFIG_KMEMCHECK
	/*
	 * kmemcheck wants to track the status of each byte in a page; this
	 * is a pointer to such a status block. NULL if not tracked.
	 */
	void *shadow;
#endif
}
/*
 * The struct page can be forced to be double word aligned so that atomic ops
 * on double words work. The SLUB allocator can make use of such a feature.
 */
#ifdef CONFIG_HAVE_ALIGNED_STRUCT_PAGE
	__aligned(2 * sizeof(unsigned long))
#endif
;

struct page_frag {
	struct page *page;
#if (BITS_PER_LONG > 32) || (PAGE_SIZE >= 65536)
	__u32 offset;
	__u32 size;
#else
	__u16 offset;
	__u16 size;
#endif
};

typedef unsigned long __nocast vm_flags_t;

/*
 * A region containing a mapping of a non-memory backed file under NOMMU
 * conditions.  These are held in a global tree and are pinned by the VMAs that
 * map parts of them.
 */
struct vm_region {
	struct rb_node	vm_rb;		/* link in global region tree */
	vm_flags_t	vm_flags;	/* VMA vm_flags */
	unsigned long	vm_start;	/* start address of region */
	unsigned long	vm_end;		/* region initialised to here */
	unsigned long	vm_top;		/* region allocated to here */
	unsigned long	vm_pgoff;	/* the offset in vm_file corresponding to vm_start */
	struct file	*vm_file;	/* the backing file or NULL */

	int		vm_usage;	/* region usage count (access under nommu_region_sem) */
	bool		vm_icache_flushed : 1; /* true if the icache has been flushed for
						* this region */
};

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
/** 20140531
 * task의 VM영역 하나에 대한 자료구조.
 *
 * task의 VM-area당 하나의 구조체로 영역에 대한 디스크립션.
 * (속성이 다르거나 영역이 분리되어 있을 때마다 생기는 각 영역들)
 *
 * cat /proc/<PID>/maps 로 각 vm_area_struct 정보 확인 가능.
 **/
struct vm_area_struct {
	/** 20160416
	 * 이 vma가 속한 mm_struct 연결.
	 **/
	struct mm_struct * vm_mm;	/* The address space we belong to. */
	/** 20160416
	 * vm영역의 시작/끝 주소.
	 **/
	unsigned long vm_start;		/* Our start address within vm_mm. */
	unsigned long vm_end;		/* The first byte after our end address
					   within vm_mm. */

	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next, *vm_prev;

	pgprot_t vm_page_prot;		/* Access permissions of this VMA. */
	unsigned long vm_flags;		/* Flags, see mm.h. */

	/** 20160416
	 * mm_struct의 RB Tree에 연결되는 노드.
	 **/
	struct rb_node vm_rb;

	/*
	 * For areas with an address space and backing store,
	 * linkage into the address_space->i_mmap prio tree, or
	 * linkage to the list of like vmas hanging off its node, or
	 * linkage of vma in the address_space->i_mmap_nonlinear list.
	 */
	union {
		struct {
			struct list_head list;
			void *parent;	/* aligns with prio_tree_node parent */
			struct vm_area_struct *head;
		} vm_set;

		struct raw_prio_tree_node prio_tree_node;
	} shared;

	/*
	 * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
	 * list, after a COW of one of the file pages.	A MAP_SHARED vma
	 * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
	 * or brk vma (with NULL file) can only be in an anon_vma list.
	 */
	/** 20160423
	 * avc (anon_vma_chain)에 연결되는 list entry.
	 **/
	struct list_head anon_vma_chain; /* Serialized by mmap_sem &
					  * page_table_lock */
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */

	/* Function pointers to deal with this struct. */
	const struct vm_operations_struct *vm_ops;

	/* Information about our backing store: */
	unsigned long vm_pgoff;		/* Offset (within vm_file) in PAGE_SIZE
					   units, *not* PAGE_CACHE_SIZE */
	/** 20160430
	 * map 시킨 file 구조체
	 **/
	struct file * vm_file;		/* File we map to (can be NULL). */
	void * vm_private_data;		/* was vm_pte (shared mem) */

#ifndef CONFIG_MMU
	struct vm_region *vm_region;	/* NOMMU mapping region */
#endif
#ifdef CONFIG_NUMA
	struct mempolicy *vm_policy;	/* NUMA policy for the VMA */
#endif
};

struct core_thread {
	struct task_struct *task;
	struct core_thread *next;
};

struct core_state {
	atomic_t nr_threads;
	struct core_thread dumper;
	struct completion startup;
};

/** 20140531
 * Task의 RSS를 file/anon/swap 별로 counting.
 **/
enum {
	MM_FILEPAGES,
	MM_ANONPAGES,
	MM_SWAPENTS,
	NR_MM_COUNTERS
};

#if USE_SPLIT_PTLOCKS && defined(CONFIG_MMU)
/** 20140531
 * SPLIT_RSS_COUNTING 정의됨
 **/
#define SPLIT_RSS_COUNTING
/* per-thread cached information, */
/** 20140531
 * task의 rss를 page 종류별로 counting.
 *
 * RSS stands for "Resident Set Size."
 * It explains how many of the allocated blocks owned by the task currently reside in RAM
 * 프로세스와 관련된 물리적 페이지(physical pages) 수.
 **/
struct task_rss_stat {
	int events;	/* for synchronization threshold */
	int count[NR_MM_COUNTERS];
};
#endif /* USE_SPLIT_PTLOCKS */

struct mm_rss_stat {
	atomic_long_t count[NR_MM_COUNTERS];
};

/** 20160416
 * task의 memory management를 위한 구조체.
 *
 * struct task_struct		; process의 경우 mm으로 가리킴
 *	struct mm_struct	; memory descriptor. vm_area_struct를 RB로 관리
 *		struct vm_area_struct	: 같은 속성인 하나의 영역에 대한 디스크립터
 **/
struct mm_struct {
	/** 20160416
	 * struct vm_area_struct들을 리스트로 관리하기 위한 포인터.
	 **/
	struct vm_area_struct * mmap;		/* list of VMAs */
	/** 20160416
	 * struct vm_area_struct 들을 RB Tree로 관리하기 위한 root.
	 **/
	struct rb_root mm_rb;
	/** 20160528
	 * find_vma로 찾은 마지막 vm_area_struct. 이름 그대로 접근 캐시
	 **/
	struct vm_area_struct * mmap_cache;	/* last find_vma result */
#ifdef CONFIG_MMU
	unsigned long (*get_unmapped_area) (struct file *filp,
				unsigned long addr, unsigned long len,
				unsigned long pgoff, unsigned long flags);
	void (*unmap_area) (struct mm_struct *mm, unsigned long addr);
#endif
	unsigned long mmap_base;		/* base of mmap area */
	unsigned long task_size;		/* size of task vm space */
	unsigned long cached_hole_size; 	/* if non-zero, the largest hole below free_area_cache */
	unsigned long free_area_cache;		/* first hole of size cached_hole_size or larger */
	pgd_t * pgd;
	/** 20160416
	 * how many "real address space users" there are
	 *
	 * thread의 경우 task의 수만큼 증가.
	 **/
	atomic_t mm_users;			/* How many users with user space? */
	/** 20150801
	 * 이 mm_struct가 참조되는 count (커널에 의한 참조시에도 증가)
	 *
	 * the number of "lazy" users (ie anonymous users) plus one
	 * if there are any real users.
	 **/
	atomic_t mm_count;			/* How many references to "struct mm_struct" (users count as 1) */
	/** 20160528
	 * 이 mm_struct에 속한 vm_area_sturct의 수.
	 **/
	int map_count;				/* number of VMAs */

	/** 20160416
	 * page table과 counter를 보호하기 위한 spinlock.
	 **/
	spinlock_t page_table_lock;		/* Protects page tables and some counters */
	struct rw_semaphore mmap_sem;

	/** 20140531
	 * 메모리 디스크립터들의 리스트에서 인접한 노드를 가리킴
	 *
	 * mmlist의 첫번째 element는 init_mm.
	 * try_to_unmap_one에서 swapout 시킨 mm을 등록시키는 list head.
	 **/
	struct list_head mmlist;		/* List of maybe swapped mm's.	These are globally strung
						 * together off init_mm.mmlist, and are protected
						 * by mmlist_lock
						 */


	/** 20140531
	 * rss 사용량에 대한 high-watermark.
	 **/
	unsigned long hiwater_rss;	/* High-watermark of RSS usage */
	unsigned long hiwater_vm;	/* High-water virtual memory usage */

	unsigned long total_vm;		/* Total pages mapped */
	unsigned long locked_vm;	/* Pages that have PG_mlocked set */
	unsigned long pinned_vm;	/* Refcount permanently increased */
	unsigned long shared_vm;	/* Shared pages (files) */
	unsigned long exec_vm;		/* VM_EXEC & ~VM_WRITE */
	unsigned long stack_vm;		/* VM_GROWSUP/DOWN */
	unsigned long reserved_vm;	/* VM_RESERVED|VM_IO pages */
	unsigned long def_flags;
	unsigned long nr_ptes;		/* Page table pages */
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;

	unsigned long saved_auxv[AT_VECTOR_SIZE]; /* for /proc/PID/auxv */

	/*
	 * Special counters, in some configurations protected by the
	 * page_table_lock, in other configurations by being atomic.
	 */
	struct mm_rss_stat rss_stat;

	struct linux_binfmt *binfmt;

	/** 20150801
	 * cpu_vm_mask 변수
	 **/
	cpumask_var_t cpu_vm_mask_var;

	/* Architecture-specific MM context */
	mm_context_t context;

	unsigned long flags; /* Must use atomic bitops to access the bits */

	struct core_state *core_state; /* coredumping support */
#ifdef CONFIG_AIO
	spinlock_t		ioctx_lock;
	struct hlist_head	ioctx_list;
#endif
#ifdef CONFIG_MM_OWNER
	/*
	 * "owner" points to a task that is regarded as the canonical
	 * user/owner of this mm. All of the following must be true in
	 * order for it to be changed:
	 *
	 * current == mm->owner
	 * current->mm != mm
	 * new_owner->mm == mm
	 * new_owner->alloc_lock is held
	 */
	struct task_struct __rcu *owner;
#endif

	/* store ref to file /proc/<pid>/exe symlink points to */
	struct file *exe_file;
	unsigned long num_exe_file_vmas;
#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_notifier_mm *mmu_notifier_mm;
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	pgtable_t pmd_huge_pte; /* protected by page_table_lock */
#endif
#ifdef CONFIG_CPUMASK_OFFSTACK
	struct cpumask cpumask_allocation;
#endif
	struct uprobes_state uprobes_state;
};

/** 20160416
 * mm의 cpumask 관련 초기화.
 **/
static inline void mm_init_cpumask(struct mm_struct *mm)
{
#ifdef CONFIG_CPUMASK_OFFSTACK
	mm->cpu_vm_mask_var = &mm->cpumask_allocation;
#endif
}

/* Future-safe accessor for struct mm_struct's cpu_vm_mask. */
/** 20150801
 * mm_struct 구조체 내의 cpu_vm_mask_var 위치를 리턴한다.
 **/
static inline cpumask_t *mm_cpumask(struct mm_struct *mm)
{
	return mm->cpu_vm_mask_var;
}

#endif /* _LINUX_MM_TYPES_H */
