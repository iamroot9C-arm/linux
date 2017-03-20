/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/nsproxy.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/seccomp.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/random.h>
#include <linux/tty.h>
#include <linux/blkdev.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>
#include <linux/oom.h>
#include <linux/khugepaged.h>
#include <linux/signalfd.h>
#include <linux/uprobes.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/task.h>

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads;			/* The idle threads do not count.. */

int max_threads;		/* tunable limit on nr_threads */

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

__cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

#ifdef CONFIG_PROVE_RCU
int lockdep_tasklist_lock_is_held(void)
{
	return lockdep_is_held(&tasklist_lock);
}
EXPORT_SYMBOL_GPL(lockdep_tasklist_lock_is_held);
#endif /* #ifdef CONFIG_PROVE_RCU */

int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_possible_cpu(cpu)
		total += per_cpu(process_counts, cpu);

	return total;
}

/** 20150530
 * arm에서 오버라이드 하지 않는다.
 **/
void __weak arch_release_task_struct(struct task_struct *tsk)
{
}

#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
/** 20150530
 * fork_init에서 kmem_cache를 생성한다.
 **/
static struct kmem_cache *task_struct_cachep;

/** 20160227
 * task_struct_cachep kmem_cache로부터 object를 생성한다. 생성한다.
 **/
static inline struct task_struct *alloc_task_struct_node(int node)
{
	return kmem_cache_alloc_node(task_struct_cachep, GFP_KERNEL, node);
}

/** 20150530
 * struct task_struct로 할당받은 메모리를 반환한다.
 **/
static inline void free_task_struct(struct task_struct *tsk)
{
	kmem_cache_free(task_struct_cachep, tsk);
}
#endif

/** 20150530
 * arm에서 오버라이드 하지 않음.
 **/
void __weak arch_release_thread_info(struct thread_info *ti)
{
}

#ifndef CONFIG_ARCH_THREAD_INFO_ALLOCATOR

/*
 * Allocate pages if THREAD_SIZE is >= PAGE_SIZE, otherwise use a
 * kmemcache based allocator.
 */
# if THREAD_SIZE >= PAGE_SIZE
/** 20150117
 * thread_info 용으로 사용할 메모리를 할당해 리턴한다.
 *
 * thread_info는 stack에 overlay 되어 있으므로 stack으로 사용할 크기만큼 할당.
 *
 * # define THREADINFO_GFP		(GFP_KERNEL | __GFP_NOTRACK)
 **/
static struct thread_info *alloc_thread_info_node(struct task_struct *tsk,
						  int node)
{
	struct page *page = alloc_pages_node(node, THREADINFO_GFP,
					     THREAD_SIZE_ORDER);

	return page ? page_address(page) : NULL;
}

/** 20150530
 * thread_info로 사용 중인 페이지를 해제한다.
 **/
static inline void free_thread_info(struct thread_info *ti)
{
	free_pages((unsigned long)ti, THREAD_SIZE_ORDER);
}
# else
static struct kmem_cache *thread_info_cache;

static struct thread_info *alloc_thread_info_node(struct task_struct *tsk,
						  int node)
{
	return kmem_cache_alloc_node(thread_info_cache, THREADINFO_GFP, node);
}

static void free_thread_info(struct thread_info *ti)
{
	kmem_cache_free(thread_info_cache, ti);
}

void thread_info_cache_init(void)
{
	thread_info_cache = kmem_cache_create("thread_info", THREAD_SIZE,
					      THREAD_SIZE, 0, NULL);
	BUG_ON(thread_info_cache == NULL);
}
# endif
#endif

/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for vm_area_struct structures */
struct kmem_cache *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

/** 20150530
 * thread_info에 대한 account를 해당 zone에 반영한다.
 **/
static void account_kernel_stack(struct thread_info *ti, int account)
{
	/** 20150530
	 * thread_info가 할당된 page가 속한 zone을 찾는다.
	 **/
	struct zone *zone = page_zone(virt_to_page(ti));

	/** 20150530
	 * zone의 stat 중 NR_KERNEL_STACK 항목을 갱신한다.
	 **/
	mod_zone_page_state(zone, NR_KERNEL_STACK, account);
}

/** 20150530
 * task 관리를 위해 사용 중이던 메모리를 반환한다.
 * task_struct 구조체와 thread_info 구조체.
 **/
void free_task(struct task_struct *tsk)
{
	/** 20150530
	 * kernel_stack 사용량을 하나 감소시킨다.
	 **/
	account_kernel_stack(tsk->stack, -1);
	arch_release_thread_info(tsk->stack);
	/** 20150530
	 * thread_info로 할당된 메모리를 해제한다.
	 **/
	free_thread_info(tsk->stack);
	rt_mutex_debug_task_free(tsk);
	ftrace_graph_exit_task(tsk);
	put_seccomp_filter(tsk);
	arch_release_task_struct(tsk);
	/** 20150530
	 * task_struct로 할당된 메모리를 해제한다.
	 **/
	free_task_struct(tsk);
}
EXPORT_SYMBOL(free_task);

/** 20150530
 * 사용이 끝난 signal_struct 를 반환한다.
 **/
static inline void free_signal_struct(struct signal_struct *sig)
{
	taskstats_tgid_free(sig);
	sched_autogroup_exit(sig);
	/** 20150530
	 * struct signal_struct 하나를 해제한다.
	 **/
	kmem_cache_free(signal_cachep, sig);
}

/** 20150530
 * signal_struct 하나의 sigcnt를 감소시키고, 0이 되었다면 메모리를 반환한다.
 **/
static inline void put_signal_struct(struct signal_struct *sig)
{
	if (atomic_dec_and_test(&sig->sigcnt))
		free_signal_struct(sig);
}

/** 20150530
 * task의 사용이 완료되어 task 관리를 위해 사용 중이던 리소스를 반환한다.
 **/
void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!tsk->exit_state);
	WARN_ON(atomic_read(&tsk->usage));
	WARN_ON(tsk == current);

	security_task_free(tsk);
	exit_creds(tsk);
	delayacct_tsk_free(tsk);
	/** 20150530
	 * signal_struct의 사용을 종료한다.
	 **/
	put_signal_struct(tsk->signal);

	/** 20150530
	 * task 관리를 위해 사용 중이던 메모리를 반환한다.
	 **/
	if (!profile_handoff_task(tsk))
		free_task(tsk);
}
EXPORT_SYMBOL_GPL(__put_task_struct);

void __init __weak arch_task_cache_init(void) { }

/** 20150207
 * fork 관련 동작 수행을 위한 초기화.
 **/
void __init fork_init(unsigned long mempages)
{
#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	L1_CACHE_BYTES
#endif
	/* create a slab on which task_structs can be allocated */
	/** 20150207
	 * struct task_struct 용 kmem_cache를 생성한다.
	 **/
	task_struct_cachep =
		kmem_cache_create("task_struct", sizeof(struct task_struct),
			ARCH_MIN_TASKALIGN, SLAB_PANIC | SLAB_NOTRACK, NULL);
#endif

	/* do the arch specific task caches init */
	arch_task_cache_init();

	/*
	 * The default maximum number of threads is set to a safe
	 * value: the thread structures can take up at most half
	 * of memory.
	 */
	/** 20150207
	 * mempages를 기준으로 하여 최대 threads의 수를 계산한다.
	 **/
	max_threads = mempages / (8 * THREAD_SIZE / PAGE_SIZE);

	/*
	 * we need to allow at least 20 threads to boot a system
	 */
	/** 20150207
	 * max_threads는 최하 20이어야 한다.
	 **/
	if (max_threads < 20)
		max_threads = 20;

	/** 20150207
	 * init_task의 signal handler 관련 rlimit 값을 채운다.
	 **/
	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
	init_task.signal->rlim[RLIMIT_SIGPENDING] =
		init_task.signal->rlim[RLIMIT_NPROC];
}

/** 20150117
 * task_struct 내용 copy.
 **/
int __attribute__((weak)) arch_dup_task_struct(struct task_struct *dst,
					       struct task_struct *src)
{
	*dst = *src;
	return 0;
}

/** 20160227
 * orig task를 복사해 task_struct를 리턴한다.
 *
 * task_struct과 thread_info용 메모리를 할당받고, task_struct의 내용은 복사하고
 * thread_info는 독자적으로 유지한다.
 **/
static struct task_struct *dup_task_struct(struct task_struct *orig)
{
	struct task_struct *tsk;
	struct thread_info *ti;
	unsigned long *stackend;
	/** 20160227
	 * task에서 사용할 node 정보를 받아온다.
	 **/
	int node = tsk_fork_get_node(orig);
	int err;

