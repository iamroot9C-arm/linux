/*
 * fs/sysfs/inode.c - basic sysfs inode and dentry operations
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

#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include "sysfs.h"

extern struct super_block * sysfs_sb;

/** 20150321    
 * sysfs address_space operations.
 **/
static const struct address_space_operations sysfs_aops = {
	.readpage	= simple_readpage,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
};

/** 20150221    
 * sysfs 관련 bdi 정의.
 *
 *   readahead를 사용하지 않고, ACCT와 WRITEBACK을 사용하지 않는다
 **/
static struct backing_dev_info sysfs_backing_dev_info = {
	.name		= "sysfs",
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK,
};

/** 20150321    
 * sysfs의 inode에 대한 operations.
 *
 * sysfs는 가상파일시스템이므로 create 등에 대한 opeartions는 별도로 정의되어 있지 않다.
 **/
static const struct inode_operations sysfs_inode_operations ={
	.permission	= sysfs_permission,
	.setattr	= sysfs_setattr,
	.getattr	= sysfs_getattr,
	.setxattr	= sysfs_setxattr,
};

/** 20150221    
 * sysfs_inode_init 에서는 sysfs bdi 초기화만 수행한다.
 **/
int __init sysfs_inode_init(void)
{
	return bdi_init(&sysfs_backing_dev_info);
}

static struct sysfs_inode_attrs *sysfs_init_inode_attrs(struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *attrs;
	struct iattr *iattrs;

	attrs = kzalloc(sizeof(struct sysfs_inode_attrs), GFP_KERNEL);
	if (!attrs)
		return NULL;
	iattrs = &attrs->ia_iattr;

	/* assign default attributes */
	iattrs->ia_mode = sd->s_mode;
	iattrs->ia_uid = GLOBAL_ROOT_UID;
	iattrs->ia_gid = GLOBAL_ROOT_GID;
	iattrs->ia_atime = iattrs->ia_mtime = iattrs->ia_ctime = CURRENT_TIME;

	return attrs;
}

int sysfs_sd_setattr(struct sysfs_dirent *sd, struct iattr * iattr)
{
	struct sysfs_inode_attrs *sd_attrs;
	struct iattr *iattrs;
	unsigned int ia_valid = iattr->ia_valid;

	sd_attrs = sd->s_iattr;

	if (!sd_attrs) {
		/* setting attributes for the first time, allocate now */
		sd_attrs = sysfs_init_inode_attrs(sd);
		if (!sd_attrs)
			return -ENOMEM;
		sd->s_iattr = sd_attrs;
	}
	/* attributes were changed at least once in past */
	iattrs = &sd_attrs->ia_iattr;

	if (ia_valid & ATTR_UID)
		iattrs->ia_uid = iattr->ia_uid;
	if (ia_valid & ATTR_GID)
		iattrs->ia_gid = iattr->ia_gid;
	if (ia_valid & ATTR_ATIME)
		iattrs->ia_atime = iattr->ia_atime;
	if (ia_valid & ATTR_MTIME)
		iattrs->ia_mtime = iattr->ia_mtime;
	if (ia_valid & ATTR_CTIME)
		iattrs->ia_ctime = iattr->ia_ctime;
	if (ia_valid & ATTR_MODE) {
		umode_t mode = iattr->ia_mode;
		iattrs->ia_mode = sd->s_mode = mode;
	}
	return 0;
}

int sysfs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	struct sysfs_dirent *sd = dentry->d_fsdata;
	int error;

	if (!sd)
		return -EINVAL;

	mutex_lock(&sysfs_mutex);
	error = inode_change_ok(inode, iattr);
	if (error)
		goto out;

	error = sysfs_sd_setattr(sd, iattr);
	if (error)
		goto out;

	/* this ignores size changes */
	setattr_copy(inode, iattr);

out:
	mutex_unlock(&sysfs_mutex);
	return error;
}

