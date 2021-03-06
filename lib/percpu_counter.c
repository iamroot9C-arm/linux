/*
 * Fast batching percpu counters.
 */

#include <linux/percpu_counter.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/debugobjects.h>

#ifdef CONFIG_HOTPLUG_CPU
/** 20150207
 * percpu_counter 전역 리스트.
 * spinlock으로 보호된다.
 **/
static LIST_HEAD(percpu_counters);
static DEFINE_SPINLOCK(percpu_counters_lock);
#endif

#ifdef CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER

static struct debug_obj_descr percpu_counter_debug_descr;

static int percpu_counter_fixup_free(void *addr, enum debug_obj_state state)
{
	struct percpu_counter *fbc = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		percpu_counter_destroy(fbc);
		debug_object_free(fbc, &percpu_counter_debug_descr);
		return 1;
	default:
		return 0;
	}
}

static struct debug_obj_descr percpu_counter_debug_descr = {
	.name		= "percpu_counter",
	.fixup_free	= percpu_counter_fixup_free,
};

static inline void debug_percpu_counter_activate(struct percpu_counter *fbc)
{
	debug_object_init(fbc, &percpu_counter_debug_descr);
	debug_object_activate(fbc, &percpu_counter_debug_descr);
}

static inline void debug_percpu_counter_deactivate(struct percpu_counter *fbc)
{
	debug_object_deactivate(fbc, &percpu_counter_debug_descr);
	debug_object_free(fbc, &percpu_counter_debug_descr);
}

#else	/* CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER */
/** 20150207
 **/
static inline void debug_percpu_counter_activate(struct percpu_counter *fbc)
{ }
static inline void debug_percpu_counter_deactivate(struct percpu_counter *fbc)
{ }
#endif	/* CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER */

void percpu_counter_set(struct percpu_counter *fbc, s64 amount)
{
	int cpu;

	raw_spin_lock(&fbc->lock);
	for_each_possible_cpu(cpu) {
		s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
		*pcount = 0;
	}
	fbc->count = amount;
	raw_spin_unlock(&fbc->lock);
}
EXPORT_SYMBOL(percpu_counter_set);

/** 20151219
 * percpu counter에 amount를 반영. threshold 구간은 batch에서 제공.
 * threshold에 도달하면 전역 카운터에 반영하고, 그렇지 않은 경우 percpu에만 반영.
 **/
void __percpu_counter_add(struct percpu_counter *fbc, s64 amount, s32 batch)
{
	s64 count;

	/** 20151219
	 * 선점불가 구간.
	 **/
	preempt_disable();
	/** 20151219
	 * percpu counter를 읽어와 합산.
	 *
	 * 누적 카운터가 batch 단위에 도달하면 전역 카운터에 반영하고 percpu를 초기화.
	 * 전역 카운터에 접근할 때는 spinlock으로 보호.
	 * 그렇지 않은 경우 percpu에만 반영.
	 **/
	count = __this_cpu_read(*fbc->counters) + amount;
	if (count >= batch || count <= -batch) {
		raw_spin_lock(&fbc->lock);
		fbc->count += count;
		__this_cpu_write(*fbc->counters, 0);
		raw_spin_unlock(&fbc->lock);
	} else {
		__this_cpu_write(*fbc->counters, count);
	}
	preempt_enable();
}
EXPORT_SYMBOL(__percpu_counter_add);

/*
 * Add up all the per-cpu counts, return the result.  This is a more accurate
 * but much slower version of percpu_counter_read_positive()
 */
/** 20150221
 * percpu_counter의 count에 percpu별로 유지하고 있던 변수까지 더해 리턴한다.
 *
 * 전역 counter에 합산시키기 때문에 spinlock으로 보호한다.
 * percpu_counter_read_positive보다 느리지만 보다 정확한 값을 리턴한다.
 **/
