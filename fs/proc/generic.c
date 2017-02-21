/*
 * proc/fs/generic.c --- generic routines for the proc-fs
 *
 * This file contains generic proc-fs routines for handling
 * directories and files.
 * 
 * Copyright (C) 1991, 1992 Linus Torvalds.
 * Copyright (C) 1997 Theodore Ts'o
 */

#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/init.h>
#include <linux/idr.h>
#include <linux/namei.h>
#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <asm/uaccess.h>

#include "internal.h"

/** 20150509
 * proc subdir 조작을 위한 spinlock.
 **/
DEFINE_SPINLOCK(proc_subdir_lock);

/** 20150509
 * proc_dir_entry의 name과 길이, 문자열을 순서대로 비교해
 * 일치하면 0이 아닌 값을 리턴한다.
 **/
static int proc_match(unsigned int len, const char *name, struct proc_dir_entry *de)
{
	if (de->namelen != len)
		return 0;
	return !memcmp(name, de->name, len);
}

/* buffer size is one page but our output routines use some slack for overruns */
#define PROC_BLOCK_SIZE	(PAGE_SIZE - 1024)

static ssize_t
__proc_file_read(struct file *file, char __user *buf, size_t nbytes,
	       loff_t *ppos)
{
	struct inode * inode = file->f_path.dentry->d_inode;
	char 	*page;
	ssize_t	retval=0;
	int	eof=0;
	ssize_t	n, count;
	char	*start;
	struct proc_dir_entry * dp;
	unsigned long long pos;

	/*
	 * Gaah, please just use "seq_file" instead. The legacy /proc
	 * interfaces cut loff_t down to off_t for reads, and ignore
	 * the offset entirely for writes..
	 */
	pos = *ppos;
	if (pos > MAX_NON_LFS)
		return 0;
	if (nbytes > MAX_NON_LFS - pos)
		nbytes = MAX_NON_LFS - pos;

	dp = PDE(inode);
	if (!(page = (char*) __get_free_page(GFP_TEMPORARY)))
		return -ENOMEM;

	while ((nbytes > 0) && !eof) {
		count = min_t(size_t, PROC_BLOCK_SIZE, nbytes);

		start = NULL;
		if (dp->read_proc) {
			/*
			 * How to be a proc read function
			 * ------------------------------
			 * Prototype:
			 *    int f(char *buffer, char **start, off_t offset,
			 *          int count, int *peof, void *dat)
			 *
			 * Assume that the buffer is "count" bytes in size.
			 *
			 * If you know you have supplied all the data you
			 * have, set *peof.
			 *
			 * You have three ways to return data:
			 * 0) Leave *start = NULL.  (This is the default.)
			 *    Put the data of the requested offset at that
			 *    offset within the buffer.  Return the number (n)
			 *    of bytes there are from the beginning of the
			 *    buffer up to the last byte of data.  If the
			 *    number of supplied bytes (= n - offset) is 
			 *    greater than zero and you didn't signal eof
			 *    and the reader is prepared to take more data
			 *    you will be called again with the requested
			 *    offset advanced by the number of bytes 
			 *    absorbed.  This interface is useful for files
			 *    no larger than the buffer.
			 * 1) Set *start = an unsigned long value less than
			 *    the buffer address but greater than zero.
			 *    Put the data of the requested offset at the
			 *    beginning of the buffer.  Return the number of
			 *    bytes of data placed there.  If this number is
			 *    greater than zero and you didn't signal eof
			 *    and the reader is prepared to take more data
			 *    you will be called again with the requested
			 *    offset advanced by *start.  This interface is
			 *    useful when you have a large file consisting
			 *    of a series of blocks which you want to count
			 *    and return as wholes.
			 *    (Hack by Paul.Russell@rustcorp.com.au)
			 * 2) Set *start = an address within the buffer.
			 *    Put the data of the requested offset at *start.
			 *    Return the number of bytes of data placed there.
			 *    If this number is greater than zero and you
			 *    didn't signal eof and the reader is prepared to
			 *    take more data you will be called again with the
			 *    requested offset advanced by the number of bytes
			 *    absorbed.
			 */
			n = dp->read_proc(page, &start, *ppos,
					  count, &eof, dp->data);
		} else
			break;

		if (n == 0)   /* end of file */
			break;
		if (n < 0) {  /* error */
			if (retval == 0)
				retval = n;
			break;
		}

		if (start == NULL) {
			if (n > PAGE_SIZE) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE;
			}
			n -= *ppos;
			if (n <= 0)
				break;
			if (n > count)
				n = count;
			start = page + *ppos;
		} else if (start < page) {
			if (n > PAGE_SIZE) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE;
			}
			if (n > count) {
				/*
				 * Don't reduce n because doing so might
				 * cut off part of a data block.
				 */
				printk(KERN_WARNING
				       "proc_file_read: Read count exceeded\n");
			}
		} else /* start >= page */ {
			unsigned long startoff = (unsigned long)(start - page);
			if (n > (PAGE_SIZE - startoff)) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE - startoff;
			}
			if (n > count)
				n = count;
		}
		
 		n -= copy_to_user(buf, start < page ? page : start, n);
		if (n == 0) {
			if (retval == 0)
				retval = -EFAULT;
			break;
		}

		*ppos += start < page ? (unsigned long)start : n;
		nbytes -= n;
		buf += n;
		retval += n;
	}
	free_page((unsigned long) page);
	return retval;
}

