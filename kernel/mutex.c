/*
 * kernel/mutex.c
 *
 * Mutexes: blocking mutual exclusion locks
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * Many thanks to Arjan van de Ven, Thomas Gleixner, Steven Rostedt and
 * David Howells for suggestions and improvements.
 *
 *  - Adaptive spinning for mutexes by Peter Zijlstra. (Ported to mainline
 *    from the -rt tree, where it was originally implemented for rtmutexes
 *    by Steven Rostedt, based on work by Gregory Haskins, Peter Morreale
 *    and Sven Dietrich.
 *
 * Also see Documentation/mutex-design.txt.
 */
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>

/*
 * In the DEBUG case we are using the "NULL fastpath" for mutexes,
 * which forces all calls into the slowpath:
 */
#ifdef CONFIG_DEBUG_MUTEXES
# include "mutex-debug.h"
# include <asm-generic/mutex-null.h>
#else
# include "mutex.h"
# include <asm/mutex.h>
#endif

/** 20121117
 * struct mutex 자료구조를 초기화 한다. 
 * 	mutex가 spin_lock을 통해 구현되는듯. ???
 *
 * 20150905
 * mutex 내에서 사용되는 spinlock은 wait_list를 관리하기 위한 것이다.
 * mutex_lock()/mutex_unlock()에서 핵심 변수인 owner는 원자적 연산으로 접근한다.
 */
void
__mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_set(&lock->count, 1);
	spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
	mutex_clear_owner(lock);

	debug_mutex_init(lock, name, key);
}

EXPORT_SYMBOL(__mutex_init);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * We split the mutex lock/unlock logic into separate fastpath and
 * slowpath functions, to reduce the register pressure on the fastpath.
 * We also put the fastpath first in the kernel image, to make sure the
 * branch is predicted by the CPU as default-untaken.
 */
static __used noinline void __sched
__mutex_lock_slowpath(atomic_t *lock_count);

/**
 * mutex_lock - acquire the mutex
 * @lock: the mutex to be acquired
 *
 * Lock the mutex exclusively for this task. If the mutex is not
 * available right now, it will sleep until it can get it.
 *
 * The mutex must later on be released by the same task that
 * acquired it. Recursive locking is not allowed. The task
 * may not exit without first unlocking the mutex. Also, kernel
 * memory where the mutex resides mutex must not be freed with
 * the mutex still locked. The mutex must first be initialized
 * (or statically defined) before it can be locked. memset()-ing
 * the mutex to 0 is not allowed.
 *
 * ( The CONFIG_DEBUG_MUTEXES .config option turns on debugging
 *   checks that will enforce the restrictions and will also do
 *   deadlock debugging. )
 *
 * This function is similar to (but not equivalent to) down().
 */
/** 20130706
 * mutex lock.
 **/
void __sched mutex_lock(struct mutex *lock)
{
	/** 20130706
	 * preemption point를 둔다.
	 **/
	might_sleep();
	/*
	 * The locking fastpath is the 1->0 transition from
	 * 'unlocked' into 'locked' state.
	 */
	__mutex_fastpath_lock(&lock->count, __mutex_lock_slowpath);
	/** 20130713
	 * lock의 owner로 지정
	 **/
	mutex_set_owner(lock);
}

EXPORT_SYMBOL(mutex_lock);
#endif

static __used noinline void __sched __mutex_unlock_slowpath(atomic_t *lock_count);

/**
 * mutex_unlock - release the mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a not locked mutex is not allowed.
 *
 * This function is similar to (but not equivalent to) up().
 */
/** 20130720
 * mutex unlock 함수.
 * mutex lock과 다른 부분은 wait_list에 대기 중인 task가 있다면 가져와 실행시킨다.
 **/
void __sched mutex_unlock(struct mutex *lock)
{
	/*
	 * The unlocking fastpath is the 0->1 transition from 'locked'
	 * into 'unlocked' state:
	 */
#ifndef CONFIG_DEBUG_MUTEXES
	/*
	 * When debugging is enabled we must not clear the owner before time,
	 * the slow path will always be taken, and that clears the owner field
	 * after verifying that it was indeed current.
	 */
	/** 20130713
	 * lock의 owner 해제
	 **/
	mutex_clear_owner(lock);
#endif
	/** 20130720
	 * __mutex_fastpath_unlock는 __mutex_unlock_slowpath를 호출한다.
	 **/
	__mutex_fastpath_unlock(&lock->count, __mutex_unlock_slowpath);
}

