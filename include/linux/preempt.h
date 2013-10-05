#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

/*
 * include/linux/preempt.h - macros for accessing and manipulating
 * preempt_count (used for kernel preemption, interrupt count, etc.)
 */

#include <linux/thread_info.h>
#include <linux/linkage.h>
#include <linux/list.h>

#if defined(CONFIG_DEBUG_PREEMPT) || defined(CONFIG_PREEMPT_TRACER)
  extern void add_preempt_count(int val);
  extern void sub_preempt_count(int val);
#else
/** 20130413
 * preempt count 를 val만큼 증가/감소 시킨다.
 */
# define add_preempt_count(val)	do { preempt_count() += (val); } while (0)
# define sub_preempt_count(val)	do { preempt_count() -= (val); } while (0)
#endif

/** 20130713    
 * preempt_count++;
 * preempt_count--;
 **/
#define inc_preempt_count() add_preempt_count(1)
#define dec_preempt_count() sub_preempt_count(1)

/** 20131005    
 * 현재 task의 thread_info에서 preempt_count를 리턴한다.
 **/
#define preempt_count()	(current_thread_info()->preempt_count)

#ifdef CONFIG_PREEMPT

asmlinkage void preempt_schedule(void);

#define preempt_check_resched() \
do { \
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED))) \
		preempt_schedule(); \
} while (0)

#else /* !CONFIG_PREEMPT */

#define preempt_check_resched()		do { } while (0)

#endif /* CONFIG_PREEMPT */


#ifdef CONFIG_PREEMPT_COUNT

/** 20130413
 * preempt count를 증가시켜 preempt 방지.
 * 0이 preemptable.
 * barrier를 preempt_count 변경 뒤에 둠.
 *   -> preemption이 불가능해진 뒤에 작업이 이뤄져야 하므로.
 */
#define preempt_disable() \
do { \
	inc_preempt_count(); \
	barrier(); \
} while (0)

/** 20130713    
 * preempt count를 감소.
 * barrier를 preempt_count 변경 전에 둠.
 *   -> 현재 작업 내용이 메모리에 반영된 뒤에 preemption이 가능해야 하므로.
 **/
#define sched_preempt_enable_no_resched() \
do { \
	barrier(); \
	dec_preempt_count(); \
} while (0)

#define preempt_enable_no_resched()	sched_preempt_enable_no_resched()

#define preempt_enable() \
do { \
	preempt_enable_no_resched(); \
	barrier(); \
	preempt_check_resched(); \
} while (0)

/* For debugging and tracer internals only! */
#define add_preempt_count_notrace(val)			\
	do { preempt_count() += (val); } while (0)
#define sub_preempt_count_notrace(val)			\
	do { preempt_count() -= (val); } while (0)
#define inc_preempt_count_notrace() add_preempt_count_notrace(1)
#define dec_preempt_count_notrace() sub_preempt_count_notrace(1)

#define preempt_disable_notrace() \
do { \
	inc_preempt_count_notrace(); \
	barrier(); \
} while (0)

#define preempt_enable_no_resched_notrace() \
do { \
	barrier(); \
	dec_preempt_count_notrace(); \
} while (0)

/* preempt_check_resched is OK to trace */
#define preempt_enable_notrace() \
do { \
	preempt_enable_no_resched_notrace(); \
	barrier(); \
	preempt_check_resched(); \
} while (0)

#else /* !CONFIG_PREEMPT_COUNT */

/** 20130706    
 * CONFIG_PREEMPT_COUNT가 정의되어 있지 않아 관련 함수들은 NULL.
 **/
#define preempt_disable()		do { } while (0)
#define sched_preempt_enable_no_resched()	do { } while (0)
#define preempt_enable_no_resched()	do { } while (0)
#define preempt_enable()		do { } while (0)

#define preempt_disable_notrace()		do { } while (0)
#define preempt_enable_no_resched_notrace()	do { } while (0)
#define preempt_enable_notrace()		do { } while (0)

#endif /* CONFIG_PREEMPT_COUNT */

#ifdef CONFIG_PREEMPT_NOTIFIERS

struct preempt_notifier;

/**
 * preempt_ops - notifiers called when a task is preempted and rescheduled
 * @sched_in: we're about to be rescheduled:
 *    notifier: struct preempt_notifier for the task being scheduled
 *    cpu:  cpu we're scheduled on
 * @sched_out: we've just been preempted
 *    notifier: struct preempt_notifier for the task being preempted
 *    next: the task that's kicking us out
 *
 * Please note that sched_in and out are called under different
 * contexts.  sched_out is called with rq lock held and irq disabled
 * while sched_in is called without rq lock and irq enabled.  This
 * difference is intentional and depended upon by its users.
 */
struct preempt_ops {
	void (*sched_in)(struct preempt_notifier *notifier, int cpu);
	void (*sched_out)(struct preempt_notifier *notifier,
			  struct task_struct *next);
};

/**
 * preempt_notifier - key for installing preemption notifiers
 * @link: internal use
 * @ops: defines the notifier functions to be called
 *
 * Usually used in conjunction with container_of().
 */
struct preempt_notifier {
	struct hlist_node link;
	struct preempt_ops *ops;
};

void preempt_notifier_register(struct preempt_notifier *notifier);
void preempt_notifier_unregister(struct preempt_notifier *notifier);

static inline void preempt_notifier_init(struct preempt_notifier *notifier,
				     struct preempt_ops *ops)
{
	INIT_HLIST_NODE(&notifier->link);
	notifier->ops = ops;
}

#endif

#endif /* __LINUX_PREEMPT_H */