static ssize_t
proc_file_read(struct file *file, char __user *buf, size_t nbytes,
	       loff_t *ppos)
{
	struct proc_dir_entry *pde = PDE(file->f_path.dentry->d_inode);
	ssize_t rv = -EIO;

	spin_lock(&pde->pde_unload_lock);
	if (!pde->proc_fops) {
		spin_unlock(&pde->pde_unload_lock);
		return rv;
	}
	pde->pde_users++;
	spin_unlock(&pde->pde_unload_lock);

	rv = __proc_file_read(file, buf, nbytes, ppos);

	pde_users_dec(pde);
	return rv;
}

static ssize_t
proc_file_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	struct proc_dir_entry *pde = PDE(file->f_path.dentry->d_inode);
	ssize_t rv = -EIO;

	if (pde->write_proc) {
		spin_lock(&pde->pde_unload_lock);
		if (!pde->proc_fops) {
			spin_unlock(&pde->pde_unload_lock);
			return rv;
		}
		pde->pde_users++;
		spin_unlock(&pde->pde_unload_lock);

		/* FIXME: does this routine need ppos?  probably... */
		rv = pde->write_proc(file, buffer, count, pde->data);
		pde_users_dec(pde);
	}
	return rv;
}


static loff_t
proc_file_lseek(struct file *file, loff_t offset, int orig)
{
	loff_t retval = -EINVAL;
	switch (orig) {
	case 1:
		offset += file->f_pos;
	/* fallthrough */
	case 0:
		if (offset < 0 || offset > MAX_NON_LFS)
			break;
		file->f_pos = retval = offset;
	}
	return retval;
}

/** 20150509
 * proc의 regular file에 대한 file ops.
 **/
static const struct file_operations proc_file_operations = {
	.llseek		= proc_file_lseek,
	.read		= proc_file_read,
	.write		= proc_file_write,
};

static int proc_notify_change(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	struct proc_dir_entry *de = PDE(inode);
	int error;

	error = inode_change_ok(inode, iattr);
	if (error)
		return error;

	if ((iattr->ia_valid & ATTR_SIZE) &&
	    iattr->ia_size != i_size_read(inode)) {
		error = vmtruncate(inode, iattr->ia_size);
		if (error)
			return error;
	}

	setattr_copy(inode, iattr);
	mark_inode_dirty(inode);
	
	de->uid = inode->i_uid;
	de->gid = inode->i_gid;
	de->mode = inode->i_mode;
	return 0;
}

static int proc_getattr(struct vfsmount *mnt, struct dentry *dentry,
			struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct proc_dir_entry *de = PROC_I(inode)->pde;
	if (de && de->nlink)
		set_nlink(inode, de->nlink);

	generic_fillattr(inode, stat);
	return 0;
}

/** 20150509
 * proc의 regular file에 대한 inode ops.
 **/
static const struct inode_operations proc_file_inode_operations = {
	.setattr	= proc_notify_change,
};

/*
 * This function parses a name such as "tty/driver/serial", and
 * returns the struct proc_dir_entry for "/proc/tty/driver", and
 * returns "serial" in residual.
 */