static int sysfs_sd_setsecdata(struct sysfs_dirent *sd, void **secdata, u32 *secdata_len)
{
	struct sysfs_inode_attrs *iattrs;
	void *old_secdata;
	size_t old_secdata_len;

	if (!sd->s_iattr) {
		sd->s_iattr = sysfs_init_inode_attrs(sd);
		if (!sd->s_iattr)
			return -ENOMEM;
	}

	iattrs = sd->s_iattr;
	old_secdata = iattrs->ia_secdata;
	old_secdata_len = iattrs->ia_secdata_len;

	iattrs->ia_secdata = *secdata;
	iattrs->ia_secdata_len = *secdata_len;

	*secdata = old_secdata;
	*secdata_len = old_secdata_len;
	return 0;
}

int sysfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		size_t size, int flags)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	void *secdata;
	int error;
	u32 secdata_len = 0;

	if (!sd)
		return -EINVAL;

	if (!strncmp(name, XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN)) {
		const char *suffix = name + XATTR_SECURITY_PREFIX_LEN;
		error = security_inode_setsecurity(dentry->d_inode, suffix,
						value, size, flags);
		if (error)
			goto out;
		error = security_inode_getsecctx(dentry->d_inode,
						&secdata, &secdata_len);
		if (error)
			goto out;

		mutex_lock(&sysfs_mutex);
		error = sysfs_sd_setsecdata(sd, &secdata, &secdata_len);
		mutex_unlock(&sysfs_mutex);

		if (secdata)
			security_release_secctx(secdata, secdata_len);
	} else
		return -EINVAL;
out:
	return error;
}

/** 20150328    
 * 기본 inode 속성 지정.
 **/
static inline void set_default_inode_attr(struct inode * inode, umode_t mode)
{
	inode->i_mode = mode;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

/** 20150328    
 * inode의 권한, 관리시간 정보를 iattr로 설정한다.
 **/
static inline void set_inode_attr(struct inode * inode, struct iattr * iattr)
{
	inode->i_uid = iattr->ia_uid;
	inode->i_gid = iattr->ia_gid;
	inode->i_atime = iattr->ia_atime;
	inode->i_mtime = iattr->ia_mtime;
	inode->i_ctime = iattr->ia_ctime;
}

/** 20150328    
 * inode 정보를 갱신한다.
 **/
static void sysfs_refresh_inode(struct sysfs_dirent *sd, struct inode *inode)
{
	struct sysfs_inode_attrs *iattrs = sd->s_iattr;

	/** 20150328    
	 * sd의 s_mode로 inode의 i_mode 지정
	 **/
	inode->i_mode = sd->s_mode;
	/** 20150328    
	 * sysfs_dirent는 기본 속성이 지정되어 있지 않으므로 sysfs_dirent의 attr를
	 * 복사한다.
	 **/
	if (iattrs) {
		/* sysfs_dirent has non-default attributes
		 * get them from persistent copy in sysfs_dirent
		 */
		set_inode_attr(inode, &iattrs->ia_iattr);
		security_inode_notifysecctx(inode,
					    iattrs->ia_secdata,
					    iattrs->ia_secdata_len);
	}

	/** 20150328    
	 * sd의 type이 directory라면 nlink를 subdirs + 2로 설정한다.
	 **/
	if (sysfs_type(sd) == SYSFS_DIR)
		set_nlink(inode, sd->s_dir.subdirs + 2);
}

int sysfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	struct inode *inode = dentry->d_inode;

	mutex_lock(&sysfs_mutex);
	sysfs_refresh_inode(sd, inode);
	mutex_unlock(&sysfs_mutex);

	generic_fillattr(inode, stat);
	return 0;
}

/** 20150328    
 * sd에 맞게 inode의 정보를 초기화 한다.
 **/
