/*
 * fs/sysfs/dir.c - sysfs core and dir operation implementation
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/idr.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/security.h>
#include <linux/hash.h>
#include "sysfs.h"

DEFINE_MUTEX(sysfs_mutex);
DEFINE_SPINLOCK(sysfs_assoc_lock);

/** 20150411
 * rb_node를 포함하는 sysfs_dirent를 가져온다.
 **/
#define to_sysfs_dirent(X) rb_entry((X), struct sysfs_dirent, s_rb);

static DEFINE_SPINLOCK(sysfs_ino_lock);
static DEFINE_IDA(sysfs_ino_ida);

/**
 *	sysfs_name_hash
 *	@ns:   Namespace tag to hash
 *	@name: Null terminated string to hash
 *
 *	Returns 31 bit hash of ns + name (so it fits in an off_t )
 */
/** 20150411
 * ns와 name을 바탕으로 hash값을 뽑아낸다.
 *
 * 자세한 분석 생략???
 **/
static unsigned int sysfs_name_hash(const void *ns, const char *name)
{
	unsigned long hash = init_name_hash();
	unsigned int len = strlen(name);
	while (len--)
		hash = partial_name_hash(*name++, hash);
	hash = ( end_name_hash(hash) ^ hash_ptr( (void *)ns, 31 ) );
	hash &= 0x7fffffffU;
	/* Reserve hash numbers 0, 1 and INT_MAX for magic directory entries */
	if (hash < 1)
		hash += 2;
	if (hash >= INT_MAX)
		hash = INT_MAX - 1;
	return hash;
}

/** 20150411
 * sysfs_dirent 비교시 평가순서
 *   hash -> ns -> s_name
 **/
static int sysfs_name_compare(unsigned int hash, const void *ns,
	const char *name, const struct sysfs_dirent *sd)
{
	if (hash != sd->s_hash)
		return hash - sd->s_hash;
	if (ns != sd->s_ns)
		return ns - sd->s_ns;
	return strcmp(name, sd->s_name);
}

/** 20150411
 * sysfs_dirent 비교함수.
 * hash -> ns -> name 순으로 비교한다.
 **/
static int sysfs_sd_compare(const struct sysfs_dirent *left,
			    const struct sysfs_dirent *right)
{
	return sysfs_name_compare(left->s_hash, left->s_ns, left->s_name,
				  right);
}

/**
 *	sysfs_link_subling - link sysfs_dirent into sibling rbtree
 *	@sd: sysfs_dirent of interest
 *
 *	Link @sd into its sibling rbtree which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 *
 *	RETURNS:
 *	0 on susccess -EEXIST on failure.
 */
/** 20150411
 * 새로운 sysfs_dirent를 parent 아래 sibling rbtree에 연결한다.
 **/
static int sysfs_link_sibling(struct sysfs_dirent *sd)
{
	struct rb_node **node = &sd->s_parent->s_dir.children.rb_node;
	struct rb_node *parent = NULL;

	/** 20150411
	 * 추가할 sysfs_dirent가 디렉토리라면 parent의 subdirs 개수를 증가시킨다.
	 **/
	if (sysfs_type(sd) == SYSFS_DIR)
		sd->s_parent->s_dir.subdirs++;

	/** 20150411
	 * node가 NULL이 될 때까지 rb_tree를 탐색해 추가할 위치를 찾는다.
	 **/
	while (*node) {
		struct sysfs_dirent *pos;
		int result;

		/** 20150411
		 * node를 포함하는 sysfs_dirent를 가져온다.
		 **/
		pos = to_sysfs_dirent(*node);
		/** 20150411
		 * parent는 탐색이 이뤄질 때마다 갱신된다.
		 **/
		parent = *node;
		/** 20150411
		 * 추가할 sd와 node에 해당하는 sysfs_dirent를 비교해
		 * 다음 traverse할 위치를 판단한다.
		 **/
		result = sysfs_sd_compare(sd, pos);
		if (result < 0)
			node = &pos->s_rb.rb_left;
		else if (result > 0)
			node = &pos->s_rb.rb_right;
		else
			return -EEXIST;
	}
	/** 20150411
	 * rb_tree에 새로운 node를 추가하고,
	 * rb_tree의 규칙을 지키기 위해 rebalance 시킨다.
	 **/
	/* add new node and rebalance the tree */
	rb_link_node(&sd->s_rb, parent, node);
	rb_insert_color(&sd->s_rb, &sd->s_parent->s_dir.children);
	return 0;
}

/**
 *	sysfs_unlink_sibling - unlink sysfs_dirent from sibling rbtree
 *	@sd: sysfs_dirent of interest
 *
 *	Unlink @sd from its sibling rbtree which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 */
/** 20150418
 * 넘겨진 sysfs_dirent를 sibling rbtree에서 제거한다.
 **/
