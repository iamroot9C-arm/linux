#ifndef __LINUX_SPINLOCK_H
#define __LINUX_SPINLOCK_H

/*
 * include/linux/spinlock.h - generic spinlock/rwlock declarations
 *
 * here's the role of the various spinlock/rwlock related include files:
 *
 * on SMP builds:
 *
 *  asm/spinlock_types.h: contains the arch_spinlock_t/arch_rwlock_t and the
 *                        initializers
 *
 *  linux/spinlock_types.h:
 *                        defines the generic type and initializers
 *
 *  asm/spinlock.h:       contains the arch_spin_*()/etc. lowlevel
 *                        implementations, mostly inline assembly code
 *
 *   (also included on UP-debug builds:)
 *
 *  linux/spinlock_api_smp.h:
 *                        contains the prototypes for the _spin_*() APIs.
 *
 *  linux/spinlock.h:     builds the final spin_*() APIs.
 *
 * on UP builds:
 *
 *  linux/spinlock_type_up.h:
 *                        contains the generic, simplified UP spinlock type.
 *                        (which is an empty structure on non-debug builds)
 *
 *  linux/spinlock_types.h:
 *                        defines the generic type and initializers
 *
 *  linux/spinlock_up.h:
 *                        contains the arch_spin_*()/etc. version of UP
 *                        builds. (which are NOPs on non-debug, non-preempt
 *                        builds)
 *
 *   (included on UP-non-debug builds:)
 *
 *  linux/spinlock_api_up.h:
 *                        builds the _spin_*() APIs.
 *
 *  linux/spinlock.h:     builds the final spin_*() APIs.
 */

#include <linux/typecheck.h>
#include <linux/preempt.h>
#include <linux/linkage.h>
#include <linux/compiler.h>
#include <linux/irqflags.h>
#include <linux/thread_info.h>
#include <linux/kernel.h>
#include <linux/stringify.h>
#include <linux/bottom_half.h>
#include <asm/barrier.h>


/*
 * Must define these before including other files, inline functions need them
 */
#define LOCK_SECTION_NAME ".text..lock."KBUILD_BASENAME

#define LOCK_SECTION_START(extra)               \
        ".subsection 1\n\t"                     \
        extra                                   \
        ".ifndef " LOCK_SECTION_NAME "\n\t"     \
        LOCK_SECTION_NAME ":\n\t"               \
        ".endif\n"

#define LOCK_SECTION_END                        \
        ".previous\n\t"

#define __lockfunc __attribute__((section(".spinlock.text")))

/*
 * Pull the arch_spinlock_t and arch_rwlock_t definitions:
 */
#include <linux/spinlock_types.h>

/*
 * Pull the arch_spin*() functions/declarations (UP-nondebug doesn't need them):
 */
#ifdef CONFIG_SMP
# include <asm/spinlock.h>
#else
# include <linux/spinlock_up.h>
#endif

#ifdef CONFIG_DEBUG_SPINLOCK
  extern void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
				   struct lock_class_key *key);
