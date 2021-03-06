/*
 *  linux/kernel/time/tick-sched.c
 *
 *  Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  No idle tick implementation for low and high resolution timers
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Distribute under GPLv2.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/module.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Per cpu nohz control structure
 */
/** 20141108
 * percpu 변수로 tick_sched 구조체 정의.
 **/
static DEFINE_PER_CPU(struct tick_sched, tick_cpu_sched);

/*
 * The time, when the last jiffy update happened. Protected by xtime_lock.
 */
/** 20141108
 * 마지막으로 jiffies가 갱신된 시간을 저장한다.
 **/
static ktime_t last_jiffies_update;

struct tick_sched *tick_get_tick_sched(int cpu)
{
	return &per_cpu(tick_cpu_sched, cpu);
}

/*
 * Must be called with interrupts disabled !
 */
/** 20141101
 * jiffies64 값을 갱신한다.
 *
 * periodic tick, NOHZ에 해당하는 dynticks에 모두 사용할 수 있는 함수.
 **/
static void tick_do_update_jiffies64(ktime_t now)
{
	unsigned long ticks = 0;
	ktime_t delta;

	/*
	 * Do a quick check without holding xtime_lock:
	 */
	/** 20141101
	 * 현재 ktime과 마지막 jiffies에서 업데이트한 값의 차가
	 * 주기보다 작다면 바로 리턴.
	 **/
	delta = ktime_sub(now, last_jiffies_update);
	if (delta.tv64 < tick_period.tv64)
		return;

	/* Reevalute with xtime_lock held */
	/** 20141101
	 * xtime_lock을 갱신하기 위한 lock을 잡는다.
	 **/
	write_seqlock(&xtime_lock);

	/** 20141101
	 * 현재 ktime이 마지막 jiffies에서 업데이트한 ktime의 차가 주기 이상이라면
	 * 아래 동작을 수행한다.
	 **/
	delta = ktime_sub(now, last_jiffies_update);
	if (delta.tv64 >= tick_period.tv64) {

		/** 20141101
		 * 주기 이상의 delta값을 구한다.
		 * last_jiffies_update는 일단 주기만큼만 더해 놓는다.
		 **/
		delta = ktime_sub(delta, tick_period);
		last_jiffies_update = ktime_add(last_jiffies_update,
						tick_period);

		/* Slow path for long timeouts */
		/** 20141101
		 * 주기만큼 빼준 delta가 주기값 이상이라면,
		 * ticks 단위로 증분값을 환산해 last_jiffies_update를 갱신한다.
		 **/
		if (unlikely(delta.tv64 >= tick_period.tv64)) {
			s64 incr = ktime_to_ns(tick_period);

			ticks = ktime_divns(delta, incr);

			last_jiffies_update = ktime_add_ns(last_jiffies_update,
							   incr * ticks);
		}
		/** 20141101
		 * long timeouts와 periodic의 경우 모두 해당하도록 ticks를 더해
		 * do_timer를 호출한다.
		 **/
		do_timer(++ticks);

		/* Keep the tick_next_period variable up to date */
		/** 20141101
		 * 다음 주기가 발생할 시간을 새로 계산해둔다.
		 **/
		tick_next_period = ktime_add(last_jiffies_update, tick_period);
	}
	write_sequnlock(&xtime_lock);
}

/*
 * Initialize and return retrieve the jiffies update.
 */
/** 20141108
 * tick emulation을 하기 위해 jiffies update 값을 리턴한다.
 * jiffies가 아직 update되지 않았다면 다음 period를 더한 값으로 초기화 한다.
 *
 * ktime_get으로 현재 ktime_t 값을 가진다.
 **/
static ktime_t tick_init_jiffy_update(void)
{
	ktime_t period;

	write_seqlock(&xtime_lock);
	/* Did we start the jiffies update yet ? */
	if (last_jiffies_update.tv64 == 0)
		last_jiffies_update = tick_next_period;
	period = last_jiffies_update;
	write_sequnlock(&xtime_lock);
	return period;
}

/*
 * NOHZ - aka dynamic tick functionality
 */
#ifdef CONFIG_NO_HZ
/*
 * NO HZ enabled ?
 */
/** 20141115
 * dynticks인 경우 default로 nohz enabled이다.
 **/
int tick_nohz_enabled __read_mostly  = 1;

/*
 * Enable / Disable tickless mode
 */
