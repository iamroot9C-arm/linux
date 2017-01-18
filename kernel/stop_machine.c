/*
 * kernel/stop_machine.c
 *
 * Copyright (C) 2008, 2005	IBM Corporation.
 * Copyright (C) 2008, 2005	Rusty Russell rusty@rustcorp.com.au
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This file is released under the GPLv2 and any later version.
 */
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/stop_machine.h>
#include <linux/interrupt.h>
#include <linux/kallsyms.h>

#include <linux/atomic.h>

/*
 * Structure to determine completion condition and record errors.  May
 * be shared by works on different cpus.
 */
/** 20150524
 * 완료 조건을 검사하고, 결과를 기록하는 구조체.
 *
 * nr_todo : completion을 판단하기 위해 실행되어야 할 횟수
 *
 * 예를 들어 cpu_stop_signal_done()에서 nr_todo가 0이 되면 completion을 호출한다.
 **/
struct cpu_stop_done {
	atomic_t		nr_todo;	/* nr left to execute */
	bool			executed;	/* actually executed? */
	int			ret;		/* collected return value */
	struct completion	completion;	/* fired if nr_todo reaches 0 */
};

/* the actual stopper, one per every possible cpu, enabled on online cpus */
/** 20150530
 * cpu_stopper 구조체.
 *
 * spinlock으로 보호되며, 실행해야 할 works 리스트가 존재한다.
 **/
struct cpu_stopper {
	spinlock_t		lock;
	bool			enabled;	/* is this stopper enabled? */
	struct list_head	works;		/* list of pending works */
	struct task_struct	*thread;	/* stopper thread */
};

/** 20150725
 * percpu cpu_stopper 선언.
 **/
static DEFINE_PER_CPU(struct cpu_stopper, cpu_stopper);
/** 20150801
 * cpu_stop_init이 완료되면 true.
 **/
static bool stop_machine_initialized = false;

/** 20130720
 * cpu_stop_done 자료구조 초기화
 **/
static void cpu_stop_init_done(struct cpu_stop_done *done, unsigned int nr_todo)
{
	/** 20130720
	 * 메모리 초기화한 뒤, done->nr_todo를 nr_todo로 설정
	 *   __stop_cpus에서 호출된 경우 nr_todo는 cpumask_weight(cpumask).
	 *     cpumask는 cpu_online_mask 등
	 **/
	memset(done, 0, sizeof(*done));
	/** 20130720
	 * 완료해야 할 cpu의 수를 지정
	 **/
	atomic_set(&done->nr_todo, nr_todo);
	init_completion(&done->completion);
}

/* signal completion unless @done is NULL */
/** 20130720
 * cpu_stop 작업의 완료를 통보하기 위한 함수.
 *
 * done->nr_todo만큼 모두 수행이 완료되었을 때만 complete를 날린다.
 * 즉, 작업이 각 cpu에서 수행되도록 지시한 task는 모든 실행이 완료된
 * 후에 동작을 재개한다.
 **/
static void cpu_stop_signal_done(struct cpu_stop_done *done, bool executed)
{
	if (done) {
		/** 20130720
		 * executed가 true이면 done->executed에 true 설정
		 **/
		if (executed)
			done->executed = true;
		/** 20130720
		 * nr_todo만큼 모두 실행되었다면 completion 시킨다.
		 **/
		if (atomic_dec_and_test(&done->nr_todo))
			complete(&done->completion);
	}
}

/* queue @work to @stopper.  if offline, @work is completed immediately */
/** 20150524
 * cpu_stopper에게 work를 queue시키고 깨운다.
 *
 * 여기서 queue된 작업은 cpu_stopper_thread에서 꺼내져 실행된다.
 **/
static void cpu_stop_queue_work(struct cpu_stopper *stopper,
				struct cpu_stop_work *work)
{
	unsigned long flags;

	/** 20150524
	 * 인터럽트를 막고, 스핀락으로 stopper의 동기화를 보장한다.
	 **/
	spin_lock_irqsave(&stopper->lock, flags);

