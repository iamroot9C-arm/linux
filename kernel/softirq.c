/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 *	Distribute under GPLv2.
 *
 *	Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 *
 *	Remote softirq infrastructure is by Jens Axboe.
 */

#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/ftrace.h>
#include <linux/smp.h>
#include <linux/tick.h>

#define CREATE_TRACE_POINTS
#include <trace/events/irq.h>

#include <asm/irq.h>
/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
/** 20140920    
 * ARM은 __ARCH_IRQ_STAT이 지정되지 않아 전역변수 irq_stat으로 irq의 상태를 관리한다.
 * 빠른 성능을 위해 cacheline으로 정렬시킨다.
 **/
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;
EXPORT_SYMBOL(irq_stat);
#endif

/** 20140426    
 * 미리 지정된 SOFTIRQ만큼의 action을 저장하는 vector (일종의 table)
 * 저장되는 section은 __cacheline_aligned. align은 cacheline 크기 (1<<6)
 **/
static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;

/** 20140726    
 * percpu로 ksoftirqd 용 task를 가리키는 포인터를 정의한다.
 **/
DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

char *softirq_to_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "BLOCK_IOPOLL",
	"TASKLET", "SCHED", "HRTIMER", "RCU"
};

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
/** 20140726    
 * 현재 cpu의 ksoftirqd가 동작 중이지 않다면 깨운다.
 **/