static int __init setup_tick_nohz(char *str)
{
	if (!strcmp(str, "off"))
		tick_nohz_enabled = 0;
	else if (!strcmp(str, "on"))
		tick_nohz_enabled = 1;
	else
		return 0;
	return 1;
}

__setup("nohz=", setup_tick_nohz);

/**
 * tick_nohz_update_jiffies - update jiffies when idle was interrupted
 *
 * Called from interrupt entry when the CPU was idle
 *
 * In case the sched_tick was stopped on this CPU, we have to check if jiffies
 * must be updated. Otherwise an interrupt handler could use a stale jiffy
 * value. We do this unconditionally on any cpu, as we don't know whether the
 * cpu, which has the update task assigned is in a long sleep.
 */
static void tick_nohz_update_jiffies(ktime_t now)
{
	int cpu = smp_processor_id();
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	unsigned long flags;

	ts->idle_waketime = now;

	local_irq_save(flags);
	tick_do_update_jiffies64(now);
	local_irq_restore(flags);

	touch_softlockup_watchdog();
}

/*
 * Updates the per cpu time idle statistics counters
 */
static void
update_ts_time_stats(int cpu, struct tick_sched *ts, ktime_t now, u64 *last_update_time)
{
	ktime_t delta;

	if (ts->idle_active) {
		delta = ktime_sub(now, ts->idle_entrytime);
		if (nr_iowait_cpu(cpu) > 0)
			ts->iowait_sleeptime = ktime_add(ts->iowait_sleeptime, delta);
		else
			ts->idle_sleeptime = ktime_add(ts->idle_sleeptime, delta);
		ts->idle_entrytime = now;
	}

	if (last_update_time)
		*last_update_time = ktime_to_us(now);

}

static void tick_nohz_stop_idle(int cpu, ktime_t now)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);

	update_ts_time_stats(cpu, ts, now, NULL);
	ts->idle_active = 0;

	sched_clock_idle_wakeup_event(0);
}

static ktime_t tick_nohz_start_idle(int cpu, struct tick_sched *ts)
{
	ktime_t now = ktime_get();

	ts->idle_entrytime = now;
	ts->idle_active = 1;
	sched_clock_idle_sleep_event();
	return now;
}

/**
 * get_cpu_idle_time_us - get the total idle time of a cpu
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cummulative idle time (since boot) for a given
 * CPU, in microseconds.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * This function returns -1 if NOHZ is not enabled.
 */
u64 get_cpu_idle_time_us(int cpu, u64 *last_update_time)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now, idle;

	if (!tick_nohz_enabled)
		return -1;

	now = ktime_get();
	if (last_update_time) {
		update_ts_time_stats(cpu, ts, now, last_update_time);
		idle = ts->idle_sleeptime;
	} else {
		if (ts->idle_active && !nr_iowait_cpu(cpu)) {
			ktime_t delta = ktime_sub(now, ts->idle_entrytime);

			idle = ktime_add(ts->idle_sleeptime, delta);
		} else {
			idle = ts->idle_sleeptime;
		}
	}

	return ktime_to_us(idle);

}
EXPORT_SYMBOL_GPL(get_cpu_idle_time_us);

/**
 * get_cpu_iowait_time_us - get the total iowait time of a cpu
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cummulative iowait time (since boot) for a given
 * CPU, in microseconds.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * This function returns -1 if NOHZ is not enabled.
 */
u64 get_cpu_iowait_time_us(int cpu, u64 *last_update_time)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now, iowait;

	if (!tick_nohz_enabled)
		return -1;

	now = ktime_get();
	if (last_update_time) {
		update_ts_time_stats(cpu, ts, now, last_update_time);
		iowait = ts->iowait_sleeptime;
	} else {
		if (ts->idle_active && nr_iowait_cpu(cpu) > 0) {
			ktime_t delta = ktime_sub(now, ts->idle_entrytime);

			iowait = ktime_add(ts->iowait_sleeptime, delta);
		} else {
			iowait = ts->iowait_sleeptime;
		}
	}

	return ktime_to_us(iowait);
}
EXPORT_SYMBOL_GPL(get_cpu_iowait_time_us);

