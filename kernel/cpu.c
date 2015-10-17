/* CPU control.
 * (C) 2001, 2002, 2003, 2004 Rusty Russell
 *
 * This code is licenced under the GPL.
 */
#include <linux/proc_fs.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/cpu.h>
#include <linux/oom.h>
#include <linux/rcupdate.h>
#include <linux/export.h>
#include <linux/bug.h>
#include <linux/kthread.h>
#include <linux/stop_machine.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/suspend.h>

#include "smpboot.h"

#ifdef CONFIG_SMP
/* Serializes the updates to cpu_online_mask, cpu_present_mask */
/** 20150801    
 * cpu_add_remove_lock으로 cpu_online_mask, cpu_present_mask을 보호한다.
 **/
static DEFINE_MUTEX(cpu_add_remove_lock);

/*
 * The following two API's must be used when attempting
 * to serialize the updates to cpu_online_mask, cpu_present_mask.
 */
/** 20130727    
 * cpu_online_mask, cpu_present_mask가 변경되는 작업을 serialize 시킨다.
 *   - cpu_add_remove_lock을 mutex lock으로 건다
 *
 * e.g.
 *   cpu_maps_update_begin
 *   _cpu_up(cpu, 0);
 *   cpu_maps_update_done
 **/
void cpu_maps_update_begin(void)
{
	mutex_lock(&cpu_add_remove_lock);
}

void cpu_maps_update_done(void)
{
	mutex_unlock(&cpu_add_remove_lock);
}

/** 20130727    
 * cpu_chain이라는 이름으로 notifier head를 선언한다.
 *
 * notifier block이 등록되는 전역 chain.
 * 추후 cpu_notify가 호출되었을 때 이 리스트의 nb의 callback이 호출된다.
 **/
static RAW_NOTIFIER_HEAD(cpu_chain);

/* If set, cpu_up and cpu_down will return -EBUSY and do nothing.
 * Should always be manipulated under cpu_add_remove_lock
 */
/** 20151010    
 * cpu hotplug 동작을 불가능하게 하는 조건 변수.
 * 이 변수가 설정되어 있으면 cpu_up, cpu_down시 -EBUSY가 리턴된다.
 *
 * cpu_add_remove_lock ( cpu_maps_update_begin()과 cpu_maps_update_done() )으로
 * 보호된다.
 **/
static int cpu_hotplug_disabled;

#ifdef CONFIG_HOTPLUG_CPU

static struct {
	struct task_struct *active_writer;
	struct mutex lock; /* Synchronizes accesses to refcount, */
	/*
	 * Also blocks the new readers during
	 * an ongoing cpu hotplug operation.
	 */
	int refcount;
} cpu_hotplug = {
	.active_writer = NULL,
	.lock = __MUTEX_INITIALIZER(cpu_hotplug.lock),
	.refcount = 0,
};

/** 20130706    
 * CONFIG_HOTPLUG_CPU 옵션이 켜 있어 이 함수 실행
 **/
/** 20130720    
 * cpu_hotplug를 참조하는 refcount를 증가시킨다.
 *     cpu_hotplug_begin 에서 refcount가 0이 될 때까지 반복하며 대기한다.
 **/
void get_online_cpus(void)
{
	/** 20130720    
	 * schedule 포인트를 둔다
	 **/
	might_sleep();
	/** 20130706    
	 * cpu_hotplug_begin 전에는 초기값 NULL.
	 * cpu_hotplug_begin에서 active_writer를 current로 넣어 수행 중인 task를 기록한다.
	 * activate_writer가 현재 함수를 수행 중인 task와 같다면
	 * refcount를 증가하지 않고 리턴한다.
	 **/
	if (cpu_hotplug.active_writer == current)
		return;
	mutex_lock(&cpu_hotplug.lock);
	/** 20130713    
	 * refcount를 증가.
	 *
	 * from kernel/cpu.c
	 *   This ensures that the hotplug operation can begin only when the
	 *   refcount goes to zero.
	 **/
	cpu_hotplug.refcount++;
	mutex_unlock(&cpu_hotplug.lock);

}
EXPORT_SYMBOL_GPL(get_online_cpus);