/** 20150509
 * name을 받아 proc entry를 찾아 리턴한다.
 *
 * "tty/driver/serial"를 예로 들면
 * /proc/tty/driver에 대한 proc_dir_entry와 "serial"을 매개변수로 전달된 위치에
 * 각각 저장한다.
 **/
static int __xlate_proc_name(const char *name, struct proc_dir_entry **ret,
			     const char **residual)
{
	const char     		*cp = name, *next;
	struct proc_dir_entry	*de;
	unsigned int		len;

	/** 20150509
	 * ret에 지정된 proc_dir_entry를 가져온다.
	 * 만약 NULL이 넘어왔다면 proc_root를 가져온다.
	 **/
	de = *ret;
	if (!de)
		de = &proc_root;

	/** 20150509
	 * '/'를 기준으로 파싱해서 '/'가 없을 때까지 반복한다.
	 **/
	while (1) {
		next = strchr(cp, '/');
		if (!next)
			break;

		len = next - cp;
		/** 20150509
		 * de의 subdir들을 순회하며 len 길이에 일치하는 문자열을 찾는다.
		 **/
		for (de = de->subdir; de ; de = de->next) {
			if (proc_match(len, cp, de))
				break;
		}
		if (!de) {
			WARN(1, "name '%s'\n", name);
			return -ENOENT;
		}
		/** 20150509
		 * 다음 반복문을 위해 '/' 이후로 이동한다.
		 **/
		cp += len + 1;
	}
	/** 20150509
	 * '/' 이후 부분을 *residual에 저장한다.
	 * 마지막 찾은 proc_dir_entry를 *ret에 저장한다.
	 **/
	*residual = cp;
	*ret = de;
	return 0;
}

/** 20150509
 * name을 받아 마지막 dir proc entry와 나머지 문자열로 변환해 리턴한다.
 **/
static int xlate_proc_name(const char *name, struct proc_dir_entry **ret,
			   const char **residual)
{
	int rv;

	/** 20150509
	 * proc_dir_entry의 subdir 조작은 spinlock으로 보호된다.
	 **/
	spin_lock(&proc_subdir_lock);
	rv = __xlate_proc_name(name, ret, residual);
	spin_unlock(&proc_subdir_lock);
	return rv;
}

/** 20150509
 * proc용 inode number 할당을 위한 IDA를 정의한다.
 * 해당 ida는 proc_inum_lock으로 보호한다.
 **/
static DEFINE_IDA(proc_inum_ida);
static DEFINE_SPINLOCK(proc_inum_lock); /* protects the above */

/** 20150509
 * proc에서 동적으로 할당해줄 inode number의 시작값.
 **/
#define PROC_DYNAMIC_FIRST 0xF0000000U

/*
 * Return an inode number between PROC_DYNAMIC_FIRST and
 * 0xffffffff, or zero on failure.
 */
/** 20150509
 * ida를 통해 inode number를 할당 받는다.
 * 동적할당된 값은 PROC_DYNAMIC_FIRST + 1 ~ UINT_MAX 사이값이다.
 **/
static unsigned int get_inode_number(void)
{
	unsigned int i;
	int error;

retry:
	/** 20150509
	 * proc_inum_ida 용으로 자원을 할당 받는다.
	 **/
	if (ida_pre_get(&proc_inum_ida, GFP_KERNEL) == 0)
		return 0;

	spin_lock(&proc_inum_lock);
	/** 20150509
	 * ida로부터 id를 할당 받아 i에 저장한다.
	 * proc_inum_ida는 spinlock으로 보호된다.
	 **/
	error = ida_get_new(&proc_inum_ida, &i);
	spin_unlock(&proc_inum_lock);
	if (error == -EAGAIN)
		goto retry;
	else if (error)
		return 0;

	/** 20150509
	 * 받아온 id가 proc의 동적 할당용 id보다 크다면 
	 * 해당 id를 반환하고 0을 리턴한다.
	 **/
	if (i > UINT_MAX - PROC_DYNAMIC_FIRST) {
		spin_lock(&proc_inum_lock);
		ida_remove(&proc_inum_ida, i);
		spin_unlock(&proc_inum_lock);
		return 0;
	}
	/** 20150509
	 * 받아온 id에 동적 할당의 시작값을 더해 리턴한다.
	 **/
	return PROC_DYNAMIC_FIRST + i;
}