static ktime_t tick_nohz_stop_sched_tick(struct tick_sched *ts,
					 ktime_t now, int cpu)
{
	unsigned long seq, last_jiffies, next_jiffies, delta_jiffies;
	ktime_t last_update, expires, ret = { .tv64 = 0 };
	unsigned long rcu_delta_jiffies;
	struct clock_event_device *dev = __get_cpu_var(tick_cpu_device).evtdev;
	u64 time_delta;

	/* Read jiffies and the time when jiffies were updated last */
	/** 20141025
	 * 마지막으로 jiffies가 업데이트 되었을 때의 jiffies값과 시간을 저장한다.
	 **/
	do {
		seq = read_seqbegin(&xtime_lock);
		last_update = last_jiffies_update;
		last_jiffies = jiffies;
		time_delta = timekeeping_max_deferment();
	} while (read_seqretry(&xtime_lock, seq));

	/** 20141018
	 * rcu가 cpu 작업을 필요로 하거나, printk가 pending되어 있거나, 
	 * architecture에서 필요로 하다면 next_jiffies는 다음 jiffies가 된다.
	 **/
	if (rcu_needs_cpu(cpu, &rcu_delta_jiffies) || printk_needs_cpu(cpu) ||
	    arch_needs_cpu(cpu)) {
		next_jiffies = last_jiffies + 1;
		delta_jiffies = 1;
	} else {
	/** 20141025
	 **/
		/* Get the next timer wheel timer */
		next_jiffies = get_next_timer_interrupt(last_jiffies);
		delta_jiffies = next_jiffies - last_jiffies;
		if (rcu_delta_jiffies < delta_jiffies) {
			next_jiffies = last_jiffies + rcu_delta_jiffies;
			delta_jiffies = rcu_delta_jiffies;
		}
	}
	/*
	 * Do not stop the tick, if we are only one off
	 * or if the cpu is required for rcu
	 */
	if (!ts->tick_stopped && delta_jiffies == 1)
		goto out;

	/* Schedule the tick, if we are at least one jiffie off */
	if ((long)delta_jiffies >= 1) {

		/*
		 * If this cpu is the one which updates jiffies, then
		 * give up the assignment and let it be taken by the
		 * cpu which runs the tick timer next, which might be
		 * this cpu as well. If we don't drop this here the
		 * jiffies might be stale and do_timer() never
		 * invoked. Keep track of the fact that it was the one
		 * which had the do_timer() duty last. If this cpu is
		 * the one which had the do_timer() duty last, we
		 * limit the sleep time to the timekeeping
		 * max_deferement value which we retrieved
		 * above. Otherwise we can sleep as long as we want.
		 */
		if (cpu == tick_do_timer_cpu) {
			tick_do_timer_cpu = TICK_DO_TIMER_NONE;
			ts->do_timer_last = 1;
		} else if (tick_do_timer_cpu != TICK_DO_TIMER_NONE) {
			time_delta = KTIME_MAX;
			ts->do_timer_last = 0;
		} else if (!ts->do_timer_last) {
			time_delta = KTIME_MAX;
		}

		/*
		 * calculate the expiry time for the next timer wheel
		 * timer. delta_jiffies >= NEXT_TIMER_MAX_DELTA signals
		 * that there is no timer pending or at least extremely
		 * far into the future (12 days for HZ=1000). In this
		 * case we set the expiry to the end of time.
		 */
		if (likely(delta_jiffies < NEXT_TIMER_MAX_DELTA)) {
			/*
			 * Calculate the time delta for the next timer event.
			 * If the time delta exceeds the maximum time delta
			 * permitted by the current clocksource then adjust
			 * the time delta accordingly to ensure the
			 * clocksource does not wrap.
			 */
			time_delta = min_t(u64, time_delta,
					   tick_period.tv64 * delta_jiffies);
		}

		if (time_delta < KTIME_MAX)
			expires = ktime_add_ns(last_update, time_delta);
		else
			expires.tv64 = KTIME_MAX;

		/* Skip reprogram of event if its not changed */
		if (ts->tick_stopped && ktime_equal(expires, dev->next_event))
			goto out;

		ret = expires;

		/*
		 * nohz_stop_sched_tick can be called several times before
		 * the nohz_restart_sched_tick is called. This happens when
		 * interrupts arrive which do not cause a reschedule. In the
		 * first call we save the current tick time, so we can restart
		 * the scheduler tick in nohz_restart_sched_tick.
		 */
		if (!ts->tick_stopped) {
			select_nohz_load_balancer(1);
			calc_load_enter_idle();

			ts->last_tick = hrtimer_get_expires(&ts->sched_timer);
			ts->tick_stopped = 1;
		}

		/*
		 * If the expiration time == KTIME_MAX, then
		 * in this case we simply stop the tick timer.
		 */
		 if (unlikely(expires.tv64 == KTIME_MAX)) {
			if (ts->nohz_mode == NOHZ_MODE_HIGHRES)
				hrtimer_cancel(&ts->sched_timer);
			goto out;
		}

		if (ts->nohz_mode == NOHZ_MODE_HIGHRES) {
			hrtimer_start(&ts->sched_timer, expires,
				      HRTIMER_MODE_ABS_PINNED);
			/* Check, if the timer was already in the past */
			if (hrtimer_active(&ts->sched_timer))
				goto out;
		} else if (!tick_program_event(expires, 0))
				goto out;
		/*
		 * We are past the event already. So we crossed a
		 * jiffie boundary. Update jiffies and raise the
		 * softirq.
		 */
		tick_do_update_jiffies64(ktime_get());
	}
	raise_softirq_irqoff(TIMER_SOFTIRQ);
out:
	ts->next_jiffies = next_jiffies;
	ts->last_jiffies = last_jiffies;
	ts->sleep_length = ktime_sub(dev->next_event, now);

	return ret;
}

