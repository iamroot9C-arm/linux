/*
 *  linux/kernel/hrtimer.c
 *
 *  Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  High-resolution kernel timers
 *
 *  In contrast to the low-resolution timeout API implemented in
 *  kernel/timer.c, hrtimers provide finer resolution and accuracy
 *  depending on system configuration and capabilities.
 *
 *  These timers are currently used for:
 *   - itimers
 *   - POSIX timers
 *   - nanosleep
 *   - precise in-kernel timing
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Credits:
 *	based on kernel/timer.c
 *
 *	Help, testing, suggestions, bugfixes, improvements were
 *	provided by:
 *
 *	George Anzinger, Andrew Morton, Steven Rostedt, Roman Zippel
 *	et. al.
 *
 *  For licencing details see kernel-base/COPYING
 */

#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/notifier.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/debugobjects.h>
#include <linux/sched.h>
#include <linux/timer.h>

#include <asm/uaccess.h>

#include <trace/events/timer.h>

/*
 * The timer bases:
 *
 * There are more clockids then hrtimer bases. Thus, we index
 * into the timer bases by the hrtimer_base_type enum. When trying
 * to reach a base using a clockid, hrtimer_clockid_to_base()
 * is used to convert from clockid to the proper hrtimer_base_type.
 */
/** 20141108
 **/
DEFINE_PER_CPU(struct hrtimer_cpu_base, hrtimer_bases) =
{

	/** 20141108
	 * struct hrtimer_clock_base
	 * clock base
	 *	.resolution은 KTIME_LOW_RES로 동작한다.
	 * hrtimer_switch_to_hres에서 hres로 동작시키며 변경시킨다.
	 **/
	.clock_base =
	{
		{
			.index = HRTIMER_BASE_MONOTONIC,
			.clockid = CLOCK_MONOTONIC,
			.get_time = &ktime_get,
			.resolution = KTIME_LOW_RES,
		},
		{
			.index = HRTIMER_BASE_REALTIME,
			.clockid = CLOCK_REALTIME,
			.get_time = &ktime_get_real,
			.resolution = KTIME_LOW_RES,
		},
		{
			.index = HRTIMER_BASE_BOOTTIME,
			.clockid = CLOCK_BOOTTIME,
			.get_time = &ktime_get_boottime,
			.resolution = KTIME_LOW_RES,
		},
	}
};

static const int hrtimer_clock_to_base_table[MAX_CLOCKS] = {
	[CLOCK_REALTIME]	= HRTIMER_BASE_REALTIME,
	[CLOCK_MONOTONIC]	= HRTIMER_BASE_MONOTONIC,
	[CLOCK_BOOTTIME]	= HRTIMER_BASE_BOOTTIME,
};

/** 20141108
 * clock_id로 하여 clock base의 index를 가져온다.
 **/
static inline int hrtimer_clockid_to_base(clockid_t clock_id)
{
	return hrtimer_clock_to_base_table[clock_id];
}


/*
 * Get the coarse grained time at the softirq based on xtime and
 * wall_to_monotonic.
 */
static void hrtimer_get_softirq_time(struct hrtimer_cpu_base *base)
{
	ktime_t xtim, mono, boot;
	struct timespec xts, tom, slp;

	get_xtime_and_monotonic_and_sleep_offset(&xts, &tom, &slp);

	xtim = timespec_to_ktime(xts);
	mono = ktime_add(xtim, timespec_to_ktime(tom));
	boot = ktime_add(mono, timespec_to_ktime(slp));
	base->clock_base[HRTIMER_BASE_REALTIME].softirq_time = xtim;
	base->clock_base[HRTIMER_BASE_MONOTONIC].softirq_time = mono;
	base->clock_base[HRTIMER_BASE_BOOTTIME].softirq_time = boot;
}

/*
 * Functions and macros which are different for UP/SMP systems are kept in a
 * single place
 */
#ifdef CONFIG_SMP

/*
 * We are using hashed locking: holding per_cpu(hrtimer_bases)[n].lock
 * means that all timers which are tied to this base via timer->base are
 * locked, and the base itself is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found on the lists/queues.
 *
 * When the timer's base is locked, and the timer removed from list, it is
 * possible to set timer->base = NULL and drop the lock: the timer remains
 * locked.
 */
/** 20141108
 * hrtimer의 hrtimer_cpu_base에 lock을 건다.
 *
 * lock변수 위치는 hrtimer_cpu_base 내에 둔다.
 * 단, lock의 위치가 hrtimer_clock_base에 있지 않으므로 spinlock 전후에
 * timer->base가 변경되지 않았음을 확인해야 한다.
 *
 * __run_timers나 migrate_timers에서 리스트나 큐에 존재하는 모든 타이머를
 * 안전하게 수정할 수 있다.
 *
 * timer의 base가 lock이 걸린 상태에서 timer가 list로부터 제거되면,
 * timer의 base를 NULL로 설정함으로써 lock을 버릴 수 있다. timer 자체는 lock을 건 상태로.
 **/
static
struct hrtimer_clock_base *lock_hrtimer_base(const struct hrtimer *timer,
					     unsigned long *flags)
{
	struct hrtimer_clock_base *base;

	for (;;) {
		base = timer->base;
		if (likely(base != NULL)) {
			raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);
			if (likely(base == timer->base))
				return base;
			/* The timer has migrated to another CPU: */
			raw_spin_unlock_irqrestore(&base->cpu_base->lock, *flags);
		}
		cpu_relax();
	}
}


/*
 * Get the preferred target CPU for NOHZ
 */
/** 20141108
 * hrtimer를 수행할 target을 가져온다.
 *
 * NOHZ에서 현재 cpu가 idle_cpu라면 timer를 수행할 target cpu를 가져온다.
 * 그렇지 않을 경우 this_cpu를 리턴한다.
 *
 * 따라서 pinned되어 있을 경우 항상 현재 cpu가 반환된다.
 **/
static int hrtimer_get_target(int this_cpu, int pinned)
{
#ifdef CONFIG_NO_HZ
	if (!pinned && get_sysctl_timer_migration() && idle_cpu(this_cpu))
		return get_nohz_timer_target();
#endif
	return this_cpu;
}

/*
 * With HIGHRES=y we do not migrate the timer when it is expiring
 * before the next event on the target cpu because we cannot reprogram
 * the target cpu hardware and we would cause it to fire late.
 *
 * Called with cpu_base->lock of target cpu held.
 */
/** 20141129
 * timer를 migrate할 때, target이 되는 cpu의 다음 만료시간보다
 * migration 할 timer의 만료시간이 짧을 때는 migrate하지 못한다.
 **/
static int
hrtimer_check_target(struct hrtimer *timer, struct hrtimer_clock_base *new_base)
{
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t expires;

	if (!new_base->cpu_base->hres_active)
		return 0;

	expires = ktime_sub(hrtimer_get_expires(timer), new_base->offset);
	return expires.tv64 <= new_base->cpu_base->expires_next.tv64;
#else
	return 0;
#endif
}

/*
 * Switch the timer base to the current CPU when possible.
 */
/** 20141108   
 * hrtimer의 base를 현재 cpu(pinned되어 있지 않은 상태에서 NO_HZ라면 다른 cpu)의
 * hrtimer_clock_base로 설정한다.
 **/
