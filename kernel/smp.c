/*
 * Generic helpers for smp ipi calls
 *
 * (C) Jens Axboe <jens.axboe@oracle.com> 2008
 */
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <linux/smp.h>
#include <linux/cpu.h>

#include "smpboot.h"

#ifdef CONFIG_USE_GENERIC_SMP_HELPERS
/** 20140621
 * 전역변수 call_function 를 초기화.
 **/
static struct {
	struct list_head	queue;
	raw_spinlock_t		lock;
} call_function __cacheline_aligned_in_smp =
	{
		.queue		= LIST_HEAD_INIT(call_function.queue),
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(call_function.lock),
	};

enum {
	CSD_FLAG_LOCK		= 0x01,
};

/** 20140621
 * call_function_data.
 *	refs : 수신해야 할 cpu의 수를 계산해 넣는다. (송신자 제외)
 *	cpumask : 수신할 cpu 목록 bitmap.
 **/
struct call_function_data {
	struct call_single_data	csd;
	atomic_t		refs;
	cpumask_var_t		cpumask;
};

/** 20140621
 * percpu로 struct call_function_data 타입의 cfd_data를 정의한다.
 **/
static DEFINE_PER_CPU_SHARED_ALIGNED(struct call_function_data, cfd_data);

struct call_single_queue {
	struct list_head	list;
	raw_spinlock_t		lock;
};

/** 20140621
 * list와 lock을 보유한 call_single_queue per cpu 변수를 선언한다.
 *
 * call_function_init에서 초기화.
 * generic_exec_single에서 추가된다.
 * generic_smp_call_function_single_interrupt에서 호출된다.
 **/
static DEFINE_PER_CPU_SHARED_ALIGNED(struct call_single_queue, call_single_queue);

/** 20150111
 * action에 따라 call_function_data에 필요한 동작을 수행한다.
 **/
static int
hotplug_cfd(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	struct call_function_data *cfd = &per_cpu(cfd_data, cpu);

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		if (!zalloc_cpumask_var_node(&cfd->cpumask, GFP_KERNEL,
				cpu_to_node(cpu)))
			return notifier_from_errno(-ENOMEM);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:

	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		free_cpumask_var(cfd->cpumask);
		break;
#endif
	};

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata hotplug_cfd_notifier = {
	.notifier_call		= hotplug_cfd,
};

/** 20150111
 * call function 관련 초기화를 수행한다.
 **/
void __init call_function_init(void)
{
	/** 20150110
	 * 현재 cpu 번호를 받아온다.
	 **/
	void *cpu = (void *)(long)smp_processor_id();
	int i;

	/** 20150110
	 * cpu_possible_mask에 설정된 cpu를 순회.
	 **/
	for_each_possible_cpu(i) {
		/** 20150110
		 * per_cpu 전역변수로 선언된 call_single_queue에서 cpu에 해당하는
		 * queue를 받아와 spinlock과 list head를 초기화 한다.
		 **/
		struct call_single_queue *q = &per_cpu(call_single_queue, i);

		raw_spin_lock_init(&q->lock);
		INIT_LIST_HEAD(&q->list);
	}

	/** 20150111
	 * CPU_UP_PREPARE를 먼저 내려 hotplug_cfd를 준비한다.
	 * hotplug_cfd_notifier를 cpu notify block에 등록한다.
	 **/
	hotplug_cfd(&hotplug_cfd_notifier, CPU_UP_PREPARE, cpu);
	register_cpu_notifier(&hotplug_cfd_notifier);
}

/*
 * csd_lock/csd_unlock used to serialize access to per-cpu csd resources
 *
 * For non-synchronous ipi calls the csd can still be in use by the
 * previous function call. For multi-cpu calls its even more interesting
 * as we'll have to ensure no other cpu is observing our csd.
 */
/** 20140621
 * percpu 변수 call_single_data의 lock이 해제될 때까지 기다린다.
 **/
static void csd_lock_wait(struct call_single_data *data)
{
	while (data->flags & CSD_FLAG_LOCK)
		cpu_relax();
}

/** 20140621
 * call single data에 lock을 건다.
 **/
static void csd_lock(struct call_single_data *data)
{
	/** 20140621
	 * data의 lock이 해제될 때까지 기다린 후,
	 * lock이 풀리면 data에 lock을 건다.
	 **/
	csd_lock_wait(data);
	data->flags = CSD_FLAG_LOCK;

	/*
	 * prevent CPU from reordering the above assignment
	 * to ->flags with any subsequent assignments to other
	 * fields of the specified call_single_data structure:
	 */
	/** 20140621
	 * memory barrier를 통해 변경을 확보한다.
	 **/
	smp_mb();
}

