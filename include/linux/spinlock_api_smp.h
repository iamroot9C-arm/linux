#ifndef __LINUX_SPINLOCK_API_SMP_H
#define __LINUX_SPINLOCK_API_SMP_H

#ifndef __LINUX_SPINLOCK_H
# error "please don't include this file directly"
#endif

/*
 * include/linux/spinlock_api_smp.h
 *
 * spinlock API declarations on SMP (and debug)
 * (implemented in kernel/spinlock.c)
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 */

int in_lock_functions(unsigned long addr);

/** 20130713    
 * spinlock이 잠겨 있지 않다면 BUG.
 **/
#define assert_raw_spin_locked(x)	BUG_ON(!raw_spin_is_locked(x))

/** 20130706    
 * __acquires는 attribute
 **/
void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)		__acquires(lock);
void __lockfunc _raw_spin_lock_nested(raw_spinlock_t *lock, int subclass)
								__acquires(lock);
void __lockfunc
_raw_spin_lock_nest_lock(raw_spinlock_t *lock, struct lockdep_map *map)
								__acquires(lock);
void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)		__acquires(lock);
void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
								__acquires(lock);

/** 20131005    
 * 이곳에서 선언
 **/
unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
								__acquires(lock);
unsigned long __lockfunc
_raw_spin_lock_irqsave_nested(raw_spinlock_t *lock, int subclass)
								__acquires(lock);
int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock);
int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock);
void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)		__releases(lock);
void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)	__releases(lock);
void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)	__releases(lock);
void __lockfunc
_raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
								__releases(lock);

#ifdef CONFIG_INLINE_SPIN_LOCK
#define _raw_spin_lock(lock) __raw_spin_lock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_BH
#define _raw_spin_lock_bh(lock) __raw_spin_lock_bh(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_IRQ
#define _raw_spin_lock_irq(lock) __raw_spin_lock_irq(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_IRQSAVE
#define _raw_spin_lock_irqsave(lock) __raw_spin_lock_irqsave(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_TRYLOCK
#define _raw_spin_trylock(lock) __raw_spin_trylock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_TRYLOCK_BH
#define _raw_spin_trylock_bh(lock) __raw_spin_trylock_bh(lock)
#endif

#ifndef CONFIG_UNINLINE_SPIN_UNLOCK
/** 20130713    
 **/
#define _raw_spin_unlock(lock) __raw_spin_unlock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_BH
#define _raw_spin_unlock_bh(lock) __raw_spin_unlock_bh(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_IRQ
/** 20131109
**/
#define _raw_spin_unlock_irq(lock) __raw_spin_unlock_irq(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_IRQRESTORE
#define _raw_spin_unlock_irqrestore(lock, flags) __raw_spin_unlock_irqrestore(lock, flags)
#endif

/** 20140809    
 * 선점불가 상태에서 lock 획득을 시도.
 * 성공시 1 리턴. 실패시 0 리턴.
 **/
static inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
	preempt_disable();
	if (do_raw_spin_trylock(lock)) {
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
		return 1;
	}
	preempt_enable();
	return 0;
}

/*
 * If lockdep is enabled then we use the non-preemption spin-ops
 * even on CONFIG_PREEMPT, because lockdep assumes that interrupts are
 * not re-enabled during lock-acquire (which the preempt-spin-ops do):
 */
#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)

static inline unsigned long __raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags;

	/** 20121124
	 * flags에 cpsr 저장
	 **/
	local_irq_save(flags);
	/** 20121124
	 * 현재의 autoconf.h에는 CONFIG_PREEMPT_NONE 1임.
	 * CONFIG_PREEMPT_COUNT 역시 define 되어 있지 않아 실행하지 않음
	 **/
	preempt_disable();
	/** 20121124
	 * NULL 함수
	 **/
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	/*
	 * On lockdep we dont want the hand-coded irq-enable of
	 * do_raw_spin_lock_flags() code, because lockdep assumes
	 * that interrupts are not re-enabled during lock-acquire:
	 */
#ifdef CONFIG_LOCKDEP
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
#else
	/** 20121124
	 * 현재 config로 아래 함수 수행
	 **/
	do_raw_spin_lock_flags(lock, &flags);
#endif
	return flags;
}

/** 20131026    
 * irq disable, preempt_disable 호출 후 spinlock을 획득하는 함수
 **/
static inline void __raw_spin_lock_irq(raw_spinlock_t *lock)
{
	/** 20131026    
	 * local irq disable. 이전 상태는 별도로 저장하지 않는다.
	 **/
	local_irq_disable();
	/** 20131026    
	 * 선점 불가.
	 **/
	preempt_disable();
	/** 20131026    
	 * vexpress에서 NULL함수
	 **/
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	/** 20131026    
	 *do_raw_spin_lock을 호출해 lock을 획득.
	 **/
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

/** 20150214    
 * bottom-half를 막고, 선점을 금지시킨 상태로 spinlock을 건다.
 **/
static inline void __raw_spin_lock_bh(raw_spinlock_t *lock)
{
	local_bh_disable();
	preempt_disable();
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

/** 20130706    
 * spin_lock을 실행하는 부분.
 **/
static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
	/** 20130706    
	 * 선점불가로 지정. lock을 잡은채로 선점당하면 안 된다.
	 **/
	preempt_disable();
	/** 20130706    
	 * LOCK DEBUG를 사용하지 않으면 NULL 함수
	 **/
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	/** 20130706    
	 * LOCK DEBUG를 사용하지 않으면 do_raw_spin_lock 실행
	 **/
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

#endif /* CONFIG_PREEMPT */

/** 20130713    
 * spin unlock.
 **/
static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	/** 20130713    
	 * DEBUG용. NULL 함수
	 **/
	spin_release(&lock->dep_map, 1, _RET_IP_);
	/** 20130713    
	 * 실제 lock 해제.
	 **/
	do_raw_spin_unlock(lock);
	/** 20130713    
	 * non preempt로 NULL 함수.
	 **/
	preempt_enable();
}

static inline void __raw_spin_unlock_irqrestore(raw_spinlock_t *lock,
					    unsigned long flags)
{
/** 20121201
 * #ifdef CONFIG_DEBUG_LOCK_ALLOC -> 설정되어 있지 않으면
 * #define spin_release(l, n, i)  do { } while (0)
 **/
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	local_irq_restore(flags);
	preempt_enable();
}
/** 20131109
 * spin lock을 해제하고 irq를 enable한다
 **/
static inline void __raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	/** 20131109
	 * #ifdef CONFIG_DEBUG_LOCK_ALLOC이 설정되어 있지 않음
	 * # define spin_release(l, n, i)			do { } while (0)
	 **/
	spin_release(&lock->dep_map, 1, _RET_IP_);
	/** 20131109
	 * raw_spin_lock을 해제해준다
	 **/
	do_raw_spin_unlock(lock);
	/** 2013110
	 * local irq를 enable한다
	 **/
	local_irq_enable();
	/** 20131109
	 * 정의 되어 있지 않으므로 NULL
	 **/
	preempt_enable();
}

static inline void __raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	preempt_enable_no_resched();
	local_bh_enable_ip((unsigned long)__builtin_return_address(0));
}

static inline int __raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	local_bh_disable();
	preempt_disable();
	if (do_raw_spin_trylock(lock)) {
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
		return 1;
	}
	preempt_enable_no_resched();
	local_bh_enable_ip((unsigned long)__builtin_return_address(0));
	return 0;
}

#include <linux/rwlock_api_smp.h>

#endif /* __LINUX_SPINLOCK_API_SMP_H */
