#ifndef __LINUX_SEQLOCK_H
#define __LINUX_SEQLOCK_H
/*
 * Reader/writer consistent mechanism without starving writers. This type of
 * lock for data where the reader wants a consistent set of information
 * and is willing to retry if the information changes.  Readers never
 * block but they may have to retry if a writer is in
 * progress. Writers do not wait for readers. 
 *
 * This is not as cache friendly as brlock. Also, this will not work
 * for data that contains pointers, because any writer could
 * invalidate a pointer that a reader was following.
 *
 * Expected reader usage:
 * 	do {
 *	    seq = read_seqbegin(&foo);
 * 	...
 *      } while (read_seqretry(&foo, seq));
 *
 *
 * On non-SMP the spin locks disappear but the writer still needs
 * to increment the sequence variables because an interrupt routine could
 * change the state of the data.
 *
 * Based on x86_64 vsyscall gettimeofday 
 * by Keith Owens and Andrea Arcangeli
 */

#include <linux/spinlock.h>
#include <linux/preempt.h>
#include <asm/processor.h>

/** 20141011
 * reader-writer lock의 문제점인 writer가 굶주리는 문제(starving writer problem)를 해결한 lock.
 * 
 * - writer
 *   lock/unlock시에 spinlock을 사용하며, smp_wmb()로 동기화되는 sequence number를 단조 증가시킨다.
 *   즉, writer 임계구간에서 sequence number는 홀수이다.
 * 
 * - reader
 *   lock/unlock으로 구성되지 않고 smp_rmb()로 동기화되는 sequence를 검사한다.
 *   writer가 임계구역에 있거나, reader가 데이터에 접근하기 전후의 sequence number가 달라졌다면 retry한다.
 *
 *
 * 따라서 writer는 reader에 방해받지 않으며, 다수의 reader 사이에 lock이 존재하지 않은 특징을 갖고 있다.
 * 단 reader의 참조 구간이 길어질수록 재시도로 인한 비용이 높아진다.
 *
 * 일반적인 reader의 구현은 다음과 같으며, 대표적인 예는 jiffies, timekeeping 값이다.
 * 	do {
 *	    seq = read_seqbegin(&foo);
 * 	...
 *      } while (read_seqretry(&foo, seq));
 *
 *
 *       write access                |                   read access
 *       ------------                |                   -----------
 *                                   |
 *  +++++++++++++++++++++++++++      |      ++++++++++++++++++++++++++++++++++++
 *  + acquire write_seqlock   +      |      +   +   get count_pre              +
 *  +  increment counter      +      |      +   +------------------------------+
 *  +++++++++++++++++++++++++++      |      +   +   copy data                  +
 *  |                         |      |      +   +------------------------------+
 *  |   modify data ...       |      |      +  while (count_pre != count_post) +
 *  |                         |      |      ++++++++++++++++++++++++++++++++++++
 *  +++++++++++++++++++++++++++      |      |                                  |
 *  +  increment counter      +      |      |       working with copy ...      |
 *  + drop write_seqlock      +      |      |                                  |
 *  +++++++++++++++++++++++++++      |      |__________________________________|
 *                                   |
 *
 *  [참고] http://irl.cs.ucla.edu/~yingdi/web/paperreading/smp_locking.pdf
 **/
typedef struct {
	unsigned sequence;
	spinlock_t lock;
} seqlock_t;

/*
 * These macros triggered gcc-3.x compile-time problems.  We think these are
 * OK now.  Be cautious.
 */
#define __SEQLOCK_UNLOCKED(lockname) \
		 { 0, __SPIN_LOCK_UNLOCKED(lockname) }

#define seqlock_init(x)					\
	do {						\
		(x)->sequence = 0;			\
		spin_lock_init(&(x)->lock);		\
	} while (0)

#define DEFINE_SEQLOCK(x) \
		seqlock_t x = __SEQLOCK_UNLOCKED(x)

/* Lock out other writers and update the count.
 * Acts like a normal spin_lock/unlock.
 * Don't need preempt_disable() because that is in the spin_lock already.
 */
/** 20141011
 * spinlock에 기반한 writer 임계구역 설정/해제 함수.
 * lock : sequence를 변경한 뒤 memory barrier를 둔다.
 * unlock : memory barrier 이후 sequence를 변경한다.
 **/
