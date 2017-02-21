#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/poll.h>

/** 20150425
 * mnt_namespace 구조체.
 *
 * count : reference count. (alloc_mnt_ns에서 1로 시작)
 * list  : mount->mnt_list에 연결되는 list_head
 **/
struct mnt_namespace {
	atomic_t		count;
	struct mount *	root;
	struct list_head	list;
	wait_queue_head_t poll;
	int event;
};

/** 20150425
 * struct mount 내의 percpu 자료구조 
 * alloc_vfsmnt에서 alloc_percpu로 할당해 초기화 한다.
 *
 * mnt_count : mount 횟수.
 **/
struct mnt_pcp {
	int mnt_count;
	int mnt_writers;
};

/** 20150411
 * mount 구조체.
 *
 * vfsmount를 내부에 포함하고 있다.
 **/
struct mount {
	struct list_head mnt_hash;
	struct mount *mnt_parent;
	struct dentry *mnt_mountpoint;
	/** 20150411
	 * vfs에서 mount 될 때 사용되는 object 정보의 구조체.
	 **/
	struct vfsmount mnt;
#ifdef CONFIG_SMP
	/** 20150425
	 * SMP인 경우 percpu로 관리한다.
	 **/
	struct mnt_pcp __percpu *mnt_pcp;
#else
	int mnt_count;
	int mnt_writers;
#endif
	struct list_head mnt_mounts;	/* list of children, anchored here */
	struct list_head mnt_child;	/* and going through their mnt_child */
	/** 20150411
	 * struct super_block에 달리는 포인트.
	 **/
	struct list_head mnt_instance;	/* mount instance on sb->s_mounts */
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */
	/** 20150425
	 * struct mnt_namespace가 등록되는 포인트.
	 **/
	struct list_head mnt_list;
	struct list_head mnt_expire;	/* link in fs-specific expiry list */
	struct list_head mnt_share;	/* circular list of shared mounts */
	struct list_head mnt_slave_list;/* list of slave mounts */
	struct list_head mnt_slave;	/* slave list entry */
	struct mount *mnt_master;	/* slave is on master->mnt_slave_list */
	struct mnt_namespace *mnt_ns;	/* containing namespace */
#ifdef CONFIG_FSNOTIFY
	struct hlist_head mnt_fsnotify_marks;
	__u32 mnt_fsnotify_mask;
#endif
	int mnt_id;			/* mount identifier */
	int mnt_group_id;		/* peer group identifier */
	int mnt_expiry_mark;		/* true if marked for expiry */
	int mnt_pinned;
	int mnt_ghosts;
};

/** 20150411
 * unique한 mnt_namespace값.
 **/
#define MNT_NS_INTERNAL ERR_PTR(-EINVAL) /* distinct from any mnt_namespace */

/** 20150411
 * vfsmount를 포함하고 있는 mount 구조체를 추출한다.
 **/
static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static inline int mnt_has_parent(struct mount *mnt)
{
	return mnt != mnt->mnt_parent;
}

static inline int is_mounted(struct vfsmount *mnt)
{
	/* neither detached nor internal? */
	return !IS_ERR_OR_NULL(real_mount(mnt));
}

extern struct mount *__lookup_mnt(struct vfsmount *, struct dentry *, int);

/** 20150425
 * mnt_namespace의 reference count 증가.
 **/
static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	atomic_inc(&ns->count);
}

struct proc_mounts {
	struct seq_file m;
	struct mnt_namespace *ns;
	struct path root;
	int (*show)(struct seq_file *, struct vfsmount *);
};

#define proc_mounts(p) (container_of((p), struct proc_mounts, m))

extern const struct seq_operations mounts_op;
