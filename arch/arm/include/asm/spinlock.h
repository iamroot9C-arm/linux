#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#if __LINUX_ARM_ARCH__ < 6
#error SMP not supported on pre-ARMv6 CPUs
#endif

#include <asm/processor.h>

/*
 * sev and wfe are ARMv6K extensions.  Uniprocessor ARMv6 may not have the K
 * extensions, so when running on UP, we have to patch these instructions away.
 */
/** 20130323
* sev: Set Event, wfe: Wait For Event
*/
#define ALT_SMP(smp, up)					\
	"9998:	" smp "\n"					\
	"	.pushsection \".alt.smp.init\", \"a\"\n"	\
	"	.long	9998b\n"				\
	"	" up "\n"					\
	"	.popsection\n"

#ifdef CONFIG_THUMB2_KERNEL
#define SEV		ALT_SMP("sev.w", "nop.w")
/*
 * For Thumb-2, special care is needed to ensure that the conditional WFE
 * instruction really does assemble to exactly 4 bytes (as required by
 * the SMP_ON_UP fixup code).   By itself "wfene" might cause the
 * assembler to insert a extra (16-bit) IT instruction, depending on the
 * presence or absence of neighbouring conditional instructions.
 *
 * To avoid this unpredictableness, an approprite IT is inserted explicitly:
 * the assembler won't change IT instructions which are explicitly present
 * in the input.
 */
#define WFE(cond)	ALT_SMP(		\
	"it " cond "\n\t"			\
	"wfe" cond ".n",			\
						\
	"nop.w"					\
)
#else
#define SEV		ALT_SMP("sev", "nop")
/** 20130323
* SMP 경우 "wfe" cond 수행
* UP 경우 "nop" 수행
*/
#define WFE(cond)	ALT_SMP("wfe" cond, "nop")
#endif

static inline void dsb_sev(void)
{
#if __LINUX_ARM_ARCH__ >= 7
/** 20121201
 * Data Synchronization Barrier
 * The DSB instruction is a special memory barrier,
 * that synchronizes the execution stream with memory accesses.
**/
	__asm__ __volatile__ (
		"dsb\n"
		SEV
	);
#else
	__asm__ __volatile__ (
		"mcr p15, 0, %0, c7, c10, 4\n"
		SEV
		: : "r" (0)
	);
#endif
}

/*
 * ARMv6 ticket-based spin-locking.
 *
 * A memory barrier is required after we get a lock, and before we
 * release it, because V6 CPUs are assumed to have weakly ordered
 * memory.
 */

#define arch_spin_unlock_wait(lock) \
	do { while (arch_spin_is_locked(lock)) cpu_relax(); } while (0)

/** 20130706    
 * do_raw_spin_lock_flags 에서 온 경우
 **/
#define arch_spin_lock_flags(lock, flags) arch_spin_lock(lock)

/** 20121201
 *  
 * +--------------+
 * | NEXT | OWNER |
 * +--------------+
 * OWNER : lock을 획득한 core의 ticket값
 * NEXT : 다음의 lock을 획득하려는 core에게 발급되는 ticket값
 * 참고 http://lwn.net/Articles/267968/
 * 
 * Case 1 : core1이 처음 lock을 획득하려는 상황
 *   core1(OWNER=0, NEXT=0) MEORY(OWNER=0, NEXT=0) -> lock 획득 전
 *   OWNER == NEXT값이 같아서 lock을 획득
 *   core1(OWNER=0, NEXT=0) MEORY(OWNER=0, NEXT=1) -> lock을 획득
 * 
 * Case 2 : core1이 lock을  획득한 상황에서 core2가 lock을 획득하려는 상황
 *   core2(OWNER=0, NEXT=1) MEORY(OWNER=0, NEXT=1) -> wfe() 대기
 *   spin unlock에서 Send Event를 보냄
 *   core2(OWNER=1, NEXT=1) MEORY(OWNER=1, NEXT=1) -> lock을 획득
 **/ 
