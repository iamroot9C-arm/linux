/*
 *  fs/anon_inodes.c
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *
 *  Thanks to Arnd Bergmann for code review and suggestions.
 *  More changes for Thomas Gleixner suggestions.
 *
 */

#include <linux/cred.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/anon_inodes.h>

#include <asm/uaccess.h>

/** 20151219    
 * anon_inode_init에서 한 번 mount 시킨다.
 **/
static struct vfsmount *anon_inode_mnt __read_mostly;
static struct inode *anon_inode_inode;
static const struct file_operations anon_inode_fops;

/*
 * anon_inodefs_dname() is called from d_path().
 */
/** 20151219    
 * anon_inode_getfile 호출지 지정한 이름을 출력한다.
 **/
static char *anon_inodefs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	return dynamic_dname(dentry, buffer, buflen, "anon_inode:%s",
				dentry->d_name.name);
}

/** 20151219    
 * anon inode dentry name을 출력하는 ops만 지원한다.
 **/
static const struct dentry_operations anon_inodefs_dentry_operations = {
	.d_dname	= anon_inodefs_dname,
};

/*
 * nop .set_page_dirty method so that people can use .page_mkwrite on
 * anon inodes.
 */
static int anon_set_page_dirty(struct page *page)
{
	return 0;
};

static const struct address_space_operations anon_aops = {
	.set_page_dirty = anon_set_page_dirty,
};

/*
 * A single inode exists for all anon_inode files. Contrary to pipes,
 * anon_inode inodes have no associated per-instance data, so we need
 * only allocate one of them.
 */
/** 20151219    
 * anon_inodefs의 inode 생성.
 **/
static struct inode *anon_inode_mkinode(struct super_block *s)
{
	struct inode *inode = new_inode_pseudo(s);

	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino = get_next_ino();
	inode->i_fop = &anon_inode_fops;

	inode->i_mapping->a_ops = &anon_aops;

	/*
	 * Mark the inode dirty from the very beginning,
	 * that way it will never be moved to the dirty
	 * list because mark_inode_dirty() will think
	 * that it already _is_ on the dirty list.
	 */
	inode->i_state = I_DIRTY;
	inode->i_mode = S_IRUSR | S_IWUSR;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_flags |= S_PRIVATE;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	return inode;
}

static struct dentry *anon_inodefs_mount(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data)
{
	struct dentry *root;
	/** 20151219    
	 * pseudo 파일시스템을 mount한다.
	 **/
	root = mount_pseudo(fs_type, "anon_inode:", NULL,
			&anon_inodefs_dentry_operations, ANON_INODE_FS_MAGIC);
	if (!IS_ERR(root)) {
		/** 20151219    
		 * superblock에 대한 inode를 생성한다.
		 **/
		struct super_block *s = root->d_sb;
		anon_inode_inode = anon_inode_mkinode(s);
		if (IS_ERR(anon_inode_inode)) {
			dput(root);
			deactivate_locked_super(s);
			root = ERR_CAST(anon_inode_inode);
		}
	}
	return root;
}

/** 20151219    
 * anonymous inode fs시스템.
 *
 * 연결된 directory entry 없이 inode에 접근할 수 있다.
 * /proc/filesystems에서 확인 가능
 **/
static struct file_system_type anon_inode_fs_type = {
	.name		= "anon_inodefs",
	.mount		= anon_inodefs_mount,
	.kill_sb	= kill_anon_super,
};

/**
 * anon_inode_getfile - creates a new file instance by hooking it up to an
 *                      anonymous inode, and a dentry that describe the "class"
 *                      of the file
 *
 * @name:    [in]    name of the "class" of the new file
 * @fops:    [in]    file operations for the new file
 * @priv:    [in]    private data for the new file (will be file's private_data)
 * @flags:   [in]    flags
 *
 * Creates a new file by hooking it on a single inode. This is useful for files
 * that do not need to have a full-fledged inode in order to operate correctly.
 * All the files created with anon_inode_getfile() will share a single inode,
 * hence saving memory and avoiding code duplication for the file/inode/dentry
 * setup.  Returns the newly created file* or an error pointer.
 */
/** 20151219    
 * anon_inodefs로부터 anonymous inode 에 대한 file 인스턴스를 생성한다.
 * 전달된 정보를 생성한 file 인스턴스에 채워 리턴한다.
 **/