/** 20150509
 * proc용 inode number를 해제한다.
 **/
static void release_inode_number(unsigned int inum)
{
	/** 20150509
	 * proc용 inode number에서 해당 inum을 제거한다.
	 **/
	spin_lock(&proc_inum_lock);
	ida_remove(&proc_inum_ida, inum - PROC_DYNAMIC_FIRST);
	spin_unlock(&proc_inum_lock);
}

static void *proc_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	nd_set_link(nd, PDE(dentry->d_inode)->data);
	return NULL;
}

/** 20150509
 * proc의 심볼릭 링크에 대한 inode ops.
 **/
static const struct inode_operations proc_link_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= proc_follow_link,
};

/*
 * As some entries in /proc are volatile, we want to 
 * get rid of unused dentries.  This could be made 
 * smarter: we could keep a "volatile" flag in the 
 * inode to indicate which ones to keep.
 */
static int proc_delete_dentry(const struct dentry * dentry)
{
	return 1;
}

static const struct dentry_operations proc_dentry_operations =
{
	.d_delete	= proc_delete_dentry,
};

/*
 * Don't create negative dentries here, return -ENOENT by hand
 * instead.
 */
struct dentry *proc_lookup_de(struct proc_dir_entry *de, struct inode *dir,
		struct dentry *dentry)
{
	struct inode *inode = NULL;
	int error = -ENOENT;

	spin_lock(&proc_subdir_lock);
	for (de = de->subdir; de ; de = de->next) {
		if (de->namelen != dentry->d_name.len)
			continue;
		if (!memcmp(dentry->d_name.name, de->name, de->namelen)) {
			pde_get(de);
			spin_unlock(&proc_subdir_lock);
			error = -EINVAL;
			inode = proc_get_inode(dir->i_sb, de);
			goto out_unlock;
		}
	}
	spin_unlock(&proc_subdir_lock);
out_unlock:

	if (inode) {
		d_set_d_op(dentry, &proc_dentry_operations);
		d_add(dentry, inode);
		return NULL;
	}
	if (de)
		pde_put(de);
	return ERR_PTR(error);
}

struct dentry *proc_lookup(struct inode *dir, struct dentry *dentry,
		unsigned int flags)
{
	return proc_lookup_de(PDE(dir), dir, dentry);
}

/*
 * This returns non-zero if at EOF, so that the /proc
 * root directory can use this and check if it should
 * continue with the <pid> entries..
 *
 * Note that the VFS-layer doesn't care about the return
 * value of the readdir() call, as long as it's non-negative
 * for success..
 */
int proc_readdir_de(struct proc_dir_entry *de, struct file *filp, void *dirent,
		filldir_t filldir)
{
	unsigned int ino;
	int i;
	struct inode *inode = filp->f_path.dentry->d_inode;
	int ret = 0;

	ino = inode->i_ino;
	i = filp->f_pos;
	switch (i) {
		case 0:
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
				goto out;
			i++;
			filp->f_pos++;
			/* fall through */
		case 1:
			if (filldir(dirent, "..", 2, i,
				    parent_ino(filp->f_path.dentry),
				    DT_DIR) < 0)
				goto out;
			i++;
			filp->f_pos++;
			/* fall through */
		default:
			spin_lock(&proc_subdir_lock);
			de = de->subdir;
			i -= 2;
			for (;;) {
				if (!de) {
					ret = 1;
					spin_unlock(&proc_subdir_lock);
					goto out;
				}
				if (!i)
					break;
				de = de->next;
				i--;
			}

			do {
				struct proc_dir_entry *next;

				/* filldir passes info to user space */
				pde_get(de);
				spin_unlock(&proc_subdir_lock);
				if (filldir(dirent, de->name, de->namelen, filp->f_pos,
					    de->low_ino, de->mode >> 12) < 0) {
					pde_put(de);
					goto out;
				}
				spin_lock(&proc_subdir_lock);
				filp->f_pos++;
				next = de->next;
				pde_put(de);
				de = next;
			} while (de);
			spin_unlock(&proc_subdir_lock);
	}
	ret = 1;
out:
	return ret;	
}

int proc_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_path.dentry->d_inode;

	return proc_readdir_de(PDE(inode), filp, dirent, filldir);
}

