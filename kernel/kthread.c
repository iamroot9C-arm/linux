/* Kernel thread helper functions.
 *   Copyright (C) 2004 IBM Corporation, Rusty Russell.
 *
 * Creation is done via kthreadd, so that we get a clean environment
 * even if we're invoked from userspace (think modprobe, hotplug cpu,
 * etc.).
 */
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/cpuset.h>
#include <linux/unistd.h>
#include <linux/file.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/freezer.h>
#include <trace/events/sched.h>

/** 20160213
 * kthread_create_list에 추가/삭제시 사용하는 lock.
 **/
static DEFINE_SPINLOCK(kthread_create_lock);
/** 20150801
 * kthread_create 할 구조체를 list로 관리
 *
 * kthreadd_task는 rest_init에서 생성해 저장.
 **/
static LIST_HEAD(kthread_create_list);
struct task_struct *kthreadd_task;

/** 20150801
 * kthreadd가 kthread 생성에 필요한 데이터를 관리하는 구조체.
 *
 * kthreadd로 전달해 주는 부분과 kthreadd로부터 결과를 리턴받는 부분으로 구성.
 **/
struct kthread_create_info
{
	/* Information passed to kthread() from kthreadd. */
	int (*threadfn)(void *data);
	void *data;
	int node;

	/* Result passed back to kthread_create() from kthreadd. */
	struct task_struct *result;
	struct completion done;

	struct list_head list;
};

/** 20150530
 **/
struct kthread {
	int should_stop;
	void *data;
	struct completion exited;
};

/** 20130720
 * task로 kthread 구조체의 포인터를 얻어 온다.
 * 
 * task를 받아서 task의 vfork_done (struct completion *)에 저장된 포인터 주소를
 * 포함하는 struct kthread 구조체의 시작 주소를 얻어 온다.
 **/
#define to_kthread(tsk)	\
	container_of((tsk)->vfork_done, struct kthread, exited)

/**
 * kthread_should_stop - should this kthread return now?
 *
 * When someone calls kthread_stop() on your kthread, it will be woken
 * and this will return true.  You should then return, and your return
 * value will be passed through to kthread_stop().
 */
/** 20140927
 * kthread_stop() 등에 의해 현재 task와 연결된 kthread가 멈춰야 하는지 검사한다.
 **/
int kthread_should_stop(void)
{
	return to_kthread(current)->should_stop;
}
EXPORT_SYMBOL(kthread_should_stop);

/**
 * kthread_freezable_should_stop - should this freezable kthread return now?
 * @was_frozen: optional out parameter, indicates whether %current was frozen
 *
 * kthread_should_stop() for freezable kthreads, which will enter
 * refrigerator if necessary.  This function is safe from kthread_stop() /
 * freezer deadlock and freezable kthreads should use this function instead
 * of calling try_to_freeze() directly.
 */
bool kthread_freezable_should_stop(bool *was_frozen)
{
	bool frozen = false;

	might_sleep();

	if (unlikely(freezing(current)))
		frozen = __refrigerator(true);

	if (was_frozen)
		*was_frozen = frozen;

	return kthread_should_stop();
}
EXPORT_SYMBOL_GPL(kthread_freezable_should_stop);

/**
 * kthread_data - return data value specified on kthread creation
 * @task: kthread task in question
 *
 * Return the data value specified when kthread @task was created.
 * The caller is responsible for ensuring the validity of @task when
 * calling this function.
 */
/** 20130720
 * task를 받아와 task에 해당하는 kthread의 data를 리턴한다.
 **/
void *kthread_data(struct task_struct *task)
{
	/** 20130720
	 * task의 vfork_done이 가리키는 kthread를 가져와 data 포인터를 리턴한다.
	 **/
	return to_kthread(task)->data;
}

/** 20160213
 * kernel thread 형태로 관리하기 위해 kthreadd()에 의해 task로 생성되는 함수.
 *
 * kthread_create 함수에서 전달받은 create 구조체에 저장된 함수를 수행해
 * kthread로 함수를 진행한다.
 *
 * do_exit()를 호출해 생성한 task의 리소스를 회수하고 task를 제거한다.
 **/