static inline struct hrtimer_clock_base *
switch_hrtimer_base(struct hrtimer *timer, struct hrtimer_clock_base *base,
		    int pinned)
{
	struct hrtimer_clock_base *new_base;
	struct hrtimer_cpu_base *new_cpu_base;
	int this_cpu = smp_processor_id();
	/** 20141115
	 * pinned로 넘어온 경우, hrtimer를 수행할 함수는 this_cpu가 된다.
	 **/
	int cpu = hrtimer_get_target(this_cpu, pinned);
	int basenum = base->index;

again:
	new_cpu_base = &per_cpu(hrtimer_bases, cpu);
	new_base = &new_cpu_base->clock_base[basenum];

	/** 20141115
	 * 기존 base와 new_base가 다르다면 new_base를 timer의 base로 설정한다.
	 **/
	if (base != new_base) {
		/*
		 * We are trying to move timer to new_base.
		 * However we can't change timer's base while it is running,
		 * so we keep it on the same CPU. No hassle vs. reprogramming
		 * the event source in the high resolution case. The softirq
		 * code will take care of this when the timer function has
		 * completed. There is no conflict as we hold the lock until
		 * the timer is enqueued.
		 */
		if (unlikely(hrtimer_callback_running(timer)))
			return base;

		/* See the comment in lock_timer_base() */
		timer->base = NULL;
		raw_spin_unlock(&base->cpu_base->lock);
		raw_spin_lock(&new_base->cpu_base->lock);

		/** 20141129
		 * timer migrate할 cpu가 현재 cpu가 아닌데, expires 때문에 migrate시키지
		 * 못한다면, 현재 timer base에 다시 등록시키고 다시 시도한다.
		 **/
		if (cpu != this_cpu && hrtimer_check_target(timer, new_base)) {
			cpu = this_cpu;
			raw_spin_unlock(&new_base->cpu_base->lock);
			raw_spin_lock(&base->cpu_base->lock);
			timer->base = base;
			goto again;
		}
		timer->base = new_base;
	}
	/** 20141115
	 * new_base를 넘긴다.
	 * 만약 base와 new_base가 같다면 별다른 동작없이 현재 clock_base가 리턴된다.
	 **/
	return new_base;
}

#else /* CONFIG_SMP */

static inline struct hrtimer_clock_base *
lock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	struct hrtimer_clock_base *base = timer->base;

	raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);

	return base;
}

# define switch_hrtimer_base(t, b, p)	(b)

#endif	/* !CONFIG_SMP */

/*
 * Functions for the union type storage format of ktime_t which are
 * too large for inlining:
 */
#if BITS_PER_LONG < 64
# ifndef CONFIG_KTIME_SCALAR
/**
 * ktime_add_ns - Add a scalar nanoseconds value to a ktime_t variable
 * @kt:		addend
 * @nsec:	the scalar nsec value to add
 *
 * Returns the sum of kt and nsec in ktime_t format
 */
ktime_t ktime_add_ns(const ktime_t kt, u64 nsec)
{
	ktime_t tmp;

	if (likely(nsec < NSEC_PER_SEC)) {
		tmp.tv64 = nsec;
	} else {
		unsigned long rem = do_div(nsec, NSEC_PER_SEC);

		tmp = ktime_set((long)nsec, rem);
	}

	return ktime_add(kt, tmp);
}

EXPORT_SYMBOL_GPL(ktime_add_ns);

/**
 * ktime_sub_ns - Subtract a scalar nanoseconds value from a ktime_t variable
 * @kt:		minuend
 * @nsec:	the scalar nsec value to subtract
 *
 * Returns the subtraction of @nsec from @kt in ktime_t format
 */
ktime_t ktime_sub_ns(const ktime_t kt, u64 nsec)
{
	ktime_t tmp;

	if (likely(nsec < NSEC_PER_SEC)) {
		tmp.tv64 = nsec;
	} else {
		unsigned long rem = do_div(nsec, NSEC_PER_SEC);

		tmp = ktime_set((long)nsec, rem);
	}

	return ktime_sub(kt, tmp);
}

EXPORT_SYMBOL_GPL(ktime_sub_ns);
# endif /* !CONFIG_KTIME_SCALAR */

/*
 * Divide a ktime value by a nanosecond value
 */
/** 20140419
 * ktime을 ns로 나눠 그 몫을 리턴한다.
 **/
u64 ktime_divns(const ktime_t kt, s64 div)
{
	u64 dclc;
	int sft = 0;

	/** 20140419
	 * ktime을 ns로 변환.
	 **/
	dclc = ktime_to_ns(kt);
	/* Make sure the divisor is less than 2^32: */
	/** 20140419
	 * unsigned long으로 나누기 위해 sft 값을 구한다.
	 **/
	while (div >> 32) {
		sft++;
		div >>= 1;
	}
	/** 20140419
	 * 나누는 수를 shift 시킨만큼 나눠지는 수도 shift
	 **/
	dclc >>= sft;
	/** 20140419
	 * unsigned long으로 나눈다.
	 **/
	do_div(dclc, (unsigned long) div);

	return dclc;
}
#endif /* BITS_PER_LONG >= 64 */

/*
 * Add two ktime values and do a safety check for overflow:
 */
/** 20140419
 * ktime 값을 더한다.
 * overflow 된다면 SEC MAX로 지정한다.
 **/
ktime_t ktime_add_safe(const ktime_t lhs, const ktime_t rhs)
{
	ktime_t res = ktime_add(lhs, rhs);

	/*
	 * We use KTIME_SEC_MAX here, the maximum timeout which we can
	 * return to user space in a timespec:
	 */
	/** 20140419
	 * overflow 되었다면 ktime을 SEC MAX로 지정한다.
	 **/
	if (res.tv64 < 0 || res.tv64 < lhs.tv64 || res.tv64 < rhs.tv64)
		res = ktime_set(KTIME_SEC_MAX, 0);

	return res;
}

EXPORT_SYMBOL_GPL(ktime_add_safe);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static struct debug_obj_descr hrtimer_debug_descr;