/** 20140510    
 * cpu_hotplug의 사용을 끝낸다.
 **/
void put_online_cpus(void)
{
	if (cpu_hotplug.active_writer == current)
		return;
	mutex_lock(&cpu_hotplug.lock);
	/** 20140510    
	 * cpu_hotplug가 다른 작업에 의해 참조되지 않고 (refcount),
	 * active_write가 존재한다면 (cpu_hotplug_begin 상태에서 대기)
	 * 해당 task를 깨운다.
	 **/
	if (!--cpu_hotplug.refcount && unlikely(cpu_hotplug.active_writer))
		wake_up_process(cpu_hotplug.active_writer);
	mutex_unlock(&cpu_hotplug.lock);

}
EXPORT_SYMBOL_GPL(put_online_cpus);

/*
 * This ensures that the hotplug operation can begin only when the
 * refcount goes to zero.
 *
 * Note that during a cpu-hotplug operation, the new readers, if any,
 * will be blocked by the cpu_hotplug.lock
 *
 * Since cpu_hotplug_begin() is always called after invoking
 * cpu_maps_update_begin(), we can be sure that only one writer is active.
 *
 * Note that theoretically, there is a possibility of a livelock:
 * - Refcount goes to zero, last reader wakes up the sleeping
 *   writer.
 * - Last reader unlocks the cpu_hotplug.lock.
 * - A new reader arrives at this moment, bumps up the refcount.
 * - The writer acquires the cpu_hotplug.lock finds the refcount
 *   non zero and goes to sleep again.
 *
 * However, this is very difficult to achieve in practice since
 * get_online_cpus() not an api which is called all that often.
 *
 */
/** 20150808    
 * hotplug operation 작업 전에, refcount가 0인 경우에만 진입하도록 함.
 * cpu_maps_update_begin 이후 호출되므로 하나의 writer만 활성화되는 것이 보장된다.
 **/
static void cpu_hotplug_begin(void)
{
	/** 20150808    
	 * 현재 task를 active_writer로 기록한다.
	 **/
	cpu_hotplug.active_writer = current;

	/** 20150808    
	 * refcount가 0일 때(get_online_cpus가 아닌 상태)까지 lock을 잡은 상태로 리턴.
	 **/
	for (;;) {
		mutex_lock(&cpu_hotplug.lock);
		if (likely(!cpu_hotplug.refcount))
			break;
		__set_current_state(TASK_UNINTERRUPTIBLE);
		mutex_unlock(&cpu_hotplug.lock);
		schedule();
	}
}

/** 20150808    
 * cpu_hotplug의 active_writer를 비우고, hotplug lock을 해제한다.
 **/
static void cpu_hotplug_done(void)
{
	cpu_hotplug.active_writer = NULL;
	mutex_unlock(&cpu_hotplug.lock);
}

#else /* #if CONFIG_HOTPLUG_CPU */
static void cpu_hotplug_begin(void) {}
static void cpu_hotplug_done(void) {}
#endif	/* #else #if CONFIG_HOTPLUG_CPU */

/* Need to know about CPUs going up/down? */
/** 20130727    
 * cpu_chain이라는 notifier head에 nb를 등록한다.
 * notifier_block은 우선순위가 높은 순서부터 정렬된다.
 *
 * 이후 cpu_notify를 통해 notify를 준다.
 **/
int __ref register_cpu_notifier(struct notifier_block *nb)
{
	int ret;
	/** 20130727    
	 * cpu 관련 자료구조 전후에  호출해 원자성을 보장한다.
	 **/
	cpu_maps_update_begin();
	/** 20130727    
	 * cpu_chain이라는 notifier chain에 notifier_block을 등록한다.
	 **/
	ret = raw_notifier_chain_register(&cpu_chain, nb);
	cpu_maps_update_done();
	return ret;
}

/** 20140927    
 * cpu_chain에 등록된 notifier_block을 호출한다.
 *
 * val : 전달할 event
 * v   : 보통 cpu번호를 전달한다.
 * nr_to_call : 호출할 콜백 갯수
 * nr_calls   : 호출된 콜백 갯수
 **/