static int kthread(void *_create)
{
	/* Copy data: it's on kthread's stack */
	/** 20160213
	 * kthread_thread() 호출시 arg 위치에 지정된 create를 받아와 데이터를 꺼낸다. 
	 **/
	struct kthread_create_info *create = _create;
	int (*threadfn)(void *data) = create->threadfn;
	void *data = create->data;
	struct kthread self;
	int ret;

	self.should_stop = 0;
	self.data = data;
	init_completion(&self.exited);
	/** 20160213
	 * vfork_done 에서 complete는 self.exited로 지정.
	 **/
	current->vfork_done = &self.exited;

	/* OK, tell user we're spawned, wait for stop or wakeup */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	/** 20160213
	 * kthread_create_on_node()를 호출한 task에 complete를 전달한다.
	 **/
	create->result = current;
	complete(&create->done);
	schedule();

	ret = -EINTR;
	/** 20160213
	 * kthread_stop()이 threadfn 실행 전에 호출되었다면
	 * 한 번도 수행되지 않고 -EINTR을 리턴.
	 **/
	if (!self.should_stop)
		/** 20160213
		 * threadfn은 kthread_create_on_node(run_ksoftirqd)와 같은 함수를 통해
		 * 전달된 task. 보통 daemon 형태의 함수는 while문으로 계속 실행된다.
		 **/
		ret = threadfn(data);

	/* we can't just return, we must preserve "self" on stack */
	do_exit(ret);
}

/* called from do_fork() to get node information for about to be created task */
/** 20160227
 * 새 task에 저장할 node 정보를 리턴한다. 전달받은 tsk는 orig task.
 **/
int tsk_fork_get_node(struct task_struct *tsk)
{
#ifdef CONFIG_NUMA
	if (tsk == kthreadd_task)
		return tsk->pref_node_fork;
#endif
	return numa_node_id();
}

/** 20160213
 * kthread를 생성하고 수행한다.
 **/
static void create_kthread(struct kthread_create_info *create)
{
	int pid;

#ifdef CONFIG_NUMA
	current->pref_node_fork = create->node;
#endif
	/* We want our own signal handler (we take no signals by default). */
	/** 20160213
	 * kthread를 새로운 스레드로 생성한다.
	 * FS와 FILES를 공유하고, 자식 task가 종료되었을 때 시그널을 받는다.
	 * SIGCHLD를 받는 이유는???
	 **/
	pid = kernel_thread(kthread, create, CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		create->result = ERR_PTR(pid);
		complete(&create->done);
	}
}

/**
 * kthread_create_on_node - create a kthread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @node: memory node number.
 * @namefmt: printf-style name for the thread.
 *
 * Description: This helper function creates and names a kernel
 * thread.  The thread will be stopped: use wake_up_process() to start
 * it.  See also kthread_run().
 *
 * If thread is going to be bound on a particular cpu, give its node
 * in @node, to get NUMA affinity for kthread stack, or else give -1.
 * When woken, the thread will run @threadfn() with @data as its
 * argument. @threadfn() can either call do_exit() directly if it is a
 * standalone thread for which no one will call kthread_stop(), or
 * return when 'kthread_should_stop()' is true (which means
 * kthread_stop() has been called).  The return value should be zero
 * or a negative error number; it will be passed to kthread_stop().
 *
 * Returns a task_struct or ERR_PTR(-ENOMEM).
 */