static void *hrtimer_debug_hint(void *addr)
{
	return ((struct hrtimer *) addr)->function;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int hrtimer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_init(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
static int hrtimer_fixup_activate(void *addr, enum debug_obj_state state)
{
	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		WARN_ON_ONCE(1);
		return 0;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);

	default:
		return 0;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static int hrtimer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_free(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

static struct debug_obj_descr hrtimer_debug_descr = {
	.name		= "hrtimer",
	.debug_hint	= hrtimer_debug_hint,
	.fixup_init	= hrtimer_fixup_init,
	.fixup_activate	= hrtimer_fixup_activate,
	.fixup_free	= hrtimer_fixup_free,
};

static inline void debug_hrtimer_init(struct hrtimer *timer)
{
	debug_object_init(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_activate(struct hrtimer *timer)
{
	debug_object_activate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_deactivate(struct hrtimer *timer)
{
	debug_object_deactivate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_free(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode);

void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	debug_object_init_on_stack(timer, &hrtimer_debug_descr);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init_on_stack);

void destroy_hrtimer_on_stack(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

#else
static inline void debug_hrtimer_init(struct hrtimer *timer) { }
static inline void debug_hrtimer_activate(struct hrtimer *timer) { }
static inline void debug_hrtimer_deactivate(struct hrtimer *timer) { }
#endif

static inline void
debug_init(struct hrtimer *timer, clockid_t clockid,
	   enum hrtimer_mode mode)
{
	debug_hrtimer_init(timer);
	trace_hrtimer_init(timer, clockid, mode);
}

static inline void debug_activate(struct hrtimer *timer)
{
	debug_hrtimer_activate(timer);
	trace_hrtimer_start(timer);
}

static inline void debug_deactivate(struct hrtimer *timer)
{
	debug_hrtimer_deactivate(timer);
	trace_hrtimer_cancel(timer);
}

/* High resolution timer related functions */
#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer enabled ?
 */
/** 20141101
 * HIGH_RES_TIMERS가 사용될 경우 default 값은 1.
 **/
static int hrtimer_hres_enabled __read_mostly  = 1;

/*
 * Enable / Disable high resolution mode
 */
static int __init setup_hrtimer_hres(char *str)
{
	if (!strcmp(str, "off"))
		hrtimer_hres_enabled = 0;
	else if (!strcmp(str, "on"))
		hrtimer_hres_enabled = 1;
	else
		return 0;
	return 1;
}

__setup("highres=", setup_hrtimer_hres);

/*
 * hrtimer_high_res_enabled - query, if the highres mode is enabled
 */
/** 20141101
 * hrtimer가 high-res로 설정되어 있는지 리턴.
 **/
static inline int hrtimer_is_hres_enabled(void)
{
	return hrtimer_hres_enabled;
}

/*
 * Is the high resolution mode active ?
 */
/** 20141101
 * 현재 cpu에서 high-res timer가 동작 중인지 리턴한다.
 **/
static inline int hrtimer_hres_active(void)
{
	return __this_cpu_read(hrtimer_bases.hres_active);
}

/*
 * Reprogram the event source with checking both queues for the
 * next event
 * Called with interrupts disabled and base->lock held
 */
static void
hrtimer_force_reprogram(struct hrtimer_cpu_base *cpu_base, int skip_equal)
{
	int i;
	struct hrtimer_clock_base *base = cpu_base->clock_base;
	ktime_t expires, expires_next;

	expires_next.tv64 = KTIME_MAX;

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++, base++) {
		struct hrtimer *timer;
		struct timerqueue_node *next;

		next = timerqueue_getnext(&base->active);
		if (!next)
			continue;
		timer = container_of(next, struct hrtimer, node);

		expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
		/*
		 * clock_was_set() has changed base->offset so the
		 * result might be negative. Fix it up to prevent a
		 * false positive in clockevents_program_event()
		 */
		if (expires.tv64 < 0)
			expires.tv64 = 0;
		if (expires.tv64 < expires_next.tv64)
			expires_next = expires;
	}

	if (skip_equal && expires_next.tv64 == cpu_base->expires_next.tv64)
		return;

	cpu_base->expires_next.tv64 = expires_next.tv64;

	if (cpu_base->expires_next.tv64 != KTIME_MAX)
		tick_program_event(cpu_base->expires_next, 1);
}

/*
 * Shared reprogramming for clock_realtime and clock_monotonic
 *
 * When a timer is enqueued and expires earlier than the already enqueued
 * timers, we have to check, whether it expires earlier than the timer for
 * which the clock event device was armed.
 *
 * Called with interrupts disabled and base->cpu_base.lock held
 */
static int hrtimer_reprogram(struct hrtimer *timer,
			     struct hrtimer_clock_base *base)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	ktime_t expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
	int res;

	WARN_ON_ONCE(hrtimer_get_expires_tv64(timer) < 0);

	/*
	 * When the callback is running, we do not reprogram the clock event
	 * device. The timer callback is either running on a different CPU or
	 * the callback is executed in the hrtimer_interrupt context. The
	 * reprogramming is handled either by the softirq, which called the
	 * callback or at the end of the hrtimer_interrupt.
	 */
	if (hrtimer_callback_running(timer))
		return 0;

	/*
	 * CLOCK_REALTIME timer might be requested with an absolute
	 * expiry time which is less than base->offset. Nothing wrong
	 * about that, just avoid to call into the tick code, which
	 * has now objections against negative expiry values.
	 */
	if (expires.tv64 < 0)
		return -ETIME;

	if (expires.tv64 >= cpu_base->expires_next.tv64)
		return 0;

	/*
	 * If a hang was detected in the last timer interrupt then we
	 * do not schedule a timer which is earlier than the expiry
	 * which we enforced in the hang detection. We want the system
	 * to make progress.
	 */
	if (cpu_base->hang_detected)
		return 0;

	/*
	 * Clockevents returns -ETIME, when the event was in the past.
	 */
	res = tick_program_event(expires, 0);
	if (!IS_ERR_VALUE(res))
		cpu_base->expires_next = expires;
	return res;
}

/*
 * Initialize the high resolution related parts of cpu_base
 */
/** 20141011
 * hres timer base를 초기화 한다.
 **/
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base)
{
	base->expires_next.tv64 = KTIME_MAX;
	base->hres_active = 0;
}

/*
 * When High resolution timers are active, try to reprogram. Note, that in case
 * the state has HRTIMER_STATE_CALLBACK set, no reprogramming and no expiry
 * check happens. The timer gets enqueued into the rbtree. The reprogramming
 * and expiry check is done in the hrtimer_interrupt or in the softirq.
 */
/** 20141115
 * hrtimer가 timerqueue에 enqueue되었을 때 가장 가까운 expire라면
 * hrtimer를 reprogram 한다.
 **/
static inline int hrtimer_enqueue_reprogram(struct hrtimer *timer,
					    struct hrtimer_clock_base *base,
					    int wakeup)
{
	/** 20141115
	 * hres timer로 동작 중이며, hrtimer_reprogram이 성공한 뒤
	 * hrtimer_interrup에서 expire 되었는지 check하고, softirq를 발생시킨다.
	 **/
	if (base->cpu_base->hres_active && hrtimer_reprogram(timer, base)) {
		/** 20141115
		 * wakeup이 설정된 경우
		 *   cpu_base에 lock을 건 상태로 HRTIMER_SOFTIRQ를 raise 한다.
		 * wakeup이 설정되지 않은 경우
		 *   pending만 표시한다.
		 **/
		if (wakeup) {
			raw_spin_unlock(&base->cpu_base->lock);
			raise_softirq_irqoff(HRTIMER_SOFTIRQ);
			raw_spin_lock(&base->cpu_base->lock);
		} else
			__raise_softirq_irqoff(HRTIMER_SOFTIRQ);

		return 1;
	}

	return 0;
}

static inline ktime_t hrtimer_update_base(struct hrtimer_cpu_base *base)
{
	ktime_t *offs_real = &base->clock_base[HRTIMER_BASE_REALTIME].offset;
	ktime_t *offs_boot = &base->clock_base[HRTIMER_BASE_BOOTTIME].offset;

	return ktime_get_update_offsets(offs_real, offs_boot);
}

/*
 * Retrigger next event is called after clock was set
 *
 * Called with interrupts disabled via on_each_cpu()
 */
/** 20141129
 * timekeeping 분석 후 분석예정???
 **/
static void retrigger_next_event(void *arg)
{
	struct hrtimer_cpu_base *base = &__get_cpu_var(hrtimer_bases);

	if (!hrtimer_hres_active())
		return;

	raw_spin_lock(&base->lock);
	hrtimer_update_base(base);
	hrtimer_force_reprogram(base, 0);
	raw_spin_unlock(&base->lock);
}

/*
 * Switch to high resolution mode
 */
/** 20141201
 * hrtimer로 highres로 동작한다.
 *
 * tick을 emulation하기 위한 hrtimer를 생성해 시작시킨다.
 **/
static int hrtimer_switch_to_hres(void)
{
	int i, cpu = smp_processor_id();
	struct hrtimer_cpu_base *base = &per_cpu(hrtimer_bases, cpu);
	unsigned long flags;

	/** 20141115
	 * 현재 hres_active되어 동작 중이면 바로 리턴한다.
	 **/
	if (base->hres_active)
		return 1;

	/** 20141129
	 * tick을 highres로 변경시키는동안 interrupt가 발생하지 않도록 한다.
	 **/
	local_irq_save(flags);

	/** 20141129
	 * highres를 위한 tick init.
	 * clock_event_device를 ONESHOT 방식으로 설정한다.
	 **/
	if (tick_init_highres()) {
		local_irq_restore(flags);
		printk(KERN_WARNING "Could not switch to high resolution "
				    "mode on CPU %d\n", cpu);
		return 0;
	}
	/** 20141101
	 * high-res timer가 high resolution으로 동작한다.
	 **/
	base->hres_active = 1;
	/** 20141115
	 * 해당 hrtimer_cpu_base의 array의 resolution을 초기화 한다.
	 **/
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++)
		base->clock_base[i].resolution = KTIME_HIGH_RES;

	/** 20141129
	 * tick_sched를 위한 timer를 하나 생성해 등록시킨다.
	 **/
	tick_setup_sched_timer();
	/* "Retrigger" the interrupt to get things going */
	retrigger_next_event(NULL);
	local_irq_restore(flags);
	return 1;
}

/*
 * Called from timekeeping code to reprogramm the hrtimer interrupt
 * device. If called from the timer interrupt context we defer it to
 * softirq context.
 */
void clock_was_set_delayed(void)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);

	cpu_base->clock_was_set = 1;
	__raise_softirq_irqoff(HRTIMER_SOFTIRQ);
}

#else

/** 20140920
 * CONFIG_HIGH_RES_TIMERS가 정의되지 않았다.
 **/
static inline int hrtimer_hres_active(void) { return 0; }
static inline int hrtimer_is_hres_enabled(void) { return 0; }
static inline int hrtimer_switch_to_hres(void) { return 0; }
static inline void
hrtimer_force_reprogram(struct hrtimer_cpu_base *base, int skip_equal) { }
static inline int hrtimer_enqueue_reprogram(struct hrtimer *timer,
					    struct hrtimer_clock_base *base,
					    int wakeup)
{
	return 0;
}
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base) { }
static inline void retrigger_next_event(void *arg) { }