static int __cpu_notify(unsigned long val, void *v, int nr_to_call,
			int *nr_calls)
{
	int ret;

	ret = __raw_notifier_call_chain(&cpu_chain, val, v, nr_to_call,
					nr_calls);

	return notifier_to_errno(ret);
}

/** 20140927    
 * register_cpu_notifier로 등록한 notifier_block을 호출한다.
 **/
static int cpu_notify(unsigned long val, void *v)
{
	return __cpu_notify(val, v, -1, NULL);
}

#ifdef CONFIG_HOTPLUG_CPU

/** 20140927    
 **/
static void cpu_notify_nofail(unsigned long val, void *v)
{
	BUG_ON(cpu_notify(val, v));
}
EXPORT_SYMBOL(register_cpu_notifier);

void __ref unregister_cpu_notifier(struct notifier_block *nb)
{
	cpu_maps_update_begin();
	raw_notifier_chain_unregister(&cpu_chain, nb);
	cpu_maps_update_done();
}
EXPORT_SYMBOL(unregister_cpu_notifier);

/**
 * clear_tasks_mm_cpumask - Safely clear tasks' mm_cpumask for a CPU
 * @cpu: a CPU id
 *
 * This function walks all processes, finds a valid mm struct for each one and
 * then clears a corresponding bit in mm's cpumask.  While this all sounds
 * trivial, there are various non-obvious corner cases, which this function
 * tries to solve in a safe manner.
 *
 * Also note that the function uses a somewhat relaxed locking scheme, so it may
 * be called only for an already offlined CPU.
 */
void clear_tasks_mm_cpumask(int cpu)
{
	struct task_struct *p;

	/*
	 * This function is called after the cpu is taken down and marked
	 * offline, so its not like new tasks will ever get this cpu set in
	 * their mm mask. -- Peter Zijlstra
	 * Thus, we may use rcu_read_lock() here, instead of grabbing
	 * full-fledged tasklist_lock.
	 */
	WARN_ON(cpu_online(cpu));
	rcu_read_lock();
	for_each_process(p) {
		struct task_struct *t;

		/*
		 * Main thread might exit, but other threads may still have
		 * a valid mm. Find one.
		 */
		t = find_lock_task_mm(p);
		if (!t)
			continue;
		cpumask_clear_cpu(cpu, mm_cpumask(t->mm));
		task_unlock(t);
	}
	rcu_read_unlock();
}

static inline void check_for_tasks(int cpu)
{
	struct task_struct *p;

	write_lock_irq(&tasklist_lock);
	for_each_process(p) {
		if (task_cpu(p) == cpu && p->state == TASK_RUNNING &&
		    (p->utime || p->stime))
			printk(KERN_WARNING "Task %s (pid = %d) is on cpu %d "
				"(state = %ld, flags = %x)\n",
				p->comm, task_pid_nr(p), cpu,
				p->state, p->flags);
	}
	write_unlock_irq(&tasklist_lock);
}

struct take_cpu_down_param {
	unsigned long mod;
	void *hcpu;
};

/* Take this CPU down. */
static int __ref take_cpu_down(void *_param)
{
	struct take_cpu_down_param *param = _param;
	int err;

	/* Ensure this CPU doesn't handle any more interrupts. */
	err = __cpu_disable();
	if (err < 0)
		return err;

	cpu_notify(CPU_DYING | param->mod, param->hcpu);
	return 0;
}

