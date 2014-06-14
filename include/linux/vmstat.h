#ifndef _LINUX_VMSTAT_H
#define _LINUX_VMSTAT_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/vm_event_item.h>
#include <linux/atomic.h>

extern int sysctl_stat_interval;

#ifdef CONFIG_VM_EVENT_COUNTERS
/*
 * Light weight per cpu counter implementation.
 *
 * Counters should only be incremented and no critical kernel component
 * should rely on the counter values.
 *
 * Counters are handled completely inline. On many platforms the code
 * generated will simply be the increment of a global address.
 */

struct vm_event_state {
	unsigned long event[NR_VM_EVENT_ITEMS];
};

DECLARE_PER_CPU(struct vm_event_state, vm_event_states);

/** 20140104    
 * percpu vm event states를 증가시킨다.
 **/
static inline void __count_vm_event(enum vm_event_item item)
{
	__this_cpu_inc(vm_event_states.event[item]);
}

/** 20131207
 * percpu로 존재하는 vm_event_states의 event[item]를 하나씩 증가
 */
static inline void count_vm_event(enum vm_event_item item)
{
	this_cpu_inc(vm_event_states.event[item]);
}

/** 20130831    
 * percpu 변수인 vm_event_states 의 event 중 item에 해당하는 값에 delta 만큼을 더한다.
 * __count_vm_events : non-atomic 버전
 * count_vm_events : atomic 버전
 **/
static inline void __count_vm_events(enum vm_event_item item, long delta)
{
	__this_cpu_add(vm_event_states.event[item], delta);
}

static inline void count_vm_events(enum vm_event_item item, long delta)
{
	this_cpu_add(vm_event_states.event[item], delta);
}

extern void all_vm_events(unsigned long *);
#ifdef CONFIG_HOTPLUG
extern void vm_events_fold_cpu(int cpu);
#else
static inline void vm_events_fold_cpu(int cpu)
{
}
#endif

#else

/* Disable counters */
static inline void count_vm_event(enum vm_event_item item)
{
}
static inline void count_vm_events(enum vm_event_item item, long delta)
{
}
static inline void __count_vm_event(enum vm_event_item item)
{
}
static inline void __count_vm_events(enum vm_event_item item, long delta)
{
}
static inline void all_vm_events(unsigned long *ret)
{
}
static inline void vm_events_fold_cpu(int cpu)
{
}

#endif /* CONFIG_VM_EVENT_COUNTERS */

/** 20131005    
 * item##_NORMAL - ZONE_NORMAL + zone_idx(zone)의 의미는 ???
 **/
/** 20140111
 * zone에서 발생한 event에 대한 카운팅을 해당 cpu에 반영한다.
 **/
#define __count_zone_vm_events(item, zone, delta) \
		__count_vm_events(item##_NORMAL - ZONE_NORMAL + \
		zone_idx(zone), delta)

/*
 * Zone based page accounting with per cpu differentials.
 */
extern atomic_long_t vm_stat[NR_VM_ZONE_STAT_ITEMS];

/** 20130831    
 * item에 해당하는 vm_stat의 값을 x만큼 증가
 **/
static inline void zone_page_state_add(long x, struct zone *zone,
				 enum zone_stat_item item)
{
	/** 20130831    
	 * zone->vm_stat[item], vm_stat[item] 각각에 atomic연산으로 x를 더한다.
	 **/
	atomic_long_add(x, &zone->vm_stat[item]);
	atomic_long_add(x, &vm_stat[item]);
}