/** 20140621
 * call single data에 lock을 해제한다.
 **/
static void csd_unlock(struct call_single_data *data)
{
	WARN_ON(!(data->flags & CSD_FLAG_LOCK));

	/*
	 * ensure we're all done before releasing data:
	 */
	/** 20140621
	 * 이전 동작의 memory access 명령 이후 다음 memory access가 진행되도록 한다.
	 **/
	smp_mb();

	data->flags &= ~CSD_FLAG_LOCK;
}

/*
 * Insert a previously allocated call_single_data element
 * for execution on the given CPU. data must already have
 * ->func, ->info, and ->flags set.
 */
/** 20140621
 * single IPI를 통해 cpu에 보낼 data를 target cpu의 queue에 등록하고,
 * architecture 별로 준비된 함수를 호출해 irq를 전달한다.
 **/
static
void generic_exec_single(int cpu, struct call_single_data *data, int wait)
{
	/** 20140621
	 * target cpu의 call_single_queue를 가져온다.
	 **/
	struct call_single_queue *dst = &per_cpu(call_single_queue, cpu);
	unsigned long flags;
	int ipi;

	/** 20140621
	 * atomic context를 확보한 상태에서,
	 * dst의 list가 비어있는지 여부를 ipi에 저장한다.
	 * 보낼 cpu의 data를 data를 받을 cpu의 queue list에 등록한다.
	 **/
	raw_spin_lock_irqsave(&dst->lock, flags);
	ipi = list_empty(&dst->list);
	list_add_tail(&data->list, &dst->list);
	raw_spin_unlock_irqrestore(&dst->lock, flags);

	/*
	 * The list addition should be visible before sending the IPI
	 * handler locks the list to pull the entry off it because of
	 * normal cache coherency rules implied by spinlocks.
	 *
	 * If IPIs can go out of order to the cache coherency protocol
	 * in an architecture, sufficient synchronisation should be added
	 * to arch code to make it appear to obey cache coherency WRT
	 * locking and barrier primitives. Generic code isn't really
	 * equipped to do the right thing...
	 */
	/** 20140621
	 * 기존의 dst list가 비어 있었다면 cpu에 IPI_CALL_FUNC_SINGLE irq를 날린다.
	 **/
	if (ipi)
		arch_send_call_function_single_ipi(cpu);

	/** 20140621
	 * wait이 필요할 경우 wait 해야 한다고 설정되어 있으면
	 * data의 lock이 해제될 때까지 기다린다.
	 **/
	if (wait)
		csd_lock_wait(data);
}

/*
 * Invoked by arch to handle an IPI for call function. Must be called with
 * interrupts disabled.
 */
/** 20140621
 * smp call function interrupt에 대한 핸들러 함수.
 * 여러 함수에 전달할 데이터(송신자의 percpu 변수)를 전역 queue에서 꺼낸다.
 **/
