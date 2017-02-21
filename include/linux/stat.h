#ifndef _LINUX_STAT_H
#define _LINUX_STAT_H

#ifdef __KERNEL__

#include <asm/stat.h>

#endif

#if defined(__KERNEL__) || !defined(__GLIBC__) || (__GLIBC__ < 2)

/** 20150425
 * S_IFMT     0170000   bit mask for the file type bit fields (파일종류 마스크)
 * S_IFSOCK   0140000   socket
 * S_IFLNK    0120000   symbolic link
 * S_IFREG    0100000   regular file
 * S_IFBLK    0060000   block device
 * S_IFDIR    0040000   directory
 * S_IFCHR    0020000   character device
 * S_IFIFO    0010000   FIFO
 *
 * S_ISUID   0004000   set-user-ID bit
 * S_ISGID   0002000   set-group-ID bit
 * S_ISVTX   0001000   sticky bit
 *   디렉토리에 주어진 경우 : 디렉토리 내 디렉토리/파일은 소유자에 의해서만
 *                            변경되거나 삭제될 수 있다.
 * 
 * S_IRWXU     00700   mask for file owner permissions
 * S_IRUSR     00400   owner has read permission
 * S_IWUSR     00200   owner has write permission
 * S_IXUSR     00100   owner has execute permission
 * 
 * S_IRWXG     00070   mask for group permissions
 * S_IRGRP     00040   group has read permission
 * S_IWGRP     00020   group has write permission
 * S_IXGRP     00010   group has execute permission
 * 
 * S_IRWXO     00007   mask for permissions for others
 * S_IROTH     00004   others have read permission
 * S_IWOTH     00002   others have write permission
 * S_IXOTH     00001   others have execute permission
 *
 * [출처] http://man7.org/linux/man-pages/man2/stat.2.html
 **/
#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK	 0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#endif

#ifdef __KERNEL__
/** 20150425
 * 위 비트의 조합값.
 * S_IRWXUGO : user/group/owner 권한
 * S_IALLUGO : S_IRWXUGO + set-user-id, set-group-id, sticky bit 포함
 **/
#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

#define UTIME_NOW	((1l << 30) - 1l)
#define UTIME_OMIT	((1l << 30) - 2l)

#include <linux/types.h>
#include <linux/time.h>
#include <linux/uidgid.h>

struct kstat {
	u64		ino;
	dev_t		dev;
	umode_t		mode;
	unsigned int	nlink;
	kuid_t		uid;
	kgid_t		gid;
	dev_t		rdev;
	loff_t		size;
	struct timespec  atime;
	struct timespec	mtime;
	struct timespec	ctime;
	unsigned long	blksize;
	unsigned long long	blocks;
};

#endif

#endif