	/** 20150523
	 * stopper가 사용 가능하면 (해당 cpu가 online이면 enabled로 만든다)
	 * work를 stopper 실행 목록의 끝에 달아주고 stopper를 깨운다.
	 * 그렇지 않다면 실패를 통보한다.
	 **/
	if (stopper->enabled) {
		list_add_tail(&work->list, &stopper->works);
		wake_up_process(stopper->thread);
	} else
		cpu_stop_signal_done(work->done, false);

	spin_unlock_irqrestore(&stopper->lock, flags);
}

/**
 * stop_one_cpu - stop a cpu
 * @cpu: cpu to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Execute @fn(@arg) on @cpu.  @fn is run in a process context with
 * the highest priority preempting any task on the cpu and
 * monopolizing it.  This function returns after the execution is
 * complete.
 *
 * This function doesn't guarantee @cpu stays online till @fn
 * completes.  If @cpu goes down in the middle, execution may happen
 * partially or fully on different cpus.  @fn should either be ready
 * for that or the caller should ensure that @cpu stays online until
 * this function completes.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * -ENOENT if @fn(@arg) was not executed because @cpu was offline;
 * otherwise, the return value of @fn.
 */
/** 20150524
 * cpu에서 fn이 돌아가도록 큐잉시키고 완료를 기다린다.
 **/
int stop_one_cpu(unsigned int cpu, cpu_stop_fn_t fn, void *arg)
{
	struct cpu_stop_done done;
	struct cpu_stop_work work = { .fn = fn, .arg = arg, .done = &done };

	cpu_stop_init_done(&done, 1);
	/** 20150524
	 * 전달받은 argument로 work을 채워 큐에 넣고 stopper를 깨운다.
	 **/
	cpu_stop_queue_work(&per_cpu(cpu_stopper, cpu), &work);
	/** 20150524
	 * done이 완료되길 기다리고, 결과를 리턴한다.
	 **/
	wait_for_completion(&done.completion);
	return done.executed ? done.ret : -ENOENT;
}

/**
 * stop_one_cpu_nowait - stop a cpu but don't wait for completion
 * @cpu: cpu to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Similar to stop_one_cpu() but doesn't wait for completion.  The
 * caller is responsible for ensuring @work_buf is currently unused
 * and will remain untouched until stopper starts executing @fn.
 *
 * CONTEXT:
 * Don't care.
 */
void stop_one_cpu_nowait(unsigned int cpu, cpu_stop_fn_t fn, void *arg,
			struct cpu_stop_work *work_buf)
{
	*work_buf = (struct cpu_stop_work){ .fn = fn, .arg = arg, };
	cpu_stop_queue_work(&per_cpu(cpu_stopper, cpu), work_buf);
}

/* static data for stop_cpus */
static DEFINE_MUTEX(stop_cpus_mutex);
static DEFINE_PER_CPU(struct cpu_stop_work, stop_cpus_work);

/** 20130720
 * cpu_stopper에서 실행할 work을 설정하고,
 * cpumask의 각 cpu마다 work을 queue시킨 뒤 stopper를 깨운다.
 *
 * __stop_machine 에서 호출되었을 경우
 *   cpumask : cpu_online_mask
 *   fn      : stop_machine_cpu_stop
 *   arp     : &smdata
 **/
static void queue_stop_cpus_work(const struct cpumask *cpumask,
				 cpu_stop_fn_t fn, void *arg,
				 struct cpu_stop_done *done)
{
	/** 20151128
	 * cpu stopper에게 전달할 작업
	 **/
	struct cpu_stop_work *work;
	unsigned int cpu;

	/* initialize works and done */
	/** 20130720
	 * cpumask의 cpu들을 순회하며
	 *   per-cpu stop_cpus_work 각각에
	 *   cpu_stopper에서 전달할 각 work을 설정한다.
	 **/
	for_each_cpu(cpu, cpumask) {
		work = &per_cpu(stop_cpus_work, cpu);
		work->fn = fn;
		work->arg = arg;
		work->done = done;
	}