static void sysfs_unlink_sibling(struct sysfs_dirent *sd)
{
	/** 20150418
	 * sd가 directory라면 parent의 subdirs수를 감소시킨다.
	 **/
	if (sysfs_type(sd) == SYSFS_DIR)
		sd->s_parent->s_dir.subdirs--;

	/** 20150418
	 * parent의 children에서 sd에 해당하는 rb_node를 제거한다.
	 **/
	rb_erase(&sd->s_rb, &sd->s_parent->s_dir.children);
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/* Test for attributes that want to ignore lockdep for read-locking */
static bool ignore_lockdep(struct sysfs_dirent *sd)
{
	return sysfs_type(sd) == SYSFS_KOBJ_ATTR &&
			sd->s_attr.attr->ignore_lockdep;
}

#else

static inline bool ignore_lockdep(struct sysfs_dirent *sd)
{
	return true;
}

#endif

/**
 *	sysfs_get_active - get an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to get an active reference to
 *
 *	Get an active reference of @sd.  This function is noop if @sd
 *	is NULL.
 *
 *	RETURNS:
 *	Pointer to @sd on success, NULL on failure.
 */
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd)
{
	if (unlikely(!sd))
		return NULL;

	while (1) {
		int v, t;

		v = atomic_read(&sd->s_active);
		if (unlikely(v < 0))
			return NULL;

		t = atomic_cmpxchg(&sd->s_active, v, v + 1);
		if (likely(t == v))
			break;
		if (t < 0)
			return NULL;

		cpu_relax();
	}

	if (likely(!ignore_lockdep(sd)))
		rwsem_acquire_read(&sd->dep_map, 0, 1, _RET_IP_);
	return sd;
}

/**
 *	sysfs_put_active - put an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to put an active reference to
 *
 *	Put an active reference to @sd.  This function is noop if @sd
 *	is NULL.
 */
void sysfs_put_active(struct sysfs_dirent *sd)
{
	int v;

	if (unlikely(!sd))
		return;

	if (likely(!ignore_lockdep(sd)))
		rwsem_release(&sd->dep_map, 1, _RET_IP_);
	v = atomic_dec_return(&sd->s_active);
	if (likely(v != SD_DEACTIVATED_BIAS))
		return;

	/* atomic_dec_return() is a mb(), we'll always see the updated
	 * sd->u.completion.
	 */
	complete(sd->u.completion);
}

/**
 *	sysfs_deactivate - deactivate sysfs_dirent
 *	@sd: sysfs_dirent to deactivate
 *
 *	Deny new active references and drain existing ones.
 */
/** 20150418
 * SYSFS_KOBJ_ATTR | SYSFS_KOBJ_BIN_ATTR인 sysfs_dirent를 deactivate시킨다.
 * active시켜 사용 중인 곳이 있다면 wait_for_complete로 기다린다.
 **/
static void sysfs_deactivate(struct sysfs_dirent *sd)
{
	/** 20150418
	 * struct completion wait을 스택(함수 안이므로)에 선언하고 초기화 한다.
	 **/
	DECLARE_COMPLETION_ONSTACK(wait);
	int v;

	/** 20150418
	 * SYSFS_FLAG_REMOVED에 대해서만 deactivate가 호출되어야 한다.
	 **/
	BUG_ON(!(sd->s_flags & SYSFS_FLAG_REMOVED));

	/** 20150418
	 * active reference(SYSFS_KOBJ_ATTR | SYSFS_KOBJ_BIN_ATTR)에 해당한다.
	 **/
	if (!(sysfs_type(sd) & SYSFS_ACTIVE_REF))
		return;

	/** 20150418
	 * deactivate 시킬 sd의 주소에 wait을 넣는다.
	 **/
	sd->u.completion = (void *)&wait;

	rwsem_acquire(&sd->dep_map, 0, 0, _RET_IP_);
	/* atomic_add_return() is a mb(), put_active() will always see
	 * the updated sd->u.completion.
	 */
	/** 20150418
	 * s_active에 SD_DEACTIVATED_BIAS를 주고 결과값을 받아온다.
	 **/
	v = atomic_add_return(SD_DEACTIVATED_BIAS, &sd->s_active);

	/** 20150418
	 * s_active가 사용 중이라면, 사용 중인 곳에서 해제하고 complete()을
	 * 줄 때까지 대기한다.
	 *   get active 함수 : sysfs_get_active
	 *   put active 함수 : sysfs_put_active
	 **/
	if (v != SD_DEACTIVATED_BIAS) {
		lock_contended(&sd->dep_map, _RET_IP_);
		wait_for_completion(&wait);
	}

	lock_acquired(&sd->dep_map, _RET_IP_);
	rwsem_release(&sd->dep_map, 1, _RET_IP_);
}

/** 20150404
 * sysfs를 위한 ida에서 inode로 사용한 정수값을 할당 받는다.
 **/
static int sysfs_alloc_ino(unsigned int *pino)
{
	int ino, rc;

 retry:
	spin_lock(&sysfs_ino_lock);
	/** 20150404
	 * sysfs_ino_ida로부터 새로운 ino를 받아온다.
	 *
	 * sysfs_root의 ino가 1번으로 고정되어 있고,
	 * 새로운 dirent를 위해 받아오는 s_ino는 2이상의 값이다.
	 **/
	rc = ida_get_new_above(&sysfs_ino_ida, 2, &ino);
	spin_unlock(&sysfs_ino_lock);

	/** 20150404
	 * 미리 받아둔 ida가 부족하면 추가로 할당받아 채운 뒤 재시도 한다.
	 **/
	if (rc == -EAGAIN) {
		if (ida_pre_get(&sysfs_ino_ida, GFP_KERNEL))
			goto retry;
		rc = -ENOMEM;
	}

	/** 20150404
	 * 받아온 ino를 매개변수로 넘어온 곳에 저장한다.
	 **/
	*pino = ino;
	return rc;
}