#endif /* CONFIG_HIGH_RES_TIMERS */

/*
 * Clock realtime was set
 *
 * Change the offset of the realtime clock vs. the monotonic
 * clock.
 *
 * We might have to reprogram the high resolution timer interrupt. On
 * SMP we call the architecture specific code to retrigger _all_ high
 * resolution timer interrupts. On UP we just disable interrupts and
 * call the high resolution interrupt code.
 */
void clock_was_set(void)
{
#ifdef CONFIG_HIGH_RES_TIMERS
	/* Retrigger the CPU local events everywhere */
	on_each_cpu(retrigger_next_event, NULL, 1);
#endif
	timerfd_clock_was_set();
}

/*
 * During resume we might have to reprogram the high resolution timer
 * interrupt (on the local CPU):
 */
void hrtimers_resume(void)
{
	WARN_ONCE(!irqs_disabled(),
		  KERN_INFO "hrtimers_resume() called with IRQs enabled!");

	retrigger_next_event(NULL);
	timerfd_clock_was_set();
}

/** 20141115
 * TIMER_STATS 분석 생략.
 **/
static inline void timer_stats_hrtimer_set_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (timer->start_site)
		return;
	timer->start_site = __builtin_return_address(0);
	memcpy(timer->start_comm, current->comm, TASK_COMM_LEN);
	timer->start_pid = current->pid;
#endif
}

/** 20141108
 * TIMER_STATS 분석 생략.
 **/
static inline void timer_stats_hrtimer_clear_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
#endif
}

static inline void timer_stats_account_hrtimer(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (likely(!timer_stats_active))
		return;
	timer_stats_update_stats(timer, timer->start_pid, timer->start_site,
				 timer->function, timer->start_comm, 0);
#endif
}

/*
 * Counterpart to lock_hrtimer_base above:
 */
/** 20141108
 * lock_hrtimer_base에서 걸어둔 spinlock을 해제한다.
 **/
static inline
void unlock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	raw_spin_unlock_irqrestore(&timer->base->cpu_base->lock, *flags);
}

/**
 * hrtimer_forward - forward the timer expiry
 * @timer:	hrtimer to forward
 * @now:	forward past this time
 * @interval:	the interval to forward
 *
 * Forward the timer expiry so it will expire in the future.
 * Returns the number of overruns.
 */
/** 20140419
 * timer의 expires에 interval을 더해 앞으로 있을 다음 interval에 만료되도록 한다.
 * 현재 시간이 timer의 만료시간을 초과한 횟수(/interval)가 리턴된다.
 *
 * sched_rt_period_timer에서 호출하였을 때 interval은 rt_period.
 **/
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta;

	/** 20140419
	 * 현재 ktime 값에서 timer의 expires를 뺀다.
	 **/
	delta = ktime_sub(now, hrtimer_get_expires(timer));

	/** 20140419
	 * delta 값이 0보다 작다면, 만료가 안된 경우 0을 리턴.
	 **/
	if (delta.tv64 < 0)
		return 0;

	/** 20140419
	 * interval의 최소값을 base clock의 resolution으로 삼는다.
	 **/
	if (interval.tv64 < timer->base->resolution.tv64)
		interval.tv64 = timer->base->resolution.tv64;

	/** 20140419
	 * delta는 만료 후 얼마가 지났는지 표현.
	 * delta가 interval 이상인 경우 아래 부분을 수행
	 **/
	if (unlikely(delta.tv64 >= interval.tv64)) {
		/** 20140419
		 * interval (ktime)을 ns 단위로 변환.
		 **/
		s64 incr = ktime_to_ns(interval);

		/** 20140419
		 * delta가 interval이 몇 번 지난 값인지 몫을 구해 orun에 저장.
		 * overun된 interval 시간을 ns로 다시 변환해 expires에 저장
		 **/
		orun = ktime_divns(delta, incr);
		/** 20140419
		 * overrun 된 ns를 timer의 expires에 반영한다.
		 **/
		hrtimer_add_expires_ns(timer, incr * orun);
		/** 20140419
		 * expires 값이 현재 시간보다 크면 이후 만료될 것이므로 overrun 값을 리턴
		 **/
		if (hrtimer_get_expires_tv64(timer) > now.tv64)
			return orun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		/** 20140419
		 * interval을 하나 더 증가시킨다.
		 **/
		orun++;
	}
	/** 20140419
	 * timer의 expires에 interval을 더한다.
	 **/
	hrtimer_add_expires(timer, interval);

	return orun;
}
EXPORT_SYMBOL_GPL(hrtimer_forward);

/*
 * enqueue_hrtimer - internal function to (re)start a timer
 *
 * The timer is inserted in expiry order. Insertion into the
 * red black tree is O(log(n)). Must hold the base lock.
 *
 * Returns 1 when the new timer is the leftmost timer in the tree.
 */
/** 20141115
 * lock이 걸린 상태에서 새로운 hrtimer를 queue(rb-tree)에 추가하고,
 * 관련 자료구조를 설정한다.
 **/
static int enqueue_hrtimer(struct hrtimer *timer,
			   struct hrtimer_clock_base *base)
{
	debug_activate(timer);

	/** 20141115
	 * timerqueue에 새로운 timer를 추가하고,
	 * 해당 cpu_base의 active_bases에 active timer가 존재함을 표시한다.
	 **/
	timerqueue_add(&base->active, &timer->node);
	base->cpu_base->active_bases |= 1 << base->index;

	/*
	 * HRTIMER_STATE_ENQUEUED is or'ed to the current state to preserve the
	 * state of a possibly running callback.
	 */
	/** 20141115
	 * hrtimer의 상태에 enqueue되었음을 표시한다.
	 **/
	timer->state |= HRTIMER_STATE_ENQUEUED;

	/** 20141115
	 * 새로 추가된 timer가 timerqueue의 다음 expires될 함수라면 1이 리턴된다.
	 **/
	return (&timer->node == base->active.next);
}

/*
 * __remove_hrtimer - internal function to remove a timer
 *
 * Caller must hold the base lock.
 *
 * High resolution timer mode reprograms the clock event device when the
 * timer is the one which expires next. The caller can disable this by setting
 * reprogram to zero. This is useful, when the context does a reprogramming
 * anyway (e.g. timer interrupt)
 */
/** 20141108
 * timerqueue에서 timer를 제거한다.
 * 
 * 제거하는 timer가 다음 만료될 timer였다면 reprogram에 따라 clock event device를
 * 재프로그램 한다.
 **/