EXPORT_SYMBOL(mutex_unlock);

/*
 * Lock a mutex (possibly interruptible), slowpath:
 */
/** 20130706
 * case: __mutex_lock_slowpath
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);

	mutex lock 핵심함수
	1. owner가 없다면 자신을 owner로 지정하고 lock을 획득한다.
	2. owner가 있다면
	   spin_on_onwer로 owner가 lock 을 해제할 때까지 대기한다.
	     lock이 해제된 후 owner가 없다면 lock을 획득한다.
		                          있다면 wait_list에 자신을 추가하고 schedule()
    2.5 state가 TASK_INTERRUPTIBLE이고 pending된 signal이 있다면 -EINTR로 리턴
	            TASK_KILLABLE이고 SIGKILL이 pending 되어 있다면  -EINTR로 리턴
	3. 이후 sleep에서 깨어났을 때
	   lock->count를 확인해 1이면 wait_list에서 자신을 제거하고 lock을 획득.
	                        1이 아니면 다시 sleep 상태로 진입.
 **/
static inline int __sched
__mutex_lock_common(struct mutex *lock, long state, unsigned int subclass,
		    struct lockdep_map *nest_lock, unsigned long ip)
{
	struct task_struct *task = current;
	struct mutex_waiter waiter;
	unsigned long flags;

	/** 20130706
	 * lock을 소유한 채 선점당하면 안 되므로 선점 불가로 설정.
	 **/
	preempt_disable();
	/** 20130706
	 * mutex debugging을 위해 호출.
	 **/
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	/** 20130706
	 *   긍정적인 spinning.
	 *
	 *   pending된 대기자가 없고, lock owner가 다른 CPU에서 수행되고 있다면,
	 *   lock을 획득하기 위해 spin을 시도한다.
	 *
	 *   이렇게 하는 이유는 lock owner가 돌고 있다면(running) 아마도 lock을 금방 해제할 것이라 기대하기 때문이다.
	 *
	 *   이런 방식으로 사용하기 위해서는 lock owner가 필요하고,
	 *   이 뮤텍스 구현 방식은 lock field에서 owner를 원자적으로 추적하지 않는다.
	 **/
	/*
	 * Optimistic spinning.
	 *
	 * We try to spin for acquisition when we find that there are no
	 * pending waiters and the lock owner is currently running on a
	 * (different) CPU.
	 *
	 * The rationale is that if the lock owner is running, it is likely to
	 * release the lock soon.
	 *
	 * Since this needs the lock owner, and this mutex implementation
	 * doesn't track the owner atomically in the lock field, we need to
	 * track it non-atomically.
	 *
	 * We can't do this for DEBUG_MUTEXES because that relies on wait_lock
	 * to serialize everything.
	 */