void generic_smp_call_function_interrupt(void)
{
	struct call_function_data *data;
	/** 20140621
	 * 현재 processor의 id를 불러온다.
	 **/
	int cpu = smp_processor_id();

	/*
	 * Shouldn't receive this interrupt on a cpu that is not yet online.
	 */
	WARN_ON_ONCE(!cpu_online(cpu));

	/*
	 * Ensure entry is visible on call_function_queue after we have
	 * entered the IPI. See comment in smp_call_function_many.
	 * If we don't have this, then we may miss an entry on the list
	 * and never get another IPI to process it.
	 */
	/** 20140621
	 * call_function.queue에 접근을 보장하는 data memory barrier.
	 **/
	smp_mb();

	/*
	 * It's ok to use list_for_each_rcu() here even though we may
	 * delete 'pos', since list_del_rcu() doesn't clear ->next
	 */
	/** 20140621
	 * call_function.queue의 csd.list에 등록된 멤버에 대한 loop.
	 **/
	list_for_each_entry_rcu(data, &call_function.queue, csd.list) {
		int refs;
		smp_call_func_t func;

		/*
		 * Since we walk the list without any locks, we might
		 * see an entry that was completed, removed from the
		 * list and is in the process of being reused.
		 *
		 * We must check that the cpu is in the cpumask before
		 * checking the refs, and both must be set before
		 * executing the callback on this cpu.
		 */

		/** 20140621
		 * 현재 cpu가 cpumask에 포함되어 있지 않다면 continue.
		 **/
		if (!cpumask_test_cpu(cpu, data->cpumask))
			continue;

		/** 20140621
		 * read memory barrier.
		 **/
		smp_rmb();

		/** 20140621
		 * call_function_data의 refs가 0이라면
		 * 메시지를 보낼 때 지정한 cpu들이 모두 꺼내갔다 판단해 continue.
		 **/
		if (atomic_read(&data->refs) == 0)
			continue;

		/** 20140621
		 * call_single_data 함수를 꺼내 info를 매개변수로 호출.
		 **/
		func = data->csd.func;		/* save for later warn */
		func(data->csd.info);

		/*
		 * If the cpu mask is not still set then func enabled
		 * interrupts (BUG), and this cpu took another smp call
		 * function interrupt and executed func(info) twice
		 * on this cpu.  That nested execution decremented refs.
		 */
		/** 20140621
		 * 현재 cpu가 data의 cpumask에 속하는지 여부를 검사하고,
		 * 설정되어 있다면 clear 시킨다.
		 **/
		if (!cpumask_test_and_clear_cpu(cpu, data->cpumask)) {
			WARN(1, "%pf enabled interrupts and double executed\n", func);
			continue;
		}

		/** 20140621
		 * call_function_data의 refs를 하나 원자적으로 감소시키고
		 * 결과값을 local 변수에 저장.
		 **/
		refs = atomic_dec_return(&data->refs);
		WARN_ON(refs < 0);

		/** 20140621
		 * 마지막 refs를 가져온 cpu는 이후 루틴을 수행함.
		 **/
		if (refs)
			continue;

		WARN_ON(!cpumask_empty(data->cpumask));

		/** 20140621
		 * spinlock으로 보호된 context에서 data를 queue에서 제거한다.
		 **/
		raw_spin_lock(&call_function.lock);
		list_del_rcu(&data->csd.list);
		raw_spin_unlock(&call_function.lock);

		/** 20140621
		 * data의 lock을 해제한다.
		 * 마지막 수행된 cpu가 동작을 마무리 하면 lock을 해제해
		 * smp_call_function_many 에서 송신 함수가 대기 중이라면 이후 루틴이 실행된다.
		 **/
		csd_unlock(&data->csd);
	}

}

/*
 * Invoked by arch to handle an IPI for call function single. Must be
 * called from the arch with interrupts disabled.
 */
/** 20140621
 * cpu 하나에 대해 IPI_CALL_FUNC 를 받은 경우 호출되는 핸들러.
 *
 * architecture specific 한 interrupt 핸들러에서 호출된다.
 * interrupt가 금지된 상태여야 한다.
 **/
void generic_smp_call_function_single_interrupt(void)
{
	/** 20140621
	 * percpu call_single_queue 에서 자신에 해당하는 변수 포인터를 가져온다.
	 *
	 * generic_exec_single에서 target의 cpuu번호를 넣어준다.
	 **/
	struct call_single_queue *q = &__get_cpu_var(call_single_queue);
	unsigned int data_flags;
	LIST_HEAD(list);

	/*
	 * Shouldn't receive this interrupt on a cpu that is not yet online.
	 */
	/** 20140621
	 * 수신 cpu는 online 상태여야 한다.
	 **/
	WARN_ON_ONCE(!cpu_online(smp_processor_id()));

	/** 20140621
	 * percpu의 list를 지역변수 list에 옮겨단다.
	 *
	 * queue는 spin lock을 걸어 atomic context를 보장한다.
	 * interrupt는 이미 disable되어 있다.
	 **/
	raw_spin_lock(&q->lock);
	list_replace_init(&q->list, &list);
	raw_spin_unlock(&q->lock);

	/** 20140621
	 * list가 빌 때까지 수행한다.
	 **/
	while (!list_empty(&list)) {
		struct call_single_data *data;

		/** 20140621
		 * list에서 call_single_data entry를 가져오고 list에서 제거한다.
		 **/
		data = list_entry(list.next, struct call_single_data, list);
		list_del(&data->list);

		/*
		 * 'data' can be invalid after this call if flags == 0
		 * (when called through generic_exec_single()),
		 * so save them away before making the call:
		 */
		/** 20140621
		 * data의 flags를 백업 받아두고, func을 info를 전달해 호출한다.
		 **/
		data_flags = data->flags;

		data->func(data->info);

		/*
		 * Unlocked CSDs are valid through generic_exec_single():
		 */
		/** 20140621
		 * flag에 CSD_FLAG_LOCK이 포함되어 있다면
		 * csd_unlock을 해준다. 
		 **/
		if (data_flags & CSD_FLAG_LOCK)
			csd_unlock(data);
	}
}

