/*
 * Mutexes: blocking mutual exclusion locks
 *
 * started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * This file contains mutex debugging related internal prototypes, for the
 * !CONFIG_DEBUG_MUTEXES case. Most of them are NOPs:
 */

/** 20130713    
 * spin_lock으로 mutex의 lock을 획득. 선점 불가
 * spin_unlock으로 mutex의 lock을 해제. 선점 가능
 *
 *   flags는 mutex_debug.h에서 사용됨
 **/
#define spin_lock_mutex(lock, flags) \
		do { spin_lock(lock); (void)(flags); } while (0)
#define spin_unlock_mutex(lock, flags) \
		do { spin_unlock(lock); (void)(flags); } while (0)
/** 20130713    
 * waiter를 list에서 제거.
 *   ti는 mutex-debug.c에서 사용
 **/
#define mutex_remove_waiter(lock, waiter, ti) \
		__list_del((waiter)->list.prev, (waiter)->list.next)

#ifdef CONFIG_SMP
/** 20130706    
 * current task로 owner를 설정.
 **/
static inline void mutex_set_owner(struct mutex *lock)
{
	lock->owner = current;
}

/** 20130713    
 * lock의 owner를 NULL로 함.
 **/
static inline void mutex_clear_owner(struct mutex *lock)
{
	lock->owner = NULL;
}
#else
static inline void mutex_set_owner(struct mutex *lock)
{
}

static inline void mutex_clear_owner(struct mutex *lock)
{
}
#endif

/** 20130713    
 * debug 관련 기능은 MUTEX DEBUG CONFIG를 설정하지 않아 NULL 함수.
 **/
#define debug_mutex_wake_waiter(lock, waiter)		do { } while (0)
#define debug_mutex_free_waiter(waiter)			do { } while (0)
#define debug_mutex_add_waiter(lock, waiter, ti)	do { } while (0)
#define debug_mutex_unlock(lock)			do { } while (0)
#define debug_mutex_init(lock, name, key)		do { } while (0)

/** 20130713    
 * mutex debugging옵션을 사용하지 않는다면 NULL 함수.
 *  debug 관련 내용은 mutex-debug.c 참고.
 **/
static inline void
debug_mutex_lock_common(struct mutex *lock, struct mutex_waiter *waiter)
{
}