static bool can_stop_idle_tick(int cpu, struct tick_sched *ts)
{
	/*
	 * If this cpu is offline and it is the one which updates
	 * jiffies, then give up the assignment and let it be taken by
	 * the cpu which runs the tick timer next. If we don't drop
	 * this here the jiffies might be stale and do_timer() never
	 * invoked.
	 */
	/** 20160227
	 * cpu가 offline이고, do_timer를 수행(jiffies 업데이트)하는 cpu라면
	 * tick_do_timer_cpu를 비워둔다.
	 * 나중에 nohz handler를 실행하는 cpu가 자신의 cpu번호를 채운다.
	 **/
	if (unlikely(!cpu_online(cpu))) {
		if (cpu == tick_do_timer_cpu)
			tick_do_timer_cpu = TICK_DO_TIMER_NONE;
	}

	if (unlikely(ts->nohz_mode == NOHZ_MODE_INACTIVE))
		return false;

	if (need_resched())
		return false;

	if (unlikely(local_softirq_pending() && cpu_online(cpu))) {
		static int ratelimit;

		if (ratelimit < 10) {
			printk(KERN_ERR "NOHZ: local_softirq_pending %02x\n",
			       (unsigned int) local_softirq_pending());
			ratelimit++;
		}
		return false;
	}

	return true;
}

static void __tick_nohz_idle_enter(struct tick_sched *ts)
{
	ktime_t now, expires;
	int cpu = smp_processor_id();

	now = tick_nohz_start_idle(cpu, ts);

	if (can_stop_idle_tick(cpu, ts)) {
		int was_stopped = ts->tick_stopped;

		ts->idle_calls++;

		/** 20141018
		 **/
		expires = tick_nohz_stop_sched_tick(ts, now, cpu);
		if (expires.tv64 > 0LL) {
			ts->idle_sleeps++;
			ts->idle_expires = expires;
		}

		if (!was_stopped && ts->tick_stopped)
			ts->idle_jiffies = ts->last_jiffies;
	}
}

/**
 * tick_nohz_idle_enter - stop the idle tick from the idle task
 *
 * When the next event is more than a tick into the future, stop the idle tick
 * Called when we start the idle loop.
 *
 * The arch is responsible of calling:
 *
 * - rcu_idle_enter() after its last use of RCU before the CPU is put
 *  to sleep.
 * - rcu_idle_exit() before the first use of RCU after the CPU is woken up.
 */
/** 20160220
 * 자세한 내용은 추후분석???
 * 
 * NOHZ로 config 변경 후 빌드해 ddd로 분석.
 * boot cpu인 경우 nohz로 idle_tick을 날릴지 않는지 확인해야 한다.
 * 
 * tick_nohz_idle_enter() ~ tick_nohz_idle_exit() 사이에 idle tick이 발생하는
 * 것을 막아 low power mode에서 불필요하게 깨어나지 않도록 한다.
 * [참고] StatusOfLinuxDynaticks.pdf
 **/
void tick_nohz_idle_enter(void)
{
	struct tick_sched *ts;

	WARN_ON_ONCE(irqs_disabled());

	/*
 	 * Update the idle state in the scheduler domain hierarchy
 	 * when tick_nohz_stop_sched_tick() is called from the idle loop.
 	 * State will be updated to busy during the first busy tick after
 	 * exiting idle.
 	 */
	set_cpu_sd_state_idle();

	local_irq_disable();

	ts = &__get_cpu_var(tick_cpu_sched);
	/*
	 * set ts->inidle unconditionally. even if the system did not
	 * switch to nohz mode the cpu frequency governers rely on the
	 * update of the idle time accounting in tick_nohz_start_idle().
	 */
	ts->inidle = 1;
	/** 20141018
	 **/
	__tick_nohz_idle_enter(ts);

	local_irq_enable();
}