	/*
	 * Disable preemption while queueing to avoid getting
	 * preempted by a stopper which might wait for other stoppers
	 * to enter @fn which can lead to deadlock.
	 */
	preempt_disable();
	/** 20151128
	 * cpumask의 각 cpu에 work을 걸고 stopper를 깨운다.
	 *
	 * work을 큐잉하는동안 다른 stopper에게 선점되어 deadlock에
	 * 빠지지 않도록 선점불가 상태로 진행한다.
	 **/
	for_each_cpu(cpu, cpumask)
		cpu_stop_queue_work(&per_cpu(cpu_stopper, cpu),
				    &per_cpu(stop_cpus_work, cpu));
	preempt_enable();
}

/** 20151128
 * cpumask에 존재하는 cpu들의 cpu_stopper에서 fn을 실행하도록 설정하고
 * 완료시까지 대기(sleep상태)한다.
 **/
static int __stop_cpus(const struct cpumask *cpumask,
		       cpu_stop_fn_t fn, void *arg)
{
	struct cpu_stop_done done;

	/** 20130720
	 * cpu_stop에서 사용하는 done 자료구조 초기화.
	 **/
	cpu_stop_init_done(&done, cpumask_weight(cpumask));
	/** 20151128
	 * 각 cpu_stopper가 실행할 work을 설정하고, queue시킨다.
	 * 작업이 완료되어 completion이 도달할 때까지 대기한다.
	 * 대기가 끝나면 done의 결과를 리턴한다.
	 *
	 * cpumask에 지시한 cpu들이 모두 작업 완료하고
	 * cpu_stop_signal_done를 통해 complete가 올 때까지 대기한다.
	 **/
	queue_stop_cpus_work(cpumask, fn, arg, &done);
	wait_for_completion(&done.completion);
	return done.executed ? done.ret : -ENOENT;
}

/**
 * stop_cpus - stop multiple cpus
 * @cpumask: cpus to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Execute @fn(@arg) on online cpus in @cpumask.  On each target cpu,
 * @fn is run in a process context with the highest priority
 * preempting any task on the cpu and monopolizing it.  This function
 * returns after all executions are complete.
 *
 * This function doesn't guarantee the cpus in @cpumask stay online
 * till @fn completes.  If some cpus go down in the middle, execution
 * on the cpu may happen partially or fully on different cpus.  @fn
 * should either be ready for that or the caller should ensure that
 * the cpus stay online until this function completes.
 *
 * All stop_cpus() calls are serialized making it safe for @fn to wait
 * for all cpus to start executing it.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * -ENOENT if @fn(@arg) was not executed at all because all cpus in
 * @cpumask were offline; otherwise, 0 if all executions of @fn
 * returned 0, any non zero return value if any returned non zero.
 */
/** 20151128
 * cpumask 상의 각 online cpu들에서 stopper task로 fn을 실행시킨다.
 * stopper task는 stop_sched_class이므로 스케쥴링 우선순위가 가장 높다.
 **/
int stop_cpus(const struct cpumask *cpumask, cpu_stop_fn_t fn, void *arg)
{
	int ret;

	/* static works are used, process one request at a time */
	mutex_lock(&stop_cpus_mutex);
	ret = __stop_cpus(cpumask, fn, arg);
	mutex_unlock(&stop_cpus_mutex);
	return ret;
}

/**
 * try_stop_cpus - try to stop multiple cpus
 * @cpumask: cpus to stop
 * @fn: function to execute
 * @arg: argument to @fn
 *
 * Identical to stop_cpus() except that it fails with -EAGAIN if
 * someone else is already using the facility.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * -EAGAIN if someone else is already stopping cpus, -ENOENT if
 * @fn(@arg) was not executed at all because all cpus in @cpumask were
 * offline; otherwise, 0 if all executions of @fn returned 0, any non
 * zero return value if any returned non zero.
 */
int try_stop_cpus(const struct cpumask *cpumask, cpu_stop_fn_t fn, void *arg)
{
	int ret;

	/* static works are used, process one request at a time */
	if (!mutex_trylock(&stop_cpus_mutex))
		return -EAGAIN;
	ret = __stop_cpus(cpumask, fn, arg);
	mutex_unlock(&stop_cpus_mutex);
	return ret;
}