/** 20140621
 * struct call_single_data 타입의 percpu 변수 csd_data를 선언.
 **/
static DEFINE_PER_CPU_SHARED_ALIGNED(struct call_single_data, csd_data);

/*
 * smp_call_function_single - Run a function on a specific CPU
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait until function has completed on other CPUs.
 *
 * Returns 0 on success, else a negative status code.
 */
/** 20140621
 * 특정 cpu가 info를 매개변수로 func을 수행하게 한다.
 * 그 cpu가 자신인 경우 직접 호출하고, 다른 cpu인 경우 architecture별 함수를 호출한다.
 **/
int smp_call_function_single(int cpu, smp_call_func_t func, void *info,
			     int wait)
{
	struct call_single_data d = {
		.flags = 0,
	};
	unsigned long flags;
	int this_cpu;
	int err = 0;

	/*
	 * prevent preemption and reschedule on another processor,
	 * as well as CPU removal
	 */
	/** 20140621
	 * 선점 불가 상태로 현재 cpu의 번호를 가져온다.
	 **/
	this_cpu = get_cpu();

	/*
	 * Can deadlock when called with interrupts disabled.
	 * We allow cpu's that are not yet online though, as no one else can
	 * send smp call function interrupt to this cpu and as such deadlocks
	 * can't happen.
	 */
	WARN_ON_ONCE(cpu_online(this_cpu) && irqs_disabled()
		     && !oops_in_progress);

	/** 20140621
	 * 대상 cpu가 현재 cpu라면, 인터럽트를 막은 상태에서 func을 호출한다.
	 **/
	if (cpu == this_cpu) {
		local_irq_save(flags);
		func(info);
		local_irq_restore(flags);
	} else {
	/** 20140621
	 * 그렇지 않은 경우 percpu 변수 csd_data 중 현재 cpu에 해당하는 변수 위치를 가져와 조건에 따라 실행한다.
	 **/
		if ((unsigned)cpu < nr_cpu_ids && cpu_online(cpu)) {
			struct call_single_data *data = &d;

			/** 20140621
			 * wait이 설정되어 있지 않다면 percpu변수 csd_data의 현재 cpu 위치를 data로 전달한다.
			 **/
			if (!wait)
				data = &__get_cpu_var(csd_data);

			/** 20140621
			 * data의 lock을 걸고, func과 info를 저장한다.
			 **/
			csd_lock(data);

			data->func = func;
			data->info = info;
			/** 20140621
			 * 특정 cpu에 전달할 data를 채우고 IPI 메시지를 날린다.
			 **/
			generic_exec_single(cpu, data, wait);
		} else {
			/** 20140621
			 * 대상 CPU가 online이 아니다.
			 **/
			err = -ENXIO;	/* CPU not online */
		}
	}

	/** 20140621
	 * cpu 독점을 해제.
	 **/
	put_cpu();

	return err;
}
EXPORT_SYMBOL(smp_call_function_single);

/*
 * smp_call_function_any - Run a function on any of the given cpus
 * @mask: The mask of cpus it can run on.
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait until function has completed.
 *
 * Returns 0 on success, else a negative status code (if no cpus were online).
 * Note that @wait will be implicitly turned on in case of allocation failures,
 * since we fall back to on-stack allocation.
 *
 * Selection preference:
 *	1) current cpu if in @mask
 *	2) any cpu of current node if in @mask
 *	3) any other online cpu in @mask
 */
int smp_call_function_any(const struct cpumask *mask,
			  smp_call_func_t func, void *info, int wait)
{
	unsigned int cpu;
	const struct cpumask *nodemask;
	int ret;

	/* Try for same CPU (cheapest) */
	cpu = get_cpu();
	if (cpumask_test_cpu(cpu, mask))
		goto call;

	/* Try for same node. */
	nodemask = cpumask_of_node(cpu_to_node(cpu));
	for (cpu = cpumask_first_and(nodemask, mask); cpu < nr_cpu_ids;
	     cpu = cpumask_next_and(cpu, nodemask, mask)) {
		if (cpu_online(cpu))
			goto call;
	}

	/* Any online will do: smp_call_function_single handles nr_cpu_ids. */
	cpu = cpumask_any_and(mask, cpu_online_mask);
call:
	ret = smp_call_function_single(cpu, func, info, wait);
	put_cpu();
	return ret;
}
EXPORT_SYMBOL_GPL(smp_call_function_any);

