#ifndef _LINUX_TIMER_H
#define _LINUX_TIMER_H

#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/stddef.h>
#include <linux/debugobjects.h>
#include <linux/stringify.h>

struct tvec_base;
/** 20130601
 * per_cpu 변수인 tvec_bases의 tvN의 리스트에 등록되는 자료구조.
 * expires와 만료시 호출할 function으로 이뤄져 있다.
 *
 * .tvec_base : timer_list가 등록되는 tvec_base
 *
 * 해당 timer가 deferrable일 경우 tvec_base 포인터의 LSB 비트를 1로 설정해 표시한다.
**/
struct timer_list {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head entry;
	unsigned long expires;
	struct tvec_base *base;

	void (*function)(unsigned long);
	unsigned long data;

	int slack;

#ifdef CONFIG_TIMER_STATS
	int start_pid;
	void *start_site;
	char start_comm[16];
#endif
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;
#endif
};

extern struct tvec_base boot_tvec_bases;

#ifdef CONFIG_LOCKDEP
/*
 * NB: because we have to copy the lockdep_map, setting the lockdep_map key
 * (second argument) here is required, otherwise it could be initialised to
 * the copy of the lockdep_map later! We use the pointer to and the string
 * "<file>:<line>" as the key resp. the name of the lockdep_map.
 */
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)				\
	.lockdep_map = STATIC_LOCKDEP_MAP_INIT(_kn, &_kn),
#else
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)
#endif

/*
 * Note that all tvec_bases are 2 byte aligned and lower bit of
 * base in timer_list is guaranteed to be zero. Use the LSB to
 * indicate whether the timer is deferrable.
 *
 * A deferrable timer will work normally when the system is busy, but
 * will not cause a CPU to come out of idle just to service it; instead,
 * the timer will be serviced when the CPU eventually wakes up with a
 * subsequent non-deferrable timer.
 */
/** 20150627
 * tvec_bases는 2바이트 정렬되어 있고, timer_list 내의 base의 lower bit는 0임이 보장된다.
 * 이 LSB는 timer가 deferrable 가능 여부를 나타낸다.
 **/
#define TBASE_DEFERRABLE_FLAG		(0x1)

#define TIMER_INITIALIZER(_function, _expires, _data) {		\
		.entry = { .prev = TIMER_ENTRY_STATIC },	\
		.function = (_function),			\
		.expires = (_expires),				\
		.data = (_data),				\
		.base = &boot_tvec_bases,			\
		.slack = -1,					\
		__TIMER_LOCKDEP_MAP_INITIALIZER(		\
			__FILE__ ":" __stringify(__LINE__))	\
	}

/** 20150704
 * tvec_base 포인터의 LSB 한 비트를 켜 지연가능한 tbase로 만든다.
 **/
#define TBASE_MAKE_DEFERRED(ptr) ((struct tvec_base *)		\
		  ((unsigned char *)(ptr) + TBASE_DEFERRABLE_FLAG))

#define TIMER_DEFERRED_INITIALIZER(_function, _expires, _data) {\
		.entry = { .prev = TIMER_ENTRY_STATIC },	\
		.function = (_function),			\
		.expires = (_expires),				\
		.data = (_data),				\
		.base = TBASE_MAKE_DEFERRED(&boot_tvec_bases),	\
		__TIMER_LOCKDEP_MAP_INITIALIZER(		\
			__FILE__ ":" __stringify(__LINE__))	\
	}

/** 20150103
 * timer_list 하나를 정의한다.
 *
 * timer_list 이름, 콜백함수, 만료시간, data를 전달 받는다.
 **/
#define DEFINE_TIMER(_name, _function, _expires, _data)		\
	struct timer_list _name =				\
		TIMER_INITIALIZER(_function, _expires, _data)

void init_timer_key(struct timer_list *timer,
		    const char *name,
		    struct lock_class_key *key);
void init_timer_deferrable_key(struct timer_list *timer,
			       const char *name,
			       struct lock_class_key *key);

#ifdef CONFIG_LOCKDEP
#define init_timer(timer)						\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_key((timer), #timer, &__key);		\
	} while (0)

#define init_timer_deferrable(timer)					\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_deferrable_key((timer), #timer, &__key);	\
	} while (0)

#define init_timer_on_stack(timer)					\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_on_stack_key((timer), #timer, &__key);	\
	} while (0)

