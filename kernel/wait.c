/*
 * Generic waiting primitives.
 *
 * (C) 2004 William Irwin, Oracle
 */
#include <linux/init.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/hash.h>

/** 20130427
 * wait_queue를 사용하기 위한 자료구조 초기화
 **/
void __init_waitqueue_head(wait_queue_head_t *q, const char *name, struct lock_class_key *key)
{
	/** 20130427
	 * spin_lock 초기화
	 **/
	spin_lock_init(&q->lock);
	/** 20130427
	 * CONFIG_LOCKDEP이 정의되어 있지 않아 key와 name만 나열됨
	 **/
	lockdep_set_class_and_name(&q->lock, key, name);
	/** 20130427
	 * task_list 초기화
	 **/
	INIT_LIST_HEAD(&q->task_list);
}

EXPORT_SYMBOL(__init_waitqueue_head);

void add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	/** 20131102
	 * 전달된 wait queue의 flags에서 WQ_FLAG_EXCLUSIVE를 제거.
	 * exclusive한 버전은 add_wait_queue_exclusive
	 **/
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	/** 20131102
	 * spinlock으로 wait queue head에 대한 동기화를 보장.
	 **/
	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue);

/** 20151031
 * queue에 wait 함수를 exclusive로 등록한다.
 **/
void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	wait->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue_tail(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue_exclusive);

/** 20131102
 * wait queue entry 하나를 wait queue head가 가리키는 list에서 제거한다.
 * 즉, wait queue를 벗어난다.
 **/
void remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	/** 20131102
	 * wait queue head에 대한 동기화 구간 안에서
	 * wait을 wait queue list에서 제거한다.
	 **/
	spin_lock_irqsave(&q->lock, flags);
	__remove_wait_queue(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(remove_wait_queue);


/*
 * SMP에서 memory barrier가 필요하므로
 * set_current_state()를 wait-queue add 이후에 썼다.
 * set_current_state 내에 smb가 존재한다.
 *
 * wait-queue가 active한지 검사하는 어떤 wait-함수가
 * 이 스레드에서 waitqueue 추가 또는 이어지는 다른 검사가
 * wakeup이 발생(taken place) 했음을 알게 된다.
 *   -> wake-함수가 wakeup이 발생했음을 알게 된다.
 * 
 * Note: we use "set_current_state()" _after_ the wait-queue add,
 * because we need a memory barrier there on SMP, so that any
 * wake-function that tests for the wait-queue being active
 * will be guaranteed to see waitqueue addition _or_ subsequent
 * tests in this thread will see the wakeup having taken place.
 *
 * The spin_unlock() itself is semi-permeable and only protects
 * one way (it only protects stuff inside the critical region and
 * stops them from bleeding out - it would still allow subsequent
 * loads to move into the critical region).
 */
/** 20131130
 * wait하기 전에 필요한 자료구조를 설정한다.
 *   - wait queue에 들어가 있지 않다면 queue에 등록해준다.
 *   - 현재 task의 state를 변경한다. (dmb가 포함되어 있다)
 * 아직 스케쥴러는 호출되지 않았다.
 **/
void
prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;

	/** 20131130
	 * WQ_FLAG_EXCLUSIVE 속성을 제거한다.
	 **/
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	/** 20131130
	 * queue에 lock을 건다.
	 **/
	spin_lock_irqsave(&q->lock, flags);
	/** 20131130
	 * wait의 task_list가 비어 있다면
	 *   즉, list에 연결되어 있지 않다면 wait을 q에 연결한다.
	 **/
	if (list_empty(&wait->task_list))
		__add_wait_queue(q, wait);
	/** 20131130
	 * 넘어온 state로 현재 task의 상태를 변경한다.
	 **/
	set_current_state(state);
	/** 20131130
	 * queue의 lock을 해제한다.
	 **/
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait);

void
prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;

	wait->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	if (list_empty(&wait->task_list))
		__add_wait_queue_tail(q, wait);
	set_current_state(state);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait_exclusive);

/**
 * finish_wait - clean up after waiting in a queue
 * @q: waitqueue waited on
 * @wait: wait descriptor
 *
 * Sets current thread back to running state and removes
 * the wait descriptor from the given waitqueue if still
 * queued.
 */