/**
 * __smp_call_function_single(): Run a function on a specific CPU
 * @cpu: The CPU to run on.
 * @data: Pre-allocated and setup data structure
 * @wait: If true, wait until function has completed on specified CPU.
 *
 * Like smp_call_function_single(), but allow caller to pass in a
 * pre-allocated data structure. Useful for embedding @data inside
 * other structures, for instance.
 */
void __smp_call_function_single(int cpu, struct call_single_data *data,
				int wait)
{
	unsigned int this_cpu;
	unsigned long flags;

	this_cpu = get_cpu();
	/*
	 * Can deadlock when called with interrupts disabled.
	 * We allow cpu's that are not yet online though, as no one else can
	 * send smp call function interrupt to this cpu and as such deadlocks
	 * can't happen.
	 */
	WARN_ON_ONCE(cpu_online(smp_processor_id()) && wait && irqs_disabled()
		     && !oops_in_progress);

	if (cpu == this_cpu) {
		local_irq_save(flags);
		data->func(data->info);
		local_irq_restore(flags);
	} else {
		csd_lock(data);
		generic_exec_single(cpu, data, wait);
	}
	put_cpu();
}

/**
 * smp_call_function_many(): Run a function on a set of other CPUs.
 * @mask: The set of cpus to run on (only runs on online subset).
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait (atomically) until function has completed
 *        on other CPUs.
 *
 * If @wait is true, then returns once @func has returned.
 *
 * You must not call this function with disabled interrupts or from a
 * hardware interrupt handler or from a bottom half handler. Preemption
 * must be disabled when calling this function.
 */
/** 20140621
 * mask에 속한 cpu들에 대해 메시지를 날려 func(info)을 수행하도록 한다.
 * wait이 지정된 경우, 모든 함수의 수행이 끝날 때까지 기다려야 한다.
 *
 * disabled interrupts에서 호출하면 안 되고,
 * hardware interrupt 핸들러에서나 bottom-hal 핸들러에서는 호출하면 안 된다.
 * 이 함수를 호출하기 전에 항상 선점 불가 상태여야 한다.
 **/