# define raw_spin_lock_init(lock)				\
do {								\
	static struct lock_class_key __key;			\
								\
	__raw_spin_lock_init((lock), #lock, &__key);		\
} while (0)

#else
/** 20130427    
 * lock 구조체 초기화
 **/
# define raw_spin_lock_init(lock)				\
	do { *(lock) = __RAW_SPIN_LOCK_UNLOCKED(lock); } while (0)
#endif

/** 20130713    
 **/
#define raw_spin_is_locked(lock)	arch_spin_is_locked(&(lock)->raw_lock)

#ifdef CONFIG_GENERIC_LOCKBREAK
#define raw_spin_is_contended(lock) ((lock)->break_lock)
#else

#ifdef arch_spin_is_contended
#define raw_spin_is_contended(lock)	arch_spin_is_contended(&(lock)->raw_lock)
#else
#define raw_spin_is_contended(lock)	(((void)(lock), 0))
#endif /*arch_spin_is_contended*/
#endif

/* The lock does not imply full memory barrier. */
#ifndef ARCH_HAS_SMP_MB_AFTER_LOCK
static inline void smp_mb__after_lock(void) { smp_mb(); }
#endif

/**
 * raw_spin_unlock_wait - wait until the spinlock gets unlocked
 * @lock: the spinlock in question.
 */
#define raw_spin_unlock_wait(lock)	arch_spin_unlock_wait(&(lock)->raw_lock)

#ifdef CONFIG_DEBUG_SPINLOCK
 extern void do_raw_spin_lock(raw_spinlock_t *lock) __acquires(lock);
#define do_raw_spin_lock_flags(lock, flags) do_raw_spin_lock(lock)
 extern int do_raw_spin_trylock(raw_spinlock_t *lock);
 extern void do_raw_spin_unlock(raw_spinlock_t *lock) __releases(lock);
#else
 /** 20130706    
  * 실제 lock을 획득하는 함수.
  **/
static inline void do_raw_spin_lock(raw_spinlock_t *lock) __acquires(lock)
{
	__acquire(lock);
	arch_spin_lock(&lock->raw_lock);
}

static inline void
do_raw_spin_lock_flags(raw_spinlock_t *lock, unsigned long *flags) __acquires(lock)
{
	/** 20121124
	 * 우리는 sparse를 사용하지 않기 때문에 __CHECKER__가 정의되어 있지 않음.
	 **/
	__acquire(lock);
	arch_spin_lock_flags(&lock->raw_lock, *flags);
}

/** 20140809    
 **/
static inline int do_raw_spin_trylock(raw_spinlock_t *lock)
{
	return arch_spin_trylock(&(lock)->raw_lock);
}

/** 20121201
 * raw_spin_lock을 해제해준다
 **/ 
static inline void do_raw_spin_unlock(raw_spinlock_t *lock) __releases(lock)
{
	arch_spin_unlock(&lock->raw_lock);
	__release(lock);
}
#endif

/*
 * Define the various spin_lock methods.  Note we define these
 * regardless of whether CONFIG_SMP or CONFIG_PREEMPT are set. The
 * various methods are defined as nops in the case they are not
 * required.
 */
/** 20140809    
 **/
#define raw_spin_trylock(lock)	__cond_lock(lock, _raw_spin_trylock(lock))

/** 20130706    
 * _raw_spin_lock 호출
 **/
#define raw_spin_lock(lock)	_raw_spin_lock(lock)

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define raw_spin_lock_nested(lock, subclass) \
	_raw_spin_lock_nested(lock, subclass)

# define raw_spin_lock_nest_lock(lock, nest_lock)			\
	 do {								\
		 typecheck(struct lockdep_map *, &(nest_lock)->dep_map);\
		 _raw_spin_lock_nest_lock(lock, &(nest_lock)->dep_map);	\
	 } while (0)
#else
/** 20160430    
 * CONFIG_DEBUG_LOCK_ALLOC이 설정되지 않아 단순 _raw_spin_lock() 호출.
 **/
# define raw_spin_lock_nested(lock, subclass)		_raw_spin_lock(lock)
# define raw_spin_lock_nest_lock(lock, nest_lock)	_raw_spin_lock(lock)
#endif

#if defined(CONFIG_SMP) || defined(CONFIG_DEBUG_SPINLOCK)

/** 20130713    
 * flags의 type을 check하고,
 * lock을 걸고, 이전 cpsr을 flags에 저장한다.
 **/
#define raw_spin_lock_irqsave(lock, flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		flags = _raw_spin_lock_irqsave(lock);	\
	} while (0)

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define raw_spin_lock_irqsave_nested(lock, flags, subclass)		\
	do {								\
		typecheck(unsigned long, flags);			\
		flags = _raw_spin_lock_irqsave_nested(lock, subclass);	\
	} while (0)
#else
#define raw_spin_lock_irqsave_nested(lock, flags, subclass)		\
	do {								\
		typecheck(unsigned long, flags);			\
		flags = _raw_spin_lock_irqsave(lock);			\
	} while (0)
#endif

#else

#define raw_spin_lock_irqsave(lock, flags)		\
	do {						\
		typecheck(unsigned long, flags);	\
		_raw_spin_lock_irqsave(lock, flags);	\
	} while (0)