/** 20131130
 * 현재 task 상태를 running으로 변경하고,
 * waitqueue에 아직 queue되어 있다면 제거한다.
 **/
void finish_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	/** 20131130
	 * 현재 task의 상태를 TASK_RUNNING로 변경.
	 *   prepare_to_wait에서는 set_current_state() 함수 사용
	 **/
	__set_current_state(TASK_RUNNING);
	/*
	 * We can check for list emptiness outside the lock
	 * IFF:
	 *   ** 20131130
	 *     "careful" 체크를 사용해 next와 prev 포인터를 같이 검사한다.
	 *     다른 CPU에서 갱신 중인 경우라도 half-pending 이 발생할 수 없다.
	 *     (스택 영역에서 변경은 진행 중일 수 있다)
	 *   **
	 *  - we use the "careful" check that verifies both
	 *    the next and prev pointers, so that there cannot
	 *    be any half-pending updates in progress on other
	 *    CPU's that we haven't seen yet (and that might
	 *    still change the stack area.
	 * and
	 *  - all other users take the lock (ie we can only
	 *    have _one_ other CPU that looks at or modifies
	 *    the list).
	 */
	/** 20131130
	 * wait->task_list이 empty가 아닌 경우 (careful로 검사)
	 * spinlock으로 q를 보호한 상태에서 wait을 list에서 제거한다.
	 **/
	if (!list_empty_careful(&wait->task_list)) {
		spin_lock_irqsave(&q->lock, flags);
		list_del_init(&wait->task_list);
		spin_unlock_irqrestore(&q->lock, flags);
	}
}
EXPORT_SYMBOL(finish_wait);

/**
 * abort_exclusive_wait - abort exclusive waiting in a queue
 * @q: waitqueue waited on
 * @wait: wait descriptor
 * @mode: runstate of the waiter to be woken
 * @key: key to identify a wait bit queue or %NULL
 *
 * Sets current thread back to running state and removes
 * the wait descriptor from the given waitqueue if still
 * queued.
 *
 * Wakes up the next waiter if the caller is concurrently
 * woken up through the queue.
 *
 * This prevents waiter starvation where an exclusive waiter
 * aborts and is woken up concurrently and no one wakes up
 * the next waiter.
 */
void abort_exclusive_wait(wait_queue_head_t *q, wait_queue_t *wait,
			unsigned int mode, void *key)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	spin_lock_irqsave(&q->lock, flags);
	if (!list_empty(&wait->task_list))
		list_del_init(&wait->task_list);
	else if (waitqueue_active(q))
		__wake_up_locked_key(q, mode, key);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(abort_exclusive_wait);

/** 20131123
 * wait으로 받은 task를 깨우고, 성공적으로 삭제했다면 list에서 제거한다.
 * 성공했다면 참을 반환.
 **/
int autoremove_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	/** 20131123
	 * default_wake_function을 호출하고, 결과를 받아온다.
	 **/
	int ret = default_wake_function(wait, mode, sync, key);

	/** 20131123
	 * wake up에 성공했다면 wakequeue의 task_list에서 제거하고 포인터를 초기화 한다.
	 **/
	if (ret)
		list_del_init(&wait->task_list);
	/** 20131123
	 * wake 결과를 반환
	 **/
	return ret;
}
EXPORT_SYMBOL(autoremove_wake_function);

int wake_bit_function(wait_queue_t *wait, unsigned mode, int sync, void *arg)
{
	struct wait_bit_key *key = arg;
	struct wait_bit_queue *wait_bit
		= container_of(wait, struct wait_bit_queue, wait);

	if (wait_bit->key.flags != key->flags ||
			wait_bit->key.bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->flags))
		return 0;
	else
		return autoremove_wake_function(wait, mode, sync, key);
}
EXPORT_SYMBOL(wake_bit_function);

/*
 * To allow interruptible waiting and asynchronous (i.e. nonblocking)
 * waiting, the actions of __wait_on_bit() and __wait_on_bit_lock() are
 * permitted return codes. Nonzero return codes halt waiting and return.
 */
/** 20150314
 * q에 지정된 bit가 풀릴 때까지 wq에서 대기하는 함수.
 * action을 호출해 sleep 상태로 들어간다.
 **/
