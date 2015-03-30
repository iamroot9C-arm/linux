/*
 * fs/sysfs/symlink.c - operations for initializing and mounting sysfs
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#define DEBUG 

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "sysfs.h"


static struct vfsmount *sysfs_mnt;
/** 20150228    
 * "struct sysfs_dirent" 용 kmem cache.
 **/
struct kmem_cache *sysfs_dir_cachep;

/** 20150314    
 * sysfs의 superblock ops.
 **/
static const struct super_operations sysfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.evict_inode	= sysfs_evict_inode,
};

/** 20150314    
 * sysfs의 root 노드의 dirent 정보.
 *
 * root의 inode는 1.
 * s_mode는 DIR이며, User/Group/Other에 읽기와 실행 퍼미션이 부여된다.
 **/
struct sysfs_dirent sysfs_root = {
	.s_name		= "",
	.s_count	= ATOMIC_INIT(1),
	.s_flags	= SYSFS_DIR | (KOBJ_NS_TYPE_NONE << SYSFS_NS_TYPE_SHIFT),
	.s_mode		= S_IFDIR | S_IRUGO | S_IXUGO,
	.s_ino		= 1,
};

/** 20150321    
 *
 * sb에 정보를 채워 넣는다.
 *   struct super_operations 's_op'도 여기서 채워진다.
 **/
static int sysfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;
	struct dentry *root;

	/** 20150314    
	 * superblock의 속성을 채워넣는다.
	 **/
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = SYSFS_MAGIC;
	sb->s_op = &sysfs_ops;
	sb->s_time_gran = 1;

	/* get root inode, initialize and unlock it */
	/** 20150328    
	 * root의 inode를 받아온다.
	 **/
	mutex_lock(&sysfs_mutex);
	/** 20150321    
	 * sysfs_root를 위한 inode를 가져온다. 존재하지 않는다면 할당받아 초기화 한다.
	 *
	 * sysfs_root의 s_ino는 1.
	 **/
	inode = sysfs_get_inode(sb, &sysfs_root);
	mutex_unlock(&sysfs_mutex);
	if (!inode) {
		pr_debug("sysfs: could not get root inode\n");
		return -ENOMEM;
	}

	/* instantiate and link root dentry */
	root = d_make_root(inode);
	if (!root) {
		pr_debug("%s: could not get root dentry!\n",__func__);
		return -ENOMEM;
	}
	root->d_fsdata = &sysfs_root;
	sb->s_root = root;
	sb->s_d_op = &sysfs_dentry_ops;
	return 0;
}

static int sysfs_test_super(struct super_block *sb, void *data)
{
	/** 20150307    
	 * sb의 fs private 정보를 가져온다.
	 **/
	struct sysfs_super_info *sb_info = sysfs_info(sb);
	struct sysfs_super_info *info = data;
	enum kobj_ns_type type;
	int found = 1;

	/** 20150307    
	 * KOBJ NS 타입들을 순회하며 sb에서 찾아온 정보와 
	 **/
	for (type = KOBJ_NS_TYPE_NONE; type < KOBJ_NS_TYPES; type++) {
		if (sb_info->ns[type] != info->ns[type])
			found = 0;
	}
	return found;
}

static int sysfs_set_super(struct super_block *sb, void *data)
{
	int error;
	error = set_anon_super(sb, data);
	if (!error)
		sb->s_fs_info = data;
	return error;
}

/** 20150314    
 **/
static void free_sysfs_super_info(struct sysfs_super_info *info)
{
	int type;
	for (type = KOBJ_NS_TYPE_NONE; type < KOBJ_NS_TYPES; type++)
		kobj_ns_drop(type, info->ns[type]);
	kfree(info);
}

/** 20150221    
 * sysfs용 mount 함수. super block에 대한 dentry를 받아와 리턴한다.
 *
 * sysfs는 superblock을 sget으로 받아온다.
 **/
