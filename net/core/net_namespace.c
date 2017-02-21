#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/workqueue.h>
#include <linux/rtnetlink.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/idr.h>
#include <linux/rculist.h>
#include <linux/nsproxy.h>
#include <linux/proc_fs.h>
#include <linux/file.h>
#include <linux/export.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

/*
 *	Our network namespace constructor/destructor lists
 */

/** 20151024
 * network namespace 리스트인 pernet_list.
 **/
static LIST_HEAD(pernet_list);
static struct list_head *first_device = &pernet_list;
static DEFINE_MUTEX(net_mutex);

LIST_HEAD(net_namespace_list);
EXPORT_SYMBOL_GPL(net_namespace_list);

/** 20151024
 * init_net.
 **/
struct net init_net = {
	.dev_base_head = LIST_HEAD_INIT(init_net.dev_base_head),
};
EXPORT_SYMBOL(init_net);

#define INITIAL_NET_GEN_PTRS	13 /* +1 for len +2 for rcu_head */

static unsigned int max_gen_ptrs = INITIAL_NET_GEN_PTRS;

/** 20151024
 * net_generic을 위한 메모리를 할당 받는다.
 **/
static struct net_generic *net_alloc_generic(void)
{
	/** 20151024
	 * max generic pointer 크기를 포함하여 메모리를 할당 받는다.
	 * len에 max_gen_ptrs의 수를 저장한다.
	 **/
	struct net_generic *ng;
	size_t generic_size = offsetof(struct net_generic, ptr[max_gen_ptrs]);

	ng = kzalloc(generic_size, GFP_KERNEL);
	if (ng)
		ng->len = max_gen_ptrs;

	return ng;
}

/** 20151024
 * net의 net_generic의 가변배열에 'id에 해당하는 data'를 저장한다.
 *
 * 기존 net_generic의 len보다 큰 id라면 가변 배열이 부족하므로
 * 메모리를 새로 할당해 기존 데이터를 복사해 추가하고, 기존 net_generic은 제거한다.
 **/
static int net_assign_generic(struct net *net, int id, void *data)
{
	struct net_generic *ng, *old_ng;

	BUG_ON(!mutex_is_locked(&net_mutex));
	BUG_ON(id == 0);

	/** 20151024
	 * 현재 net에 저장되어 있는 net_generic을 받아온다.
	 *
	 * 현재 net_generic이 저장할 ops id 이상이라면 assign된 것으로 간주한다.
	 **/
	old_ng = rcu_dereference_protected(net->gen,
					   lockdep_is_held(&net_mutex));
	ng = old_ng;
	if (old_ng->len >= id)
		goto assign;

	/** 20151024
	 * 새 net_generic을 할당 받는다.
	 **/
	ng = net_alloc_generic();
	if (ng == NULL)
		return -ENOMEM;

	/*
	 * Some synchronisation notes:
	 *
	 * The net_generic explores the net->gen array inside rcu
	 * read section. Besides once set the net->gen->ptr[x]
	 * pointer never changes (see rules in netns/generic.h).
	 *
	 * That said, we simply duplicate this array and schedule
	 * the old copy for kfree after a grace period.
	 */
	/** 20151024
	 * 기존 net_generic의 ptr는 그대로 복사한다.
	 **/
	memcpy(&ng->ptr, &old_ng->ptr, old_ng->len * sizeof(void*));

	/** 20151024
	 * 주어진 net의 net_generic 포인터가 새로 생성된 net_generic을 가리키게 한다.
	 * 기존의 net_generic은 삭제한다.
	 **/
	rcu_assign_pointer(net->gen, ng);
	kfree_rcu(old_ng, rcu);
assign:
	/** 20151024
	 * data를 새로 생성된 net_generic의 ptr 위치에 저장한다.
	 **/
	ng->ptr[id - 1] = data;
	return 0;
}

/** 20151024
 * ops의 id와 size가 있을 경우 net_generic에 등록하고,
 * init 함수가 제공될 경우 init 함수를 호출한다.
 **/