/** 20150530
 * stopper thread.
 *
 * 리스트 등록된 work을 순차적으로 꺼내 실행하고 결과를 리턴한다.
 * work은 cpu_stop_queue_work에서 큐잉된다.
 *
 * kthreadd(로 생성될 때 percpu로 해당 cpu의 cpu_stopper를 매개변수로 전달한다.
 **/
static int cpu_stopper_thread(void *data)
{
	struct cpu_stopper *stopper = data;
	struct cpu_stop_work *work;
	int ret;

repeat:
	set_current_state(TASK_INTERRUPTIBLE);	/* mb paired w/ kthread_stop */

	/** 20150530
	 * stopper가 정지되어야 한다면 TASK_RUNNING으로 상태를 지정한 후 리턴한다.
	 **/
	if (kthread_should_stop()) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	work = NULL;
	/** 20150530
	 * stopper는 percpu 변수이지만, spin lock으로 보호되어야 한다.
	 **/
	spin_lock_irq(&stopper->lock);
	/** 20150530
	 * stopper가 수행해야 할 work이 존재하면 첫번째 entry를 분리한다.
	 **/
	if (!list_empty(&stopper->works)) {
		work = list_first_entry(&stopper->works,
					struct cpu_stop_work, list);
		list_del_init(&work->list);
	}
	spin_unlock_irq(&stopper->lock);

	/** 20150530
	 * work이 존재하면 work에 저장되어 있는 함수를 호출한다.
	 * 결과는 work 내의 done->ret에 저장한다.
	 *
	 * work이 없다면 scheduling 된다.
	 **/
	if (work) {
		cpu_stop_fn_t fn = work->fn;
		void *arg = work->arg;
		struct cpu_stop_done *done = work->done;
		char ksym_buf[KSYM_NAME_LEN] __maybe_unused;

		__set_current_state(TASK_RUNNING);

		/* cpu stop callbacks are not allowed to sleep */
		/** 20150530
		 * cpu stop 콜백은 선점이 허용되지 않는다.
		 * 실행결과는 ret에 저장한다.
		 **/
		preempt_disable();

		ret = fn(arg);
		if (ret)
			done->ret = ret;

		/* restore preemption and check it's still balanced */
		preempt_enable();
		WARN_ONCE(preempt_count(),
			  "cpu_stop: %s(%p) leaked preempt count\n",
			  kallsyms_lookup((unsigned long)fn, NULL, NULL, NULL,
					  ksym_buf), arg);

		/** 20150530
		 * 실행이 완료되면 work 내의 done에 따라 완료를 통보(complete)한다.
		 **/
		cpu_stop_signal_done(done, true);
	} else
		schedule();

	goto repeat;
}

extern void sched_set_stop_task(int cpu, struct task_struct *stop);

/* manage stopper for a cpu, mostly lifted from sched migration thread mgmt */
/** 20150801
 * cpu event 발생시 호출되는 콜백 함수.
 *
 * cpu notifier chain에 등록할 때 cpu_stop_cpu_notifier가 가장 높은 우선순위를 갖기 때문에 이 콜백이 먼저 호출된다.
 *
 * CPU_UP_PREPARE시 per-cpu로 migration thread를 생성한다.
 * CPU_ONLINE시 thread를 실행시킨다.
 **/