/**
 * tick_nohz_irq_exit - update next tick event from interrupt exit
 *
 * When an interrupt fires while we are idle and it doesn't cause
 * a reschedule, it may still add, modify or delete a timer, enqueue
 * an RCU callback, etc...
 * So we need to re-calculate and reprogram the next tick event.
 */
void tick_nohz_irq_exit(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	if (!ts->inidle)
		return;

	__tick_nohz_idle_enter(ts);
}

/**
 * tick_nohz_get_sleep_length - return the length of the current sleep
 *
 * Called from power state control code with interrupts disabled
 */
ktime_t tick_nohz_get_sleep_length(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	return ts->sleep_length;
}

static void tick_nohz_restart(struct tick_sched *ts, ktime_t now)
{
	hrtimer_cancel(&ts->sched_timer);
	hrtimer_set_expires(&ts->sched_timer, ts->last_tick);

	while (1) {
		/* Forward the time to expire in the future */
		hrtimer_forward(&ts->sched_timer, now, tick_period);

		if (ts->nohz_mode == NOHZ_MODE_HIGHRES) {
			hrtimer_start_expires(&ts->sched_timer,
					      HRTIMER_MODE_ABS_PINNED);
			/* Check, if the timer was already in the past */
			if (hrtimer_active(&ts->sched_timer))
				break;
		} else {
			if (!tick_program_event(
				hrtimer_get_expires(&ts->sched_timer), 0))
				break;
		}
		/* Reread time and update jiffies */
		now = ktime_get();
		tick_do_update_jiffies64(now);
	}
}

static void tick_nohz_restart_sched_tick(struct tick_sched *ts, ktime_t now)
{
	/* Update jiffies first */
	select_nohz_load_balancer(0);
	tick_do_update_jiffies64(now);
	update_cpu_load_nohz();

	touch_softlockup_watchdog();
	/*
	 * Cancel the scheduled timer and restore the tick
	 */
	ts->tick_stopped  = 0;
	ts->idle_exittime = now;

	tick_nohz_restart(ts, now);
}

static void tick_nohz_account_idle_ticks(struct tick_sched *ts)
{
#ifndef CONFIG_VIRT_CPU_ACCOUNTING
	unsigned long ticks;
	/*
	 * We stopped the tick in idle. Update process times would miss the
	 * time we slept as update_process_times does only a 1 tick
	 * accounting. Enforce that this is accounted to idle !
	 */
	ticks = jiffies - ts->idle_jiffies;
	/*
	 * We might be one off. Do not randomly account a huge number of ticks!
	 */
	if (ticks && ticks < LONG_MAX)
		account_idle_ticks(ticks);
#endif
}

/**
 * tick_nohz_idle_exit - restart the idle tick from the idle task
 *
 * Restart the idle tick when the CPU is woken up from idle
 * This also exit the RCU extended quiescent state. The CPU
 * can use RCU again after this function is called.
 */
/** 20160227
 * tick_nohz_idle_enter와 함께 추후분석???
 **/
void tick_nohz_idle_exit(void)
{
	int cpu = smp_processor_id();
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now;

	local_irq_disable();

	WARN_ON_ONCE(!ts->inidle);

	ts->inidle = 0;

	if (ts->idle_active || ts->tick_stopped)
		now = ktime_get();

	if (ts->idle_active)
		tick_nohz_stop_idle(cpu, now);

	if (ts->tick_stopped) {
		tick_nohz_restart_sched_tick(ts, now);
		tick_nohz_account_idle_ticks(ts);
	}

	local_irq_enable();
}

static int tick_nohz_reprogram(struct tick_sched *ts, ktime_t now)
{
	hrtimer_forward(&ts->sched_timer, now, tick_period);
	return tick_program_event(hrtimer_get_expires(&ts->sched_timer), 0);
}

/*
 * The nohz low res interrupt handler
 */
/** 20141206
 * nohz timer interrupt handler for low res.
 *
 * high res를 위한 handler는 hrtimer_interrupt이다.
 * 두 핸들러 함수는 추후분석???
 **/
