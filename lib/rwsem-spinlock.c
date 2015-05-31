/* rwsem-spinlock.c: R/W semaphores: contention handling functions for
 * generic spinlock implementation
 *
 * Copyright (c) 2001   David Howells (dhowells@redhat.com).
 * - Derived partially from idea by Andrea Arcangeli <andrea@suse.de>
 * - Derived also from comments by Linus
 */
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/export.h>

/** 20150530    
 * waiter 구조체.
 *
 * task는 대기 중인 task를 기록한다.
 * flags는 reader인지 writer인지 구분하기 위해 사용된다.
 **/
struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	unsigned int flags;
#define RWSEM_WAITING_FOR_READ	0x00000001
#define RWSEM_WAITING_FOR_WRITE	0x00000002
};

/** 20150530    
 * reader 또는 writer가 lock을 잡고 있다.
 **/
int rwsem_is_locked(struct rw_semaphore *sem)
{
	int ret = 1;
	unsigned long flags;

	if (raw_spin_trylock_irqsave(&sem->wait_lock, flags)) {
		ret = (sem->activity != 0);
		raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
	}
	return ret;
}
EXPORT_SYMBOL(rwsem_is_locked);

/*
 * initialise the semaphore
 */
/** 20150221    
 * rw semaphore를 초기화 한다.
 **/
void __init_rwsem(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map(&sem->dep_map, name, key, 0);
#endif
	sem->activity = 0;
	raw_spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
}
EXPORT_SYMBOL(__init_rwsem);

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here, then:
 *   - the 'active count' _reached_ zero
 *   - the 'waiting count' is non-zero
 * - the spinlock must be held by the caller
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only woken if wakewrite is non-zero
 */
/** 20150530    
 * wait list에 대기 중엔 waiter를 깨운다.
 * wakewrite는 writer를 깨우는 것이 가능한가를 나타낸다.
 *
 * 가장 앞의 writer 하나를 깨우거나, writer를 만나기 전의 reader를 깨운다.
 **/
static inline struct rw_semaphore *
__rwsem_do_wake(struct rw_semaphore *sem, int wakewrite)
{
	struct rwsem_waiter *waiter;
	struct task_struct *tsk;
	int woken;

	/** 20150530    
	 * wiat_list 가장 앞의 waiter를 가져온다.
	 **/
	waiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);

	/** 20150530    
	 * writer를 깨우도록 설정되지 않았고
	 *   맨 앞의 waiter가 writer였다면 깨울 수 없으므로 나간다.
	 *   그렇지 않다면 reader를 깨우도록 이동한다.
	 **/
	if (!wakewrite) {
		if (waiter->flags & RWSEM_WAITING_FOR_WRITE)
			goto out;
		goto dont_wake_writers;
	}

	/* if we are allowed to wake writers try to grant a single write lock
	 * if there's a writer at the front of the queue
	 * - we leave the 'waiting count' incremented to signify potential
	 *   contention
	 */
	/** 20150530    
	 * writer를 깨울 수 있도록 설정되어 있고,
	 * 맨 앞의 waiter가 writer라면
	 *   activity를 writer가 lock을 잡은 상태로 변경하고
	 *   waiter를 분리해 깨우고 종료한다.
	 **/
	if (waiter->flags & RWSEM_WAITING_FOR_WRITE) {
		sem->activity = -1;
		list_del(&waiter->list);
		tsk = waiter->task;
		/* Don't touch waiter after ->task has been NULLed */
		smp_mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		put_task_struct(tsk);
		goto out;
	}

	/* grant an infinite number of read locks to the front of the queue */
 dont_wake_writers:
	/** 20150530    
	 * 위 조건에 해당하지 않으므로 reader를 깨운다.
	 * 대기 중인 writer를 만나기 전까지 만난 reader를 모두 깨우고,
	 * 깨운 task의 수를 activity에 반영한다.
	 **/
	woken = 0;
	while (waiter->flags & RWSEM_WAITING_FOR_READ) {
		struct list_head *next = waiter->list.next;

		list_del(&waiter->list);
		tsk = waiter->task;
		smp_mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		put_task_struct(tsk);
		woken++;
		if (list_empty(&sem->wait_list))
			break;
		waiter = list_entry(next, struct rwsem_waiter, list);
	}

	sem->activity += woken;

 out:
	return sem;
}