/* Requires cpu_add_remove_lock to be held */
static int __ref _cpu_down(unsigned int cpu, int tasks_frozen)
{
	int err, nr_calls = 0;
	/** 20141018    
	 * handled cpu.
	 **/
	void *hcpu = (void *)(long)cpu;
	unsigned long mod = tasks_frozen ? CPU_TASKS_FROZEN : 0;
	struct take_cpu_down_param tcd_param = {
		.mod = mod,
		.hcpu = hcpu,
	};

	if (num_online_cpus() == 1)
		return -EBUSY;

	if (!cpu_online(cpu))
		return -EINVAL;

	cpu_hotplug_begin();

	err = __cpu_notify(CPU_DOWN_PREPARE | mod, hcpu, -1, &nr_calls);
	if (err) {
		nr_calls--;
		__cpu_notify(CPU_DOWN_FAILED | mod, hcpu, nr_calls, NULL);
		printk("%s: attempt to take down CPU %u failed\n",
				__func__, cpu);
		goto out_release;
	}

	err = __stop_machine(take_cpu_down, &tcd_param, cpumask_of(cpu));
	if (err) {
		/* CPU didn't die: tell everyone.  Can't complain. */
		cpu_notify_nofail(CPU_DOWN_FAILED | mod, hcpu);

		goto out_release;
	}
	BUG_ON(cpu_online(cpu));

	/*
	 * The migration_call() CPU_DYING callback will have removed all
	 * runnable tasks from the cpu, there's only the idle task left now
	 * that the migration thread is done doing the stop_machine thing.
	 *
	 * Wait for the stop thread to go away.
	 */
	while (!idle_cpu(cpu))
		cpu_relax();

	/* This actually kills the CPU. */
	__cpu_die(cpu);

	/* CPU is completely dead: tell everyone.  Too late to complain. */
	/** 20140927    
	 * hcpu가 정상적으로 죽은 경우 CPU_DEAD message를 보낸다.
	 **/
	cpu_notify_nofail(CPU_DEAD | mod, hcpu);

	check_for_tasks(cpu);

out_release:
	cpu_hotplug_done();
	if (!err)
		cpu_notify_nofail(CPU_POST_DEAD | mod, hcpu);
	return err;
}

int __ref cpu_down(unsigned int cpu)
{
	int err;

	cpu_maps_update_begin();

	if (cpu_hotplug_disabled) {
		err = -EBUSY;
		goto out;
	}

	err = _cpu_down(cpu, 0);

out:
	cpu_maps_update_done();
	return err;
}
EXPORT_SYMBOL(cpu_down);
#endif /*CONFIG_HOTPLUG_CPU*/

/* Requires cpu_add_remove_lock to be held */
/** 20150808    
 * 주어진 cpu를 up시킨다. 
 * 임계구역의 직렬화를 위해 cpu_maps_update_begin ~ cpu_maps_update_done 사이에서 진행한다.
 **/
static int __cpuinit _cpu_up(unsigned int cpu, int tasks_frozen)
{
	int ret, nr_calls = 0;
	void *hcpu = (void *)(long)cpu;
	unsigned long mod = tasks_frozen ? CPU_TASKS_FROZEN : 0;
	struct task_struct *idle;

	if (cpu_online(cpu) || !cpu_present(cpu))
		return -EINVAL;

	/** 20150808    
	 * cpu hotplug 동작이 진행되므로 lock을 잡은 상태로 진행한다.
	 **/
	cpu_hotplug_begin();

	/** 20150118    
	 * idle_threads_init에서 넣어둔 idle thread를 cpu에 대한 idle task로 지정하고,
	 * 해당 task를 받아온다.
	 **/
	idle = idle_thread_get(cpu);
	if (IS_ERR(idle)) {
		ret = PTR_ERR(idle);
		goto out;
	}

	/** 20150801    
	 * __cpu_up 이전에 CPU_UP_PREPARE notify를 보낸다.
	 * 
	 * 등록되어 있는 각 nb의 callback 함수들에서 CPU_UP_PREPARE에 해당하는 동작을 실행한다.
	 **/
	ret = __cpu_notify(CPU_UP_PREPARE | mod, hcpu, -1, &nr_calls);
	/** 20150801    
	 *
	 **/
	if (ret) {
		nr_calls--;
		printk(KERN_WARNING "%s: attempt to bring up CPU %u failed\n",
				__func__, cpu);
		goto out_notify;
	}

	/* Arch-specific enabling code. */
	/** 20150808    
	 * architecture에서 제공하는 방식으로 cpu를 up시킨다.
	 **/
	ret = __cpu_up(cpu, idle);
	if (ret != 0)
		goto out_notify;
	BUG_ON(!cpu_online(cpu));

	/* Now call notifier in preparation. */
	/** 20150808    
	 * cpu가 up 되었으므로 CPU_ONLINE notify를 날린다.
	 **/
	cpu_notify(CPU_ONLINE | mod, hcpu);

out_notify:
	/** 20150801    
	 * ret이 0이 아닌 경우, 성공한 notifier chain의 callback 함수들에게
	 * CPU_UP_CANCELED notify를 보낸다.
	 **/
	if (ret != 0)
		__cpu_notify(CPU_UP_CANCELED | mod, hcpu, nr_calls, NULL);
out:
	/** 20150808    
	 * cpu hotplug 작업을 마치고 lock을 해제한다.
	 **/
	cpu_hotplug_done();

	return ret;
}