/** 20150801
 * kthreadd에게 요청할 정보를 create에 채우고 리스트에 등록시켜
 * kthreadd가 준비된 이후 argument를 받아 새로운 kthread를 생성시킨다.
 *
 * threadfn : 새로운 kthread가 수행할 함수
 * data     : threadfn에 전달할 매개변수
 * node     : 수행될 노드. NUMA인 경우
 * namefmt  : task_struct의 comm에 지정되는 이름
 **/
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data,
					   int node,
					   const char namefmt[],
					   ...)
{
	struct kthread_create_info create;

	/** 20150801
	 * kthread_create_info에서 ktheadd로 전달할 자료구조를 채운다.
	 **/
	create.threadfn = threadfn;
	create.data = data;
	create.node = node;
	/** 20150801
	 * kthreadd의 complete를 기다릴 completion을 초기화 한다.
	 **/
	init_completion(&create.done);

	spin_lock(&kthread_create_lock);
	/** 20150801
	 * 전역 리스트에 kthread_create 요청 자료구조를 등록한다.
	 * kthread_create_list의 접근은 spinlock으로 보호된다.
	 **/
	list_add_tail(&create.list, &kthread_create_list);
	spin_unlock(&kthread_create_lock);

	/** 20150801
	 * kthreadd_task를 깨워 kthread 생성을 진행시킨다.
	 **/
	wake_up_process(kthreadd_task);
	/** 20150801
	 * kthreadd로부터 complete()를 기다리며 대기한다.
	 **/
	wait_for_completion(&create.done);

	/** 20150801
	 * result가 에러가 아니라면
	 **/
	if (!IS_ERR(create.result)) {
		static const struct sched_param param = { .sched_priority = 0 };
		va_list args;

		/** 20150801
		 * kthead_create을 요청한 곳에서 넘어온 이름대로 task의 comm을 지정.
		 **/
		va_start(args, namefmt);
		vsnprintf(create.result->comm, sizeof(create.result->comm),
			  namefmt, args);
		va_end(args);
		/*
		 * root may have changed our (kthreadd's) priority or CPU mask.
		 * The kernel thread should not inherit these properties.
		 */
		/** 20150801
		 * kthreadd로부터 생성된 task의 scheduling policy와 RT priority를 지정한다.
		 **/
		sched_setscheduler_nocheck(create.result, SCHED_NORMAL, &param);
		/** 20150801
		 * 생성된 task는 모든 cpu에서 수행될 수 있다.
		 **/
		set_cpus_allowed_ptr(create.result, cpu_all_mask);
	}
	return create.result;
}
EXPORT_SYMBOL(kthread_create_on_node);

/**
 * kthread_bind - bind a just-created kthread to a cpu.
 * @p: thread created by kthread_create().
 * @cpu: cpu (might not be online, must be possible) for @k to run on.
 *
 * Description: This function is equivalent to set_cpus_allowed(),
 * except that @cpu doesn't need to be online, and the thread must be
 * stopped (i.e., just returned from kthread_create()).
 */
/** 20140927
 * task를 특정 cpu에서 실행하도록 설정한다.
 * kthread를 생성한 뒤에 아직 run 상태가 아닐 때 설정한다.
 * 
 * task가 inactive가 될때까지 기다리는데 schedule()이 한 번도 호출되지 않았다면
 * bind 하지 않고 리턴한다.
 **/
void kthread_bind(struct task_struct *p, unsigned int cpu)
{
	/* Must have done schedule() in kthread() before we set_task_cpu */
	/** 20160130
	 * bind 시킬 task가 inactive, 즉 동작 중이지 않을 때까지 기다린다.
	 * 생성된 이후 kthread()에서 schedule()이 한 번 호출 되어야 한다.
	 **/
	if (!wait_task_inactive(p, TASK_UNINTERRUPTIBLE)) {
		WARN_ON(1);
		return;
	}

	/* It's safe because the task is inactive. */
	/** 20160130
	 * bind될 cpu로 mask를 생성해 task의 allowed로 지정하고 flags를 bound로 설정.
	 **/
	do_set_cpus_allowed(p, cpumask_of(cpu));
	p->flags |= PF_THREAD_BOUND;
}
EXPORT_SYMBOL(kthread_bind);

/**
 * kthread_stop - stop a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_stop() for @k to return true, wakes it, and
 * waits for it to exit. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will exit without
 * calling threadfn().
 *
 * If threadfn() may call do_exit() itself, the caller must ensure
 * task_struct can't go away.
 *
 * Returns the result of threadfn(), or %-EINTR if wake_up_process()
 * was never called.
 */
/** 20150530
 * kthread_create로 생성된 task를 정지시킨다.
 *
 * should_stop에 표시하고 task를 깨운 뒤, 완료를 기다리다가 완료되면
 * task의 리턴값을 반환하고 종료한다.
 **/
