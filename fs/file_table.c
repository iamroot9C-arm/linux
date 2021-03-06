/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/eventpoll.h>
#include <linux/rcupdate.h>
#include <linux/mount.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/fsnotify.h>
#include <linux/sysctl.h>
#include <linux/lglock.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/task_work.h>
#include <linux/ima.h>

#include <linux/atomic.h>

#include "internal.h"

/* sysctl tunables... */
/** 20151219
 * files_init에서 전체 메모리량을 보고 update 시킨다.
 **/
struct files_stat_struct files_stat = {
	.max_files = NR_FILE
};

DEFINE_LGLOCK(files_lglock);

/* SLAB cache for file structures */
static struct kmem_cache *filp_cachep __read_mostly;

/** 20150221
 * percpu counter로 nr_files를 선언.
 * 
 * threshold를 넘어설 때 업데이트 되는 전역 카운터와 percpu 카운터가 존재.
 **/
static struct percpu_counter nr_files __cacheline_aligned_in_smp;

static void file_free_rcu(struct rcu_head *head)
{
	struct file *f = container_of(head, struct file, f_u.fu_rcuhead);

	put_cred(f->f_cred);
	kmem_cache_free(filp_cachep, f);
}

static inline void file_free(struct file *f)
{
	percpu_counter_dec(&nr_files);
	file_check_state(f);
	call_rcu(&f->f_u.fu_rcuhead, file_free_rcu);
}

/*
 * Return the total number of open files in the system
 */
/** 20151219
 * percpu_counter로 관리하는 파일 개수를 리턴한다.
 **/
static long get_nr_files(void)
{
	return percpu_counter_read_positive(&nr_files);
}

/*
 * Return the maximum number of open files in the system
 */
unsigned long get_max_files(void)
{
	return files_stat.max_files;
}
EXPORT_SYMBOL_GPL(get_max_files);

/*
 * Handle nr_files sysctl
 */
#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)
int proc_nr_files(ctl_table *table, int write,
                     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	files_stat.nr_files = get_nr_files();
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}
#else
int proc_nr_files(ctl_table *table, int write,
                     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return -ENOSYS;
}
#endif

/* Find an unused file structure and return a pointer to it.
 * Returns NULL, if there are no more free file structures or
 * we run out of memory.
 *
 * Be very careful using this.  You are responsible for
 * getting write access to any mount that you might assign
 * to this filp, if it is opened for write.  If this is not
 * done, you will imbalance int the mount's writer count
 * and a warning at __fput() time.
 */
/** 20151219
 * 시스템 limit에 도달하지 않았다면 file 구조체를 할당 받아 리턴한다.
 **/
struct file *get_empty_filp(void)
{
	const struct cred *cred = current_cred();
	static long old_max;
	struct file * f;

	/*
	 * Privileged users can go above max_files
	 */
	/** 20151219
	 * 시스템의 파일수가 limit에 도달했을 경우에는 ADMIN의 경우에만 추가 생성이 가능하다.
	 **/
	if (get_nr_files() >= files_stat.max_files && !capable(CAP_SYS_ADMIN)) {
		/*
		 * percpu_counters are inaccurate.  Do an expensive check before
		 * we go and fail.
		 */
		/** 20151219
		 * get_nr_files는 전역 카운트값만 반영하므로 에러 처리 전에
		 * 정확한 값으로 다시 확인한다.
		 **/
		if (percpu_counter_sum_positive(&nr_files) >= files_stat.max_files)
			goto over;
	}

	/** 20151219
	 * filp 캐시로부터 오브젝트를 할당 받는다.
	 **/
	f = kmem_cache_zalloc(filp_cachep, GFP_KERNEL);
	if (f == NULL)
		goto fail;

	/** 20151219
	 * nr_files 증가.
	 **/
	percpu_counter_inc(&nr_files);
	f->f_cred = get_cred(cred);
	if (security_file_alloc(f))
		goto fail_sec;