static void __remove_hrtimer(struct hrtimer *timer,
			     struct hrtimer_clock_base *base,
			     unsigned long newstate, int reprogram)
{
	struct timerqueue_node *next_timer;
	if (!(timer->state & HRTIMER_STATE_ENQUEUED))
		goto out;

	/** 20141108
	 * 다음 만료될 timerqueue_node를 먼저 가져온다. (제거 전)
	 * timerqueue에서 node를 제거한다.
	 * 제거한 노드가 다음 만료될 타이머라면 reprogram에 따라
	 * clock event device를 reprogram 한다.
	 **/
	next_timer = timerqueue_getnext(&base->active);
	timerqueue_del(&base->active, &timer->node);
	if (&timer->node == next_timer) {
#ifdef CONFIG_HIGH_RES_TIMERS
		/* Reprogram the clock event device. if enabled */
		if (reprogram && hrtimer_hres_active()) {
			ktime_t expires;

			expires = ktime_sub(hrtimer_get_expires(timer),
					    base->offset);
			if (base->cpu_base->expires_next.tv64 == expires.tv64)
				hrtimer_force_reprogram(base->cpu_base, 1);
		}
#endif
	}
	/** 20141108
	 * timerqueue에 남은 node가 없다면 hrtimer_cpu_base 중
	 * 해당 clock base를 active_bases에서 지운다.
	 **/
	if (!timerqueue_getnext(&base->active))
		base->cpu_base->active_bases &= ~(1 << base->index);
out:
	timer->state = newstate;
}

/*
 * remove hrtimer, called with base lock held
 */
/** 20141108
 * hrimter를 timerqueue에서 제거한다.
 **/
static inline int
remove_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base)
{
	if (hrtimer_is_queued(timer)) {
		unsigned long state;
		int reprogram;

		/*
		 * Remove the timer and force reprogramming when high
		 * resolution mode is active and the timer is on the current
		 * CPU. If we remove a timer on another CPU, reprogramming is
		 * skipped. The interrupt event on this CPU is fired and
		 * reprogramming happens in the interrupt handler. This is a
		 * rare case and less expensive than a smp call.
		 */
		debug_deactivate(timer);
		timer_stats_hrtimer_clear_start_info(timer);
		/** 20141108
		 * clock_base가 현재 cpu에 해당할 경우에만 강제로 reprogram 한다.
		 * (호출 비용)
		 **/
		reprogram = base->cpu_base == &__get_cpu_var(hrtimer_bases);
		/*
		 * We must preserve the CALLBACK state flag here,
		 * otherwise we could move the timer base in
		 * switch_hrtimer_base.
		 */
		state = timer->state & HRTIMER_STATE_CALLBACK;
		__remove_hrtimer(timer, base, state, reprogram);
		return 1;
	}
	return 0;
}

/** 20141115
 * hrtimer를 range (soft expires + delta_ns)를 받아 등록시킨다.
 *
 * hrtimer를 clock_base에 등록(enqueue)시키고,
 * 가장 가까운 expire값이라면 clock device를 reprogram 하고 softirq를 발생시킨다.
 **/
int __hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode,
		int wakeup)
{
	struct hrtimer_clock_base *base, *new_base;
	unsigned long flags;
	int ret, leftmost;

	/** 20141108
	 * hrtimer의 clock_base에 lock을 걸고,
	 * hrtimer의 hrtimer_clock_base를 리턴한다.
	 **/
	base = lock_hrtimer_base(timer, &flags);

	/* Remove an active timer from the queue: */
	/** 20141108
	 * active timer를 timerqueue에서 제거한다.
	 **/
	ret = remove_hrtimer(timer, base);

	/* Switch the timer base, if necessary: */
	/** 20141129
	 * 필요하다면 hrtimer의 base를 변경한다.
	 * mode에 PINNED가 설정되어 있다면 new_base는 기존 base와 동일하다.
	 **/
	new_base = switch_hrtimer_base(timer, base, mode & HRTIMER_MODE_PINNED);

	/** 20141115
	 * 넘어온 mode가 HRTIMER_MODE_REL인 경우(현재 시간에 상대적일 때) 수행.
	 **/
	if (mode & HRTIMER_MODE_REL) {
		/** 20141115
		 * 새로운 base에서 시간을 얻어와 ktime에 더해준다.
		 **/
		tim = ktime_add_safe(tim, new_base->get_time());
		/*
		 * CONFIG_TIME_LOW_RES is a temporary way for architectures
		 * to signal that they simply return xtime in
		 * do_gettimeoffset(). In this case we want to round up by
		 * resolution when starting a relative timer, to avoid short
		 * timeouts. This will go away with the GTOD framework.
		 */
#ifdef CONFIG_TIME_LOW_RES
		tim = ktime_add_safe(tim, base->resolution);
#endif
	}

	/** 20141115
	 * hrtimer의 expires range를 설정한다. delta_ns는 ns.
	 **/
	hrtimer_set_expires_range_ns(timer, tim, delta_ns);

	timer_stats_hrtimer_set_start_info(timer);

	/** 20141115
	 * hrtimer를 new_base의 timerqueue(rbtree로 구성)에 추가하고,
	 * leftmost node여부를 리턴받는다.
	 **/
	leftmost = enqueue_hrtimer(timer, new_base);

	/*
	 * Only allow reprogramming if the new base is on this CPU.
	 * (it might still be on another CPU if the timer was pending)
	 *
	 * XXX send_remote_softirq() ?
	 */
	/** 20141115
	 * enqueue한 hrtimer가 leftmost, 즉 가장 가까운 expire될 타이머이며,
	 * 해당 cpu_base가 현재 cpu라면
	 * hrtimer_enqueue_reprogram을 호출해 reprogram 하고 softirq를 호출한다.
	 **/
	if (leftmost && new_base->cpu_base == &__get_cpu_var(hrtimer_bases))
		hrtimer_enqueue_reprogram(timer, new_base, wakeup);

	/** 20141115
	 * hrtimer_cpu_base에 걸었던 lock을 해제한다.
	 **/
	unlock_hrtimer_base(timer, &flags);

	return ret;
}

/**
 * hrtimer_start_range_ns - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	expiry mode: absolute (HRTIMER_ABS) or relative (HRTIMER_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
/** 20141129
 * 현재 cpu에 hrtimer의 만료시간을 받아 등록한다.
 **/
int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode)
{
	/** 20141129
	 * softirq wakeup을 1로 설정한다.
	 **/
	return __hrtimer_start_range_ns(timer, tim, delta_ns, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns);

/**
 * hrtimer_start - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @mode:	expiry mode: absolute (HRTIMER_ABS) or relative (HRTIMER_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
int
hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode)
{
	return __hrtimer_start_range_ns(timer, tim, 0, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start);


/**
 * hrtimer_try_to_cancel - try to deactivate a timer
 * @timer:	hrtimer to stop
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 * -1 when the timer is currently excuting the callback function and
 *    cannot be stopped
 */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	int ret = -1;

	base = lock_hrtimer_base(timer, &flags);

	if (!hrtimer_callback_running(timer))
		ret = remove_hrtimer(timer, base);

	unlock_hrtimer_base(timer, &flags);

	return ret;

}
EXPORT_SYMBOL_GPL(hrtimer_try_to_cancel);