static void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __this_cpu_read(ksoftirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

/*
 * preempt_count and SOFTIRQ_OFFSET usage:
 * - preempt_count is changed by SOFTIRQ_OFFSET on entering or leaving
 *   softirq processing.
 * - preempt_count is changed by SOFTIRQ_DISABLE_OFFSET (= 2 * SOFTIRQ_OFFSET)
 *   on local_bh_disable or local_bh_enable.
 * This lets us distinguish between whether we are currently processing
 * softirq and whether we just have bh disabled.
 */
/** 20140927    
 * hardirq.h에 preempt_count를 나누는 비트 설정이 정의되어 있다.
 **/

/*
 * This one is for softirq.c-internal use,
 * where hardirqs are disabled legitimately:
 */
#ifdef CONFIG_TRACE_IRQFLAGS
static void __local_bh_disable(unsigned long ip, unsigned int cnt)
{
	unsigned long flags;

	WARN_ON_ONCE(in_irq());

	raw_local_irq_save(flags);
	/*
	 * The preempt tracer hooks into add_preempt_count and will break
	 * lockdep because it calls back into lockdep after SOFTIRQ_OFFSET
	 * is set and before current->softirq_enabled is cleared.
	 * We must manually increment preempt_count here and manually
	 * call the trace_preempt_off later.
	 */
	preempt_count() += cnt;
	/*
	 * Were softirqs turned off above:
	 */
	if (softirq_count() == cnt)
		trace_softirqs_off(ip);
	raw_local_irq_restore(flags);

	if (preempt_count() == cnt)
		trace_preempt_off(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
}
#else /* !CONFIG_TRACE_IRQFLAGS */
/** 20140622    
 * 선점 count를 이용한 bottom half disable.
 *
 * local_bh_disable에서는 SOFTIRQ_DISABLE_OFFSET
 * __do_softirq에서는 SOFTIRQ_OFFSET를 전달.
 **/
static inline void __local_bh_disable(unsigned long ip, unsigned int cnt)
{
	/** 20140622    
	 * preempt_count에 cnt를 증가시키고 compiler barrier를 둔다.
	 * (cnt: SOFTIRQ_OFFSET, SOFTIRQ_DISABLE_OFFSET)
	 **/
	add_preempt_count(cnt);
	barrier();
}
#endif /* CONFIG_TRACE_IRQFLAGS */

/** 20140622    
 * 현재 cpu의 bottom half를 막는다.
 **/
void local_bh_disable(void)
{
	__local_bh_disable((unsigned long)__builtin_return_address(0),
				SOFTIRQ_DISABLE_OFFSET);
}

EXPORT_SYMBOL(local_bh_disable);

/** 20140927    
 * preempt_count에서 cnt를 감소시킨다.
 * cnt가 SOFTIRQ_OFFSET으로 온 경우, SOFTIRQ가 끝났음을 표시한다.
 **/
static void __local_bh_enable(unsigned int cnt)
{
	WARN_ON_ONCE(in_irq());
	WARN_ON_ONCE(!irqs_disabled());

	if (softirq_count() == cnt)
		trace_softirqs_on((unsigned long)__builtin_return_address(0));
	sub_preempt_count(cnt);
}

/*
 * Special-case - softirqs can safely be enabled in
 * cond_resched_softirq(), or by __do_softirq(),
 * without processing still-pending softirqs:
 */
void _local_bh_enable(void)
{
	__local_bh_enable(SOFTIRQ_DISABLE_OFFSET);
}

EXPORT_SYMBOL(_local_bh_enable);

static inline void _local_bh_enable_ip(unsigned long ip)
{
	WARN_ON_ONCE(in_irq() || irqs_disabled());
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	if (softirq_count() == SOFTIRQ_DISABLE_OFFSET)
		trace_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
 	 */
	sub_preempt_count(SOFTIRQ_DISABLE_OFFSET - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending()))
		do_softirq();

	dec_preempt_count();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}

void local_bh_enable(void)
{
	_local_bh_enable_ip((unsigned long)__builtin_return_address(0));
}
EXPORT_SYMBOL(local_bh_enable);

void local_bh_enable_ip(unsigned long ip)
{
	_local_bh_enable_ip(ip);
}
EXPORT_SYMBOL(local_bh_enable_ip);

/*
 * We restart softirq processing MAX_SOFTIRQ_RESTART times,
 * and we fall back to softirqd after that.
 *
 * This number has been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
/** 20140927    
 * softirq의 action을 호출하고, 다시 검사해 softirq가 pending되어 있다면
 * restart하는데, 이 수치를 넘는다면 softirqd로 실행한다.
 **/
#define MAX_SOFTIRQ_RESTART 10

/** 20140927    
 * softirq가 pending되어 있는지 검사해 LSB부터 action을 수행한다.
 * action은 SOFTIRQ interrupt context에서 local interrupt가 활성화된 상태로 실행된다.
 **/
asmlinkage void __do_softirq(void)
{
	struct softirq_action *h;
	__u32 pending;
	int max_restart = MAX_SOFTIRQ_RESTART;
	int cpu;
	unsigned long old_flags = current->flags;

	/*
	 * Mask out PF_MEMALLOC s current task context is borrowed for the
	 * softirq. A softirq handled such as network RX might set PF_MEMALLOC
	 * again if the socket is related to swap
	 */
	/** 20140927    
	 * flags에서 PF_MEMALLOC을 지워 별도의 memory allocating을 하지 않고,
	 * softirq를 위한 메모리에서 빌려오도록 한다.
	 **/
	current->flags &= ~PF_MEMALLOC;

	/** 20140927    
	 * 현재 cpu의 softirq의 pending 상태를 가져온다.
	 **/
	pending = local_softirq_pending();
	/** 20140927    
	 * 추후분석 ???
	 **/
	account_system_vtime(current);

	/** 20140927    
	 * __local_bh_disable을 이용해 preempt_count에 SOFTIRQ_OFFSET를 기록한다.
	 * SOFTIRQ 진행 중임을 표시한다. 즉, interrupt context에서 이후를 실행한다.
	 **/
	__local_bh_disable((unsigned long)__builtin_return_address(0),
				SOFTIRQ_OFFSET);
	lockdep_softirq_enter();

	/** 20140927    
	 * 현재 cpu의 번호를 가져온다.
	 **/
	cpu = smp_processor_id();
restart:
	/* Reset the pending bitmask before enabling irqs */
	/** 20140927    
	 * irq 할성화 전에 pending bitmask를 0으로 설정한다.
	 *
	 * 인터럽트가 발생한다면 기존의 pending 된 softirq에 의해 irq_exit에서
	 * 다시 softirq handling을 시도할 것이다.
	 **/
	set_softirq_pending(0);

	local_irq_enable();

	h = softirq_vec;

	/** 20140927    
	 * 즉, softirq action은 SOFTIRQ interrupt context에서, interrupt를 활성화한 상태로 실행한다.
	 * softirq action 은 우선 순위에 따라 LSB에서부터 실행한다.
	 **/
	do {
		if (pending & 1) {
			unsigned int vec_nr = h - softirq_vec;
			int prev_count = preempt_count();

			/** 20140927    
			 * kstat 값 증가.
			 **/
			kstat_incr_softirqs_this_cpu(vec_nr);

			trace_softirq_entry(vec_nr);
			/** 20140927    
			 * softirq의 action호출. softirq_action이 argument로 넘어간다.
			 **/
			h->action(h);
			trace_softirq_exit(vec_nr);
			if (unlikely(prev_count != preempt_count())) {
				printk(KERN_ERR "huh, entered softirq %u %s %p"
				       "with preempt_count %08x,"
				       " exited with %08x?\n", vec_nr,
				       softirq_to_name[vec_nr], h->action,
				       prev_count, preempt_count());
				preempt_count() = prev_count;
			}

			/** 20140927    
			 * softirq action 호출 이후에, rcu_bh에 대한 QS가 되었음을 기록한다.
			 * interrupt enable 상태이므로 각 softirq 하나를 처리할 때마다 QS를 기록한다.
			 *
			 * quiescent states for rcu_bh are any code outside of softirq with interrupts enabled.
			 * from http://lwn.net/Articles/305782/
			 **/
			rcu_bh_qs(cpu);
		}
		/** 20140927    
		 * 다음 softirq 항목으로 이동.
		 **/
		h++;
		pending >>= 1;
	} while (pending);

	local_irq_disable();

	/** 20140927    
	 * 다시 softirq pending 값을 가져온다.
	 * 그 사이에 softirq가 들어왔다면 max_restart까지만 실행한다.
	 **/
	pending = local_softirq_pending();
	if (pending && --max_restart)
		goto restart;

	/** 20140927    
	 * max_restart만큼 실행한 뒤에 pending되어 있다면 softirq daemon을 깨운다.
	 **/
	if (pending)
		wakeup_softirqd();

	lockdep_softirq_exit();

	account_system_vtime(current);
	/** 20140927    
	 * SOFTIRQ_OFFSET
	 **/
	__local_bh_enable(SOFTIRQ_OFFSET);
	/** 20140927    
	 * 현재 task의 PF_MEMALLOC이 설정되어 있었다면 복원한다.
	 **/
	tsk_restore_flags(current, old_flags, PF_MEMALLOC);
}

#ifndef __ARCH_HAS_DO_SOFTIRQ

asmlinkage void do_softirq(void)
{
	__u32 pending;
	unsigned long flags;

	if (in_interrupt())
		return;

	local_irq_save(flags);

	pending = local_softirq_pending();

	if (pending)
		__do_softirq();

	local_irq_restore(flags);
}

#endif

/*
 * Enter an interrupt context.
 */
/** 20140622    
 * 추후 분석
 **/
void irq_enter(void)
{
	/** 20140621    
	 * 현재 processor의 id를 가져온다.
	 **/
	int cpu = smp_processor_id();

	/** 20141004    
	 * RCU에 irq 진입을 알린다.
	 **/
	rcu_irq_enter();
	/** 20140621    
	 * 현재 task가 idle task이고, interrupt context가 아닐 때
	 **/
	if (is_idle_task(current) && !in_interrupt()) {
		/*
		 * Prevent raise_softirq from needlessly waking up ksoftirqd
		 * here, as softirq will be serviced on return from interrupt.
		 */
		local_bh_disable();
		tick_check_idle(cpu);
		_local_bh_enable();
	}

	/** 20141004    
	 * HARDIRQ 진행 중임을 preempt_count에 기록한다.
	 **/
	__irq_enter();
}

/** 20140927    
 * softirq를 실행한다.
 **/
static inline void invoke_softirq(void)
{
	if (!force_irqthreads) {
#ifdef __ARCH_IRQ_EXIT_IRQS_DISABLED
		/** 20140927    
		 * architecture가 irq exit에서 irq disabled로 처리하는지 여부에 따라
		 * local_irq_save 여부가 결정된다.
		 *
		 * local_irq_save를 호출하지 않는 __do_softirq로 처리
		 **/
		__do_softirq();
#else
		do_softirq();
#endif
	} else {
		/** 20140927    
		 * softirq를 강제로 irqthread로 처리하도록 지정되어 있다면
		 * 항상 softirqd를 깨워 실행시킨다.
		 *
		 * __local_bh_disable를 이용해 현재 상태를 SOFTIRQ 실행 중임을 표시한다.
		 **/
		__local_bh_disable((unsigned long)__builtin_return_address(0),
				SOFTIRQ_OFFSET);
		wakeup_softirqd();
		__local_bh_enable(SOFTIRQ_OFFSET);
	}
}

/*
 * Exit an interrupt context. Process softirqs if needed and possible:
 */
/** 20140622    
 * 추후 분석
 **/
void irq_exit(void)
{
	account_system_vtime(current);
	trace_hardirq_exit();
	sub_preempt_count(IRQ_EXIT_OFFSET);
	/** 20140927    
	 * interrupt context가 아니고, local cpu에 softirq가 pending 되어 있다면 (__raise_softirq 함수로 pending됨)
	 * softirq를 처리시킨다.
	 **/
	if (!in_interrupt() && local_softirq_pending())
		invoke_softirq();

#ifdef CONFIG_NO_HZ
	/* Make sure that timer wheel updates are propagated */
	if (idle_cpu(smp_processor_id()) && !in_interrupt() && !need_resched())
		tick_nohz_irq_exit();
#endif
	/** 20141004
	 **/
	rcu_irq_exit();
	sched_preempt_enable_no_resched();
}

/*
 * This function must run with irqs disabled!
 */
/** 20140726    
 * irq가 금지된 상태에서 softirq를 발생시킨다.
 * softirq를 pending 시킨 후 interrupt context가 아니라면 ksoftirqd를 깨운다.
 **/
inline void raise_softirq_irqoff(unsigned int nr)
{
	/** 20140726    
	 * nr번 softirq의 발생을 기록한다(pending)
	 **/
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	/** 20140726    
	 * 현재 interrupt context가 아니라면 softirqd를 깨운다.
	 **/
	if (!in_interrupt())
		wakeup_softirqd();
}

/** 20140726    
 * local cpu interrupt를 disable한 상태에서 nr에 해당하는 softirq를 발생시킨다.
 * 실제로는 local_softirq_pending()에서 해당 비트 위치에 pending만을 표시한다.
 **/
void raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

/** 20140726    
 * nr번 softirq의 pending을 추가(or) 기록한다.
 * 즉, 처리가 동기적으로 이뤄지지 않고 pending만을 기록한다.
 **/
void __raise_softirq_irqoff(unsigned int nr)
{
	trace_softirq_raise(nr);
	or_softirq_pending(1UL << nr);
}

/** 20140426    
 * nr SOFTIRQ의 action 을 지정.
 *
 * 등록된 softirq를 발생시킬 때는 raise_softirq를 사용한다.
 **/
void open_softirq(int nr, void (*action)(struct softirq_action *))
{
	softirq_vec[nr].action = action;
}

/*
 * Tasklets
 */
/** 20140920    
 * tasklet_head 구조체.
 * 
 * tasklet_vec                                           --------
 * +-----------+  +-----------+  +-----------+  +-------|---+   |
 * | head (*) -|->| next (*) -|->| next (*) -|->| next (*) -|   |
 * | tail (**) |  | func()    |  | func()    |  | func()    |   |
 * +-----------+  | data      |  | data      |  | data      |   |
 *         |      | ...       |  | ...       |  | ...       |   |
 *         |      +-----------+  +-----------+  +-----------+   |
 *         |      tasklet_struct tasklet_struct tasklet_struct  |
 *         ------------------------------------------------------
 **/
struct tasklet_head
{
	struct tasklet_struct *head;
	struct tasklet_struct **tail;
};

/** 20140920    
 * per-cpu로 tasklet_vec, tasklet_hi_vec라는 tasklet_head를 선언한다.
 **/
static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);