	/** 20160227
	 * task_struct object를 받아온다.
	 **/
	tsk = alloc_task_struct_node(node);
	if (!tsk)
		return NULL;

	/** 20150117
	 * thread_info용 메모리 할당
	 **/
	ti = alloc_thread_info_node(tsk, node);
	if (!ti)
		goto free_tsk;

	/** 20150117
	 * orig의 struct task_struct를 복사한다.
	 **/
	err = arch_dup_task_struct(tsk, orig);
	if (err)
		goto free_ti;

	/** 20150117
	 * task_struct의 .stack은 새로 할당한 thread_info를 지정한다.
	 **/
	tsk->stack = ti;

	/** 20150117
	 * orig task의 struct thread_info를 복사하되 task는 새 task를 가리킨다.
	 **/
	setup_thread_stack(tsk, orig);
	clear_user_return_notifier(tsk);
	/** 20160227
	 * flag에서 need_resched flag는 제거한다.
	 **/
	clear_tsk_need_resched(tsk);
	/** 20160227
	 * stack은 thread_info 구조체를 침범하지 않을 때까지 자란다.
	 * stack의 끝에 MAGIC키를 저장해 overflow detection 용으로 사용된다.
	 **/
	stackend = end_of_stack(tsk);
	*stackend = STACK_END_MAGIC;	/* for overflow detection */

#ifdef CONFIG_CC_STACKPROTECTOR
	tsk->stack_canary = get_random_int();
#endif

	/*
	 * One for us, one for whoever does the "release_task()" (usually
	 * parent)
	 */
	/** 20160227
	 * usage를 2로 초기화 한다.
	 * 이 task를 위해 하나, parent를 위해 하나(release_task()를 호출하는).
	 **/
	atomic_set(&tsk->usage, 2);
#ifdef CONFIG_BLK_DEV_IO_TRACE
	tsk->btrace_seq = 0;
#endif
	/** 20160227
	 * NULL로 초기화.
	 **/
	tsk->splice_pipe = NULL;

	/** 20160227
	 * kernel_stack 사용량을 1증가시킨다.
	 **/
	account_kernel_stack(ti, 1);

	return tsk;

free_ti:
	free_thread_info(ti);
free_tsk:
	free_task_struct(tsk);
	return NULL;
}

#ifdef CONFIG_MMU
/** 20170228
 * oldmm (parent)의 mmap (vm_area_struct)을 순회하며 new mm에 복사한다.
 **/
static int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct vm_area_struct *mpnt, *tmp, *prev, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int retval;
	unsigned long charge;
	struct mempolicy *pol;

	/** 20160416
	 * mmap write semaphore를 잡는다.
	 **/
	down_write(&oldmm->mmap_sem);
	/** 20160416
	 * 복사전 oldmm의 cache flush.
	 **/
	flush_cache_dup_mm(oldmm);
	/*
	 * Not linked in yet - no deadlock potential:
	 */
	down_write_nested(&mm->mmap_sem, SINGLE_DEPTH_NESTING);

	/** 20170228
	 * 복사한 mm의 멤버를 초기화 한다.
	 **/
	mm->locked_vm = 0;
	mm->mmap = NULL;
	mm->mmap_cache = NULL;
	mm->free_area_cache = oldmm->mmap_base;
	mm->cached_hole_size = ~0UL;
	mm->map_count = 0;
	cpumask_clear(mm_cpumask(mm));
	mm->mm_rb = RB_ROOT;
	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	pprev = &mm->mmap;
	retval = ksm_fork(mm, oldmm);
	if (retval)
		goto out;
	retval = khugepaged_fork(mm, oldmm);
	if (retval)
		goto out;

	prev = NULL;
	/** 20160416
	 * oldmm (parent)의 mmap (vm_area_struct)을 하나씩 순회하며
	 * new mm (child)에 복사. 해당 영역에 대한 page table entry도 구성.
	 **/
	for (mpnt = oldmm->mmap; mpnt; mpnt = mpnt->vm_next) {
		struct file *file;

		/** 20160416
		 * vm_area_struct의 vm속성에 VM_DONTCOPY가 있다면 해당 vma는 건너뜀.
		 **/
		if (mpnt->vm_flags & VM_DONTCOPY) {
			vm_stat_account(mm, mpnt->vm_flags, mpnt->vm_file,
							-vma_pages(mpnt));
			continue;
		}
		charge = 0;
		/** 20160416
		 * VM_ACCOUNT 플래그가 주어진 경우
		 **/
		if (mpnt->vm_flags & VM_ACCOUNT) {
			/** 20160416
			 * mpnt 영역에 해당하는 페이지 수를 얻어온다.
			 **/
			unsigned long len = vma_pages(mpnt);

			if (security_vm_enough_memory_mm(oldmm, len)) /* sic */
				goto fail_nomem;
			charge = len;
		}
		/** 20160416
		 * vm_area_struct 객체 할당.
		 **/
		tmp = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);
		if (!tmp)
			goto fail_nomem;
		/** 20160416
		 * oldmm의 현재 vm_area_struct를 복사.
		 **/
		*tmp = *mpnt;
		INIT_LIST_HEAD(&tmp->anon_vma_chain);
		/** 20160416
		 * memory policy는 NUMA가 아닌 경우 NULL.
		 **/
		pol = mpol_dup(vma_policy(mpnt));
		retval = PTR_ERR(pol);
		if (IS_ERR(pol))
			goto fail_nomem_policy;
		vma_set_policy(tmp, pol);
		/** 20160416
		 * vm_area_struct이 속한 mm을 초기화.
		 **/
		tmp->vm_mm = mm;
		/** 20160430
		 * parent의 vma 각각에 대해 anon_vma를 생성받고 연결한다.
		 **/
		if (anon_vma_fork(tmp, mpnt))
			goto fail_nomem_anon_vma_fork;
		/** 20160430
		 * 복사한 vm_flags에서 VM_LOCKED 속성을 제거한다.
		 **/
		tmp->vm_flags &= ~VM_LOCKED;
		/** 20160430
		 * 선형 리스트 연결을 위한 vm_next와 vm_prev는 parent로부터 복사한 값을
		 * 로 초기화시킨다.
		 **/
		tmp->vm_next = tmp->vm_prev = NULL;
		/** 20160430
		 * parent에서 복사한 vm_file 값이 있을 때
		 * 파일의 address_space를 참조해 처리.
		 **/
		file = tmp->vm_file;
		if (file) {
			struct inode *inode = file->f_path.dentry->d_inode;
			struct address_space *mapping = file->f_mapping;

			get_file(file);
			if (tmp->vm_flags & VM_DENYWRITE)
				atomic_dec(&inode->i_writecount);
			mutex_lock(&mapping->i_mmap_mutex);
			if (tmp->vm_flags & VM_SHARED)
				mapping->i_mmap_writable++;
			/** 20160430
			 * radix tree에 lock을 건다.
			 **/
			flush_dcache_mmap_lock(mapping);
			/* insert tmp into the share list, just after mpnt */
			/** 20160430
			 * vma를 mpnt 다음으로 share list에 추가한다.
			 **/
			vma_prio_tree_add(tmp, mpnt);
			flush_dcache_mmap_unlock(mapping);
			mutex_unlock(&mapping->i_mmap_mutex);
		}

		/*
		 * Clear hugetlb-related page reserves for children. This only
		 * affects MAP_PRIVATE mappings. Faults generated by the child
		 * are not guaranteed to succeed, even if read-only
		 */
		/** 20160430
		 * hugetlb 관련 page reserve를 clear한다.
		 **/
		if (is_vm_hugetlb_page(tmp))
			reset_vma_resv_huge_pages(tmp);

		/*
		 * Link in the new vma and copy the page table entries.
		 */
		/** 20160528
		 * parent의 mm_struct의 mmap을 하나씩 복사해
		 * child의 mm_struct에 구성한다.
		 **/
		*pprev = tmp;
		pprev = &tmp->vm_next;
		tmp->vm_prev = prev;
		prev = tmp;

		/** 20160528
		 * 동일한 vm_area_struct을 rb tree로 관리하기 위해 mm_rb에 추가.
		 **/
		__vma_link_rb(mm, tmp, rb_link, rb_parent);
		rb_link = &tmp->vm_rb.rb_right;
		rb_parent = &tmp->vm_rb;

		/** 20160528
		 * VMA count 증가
		 **/
		mm->map_count++;
		/** 20160528
		 * mpnt에 해당하는 영역을 mm->pgd에 복사하여 구성.
		 **/
		retval = copy_page_range(mm, oldmm, mpnt);

		/** 20160528
		 * vm_ops 중 open이 구현된 경우 호출한다.
		 *
		 * 예를 들어 shared memory처럼 메모리 공간에 대한 독자적인 사용 방식이
		 * 정해진 경우, 해당 공간을 사용하기 위한 초기화 등의 연산이 필요하다.
		 **/
		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);

		if (retval)
			goto out;

		if (file && uprobe_mmap(tmp))
			goto out;
	}
	/* a new mm has just been created */
	/** 20160528
	 * architecture level의 dup_mmap 함수가 정의되어 있다면 호출.
	 **/
	arch_dup_mmap(oldmm, mm);
	retval = 0;