	/** 20151219
	 * filp의 멤버 초기화.
	 **/
	INIT_LIST_HEAD(&f->f_u.fu_list);
	atomic_long_set(&f->f_count, 1);
	rwlock_init(&f->f_owner.lock);
	spin_lock_init(&f->f_lock);
	eventpoll_init_file(f);
	/* f->f_version: 0 */
	return f;

over:
	/* Ran out of filps - report that */
	if (get_nr_files() > old_max) {
		pr_info("VFS: file-max limit %lu reached\n", get_max_files());
		old_max = get_nr_files();
	}
	goto fail;

fail_sec:
	file_free(f);
fail:
	return NULL;
}

/**
 * alloc_file - allocate and initialize a 'struct file'
 * @mnt: the vfsmount on which the file will reside
 * @dentry: the dentry representing the new file
 * @mode: the mode with which the new file will be opened
 * @fop: the 'struct file_operations' for the new file
 *
 * Use this instead of get_empty_filp() to get a new
 * 'struct file'.  Do so because of the same initialization
 * pitfalls reasons listed for init_file().  This is a
 * preferred interface to using init_file().
 *
 * If all the callers of init_file() are eliminated, its
 * code should be moved into this function.
 */
/** 20151219
 * file 구조체를 할당 받고,  전달받은 mode와 fop을 채운다.
 **/
struct file *alloc_file(struct path *path, fmode_t mode,
		const struct file_operations *fop)
{
	struct file *file;

	/** 20151219
	 * file 구조체를 할당 받는다.
	 **/
	file = get_empty_filp();
	if (!file)
		return NULL;

	/** 20151219
	 * 전달받은 argument로 file 구조체를 설정한다.
	 **/
	file->f_path = *path;
	file->f_mapping = path->dentry->d_inode->i_mapping;
	file->f_mode = mode;
	file->f_op = fop;

	/*
	 * These mounts don't really matter in practice
	 * for r/o bind mounts.  They aren't userspace-
	 * visible.  We do this for consistency, and so
	 * that we can do debugging checks at __fput()
	 */
	/** 20151219
	 * write 모드로 열렸고, special file이 아닌 경우 간단히 write access를 획득.
	 **/
	if ((mode & FMODE_WRITE) && !special_file(path->dentry->d_inode->i_mode)) {
		file_take_write(file);
		WARN_ON(mnt_clone_write(path->mnt));
	}
	/** 20151219
	 * readonly로 요청된 경우
	 **/
	if ((mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_inc(path->dentry->d_inode);
	return file;
}
EXPORT_SYMBOL(alloc_file);

/**
 * drop_file_write_access - give up ability to write to a file
 * @file: the file to which we will stop writing
 *
 * This is a central place which will give up the ability
 * to write to @file, along with access to write through
 * its vfsmount.
 */
static void drop_file_write_access(struct file *file)
{
	struct vfsmount *mnt = file->f_path.mnt;
	struct dentry *dentry = file->f_path.dentry;
	struct inode *inode = dentry->d_inode;

	put_write_access(inode);

	if (special_file(inode->i_mode))
		return;
	if (file_check_writeable(file) != 0)
		return;
	__mnt_drop_write(mnt);
	file_release_write(file);
}

/* the real guts of fput() - releasing the last reference to file
 */
/** 20160130
 *
 * file->f_op->release
 **/
static void __fput(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry;
	struct vfsmount *mnt = file->f_path.mnt;
	struct inode *inode = dentry->d_inode;

	might_sleep();

	fsnotify_close(file);
	/*
	 * The function eventpoll_release() should be the first called
	 * in the file cleanup chain.
	 */
	eventpoll_release(file);
	locks_remove_flock(file);

	if (unlikely(file->f_flags & FASYNC)) {
		if (file->f_op && file->f_op->fasync)
			file->f_op->fasync(-1, file, 0);
	}
	if (file->f_op && file->f_op->release)
		file->f_op->release(inode, file);
	security_file_free(file);
	ima_file_free(file);
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL &&
		     !(file->f_mode & FMODE_PATH))) {
		cdev_put(inode->i_cdev);
	}
	fops_put(file->f_op);
	put_pid(file->f_owner.pid);
	if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_dec(inode);
	if (file->f_mode & FMODE_WRITE)
		drop_file_write_access(file);
	file->f_path.dentry = NULL;
	file->f_path.mnt = NULL;
	file_free(file);
	dput(dentry);
	mntput(mnt);
}