static inline void arch_spin_lock(arch_spinlock_t *lock)
{
	unsigned long tmp;
	u32 newval;
	arch_spinlock_t lockval;

	/** 20121124
	 *  ldrex/strex 쌍을 이용해 구현
	 *  %0: lockval
	 *  %1: newval
	 *  %2: tmp				// 원자적 실행을 확인하는 리턴값
	 *  %3: &lock->slock	// lock은 __ARCH_SPIN_LOCK_UNLOCKED   { { 0 } }
	 *  %4: 1 << TICKER_SHIFT
	 *
	 *  newval = lockval + (1 << TICKET_SHIFT)
	 **/
	__asm__ __volatile__(
"1:	ldrex	%0, [%3]\n"
"	add	%1, %0, %4\n"
"	strex	%2, %1, [%3]\n"
"	teq	%2, #0\n"
"	bne	1b"
	: "=&r" (lockval), "=&r" (newval), "=&r" (tmp)
	: "r" (&lock->slock), "I" (1 << TICKET_SHIFT)
	: "cc");

	 /** 20121208 
     next와 owner가 같다면 while 문 수행 안함(20121124문서가  갱신됨)
	 **/

	while (lockval.tickets.next != lockval.tickets.owner) {
		wfe();
		lockval.tickets.owner = ACCESS_ONCE(lock->tickets.owner);
	}

	smp_mb();
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
	unsigned long tmp;
	u32 slock;

	__asm__ __volatile__(
"	ldrex	%0, [%2]\n"
"	subs	%1, %0, %0, ror #16\n"
"	addeq	%0, %0, %3\n"
"	strexeq	%1, %0, [%2]"
	: "=&r" (slock), "=&r" (tmp)
	: "r" (&lock->slock), "I" (1 << TICKET_SHIFT)
	: "cc");

	if (tmp == 0) {
		smp_mb();
		return 1;
	} else {
		return 0;
	}
}

/** 20121201
 * arch_spin_lock()에서 걸어준 lock을 해제해준다.
 **/
static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
/** 20121201
 *  %0: slock
 *  %1: tmp
 *  %2: &lock->slock
 * 
 * &lock->slock의 값을 slock으로 가져와서
 * owner값에 1을 증가시키고&lock->slock에 저장한다. 
**/
	unsigned long tmp;
	u32 slock;
/** 20121201
 * dmb()를 사용하였는데 왜???
**/
	smp_mb();

	__asm__ __volatile__(
"	mov	%1, #1\n"
"1:	ldrex	%0, [%2]\n"
"	uadd16	%0, %0, %1\n"
"	strex	%1, %0, [%2]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (slock), "=&r" (tmp)
	: "r" (&lock->slock)
	: "cc");
/** 20121201
 * dsb와 sev를 연속으로 호출하는 함수(dsb를 사용하였는데왜???)
 * ARM문서 B1.8.13 Wait For Event and Send Event
 * 멀티프로세서 시스템에서 모든 프로세스에게 이벤트를 전달해준다
**/
	dsb_sev();
}

static inline int arch_spin_is_locked(arch_spinlock_t *lock)
{
	struct __raw_tickets tickets = ACCESS_ONCE(lock->tickets);
	return tickets.owner != tickets.next;
}

static inline int arch_spin_is_contended(arch_spinlock_t *lock)
{
	struct __raw_tickets tickets = ACCESS_ONCE(lock->tickets);
	return (tickets.next - tickets.owner) > 1;
}
#define arch_spin_is_contended	arch_spin_is_contended

/*
 * RWLOCKS
 *
 *
 * Write locks are easy - we just set bit 31.  When unlocking, we can
 * just write zero since the lock is exclusively held.
 */

static inline void arch_write_lock(arch_rwlock_t *rw)
{
	unsigned long tmp;
/** 20130323
*1: ldrex temp, [rw->lock] : temp = *rw->lock
*	teq temp, #0 : temp = temp ^ 0
*	wfene        : if temp != 0, wait for event
*	strexeq temp 0x80000000, *rw->lock : *rw->lock = -INT_MAX, 
*                                        다른프로세스가 접근했으면 temp = 1, 그외는  temp = 0;
*	teq temp, 0  : temp = temp ^ 0;
* 	bne 1b       : if temp != 0, go 1b 
*/
	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
	WFE("ne")
"	strexeq	%0, %2, [%1]\n"
"	teq	%0, #0\n"
"	bne	1b"
	: "=&r" (tmp)
	: "r" (&rw->lock), "r" (0x80000000)
	: "cc");

	smp_mb();
}