struct file *anon_inode_getfile(const char *name,
				const struct file_operations *fops,
				void *priv, int flags)
{
	struct qstr this;
	struct path path;
	struct file *file;
	int error;

	if (IS_ERR(anon_inode_inode))
		return ERR_PTR(-ENODEV);

	if (fops->owner && !try_module_get(fops->owner))
		return ERR_PTR(-ENOENT);

	/*
	 * Link the inode to a directory entry by creating a unique name
	 * using the inode sequence number.
	 */
	error = -ENOMEM;
	this.name = name;
	this.len = strlen(name);
	this.hash = 0;
	/** 20151219    
	 * dentry를 생성해 path에 저장한다.
	 **/
	path.dentry = d_alloc_pseudo(anon_inode_mnt->mnt_sb, &this);
	if (!path.dentry)
		goto err_module;

	/** 20151219    
	 * anon_inode_mnt의 mount count를 증가시키고, 해당 vfsmount를 pat에 저장한다.
	 **/
	path.mnt = mntget(anon_inode_mnt);
	/*
	 * We know the anon_inode inode count is always greater than zero,
	 * so ihold() is safe.
	 */
	ihold(anon_inode_inode);

	/** 20151219    
	 * 새로 생성한 dentry에 anon_inode_inode의 정보를 채운다.
	 **/
	d_instantiate(path.dentry, anon_inode_inode);

	error = -ENFILE;
	/** 20151219    
	 * file 구조체를 할당 받고 전달받은 open mode와 fops로 설정한다.
	 **/
	file = alloc_file(&path, OPEN_FMODE(flags), fops);
	if (!file)
		goto err_dput;
	/** 20151219    
	 * inode에 저장된 address_space ops를 복사한다.
	 **/
	file->f_mapping = anon_inode_inode->i_mapping;

	file->f_pos = 0;
	file->f_flags = flags & (O_ACCMODE | O_NONBLOCK);
	file->f_version = 0;
	file->private_data = priv;

	return file;

err_dput:
	path_put(&path);
err_module:
	module_put(fops->owner);
	return ERR_PTR(error);
}
EXPORT_SYMBOL_GPL(anon_inode_getfile);

/**
 * anon_inode_getfd - creates a new file instance by hooking it up to an
 *                    anonymous inode, and a dentry that describe the "class"
 *                    of the file
 *
 * @name:    [in]    name of the "class" of the new file
 * @fops:    [in]    file operations for the new file
 * @priv:    [in]    private data for the new file (will be file's private_data)
 * @flags:   [in]    flags
 *
 * Creates a new file by hooking it on a single inode. This is useful for files
 * that do not need to have a full-fledged inode in order to operate correctly.
 * All the files created with anon_inode_getfd() will share a single inode,
 * hence saving memory and avoiding code duplication for the file/inode/dentry
 * setup.  Returns new descriptor or an error code.
 */
int anon_inode_getfd(const char *name, const struct file_operations *fops,
		     void *priv, int flags)
{
	int error, fd;
	struct file *file;

	error = get_unused_fd_flags(flags);
	if (error < 0)
		return error;
	fd = error;

	file = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto err_put_unused_fd;
	}
	fd_install(fd, file);

	return fd;

err_put_unused_fd:
	put_unused_fd(fd);
	return error;
}
EXPORT_SYMBOL_GPL(anon_inode_getfd);

/** 20151219    
 * anon inodefs를 등록하고 마운트한다.
 *
 * eventfs, eventpoll 등에서 anon_inode_getfd를 직접 호출해 사용한다.
 **/
static int __init anon_inode_init(void)
{
	int error;

	/** 20151219    
	 * anon_inodefs를 등록한다.
	 **/
	error = register_filesystem(&anon_inode_fs_type);
	if (error)
		goto err_exit;
	/** 20151219    
	 * anon_inodefs를 마운트한다.
	 **/
	anon_inode_mnt = kern_mount(&anon_inode_fs_type);
	if (IS_ERR(anon_inode_mnt)) {
		error = PTR_ERR(anon_inode_mnt);
		goto err_unregister_filesystem;
	}
	return 0;

err_unregister_filesystem:
	unregister_filesystem(&anon_inode_fs_type);
err_exit:
	panic(KERN_ERR "anon_inode_init() failed (%d)\n", error);
}

fs_initcall(anon_inode_init);