static int ops_init(const struct pernet_operations *ops, struct net *net)
{
	int err = -ENOMEM;
	void *data = NULL;

	/** 20151024
	 * id와 size가 주어졌다면 size크기만큼 0으로 초기화된 메모리를 받아온다.
	 **/
	if (ops->id && ops->size) {
		data = kzalloc(ops->size, GFP_KERNEL);
		if (!data)
			goto out;

		/** 20151024
		 * net의 net_generic의 ptr에 id에 해당하는 위치에 data를 저장한다.
		 **/
		err = net_assign_generic(net, *ops->id, data);
		if (err)
			goto cleanup;
	}
	err = 0;
	/** 20151024
	 * ops에서 init 함수를 제공하면 호출한다.
	 **/
	if (ops->init)
		err = ops->init(net);
	if (!err)
		return 0;

cleanup:
	kfree(data);

out:
	return err;
}

static void ops_free(const struct pernet_operations *ops, struct net *net)
{
	if (ops->id && ops->size) {
		int id = *ops->id;
		kfree(net_generic(net, id));
	}
}

static void ops_exit_list(const struct pernet_operations *ops,
			  struct list_head *net_exit_list)
{
	struct net *net;
	if (ops->exit) {
		list_for_each_entry(net, net_exit_list, exit_list)
			ops->exit(net);
	}
	if (ops->exit_batch)
		ops->exit_batch(net_exit_list);
}

static void ops_free_list(const struct pernet_operations *ops,
			  struct list_head *net_exit_list)
{
	struct net *net;
	if (ops->size && ops->id) {
		list_for_each_entry(net, net_exit_list, exit_list)
			ops_free(ops, net);
	}
}

/*
 * setup_net runs the initializers for the network namespace object.
 */
/** 20151024
 * network namespace object를 초기화 한다.
 **/
static __net_init int setup_net(struct net *net)
{
	/* Must be called with net_mutex held */
	const struct pernet_operations *ops, *saved_ops;
	int error = 0;
	LIST_HEAD(net_exit_list);

	atomic_set(&net->count, 1);
	atomic_set(&net->passive, 1);
	net->dev_base_seq = 1;

#ifdef NETNS_REFCNT_DEBUG
	atomic_set(&net->use_count, 0);
#endif

	/** 20151024
	 * pernet_list의 멤버를 순회하며 ops init으로 초기화 한다.
	 **/
	list_for_each_entry(ops, &pernet_list, list) {
		error = ops_init(ops, net);
		if (error < 0)
			goto out_undo;
	}
out:
	return error;

out_undo:
	/* Walk through the list backwards calling the exit functions
	 * for the pernet modules whose init functions did not fail.
	 */
	list_add(&net->exit_list, &net_exit_list);
	saved_ops = ops;
	list_for_each_entry_continue_reverse(ops, &pernet_list, list)
		ops_exit_list(ops, &net_exit_list);

	ops = saved_ops;
	list_for_each_entry_continue_reverse(ops, &pernet_list, list)
		ops_free_list(ops, &net_exit_list);

	rcu_barrier();
	goto out;
}


#ifdef CONFIG_NET_NS
static struct kmem_cache *net_cachep;
static struct workqueue_struct *netns_wq;

static struct net *net_alloc(void)
{
	struct net *net = NULL;
	struct net_generic *ng;

	ng = net_alloc_generic();
	if (!ng)
		goto out;

	net = kmem_cache_zalloc(net_cachep, GFP_KERNEL);
	if (!net)
		goto out_free;

	rcu_assign_pointer(net->gen, ng);
out:
	return net;

out_free:
	kfree(ng);
	goto out;
}

static void net_free(struct net *net)
{
#ifdef NETNS_REFCNT_DEBUG
	if (unlikely(atomic_read(&net->use_count) != 0)) {
		pr_emerg("network namespace not free! Usage: %d\n",
			 atomic_read(&net->use_count));
		return;
	}
#endif
	kfree(net->gen);
	kmem_cache_free(net_cachep, net);
}

void net_drop_ns(void *p)
{
	struct net *ns = p;
	if (ns && atomic_dec_and_test(&ns->passive))
		net_free(ns);
}

struct net *copy_net_ns(unsigned long flags, struct net *old_net)
{
	struct net *net;
	int rv;

	if (!(flags & CLONE_NEWNET))
		return get_net(old_net);

	net = net_alloc();
	if (!net)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&net_mutex);
	rv = setup_net(net);
	if (rv == 0) {
		rtnl_lock();
		list_add_tail_rcu(&net->list, &net_namespace_list);
		rtnl_unlock();
	}
	mutex_unlock(&net_mutex);
	if (rv < 0) {
		net_drop_ns(net);
		return ERR_PTR(rv);
	}
	return net;
}

static DEFINE_SPINLOCK(cleanup_list_lock);
static LIST_HEAD(cleanup_list);  /* Must hold cleanup_list_lock to touch */

