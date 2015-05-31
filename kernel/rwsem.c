/* kernel/rwsem.c: R/W semaphores, public implementation
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/rwsem.h>

#include <linux/atomic.h>

/*
 * lock for reading
 */
/** 20150530    
 * rw_semaphore의 read lock을 잡는다.
 **/
void __sched down_read(struct rw_semaphore *sem)
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	/** 20150530    
	 * __down_read (sem)
	 **/
	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}

EXPORT_SYMBOL(down_read);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
/** 20140531    
 * rwsem의 reader side에서 lock을 시도한다.
 **/
int down_read_trylock(struct rw_semaphore *sem)
{
	int ret = __down_read_trylock(sem);

	if (ret == 1)
		rwsem_acquire_read(&sem->dep_map, 0, 1, _RET_IP_);
	return ret;
}

EXPORT_SYMBOL(down_read_trylock);

/*
 * lock for writing
 */
/** 20150530    
 * rw_semaphore의 write lock을 잡는다.
 **/
void __sched down_write(struct rw_semaphore *sem)
{
	/** 20140628    
	 * resched가 필요하다면 schedule 함수를 호출할 수 있다.
	 **/
	might_sleep();
	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);

	/** 20140628    
	 * __down_write (sem)
	 **/
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}

EXPORT_SYMBOL(down_write);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
/** 20150530    
 * rw semaphore에서 writer side의 lock을 시도한다.
 *
 * 다른 writer나 reader가 모두 lock을 획득하고 있지 않고, 대기 중인 waiter도 없어
 * lock을 획득할 수 있다면 lock을 잡고 1을 리턴한다.
 **/
int down_write_trylock(struct rw_semaphore *sem)
{
	int ret = __down_write_trylock(sem);

	if (ret == 1)
		rwsem_acquire(&sem->dep_map, 0, 1, _RET_IP_);
	return ret;
}

EXPORT_SYMBOL(down_write_trylock);

/*
 * release a read lock
 */
void up_read(struct rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, 1, _RET_IP_);

	__up_read(sem);
}

EXPORT_SYMBOL(up_read);

/*
 * release a write lock
 */
/** 20150530    
 * rw_semaphore의 write lock을 해제한다.
 **/
void up_write(struct rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, 1, _RET_IP_);

	__up_write(sem);
}

EXPORT_SYMBOL(up_write);

/*
 * downgrade write lock to read lock
 */
void downgrade_write(struct rw_semaphore *sem)
{
	/*
	 * lockdep: a downgraded write will live on as a write
	 * dependency.
	 */
	__downgrade_write(sem);
}

EXPORT_SYMBOL(downgrade_write);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

void down_read_nested(struct rw_semaphore *sem, int subclass)
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, subclass, 0, _RET_IP_);

	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}

EXPORT_SYMBOL(down_read_nested);

void down_write_nested(struct rw_semaphore *sem, int subclass)
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, subclass, 0, _RET_IP_);

	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}

EXPORT_SYMBOL(down_write_nested);

#endif


