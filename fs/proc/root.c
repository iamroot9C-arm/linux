/*
 *  linux/fs/proc/root.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  proc root directory handling functions
 */

#include <asm/uaccess.h>

#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/mount.h>
#include <linux/pid_namespace.h>
#include <linux/parser.h>

#include "internal.h"

/** 20150502    
 * 등록된 filesystem에서 proc superblock를 찾아올 때 사용되는 함수.
 *
 * s_fs_info에 저장해둔 ns가 data와 일치해야 한다.
 **/
static int proc_test_super(struct super_block *sb, void *data)
{
	return sb->s_fs_info == data;
}

/** 20150502    
 * proc filesystem 타입의 superblock을 설정하는 콜백 함수.
 *
 * userspace에서 마운트 했을 때의 data를 s_fs_info에 저장해 비교하므로
 * 각 task마다 다른 proc 정보를 접근할 수 있다.
 **/
static int proc_set_super(struct super_block *sb, void *data)
{
	/** 20150502    
	 * superblock에 anonymous block 디바이스 번호를 저장하고, bdi를 지정한다.
	 **/
	int err = set_anon_super(sb, NULL);
	if (!err) {
		struct pid_namespace *ns = (struct pid_namespace *)data;
		/** 20150502    
		 * ns를 받아와 s_fs_info에 저장한다.
		 **/
		sb->s_fs_info = get_pid_ns(ns);
	}
	return err;
}

enum {
	Opt_gid, Opt_hidepid, Opt_err,
};

/** 20150509    
 * proc option용 match_table.
 **/
static const match_table_t tokens = {
	{Opt_hidepid, "hidepid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_err, NULL},
};

/** 20150509    
 * proc용 mount 옵션 처리 함수.
 * 자세한 내용은 추후 분석???
 **/
static int proc_parse_options(char *options, struct pid_namespace *pid)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;

	if (!options)
		return 1;

	/** 20150509    
	 * ","를 기준으로 파싱해 옵션을 처리한다.
	 **/
	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		args[0].to = args[0].from = 0;
		/** 20150509    
		 * tokens에 존재하는 token별로 값을 가져와 처리한다.
		 **/
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_gid:
			if (match_int(&args[0], &option))
				return 0;
			pid->pid_gid = make_kgid(current_user_ns(), option);
			break;
		case Opt_hidepid:
			if (match_int(&args[0], &option))
				return 0;
			if (option < 0 || option > 2) {
				pr_err("proc: hidepid value must be between 0 and 2.\n");
				return 0;
			}
			pid->hide_pid = option;
			break;
		default:
			pr_err("proc: unrecognized mount option \"%s\" "
			       "or missing value\n", p);
			return 0;
		}
	}

	return 1;
}

int proc_remount(struct super_block *sb, int *flags, char *data)
{
	struct pid_namespace *pid = sb->s_fs_info;
	return !proc_parse_options(data, pid);
}

/** 20150509    
 * proc filesystem의 mount 콜백 함수.
 *
 * proc_inode라는 구조체가 VFS inode에 대응되는 개념으로 사용되며,
 * data로 전달된 struct pid_namespace에서 1에 해당하는 struct pid가 저장된다.
 **/
static struct dentry *proc_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	int err;
	struct super_block *sb;
	struct pid_namespace *ns;
	struct proc_inode *ei;
	char *options;

	/** 20150502    
	 * flags에 MS_KERNMOUNT가 주어졌다면, 즉 커널 내에서 호출된 경우와
	 * userspace에서 mount한 경우에 대해 namespace와 options를 달리한다.
	 **/
	if (flags & MS_KERNMOUNT) {
		ns = (struct pid_namespace *)data;
		options = NULL;
	} else {
		ns = current->nsproxy->pid_ns;
		options = data;
	}

	/** 20150502    
	 * filesystem의 superblock을 받아온다.
	 * proc용 test, set 함수를 지정하고, fs specific한 데이터로 ns를 저장한다.
	 **/
	sb = sget(fs_type, proc_test_super, proc_set_super, flags, ns);
	if (IS_ERR(sb))
		return ERR_CAST(sb);

	/** 20150509    
	 * mount시 제공된 options들을 파싱해 처리한다.
	 **/
	if (!proc_parse_options(options, ns)) {
		deactivate_locked_super(sb);
		return ERR_PTR(-EINVAL);
	}

	/** 20150509    
	 * superblock의 root dentry가 비어 있는 상태라면
	 * 새로 할당된 superblock이므로 proc을 위한 superblock 정보를 채운다.
	 **/
	if (!sb->s_root) {
		err = proc_fill_super(sb);
		if (err) {
			deactivate_locked_super(sb);
			return ERR_PTR(err);
		}

		/** 20150509    
		 * superblock flag에 동작 중임을 표시.
		 **/
		sb->s_flags |= MS_ACTIVE;
	}

	/** 20150509    
	 * root dentry의 inode를 멤버로 포함하는 proc_inode를 받아온다.
	 **/
	ei = PROC_I(sb->s_root->d_inode);
	/** 20150509    
	 * pid 정보가 비어있다면 
	 **/
	if (!ei->pid) {
		rcu_read_lock();
		/** 20150509    
		 * ns에서 pid 1번에 해당하는 struct pid를 struct proc_inode에 저장한다.
		 **/
		ei->pid = get_pid(find_pid_ns(1, ns));
		rcu_read_unlock();
	}

	/** 20150509    
	 * superblock의 root inode의 dentry를 리턴한다.
	 **/
	return dget(sb->s_root);
}

