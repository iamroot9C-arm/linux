#ifndef _LINUX_PID_NS_H
#define _LINUX_PID_NS_H

#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/threads.h>
#include <linux/nsproxy.h>
#include <linux/kref.h>

/** 20150131    
 * 각 pidmap entry가 나타내는 구조체.
 *
 * nr_free :  사용 가능한 pid의 수를 기록한다.
 * page    :  pid bitmap용으로 할당받은 페이지를 가리킨다.
 **/
struct pidmap {
       atomic_t nr_free;
       void *page;
};

/** 20150131    
 * namespace에 속하는 pid를 비트맵으로 표시했을 때, 몇 개의 페이지가 필요한지 결정한다.
 *
 * PID_MAX_LIMIT을 (BITS_PER_PAGE)로 나눠 entry 수를 계산한다.
 *
 * DIV_ROUND_UP과 같은 의미.
 * ( 32768 + 32768 - 1 ) / 32768 = 1
 **/
#define PIDMAP_ENTRIES         ((PID_MAX_LIMIT + 8*PAGE_SIZE - 1)/PAGE_SIZE/8)

struct bsd_acct_struct;

/** 20150131    
 * 2.6대 버전에 새로 추가된 pid_namespace.
 * [참고] http://studyfoss.egloos.com/5242243
 *        Professional Linux Kernel Architecture Figure 2-5.
 *         Overview of data structures used to implement a namespace-aware representation of IDs.
 *
 * kref       : reference count를 관리하기 위해 사용된다.
 * pidmap     : 이 ns에 속한 pid를 관리하기 위한 bitmap.
 * last_pid   : 마지막으로 할당한 pid  번호를 저장한다.
 * pid_cachep : struct pid를 할당하기 위한 kmem_cache로 pid_namespace를 생성할 때 
 *              같이 생성한다.
 * level      : pid_namespace는 init_pid_ns부터 tree 형태의 계층 구조를 갖는데,
 *              이 ns가 몇 번째 level에 속하는지 정보를 나타낸다.
 * parent     : parent pid_namespace를 가리킨다.
 * proc_mnt   : 이 pid_namespace용으로 마운트된 vfsmount를 저장한다.
 *
 * struct pid 할당을 위해 alloc_pid()에 할당 받을 namespace를 전달한다.
 *
 * pid_namespace마다 struct pid를 할당하기 위한 kmem_cache를 가지고 있는데,
 * pid_namespace가 다름에 따라 할당 받아올 메모리 역시 분리되어 있기 때문이다.
 **/
struct pid_namespace {
	struct kref kref;
	struct pidmap pidmap[PIDMAP_ENTRIES];
	int last_pid;
	struct task_struct *child_reaper;
	struct kmem_cache *pid_cachep;
	unsigned int level;
	struct pid_namespace *parent;
#ifdef CONFIG_PROC_FS
	struct vfsmount *proc_mnt;
#endif
#ifdef CONFIG_BSD_PROCESS_ACCT
	struct bsd_acct_struct *bacct;
#endif
	kgid_t pid_gid;
	int hide_pid;
	int reboot;	/* group exit code if this pidns was rebooted */
};

extern struct pid_namespace init_pid_ns;

#ifdef CONFIG_PID_NS
static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	if (ns != &init_pid_ns)
		kref_get(&ns->kref);
	return ns;
}

extern struct pid_namespace *copy_pid_ns(unsigned long flags, struct pid_namespace *ns);
extern void free_pid_ns(struct kref *kref);
extern void zap_pid_ns_processes(struct pid_namespace *pid_ns);
extern int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);

static inline void put_pid_ns(struct pid_namespace *ns)
{
	if (ns != &init_pid_ns)
		kref_put(&ns->kref, free_pid_ns);
}

#else /* !CONFIG_PID_NS */
#include <linux/err.h>

static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	return ns;
}

static inline struct pid_namespace *
copy_pid_ns(unsigned long flags, struct pid_namespace *ns)
{
	if (flags & CLONE_NEWPID)
		ns = ERR_PTR(-EINVAL);
	return ns;
}

static inline void put_pid_ns(struct pid_namespace *ns)
{
}

static inline void zap_pid_ns_processes(struct pid_namespace *ns)
{
	BUG();
}

static inline int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd)
{
	return 0;
}
#endif /* CONFIG_PID_NS */

extern struct pid_namespace *task_active_pid_ns(struct task_struct *tsk);
void pidhash_init(void);
void pidmap_init(void);

#endif /* _LINUX_PID_NS_H */