static void sysfs_init_inode(struct sysfs_dirent *sd, struct inode *inode)
{
	struct bin_attribute *bin_attr;

	/** 20150321    
	 * inode의 i_private에는 sysfs_dirent 포인터(sysfs_root의 주소)를 저장한다.
	 * a_ops에 sysfs_aops 저장
	 **/
	inode->i_private = sysfs_get(sd);
	inode->i_mapping->a_ops = &sysfs_aops;
	inode->i_mapping->backing_dev_info = &sysfs_backing_dev_info;
	inode->i_op = &sysfs_inode_operations;

	set_default_inode_attr(inode, sd->s_mode);
	sysfs_refresh_inode(sd, inode);

	/* initialize inode according to type */
	/** 20150328    
	 * sd의 type에 따른 동작을 수행한다.
	 **/
	switch (sysfs_type(sd)) {
	case SYSFS_DIR:
		inode->i_op = &sysfs_dir_inode_operations;
		inode->i_fop = &sysfs_dir_operations;
		break;
	case SYSFS_KOBJ_ATTR:
		inode->i_size = PAGE_SIZE;
		inode->i_fop = &sysfs_file_operations;
		break;
	case SYSFS_KOBJ_BIN_ATTR:
		bin_attr = sd->s_bin_attr.bin_attr;
		inode->i_size = bin_attr->size;
		inode->i_fop = &bin_fops;
		break;
	case SYSFS_KOBJ_LINK:
		inode->i_op = &sysfs_symlink_inode_operations;
		break;
	default:
		BUG();
	}

	unlock_new_inode(inode);
}

/**
 *	sysfs_get_inode - get inode for sysfs_dirent
 *	@sb: super block
 *	@sd: sysfs_dirent to allocate inode for
 *
 *	Get inode for @sd.  If such inode doesn't exist, a new inode
 *	is allocated and basics are initialized.  New inode is
 *	returned locked.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).
 *
 *	RETURNS:
 *	Pointer to allocated inode on success, NULL on failure.
 */
/** 20150328    
 * sysfs_dirent를 위한 inode를 가져온다.
 * 이미 존재하지 않는다면 새로 할당받아 초기화 한다.
 **/
struct inode * sysfs_get_inode(struct super_block *sb, struct sysfs_dirent *sd)
{
	struct inode *inode;

	/** 20150321    
	 * 해당 superblock에서 sysfs_dirent의 inode 번호로 inode를 찾아온다.
	 * 존재하지 않는다면 할당받아 설정한다.
	 **/
	inode = iget_locked(sb, sd->s_ino);
	if (inode && (inode->i_state & I_NEW))
		sysfs_init_inode(sd, inode);

	return inode;
}

/*
 * The sysfs_dirent serves as both an inode and a directory entry for sysfs.
 * To prevent the sysfs inode numbers from being freed prematurely we take a
 * reference to sysfs_dirent from the sysfs inode.  A
 * super_operations.evict_inode() implementation is needed to drop that
 * reference upon inode destruction.
 */
/** 20150404    
 * sysfs의 evice_inode 콜백 함수.
 **/
void sysfs_evict_inode(struct inode *inode)
{
	/** 20150404    
	 * i_private에 저장해 뒀던 sysfs_direct를 받아와
	 **/
	struct sysfs_dirent *sd  = inode->i_private;

	/** 20150404    
	 * 추후 분석???
	 **/
	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	/** 20150404    
	 * 해당 sysfs_direct의 reference count를 감소시키고, 해제한다.
	 **/
	sysfs_put(sd);
}

/** 20150905    
 * sysfs에서 hash로 찾아 제거한다.
 **/
int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const void *ns, const char *name)
{
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent *sd;

	if (!dir_sd) {
		WARN(1, KERN_WARNING "sysfs: can not remove '%s', no directory\n",
			name);
		return -ENOENT;
	}

	/** 20150905    
	 * dir_sd 아래에서 name이 동일한 sysfs_dirent를 찾아 존재하면 제거한다.
	 * sysfs dirent 추가/제거 동작이므로 context를 보호한다.
	 **/
	sysfs_addrm_start(&acxt, dir_sd);

	sd = sysfs_find_dirent(dir_sd, ns, name);
	if (sd)
		sysfs_remove_one(&acxt, sd);

	sysfs_addrm_finish(&acxt);

	if (sd)
		return 0;
	else
		return -ENOENT;
}

int sysfs_permission(struct inode *inode, int mask)
{
	struct sysfs_dirent *sd;

	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;

	sd = inode->i_private;

	mutex_lock(&sysfs_mutex);
	sysfs_refresh_inode(sd, inode);
	mutex_unlock(&sysfs_mutex);

	return generic_permission(inode, mask);
}