static void tick_nohz_handler(struct clock_event_device *dev)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	struct pt_regs *regs = get_irq_regs();
	int cpu = smp_processor_id();
	ktime_t now = ktime_get();

	dev->next_event.tv64 = KTIME_MAX;

	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the cpu in charge went
	 * into a long sleep. If two cpus happen to assign themself to
	 * this duty, then the jiffies update is still serialized by
	 * xtime_lock.
	 */
	/** 20160227
	 * can_stop_idle_tick 에서 do_timer를 수행하던 cpu가 offline이 되었다면
	 * nohz_handler 함수가 자신의 cpu 번호를 넣는다.
	 * 두 cpu 이상에서 동시에 이 부분이 실행된다고 해도, jiffies update는
	 * xtime_lock에 의해 serialize 되므로 여기서 lock을 사용하지 않았다.
	 **/
	if (unlikely(tick_do_timer_cpu == TICK_DO_TIMER_NONE))
		tick_do_timer_cpu = cpu;

	/* Check, if the jiffies need an update */
	if (tick_do_timer_cpu == cpu)
		tick_do_update_jiffies64(now);

	/*
	 * When we are idle and the tick is stopped, we have to touch
	 * the watchdog as we might not schedule for a really long
	 * time. This happens on complete idle SMP systems while
	 * waiting on the login prompt. We also increment the "start
	 * of idle" jiffy stamp so the idle accounting adjustment we
	 * do when we go busy again does not account too much ticks.
	 */
	if (ts->tick_stopped) {
		touch_softlockup_watchdog();
		ts->idle_jiffies++;
	}

	update_process_times(user_mode(regs));
	profile_tick(CPU_PROFILING);

	while (tick_nohz_reprogram(ts, now)) {
		now = ktime_get();
		tick_do_update_jiffies64(now);
	}
}

/**
 * tick_nohz_switch_to_nohz - switch to nohz mode
 */
/** 20141206
 * tick NOHZ로 설정되어 있을 때, sched_timer를 no_hz로 동작시킨다.
 *
 * oneshot notify가 왔을 때 hres config가 되어 있지 않을 때 실행된다.
 **/
static void tick_nohz_switch_to_nohz(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	ktime_t next;

	/** 20141206
	 * CONFIG_NO_HZ인 경우 tick_nohz_enabled의 default가 1이므로 if는 거짓이 된다.
	 **/
	if (!tick_nohz_enabled)
		return;

	local_irq_disable();
	/** 20141206
	 * tick을 oneshot mode로 동작시킨다.
	 * event handler함수를 tick_nohz_handler로 지정한다.
	 *
	 * tick_init_highres에서는 hrtimer_interrupt를 event_handler로 지정한다.
	 **/
	if (tick_switch_to_oneshot(tick_nohz_handler)) {
		local_irq_enable();
		return;
	}

	/** 20141206
	 * hres는 설정되어 있지 않아 tick_sched의 nohz_mode는 NOHZ_MODE_LOWRES이다.
	 **/
	ts->nohz_mode = NOHZ_MODE_LOWRES;

	/*
	 * Recycle the hrtimer in ts, so we can share the
	 * hrtimer_forward with the highres code.
	 */
	/** 20141206
	 * sched tick으로 사용할 hrtimer를 초기화 한다.
	 **/
	hrtimer_init(&ts->sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	/* Get the next period */
	/** 20141206
	 * nohz로 동작시킬 때 아직 jiffy가 update되지 않았다면,
	 * next period 값이 리턴될 것이다.
	 **/
	next = tick_init_jiffy_update();

	/** 20141206
	 * sched_timer의 expire 시간을 next로 설정하고, program 시킨다.
	 * program이 실패하면, next는 tick_period만큼 증가되어 다시 시도한다.
	 **/
	for (;;) {
		hrtimer_set_expires(&ts->sched_timer, next);
		if (!tick_program_event(next, 0))
			break;
		next = ktime_add(next, tick_period);
	}
	local_irq_enable();
}

/*
 * When NOHZ is enabled and the tick is stopped, we need to kick the
 * tick timer from irq_enter() so that the jiffies update is kept
 * alive during long running softirqs. That's ugly as hell, but
 * correctness is key even if we need to fix the offending softirq in
 * the first place.
 *
 * Note, this is different to tick_nohz_restart. We just kick the
 * timer and do not touch the other magic bits which need to be done
 * when idle is left.
 */
static void tick_nohz_kick_tick(int cpu, ktime_t now)
{
#if 0
	/* Switch back to 2.6.27 behaviour */

	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t delta;

	/*
	 * Do not touch the tick device, when the next expiry is either
	 * already reached or less/equal than the tick period.
	 */
	delta =	ktime_sub(hrtimer_get_expires(&ts->sched_timer), now);
	if (delta.tv64 <= tick_period.tv64)
		return;

	tick_nohz_restart(ts, now);
#endif
}

static inline void tick_check_nohz(int cpu)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now;

	if (!ts->idle_active && !ts->tick_stopped)
		return;
	now = ktime_get();
	if (ts->idle_active)
		tick_nohz_stop_idle(cpu, now);
	if (ts->tick_stopped) {
		tick_nohz_update_jiffies(now);
		tick_nohz_kick_tick(cpu, now);
	}
}