/** 20150404
 * sysfs_ino_ida에서 ino를 제거한다.
 **/
static void sysfs_free_ino(unsigned int ino)
{
	spin_lock(&sysfs_ino_lock);
	ida_remove(&sysfs_ino_ida, ino);
	spin_unlock(&sysfs_ino_lock);
}

/** 20150404
 * sysfs_direct를 받아와 사용한 메모리를 해제하고 관련 자료구조를 정리한다.
 * 해당 sd를 제거하고, 부모 layer로 올라가 반복해 호출한다.
 *
 * 꼬리 재귀(tail recursion)의 형태를 repeat으로 구현함으로써 스택의 과도한
 * 사용을 막는 구조이다.
 * dput 등도 같은 원리로 구현되어 있다.
 **/
void release_sysfs_dirent(struct sysfs_dirent * sd)
{
	struct sysfs_dirent *parent_sd;

 repeat:
	/* Moving/renaming is always done while holding reference.
	 * sd->s_parent won't change beneath us.
	 */
	/** 20150404
	 * sysfs_dirent의 parent를 받아둔다.
	 **/
	parent_sd = sd->s_parent;

	/** 20150404
	 * sd의 sysfs_type이 kobj link라면 target_sd의 reference count를 감소시킨다.
	 **/
	if (sysfs_type(sd) == SYSFS_KOBJ_LINK)
		sysfs_put(sd->s_symlink.target_sd);
	/** 20150404
	 * sd의 sysfs_type에 SYSFS_COPY_NAME 속성이 있다면
	 * s_name을 위해 할당한 메모리를 해제한다.
	 **/
	if (sysfs_type(sd) & SYSFS_COPY_NAME)
		kfree(sd->s_name);
	if (sd->s_iattr && sd->s_iattr->ia_secdata)
		security_release_secctx(sd->s_iattr->ia_secdata,
					sd->s_iattr->ia_secdata_len);
	/** 20150404
	 * sd의 s_iaddtr를 해제한다.
	 **/
	kfree(sd->s_iattr);
	/** 20150404
	 * sysfs ida에서 받은 s_ino를 제거한다.
	 **/
	sysfs_free_ino(sd->s_ino);
	/** 20150404
	 * sd용 slub object를 해제한다.
	 **/
	kmem_cache_free(sysfs_dir_cachep, sd);

	/** 20150404
	 * 백업 받아둔 parent_sd가 존재한다면 reference count를 감소시키고,
	 * 반복해 호출한다.
	 **/
	sd = parent_sd;
	if (sd && atomic_dec_and_test(&sd->s_count))
		goto repeat;
}

/** 20150404
 * sysfs_dirent의 removed 속성을 검사해 결과를 리턴한다.
 **/
static int sysfs_dentry_delete(const struct dentry *dentry)
{
	/** 20150404
	 * dentry의 fsdata에 저장해둔 sysfs_dirent를 찾아와
	 * SYSFS_FLAG_REMOVED가 아니라면 false의 의미로 0이 리턴된다.
	 **/
	struct sysfs_dirent *sd = dentry->d_fsdata;
	return !(sd && !(sd->s_flags & SYSFS_FLAG_REMOVED));
}

static int sysfs_dentry_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct sysfs_dirent *sd;
	int is_dir;
	int type;

	if (flags & LOOKUP_RCU)
		return -ECHILD;

	sd = dentry->d_fsdata;
	mutex_lock(&sysfs_mutex);

	/* The sysfs dirent has been deleted */
	if (sd->s_flags & SYSFS_FLAG_REMOVED)
		goto out_bad;

	/* The sysfs dirent has been moved? */
	if (dentry->d_parent->d_fsdata != sd->s_parent)
		goto out_bad;

	/* The sysfs dirent has been renamed */
	if (strcmp(dentry->d_name.name, sd->s_name) != 0)
		goto out_bad;

	/* The sysfs dirent has been moved to a different namespace */
	type = KOBJ_NS_TYPE_NONE;
	if (sd->s_parent) {
		type = sysfs_ns_type(sd->s_parent);
		if (type != KOBJ_NS_TYPE_NONE &&
				sysfs_info(dentry->d_sb)->ns[type] != sd->s_ns)
			goto out_bad;
	}

	mutex_unlock(&sysfs_mutex);
out_valid:
	return 1;
out_bad:
	/* Remove the dentry from the dcache hashes.
	 * If this is a deleted dentry we use d_drop instead of d_delete
	 * so sysfs doesn't need to cope with negative dentries.
	 *
	 * If this is a dentry that has simply been renamed we
	 * use d_drop to remove it from the dcache lookup on its
	 * old parent.  If this dentry persists later when a lookup
	 * is performed at its new name the dentry will be readded
	 * to the dcache hashes.
	 */
	is_dir = (sysfs_type(sd) == SYSFS_DIR);
	mutex_unlock(&sysfs_mutex);
	if (is_dir) {
		/* If we have submounts we must allow the vfs caches
		 * to lie about the state of the filesystem to prevent
		 * leaks and other nasty things.
		 */
		if (have_submounts(dentry))
			goto out_valid;
		shrink_dcache_parent(dentry);
	}
	d_drop(dentry);
	return 0;
}