/*
 * wake a single writer
 */
/** 20150530    
 * wait list에서 대기 중인 writer를 하나 깨운다.
 **/
static inline struct rw_semaphore *
__rwsem_wake_one_writer(struct rw_semaphore *sem)
{
	struct rwsem_waiter *waiter;
	struct task_struct *tsk;

	/** 20150530    
	 * rw_semaphore의 activity를 write lock 보유 중으로 설정하고,
	 * waiter (항상 writer이다)를 하나 분리한다.
	 **/
	sem->activity = -1;

	waiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);
	list_del(&waiter->list);

	/** 20150530    
	 * task를 깨운다.
	 **/
	tsk = waiter->task;
	smp_mb();
	waiter->task = NULL;
	wake_up_process(tsk);
	put_task_struct(tsk);
	return sem;
}

/*
 * get a read lock on the semaphore
 */
/** 20150530    
 * rw_semaphore에서 read lock을 잡는다.
 *
 * activity가 >= 0이며 wait_list에 다른 task가 대기 중이지 않으면 lock을 잡는다.
 **/
void __sched __down_read(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;
	struct task_struct *tsk;
	unsigned long flags;

	/** 20150530    
	 * semaphore에 접근하기 전에 wait_lock을 잡는다.
	 **/
	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20150530    
	 * writer가 lock을 잡고 있지 않고 wait_list가 비어 있다면
	 *   reader semaphore 획득이 가능하므로 activity를 1 증가시킨다.
	 * (overflow에 대한 대응은???)
	 **/
	if (sem->activity >= 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity++;
		raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
		goto out;
	}

	/** 20150530    
	 * semaphore 획득이 불가능하므로 대기하기 위해
	 * 현재 task를 TASK_UNINTERRUPTIBLE로 변경한다.
	 **/
	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	/* set up my own style of waitqueue */
	/** 20150530    
	 * read로 waiter를 설정하고 wait_list에 등록시킨다.
	 **/
	waiter.task = tsk;
	waiter.flags = RWSEM_WAITING_FOR_READ;
	get_task_struct(tsk);

	list_add_tail(&waiter.list, &sem->wait_list);

	/* we don't need to touch the semaphore struct anymore */
	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);

	/* wait to be given the lock */
	/** 20150530    
	 * up에서 깨울 때 waiter.task를 비우므로
	 * 그 전까지는 schedule을 반복한다.
	 **/
	for (;;) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	/** 20150530    
	 * semaphore를 획득했으므로 state를 변경하고 리턴한다.
	 **/
	tsk->state = TASK_RUNNING;
 out:
	;
}

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
/** 20140531    
 * rw semaphore에서 reader side의 lock을 시도한다.
 * writer가 lock을 소유 중이거나 wait_list에 대기 중이지 않은 경우 lock을 획득할 수 있다.
 * 
 * 성공하면 1을 리턴, 실패시 0을 리턴
 **/
int __down_read_trylock(struct rw_semaphore *sem)
{
	unsigned long flags;
	int ret = 0;


	/** 20140531    
	 * interrupt를 막은 상태에서 spinlock을 건다.
	 **/
	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20140531    
	 * activity가 양수이면 writer가 lock을 소유하지 않은 상태이므로 reader는
	 * lock을 소유할 수 있다. writer가 대기하는 wait_list 역시 비어 있어야 한다.
	 **/
	if (sem->activity >= 0 && list_empty(&sem->wait_list)) {
		/* granted */
		/** 20140531    
		 * activity를 하나 증가시키고, return 값을 설정한다.
		 **/
		sem->activity++;
		ret = 1;
	}

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);

	return ret;
}

/*
 * get a write lock on the semaphore
 * - we increment the waiting count anyway to indicate an exclusive lock
 */
/** 20150530    
 * rw_semaphore에서 write lock을 잡는다.
 *
 * activity가 0이며 wait_list에 다른 task가 대기 중이지 않으면 lock을 잡는다.
 **/