/** 20150808    
 * 해당 cpu를 up시킨다.
 **/
int __cpuinit cpu_up(unsigned int cpu)
{
	int err = 0;

#ifdef	CONFIG_MEMORY_HOTPLUG
	int nid;
	pg_data_t	*pgdat;
#endif

	/** 20150801    
	 * cpu가 possible하지 않는다면 error.
	 **/
	if (!cpu_possible(cpu)) {
		printk(KERN_ERR "can't online cpu %d because it is not "
			"configured as may-hotadd at boot time\n", cpu);
#if defined(CONFIG_IA64)
		printk(KERN_ERR "please check additional_cpus= boot "
				"parameter\n");
#endif
		return -EINVAL;
	}

	/** 20150801    
	 * MEMORY HOTPLUG가 정의되어 있지 않다.
	 **/
#ifdef	CONFIG_MEMORY_HOTPLUG
	nid = cpu_to_node(cpu);
	if (!node_online(nid)) {
		err = mem_online_node(nid);
		if (err)
			return err;
	}

	pgdat = NODE_DATA(nid);
	if (!pgdat) {
		printk(KERN_ERR
			"Can't online cpu %d due to NULL pgdat\n", cpu);
		return -ENOMEM;
	}

	if (pgdat->node_zonelists->_zonerefs->zone == NULL) {
		mutex_lock(&zonelists_mutex);
		build_all_zonelists(NULL, NULL);
		mutex_unlock(&zonelists_mutex);
	}
#endif

	/** 20150801    
	 * cpu online, present mask가 변경되는 작업을 serialize 한다.
	 **/
	cpu_maps_update_begin();

	if (cpu_hotplug_disabled) {
		err = -EBUSY;
		goto out;
	}

	/** 20150808    
	 * cpu를 up시킨다.
	 *
	 * 내부에서 platform 의존적인 함수를 호출한다.
	 **/
	err = _cpu_up(cpu, 0);

out:
	/** 20150808    
	 * cpu online, present mask가 변경되는 임계구역의 끝.
	 **/
	cpu_maps_update_done();
	return err;
}
EXPORT_SYMBOL_GPL(cpu_up);

#ifdef CONFIG_PM_SLEEP_SMP
/** 20151010    
 * PM_SLEEP_SMP가 선언되어 있으므로 frozen_cpus인 cpumask를 사용한다.
 **/
static cpumask_var_t frozen_cpus;

void __weak arch_disable_nonboot_cpus_begin(void)
{
}

void __weak arch_disable_nonboot_cpus_end(void)
{
}

int disable_nonboot_cpus(void)
{
	int cpu, first_cpu, error = 0;

	cpu_maps_update_begin();
	first_cpu = cpumask_first(cpu_online_mask);
	/*
	 * We take down all of the non-boot CPUs in one shot to avoid races
	 * with the userspace trying to use the CPU hotplug at the same time
	 */
	cpumask_clear(frozen_cpus);
	arch_disable_nonboot_cpus_begin();

	printk("Disabling non-boot CPUs ...\n");
	for_each_online_cpu(cpu) {
		if (cpu == first_cpu)
			continue;
		error = _cpu_down(cpu, 1);
		if (!error)
			cpumask_set_cpu(cpu, frozen_cpus);
		else {
			printk(KERN_ERR "Error taking CPU%d down: %d\n",
				cpu, error);
			break;
		}
	}

	arch_disable_nonboot_cpus_end();

	if (!error) {
		BUG_ON(num_online_cpus() > 1);
		/* Make sure the CPUs won't be enabled by someone else */
		cpu_hotplug_disabled = 1;
	} else {
		printk(KERN_ERR "Non-boot CPUs are not disabled\n");
	}
	cpu_maps_update_done();
	return error;
}