/*
 * These are the generic /proc directory operations. They
 * use the in-memory "struct proc_dir_entry" tree to parse
 * the /proc directory.
 */
static const struct file_operations proc_dir_operations = {
	.llseek			= generic_file_llseek,
	.read			= generic_read_dir,
	.readdir		= proc_readdir,
};

/*
 * proc directories can do almost nothing..
 */
/** 20150509
 * proc의 dir에 대한 inode ops.
 **/
static const struct inode_operations proc_dir_inode_operations = {
	.lookup		= proc_lookup,
	.getattr	= proc_getattr,
	.setattr	= proc_notify_change,
};

/** 20150509
 * 새로운 proc_dir_entry를 dir에 추가한다.
 **/
static int proc_register(struct proc_dir_entry * dir, struct proc_dir_entry * dp)
{
	unsigned int i;
	struct proc_dir_entry *tmp;
	
	/** 20150509
	 * inode 번호를 하나 받아 proc_dir_entry에 저장한다.
	 **/
	i = get_inode_number();
	if (i == 0)
		return -EAGAIN;
	dp->low_ino = i;

	/** 20150509
	 * mode를 검사해 파일 타입에 따라 proc_iops 비어 있을 경우
	 * proc_fops와 proc_iops를 지정한다.
	 * 타입은 디렉토리, 심볼릭링크, 파일이 존재한다.
	 **/
	if (S_ISDIR(dp->mode)) {
		if (dp->proc_iops == NULL) {
			dp->proc_fops = &proc_dir_operations;
			dp->proc_iops = &proc_dir_inode_operations;
		}
		dir->nlink++;
	} else if (S_ISLNK(dp->mode)) {
		if (dp->proc_iops == NULL)
			dp->proc_iops = &proc_link_inode_operations;
	} else if (S_ISREG(dp->mode)) {
		if (dp->proc_fops == NULL)
			dp->proc_fops = &proc_file_operations;
		if (dp->proc_iops == NULL)
			dp->proc_iops = &proc_file_inode_operations;
	}

	/** 20150509
	 * proc_dir_entry의 subdir은 spinlock으로 보호된다.
	 **/
	spin_lock(&proc_subdir_lock);

	/** 20150509
	 * 전달받은 상위 dir의 subdir을 순회하며
	 **/
	for (tmp = dir->subdir; tmp; tmp = tmp->next)
		/** 20150509
		 * 이미 존재하는 subdir 중 추가할 entry와 일치하는 이름이 있다면
		 * 경고를 출력한다.
		 **/
		if (strcmp(tmp->name, dp->name) == 0) {
			WARN(1, KERN_WARNING "proc_dir_entry '%s/%s' already registered\n",
				dir->name, dp->name);
			break;
		}

	/** 20150509
	 * 새로운 entry를  현재 dir->subdir의 처음에 추가한다.
	 **/
	dp->next = dir->subdir;
	dp->parent = dir;
	dir->subdir = dp;
	spin_unlock(&proc_subdir_lock);

	return 0;
}

/** 20150509
 * name을 파싱해 parent에 proc_dir_entry를 찾아 저장하고,
 * 나머지 문자열을 위한 proc_dir_entry로 생성해 리턴한다.
 *
 * mode와 nlink를 직접 지정한다.
 **/