void __sched __down_write_nested(struct rw_semaphore *sem, int subclass)
{
	struct rwsem_waiter waiter;
	struct task_struct *tsk;
	unsigned long flags;

	/** 20150530    
	 * semaphore 변경을 위한 wait_lock을 잡고 인터럽트를 막는다.
	 **/
	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20150530    
	 * activity가 없고 wait_list가 비어 있다면
	 *   writer semaphore 획득이 가능하므로 activity를 -1로 변경한다.
	 **/
	if (sem->activity == 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity = -1;
		raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
		goto out;
	}

	/** 20150530    
	 * semaphore 획득이 불가능하므로 대기하기 위해
	 * 현재 task를 TASK_UNINTERRUPTIBLE로 변경한다.
	 **/
	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	/* set up my own style of waitqueue */
	/** 20150530    
	 * writer로 waiter를 설정하고 wait_list에 등록시킨다.
	 **/
	waiter.task = tsk;
	waiter.flags = RWSEM_WAITING_FOR_WRITE;
	get_task_struct(tsk);

	list_add_tail(&waiter.list, &sem->wait_list);

	/* we don't need to touch the semaphore struct anymore */
	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);

	/* wait to be given the lock */
	/** 20150530    
	 * up_write에서 깨울 때 waiter.task를 비우므로
	 * 그 전까지는 schedule을 반복한다.
	 **/
	for (;;) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	/** 20150530    
	 * semaphore를 획득했으므로 state를 변경하고 리턴한다.
	 **/
	tsk->state = TASK_RUNNING;
 out:
	;
}

/** 20150530    
 * rw_semaphore의 write lock을 잡는다.
 **/
void __sched __down_write(struct rw_semaphore *sem)
{
	__down_write_nested(sem, 0);
}

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
/** 20150530    
 * writer lock 획득을 시도한다.
 *
 * rw lock에서 writer는 다른 writer나 reader가 모두 lock을 잡고 있지 않을 때만
 * lock을 획득할 수 있다.
 **/
int __down_write_trylock(struct rw_semaphore *sem)
{
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20150530    
	 * reader/writer 모두 lock을 잡고 있지 않고, wait_list가 비어 있을 때만
	 * writer lock을 잡을 수 있다.
	 *
	 * acitity를 writer가 잡은 상태로 변경하고, 성공을 리턴한다.
	 **/
	if (sem->activity == 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity = -1;
		ret = 1;
	}

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);

	return ret;
}

/*
 * release a read lock on the semaphore
 */
/** 20150530    
 * semaphore에서 read lock을 해제한다.
 *
 * wait_list에는 writer만 대기 중일 수 있으므로
 * wait_list가 비어있지 않을 때 writer를 깨운다.
 **/
void __up_read(struct rw_semaphore *sem)
{
	unsigned long flags;

	/** 20150530    
	 * semaphore에 접근할 때 irq를 막고 spinlock을 획득한다.
	 **/
	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20150530    
	 * reader가 줄었으므로 activity를 감소시키고,
	 * 그 결과 lock을 잡은 reader, writer가 없고 대기 중인 task가 있다면
	 * 대기 중인 writer를 하나 깨운다.
	 **/
	if (--sem->activity == 0 && !list_empty(&sem->wait_list))
		sem = __rwsem_wake_one_writer(sem);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
}

/*
 * release a write lock on the semaphore
 */
/** 20150530    
 * rw_semaphore의 write lock을 해제한다.
 *
 * writer에 의해 다른 writer나 다수의 reader가 잠들고 있을 수 있으므로
 * 가장 앞에 대기 중인 waiter 특성에 따라 깨운다.
 **/
void __up_write(struct rw_semaphore *sem)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	/** 20150530    
	 * 유일하게 lock을 잡고 있던 writer가 줄었으므로 activity를 0으로 설정한다.
	 * wait_list에 reader 또는 writer가 모두 대기 중일 수 있으므로
	 * wait_list가 비어있지 않다면 writer도 깨울 수 있도록 해 깨운다.
	 **/
	sem->activity = 0;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 1);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
}

/*
 * downgrade a write lock into a read lock
 * - just wake up any readers at the front of the queue
 */
void __downgrade_write(struct rw_semaphore *sem)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	sem->activity = 1;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 0);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
}