static inline void write_seqlock(seqlock_t *sl)
{
	spin_lock(&sl->lock);
	++sl->sequence;
	smp_wmb();
}

static inline void write_sequnlock(seqlock_t *sl)
{
	smp_wmb();
	sl->sequence++;
	spin_unlock(&sl->lock);
}

static inline int write_tryseqlock(seqlock_t *sl)
{
	int ret = spin_trylock(&sl->lock);

	if (ret) {
		++sl->sequence;
		smp_wmb();
	}
	return ret;
}

/* Start of read calculation -- fetch last complete writer token */
/** 20141011
 * writer가 임계구역에서 벗어난 상태에서 리턴된다.
 **/
static __always_inline unsigned read_seqbegin(const seqlock_t *sl)
{
	unsigned ret;

repeat:
	ret = ACCESS_ONCE(sl->sequence);
	if (unlikely(ret & 1)) {
		cpu_relax();
		goto repeat;
	}
	smp_rmb();

	return ret;
}

/*
 * Test if reader processed invalid data.
 *
 * If sequence value changed then writer changed data while in section.
 */
/** 20141011
 * reader가 값을 읽기 전후에 sequence number가 변경되었는지 확인한다.
 **/
static __always_inline int read_seqretry(const seqlock_t *sl, unsigned start)
{
	smp_rmb();

	return unlikely(sl->sequence != start);
}


/*
 * Version using sequence counter only.
 * This can be used when code has its own mutex protecting the
 * updating starting before the write_seqcountbeqin() and ending
 * after the write_seqcount_end().
 */


typedef struct seqcount {
	unsigned sequence;
} seqcount_t;

#define SEQCNT_ZERO { 0 }
#define seqcount_init(x)	do { *(x) = (seqcount_t) SEQCNT_ZERO; } while (0)

/**
 * __read_seqcount_begin - begin a seq-read critical section (without barrier)
 * @s: pointer to seqcount_t
 * Returns: count to be passed to read_seqcount_retry
 *
 * __read_seqcount_begin is like read_seqcount_begin, but has no smp_rmb()
 * barrier. Callers should ensure that smp_rmb() or equivalent ordering is
 * provided before actually loading any of the variables that are to be
 * protected in this critical section.
 *
 * Use carefully, only in critical code, and comment how the barrier is
 * provided.
 */
/** 20130907    
 * seqcount_t를 읽어와 리턴한다.
 * 만약 seqcount_t가 홀수라면, 즉 write_seqcount 중에 발생했다면 write가 끝날 때까지 대기한다.
 **/
static inline unsigned __read_seqcount_begin(const seqcount_t *s)
{
	unsigned ret;

repeat:
	ret = ACCESS_ONCE(s->sequence);
	/** 20130907    
	 * sequence가 홀수인 경우에만 memory barrier를 세우고 반복한다.
	 * sequence가 홀수인 경우는 __read_seqcount_begin이
	 * write_sequence_begin과 write_sequence_end 중에 발생했을 경우이다.
	 **/
	if (unlikely(ret & 1)) {
		/** 20130907    
		 * cpu_relax()는 memory barrier 역할을 한다.
		 * 반복문에서 메모리의 값에 따라 조건결과가 달라지는 경우에 등에 주로 사용된다.
		 **/
		cpu_relax();
		goto repeat;
	}
	return ret;
}

/** 20130907    
 * seqlock
 * [참고]
 *   http://www.makelinux.net/ldd3/chp-5-sect-7
 *   http://en.wikipedia.org/wiki/Seqlock
 *   http://codebank.tistory.com/43
 **/
/**
 * read_seqcount_begin - begin a seq-read critical section
 * @s: pointer to seqcount_t
 * Returns: count to be passed to read_seqcount_retry
 *
 * read_seqcount_begin opens a read critical section of the given seqcount.
 * Validity of the critical section is tested by checking read_seqcount_retry
 * function.
 */
/** 20130907    
 * __read_seqcount_begin로 seqcount_t 값을 읽어오고 read memory barrier를 세운다.
 * read와 memory barrier는 항상 이 순서로 배치되어야 한다.
 **/
static inline unsigned read_seqcount_begin(const seqcount_t *s)
{
	unsigned ret = __read_seqcount_begin(s);
	/** 20130907    
	 * ARCH 7 이상에서는 dmb 인스트럭션이 들어온다.
	 **/
	smp_rmb();
	return ret;
}