void __weak arch_enable_nonboot_cpus_begin(void)
{
}

void __weak arch_enable_nonboot_cpus_end(void)
{
}

void __ref enable_nonboot_cpus(void)
{
	int cpu, error;

	/* Allow everyone to use the CPU hotplug again */
	cpu_maps_update_begin();
	cpu_hotplug_disabled = 0;
	if (cpumask_empty(frozen_cpus))
		goto out;

	printk(KERN_INFO "Enabling non-boot CPUs ...\n");

	arch_enable_nonboot_cpus_begin();

	for_each_cpu(cpu, frozen_cpus) {
		error = _cpu_up(cpu, 1);
		if (!error) {
			printk(KERN_INFO "CPU%d is up\n", cpu);
			continue;
		}
		printk(KERN_WARNING "Error taking CPU%d up: %d\n", cpu, error);
	}

	arch_enable_nonboot_cpus_end();

	cpumask_clear(frozen_cpus);
out:
	cpu_maps_update_done();
}

/** 20151010    
 * frozen_cpus를 위한 cpumask 변수를 할당한다.
 *
 * CPUMASK_OFFSTACK를 선언하지 않았으므로 실제로 취하는 동작은 없다.
 **/
static int __init alloc_frozen_cpus(void)
{
	if (!alloc_cpumask_var(&frozen_cpus, GFP_KERNEL|__GFP_ZERO))
		return -ENOMEM;
	return 0;
}
core_initcall(alloc_frozen_cpus);

/*
 * Prevent regular CPU hotplug from racing with the freezer, by disabling CPU
 * hotplug when tasks are about to be frozen. Also, don't allow the freezer
 * to continue until any currently running CPU hotplug operation gets
 * completed.
 * To modify the 'cpu_hotplug_disabled' flag, we need to acquire the
 * 'cpu_add_remove_lock'. And this same lock is also taken by the regular
 * CPU hotplug path and released only after it is complete. Thus, we
 * (and hence the freezer) will block here until any currently running CPU
 * hotplug operation gets completed.
 */
/** 20151010    
 * CPU hotplug 동작과 freezer 사이의 경쟁을 방지하기 위해 freeze 동작 전
 * hotplug 불가상태로 만든다.
 **/
void cpu_hotplug_disable_before_freeze(void)
{
	cpu_maps_update_begin();
	cpu_hotplug_disabled = 1;
	cpu_maps_update_done();
}


/*
 * When tasks have been thawed, re-enable regular CPU hotplug (which had been
 * disabled while beginning to freeze tasks).
 */
/** 20151010    
 * tasks들(freezer 포함)의 재개가 이뤄진 후, CPU hotplug 동작을 가능하도록 한다.
 **/
void cpu_hotplug_enable_after_thaw(void)
{
	cpu_maps_update_begin();
	cpu_hotplug_disabled = 0;
	cpu_maps_update_done();
}

/*
 * When callbacks for CPU hotplug notifications are being executed, we must
 * ensure that the state of the system with respect to the tasks being frozen
 * or not, as reported by the notification, remains unchanged *throughout the
 * duration* of the execution of the callbacks.
 * Hence we need to prevent the freezer from racing with regular CPU hotplug.
 *
 * This synchronization is implemented by mutually excluding regular CPU
 * hotplug and Suspend/Hibernate call paths by hooking onto the Suspend/
 * Hibernate notifications.
 */
/** 20151010    
 * PM 관련 작업이 이뤄지기 전, CPU hotplug와 freezer 사이의 경쟁을 회피하기 위한
 * 콜백 함수.
 *
 * suspend : machine state is saved in RAM
 * hibernate : machine state is saved in swap
 *
 * http://events.linuxfoundation.org/sites/events/files/slides/kernel_PM_plain.pdf
 **/