static void sysfs_dentry_release(struct dentry *dentry)
{
	sysfs_put(dentry->d_fsdata);
}

/** 20150404
 * dentry operations.
 **/
const struct dentry_operations sysfs_dentry_ops = {
	.d_revalidate	= sysfs_dentry_revalidate,
	.d_delete	= sysfs_dentry_delete,
	.d_release	= sysfs_dentry_release,
};

/** 20150418
 * 새로운 sysfs_dirent를 할당받고, name과 mode, type을 설정한다.
 **/
struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type)
{
	char *dup_name = NULL;
	struct sysfs_dirent *sd;

	/** 20150418
	 * type이 SYSFS_COPY_NAME 속성 중 하나에 해당하면 name을 복사한다.
	 * (SYSFS_DIR | SYSFS_KOBJ_LINK)
	 **/
	if (type & SYSFS_COPY_NAME) {
		name = dup_name = kstrdup(name, GFP_KERNEL);
		if (!name)
			return NULL;
	}

	/** 20150418
	 * sysfs_dir_cachep의 object(struct sysfs_dirent)를 kmem_cache에서 할당받는다.
	 **/
	sd = kmem_cache_zalloc(sysfs_dir_cachep, GFP_KERNEL);
	if (!sd)
		goto err_out1;

	/** 20150418
	 * sysfs를 위한 ida로부터 ino를 하나 할당받는다.
	 **/
	if (sysfs_alloc_ino(&sd->s_ino))
		goto err_out2;

	/** 20150418
	 * 할당받은 sd의 reference count와 active 정보를 초기화 한다.
	 **/
	atomic_set(&sd->s_count, 1);
	atomic_set(&sd->s_active, 0);

	/** 20150418
	 * name, mode, flag를 할당한다.
	 **/
	sd->s_name = name;
	sd->s_mode = mode;
	sd->s_flags = type;

	return sd;

 err_out2:
	kmem_cache_free(sysfs_dir_cachep, sd);
 err_out1:
	kfree(dup_name);
	return NULL;
}

/**
 *	sysfs_addrm_start - prepare for sysfs_dirent add/remove
 *	@acxt: pointer to sysfs_addrm_cxt to be used
 *	@parent_sd: parent sysfs_dirent
 *
 *	This function is called when the caller is about to add or
 *	remove sysfs_dirent under @parent_sd.  This function acquires
 *	sysfs_mutex.  @acxt is used to keep and pass context to
 *	other addrm functions.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  sysfs_mutex is locked on
 *	return.
 */
/** 20150411
 * parent_sd 아래에 sysfs_dirent를 추가/삭제할 때 호출한다.
 * 
 * sysfs_mutex lock을 걸고 리턴한다.
 **/
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd)
{
	/** 20150411
	 * sysfs에 add/rm시 사용할 context를 초기화 하고, parent_sd를 지정한다.
	 **/
	memset(acxt, 0, sizeof(*acxt));
	acxt->parent_sd = parent_sd;

	mutex_lock(&sysfs_mutex);
}

/**
 *	__sysfs_add_one - add sysfs_dirent to parent without warning
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
/** 20150411
 * 새로운 sysfs_dirent를 addrm_cxt에 저장된 parent에 추가한다.
 *
 * s_hash와 parent를 연결하고, sibling link를 연결한다.
 **/
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;
	int ret;

	/** 20150411
	 * parent의 sysfs_dirent의 ns_type과 sd의 ns_type의 유무를 비교해
	 * 같지 않으면 에러.
	 **/
	if (!!sysfs_ns_type(acxt->parent_sd) != !!sd->s_ns) {
		WARN(1, KERN_WARNING "sysfs: ns %s in '%s' for '%s'\n",
			sysfs_ns_type(acxt->parent_sd)? "required": "invalid",
			acxt->parent_sd->s_name, sd->s_name);
		return -EINVAL;
	}

	/** 20150411
	 * ns와 name을 바탕으로 hash값을 생성한다.
	 **/
	sd->s_hash = sysfs_name_hash(sd->s_ns, sd->s_name);
	/** 20150411
	 * context에서 가리키는 parent의 sysfs_dirent를 가져와(reference 증가)
	 * sd의 parent로 지정한다.
	 **/
	sd->s_parent = sysfs_get(acxt->parent_sd);

	/** 20150411
	 * sysfs_dirent를 sibling rb-tree에 추가한다.
	 **/
	ret = sysfs_link_sibling(sd);
	if (ret)
		return ret;

	/* Update timestamps on the parent */
	/** 20150411
	 * parent의 timestamps를 수정한다.
	 *
	 * data modification과 status change 시간을 현재 시간으로 갱신한다.
	 **/
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	return 0;
}

/**
 *	sysfs_pathname - return full path to sysfs dirent
 *	@sd: sysfs_dirent whose path we want
 *	@path: caller allocated buffer
 *
 *	Gives the name "/" to the sysfs_root entry; any path returned
 *	is relative to wherever sysfs is mounted.
 *
 *	XXX: does no error checking on @path size
 */