void __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	/** 20141011    
	 * interrupt를 막은채로 tasklet을 tasklet vector의 마지막에 schedule (등록) 한다.
	 **/
	local_irq_save(flags);
	t->next = NULL;
	*__this_cpu_read(tasklet_vec.tail) = t;
	__this_cpu_write(tasklet_vec.tail, &(t->next));
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_schedule);

void __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = NULL;
	*__this_cpu_read(tasklet_hi_vec.tail) = t;
	__this_cpu_write(tasklet_hi_vec.tail,  &(t->next));
	raise_softirq_irqoff(HI_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_hi_schedule);

void __tasklet_hi_schedule_first(struct tasklet_struct *t)
{
	BUG_ON(!irqs_disabled());

	t->next = __this_cpu_read(tasklet_hi_vec.head);
	__this_cpu_write(tasklet_hi_vec.head, t);
	__raise_softirq_irqoff(HI_SOFTIRQ);
}

EXPORT_SYMBOL(__tasklet_hi_schedule_first);

static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __this_cpu_read(tasklet_vec.head);
	__this_cpu_write(tasklet_vec.head, NULL);
	__this_cpu_write(tasklet_vec.tail, &__get_cpu_var(tasklet_vec).head);
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = NULL;
		*__this_cpu_read(tasklet_vec.tail) = t;
		__this_cpu_write(tasklet_vec.tail, &(t->next));
		__raise_softirq_irqoff(TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}