out:
	/** 20160528
	 * mm_struct의 write semaphore를 놓는다.
	 **/
	up_write(&mm->mmap_sem);
	flush_tlb_mm(oldmm);
	up_write(&oldmm->mmap_sem);
	return retval;
fail_nomem_anon_vma_fork:
	mpol_put(pol);
fail_nomem_policy:
	kmem_cache_free(vm_area_cachep, tmp);
fail_nomem:
	retval = -ENOMEM;
	vm_unacct_memory(charge);
	goto out;
}

/** 20160416
 * pgd를 할당 받고, kernel 영역에 대한 정보를 복사한다.
 **/
static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}
#else
#define dup_mmap(mm, oldmm)	(0)
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

/** 20160416
 * mm_cachep kmem_cache에서 오브젝트를 하나 할당 받는다.
 **/
#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

static unsigned long default_dump_filter = MMF_DUMP_FILTER_DEFAULT;

static int __init coredump_filter_setup(char *s)
{
	default_dump_filter =
		(simple_strtoul(s, NULL, 0) << MMF_DUMP_FILTER_SHIFT) &
		MMF_DUMP_FILTER_MASK;
	return 1;
}

__setup("coredump_filter=", coredump_filter_setup);

#include <linux/init_task.h>

static void mm_init_aio(struct mm_struct *mm)
{
#ifdef CONFIG_AIO
	spin_lock_init(&mm->ioctx_lock);
	INIT_HLIST_HEAD(&mm->ioctx_list);
#endif
}

/** 20160416
 * fork시 mm_struct를 초기화.
 *
 * user task의 mm_struct의 초기화시 pgd를 할당 받고, kernel 영역에 대한 entry는
 * 복사한다.
 **/
static struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p)
{
	/** 20160416
	 * mm_user와 mm_count는 1로 초기화.
	 **/
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	/** 20160416
	 * memory map rw semaphore 초기화.
	 * mmlist 초기화.
	 **/
	init_rwsem(&mm->mmap_sem);
	INIT_LIST_HEAD(&mm->mmlist);
	/** 20160416
	 * current의 mm이 존재하면 현재 설정된 flag 중 INIT_MASK에 해당하는 것만 복사
	 **/
	mm->flags = (current->mm) ?
		(current->mm->flags & MMF_INIT_MASK) : default_dump_filter;
	mm->core_state = NULL;
	mm->nr_ptes = 0;
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat));
	spin_lock_init(&mm->page_table_lock);
	mm->free_area_cache = TASK_UNMAPPED_BASE;
	mm->cached_hole_size = ~0UL;
	mm_init_aio(mm);
	mm_init_owner(mm, p);

	/** 20160416
	 * mm의 pgd 할당이 성공했다면 def_flags와 notifier 등을 초기화 하고 리턴.
	 **/
	if (likely(!mm_alloc_pgd(mm))) {
		mm->def_flags = 0;
		mmu_notifier_mm_init(mm);
		return mm;
	}

	/** 20160416
	 * 실패한 경우 할당 받은 mm를 반환하고 리턴.
	 **/
	free_mm(mm);
	return NULL;
}

static void check_mm(struct mm_struct *mm)
{
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = atomic_long_read(&mm->rss_stat.count[i]);

		if (unlikely(x))
			printk(KERN_ALERT "BUG: Bad rss-counter state "
					  "mm:%p idx:%d val:%ld\n", mm, i, x);
	}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	VM_BUG_ON(mm->pmd_huge_pte);
#endif
}

/*
 * Allocate and initialize an mm_struct.
 */
/** 20161207
 * mm_struct 할당과 초기화.
 *
 * do_execve류에서 시작
 **/
struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	/** 20161207
	 * kmem_cache로부터 mm_struct 오브젝트 할당
	 **/
	mm = allocate_mm();
	if (!mm)
		return NULL;

	/** 20161207
	 * 구조체 초기화해서 object 준비
	 **/
	memset(mm, 0, sizeof(*mm));
	mm_init_cpumask(mm);
	return mm_init(mm, current);
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	mm_free_pgd(mm);
	destroy_context(mm);
	mmu_notifier_mm_destroy(mm);
	check_mm(mm);
	free_mm(mm);
}
EXPORT_SYMBOL_GPL(__mmdrop);

/*
 * Decrement the use count and release all resources for an mm.
 */
void mmput(struct mm_struct *mm)
{
	might_sleep();

	/** 20170228
	 * mm_users를 감소시키고 0이 되었는지 테스트 한다.
	 **/
	if (atomic_dec_and_test(&mm->mm_users)) {
		uprobe_clear_state(mm);
		exit_aio(mm);
		ksm_exit(mm);
		khugepaged_exit(mm); /* must run before exit_mmap */
		exit_mmap(mm);
		set_mm_exe_file(mm, NULL);
		if (!list_empty(&mm->mmlist)) {
			spin_lock(&mmlist_lock);
			list_del(&mm->mmlist);
			spin_unlock(&mmlist_lock);
		}
		if (mm->binfmt)
			module_put(mm->binfmt->module);
		mmdrop(mm);
	}
}
EXPORT_SYMBOL_GPL(mmput);

/*
 * We added or removed a vma mapping the executable. The vmas are only mapped
 * during exec and are not mapped with the mmap system call.
 * Callers must hold down_write() on the mm's mmap_sem for these
 */
void added_exe_file_vma(struct mm_struct *mm)
{
	mm->num_exe_file_vmas++;
}

void removed_exe_file_vma(struct mm_struct *mm)
{
	mm->num_exe_file_vmas--;
	if ((mm->num_exe_file_vmas == 0) && mm->exe_file) {
		fput(mm->exe_file);
		mm->exe_file = NULL;
	}

}

void set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	if (new_exe_file)
		get_file(new_exe_file);
	if (mm->exe_file)
		fput(mm->exe_file);
	mm->exe_file = new_exe_file;
	mm->num_exe_file_vmas = 0;
}

/** 20160416
 * mm의 exe_file의 참조카운트를 증가시키고 읽어온다.
 **/
struct file *get_mm_exe_file(struct mm_struct *mm)
{
	struct file *exe_file;

	/* We need mmap_sem to protect against races with removal of
	 * VM_EXECUTABLE vmas */
	/** 20160416
	 * read semaphore를 잡고 mm의 exe_file을 읽어
	 * reference count를 증가시키고 리턴.
	 **/
	down_read(&mm->mmap_sem);
	exe_file = mm->exe_file;
	if (exe_file)
		get_file(exe_file);
	up_read(&mm->mmap_sem);
	return exe_file;
}

/** 20160416
 * oldmm의 exe_file 정보를 newmm으로 복사한다.
 **/
static void dup_mm_exe_file(struct mm_struct *oldmm, struct mm_struct *newmm)
{
	/* It's safe to write the exe_file pointer without exe_file_lock because
	 * this is called during fork when the task is not yet in /proc */
	newmm->exe_file = get_mm_exe_file(oldmm);
}

/**
 * get_task_mm - acquire a reference to the task's mm
 *
 * Returns %NULL if the task has no mm.  Checks PF_KTHREAD (meaning
 * this kernel workthread has transiently adopted a user mm with use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	task_lock(task);
	mm = task->mm;
	if (mm) {
		if (task->flags & PF_KTHREAD)
			mm = NULL;
		else
			atomic_inc(&mm->mm_users);
	}
	task_unlock(task);
	return mm;
}
EXPORT_SYMBOL_GPL(get_task_mm);

struct mm_struct *mm_access(struct task_struct *task, unsigned int mode)
{
	struct mm_struct *mm;
	int err;

	err =  mutex_lock_killable(&task->signal->cred_guard_mutex);
	if (err)
		return ERR_PTR(err);

	mm = get_task_mm(task);
	if (mm && mm != current->mm &&
			!ptrace_may_access(task, mode)) {
		mmput(mm);
		mm = ERR_PTR(-EACCES);
	}
	mutex_unlock(&task->signal->cred_guard_mutex);

	return mm;
}

static void complete_vfork_done(struct task_struct *tsk)
{
	struct completion *vfork;

	task_lock(tsk);
	vfork = tsk->vfork_done;
	if (likely(vfork)) {
		tsk->vfork_done = NULL;
		complete(vfork);
	}
	task_unlock(tsk);
}

static int wait_for_vfork_done(struct task_struct *child,
				struct completion *vfork)
{
	int killed;

	freezer_do_not_count();
	killed = wait_for_completion_killable(vfork);
	freezer_count();

	if (killed) {
		task_lock(child);
		child->vfork_done = NULL;
		task_unlock(child);
	}

	put_task_struct(child);
	return killed;
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	/* Get rid of any futexes when releasing the mm */
#ifdef CONFIG_FUTEX
	if (unlikely(tsk->robust_list)) {
		exit_robust_list(tsk);
		tsk->robust_list = NULL;
	}