static int __cpuinit cpu_stop_cpu_callback(struct notifier_block *nfb,
					   unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	/** 20150801
	 * percpu cpu_stopper에서 해당 cpu에 해당하는 변수위치를 받아온다.
	 **/
	struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);
	struct task_struct *p;

	/** 20150725
	 * CPU_TASKS_FROZEN를 제외한 action을 보고 필요한 동작을 한다.
	 **/
	switch (action & ~CPU_TASKS_FROZEN) {
	/** 20150725
	 * migration thread를 생성한다.
	 *
	 * cpu_stopper_thread를 생성하고, percpu stopper를 매개변수로 전달한다.
	 **/
	case CPU_UP_PREPARE:
		BUG_ON(stopper->thread || stopper->enabled ||
		       !list_empty(&stopper->works));
		p = kthread_create_on_node(cpu_stopper_thread,
					   stopper,
					   cpu_to_node(cpu),
					   "migration/%d", cpu);
		if (IS_ERR(p))
			return notifier_from_errno(PTR_ERR(p));
		get_task_struct(p);
		/** 20150801
		 * kthreadd로 생성한 task를 지정된 cpu에서만 실행하도록 설정한다.
		 * cpu의 stop task로 생성한 task를 지정한다.
		 *   stop sched class는 다음 실행할 task를 고를 때 가장 먼저 선택된다.
		 * percpu stopper의 thread로 생성한 task를 저장한다.
		 **/
		kthread_bind(p, cpu);
		sched_set_stop_task(cpu, p);
		stopper->thread = p;
		break;

	/** 20150801
	 * CPU_ONLINE시 stopper thread를 깨운다.
	 * stopper에 enabled를 기록한다.
	 **/
	case CPU_ONLINE:
		/* strictly unnecessary, as first user will wake it */
		wake_up_process(stopper->thread);
		/* mark enabled */
		spin_lock_irq(&stopper->lock);
		stopper->enabled = true;
		spin_unlock_irq(&stopper->lock);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	/** 20150801
	 * HOTPLUG CPU일 경우
	 * CPU_UP_CANCELED, CPU_POST_DEAD 이벤트에 대한 처리.
	 *
	 * 해당 cpu의 stop task를 NULL로 날린다.
	 * stopper의 thread를 정지시킨다.
	 * stopper에 pending되어 있는 works를 false로 실패를 통지한다.
	 * stopper의 thread를 날린다.
	 *
	 * percpu 변수 stopper에 대한 접근시에는 spinlock을 사용한다.
	 **/
	case CPU_UP_CANCELED:
	case CPU_POST_DEAD:
	{
		struct cpu_stop_work *work;

		sched_set_stop_task(cpu, NULL);
		/* kill the stopper */
		kthread_stop(stopper->thread);
		/* drain remaining works */
		spin_lock_irq(&stopper->lock);
		list_for_each_entry(work, &stopper->works, list)
			cpu_stop_signal_done(work->done, false);
		stopper->enabled = false;
		spin_unlock_irq(&stopper->lock);
		/* release the stopper */
		put_task_struct(stopper->thread);
		stopper->thread = NULL;
		break;
	}
#endif
	}

	return NOTIFY_OK;
}

/*
 * Give it a higher priority so that cpu stopper is available to other
 * cpu notifiers.  It currently shares the same priority as sched
 * migration_notifier.
 */
/** 20150725
 * cpu_stop을 위한 cpu notifier을 선언한다.
 * priority를 높게 주어 migration_notifier와 같은 priority를 사용한다.
 **/
static struct notifier_block __cpuinitdata cpu_stop_cpu_notifier = {
	.notifier_call	= cpu_stop_cpu_callback,
	.priority	= 10,
};

/** 20150801
 * cpu_stop관련 초기화.
 *
 * percpu cpu_stopper를 초기화 하고, cpu notifier chain 콜백 함수를 등록시킨다.
 **/
static int __init cpu_stop_init(void)
{
	void *bcpu = (void *)(long)smp_processor_id();
	unsigned int cpu;
	int err;

	/** 20150725
	 * possible cpu들을 순회한다.
	 **/
	for_each_possible_cpu(cpu) {
		/** 20150725
		 * 해당 cpu에 대한 cpu_stopper를 가져와 lock과 works를 초기화 한다.
		 **/
		struct cpu_stopper *stopper = &per_cpu(cpu_stopper, cpu);

		spin_lock_init(&stopper->lock);
		INIT_LIST_HEAD(&stopper->works);
	}

	/* start one for the boot cpu */
	/** 20150801
	 * boot cpu를 위해 직접 CPU_UP_PREPARE, CPU_ONLINE를 호출하고
	 * 다른 cpu를 위해 cpu notifier로 등록한다.
	 **/
	err = cpu_stop_cpu_callback(&cpu_stop_cpu_notifier, CPU_UP_PREPARE,
				    bcpu);
	BUG_ON(err != NOTIFY_OK);
	cpu_stop_cpu_callback(&cpu_stop_cpu_notifier, CPU_ONLINE, bcpu);
	register_cpu_notifier(&cpu_stop_cpu_notifier);

	stop_machine_initialized = true;

	return 0;
}
early_initcall(cpu_stop_init);