#define setup_timer(timer, fn, data)					\
	do {								\
		static struct lock_class_key __key;			\
		setup_timer_key((timer), #timer, &__key, (fn), (data));\
	} while (0)

#define setup_timer_on_stack(timer, fn, data)				\
	do {								\
		static struct lock_class_key __key;			\
		setup_timer_on_stack_key((timer), #timer, &__key,	\
					 (fn), (data));			\
	} while (0)
#define setup_deferrable_timer_on_stack(timer, fn, data)		\
	do {								\
		static struct lock_class_key __key;			\
		setup_deferrable_timer_on_stack_key((timer), #timer,	\
						    &__key, (fn),	\
						    (data));		\
	} while (0)
#else
/** 20150221
 * LOCKDEP이 설정되어 있지 않음.
 **/
#define init_timer(timer)\
	init_timer_key((timer), NULL, NULL)
#define init_timer_deferrable(timer)\
	init_timer_deferrable_key((timer), NULL, NULL)
#define init_timer_on_stack(timer)\
	init_timer_on_stack_key((timer), NULL, NULL)
/** 20150221
 * timer를 주어진 fn과 data로 설정한다.
 **/
#define setup_timer(timer, fn, data)\
	setup_timer_key((timer), NULL, NULL, (fn), (data))
/** 20140628
 * timer를 주어진 fn과 data로 설정한다. key는 stack 변수를 사용한다.
 **/
#define setup_timer_on_stack(timer, fn, data)\
	setup_timer_on_stack_key((timer), NULL, NULL, (fn), (data))
#define setup_deferrable_timer_on_stack(timer, fn, data)\
	setup_deferrable_timer_on_stack_key((timer), NULL, NULL, (fn), (data))
#endif

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void init_timer_on_stack_key(struct timer_list *timer,
				    const char *name,
				    struct lock_class_key *key);
extern void destroy_timer_on_stack(struct timer_list *timer);
#else
/** 20140628
 * CONFIG_DEBUG_OBJECTS_TIMERS 선언되지 않아 NULL 함수.
 **/
static inline void destroy_timer_on_stack(struct timer_list *timer) { }
static inline void init_timer_on_stack_key(struct timer_list *timer,
					   const char *name,
					   struct lock_class_key *key)
{
	init_timer_key(timer, name, key);
}
#endif

/** 20150221
 * timer를 주어진 argument로 설정한다.
 **/
static inline void setup_timer_key(struct timer_list * timer,
				const char *name,
				struct lock_class_key *key,
				void (*function)(unsigned long),
				unsigned long data)
{
	timer->function = function;
	timer->data = data;
	init_timer_key(timer, name, key);
}

/** 20140628
 * timer를 주어진 function과 data로 설정한다.
 **/
static inline void setup_timer_on_stack_key(struct timer_list *timer,
					const char *name,
					struct lock_class_key *key,
					void (*function)(unsigned long),
					unsigned long data)
{
	timer->function = function;
	timer->data = data;
	init_timer_on_stack_key(timer, name, key);
}

extern void setup_deferrable_timer_on_stack_key(struct timer_list *timer,
						const char *name,
						struct lock_class_key *key,
						void (*function)(unsigned long),
						unsigned long data);

/**
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
/** 20150704
 * timer의 list entry가 존재한다면 timer가 이미 어딘가에 등록되어 있다.
 **/
static inline int timer_pending(const struct timer_list * timer)
{
	return timer->entry.next != NULL;
}

extern void add_timer_on(struct timer_list *timer, int cpu);
extern int del_timer(struct timer_list * timer);
extern int mod_timer(struct timer_list *timer, unsigned long expires);
extern int mod_timer_pending(struct timer_list *timer, unsigned long expires);
extern int mod_timer_pinned(struct timer_list *timer, unsigned long expires);

extern void set_timer_slack(struct timer_list *time, int slack_hz);

#define TIMER_NOT_PINNED	0
#define TIMER_PINNED		1
/*
 * The jiffies value which is added to now, when there is no timer
 * in the timer wheel:
 */
#define NEXT_TIMER_MAX_DELTA	((1UL << 30) - 1)

/*
 * Return when the next timer-wheel timeout occurs (in absolute jiffies),
 * locks the timer base and does the comparison against the given
 * jiffie.
 */
extern unsigned long get_next_timer_interrupt(unsigned long now);

/*
 * Timer-statistics info:
 */
#ifdef CONFIG_TIMER_STATS

extern int timer_stats_active;

#define TIMER_STATS_FLAG_DEFERRABLE	0x1

extern void init_timer_stats(void);

extern void timer_stats_update_stats(void *timer, pid_t pid, void *startf,
				     void *timerf, char *comm,
				     unsigned int timer_flag);

extern void __timer_stats_timer_set_start_info(struct timer_list *timer,
					       void *addr);

static inline void timer_stats_timer_set_start_info(struct timer_list *timer)
{
	if (likely(!timer_stats_active))
		return;
	__timer_stats_timer_set_start_info(timer, __builtin_return_address(0));
}

static inline void timer_stats_timer_clear_start_info(struct timer_list *timer)
{
	timer->start_site = NULL;
}
#else
/** 20140920
 * CONFIG_TIMER_STATS 가 선언되지 않아 NULL 함수.
 **/
static inline void init_timer_stats(void)
{
}

/** 20140628
 * CONFIG_TIMER_STATS 가 선언되지 않아 NULL 함수.
 **/
static inline void timer_stats_timer_set_start_info(struct timer_list *timer)
{
}

static inline void timer_stats_timer_clear_start_info(struct timer_list *timer)
{
}
#endif

extern void add_timer(struct timer_list *timer);

extern int try_to_del_timer_sync(struct timer_list *timer);

#ifdef CONFIG_SMP
  extern int del_timer_sync(struct timer_list *timer);
#else
# define del_timer_sync(t)		del_timer(t)
#endif

#define del_singleshot_timer_sync(t) del_timer_sync(t)

extern void init_timers(void);
extern void run_local_timers(void);
struct hrtimer;
extern enum hrtimer_restart it_real_fn(struct hrtimer *);

unsigned long __round_jiffies(unsigned long j, int cpu);
unsigned long __round_jiffies_relative(unsigned long j, int cpu);
unsigned long round_jiffies(unsigned long j);
unsigned long round_jiffies_relative(unsigned long j);

unsigned long __round_jiffies_up(unsigned long j, int cpu);
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu);
unsigned long round_jiffies_up(unsigned long j);
unsigned long round_jiffies_up_relative(unsigned long j);

#endif