static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __this_cpu_read(tasklet_hi_vec.head);
	__this_cpu_write(tasklet_hi_vec.head, NULL);
	__this_cpu_write(tasklet_hi_vec.tail, &__get_cpu_var(tasklet_hi_vec).head);
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = NULL;
		*__this_cpu_read(tasklet_hi_vec.tail) = t;
		__this_cpu_write(tasklet_hi_vec.tail, &(t->next));
		__raise_softirq_irqoff(HI_SOFTIRQ);
		local_irq_enable();
	}
}


/** 20141011    
 * tasklet_struct를 받아 tasklet을 초기화 한다.
 **/
void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

EXPORT_SYMBOL(tasklet_init);

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do {
			yield();
		} while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

EXPORT_SYMBOL(tasklet_kill);

/*
 * tasklet_hrtimer
 */

/*
 * The trampoline is called when the hrtimer expires. It schedules a tasklet
 * to run __tasklet_hrtimer_trampoline() which in turn will call the intended
 * hrtimer callback, but from softirq context.
 */
static enum hrtimer_restart __hrtimer_tasklet_trampoline(struct hrtimer *timer)
{
	struct tasklet_hrtimer *ttimer =
		container_of(timer, struct tasklet_hrtimer, timer);

	tasklet_hi_schedule(&ttimer->tasklet);
	return HRTIMER_NORESTART;
}