int kthread_stop(struct task_struct *k)
{
	struct kthread *kthread;
	int ret;

	trace_sched_kthread_stop(k);
	/** 20150530
	 * task_struct의 사용을 시작한다.
	 **/
	get_task_struct(k);

	/** 20150530
	 * task를 통해 kthread 구조체 위치를 찾아온다.
	 **/
	kthread = to_kthread(k);
	barrier(); /* it might have exited */
	/** 20150530
	 * vfork_done이 채워져 있다면 kthread가 할당되어 있는 경우다.
	 **/
	if (k->vfork_done != NULL) {
		/** 20150530
		 * 해당 kthread가 멈춰져야 한다고 기록하고,
		 * task를 깨운 뒤 complete 될 때까지 기다린다.
		 *
		 * complete()을 호출하는 위치는 complete_vfork_done()
		 * 예상되는 path:
		 *   do_exit -> exit_mm -> mm_release -> complete_vfork_done
		 **/
		kthread->should_stop = 1;
		wake_up_process(k);
		wait_for_completion(&kthread->exited);
	}
	/** 20150530
	 * kthread의 종료코드를 가져와 반환한다.
	 **/
	ret = k->exit_code;

	/** 20150530
	 * task_struct의 사용을 종료한다.
	 **/
	put_task_struct(k);
	trace_sched_kthread_stop_ret(ret);

	return ret;
}
EXPORT_SYMBOL(kthread_stop);

/** 20160213
 * kthread 생성 요청을 받아 kthread를 생성하는 kthreadd.
 *
 * kernel_init 이후 생성되어 pid 2번으로 수행된다.
 **/
int kthreadd(void *unused)
{
	struct task_struct *tsk = current;

	/* Setup a clean context for our children to inherit. */
	/** 20160206
	 * task의 이름을 kthreadd로 한다.
	 **/
	set_task_comm(tsk, "kthreadd");
	/** 20160213
	 * kthreadd는 시그널들을 무시한다.
	 **/
	ignore_signals(tsk);
	/** 20160213
	 * kthreadd는 모든 cpu에서 수행될 수 있다.
	 * 설정하지 않았을 경우 초기값은??? copy_process의 내용을 확인해야 할 듯.
	 **/
	set_cpus_allowed_ptr(tsk, cpu_all_mask);
	set_mems_allowed(node_states[N_HIGH_MEMORY]);

	/** 20160213
	 * kthreadd는 FREEZABLE 하지 않다.
	 *
	 * freezing_slow_path에서 false로 리턴.
	 **/
	current->flags |= PF_NOFREEZE;

	/** 20160213
	 * kthreadd는 생성된 후 계속 동작한다.
	 *
	 * 생성요청이 들어왔을 경우 kthread를 생성하고, 없을 경우 schedule로 대기.
	 **/
	for (;;) {
		/** 20160213
		 * 생성할 kthread가 없다면(리스트 대기열 확인) schedule.
		 *
		 * set_current_state()	; current->state 변경 후 barrier
		 *   schedule()			; current->state 변경
		 * __set_current_state  ; current->state 변경
		 **/
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&kthread_create_list))
			schedule();
		/** 20160213
		 * 상태를 다시 running으로 변경.
		 **/
		__set_current_state(TASK_RUNNING);

		spin_lock(&kthread_create_lock);
		/** 20160213
		 * kthread_create_list를 다 비울 때까지 kthread 생성 반복.
		 **/
		while (!list_empty(&kthread_create_list)) {
			struct kthread_create_info *create;

			/** 20160213
			 * kthread_create_info를 리스트에서 분리해 create_thread 함수 실행.
			 **/
			create = list_entry(kthread_create_list.next,
					    struct kthread_create_info, list);
			list_del_init(&create->list);
			spin_unlock(&kthread_create_lock);

			/** 20160213
			 * create 정보대로 kthread를 생성하고 kthread 내에서 함수로 수행한다.
			 **/
			create_kthread(create);

			spin_lock(&kthread_create_lock);
		}
		spin_unlock(&kthread_create_lock);
	}

	return 0;
}

void __init_kthread_worker(struct kthread_worker *worker,
				const char *name,
				struct lock_class_key *key)
{
	spin_lock_init(&worker->lock);
	lockdep_set_class_and_name(&worker->lock, key, name);
	INIT_LIST_HEAD(&worker->work_list);
	worker->task = NULL;
}
EXPORT_SYMBOL_GPL(__init_kthread_worker);

