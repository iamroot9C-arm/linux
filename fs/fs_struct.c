#include <linux/export.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/fs_struct.h>
#include "internal.h"

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block.
 */
/** 20150502    
 * fs_struct의 root path를 주어진 path로 지정한다.
 * 이전에 root가 지정되어 있었다면 put (reference count 감소 및 release처리)한다.
 **/
void set_fs_root(struct fs_struct *fs, struct path *path)
{
	struct path old_root;

	/** 20150502    
	 * 새로운 path의 reference count를 증가시킨다.
	 **/
	path_get(path);
	spin_lock(&fs->lock);
	/** 20150502    
	 * fs의 root/pwd 변경시 write sequnece lock을 사용한다.
	 **/
	write_seqcount_begin(&fs->seq);
	old_root = fs->root;
	fs->root = *path;
	write_seqcount_end(&fs->seq);
	spin_unlock(&fs->lock);
	/** 20150502    
	 * dentry가 존재하는지 검사하는데
	 * vfsmount와 dentry가 항상 같이 지정되기 때문에 하나만 검사한 것인지,
	 * vfsmount만 지정되어 있다면 path_put이 필요없다는 것인지???
	 **/
	if (old_root.dentry)
		path_put(&old_root);
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block.
 */
/** 20150502    
 * fs_struct의 pwd path를 주어진 path로 지정한다.
 * 이전에 root가 지정되어 있었다면 put (reference count 감소 및 release처리)한다.
 **/
void set_fs_pwd(struct fs_struct *fs, struct path *path)
{
	struct path old_pwd;

	/** 20150502    
	 * 새로운 path의 reference count를 증가시킨다.
	 **/
	path_get(path);
	spin_lock(&fs->lock);
	/** 20150502    
	 * fs의 struct path를 변경할 때는 write sequence lock이 사용된다.
	 **/
	write_seqcount_begin(&fs->seq);
	old_pwd = fs->pwd;
	fs->pwd = *path;
	write_seqcount_end(&fs->seq);
	spin_unlock(&fs->lock);

	/** 20150502    
	 * 이전 pwd가 지정되어 있었다면 put 한다.
	 **/
	if (old_pwd.dentry)
		path_put(&old_pwd);
}

static inline int replace_path(struct path *p, const struct path *old, const struct path *new)
{
	if (likely(p->dentry != old->dentry || p->mnt != old->mnt))
		return 0;
	*p = *new;
	return 1;
}

void chroot_fs_refs(struct path *old_root, struct path *new_root)
{
	struct task_struct *g, *p;
	struct fs_struct *fs;
	int count = 0;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_lock(p);
		fs = p->fs;
		if (fs) {
			int hits = 0;
			spin_lock(&fs->lock);
			write_seqcount_begin(&fs->seq);
			hits += replace_path(&fs->root, old_root, new_root);
			hits += replace_path(&fs->pwd, old_root, new_root);
			write_seqcount_end(&fs->seq);
			while (hits--) {
				count++;
				path_get(new_root);
			}
			spin_unlock(&fs->lock);
		}
		task_unlock(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
	while (count--)
		path_put(old_root);
}

void free_fs_struct(struct fs_struct *fs)
{
	path_put(&fs->root);
	path_put(&fs->pwd);
	kmem_cache_free(fs_cachep, fs);
}

void exit_fs(struct task_struct *tsk)
{
	struct fs_struct *fs = tsk->fs;

	if (fs) {
		int kill;
		task_lock(tsk);
		spin_lock(&fs->lock);
		tsk->fs = NULL;
		kill = !--fs->users;
		spin_unlock(&fs->lock);
		task_unlock(tsk);
		if (kill)
			free_fs_struct(fs);
	}
}

/** 20160409    
 * 자식 프로세스의 fs_struct을 할당받고 부모의 fs_struct을 복사한다.
 **/
struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
	/** 20160409    
	 * fs_struct을 kmem_cache로부터 할당 받는다.
	 *
	 * old를 복사한다.
	 * struct path를 복사하고, path_get에서 레퍼런스 카운트를 증가시킨다.
	 **/
	struct fs_struct *fs = kmem_cache_alloc(fs_cachep, GFP_KERNEL);
	/* We don't need to lock fs - think why ;-) */
	if (fs) {
		fs->users = 1;
		fs->in_exec = 0;
		spin_lock_init(&fs->lock);
		seqcount_init(&fs->seq);
		fs->umask = old->umask;

		/** 20160409    
		 * 부모의 fs_struct은 spinlock으로 보호되므로 lock을 잡고 참조한다.
		 **/
		spin_lock(&old->lock);
		fs->root = old->root;
		path_get(&fs->root);
		fs->pwd = old->pwd;
		path_get(&fs->pwd);
		spin_unlock(&old->lock);
	}
	return fs;
}

int unshare_fs_struct(void)
{
	struct fs_struct *fs = current->fs;
	struct fs_struct *new_fs = copy_fs_struct(fs);
	int kill;

	if (!new_fs)
		return -ENOMEM;

	task_lock(current);
	spin_lock(&fs->lock);
	kill = !--fs->users;
	current->fs = new_fs;
	spin_unlock(&fs->lock);
	task_unlock(current);

	if (kill)
		free_fs_struct(fs);

	return 0;
}
EXPORT_SYMBOL_GPL(unshare_fs_struct);

int current_umask(void)
{
	return current->fs->umask;
}
EXPORT_SYMBOL(current_umask);

/* to be mentioned only in INIT_TASK */
struct fs_struct init_fs = {
	.users		= 1,
	.lock		= __SPIN_LOCK_UNLOCKED(init_fs.lock),
	.seq		= SEQCNT_ZERO,
	.umask		= 0022,
};

void daemonize_fs_struct(void)
{
	struct fs_struct *fs = current->fs;

	if (fs) {
		int kill;

		task_lock(current);

		spin_lock(&init_fs.lock);
		init_fs.users++;
		spin_unlock(&init_fs.lock);

		spin_lock(&fs->lock);
		current->fs = &init_fs;
		kill = !--fs->users;
		spin_unlock(&fs->lock);

		task_unlock(current);
		if (kill)
			free_fs_struct(fs);
	}
}