	for (;;) {
		struct task_struct *owner;

		/*
		 * If there's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */
		/** 20130706
		 * 메모리에 접근해 lock->owner의 값을 가져온다.
		 **/
		owner = ACCESS_ONCE(lock->owner);
		/** 20130706
		 * 위에서 가져온 owner 정보가 변경되었는지 검사한다.
		 * lock->owner가 NULL인 경우에만 true 리턴.
		 *
		 * mutex_spin_on_owner가 리턴되는 경우
		 *   1. lock->owner와 owner가 불일치 하는 경우
		 *   2. context_switch 되어 on_cpu가 0이 된 경우
		 *   true  - lock->owner == NULL
		 *		-> lock 획득을 시도
		 *   false - lock->owner != NULL
		 *      -> break;
		 **/
		if (owner && !mutex_spin_on_owner(lock, owner))
			break;

		/** 20130706
		 * atomic_cmpxchg에서 검사한 이전값이 1이라면 아래 문장 수행.
		 * 1->0을 원자적으로 수행하는데 이전 값이 1이라면 수행
		 *   -> 자신을 owner로 지정 하고 리턴.
		 **/
		if (atomic_cmpxchg(&lock->count, 1, 0) == 1) {
			/** 20130706
			 * LOCK DEBUG를 사용하지 않아 NULL 함수
			 **/
			lock_acquired(&lock->dep_map, ip);
			/** 20130706
			 * mutex의 owner를 현재 task로 설정
			 **/
			mutex_set_owner(lock);
			/** 20130706
			 * preempt를 사용한다면 enable로 돌려놓고 return.
			 * vexpress 기본 설정에서는 사용하지 않음.
			 **/
			preempt_enable();
			return 0;
		}

		/*
		 * When there's no owner, we might have preempted between the
		 * owner acquiring the lock and setting the owner field. If
		 * we're an RT task that will live-lock because we won't let
		 * the owner complete.
		 */
		/** 20130706
		 * 처음 owner가 NULL이거나, spin으로 대기 후 NULL로 변경되었을 때와
		 * atomic_cmpxchg로 count를 감소시키기 전에 선점이 되었다면,
		 * 이곳까지 진행되었을 것이다.
		 *
		 * owner가 NULL이고
		 * need_resched()가 참이거나, 즉 rescheduling이 필요하거나
		 * 현재 task가 rt task라면 for문을 빠져나간다.
		 **/
		if (!owner && (need_resched() || rt_task(task)))
			break;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		arch_mutex_cpu_relax();
	}
#endif
	/** 20130713
	 * mutex의 wait_list에 대한 wait_lock을 spin_lock으로 획득.
	 **/
	spin_lock_mutex(&lock->wait_lock, flags);

	debug_mutex_lock_common(lock, &waiter);
	debug_mutex_add_waiter(lock, &waiter, task_thread_info(task));

	/* add waiting tasks to the end of the waitqueue (FIFO): */
	/** 20130713
	 * lock의 wait_list에 waiter를 추가
	 *   waiter는 지역변수인데 wait_list에 추가???
	 **/
	list_add_tail(&waiter.list, &lock->wait_list);
	/** 20130713
	 * task(current)를 waiter.task로 지정.
	 **/
	waiter.task = task;

	/** 20130713
	 * lock->count를 -1로 변경.
	 * 이전값이 1이라면 unlocked로 wait_list에 넣어줄 필요가 없어 done으로 이동.
	 **/
	if (atomic_xchg(&lock->count, -1) == 1)
		goto done;

	/** 20130713
	 * debug 함수 호출
	 **/
	lock_contended(&lock->dep_map, ip);

	for (;;) {
		/*
		 * Lets try to take the lock again - this is needed even if
		 * we get here for the first time (shortly after failing to
		 * acquire the lock), to make sure that we get a wakeup once
		 * it's unlocked. Later on, if we sleep, this is the
		 * operation that gives us the lock. We xchg it to -1, so
		 * that when we release the lock, we properly wake up the
		 * other waiters:
		 */
		/** 20130713
		 * loop을 돌 때마다 lock->count를 -1로 변경 (unlock에 의해 상태가 변경되었을 경우에도),
		 * 이전 값이 1일 경우(unlocked) break
		 *   -> wait_list에 들어가서 sleep되어 있다가 unlock 함수에 의해 깨어날 경우, lock->count가 1이라면 unlock 상태이므로 루프를 벗어난다.
		 **/
		if (atomic_xchg(&lock->count, -1) == 1)
			break;

		/*
		 * got a signal? (This code gets eliminated in the
		 * TASK_UNINTERRUPTIBLE case.)
		 */
		/** 20130713
		 * task의 state에 따라 pending된 signal을 처리해야 할 경우
		 *   ex) state가 TASK_UNINTERRUPTIBLE일 경우 false
		 * wait_list에서 제거하고, EINTR을 리턴.
		 *
		 * task state에 대해서...
		 * [참고]  http://www.test104.com/kr/tech/3844.html
		 **/
		if (unlikely(signal_pending_state(state, task))) {
			/** 20130713
			 * waiter를 wait_list에서 제거.
			 **/
			mutex_remove_waiter(lock, &waiter,
					    task_thread_info(task));
			/** 20130713
			 * DEBUG용
			 **/
			mutex_release(&lock->dep_map, 1, ip);
			/** 20130713
			 * wait_lock을 해제.
			 **/
			spin_unlock_mutex(&lock->wait_lock, flags);

			/** 20130713
			 * NULL 함수
			 **/
			debug_mutex_free_waiter(&waiter);
			/** 20130713
			 * 선점 가능하게 변경.
			 * preemption을 사용하지 않아  NULL 함수.
			 **/
			preempt_enable();
			return -EINTR;
		}
		/** 20130713
		 * task의 state를 지정.
		 *   ex) task를 TASK_UNINTERRUPT로 설정.
		 **/
		__set_task_state(task, state);

		/* didn't get the lock, go to sleep: */
		/** 20130713
		 * wait_lock을 해제
		 **/
		spin_unlock_mutex(&lock->wait_lock, flags);
		/** 20130713
		 * scheduling 수행.
		 **/
		schedule_preempt_disabled();
		/** 20130713
		 * sleep에서 벗어났을 경우 wait_lock에 lock을 다시 건다.
		 **/
		spin_lock_mutex(&lock->wait_lock, flags);
	}

done:
	/** 20130713
	 * DEBUG용 함수
	 **/
	lock_acquired(&lock->dep_map, ip);
	/* got the lock - rejoice! */
	/** 20130713
	 * 자신을 wait_list에서 제거.
	 **/
	mutex_remove_waiter(lock, &waiter, current_thread_info());
	/** 20130713
	 * lock의 owner로 자신을 등록.
	 **/
	mutex_set_owner(lock);

