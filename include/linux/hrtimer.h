/*
 *  include/linux/hrtimer.h
 *
 *  hrtimers - High-resolution kernel timers
 *
 *   Copyright(C) 2005, Thomas Gleixner <tglx@linutronix.de>
 *   Copyright(C) 2005, Red Hat, Inc., Ingo Molnar
 *
 *  data type definitions, declarations, prototypes
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  For licencing details see kernel-base/COPYING
 */
#ifndef _LINUX_HRTIMER_H
#define _LINUX_HRTIMER_H

#include <linux/rbtree.h>
#include <linux/ktime.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/percpu.h>
#include <linux/timer.h>
#include <linux/timerqueue.h>

struct hrtimer_clock_base;
struct hrtimer_cpu_base;

/*
 * Mode arguments of xxx_hrtimer functions:
 */
/** 20140419
 * high resolution timer mode 설정
 **/
enum hrtimer_mode {
	HRTIMER_MODE_ABS = 0x0,		/* Time value is absolute */
	HRTIMER_MODE_REL = 0x1,		/* Time value is relative to now */
	HRTIMER_MODE_PINNED = 0x02,	/* Timer is bound to CPU */
	HRTIMER_MODE_ABS_PINNED = 0x02,
	HRTIMER_MODE_REL_PINNED = 0x03,
};

/*
 * Return values for the callback function
 */
/** 20140419
 **/
enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART,	/* Timer must be restarted */
};

/*
 * Values to track state of the timer
 *
 * Possible states:
 *
 * 0x00		inactive
 * 0x01		enqueued into rbtree
 * 0x02		callback function running
 * 0x04		timer is migrated to another cpu
 *
 * Special cases:
 * 0x03		callback function running and enqueued
 *		(was requeued on another CPU)
 * 0x05		timer was migrated on CPU hotunplug
 *
 * The "callback function running and enqueued" status is only possible on
 * SMP. It happens for example when a posix timer expired and the callback
 * queued a signal. Between dropping the lock which protects the posix timer
 * and reacquiring the base lock of the hrtimer, another CPU can deliver the
 * signal and rearm the timer. We have to preserve the callback running state,
 * as otherwise the timer could be removed before the softirq code finishes the
 * the handling of the timer.
 *
 * The HRTIMER_STATE_ENQUEUED bit is always or'ed to the current state
 * to preserve the HRTIMER_STATE_CALLBACK in the above scenario. This
 * also affects HRTIMER_STATE_MIGRATE where the preservation is not
 * necessary. HRTIMER_STATE_MIGRATE is cleared after the timer is
 * enqueued on the new cpu.
 *
 * All state transitions are protected by cpu_base->lock.
 */
/** 20141115
 * HRTIMER의 상태.
 *
 * 0x00		비활성 상태
 * 0x01		enqueue(rb_tree에 추가)된 상태
 * 0x02		CB 함수 동작 중
 * 0x04		다른 CPU로 마이그레이트된 상태
 *
 * Special cases:
 * 0x03		CB 함수 동작 중이며, enqueue된 상태
 *		(다른 CPU에 requeue되었다)
 * 0x05		타이머가 CPU hotunplug 되어 마이그레이트 되었다.
 **/
#define HRTIMER_STATE_INACTIVE	0x00
#define HRTIMER_STATE_ENQUEUED	0x01
#define HRTIMER_STATE_CALLBACK	0x02
#define HRTIMER_STATE_MIGRATE	0x04

/**
 * struct hrtimer - the basic hrtimer structure
 * @node:	timerqueue node, which also manages node.expires,
 *		the absolute expiry time in the hrtimers internal
 *		representation. The time is related to the clock on
 *		which the timer is based. Is setup by adding
 *		slack to the _softexpires value. For non range timers
 *		identical to _softexpires.
 * @_softexpires: the absolute earliest expiry time of the hrtimer.
 *		The time which was given as expiry time when the timer
 *		was armed.
 * @function:	timer expiry callback function
 * @base:	pointer to the timer base (per cpu and per clock)
 * @state:	state information (See bit values above)
 * @start_site:	timer statistics field to store the site where the timer
 *		was started
 * @start_comm: timer statistics field to store the name of the process which
 *		started the timer
 * @start_pid: timer statistics field to store the pid of the task which
 *		started the timer
 *
 * The hrtimer structure must be initialized by hrtimer_init()
 */
