/*
 * Common SMP CPU bringup/teardown functions
 */
#include <linux/err.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/percpu.h>

#include "smpboot.h"

#ifdef CONFIG_GENERIC_SMP_IDLE_THREAD
/*
 * For the hotplug case we keep the task structs around and reuse
 * them.
 */
/** 20140426    
 * idle_threads는 static per cpu 변수.
 **/
static DEFINE_PER_CPU(struct task_struct *, idle_threads);

/** 20150118    
 * cpu에 대한 idle thread를 설정하고, task를 리턴한다.
 **/
struct task_struct * __cpuinit idle_thread_get(unsigned int cpu)
{
	struct task_struct *tsk = per_cpu(idle_threads, cpu);

	if (!tsk)
		return ERR_PTR(-ENOMEM);
	/** 20150117    
	 * percpu idle_threads인 task를 가져와 cpu에 대한 idle task로 설정한다.
	 **/
	init_idle(tsk, cpu);
	return tsk;
}

/** 20140426    
 * boot cpu의 idle_threads를 current task (init_task)로 지정한다.
 **/
void __init idle_thread_set_boot_cpu(void)
{
	per_cpu(idle_threads, smp_processor_id()) = current;
}

/**
 * idle_init - Initialize the idle thread for a cpu
 * @cpu:	The cpu for which the idle thread should be initialized
 *
 * Creates the thread if it does not exist.
 */
/** 20150801    
 * cpu를 위한 idle task를 초기화 한다.
 *
 * percpu idle_threads가 지정되지 않은 경우 idle task를 복사해 초기화 하고,
 * percpu idle_threads로 지정한다.
 **/
static inline void idle_init(unsigned int cpu)
{
	struct task_struct *tsk = per_cpu(idle_threads, cpu);

	/** 20150801    
	 * percpu idle_threads에서 해당 cpu에 대한 변수에 접근해 지정되지 않았을 경우
	 **/
	if (!tsk) {
		/** 20150118    
		 * 현재 thread(init_task)를 복사해 지정한 cpu가 사용할 task를 받아온다.
		 * 해당 cpu의 idle thread로 task를 지정한 상태이다.
		 **/
		tsk = fork_idle(cpu);
		if (IS_ERR(tsk))
			pr_err("SMP: fork_idle() failed for CPU %u\n", cpu);
		else
			/** 20150117    
			 * per_cpu변수 idle_threads의 해당 cpu의 task로 fork한 tsk를 지정한다.
			 **/
			per_cpu(idle_threads, cpu) = tsk;
	}
}

/**
 * idle_threads_init - Initialize idle threads for all cpus
 */
/** 20150118    
 * boot cpu를 제외한 cpu들에 대해 idle_init을 진행한다.
 *
 * idle task를 생성해 idle_threads에 지정한다.
 **/
void __init idle_threads_init(void)
{
	unsigned int cpu, boot_cpu;

	/** 20150118    
	 * 현재 task가 실행 중인 cpu는 boot_cpu로 이미 초기화 시켜두었다.
	 * cpu_possible_mask에서 boot_cpu를 제외한 cpu에 대해 idle_init을 실행한다.
	 **/
	boot_cpu = smp_processor_id();

	for_each_possible_cpu(cpu) {
		if (cpu != boot_cpu)
			idle_init(cpu);
	}
}
#endif