#ifdef CONFIG_STOP_MACHINE

/* This controls the threads on each CPU. */
enum stopmachine_state {
	/* Dummy starting state for thread. */
	STOPMACHINE_NONE,
	/* Awaiting everyone to be scheduled. */
	STOPMACHINE_PREPARE,
	/* Disable interrupts. */
	STOPMACHINE_DISABLE_IRQ,
	/* Run the function */
	STOPMACHINE_RUN,
	/* Exit */
	STOPMACHINE_EXIT,
};

struct stop_machine_data {
	int			(*fn)(void *);
	void			*data;
	/* Like num_online_cpus(), but hotplug cpu uses us, so we need this. */
	unsigned int		num_threads;
	const struct cpumask	*active_cpus;

	enum stopmachine_state	state;
	atomic_t		thread_ack;
};

/** 20130720
 * smdata의 thread_ack에 online cpu의 수를 저장하고, state를 newstate로 저장.
 *   추후 ack_state 에서 사용
 **/
static void set_state(struct stop_machine_data *smdata,
		      enum stopmachine_state newstate)
{
	/* Reset ack counter. */
	/** 20130720
	 * thread_ack에 online_cpu의 수를 저장한다.
	 **/
	atomic_set(&smdata->thread_ack, smdata->num_threads);
	/** 20130720
	 * memory barrier
	 **/
	smp_wmb();
	/** 20130720
	 * newstate를 smdata->state에 저장
	 **/
	smdata->state = newstate;
}

/* Last one to ack a state moves to the next state. */
static void ack_state(struct stop_machine_data *smdata)
{
	if (atomic_dec_and_test(&smdata->thread_ack))
		set_state(smdata, smdata->state + 1);
}

/* This is the cpu_stop function which stops the CPU. */
static int stop_machine_cpu_stop(void *data)
{
	struct stop_machine_data *smdata = data;
	enum stopmachine_state curstate = STOPMACHINE_NONE;
	int cpu = smp_processor_id(), err = 0;
	unsigned long flags;
	bool is_active;

	/*
	 * When called from stop_machine_from_inactive_cpu(), irq might
	 * already be disabled.  Save the state and restore it on exit.
	 */
	local_save_flags(flags);

	if (!smdata->active_cpus)
		is_active = cpu == cpumask_first(cpu_online_mask);
	else
		is_active = cpumask_test_cpu(cpu, smdata->active_cpus);

	/* Simple state machine */
	do {
		/* Chill out and ensure we re-read stopmachine_state. */
		cpu_relax();
		if (smdata->state != curstate) {
			curstate = smdata->state;
			switch (curstate) {
			case STOPMACHINE_DISABLE_IRQ:
				local_irq_disable();
				hard_irq_disable();
				break;
			case STOPMACHINE_RUN:
				if (is_active)
					err = smdata->fn(smdata->data);
				break;
			default:
				break;
			}
			ack_state(smdata);
		}
	} while (curstate != STOPMACHINE_EXIT);

	local_irq_restore(flags);
	return err;
}

/** 20151130
 * stop_machine이 초기화 되기 이전이라면 현재 코어에서만 fn을 실행하고,
 * stop_machine이 초기화 된 이후라면 online 상태의 cpu들이
 * 각각 stop_machine_cpu_stop(smdata)를 실행하도록 한다.
 **/