/** 20140419
 * hrtimer
 *
 * node : timer queue, expires값과 rb_tree의 entry point를 갖고 있음.
 *	node.expires(hard) : _softexpires + slack(또는 delta로 지정된 값)
 * _softexpires (soft) : timer의 가장 빠른 만료시간.
 *
 * soft, hard expires를 지정하는 API
 *   - hrtimer_set_expires : 동일한 값으로 지정
 *   - hrtimer_set_expires_range : range로 지정
 * 
 * [참고] Greedy hrtimer expiration
 * http://lwn.net/Articles/461592/
 *
 * base : clock base 정보. hrtimer_bases 선언시 초기화 되어 있다.
 *        (BASE_MONOTONIC, REALTIME, BOOTTIME)
 * function : timer 만료시 호출될 함수
 *
 *
 * - sched_timer의 사용예
 * 1. hrtimer_init()으로 timerqueue_node와 hrtimer_clock_base 초기화
 * 2. callback function  지정
 * 3. hrtimer_set_expires()으로 expires 지정
 **/
struct hrtimer {
	struct timerqueue_node		node;
	ktime_t				_softexpires;
	/** 20140419
	 * init_rt_bandwidth 에서 지정
	 **/
	enum hrtimer_restart		(*function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;
	unsigned long			state;
#ifdef CONFIG_TIMER_STATS
	int				start_pid;
	void				*start_site;
	char				start_comm[16];
#endif
};

/**
 * struct hrtimer_sleeper - simple sleeper structure
 * @timer:	embedded timer structure
 * @task:	task to wake up
 *
 * task is set to NULL, when the timer expires.
 */
struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};

/**
 * struct hrtimer_clock_base - the timer base for a specific clock
 * @cpu_base:		per cpu clock base
 * @index:		clock type index for per_cpu support when moving a
 *			timer to a base on another cpu.
 * @clockid:		clock id for per_cpu support
 * @active:		red black tree root node for the active timers
 * @resolution:		the resolution of the clock, in nanoseconds
 * @get_time:		function to retrieve the current time of the clock
 * @softirq_time:	the time when running the hrtimer queue in the softirq
 * @offset:		offset of this clock to the monotonic base
 */
/** 20141108
 * 특정 clock에 대한 timer base 자료구조.
 *
 * cpu_base : hrtimer_clock_base가 위치하는(포함하는) hrtimer_cpu_base를 가리킨다.
 * active   : rb-tree의 head를 가리키는 구조체.
 **/
struct hrtimer_clock_base {
	struct hrtimer_cpu_base	*cpu_base;
	int			index;
	clockid_t		clockid;
	struct timerqueue_head	active;
	ktime_t			resolution;
	ktime_t			(*get_time)(void);
	ktime_t			softirq_time;
	ktime_t			offset;
};

/** 20141101
 * hrtimer의 base type 3가지.
 *   MONOTONIC : 부팅 이후 0부터 시작. 단조 증가.
 *   REALTIME  : 시스템이 파악하는 실제 시간. 시스템 외부 시간에 영향을 받는다.
 *   BOOTTIME  :
 **/
enum  hrtimer_base_type {
	HRTIMER_BASE_MONOTONIC,
	HRTIMER_BASE_REALTIME,
	HRTIMER_BASE_BOOTTIME,
	HRTIMER_MAX_CLOCK_BASES,
};

/*
 * struct hrtimer_cpu_base - the per cpu clock bases
 * @lock:		lock protecting the base and associated clock bases
 *			and timers
 * @active_bases:	Bitfield to mark bases with active timers
 * @clock_was_set:	Indicates that clock was set from irq context.
 * @expires_next:	absolute time of the next event which was scheduled
 *			via clock_set_next_event()
 * @hres_active:	State of high resolution mode
 * @hang_detected:	The last hrtimer interrupt detected a hang
 * @nr_events:		Total number of hrtimer interrupt events
 * @nr_retries:		Total number of hrtimer interrupt retries
 * @nr_hangs:		Total number of hrtimer interrupt hangs
 * @max_hang_time:	Maximum time spent in hrtimer_interrupt
 * @clock_base:		array of clock bases for this cpu
 */
/** 20141108
 * hrtimer 프레임워크를 사용하기 위해 cpu별 clock bases를 가진다.
 *
 * expires_next :	clock_set_next_event()에 의해 설정된, 다음 event가 발생할 절대 시간.
 * lock         : cpu base와 clock bases에 대해 동시에 보호하는 lock.
 * active_bases : clock bases 중 active timer를 보유한 경우 1로 표시
 **/
struct hrtimer_cpu_base {
	raw_spinlock_t			lock;
	unsigned int			active_bases;
	unsigned int			clock_was_set;
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t				expires_next;
	int				hres_active;
	int				hang_detected;
	unsigned long			nr_events;
	unsigned long			nr_retries;
	unsigned long			nr_hangs;
	ktime_t				max_hang_time;
#endif
	struct hrtimer_clock_base	clock_base[HRTIMER_MAX_CLOCK_BASES];
};