#ifdef CONFIG_COMPAT
	if (unlikely(tsk->compat_robust_list)) {
		compat_exit_robust_list(tsk);
		tsk->compat_robust_list = NULL;
	}
#endif
	if (unlikely(!list_empty(&tsk->pi_state_list)))
		exit_pi_state_list(tsk);
#endif

	uprobe_free_utask(tsk);

	/* Get rid of any cached register state */
	deactivate_mm(tsk, mm);

	/*
	 * If we're exiting normally, clear a user-space tid field if
	 * requested.  We leave this alone when dying by signal, to leave
	 * the value intact in a core dump, and to save the unnecessary
	 * trouble, say, a killed vfork parent shouldn't touch this mm.
	 * Userland only wants this done for a sys_exit.
	 */
	if (tsk->clear_child_tid) {
		if (!(tsk->flags & PF_SIGNALED) &&
		    atomic_read(&mm->mm_users) > 1) {
			/*
			 * We don't check the error code - if userspace has
			 * not set up a proper pointer then tough luck.
			 */
			put_user(0, tsk->clear_child_tid);
			sys_futex(tsk->clear_child_tid, FUTEX_WAKE,
					1, NULL, NULL, 0);
		}
		tsk->clear_child_tid = NULL;
	}

	/*
	 * All done, finally we can wake up parent and return this mm to him.
	 * Also kthread_stop() uses this completion for synchronization.
	 */
	if (tsk->vfork_done)
		complete_vfork_done(tsk);
}

/*
 * Allocate a new mm structure and copy contents from the
 * mm structure of the passed in task structure.
 */
/** 20170228
 * 새 tsk를 위해 mm_struct 구조체를 할당 받고 초기화 한다.
 * oldmm의 mm_struct의 vma 등을 복사한다.
 **/
struct mm_struct *dup_mm(struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm = current->mm;
	int err;

	if (!oldmm)
		return NULL;

	/** 20160416
	 * struct  mm_struct를 할당 받는다.
	 **/
	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	/** 20160416
	 * 현재 task의 mm을 할당받은 mm에 복사한다.
	 **/
	memcpy(mm, oldmm, sizeof(*mm));
	mm_init_cpumask(mm);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	mm->pmd_huge_pte = NULL;
#endif
	uprobe_reset_state(mm);

	/** 20160416
	 * mm의 독자적인 부분을 초기화.
	 * pgd 할당 포함.
	 **/
	if (!mm_init(mm, tsk))
		goto fail_nomem;

	/** 20160416
	 * 새 context 초기화.
	 **/
	if (init_new_context(tsk, mm))
		goto fail_nocontext;

	/** 20160416
	 * oldmm의 exe_file 정보를 복사한다.
	 **/
	dup_mm_exe_file(oldmm, mm);

	/** 20170228
	 * oldmm의 vma 등을 mm에 복사한다.
	 **/
	err = dup_mmap(mm, oldmm);
	if (err)
		goto free_pt;

	/** 20170228
	 * taskstat에서 동작하는 High-watermark 메모리 사용량을 설정한다
	 **/
	mm->hiwater_rss = get_mm_rss(mm);
	mm->hiwater_vm = mm->total_vm;

	if (mm->binfmt && !try_module_get(mm->binfmt->module))
		goto free_pt;

	return mm;

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet */
	mm->binfmt = NULL;
	mmput(mm);

fail_nomem:
	return NULL;

fail_nocontext:
	/*
	 * If init_new_context() failed, we cannot use mmput() to free the mm
	 * because it calls destroy_context()
	 */
	mm_free_pgd(mm);
	free_mm(mm);
	return NULL;
}

/** 20170228
 * copy_process 중 task의 mm을 설정한다.
 *
 * 부모가 kernel task일 경우 mm이 NULL이고 context switch시 active_mm이
 * 결정되므로 새로 생성하지 않는다.
 * 부모가 user task일 경우, clone_flags에 VM을 공유해야 한다는 속성이 없으면
 * 새로 생성하고 vma를 복사해 설정한다.
 **/
static int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm;
	int retval;

	/** 20160416
	 * fault count를 0으로 초기화.
	 **/
	tsk->min_flt = tsk->maj_flt = 0;
	/** 20160416
	 * voluntary/involuntary context switching count를 초기화
	 **/
	tsk->nvcsw = tsk->nivcsw = 0;
#ifdef CONFIG_DETECT_HUNG_TASK
	/** 20160416
	 * CONFIG_DETECT_HUNG_TASK 설정되어 있을 경우 last switch count를 가진다.
	 **/
	tsk->last_switch_count = tsk->nvcsw + tsk->nivcsw;
#endif

	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	/** 20160416
	 * 부모 프로세스의 mm이 NULL, 즉 kernel thread라면 바로 리턴.
	 * 이후 user task
	 **/
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	/** 20160416
	 * CLONE_VM 플래그(vm shared)가 주어졌다면 mm을 새로 만들지 않고
	 * user counter만 증가시키고 부모 task의 mm을 공유.
	 **/
	if (clone_flags & CLONE_VM) {
		atomic_inc(&oldmm->mm_users);
		mm = oldmm;
		goto good_mm;
	}

	/** 20170228
	 * 새 task를 위한 mm을 생성하고 기본적인 내용은 초기화 한다.
	 * 현재 task의 mm_struct를 복사해 설정한다.
	 *
	 * 이 부분은 user task에만 해당.
	 **/
	retval = -ENOMEM;
	mm = dup_mm(tsk);
	if (!mm)
		goto fail_nomem;

good_mm:
	/** 20170228
	 * mm과 active_mm에 설정한다.
	 **/
	tsk->mm = mm;
	tsk->active_mm = mm;
	return 0;

fail_nomem:
	return retval;
}

/** 20160409
 * CLONE_FS가 주어진 경우, 부모 프로세스의 fs_struct을 공유한다. users만 증가.
 * 주어지지 않은 경우, 새로 메모리를 할당 받아 부모의 현재 fs_struct을 복사한다.
 **/