#else

static inline void tick_nohz_switch_to_nohz(void) { }
static inline void tick_check_nohz(int cpu) { }

#endif /* NO_HZ */

/*
 * Called from irq_enter to notify about the possible interruption of idle()
 */
void tick_check_idle(int cpu)
{
	tick_check_oneshot_broadcast(cpu);
	tick_check_nohz(cpu);
}

/*
 * High resolution timer specific code
 */
#ifdef CONFIG_HIGH_RES_TIMERS
/*
 * We rearm the timer until we get disabled by the idle code.
 * Called with interrupts disabled and timer->base->cpu_base->lock held.
 */
static enum hrtimer_restart tick_sched_timer(struct hrtimer *timer)
{
	struct tick_sched *ts =
		container_of(timer, struct tick_sched, sched_timer);
	struct pt_regs *regs = get_irq_regs();
	ktime_t now = ktime_get();
	int cpu = smp_processor_id();

#ifdef CONFIG_NO_HZ
	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the cpu in charge went
	 * into a long sleep. If two cpus happen to assign themself to
	 * this duty, then the jiffies update is still serialized by
	 * xtime_lock.
	 */
	if (unlikely(tick_do_timer_cpu == TICK_DO_TIMER_NONE))
		tick_do_timer_cpu = cpu;
#endif

	/* Check, if the jiffies need an update */
	/** 20141115
	 * 현재 cpu가 do_timer를 동작시키는 cpu인 경우,
	 *   jiffies64를 update 시킨다.
	 **/
	if (tick_do_timer_cpu == cpu)
		tick_do_update_jiffies64(now);

	/*
	 * Do not call, when we are not in irq context and have
	 * no valid regs pointer
	 */
	if (regs) {
		/*
		 * When we are idle and the tick is stopped, we have to touch
		 * the watchdog as we might not schedule for a really long
		 * time. This happens on complete idle SMP systems while
		 * waiting on the login prompt. We also increment the "start of
		 * idle" jiffy stamp so the idle accounting adjustment we do
		 * when we go busy again does not account too much ticks.
		 */
		if (ts->tick_stopped) {
			touch_softlockup_watchdog();
			if (idle_cpu(cpu))
				ts->idle_jiffies++;
		}
		update_process_times(user_mode(regs));
		profile_tick(CPU_PROFILING);
	}

	hrtimer_forward(timer, now, tick_period);

	return HRTIMER_RESTART;
}

static int sched_skew_tick;

/** 20141108
 * early param을 받아 sched_skew_tick 값으로 설정한다.
 **/
static int __init skew_tick(char *str)
{
	get_option(&str, &sched_skew_tick);

	return 0;
}
early_param("skew_tick", skew_tick);

/**
 * tick_setup_sched_timer - setup the tick emulation timer
 */
/** 20141115
 * tick emulation layer를 설정한다.
 * - percpu로 동작하는 hrtimer를 하나 설정한다. CB function은 tick_sched_timer.
 **/