/**
 * hrtimer_cancel - cancel a timer and wait for the handler to finish.
 * @timer:	the timer to be cancelled
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 */
int hrtimer_cancel(struct hrtimer *timer)
{
	for (;;) {
		int ret = hrtimer_try_to_cancel(timer);

		if (ret >= 0)
			return ret;
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(hrtimer_cancel);

/**
 * hrtimer_get_remaining - get remaining time for the timer
 * @timer:	the timer to read
 */
ktime_t hrtimer_get_remaining(const struct hrtimer *timer)
{
	unsigned long flags;
	ktime_t rem;

	lock_hrtimer_base(timer, &flags);
	rem = hrtimer_expires_remaining(timer);
	unlock_hrtimer_base(timer, &flags);

	return rem;
}
EXPORT_SYMBOL_GPL(hrtimer_get_remaining);

#ifdef CONFIG_NO_HZ
/**
 * hrtimer_get_next_event - get the time until next expiry event
 *
 * Returns the delta to the next expiry event or KTIME_MAX if no timer
 * is pending.
 */
ktime_t hrtimer_get_next_event(void)
{
	/** 20141101
	 * 현재 cpu에 해당하는 hrtimer_bases를 가져온다.
	 **/
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	struct hrtimer_clock_base *base = cpu_base->clock_base;
	ktime_t delta, mindelta = { .tv64 = KTIME_MAX };
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&cpu_base->lock, flags);

	if (!hrtimer_hres_active()) {
		for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++, base++) {
			struct hrtimer *timer;
			struct timerqueue_node *next;

			next = timerqueue_getnext(&base->active);
			if (!next)
				continue;

			timer = container_of(next, struct hrtimer, node);
			delta.tv64 = hrtimer_get_expires_tv64(timer);
			delta = ktime_sub(delta, base->get_time());
			if (delta.tv64 < mindelta.tv64)
				mindelta.tv64 = delta.tv64;
		}
	}

	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);

	if (mindelta.tv64 < 0)
		mindelta.tv64 = 0;
	return mindelta;
}
#endif

/** 20141108
 * hrtimer를 초기화 한다.
 *
 * hrtimer 구조체의 node(timerqueue node)와 base(clock base)를 초기화 한다.
 **/
static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	struct hrtimer_cpu_base *cpu_base;
	int base;

	memset(timer, 0, sizeof(struct hrtimer));

	/** 20141115
	 * hrtimer_bases 변수에서 현재 cpu에 해당하는 cpu_base를 가져온다.
	 **/
	cpu_base = &__raw_get_cpu_var(hrtimer_bases);

	if (clock_id == CLOCK_REALTIME && mode != HRTIMER_MODE_ABS)
		clock_id = CLOCK_MONOTONIC;

	/** 20141115
	 * clock id(다양한 시스템 clock에 대한 ID)로 base index를 찾아온다.
	 **/
	base = hrtimer_clockid_to_base(clock_id);
	/** 20141108
	 * clock_base 중 clock_id에 해당하는 base를 찾아
	 * 이 hrtimer의 hrtimer_clock_base로 삼는다.
	 *
	 * hrtimer_bases에서 hrtimer_clock_base의 array를 찾아온다.
	 **/
	timer->base = &cpu_base->clock_base[base];
	timerqueue_init(&timer->node);

#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
	timer->start_pid = -1;
	memset(timer->start_comm, 0, TASK_COMM_LEN);
#endif
}

/**
 * hrtimer_init - initialize a timer to the given clock
 * @timer:	the timer to be initialized
 * @clock_id:	the clock to be used
 * @mode:	timer mode abs/rel
 */
/** 20141108
 * timer를 주어진 clock으로 초기화 한다.
 **/
void hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
		  enum hrtimer_mode mode)
{
	debug_init(timer, clock_id, mode);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init);

/**
 * hrtimer_get_res - get the timer resolution for a clock
 * @which_clock: which clock to query
 * @tp:		 pointer to timespec variable to store the resolution
 *
 * Store the resolution of the clock selected by @which_clock in the
 * variable pointed to by @tp.
 */
int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp)
{
	struct hrtimer_cpu_base *cpu_base;
	int base = hrtimer_clockid_to_base(which_clock);

	cpu_base = &__raw_get_cpu_var(hrtimer_bases);
	*tp = ktime_to_timespec(cpu_base->clock_base[base].resolution);

	return 0;
}
EXPORT_SYMBOL_GPL(hrtimer_get_res);

static void __run_hrtimer(struct hrtimer *timer, ktime_t *now)
{
	struct hrtimer_clock_base *base = timer->base;
	struct hrtimer_cpu_base *cpu_base = base->cpu_base;
	enum hrtimer_restart (*fn)(struct hrtimer *);
	int restart;

	WARN_ON(!irqs_disabled());

	debug_deactivate(timer);
	__remove_hrtimer(timer, base, HRTIMER_STATE_CALLBACK, 0);
	timer_stats_account_hrtimer(timer);
	fn = timer->function;

	/*
	 * Because we run timers from hardirq context, there is no chance
	 * they get migrated to another cpu, therefore its safe to unlock
	 * the timer base.
	 */
	raw_spin_unlock(&cpu_base->lock);
	trace_hrtimer_expire_entry(timer, now);
	restart = fn(timer);
	trace_hrtimer_expire_exit(timer);
	raw_spin_lock(&cpu_base->lock);

	/*
	 * Note: We clear the CALLBACK bit after enqueue_hrtimer and
	 * we do not reprogramm the event hardware. Happens either in
	 * hrtimer_start_range_ns() or in hrtimer_interrupt()
	 */
	if (restart != HRTIMER_NORESTART) {
		BUG_ON(timer->state != HRTIMER_STATE_CALLBACK);
		enqueue_hrtimer(timer, base);
	}

	WARN_ON_ONCE(!(timer->state & HRTIMER_STATE_CALLBACK));

	timer->state &= ~HRTIMER_STATE_CALLBACK;
}

#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer interrupt
 * Called with interrupts disabled
 */
/** 20141206
 *
 * tick_init_highres에서 tick_device의 event_handler로 지정.
 * hrtimer_peek_ahead_timers를 통해 들어오는 경우.
 **/
void hrtimer_interrupt(struct clock_event_device *dev)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	ktime_t expires_next, now, entry_time, delta;
	int i, retries = 0;

	BUG_ON(!cpu_base->hres_active);
	cpu_base->nr_events++;
	dev->next_event.tv64 = KTIME_MAX;

	raw_spin_lock(&cpu_base->lock);
	entry_time = now = hrtimer_update_base(cpu_base);
retry:
	expires_next.tv64 = KTIME_MAX;
	/*
	 * We set expires_next to KTIME_MAX here with cpu_base->lock
	 * held to prevent that a timer is enqueued in our queue via
	 * the migration code. This does not affect enqueueing of
	 * timers which run their callback and need to be requeued on
	 * this CPU.
	 */
	cpu_base->expires_next.tv64 = KTIME_MAX;

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		struct hrtimer_clock_base *base;
		struct timerqueue_node *node;
		ktime_t basenow;

		if (!(cpu_base->active_bases & (1 << i)))
			continue;

		base = cpu_base->clock_base + i;
		basenow = ktime_add(now, base->offset);

		while ((node = timerqueue_getnext(&base->active))) {
			struct hrtimer *timer;

			timer = container_of(node, struct hrtimer, node);

			/*
			 * The immediate goal for using the softexpires is
			 * minimizing wakeups, not running timers at the
			 * earliest interrupt after their soft expiration.
			 * This allows us to avoid using a Priority Search
			 * Tree, which can answer a stabbing querry for
			 * overlapping intervals and instead use the simple
			 * BST we already have.
			 * We don't add extra wakeups by delaying timers that
			 * are right-of a not yet expired timer, because that
			 * timer will have to trigger a wakeup anyway.
			 */

			if (basenow.tv64 < hrtimer_get_softexpires_tv64(timer)) {
				ktime_t expires;

				expires = ktime_sub(hrtimer_get_expires(timer),
						    base->offset);
				if (expires.tv64 < expires_next.tv64)
					expires_next = expires;
				break;
			}

			__run_hrtimer(timer, &basenow);
		}
	}

	/*
	 * Store the new expiry value so the migration code can verify
	 * against it.
	 */
	cpu_base->expires_next = expires_next;
	raw_spin_unlock(&cpu_base->lock);

	/* Reprogramming necessary ? */
	if (expires_next.tv64 == KTIME_MAX ||
	    !tick_program_event(expires_next, 0)) {
		cpu_base->hang_detected = 0;
		return;
	}

	/*
	 * The next timer was already expired due to:
	 * - tracing
	 * - long lasting callbacks
	 * - being scheduled away when running in a VM
	 *
	 * We need to prevent that we loop forever in the hrtimer
	 * interrupt routine. We give it 3 attempts to avoid
	 * overreacting on some spurious event.
	 *
	 * Acquire base lock for updating the offsets and retrieving
	 * the current time.
	 */
	raw_spin_lock(&cpu_base->lock);
	now = hrtimer_update_base(cpu_base);
	cpu_base->nr_retries++;
	if (++retries < 3)
		goto retry;
	/*
	 * Give the system a chance to do something else than looping
	 * here. We stored the entry time, so we know exactly how long
	 * we spent here. We schedule the next event this amount of
	 * time away.
	 */
	cpu_base->nr_hangs++;
	cpu_base->hang_detected = 1;
	raw_spin_unlock(&cpu_base->lock);
	delta = ktime_sub(now, entry_time);
	if (delta.tv64 > cpu_base->max_hang_time.tv64)
		cpu_base->max_hang_time = delta;
	/*
	 * Limit it to a sensible value as we enforce a longer
	 * delay. Give the CPU at least 100ms to catch up.
	 */
	if (delta.tv64 > 100 * NSEC_PER_MSEC)
		expires_next = ktime_add_ns(now, 100 * NSEC_PER_MSEC);
	else
		expires_next = ktime_add(now, delta);
	tick_program_event(expires_next, 1);
	printk_once(KERN_WARNING "hrtimer: interrupt took %llu ns\n",
		    ktime_to_ns(delta));
}