static int copy_fs(unsigned long clone_flags, struct task_struct *tsk)
{
	struct fs_struct *fs = current->fs;
	if (clone_flags & CLONE_FS) {
		/* tsk->fs is already what we want */
		spin_lock(&fs->lock);
		if (fs->in_exec) {
			spin_unlock(&fs->lock);
			return -EAGAIN;
		}
		fs->users++;
		spin_unlock(&fs->lock);
		return 0;
	}
	tsk->fs = copy_fs_struct(fs);
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

/** 20160409
 * clone_flags에 따라 부모 프로세스의 파일 디스크립터 테이블을 복사한다.
 *
 * CLONE_FILES 설정시: 부모와 자식 프로세스는 동일한 파일 기술자 테이블 공유.
 * 파일 기술자들은 항상 부모 자식 프로세스  내의  동일한  파일들을 참조한다.
 * 부모  혹은  자식  프로세스에 의해 만들어진 모든 파일 기술자는
 * 다른 프로세스에서도 역시 타당하다. 이와 유사하게, 만일 프로세스들중 하나가
 * 파일 기술자를 닫거나, 플래그를 변경하면, 기타 프로세스가 역시 영향을 받는다.
 *
 * CLONE_FILES 비설정시: 자식 프로세스는 __clone시 부모프로세스에서
 * 열린 파일기술자들의 복사본을 상속받는다. 부모 혹은 자식 프로세스들 중 하나에
 * 의해 나중에 수행될 파일 기술자들에 대한 연산은 다른 것에 영향을 주지 않는다.
 **/
static int copy_files(unsigned long clone_flags, struct task_struct *tsk)
{
	struct files_struct *oldf, *newf;
	int error = 0;

	/*
	 * A background process may not have any files ...
	 */
	oldf = current->files;
	if (!oldf)
		goto out;

	/** 20160409
	 * CLONE_FILES 설정시 부모와 파일기술자를 공유하므로 참조카운트만 증가.
	 **/
	if (clone_flags & CLONE_FILES) {
		atomic_inc(&oldf->count);
		goto out;
	}

	/** 20160409
	 * CLONE_FILES 미설정시 현재 부모의 파일 기술자 테이블을 복사.
	 **/
	newf = dup_fd(oldf, &error);
	if (!newf)
		goto out;

	/** 20160409
	 * 자식 task에 새로 생성한 파일 기술자 테이블을 지정한다.
	 **/
	tsk->files = newf;
	error = 0;
out:
	return error;
}

static int copy_io(unsigned long clone_flags, struct task_struct *tsk)
{
#ifdef CONFIG_BLOCK
	struct io_context *ioc = current->io_context;
	struct io_context *new_ioc;

	if (!ioc)
		return 0;
	/*
	 * Share io context with parent, if CLONE_IO is set
	 */
	if (clone_flags & CLONE_IO) {
		ioc_task_link(ioc);
		tsk->io_context = ioc;
	} else if (ioprio_valid(ioc->ioprio)) {
		new_ioc = get_task_io_context(tsk, GFP_KERNEL, NUMA_NO_NODE);
		if (unlikely(!new_ioc))
			return -ENOMEM;

		new_ioc->ioprio = ioc->ioprio;
		put_io_context(new_ioc);
	}
#endif
	return 0;
}

/** 20160409
 * 부모 프로세스의 sighand를 복사한다.
 *
 * CLONE_SIGHAND 플래그가 주어진 경우 부모-자식 프로세스간의 sighand를 공유하므로
 * 레퍼런스 카운트만 증가시켜 리턴.
 * 그렇지 않은 경우 별도로 구조체를 할당받고 현재까지의 sighand를 복사해 리턴.
 **/
static int copy_sighand(unsigned long clone_flags, struct task_struct *tsk)
{
	struct sighand_struct *sig;

	/** 20160409
	 * CLONE_SIGHAND라면 부모와 자식 프로세스간에 시그널 핸들러를 공유하므로
	 * 레퍼런스 카운트만 증가시키고 리턴.
	 **/
	if (clone_flags & CLONE_SIGHAND) {
		atomic_inc(&current->sighand->count);
		return 0;
	}
	/** 20160409
	 * 별도의 sighand 관리를 위해 kmem_cache로부터 object를 할당 받아 task에 등록.
	 **/
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
	rcu_assign_pointer(tsk->sighand, sig);
	if (!sig)
		return -ENOMEM;
	/** 20160409
	 * reference count는 1로 초기화
	 **/
	atomic_set(&sig->count, 1);
	/** 20160409
	 * 현재의 부모의 sighand 구조체를 복사한다.
	 * 포인터 타입으로 별도 할당이 필요한 데이터가 없으므로 memcpy만 수행.
	 **/
	memcpy(sig->action, current->sighand->action, sizeof(sig->action));
	return 0;
}

void __cleanup_sighand(struct sighand_struct *sighand)
{
	if (atomic_dec_and_test(&sighand->count)) {
		signalfd_cleanup(sighand);
		kmem_cache_free(sighand_cachep, sighand);
	}
}


/*
 * Initialize POSIX timer handling for a thread group.
 */
/** 20160409
 * thread group에 대한 POSIX timer handling 초기화.
 **/
static void posix_cpu_timers_init_group(struct signal_struct *sig)
{
	unsigned long cpu_limit;

	/* Thread group counters. */
	/** 20160409
	 * thread group cputimer 초기화 (thread_group_cputimer의 spinlock만 초기화)
	 **/
	thread_group_cputime_init(sig);

	/** 20160409
	 * signal_struct의 resource limit 중 cpu time 값을 얻어온다.
	 **/
	cpu_limit = ACCESS_ONCE(sig->rlim[RLIMIT_CPU].rlim_cur);
	/** 20160409
	 * INFINITY가 아니라면 cpu_limit으로 cputime_expires 중 prof_exp(stime)을 설정
	 **/
	if (cpu_limit != RLIM_INFINITY) {
		sig->cputime_expires.prof_exp = secs_to_cputime(cpu_limit);
		sig->cputimer.running = 1;
	}

	/* The timer lists. */
	/** 20160409
	 * cpu_timers 리스트 초기화.
	 **/
	INIT_LIST_HEAD(&sig->cpu_timers[0]);
	INIT_LIST_HEAD(&sig->cpu_timers[1]);
	INIT_LIST_HEAD(&sig->cpu_timers[2]);
}

/** 20160409
 * 시그널을 부모/자식 간에 공유하지 않으므로 별도로 메모리를 할당 받아 초기화.
 **/
static int copy_signal(unsigned long clone_flags, struct task_struct *tsk)
{
	struct signal_struct *sig;

	/** 20160409
	 * THREAD인 경우 signal_struct을 공유하므로 리턴.
	 * process는 CLONE 플래그 없이 독자적인 시그널을 사용한다.
	 **/
	if (clone_flags & CLONE_THREAD)
		return 0;

	/** 20160409
	 * signal_struct kmem_cache로부터 오브젝트를 할당 받아 task에 저장한다.
	 **/
	sig = kmem_cache_zalloc(signal_cachep, GFP_KERNEL);
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;

	/** 20160409
	 * 초기화.
	 **/
	sig->nr_threads = 1;
	atomic_set(&sig->live, 1);
	atomic_set(&sig->sigcnt, 1);
	init_waitqueue_head(&sig->wait_chldexit);
	if (clone_flags & CLONE_NEWPID)
		sig->flags |= SIGNAL_UNKILLABLE;
	sig->curr_target = tsk;
	init_sigpending(&sig->shared_pending);
	INIT_LIST_HEAD(&sig->posix_timers);

	/** 20160409
	 * hrtimer를 초기화. monotonic
	 **/
	hrtimer_init(&sig->real_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sig->real_timer.function = it_real_fn;

	/** 20160409
	 * 현재 task의 threadgroup leader에게 lock을 걸고,
	 * signal의 rlim 값을 복사한다.
	 **/
	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim);
	task_unlock(current->group_leader);

	posix_cpu_timers_init_group(sig);

	tty_audit_fork(sig);
	sched_autogroup_fork(sig);

#ifdef CONFIG_CGROUPS
	init_rwsem(&sig->group_rwsem);
#endif

	/** 20160409
	 * 부모 process의 oom 관련 정보를 복사한다.
	 **/
	sig->oom_adj = current->signal->oom_adj;
	sig->oom_score_adj = current->signal->oom_score_adj;
	sig->oom_score_adj_min = current->signal->oom_score_adj_min;

	sig->has_child_subreaper = current->signal->has_child_subreaper ||
				   current->signal->is_child_subreaper;

	mutex_init(&sig->cred_guard_mutex);

	return 0;
}

/** 20160319
 * copy로 생성되는 tasks의 flag를 조절한다.
 *
 * super-user priv, workqueue worker를 제거하고, exec에 의한 fork가 아님을 표시.
 **/
static void copy_flags(unsigned long clone_flags, struct task_struct *p)
{
	unsigned long new_flags = p->flags;

	new_flags &= ~(PF_SUPERPRIV | PF_WQ_WORKER);
	new_flags |= PF_FORKNOEXEC;
	p->flags = new_flags;
}

SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
	current->clear_child_tid = tidptr;

	return task_pid_vnr(current);
}

/** 20160319
 * task p에 대해 rt_mutex 관련 멤버를 초기화 한다.
 **/
static void rt_mutex_init_task(struct task_struct *p)
{
	raw_spin_lock_init(&p->pi_lock);
#ifdef CONFIG_RT_MUTEXES
	plist_head_init(&p->pi_waiters);
	p->pi_blocked_on = NULL;
#endif
}

#ifdef CONFIG_MM_OWNER
void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{
	mm->owner = p;
}
#endif /* CONFIG_MM_OWNER */

/*
 * Initialize POSIX timer handling for a single task.
 */
/** 20160319
 * POSIX timer 관련 초기화.
 **/