/** 20130914
전역 vm_stat[item]의 값을 리턴
**/
static inline unsigned long global_page_state(enum zone_stat_item item)
{
	long x = atomic_long_read(&vm_stat[item]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

/** 20130914
지정된 zone 에서 item에 해당하는 vm_stat을 읽어서 리턴한다.
(0보다 작을 경우 0으로 조정)
**/
static inline unsigned long zone_page_state(struct zone *zone,
					enum zone_stat_item item)
{
	long x = atomic_long_read(&zone->vm_stat[item]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

/*
 * More accurate version that also considers the currently pending
 * deltas. For that we need to loop over all cpus to find the current
 * deltas. There is no synchronization so the result cannot be
 * exactly accurate either.
 */
/** 20131116    
 * item에 해당하는 현재의 zone_page_state 값에 percpu 값을 반영시켜 계산한다.
 **/
static inline unsigned long zone_page_state_snapshot(struct zone *zone,
					enum zone_stat_item item)
{
	/** 20131116    
	 * vm_stat에서 item에 해당하는 값을 읽어 x에 저장한다
	 **/
	long x = atomic_long_read(&zone->vm_stat[item]);

#ifdef CONFIG_SMP
	int cpu;
	/** 20131116    
	 * 각 cpu들을 순회하면서 zone->pageset의 vm_stat_diff[item] 값을 x에 반영시킨다.
	 **/
	for_each_online_cpu(cpu)
		x += per_cpu_ptr(zone->pageset, cpu)->vm_stat_diff[item];

	/** 20131116    
	 * 최하값은 0.
	 **/
	if (x < 0)
		x = 0;
#endif
	return x;
}

extern unsigned long global_reclaimable_pages(void);
extern unsigned long zone_reclaimable_pages(struct zone *zone);

#ifdef CONFIG_NUMA
/*
 * Determine the per node value of a stat item. This function
 * is called frequently in a NUMA machine, so try to be as
 * frugal as possible.
 */
static inline unsigned long node_page_state(int node,
				 enum zone_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;

	return
#ifdef CONFIG_ZONE_DMA
		zone_page_state(&zones[ZONE_DMA], item) +
#endif
#ifdef CONFIG_ZONE_DMA32
		zone_page_state(&zones[ZONE_DMA32], item) +
#endif
#ifdef CONFIG_HIGHMEM
		zone_page_state(&zones[ZONE_HIGHMEM], item) +
#endif
		zone_page_state(&zones[ZONE_NORMAL], item) +
		zone_page_state(&zones[ZONE_MOVABLE], item);
}

extern void zone_statistics(struct zone *, struct zone *, gfp_t gfp);

#else

#define node_page_state(node, item) global_page_state(item)
/** 20131005    
 * NUMA가 아닐 때는 NULL.
 **/
#define zone_statistics(_zl, _z, gfp) do { } while (0)

#endif /* CONFIG_NUMA */

#define add_zone_page_state(__z, __i, __d) mod_zone_page_state(__z, __i, __d)
#define sub_zone_page_state(__z, __i, __d) mod_zone_page_state(__z, __i, -(__d))

extern void inc_zone_state(struct zone *, enum zone_stat_item);

#ifdef CONFIG_SMP
void __mod_zone_page_state(struct zone *, enum zone_stat_item item, int);
void __inc_zone_page_state(struct page *, enum zone_stat_item);
void __dec_zone_page_state(struct page *, enum zone_stat_item);

void mod_zone_page_state(struct zone *, enum zone_stat_item, int);
void inc_zone_page_state(struct page *, enum zone_stat_item);
void dec_zone_page_state(struct page *, enum zone_stat_item);

extern void inc_zone_state(struct zone *, enum zone_stat_item);
extern void __inc_zone_state(struct zone *, enum zone_stat_item);
extern void dec_zone_state(struct zone *, enum zone_stat_item);
extern void __dec_zone_state(struct zone *, enum zone_stat_item);

void refresh_cpu_vm_stats(int);
void refresh_zone_stat_thresholds(void);

int calculate_pressure_threshold(struct zone *zone);
int calculate_normal_threshold(struct zone *zone);
void set_pgdat_percpu_threshold(pg_data_t *pgdat,
				int (*calculate_pressure)(struct zone *));
#else /* CONFIG_SMP */

/*
 * We do not maintain differentials in a single processor configuration.
 * The functions directly modify the zone and global counters.
 */
static inline void __mod_zone_page_state(struct zone *zone,
			enum zone_stat_item item, int delta)
{
	zone_page_state_add(delta, zone, item);
}

static inline void __inc_zone_state(struct zone *zone, enum zone_stat_item item)
{
	atomic_long_inc(&zone->vm_stat[item]);
	atomic_long_inc(&vm_stat[item]);
}

static inline void __inc_zone_page_state(struct page *page,
			enum zone_stat_item item)
{
	__inc_zone_state(page_zone(page), item);
}

static inline void __dec_zone_state(struct zone *zone, enum zone_stat_item item)
{
	atomic_long_dec(&zone->vm_stat[item]);
	atomic_long_dec(&vm_stat[item]);
}

static inline void __dec_zone_page_state(struct page *page,
			enum zone_stat_item item)
{
	__dec_zone_state(page_zone(page), item);
}

/*
 * We only use atomic operations to update counters. So there is no need to
 * disable interrupts.
 */
#define inc_zone_page_state __inc_zone_page_state
#define dec_zone_page_state __dec_zone_page_state
#define mod_zone_page_state __mod_zone_page_state

#define set_pgdat_percpu_threshold(pgdat, callback) { }

static inline void refresh_cpu_vm_stats(int cpu) { }
static inline void refresh_zone_stat_thresholds(void) { }

#endif		/* CONFIG_SMP */

extern const char * const vmstat_text[];

#endif /* _LINUX_VMSTAT_H */