static struct proc_dir_entry *__proc_create(struct proc_dir_entry **parent,
					  const char *name,
					  umode_t mode,
					  nlink_t nlink)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	unsigned int len;

	/* make sure name is valid */
	if (!name || !strlen(name)) goto out;

	/** 20150509
	 * name을 받아 proc_dir_entry와 나머지 문자열로 변환해 받아온다.
	 *
	 * parent에 호출시에는 parent entry가 넘어가고,
	 * 변환 후에는 찾은 parent의 proc_dir_entry가 저장된다.
	 **/
	if (xlate_proc_name(name, parent, &fn) != 0)
		goto out;

	/** 20150509
	 * 변환 결과 fn는 마지막 '/' 이후의 문자열이 저장된다.
	 **/
	/* At this point there must not be any '/' characters beyond *fn */
	if (strchr(fn, '/'))
		goto out;

	/** 20150509
	 * 남은 문자열 길이를 구한다.
	 **/
	len = strlen(fn);

	/** 20150509
	 * proc_dir_entry와 나머지 문자열을 저장할 길이를 할당 받는다.
	 **/
	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent) goto out;

	memset(ent, 0, sizeof(struct proc_dir_entry));
	/** 20150509
	 * 나머지 문자열을 ent->name에 복사하고 길이를 저장한다.
	 **/
	memcpy(ent->name, fn, len + 1);
	ent->namelen = len;
	/** 20150509
	 * proc_dir_entry에 매개변수로 넘어온 값을 저장하고,
	 * 나머지 멤버를 초기값을 설정한다.
	 **/
	ent->mode = mode;
	ent->nlink = nlink;
	atomic_set(&ent->count, 1);
	ent->pde_users = 0;
	spin_lock_init(&ent->pde_unload_lock);
	ent->pde_unload_completion = NULL;
	INIT_LIST_HEAD(&ent->pde_openers);
 out:
	return ent;
}

/** 20150509
 * parent 아래 dest를 대상 파일로 하는 심볼릭 링크를 생성한다.
 **/
struct proc_dir_entry *proc_symlink(const char *name,
		struct proc_dir_entry *parent, const char *dest)
{
	struct proc_dir_entry *ent;

	/** 20150509
	 * name을 파싱해 parent에 마지막 '/' 전에 해당하는 proc_dir_entry를
	 * 저장하고, 나머지 문자열로 새로운 proc_dir_entry를 생성해 한다.
	 * 생성할 entry의 mode는 symbolic link이고 hard link는 1개이다.
	 **/
	ent = __proc_create(&parent, name,
			  (S_IFLNK | S_IRUGO | S_IWUGO | S_IXUGO),1);