static void posix_cpu_timers_init(struct task_struct *tsk)
{
	tsk->cputime_expires.prof_exp = 0;
	tsk->cputime_expires.virt_exp = 0;
	tsk->cputime_expires.sched_exp = 0;
	INIT_LIST_HEAD(&tsk->cpu_timers[0]);
	INIT_LIST_HEAD(&tsk->cpu_timers[1]);
	INIT_LIST_HEAD(&tsk->cpu_timers[2]);
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */
/** 20160312
 *
 * 이전 process를 복사해 새로운 process를 만든다.
 * 새로운 process의 시작은 caller에서 담당한다.
 **/
static struct task_struct *copy_process(unsigned long clone_flags,
					unsigned long stack_start,
					struct pt_regs *regs,
					unsigned long stack_size,
					int __user *child_tidptr,
					struct pid *pid,
					int trace)
{
	int retval;
	struct task_struct *p;
	int cgroup_callbacks_done = 0;

	/** 20160312
	 * clone_flags에 대한 validate.
	 * NEWNS면서 FS SHARE면 error.
	 **/
	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);

	/*
	 * Siblings of global init remain as zombies on exit since they are
	 * not reaped by their parent (swapper). To solve this and to avoid
	 * multi-rooted process trees, prevent global and container-inits
	 * from creating siblings.
	 */
	if ((clone_flags & CLONE_PARENT) &&
				current->signal->flags & SIGNAL_UNKILLABLE)
		return ERR_PTR(-EINVAL);

	retval = security_task_create(clone_flags);
	if (retval)
		goto fork_out;

	retval = -ENOMEM;
	/** 20160227
	 * 현재 task_struct를 복사한 새 task_struct을 리턴한다.
	 **/
	p = dup_task_struct(current);
	if (!p)
		goto fork_out;

	ftrace_graph_init_task(p);
	get_seccomp_filter(p);

	/** 20160319
	 * rt_mutex를 위한 멤버를 초기화 한다.
	 **/
	rt_mutex_init_task(p);

#ifdef CONFIG_PROVE_LOCKING
	DEBUG_LOCKS_WARN_ON(!p->hardirqs_enabled);
	DEBUG_LOCKS_WARN_ON(!p->softirqs_enabled);
#endif
	retval = -EAGAIN;
	if (atomic_read(&p->real_cred->user->processes) >=
			task_rlimit(p, RLIMIT_NPROC)) {
		if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_RESOURCE) &&
		    p->real_cred->user != INIT_USER)
			goto bad_fork_free;
	}
	current->flags &= ~PF_NPROC_EXCEEDED;

	/** 20160319
	 * parent task의 cred를 속성을 복사한다.
	 **/
	retval = copy_creds(p, clone_flags);
	if (retval < 0)
		goto bad_fork_free;

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	/** 20160319
	 * copy_process()가 여러 core에서 호출되어 max_threads를 초과 했다면
	 * cleanup 하고 EAGAIN을 리턴시킨다.
	 *
	 * 여기서 체크하는 것은 늦다고 볼 수 있지만 문제될 것은 없기 때문에
	 * root fork bombs에 대한 방비책으로 호출한 것이다.
	 **/
	retval = -EAGAIN;
	if (nr_threads >= max_threads)
		goto bad_fork_cleanup_count;

	/** 20160319
	 * thread의 execution domain의 module 레퍼런스 획득에 실패하면 cleanup.
	 **/
	if (!try_module_get(task_thread_info(p)->exec_domain->module))
		goto bad_fork_cleanup_count;

	p->did_exec = 0;
	delayacct_tsk_init(p);	/* Must remain after dup_task_struct() */
	copy_flags(clone_flags, p);
	INIT_LIST_HEAD(&p->children);
	INIT_LIST_HEAD(&p->sibling);
	/** 20160319
	 * rcu 관련 멤버 초기화.
	 **/
	rcu_copy_process(p);
	p->vfork_done = NULL;
	/** 20160319
	 * alloc_lock을 초기화.
	 **/
	spin_lock_init(&p->alloc_lock);

	/** 20160326
	 * 생성한 task의 sigpending 구조체 초기화
	 **/
	init_sigpending(&p->pending);

	p->utime = p->stime = p->gtime = 0;
	p->utimescaled = p->stimescaled = 0;
#ifndef CONFIG_VIRT_CPU_ACCOUNTING
	p->prev_utime = p->prev_stime = 0;
#endif
#if defined(SPLIT_RSS_COUNTING)
	memset(&p->rss_stat, 0, sizeof(p->rss_stat));
#endif

	/** 20160319
	 * parent의 timer_slack_ns를 새 프로세스의 default_timer_slack_ns로 저장.
	 **/
	p->default_timer_slack_ns = current->timer_slack_ns;

	/** 20160319
	 * accounting 관련 초기화
	 **/
	task_io_accounting_init(&p->ioac);
	acct_clear_integrals(p);

	/** 20160319
	 * posix cpu timer 초기화
	 **/
	posix_cpu_timers_init(p);

	/** 20160326
	 * 생성한 task의 start_time과 real_start_time을 저장
	 **/
	do_posix_clock_monotonic_gettime(&p->start_time);
	p->real_start_time = p->start_time;
	/** 20160326
	 * real_start_time은 boot based time으로 다시 저장.
	 **/
	monotonic_to_bootbased(&p->real_start_time);
	/** 20160326
	 * io_context와 audit_context는 초기화.
	 **/
	p->io_context = NULL;
	p->audit_context = NULL;
	/** 20160326
	 * thread 생성시 threadgroup_change 구간을 시작한다.
	 * CGROUPS가 정의되지 않은 경우 의미 없음.
	 **/
	if (clone_flags & CLONE_THREAD)
		threadgroup_change_begin(current);
	cgroup_fork(p);
#ifdef CONFIG_NUMA
	p->mempolicy = mpol_dup(p->mempolicy);
	if (IS_ERR(p->mempolicy)) {
		retval = PTR_ERR(p->mempolicy);
		p->mempolicy = NULL;
		goto bad_fork_cleanup_cgroup;
	}
	mpol_fix_fork_child_flag(p);
#endif
#ifdef CONFIG_CPUSETS
	/** 20160326
	 * cpuset을 사용하는 경우 cpuset의 mem과 slab을 rotor에 의해 분산.
	 **/
	p->cpuset_mem_spread_rotor = NUMA_NO_NODE;
	p->cpuset_slab_spread_rotor = NUMA_NO_NODE;
	/** 20160326
	 * sequence count 초기화.
	 **/
	seqcount_init(&p->mems_allowed_seq);
#endif
	/** 20160326
	 * TRACE 관련 분석 생략
	 **/
#ifdef CONFIG_TRACE_IRQFLAGS
	p->irq_events = 0;
#ifdef __ARCH_WANT_INTERRUPTS_ON_CTXSW
	p->hardirqs_enabled = 1;
#else
	p->hardirqs_enabled = 0;
#endif
	p->hardirq_enable_ip = 0;
	p->hardirq_enable_event = 0;
	p->hardirq_disable_ip = _THIS_IP_;
	p->hardirq_disable_event = 0;
	p->softirqs_enabled = 1;
	p->softirq_enable_ip = _THIS_IP_;
	p->softirq_enable_event = 0;
	p->softirq_disable_ip = 0;
	p->softirq_disable_event = 0;
	p->hardirq_context = 0;
	p->softirq_context = 0;
#endif
	/** 20160326
	 * LOCKDEP 분석 생략
	 **/
#ifdef CONFIG_LOCKDEP
	p->lockdep_depth = 0; /* no locks held yet */
	p->curr_chain_key = 0;
	p->lockdep_recursion = 0;
#endif

	/** 20160326
	 * DEBUG MUTEX 분석 생략
	 **/
#ifdef CONFIG_DEBUG_MUTEXES
	p->blocked_on = NULL; /* not blocked yet */
#endif
	/** 20160326
	 * MEMCG 분석 생략
	 **/
#ifdef CONFIG_MEMCG
	p->memcg_batch.do_batch = 0;
	p->memcg_batch.memcg = NULL;