/*
 * local version of hrtimer_peek_ahead_timers() called with interrupts
 * disabled.
 */
static void __hrtimer_peek_ahead_timers(void)
{
	struct tick_device *td;

	if (!hrtimer_hres_active())
		return;

	td = &__get_cpu_var(tick_cpu_device);
	if (td && td->evtdev)
		hrtimer_interrupt(td->evtdev);
}

/**
 * hrtimer_peek_ahead_timers -- run soft-expired timers now
 *
 * hrtimer_peek_ahead_timers will peek at the timer queue of
 * the current cpu and check if there are any timers for which
 * the soft expires time has passed. If any such timers exist,
 * they are run immediately and then removed from the timer queue.
 *
 */
void hrtimer_peek_ahead_timers(void)
{
	unsigned long flags;

	local_irq_save(flags);
	__hrtimer_peek_ahead_timers();
	local_irq_restore(flags);
}

/** 20141115
 * 분석 예정 ???
 **/
static void run_hrtimer_softirq(struct softirq_action *h)
{
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);

	if (cpu_base->clock_was_set) {
		cpu_base->clock_was_set = 0;
		clock_was_set();
	}

	hrtimer_peek_ahead_timers();
}

#else /* CONFIG_HIGH_RES_TIMERS */

static inline void __hrtimer_peek_ahead_timers(void) { }

#endif	/* !CONFIG_HIGH_RES_TIMERS */

/*
 * Called from timer softirq every jiffy, expire hrtimers:
 *
 * For HRT its the fall back code to run the softirq in the timer
 * softirq context in case the hrtimer initialization failed or has
 * not been done yet.
 */
/** 20140920
 * vexpress의 경우 high resolution timer가 config되어 있지 않다.
 *
 * 20141206
 * hrtimer로 동작한다면 highres 또는 nohz로 동작시켜야 하는지 판단해
 * 각각 함수를 호출한다.
 *
 * softirq conext 내에서 조건을 판단해 sched_timer를 만들어
 * mode change를 하는 것이 바람직한 구조는 아니다.
 **/
void hrtimer_run_pending(void)
{
	/** 20141129
	 * 이미 hrtimer가 hres로 동작 중이라면 바로 리턴한다.
	 **/
	if (hrtimer_hres_active())
		return;

	/*
	 * This _is_ ugly: We have to check in the softirq context,
	 * whether we can switch to highres and / or nohz mode. The
	 * clocksource switch happens in the timer interrupt with
	 * xtime_lock held. Notification from there only sets the
	 * check bit in the tick_oneshot code, otherwise we might
	 * deadlock vs. xtime_lock.
	 */
	/** 20141101
	 * TICK이 oneshot으로 동작해야 하는지 체크해 그렇다면 hres로 동작시킨다.
	 *
	 * HIGH_RES_TIMERS이 설정되어 있다면 hrtimer_is_hres_enabled()는 1이 된다.
	 * NO_HZ만 설정되어 있다면 tick_check_oneshot_change 내에서
	 * tick_nohz_switch_to_nohz가 호출되고,
	 * hrtimer_switch_to_hres는 호출되지 않는다.
	 **/
	if (tick_check_oneshot_change(!hrtimer_is_hres_enabled()))
		hrtimer_switch_to_hres();
}

/*
 * Called from hardirq context every jiffy
 */
void hrtimer_run_queues(void)
{
	struct timerqueue_node *node;
	struct hrtimer_cpu_base *cpu_base = &__get_cpu_var(hrtimer_bases);
	struct hrtimer_clock_base *base;
	int index, gettime = 1;

	if (hrtimer_hres_active())
		return;

	for (index = 0; index < HRTIMER_MAX_CLOCK_BASES; index++) {
		base = &cpu_base->clock_base[index];
		if (!timerqueue_getnext(&base->active))
			continue;

		if (gettime) {
			hrtimer_get_softirq_time(cpu_base);
			gettime = 0;
		}

		raw_spin_lock(&cpu_base->lock);

		while ((node = timerqueue_getnext(&base->active))) {
			struct hrtimer *timer;

			timer = container_of(node, struct hrtimer, node);
			if (base->softirq_time.tv64 <=
					hrtimer_get_expires_tv64(timer))
				break;

			__run_hrtimer(timer, &base->softirq_time);
		}
		raw_spin_unlock(&cpu_base->lock);
	}
}

/*
 * Sleep related functions:
 */
static enum hrtimer_restart hrtimer_wakeup(struct hrtimer *timer)
{
	struct hrtimer_sleeper *t =
		container_of(timer, struct hrtimer_sleeper, timer);
	struct task_struct *task = t->task;

	t->task = NULL;
	if (task)
		wake_up_process(task);

	return HRTIMER_NORESTART;
}

void hrtimer_init_sleeper(struct hrtimer_sleeper *sl, struct task_struct *task)
{
	sl->timer.function = hrtimer_wakeup;
	sl->task = task;
}
EXPORT_SYMBOL_GPL(hrtimer_init_sleeper);

static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
	hrtimer_init_sleeper(t, current);

	do {
		set_current_state(TASK_INTERRUPTIBLE);
		hrtimer_start_expires(&t->timer, mode);
		if (!hrtimer_active(&t->timer))
			t->task = NULL;

		if (likely(t->task))
			schedule();

		hrtimer_cancel(&t->timer);
		mode = HRTIMER_MODE_ABS;

	} while (t->task && !signal_pending(current));

	__set_current_state(TASK_RUNNING);

	return t->task == NULL;
}

static int update_rmtp(struct hrtimer *timer, struct timespec __user *rmtp)
{
	struct timespec rmt;
	ktime_t rem;

	rem = hrtimer_expires_remaining(timer);
	if (rem.tv64 <= 0)
		return 0;
	rmt = ktime_to_timespec(rem);

	if (copy_to_user(rmtp, &rmt, sizeof(*rmtp)))
		return -EFAULT;

	return 1;
}