/*
 * Helper function which calls the hrtimer callback from
 * tasklet/softirq context
 */
static void __tasklet_hrtimer_trampoline(unsigned long data)
{
	struct tasklet_hrtimer *ttimer = (void *)data;
	enum hrtimer_restart restart;

	restart = ttimer->function(&ttimer->timer);
	if (restart != HRTIMER_NORESTART)
		hrtimer_restart(&ttimer->timer);
}

/**
 * tasklet_hrtimer_init - Init a tasklet/hrtimer combo for softirq callbacks
 * @ttimer:	 tasklet_hrtimer which is initialized
 * @function:	 hrtimer callback function which gets called from softirq context
 * @which_clock: clock id (CLOCK_MONOTONIC/CLOCK_REALTIME)
 * @mode:	 hrtimer mode (HRTIMER_MODE_ABS/HRTIMER_MODE_REL)
 */
void tasklet_hrtimer_init(struct tasklet_hrtimer *ttimer,
			  enum hrtimer_restart (*function)(struct hrtimer *),
			  clockid_t which_clock, enum hrtimer_mode mode)
{
	hrtimer_init(&ttimer->timer, which_clock, mode);
	ttimer->timer.function = __hrtimer_tasklet_trampoline;
	tasklet_init(&ttimer->tasklet, __tasklet_hrtimer_trampoline,
		     (unsigned long)ttimer);
	ttimer->function = function;
}
EXPORT_SYMBOL_GPL(tasklet_hrtimer_init);

/*
 * Remote softirq bits
 */

/** 20140920    
 * per-cpu 변수 softirq_work_list를 정의한다.
 * 각 cpu마다 NR_SOFTIRQS의 개수만큼 list_head를 보유한다.
 **/
DEFINE_PER_CPU(struct list_head [NR_SOFTIRQS], softirq_work_list);
EXPORT_PER_CPU_SYMBOL(softirq_work_list);

