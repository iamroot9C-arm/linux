#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H

#include <linux/path.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>

/** 20150425
 * 각 task와 연관된 자료구조로 특정 파일시스템에 대한 정보를 나타낸다.
 *
 * root, pwd 지정시 set_fs_{root,pwd}가 사용된다.
 *
 * fs_struct 전체 동작은 spinlock으로 보호된다.
 * struct path인 root와 pwd는 sequence lock으로 보호된다.
 *   set_fs_root로 root 변경.
 **/
struct fs_struct {
	int users;
	spinlock_t lock;
	seqcount_t seq;
	int umask;
	int in_exec;
	struct path root, pwd;
};

extern struct kmem_cache *fs_cachep;

extern void exit_fs(struct task_struct *);
extern void set_fs_root(struct fs_struct *, struct path *);
extern void set_fs_pwd(struct fs_struct *, struct path *);
extern struct fs_struct *copy_fs_struct(struct fs_struct *);
extern void free_fs_struct(struct fs_struct *);
extern void daemonize_fs_struct(void);
extern int unshare_fs_struct(void);

static inline void get_fs_root(struct fs_struct *fs, struct path *root)
{
	spin_lock(&fs->lock);
	*root = fs->root;
	path_get(root);
	spin_unlock(&fs->lock);
}

static inline void get_fs_pwd(struct fs_struct *fs, struct path *pwd)
{
	spin_lock(&fs->lock);
	*pwd = fs->pwd;
	path_get(pwd);
	spin_unlock(&fs->lock);
}

static inline void get_fs_root_and_pwd(struct fs_struct *fs, struct path *root,
				       struct path *pwd)
{
	spin_lock(&fs->lock);
	*root = fs->root;
	path_get(root);
	*pwd = fs->pwd;
	path_get(pwd);
	spin_unlock(&fs->lock);
}

#endif /* _LINUX_FS_STRUCT_H */
