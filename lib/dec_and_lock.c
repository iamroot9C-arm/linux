#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

/*
 * This is an implementation of the notion of "decrement a
 * reference count, and return locked if it decremented to zero".
 *
 * NOTE NOTE NOTE! This is _not_ equivalent to
 *
 *	if (atomic_dec_and_test(&atomic)) {
 *		spin_lock(&lock);
 *		return 1;
 *	}
 *	return 0;
 *
 * because the spin-lock and the decrement must be
 * "atomic".
 */
/** 20150328
 * reference count를 감소시키고, 그 결과 0이 되었다면 lock을 건 상태로 리턴한다.
 **/
int _atomic_dec_and_lock(atomic_t *atomic, spinlock_t *lock)
{
	/* Subtract 1 from counter unless that drops it to 0 (ie. it was 1) */
	/** 20150328
	 * 현재 atomic이 1이 아니라면, -1시키고 바로 리턴한다.
	 **/
	if (atomic_add_unless(atomic, -1, 1))
		return 0;

	/* Otherwise do it the slow way */
	spin_lock(lock);
	/** 20150328
	 * atomic 값을 감소시키고, 결과 0이면 lock이 걸린 상태로 리턴한다.
	 **/
	if (atomic_dec_and_test(atomic))
		return 1;
	spin_unlock(lock);
	return 0;
}

EXPORT_SYMBOL(_atomic_dec_and_lock);