/** 20160130
 * 전역변수 delayed_fput_list와 spinlock을 정의.
 **/
static DEFINE_SPINLOCK(delayed_fput_lock);
static LIST_HEAD(delayed_fput_list);
/** 20160130
 * fput에서 지연되어 리스트에 달려 있던 작업을 수행한다.
 *
 * delayed_fput_work으로 등록되어 실행되는 함수.
 **/
static void delayed_fput(struct work_struct *unused)
{
	/** 20160130
	 * 지역변수 head를 선언하고, delayed_fput_list의 내용을 이관시킨다.
	 **/
	LIST_HEAD(head);
	spin_lock_irq(&delayed_fput_lock);
	list_splice_init(&delayed_fput_list, &head);
	spin_unlock_irq(&delayed_fput_lock);
	/** 20160130
	 * 가져온 리스트가 비어있지 않다면 맨 앞의 entry부터 제거하고 __fput을
	 * 수행시킨다.
	 **/
	while (!list_empty(&head)) {
		struct file *f = list_first_entry(&head, struct file, f_u.fu_list);
		list_del_init(&f->f_u.fu_list);
		__fput(f);
	}
}

static void ____fput(struct callback_head *work)
{
	__fput(container_of(work, struct file, f_u.fu_rcuhead));
}

/*
 * If kernel thread really needs to have the final fput() it has done
 * to complete, call this.  The only user right now is the boot - we
 * *do* need to make sure our writes to binaries on initramfs has
 * not left us with opened struct file waiting for __fput() - execve()
 * won't work without that.  Please, don't add more callers without
 * very good reasons; in particular, never call that with locks
 * held and never call that from a thread that might need to do
 * some work on any kind of umount.
 */
/** 20160130
 * delayed_fput을 직접 호출해 지연되어 있던 fput 기능을 flush 시킨다.
 **/
void flush_delayed_fput(void)
{
	delayed_fput(NULL);
}

/** 20160130
 * delayed_fput를 수행할 delayed_fput_work 정의.
 **/
static DECLARE_WORK(delayed_fput_work, delayed_fput);

/** 20160130
 *
 * fput
 **/
void fput(struct file *file)
{
	if (atomic_long_dec_and_test(&file->f_count)) {
		struct task_struct *task = current;
		file_sb_list_del(file);
		/** 20160130
		 * interrupt context이거나 kernel thread에서 호출되었다면
		 * delayed_fput_list에 등록시키고 work으로 걸어 실행시킨다.
		 **/
		if (unlikely(in_interrupt() || task->flags & PF_KTHREAD)) {
			unsigned long flags;
			spin_lock_irqsave(&delayed_fput_lock, flags);
			list_add(&file->f_u.fu_list, &delayed_fput_list);
			schedule_work(&delayed_fput_work);
			spin_unlock_irqrestore(&delayed_fput_lock, flags);
			return;
		}
		/** 20160130
		 * file 구조체의 rcuhead에 ____fput을 지정한다.
		 **/
		init_task_work(&file->f_u.fu_rcuhead, ____fput);
		task_work_add(task, &file->f_u.fu_rcuhead, true);
	}
}

/*
 * synchronous analog of fput(); for kernel threads that might be needed
 * in some umount() (and thus can't use flush_delayed_fput() without
 * risking deadlocks), need to wait for completion of __fput() and know
 * for this specific struct file it won't involve anything that would
 * need them.  Use only if you really need it - at the very least,
 * don't blindly convert fput() by kernel thread to that.
 */