int __stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	/** 20130720
	 * struct stop_machine_data를 전달받은 매개변수로 채워준다.
	 **/
	struct stop_machine_data smdata = { .fn = fn, .data = data,
					    .num_threads = num_online_cpus(),
					    .active_cpus = cpus };

	/** 20130720
	 * stop_machine_initialized 이 초기화 되어 있지 않다면 수행
	 * cpu_stop_init에서 true로 설정해줌
	 **/
	if (!stop_machine_initialized) {
		/*
		 * Handle the case where stop_machine() is called
		 * early in boot before stop_machine() has been
		 * initialized.
		 */
		unsigned long flags;
		int ret;

		/** 20130720
		 * num_online_cpus()가 1이 아닐 경우 WARN
		 **/
		WARN_ON_ONCE(smdata.num_threads != 1);

		/** 20130720
		 * local cpu의 irq를 막은 상태에서 fn을 실행시킨다.
		 **/
		local_irq_save(flags);
		hard_irq_disable();
		/** 20130720
		 * fn을 수행한다.
		 **/
		ret = (*fn)(data);
		/** 20130720
		 * local cpu의 irq를 복원한다.
		 **/
		local_irq_restore(flags);

		/** 20130720
		 * 함수 수행 결과를 바로 리턴한다.
		 **/
		return ret;
	}

	/** 20130720
	 * 초기화 된 후 호출되었다면 이 부분 수행됨.
	 *
	 * smdata의 상태를 STOPMACHINE_PREPARE로 설정
	 **/
	/* Set the initial state and stop all online cpus. */
	set_state(&smdata, STOPMACHINE_PREPARE);
	/** 20151128
	 * online 상태의 cpu들에서 stop_machine_cpu_stop() 함수를 실행시킨다.
	 **/
	return stop_cpus(cpu_online_mask, stop_machine_cpu_stop, &smdata);
}

/** 20151212
 * cpumask에 속하는 online 상태의 cpu들이 fn을 실행하도록 한다.
 *
 * 런타임시 간략화한 호출구조
 * stop_machine
 *   __stop_machine
 *     stop_cpus : 다른 cpus에 지정된 cpu들 모두가 각각의 stop task로
 *                 fn을 실행시키로 리턴할 때까지 sleep 상태로 대기한다
 **/
int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	int ret;

	/* No CPUs can come up or down during this. */
	/** 20130706
	 * kernel/cpu.c 에 있는 함수 호출
	 *
	 * 20130720
	 * cpu_hotplug.refcount를 증가해 hotplug begin을 하지 못하게 한다.
	 **/
	get_online_cpus();
	ret = __stop_machine(fn, data, cpus);
	put_online_cpus();
	return ret;
}
EXPORT_SYMBOL_GPL(stop_machine);

/**
 * stop_machine_from_inactive_cpu - stop_machine() from inactive CPU
 * @fn: the function to run
 * @data: the data ptr for the @fn()
 * @cpus: the cpus to run the @fn() on (NULL = any online cpu)
 *
 * This is identical to stop_machine() but can be called from a CPU which
 * is not active.  The local CPU is in the process of hotplug (so no other
 * CPU hotplug can start) and not marked active and doesn't have enough
 * context to sleep.
 *
 * This function provides stop_machine() functionality for such state by
 * using busy-wait for synchronization and executing @fn directly for local
 * CPU.
 *
 * CONTEXT:
 * Local CPU is inactive.  Temporarily stops all active CPUs.
 *
 * RETURNS:
 * 0 if all executions of @fn returned 0, any non zero return value if any
 * returned non zero.
 */
int stop_machine_from_inactive_cpu(int (*fn)(void *), void *data,
				  const struct cpumask *cpus)
{
	struct stop_machine_data smdata = { .fn = fn, .data = data,
					    .active_cpus = cpus };
	struct cpu_stop_done done;
	int ret;

	/* Local CPU must be inactive and CPU hotplug in progress. */
	BUG_ON(cpu_active(raw_smp_processor_id()));
	smdata.num_threads = num_active_cpus() + 1;	/* +1 for local */

	/* No proper task established and can't sleep - busy wait for lock. */
	while (!mutex_trylock(&stop_cpus_mutex))
		cpu_relax();

	/* Schedule work on other CPUs and execute directly for local CPU */
	set_state(&smdata, STOPMACHINE_PREPARE);
	cpu_stop_init_done(&done, num_active_cpus());
	queue_stop_cpus_work(cpu_active_mask, stop_machine_cpu_stop, &smdata,
			     &done);
	ret = stop_machine_cpu_stop(&smdata);

	/* Busy wait for completion. */
	while (!completion_done(&done.completion))
		cpu_relax();

	mutex_unlock(&stop_cpus_mutex);
	return ret ?: done.ret;
}

#endif	/* CONFIG_STOP_MACHINE */