/** 20141108
 * hrtimer node의 만료시간을 같은 시간으로 지정한다.
 **/
static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = time;
	timer->_softexpires = time;
}

/** 20141115
 * hrtimer node의 만료시간을 range로 지정한다.
 * soft expires와 delta를 받는다.
 **/
static inline void hrtimer_set_expires_range(struct hrtimer *timer, ktime_t time, ktime_t delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, delta);
}

/** 20141115
 * hrtimer node의 만료시간을 range로 지정한다.
 * soft expires와 delta를 받는다.
 *
 * expires range의 delta가 ns인 경우 사용.
 **/
static inline void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time, unsigned long delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, ns_to_ktime(delta));
}

static inline void hrtimer_set_expires_tv64(struct hrtimer *timer, s64 tv64)
{
	timer->node.expires.tv64 = tv64;
	timer->_softexpires.tv64 = tv64;
}

/** 20140419
 * hrtimer의 expires 값을 time만큼 더한다. ktime_t version.
 **/
static inline void hrtimer_add_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = ktime_add_safe(timer->node.expires, time);
	timer->_softexpires = ktime_add_safe(timer->_softexpires, time);
}

/** 20140419
 * hrtimer의 expires와 _softexpires에 ns만큼을 증가시킨다.
 **/
static inline void hrtimer_add_expires_ns(struct hrtimer *timer, u64 ns)
{
	timer->node.expires = ktime_add_ns(timer->node.expires, ns);
	timer->_softexpires = ktime_add_ns(timer->_softexpires, ns);
}

/** 20140419
 * hrtimer의 node의 expires값을 가져온다.
 **/
static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
	return timer->node.expires;
}

/** 20141108
 * hrtimer의 soft expires값을 가져온다.
 **/
static inline ktime_t hrtimer_get_softexpires(const struct hrtimer *timer)
{
	return timer->_softexpires;
}

/** 20140419
 * hrtimer의 expires 값을 반환한다.
 **/
static inline s64 hrtimer_get_expires_tv64(const struct hrtimer *timer)
{
	return timer->node.expires.tv64;
}
static inline s64 hrtimer_get_softexpires_tv64(const struct hrtimer *timer)
{
	return timer->_softexpires.tv64;
}

static inline s64 hrtimer_get_expires_ns(const struct hrtimer *timer)
{
	return ktime_to_ns(timer->node.expires);
}

static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *timer)
{
	return ktime_sub(timer->node.expires, timer->base->get_time());
}

#ifdef CONFIG_HIGH_RES_TIMERS
/** 20140419
 * vexress의 default config에는 HIGH_RES_TIMERS를 사용하지 않음
 **/
struct clock_event_device;

extern void hrtimer_interrupt(struct clock_event_device *dev);

/*
 * In high resolution mode the time reference must be read accurate
 */
/** 20140419
 * hrtimer clock의 현재 time값을 가져오는 callback 함수 호출
 **/
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->get_time();
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return timer->base->cpu_base->hres_active;
}

extern void hrtimer_peek_ahead_timers(void);

/*
 * The resolution of the clocks. The resolution value is returned in
 * the clock_getres() system call to give application programmers an
 * idea of the (in)accuracy of timers. Timer values are rounded up to
 * this resolution values.
 */
# define HIGH_RES_NSEC		1
# define KTIME_HIGH_RES		(ktime_t) { .tv64 = HIGH_RES_NSEC }
# define MONOTONIC_RES_NSEC	HIGH_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_HIGH_RES

extern void clock_was_set_delayed(void);

#else

# define MONOTONIC_RES_NSEC	LOW_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_LOW_RES

static inline void hrtimer_peek_ahead_timers(void) { }

/*
 * In non high resolution mode the time reference is taken from
 * the base softirq time variable.
 */
/** 20140419
 * softirq_time값을 리턴
 **/
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->softirq_time;
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return 0;
}

static inline void clock_was_set_delayed(void) { }

#endif

extern void clock_was_set(void);
#ifdef CONFIG_TIMERFD
extern void timerfd_clock_was_set(void);
#else
static inline void timerfd_clock_was_set(void) { }
#endif
extern void hrtimers_resume(void);

extern ktime_t ktime_get(void);
extern ktime_t ktime_get_real(void);
extern ktime_t ktime_get_boottime(void);
extern ktime_t ktime_get_monotonic_offset(void);
extern ktime_t ktime_get_update_offsets(ktime_t *offs_real, ktime_t *offs_boot);