	if (ent) {
		/** 20150509
		 * 새로 생성한 entry의 data에 symlink의 대상 파일명을 저장한다.
		 **/
		ent->data = kmalloc((ent->size=strlen(dest))+1, GFP_KERNEL);
		if (ent->data) {
			strcpy((char*)ent->data,dest);
			/** 20150509
			 * 생성한 entry를 parent 의 subdir로 추가한다.
			 **/
			if (proc_register(parent, ent) < 0) {
				kfree(ent->data);
				kfree(ent);
				ent = NULL;
			}
		} else {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}
EXPORT_SYMBOL(proc_symlink);

/** 20150509
 * parent 아래에 디렉토리로 새로운 proc_dir_entry를 생성하고,
 * 생성한 entry를 parent 아래에 등록한다.
 **/
struct proc_dir_entry *proc_mkdir_mode(const char *name, umode_t mode,
		struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent;

	ent = __proc_create(&parent, name, S_IFDIR | mode, 2);
	if (ent) {
		if (proc_register(parent, ent) < 0) {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}
EXPORT_SYMBOL(proc_mkdir_mode);

struct proc_dir_entry *proc_net_mkdir(struct net *net, const char *name,
		struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent;

	ent = __proc_create(&parent, name, S_IFDIR | S_IRUGO | S_IXUGO, 2);
	if (ent) {
		ent->data = net;
		if (proc_register(parent, ent) < 0) {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}
EXPORT_SYMBOL_GPL(proc_net_mkdir);

/** 20150510
 * 디렉토리 타입의 새로운 proc entry를 생성해 parent 아래에 등록한다.
 **/
struct proc_dir_entry *proc_mkdir(const char *name,
		struct proc_dir_entry *parent)
{
	return proc_mkdir_mode(name, S_IRUGO | S_IXUGO, parent);
}
EXPORT_SYMBOL(proc_mkdir);

struct proc_dir_entry *create_proc_entry(const char *name, umode_t mode,
					 struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent;
	nlink_t nlink;

	if (S_ISDIR(mode)) {
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO | S_IXUGO;
		nlink = 2;
	} else {
		if ((mode & S_IFMT) == 0)
			mode |= S_IFREG;
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO;
		nlink = 1;
	}

	ent = __proc_create(&parent, name, mode, nlink);
	if (ent) {
		if (proc_register(parent, ent) < 0) {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}
EXPORT_SYMBOL(create_proc_entry);

/** 20150510
 * proc entry를 새로 생성하고 주어진 argument로 설정하고 parent에 추가한다.
 **/
struct proc_dir_entry *proc_create_data(const char *name, umode_t mode,
					struct proc_dir_entry *parent,
					const struct file_operations *proc_fops,
					void *data)
{
	struct proc_dir_entry *pde;
	nlink_t nlink;

	/** 20150510
	 * 생성할 entry의 기본 모드를 설정한다.
	 **/
	if (S_ISDIR(mode)) {
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO | S_IXUGO;
		nlink = 2;
	} else {
		if ((mode & S_IFMT) == 0)
			mode |= S_IFREG;
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO;
		nlink = 1;
	}

	/** 20150510
	 * proc 엔트리를 생성하고, 매개변수로 넘어온 proc_fops와 data를 저장한다.
	 **/
	pde = __proc_create(&parent, name, mode, nlink);
	if (!pde)
		goto out;
	pde->proc_fops = proc_fops;
	pde->data = data;
	/** 20150510
	 * parent 아래 생성한 proc 엔트리를 추가한다.
	 **/
	if (proc_register(parent, pde) < 0)
		goto out_free;
	return pde;
out_free:
	kfree(pde);
out:
	return NULL;
}
EXPORT_SYMBOL(proc_create_data);

/** 20150509
 * 전달받은 proc_dir_entry를 해제한다.
 **/
static void free_proc_entry(struct proc_dir_entry *de)
{
	/** 20150509
	 * proc용 inode number를 해제한다.
	 **/
	release_inode_number(de->low_ino);

	/** 20150509
	 * 해당 entry가 심볼릭 링크라면 entry의 data까지 같이 해제한다.
	 *
	 * entry를 해제한다.
	 **/
	if (S_ISLNK(de->mode))
		kfree(de->data);
	kfree(de);
}

/** 20150509
 * proc_dir_entry를 받아 usage count를 감소시키고,
 * 결과 0이 되었다면 entry를 제거한다.
 **/
void pde_put(struct proc_dir_entry *pde)
{
	if (atomic_dec_and_test(&pde->count))
		free_proc_entry(pde);
}

/*
 * Remove a /proc entry and free it if it's not currently in use.
 */
void remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
	struct proc_dir_entry **p;
	struct proc_dir_entry *de = NULL;
	const char *fn = name;
	unsigned int len;

	spin_lock(&proc_subdir_lock);
	if (__xlate_proc_name(name, &parent, &fn) != 0) {
		spin_unlock(&proc_subdir_lock);
		return;
	}
	len = strlen(fn);

	for (p = &parent->subdir; *p; p=&(*p)->next ) {
		if (proc_match(len, fn, *p)) {
			de = *p;
			*p = de->next;
			de->next = NULL;
			break;
		}
	}
	spin_unlock(&proc_subdir_lock);
	if (!de) {
		WARN(1, "name '%s'\n", name);
		return;
	}

	spin_lock(&de->pde_unload_lock);
	/*
	 * Stop accepting new callers into module. If you're
	 * dynamically allocating ->proc_fops, save a pointer somewhere.
	 */
	de->proc_fops = NULL;
	/* Wait until all existing callers into module are done. */
	if (de->pde_users > 0) {
		DECLARE_COMPLETION_ONSTACK(c);

		if (!de->pde_unload_completion)
			de->pde_unload_completion = &c;

		spin_unlock(&de->pde_unload_lock);

		wait_for_completion(de->pde_unload_completion);

		spin_lock(&de->pde_unload_lock);
	}

	while (!list_empty(&de->pde_openers)) {
		struct pde_opener *pdeo;

		pdeo = list_first_entry(&de->pde_openers, struct pde_opener, lh);
		list_del(&pdeo->lh);
		spin_unlock(&de->pde_unload_lock);
		pdeo->release(pdeo->inode, pdeo->file);
		kfree(pdeo);
		spin_lock(&de->pde_unload_lock);
	}
	spin_unlock(&de->pde_unload_lock);

	if (S_ISDIR(de->mode))
		parent->nlink--;
	de->nlink = 0;
	WARN(de->subdir, KERN_WARNING "%s: removing non-empty directory "
			"'%s/%s', leaking at least '%s'\n", __func__,
			de->parent->name, de->name, de->subdir->name);
	pde_put(de);
}
EXPORT_SYMBOL(remove_proc_entry);