/**
 * raw_seqcount_begin - begin a seq-read critical section
 * @s: pointer to seqcount_t
 * Returns: count to be passed to read_seqcount_retry
 *
 * raw_seqcount_begin opens a read critical section of the given seqcount.
 * Validity of the critical section is tested by checking read_seqcount_retry
 * function.
 *
 * Unlike read_seqcount_begin(), this function will not wait for the count
 * to stabilize. If a writer is active when we begin, we will fail the
 * read_seqcount_retry() instead of stabilizing at the beginning of the
 * critical section.
 */
static inline unsigned raw_seqcount_begin(const seqcount_t *s)
{
	unsigned ret = ACCESS_ONCE(s->sequence);
	smp_rmb();
	return ret & ~1;
}

/**
 * __read_seqcount_retry - end a seq-read critical section (without barrier)
 * @s: pointer to seqcount_t
 * @start: count, from read_seqcount_begin
 * Returns: 1 if retry is required, else 0
 *
 * __read_seqcount_retry is like read_seqcount_retry, but has no smp_rmb()
 * barrier. Callers should ensure that smp_rmb() or equivalent ordering is
 * provided before actually loading any of the variables that are to be
 * protected in this critical section.
 *
 * Use carefully, only in critical code, and comment how the barrier is
 * provided.
 */ 

/** 20140705
 * 현재 seqcount값과 초기값과 다른지 검사한다.
 */
static inline int __read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	return unlikely(s->sequence != start);
}

/**
 * read_seqcount_retry - end a seq-read critical section
 * @s: pointer to seqcount_t
 * @start: count, from read_seqcount_begin
 * Returns: 1 if retry is required, else 0
 *
 * read_seqcount_retry closes a read critical section of the given seqcount.
 * If the critical section was invalid, it must be ignored (and typically
 * retried).
 */

/** 20140705
 * 메모리 배리어를 설정하고 현재 seqcount값과 다를 경우 seqcount를 다시 읽어야 한다.
 */
static inline int read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	smp_rmb();

	return __read_seqcount_retry(s, start);
}


/*
 * Sequence counter only version assumes that callers are using their
 * own mutexing.
 */
/** 20140705
 * writer의 critical section이 시작할 때 seqence값을 증가시키고 write memory barrier를 설정한다.
 */
static inline void write_seqcount_begin(seqcount_t *s)
{
	s->sequence++;
	smp_wmb();
}
/** 20140705
 * writer의 critical section이 끝날때 write memory barrier이후 sequence값을 증가시킨다.
 */
static inline void write_seqcount_end(seqcount_t *s)
{
	smp_wmb();
	s->sequence++;
}

/**
 * write_seqcount_barrier - invalidate in-progress read-side seq operations
 * @s: pointer to seqcount_t
 *
 * After write_seqcount_barrier, no read-side seq operations will complete
 * successfully and see data older than this.
 */
static inline void write_seqcount_barrier(seqcount_t *s)
{
	smp_wmb();
	s->sequence+=2;
}

/*
 * Possible sw/hw IRQ protected versions of the interfaces.
 */
#define write_seqlock_irqsave(lock, flags)				\
	do { local_irq_save(flags); write_seqlock(lock); } while (0)
#define write_seqlock_irq(lock)						\
	do { local_irq_disable();   write_seqlock(lock); } while (0)
#define write_seqlock_bh(lock)						\
        do { local_bh_disable();    write_seqlock(lock); } while (0)

#define write_sequnlock_irqrestore(lock, flags)				\
	do { write_sequnlock(lock); local_irq_restore(flags); } while(0)
#define write_sequnlock_irq(lock)					\
	do { write_sequnlock(lock); local_irq_enable(); } while(0)
#define write_sequnlock_bh(lock)					\
	do { write_sequnlock(lock); local_bh_enable(); } while(0)

#define read_seqbegin_irqsave(lock, flags)				\
	({ local_irq_save(flags);   read_seqbegin(lock); })

#define read_seqretry_irqrestore(lock, iv, flags)			\
	({								\
		int ret = read_seqretry(lock, iv);			\
		local_irq_restore(flags);				\
		ret;							\
	})

#endif /* __LINUX_SEQLOCK_H */
