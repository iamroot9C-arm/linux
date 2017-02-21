/* See include/linux/lglock.h for description */
#include <linux/module.h>
#include <linux/lglock.h>
#include <linux/cpu.h>
#include <linux/string.h>

/*
 * Note there is no uninit, so lglocks cannot be defined in
 * modules (but it's fine to use them from there)
 * Could be added though, just undo lg_lock_init
 */

/** 20150214
 * lg lock 초기화.
 *
 * LOCK ALLOC을 디버깅하지 않으면 아무 동작도 취하지 않는다.
 **/
void lg_lock_init(struct lglock *lg, char *name)
{
	LOCKDEP_INIT_MAP(&lg->lock_dep_map, name, &lg->lock_key, 0);
}
EXPORT_SYMBOL(lg_lock_init);

void lg_local_lock(struct lglock *lg)
{
	arch_spinlock_t *lock;

	preempt_disable();
	rwlock_acquire_read(&lg->lock_dep_map, 0, 0, _RET_IP_);
	lock = this_cpu_ptr(lg->lock);
	arch_spin_lock(lock);
}
EXPORT_SYMBOL(lg_local_lock);

void lg_local_unlock(struct lglock *lg)
{
	arch_spinlock_t *lock;

	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	lock = this_cpu_ptr(lg->lock);
	arch_spin_unlock(lock);
	preempt_enable();
}
EXPORT_SYMBOL(lg_local_unlock);

void lg_local_lock_cpu(struct lglock *lg, int cpu)
{
	arch_spinlock_t *lock;

	preempt_disable();
	rwlock_acquire_read(&lg->lock_dep_map, 0, 0, _RET_IP_);
	lock = per_cpu_ptr(lg->lock, cpu);
	arch_spin_lock(lock);
}
EXPORT_SYMBOL(lg_local_lock_cpu);

void lg_local_unlock_cpu(struct lglock *lg, int cpu)
{
	arch_spinlock_t *lock;

	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	lock = per_cpu_ptr(lg->lock, cpu);
	arch_spin_unlock(lock);
	preempt_enable();
}
EXPORT_SYMBOL(lg_local_unlock_cpu);

/** 20150221
 * lg global lock을 건다.
 * cpu들을 순회하며 각 코어별로 spinlock을 건다. 
 **/
void lg_global_lock(struct lglock *lg)
{
	int i;

	/** 20150221
	 * 선점불가 상태로 만든다.
	 * deadlock을 방지하기 위해.
	 * [참고] Documentation/locking/lglock.txt history.
	 **/
	preempt_disable();
	rwlock_acquire(&lg->lock_dep_map, 0, 0, _RET_IP_);
	for_each_possible_cpu(i) {
		arch_spinlock_t *lock;
		lock = per_cpu_ptr(lg->lock, i);
		arch_spin_lock(lock);
	}
}
EXPORT_SYMBOL(lg_global_lock);

/** 20150221
 * lg global lock을 해제한다.
 **/
void lg_global_unlock(struct lglock *lg)
{
	int i;

	/** 20150221
	 **/
	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	for_each_possible_cpu(i) {
		arch_spinlock_t *lock;
		lock = per_cpu_ptr(lg->lock, i);
		arch_spin_unlock(lock);
	}
	preempt_enable();
}
EXPORT_SYMBOL(lg_global_unlock);