void smp_call_function_many(const struct cpumask *mask,
			    smp_call_func_t func, void *info, bool wait)
{
	struct call_function_data *data;
	unsigned long flags;
	int refs, cpu, next_cpu, this_cpu = smp_processor_id();

	/*
	 * Can deadlock when called with interrupts disabled.
	 * We allow cpu's that are not yet online though, as no one else can
	 * send smp call function interrupt to this cpu and as such deadlocks
	 * can't happen.
	 */
	/** 20140621
	 * 현재 cpu가 online이고, irqs는 금지되어 있어야 하며,
	 * oops_in_progress가 실행 중이지 않은 상태,
	 * early_boot_irqs_disabled가 아니어야 한다.
	 *
	 * interrupt가 disabled된 상태에서는 deadlock이 발생할 수 있다.
	 **/
	WARN_ON_ONCE(cpu_online(this_cpu) && irqs_disabled()
		     && !oops_in_progress && !early_boot_irqs_disabled);

	/** 20140621
	 * 예를 들어 SMP에서 4개의 cpu가 모두 켜 있고, this_cpu가 0이고,
	 *
	 * mask에 4개 모두 속해 있다면 cpu는 1, next_cpu는 2가 될 것이다.
	 **/

	/* Try to fastpath.  So, what's a CPU they want? Ignoring this one. */
	/** 20140621
	 * mask와 online 상태인 첫번째 cpu를 찾는다.
	 **/
	cpu = cpumask_first_and(mask, cpu_online_mask);
	/** 20140621
	 * 현재 cpu라면 그 다음 교차검색된 cpu를 찾는다.
	 **/
	if (cpu == this_cpu)
		cpu = cpumask_next_and(cpu, mask, cpu_online_mask);

	/* No online cpus?  We're done. */
	/** 20140621
	 * 가져온 cpu 번호가 마지막 ids보다 크다면 return.
	 **/
	if (cpu >= nr_cpu_ids)
		return;

	/* Do we have another CPU which isn't us? */
	/** 20140621
	 * 다시 한 번 online mask를 검색해 next_cpu를 삼는다.
	 **/
	next_cpu = cpumask_next_and(cpu, mask, cpu_online_mask);
	if (next_cpu == this_cpu)
		next_cpu = cpumask_next_and(next_cpu, mask, cpu_online_mask);

	/* Fastpath: do that cpu by itself. */
	/** 20140621
	 * 추가 next가 없을 경우 first로 찾은 cpu에 대해 func을 수행한다.
	 **/
	if (next_cpu >= nr_cpu_ids) {
		smp_call_function_single(cpu, func, info, wait);
		return;
	}

	/** 20140621
	 * first 외 next가 더 존재할 경우 현재 cpu의 data를 가져와 lock을 건다.
	 **/
	data = &__get_cpu_var(cfd_data);
	csd_lock(&data->csd);

	/* This BUG_ON verifies our reuse assertions and can be removed */
	BUG_ON(atomic_read(&data->refs) || !cpumask_empty(data->cpumask));

	/*
	 * The global call function queue list add and delete are protected
	 * by a lock, but the list is traversed without any lock, relying
	 * on the rcu list add and delete to allow safe concurrent traversal.
	 * We reuse the call function data without waiting for any grace
	 * period after some other cpu removes it from the global queue.
	 * This means a cpu might find our data block as it is being
	 * filled out.
	 *
	 * We hold off the interrupt handler on the other cpu by
	 * ordering our writes to the cpu mask vs our setting of the
	 * refs counter.  We assert only the cpu owning the data block
	 * will set a bit in cpumask, and each bit will only be cleared
	 * by the subject cpu.  Each cpu must first find its bit is
	 * set and then check that refs is set indicating the element is
	 * ready to be processed, otherwise it must skip the entry.
	 *
	 * On the previous iteration refs was set to 0 by another cpu.
	 * To avoid the use of transitivity, set the counter to 0 here
	 * so the wmb will pair with the rmb in the interrupt handler.
	 */
	/** 20140621
	 * 현재 cpu의 cfd_data의 refs 초기화.
	 * call_single_data의 func와 info 설정.
	 **/
	atomic_set(&data->refs, 0);	/* convert 3rd to 1st party write */

	data->csd.func = func;
	data->csd.info = info;

	/* Ensure 0 refs is visible before mask.  Also orders func and info */
	/** 20140621
	 * write memory barrier.
	 **/
	smp_wmb();

	/* We rely on the "and" being processed before the store */
	/** 20140621
	 * call_function_data의 cpumask를 mask & cpu_online_mask로 설정하고,
	 * 현재 cpu는 그 대상에서 제외시키고 남은 bit의 수를 refs로 한다.
	 **/
	cpumask_and(data->cpumask, mask, cpu_online_mask);
	cpumask_clear_cpu(this_cpu, data->cpumask);
	refs = cpumask_weight(data->cpumask);

	/* Some callers race with other cpus changing the passed mask */
	if (unlikely(!refs)) {
		csd_unlock(&data->csd);
		return;
	}

	/** 20140621
	 * call_function에 대해 atomic context를 만든다.
	 **/
	raw_spin_lock_irqsave(&call_function.lock, flags);
	/*
	 * Place entry at the _HEAD_ of the list, so that any cpu still
	 * observing the entry in generic_smp_call_function_interrupt()
	 * will not miss any other list entries:
	 */
	/** 20140621
	 * 전역변수 call_function에 call_function_data를 등록한다.
	 **/
	list_add_rcu(&data->csd.list, &call_function.queue);
	/*
	 * We rely on the wmb() in list_add_rcu to complete our writes
	 * to the cpumask before this write to refs, which indicates
	 * data is on the list and is ready to be processed.
	 */
	/** 20140621
	 * call_function_data의 refs를 저장한다.
	 **/
	atomic_set(&data->refs, refs);
	raw_spin_unlock_irqrestore(&call_function.lock, flags);

	/*
	 * Make the list addition visible before sending the ipi.
	 * (IPIs must obey or appear to obey normal Linux cache
	 * coherency rules -- see comment in generic_exec_single).
	 */
	/** 20140621
	 * memory barrier.
	 **/
	smp_mb();

	/* Send a message to all CPUs in the map */
	/** 20140621
	 * cpumask의 각 cpu에게 ipi를 보낸다.
	 **/
	arch_send_call_function_ipi_mask(data->cpumask);

	/* Optionally wait for the CPUs to complete */
	/** 20140621
	 * IPI를 수신한 마지막 cpu의 핸들러에서 lock을 해제할 때까지 기다린다.
	 **/
	if (wait)
		csd_lock_wait(&data->csd);
}
EXPORT_SYMBOL(smp_call_function_many);

/**
 * smp_call_function(): Run a function on all other CPUs.
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait (atomically) until function has completed
 *        on other CPUs.
 *
 * Returns 0.
 *
 * If @wait is true, then returns once @func has returned; otherwise
 * it returns just before the target cpu calls @func.
 *
 * You must not call this function with disabled interrupts or from a
 * hardware interrupt handler or from a bottom half handler.
 */
