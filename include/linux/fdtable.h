/*
 * descriptor table internals; you almost certainly want file.h instead.
 */

#ifndef __LINUX_FDTABLE_H
#define __LINUX_FDTABLE_H

#include <linux/posix_types.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/fs.h>

#include <linux/atomic.h>

/*
 * The default fd array needs to be at least BITS_PER_LONG,
 * as this is the granularity returned by copy_fdset().
 */
/** 20160409    
 **/
#define NR_OPEN_DEFAULT BITS_PER_LONG

/** 20160409    
 * fdtable 구조체는 오픈된 여러 파일에 대한 정보를 관리한다.
 **/
struct fdtable {
	unsigned int max_fds;
	/** 20160409    
	 * fd는 상위 구조체인 files_struct의 fd_array 배열의 특정 멤버를 
	 **/
	struct file __rcu **fd;      /* current fd array */
	unsigned long *close_on_exec;
	unsigned long *open_fds;
	struct rcu_head rcu;
	struct fdtable *next;
};

static inline void __set_close_on_exec(int fd, struct fdtable *fdt)
{
	__set_bit(fd, fdt->close_on_exec);
}

static inline void __clear_close_on_exec(int fd, struct fdtable *fdt)
{
	__clear_bit(fd, fdt->close_on_exec);
}

static inline bool close_on_exec(int fd, const struct fdtable *fdt)
{
	return test_bit(fd, fdt->close_on_exec);
}

static inline void __set_open_fd(int fd, struct fdtable *fdt)
{
	__set_bit(fd, fdt->open_fds);
}

static inline void __clear_open_fd(int fd, struct fdtable *fdt)
{
	__clear_bit(fd, fdt->open_fds);
}

static inline bool fd_is_open(int fd, const struct fdtable *fdt)
{
	return test_bit(fd, fdt->open_fds);
}

/*
 * Open file table structure
 */
/** 20160409    
 * task에서 오픈한 파일 테이블
 **/
struct files_struct {
  /*
   * read mostly part
   */
	atomic_t count;
	/** 20160409    
	 * fdtab은 프로세스가 오픈한 파일 테이블.
	 * fdt는 마지막 fdtab을 참조하는듯???
	 *
	 * task는 files_struct 내부에 BITS_PER_LONG개의 fd를 관리하는 fdtable을
	 * 하나만 유지하고, 오픈 파일이 늘어날수록 fdtable을 늘리고 리스트로
	 * 연결시킨다.
	 **/
	struct fdtable __rcu *fdt;
	struct fdtable fdtab;
  /*
   * written part on a separate cache line in SMP
   */
	spinlock_t file_lock ____cacheline_aligned_in_smp;
	int next_fd;
	unsigned long close_on_exec_init[1];
	unsigned long open_fds_init[1];
	struct file __rcu * fd_array[NR_OPEN_DEFAULT];
};

/** 20160409    
 * rcu 포인터 변수인 fdt를 참조하는 매크로.
 * 정확한 위치(lock/unlock 사이)에서 참조가 이뤄졌는지 검사하는 매크로.
 * 여러 조건 중 하나만 만족하면 된다.
 **/
#define rcu_dereference_check_fdtable(files, fdtfd) \
	(rcu_dereference_check((fdtfd), \
			       lockdep_is_held(&(files)->file_lock) || \
			       atomic_read(&(files)->count) == 1 || \
			       rcu_my_thread_group_empty()))

/** 20160409    
 * struct files_struct에서 struct fdtable *인 fdt를 리턴하는 매크로.
 **/
#define files_fdtable(files) \
		(rcu_dereference_check_fdtable((files), (files)->fdt))

struct file_operations;
struct vfsmount;
struct dentry;

extern int expand_files(struct files_struct *, int nr);
extern void free_fdtable_rcu(struct rcu_head *rcu);
extern void __init files_defer_init(void);

static inline void free_fdtable(struct fdtable *fdt)
{
	call_rcu(&fdt->rcu, free_fdtable_rcu);
}

static inline struct file * fcheck_files(struct files_struct *files, unsigned int fd)
{
	struct file * file = NULL;
	struct fdtable *fdt = files_fdtable(files);

	if (fd < fdt->max_fds)
		file = rcu_dereference_check_fdtable(files, fdt->fd[fd]);
	return file;
}

/*
 * Check whether the specified fd has an open file.
 */
#define fcheck(fd)	fcheck_files(current->files, fd)

struct task_struct;

struct files_struct *get_files_struct(struct task_struct *);
void put_files_struct(struct files_struct *fs);
void reset_files_struct(struct files_struct *);
int unshare_files(struct files_struct **);
struct files_struct *dup_fd(struct files_struct *, int *);

extern struct kmem_cache *files_cachep;

#endif /* __LINUX_FDTABLE_H */