static void cleanup_net(struct work_struct *work)
{
	const struct pernet_operations *ops;
	struct net *net, *tmp;
	LIST_HEAD(net_kill_list);
	LIST_HEAD(net_exit_list);

	/* Atomically snapshot the list of namespaces to cleanup */
	spin_lock_irq(&cleanup_list_lock);
	list_replace_init(&cleanup_list, &net_kill_list);
	spin_unlock_irq(&cleanup_list_lock);

	mutex_lock(&net_mutex);

	/* Don't let anyone else find us. */
	rtnl_lock();
	list_for_each_entry(net, &net_kill_list, cleanup_list) {
		list_del_rcu(&net->list);
		list_add_tail(&net->exit_list, &net_exit_list);
	}
	rtnl_unlock();

	/*
	 * Another CPU might be rcu-iterating the list, wait for it.
	 * This needs to be before calling the exit() notifiers, so
	 * the rcu_barrier() below isn't sufficient alone.
	 */
	synchronize_rcu();

	/* Run all of the network namespace exit methods */
	list_for_each_entry_reverse(ops, &pernet_list, list)
		ops_exit_list(ops, &net_exit_list);

	/* Free the net generic variables */
	list_for_each_entry_reverse(ops, &pernet_list, list)
		ops_free_list(ops, &net_exit_list);

	mutex_unlock(&net_mutex);

	/* Ensure there are no outstanding rcu callbacks using this
	 * network namespace.
	 */
	rcu_barrier();

	/* Finally it is safe to free my network namespace structure */
	list_for_each_entry_safe(net, tmp, &net_exit_list, exit_list) {
		list_del_init(&net->exit_list);
		net_drop_ns(net);
	}
}
static DECLARE_WORK(net_cleanup_work, cleanup_net);

void __put_net(struct net *net)
{
	/* Cleanup the network namespace in process context */
	unsigned long flags;

	spin_lock_irqsave(&cleanup_list_lock, flags);
	list_add(&net->cleanup_list, &cleanup_list);
	spin_unlock_irqrestore(&cleanup_list_lock, flags);

	queue_work(netns_wq, &net_cleanup_work);
}
EXPORT_SYMBOL_GPL(__put_net);

struct net *get_net_ns_by_fd(int fd)
{
	struct proc_inode *ei;
	struct file *file;
	struct net *net;

	file = proc_ns_fget(fd);
	if (IS_ERR(file))
		return ERR_CAST(file);

	ei = PROC_I(file->f_dentry->d_inode);
	if (ei->ns_ops == &netns_operations)
		net = get_net(ei->ns);
	else
		net = ERR_PTR(-EINVAL);

	fput(file);
	return net;
}

#else
struct net *copy_net_ns(unsigned long flags, struct net *old_net)
{
	if (flags & CLONE_NEWNET)
		return ERR_PTR(-EINVAL);
	return old_net;
}

struct net *get_net_ns_by_fd(int fd)
{
	return ERR_PTR(-EINVAL);
}
#endif

struct net *get_net_ns_by_pid(pid_t pid)
{
	struct task_struct *tsk;
	struct net *net;

	/* Lookup the network namespace */
	net = ERR_PTR(-ESRCH);
	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (tsk) {
		struct nsproxy *nsproxy;
		nsproxy = task_nsproxy(tsk);
		if (nsproxy)
			net = get_net(nsproxy->net_ns);
	}
	rcu_read_unlock();
	return net;
}
EXPORT_SYMBOL_GPL(get_net_ns_by_pid);

/** 20151024
 * network namespace를 초기화 한다.
 **/
static int __init net_ns_init(void)
{
	struct net_generic *ng;

	/** 20151024
	 * CONFIG_NET_NS 정의되지 않음.
	 **/
#ifdef CONFIG_NET_NS
	net_cachep = kmem_cache_create("net_namespace", sizeof(struct net),
					SMP_CACHE_BYTES,
					SLAB_PANIC, NULL);

	/* Create workqueue for cleanup */
	netns_wq = create_singlethread_workqueue("netns");
	if (!netns_wq)
		panic("Could not create netns workq");
#endif

	/** 20151024
	 * net_generic 구조체를 할당 받는다.
	 **/
	ng = net_alloc_generic();
	if (!ng)
		panic("Could not allocate generic netns");

	/** 20151024
	 * struct net의 gen 포인터에 할당 받은 주소를 저장한다.
	 **/
	rcu_assign_pointer(init_net.gen, ng);

	mutex_lock(&net_mutex);
	/** 20151024
	 * mutex 구간 안에서 init_net을 초기화 한다.
	 **/
	if (setup_net(&init_net))
		panic("Could not setup the initial network namespace");

	rtnl_lock();
	/** 20151024
	 * network namespace 리스트에 init_net을 등록한다.
	 **/
	list_add_tail_rcu(&init_net.list, &net_namespace_list);
	rtnl_unlock();

	mutex_unlock(&net_mutex);

	return 0;
}