	/* set it to 0 if there are no waiters left: */
	/** 20130713
	 * wait_list가 비어 있다면 lock->count를 0으로 만든다.
	 **/
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	/** 20130713
	 * wait_lock의 lock을 해제.
	 **/
	spin_unlock_mutex(&lock->wait_lock, flags);

	/** 20130713
	 * mutex lock DEBUG용 함수
	 **/
	debug_mutex_free_waiter(&waiter);
	/** 20130713
	 * 선점 가능으로 변경
	 **/
	preempt_enable();

	return 0;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __sched
mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
}

EXPORT_SYMBOL_GPL(mutex_lock_nested);

void __sched
_mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, nest, _RET_IP_);
}

EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

int __sched
mutex_lock_killable_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_KILLABLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_killable_nested);

int __sched
mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE,
				   subclass, NULL, _RET_IP_);
}

EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);
#endif

/*
 * Release the lock, slowpath:
 */
/** 20130720
 * wait_list에서 waiter를 가져와 깨워준다
 **/
static inline void
__mutex_unlock_common_slowpath(atomic_t *lock_count, int nested)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);
	unsigned long flags;

	/** 20130713
	 * wait_lock을 spin_lock으로 잡는다.
	 **/
	spin_lock_mutex(&lock->wait_lock, flags);
	/** 20130713
	 * NULL 함수
	 **/
	mutex_release(&lock->dep_map, nested, _RET_IP_);
	/** 20130713
	 * MUTEX DEBUG용 함수.
	 **/
	debug_mutex_unlock(lock);

	/*
	 * some architectures leave the lock unlocked in the fastpath failure
	 * case, others need to leave it locked. In the later case we have to
	 * unlock it here
	 */
	/** 20130713
	 * include/asm-generic/mutex-xchg.h
	 *     #define __mutex_slowpath_needs_to_unlock()		0
	 **/
	if (__mutex_slowpath_needs_to_unlock())
		atomic_set(&lock->count, 1);

	/** 20130713
	 * wait_list가 비어 있지 않으면 수행
	 **/
	if (!list_empty(&lock->wait_list)) {
		/* get the first entry from the wait-list: */
		/** 20130713
		 * list_entry로 wait_list의 next가 가리키는 멤버를 포함한 구조체를 가져온다.
		 **/
		struct mutex_waiter *waiter =
				list_entry(lock->wait_list.next,
					   struct mutex_waiter, list);

		/** 20130713
		 * DEBUG용 함수
		 **/
		debug_mutex_wake_waiter(lock, waiter);

		/** 20130720
		 * wait_list에서 가져온 waiter를 깨운다.
		 * (runqueue에 넣는다)
		 **/
		wake_up_process(waiter->task);
	}

	/** 20130720
	 * wait_lock을 해제하고 flags에 저장된 cpsr을 복원
	 **/
	spin_unlock_mutex(&lock->wait_lock, flags);
}

/*
 * Release the lock, slowpath:
 */
/** 20130720
 * mutex unlock하고 wait_list에 entry가 있다면 깨운다.
 **/