static char *sysfs_pathname(struct sysfs_dirent *sd, char *path)
{
	if (sd->s_parent) {
		sysfs_pathname(sd->s_parent, path);
		strcat(path, "/");
	}
	strcat(path, sd->s_name);
	return path;
}

/**
 *	sysfs_add_one - add sysfs_dirent to parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
/** 20150418
 * addrm context에서 acxt의 parent에 sd를 새로 추가한다.
 *
 * s_hash와 parent를 연결하고, sibling link를 연결한다.
 **/
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	int ret;

	/** 20150418
	 * acxt의 저장된 parent 아래에 sd를 추가한다.
	 * 이미 존재한다면 추가하지 못한다.
	 **/
	ret = __sysfs_add_one(acxt, sd);
	if (ret == -EEXIST) {
		char *path = kzalloc(PATH_MAX, GFP_KERNEL);
		WARN(1, KERN_WARNING
		     "sysfs: cannot create duplicate filename '%s'\n",
		     (path == NULL) ? sd->s_name :
		     strcat(strcat(sysfs_pathname(acxt->parent_sd, path), "/"),
		            sd->s_name));
		kfree(path);
	}

	return ret;
}

/**
 *	sysfs_remove_one - remove sysfs_dirent from parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be removed
 *
 *	Mark @sd removed and drop nlink of parent inode if @sd is a
 *	directory.  @sd is unlinked from the children list.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 */
/** 20150418
 * acxt에 기록된 parent에서 sd를 제거한다.
 *
 * sibling link에서 sd를 제거하고, add/rm context의 removed에 추가한다.
 **/
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;

	/** 20150418
	 * s_flags에 SYSFS_FLAG_REMOVED는 존재할 수 없다.
	 **/
	BUG_ON(sd->s_flags & SYSFS_FLAG_REMOVED);

	/** 20150418
	 * 제거할 sysfs_dirent를 sibling link에서 제거한다.
	 **/
	sysfs_unlink_sibling(sd);

	/* Update timestamps on the parent */
	/** 20150418
	 * parent의 inode attribute의 ctime과 mtime을 현재시간으로 갱신한다.
	 **/
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	/** 20150418
	 * s_flags에 SYSFS_FLAG_REMOVED 속성을 추가한다.
	 * add/rm context의 removed 리스트에 sd를 추가한다.
	 **/
	sd->s_flags |= SYSFS_FLAG_REMOVED;
	sd->u.removed_list = acxt->removed;
	acxt->removed = sd;
}

/**
 *	sysfs_addrm_finish - finish up sysfs_dirent add/remove
 *	@acxt: addrm context to finish up
 *
 *	Finish up sysfs_dirent add/remove.  Resources acquired by
 *	sysfs_addrm_start() are released and removed sysfs_dirents are
 *	cleaned up.
 *
 *	LOCKING:
 *	sysfs_mutex is released.
 */
/** 20150418
 * add/rm context를 종료한다.
 *
 * mutex lock을 해제하고, removed인 경우 sysfs에서 deactivate 시킨다.
 **/
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt)
{
	/* release resources acquired by sysfs_addrm_start() */
	mutex_unlock(&sysfs_mutex);

	/* kill removed sysfs_dirents */
	while (acxt->removed) {
		struct sysfs_dirent *sd = acxt->removed;

		/** 20150418
		 * sd를 제거할 것이므로 acxt->removed가 다음 노드를 가리키게 한다.
		 **/
		acxt->removed = sd->u.removed_list;

		/** 20150418
		 * sysfs_dirent를 deactivate 한다.
		 **/
		sysfs_deactivate(sd);
		/** 20150418
		 * SYSFS_KOBJ_BIN_ATTR라면 unmap 시킨다.
		 **/
		unmap_bin_file(sd);
		/** 20150418
		 * sysfs_dirent의 reference를 감소시키고, 0이 되었다면 해제한다.
		 **/
		sysfs_put(sd);
	}
}

/**
 *	sysfs_find_dirent - find sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd.
 *
 *	LOCKING:
 *	mutex_lock(sysfs_mutex)
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
/** 20150905
 * parent_sd 아래로 ns, name이 동일한 sysfs dirent를 찾아 리턴한다.
 **/
struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const void *ns,
				       const unsigned char *name)
{
	struct rb_node *node = parent_sd->s_dir.children.rb_node;
	unsigned int hash;

	if (!!sysfs_ns_type(parent_sd) != !!ns) {
		WARN(1, KERN_WARNING "sysfs: ns %s in '%s' for '%s'\n",
			sysfs_ns_type(parent_sd)? "required": "invalid",
			parent_sd->s_name, name);
		return NULL;
	}

	/** 20150905
	 * ns와 name으로 hash를 생성해
	 * RBtree를 node에서부터 순회하여 동일한 name을 가진 sysfs dirent를 찾는다.
	 **/
	hash = sysfs_name_hash(ns, name);
	while (node) {
		struct sysfs_dirent *sd;
		int result;

		/** 20150905
		 * node에 해당하는 sysfs dirent를 가져와 동일한 이름을 가지고 있는지
		 * 비교한다.  효율성을 높이기 위해 생성해둔 hash를 먼저 비교한다.
		 **/
		sd = to_sysfs_dirent(node);
		result = sysfs_name_compare(hash, ns, name, sd);
		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return sd;
	}
	return NULL;
}