static struct dentry *sysfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
	struct sysfs_super_info *info;
	enum kobj_ns_type type;
	struct super_block *sb;
	int error;

	/** 20150307    
	 * sysfs 특정 object 'sysfs_super_info' 생성.
	 **/
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	/** 20150307    
	 * 현재 type에 해당하는 grab 콜백으로 받아온 정보를 info의 ns에 저장한다.
	 **/
	for (type = KOBJ_NS_TYPE_NONE; type < KOBJ_NS_TYPES; type++)
		info->ns[type] = kobj_ns_grab_current(type);

	/** 20150314    
	 * 파일시스템의 superblock 중 info에 해당하는 superblock을 찾아 리턴한다.
	 * 없으면 새로 생성해 등록한 뒤 리턴한다.
	 **/
	sb = sget(fs_type, sysfs_test_super, sysfs_set_super, flags, info);
	/** 20150314    
	 * 에러가 리턴되거나, 지정한 info로 s_fs_info가 설정되지 않았다면
	 * sysfs의 superblock info를 해제한다.
	 **/
	if (IS_ERR(sb) || sb->s_fs_info != info)
		free_sysfs_super_info(info);
	if (IS_ERR(sb))
		return ERR_CAST(sb);
	/** 20150314    
	 * superblock의 dentry (s_root)가 존재하지 않는다면
	 * sget에서 새로 만들어진 superblock이므로 정보를 채워 넣는다.
	 **/
	if (!sb->s_root) {
		error = sysfs_fill_super(sb, data, flags & MS_SILENT ? 1 : 0);
		if (error) {
			deactivate_locked_super(sb);
			return ERR_PTR(error);
		}
		sb->s_flags |= MS_ACTIVE;
	}

	return dget(sb->s_root);
}

/** 20150221    
 * 추후 분석???
 **/
static void sysfs_kill_sb(struct super_block *sb)
{
	struct sysfs_super_info *info = sysfs_info(sb);
	/* Remove the superblock from fs_supers/s_instances
	 * so we can't find it, before freeing sysfs_super_info.
	 */
	kill_anon_super(sb);
	free_sysfs_super_info(info);
}

/** 20150221    
 * sysfs fs type.
 **/
static struct file_system_type sysfs_fs_type = {
	.name		= "sysfs",
	.mount		= sysfs_mount,
	.kill_sb	= sysfs_kill_sb,
};

int __init sysfs_init(void)
{
	int err = -ENOMEM;

	/** 20150221    
	 * "sysfs_dir_cache" kmem cache를 생성한다.
	 **/
	sysfs_dir_cachep = kmem_cache_create("sysfs_dir_cache",
					      sizeof(struct sysfs_dirent),
					      0, 0, NULL);
	if (!sysfs_dir_cachep)
		goto out;

	/** 20150307    
	 * sysfs inode 관련 초기화 수행
	 **/
	err = sysfs_inode_init();
	if (err)
		goto out_err;

	/** 20150221    
	 * sysfs_fs_type을 filesystem 리스트에 등록한다.
	 *
	 * cat /proc/filesystems에서 확인 가능하다.
	 **/
	err = register_filesystem(&sysfs_fs_type);
	if (!err) {
		sysfs_mnt = kern_mount(&sysfs_fs_type);
		if (IS_ERR(sysfs_mnt)) {
			printk(KERN_ERR "sysfs: could not mount!\n");
			err = PTR_ERR(sysfs_mnt);
			sysfs_mnt = NULL;
			unregister_filesystem(&sysfs_fs_type);
			goto out_err;
		}
	} else
		goto out_err;
out:
	return err;
out_err:
	kmem_cache_destroy(sysfs_dir_cachep);
	sysfs_dir_cachep = NULL;
	goto out;
}

#undef sysfs_get
/** 20150328    
 * sysfs_dirent의 reference count를 증가시킨다.
 **/
struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd)
{
	return __sysfs_get(sd);
}
EXPORT_SYMBOL_GPL(sysfs_get);

#undef sysfs_put
void sysfs_put(struct sysfs_dirent *sd)
{
	__sysfs_put(sd);
}
EXPORT_SYMBOL_GPL(sysfs_put);