void tick_setup_sched_timer(void)
{
	/** 20141108
	 * 현재 cpu에 해당하는 tick_sched 구조체 정보를 가져온다.
	 **/
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	ktime_t now = ktime_get();

	/*
	 * Emulate tick processing via per-CPU hrtimers:
	 */
	/** 20141108
	 * sched tick으로 사용할 hrtimer를 초기화 한다.
	 * clock은 CLOCK_MONOTONIC으로 단조 증가하고,
	 * mode는 절대값이다.
	 *
	 * timer 만료시 호출될 function은 tick_sched_timer.
	 *
	 **/
	hrtimer_init(&ts->sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	ts->sched_timer.function = tick_sched_timer;

	/* Get the next period (per cpu) */
	/** 20141108
	 * sched_timer의 exprires를 jiffies update 시점,
	 * 즉 tick이 마지막으로 발생한, 현재 tick으로 초기화한다.
	 **/
	hrtimer_set_expires(&ts->sched_timer, tick_init_jiffy_update());

	/* Offset the tick to avert xtime_lock contention. */
	/** 20141115
	 * sched 보정치가 주어져 있다면 expires를 보정한다.
	 **/
	if (sched_skew_tick) {
		u64 offset = ktime_to_ns(tick_period) >> 1;
		do_div(offset, num_possible_cpus());
		offset *= smp_processor_id();
		hrtimer_add_expires_ns(&ts->sched_timer, offset);
	}

	for (;;) {
		/** 20141108
		 * sched_timer의 expires를 현재값에 tick_period를 더한 값으로 설정한다.
		 **/
		hrtimer_forward(&ts->sched_timer, now, tick_period);
		/** 20141129
		 * 설정한 sched_timer를 현재 CPU에서 절대시간으로 만료되도록 등록시킨다.
		 **/
		hrtimer_start_expires(&ts->sched_timer,
				      HRTIMER_MODE_ABS_PINNED);
		/* Check, if the timer was already in the past */
		/** 20141129
		 * sched_timer가 active 되었다면 벗어나고,
		 * 그렇지 않다면 새로 시간을 받아와 다음 주기에 동작하도록 등록한다.
		 **/
		if (hrtimer_active(&ts->sched_timer))
			break;
		now = ktime_get();
	}

#ifdef CONFIG_NO_HZ
	/** 20141129
	 * dynticks 등 nohz 활성상태인 경우,
	 * 이제 nohz는 HIGHRES로 동작한다.
	 **/
	if (tick_nohz_enabled)
		ts->nohz_mode = NOHZ_MODE_HIGHRES;
#endif
}
#endif /* HIGH_RES_TIMERS */

#if defined CONFIG_NO_HZ || defined CONFIG_HIGH_RES_TIMERS
void tick_cancel_sched_timer(int cpu)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);

# ifdef CONFIG_HIGH_RES_TIMERS
	if (ts->sched_timer.base)
		hrtimer_cancel(&ts->sched_timer);
# endif

	ts->nohz_mode = NOHZ_MODE_INACTIVE;
}
#endif

/**
 * Async notification about clocksource changes
 */
void tick_clock_notify(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		set_bit(0, &per_cpu(tick_cpu_sched, cpu).check_clocks);
}

/*
 * Async notification about clock event changes
 */
/** 20141115
 * tick_sched의 check_clocks의 0번 비트를 설정해 비동기적으로
 * clock event 변경을 통지한다.
 *
 * check 시점은 tick_check_oneshot_change.
 **/
void tick_oneshot_notify(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	set_bit(0, &ts->check_clocks);
}

/**
 * Check, if a change happened, which makes oneshot possible.
 *
 * Called cyclic from the hrtimer softirq (driven by the timer
 * softirq) allow_nohz signals, that we can switch into low-res nohz
 * mode, because high resolution timers are disabled (either compile
 * or runtime).
 */
/** 20141115
 * oneshot change가 필요한지 검사한다.
 **/
int tick_check_oneshot_change(int allow_nohz)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	/** 20141115
	 * tick_oneshot_notify()이 호출되면 clock event가 변경되고,
	 * test가 참이 되어 리턴되지 않고 아래가 실행된다.
	 **/
	if (!test_and_clear_bit(0, &ts->check_clocks))
		return 0;

	/** 20141115
	 * 현재 nohz_mode가 INACTIVE가 아니면 NOHZ로 동작 중이다.
	 **/
	if (ts->nohz_mode != NOHZ_MODE_INACTIVE)
		return 0;

	if (!timekeeping_valid_for_hres() || !tick_is_oneshot_available())
		return 0;

	/** 20141115
	 * !hrtimer_is_hres_enabled()가 allow_nohz로 넘어온다.
	 * 따라서 hres enabled라면 1이 리턴된다.
	 **/
	if (!allow_nohz)
		return 1;

	/** 20141115
	 * CONFIG_HIGH_RES_TIMERS이 설정되어 있다면
	 * hrtimer_run_pending에서 allow_nohz는 false이므로 위에서 리턴된다.
	 * 그렇지 않다면 nohz low res로 등작한다.
	 **/
	tick_nohz_switch_to_nohz();
	return 0;
}