static void __local_trigger(struct call_single_data *cp, int softirq)
{
	struct list_head *head = &__get_cpu_var(softirq_work_list[softirq]);

	list_add_tail(&cp->list, head);

	/* Trigger the softirq only if the list was previously empty.  */
	if (head->next == &cp->list)
		raise_softirq_irqoff(softirq);
}

#ifdef CONFIG_USE_GENERIC_SMP_HELPERS
static void remote_softirq_receive(void *data)
{
	struct call_single_data *cp = data;
	unsigned long flags;
	int softirq;

	softirq = cp->priv;

	local_irq_save(flags);
	__local_trigger(cp, softirq);
	local_irq_restore(flags);
}

static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	if (cpu_online(cpu)) {
		cp->func = remote_softirq_receive;
		cp->info = cp;
		cp->flags = 0;
		cp->priv = softirq;

		__smp_call_function_single(cpu, cp, 0);
		return 0;
	}
	return 1;
}
#else /* CONFIG_USE_GENERIC_SMP_HELPERS */
static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	return 1;
}
#endif

/**
 * __send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @this_cpu: the currently executing cpu
 * @softirq: the softirq for the work
 *
 * Attempt to schedule softirq work on a remote cpu.  If this cannot be
 * done, the work is instead queued up on the local cpu.
 *
 * Interrupts must be disabled.
 */
void __send_remote_softirq(struct call_single_data *cp, int cpu, int this_cpu, int softirq)
{
	if (cpu == this_cpu || __try_remote_softirq(cp, cpu, softirq))
		__local_trigger(cp, softirq);
}
EXPORT_SYMBOL(__send_remote_softirq);

/**
 * send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @softirq: the softirq for the work
 *
 * Like __send_remote_softirq except that disabling interrupts and
 * computing the current cpu is done for the caller.
 */
void send_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	unsigned long flags;
	int this_cpu;

	local_irq_save(flags);
	this_cpu = smp_processor_id();
	__send_remote_softirq(cp, cpu, this_cpu, softirq);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(send_remote_softirq);

/** 20140927
 * softirq 관련 cpu notify.
 **/
static int __cpuinit remote_softirq_cpu_notify(struct notifier_block *self,
					       unsigned long action, void *hcpu)
{
	/*
	 * If a CPU goes away, splice its entries to the current CPU
	 * and trigger a run of the softirq
	 */
	/** 20140927    
	 * 대상 cpu가 죽을 때, 그 cpu의 softirq_work_list를 가져와 현재 CPU에 등록하고,
	 * softirq를 발생시켜 처리한다.
	 **/
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		int cpu = (unsigned long) hcpu;
		int i;

		/** 20140920    
		 * local cpu의 irq를 금지시킨 상태에서 수행한다.
		 **/
		local_irq_disable();
		/** 20140920    
		 * 각각의 softirq에 대해 아래 동작을 수행한다.
		 **/
		for (i = 0; i < NR_SOFTIRQS; i++) {
			/** 20140920    
			 * cpu에 해당하는 softirq_work_list를 가져와서
			 **/
			struct list_head *head = &per_cpu(softirq_work_list[i], cpu);
			struct list_head *local_head;

			/** 20140920    
			 * softirq_work_list가 비어 있다면 다음 SOFTIRQ로 넘어간다.
			 **/
			if (list_empty(head))
				continue;

			/** 20140920    
			 * 현재 cpu의 softirq_work_list에 CPU_DEAD되는 cpu의 list(head)를 붙여넣고,
			 * head의 리스트를 비운다.
			 **/
			local_head = &__get_cpu_var(softirq_work_list[i]);
			list_splice_init(head, local_head);
			/** 20140927    
			 * softirq를 발생시킨다. interrupt disable 상태에서 호출하는 버전 호출.
			 **/
			raise_softirq_irqoff(i);
		}
		local_irq_enable();
	}

	return NOTIFY_OK;
}

/** 20140920    
 * remote_softirq_cpu_notifier NB 정의.
 *
 * 등록된 notifier_block은 cpu_notify, cpu_notify_nofail로 호출한다.
 **/
static struct notifier_block __cpuinitdata remote_softirq_cpu_notifier = {
	.notifier_call	= remote_softirq_cpu_notify,
};


/** 20141213
 * 각 cpu에 대해 softirq 자료구조를 초기화 한다.
 *
 * TASKLET_SOFTIRQ와 HI_SOFTIRQ에 대한 action을 지정한다.
 **/