/** 20160604
 * SMP에 속하는 다른 core들에게(cpu_online_mask) IPI를 전송해
 * func(info)를 수행하도록 한다. wait은 리턴받을 때까지 대기할지 여부를 결정.
 *
 * bottom half에서는 호출하면 안 된다.
 **/
int smp_call_function(smp_call_func_t func, void *info, int wait)
{
	preempt_disable();
	smp_call_function_many(cpu_online_mask, func, info, wait);
	preempt_enable();

	return 0;
}
EXPORT_SYMBOL(smp_call_function);
#endif /* USE_GENERIC_SMP_HELPERS */

/* Setup configured maximum number of CPUs to activate */
/** 20150530
 * activate 시킬 최대 cpu 개수.
 * boot option 등으로 처리할 수 있도록 별도의 변수로 두어 처리한다.
 **/
unsigned int setup_max_cpus = NR_CPUS;
EXPORT_SYMBOL(setup_max_cpus);


/*
 * Setup routine for controlling SMP activation
 *
 * Command-line option of "nosmp" or "maxcpus=0" will disable SMP
 * activation entirely (the MPS table probe still happens, though).
 *
 * Command-line option of "maxcpus=<NUM>", where <NUM> is an integer
 * greater than 0, limits the maximum number of CPUs activated in
 * SMP mode to <NUM>.
 */

void __weak arch_disable_smp_support(void) { }

static int __init nosmp(char *str)
{
	setup_max_cpus = 0;
	arch_disable_smp_support();

	return 0;
}

early_param("nosmp", nosmp);

/* this is hard limit */
static int __init nrcpus(char *str)
{
	int nr_cpus;

	get_option(&str, &nr_cpus);
	if (nr_cpus > 0 && nr_cpus < nr_cpu_ids)
		nr_cpu_ids = nr_cpus;

	return 0;
}

early_param("nr_cpus", nrcpus);

static int __init maxcpus(char *str)
{
	get_option(&str, &setup_max_cpus);
	if (setup_max_cpus == 0)
		arch_disable_smp_support();

	return 0;
}

early_param("maxcpus", maxcpus);

/* Setup number of possible processor ids */
/** 20130518
 * 가능한 cpu id의 개수 nr_cpu_ids는 NR_CPUS
 **/
int nr_cpu_ids __read_mostly = NR_CPUS;
EXPORT_SYMBOL(nr_cpu_ids);

/* An arch may set nr_cpu_ids earlier if needed, so this would be redundant */
/** 20130608
 * nr_cpu_ids에 cpu_possible_mask에 설정해 둔 내용을 바탕으로 값을 채움.
 **/
void __init setup_nr_cpu_ids(void)
{
	/** 20130608
	 * NR_CPUS : 4 일 때 find_last_bit에서 3이 리턴 + 1
	 **/
	nr_cpu_ids = find_last_bit(cpumask_bits(cpu_possible_mask),NR_CPUS) + 1;
}

/* Called by boot processor to activate the rest. */
/** 20150808
 * boot processor로 나머지 프로세서를 깨운다.
 *
 * 나머지 코어들이 처음 실행하는 어셈 코드는 secondary_startup,
 * 커널 함수는 secondary_start_kernel이다.
 **/