static inline int arch_write_trylock(arch_rwlock_t *rw)
{
	unsigned long tmp;

	__asm__ __volatile__(
"	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]"
	: "=&r" (tmp)
	: "r" (&rw->lock), "r" (0x80000000)
	: "cc");

	if (tmp == 0) {
		smp_mb();
		return 1;
	} else {
		return 0;
	}
}

static inline void arch_write_unlock(arch_rwlock_t *rw)
{
	smp_mb();
/** 20130323
* 	*rw->lock = 0
*/
	__asm__ __volatile__(
	"str	%1, [%0]\n"
	:
	: "r" (&rw->lock), "r" (0)
	: "cc");

	dsb_sev();
}

/* write_can_lock - would write_trylock() succeed? */
#define arch_write_can_lock(x)		((x)->lock == 0)

/*
 * Read locks are a bit more hairy:
 *  - Exclusively load the lock value.
 *  - Increment it.
 *  - Store new lock value if positive, and we still own this location.
 *    If the value is negative, we've already failed.
 *  - If we failed to store the value, we want a negative result.
 *  - If we failed, try again.
 * Unlocking is similarly hairy.  We may have multiple read locks
 * currently active.  However, we know we won't have any write
 * locks.
 */
static inline void arch_read_lock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2;

/** 20130323
*		loadex ~ strexpl 사이 명령을 아토믹하게 수행.
*		:wfemi 를 수행. wait for event on minus condition (negative	
*1:	ldrex temp, [rw->lock]  : temp = *rw->lock  
*	adds  temp, temp, #1    : temp = temp + 1; 
*	strexpl temp2, temp, [rw->lock]  : if temp >= 0, *rw->lock = temp.  
* 									   temp2 = 1 다른 프로세서가 rw->lock 주소를 접근 시, 그외 0
*								       store on pl condition (plus or zero)
*	WFE("mi")  wfemi       :  temp < 0 이면, 대기상태로 진입. ( sev가 올때까지 대기)
*	rsbpls temp, temp2, #0 :  if temp >= 0, temp = 0 - temp2, update cpsr_f 
*	bmi 1b
*/
	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	adds	%0, %0, #1\n"
"	strexpl	%1, %0, [%2]\n"
	WFE("mi")
"	rsbpls	%0, %1, #0\n"
"	bmi	1b"
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&rw->lock)
	: "cc");

	smp_mb();
}

static inline void arch_read_unlock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2;

	smp_mb();
/** 201303223
*1:	ldrex tmp, rw->lock : tmp = *rw->lock
*	sub tmp, tmp, 1     : tmp = tmp - 1;
*	strex tmp2, tmp, rw->lock : *rw->lock = tmp, tmp2 =1 다른프로세서가 접근했으면, 그외 tmp2 = 0;
*	teq tmp2, #0              : tmp2 ^ 0
* 	bne 1b                    : tmp2 != 0 이면 go 1b
*/
	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	sub	%0, %0, #1\n"
"	strex	%1, %0, [%2]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&rw->lock)
	: "cc");

/** 20130323
*	dsb / sev asm 명령 실행.
*/
	if (tmp == 0)
		dsb_sev();
}

static inline int arch_read_trylock(arch_rwlock_t *rw)
{
	unsigned long tmp, tmp2 = 1;

	__asm__ __volatile__(
"	ldrex	%0, [%2]\n"
"	adds	%0, %0, #1\n"
"	strexpl	%1, %0, [%2]\n"
	: "=&r" (tmp), "+r" (tmp2)
	: "r" (&rw->lock)
	: "cc");

	smp_mb();
	return tmp2 == 0;
}

/* read_can_lock - would read_trylock() succeed? */
#define arch_read_can_lock(x)		((x)->lock < 0x80000000)

#define arch_read_lock_flags(lock, flags) arch_read_lock(lock)
#define arch_write_lock_flags(lock, flags) arch_write_lock(lock)

#define arch_spin_relax(lock)	cpu_relax()
#define arch_read_relax(lock)	cpu_relax()
#define arch_write_relax(lock)	cpu_relax()

#endif /* __ASM_SPINLOCK_H */