/**
 *	sysfs_get_dirent - find and get sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd and get
 *	it if found.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  Grabs sysfs_mutex.
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
/** 20150905
 * parent sysfs dirent 아래에서 name을 가지는 sysfs dirent를 찾아 접근을 획득해
 * 리턴한다.
 **/
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const void *ns,
				      const unsigned char *name)
{
	struct sysfs_dirent *sd;

	mutex_lock(&sysfs_mutex);
	sd = sysfs_find_dirent(parent_sd, ns, name);
	sysfs_get(sd);
	mutex_unlock(&sysfs_mutex);

	return sd;
}
EXPORT_SYMBOL_GPL(sysfs_get_dirent);

/** 20150418
 * kobj에 새로운 sd를 생성해 parent_sd 아래에 추가한다.
 **/
static int create_dir(struct kobject *kobj, struct sysfs_dirent *parent_sd,
	enum kobj_ns_type type, const void *ns, const char *name,
	struct sysfs_dirent **p_sd)
{
	umode_t mode = S_IFDIR| S_IRWXU | S_IRUGO | S_IXUGO;
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent *sd;
	int rc;

	/* allocate */
	/** 20150418
	 * 새로운 sysfs_dirent를 할당받고 name, mode, SYSFS_DIR로 초기화 한다.
	 **/
	sd = sysfs_new_dirent(name, mode, SYSFS_DIR);
	if (!sd)
		return -ENOMEM;

	/** 20150418
	 * s_flags, s_ns, s_dir를 채운다.
	 * DIR를 생성하므로 union은 s_dir으로 접근한다.
	 **/
	sd->s_flags |= (type << SYSFS_NS_TYPE_SHIFT);
	sd->s_ns = ns;
	sd->s_dir.kobj = kobj;

	/* link in */
	/** 20150411
	 * sysfs에 sd를 add 또는 rm를 할 때 mutex lock을 건다.
	 * sysfs addrm context에 parent_sd를 지정하고,
	 * context를 넘겨 sd를 parent_sd에 추가한다.
	 **/
	sysfs_addrm_start(&acxt, parent_sd);
	rc = sysfs_add_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);

	/** 20150418
	 * 성공적으로 추가되었다면 매개변수로 받은 포인터에 저장하고,
	 * 그렇지 않다면 새로 생성한 sd를 삭제하기 위해 sysfs_put을 호출한다.
	 **/
	if (rc == 0)
		*p_sd = sd;
	else
		sysfs_put(sd);

	return rc;
}

/** 20150905
 * kobject에 name이라는 이름의 하위 디렉토리를 생성해 kobj->sd아래에 추가한다.
 * 생성된 디렉토리 sysfs dirent는 p_sd에 저장된다.
 **/
int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd)
{
	return create_dir(kobj, kobj->sd,
			  KOBJ_NS_TYPE_NONE, NULL, name, p_sd);
}

/**
 *	sysfs_read_ns_type: return associated ns_type
 *	@kobj: the kobject being queried
 *
 *	Each kobject can be tagged with exactly one namespace type
 *	(i.e. network or user).  Return the ns_type associated with
 *	this object if any
 */
/** 20150411
 * kobj의 ktype 의 속성 중 kobj_ns_type_operations에 포함된 정보인
 * ns_type을 추출해 리턴한다.
 **/
static enum kobj_ns_type sysfs_read_ns_type(struct kobject *kobj)
{
	const struct kobj_ns_type_operations *ops;
	enum kobj_ns_type type;

	/** 20150411
	 * kobj의 ktype의 child_ns ops를 받아온다.
	 * ops자체가 NULL이라면 type이 없으므로 KOBJ_NS_TYPE_NONE 를 리턴.
	 **/
	ops = kobj_child_ns_ops(kobj);
	if (!ops)
		return KOBJ_NS_TYPE_NONE;

	/** 20150411
	 * ops의 type을 가져와 검사하고 리턴.
	 **/
	type = ops->type;
	BUG_ON(type <= KOBJ_NS_TYPE_NONE);
	BUG_ON(type >= KOBJ_NS_TYPES);
	BUG_ON(!kobj_ns_type_registered(type));

	return type;
}

/**
 *	sysfs_create_dir - create a directory for an object.
 *	@kobj:		object we're creating directory for. 
 */
/** 20150418
 * kobj를 위한 directory를 생성한다.
 **/
int sysfs_create_dir(struct kobject * kobj)
{
	enum kobj_ns_type type;
	struct sysfs_dirent *parent_sd, *sd;
	const void *ns = NULL;
	int error = 0;

	BUG_ON(!kobj);

	/** 20150411
	 * kobj에 parent가 있으면 parent의 정보를 가져오고,
	 * 없으면 root의 정보를 가져온다.
	 *
	 * sysfs_root의 sysfs_dirent는 sysfs/mount.c에 위치.
	 **/
	if (kobj->parent)
		parent_sd = kobj->parent->sd;
	else
		parent_sd = &sysfs_root;

	if (!parent_sd)
		return -ENOENT;

	/** 20150411
	 * parent_sd의 ns type이 존재하면 (NONE이 아니라면)
	 * kobj의 ns를 가져온다.
	 **/
	if (sysfs_ns_type(parent_sd))
		ns = kobj->ktype->namespace(kobj);
	/** 20150411
	 * kobj의 ns_type을 가져온다.
	 **/
	type = sysfs_read_ns_type(kobj);

	error = create_dir(kobj, parent_sd, type, ns, kobject_name(kobj), &sd);
	if (!error)
		kobj->sd = sd;
	return error;
}