static int
cpu_hotplug_pm_callback(struct notifier_block *nb,
			unsigned long action, void *ptr)
{
	switch (action) {

	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		cpu_hotplug_disable_before_freeze();
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		cpu_hotplug_enable_after_thaw();
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}


/** 20151003    
 * PM 관련 동기화를 위한 cpu hotplug notifier block을 선언하고 등록한다.
 **/
static int __init cpu_hotplug_pm_sync_init(void)
{
	pm_notifier(cpu_hotplug_pm_callback, 0);
	return 0;
}
core_initcall(cpu_hotplug_pm_sync_init);

#endif /* CONFIG_PM_SLEEP_SMP */

/**
 * notify_cpu_starting(cpu) - call the CPU_STARTING notifiers
 * @cpu: cpu that just started
 *
 * This function calls the cpu_chain notifiers with CPU_STARTING.
 * It must be called by the arch code on the new cpu, before the new cpu
 * enables interrupts and before the "boot" cpu returns from __cpu_up().
 */
/** 20150808    
 * 해당 cpu가 시작했음을 cpu_notify로 통보한다.
 *
 * PM_SLEEP에서 깨어난 경우 CPU_STARTING_FROZEN를 그렇지 않은 경우 CPU_STARTING.
 **/
void __cpuinit notify_cpu_starting(unsigned int cpu)
{
	unsigned long val = CPU_STARTING;

	/** 20150808    
	 * PM_SLEEP_SMP가 정의되어 있고, 이 cpu가 frozen_cpus에 들어 있다면
	 * val를 CPU_STARTING_FROZEN로 지정한다.
	 **/
#ifdef CONFIG_PM_SLEEP_SMP
	if (frozen_cpus != NULL && cpumask_test_cpu(cpu, frozen_cpus))
		val = CPU_STARTING_FROZEN;
#endif /* CONFIG_PM_SLEEP_SMP */
	/** 20150808    
	 * 설정된 val로 cpu notify를 날린다.
	 *
	 * PM_SLEEP이 아닌 경우 CPU_STARTING notify를 날리고, 
	 * 예를 들어 sched의 경우, active mask에 해당 cpu를 추가한다.
	 **/
	cpu_notify(val, (void *)(long)cpu);
}

#endif /* CONFIG_SMP */

/*
 * cpu_bit_bitmap[] is a special, "compressed" data structure that
 * represents all NR_CPUS bits binary values of 1<<nr.
 *
 * It is used by cpumask_of() to get a constant address to a CPU
 * mask value that has a single bit set only.
 */

/* cpu_bit_bitmap[0] is empty - so we can back into it */
/** 20141122    
 * mask를 1개, 2개, 4개, 8개 선언하는 매크로.
 * 각 mask에는 비트당 
 **/
#define MASK_DECLARE_1(x)	[x+1][0] = (1UL << (x))
#define MASK_DECLARE_2(x)	MASK_DECLARE_1(x), MASK_DECLARE_1(x+1)
#define MASK_DECLARE_4(x)	MASK_DECLARE_2(x), MASK_DECLARE_2(x+2)
#define MASK_DECLARE_8(x)	MASK_DECLARE_4(x), MASK_DECLARE_4(x+4)

/** 20141122    
 * 비트가 설정된 비트맵을 왼쪽 끝으로 몰아놓은 형태의 table.
 * (re-ordering)
 *
 * 이렇게 만든 이유는, 만약 bitmap을 단순히 table로 만든다면
 * cpu_bit_bitmap[NR_CPUS][BITS_PER_LONG]가 되어 cpu의 개수가 늘어날수록
 * read-only 영역 메모리 낭비가 커진다.
 *
 * 이 테이블 형식으로 만든 뒤 cpumask를 가져올 때는
 * get_cpu_mask(cpu)를 사용한다.
 **/
const unsigned long cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)] = {

	MASK_DECLARE_8(0),	MASK_DECLARE_8(8),
	MASK_DECLARE_8(16),	MASK_DECLARE_8(24),
#if BITS_PER_LONG > 32
	MASK_DECLARE_8(32),	MASK_DECLARE_8(40),
	MASK_DECLARE_8(48),	MASK_DECLARE_8(56),
#endif
};
EXPORT_SYMBOL_GPL(cpu_bit_bitmap);

/** 20141122    
 * cpu 개수(NR_CPUS)만큼 1로 채워진 cpu_mask를 선언.
 **/