/**
 * kthread_worker_fn - kthread function to process kthread_worker
 * @worker_ptr: pointer to initialized kthread_worker
 *
 * This function can be used as @threadfn to kthread_create() or
 * kthread_run() with @worker_ptr argument pointing to an initialized
 * kthread_worker.  The started kthread will process work_list until
 * the it is stopped with kthread_stop().  A kthread can also call
 * this function directly after extra initialization.
 *
 * Different kthreads can be used for the same kthread_worker as long
 * as there's only one kthread attached to it at any given time.  A
 * kthread_worker without an attached kthread simply collects queued
 * kthread_works.
 */
int kthread_worker_fn(void *worker_ptr)
{
	struct kthread_worker *worker = worker_ptr;
	struct kthread_work *work;

	WARN_ON(worker->task);
	worker->task = current;
repeat:
	set_current_state(TASK_INTERRUPTIBLE);	/* mb paired w/ kthread_stop */

	if (kthread_should_stop()) {
		__set_current_state(TASK_RUNNING);
		spin_lock_irq(&worker->lock);
		worker->task = NULL;
		spin_unlock_irq(&worker->lock);
		return 0;
	}

	work = NULL;
	spin_lock_irq(&worker->lock);
	if (!list_empty(&worker->work_list)) {
		work = list_first_entry(&worker->work_list,
					struct kthread_work, node);
		list_del_init(&work->node);
	}
	worker->current_work = work;
	spin_unlock_irq(&worker->lock);

	if (work) {
		__set_current_state(TASK_RUNNING);
		work->func(work);
	} else if (!freezing(current))
		schedule();

	try_to_freeze();
	goto repeat;
}
EXPORT_SYMBOL_GPL(kthread_worker_fn);

/* insert @work before @pos in @worker */
static void insert_kthread_work(struct kthread_worker *worker,
			       struct kthread_work *work,
			       struct list_head *pos)
{
	lockdep_assert_held(&worker->lock);

	list_add_tail(&work->node, pos);
	work->worker = worker;
	if (likely(worker->task))
		wake_up_process(worker->task);
}

/**
 * queue_kthread_work - queue a kthread_work
 * @worker: target kthread_worker
 * @work: kthread_work to queue
 *
 * Queue @work to work processor @task for async execution.  @task
 * must have been created with kthread_worker_create().  Returns %true
 * if @work was successfully queued, %false if it was already pending.
 */
bool queue_kthread_work(struct kthread_worker *worker,
			struct kthread_work *work)
{
	bool ret = false;
	unsigned long flags;

	spin_lock_irqsave(&worker->lock, flags);
	if (list_empty(&work->node)) {
		insert_kthread_work(worker, work, &worker->work_list);
		ret = true;
	}
	spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(queue_kthread_work);

struct kthread_flush_work {
	struct kthread_work	work;
	struct completion	done;
};

static void kthread_flush_work_fn(struct kthread_work *work)
{
	struct kthread_flush_work *fwork =
		container_of(work, struct kthread_flush_work, work);
	complete(&fwork->done);
}

/**
 * flush_kthread_work - flush a kthread_work
 * @work: work to flush
 *
 * If @work is queued or executing, wait for it to finish execution.
 */
void flush_kthread_work(struct kthread_work *work)
{
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};
	struct kthread_worker *worker;
	bool noop = false;

retry:
	worker = work->worker;
	if (!worker)
		return;

	spin_lock_irq(&worker->lock);
	if (work->worker != worker) {
		spin_unlock_irq(&worker->lock);
		goto retry;
	}

	if (!list_empty(&work->node))
		insert_kthread_work(worker, &fwork.work, work->node.next);
	else if (worker->current_work == work)
		insert_kthread_work(worker, &fwork.work, worker->work_list.next);
	else
		noop = true;

	spin_unlock_irq(&worker->lock);

	if (!noop)
		wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(flush_kthread_work);

/**
 * flush_kthread_worker - flush all current works on a kthread_worker
 * @worker: worker to flush
 *
 * Wait until all currently executing or pending works on @worker are
 * finished.
 */
void flush_kthread_worker(struct kthread_worker *worker)
{
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};

	queue_kthread_work(worker, &fwork.work);
	wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(flush_kthread_worker);