s64 __percpu_counter_sum(struct percpu_counter *fbc)
{
	s64 ret;
	int cpu;

	raw_spin_lock(&fbc->lock);
	ret = fbc->count;
	for_each_online_cpu(cpu) {
		s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
		ret += *pcount;
	}
	raw_spin_unlock(&fbc->lock);
	return ret;
}
EXPORT_SYMBOL(__percpu_counter_sum);

/** 20150207
 * percpu counter 초기화.
 *
 * count 항목을 amount로 초기화 하고, percpu인 counters를 할당한다.
 **/
int __percpu_counter_init(struct percpu_counter *fbc, s64 amount,
			  struct lock_class_key *key)
{
	raw_spin_lock_init(&fbc->lock);
	lockdep_set_class(&fbc->lock, key);
	/** 20150207
	 * 전체 count 초기화.
	 * percpu를 위한 메모리 할당.
	 **/
	fbc->count = amount;
	fbc->counters = alloc_percpu(s32);
	if (!fbc->counters)
		return -ENOMEM;

	debug_percpu_counter_activate(fbc);

#ifdef CONFIG_HOTPLUG_CPU
	INIT_LIST_HEAD(&fbc->list);
	spin_lock(&percpu_counters_lock);
	list_add(&fbc->list, &percpu_counters);
	spin_unlock(&percpu_counters_lock);
#endif
	return 0;
}
EXPORT_SYMBOL(__percpu_counter_init);

/** 20150822
 * percpu_counter 구조체를 제거하기 위한 작업을 한다.
 **/
void percpu_counter_destroy(struct percpu_counter *fbc)
{
	if (!fbc->counters)
		return;

	debug_percpu_counter_deactivate(fbc);

#ifdef CONFIG_HOTPLUG_CPU
	/** 20150822
	 * 전역 리스트에서 제거한다.
	 **/
	spin_lock(&percpu_counters_lock);
	list_del(&fbc->list);
	spin_unlock(&percpu_counters_lock);
#endif
	free_percpu(fbc->counters);
	fbc->counters = NULL;
}
EXPORT_SYMBOL(percpu_counter_destroy);

/** 20151219
 * percpu_counter batch count.
 **/
int percpu_counter_batch __read_mostly = 32;
EXPORT_SYMBOL(percpu_counter_batch);

static void compute_batch_value(void)
{
	int nr = num_online_cpus();

	percpu_counter_batch = max(32, nr*2);
}

static int __cpuinit percpu_counter_hotcpu_callback(struct notifier_block *nb,
					unsigned long action, void *hcpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	unsigned int cpu;
	struct percpu_counter *fbc;

	compute_batch_value();
	if (action != CPU_DEAD)
		return NOTIFY_OK;

	cpu = (unsigned long)hcpu;
	spin_lock(&percpu_counters_lock);
	list_for_each_entry(fbc, &percpu_counters, list) {
		s32 *pcount;
		unsigned long flags;

		raw_spin_lock_irqsave(&fbc->lock, flags);
		pcount = per_cpu_ptr(fbc->counters, cpu);
		fbc->count += *pcount;
		*pcount = 0;
		raw_spin_unlock_irqrestore(&fbc->lock, flags);
	}
	spin_unlock(&percpu_counters_lock);
#endif
	return NOTIFY_OK;
}

/*
 * Compare counter against given value.
 * Return 1 if greater, 0 if equal and -1 if less
 */
int percpu_counter_compare(struct percpu_counter *fbc, s64 rhs)
{
	s64	count;

	count = percpu_counter_read(fbc);
	/* Check to see if rough count will be sufficient for comparison */
	if (abs(count - rhs) > (percpu_counter_batch*num_online_cpus())) {
		if (count > rhs)
			return 1;
		else
			return -1;
	}
	/* Need to use precise count */
	count = percpu_counter_sum(fbc);
	if (count > rhs)
		return 1;
	else if (count < rhs)
		return -1;
	else
		return 0;
}
EXPORT_SYMBOL(percpu_counter_compare);

static int __init percpu_counter_startup(void)
{
	compute_batch_value();
	hotcpu_notifier(percpu_counter_hotcpu_callback, 0);
	return 0;
}
module_init(percpu_counter_startup);