pure_initcall(net_ns_init);

#ifdef CONFIG_NET_NS
/** 20151031
 * 주어진 list에 pernet ops를 등록한다.
 * init 콜백이 존재하거나 id,size 정보가 주어졌으면 ops_init을 진행한다.
 **/
static int __register_pernet_operations(struct list_head *list,
					struct pernet_operations *ops)
{
	struct net *net;
	int error;
	LIST_HEAD(net_exit_list);

	/** 20151031
	 * pernet_operations를 list에 등록한다.
	 **/
	list_add_tail(&ops->list, list);
	/** 20151031
	 * init 콜백이 존재하거나 id와 size 정보가 존재하면
	 * 전체 namespace list를 순회하며 ops_init을 호출한다.
	 **/
	if (ops->init || (ops->id && ops->size)) {
		for_each_net(net) {
			error = ops_init(ops, net);
			if (error)
				goto out_undo;
			/** 20151031
			 * net에 추가 중 에러가 발생하면, 이미 다른 net_namespace에 등록한
			 * 정보까지 초기화 해야 하므로 지역 리스트에 추가해 둔다.
			 **/
			list_add_tail(&net->exit_list, &net_exit_list);
		}
	}
	return 0;

out_undo:
	/* If I have an error cleanup all namespaces I initialized */
	list_del(&ops->list);
	ops_exit_list(ops, &net_exit_list);
	ops_free_list(ops, &net_exit_list);
	return error;
}

static void __unregister_pernet_operations(struct pernet_operations *ops)
{
	struct net *net;
	LIST_HEAD(net_exit_list);

	list_del(&ops->list);
	for_each_net(net)
		list_add_tail(&net->exit_list, &net_exit_list);
	ops_exit_list(ops, &net_exit_list);
	ops_free_list(ops, &net_exit_list);
}

#else

static int __register_pernet_operations(struct list_head *list,
					struct pernet_operations *ops)
{
	return ops_init(ops, &init_net);
}

static void __unregister_pernet_operations(struct pernet_operations *ops)
{
	LIST_HEAD(net_exit_list);
	list_add(&init_net.exit_list, &net_exit_list);
	ops_exit_list(ops, &net_exit_list);
	ops_free_list(ops, &net_exit_list);
}

#endif /* CONFIG_NET_NS */

/** 20151024
 * net_generic용 ida 정의.
 **/
static DEFINE_IDA(net_generic_ids);

/** 20151031
 * list에 pernet ops를 등록한다.
 *
 * ops에 id포인터가 지정되어 있다면 id를 할당 받아 저장한다.
 * list에 ops를 추가하고 init 콜백이나 id/size 정보가 있다면 ops_init을 호출한다.
 **/
static int register_pernet_operations(struct list_head *list,
				      struct pernet_operations *ops)
{
	int error;

	/** 20151031
	 * ops에 id 포인터가 지정되어 있다면
	 **/
	if (ops->id) {
again:
		/** 20151024
		 * 1 보다 큰 id를 할당 받아 id 포인터가 가리키는 곳에 저장한다.
		 **/
		error = ida_get_new_above(&net_generic_ids, 1, ops->id);
		if (error < 0) {
			/** 20151031
			 * id를 위해 할당된 자원이 부족하다면 pool을 채우고 다시 시도한다.
			 **/
			if (error == -EAGAIN) {
				ida_pre_get(&net_generic_ids, GFP_KERNEL);
				goto again;
			}
			return error;
		}
		/** 20151024
		 * 할당 받은 id가 max_gen_ptrs보다 크면 갱신한다.
		 **/
		max_gen_ptrs = max_t(unsigned int, max_gen_ptrs, *ops->id);
	}
	/** 20151031
	 * list에 pernet ops를 추가한다.
	 **/
	error = __register_pernet_operations(list, ops);
	if (error) {
		rcu_barrier();
		if (ops->id)
			ida_remove(&net_generic_ids, *ops->id);
	}