#define raw_spin_lock_irqsave_nested(lock, flags, subclass)	\
	raw_spin_lock_irqsave(lock, flags)

#endif

/** 20131026    
 * irq 또는 bh를 금지시킨 상태에서 spinlock을 건다.
 **/
#define raw_spin_lock_irq(lock)		_raw_spin_lock_irq(lock)
#define raw_spin_lock_bh(lock)		_raw_spin_lock_bh(lock)
/** 20130713    
 **/
#define raw_spin_unlock(lock)		_raw_spin_unlock(lock)
/** 20131109
 **/
#define raw_spin_unlock_irq(lock)	_raw_spin_unlock_irq(lock)

/** 20131109
 **/
#define raw_spin_unlock_irqrestore(lock, flags)		\
	do {							\
		typecheck(unsigned long, flags);		\
		_raw_spin_unlock_irqrestore(lock, flags);	\
	} while (0)
#define raw_spin_unlock_bh(lock)	_raw_spin_unlock_bh(lock)

#define raw_spin_trylock_bh(lock) \
	__cond_lock(lock, _raw_spin_trylock_bh(lock))

#define raw_spin_trylock_irq(lock) \
({ \
	local_irq_disable(); \
	raw_spin_trylock(lock) ? \
	1 : ({ local_irq_enable(); 0;  }); \
})

#define raw_spin_trylock_irqsave(lock, flags) \
({ \
	local_irq_save(flags); \
	raw_spin_trylock(lock) ? \
	1 : ({ local_irq_restore(flags); 0; }); \
})

/**
 * raw_spin_can_lock - would raw_spin_trylock() succeed?
 * @lock: the spinlock in question.
 */
#define raw_spin_can_lock(lock)	(!raw_spin_is_locked(lock))

/* Include rwlock functions */
#include <linux/rwlock.h>

/*
 * Pull the _spin_*()/_read_*()/_write_*() functions/declarations:
 */
#if defined(CONFIG_SMP) || defined(CONFIG_DEBUG_SPINLOCK)
# include <linux/spinlock_api_smp.h>
#else
# include <linux/spinlock_api_up.h>
#endif

/*
 * Map the spin_lock functions to the raw variants for PREEMPT_RT=n
 */
/** 20121117
 * Map the spin_lock ??? 
 */
static inline raw_spinlock_t *spinlock_check(spinlock_t *lock)
{
	return &lock->rlock;
}

/** 20121117
 * spinlock_check(_lock) ???
 * 이건 무엇을 위한걸까요
 */
#define spin_lock_init(_lock)				\
do {							\
	spinlock_check(_lock);				\
	raw_spin_lock_init(&(_lock)->rlock);		\
} while (0)
/** 20121117
raw_spin_lock_init(&(_lock)->rlock);		

 *(lock) = __RAW_SPIN_LOCK_UNLOCKED(lock);

 *(lock) = (raw_spinlock_t) __RAW_SPIN_LOCK_INITIALIZER(lock)
 
 *(lock) = (raw_spinlock_t) { .raw_lock = __ARCH_SPIN_LOCK_UNLOCKED;}

 (_lock)->rlock = (raw_spinlock_t) { .raw_lock = __ARCH_SPIN_LOCK_UNLOCKED;}
 
 (_lock)->rlock = (raw_spinlock_t) { .raw_lock = { { 0 } };}

 */
/** 20130706    
 * spin_lock 실행 함수
 *   - 선점불가 preempt_disable()
 *   - spin_lock 자체는 인터럽트를 금지하지 않음
 *     따라서, 인터럽트 핸들러와 공유하는 자원이 있다면 interrupt를 막아야 한다.
 **/
static inline void spin_lock(spinlock_t *lock)
{
	raw_spin_lock(&lock->rlock);
}

/** 20150214    
 * bottom-half를 막고, 선점을 금지시킨 상태로 spinlock을 건다.
 **/
static inline void spin_lock_bh(spinlock_t *lock)
{
	raw_spin_lock_bh(&lock->rlock);
}

