/*
 *  linux/fs/filesystems.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  table of configured filesystems
 */

#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

/*
 * Handling of filesystem drivers list.
 * Rules:
 *	Inclusion to/removals from/scanning of list are protected by spinlock.
 *	During the unload module must call unregister_filesystem().
 *	We can access the fields of list element if:
 *		1) spinlock is held or
 *		2) we hold the reference to the module.
 *	The latter can be guaranteed by call of try_module_get(); if it
 *	returned 0 we must skip the element, otherwise we got the reference.
 *	Once the reference is obtained we can drop the spinlock.
 */

/** 20150221
 * 시스템 전체 file_system이 single list 형태로 구성되어 있다.
 * file_systems을 변경시킬 때 rwlock을 사용한다.
 **/
static struct file_system_type *file_systems;
/** 20150221
 * file systems 전역 RW lock.
 **/
static DEFINE_RWLOCK(file_systems_lock);

/* WARNING: This can be used only if we _already_ own a reference */
/** 20150307
 * filesystem의 owner module을 사용 중으로 표시한다.
 **/
void get_filesystem(struct file_system_type *fs)
{
	__module_get(fs->owner);
}

/** 20150307
 * filesystem의 owner module를 사용 안함으로 표시한다.
 **/
void put_filesystem(struct file_system_type *fs)
{
	module_put(fs->owner);
}

/** 20150221
 * file_systems에 name이라는 file_system_type을 찾는다.
 * 존재하면 해당 위치에서 리턴. 존재하지 않으면 리스트의 마지막까지 이동해 리턴.
 **/
static struct file_system_type **find_filesystem(const char *name, unsigned len)
{
	struct file_system_type **p;
	for (p=&file_systems; *p; p=&(*p)->next)
		if (strlen((*p)->name) == len &&
		    strncmp((*p)->name, name, len) == 0)
			break;
	return p;
}

/**
 *	register_filesystem - register a new filesystem
 *	@fs: the file system structure
 *
 *	Adds the file system passed to the list of file systems the kernel
 *	is aware of for mount and other syscalls. Returns 0 on success,
 *	or a negative errno code on an error.
 *
 *	The &struct file_system_type that is passed is linked into the kernel 
 *	structures and must not be freed until the file system has been
 *	unregistered.
 */
 
/** 20150221
 * 새로운 file_system_type을 등록한다.
 *
 * file_systems 리스트에 추가할 때 write_lock을 사용한다.
 **/
int register_filesystem(struct file_system_type * fs)
{
	int res = 0;
	struct file_system_type ** p;

	BUG_ON(strchr(fs->name, '.'));
	if (fs->next)
		return -EBUSY;
	write_lock(&file_systems_lock);
	/** 20150221
	 * 이미 fs가 file_systems에 등록되어 있다면(*p) 에러를 리턴.
	 * 등록되어 있지 않다면 fs를 가리킨다.
	 **/
	p = find_filesystem(fs->name, strlen(fs->name));
	if (*p)
		res = -EBUSY;
	else
		*p = fs;
	write_unlock(&file_systems_lock);
	return res;
}

EXPORT_SYMBOL(register_filesystem);

/**
 *	unregister_filesystem - unregister a file system
 *	@fs: filesystem to unregister
 *
 *	Remove a file system that was previously successfully registered
 *	with the kernel. An error is returned if the file system is not found.
 *	Zero is returned on a success.
 *	
 *	Once this function has returned the &struct file_system_type structure
 *	may be freed or reused.
 */
 
 /** 20150411
  * 주어진 파일시스템을 파일시스템 리스트에서 제거해 등록해제한다.
  **/
int unregister_filesystem(struct file_system_type * fs)
{
	struct file_system_type ** tmp;

	/** 20150411
	 * file_systems 리스트는 rwlock으로 보호된다.
	 **/
	write_lock(&file_systems_lock);
	/** 20150411
	 * 시스템의 filesystem 리스트를 순회하며,
	 * argument로 전달받은 fs인 경우 리스트에서 제거한다.
	 **/
	tmp = &file_systems;
	while (*tmp) {
		if (fs == *tmp) {
			*tmp = fs->next;
			fs->next = NULL;
			write_unlock(&file_systems_lock);
			/** 20150411
			 * 현재 존재하는 모든 rcu read-size critical section이 완료될 때까지
			 * 대기한다.
			 **/
			synchronize_rcu();
			return 0;
		}
		tmp = &(*tmp)->next;
	}
	write_unlock(&file_systems_lock);

	return -EINVAL;
}

EXPORT_SYMBOL(unregister_filesystem);