static __used noinline void
__mutex_unlock_slowpath(atomic_t *lock_count)
{
	/** 20130713
	 * lock_count와 nested를 1로 전달
	 **/
	__mutex_unlock_common_slowpath(lock_count, 1);
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Here come the less common (and hence less performance-critical) APIs:
 * mutex_lock_interruptible() and mutex_trylock().
 */
static noinline int __sched
__mutex_lock_killable_slowpath(atomic_t *lock_count);

static noinline int __sched
__mutex_lock_interruptible_slowpath(atomic_t *lock_count);

/**
 * mutex_lock_interruptible - acquire the mutex, interruptible
 * @lock: the mutex to be acquired
 *
 * Lock the mutex like mutex_lock(), and return 0 if the mutex has
 * been acquired or sleep until the mutex becomes available. If a
 * signal arrives while waiting for the lock then this function
 * returns -EINTR.
 *
 * This function is similar to (but not equivalent to) down_interruptible().
 */
int __sched mutex_lock_interruptible(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret =  __mutex_fastpath_lock_retval
			(&lock->count, __mutex_lock_interruptible_slowpath);
	if (!ret)
		mutex_set_owner(lock);

	return ret;
}

EXPORT_SYMBOL(mutex_lock_interruptible);

int __sched mutex_lock_killable(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret = __mutex_fastpath_lock_retval
			(&lock->count, __mutex_lock_killable_slowpath);
	if (!ret)
		mutex_set_owner(lock);

	return ret;
}
EXPORT_SYMBOL(mutex_lock_killable);

/** 20130713
 * mutex lock 함수
 *
 *   mutex lock 관련 코드는 noinline으로 명시적으로 선언.
 *   섹션 위치는 .sched.text로 지정
 **/
static __used noinline void __sched
__mutex_lock_slowpath(atomic_t *lock_count)
{
	/** 20130706
	 * lock_count를 가지고 있는 mutex 자료구조를 lock으로 가리킴
	 **/
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	/** 20130713
	 * mutex lock 획득.
	 *
	 * TASK_UNINTERRUPTIBLE로 호출되었기 때문에 -EINTR로 리턴되지 않는다.
	 **/
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_killable_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	return __mutex_lock_common(lock, TASK_KILLABLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_interruptible_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, 0, NULL, _RET_IP_);
}
#endif

/*
 * Spinlock based trylock, we take the spinlock and check whether we
 * can get the lock:
 */
static inline int __mutex_trylock_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);
	unsigned long flags;
	int prev;

	spin_lock_mutex(&lock->wait_lock, flags);

	prev = atomic_xchg(&lock->count, -1);
	if (likely(prev == 1)) {
		mutex_set_owner(lock);
		mutex_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	}

	/* Set it back to 0 if there are no waiters: */
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock_mutex(&lock->wait_lock, flags);

	return prev == 1;
}

/**
 * mutex_trylock - try to acquire the mutex, without waiting
 * @lock: the mutex to be acquired
 *
 * Try to acquire the mutex atomically. Returns 1 if the mutex
 * has been acquired successfully, and 0 on contention.
 *
 * NOTE: this function follows the spin_trylock() convention, so
 * it is negated from the down_trylock() return values! Be careful
 * about this when converting semaphore users to mutexes.
 *
 * This function must not be used in interrupt context. The
 * mutex must be released by the same task that acquired it.
 */
int __sched mutex_trylock(struct mutex *lock)
{
	int ret;

	ret = __mutex_fastpath_trylock(&lock->count, __mutex_trylock_slowpath);
	if (ret)
		mutex_set_owner(lock);

	return ret;
}
EXPORT_SYMBOL(mutex_trylock);

/**
 * atomic_dec_and_mutex_lock - return holding mutex if we dec to 0
 * @cnt: the atomic which we are to dec
 * @lock: the mutex to return holding if we dec to 0
 *
 * return true and hold lock if we dec to 0, return false otherwise
 */
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	/* dec if we can't possibly hit 0 */
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	/* we might hit 0, so take the lock */
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		/* when we actually did the dec, we didn't hit 0 */
		mutex_unlock(lock);
		return 0;
	}
	/* we hit 0, and we hold the lock */
	return 1;
}
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);