int __sched
__wait_on_bit(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	int ret = 0;

	/** 20150314
	 * key flags 중 bit_nr이 풀릴 때까지 action을 호출해 sleep 하는 함수.
	 **/
	do {
		/** 20150314
		 * wq에 등록하고, 대기할 수 있도록 준비한다.
		 **/
		prepare_to_wait(wq, &q->wait, mode);
		/** 20150314
		 * q의 flags 중 bit_nr 비트가 설정되어 있다면,
		 * action을 호출해 sleep 상태로 들어간다.
		 **/
		if (test_bit(q->key.bit_nr, q->key.flags))
			ret = (*action)(q->key.flags);
	} while (test_bit(q->key.bit_nr, q->key.flags) && !ret);
	/** 20150314
	 * q에서의 대기가 끝난 뒤, task 상태를 변경하고 큐에서 제거한다.
	 **/
	finish_wait(wq, &q->wait);
	return ret;
}
EXPORT_SYMBOL(__wait_on_bit);

/** 20150314
 * word의 bit가 클리어 될 때까지 대기한다.
 **/
int __sched out_of_line_wait_on_bit(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	/** 20150314
	 * word와 bit가 위치할 waitqueue 의 head를 가져오고,
	 * wait_bit_queue를 선언한다.
	 *
	 * wq에 등록하고, word 내의 bit가 해제될 때까지 대기한다.
	 **/
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	return __wait_on_bit(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit);

int __sched
__wait_on_bit_lock(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	do {
		int ret;

		prepare_to_wait_exclusive(wq, &q->wait, mode);
		if (!test_bit(q->key.bit_nr, q->key.flags))
			continue;
		ret = action(q->key.flags);
		if (!ret)
			continue;
		abort_exclusive_wait(wq, &q->wait, mode, &q->key);
		return ret;
	} while (test_and_set_bit(q->key.bit_nr, q->key.flags));
	finish_wait(wq, &q->wait);
	return 0;
}
EXPORT_SYMBOL(__wait_on_bit_lock);

int __sched out_of_line_wait_on_bit_lock(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	return __wait_on_bit_lock(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit_lock);

/** 20140607
 * wait queue에서 task 하나를 wake up 시킨다.
 **/
void __wake_up_bit(wait_queue_head_t *wq, void *word, int bit)
{
	/** 20140607
	 * word와 bit로 wait_bit_key를 생성한다.
	 * 즉, bit가 켜있는지 꺼져 있는지를 여부까지 포함된 키가 된다.
	 * 
	 * waitqueue에 대기 중인 task가 있으면
	 * key가 일치하는 task를 하나 wake up 시킨다.
	 **/
	struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);
	if (waitqueue_active(wq))
		__wake_up(wq, TASK_NORMAL, 1, &key);
}
EXPORT_SYMBOL(__wake_up_bit);

/**
 * wake_up_bit - wake up a waiter on a bit
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 *
 * There is a standard hashed waitqueue table for generic use. This
 * is the part of the hashtable's accessor API that wakes up waiters
 * on a bit. For instance, if one were to have waiters on a bitflag,
 * one would call wake_up_bit() after clearing the bit.
 *
 * In order for this to function properly, as it uses waitqueue_active()
 * internally, some kind of memory barrier must be done prior to calling
 * this. Typically, this will be smp_mb__after_clear_bit(), but in some
 * cases where bitflags are manipulated non-atomically under a lock, one
 * may need to use a less regular barrier, such fs/inode.c's smp_mb(),
 * because spin_unlock() does not guarantee a memory barrier.
 */
/** 20150314
 * word와 bit를 받아 해당 bit를 대기하고 잠든 task를 깨운다.
 **/
void wake_up_bit(void *word, int bit)
{
	__wake_up_bit(bit_waitqueue(word, bit), word, bit);
}
EXPORT_SYMBOL(wake_up_bit);

/** 20150314
 * word와 bit로 word가 속한 zone의 wait_queue head를 찾아 리턴한다.
 **/
wait_queue_head_t *bit_waitqueue(void *word, int bit)
{
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	const struct zone *zone = page_zone(virt_to_page(word));
	unsigned long val = (unsigned long)word << shift | bit;

	return &zone->wait_table[hash_long(val, zone->wait_table_bits)];
}
EXPORT_SYMBOL(bit_waitqueue);
