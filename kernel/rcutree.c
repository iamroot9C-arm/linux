/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright IBM Corporation, 2008
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 *	    Paul E. McKenney <paulmck@linux.vnet.ibm.com> Hierarchical version
 *
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *	Documentation/RCU
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/rcupdate.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/nmi.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/kernel_stat.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/prefetch.h>
#include <linux/delay.h>
#include <linux/stop_machine.h>

#include "rcutree.h"
#include <trace/events/rcu.h>

#include "rcu.h"

/* Data structures. */

static struct lock_class_key rcu_node_class[RCU_NUM_LVLS];

/** 20140823    
 * rcu_state 초기값.
 **/
#define RCU_STATE_INITIALIZER(sname, cr) { \
	.level = { &sname##_state.node[0] }, \
	.call = cr, \
	.fqs_state = RCU_GP_IDLE, \
	.gpnum = -300, \
	.completed = -300, \
	.onofflock = __RAW_SPIN_LOCK_UNLOCKED(&sname##_state.onofflock), \
	.orphan_nxttail = &sname##_state.orphan_nxtlist, \
	.orphan_donetail = &sname##_state.orphan_donelist, \
	.barrier_mutex = __MUTEX_INITIALIZER(sname##_state.barrier_mutex), \
	.fqslock = __RAW_SPIN_LOCK_UNLOCKED(&sname##_state.fqslock), \
	.name = #sname, \
}

/** 20140726    
 * rcu 전역 상태인 rcu_sched_state, rcu_bh_state 선언 및 초기화.
 * 각 state가 사용할 percpu data를 선언. (rcu_init_one에서 연결)
 *
 * call_rcu(block되지 않고 read-side critical section이 모두 완료되었을 때 호출)
 *  callback을 call_rcu_sched, call_rcu_bh로 지정.
 *
 * rsp->rda에 해당하는 struct rcu_data.
 **/
struct rcu_state rcu_sched_state =
	RCU_STATE_INITIALIZER(rcu_sched, call_rcu_sched);
DEFINE_PER_CPU(struct rcu_data, rcu_sched_data);

struct rcu_state rcu_bh_state = RCU_STATE_INITIALIZER(rcu_bh, call_rcu_bh);
DEFINE_PER_CPU(struct rcu_data, rcu_bh_data);

static struct rcu_state *rcu_state;
/** 20140726    
 * 초기화된 rcu_state들의 list.
 **/
LIST_HEAD(rcu_struct_flavors);

/* Increase (but not decrease) the CONFIG_RCU_FANOUT_LEAF at boot time. */
static int rcu_fanout_leaf = CONFIG_RCU_FANOUT_LEAF;
module_param(rcu_fanout_leaf, int, 0);
/** 20140726    
 * rcu level의 수는 config에 따라 1이 됨.
 * num_rcu_lvl는 각 레벨의 rcu_nodes의 수로 1, 4, 0, 0, 0
 **/
int rcu_num_lvls __read_mostly = RCU_NUM_LVLS;
static int num_rcu_lvl[] = {  /* Number of rcu_nodes at specified level. */
	NUM_RCU_LVL_0,
	NUM_RCU_LVL_1,
	NUM_RCU_LVL_2,
	NUM_RCU_LVL_3,
	NUM_RCU_LVL_4,
};
int rcu_num_nodes __read_mostly = NUM_RCU_NODES; /* Total # rcu_nodes in use. */

/*
 * The rcu_scheduler_active variable transitions from zero to one just
 * before the first task is spawned.  So when this variable is zero, RCU
 * can assume that there is but one task, allowing RCU to (for example)
 * optimized synchronize_sched() to a simple barrier().  When this variable
 * is one, RCU must actually do all the hard work required to detect real
 * grace periods.  This variable is also used to suppress boot-time false
 * positives from lockdep-RCU error checking.
 */
int rcu_scheduler_active __read_mostly;
EXPORT_SYMBOL_GPL(rcu_scheduler_active);

/*
 * The rcu_scheduler_fully_active variable transitions from zero to one
 * during the early_initcall() processing, which is after the scheduler
 * is capable of creating new tasks.  So RCU processing (for example,
 * creating tasks for RCU priority boosting) must be delayed until after
 * rcu_scheduler_fully_active transitions from zero to one.  We also
 * currently delay invocation of any RCU callbacks until after this point.
 *
 * It might later prove better for people registering RCU callbacks during
 * early boot to take responsibility for these callbacks, but one step at
 * a time.
 */
static int rcu_scheduler_fully_active __read_mostly;

#ifdef CONFIG_RCU_BOOST

/*
 * Control variables for per-CPU and per-rcu_node kthreads.  These
 * handle all flavors of RCU.
 */
/** 20141018    
 * rcu CBs을 처리할 per_cpu task 변수를 정의.
 * rcu_spawn_kthreads에서 task를 지정한다.
 **/
static DEFINE_PER_CPU(struct task_struct *, rcu_cpu_kthread_task);
DEFINE_PER_CPU(unsigned int, rcu_cpu_kthread_status);
DEFINE_PER_CPU(int, rcu_cpu_kthread_cpu);
DEFINE_PER_CPU(unsigned int, rcu_cpu_kthread_loops);
DEFINE_PER_CPU(char, rcu_cpu_has_work);

#endif /* #ifdef CONFIG_RCU_BOOST */

static void rcu_node_kthread_setaffinity(struct rcu_node *rnp, int outgoingcpu);
static void invoke_rcu_core(void);
static void invoke_rcu_callbacks(struct rcu_state *rsp, struct rcu_data *rdp);

/*
 * Track the rcutorture test sequence number and the update version
 * number within a given test.  The rcutorture_testseq is incremented
 * on every rcutorture module load and unload, so has an odd value
 * when a test is running.  The rcutorture_vernum is set to zero
 * when rcutorture starts and is incremented on each rcutorture update.
 * These variables enable correlating rcutorture output with the
 * RCU tracing information.
 */
unsigned long rcutorture_testseq;
unsigned long rcutorture_vernum;

/*
 * Return true if an RCU grace period is in progress.  The ACCESS_ONCE()s
 * permit this function to be invoked without holding the root rcu_node
 * structure's ->lock, but of course results can be subject to change.
 */
/** 20140809    
 * 현재 gpnum이 완료된 gpnum이 아니라면 grace period가 진행 중이다.
 **/
static int rcu_gp_in_progress(struct rcu_state *rsp)
{
	return ACCESS_ONCE(rsp->completed) != ACCESS_ONCE(rsp->gpnum);
}

/*
 * Note a quiescent state.  Because we do not need to know
 * how many quiescent states passed, just if there was at least
 * one since the start of the grace period, this just sets a flag.
 * The caller must have disabled preemption.
 */
/** 20140823    
 * scheduler 동작에 의한 qs state를 기록한다.
 *
 * gp 시작 이후로 qs가 한 번이라도 발생했는지 정보만 알면 되므로,
 * 호출될 때마다 현재 gpnum과 passed_quiesce 상태값을 업데이트 한다.
 **/
void rcu_sched_qs(int cpu)
{
	struct rcu_data *rdp = &per_cpu(rcu_sched_data, cpu);

	rdp->passed_quiesce_gpnum = rdp->gpnum;
	barrier();
	if (rdp->passed_quiesce == 0)
		trace_rcu_grace_period("rcu_sched", rdp->gpnum, "cpuqs");
	rdp->passed_quiesce = 1;
}

/** 20140906    
 * bh 동작에 의한 qs를 기록한다.
 * rch_sched와 별도로 기록해야 하는 이유에 대한 추가적인 이해가 필요하다???
 **/
void rcu_bh_qs(int cpu)
{
	struct rcu_data *rdp = &per_cpu(rcu_bh_data, cpu);

	rdp->passed_quiesce_gpnum = rdp->gpnum;
	barrier();
	if (rdp->passed_quiesce == 0)
		trace_rcu_grace_period("rcu_bh", rdp->gpnum, "cpuqs");
	rdp->passed_quiesce = 1;
}

/*
 * Note a context switch.  This is a quiescent state for RCU-sched,
 * and requires special handling for preemptible RCU.
 * The caller must have disabled preemption.
 */
/** 20140824    
 * context switch 발생을 기록한다.
 **/
void rcu_note_context_switch(int cpu)
{
	trace_rcu_utilization("Start context switch");
	rcu_sched_qs(cpu);
	rcu_preempt_note_context_switch(cpu);
	trace_rcu_utilization("End context switch");
}
EXPORT_SYMBOL_GPL(rcu_note_context_switch);

/** 20140621    
 * struct rcu_dynticks percpu 변수 선언.
 * dynticks은 최초에 1로 설정.
 *
 * dynticks 홀수: dynticks not idle.
 * dynticks 짝수: dynticks idle.
 **/
DEFINE_PER_CPU(struct rcu_dynticks, rcu_dynticks) = {
	.dynticks_nesting = DYNTICK_TASK_EXIT_IDLE,
	.dynticks = ATOMIC_INIT(1),
};

/** 20140823    
 * batch limit과 queue의 hi/low 값 선언.
 **/
static int blimit = 10;		/* Maximum callbacks per rcu_do_batch. */
static int qhimark = 10000;	/* If this many pending, ignore blimit. */
static int qlowmark = 100;	/* Once only this many pending, use blimit. */

module_param(blimit, int, 0);
module_param(qhimark, int, 0);
module_param(qlowmark, int, 0);

/** 20140823    
 * rcu에서 cpu stall 경고 메시지를 막을 것인지 여부.
 * rcu에서 cpu stall 타임아웃으로 취급할 시간을 지정.
 **/
int rcu_cpu_stall_suppress __read_mostly; /* 1 = suppress stall warnings. */
int rcu_cpu_stall_timeout __read_mostly = CONFIG_RCU_CPU_STALL_TIMEOUT;

module_param(rcu_cpu_stall_suppress, int, 0644);
module_param(rcu_cpu_stall_timeout, int, 0644);

static void force_quiescent_state(struct rcu_state *rsp, int relaxed);
static int rcu_pending(int cpu);

/*
 * Return the number of RCU-sched batches processed thus far for debug & stats.
 */
long rcu_batches_completed_sched(void)
{
	return rcu_sched_state.completed;
}
EXPORT_SYMBOL_GPL(rcu_batches_completed_sched);

/*
 * Return the number of RCU BH batches processed thus far for debug & stats.
 */
long rcu_batches_completed_bh(void)
{
	return rcu_bh_state.completed;
}
EXPORT_SYMBOL_GPL(rcu_batches_completed_bh);

/*
 * Force a quiescent state for RCU BH.
 */
void rcu_bh_force_quiescent_state(void)
{
	force_quiescent_state(&rcu_bh_state, 0);
}
EXPORT_SYMBOL_GPL(rcu_bh_force_quiescent_state);

/*
 * Record the number of times rcutorture tests have been initiated and
 * terminated.  This information allows the debugfs tracing stats to be
 * correlated to the rcutorture messages, even when the rcutorture module
 * is being repeatedly loaded and unloaded.  In other words, we cannot
 * store this state in rcutorture itself.
 */
void rcutorture_record_test_transition(void)
{
	rcutorture_testseq++;
	rcutorture_vernum = 0;
}
EXPORT_SYMBOL_GPL(rcutorture_record_test_transition);

/*
 * Record the number of writer passes through the current rcutorture test.
 * This is also used to correlate debugfs tracing stats with the rcutorture
 * messages.
 */
void rcutorture_record_progress(unsigned long vernum)
{
	rcutorture_vernum++;
}
EXPORT_SYMBOL_GPL(rcutorture_record_progress);

/*
 * Force a quiescent state for RCU-sched.
 */
void rcu_sched_force_quiescent_state(void)
{
	force_quiescent_state(&rcu_sched_state, 0);
}
EXPORT_SYMBOL_GPL(rcu_sched_force_quiescent_state);

/*
 * Does the CPU have callbacks ready to be invoked?
 */
/** 20140816
 * gp가 끝나서 호출해야 하는 callback이 있는지를 검사
 * 있으면 1을 리턴
 **/
static int
cpu_has_callbacks_ready_to_invoke(struct rcu_data *rdp)
{
	return &rdp->nxtlist != rdp->nxttail[RCU_DONE_TAIL];
}

/*
 * Does the current CPU require a yet-as-unscheduled grace period?
 */
/** 20140809    
 * 현재 cpu가 아직 schedule 되지 않은 gp에 대한 처리가 필요한지 판단한다.
 *
 * RCU_WAIT 상태의 리스트에 대기 중인 rdp가 존재하고,
 * rsp의 gp가 진행 중이지 않을 경우.
 **/
static int
cpu_needs_another_gp(struct rcu_state *rsp, struct rcu_data *rdp)
{
	return *rdp->nxttail[RCU_DONE_TAIL] && !rcu_gp_in_progress(rsp);
}

/*
 * Return the root node of the specified rcu_state structure.
 */
/** 20140809    
 * rcu_state로부터 root node 를 가져온다.
 *
 * 계층구조 http://lwn.net/Articles/305782/
 **/
static struct rcu_node *rcu_get_root(struct rcu_state *rsp)
{
	return &rsp->node[0];
}

/*
 * If the specified CPU is offline, tell the caller that it is in
 * a quiescent state.  Otherwise, whack it with a reschedule IPI.
 * Grace periods can end up waiting on an offline CPU when that
 * CPU is in the process of coming online -- it will be added to the
 * rcu_node bitmasks before it actually makes it online.  The same thing
 * can happen while a CPU is in the process of coming online.  Because this
 * race is quite rare, we check for it after detecting that the grace
 * period has been delayed rather than checking each and every CPU
 * each and every time we start a new grace period.
 */
/** 20140809    
 * cpu가 offline 상태이고, gp_start 후 3개의 tick 이상 경과했다면
 * offline 상태에 의한 fqs 임을 표시하고 1을 리턴.
 **/
static int rcu_implicit_offline_qs(struct rcu_data *rdp)
{
	/*
	 * If the CPU is offline for more than a jiffy, it is in a quiescent
	 * state.  We can trust its state not to change because interrupts
	 * are disabled.  The reason for the jiffy's worth of slack is to
	 * handle CPUs initializing on the way up and finding their way
	 * to the idle loop on the way down.
	 */
	/** 20140809    
	 * rdp에 해당하는 cpu가 offline 상태이고 gp_start + 2가 지났다면,
	 * quiescent state이므로 qs로 진행한다.
	 * (Quiescent state = CPU not using RCU)
	 **/
	if (cpu_is_offline(rdp->cpu) &&
	    ULONG_CMP_LT(rdp->rsp->gp_start + 2, jiffies)) {
		trace_rcu_fqs(rdp->rsp->name, rdp->gpnum, rdp->cpu, "ofl");
		rdp->offline_fqs++;
		return 1;
	}
	return 0;
}

/*
 * rcu_idle_enter_common - inform RCU that current CPU is moving towards idle
 *
 * If the new value of the ->dynticks_nesting counter now is zero,
 * we really have entered idle, and must do the appropriate accounting.
 * The caller must have disabled interrupts.
 */
/** 20141004    
 * RCU에게 현재 CPU가 idle 상태로 진입함을 알린다.
 * interrupt는 금지된 상태여야 한다.
 * rcu_dynticks의 dynticks 값을 증가시킨다.
 *
 * NO_HZ인 경우 rcu_prepare_for_idle 에서 특정 동작을 한다.
 **/
static void rcu_idle_enter_common(struct rcu_dynticks *rdtp, long long oldval)
{
	trace_rcu_dyntick("Start", oldval, 0);
	/** 20141004    
	 * 현재 task가 idle이 아닌 상태에서 이 함수가 호출된 경우 경고 메시지를 출력한다.
	 **/
	if (!is_idle_task(current)) {
		struct task_struct *idle = idle_task(smp_processor_id());

		trace_rcu_dyntick("Error on entry: not idle task", oldval, 0);
		ftrace_dump(DUMP_ORIG);
		WARN_ONCE(1, "Current pid: %d comm: %s / Idle pid: %d comm: %s",
			  current->pid, current->comm,
			  idle->pid, idle->comm); /* must be idle task! */
	}
	/** 20141011    
	 * idle 상태로 들어가기 전 rcu를 처리한다.
	 **/
	rcu_prepare_for_idle(smp_processor_id());
	/* CPUs seeing atomic_inc() must see prior RCU read-side crit sects */
	/** 20141004    
	 * dynticks 원자적으로 증가.
	 * RCU read-side 임계구역에 대한 조작에 선행되어 atomic_inc가 완료되어야 한다.
	 *
	 * 왜 rdtp->dynticks 를 변경하는데 smp_mb를 사용해야 하나???
	 **/
	smp_mb__before_atomic_inc();  /* See above. */
	atomic_inc(&rdtp->dynticks);
	smp_mb__after_atomic_inc();  /* Force ordering with next sojourn. */
	WARN_ON_ONCE(atomic_read(&rdtp->dynticks) & 0x1);

	/*
	 * The idle task is not permitted to enter the idle loop while
	 * in an RCU read-side critical section.
	 */
	rcu_lockdep_assert(!lock_is_held(&rcu_lock_map),
			   "Illegal idle entry in RCU read-side critical section.");
	rcu_lockdep_assert(!lock_is_held(&rcu_bh_lock_map),
			   "Illegal idle entry in RCU-bh read-side critical section.");
	rcu_lockdep_assert(!lock_is_held(&rcu_sched_lock_map),
			   "Illegal idle entry in RCU-sched read-side critical section.");
}

/**
 * rcu_idle_enter - inform RCU that current CPU is entering idle
 *
 * Enter idle mode, in other words, -leave- the mode in which RCU
 * read-side critical sections can occur.  (Though RCU read-side
 * critical sections can occur in irq handlers in idle, a possibility
 * handled by irq_enter() and irq_exit().)
 *
 * We crowbar the ->dynticks_nesting field to zero to allow for
 * the possibility of usermode upcalls having messed up our count
 * of interrupt nesting level during the prior busy period.
 */
/** 20141004    
 * RCU에게  현재 CPU가 idle 상태로 진입함을 알린다.
 *
 * idle mode로의 진입은 다른 말로 read-side critical sections을 벗어남을 의미한다.
 * (RCU read-side critical sections는 idle 상태의 인터럽트 핸들러에서도 발생할 수 있지만, 이는 irq_enter와 irq_exit에서 다뤄질 수 있다)
 *
 * dynticks_nesting을 0으로 만들어 놓어 usermode upcalls의 가능성을 허용한다.
 * usermode upcall : 앞선 busy 구간동안 인터럽트 네스팅 레벨의 카운트를 뒤섞어 놓을 수 있다.
 **/
void rcu_idle_enter(void)
{
	unsigned long flags;
	long long oldval;
	struct rcu_dynticks *rdtp;

	/** 20141004    
	 * 현재 cpu에 해당하는 rcu_dynticks에서 dynticks_nesting 값을 가져온다.
	 * 이미 IDLE 값을 가진다면 경고 메시지 출력.
	 **/
	local_irq_save(flags);
	rdtp = &__get_cpu_var(rcu_dynticks);
	oldval = rdtp->dynticks_nesting;
	WARN_ON_ONCE((oldval & DYNTICK_TASK_NEST_MASK) == 0);
	/** 20141004    
	 * 중첩 없는 NOT IDLE 상태라면 IDLE 상태로 변경.
	 * 그 외에는 중첩 count를 감소시킨다.
	 **/
	if ((oldval & DYNTICK_TASK_NEST_MASK) == DYNTICK_TASK_NEST_VALUE)
		rdtp->dynticks_nesting = 0;
	else
		rdtp->dynticks_nesting -= DYNTICK_TASK_NEST_VALUE;
	rcu_idle_enter_common(rdtp, oldval);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(rcu_idle_enter);

/**
 * rcu_irq_exit - inform RCU that current CPU is exiting irq towards idle
 *
 * Exit from an interrupt handler, which might possibly result in entering
 * idle mode, in other words, leaving the mode in which read-side critical
 * sections can occur.
 *
 * This code assumes that the idle loop never does anything that might
 * result in unbalanced calls to irq_enter() and irq_exit().  If your
 * architecture violates this assumption, RCU will give you what you
 * deserve, good and hard.  But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 */
/** 20141004    
 * RCU에게 현재 CPU가 irq를 벗어나 idle 상태로 진입함을 알린다.
 *
 * 인터럽트 핸들러에서 벗어나 idle mode로 진입할 수도 있다.
 * (이전 상태가 IDLE 이었다면)
 **/
void rcu_irq_exit(void)
{
	unsigned long flags;
	long long oldval;
	struct rcu_dynticks *rdtp;

	/** 20141004    
	 * local irq를 막아놓은 상태로 수행.
	 *
	 * dynticks_nesting을 하나 감소시켜 interrupt count를 감소시킨다.
	 **/
	local_irq_save(flags);
	rdtp = &__get_cpu_var(rcu_dynticks);
	oldval = rdtp->dynticks_nesting;
	rdtp->dynticks_nesting--;
	WARN_ON_ONCE(rdtp->dynticks_nesting < 0);
	/** 20141004    
	 * nesting이 존재할 경우 trace만 호출하고 벗어난다.
	 * 그렇지 않고 IDLE 상태였다면 irq handler 실행 후, 다시 idle로 들어간다.
	 **/
	if (rdtp->dynticks_nesting)
		trace_rcu_dyntick("--=", oldval, rdtp->dynticks_nesting);
	else
		rcu_idle_enter_common(rdtp, oldval);
	local_irq_restore(flags);
}

/*
 * rcu_idle_exit_common - inform RCU that current CPU is moving away from idle
 *
 * If the new value of the ->dynticks_nesting counter was previously zero,
 * we really have exited idle, and must do the appropriate accounting.
 * The caller must have disabled interrupts.
 */
/** 20141004    
 * RCU에게 현재 CPU가 idle 상태에서 벗어났음을 알린다.
 * rcu_dynticks의 dynticks 값을 증가시킨다.
 *
 * NO_HZ인 경우 rcu_cleanup_after_idle 에서 특정 동작을 한다.
 **/
static void rcu_idle_exit_common(struct rcu_dynticks *rdtp, long long oldval)
{
	/** 20141004    
	 * smp_mb 내에서 rdtp의 dynticks 값을 원자적으로 증가시킨다.
	 *
	 * smp_mb 존재이유???
	 * : interrupt 등으로 인해 idle 상태에서 벗어나면
	 *   RCU read-side critical section이 존재할 수 있고, 이 때 증가된 dynticks를 참고해야 한다.
	 **/
	smp_mb__before_atomic_inc();  /* Force ordering w/previous sojourn. */
	atomic_inc(&rdtp->dynticks);
	/* CPUs seeing atomic_inc() must see later RCU read-side crit sects */
	smp_mb__after_atomic_inc();  /* See above. */
	/** 20141004    
	 * idle상태에서 벗어났으므로 짝수면 잘못된 값이다.
	 **/
	WARN_ON_ONCE(!(atomic_read(&rdtp->dynticks) & 0x1));
	rcu_cleanup_after_idle(smp_processor_id());
	trace_rcu_dyntick("End", oldval, rdtp->dynticks_nesting);
	/** 20141004    
	 * 현재 task가 idle이 아닌 상태에서 이 함수가 호출된 경우 경고 메시지를 출력한다.
	 **/
	if (!is_idle_task(current)) {
		struct task_struct *idle = idle_task(smp_processor_id());

		trace_rcu_dyntick("Error on exit: not idle task",
				  oldval, rdtp->dynticks_nesting);
		ftrace_dump(DUMP_ORIG);
		WARN_ONCE(1, "Current pid: %d comm: %s / Idle pid: %d comm: %s",
			  current->pid, current->comm,
			  idle->pid, idle->comm); /* must be idle task! */
	}
}

/**
 * rcu_idle_exit - inform RCU that current CPU is leaving idle
 *
 * Exit idle mode, in other words, -enter- the mode in which RCU
 * read-side critical sections can occur.
 *
 * We crowbar the ->dynticks_nesting field to DYNTICK_TASK_NEST to
 * allow for the possibility of usermode upcalls messing up our count
 * of interrupt nesting level during the busy period that is just
 * now starting.
 */
void rcu_idle_exit(void)
{
	unsigned long flags;
	struct rcu_dynticks *rdtp;
	long long oldval;

	/** 20141004    
	 * irq 금지 상태로 현재 cpu의 rcu_dynticks를 
	 **/
	local_irq_save(flags);
	rdtp = &__get_cpu_var(rcu_dynticks);
	oldval = rdtp->dynticks_nesting;
	WARN_ON_ONCE(oldval < 0);
	/** 20141004    
	 * 현재 상태가 NOT IDLE이라면, DYNTICK_TASK_NEST_VALUE를 더해 중첩된 상태로 만든다.
	 * 현재 상태가 IDLE이라면, EXIT IDLE을 상태로 만든다(nest 값은 1).
	 **/
	if (oldval & DYNTICK_TASK_NEST_MASK)
		rdtp->dynticks_nesting += DYNTICK_TASK_NEST_VALUE;
	else
		rdtp->dynticks_nesting = DYNTICK_TASK_EXIT_IDLE;
	rcu_idle_exit_common(rdtp, oldval);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(rcu_idle_exit);

/**
 * rcu_irq_enter - inform RCU that current CPU is entering irq away from idle
 *
 * Enter an interrupt handler, which might possibly result in exiting
 * idle mode, in other words, entering the mode in which read-side critical
 * sections can occur.
 *
 * Note that the Linux kernel is fully capable of entering an interrupt
 * handler that it never exits, for example when doing upcalls to
 * user mode!  This code assumes that the idle loop never does upcalls to
 * user mode.  If your architecture does do upcalls from the idle loop (or
 * does anything else that results in unbalanced calls to the irq_enter()
 * and irq_exit() functions), RCU will give you what you deserve, good
 * and hard.  But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 */
/** 20141004    
 * RCU에게 현재 CPU가 idle에서 벗어나 irq mode로 진입할 것임을 알린다.
 *
 * idle mode에서 벗어날 수 있는 interrupt handler가 실행되며,
 * 다른 말로 read-side 임계구역이 발생할 수 있는 모드로 진입함을 의미한다.
 **/
void rcu_irq_enter(void)
{
	unsigned long flags;
	struct rcu_dynticks *rdtp;
	long long oldval;

	/** 20141004    
	 * local irq를 막아놓은 상태로 수행.
	 *
	 * percpu변수 rcu_dynticks에서 dynticks_nesting을 하나 증가시킨다.
	 **/
	local_irq_save(flags);
	rdtp = &__get_cpu_var(rcu_dynticks);
	oldval = rdtp->dynticks_nesting;
	rdtp->dynticks_nesting++;
	WARN_ON_ONCE(rdtp->dynticks_nesting == 0);
	if (oldval)
		/** 20141011
		 * 이전 상태가 IDLE이 아닌 경우. idle에서 벗어나는 함수를 호출하지 않고 리턴. 
		 **/
		trace_rcu_dyntick("++=", oldval, rdtp->dynticks_nesting);
	else
		/** 20141004    
		 * 이전 상태가 IDLE인 경우. irq로 진입하면서 rcu_idle_exit_common 호출.
		 * 이미 rcu_idle_exit가 호출된 경우, 
		 *
		 * idle 상태에서 interrupt가 발생한 경우에 해당하며, dynticks 값을 증가(odd-value: non-idle)시킨다.
		 **/
		rcu_idle_exit_common(rdtp, oldval);
	local_irq_restore(flags);
}

/**
 * rcu_nmi_enter - inform RCU of entry to NMI context
 *
 * If the CPU was idle with dynamic ticks active, and there is no
 * irq handler running, this updates rdtp->dynticks_nmi to let the
 * RCU grace-period handling know that the CPU is active.
 */
void rcu_nmi_enter(void)
{
	struct rcu_dynticks *rdtp = &__get_cpu_var(rcu_dynticks);

	if (rdtp->dynticks_nmi_nesting == 0 &&
	    (atomic_read(&rdtp->dynticks) & 0x1))
		return;
	rdtp->dynticks_nmi_nesting++;
	smp_mb__before_atomic_inc();  /* Force delay from prior write. */
	atomic_inc(&rdtp->dynticks);
	/* CPUs seeing atomic_inc() must see later RCU read-side crit sects */
	smp_mb__after_atomic_inc();  /* See above. */
	WARN_ON_ONCE(!(atomic_read(&rdtp->dynticks) & 0x1));
}

/**
 * rcu_nmi_exit - inform RCU of exit from NMI context
 *
 * If the CPU was idle with dynamic ticks active, and there is no
 * irq handler running, this updates rdtp->dynticks_nmi to let the
 * RCU grace-period handling know that the CPU is no longer active.
 */
void rcu_nmi_exit(void)
{
	struct rcu_dynticks *rdtp = &__get_cpu_var(rcu_dynticks);

	if (rdtp->dynticks_nmi_nesting == 0 ||
	    --rdtp->dynticks_nmi_nesting != 0)
		return;
	/* CPUs seeing atomic_inc() must see prior RCU read-side crit sects */
	smp_mb__before_atomic_inc();  /* See above. */
	atomic_inc(&rdtp->dynticks);
	smp_mb__after_atomic_inc();  /* Force delay to next write. */
	WARN_ON_ONCE(atomic_read(&rdtp->dynticks) & 0x1);
}

/**
 * rcu_is_cpu_idle - see if RCU thinks that the current CPU is idle
 *
 * If the current CPU is in its idle loop and is neither in an interrupt
 * or NMI handler, return true.
 */
/** 20140823    
 * 선점불가 상태에서 현재 cpu에 해당하는 dynticks를 검사해 idle(짝수) 상태인지 검사한다.
 **/
int rcu_is_cpu_idle(void)
{
	int ret;

	preempt_disable();
	ret = (atomic_read(&__get_cpu_var(rcu_dynticks).dynticks) & 0x1) == 0;
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(rcu_is_cpu_idle);

#if defined(CONFIG_PROVE_RCU) && defined(CONFIG_HOTPLUG_CPU)

/*
 * Is the current CPU online?  Disable preemption to avoid false positives
 * that could otherwise happen due to the current CPU number being sampled,
 * this task being preempted, its old CPU being taken offline, resuming
 * on some other CPU, then determining that its old CPU is now offline.
 * It is OK to use RCU on an offline processor during initial boot, hence
 * the check for rcu_scheduler_fully_active.  Note also that it is OK
 * for a CPU coming online to use RCU for one jiffy prior to marking itself
 * online in the cpu_online_mask.  Similarly, it is OK for a CPU going
 * offline to continue to use RCU for one jiffy after marking itself
 * offline in the cpu_online_mask.  This leniency is necessary given the
 * non-atomic nature of the online and offline processing, for example,
 * the fact that a CPU enters the scheduler after completing the CPU_DYING
 * notifiers.
 *
 * This is also why RCU internally marks CPUs online during the
 * CPU_UP_PREPARE phase and offline during the CPU_DEAD phase.
 *
 * Disable checking if in an NMI handler because we cannot safely report
 * errors from NMI handlers anyway.
 */
bool rcu_lockdep_current_cpu_online(void)
{
	struct rcu_data *rdp;
	struct rcu_node *rnp;
	bool ret;

	if (in_nmi())
		return 1;
	preempt_disable();
	rdp = &__get_cpu_var(rcu_sched_data);
	rnp = rdp->mynode;
	ret = (rdp->grpmask & rnp->qsmaskinit) ||
	      !rcu_scheduler_fully_active;
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL_GPL(rcu_lockdep_current_cpu_online);

#endif /* #if defined(CONFIG_PROVE_RCU) && defined(CONFIG_HOTPLUG_CPU) */

/**
 * rcu_is_cpu_rrupt_from_idle - see if idle or immediately interrupted from idle
 *
 * If the current CPU is idle or running at a first-level (not nested)
 * interrupt from idle, return true.  The caller must have at least
 * disabled preemption.
 */
/** 20141004    
 * cpu가 idle 상태이거나, idle에서 irq가 처음 발생했을 때
 * http://lwn.net/Articles/223185/
 * => dynticks로 구현되면서 rcu_irq_enter, rcu_irq_exit, rcu_idle_enter, rcu_idle_exit 함수에서 state 변화를 기록하는 방식으로 실행된다.
 *
 * 1. 현재 상태가 dynticks IDLE이거나, (dynticks_nesting == 0)
 * 2. irq_enter -> (NOT IDLE -> IDLE) -> irq_exit (dynticks_nesting < 0)
 * 3. (NOT IDLE -> IDLE) -> irq_entry -> irq_exit (dynticks_nesting == 1)
 **/
int rcu_is_cpu_rrupt_from_idle(void)
{
	return __get_cpu_var(rcu_dynticks).dynticks_nesting <= 1;
}

/*
 * Snapshot the specified CPU's dynticks counter so that we can later
 * credit them with an implicit quiescent state.  Return 1 if this CPU
 * is in dynticks idle mode, which is an extended quiescent state.
 */
/** 20140809    
 * 현재 dynticks 값을 dynticks_snap에 저장한다.
 *
 * 특정 CPU의 dyncticks counter를 읽어와 추후 묵시적인 qs 상태에 사용한다.
 * 현재 dynticks idle 모드 중일 때는 1을 리턴해 extended qs 상태임을 나타낸다.
 * 그렇지 않은 경우 snap만 남겨두는 것이다.
 **/
static int dyntick_save_progress_counter(struct rcu_data *rdp)
{
	rdp->dynticks_snap = atomic_add_return(0, &rdp->dynticks->dynticks);
	return (rdp->dynticks_snap & 0x1) == 0;
}

/*
 * Return true if the specified CPU has passed through a quiescent
 * state by virtue of being in or having passed through an dynticks
 * idle state since the last call to dyntick_save_progress_counter()
 * for this same CPU.
 */
/** 20140809    
 * 암시적인 rcu dynticks qs를 판단한다.
 *
 * true를 리턴하는 조건
 * 1. dynticks idle 상태이거나, idle/nmi를 거쳤을 경우
 * 2. offline 상태로 3개 이상의 tick이 지난 경우
 **/
static int rcu_implicit_dynticks_qs(struct rcu_data *rdp)
{
	unsigned int curr;
	unsigned int snap;

	/** 20140809    
	 * rcu data에서 현재 dynticks값과 snap을 떠둔 dynticks값을 가져온다.
	 *
	 * snap은 dyntick_save_progress_counter에서 찍어둔다.
	 **/
	curr = (unsigned int)atomic_add_return(0, &rdp->dynticks->dynticks);
	snap = (unsigned int)rdp->dynticks_snap;

	/*
	 * If the CPU passed through or entered a dynticks idle phase with
	 * no active irq/NMI handlers, then we can safely pretend that the CPU
	 * already acknowledged the request to pass through a quiescent
	 * state.  Either way, that CPU cannot possibly be in an RCU
	 * read-side critical section that started before the beginning
	 * of the current RCU grace period.
	 */
	/** 20140809    
	 * 현재 상태가 idle 상태이거나, idle 상태는 아니지만 snap + 2 이상이라면
	 * (idle이나 nmi를 한 번 이상 거쳤을 때)
	 * dynticks에 의한 fqs가 진행되었음을 표시.
	 **/
	if ((curr & 0x1) == 0 || UINT_CMP_GE(curr, snap + 2)) {
		trace_rcu_fqs(rdp->rsp->name, rdp->gpnum, rdp->cpu, "dti");
		rdp->dynticks_fqs++;
		return 1;
	}

	/* Go check for the CPU being offline. */
	return rcu_implicit_offline_qs(rdp);
}

/** 20140809    
 * rcu_cpu_stall_timeout (초)를 jiffies 로 변환.
 * 3~300초 사이로 조정.
 **/
static int jiffies_till_stall_check(void)
{
	int till_stall_check = ACCESS_ONCE(rcu_cpu_stall_timeout);

	/*
	 * Limit check must be consistent with the Kconfig limits
	 * for CONFIG_RCU_CPU_STALL_TIMEOUT.
	 */
	if (till_stall_check < 3) {
		ACCESS_ONCE(rcu_cpu_stall_timeout) = 3;
		till_stall_check = 3;
	} else if (till_stall_check > 300) {
		ACCESS_ONCE(rcu_cpu_stall_timeout) = 300;
		till_stall_check = 300;
	}
	return till_stall_check * HZ + RCU_STALL_DELAY_DELTA;
}

/** 20140809    
 * gp_start, gp stall(칸막이) check time을 기록한다.
 **/
static void record_gp_stall_check_time(struct rcu_state *rsp)
{
	rsp->gp_start = jiffies;
	rsp->jiffies_stall = jiffies + jiffies_till_stall_check();
}

/** 20140906    
 **/
static void print_other_cpu_stall(struct rcu_state *rsp)
{
	int cpu;
	long delta;
	unsigned long flags;
	int ndetected = 0;
	struct rcu_node *rnp = rcu_get_root(rsp);

	/* Only let one CPU complain about others per time interval. */

	raw_spin_lock_irqsave(&rnp->lock, flags);
	delta = jiffies - rsp->jiffies_stall;
	if (delta < RCU_STALL_RAT_DELAY || !rcu_gp_in_progress(rsp)) {
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		return;
	}
	rsp->jiffies_stall = jiffies + 3 * jiffies_till_stall_check() + 3;
	raw_spin_unlock_irqrestore(&rnp->lock, flags);

	/*
	 * OK, time to rat on our buddy...
	 * See Documentation/RCU/stallwarn.txt for info on how to debug
	 * RCU CPU stall warnings.
	 */
	printk(KERN_ERR "INFO: %s detected stalls on CPUs/tasks:",
	       rsp->name);
	print_cpu_stall_info_begin();
	rcu_for_each_leaf_node(rsp, rnp) {
		raw_spin_lock_irqsave(&rnp->lock, flags);
		ndetected += rcu_print_task_stall(rnp);
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		if (rnp->qsmask == 0)
			continue;
		for (cpu = 0; cpu <= rnp->grphi - rnp->grplo; cpu++)
			if (rnp->qsmask & (1UL << cpu)) {
				print_cpu_stall_info(rsp, rnp->grplo + cpu);
				ndetected++;
			}
	}

	/*
	 * Now rat on any tasks that got kicked up to the root rcu_node
	 * due to CPU offlining.
	 */
	rnp = rcu_get_root(rsp);
	raw_spin_lock_irqsave(&rnp->lock, flags);
	ndetected += rcu_print_task_stall(rnp);
	raw_spin_unlock_irqrestore(&rnp->lock, flags);

	print_cpu_stall_info_end();
	printk(KERN_CONT "(detected by %d, t=%ld jiffies)\n",
	       smp_processor_id(), (long)(jiffies - rsp->gp_start));
	if (ndetected == 0)
		printk(KERN_ERR "INFO: Stall ended before state dump start\n");
	else if (!trigger_all_cpu_backtrace())
		dump_stack();

	/* If so configured, complain about tasks blocking the grace period. */

	rcu_print_detail_task_stall(rsp);

	force_quiescent_state(rsp, 0);  /* Kick them all. */
}

/** 20140906    
 * cpu stall message를 출력한다.
 **/
static void print_cpu_stall(struct rcu_state *rsp)
{
	unsigned long flags;
	struct rcu_node *rnp = rcu_get_root(rsp);

	/*
	 * OK, time to rat on ourselves...
	 * See Documentation/RCU/stallwarn.txt for info on how to debug
	 * RCU CPU stall warnings.
	 */
	printk(KERN_ERR "INFO: %s self-detected stall on CPU", rsp->name);
	print_cpu_stall_info_begin();
	print_cpu_stall_info(rsp, smp_processor_id());
	print_cpu_stall_info_end();
	printk(KERN_CONT " (t=%lu jiffies)\n", jiffies - rsp->gp_start);
	if (!trigger_all_cpu_backtrace())
		dump_stack();

	raw_spin_lock_irqsave(&rnp->lock, flags);
	if (ULONG_CMP_GE(jiffies, rsp->jiffies_stall))
		rsp->jiffies_stall = jiffies +
				     3 * jiffies_till_stall_check() + 3;
	raw_spin_unlock_irqrestore(&rnp->lock, flags);

	set_need_resched();  /* kick ourselves to get things going. */
}

/** 20140906    
 * cpu stall을 체크하고, message를 출력한다.
 **/
static void check_cpu_stall(struct rcu_state *rsp, struct rcu_data *rdp)
{
	unsigned long j;
	unsigned long js;
	struct rcu_node *rnp;

	/** 20140906    
	 * rcu cpu stall message 출력이 금지되어 있다면 리턴.
	 **/
	if (rcu_cpu_stall_suppress)
		return;
	j = ACCESS_ONCE(jiffies);
	js = ACCESS_ONCE(rsp->jiffies_stall);
	rnp = rdp->mynode;
	/** 20140906    
	 * 현재 jiffies가 jiffies_stall 이상이라면 cpu stall 메시지를 출력한다.
	 **/
	if ((ACCESS_ONCE(rnp->qsmask) & rdp->grpmask) && ULONG_CMP_GE(j, js)) {

		/* We haven't checked in, so go dump stack. */
		print_cpu_stall(rsp);

	} else if (rcu_gp_in_progress(rsp) &&
		   ULONG_CMP_GE(j, js + RCU_STALL_RAT_DELAY)) {
	/** 20140906    
	 * gp가 진행 중이고, clock irq에 대한 시간을 감안값까지 초과 했다면
	 * cpu stall 메시지를 출력한다.
	 **/

		/* They had a few time units to dump stack, so complain. */
		print_other_cpu_stall(rsp);
	}
}

/** 20140823    
 * panic시 notifier chain이 의해 호출될 callback 함수.
 * stall warning 메시지를 막는다.
 **/
static int rcu_panic(struct notifier_block *this, unsigned long ev, void *ptr)
{
	rcu_cpu_stall_suppress = 1;
	return NOTIFY_DONE;
}

/**
 * rcu_cpu_stall_reset - prevent further stall warnings in current grace period
 *
 * Set the stall-warning timeout way off into the future, thus preventing
 * any RCU CPU stall-warning messages from appearing in the current set of
 * RCU grace periods.
 *
 * The caller must disable hard irqs.
 */
void rcu_cpu_stall_reset(void)
{
	struct rcu_state *rsp;

	for_each_rcu_flavor(rsp)
		rsp->jiffies_stall = jiffies + ULONG_MAX / 2;
}

/** 20140823    
 * rcu_panic을 callback 함수로 지정하는 nb.
 **/
static struct notifier_block rcu_panic_block = {
	.notifier_call = rcu_panic,
};

/** 20140823    
 * panic_notifier_list에 rcu_panic_block 등록.
 **/
static void __init check_cpu_stall_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &rcu_panic_block);
}

/*
 * Update CPU-local rcu_data state to record the newly noticed grace period.
 * This is used both when we started the grace period and when we notice
 * that someone else started the grace period.  The caller must hold the
 * ->lock of the leaf rcu_node structure corresponding to the current CPU,
 *  and must have irqs disabled.
 */
/** 20140809    
 * 새로운 rnp의 gpnum을 rdp에 기록한다.
 **/
static void __note_new_gpnum(struct rcu_state *rsp, struct rcu_node *rnp, struct rcu_data *rdp)
{
	/** 20140809    
	 * rdp와 rnp의 gpnum (현재 # of gp)이 다를 때만 해당
	 **/
	if (rdp->gpnum != rnp->gpnum) {
		/*
		 * If the current grace period is waiting for this CPU,
		 * set up to detect a quiescent state, otherwise don't
		 * go looking for one.
		 */
		/** 20140809    
		 * rnp의 gpnum을 cpu gpnum으로 기록한다.
		 **/
		rdp->gpnum = rnp->gpnum;
		trace_rcu_grace_period(rsp->name, rdp->gpnum, "cpustart");
		/** 20140809    
		 * 현재 cpu가 node의 qs state 관찰 대상에 포함되어 있다면
		 * 현재 cpu가 qs를 pending 시키고 있고,
		 * 통과한 quiesce가 없음을 표시하여 quiescent state를 검출하도록 한다.
		 *
		 * 현재 cpu가 node의 qs state 관찰 대상에 포함되지 않았다면
		 * qs를 pending 중이지 않음을 표시한다.
		 **/
		if (rnp->qsmask & rdp->grpmask) {
			rdp->qs_pending = 1;
			rdp->passed_quiesce = 0;
		} else {
			rdp->qs_pending = 0;
		}
		zero_cpu_stall_ticks(rdp);
	}
}

/** 20140809    
 * 현재 cpu가 속한 leaf node의 gpnum을 현재 cpu의 rdp에 기록한다.
 **/
static void note_new_gpnum(struct rcu_state *rsp, struct rcu_data *rdp)
{
	unsigned long flags;
	struct rcu_node *rnp;

	local_irq_save(flags);
	/** 20140809    
	 * 현재 cpu가 속한 leaf node를 가져온다.
	 **/
	rnp = rdp->mynode;
	/** 20140809    
	 * rdp와 rnp의 gpnum가 이미 같거나 (lock을 벗어났다)
	 * spinlock 획득에 실패했다면 벗어난다.
	 **/
	if (rdp->gpnum == ACCESS_ONCE(rnp->gpnum) || /* outside lock. */
	    !raw_spin_trylock(&rnp->lock)) { /* irqs already off, so later. */
		local_irq_restore(flags);
		return;
	}
	__note_new_gpnum(rsp, rnp, rdp);
	raw_spin_unlock_irqrestore(&rnp->lock, flags);
}

/*
 * Did someone else start a new RCU grace period start since we last
 * checked?  Update local state appropriately if so.  Must be called
 * on the CPU corresponding to rdp.
 */
/** 20140809    
 * 현재 cpu의 rdp가 인식하지 못한 새로운 gp가 있는지 검사하고,
 * 있다면 새로운 gpnum을 복사한다.
 **/
static int
check_for_new_grace_period(struct rcu_state *rsp, struct rcu_data *rdp)
{
	unsigned long flags;
	int ret = 0;

	local_irq_save(flags);
	/** 20140809    
	 * rdp의 gpnum과 rsp의 gpnum이 같지 않다면,
	 * 현재 cpu가 인식하지 못한 새로운 gp가 있으므로
	 * 새로운 gpnum을 기록하고 1을 리턴한다.
	 **/
	if (rdp->gpnum != rsp->gpnum) {
		note_new_gpnum(rsp, rdp);
		ret = 1;
	}
	local_irq_restore(flags);
	return ret;
}

/*
 * Initialize the specified rcu_data structure's callback list to empty.
 */
/** 20140726    
 * callback list를 각각 초기화 한다.
 **/
static void init_callback_list(struct rcu_data *rdp)
{
	int i;

	rdp->nxtlist = NULL;
	for (i = 0; i < RCU_NEXT_SIZE; i++)
		rdp->nxttail[i] = &rdp->nxtlist;
}

/*
 * Advance this CPU's callbacks, but only if the current grace period
 * has ended.  This may be called only from the CPU to whom the rdp
 * belongs.  In addition, the corresponding leaf rcu_node structure's
 * ->lock must be held by the caller, with irqs disabled.
 */
/** 20140809    
 * 현재 gp가 끝났을 때 이 cpu에 대한 callback을 진행해 gp가 끝났음을 표시한다.
 *
 * cpu가 속한 rcu node에 rdp가 알고 있는 gpnum과 다른 정보가 기억되어 있다면,
 * callback 리스트를 옮기고 rnp의 gpnum으로 갱신시킨다.
 **/
static void
__rcu_process_gp_end(struct rcu_state *rsp, struct rcu_node *rnp, struct rcu_data *rdp)
{
	/* Did another grace period end? */
	/** 20140809    
	 * rdp와 rnp의 completed (# of gp)가 같지 않다면
	 * 처리 안 된 gp가 존재한다 판단해 처리한다.
	 **/
	if (rdp->completed != rnp->completed) {

		/* Advance callbacks.  No harm if list empty. */
		/** 20140809    
		 * batch 단위로 nxtlist를 이동시킨다.
		 **/
		rdp->nxttail[RCU_DONE_TAIL] = rdp->nxttail[RCU_WAIT_TAIL];
		rdp->nxttail[RCU_WAIT_TAIL] = rdp->nxttail[RCU_NEXT_READY_TAIL];
		rdp->nxttail[RCU_NEXT_READY_TAIL] = rdp->nxttail[RCU_NEXT_TAIL];

		/* Remember that we saw this grace-period completion. */
		/** 20140809    
		 * rnp의 completed (마지막 완료된 # of gp)를 rdp에 복사.
		 **/
		rdp->completed = rnp->completed;
		trace_rcu_grace_period(rsp->name, rdp->gpnum, "cpuend");

		/*
		 * If we were in an extended quiescent state, we may have
		 * missed some grace periods that others CPUs handled on
		 * our behalf. Catch up with this state to avoid noting
		 * spurious new grace periods.  If another grace period
		 * has started, then rnp->gpnum will have advanced, so
		 * we will detect this later on.
		 */
		/** 20140809    
		 * rdp의 gpnum이 completed보다 작다면 completed로 설정.
		 *
		 * cpu가 extended qs state (IDLE)였다면 gp를 놓쳤을 수 있다.
		 * 다른 gp가 사직되었다면, gpnum이 증가되어 나중에 이것을 파악할 수 있다.
		 **/
		if (ULONG_CMP_LT(rdp->gpnum, rdp->completed))
			rdp->gpnum = rdp->completed;

		/*
		 * If RCU does not need a quiescent state from this CPU,
		 * then make sure that this CPU doesn't go looking for one.
		 */
		/** 20140809    
		 * RCU가 현재 cpu로부터의 qs state가 필요없다면 (rnp->qsmask는 관찰대상)
		 * rdp에 qs_pending을 클리어한다.
		 **/
		if ((rnp->qsmask & rdp->grpmask) == 0)
			rdp->qs_pending = 0;
	}
}

/*
 * Advance this CPU's callbacks, but only if the current grace period
 * has ended.  This may be called only from the CPU to whom the rdp
 * belongs.
 */
/** 20140809    
 * 현재 gp가 끝났을 때 이 cpu에 대한 callback을 진행해 gp가 끝났음을 표시한다.
 **/
static void
rcu_process_gp_end(struct rcu_state *rsp, struct rcu_data *rdp)
{
	unsigned long flags;
	struct rcu_node *rnp;

	local_irq_save(flags);
	/** 20140809    
	 * 현재 cpu가 속해 있는 leaf node를 가져온다.
	 **/
	rnp = rdp->mynode;
	/** 20140809    
	 * rdp와 rnp가 같은 completed (# of gp)이거나,
	 * lock을 획득하지 못했다면 벗어난다.
	 **/
	if (rdp->completed == ACCESS_ONCE(rnp->completed) || /* outside lock. */
	    !raw_spin_trylock(&rnp->lock)) { /* irqs already off, so later. */
		local_irq_restore(flags);
		return;
	}
	__rcu_process_gp_end(rsp, rnp, rdp);
	raw_spin_unlock_irqrestore(&rnp->lock, flags);
}

/*
 * Do per-CPU grace-period initialization for running CPU.  The caller
 * must hold the lock of the leaf rcu_node structure corresponding to
 * this CPU.
 */
static void
rcu_start_gp_per_cpu(struct rcu_state *rsp, struct rcu_node *rnp, struct rcu_data *rdp)
{
	/* Prior grace period ended, so advance callbacks for current CPU. */
	__rcu_process_gp_end(rsp, rnp, rdp);

	/*
	 * Because this CPU just now started the new grace period, we know
	 * that all of its callbacks will be covered by this upcoming grace
	 * period, even the ones that were registered arbitrarily recently.
	 * Therefore, advance all outstanding callbacks to RCU_WAIT_TAIL.
	 *
	 * Other CPUs cannot be sure exactly when the grace period started.
	 * Therefore, their recently registered callbacks must pass through
	 * an additional RCU_NEXT_READY stage, so that they will be handled
	 * by the next RCU grace period.
	 */
	rdp->nxttail[RCU_NEXT_READY_TAIL] = rdp->nxttail[RCU_NEXT_TAIL];
	rdp->nxttail[RCU_WAIT_TAIL] = rdp->nxttail[RCU_NEXT_TAIL];

	/* Set state so that this CPU will detect the next quiescent state. */
	__note_new_gpnum(rsp, rnp, rdp);
}

/*
 * Start a new RCU grace period if warranted, re-initializing the hierarchy
 * in preparation for detecting the next grace period.  The caller must hold
 * the root node's ->lock, which is released before return.  Hard irqs must
 * be disabled.
 *
 * Note that it is legal for a dying CPU (which is marked as offline) to
 * invoke this function.  This can happen when the dying CPU reports its
 * quiescent state.
 */
/** 20140809    
 * 필요한 경우 새로운 gp를 시작한다.
 *
 * 하나의 gp를 마친 뒤 호출될 수도 있고, removal phase 이후 시작될 수도 있다.
 * 다음 gp를 detect하기 위한 준비과정으로 hierarchy를 재초기화 한다.
 **/
static void
rcu_start_gp(struct rcu_state *rsp, unsigned long flags)
	__releases(rcu_get_root(rsp)->lock)
{
	struct rcu_data *rdp = this_cpu_ptr(rsp->rda);
	struct rcu_node *rnp = rcu_get_root(rsp);

	/** 20140809    
	 * rcu scheduler가 완전히 active 되어 있지 않거나 (early_initcall에서 active로 만듦)
	 * cpu가 다른 gp가 필요하지 않다면 벗어난다.
	 **/
	if (!rcu_scheduler_fully_active ||
	    !cpu_needs_another_gp(rsp, rdp)) {
		/*
		 * Either the scheduler hasn't yet spawned the first
		 * non-idle task or this CPU does not need another
		 * grace period.  Either way, don't start a new grace
		 * period.
		 */
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		return;
	}

	/** 20140809    
	 * force qs가 동작 중이라면,
	 * gp가 필요함을 표시하고 벗어난다.
	 **/
	if (rsp->fqs_active) {
		/*
		 * This CPU needs a grace period, but force_quiescent_state()
		 * is running.  Tell it to start one on this CPU's behalf.
		 */
		rsp->fqs_need_gp = 1;
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		return;
	}

	/* Advance to a new grace period and initialize state. */
	/** 20140809    
	 * 현재 gp 번호를 증가시켜 새로운 gp 번호로 삼는다.
	 * fqs_state를 RCU_GP_INIT로 만든다.
	 * 현재 jiffies 이후 3개의 jiffies를 force qs의 시작점으로 삼는다.
	 * cpu stall check time을 저장한다.
	 **/
	rsp->gpnum++;
	trace_rcu_grace_period(rsp->name, rsp->gpnum, "start");
	WARN_ON_ONCE(rsp->fqs_state == RCU_GP_INIT);
	rsp->fqs_state = RCU_GP_INIT; /* Hold off force_quiescent_state. */
	rsp->jiffies_force_qs = jiffies + RCU_JIFFIES_TILL_FORCE_QS;
	record_gp_stall_check_time(rsp);
	raw_spin_unlock(&rnp->lock);  /* leave irqs disabled. */

	/* Exclude any concurrent CPU-hotplug operations. */
	raw_spin_lock(&rsp->onofflock);  /* irqs already disabled. */

	/*
	 * Set the quiescent-state-needed bits in all the rcu_node
	 * structures for all currently online CPUs in breadth-first
	 * order, starting from the root rcu_node structure.  This
	 * operation relies on the layout of the hierarchy within the
	 * rsp->node[] array.  Note that other CPUs will access only
	 * the leaves of the hierarchy, which still indicate that no
	 * grace period is in progress, at least until the corresponding
	 * leaf node has been initialized.  In addition, we have excluded
	 * CPU-hotplug operations.
	 *
	 * Note that the grace period cannot complete until we finish
	 * the initialization process, as there will be at least one
	 * qsmask bit set in the root node until that time, namely the
	 * one corresponding to this CPU, due to the fact that we have
	 * irqs disabled.
	 */
	/** 20140809    
	 * 새로 설정한 rcu state를 기준으로 각 node의 값들을 새로 설정한다.
	 **/
	rcu_for_each_node_breadth_first(rsp, rnp) {
		raw_spin_lock(&rnp->lock);	/* irqs already disabled. */
		rcu_preempt_check_blocked_tasks(rnp);
		/** 20140809    
		 * qsmask를 qsmaskinit으로 설정한다.
		 **/
		rnp->qsmask = rnp->qsmaskinit;
		rnp->gpnum = rsp->gpnum;
		/** 20140809    
		 * rsp의 completed 값을 모든 rnp의 completed에 넣어준다.
		 **/
		rnp->completed = rsp->completed;
		if (rnp == rdp->mynode)
			rcu_start_gp_per_cpu(rsp, rnp, rdp);
		rcu_preempt_boost_start_gp(rnp);
		trace_rcu_grace_period_init(rsp->name, rnp->gpnum,
					    rnp->level, rnp->grplo,
					    rnp->grphi, rnp->qsmask);
		raw_spin_unlock(&rnp->lock);	/* irqs remain disabled. */
	}

	rnp = rcu_get_root(rsp);
	raw_spin_lock(&rnp->lock);		/* irqs already disabled. */
	/** 20140809    
	 * fqs_state를 RCU_SIGNAL_INIT로 만들어 동작 가능 상태로 만든다.
	 * (= RCU_SAVE_DYNTICK)
	 **/
	rsp->fqs_state = RCU_SIGNAL_INIT; /* force_quiescent_state now OK. */
	raw_spin_unlock(&rnp->lock);		/* irqs remain disabled. */
	raw_spin_unlock_irqrestore(&rsp->onofflock, flags);
}

/*
 * Report a full set of quiescent states to the specified rcu_state
 * data structure.  This involves cleaning up after the prior grace
 * period and letting rcu_start_gp() start up the next grace period
 * if one is needed.  Note that the caller must hold rnp->lock, as
 * required by rcu_start_gp(), which will release it.
 */
static void rcu_report_qs_rsp(struct rcu_state *rsp, unsigned long flags)
	__releases(rcu_get_root(rsp)->lock)
{
	unsigned long gp_duration;
	struct rcu_node *rnp = rcu_get_root(rsp);
	struct rcu_data *rdp = this_cpu_ptr(rsp->rda);

	WARN_ON_ONCE(!rcu_gp_in_progress(rsp));

	/*
	 * Ensure that all grace-period and pre-grace-period activity
	 * is seen before the assignment to rsp->completed.
	 */
	smp_mb(); /* See above block comment. */
	/** 20140809    
	 * 현재 jiffies에서 gp_start를 빼 gp_duration를 계산한다.
	 * gp_max 최대값을 갱신한다.
	 **/
	gp_duration = jiffies - rsp->gp_start;
	if (gp_duration > rsp->gp_max)
		rsp->gp_max = gp_duration;

	/*
	 * We know the grace period is complete, but to everyone else
	 * it appears to still be ongoing.  But it is also the case
	 * that to everyone else it looks like there is nothing that
	 * they can do to advance the grace period.  It is therefore
	 * safe for us to drop the lock in order to mark the grace
	 * period as completed in all of the rcu_node structures.
	 *
	 * But if this CPU needs another grace period, it will take
	 * care of this while initializing the next grace period.
	 * We use RCU_WAIT_TAIL instead of the usual RCU_DONE_TAIL
	 * because the callbacks have not yet been advanced: Those
	 * callbacks are waiting on the grace period that just now
	 * completed.
	 */
	/** 20140809    
	 * RCU_WAIT_TAIL 리스트가 비어있다면
	 **/
	if (*rdp->nxttail[RCU_WAIT_TAIL] == NULL) {
		raw_spin_unlock(&rnp->lock);	 /* irqs remain disabled. */

		/*
		 * Propagate new ->completed value to rcu_node structures
		 * so that other CPUs don't have to wait until the start
		 * of the next grace period to process their callbacks.
		 */
		/** 20140809    
		 * 모든 node를 순회하며
		 *   atomic context 하에서
		 *   node의 completed에 현재 gp 번호를 기록한다.
		 **/
		rcu_for_each_node_breadth_first(rsp, rnp) {
			raw_spin_lock(&rnp->lock); /* irqs already disabled. */
			rnp->completed = rsp->gpnum;
			raw_spin_unlock(&rnp->lock); /* irqs remain disabled. */
		}
		rnp = rcu_get_root(rsp);
		raw_spin_lock(&rnp->lock); /* irqs already disabled. */
	}

	/** 20140809    
	 * rsp의 completed에 현재 gp 번호를 기록한다.
	 * force qs 상태가 끝났으므로 RCU_GP_IDLE로 만든다.
	 * 새로운 gp를 시작한다.
	 **/
	rsp->completed = rsp->gpnum;  /* Declare the grace period complete. */
	trace_rcu_grace_period(rsp->name, rsp->completed, "end");
	rsp->fqs_state = RCU_GP_IDLE;
	rcu_start_gp(rsp, flags);  /* releases root node's rnp->lock. */
}

/*
 * Similar to rcu_report_qs_rdp(), for which it is a helper function.
 * Allows quiescent states for a group of CPUs to be reported at one go
 * to the specified rcu_node structure, though all the CPUs in the group
 * must be represented by the same rcu_node structure (which need not be
 * a leaf rcu_node structure, though it often will be).  That structure's
 * lock must be held upon entry, and it is released before return.
 */
/** 20140809    
 * 특정 rcu node에 대해 qs를 기록한다.
 *
 * 그룹에 속한 모든 CPU들은 동일한 rcu_node 구조체로 표현되어야 하지만,
 * 하나에서 시작된 qs 상태의 기록을 허용한다.
 **/
static void
rcu_report_qs_rnp(unsigned long mask, struct rcu_state *rsp,
		  struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	struct rcu_node *rnp_c;

	/* Walk up the rcu_node hierarchy. */
	/** 20140809    
	 * rcu_node hierarchy를 따라올라가 
	 **/
	for (;;) {
		/** 20140809    
		 * mask가 qsmask에 해당하지 않는다면 진행할 이유가 없다.
		 * 아래에서 관찰대상에서 제외되었을 경우에 해당하는 듯.
		 **/
		if (!(rnp->qsmask & mask)) {

			/* Our bit has already been cleared, so done. */
			raw_spin_unlock_irqrestore(&rnp->lock, flags);
			return;
		}
		/** 20140809    
		 * mask는 qsmask에서 제외시킨다. (이 함수의 핵심 동작.)
		 **/
		rnp->qsmask &= ~mask;
		trace_rcu_quiescent_state_report(rsp->name, rnp->gpnum,
						 mask, rnp->qsmask, rnp->level,
						 rnp->grplo, rnp->grphi,
						 !!rnp->gp_tasks);
		/** 20140809    
		 * node의 관찰대상(qsmask)가 남아 있거나, block된 readers가 존재한다면 벗어난다.
		 *
		 * leaf노드의 경우 qsmask는 달려 있는 cpu에 대한 완료를 검사하는 의미이고,
		 * leaf노드가 아닐 경우 qsmask는 달려 있는 node에 대한 완료를 검사하는 의미이다.
		 **/
		if (rnp->qsmask != 0 || rcu_preempt_blocked_readers_cgp(rnp)) {

			/* Other bits still set at this level, so done. */
			raw_spin_unlock_irqrestore(&rnp->lock, flags);
			return;
		}
		mask = rnp->grpmask;
		/** 20140809    
		 * 최상위 레벨까지 올라왔다면 break
		 **/
		if (rnp->parent == NULL) {

			/* No more levels.  Exit loop holding root lock. */

			break;
		}
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		/** 20140809    
		 * 현재 node를 자식 node로 저장하고 상위 level의 node로 이동한다.
		 **/
		rnp_c = rnp;
		rnp = rnp->parent;
		raw_spin_lock_irqsave(&rnp->lock, flags);
		WARN_ON_ONCE(rnp_c->qsmask);
	}

	/*
	 * Get here if we are the last CPU to pass through a quiescent
	 * state for this grace period.  Invoke rcu_report_qs_rsp()
	 * to clean up and start the next grace period if one is needed.
	 */
	/** 20140809    
	 * 해당 node에 대한 report가 끝난 상태로 도달했다면
	 * 현재 gp에 대한 qs 상태 검사를 모두 통과했다.
	 * 따라서 rsp를 clean 시키고, 필요하다면 새로운 gp를 시작한다.
	 **/
	rcu_report_qs_rsp(rsp, flags); /* releases rnp->lock. */
}

/*
 * Record a quiescent state for the specified CPU to that CPU's rcu_data
 * structure.  This must be either called from the specified CPU, or
 * called when the specified CPU is known to be offline (and when it is
 * also known that no other CPU is concurrently trying to help the offline
 * CPU).  The lastcomp argument is used to make sure we are still in the
 * grace period of interest.  We don't want to end the current grace period
 * based on quiescent states detected in an earlier grace period!
 */
/** 20140809    
 * qs state를 특정 cpu의 rdp에 기록한다.
 **/
static void
rcu_report_qs_rdp(int cpu, struct rcu_state *rsp, struct rcu_data *rdp, long lastgp)
{
	unsigned long flags;
	unsigned long mask;
	struct rcu_node *rnp;

	/** 20140809    
	 * 현재 cpu가 속한 leaf node를 가져온다.
	 **/
	rnp = rdp->mynode;
	raw_spin_lock_irqsave(&rnp->lock, flags);
	/** 20140809    
	 * lastgp (rdp->passed_quiesce_gpnum가 전달됨)과 node의 gpnum이 같지 않거나,
	 * node의 gpnum이 완료된 gp라면
	 * 새로운 gp를 위한 qs가 필요하므로 기록하지 않고 벗어난다.
	 **/
	if (lastgp != rnp->gpnum || rnp->completed == rnp->gpnum) {

		/*
		 * The grace period in which this quiescent state was
		 * recorded has ended, so don't report it upwards.
		 * We will instead need a new quiescent state that lies
		 * within the current grace period.
		 */
		rdp->passed_quiesce = 0;	/* need qs for new gp. */
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
		return;
	}
	/** 20140809    
	 * 현재 cpu가 node의 qsmask (qs 관찰 대상)에 속하는 경우
	 **/
	mask = rdp->grpmask;
	if ((rnp->qsmask & mask) == 0) {
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
	} else {
		/** 20140809    
		 * qs가 더이상 pending 되어 있지 않다.
		 **/
		rdp->qs_pending = 0;

		/*
		 * This GP can't end until cpu checks in, so all of our
		 * callbacks can be processed during the next GP.
		 */
		/** 20140809    
		 * rdp를 NEXT_TAIL을 NEXT_READY_TAIL로 옮긴다.
		 **/
		rdp->nxttail[RCU_NEXT_READY_TAIL] = rdp->nxttail[RCU_NEXT_TAIL];

		/** 20140809    
		 * node에 qs state를 기록한다.
		 **/
		rcu_report_qs_rnp(mask, rsp, rnp, flags); /* rlses rnp->lock */
	}
}

/*
 * Check to see if there is a new grace period of which this CPU
 * is not yet aware, and if so, set up local rcu_data state for it.
 * Otherwise, see if this CPU has just passed through its first
 * quiescent state for this grace period, and record that fact if so.
 */
/** 20140816
 * 새로운 gp가 존재하면 rcu_data를 갱신후 리턴하고, 
 * 그렇지 않고 qs 상태이면 report 한다.
 **/

static void
rcu_check_quiescent_state(struct rcu_state *rsp, struct rcu_data *rdp)
{
	/* If there is now a new grace period, record and return. */
	/** 20140809    
	 * 새로운 gp가 존재한다면 그것을 기록하고 리턴한다.
	 **/
	if (check_for_new_grace_period(rsp, rdp))
		return;

	/*
	 * Does this CPU still need to do its part for current grace period?
	 * If no, return and let the other CPUs do their part as well.
	 */
	/** 20140809
	 * cpu의 rdp가 현재 gp에 대해 qs_pending에 해당하지 않으므로
	 * qs 상태를 볼 필요 없이 리턴한다. (note_new_gpnum에서 1로 설정됨)
	 **/
	if (!rdp->qs_pending)
		return;

	/*
	 * Was there a quiescent state since the beginning of the grace
	 * period? If no, then exit and wait for the next call.
	 */
	/** 20140809    
	 * gp 시작 이후로 qs state가 없었다면 벗어난다.
	 **/
	if (!rdp->passed_quiesce)
		return;

	/*
	 * Tell RCU we are done (but rcu_report_qs_rdp() will be the
	 * judge of that).
	 */
	rcu_report_qs_rdp(rdp->cpu, rsp, rdp, rdp->passed_quiesce_gpnum);
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Send the specified CPU's RCU callbacks to the orphanage.  The
 * specified CPU must be offline, and the caller must hold the
 * ->onofflock.
 */
/** 20141018    
 * dead cpu에 해당하는 rdp의 CBs들을 rsp에 등록하고, rdp 리스트를 초기화 한다.
 **/
static void
rcu_send_cbs_to_orphanage(int cpu, struct rcu_state *rsp,
			  struct rcu_node *rnp, struct rcu_data *rdp)
{
	/*
	 * Orphan the callbacks.  First adjust the counts.  This is safe
	 * because ->onofflock excludes _rcu_barrier()'s adoption of
	 * the callbacks, thus no memory barrier is required.
	 */
	/** 20141018    
	 * dead cpu에 CBs이 존재한다면 dead cpu의 rdp를 rsp에 반영시킨다.
	 **/
	if (rdp->nxtlist != NULL) {
		rsp->qlen_lazy += rdp->qlen_lazy;
		rsp->qlen += rdp->qlen;
		rdp->n_cbs_orphaned += rdp->qlen;
		rdp->qlen_lazy = 0;
		ACCESS_ONCE(rdp->qlen) = 0;
	}

	/*
	 * Next, move those callbacks still needing a grace period to
	 * the orphanage, where some other CPU will pick them up.
	 * Some of the callbacks might have gone partway through a grace
	 * period, but that is too bad.  They get to start over because we
	 * cannot assume that grace periods are synchronized across CPUs.
	 * We don't bother updating the ->nxttail[] array yet, instead
	 * we just reset the whole thing later on.
	 */
	if (*rdp->nxttail[RCU_DONE_TAIL] != NULL) {
		*rsp->orphan_nxttail = *rdp->nxttail[RCU_DONE_TAIL];
		rsp->orphan_nxttail = rdp->nxttail[RCU_NEXT_TAIL];
		*rdp->nxttail[RCU_DONE_TAIL] = NULL;
	}

	/*
	 * Then move the ready-to-invoke callbacks to the orphanage,
	 * where some other CPU will pick them up.  These will not be
	 * required to pass though another grace period: They are done.
	 */
	if (rdp->nxtlist != NULL) {
		*rsp->orphan_donetail = rdp->nxtlist;
		rsp->orphan_donetail = rdp->nxttail[RCU_DONE_TAIL];
	}

	/* Finally, initialize the rcu_data structure's list to empty.  */
	init_callback_list(rdp);
}

/*
 * Adopt the RCU callbacks from the specified rcu_state structure's
 * orphanage.  The caller must hold the ->onofflock.
 */
/** 20141018    
 * rsp에 임시 저장 중인 dead cpu의 CBs을 현재 cpu로 입양한다.
 **/
static void rcu_adopt_orphan_cbs(struct rcu_state *rsp)
{
	int i;
	struct rcu_data *rdp = __this_cpu_ptr(rsp->rda);

	/*
	 * If there is an rcu_barrier() operation in progress, then
	 * only the task doing that operation is permitted to adopt
	 * callbacks.  To do otherwise breaks rcu_barrier() and friends
	 * by causing them to fail to wait for the callbacks in the
	 * orphanage.
	 */
	/** 20141018    
	 * rcu_barrier가 진행 중이고 현재 task가 아니라면 입양하면 안 된다.
	 **/
	if (rsp->rcu_barrier_in_progress &&
	    rsp->rcu_barrier_in_progress != current)
		return;

	/* Do the accounting first. */
	/** 20141018    
	 * rcu_send_cbs_to_orphanage()에서 rsp에 달아두었던 작업을 
	 * 현재 cpu의 rdp에 적용한다.
	 **/
	rdp->qlen_lazy += rsp->qlen_lazy;
	rdp->qlen += rsp->qlen;
	rdp->n_cbs_adopted += rsp->qlen;
	if (rsp->qlen_lazy != rsp->qlen)
		rcu_idle_count_callbacks_posted();
	rsp->qlen_lazy = 0;
	rsp->qlen = 0;

	/*
	 * We do not need a memory barrier here because the only way we
	 * can get here if there is an rcu_barrier() in flight is if
	 * we are the task doing the rcu_barrier().
	 */

	/* First adopt the ready-to-invoke callbacks. */
	/** 20141018    
	 * ready-to-invoke CBs를 먼저 입양한다.
	 **/
	if (rsp->orphan_donelist != NULL) {
		*rsp->orphan_donetail = *rdp->nxttail[RCU_DONE_TAIL];
		*rdp->nxttail[RCU_DONE_TAIL] = rsp->orphan_donelist;
		for (i = RCU_NEXT_SIZE - 1; i >= RCU_DONE_TAIL; i--)
			if (rdp->nxttail[i] == rdp->nxttail[RCU_DONE_TAIL])
				rdp->nxttail[i] = rsp->orphan_donetail;
		rsp->orphan_donelist = NULL;
		rsp->orphan_donetail = &rsp->orphan_donelist;
	}

	/* And then adopt the callbacks that still need a grace period. */
	/** 20141018    
	 * GP를 통해야 하는 CBs을 입양한다.
	 **/
	if (rsp->orphan_nxtlist != NULL) {
		*rdp->nxttail[RCU_NEXT_TAIL] = rsp->orphan_nxtlist;
		rdp->nxttail[RCU_NEXT_TAIL] = rsp->orphan_nxttail;
		rsp->orphan_nxtlist = NULL;
		rsp->orphan_nxttail = &rsp->orphan_nxtlist;
	}
}

/*
 * Trace the fact that this CPU is going offline.
 */
static void rcu_cleanup_dying_cpu(struct rcu_state *rsp)
{
	RCU_TRACE(unsigned long mask);
	RCU_TRACE(struct rcu_data *rdp = this_cpu_ptr(rsp->rda));
	RCU_TRACE(struct rcu_node *rnp = rdp->mynode);

	RCU_TRACE(mask = rdp->grpmask);
	trace_rcu_grace_period(rsp->name,
			       rnp->gpnum + 1 - !!(rnp->qsmask & mask),
			       "cpuofl");
}

/*
 * The CPU has been completely removed, and some other CPU is reporting
 * this fact from process context.  Do the remainder of the cleanup,
 * including orphaning the outgoing CPU's RCU callbacks, and also
 * adopting them, if there is no _rcu_barrier() instance running.
 * There can only be one CPU hotplug operation at a time, so no other
 * CPU can be attempting to update rcu_cpu_kthread_task.
 */
/** 20141018    
 * dead cpu에 관련된 rcu 관련 정보를 정리하고,
 * rdp가 속한 rnp의 task 관련 정보를 갱신한다.
 **/
static void rcu_cleanup_dead_cpu(int cpu, struct rcu_state *rsp)
{
	unsigned long flags;
	unsigned long mask;
	int need_report = 0;
	/** 20141018    
	 * dead cpu의 rcu data 위치를 받아온다.
	 **/
	struct rcu_data *rdp = per_cpu_ptr(rsp->rda, cpu);
	struct rcu_node *rnp = rdp->mynode;  /* Outgoing CPU's rdp & rnp. */

	/* Adjust any no-longer-needed kthreads. */
	rcu_stop_cpu_kthread(cpu);
	rcu_node_kthread_setaffinity(rnp, -1);

	/* Remove the dead CPU from the bitmasks in the rcu_node hierarchy. */

	/* Exclude any attempts to start a new grace period. */
	raw_spin_lock_irqsave(&rsp->onofflock, flags);

	/* Orphan the dead CPU's callbacks, and adopt them if appropriate. */
	/** 20141018    
	 * dead cpu의 CBs를 rsp에 달아둔 뒤, 다시 현재 cpu의 rdp에 달아준다.
	 * 즉, 고아가 된 CBs를 입양한다.
	 *
	 * 이 과정은 rsp에 onofflock이 걸린 상태에서 동작해야 한다.
	 **/
	rcu_send_cbs_to_orphanage(cpu, rsp, rnp, rdp);
	rcu_adopt_orphan_cbs(rsp);

	/* Remove the outgoing CPU from the masks in the rcu_node hierarchy. */
	mask = rdp->grpmask;	/* rnp->grplo is constant. */
	do {
		raw_spin_lock(&rnp->lock);	/* irqs already disabled. */
		/** 20141018    
		 * dead cpu에 해당하는 위치를 qsmaskinit에서 제거.
		 * 제거 후에도 qsmaskinit이 남아 있다면 break.
		 **/
		rnp->qsmaskinit &= ~mask;
		if (rnp->qsmaskinit != 0) {
			if (rnp != rdp->mynode)
				raw_spin_unlock(&rnp->lock); /* irqs remain disabled. */
			break;
		}
		if (rnp == rdp->mynode)
			need_report = rcu_preempt_offline_tasks(rsp, rnp, rdp);
		else
			raw_spin_unlock(&rnp->lock); /* irqs remain disabled. */
		mask = rnp->grpmask;
		rnp = rnp->parent;
	} while (rnp != NULL);

	/*
	 * We still hold the leaf rcu_node structure lock here, and
	 * irqs are still disabled.  The reason for this subterfuge is
	 * because invoking rcu_report_unblock_qs_rnp() with ->onofflock
	 * held leads to deadlock.
	 */
	raw_spin_unlock(&rsp->onofflock); /* irqs remain disabled. */
	rnp = rdp->mynode;
	if (need_report & RCU_OFL_TASKS_NORM_GP)
		rcu_report_unblock_qs_rnp(rnp, flags);
	else
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
	if (need_report & RCU_OFL_TASKS_EXP_GP)
		rcu_report_exp_rnp(rsp, rnp, true);
	WARN_ONCE(rdp->qlen != 0 || rdp->nxtlist != NULL,
		  "rcu_cleanup_dead_cpu: Callbacks on offline CPU %d: qlen=%lu, nxtlist=%p\n",
		  cpu, rdp->qlen, rdp->nxtlist);
}

#else /* #ifdef CONFIG_HOTPLUG_CPU */

static void rcu_adopt_orphan_cbs(struct rcu_state *rsp)
{
}

static void rcu_cleanup_dying_cpu(struct rcu_state *rsp)
{
}

static void rcu_cleanup_dead_cpu(int cpu, struct rcu_state *rsp)
{
}

#endif /* #else #ifdef CONFIG_HOTPLUG_CPU */

/*
 * Invoke any RCU callbacks that have made it to the end of their grace
 * period.  Thottle as specified by rdp->blimit.
 */
/** 20140824   
 * GP가 끝나 호출해야 하는 RCU 콜백들이 있으면 호출한다.
 *
 * RCU 콜백은 rdp->nxtlist에 연결되어 있으며, nxttail[]에 의해 조정된다.
 * 완료된 콜백을 리스트에서 제거해 호출한다. blimit 설정값으로 throttle을 준다.
 **/
static void rcu_do_batch(struct rcu_state *rsp, struct rcu_data *rdp)
{
	unsigned long flags;
	struct rcu_head *next, *list, **tail;
	int bl, count, count_lazy, i;

	/** 20140816
	 * 호출해야 하는 callback이 없으면 바로 리턴 
	 **/
	/* If no callbacks are ready, just return.*/
	if (!cpu_has_callbacks_ready_to_invoke(rdp)) {
		trace_rcu_batch_start(rsp->name, rdp->qlen_lazy, rdp->qlen, 0);
		trace_rcu_batch_end(rsp->name, 0, !!ACCESS_ONCE(rdp->nxtlist),
				    need_resched(), is_idle_task(current),
				    rcu_is_callbacks_kthread());
		return;
	}

	/*
	 * Extract the list of ready callbacks, disabling to prevent
	 * races with call_rcu() from interrupt handlers.
	 */
	local_irq_save(flags);
	WARN_ON_ONCE(cpu_is_offline(smp_processor_id()));
	bl = rdp->blimit;
	trace_rcu_batch_start(rsp->name, rdp->qlen_lazy, rdp->qlen, bl);
	list = rdp->nxtlist;
	/** 20140816
	 * [nxtlist,*nxttail[RCU_DONE_TAIL])부분을 nxtlist로부터 분리하고
	 * 만약 nxttail[i]이 빈 파티션일 경우 nxtlist의 주소를 가리키도록 한다.
	 **/
	rdp->nxtlist = *rdp->nxttail[RCU_DONE_TAIL];
	*rdp->nxttail[RCU_DONE_TAIL] = NULL;
	tail = rdp->nxttail[RCU_DONE_TAIL];
	for (i = RCU_NEXT_SIZE - 1; i >= 0; i--)
		if (rdp->nxttail[i] == rdp->nxttail[RCU_DONE_TAIL])
			rdp->nxttail[i] = &rdp->nxtlist;
	local_irq_restore(flags);

	/** 20140816
	 * list가 존재할 경우 list를 하나씩 분리한다. 
	 */
	/* Invoke callbacks. */
	count = count_lazy = 0;
	while (list) {
 		/** 20140816
		 * 다음에 실행될 rcu head를 prefetch한다.
	 	**/
		next = list->next;
		prefetch(next);
		debug_rcu_head_unqueue(list);
		/** 20140816
		 * rcu head의 function이 offset이면 count_lazy를 1증가시킨다.
		 * 콜백이면 호출한다.
		 **/
		if (__rcu_reclaim(rsp->name, list))
			count_lazy++;
		list = next;
		/* Stop only if limit reached and CPU has something to do. */
		/** 20140816
		 * batch limit을 초과하면서 
		 * rescheduling이 필요하거나 현재 task가 idle태스크가 아니라면 
		 * 루프를 빠져나온다
		 **/
		if (++count >= bl &&
		    (need_resched() ||
		     (!is_idle_task(current) && !rcu_is_callbacks_kthread())))
			break;
	}

	local_irq_save(flags);
	trace_rcu_batch_end(rsp->name, count, !!list, need_resched(),
			    is_idle_task(current),
			    rcu_is_callbacks_kthread());

	/* Update count, and requeue any remaining callbacks. */
	/** 20140816
	 * 처리하고 남은 list에 대해서 다시 붙여주고 nxttail 배열을 갱신시킨다.
	 **/
	if (list != NULL) {
		*tail = rdp->nxtlist;
		rdp->nxtlist = list;
		for (i = 0; i < RCU_NEXT_SIZE; i++)
			if (&rdp->nxtlist == rdp->nxttail[i])
				rdp->nxttail[i] = tail;
			else
				break;
	}
	smp_mb(); /* List handling before counting for rcu_barrier(). */
	/** 20140823    
	 * rcu_head의 func이 offset인 경우의 수만큼 qlen_lazy를 감소시킨다.
	 * queue된 callback 수를 처리한 콜백 수만큼 감소시킨다.
	 * invoke된 cbs 수를 처리한 콜백 수만큼 증가시킨다.
	 **/
	rdp->qlen_lazy -= count_lazy;
	ACCESS_ONCE(rdp->qlen) -= count;
	rdp->n_cbs_invoked += count;

	/* Reinstate batch limit if we have worked down the excess. */
	/** 20140823    
	 * batch limit이 변경된 상태이고, qlen이 lowmark 이하로 떨어지면
	 * batch limit을 복원한다. 왜???
	 **/
	if (rdp->blimit == LONG_MAX && rdp->qlen <= qlowmark)
		rdp->blimit = blimit;

	/* Reset ->qlen_last_fqs_check trigger if enough CBs have drained. */
	/** 20140823    
	 * 현재 queue에 CBs가 0이고, fqs시 마지막 check한 개수가 0이 아니라면
	 *   qlen_last_fqs_check를 초기화하고,
	 *   rcu_state의 force_qs 수를 snap으로 저장해 둔다.
	 * 그렇지 않고, qlen이 (qlen_last_fqs_check - qhimark) 보다 작다면
	 *   qlen_last_fqs_check를 현재 qlen으로 갱신시킨다.
	 **/
	if (rdp->qlen == 0 && rdp->qlen_last_fqs_check != 0) {
		rdp->qlen_last_fqs_check = 0;
		rdp->n_force_qs_snap = rsp->n_force_qs;
	} else if (rdp->qlen < rdp->qlen_last_fqs_check - qhimark)
		rdp->qlen_last_fqs_check = rdp->qlen;
	WARN_ON_ONCE((rdp->nxtlist == NULL) != (rdp->qlen == 0));

	local_irq_restore(flags);

	/* Re-invoke RCU core processing if there are callbacks remaining. */
	/** 20140830    
	 * 다시 남아 있는 CB이 있는지 검사해 남아 있다면 invoke 한다.
	 **/
	if (cpu_has_callbacks_ready_to_invoke(rdp))
		invoke_rcu_core();
}

/*
 * Check to see if this CPU is in a non-context-switch quiescent state
 * (user mode or idle loop for rcu, non-softirq execution for rcu_bh).
 * Also schedule RCU core processing.
 *
 * This function must be called from hardirq context.  It is normally
 * invoked from the scheduling-clock interrupt.  If rcu_pending returns
 * false, there is no point in invoking rcu_check_callbacks().
 */
/** 20140830    
 * tick interrupt handler에 의해 호출되어
 * 'Note Quiescent State'와 softirq를 통한 'Complete grace period' 처리가 이루어 지는 곳.
 *   => 'StatusOfLinuxDynaticks.pdf' 참고.
 *
 * user: user tick. user mode에서 timer interrupt에 의해 진입을 표시
 **/
void rcu_check_callbacks(int cpu, int user)
{
	trace_rcu_utilization("Start scheduler-tick");
	increment_cpu_stall_ticks();
	/** 20140906    
	 * user mode나 idle loop 상태에서 인터럽트 발생(nested 제외)으로 호출된 경우,
	 * CPU는 QS state다. (read-side에서 preempt_disable을 하기 때문)
	 *
	 * 따라서 qs state를 기록한다.
	 *
	 * rcu qs는 context switch, idle (either dynticks or the idle loop), and user-mode execution일 때 기록한다.
	 * rcu_bh qs는 interrupts가 활성화된 상태로 softirq의 밖에서 실행되는 코드일 때 기록한다.
	 * 따라서 rcu qs는 rch_bh qs도 포함한다.
	 **/
	if (user || rcu_is_cpu_rrupt_from_idle()) {

		/*
		 * Get here if this CPU took its interrupt from user
		 * mode or from the idle loop, and if this is not a
		 * nested interrupt.  In this case, the CPU is in
		 * a quiescent state, so note it.
		 *
		 * No memory barrier is required here because both
		 * rcu_sched_qs() and rcu_bh_qs() reference only CPU-local
		 * variables that other CPUs neither access nor modify,
		 * at least not while the corresponding CPU is online.
		 */

		rcu_sched_qs(cpu);
		rcu_bh_qs(cpu);

	} else if (!in_softirq()) {

		/*
		 * Get here if this CPU did not take its interrupt from
		 * softirq, in other words, if it is not interrupting
		 * a rcu_bh read-side critical section.  This is an _bh
		 * critical section, so note it.
		 */

		/** 20140906    
		 * timer interrupt handler를 통해 들어온 상태이다.
		 *
		 * softirq가 아닌 상태에서 진입한 경우라면, rcu_bh read-side critical
		 * section이 방해받지 않았으므로 정상적으로 rcu_bh가 qs라 표시한다.
		 **/
		rcu_bh_qs(cpu);
	}
	rcu_preempt_check_callbacks(cpu);
	/** 20140830    
	 * rcu 관련 작업이 cpu에 pending되어 있다면 softirq를 raise 해 처리한다.
	 *
	 * 'Complete grace period' 수행 동작 포함.
	 **/
	if (rcu_pending(cpu))
		invoke_rcu_core();
	trace_rcu_utilization("End scheduler-tick");
}

/*
 * Scan the leaf rcu_node structures, processing dyntick state for any that
 * have not yet encountered a quiescent state, using the function specified.
 * Also initiate boosting for any threads blocked on the root rcu_node.
 *
 * The caller must have suppressed start of new grace periods.
 */
/** 20140809    
 * leaf node를 scan해 아직 qs state가 거치지 않은 node에 주어진 함수를 수행한다.
 * caller는 새로운 gp를 막아 놓아야 한다. (rsp->fqs_active를 1로 표시)
 **/
static void force_qs_rnp(struct rcu_state *rsp, int (*f)(struct rcu_data *))
{
	unsigned long bit;
	int cpu;
	unsigned long flags;
	unsigned long mask;
	struct rcu_node *rnp;

	/** 20140809    
	 * 모든 leaf node를 순회한다.
	 *
	 * 모든 node를 순회하는 동안 lock을 걸어두지 않고,
	 * node사이를 이동할 때마다 lock/unlock을 반복한다.
	 **/
	rcu_for_each_leaf_node(rsp, rnp) {
		mask = 0;
		raw_spin_lock_irqsave(&rnp->lock, flags);
		/** 20140809    
		 * 현재 rcu의 gp가 진행 중이지 않을 경우 벗어난다.
		 **/
		if (!rcu_gp_in_progress(rsp)) {
			raw_spin_unlock_irqrestore(&rnp->lock, flags);
			return;
		}
		/** 20140809    
		 * 현재 node의 qsmask가 0이라면 rcu_initiate_boost호출 후
		 * 다음 node로 이동한다.
		 **/
		if (rnp->qsmask == 0) {
			rcu_initiate_boost(rnp, flags); /* releases rnp->lock */
			continue;
		}
		/** 20140809    
		 * 현재 node의 low부터 high까지 순회한다.
		 * bit 1부터 다음 cpu로증가할 때마다 비트 역시 하나씩 shift.
		 * cpu:     3210
		 * bit: 00001111
		 *           
		 **/
		cpu = rnp->grplo;
		bit = 1;
		/** 20140809    
		 * 현재 cpu가 qsmask에 속해 있고, 전달받은 함수의 호출 결과가 참이라면
		 * mask에 현재 cpu에 해당하는 bit를 표시한다.
		 *
		 * f가 dyntick_save_progress_counter로 전달되었을 경우,
		 * cpu가 하나 이상 idle mode일 경우 해당한다.
		 *
		 * f가 rcu_implicit_dynticks_qs가 전달되었을 경우,
		 * 현재 상태가 idle 상태가 아니거나, idle/nmi 상태를 거쳤을 경우
		 **/
		for (; cpu <= rnp->grphi; cpu++, bit <<= 1) {
			if ((rnp->qsmask & bit) != 0 &&
			    f(per_cpu_ptr(rsp->rda, cpu)))
				mask |= bit;
		}
		/** 20140809    
		 * mask가 하나라도 표시되어 있다면 rcu_report_qs_rnp 를 수행한다.
		 **/
		if (mask != 0) {

			/* rcu_report_qs_rnp() releases rnp->lock. */
			rcu_report_qs_rnp(mask, rsp, rnp, flags);
			continue;
		}
		raw_spin_unlock_irqrestore(&rnp->lock, flags);
	}
	/** 20140809    
	 * root의 qsmask가 0이라면 rcu_initiate_boost를 호출한다.
	 **/
	rnp = rcu_get_root(rsp);
	if (rnp->qsmask == 0) {
		raw_spin_lock_irqsave(&rnp->lock, flags);
		rcu_initiate_boost(rnp, flags); /* releases rnp->lock. */
	}
}

/*
 * Force quiescent states on reluctant CPUs, and also detect which
 * CPUs are in dyntick-idle mode.
 */
/** 20140726    
 * 강제로 qs state를 진행시킬 필요가 있는지 검사하고,
 * 필요하다면 force qs를 진행한다.
 *
 * force qs state-machine을 돌린다.
 *
 * [참고] RCU STATE MACHINE
 *		http://lwn.net/Articles/305782/
 *
 *	Quiescent state = CPU not using RCU
 **/
static void force_quiescent_state(struct rcu_state *rsp, int relaxed)
{
	unsigned long flags;
	struct rcu_node *rnp = rcu_get_root(rsp);

	trace_rcu_utilization("Start fqs");
	/** 20140809    
	 * rcu가 진행 중이 아니라면 강제로 qs상태로 진입할 필요가 없다.
	 **/
	if (!rcu_gp_in_progress(rsp)) {
		trace_rcu_utilization("End fqs");
		return;  /* No grace period in progress, nothing to force. */
	}
	/** 20140809    
	 * fqslock을 획득하지 못했다면 변수를 증가시키고 나간다.
	 **/
	if (!raw_spin_trylock_irqsave(&rsp->fqslock, flags)) {
		rsp->n_force_qs_lh++; /* Inexact, can lose counts.  Tough! */
		trace_rcu_utilization("End fqs");
		return;	/* Someone else is already on the job. */
	}
	/** 20140809    
	 * relaxed이면서(no emergency) force_qs가 jiffies 보다 커졌다면
	 *   (즉, jiffies_force_qs에 도달하지 못했다면)
	 * 이전 체크 후 완료되었고, lock을 해제하고 벗어난다.
	 **/
	if (relaxed && ULONG_CMP_GE(rsp->jiffies_force_qs, jiffies))
		goto unlock_fqs_ret; /* no emergency and done recently. */
	/** 20140809    
	 * force_quiescent_state가 호출된 횟수 증가.
	 **/
	rsp->n_force_qs++;
	raw_spin_lock(&rnp->lock);  /* irqs already disabled */
	/** 20140809    
	 * jiffies_force_qs를 update.
	 **/
	rsp->jiffies_force_qs = jiffies + RCU_JIFFIES_TILL_FORCE_QS;
	/** 20140809    
	 * rcu gp가 진행 중이 아니라면 no GP active를 증가시키고 벗어난다.
	 **/
	if(!rcu_gp_in_progress(rsp)) {
		rsp->n_force_qs_ngp++;
		raw_spin_unlock(&rnp->lock);  /* irqs remain disabled */
		goto unlock_fqs_ret;  /* no GP in progress, time updated. */
	}
	/** 20140809    
	 * force qs가 동작 중임을 표시.
	 *
	 * force qs state인 경우 rcu_start_gp에서 새로운 gp를 시작하지 못한다.
	 **/
	rsp->fqs_active = 1;
	switch (rsp->fqs_state) {
	case RCU_GP_IDLE:
	case RCU_GP_INIT:

		break; /* grace period idle or initializing, ignore. */

	/** 20140809    
	 * rcu_start_gp 에서 RCU_SAVE_DYNTICK 상태로 만든다.
	 * SAVE_DYNTICK: Need to scan dyntick state. 
	 **/
	case RCU_SAVE_DYNTICK:

		raw_spin_unlock(&rnp->lock);  /* irqs remain disabled */

		/* Record dyntick-idle state. */
		/** 20140809    
		 * 현재 dynticks counter를 저장한다.
		 **/
		force_qs_rnp(rsp, dyntick_save_progress_counter);
		raw_spin_lock(&rnp->lock);  /* irqs already disabled */
		/** 20140809    
		 * gp 진행 중일 경우, fqs_state를 RCU_FORCE_QS로 만든다.
		 **/
		if (rcu_gp_in_progress(rsp))
			rsp->fqs_state = RCU_FORCE_QS;
		break;

	case RCU_FORCE_QS:
/* Check dyntick-idle state, send IPI to laggarts. */
		raw_spin_unlock(&rnp->lock);  /* irqs remain disabled */
		force_qs_rnp(rsp, rcu_implicit_dynticks_qs);

		/* Leave state in case more forcing is required. */

		raw_spin_lock(&rnp->lock);  /* irqs already disabled */
		break;
	}
	/** 20140809    
	 * fqs 동작이 완료되었으므로 fqs가 동작 중이지 않음을 표시.
	 **/
	rsp->fqs_active = 0;
	/** 20140809    
	 * force qs에 의해 새로운 gp 시작이 block되어 있었다면
	 * fqs_need_gp를 지우고 새로운 gp를 시작하고 리턴한다..
	 **/
	if (rsp->fqs_need_gp) {
		raw_spin_unlock(&rsp->fqslock); /* irqs remain disabled */
		rsp->fqs_need_gp = 0;
		rcu_start_gp(rsp, flags); /* releases rnp->lock */
		trace_rcu_utilization("End fqs");
		return;
	}
	raw_spin_unlock(&rnp->lock);  /* irqs remain disabled */
unlock_fqs_ret:
	raw_spin_unlock_irqrestore(&rsp->fqslock, flags);
	trace_rcu_utilization("End fqs");
}

/*
 * This does the RCU core processing work for the specified rcu_state
 * and rcu_data structures.  This may be called only from the CPU to
 * whom the rdp belongs.
 */
/** 20141011    
 * 특정 rsp와 rdp로 RCU가 진행해야 할 작업을 처리하는 핵심함수.
 *
 * 1. 우선 force_qs를 진행할 정도로 충분한 시간이 흘렀다면 force qs를 처리한다.
 *		force_quiescent_state
 * 2. 이후 rnp의 데이터와 동기를 맞춘 뒤,
 *		rcu_process_gp_end
 * 3. 최근에 qs가 발생했는지 판단해 report 한다.
 *		rcu_check_quiescent_state
 * 4. 새로운 gp가 존재한다면 시작시키고,
 *		(force_quiescent_state에 의해 block된 것도 여기서 실행 될 것이다)
 *		rcu_start_gp
 * 5. 호출할 CB들이 있다면 호출한다.
 *		invoke_rcu_callbacks
 **/
static void
__rcu_process_callbacks(struct rcu_state *rsp)
{
	unsigned long flags;
	/** 20140726    
	 * 현재 cpu의 rcu_data 변수에 접근해 data를 가져온다.
	 **/
	struct rcu_data *rdp = __this_cpu_ptr(rsp->rda);

	WARN_ON_ONCE(rdp->beenonline == 0);

	/*
	 * If an RCU GP has gone long enough, go check for dyntick
	 * idle CPUs and, if needed, send resched IPIs.
	 */
	/** 20140726    
	 * rcu_state의 jiffies_force_qs로 지정한 값이 현재 지피보다 작다면,
	 * 강제로 quiescent_state(CPU not using RCU)로 진입한다.
	 *
	 * jiffies_force_qs는 rcu_start_gp()에서 지정.
	 **/
	if (ULONG_CMP_LT(ACCESS_ONCE(rsp->jiffies_force_qs), jiffies))
		force_quiescent_state(rsp, 1);

	/*
	 * Advance callbacks in response to end of earlier grace
	 * period that some other CPU ended.
	 */
	/** 20140809    
	 * 같은 rcu_node의 다른 CPU가 끝낸 이전 gp에 대한 완료를 처리한다.
	 **/
	rcu_process_gp_end(rsp, rdp);

	/* Update RCU state based on any recent quiescent states. */
	/** 20140816
	 * qs상태 여부를 판단하여 rdp 업데이트 및 rnp, rsp에 리포트한다.
	 **/
	rcu_check_quiescent_state(rsp, rdp);

	/** 20140816
	 * 새롭게 실행되어야 할 gp가 존재하면 gp를 시작한다
	 **/
	/* Does this CPU require a not-yet-started grace period? */
	if (cpu_needs_another_gp(rsp, rdp)) {
		raw_spin_lock_irqsave(&rcu_get_root(rsp)->lock, flags);
		rcu_start_gp(rsp, flags);  /* releases above lock */
	}

	/* If there are callbacks ready, invoke them. */
	/** 20140830    
	 * rcu_process_gp_end() 에서 nxttail을 조정한 뒤
	 * callback 리스트에 대기 중인 CB들이 있다면 콜백들을 호출한다.
	 **/
	if (cpu_has_callbacks_ready_to_invoke(rdp))
		invoke_rcu_callbacks(rsp, rdp);
}

/*
 * Do RCU core processing for the current CPU.
 */
static void rcu_process_callbacks(struct softirq_action *unused)
{
	struct rcu_state *rsp;

	trace_rcu_utilization("Start RCU core");
	for_each_rcu_flavor(rsp)
		__rcu_process_callbacks(rsp);
	trace_rcu_utilization("End RCU core");
}

/*
 * Schedule RCU callback invocation.  If the specified type of RCU
 * does not support RCU priority boosting, just do a direct call,
 * otherwise wake up the per-CPU kernel kthread.  Note that because we
 * are running on the current CPU with interrupts disabled, the
 * rcu_cpu_kthread_task cannot disappear out from under us.
 */
/** 20140830    
 * grace period 완료를 대기 중인 rcu callback들을 처리한다.
 * boost가 아닌 경우 rcu_do_batch로 처리하고,
 * 그렇지 않은 경우 kthread로 처리한다.
 **/
static void invoke_rcu_callbacks(struct rcu_state *rsp, struct rcu_data *rdp)
{
	/** 20140830    
	 * rcu scheduler가 아직 완전히 동작 중이지 않은 경우 return 한다.
	 **/
	if (unlikely(!ACCESS_ONCE(rcu_scheduler_fully_active)))
		return;
	/** 20140830    
	 * rcu boost가 아닌 경우 rcu_do_batch로 일정 개수만큼 CB을 호출한다.
	 **/
	if (likely(!rsp->boost)) {
		rcu_do_batch(rsp, rdp);
		return;
	}
	invoke_rcu_callbacks_kthread();
}

/** 20140726    
 * RCU_SOFTIRQ를 raise.
 * open_softirq에서 등록한 rcu_process_callbacks가 호출된다.
 **/
static void invoke_rcu_core(void)
{
	raise_softirq(RCU_SOFTIRQ);
}

/*
 * Handle any core-RCU processing required by a call_rcu() invocation.
 */
/** 20140831    
 * call_rcu에 의해 호출되며, RCU core 처리가 필요한 경우 처리한다.
 *
 * extended qs에서 호출된 경우, SOFTIRQ를 발생시켜 CBs를 처리한다.
 * 너무 많은 CBs가 대기 중이거나 충분히 오랜시간이 흘렀다면 강제로 qs state로 처리한다.
 **/
static void __call_rcu_core(struct rcu_state *rsp, struct rcu_data *rdp,
			    struct rcu_head *head, unsigned long flags)
{
	/*
	 * If called from an extended quiescent state, invoke the RCU
	 * core in order to force a re-evaluation of RCU's idleness.
	 */
	/** 20140830    
	 * rcu가 cpu idle 상태이고 현재 cpu가 online인 경우,
	 * 즉 extended qs에서 호출된 경우 RCU_SOFTIRQ를 raise 한다.
	 **/
	if (rcu_is_cpu_idle() && cpu_online(smp_processor_id()))
		invoke_rcu_core();

	/* If interrupts were disabled or CPU offline, don't invoke RCU core. */
	/** 20140830    
	 * call_rcu가 irq disabled 상태에서 호출되었거나 cpu가 offline이면
	 * RCU core를 호출하지 않고 바로 리턴.
	 **/
	if (irqs_disabled_flags(flags) || cpu_is_offline(smp_processor_id()))
		return;

	/*
	 * Force the grace period if too many callbacks or too long waiting.
	 * Enforce hysteresis, and don't invoke force_quiescent_state()
	 * if some other CPU has recently done so.  Also, don't bother
	 * invoking force_quiescent_state() if the newly enqueued callback
	 * is the only one waiting for a grace period to complete.
	 */
	if (unlikely(rdp->qlen > rdp->qlen_last_fqs_check + qhimark)) {
		/** 20140830    
		 * queue되어 있는 CBs의 수가 마지막 check 이후 qhimark 수를 넘는다면
		 **/

		/* Are we ignoring a completed grace period? */
		/** 20140830    
		 * 현재 gp가 끝났다고 표시하고, 새로운 gp가 있는지 검사한다.
		 **/
		rcu_process_gp_end(rsp, rdp);
		check_for_new_grace_period(rsp, rdp);

		/* Start a new grace period if one not already started. */
		if (!rcu_gp_in_progress(rsp)) {
			/** 20140831    
			 * gp가 진행 중이 아닐 경우, 새로운 gp를 시작한다.
			 **/
			unsigned long nestflag;
			struct rcu_node *rnp_root = rcu_get_root(rsp);

			raw_spin_lock_irqsave(&rnp_root->lock, nestflag);
			rcu_start_gp(rsp, nestflag);  /* rlses rnp_root->lock */
		} else {
			/* Give the grace period a kick. */
			/** 20140831    
			 * gp가 진행 중일 경우, 필요한 경우 강제로 qs state로 진행한다.
			 **/
			rdp->blimit = LONG_MAX;
			/** 20140831    
			 * 진행한 force qs 수가 snap의 수와 같고,
			 * head가 WAIT head가 아니라면 ???
			 *   force qs로 진입한다.
			 **/
			if (rsp->n_force_qs == rdp->n_force_qs_snap &&
			    *rdp->nxttail[RCU_DONE_TAIL] != head)
				force_quiescent_state(rsp, 0);
			/** 20140831    
			 * qs state 관련 정보를 갱신한다.
			 **/
			rdp->n_force_qs_snap = rsp->n_force_qs;
			rdp->qlen_last_fqs_check = rdp->qlen;
		}
	} else if (ULONG_CMP_LT(ACCESS_ONCE(rsp->jiffies_force_qs), jiffies))
		/** 20140831    
		 * qlen이 qhimark를 초과하지는 않지만,
		 * force qs 진입할 시간이 초과된 경우 강제로 qs 상태로 진입한다.
		 **/
		force_quiescent_state(rsp, 1);
}

/** 20140831    
 * func을 CB 리스트에 등록한다.
 *
 * call_rcu에 관련된 core-RCU를 처리한다.
 **/
static void
__call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *rcu),
	   struct rcu_state *rsp, bool lazy)
{
	unsigned long flags;
	struct rcu_data *rdp;

	WARN_ON_ONCE((unsigned long)head & 0x3); /* Misaligned rcu_head! */
	debug_rcu_head_queue(head);
	/** 20140823    
	 * rcu_head에 reclaim용 CB 지정.
	 **/
	head->func = func;
	head->next = NULL;

	smp_mb(); /* Ensure RCU update seen before callback registry. */

	/*
	 * Opportunistically note grace-period endings and beginnings.
	 * Note that we might see a beginning right after we see an
	 * end, but never vice versa, since this CPU has to pass through
	 * a quiescent state betweentimes.
	 */
	/** 20140830    
	 * 현재 cpu의 irq를 막고 상태를 저장해
	 * 원자적으로 실행가능하도록 한다.
	 **/
	local_irq_save(flags);
	rdp = this_cpu_ptr(rsp->rda);

	/* Add the callback to our list. */
	/** 20140823    
	 * qlen을 증가시킨다. qlen은 CBs의 수. lazy 포함.
	 **/
	ACCESS_ONCE(rdp->qlen)++;
	/** 20140823    
	 * lazy인 경우 qlen_lazy 증가.
	 * 아닌 경우 CONFIG_RCU_FAST_NO_HZ일 때는 rcu_dynticks에 발생횟수를 증가시킨다.
	 **/
	if (lazy)
		rdp->qlen_lazy++;
	else
		rcu_idle_count_callbacks_posted();
	smp_mb();  /* Count before adding callback for rcu_barrier(). */
	/** 20140823    
	 * 새로운 rcu_head를 nxtlist의 끝(nxttail[RCU_NEXT_TAIL]이 가리키는 위치)에
	 * 등록시키고, 새로운 nxttail[RCU_NEXT_TAIL]로 등록.
	 **/
	*rdp->nxttail[RCU_NEXT_TAIL] = head;
	rdp->nxttail[RCU_NEXT_TAIL] = &head->next;

	if (__is_kfree_rcu_offset((unsigned long)func))
		trace_rcu_kfree_callback(rsp->name, head, (unsigned long)func,
					 rdp->qlen_lazy, rdp->qlen);
	else
		trace_rcu_callback(rsp->name, head, rdp->qlen_lazy, rdp->qlen);

	/* Go handle any RCU core processing required. */
	/** 20140831    
	 * call_rcu에 의해 필요한 core-RCU 부분을 처리한다.
	 **/
	__call_rcu_core(rsp, rdp, head, flags);
	local_irq_restore(flags);
}

/*
 * Queue an RCU-sched callback for invocation after a grace period.
 */
/** 20141025    
 * rcu_sched_state에 callback을 등록한다.
 * 여기서 등록한 callback은 gp 이후 호출된다.
 **/
void call_rcu_sched(struct rcu_head *head, void (*func)(struct rcu_head *rcu))
{
	__call_rcu(head, func, &rcu_sched_state, 0);
}
EXPORT_SYMBOL_GPL(call_rcu_sched);

/*
 * Queue an RCU callback for invocation after a quicker grace period.
 */
void call_rcu_bh(struct rcu_head *head, void (*func)(struct rcu_head *rcu))
{
	__call_rcu(head, func, &rcu_bh_state, 0);
}
EXPORT_SYMBOL_GPL(call_rcu_bh);

/*
 * Because a context switch is a grace period for RCU-sched and RCU-bh,
 * any blocking grace-period wait automatically implies a grace period
 * if there is only one CPU online at any point time during execution
 * of either synchronize_sched() or synchronize_rcu_bh().  It is OK to
 * occasionally incorrectly indicate that there are multiple CPUs online
 * when there was in fact only one the whole time, as this just adds
 * some overhead: RCU still operates correctly.
 */
/** 20141025    
 * 현재 online인 cpu가 1개라면
 * rcu blocking이 곧 grace period를 의미한다.
 **/
static inline int rcu_blocking_is_gp(void)
{
	int ret;

	might_sleep();  /* Check for RCU read-side critical section. */
	preempt_disable();
	ret = num_online_cpus() <= 1;
	preempt_enable();
	return ret;
}

/**
 * synchronize_sched - wait until an rcu-sched grace period has elapsed.
 *
 * Control will return to the caller some time after a full rcu-sched
 * grace period has elapsed, in other words after all currently executing
 * rcu-sched read-side critical sections have completed.   These read-side
 * critical sections are delimited by rcu_read_lock_sched() and
 * rcu_read_unlock_sched(), and may be nested.  Note that preempt_disable(),
 * local_irq_disable(), and so on may be used in place of
 * rcu_read_lock_sched().
 *
 * This means that all preempt_disable code sequences, including NMI and
 * hardware-interrupt handlers, in progress on entry will have completed
 * before this primitive returns.  However, this does not guarantee that
 * softirq handlers will have completed, since in some kernels, these
 * handlers can run in process context, and can block.
 *
 * This primitive provides the guarantees made by the (now removed)
 * synchronize_kernel() API.  In contrast, synchronize_rcu() only
 * guarantees that rcu_read_lock() sections will have completed.
 * In "classic RCU", these two guarantees happen to be one and
 * the same, but can differ in realtime RCU implementations.
 */
/** 20141025    
 * rcu-sched의 grace period가 지나갈 때까지 기다린다.
 * 이미 존재하는 rcu-sched의 read-side critical section이 완료될 때까지 기다린다.
 *
 * synchronize_rcu의 non-PREEMPT 버전.
 * call_rcu_sched를 호출하고 대기한다.
 **/
void synchronize_sched(void)
{
	rcu_lockdep_assert(!lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_sched() in RCU-sched read-side critical section");
	/** 20141025    
	 * online cpu가 하나라면 block이 곧 grace period를 의미한다.
	 * 따라서 대기 없이 바로 리턴한다.
	 **/
	if (rcu_blocking_is_gp())
		return;
	/** 20141025
	 **/
	wait_rcu_gp(call_rcu_sched);
}
EXPORT_SYMBOL_GPL(synchronize_sched);

/**
 * synchronize_rcu_bh - wait until an rcu_bh grace period has elapsed.
 *
 * Control will return to the caller some time after a full rcu_bh grace
 * period has elapsed, in other words after all currently executing rcu_bh
 * read-side critical sections have completed.  RCU read-side critical
 * sections are delimited by rcu_read_lock_bh() and rcu_read_unlock_bh(),
 * and may be nested.
 */
void synchronize_rcu_bh(void)
{
	rcu_lockdep_assert(!lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_rcu_bh() in RCU-bh read-side critical section");
	if (rcu_blocking_is_gp())
		return;
	wait_rcu_gp(call_rcu_bh);
}
EXPORT_SYMBOL_GPL(synchronize_rcu_bh);

static atomic_t sync_sched_expedited_started = ATOMIC_INIT(0);
static atomic_t sync_sched_expedited_done = ATOMIC_INIT(0);

static int synchronize_sched_expedited_cpu_stop(void *data)
{
	/*
	 * There must be a full memory barrier on each affected CPU
	 * between the time that try_stop_cpus() is called and the
	 * time that it returns.
	 *
	 * In the current initial implementation of cpu_stop, the
	 * above condition is already met when the control reaches
	 * this point and the following smp_mb() is not strictly
	 * necessary.  Do smp_mb() anyway for documentation and
	 * robustness against future implementation changes.
	 */
	smp_mb(); /* See above comment block. */
	return 0;
}

/**
 * synchronize_sched_expedited - Brute-force RCU-sched grace period
 *
 * Wait for an RCU-sched grace period to elapse, but use a "big hammer"
 * approach to force the grace period to end quickly.  This consumes
 * significant time on all CPUs and is unfriendly to real-time workloads,
 * so is thus not recommended for any sort of common-case code.  In fact,
 * if you are using synchronize_sched_expedited() in a loop, please
 * restructure your code to batch your updates, and then use a single
 * synchronize_sched() instead.
 *
 * Note that it is illegal to call this function while holding any lock
 * that is acquired by a CPU-hotplug notifier.  And yes, it is also illegal
 * to call this function from a CPU-hotplug notifier.  Failing to observe
 * these restriction will result in deadlock.
 *
 * This implementation can be thought of as an application of ticket
 * locking to RCU, with sync_sched_expedited_started and
 * sync_sched_expedited_done taking on the roles of the halves
 * of the ticket-lock word.  Each task atomically increments
 * sync_sched_expedited_started upon entry, snapshotting the old value,
 * then attempts to stop all the CPUs.  If this succeeds, then each
 * CPU will have executed a context switch, resulting in an RCU-sched
 * grace period.  We are then done, so we use atomic_cmpxchg() to
 * update sync_sched_expedited_done to match our snapshot -- but
 * only if someone else has not already advanced past our snapshot.
 *
 * On the other hand, if try_stop_cpus() fails, we check the value
 * of sync_sched_expedited_done.  If it has advanced past our
 * initial snapshot, then someone else must have forced a grace period
 * some time after we took our snapshot.  In this case, our work is
 * done for us, and we can simply return.  Otherwise, we try again,
 * but keep our initial snapshot for purposes of checking for someone
 * doing our work for us.
 *
 * If we fail too many times in a row, we fall back to synchronize_sched().
 */
void synchronize_sched_expedited(void)
{
	int firstsnap, s, snap, trycount = 0;

	/* Note that atomic_inc_return() implies full memory barrier. */
	firstsnap = snap = atomic_inc_return(&sync_sched_expedited_started);
	get_online_cpus();
	WARN_ON_ONCE(cpu_is_offline(raw_smp_processor_id()));

	/*
	 * Each pass through the following loop attempts to force a
	 * context switch on each CPU.
	 */
	while (try_stop_cpus(cpu_online_mask,
			     synchronize_sched_expedited_cpu_stop,
			     NULL) == -EAGAIN) {
		put_online_cpus();

		/* No joy, try again later.  Or just synchronize_sched(). */
		if (trycount++ < 10) {
			udelay(trycount * num_online_cpus());
		} else {
			synchronize_sched();
			return;
		}

		/* Check to see if someone else did our work for us. */
		s = atomic_read(&sync_sched_expedited_done);
		if (UINT_CMP_GE((unsigned)s, (unsigned)firstsnap)) {
			smp_mb(); /* ensure test happens before caller kfree */
			return;
		}

		/*
		 * Refetching sync_sched_expedited_started allows later
		 * callers to piggyback on our grace period.  We subtract
		 * 1 to get the same token that the last incrementer got.
		 * We retry after they started, so our grace period works
		 * for them, and they started after our first try, so their
		 * grace period works for us.
		 */
		get_online_cpus();
		snap = atomic_read(&sync_sched_expedited_started);
		smp_mb(); /* ensure read is before try_stop_cpus(). */
	}

	/*
	 * Everyone up to our most recent fetch is covered by our grace
	 * period.  Update the counter, but only if our work is still
	 * relevant -- which it won't be if someone who started later
	 * than we did beat us to the punch.
	 */
	do {
		s = atomic_read(&sync_sched_expedited_done);
		if (UINT_CMP_GE((unsigned)s, (unsigned)snap)) {
			smp_mb(); /* ensure test happens before caller kfree */
			break;
		}
	} while (atomic_cmpxchg(&sync_sched_expedited_done, s, snap) != s);

	put_online_cpus();
}
EXPORT_SYMBOL_GPL(synchronize_sched_expedited);

/*
 * Check to see if there is any immediate RCU-related work to be done
 * by the current CPU, for the specified type of RCU, returning 1 if so.
 * The checks are in order of increasing expense: checks that can be
 * carried out against CPU-local state are performed first.  However,
 * we must check for CPU stalls first, else we might not get a chance.
 */
/** 20140906    
 * rcu 관련된 작업이 현재 cpu에 pending되어 있다면 1이 리턴된다.
 **/
static int __rcu_pending(struct rcu_state *rsp, struct rcu_data *rdp)
{
	struct rcu_node *rnp = rdp->mynode;

	/** 20140906    
	 * trace용 변수.
	 **/
	rdp->n_rcu_pending++;

	/* Check for CPU stalls, if enabled. */
	check_cpu_stall(rsp, rdp);

	/* Is the RCU core waiting for a quiescent state from this CPU? */
	/** 20140906    
	 * rcu scheduler가 완전히 동작 중인 상태에서, qs가 pending되어 있고,
	 * 새로운 gp 시작 이후 qs가 한 번도 이뤄지지 않은 경우
	 **/
	if (rcu_scheduler_fully_active &&
	    rdp->qs_pending && !rdp->passed_quiesce) {

		/*
		 * If force_quiescent_state() coming soon and this CPU
		 * needs a quiescent state, and this is either RCU-sched
		 * or RCU-bh, force a local reschedule.
		 */
		rdp->n_rp_qs_pending++;
		if (!rdp->preemptible &&
		    ULONG_CMP_LT(ACCESS_ONCE(rsp->jiffies_force_qs) - 1,
				 jiffies))
			set_need_resched();
	} else if (rdp->qs_pending && rdp->passed_quiesce) {
	/** 20140906    
	 * qs state를 기다리고 있고, passed_quiesce가 존재하면
	 * rcu가 pending되어 있다.
	 **/
		rdp->n_rp_report_qs++;
		return 1;
	}

	/* Does this CPU have callbacks ready to invoke? */
	/** 20140906    
	 * 호출을 기다리는 cb 함수들이 존재할 경우 pending이다.
	 **/
	if (cpu_has_callbacks_ready_to_invoke(rdp)) {
		rdp->n_rp_cb_ready++;
		return 1;
	}

	/* Has RCU gone idle with this CPU needing another grace period? */
	if (cpu_needs_another_gp(rsp, rdp)) {
		rdp->n_rp_cpu_needs_gp++;
		return 1;
	}

	/* Has another RCU grace period completed?  */
	if (ACCESS_ONCE(rnp->completed) != rdp->completed) { /* outside lock */
		rdp->n_rp_gp_completed++;
		return 1;
	}

	/* Has a new RCU grace period started? */
	if (ACCESS_ONCE(rnp->gpnum) != rdp->gpnum) { /* outside lock */
		rdp->n_rp_gp_started++;
		return 1;
	}

	/* Has an RCU GP gone long enough to send resched IPIs &c? */
	if (rcu_gp_in_progress(rsp) &&
	    ULONG_CMP_LT(ACCESS_ONCE(rsp->jiffies_force_qs), jiffies)) {
		rdp->n_rp_need_fqs++;
		return 1;
	}

	/* nothing to do */
	rdp->n_rp_need_nothing++;
	return 0;
}

/*
 * Check to see if there is any immediate RCU-related work to be done
 * by the current CPU, returning 1 if so.  This function is part of the
 * RCU implementation; it is -not- an exported member of the RCU API.
 */
/** 20140830    
 * 현재 cpu에 RCU 관련 작업이 pending되어 있다면 1을 리턴한다.
 **/
static int rcu_pending(int cpu)
{
	struct rcu_state *rsp;

	for_each_rcu_flavor(rsp)
		if (__rcu_pending(rsp, per_cpu_ptr(rsp->rda, cpu)))
			return 1;
	return 0;
}

/*
 * Check to see if any future RCU-related work will need to be done
 * by the current CPU, even if none need be done immediately, returning
 * 1 if so.
 */
/** 20141011    
 * 각 rsp 중에 특정 cpu에 해당하는 rcu_data를 가져와 아직 수행하지 않은
 * CBs이 존재하는지 검사한다.
 **/
static int rcu_cpu_has_callbacks(int cpu)
{
	struct rcu_state *rsp;

	/* RCU callbacks either ready or pending? */
	for_each_rcu_flavor(rsp)
		if (per_cpu_ptr(rsp->rda, cpu)->nxtlist)
			return 1;
	return 0;
}

/*
 * Helper function for _rcu_barrier() tracing.  If tracing is disabled,
 * the compiler is expected to optimize this away.
 */
static void _rcu_barrier_trace(struct rcu_state *rsp, char *s,
			       int cpu, unsigned long done)
{
	trace_rcu_barrier(rsp->name, s, cpu,
			  atomic_read(&rsp->barrier_cpu_count), done);
}

/*
 * RCU callback function for _rcu_barrier().  If we are last, wake
 * up the task executing _rcu_barrier().
 */
static void rcu_barrier_callback(struct rcu_head *rhp)
{
	struct rcu_data *rdp = container_of(rhp, struct rcu_data, barrier_head);
	struct rcu_state *rsp = rdp->rsp;

	if (atomic_dec_and_test(&rsp->barrier_cpu_count)) {
		_rcu_barrier_trace(rsp, "LastCB", -1, rsp->n_barrier_done);
		complete(&rsp->barrier_completion);
	} else {
		_rcu_barrier_trace(rsp, "CB", -1, rsp->n_barrier_done);
	}
}

/*
 * Called with preemption disabled, and from cross-cpu IRQ context.
 */
static void rcu_barrier_func(void *type)
{
	struct rcu_state *rsp = type;
	struct rcu_data *rdp = __this_cpu_ptr(rsp->rda);

	_rcu_barrier_trace(rsp, "IRQ", -1, rsp->n_barrier_done);
	atomic_inc(&rsp->barrier_cpu_count);
	rsp->call(&rdp->barrier_head, rcu_barrier_callback);
}

/*
 * Orchestrate the specified type of RCU barrier, waiting for all
 * RCU callbacks of the specified type to complete.
 */
static void _rcu_barrier(struct rcu_state *rsp)
{
	int cpu;
	unsigned long flags;
	struct rcu_data *rdp;
	struct rcu_data rd;
	unsigned long snap = ACCESS_ONCE(rsp->n_barrier_done);
	unsigned long snap_done;

	init_rcu_head_on_stack(&rd.barrier_head);
	_rcu_barrier_trace(rsp, "Begin", -1, snap);

	/* Take mutex to serialize concurrent rcu_barrier() requests. */
	mutex_lock(&rsp->barrier_mutex);

	/*
	 * Ensure that all prior references, including to ->n_barrier_done,
	 * are ordered before the _rcu_barrier() machinery.
	 */
	smp_mb();  /* See above block comment. */

	/*
	 * Recheck ->n_barrier_done to see if others did our work for us.
	 * This means checking ->n_barrier_done for an even-to-odd-to-even
	 * transition.  The "if" expression below therefore rounds the old
	 * value up to the next even number and adds two before comparing.
	 */
	snap_done = ACCESS_ONCE(rsp->n_barrier_done);
	_rcu_barrier_trace(rsp, "Check", -1, snap_done);
	if (ULONG_CMP_GE(snap_done, ((snap + 1) & ~0x1) + 2)) {
		_rcu_barrier_trace(rsp, "EarlyExit", -1, snap_done);
		smp_mb(); /* caller's subsequent code after above check. */
		mutex_unlock(&rsp->barrier_mutex);
		return;
	}

	/*
	 * Increment ->n_barrier_done to avoid duplicate work.  Use
	 * ACCESS_ONCE() to prevent the compiler from speculating
	 * the increment to precede the early-exit check.
	 */
	ACCESS_ONCE(rsp->n_barrier_done)++;
	WARN_ON_ONCE((rsp->n_barrier_done & 0x1) != 1);
	_rcu_barrier_trace(rsp, "Inc1", -1, rsp->n_barrier_done);
	smp_mb(); /* Order ->n_barrier_done increment with below mechanism. */

	/*
	 * Initialize the count to one rather than to zero in order to
	 * avoid a too-soon return to zero in case of a short grace period
	 * (or preemption of this task).  Also flag this task as doing
	 * an rcu_barrier().  This will prevent anyone else from adopting
	 * orphaned callbacks, which could cause otherwise failure if a
	 * CPU went offline and quickly came back online.  To see this,
	 * consider the following sequence of events:
	 *
	 * 1.	We cause CPU 0 to post an rcu_barrier_callback() callback.
	 * 2.	CPU 1 goes offline, orphaning its callbacks.
	 * 3.	CPU 0 adopts CPU 1's orphaned callbacks.
	 * 4.	CPU 1 comes back online.
	 * 5.	We cause CPU 1 to post an rcu_barrier_callback() callback.
	 * 6.	Both rcu_barrier_callback() callbacks are invoked, awakening
	 *	us -- but before CPU 1's orphaned callbacks are invoked!!!
	 */
	init_completion(&rsp->barrier_completion);
	atomic_set(&rsp->barrier_cpu_count, 1);
	raw_spin_lock_irqsave(&rsp->onofflock, flags);
	rsp->rcu_barrier_in_progress = current;
	raw_spin_unlock_irqrestore(&rsp->onofflock, flags);

	/*
	 * Force every CPU with callbacks to register a new callback
	 * that will tell us when all the preceding callbacks have
	 * been invoked.  If an offline CPU has callbacks, wait for
	 * it to either come back online or to finish orphaning those
	 * callbacks.
	 */
	for_each_possible_cpu(cpu) {
		preempt_disable();
		rdp = per_cpu_ptr(rsp->rda, cpu);
		if (cpu_is_offline(cpu)) {
			_rcu_barrier_trace(rsp, "Offline", cpu,
					   rsp->n_barrier_done);
			preempt_enable();
			while (cpu_is_offline(cpu) && ACCESS_ONCE(rdp->qlen))
				schedule_timeout_interruptible(1);
		} else if (ACCESS_ONCE(rdp->qlen)) {
			_rcu_barrier_trace(rsp, "OnlineQ", cpu,
					   rsp->n_barrier_done);
			smp_call_function_single(cpu, rcu_barrier_func, rsp, 1);
			preempt_enable();
		} else {
			_rcu_barrier_trace(rsp, "OnlineNQ", cpu,
					   rsp->n_barrier_done);
			preempt_enable();
		}
	}

	/*
	 * Now that all online CPUs have rcu_barrier_callback() callbacks
	 * posted, we can adopt all of the orphaned callbacks and place
	 * an rcu_barrier_callback() callback after them.  When that is done,
	 * we are guaranteed to have an rcu_barrier_callback() callback
	 * following every callback that could possibly have been
	 * registered before _rcu_barrier() was called.
	 */
	raw_spin_lock_irqsave(&rsp->onofflock, flags);
	rcu_adopt_orphan_cbs(rsp);
	rsp->rcu_barrier_in_progress = NULL;
	raw_spin_unlock_irqrestore(&rsp->onofflock, flags);
	atomic_inc(&rsp->barrier_cpu_count);
	smp_mb__after_atomic_inc(); /* Ensure atomic_inc() before callback. */
	rd.rsp = rsp;
	rsp->call(&rd.barrier_head, rcu_barrier_callback);

	/*
	 * Now that we have an rcu_barrier_callback() callback on each
	 * CPU, and thus each counted, remove the initial count.
	 */
	if (atomic_dec_and_test(&rsp->barrier_cpu_count))
		complete(&rsp->barrier_completion);

	/* Increment ->n_barrier_done to prevent duplicate work. */
	smp_mb(); /* Keep increment after above mechanism. */
	ACCESS_ONCE(rsp->n_barrier_done)++;
	WARN_ON_ONCE((rsp->n_barrier_done & 0x1) != 0);
	_rcu_barrier_trace(rsp, "Inc2", -1, rsp->n_barrier_done);
	smp_mb(); /* Keep increment before caller's subsequent code. */

	/* Wait for all rcu_barrier_callback() callbacks to be invoked. */
	wait_for_completion(&rsp->barrier_completion);

	/* Other rcu_barrier() invocations can now safely proceed. */
	mutex_unlock(&rsp->barrier_mutex);

	destroy_rcu_head_on_stack(&rd.barrier_head);
}

/**
 * rcu_barrier_bh - Wait until all in-flight call_rcu_bh() callbacks complete.
 */
void rcu_barrier_bh(void)
{
	_rcu_barrier(&rcu_bh_state);
}
EXPORT_SYMBOL_GPL(rcu_barrier_bh);

/**
 * rcu_barrier_sched - Wait for in-flight call_rcu_sched() callbacks.
 */
void rcu_barrier_sched(void)
{
	_rcu_barrier(&rcu_sched_state);
}
EXPORT_SYMBOL_GPL(rcu_barrier_sched);

/*
 * Do boot-time initialization of a CPU's per-CPU RCU data.
 */
/** 20140726    
 * boot-time에 percpu rcu_data를 초기화 한다.
 **/
static void __init
rcu_boot_init_percpu_data(int cpu, struct rcu_state *rsp)
{
	unsigned long flags;
	struct rcu_data *rdp = per_cpu_ptr(rsp->rda, cpu);
	struct rcu_node *rnp = rcu_get_root(rsp);

	/* Set up local state, ensuring consistent view of global state. */
	raw_spin_lock_irqsave(&rnp->lock, flags);
	rdp->grpmask = 1UL << (cpu - rdp->mynode->grplo);
	init_callback_list(rdp);
	rdp->qlen_lazy = 0;
	ACCESS_ONCE(rdp->qlen) = 0;
	rdp->dynticks = &per_cpu(rcu_dynticks, cpu);
	WARN_ON_ONCE(rdp->dynticks->dynticks_nesting != DYNTICK_TASK_EXIT_IDLE);
	WARN_ON_ONCE(atomic_read(&rdp->dynticks->dynticks) != 1);
	rdp->cpu = cpu;
	rdp->rsp = rsp;
	raw_spin_unlock_irqrestore(&rnp->lock, flags);
}

/*
 * Initialize a CPU's per-CPU RCU data.  Note that only one online or
 * offline event can be happening at a given time.  Note also that we
 * can accept some slop in the rsp->completed access due to the fact
 * that this CPU cannot possibly have any RCU callbacks in flight yet.
 */
/** 20140823    
 * per-cpu data인 rcu_data 변수를 초기화 한다.
 **/
static void __cpuinit
rcu_init_percpu_data(int cpu, struct rcu_state *rsp, int preemptible)
{
	unsigned long flags;
	unsigned long mask;
	struct rcu_data *rdp = per_cpu_ptr(rsp->rda, cpu);
	struct rcu_node *rnp = rcu_get_root(rsp);

	/* Set up local state, ensuring consistent view of global state. */
	/** 20140823    
	 * rcu_node에 lock을 걸고, rcu_data 변수를 초기화 한다.
	 **/
	raw_spin_lock_irqsave(&rnp->lock, flags);
	rdp->beenonline = 1;	 /* We have now been online. */
	rdp->preemptible = preemptible;
	rdp->qlen_last_fqs_check = 0;
	rdp->n_force_qs_snap = rsp->n_force_qs;
	rdp->blimit = blimit;
	/** 20141004    
	 * dyntick이 EXIT IDLE 상태이다.
	 **/
	rdp->dynticks->dynticks_nesting = DYNTICK_TASK_EXIT_IDLE;
	/** 20140823    
	 * dynticks를 홀수값으로 만들어 idle 상태가 아니도록 한다.
	 **/
	atomic_set(&rdp->dynticks->dynticks,
		   (atomic_read(&rdp->dynticks->dynticks) & ~0x1) + 1);
	/** 20141011
	 * 
	 **/
	rcu_prepare_for_idle_init(cpu);
	raw_spin_unlock(&rnp->lock);		/* irqs remain disabled. */

	/*
	 * A new grace period might start here.  If so, we won't be part
	 * of it, but that is OK, as we are currently in a quiescent state.
	 */

	/* Exclude any attempts to start a new GP on large systems. */
	raw_spin_lock(&rsp->onofflock);		/* irqs already disabled. */

	/* Add CPU to rcu_node bitmasks. */
	/** 20140823    
	 * rcu_data가 속한 node를 가져옴
	 * grpmask를 가져옴.
	 **/
	rnp = rdp->mynode;
	mask = rdp->grpmask;
	/** 20140823    
	 * rcu_data가 등록된 node에 대해 한 번 수행하고,
	 * parent로 올라가면서 NULL이 아니고, node의 gsmaskinit에 등록되지 않은 경우 수행.
	 **/
	do {
		/* Exclude any attempts to start a new GP on small systems. */
		raw_spin_lock(&rnp->lock);	/* irqs already disabled. */
		/** 20140823    
		 * node의 qsmaskinit에 추가.
		 **/
		rnp->qsmaskinit |= mask;
		mask = rnp->grpmask;
		/** 20140823    
		 * node hierarchy의 parent를 올라갈 때,
		 * rdp의 mynode일 때만 수행하는 동작.
		 **/
		if (rnp == rdp->mynode) {
			/*
			 * If there is a grace period in progress, we will
			 * set up to wait for it next time we run the
			 * RCU core code.
			 */
			/** 20140823    
			 * node가 갖고 있는 completed (마지막 완료된 gp num)을 rdp에 복사.
			 **/
			rdp->gpnum = rnp->completed;
			rdp->completed = rnp->completed;
			rdp->passed_quiesce = 0;
			rdp->qs_pending = 0;
			/** 20140823    
			 * 현재 node가 알고 있는 gpnum - 1로 초기화.
			 * 0으로 초기화 하지 않는 이유는???
			 **/
			rdp->passed_quiesce_gpnum = rnp->gpnum - 1;
			trace_rcu_grace_period(rsp->name, rdp->gpnum, "cpuonl");
		}
		raw_spin_unlock(&rnp->lock); /* irqs already disabled. */
		rnp = rnp->parent;
	} while (rnp != NULL && !(rnp->qsmaskinit & mask));

	raw_spin_unlock_irqrestore(&rsp->onofflock, flags);
}

/** 20140823    
 * 특정 cpu가 rcu로 동작하도록 준비하는 과정.
 * 예를 들어 HOTPLUG cpu가 새로운 online 된 경우. 
 **/
static void __cpuinit rcu_prepare_cpu(int cpu)
{
	struct rcu_state *rsp;

	/** 20140823    
	 * flavor list에 등록된 rsp를 순회하며
	 *		cpu로 지정된 cpu의 rcu_data를 초기화 한다.
	 *		rsp가 "rcu_preempt"인 경우 preemptible.
	 **/
	for_each_rcu_flavor(rsp)
		rcu_init_percpu_data(cpu, rsp,
				     strcmp(rsp->name, "rcu_preempt") == 0);
}

/*
 * Handle CPU online/offline notification events.
 */
/** 20140823    
 * rcu cpu notify callback 함수.
 * CPU online/offline notification에 대해 처리한다.
 *
 * CPU_UP_PREPARE에 해당하는 action만 분석한 상태.
 **/
static int __cpuinit rcu_cpu_notify(struct notifier_block *self,
				    unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	struct rcu_data *rdp = per_cpu_ptr(rcu_state->rda, cpu);
	struct rcu_node *rnp = rdp->mynode;
	struct rcu_state *rsp;

	trace_rcu_utilization("Start CPU hotplug");
	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		rcu_prepare_cpu(cpu);
		rcu_prepare_kthreads(cpu);
		break;
	case CPU_ONLINE:
	case CPU_DOWN_FAILED:
		rcu_node_kthread_setaffinity(rnp, -1);
		rcu_cpu_kthread_setrt(cpu, 1);
		break;
	case CPU_DOWN_PREPARE:
		rcu_node_kthread_setaffinity(rnp, cpu);
		rcu_cpu_kthread_setrt(cpu, 0);
		break;
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		/*
		 * The whole machine is "stopped" except this CPU, so we can
		 * touch any data without introducing corruption. We send the
		 * dying CPU's callbacks to an arbitrarily chosen online CPU.
		 */
		for_each_rcu_flavor(rsp)
			rcu_cleanup_dying_cpu(rsp);
		rcu_cleanup_after_idle(cpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
	/** 20141018    
	 * CPU_DEAD notify 받았을 때 죽은 cpu의 rcu 관련 작업을 정리한다.
	 **/
		for_each_rcu_flavor(rsp)
			rcu_cleanup_dead_cpu(cpu, rsp);
		break;
	default:
		break;
	}
	trace_rcu_utilization("End CPU hotplug");
	return NOTIFY_OK;
}

/*
 * This function is invoked towards the end of the scheduler's initialization
 * process.  Before this is called, the idle task might contain
 * RCU read-side critical sections (during which time, this idle
 * task is booting the system).  After this function is called, the
 * idle tasks are prohibited from containing RCU read-side critical
 * sections.  This function also enables RCU lockdep checking.
 */
void rcu_scheduler_starting(void)
{
	WARN_ON(num_online_cpus() != 1);
	WARN_ON(nr_context_switches() > 0);
	rcu_scheduler_active = 1;
}

/*
 * Compute the per-level fanout, either using the exact fanout specified
 * or balancing the tree, depending on CONFIG_RCU_FANOUT_EXACT.
 */
#ifdef CONFIG_RCU_FANOUT_EXACT
static void __init rcu_init_levelspread(struct rcu_state *rsp)
{
	int i;

	for (i = rcu_num_lvls - 1; i > 0; i--)
		rsp->levelspread[i] = CONFIG_RCU_FANOUT;
	rsp->levelspread[0] = rcu_fanout_leaf;
}
#else /* #ifdef CONFIG_RCU_FANOUT_EXACT */
/** 20140726    
 * CONFIG_RCU_FANOUT_EXACT가 정의되지 않음.
 *
 * 각 레벨에서 node에 할당된 하위 레벨 node의 수를 계산해 넣는다.
 **/
static void __init rcu_init_levelspread(struct rcu_state *rsp)
{
	int ccur;
	int cprv;
	int i;

	cprv = NR_CPUS;
	/** 20140726    
	 * 하위 레벨부터 상위레벨로 올라가며 해당 레벨의 spread 값을 계산한다.
	 *
	 * 하위 레벨 노드의 수와 현재 레벨 노드의 수를 현재 레벨 노드로 나눈다.
	 * 최초 하위 레벨의 spread값은 cpu의 개수를 균등히 분할한 값이다.
	 *
	 * ccur = rsp->levelcnt[0] = 1;
	 * rsp->levelspread[0] = (4 + 1 - 1)/1 = 4;
	 *
	 * cprv : cpu prev, cpu의 개수로 시작, 다음 레벨에서는 하위 레벨 노드의 수.
	 * ccur : cpu current, 현재 레벨의 노드의 수.
	 *
	 * 예) cpu의 개수가 20이라 가정하면 level의 수는 2.
	 *     levelspread[0] = (2  + 1 - 1) / 2 = 1;
	 *     levelspread[1] = (20 + 2 - 1) / 2 = 10;
	 **/
	for (i = rcu_num_lvls - 1; i >= 0; i--) {
		ccur = rsp->levelcnt[i];
		rsp->levelspread[i] = (cprv + ccur - 1) / ccur;
		cprv = ccur;
	}
}
#endif /* #else #ifdef CONFIG_RCU_FANOUT_EXACT */

/*
 * Helper function for rcu_init() that initializes one rcu_state structure.
 */
/** 20140726    
 * rcu_state와 rcu_data를 전달받아 초기화 한다.
 **/
static void __init rcu_init_one(struct rcu_state *rsp,
		struct rcu_data __percpu *rda)
{
	static char *buf[] = { "rcu_node_level_0",
			       "rcu_node_level_1",
			       "rcu_node_level_2",
			       "rcu_node_level_3" };  /* Match MAX_RCU_LVLS */
	int cpustride = 1;
	int i;
	int j;
	struct rcu_node *rnp;

	BUILD_BUG_ON(MAX_RCU_LVLS > ARRAY_SIZE(buf));  /* Fix buf[] init! */

	/* Initialize the level-tracking arrays. */

	/** 20140726    
	 * 각 레벨에 속하는 nodes의 수를 초기화.
	 *
	 * levelcnt[0] = num_rcu_lvl[0] = 1;
	 **/
	for (i = 0; i < rcu_num_lvls; i++)
		rsp->levelcnt[i] = num_rcu_lvl[i];
	/** 20140726    
	 * level은 배열로 이루어진 rcu_node에서 각 레벨에 해당하는
	 * 시작 노드의 주소를 할당한다.
	 **/
	for (i = 1; i < rcu_num_lvls; i++)
		rsp->level[i] = rsp->level[i - 1] + rsp->levelcnt[i - 1];
	rcu_init_levelspread(rsp);

	/* Initialize the elements themselves, starting from the leaves. */

	/** 20140726    
	 * 하위 level에서부터 순회하며 level의 모든 node를 순회하며 초기화 시킨다.
	 **/
	for (i = rcu_num_lvls - 1; i >= 0; i--) {
		/** 20140726    
		 * cpustride = 4;
		 **/
		cpustride *= rsp->levelspread[i];
		rnp = rsp->level[i];
		/** 20140726    
		 * 해당 레벨의 모든 노드를 순회한다.
		 **/
		for (j = 0; j < rsp->levelcnt[i]; j++, rnp++) {
			raw_spin_lock_init(&rnp->lock);
			lockdep_set_class_and_name(&rnp->lock,
						   &rcu_node_class[i], buf[i]);
			rnp->gpnum = 0;
			rnp->qsmask = 0;
			rnp->qsmaskinit = 0;
			rnp->grplo = j * cpustride;
			rnp->grphi = (j + 1) * cpustride - 1;
			if (rnp->grphi >= NR_CPUS)
				rnp->grphi = NR_CPUS - 1;
			if (i == 0) {
				rnp->grpnum = 0;
				rnp->grpmask = 0;
				rnp->parent = NULL;
			} else {
				rnp->grpnum = j % rsp->levelspread[i - 1];
				rnp->grpmask = 1UL << rnp->grpnum;
				rnp->parent = rsp->level[i - 1] +
					      j / rsp->levelspread[i - 1];
			}
			rnp->level = i;
			INIT_LIST_HEAD(&rnp->blkd_tasks);
		}
	}

	/** 20140726    
	 * rcu_state의 data는 전달받은 data.
	 *
	 * rnp은 leaf노드의 시작 위치로 지정하고,
	 * cpu를 순회하며 rcu_data에 percpu data에 해당하는 node 위치를 연결한다.
	 **/
	rsp->rda = rda;
	rnp = rsp->level[rcu_num_lvls - 1];
	for_each_possible_cpu(i) {
		while (i > rnp->grphi)
			rnp++;
		per_cpu_ptr(rsp->rda, i)->mynode = rnp;
		rcu_boot_init_percpu_data(i, rsp);
	}
	/** 20140726    
	 * 초기화된 rcu_state를 rcu_struct_flavors 라는 리스트에 연결시킨다.
	 **/
	list_add(&rsp->flavors, &rcu_struct_flavors);
}

/*
 * Compute the rcu_node tree geometry from kernel parameters.  This cannot
 * replace the definitions in rcutree.h because those are needed to size
 * the ->node array in the rcu_state structure.
 */
/** 20140719
 * rcu_fanout_leaf값이 CONFIG_RCU_FANOUT_LEAF값과 다르다면
 * rcu에 대한 geometry를 갱신시킨다.
 */
static void __init rcu_init_geometry(void)
{
	int i;
	int j;
	int n = nr_cpu_ids;
	int rcu_capacity[MAX_RCU_LVLS + 1];

	/* If the compile-time values are accurate, just leave. */
	if (rcu_fanout_leaf == CONFIG_RCU_FANOUT_LEAF)
		return;

	/*
	 * Compute number of nodes that can be handled an rcu_node tree
	 * with the given number of levels.  Setting rcu_capacity[0] makes
	 * some of the arithmetic easier.
	 */
	rcu_capacity[0] = 1;
	rcu_capacity[1] = rcu_fanout_leaf;
	for (i = 2; i <= MAX_RCU_LVLS; i++)
		rcu_capacity[i] = rcu_capacity[i - 1] * CONFIG_RCU_FANOUT;

	/*
	 * The boot-time rcu_fanout_leaf parameter is only permitted
	 * to increase the leaf-level fanout, not decrease it.  Of course,
	 * the leaf-level fanout cannot exceed the number of bits in
	 * the rcu_node masks.  Finally, the tree must be able to accommodate
	 * the configured number of CPUs.  Complain and fall back to the
	 * compile-time values if these limits are exceeded.
	 */
	if (rcu_fanout_leaf < CONFIG_RCU_FANOUT_LEAF ||
	    rcu_fanout_leaf > sizeof(unsigned long) * 8 ||
	    n > rcu_capacity[MAX_RCU_LVLS]) {
		WARN_ON(1);
		return;
	}

	/* Calculate the number of rcu_nodes at each level of the tree. */
	for (i = 1; i <= MAX_RCU_LVLS; i++)
		if (n <= rcu_capacity[i]) {
			for (j = 0; j <= i; j++)
				num_rcu_lvl[j] =
					DIV_ROUND_UP(n, rcu_capacity[i - j]);
			rcu_num_lvls = i;
			for (j = i + 1; j <= MAX_RCU_LVLS; j++)
				num_rcu_lvl[j] = 0;
			break;
		}

	/* Calculate the total number of rcu_node structures. */
	rcu_num_nodes = 0;
	for (i = 0; i <= MAX_RCU_LVLS; i++)
		rcu_num_nodes += num_rcu_lvl[i];
	rcu_num_nodes -= n;
}

/** 20140823    
 * rcu를 사용하기 위한 초기화.
 *
 * - cpu의 개수대로 geometry를 구성하고,
 * - rcu_sched_state, rcu_bh_state, rcu_preempt_state를 초기화 한다.
 * - RCU_SOFTIRQ를 처리할 softirq 콜백을 등록한다.
 * - cpu notify chain에 rcu_cpu_notify를 등록하고, CPU_UP_PREPARE를 보내
 *   rcu_data 관련 자료구조를 초기화 시킨다.
 **/
void __init rcu_init(void)
{
	int cpu;

	/** 20140719
	 * RCU 옵션에 대한 메세지 표시
	 **/
	rcu_bootup_announce();
	/** 20140719
	 * rcu의 geometry에 대한 설정 초기화
	 **/
	rcu_init_geometry();
	/** 20140719
	 * 기본 rcu_state인 rcu_sched_state, rcu_bh_state 를 초기화 한다.
	 **/	
	rcu_init_one(&rcu_sched_state, &rcu_sched_data);
	rcu_init_one(&rcu_bh_state, &rcu_bh_data);
	/** 20140726    
	 * RCU INIT PREEMPT.
	 **/
	__rcu_init_preempt();
	/** 20140726    
	 * RCU softirq를 vector에 등록시킨다.
	 **/
	 open_softirq(RCU_SOFTIRQ, rcu_process_callbacks);

	/*
	 * We don't need protection against CPU-hotplug here because
	 * this is called early in boot, before either interrupts
	 * or the scheduler are operational.
	 */
	/** 20140823    
	 * cpu_notifier에 새로운 nb rcu_cpu_notify를 가장 낮은 우선순위로 등록.
	 **/
	cpu_notifier(rcu_cpu_notify, 0);
	/** 20140823    
	 * 각 online cpu를 순회하며 rcu_cpu_notify를 CPU_UP_PREPARE로 호출
	 **/
	for_each_online_cpu(cpu)
		rcu_cpu_notify(NULL, CPU_UP_PREPARE, (void *)(long)cpu);
	/** 20140823    
	 * rcu cpu stall 체크관련 초기화.
	 **/
	check_cpu_stall_init();
}

#include "rcutree_plugin.h"
