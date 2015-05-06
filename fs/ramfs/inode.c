/*
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "internal.h"

/** 20150425    
 * RAMFS의 기본 옵션 user rwx, group rx, other rx
 **/
#define RAMFS_DEFAULT_MODE	0755

static const struct super_operations ramfs_ops;
static const struct inode_operations ramfs_dir_inode_operations;

/** 20150418    
 * ramfs을 위한 backing device info.
 **/
static struct backing_dev_info ramfs_backing_dev_info = {
	.name		= "ramfs",
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK |
			  BDI_CAP_MAP_DIRECT | BDI_CAP_MAP_COPY |
			  BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP,
};

/** 20150425    
 * ramfs의 inode 할당용 콜백함수.
 *
 * superblock에 대해 inode를 하나 할당받고, 속성과 콜백함수 등을 설정한다.
 **/
struct inode *ramfs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	/** 20150425    
	 * superblock에 대한 새로운 inode를 받아온다.
	 **/
	struct inode * inode = new_inode(sb);

	if (inode) {
		/** 20150425    
		 * - 시스템 전체에서 unique한 i_ino를 받아온다.
		 * - uid, gid, mode를 설정한다.
		 * - ramfs를 위한 address space ops와 bdi를 지정한다.
		 **/
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_mapping->a_ops = &ramfs_aops;
		inode->i_mapping->backing_dev_info = &ramfs_backing_dev_info;
		/** 20150425    
		 * inode의 address_space의 flags를
		 *   GFP_HIGHUSER로 설정한다.
		 *   unevictable로 설정한다.
		 **/
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		/** 20150425    
		 * inode의 접근/수정/변경 시간을 현재 시간으로 설정한다.
		 **/
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		/** 20150425    
		 * mode에 따라 사용할 inode ops, file ops를 지정한다.
		 **/
		switch (mode & S_IFMT) {
		/** 20150425    
		 * 그 외 특별한 파일인 경우(디바이스, 소켓, FIFO 등)
		 **/
		default:
			init_special_inode(inode, mode, dev);
			break;
		/** 20150425    
		 * 일반 파일인 경우
		 **/
		case S_IFREG:
			inode->i_op = &ramfs_file_inode_operations;
			inode->i_fop = &ramfs_file_operations;
			break;
		/** 20150425    
		 * 디렉토리인 경우
		 **/
		case S_IFDIR:
			inode->i_op = &ramfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		/** 20150425    
		 * 심볼릭 링크인 경우
		 **/
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
ramfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = ramfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	}
	return error;
}

static int ramfs_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = ramfs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return ramfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int ramfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = ramfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		} else
			iput(inode);
	}
	return error;
}

static const struct inode_operations ramfs_dir_inode_operations = {
	.create		= ramfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= ramfs_symlink,
	.mkdir		= ramfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= ramfs_mknod,
	.rename		= simple_rename,
};

/** 20150425    
 * ramfs에 정의된 superblock ops.
 **/
static const struct super_operations ramfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
};

/** 20150418    
 **/
struct ramfs_mount_opts {
	umode_t mode;
};

enum {
	Opt_mode,
	Opt_err
};

/** 20150418    
 * match table 'tokens'
 **/
static const match_table_t tokens = {
	{Opt_mode, "mode=%o"},
	{Opt_err, NULL}
};

/** 20150418    
 **/
struct ramfs_fs_info {
	struct ramfs_mount_opts mount_opts;
};

/** 20150425    
 * ramfs의 mount option 처리 함수.
 **/
static int ramfs_parse_options(char *data, struct ramfs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

	opts->mode = RAMFS_DEFAULT_MODE;

	/** 20150425    
	 * data에 저장된 옵션을 ","를 기준으로 파싱해 토큰으로 처리한다.
	 *
	 * ramfs는 mode 옵션만 처리한다.
	 **/
	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		/** 20150418    
		 * match table에서 p에 해당하는 token들을 찾아 token을 리턴하고,
		 * argument를 파싱해 args에 채워온다.
		 **/
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_mode:
			/** 20150418    
			 * args[0]에 해당하는 값을 octal로 option에 받아온다.
			 **/
			if (match_octal(&args[0], &option))
				return -EINVAL;
			/** 20150418    
			 * option 중에서 S_IALLUGO에 해당하는 부분만 추출해 mode에 채운다.
			 **/
			opts->mode = option & S_IALLUGO;
			break;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally ramfs has ignored all mount options,
		 * and as it is used as a !CONFIG_SHMEM simple substitute
		 * for tmpfs, better continue to ignore other mount options.
		 */
		}
	}

	return 0;
}

/** 20150418    
 * ramfs, rootfs 용 fill_super함수.
 * mount_node에서 file_system_type에 특징적인 콜백함수로 지정된다.
 *
 **/
int ramfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct ramfs_fs_info *fsi;
	struct inode *inode;
	int err;

	/** 20150418    
	 * superblock에 data 옵션을 저장한다.
	 **/
	save_mount_options(sb, data);

	/** 20150418    
	 * struct ramfs_fs_info를 할당 받아 fs private으로 저장한다.
	 **/
	fsi = kzalloc(sizeof(struct ramfs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi)
		return -ENOMEM;

	/** 20150425    
	 * data에서 옵션을 추출해 mount_opts에 저장한다.
	 **/
	err = ramfs_parse_options(data, &fsi->mount_opts);
	if (err)
		return err;

	/** 20150425    
	 * superblock의 크기, magic 정보 등을 채운다.
	 * 메모리상에 존재하는 파일시스템이므로 블록단위는 PAGE_CACHE_SIZE이다.
	 **/
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_CACHE_SIZE;
	sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	sb->s_magic		= RAMFS_MAGIC;
	/** 20150425    
	 * superblock의 operations를 ramfs_ops로 지정
	 **/
	sb->s_op		= &ramfs_ops;
	sb->s_time_gran		= 1;

	inode = ramfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

/** 20150502    
 **/
struct dentry *ramfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, ramfs_fill_super);
}

static struct dentry *rootfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	/** 20150425    
	 * rootfs를 nodev용 mount 함수로 mount 한다.
	 * MS_NOUSER 플래그가 추가되어 userspace에서 mount 될 수 없다.
	 **/
	return mount_nodev(fs_type, flags|MS_NOUSER, data, ramfs_fill_super);
}

static void ramfs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type ramfs_fs_type = {
	.name		= "ramfs",
	.mount		= ramfs_mount,
	.kill_sb	= ramfs_kill_sb,
};
/** 20150418    
 * rootfs file system type 정의.
 **/
static struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.mount		= rootfs_mount,
	.kill_sb	= kill_litter_super,
};

static int __init init_ramfs_fs(void)
{
	return register_filesystem(&ramfs_fs_type);
}
module_init(init_ramfs_fs)

/** 20150425    
 * rootfs 파일시스템을 등록한다.
 **/
int __init init_rootfs(void)
{
	int err;

	/** 20150418    
	 * ramfs_backing_dev_info의 속성을 초기화 한다.
	 **/
	err = bdi_init(&ramfs_backing_dev_info);
	if (err)
		return err;

	/** 20150418    
	 * rootfs_fs_type을 새로운 filesystem으로 등록한다.
	 *
	 * rootfs의 mount는 init_mount_tree에서 이뤄진다.
	 **/
	err = register_filesystem(&rootfs_fs_type);
	if (err)
		bdi_destroy(&ramfs_backing_dev_info);

	return err;
}