static int fs_index(const char __user * __name)
{
	struct file_system_type * tmp;
	char * name;
	int err, index;

	name = getname(__name);
	err = PTR_ERR(name);
	if (IS_ERR(name))
		return err;

	err = -EINVAL;
	read_lock(&file_systems_lock);
	for (tmp=file_systems, index=0 ; tmp ; tmp=tmp->next, index++) {
		if (strcmp(tmp->name,name) == 0) {
			err = index;
			break;
		}
	}
	read_unlock(&file_systems_lock);
	putname(name);
	return err;
}

static int fs_name(unsigned int index, char __user * buf)
{
	struct file_system_type * tmp;
	int len, res;

	read_lock(&file_systems_lock);
	for (tmp = file_systems; tmp; tmp = tmp->next, index--)
		if (index <= 0 && try_module_get(tmp->owner))
			break;
	read_unlock(&file_systems_lock);
	if (!tmp)
		return -EINVAL;

	/* OK, we got the reference, so we can safely block */
	len = strlen(tmp->name) + 1;
	res = copy_to_user(buf, tmp->name, len) ? -EFAULT : 0;
	put_filesystem(tmp);
	return res;
}

static int fs_maxindex(void)
{
	struct file_system_type * tmp;
	int index;

	read_lock(&file_systems_lock);
	for (tmp = file_systems, index = 0 ; tmp ; tmp = tmp->next, index++)
		;
	read_unlock(&file_systems_lock);
	return index;
}

/*
 * Whee.. Weird sysv syscall. 
 */
SYSCALL_DEFINE3(sysfs, int, option, unsigned long, arg1, unsigned long, arg2)
{
	int retval = -EINVAL;

	switch (option) {
		case 1:
			retval = fs_index((const char __user *) arg1);
			break;

		case 2:
			retval = fs_name(arg1, (char __user *) arg2);
			break;

		case 3:
			retval = fs_maxindex();
			break;
	}
	return retval;
}

int __init get_filesystem_list(char *buf)
{
	int len = 0;
	struct file_system_type * tmp;

	read_lock(&file_systems_lock);
	tmp = file_systems;
	while (tmp && len < PAGE_SIZE - 80) {
		len += sprintf(buf+len, "%s\t%s\n",
			(tmp->fs_flags & FS_REQUIRES_DEV) ? "" : "nodev",
			tmp->name);
		tmp = tmp->next;
	}
	read_unlock(&file_systems_lock);
	return len;
}

#ifdef CONFIG_PROC_FS
static int filesystems_proc_show(struct seq_file *m, void *v)
{
	struct file_system_type * tmp;

	read_lock(&file_systems_lock);
	tmp = file_systems;
	while (tmp) {
		seq_printf(m, "%s\t%s\n",
			(tmp->fs_flags & FS_REQUIRES_DEV) ? "" : "nodev",
			tmp->name);
		tmp = tmp->next;
	}
	read_unlock(&file_systems_lock);
	return 0;
}

static int filesystems_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, filesystems_proc_show, NULL);
}

static const struct file_operations filesystems_proc_fops = {
	.open		= filesystems_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_filesystems_init(void)
{
	proc_create("filesystems", 0, NULL, &filesystems_proc_fops);
	return 0;
}
module_init(proc_filesystems_init);
#endif

/** 20150425
 * 파일시스템에서 name을 찾아 리턴한다.
 **/
static struct file_system_type *__get_fs_type(const char *name, int len)
{
	struct file_system_type *fs;

	/** 20150425
	 * file_systems_lock read lock.
	 **/
	read_lock(&file_systems_lock);
	/** 20150425
	 * file_systems에서 name인 파일시스템을 찾는다.
	 * 파일시스템이 존재하면 module 참조 카운트를 증가시킨다.
	 * module 획득이 실패하면 파일시스템이 NULL을 리턴한다.
	 **/
	fs = *(find_filesystem(name, len));
	if (fs && !try_module_get(fs->owner))
		fs = NULL;
	read_unlock(&file_systems_lock);
	return fs;
}

/** 20150425
 * name인 파일시스템을 찾아 리턴한다.
 **/
struct file_system_type *get_fs_type(const char *name)
{
	struct file_system_type *fs;
	/** 20150425
	 * '.' 전까지 문자열 길이를 리턴
	 **/
	const char *dot = strchr(name, '.');
	int len = dot ? dot - name : strlen(name);

	/** 20150425
	 * name으로 등록된 파일시스템을 찾는다.
	 **/
	fs = __get_fs_type(name, len);
	if (!fs && (request_module("%.*s", len, name) == 0))
		fs = __get_fs_type(name, len);

	/** 20150425
	 * name에 dot이 포함되어 있지만, 서브타입이 존재하지 않는 경우에는
	 * 파일시스템 사용이 불가능한 것으로 리턴.
	 **/
	if (dot && fs && !(fs->fs_flags & FS_HAS_SUBTYPE)) {
		put_filesystem(fs);
		fs = NULL;
	}
	return fs;
}

EXPORT_SYMBOL(get_fs_type);
