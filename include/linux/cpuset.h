#ifndef _LINUX_CPUSET_H
#define _LINUX_CPUSET_H
/*
 *  cpuset interface
 *
 *  Copyright (C) 2003 BULL SA
 *  Copyright (C) 2004-2006 Silicon Graphics, Inc.
 *
 */

#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/cgroup.h>
#include <linux/mm.h>

/** 20130629    
 * vexpress에서 정의되어 있음.
 **/
#ifdef CONFIG_CPUSETS

extern int number_of_cpusets;	/* How many cpusets are defined in system? */

extern int cpuset_init(void);
extern void cpuset_init_smp(void);
extern void cpuset_update_active_cpus(bool cpu_online);
extern void cpuset_cpus_allowed(struct task_struct *p, struct cpumask *mask);
extern void cpuset_cpus_allowed_fallback(struct task_struct *p);
extern nodemask_t cpuset_mems_allowed(struct task_struct *p);
#define cpuset_current_mems_allowed (current->mems_allowed)
void cpuset_init_current_mems_allowed(void);
int cpuset_nodemask_valid_mems_allowed(nodemask_t *nodemask);

extern int __cpuset_node_allowed_softwall(int node, gfp_t gfp_mask);
extern int __cpuset_node_allowed_hardwall(int node, gfp_t gfp_mask);

/** 20130914
number_of_cpuset 은 부팅 직후 0
softwall과 hardwall 두가지가 있는데 차이는???
참고 : man cpuset
**/
static inline int cpuset_node_allowed_softwall(int node, gfp_t gfp_mask)
{
	return number_of_cpusets <= 1 ||
		__cpuset_node_allowed_softwall(node, gfp_mask);
}

/** 20131116    
 *
 * cpuset 참고
 * https://access.redhat.com/site/documentation/ko-KR/Red_Hat_Enterprise_Linux/6/html/Resource_Management_Guide/sec-cpuset.html
 **/
static inline int cpuset_node_allowed_hardwall(int node, gfp_t gfp_mask)
{
	return number_of_cpusets <= 1 ||
		__cpuset_node_allowed_hardwall(node, gfp_mask);
}

/** 20130928    
 * 추후 보기로 함
 **/
static inline int cpuset_zone_allowed_softwall(struct zone *z, gfp_t gfp_mask)
{
	return cpuset_node_allowed_softwall(zone_to_nid(z), gfp_mask);
}

/** 20131123    
 **/
static inline int cpuset_zone_allowed_hardwall(struct zone *z, gfp_t gfp_mask)
{
	return cpuset_node_allowed_hardwall(zone_to_nid(z), gfp_mask);
}

extern int cpuset_mems_allowed_intersects(const struct task_struct *tsk1,
					  const struct task_struct *tsk2);

#define cpuset_memory_pressure_bump() 				\
	do {							\
		if (cpuset_memory_pressure_enabled)		\
			__cpuset_memory_pressure_bump();	\
	} while (0)
extern int cpuset_memory_pressure_enabled;
extern void __cpuset_memory_pressure_bump(void);

extern const struct file_operations proc_cpuset_operations;
struct seq_file;
extern void cpuset_task_status_allowed(struct seq_file *m,
					struct task_struct *task);

extern int cpuset_mem_spread_node(void);
extern int cpuset_slab_spread_node(void);

static inline int cpuset_do_page_mem_spread(void)
{
	return current->flags & PF_SPREAD_PAGE;
}

static inline int cpuset_do_slab_mem_spread(void)
{
	return current->flags & PF_SPREAD_SLAB;
}

extern int current_cpuset_is_being_rebound(void);

extern void rebuild_sched_domains(void);

extern void cpuset_print_task_mems_allowed(struct task_struct *p);

/*
 * get_mems_allowed is required when making decisions involving mems_allowed
 * such as during page allocation. mems_allowed can be updated in parallel
 * and depending on the new value an operation can fail potentially causing
 * process failure. A retry loop with get_mems_allowed and put_mems_allowed
 * prevents these artificial failures.
 */
/** 20130914
read,write의 상호배제적으로 수행하기 위해서 sequence lock을 사용하는 부분으로..
	- read시 mems_alloed_seq의 값을 체크하여 다음 operation을 계속 할지 아니면 기다릴지를 확인.
**/
static inline unsigned int get_mems_allowed(void)
{
	return read_seqcount_begin(&current->mems_allowed_seq);
}

/*
 * If this returns false, the operation that took place after get_mems_allowed
 * may have failed. It is up to the caller to retry the operation if
 * appropriate.
 */
static inline bool put_mems_allowed(unsigned int seq)
{
	return !read_seqcount_retry(&current->mems_allowed_seq, seq);
}

static inline void set_mems_allowed(nodemask_t nodemask)
{
	task_lock(current);
	write_seqcount_begin(&current->mems_allowed_seq);
	current->mems_allowed = nodemask;
	write_seqcount_end(&current->mems_allowed_seq);
	task_unlock(current);
}

#else /* !CONFIG_CPUSETS */

static inline int cpuset_init(void) { return 0; }
static inline void cpuset_init_smp(void) {}

static inline void cpuset_update_active_cpus(bool cpu_online)
{
	partition_sched_domains(1, NULL, NULL);
}

static inline void cpuset_cpus_allowed(struct task_struct *p,
				       struct cpumask *mask)
{
	cpumask_copy(mask, cpu_possible_mask);
}

static inline void cpuset_cpus_allowed_fallback(struct task_struct *p)
{
}

static inline nodemask_t cpuset_mems_allowed(struct task_struct *p)
{
	return node_possible_map;
}

#define cpuset_current_mems_allowed (node_states[N_HIGH_MEMORY])
static inline void cpuset_init_current_mems_allowed(void) {}

static inline int cpuset_nodemask_valid_mems_allowed(nodemask_t *nodemask)
{
	return 1;
}

static inline int cpuset_node_allowed_softwall(int node, gfp_t gfp_mask)
{
	return 1;
}

static inline int cpuset_node_allowed_hardwall(int node, gfp_t gfp_mask)
{
	return 1;
}

static inline int cpuset_zone_allowed_softwall(struct zone *z, gfp_t gfp_mask)
{
	return 1;
}

static inline int cpuset_zone_allowed_hardwall(struct zone *z, gfp_t gfp_mask)
{
	return 1;
}

static inline int cpuset_mems_allowed_intersects(const struct task_struct *tsk1,
						 const struct task_struct *tsk2)
{
	return 1;
}

static inline void cpuset_memory_pressure_bump(void) {}

static inline void cpuset_task_status_allowed(struct seq_file *m,
						struct task_struct *task)
{
}

static inline int cpuset_mem_spread_node(void)
{
	return 0;
}

static inline int cpuset_slab_spread_node(void)
{
	return 0;
}

static inline int cpuset_do_page_mem_spread(void)
{
	return 0;
}

static inline int cpuset_do_slab_mem_spread(void)
{
	return 0;
}

static inline int current_cpuset_is_being_rebound(void)
{
	return 0;
}

static inline void rebuild_sched_domains(void)
{
	partition_sched_domains(1, NULL, NULL);
}

static inline void cpuset_print_task_mems_allowed(struct task_struct *p)
{
}

static inline void set_mems_allowed(nodemask_t nodemask)
{
}

static inline unsigned int get_mems_allowed(void)
{
	return 0;
}

static inline bool put_mems_allowed(unsigned int seq)
{
	return true;
}

#endif /* !CONFIG_CPUSETS */

#endif /* _LINUX_CPUSET_H */