	return error;
}

static void unregister_pernet_operations(struct pernet_operations *ops)
{
	
	__unregister_pernet_operations(ops);
	rcu_barrier();
	if (ops->id)
		ida_remove(&net_generic_ids, *ops->id);
}

/**
 *      register_pernet_subsys - register a network namespace subsystem
 *	@ops:  pernet operations structure for the subsystem
 *
 *	Register a subsystem which has init and exit functions
 *	that are called when network namespaces are created and
 *	destroyed respectively.
 *
 *	When registered all network namespace init functions are
 *	called for every existing network namespace.  Allowing kernel
 *	modules to have a race free view of the set of network namespaces.
 *
 *	When a new network namespace is created all of the init
 *	methods are called in the order in which they were registered.
 *
 *	When a network namespace is destroyed all of the exit methods
 *	are called in the reverse of the order with which they were
 *	registered.
 */
/** 20150509
 * network namespace 서브시스템을 등록한다.
 *
 * 20151031
 * init과 exit 함수를 가질 수 있으며, init 함수가 지정되면 등록 과정에서 호출된다.
 **/
int register_pernet_subsys(struct pernet_operations *ops)
{
	int error;
	mutex_lock(&net_mutex);
	error =  register_pernet_operations(first_device, ops);
	mutex_unlock(&net_mutex);
	return error;
}
EXPORT_SYMBOL_GPL(register_pernet_subsys);

/**
 *      unregister_pernet_subsys - unregister a network namespace subsystem
 *	@ops: pernet operations structure to manipulate
 *
 *	Remove the pernet operations structure from the list to be
 *	used when network namespaces are created or destroyed.  In
 *	addition run the exit method for all existing network
 *	namespaces.
 */
void unregister_pernet_subsys(struct pernet_operations *ops)
{
	mutex_lock(&net_mutex);
	unregister_pernet_operations(ops);
	mutex_unlock(&net_mutex);
}
EXPORT_SYMBOL_GPL(unregister_pernet_subsys);

/**
 *      register_pernet_device - register a network namespace device
 *	@ops:  pernet operations structure for the subsystem
 *
 *	Register a device which has init and exit functions
 *	that are called when network namespaces are created and
 *	destroyed respectively.
 *
 *	When registered all network namespace init functions are
 *	called for every existing network namespace.  Allowing kernel
 *	modules to have a race free view of the set of network namespaces.
 *
 *	When a new network namespace is created all of the init
 *	methods are called in the order in which they were registered.
 *
 *	When a network namespace is destroyed all of the exit methods
 *	are called in the reverse of the order with which they were
 *	registered.
 */
int register_pernet_device(struct pernet_operations *ops)
{
	int error;
	mutex_lock(&net_mutex);
	error = register_pernet_operations(&pernet_list, ops);
	if (!error && (first_device == &pernet_list))
		first_device = &ops->list;
	mutex_unlock(&net_mutex);
	return error;
}
EXPORT_SYMBOL_GPL(register_pernet_device);

/**
 *      unregister_pernet_device - unregister a network namespace netdevice
 *	@ops: pernet operations structure to manipulate
 *
 *	Remove the pernet operations structure from the list to be
 *	used when network namespaces are created or destroyed.  In
 *	addition run the exit method for all existing network
 *	namespaces.
 */
void unregister_pernet_device(struct pernet_operations *ops)
{
	mutex_lock(&net_mutex);
	if (&ops->list == first_device)
		first_device = first_device->next;
	unregister_pernet_operations(ops);
	mutex_unlock(&net_mutex);
}
EXPORT_SYMBOL_GPL(unregister_pernet_device);

#ifdef CONFIG_NET_NS
static void *netns_get(struct task_struct *task)
{
	struct net *net = NULL;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	nsproxy = task_nsproxy(task);
	if (nsproxy)
		net = get_net(nsproxy->net_ns);
	rcu_read_unlock();

	return net;
}

static void netns_put(void *ns)
{
	put_net(ns);
}

static int netns_install(struct nsproxy *nsproxy, void *ns)
{
	put_net(nsproxy->net_ns);
	nsproxy->net_ns = get_net(ns);
	return 0;
}

const struct proc_ns_operations netns_operations = {
	.name		= "net",
	.type		= CLONE_NEWNET,
	.get		= netns_get,
	.put		= netns_put,
	.install	= netns_install,
};
#endif
