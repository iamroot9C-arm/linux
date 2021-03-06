#ifndef __LINUX_SMP_H
#define __LINUX_SMP_H

/*
 *	Generic SMP support
 *		Alan Cox. <alan@redhat.com>
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/init.h>

extern void cpu_idle(void);

typedef void (*smp_call_func_t)(void *info);
/** 20140621
 * call_function_data의 member이기도 함.
 **/
struct call_single_data {
	struct list_head list;
	smp_call_func_t func;
	void *info;
	u16 flags;
	u16 priv;
};

/* total number of cpus in this system (may exceed NR_CPUS) */
extern unsigned int total_cpus;

int smp_call_function_single(int cpuid, smp_call_func_t func, void *info,
			     int wait);

#ifdef CONFIG_SMP

#include <linux/preempt.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/thread_info.h>
#include <asm/smp.h>

/*
 * main cross-CPU interfaces, handles INIT, TLB flush, STOP, etc.
 * (defined in asm header):
 */

/*
 * stops all CPUs but the current one:
 */
extern void smp_send_stop(void);

/*
 * sends a 'reschedule' event to another CPU:
 */
extern void smp_send_reschedule(int cpu);


/*
 * Prepare machine for booting other CPUs.
 */
extern void smp_prepare_cpus(unsigned int max_cpus);

/*
 * Bring a CPU up
 */
extern int __cpu_up(unsigned int cpunum, struct task_struct *tidle);

/*
 * Final polishing of CPUs
 */
extern void smp_cpus_done(unsigned int max_cpus);

/*
 * Call a function on all other processors
 */
int smp_call_function(smp_call_func_t func, void *info, int wait);
void smp_call_function_many(const struct cpumask *mask,
			    smp_call_func_t func, void *info, bool wait);

void __smp_call_function_single(int cpuid, struct call_single_data *data,
				int wait);

int smp_call_function_any(const struct cpumask *mask,
			  smp_call_func_t func, void *info, int wait);

void kick_all_cpus_sync(void);

/*
 * Generic and arch helpers
 */
#ifdef CONFIG_USE_GENERIC_SMP_HELPERS
void __init call_function_init(void);
void generic_smp_call_function_single_interrupt(void);
void generic_smp_call_function_interrupt(void);
#else
static inline void call_function_init(void) { }
#endif

/*
 * Call a function on all processors
 */
int on_each_cpu(smp_call_func_t func, void *info, int wait);

/*
 * Call a function on processors specified by mask, which might include
 * the local one.
 */
void on_each_cpu_mask(const struct cpumask *mask, smp_call_func_t func,
		void *info, bool wait);

/*
 * Call a function on each processor for which the supplied function
 * cond_func returns a positive value. This may include the local
 * processor.
 */
void on_each_cpu_cond(bool (*cond_func)(int cpu, void *info),
		smp_call_func_t func, void *info, bool wait,
		gfp_t gfp_flags);

/*
 * Mark the boot cpu "online" so that it can call console drivers in
 * printk() and can access its per-cpu storage.
 */
void smp_prepare_boot_cpu(void);

extern unsigned int setup_max_cpus;
extern void __init setup_nr_cpu_ids(void);
extern void __init smp_init(void);

#else /* !SMP */

static inline void smp_send_stop(void) { }

/*
 *	These macros fold the SMP functionality into a single CPU system
 */
#define raw_smp_processor_id()			0
static inline int up_smp_call_function(smp_call_func_t func, void *info)
{
	return 0;
}
#define smp_call_function(func, info, wait) \
			(up_smp_call_function(func, info))
/** 20131026
 * 20160604 SMP를 기준으로 분석하므로 해당 안 함
 *
 * local irq disable, enable을 해주는 이유는???
 * func 함수를 실행하는 매크로
 **/
#define on_each_cpu(func,info,wait)		\
	({					\
		local_irq_disable();		\
		func(info);			\
		local_irq_enable();		\
		0;				\
	})
/*
 * Note we still need to test the mask even for UP
 * because we actually can get an empty mask from
 * code that on SMP might call us without the local
 * CPU in the mask.
 */
#define on_each_cpu_mask(mask, func, info, wait) \
	do {						\
		if (cpumask_test_cpu(0, (mask))) {	\
			local_irq_disable();		\
			(func)(info);			\
			local_irq_enable();		\
		}					\
	} while (0)
/*
 * Preemption is disabled here to make sure the cond_func is called under the
 * same condtions in UP and SMP.
 */
/** 20140510
 * 선점 불가 상태에서 cond_func을 만족할 경우
 * atomic context (SMP인 경우 포함)에서 func을 실행한다.
 *
 * 0번 cpu가 조건을 만족할 경우 func 구현에 따라 각 cpu마다 동작을 수행한다.
 **/
#define on_each_cpu_cond(cond_func, func, info, wait, gfp_flags)\
	do {							\
		void *__info = (info);				\
		preempt_disable();				\
		if ((cond_func)(0, __info)) {			\
			local_irq_disable();			\
			(func)(__info);				\
			local_irq_enable();			\
		}						\
		preempt_enable();				\
	} while (0)

static inline void smp_send_reschedule(int cpu) { }
#define smp_prepare_boot_cpu()			do {} while (0)
#define smp_call_function_many(mask, func, info, wait) \
			(up_smp_call_function(func, info))
static inline void call_function_init(void) { }

static inline int
smp_call_function_any(const struct cpumask *mask, smp_call_func_t func,
		      void *info, int wait)
{
	return smp_call_function_single(0, func, info, wait);
}

static inline void kick_all_cpus_sync(void) {  }

#endif /* !SMP */

/*
 * smp_processor_id(): get the current CPU ID.
 *
 * if DEBUG_PREEMPT is enabled then we check whether it is
 * used in a preemption-safe way. (smp_processor_id() is safe
 * if it's used in a preemption-off critical section, or in
 * a thread that is bound to the current CPU.)
 *
 * NOTE: raw_smp_processor_id() is for internal use only
 * (smp_processor_id() is the preferred variant), but in rare
 * instances it might also be used to turn off false positives
 * (i.e. smp_processor_id() use that the debugging code reports but
 * which use for some reason is legal). Don't use this to hack around
 * the warning message, as your code might not work under PREEMPT.
 */
#ifdef CONFIG_DEBUG_PREEMPT
  extern unsigned int debug_smp_processor_id(void);
# define smp_processor_id() debug_smp_processor_id()
#else
/** 20130713
 * CONFIG_DEBUG_PREEMPT 정의되어 있지 않아 다음 매크로가 호출된다.
 * 현재 task의 thread_info로부터 cpu번호를 리턴한다.
 **/
# define smp_processor_id() raw_smp_processor_id()
#endif
/** 20131221
 * 선점불가로 만들고 현재 cpuid를 리턴한다.
 **/
#define get_cpu()		({ preempt_disable(); smp_processor_id(); })
/** 20131221
 * 선점 가능으로 만든다.
 *   - task 선점이 대기 중이라면 __schedule 함수를 실행한다.
 **/
#define put_cpu()		preempt_enable()

/*
 * Callback to arch code if there's nosmp or maxcpus=0 on the
 * boot command line:
 */
extern void arch_disable_smp_support(void);

void smp_setup_processor_id(void);

#endif /* __LINUX_SMP_H */