DECLARE_PER_CPU(struct tick_device, tick_cpu_device);


/* Exported timer functions: */

/* Initialize timers: */
extern void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
			 enum hrtimer_mode mode);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t which_clock,
				  enum hrtimer_mode mode);

extern void destroy_hrtimer_on_stack(struct hrtimer *timer);
#else
static inline void hrtimer_init_on_stack(struct hrtimer *timer,
					 clockid_t which_clock,
					 enum hrtimer_mode mode)
{
	hrtimer_init(timer, which_clock, mode);
}
static inline void destroy_hrtimer_on_stack(struct hrtimer *timer) { }
#endif

/* Basic timer operations: */
extern int hrtimer_start(struct hrtimer *timer, ktime_t tim,
			 const enum hrtimer_mode mode);
extern int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			unsigned long range_ns, const enum hrtimer_mode mode);
extern int
__hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			 unsigned long delta_ns,
			 const enum hrtimer_mode mode, int wakeup);

extern int hrtimer_cancel(struct hrtimer *timer);
extern int hrtimer_try_to_cancel(struct hrtimer *timer);

/** 20141129
 * 현재 cpu에 hrtimer가 expire되도록 등록한다.
 **/
static inline int hrtimer_start_expires(struct hrtimer *timer,
						enum hrtimer_mode mode)
{
	unsigned long delta;
	ktime_t soft, hard;
	/** 20141115
	 * hrtimer의 hard, soft expires의 차를 구하고,
	 * 이 값들을 사용하여 hrtimer_start_range_ns 인터페이스를 통해 호출한다.
	 **/
	soft = hrtimer_get_softexpires(timer);
	hard = hrtimer_get_expires(timer);
	delta = ktime_to_ns(ktime_sub(hard, soft));
	return hrtimer_start_range_ns(timer, soft, delta, mode);
}

static inline int hrtimer_restart(struct hrtimer *timer)
{
	return hrtimer_start_expires(timer, HRTIMER_MODE_ABS);
}

/* Query timers: */
extern ktime_t hrtimer_get_remaining(const struct hrtimer *timer);
extern int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp);

extern ktime_t hrtimer_get_next_event(void);

/*
 * A timer is active, when it is enqueued into the rbtree or the
 * callback function is running or it's in the state of being migrated
 * to another cpu.
 */
/** 20141129
 * hrtimer가 INACTIVE 상태가 아니라면 active이다.
 * (ENQUEUED, CALLBACK, MIGRATE)
 **/
static inline int hrtimer_active(const struct hrtimer *timer)
{
	return timer->state != HRTIMER_STATE_INACTIVE;
}

/*
 * Helper function to check, whether the timer is on one of the queues
 */
/** 20141108
 * timer가 현재 timerqueue에 포함되어 있는지 검사.
 **/
static inline int hrtimer_is_queued(struct hrtimer *timer)
{
	return timer->state & HRTIMER_STATE_ENQUEUED;
}

/*
 * Helper function to check, whether the timer is running the callback
 * function
 */
static inline int hrtimer_callback_running(struct hrtimer *timer)
{
	return timer->state & HRTIMER_STATE_CALLBACK;
}

/* Forward a hrtimer so it expires after now: */
extern u64
hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);

/* Forward a hrtimer so it expires after the hrtimer's current now */
static inline u64 hrtimer_forward_now(struct hrtimer *timer,
				      ktime_t interval)
{
	return hrtimer_forward(timer, timer->base->get_time(), interval);
}

/* Precise sleep: */
extern long hrtimer_nanosleep(struct timespec *rqtp,
			      struct timespec __user *rmtp,
			      const enum hrtimer_mode mode,
			      const clockid_t clockid);
extern long hrtimer_nanosleep_restart(struct restart_block *restart_block);

extern void hrtimer_init_sleeper(struct hrtimer_sleeper *sl,
				 struct task_struct *tsk);

extern int schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
						const enum hrtimer_mode mode);
extern int schedule_hrtimeout_range_clock(ktime_t *expires,
		unsigned long delta, const enum hrtimer_mode mode, int clock);
extern int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);

/* Soft interrupt function to run the hrtimer queues: */
extern void hrtimer_run_queues(void);
extern void hrtimer_run_pending(void);

/* Bootup initialization: */
extern void __init hrtimers_init(void);

#if BITS_PER_LONG < 64
extern u64 ktime_divns(const ktime_t kt, s64 div);
#else /* BITS_PER_LONG < 64 */
# define ktime_divns(kt, div)		(u64)((kt).tv64 / (div))
#endif

/* Show pending timers: */
extern void sysrq_timer_list_show(void);

#endif