#endif

	/* Perform scheduler related setup. Assign this task to a CPU. */
	/** 20160402
	 * scheduler 관련 설정을 수행한다.
	 **/
	sched_fork(p);

	/** 20160402
	 * PERF 관련 분석 생략
	 **/
	retval = perf_event_init_task(p);
	if (retval)
		goto bad_fork_cleanup_policy;
	/** 20160402
	 * AUDITSYSCALL 관련 분석 생략
	 **/
	retval = audit_alloc(p);
	if (retval)
		goto bad_fork_cleanup_policy;
	/* copy all the process information */
	/** 20160409
	 * clone_flags에 따라 세마포어 undo_list 공유 여부를 결정한다.
	 **/
	retval = copy_semundo(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_audit;
	/** 20160409
	 * CLONE_FILES에 따라 파일 디스크립터 테이블을 복사한다.
	 **/
	retval = copy_files(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_semundo;
	/** 20160409
	 * CLONE_FS 플래그에 따라 task의 파일시스템 정보를 복사한다.
	 **/
	retval = copy_fs(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_files;
	/** 20160409
	 * CLONE_SIGHAND 플래그에 따라 task의 sighand 정보를 복사한다.
	 **/
	retval = copy_sighand(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_fs;
	/** 20160409
	 * task의 signal 정보를 설정한다. 공유데이터는 rlimit과 oom 설정 등 일부이다.
	 **/
	retval = copy_signal(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_sighand;
	retval = copy_mm(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_signal;
	retval = copy_namespaces(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_mm;
	retval = copy_io(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_namespaces;
	retval = copy_thread(clone_flags, stack_start, stack_size, p, regs);
	if (retval)
		goto bad_fork_cleanup_io;

	/** 20160227
	 * pid가 init_struct_pid라면 (fork_idle인 경우) init의 pid를 그대로 복사할
	 * 것이므로 alloc_pid를 하지 않는다.
	 * 그렇지 않은 경우에는 해당 pid_ns로부터 pid를 하나 받아온다.
	 **/
	if (pid != &init_struct_pid) {
		retval = -ENOMEM;
		pid = alloc_pid(p->nsproxy->pid_ns);
		if (!pid)
			goto bad_fork_cleanup_io;
	}

	p->pid = pid_nr(pid);
	p->tgid = p->pid;
	if (clone_flags & CLONE_THREAD)
		p->tgid = current->tgid;

	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? child_tidptr : NULL;
	/*
	 * Clear TID on mm_release()?
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? child_tidptr : NULL;
#ifdef CONFIG_BLOCK
	p->plug = NULL;
#endif
#ifdef CONFIG_FUTEX
	p->robust_list = NULL;
#ifdef CONFIG_COMPAT
	p->compat_robust_list = NULL;
#endif
	INIT_LIST_HEAD(&p->pi_state_list);
	p->pi_state_cache = NULL;
#endif
	uprobe_copy_process(p);
	/*
	 * sigaltstack should be cleared when sharing the same VM
	 */
	if ((clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM)
		p->sas_ss_sp = p->sas_ss_size = 0;

	/*
	 * Syscall tracing and stepping should be turned off in the
	 * child regardless of CLONE_PTRACE.
	 */
	user_disable_single_step(p);
	clear_tsk_thread_flag(p, TIF_SYSCALL_TRACE);
#ifdef TIF_SYSCALL_EMU
	clear_tsk_thread_flag(p, TIF_SYSCALL_EMU);
#endif
	clear_all_latency_tracing(p);

	/* ok, now we should be set up.. */
	if (clone_flags & CLONE_THREAD)
		p->exit_signal = -1;
	else if (clone_flags & CLONE_PARENT)
		p->exit_signal = current->group_leader->exit_signal;
	else
		p->exit_signal = (clone_flags & CSIGNAL);

	p->pdeath_signal = 0;
	p->exit_state = 0;

	p->nr_dirtied = 0;
	p->nr_dirtied_pause = 128 >> (PAGE_SHIFT - 10);
	p->dirty_paused_when = 0;

	/*
	 * Ok, make it visible to the rest of the system.
	 * We dont wake it up yet.
	 */
	p->group_leader = p;
	INIT_LIST_HEAD(&p->thread_group);
	p->task_works = NULL;

	/* Now that the task is set up, run cgroup callbacks if
	 * necessary. We need to run them before the task is visible
	 * on the tasklist. */
	cgroup_fork_callbacks(p);
	cgroup_callbacks_done = 1;

	/* Need tasklist lock for parent etc handling! */
	write_lock_irq(&tasklist_lock);

	/* CLONE_PARENT re-uses the old parent */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) {
		p->real_parent = current->real_parent;
		p->parent_exec_id = current->parent_exec_id;
	} else {
		p->real_parent = current;
		p->parent_exec_id = current->self_exec_id;
	}

	spin_lock(&current->sighand->siglock);

	/*
	 * Process group and session signals need to be delivered to just the
	 * parent before the fork or both the parent and the child after the
	 * fork. Restart if a signal comes in before we add the new process to
	 * it's process group.
	 * A fatal signal pending means that current will exit, so the new
	 * thread can't slip out of an OOM kill (or normal SIGKILL).
	*/
	recalc_sigpending();
	if (signal_pending(current)) {
		spin_unlock(&current->sighand->siglock);
		write_unlock_irq(&tasklist_lock);
		retval = -ERESTARTNOINTR;
		goto bad_fork_free_pid;
	}

	if (clone_flags & CLONE_THREAD) {
		current->signal->nr_threads++;
		atomic_inc(&current->signal->live);
		atomic_inc(&current->signal->sigcnt);
		p->group_leader = current->group_leader;
		list_add_tail_rcu(&p->thread_group, &p->group_leader->thread_group);
	}

	if (likely(p->pid)) {
		ptrace_init_task(p, (clone_flags & CLONE_PTRACE) || trace);

		if (thread_group_leader(p)) {
			if (is_child_reaper(pid))
				p->nsproxy->pid_ns->child_reaper = p;

			p->signal->leader_pid = pid;
			p->signal->tty = tty_kref_get(current->signal->tty);
			attach_pid(p, PIDTYPE_PGID, task_pgrp(current));
			attach_pid(p, PIDTYPE_SID, task_session(current));
			list_add_tail(&p->sibling, &p->real_parent->children);
			list_add_tail_rcu(&p->tasks, &init_task.tasks);
			__this_cpu_inc(process_counts);
		}
		attach_pid(p, PIDTYPE_PID, pid);
		nr_threads++;
	}

	total_forks++;
	spin_unlock(&current->sighand->siglock);
	write_unlock_irq(&tasklist_lock);
	proc_fork_connector(p);
	cgroup_post_fork(p);
	if (clone_flags & CLONE_THREAD)
		threadgroup_change_end(current);
	perf_event_fork(p);

	trace_task_newtask(p, clone_flags);

	return p;

bad_fork_free_pid:
	if (pid != &init_struct_pid)
		free_pid(pid);
bad_fork_cleanup_io:
	if (p->io_context)
		exit_io_context(p);
bad_fork_cleanup_namespaces:
	if (unlikely(clone_flags & CLONE_NEWPID))
		pid_ns_release_proc(p->nsproxy->pid_ns);
	exit_task_namespaces(p);
bad_fork_cleanup_mm:
	if (p->mm)
		mmput(p->mm);
bad_fork_cleanup_signal:
	if (!(clone_flags & CLONE_THREAD))
		free_signal_struct(p->signal);
bad_fork_cleanup_sighand:
	__cleanup_sighand(p->sighand);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup_semundo:
	exit_sem(p);
bad_fork_cleanup_audit:
	audit_free(p);
bad_fork_cleanup_policy:
	perf_event_free_task(p);
#ifdef CONFIG_NUMA
	mpol_put(p->mempolicy);
bad_fork_cleanup_cgroup:
#endif
	if (clone_flags & CLONE_THREAD)
		threadgroup_change_end(current);
	cgroup_exit(p, cgroup_callbacks_done);
	delayacct_tsk_free(p);
	module_put(task_thread_info(p)->exec_domain->module);
bad_fork_cleanup_count:
	atomic_dec(&p->cred->user->processes);
	exit_creds(p);
bad_fork_free:
	free_task(p);
fork_out:
	return ERR_PTR(retval);
}

/** 20150801
 * idle task를 위한 pt_regs를 받아 0으로 초기화 한다.
 **/
noinline struct pt_regs * __cpuinit __attribute__((weak)) idle_regs(struct pt_regs *regs)
{
	memset(regs, 0, sizeof(struct pt_regs));
	return regs;
}

/** 20150118
 * taks의 pid_link 포인터를 받아와 PIDTYPE만큼 돌며 초기화 한다.
 **/
static inline void init_idle_pids(struct pid_link *links)
{
	enum pid_type type;

	/** 20150801
	 * task의 pids를 순회하며 각 pid_link를 초기화 한다.
	 *   hlist_node를 초기화 하고, struct pid를 init_struct_pid로 지정한다.
	 **/
	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&links[type].node); /* not really needed */
		links[type].pid = &init_struct_pid;
	}
}

/** 20150118
 * 지정된 cpu를 위한 idle thread를 생성하고, idle thread로 지정한다.
 **/
struct task_struct * __cpuinit fork_idle(int cpu)
{
	struct task_struct *task;
	struct pt_regs regs;

	/** 20150118
	 * current task를 복사해 새로운 task_struct를 생성한다.
	 * pt_regs는 idle_regs로 설정하고 struct pid는 init_struct_pid를 지정한다.
	 **/
	task = copy_process(CLONE_VM, 0, idle_regs(&regs), 0, NULL,
			    &init_struct_pid, 0);
	if (!IS_ERR(task)) {
		/** 20150801
		 * task의 pids를 초기화 한다.
		 **/
		init_idle_pids(task->pids);
		/** 20150117
		 * cpu에 해당하는 rq의 idle thread로 task를 지정한다.
		 **/
		init_idle(task, cpu);
	}

	/** 20150117
	 * 생성한 task를 리턴한다.
	 **/
	return task;
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 */
long do_fork(unsigned long clone_flags,
	      unsigned long stack_start,
	      struct pt_regs *regs,
	      unsigned long stack_size,
	      int __user *parent_tidptr,
	      int __user *child_tidptr)
{
	struct task_struct *p;
	int trace = 0;
	long nr;

	/*
	 * Do some preliminary argument and permissions checking before we
	 * actually start allocating stuff
	 */
	/** 20160312
	 * clone_flags에 CLONE_NEWUSER 속성이 있다면 처리
	 *
	 * 현재 커널에서 사용 중인 곳은 없음.
	 **/
	if (clone_flags & CLONE_NEWUSER) {
		if (clone_flags & CLONE_THREAD)
			return -EINVAL;
		/* hopefully this check will go away when userns support is
		 * complete
		 */
		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SETUID) ||
				!capable(CAP_SETGID))
			return -EPERM;
	}

	/*
	 * Determine whether and which event to report to ptracer.  When
	 * called from kernel_thread or CLONE_UNTRACED is explicitly
	 * requested, no event is reported; otherwise, report if the event
	 * for the type of forking is enabled.
	 */
	/** 20160312
	 * ptrace 이벤트 관련 코드. 생략.
	 **/
	if (likely(user_mode(regs)) && !(clone_flags & CLONE_UNTRACED)) {
		if (clone_flags & CLONE_VFORK)
			trace = PTRACE_EVENT_VFORK;
		else if ((clone_flags & CSIGNAL) != SIGCHLD)
			trace = PTRACE_EVENT_CLONE;
		else
			trace = PTRACE_EVENT_FORK;

		if (likely(!ptrace_event_enabled(current, trace)))
			trace = 0;
	}

	/** 20160312
	 **/
	p = copy_process(clone_flags, stack_start, regs, stack_size,
			 child_tidptr, NULL, trace);
	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	if (!IS_ERR(p)) {
		struct completion vfork;

		trace_sched_process_fork(current, p);

		nr = task_pid_vnr(p);

		if (clone_flags & CLONE_PARENT_SETTID)
			put_user(nr, parent_tidptr);

		if (clone_flags & CLONE_VFORK) {
			p->vfork_done = &vfork;
			init_completion(&vfork);
			get_task_struct(p);
		}

		wake_up_new_task(p);

		/* forking complete and child started to run, tell ptracer */
		if (unlikely(trace))
			ptrace_event(trace, nr);

		if (clone_flags & CLONE_VFORK) {
			if (!wait_for_vfork_done(p, &vfork))
				ptrace_event(PTRACE_EVENT_VFORK_DONE, nr);
		}
	} else {
		nr = PTR_ERR(p);
	}
	return nr;
}

#ifndef ARCH_MIN_MMSTRUCT_ALIGN
#define ARCH_MIN_MMSTRUCT_ALIGN 0
#endif

static void sighand_ctor(void *data)
{
	struct sighand_struct *sighand = data;

	spin_lock_init(&sighand->siglock);
	init_waitqueue_head(&sighand->signalfd_wqh);
}

/** 20150207
 * process 관련 kmem_cache들을 생성한다.
 **/
void __init proc_caches_init(void)
{
	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_DESTROY_BY_RCU|
			SLAB_NOTRACK, sighand_ctor);
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK, NULL);
	files_cachep = kmem_cache_create("files_cache",
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK, NULL);
	fs_cachep = kmem_cache_create("fs_cache",
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK, NULL);
	/*
	 * FIXME! The "sizeof(struct mm_struct)" currently includes the
	 * whole struct cpumask for the OFFSTACK case. We could change
	 * this to *only* allocate as much of it as required by the
	 * maximum number of CPU's we can ever have.  The cpumask_allocation
	 * is at the end of the structure, exactly for that reason.
	 */
	mm_cachep = kmem_cache_create("mm_struct",
			sizeof(struct mm_struct), ARCH_MIN_MMSTRUCT_ALIGN,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK, NULL);
	vm_area_cachep = KMEM_CACHE(vm_area_struct, SLAB_PANIC);
	/** 20150207
	 * VMA 관련 초기화.
	 **/
	mmap_init();
	/** 20150214
	 * nsproxy cache 초기화 : kmem_cache 생성.
	 **/
	nsproxy_cache_init();
}

/*
 * Check constraints on flags passed to the unshare system call.
 */
static int check_unshare_flags(unsigned long unshare_flags)
{
	if (unshare_flags & ~(CLONE_THREAD|CLONE_FS|CLONE_NEWNS|CLONE_SIGHAND|
				CLONE_VM|CLONE_FILES|CLONE_SYSVSEM|
				CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWNET))
		return -EINVAL;
	/*
	 * Not implemented, but pretend it works if there is nothing to
	 * unshare. Note that unsharing CLONE_THREAD or CLONE_SIGHAND
	 * needs to unshare vm.
	 */
	if (unshare_flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) {
		/* FIXME: get_task_mm() increments ->mm_users */
		if (atomic_read(&current->mm->mm_users) > 1)
			return -EINVAL;
	}

	return 0;
}

/*
 * Unshare the filesystem structure if it is being shared
 */
static int unshare_fs(unsigned long unshare_flags, struct fs_struct **new_fsp)
{
	struct fs_struct *fs = current->fs;

	if (!(unshare_flags & CLONE_FS) || !fs)
		return 0;

	/* don't need lock here; in the worst case we'll do useless copy */
	if (fs->users == 1)
		return 0;

	*new_fsp = copy_fs_struct(fs);
	if (!*new_fsp)
		return -ENOMEM;

	return 0;
}

/*
 * Unshare file descriptor table if it is being shared
 */
static int unshare_fd(unsigned long unshare_flags, struct files_struct **new_fdp)
{
	struct files_struct *fd = current->files;
	int error = 0;

	if ((unshare_flags & CLONE_FILES) &&
	    (fd && atomic_read(&fd->count) > 1)) {
		*new_fdp = dup_fd(fd, &error);
		if (!*new_fdp)
			return error;
	}

	return 0;
}

/*
 * unshare allows a process to 'unshare' part of the process
 * context which was originally shared using clone.  copy_*
 * functions used by do_fork() cannot be used here directly
 * because they modify an inactive task_struct that is being
 * constructed. Here we are modifying the current, active,
 * task_struct.
 */
SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
	struct fs_struct *fs, *new_fs = NULL;
	struct files_struct *fd, *new_fd = NULL;
	struct nsproxy *new_nsproxy = NULL;
	int do_sysvsem = 0;
	int err;

	err = check_unshare_flags(unshare_flags);
	if (err)
		goto bad_unshare_out;

	/*
	 * If unsharing namespace, must also unshare filesystem information.
	 */
	if (unshare_flags & CLONE_NEWNS)
		unshare_flags |= CLONE_FS;
	/*
	 * CLONE_NEWIPC must also detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.
	 */
	if (unshare_flags & (CLONE_NEWIPC|CLONE_SYSVSEM))
		do_sysvsem = 1;
	err = unshare_fs(unshare_flags, &new_fs);
	if (err)
		goto bad_unshare_out;
	err = unshare_fd(unshare_flags, &new_fd);
	if (err)
		goto bad_unshare_cleanup_fs;
	err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy, new_fs);
	if (err)
		goto bad_unshare_cleanup_fd;

	if (new_fs || new_fd || do_sysvsem || new_nsproxy) {
		if (do_sysvsem) {
			/*
			 * CLONE_SYSVSEM is equivalent to sys_exit().
			 */
			exit_sem(current);
		}

		if (new_nsproxy) {
			switch_task_namespaces(current, new_nsproxy);
			new_nsproxy = NULL;
		}

		task_lock(current);

		if (new_fs) {
			fs = current->fs;
			spin_lock(&fs->lock);
			current->fs = new_fs;
			if (--fs->users)
				new_fs = NULL;
			else
				new_fs = fs;
			spin_unlock(&fs->lock);
		}

		if (new_fd) {
			fd = current->files;
			current->files = new_fd;
			new_fd = fd;
		}

		task_unlock(current);
	}

	if (new_nsproxy)
		put_nsproxy(new_nsproxy);

bad_unshare_cleanup_fd:
	if (new_fd)
		put_files_struct(new_fd);

bad_unshare_cleanup_fs:
	if (new_fs)
		free_fs_struct(new_fs);

bad_unshare_out:
	return err;
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */

int unshare_files(struct files_struct **displaced)
{
	struct task_struct *task = current;
	struct files_struct *copy = NULL;
	int error;

	error = unshare_fd(CLONE_FILES, &copy);
	if (error || !copy) {
		*displaced = NULL;
		return error;
	}
	*displaced = task->files;
	task_lock(task);
	task->files = copy;
	task_unlock(task);
	return 0;
}