void __fput_sync(struct file *file)
{
	if (atomic_long_dec_and_test(&file->f_count)) {
		struct task_struct *task = current;
		file_sb_list_del(file);
		BUG_ON(!(task->flags & PF_KTHREAD));
		__fput(file);
	}
}

EXPORT_SYMBOL(fput);

struct file *fget(unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files;

	rcu_read_lock();
	file = fcheck_files(files, fd);
	if (file) {
		/* File object ref couldn't be taken */
		if (file->f_mode & FMODE_PATH ||
		    !atomic_long_inc_not_zero(&file->f_count))
			file = NULL;
	}
	rcu_read_unlock();

	return file;
}

EXPORT_SYMBOL(fget);

struct file *fget_raw(unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files;

	rcu_read_lock();
	file = fcheck_files(files, fd);
	if (file) {
		/* File object ref couldn't be taken */
		if (!atomic_long_inc_not_zero(&file->f_count))
			file = NULL;
	}
	rcu_read_unlock();

	return file;
}

EXPORT_SYMBOL(fget_raw);

/*
 * Lightweight file lookup - no refcnt increment if fd table isn't shared.
 *
 * You can use this instead of fget if you satisfy all of the following
 * conditions:
 * 1) You must call fput_light before exiting the syscall and returning control
 *    to userspace (i.e. you cannot remember the returned struct file * after
 *    returning to userspace).
 * 2) You must not call filp_close on the returned struct file * in between
 *    calls to fget_light and fput_light.
 * 3) You must not clone the current task in between the calls to fget_light
 *    and fput_light.
 *
 * The fput_needed flag returned by fget_light should be passed to the
 * corresponding fput_light.
 */
struct file *fget_light(unsigned int fd, int *fput_needed)
{
	struct file *file;
	struct files_struct *files = current->files;

	*fput_needed = 0;
	if (atomic_read(&files->count) == 1) {
		file = fcheck_files(files, fd);
		if (file && (file->f_mode & FMODE_PATH))
			file = NULL;
	} else {
		rcu_read_lock();
		file = fcheck_files(files, fd);
		if (file) {
			if (!(file->f_mode & FMODE_PATH) &&
			    atomic_long_inc_not_zero(&file->f_count))
				*fput_needed = 1;
			else
				/* Didn't get the reference, someone's freed */
				file = NULL;
		}
		rcu_read_unlock();
	}

	return file;
}

struct file *fget_raw_light(unsigned int fd, int *fput_needed)
{
	struct file *file;
	struct files_struct *files = current->files;

	*fput_needed = 0;
	if (atomic_read(&files->count) == 1) {
		file = fcheck_files(files, fd);
	} else {
		rcu_read_lock();
		file = fcheck_files(files, fd);
		if (file) {
			if (atomic_long_inc_not_zero(&file->f_count))
				*fput_needed = 1;
			else
				/* Didn't get the reference, someone's freed */
				file = NULL;
		}
		rcu_read_unlock();
	}

	return file;
}

void put_filp(struct file *file)
{
	if (atomic_long_dec_and_test(&file->f_count)) {
		security_file_free(file);
		file_sb_list_del(file);
		file_free(file);
	}
}

static inline int file_list_cpu(struct file *file)
{
#ifdef CONFIG_SMP
	return file->f_sb_list_cpu;
#else
	return smp_processor_id();
#endif
}

/* helper for file_sb_list_add to reduce ifdefs */
static inline void __file_sb_list_add(struct file *file, struct super_block *sb)
{
	struct list_head *list;
#ifdef CONFIG_SMP
	int cpu;
	cpu = smp_processor_id();
	file->f_sb_list_cpu = cpu;
	list = per_cpu_ptr(sb->s_files, cpu);
#else
	list = &sb->s_files;
#endif
	list_add(&file->f_u.fu_list, list);
}

/**
 * file_sb_list_add - add a file to the sb's file list
 * @file: file to add
 * @sb: sb to add it to
 *
 * Use this function to associate a file with the superblock of the inode it
 * refers to.
 */