void __init softirq_init(void)
{
	int cpu;

	/** 20140920    
	 * softirq는 per-cpu 기반으로 동작시킨다.
	 * possible cpu mask의 각 cpu에 대해 다음 동작을 수행한다.
	 **/
	for_each_possible_cpu(cpu) {
		int i;

		/** 20140920    
		 * tasklet_vec, tasklet_hi_vec 구조체를 초기화 한다.
		 * struct tasklet_head 위의 ascii 참고.
		 **/
		per_cpu(tasklet_vec, cpu).tail =
			&per_cpu(tasklet_vec, cpu).head;
		per_cpu(tasklet_hi_vec, cpu).tail =
			&per_cpu(tasklet_hi_vec, cpu).head;
		/** 20140920    
		 * SOFTIRQS 각각의 softirq_work_list를 초기화 한다.
		 **/
		for (i = 0; i < NR_SOFTIRQS; i++)
			INIT_LIST_HEAD(&per_cpu(softirq_work_list[i], cpu));
	}

	/** 20140920    
	 * hotcpu notifier chain에 remote_softirq_cpu_notifier를 등록시킨다.
	 **/
	register_hotcpu_notifier(&remote_softirq_cpu_notifier);

	/** 20140927    
	 * TASKLET_SOFTIRQ와 HI_SOFTIRQ에 action을 지정한다.
	 **/
	open_softirq(TASKLET_SOFTIRQ, tasklet_action);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}

/** 20140927    
 * ksoftirqd의 동작.
 *
 * local_softirq_pending 되어 있다면 __do_softirq로 pending된 softirq를 실행하고,
 * 그렇지 않다면 외부에서 wakeup시킬 수 있도록 TASK_INTERRUPTIBLE로 실행된다.
 **/
static int run_ksoftirqd(void * __bind_cpu)
{
	set_current_state(TASK_INTERRUPTIBLE);

	while (!kthread_should_stop()) {
		/** 20140920    
		 * 선점 불가 상태로 만들어 scheduling이 발생하지 않도록 한다.
		 **/
		preempt_disable();
		/** 20140920    
		 * softirq가 pending되어 있지 않다면 schedule_preempt_disabled를 호출한다.
		 * 즉, 특별한 동작을 하지 않고 schedule out 한다.
		 *
		 * 이후 softirq가 raise 되어야 할 때 wakeup_softirqd에 의해 깨어난다.
		 * 깨어난 이후에는 다시 선점불가 상태가 된다.
		 **/
		if (!local_softirq_pending()) {
			schedule_preempt_disabled();
		}

		/** 20140920    
		 * task의 상태를 TASK_RUNNING으로 만든다.
		 **/
		__set_current_state(TASK_RUNNING);

		/** 20140920    
		 * local softirq(현재 cpu)가 pending 되어 있는동안 irq를 막은 상태에서
		 * __do_softirq로 pending된 softirq를 처리한다.
		 *
		 **/
		while (local_softirq_pending()) {
			/* Preempt disable stops cpu going offline.
			   If already offline, we'll be on wrong CPU:
			   don't process */
			/** 20140927    
			 * __bind_cpu가 offline이라면 wait_to_die로 이동.
			 **/
			if (cpu_is_offline((long)__bind_cpu))
				goto wait_to_die;
			/** 20140927    
			 * __do_softirq 내에서 softirq action을 수행하기 전후에
			 * local_irq_enable(), local_irq_disable을 수행한다.
			 **/
			local_irq_disable();
			if (local_softirq_pending())
				__do_softirq();
			local_irq_enable();
			sched_preempt_enable_no_resched();
			/** 20140920    
			 * rescheduling point를 둔다.
			 **/
			cond_resched();
			preempt_disable();
			/** 20140830    
			 * 현재 ksoftirqd가 수행되는 __bind_cpu에 대해 context switch를 note 한다.
			 * ddd로 수행시 호출되지 않는데, 왜???
			 **/
			rcu_note_context_switch((long)__bind_cpu);
		}
		preempt_enable();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	/** 20140927    
	 * kthread_stop()을 호출할 쪽에서 처리할 수 있도록 TASK_RUNNING으로 변경시키고
	 * 0을 리턴한다.
	 **/
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	/** 20140927    
	 * 수행될 cpu가 offline이므로 kthread_stop()이 호출될 때까지
	 * TASK_INTERRUPTIBLE로 schdule된다.
	 **/
	preempt_enable();
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	BUG_ON(cpu_online(cpu));
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	for (i = &per_cpu(tasklet_vec, cpu).head; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			/* If this was the tail element, move the tail ptr */
			if (*i == NULL)
				per_cpu(tasklet_vec, cpu).tail = i;
			return;
		}
	}
	BUG();
}