const DECLARE_BITMAP(cpu_all_bits, NR_CPUS) = CPU_BITS_ALL;
EXPORT_SYMBOL(cpu_all_bits);

#ifdef CONFIG_INIT_ALL_POSSIBLE
static DECLARE_BITMAP(cpu_possible_bits, CONFIG_NR_CPUS) __read_mostly
	= CPU_BITS_ALL;
#else
/** 20130518    
 * unsigned long cpu_possible_bits[BITS_TO_LONGS(CONFIG_NR_CPUS)]
 *   -> unsigned long cpu_possible_bits[1]
 **/
static DECLARE_BITMAP(cpu_possible_bits, CONFIG_NR_CPUS) __read_mostly;
#endif
/** 20150523    
 * cpu_XXX_bits로 cpumask로 변환한다.
 *
 * cpu_possible_mask - 해당 비트에 대한 CPU가 존재할 수 있다.
 * cpu_present_mask - 해당 비트에 대한 CPU가 존재한다.
 * cpu_online_mask - 해당 비트에 대한 CPU가 존재하며 스케줄러가 이를 관리한다.
 * cpu_active_mask - 해당 비트에 대한 CPU가 존재하며 task migration 시 이를 이용할 수 있다.
 *
 * [출처] http://studyfoss.egloos.com/5444259
 **/
const struct cpumask *const cpu_possible_mask = to_cpumask(cpu_possible_bits);
EXPORT_SYMBOL(cpu_possible_mask);

static DECLARE_BITMAP(cpu_online_bits, CONFIG_NR_CPUS) __read_mostly;
const struct cpumask *const cpu_online_mask = to_cpumask(cpu_online_bits);
EXPORT_SYMBOL(cpu_online_mask);

static DECLARE_BITMAP(cpu_present_bits, CONFIG_NR_CPUS) __read_mostly;
const struct cpumask *const cpu_present_mask = to_cpumask(cpu_present_bits);
EXPORT_SYMBOL(cpu_present_mask);

static DECLARE_BITMAP(cpu_active_bits, CONFIG_NR_CPUS) __read_mostly;
const struct cpumask *const cpu_active_mask = to_cpumask(cpu_active_bits);
EXPORT_SYMBOL(cpu_active_mask);

/** 20130518    
 * cpu possible bitmap mask 설정.
 *
 * cpu: n번째 cpu bit.
 * possible : true - set, false - clear
 **/
void set_cpu_possible(unsigned int cpu, bool possible)
{
	if (possible)
		cpumask_set_cpu(cpu, to_cpumask(cpu_possible_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_possible_bits));
}

void set_cpu_present(unsigned int cpu, bool present)
{
	if (present)
		cpumask_set_cpu(cpu, to_cpumask(cpu_present_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_present_bits));
}

/** 20121208
 * cpu online bits에 cpu에 해당하는 비트를 설정/제거한다.
 *
 * online=true이면 cpu_online_bits변수에 cpu bit를 1로 세팅한다
 * online=false이면 cpu_online_bits변수에 cpu bit를 0으로 클리어한다
 **/
void set_cpu_online(unsigned int cpu, bool online)
{
	if (online)
		cpumask_set_cpu(cpu, to_cpumask(cpu_online_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_online_bits));
}

/** 20150725    
 * active에 따라 cpu를 active mask에 포함하거나 제거한다.
 **/
void set_cpu_active(unsigned int cpu, bool active)
{
	if (active)
		cpumask_set_cpu(cpu, to_cpumask(cpu_active_bits));
	else
		cpumask_clear_cpu(cpu, to_cpumask(cpu_active_bits));
}

/** 20150613    
 * 소스 cpumask를 present mask에 복사.
 **/
void init_cpu_present(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_present_bits), src);
}

/** 20150613    
 * 소스 cpumask를 possible mask에 복사.
 **/
void init_cpu_possible(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_possible_bits), src);
}

/** 20150613    
 * 소스 cpumask를 online mask에 복사.
 **/
void init_cpu_online(const struct cpumask *src)
{
	cpumask_copy(to_cpumask(cpu_online_bits), src);
}