static inline int spin_trylock(spinlock_t *lock)
{
	return raw_spin_trylock(&lock->rlock);
}

/** 20160430    
 * CONFIG_DEBUG_LOCK_ALLOC 설정하지 않아 _raw_spin_lock.
 **/
#define spin_lock_nested(lock, subclass)			\
do {								\
	raw_spin_lock_nested(spinlock_check(lock), subclass);	\
} while (0)

#define spin_lock_nest_lock(lock, nest_lock)				\
do {									\
	raw_spin_lock_nest_lock(spinlock_check(lock), nest_lock);	\
} while (0)

/** 20131026    
 * irq disable 시킨 뒤, spinlock을 거는 함수.
 * irq 상태는 저장하지 않는다.
 *
 * 이 함수를 사용하려면
 * 1) 재진입이 불가능하다. (irq state를 저장할 필요가 없다)
 * 2) 인터럽트 컨텍스트에서 해당 자원에 대한 접근을 하지 않는다.
 **/
static inline void spin_lock_irq(spinlock_t *lock)
{
	raw_spin_lock_irq(&lock->rlock);
}

/** 20131005    
 * irq를 disable 시킨 뒤 spinlock을 건다.
 **/
#define spin_lock_irqsave(lock, flags)				\
do {								\
	raw_spin_lock_irqsave(spinlock_check(lock), flags);	\
} while (0)

#define spin_lock_irqsave_nested(lock, flags, subclass)			\
do {									\
	raw_spin_lock_irqsave_nested(spinlock_check(lock), flags, subclass); \
} while (0)

/** 20130713    
 * raw_spin_unlock 호출
 **/
static inline void spin_unlock(spinlock_t *lock)
{
	raw_spin_unlock(&lock->rlock);
}

static inline void spin_unlock_bh(spinlock_t *lock)
{
	raw_spin_unlock_bh(&lock->rlock);
}
/** 20131109
 **/
static inline void spin_unlock_irq(spinlock_t *lock)
{
	raw_spin_unlock_irq(&lock->rlock);
}
/** 20131109
 **/
static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
	raw_spin_unlock_irqrestore(&lock->rlock, flags);
}

static inline int spin_trylock_bh(spinlock_t *lock)
{
	return raw_spin_trylock_bh(&lock->rlock);
}

static inline int spin_trylock_irq(spinlock_t *lock)
{
	return raw_spin_trylock_irq(&lock->rlock);
}

#define spin_trylock_irqsave(lock, flags)			\
({								\
	raw_spin_trylock_irqsave(spinlock_check(lock), flags); \
})

static inline void spin_unlock_wait(spinlock_t *lock)
{
	raw_spin_unlock_wait(&lock->rlock);
}

static inline int spin_is_locked(spinlock_t *lock)
{
	return raw_spin_is_locked(&lock->rlock);
}

static inline int spin_is_contended(spinlock_t *lock)
{
	return raw_spin_is_contended(&lock->rlock);
}

static inline int spin_can_lock(spinlock_t *lock)
{
	return raw_spin_can_lock(&lock->rlock);
}

/** 20150328    
 * spinlock이 잠겨있지 않다면 ASSERT.
 **/
#define assert_spin_locked(lock)	assert_raw_spin_locked(&(lock)->rlock)

/*
 * Pull the atomic_t declaration:
 * (asm-mips/atomic.h needs above definitions)
 */
#include <linux/atomic.h>
/**
 * atomic_dec_and_lock - lock on reaching reference count zero
 * @atomic: the atomic counter
 * @lock: the spinlock in question
 *
 * Decrements @atomic by 1.  If the result is 0, returns true and locks
 * @lock.  Returns false for all other cases.
 */
extern int _atomic_dec_and_lock(atomic_t *atomic, spinlock_t *lock);
/** 20150328    
 * atomic을 감소시키고, 그 결과 0이라면 spinlock을 건 상태로 만든다.
 **/
#define atomic_dec_and_lock(atomic, lock) \
		__cond_lock(lock, _atomic_dec_and_lock(atomic, lock))

#endif /* __LINUX_SPINLOCK_H */