static void proc_kill_sb(struct super_block *sb)
{
	struct pid_namespace *ns;

	ns = (struct pid_namespace *)sb->s_fs_info;
	kill_anon_super(sb);
	put_pid_ns(ns);
}

/** 20150502    
 * "proc" filesystem 속성.
 **/
static struct file_system_type proc_fs_type = {
	.name		= "proc",
	.mount		= proc_mount,
	.kill_sb	= proc_kill_sb,
};

void __init proc_root_init(void)
{
	int err;

	/** 20150502    
	 * proc의 inode용 object를 할당할 kmem_cache를 생성한다.
	 **/
	proc_init_inodecache();
	/** 20150502    
	 * proc파일시스템을 등록한다.
	 **/
	err = register_filesystem(&proc_fs_type);
	if (err)
		return;
	/** 20150502    
	 * proc파일시스템이 등록된 후, init_pid_ns를 위한 proc mount를 수행한다.
	 **/
	err = pid_ns_prepare_proc(&init_pid_ns);
	if (err) {
		unregister_filesystem(&proc_fs_type);
		return;
	}

	/** 20150509    
	 * /proc 아래 "self/mounts"를 대상으로 하는 심볼릭 링크 "mounts"를 생성한다.
	 **/
	proc_symlink("mounts", NULL, "self/mounts");

	/** 20150509    
	 * proc net 관련 초기화를 한다.
	 **/
	proc_net_init();

	/** 20150509    
	 * CONFIG_SYSVIPC가 설정되어 있다.
	 **/
#ifdef CONFIG_SYSVIPC
	/** 20150509    
	 * /proc 아래 "sysvipic", "fs", "driver", "fs/nfsd" 디렉토리 entry를 생성한다.
	 **/
	proc_mkdir("sysvipc", NULL);
#endif
	proc_mkdir("fs", NULL);
	proc_mkdir("driver", NULL);
	proc_mkdir("fs/nfsd", NULL); /* somewhere for the nfsd filesystem to be mounted */
#if defined(CONFIG_SUN_OPENPROMFS) || defined(CONFIG_SUN_OPENPROMFS_MODULE)
	/* just give it a mountpoint */
	proc_mkdir("openprom", NULL);
#endif
	/** 20150509    
	 * "/proc/tty" 아래 entry를 생성한다.
	 **/
	proc_tty_init();
#ifdef CONFIG_PROC_DEVICETREE
	/** 20150509    
	 * CONFIG_PROC_DEVICETREE 설정하지 않음.
	 **/
	proc_device_tree_init();
#endif
	/** 20150509    
	 * /proc 아래 "bus" 디렉토리 entry를 생성한다.
	 **/
	proc_mkdir("bus", NULL);
	proc_sys_init();
}

static int proc_root_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat
)
{
	generic_fillattr(dentry->d_inode, stat);
	stat->nlink = proc_root.nlink + nr_processes();
	return 0;
}

static struct dentry *proc_root_lookup(struct inode * dir, struct dentry * dentry, unsigned int flags)
{
	if (!proc_lookup(dir, dentry, flags))
		return NULL;
	
	return proc_pid_lookup(dir, dentry, flags);
}

static int proc_root_readdir(struct file * filp,
	void * dirent, filldir_t filldir)
{
	unsigned int nr = filp->f_pos;
	int ret;

	if (nr < FIRST_PROCESS_ENTRY) {
		int error = proc_readdir(filp, dirent, filldir);
		if (error <= 0)
			return error;
		filp->f_pos = FIRST_PROCESS_ENTRY;
	}

	ret = proc_pid_readdir(filp, dirent, filldir);
	return ret;
}

/*
 * The root /proc directory is special, as it has the
 * <pid> directories. Thus we don't use the generic
 * directory handling functions for that..
 */
static const struct file_operations proc_root_operations = {
	.read		 = generic_read_dir,
	.readdir	 = proc_root_readdir,
	.llseek		= default_llseek,
};

/*
 * proc root can do almost nothing..
 */
/** 20150509    
 * proc root inode ops.
 **/
static const struct inode_operations proc_root_inode_operations = {
	.lookup		= proc_root_lookup,
	.getattr	= proc_root_getattr,
};

/*
 * This is the root "inode" in the /proc tree..
 */
/** 20150509    
 * proc filesystem에서 entry 관리를 위해 사용되는 proc_dir_entry 중 root를 정의.
 **/
struct proc_dir_entry proc_root = {
	.low_ino	= PROC_ROOT_INO, 
	.namelen	= 5, 
	.mode		= S_IFDIR | S_IRUGO | S_IXUGO, 
	.nlink		= 2, 
	.count		= ATOMIC_INIT(1),
	.proc_iops	= &proc_root_inode_operations, 
	.proc_fops	= &proc_root_operations,
	.parent		= &proc_root,
	.name		= "/proc",
};

/** 20150509    
 * 특정 ns에 대하여 proc 파일시스템을 마운트 한다.
 * 마운트 후 생성된 vfsmount 객체 주소를 ns에 저장한다.
 **/
int pid_ns_prepare_proc(struct pid_namespace *ns)
{
	struct vfsmount *mnt;

	/** 20150509    
	 * proc filesystem을 mount 한다. mountdata로 ns를 전달한다.
	 * proc fs에 해당하는 proc_mount가 호출된다.
	 *
	 * 내부에서 superblock에 root inode의 dentry가 저장되고,
	 * struct vfsmount 객체가 설정되어 리턴된다.
	 **/
	mnt = kern_mount_data(&proc_fs_type, ns);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	ns->proc_mnt = mnt;
	return 0;
}

void pid_ns_release_proc(struct pid_namespace *ns)
{
	kern_unmount(ns->proc_mnt);
}