static struct dentry * sysfs_lookup(struct inode *dir, struct dentry *dentry,
				unsigned int flags)
{
	struct dentry *ret = NULL;
	struct dentry *parent = dentry->d_parent;
	struct sysfs_dirent *parent_sd = parent->d_fsdata;
	struct sysfs_dirent *sd;
	struct inode *inode;
	enum kobj_ns_type type;
	const void *ns;

	mutex_lock(&sysfs_mutex);

	type = sysfs_ns_type(parent_sd);
	ns = sysfs_info(dir->i_sb)->ns[type];

	sd = sysfs_find_dirent(parent_sd, ns, dentry->d_name.name);

	/* no such entry */
	if (!sd) {
		ret = ERR_PTR(-ENOENT);
		goto out_unlock;
	}
	dentry->d_fsdata = sysfs_get(sd);

	/* attach dentry and inode */
	inode = sysfs_get_inode(dir->i_sb, sd);
	if (!inode) {
		ret = ERR_PTR(-ENOMEM);
		goto out_unlock;
	}

	/* instantiate and hash dentry */
	ret = d_materialise_unique(dentry, inode);
 out_unlock:
	mutex_unlock(&sysfs_mutex);
	return ret;
}

const struct inode_operations sysfs_dir_inode_operations = {
	.lookup		= sysfs_lookup,
	.permission	= sysfs_permission,
	.setattr	= sysfs_setattr,
	.getattr	= sysfs_getattr,
	.setxattr	= sysfs_setxattr,
};

/** 20150418
 * sysfs directory를 제거한다.
 **/
static void remove_dir(struct sysfs_dirent *sd)
{
	struct sysfs_addrm_cxt acxt;

	sysfs_addrm_start(&acxt, sd->s_parent);
	sysfs_remove_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);
}

void sysfs_remove_subdir(struct sysfs_dirent *sd)
{
	remove_dir(sd);
}


/** 20150418
 * directory type의 sysfs_dirent를 받아 하위 sysfs_dirent와 s_dir 자신을 제거한다.
 **/
static void __sysfs_remove_dir(struct sysfs_dirent *dir_sd)
{
	struct sysfs_addrm_cxt acxt;
	struct rb_node *pos;

	if (!dir_sd)
		return;

	pr_debug("sysfs %s: removing dir\n", dir_sd->s_name);
	/** 20150418
	 * 제거할 dir_sd를 parent sysfs_dirent로 하는
	 * sysfs add/rm context를 생성한다.
	 **/
	sysfs_addrm_start(&acxt, dir_sd);
	/** 20150418
	 * rbtree에서 children hierarchy를 찾아 하나씩 제거한다.
	 **/
	pos = rb_first(&dir_sd->s_dir.children);
	while (pos) {
		struct sysfs_dirent *sd = to_sysfs_dirent(pos);
		pos = rb_next(pos);
		if (sysfs_type(sd) != SYSFS_DIR)
			sysfs_remove_one(&acxt, sd);
	}
	sysfs_addrm_finish(&acxt);

	/** 20150418
	 * 마지막으로 dir_sd 자신을 sysfs 상에서 제거한다.
	 **/
	remove_dir(dir_sd);
}

/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object.
 *
 *	The only thing special about this is that we remove any files in
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */

/** 20150418
 * kobj에 해당하는 sysfs 디렉토리를 제거한다.
 **/
void sysfs_remove_dir(struct kobject * kobj)
{
	/** 20150418
	 * kobject의 sysfs_dirent를 가져온다.
	 **/
	struct sysfs_dirent *sd = kobj->sd;

	/** 20150418
	 * kobj에서 sd를 분리한다.
	 **/
	spin_lock(&sysfs_assoc_lock);
	kobj->sd = NULL;
	spin_unlock(&sysfs_assoc_lock);

	/** 20150418
	 * kob는 sysfs_dirent에서 디렉토리이므로,
	 * sysfs 디렉토리 제거 함수에 sysfs_dirent를 전달해 제거한다.
	 **/
	__sysfs_remove_dir(sd);
}