void __init smp_init(void)
{
	unsigned int cpu;

	/** 20150801
	 * boot cpu를 제외한 cpu들에 해당하는 idle task를 init한다.
	 * idle task는 percpu로 존재한다.
	 **/
	idle_threads_init();

	/* FIXME: This should be done in userspace --RR */
	/** 20150117
	 * cpu_present_mask의 각 cpu를 보며, 현 상태가 online이 아닌 경우 up시킴.
	 **/
	for_each_present_cpu(cpu) {
		if (num_online_cpus() >= setup_max_cpus)
			break;
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	/* Any cleanup work */
	/** 20150808
	 * 출력 예 : Brought up 4 CPUs
	 **/
	printk(KERN_INFO "Brought up %ld CPUs\n", (long)num_online_cpus());
	smp_cpus_done(setup_max_cpus);
}

/*
 * Call a function on all processors.  May be used during early boot while
 * early_boot_irqs_disabled is set.  Use local_irq_save/restore() instead
 * of local_irq_disable/enable().
 */
int on_each_cpu(void (*func) (void *info), void *info, int wait)
{
	unsigned long flags;
	int ret = 0;

	preempt_disable();
	ret = smp_call_function(func, info, wait);
	local_irq_save(flags);
	func(info);
	local_irq_restore(flags);
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(on_each_cpu);

/**
 * on_each_cpu_mask(): Run a function on processors specified by
 * cpumask, which may include the local processor.
 * @mask: The set of cpus to run on (only runs on online subset).
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait (atomically) until function has completed
 *        on other CPUs.
 *
 * If @wait is true, then returns once @func has returned.
 *
 * You must not call this function with disabled interrupts or
 * from a hardware interrupt handler or from a bottom half handler.
 */
/** 20140622
 * cpu mask에 속한 cpu가 특정 함수를 실행하도록 한다.
 * wait은 다른 cpu의 동작 완료까지 현재 cpu의 대기 여부를 결정한다.
 **/
void on_each_cpu_mask(const struct cpumask *mask, smp_call_func_t func,
			void *info, bool wait)
{
	/** 20140622
	 * 선점 불가 상태로 만든다.
	 **/
	int cpu = get_cpu();

	/** 20140622
	 * mask 속한 cpu들이 func(info); 을 수행하도록 한다.
	 * wait 여부에 따라 완료시까지 대기 여부를 결정한다.
	 **/
	smp_call_function_many(mask, func, info, wait);
	/** 20140622
	 * 현재 cpu가 mask에 속해 있다면, 인터럽트를 금지한 상태로 함수를 실행한다.
	 * 즉, atomic context로 만든다.
	 **/
	if (cpumask_test_cpu(cpu, mask)) {
		local_irq_disable();
		func(info);
		local_irq_enable();
	}
	/** 20140622
	 * 선점 가능 상태로 만든다.
	 **/
	put_cpu();
}
EXPORT_SYMBOL(on_each_cpu_mask);

/*
 * on_each_cpu_cond(): Call a function on each processor for which
 * the supplied function cond_func returns true, optionally waiting
 * for all the required CPUs to finish. This may include the local
 * processor.
 * @cond_func:	A callback function that is passed a cpu id and
 *		the the info parameter. The function is called
 *		with preemption disabled. The function should
 *		return a blooean value indicating whether to IPI
 *		the specified CPU.
 * @func:	The function to run on all applicable CPUs.
 *		This must be fast and non-blocking.
 * @info:	An arbitrary pointer to pass to both functions.
 * @wait:	If true, wait (atomically) until function has
 *		completed on other CPUs.
 * @gfp_flags:	GFP flags to use when allocating the cpumask
 *		used internally by the function.
 *
 * The function might sleep if the GFP flags indicates a non
 * atomic allocation is allowed.
 *
 * Preemption is disabled to protect against CPUs going offline but not online.
 * CPUs going online during the call will not be seen or sent an IPI.
 *
 * You must not call this function with disabled interrupts or
 * from a hardware interrupt handler or from a bottom half handler.
 */
void on_each_cpu_cond(bool (*cond_func)(int cpu, void *info),
			smp_call_func_t func, void *info, bool wait,
			gfp_t gfp_flags)
{
	cpumask_var_t cpus;
	int cpu, ret;

	might_sleep_if(gfp_flags & __GFP_WAIT);

	if (likely(zalloc_cpumask_var(&cpus, (gfp_flags|__GFP_NOWARN)))) {
		preempt_disable();
		for_each_online_cpu(cpu)
			if (cond_func(cpu, info))
				cpumask_set_cpu(cpu, cpus);
		on_each_cpu_mask(cpus, func, info, wait);
		preempt_enable();
		free_cpumask_var(cpus);
	} else {
		/*
		 * No free cpumask, bother. No matter, we'll
		 * just have to IPI them one by one.
		 */
		preempt_disable();
		for_each_online_cpu(cpu)
			if (cond_func(cpu, info)) {
				ret = smp_call_function_single(cpu, func,
								info, wait);
				WARN_ON_ONCE(!ret);
			}
		preempt_enable();
	}
}
EXPORT_SYMBOL(on_each_cpu_cond);

static void do_nothing(void *unused)
{
}

/**
 * kick_all_cpus_sync - Force all cpus out of idle
 *
 * Used to synchronize the update of pm_idle function pointer. It's
 * called after the pointer is updated and returns after the dummy
 * callback function has been executed on all cpus. The execution of
 * the function can only happen on the remote cpus after they have
 * left the idle function which had been called via pm_idle function
 * pointer. So it's guaranteed that nothing uses the previous pointer
 * anymore.
 */
void kick_all_cpus_sync(void)
{
	/* Make sure the change is visible before we kick the cpus */
	smp_mb();
	smp_call_function(do_nothing, NULL, 1);
}
EXPORT_SYMBOL_GPL(kick_all_cpus_sync);