void file_sb_list_add(struct file *file, struct super_block *sb)
{
	lg_local_lock(&files_lglock);
	__file_sb_list_add(file, sb);
	lg_local_unlock(&files_lglock);
}

/**
 * file_sb_list_del - remove a file from the sb's file list
 * @file: file to remove
 * @sb: sb to remove it from
 *
 * Use this function to remove a file from its superblock.
 */
void file_sb_list_del(struct file *file)
{
	if (!list_empty(&file->f_u.fu_list)) {
		lg_local_lock_cpu(&files_lglock, file_list_cpu(file));
		list_del_init(&file->f_u.fu_list);
		lg_local_unlock_cpu(&files_lglock, file_list_cpu(file));
	}
}

#ifdef CONFIG_SMP

/*
 * These macros iterate all files on all CPUs for a given superblock.
 * files_lglock must be held globally.
 */
#define do_file_list_for_each_entry(__sb, __file)		\
{								\
	int i;							\
	for_each_possible_cpu(i) {				\
		struct list_head *list;				\
		list = per_cpu_ptr((__sb)->s_files, i);		\
		list_for_each_entry((__file), list, f_u.fu_list)

#define while_file_list_for_each_entry				\
	}							\
}

#else

#define do_file_list_for_each_entry(__sb, __file)		\
{								\
	struct list_head *list;					\
	list = &(sb)->s_files;					\
	list_for_each_entry((__file), list, f_u.fu_list)

#define while_file_list_for_each_entry				\
}

#endif

/**
 *	mark_files_ro - mark all files read-only
 *	@sb: superblock in question
 *
 *	All files are marked read-only.  We don't care about pending
 *	delete files so this should be used in 'force' mode only.
 */
void mark_files_ro(struct super_block *sb)
{
	struct file *f;

	lg_global_lock(&files_lglock);
	do_file_list_for_each_entry(sb, f) {
		if (!S_ISREG(f->f_path.dentry->d_inode->i_mode))
		       continue;
		if (!file_count(f))
			continue;
		if (!(f->f_mode & FMODE_WRITE))
			continue;
		spin_lock(&f->f_lock);
		f->f_mode &= ~FMODE_WRITE;
		spin_unlock(&f->f_lock);
		if (file_check_writeable(f) != 0)
			continue;
		file_release_write(f);
		mnt_drop_write_file(f);
	} while_file_list_for_each_entry;
	lg_global_unlock(&files_lglock);
}

/** 20150221
 * file 관련 초기화를 수행한다.
 *
 * "filp" kmem_cache를 생성하고, max_files 등 관련 전역 변수를 초기화 한다.
 **/
void __init files_init(unsigned long mempages)
{ 
	unsigned long n;

	/** 20150214
	 * "filp" kmem_cache를 생성한다.
	 **/
	filp_cachep = kmem_cache_create("filp", sizeof(struct file), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	/*
	 * One file with associated inode and dcache is very roughly 1K.
	 * Per default don't use more than 10% of our memory for files. 
	 */ 

	/** 20150214
	 * inode와 dcache가 필요한 용량을 대략 1K로 잡고
	 * 한 페이지에 표현할 수 있는 갯수를 구하고,
	 * 제공된 mempages의 10%만큼 페이지를 사용한다고 했을 때 생성할 수 있는
	 * 파일의 수를 구한다.
	 *
	 * max_files의 최소치는 NR_FILE이다.
	 **/
	n = (mempages * (PAGE_SIZE / 1024)) / 10;
	files_stat.max_files = max_t(unsigned long, n, NR_FILE);
	/** 20150214
	 * files defer list init.
	 **/
	files_defer_init();
	/** 20150221
	 * local global lock init.
	 **/
	lg_lock_init(&files_lglock, "files_lglock");
	/** 20150221
	 * nr_files percpu counter 초기화.
	 **/
	percpu_counter_init(&nr_files, 0);
} 