int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const void *new_ns,
	const char *new_name)
{
	int error;

	mutex_lock(&sysfs_mutex);

	error = 0;
	if ((sd->s_parent == new_parent_sd) && (sd->s_ns == new_ns) &&
	    (strcmp(sd->s_name, new_name) == 0))
		goto out;	/* nothing to rename */

	error = -EEXIST;
	if (sysfs_find_dirent(new_parent_sd, new_ns, new_name))
		goto out;

	/* rename sysfs_dirent */
	if (strcmp(sd->s_name, new_name) != 0) {
		error = -ENOMEM;
		new_name = kstrdup(new_name, GFP_KERNEL);
		if (!new_name)
			goto out;

		kfree(sd->s_name);
		sd->s_name = new_name;
	}

	/* Move to the appropriate place in the appropriate directories rbtree. */
	sysfs_unlink_sibling(sd);
	sysfs_get(new_parent_sd);
	sysfs_put(sd->s_parent);
	sd->s_ns = new_ns;
	sd->s_hash = sysfs_name_hash(sd->s_ns, sd->s_name);
	sd->s_parent = new_parent_sd;
	sysfs_link_sibling(sd);

	error = 0;
 out:
	mutex_unlock(&sysfs_mutex);
	return error;
}

int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	struct sysfs_dirent *parent_sd = kobj->sd->s_parent;
	const void *new_ns = NULL;

	if (sysfs_ns_type(parent_sd))
		new_ns = kobj->ktype->namespace(kobj);

	return sysfs_rename(kobj->sd, parent_sd, new_ns, new_name);
}

int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent_kobj)
{
	struct sysfs_dirent *sd = kobj->sd;
	struct sysfs_dirent *new_parent_sd;
	const void *new_ns = NULL;

	BUG_ON(!sd->s_parent);
	if (sysfs_ns_type(sd->s_parent))
		new_ns = kobj->ktype->namespace(kobj);
	new_parent_sd = new_parent_kobj && new_parent_kobj->sd ?
		new_parent_kobj->sd : &sysfs_root;

	return sysfs_rename(sd, new_parent_sd, new_ns, sd->s_name);
}

/* Relationship between s_mode and the DT_xxx types */
static inline unsigned char dt_type(struct sysfs_dirent *sd)
{
	return (sd->s_mode >> 12) & 15;
}

static int sysfs_dir_release(struct inode *inode, struct file *filp)
{
	sysfs_put(filp->private_data);
	return 0;
}

static struct sysfs_dirent *sysfs_dir_pos(const void *ns,
	struct sysfs_dirent *parent_sd,	loff_t hash, struct sysfs_dirent *pos)
{
	if (pos) {
		int valid = !(pos->s_flags & SYSFS_FLAG_REMOVED) &&
			pos->s_parent == parent_sd &&
			hash == pos->s_hash;
		sysfs_put(pos);
		if (!valid)
			pos = NULL;
	}
	if (!pos && (hash > 1) && (hash < INT_MAX)) {
		struct rb_node *node = parent_sd->s_dir.children.rb_node;
		while (node) {
			pos = to_sysfs_dirent(node);

			if (hash < pos->s_hash)
				node = node->rb_left;
			else if (hash > pos->s_hash)
				node = node->rb_right;
			else
				break;
		}
	}
	/* Skip over entries in the wrong namespace */
	while (pos && pos->s_ns != ns) {
		struct rb_node *node = rb_next(&pos->s_rb);
		if (!node)
			pos = NULL;
		else
			pos = to_sysfs_dirent(node);
	}
	return pos;
}

static struct sysfs_dirent *sysfs_dir_next_pos(const void *ns,
	struct sysfs_dirent *parent_sd,	ino_t ino, struct sysfs_dirent *pos)
{
	pos = sysfs_dir_pos(ns, parent_sd, ino, pos);
	if (pos) do {
		struct rb_node *node = rb_next(&pos->s_rb);
		if (!node)
			pos = NULL;
		else
			pos = to_sysfs_dirent(node);
	} while (pos && pos->s_ns != ns);
	return pos;
}

static int sysfs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;
	struct sysfs_dirent *pos = filp->private_data;
	enum kobj_ns_type type;
	const void *ns;
	ino_t ino;

	type = sysfs_ns_type(parent_sd);
	ns = sysfs_info(dentry->d_sb)->ns[type];

	if (filp->f_pos == 0) {
		ino = parent_sd->s_ino;
		if (filldir(dirent, ".", 1, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	if (filp->f_pos == 1) {
		if (parent_sd->s_parent)
			ino = parent_sd->s_parent->s_ino;
		else
			ino = parent_sd->s_ino;
		if (filldir(dirent, "..", 2, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	mutex_lock(&sysfs_mutex);
	for (pos = sysfs_dir_pos(ns, parent_sd, filp->f_pos, pos);
	     pos;
	     pos = sysfs_dir_next_pos(ns, parent_sd, filp->f_pos, pos)) {
		const char * name;
		unsigned int type;
		int len, ret;

		name = pos->s_name;
		len = strlen(name);
		ino = pos->s_ino;
		type = dt_type(pos);
		filp->f_pos = pos->s_hash;
		filp->private_data = sysfs_get(pos);

		mutex_unlock(&sysfs_mutex);
		ret = filldir(dirent, name, len, filp->f_pos, ino, type);
		mutex_lock(&sysfs_mutex);
		if (ret < 0)
			break;
	}
	mutex_unlock(&sysfs_mutex);
	if ((filp->f_pos > 1) && !pos) { /* EOF */
		filp->f_pos = INT_MAX;
		filp->private_data = NULL;
	}
	return 0;
}


const struct file_operations sysfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= sysfs_readdir,
	.release	= sysfs_dir_release,
	.llseek		= generic_file_llseek,
};