long __sched hrtimer_nanosleep_restart(struct restart_block *restart)
{
	struct hrtimer_sleeper t;
	struct timespec __user  *rmtp;
	int ret = 0;

	hrtimer_init_on_stack(&t.timer, restart->nanosleep.clockid,
				HRTIMER_MODE_ABS);
	hrtimer_set_expires_tv64(&t.timer, restart->nanosleep.expires);

	if (do_nanosleep(&t, HRTIMER_MODE_ABS))
		goto out;

	rmtp = restart->nanosleep.rmtp;
	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	/* The other values in restart are already filled in */
	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

long hrtimer_nanosleep(struct timespec *rqtp, struct timespec __user *rmtp,
		       const enum hrtimer_mode mode, const clockid_t clockid)
{
	struct restart_block *restart;
	struct hrtimer_sleeper t;
	int ret = 0;
	unsigned long slack;

	slack = current->timer_slack_ns;
	if (rt_task(current))
		slack = 0;

	hrtimer_init_on_stack(&t.timer, clockid, mode);
	hrtimer_set_expires_range_ns(&t.timer, timespec_to_ktime(*rqtp), slack);
	if (do_nanosleep(&t, mode))
		goto out;

	/* Absolute timers do not update the rmtp value and restart: */
	if (mode == HRTIMER_MODE_ABS) {
		ret = -ERESTARTNOHAND;
		goto out;
	}

	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	restart = &current_thread_info()->restart_block;
	restart->fn = hrtimer_nanosleep_restart;
	restart->nanosleep.clockid = t.timer.base->clockid;
	restart->nanosleep.rmtp = rmtp;
	restart->nanosleep.expires = hrtimer_get_expires_tv64(&t.timer);

	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

SYSCALL_DEFINE2(nanosleep, struct timespec __user *, rqtp,
		struct timespec __user *, rmtp)
{
	struct timespec tu;

	if (copy_from_user(&tu, rqtp, sizeof(tu)))
		return -EFAULT;

	if (!timespec_valid(&tu))
		return -EINVAL;

	return hrtimer_nanosleep(&tu, rmtp, HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}

/*
 * Functions related to boot-time initialization:
 */
static void __cpuinit init_hrtimers_cpu(int cpu)
{
	struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);
	int i;

	raw_spin_lock_init(&cpu_base->lock);

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		cpu_base->clock_base[i].cpu_base = cpu_base;
		timerqueue_init_head(&cpu_base->clock_base[i].active);
	}

	hrtimer_init_hres(cpu_base);
}

#ifdef CONFIG_HOTPLUG_CPU

static void migrate_hrtimer_list(struct hrtimer_clock_base *old_base,
				struct hrtimer_clock_base *new_base)
{
	struct hrtimer *timer;
	struct timerqueue_node *node;

	while ((node = timerqueue_getnext(&old_base->active))) {
		timer = container_of(node, struct hrtimer, node);
		BUG_ON(hrtimer_callback_running(timer));
		debug_deactivate(timer);

		/*
		 * Mark it as STATE_MIGRATE not INACTIVE otherwise the
		 * timer could be seen as !active and just vanish away
		 * under us on another CPU
		 */
		__remove_hrtimer(timer, old_base, HRTIMER_STATE_MIGRATE, 0);
		timer->base = new_base;
		/*
		 * Enqueue the timers on the new cpu. This does not
		 * reprogram the event device in case the timer
		 * expires before the earliest on this CPU, but we run
		 * hrtimer_interrupt after we migrated everything to
		 * sort out already expired timers and reprogram the
		 * event device.
		 */
		enqueue_hrtimer(timer, new_base);

		/* Clear the migration state bit */
		timer->state &= ~HRTIMER_STATE_MIGRATE;
	}
}

static void migrate_hrtimers(int scpu)
{
	struct hrtimer_cpu_base *old_base, *new_base;
	int i;

	BUG_ON(cpu_online(scpu));
	tick_cancel_sched_timer(scpu);

	local_irq_disable();
	old_base = &per_cpu(hrtimer_bases, scpu);
	new_base = &__get_cpu_var(hrtimer_bases);
	/*
	 * The caller is globally serialized and nobody else
	 * takes two locks at once, deadlock is not possible.
	 */
	raw_spin_lock(&new_base->lock);
	raw_spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		migrate_hrtimer_list(&old_base->clock_base[i],
				     &new_base->clock_base[i]);
	}

	raw_spin_unlock(&old_base->lock);
	raw_spin_unlock(&new_base->lock);

	/* Check, if we got expired work to do */
	__hrtimer_peek_ahead_timers();
	local_irq_enable();
}

#endif /* CONFIG_HOTPLUG_CPU */

/** 20141011
 * high-res timer cpu notify.
 **/
static int __cpuinit hrtimer_cpu_notify(struct notifier_block *self,
					unsigned long action, void *hcpu)
{
	int scpu = (long)hcpu;

	switch (action) {

	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		init_hrtimers_cpu(scpu);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DYING, &scpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	{
		/** 20150613
		 * CPU_DEAD, CPU_DEAD_FROZEN을 받았을 때 clockevents notify를 보낸다.
		 **/
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DEAD, &scpu);
		migrate_hrtimers(scpu);
		break;
	}
#endif

	default:
		break;
	}

	return NOTIFY_OK;
}

/** 20141011
 * high-res timer Notifier block.
 **/
static struct notifier_block __cpuinitdata hrtimers_nb = {
	.notifier_call = hrtimer_cpu_notify,
};

/** 20140920
 * 추후 분석???
 **/
void __init hrtimers_init(void)
{
	/** 20141011
	 * cpu_notifier를 등록하기 전에, callback을 직접 호출해 init을 호출한다.
	 * 이후 cpu notify chain에 hrtimers_nb을 등록해 event를 받아 실행되도록 한다.
	 **/
	hrtimer_cpu_notify(&hrtimers_nb, (unsigned long)CPU_UP_PREPARE,
			  (void *)(long)smp_processor_id());
	register_cpu_notifier(&hrtimers_nb);
#ifdef CONFIG_HIGH_RES_TIMERS
	open_softirq(HRTIMER_SOFTIRQ, run_hrtimer_softirq);
#endif
}

/**
 * schedule_hrtimeout_range_clock - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 * @clock:	timer clock, CLOCK_MONOTONIC or CLOCK_REALTIME
 */
int __sched
schedule_hrtimeout_range_clock(ktime_t *expires, unsigned long delta,
			       const enum hrtimer_mode mode, int clock)
{
	struct hrtimer_sleeper t;

	/*
	 * Optimize when a zero timeout value is given. It does not
	 * matter whether this is an absolute or a relative time.
	 */
	if (expires && !expires->tv64) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	/*
	 * A NULL parameter means "infinite"
	 */
	if (!expires) {
		schedule();
		__set_current_state(TASK_RUNNING);
		return -EINTR;
	}

	hrtimer_init_on_stack(&t.timer, clock, mode);
	hrtimer_set_expires_range_ns(&t.timer, *expires, delta);

	hrtimer_init_sleeper(&t, current);

	hrtimer_start_expires(&t.timer, mode);
	if (!hrtimer_active(&t.timer))
		t.task = NULL;

	if (likely(t.task))
		schedule();

	hrtimer_cancel(&t.timer);
	destroy_hrtimer_on_stack(&t.timer);

	__set_current_state(TASK_RUNNING);

	return !t.task ? 0 : -EINTR;
}

/**
 * schedule_hrtimeout_range - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * The @delta argument gives the kernel the freedom to schedule the
 * actual wakeup to a time that is both power and performance friendly.
 * The kernel give the normal best effort behavior for "@expires+@delta",
 * but may decide to fire the timer earlier, but no earlier than @expires.
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
int __sched schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
				     const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range_clock(expires, delta, mode,
					      CLOCK_MONOTONIC);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout_range);

/**
 * schedule_hrtimeout - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
int __sched schedule_hrtimeout(ktime_t *expires,
			       const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range(expires, 0, mode);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout);