static void takeover_tasklets(unsigned int cpu)
{
	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	if (&per_cpu(tasklet_vec, cpu).head != per_cpu(tasklet_vec, cpu).tail) {
		*__this_cpu_read(tasklet_vec.tail) = per_cpu(tasklet_vec, cpu).head;
		this_cpu_write(tasklet_vec.tail, per_cpu(tasklet_vec, cpu).tail);
		per_cpu(tasklet_vec, cpu).head = NULL;
		per_cpu(tasklet_vec, cpu).tail = &per_cpu(tasklet_vec, cpu).head;
	}
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	if (&per_cpu(tasklet_hi_vec, cpu).head != per_cpu(tasklet_hi_vec, cpu).tail) {
		*__this_cpu_read(tasklet_hi_vec.tail) = per_cpu(tasklet_hi_vec, cpu).head;
		__this_cpu_write(tasklet_hi_vec.tail, per_cpu(tasklet_hi_vec, cpu).tail);
		per_cpu(tasklet_hi_vec, cpu).head = NULL;
		per_cpu(tasklet_hi_vec, cpu).tail = &per_cpu(tasklet_hi_vec, cpu).head;
	}
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __cpuinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct task_struct *p;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		/** 20140927    
		 * CPU_UP_PREPARE, CPU_UP_PREPARE_FROZEN에
		 * cpu마다 존재하는 ksoftirqd를 생성한다.
		 **/
		p = kthread_create_on_node(run_ksoftirqd,
					   hcpu,
					   cpu_to_node(hotcpu),
					   "ksoftirqd/%d", hotcpu);
		if (IS_ERR(p)) {
			printk("ksoftirqd for %i failed\n", hotcpu);
			return notifier_from_errno(PTR_ERR(p));
		}
		/** 20140927    
		 * hotcpu에서만 수행하도록 설정한다.
		 **/
		kthread_bind(p, hotcpu);
  		per_cpu(ksoftirqd, hotcpu) = p;
 		break;
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		/** 20140927    
		 * CPU_ONLINE, CPU_ONLINE_FROZEN에 ksoftirqd를 실행시킨다.
		 **/
		wake_up_process(per_cpu(ksoftirqd, hotcpu));
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		if (!per_cpu(ksoftirqd, hotcpu))
			break;
		/* Unbind so it can run.  Fall thru. */
		kthread_bind(per_cpu(ksoftirqd, hotcpu),
			     cpumask_any(cpu_online_mask));
	case CPU_DEAD:
	case CPU_DEAD_FROZEN: {
		static const struct sched_param param = {
			.sched_priority = MAX_RT_PRIO-1
		};

		p = per_cpu(ksoftirqd, hotcpu);
		per_cpu(ksoftirqd, hotcpu) = NULL;
		sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
		kthread_stop(p);
		takeover_tasklets(hotcpu);
		break;
	}
#endif /* CONFIG_HOTPLUG_CPU */
 	}
	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata cpu_nfb = {
	.notifier_call = cpu_callback
};

static __init int spawn_ksoftirqd(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	int err = cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);

	BUG_ON(err != NOTIFY_OK);
	cpu_callback(&cpu_nfb, CPU_ONLINE, cpu);
	register_cpu_notifier(&cpu_nfb);
	return 0;
}
early_initcall(spawn_ksoftirqd);

/*
 * [ These __weak aliases are kept in a separate compilation unit, so that
 *   GCC does not inline them incorrectly. ]
 */

int __init __weak early_irq_init(void)
{
	return 0;
}

#ifdef CONFIG_GENERIC_HARDIRQS
int __init __weak arch_probe_nr_irqs(void)
{
	return NR_IRQS_LEGACY;
}

int __init __weak arch_early_irq_init(void)
{
	return 0;
}
#endif
