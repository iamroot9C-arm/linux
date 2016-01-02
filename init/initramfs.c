/*
 * Many of the syscalls used in this file expect some of the arguments
 * to be __user pointers not __kernel pointers.  To limit the sparse
 * noise, turn off sparse checking for this file.
 */
#ifdef __CHECKER__
#undef __CHECKER__
#warning "Sparse checking disabled for this file"
#endif

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/utime.h>

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

/* link hash */

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc(sizeof(struct hash), GFP_KERNEL);
	if (!q)
		panic("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strcpy(q->name, name);
	q->next = NULL;
	*p = q;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
}

/** 20160103    
 * CWD에서 filename을 찾아 mtime으로 설정된 timespec으로 시간을 수정한다.
 **/
static long __init do_utime(char *filename, time_t mtime)
{
	struct timespec t[2];

	t[0].tv_sec = mtime;
	t[0].tv_nsec = 0;
	t[1].tv_sec = mtime;
	t[1].tv_nsec = 0;

	return do_utimes(AT_FDCWD, filename, t, AT_SYMLINK_NOFOLLOW);
}

static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	char *name;
	time_t mtime;
};

/** 20160103    
 * directory entry를 생성하고 초기화해 파일 전역 리스트에 등록한다.
 **/
static void __init dir_add(const char *name, time_t mtime)
{
	struct dir_entry *de = kmalloc(sizeof(struct dir_entry), GFP_KERNEL);
	if (!de)
		panic("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	de->name = kstrdup(name, GFP_KERNEL);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

/** 20160103    
 * dir_list에 등록된 목록의 시간을 갱신하고, 디렉토리 엔트리는 삭제한다.
 **/
static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de->name);
		kfree(de);
	}
}

static __initdata time_t mtime;

/* cpio header parsing */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;

/** 20160103    
 * cpio header를 파싱해 각 변수에 저장한다.
 * 저장한 데이터는 파일 트리를 구성할 때 사용된다.
 *
 * https://www.kernel.org/doc/Documentation/early-userspace/buffer-format.txt
 **/
static void __init parse_header(char *s)
{
	unsigned long parsed[12];
	char buf[9];
	int i;

	buf[8] = '\0';
	for (i = 0, s += 6; i < 12; i++, s += 8) {
		memcpy(buf, s, 8);
		parsed[i] = simple_strtoul(buf, NULL, 16);
	}
	ino = parsed[0];
	mode = parsed[1];
	uid = parsed[2];
	gid = parsed[3];
	nlink = parsed[4];
	mtime = parsed[5];
	body_len = parsed[6];
	major = parsed[7];
	minor = parsed[8];
	rdev = new_encode_dev(MKDEV(parsed[9], parsed[10]));
	name_len = parsed[11];
}

/* FSM */

static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

static __initdata char *victim;
static __initdata unsigned count;
static __initdata loff_t this_header, next_header;

static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	count -= n;
}

static __initdata char *vcollected;
static __initdata char *collected;
static __initdata int remains;
static __initdata char *collect;

/** 20160102    
 * size byte씩 읽어 buf에 넣는다.
 * 읽을 크기가 size 이상이면 next state로 계속 복사.
 * 읽을 크기가 size보다 작다면 복사가 끝났으므로 Collect state로 이동.
 **/
static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

/** 20160102    
 * 
 **/
static int __init do_start(void)
{
	read_into(header_buf, 110, GotHeader);
	return 0;
}

static int __init do_collect(void)
{
	unsigned n = remains;
	if (count < n)
		n = count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

/** 20160102    
 * 읽은 cpio의 헤더를 파싱해 심볼릭 링크를 읽거나 name_buf에 이름을 읽고 GotName.
 **/
static int __init do_header(void)
{
	if (memcmp(collected, "070707", 6)==0) {
		error("incorrect cpio method used: use -H newc option");
		return 1;
	}
	if (memcmp(collected, "070701", 6)) {
		error("no cpio magic");
		return 1;
	}
	parse_header(collected);
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

static int __init do_skip(void)
{
	if (this_header + count < next_header) {
		eat(count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

static int __init do_reset(void)
{
	while(count && *victim == '\0')
		eat(1);
	if (count && (this_header & 3))
		error("broken padding");
	return 1;
}

static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old)
			return (sys_link(old, collected) < 0) ? -1 : 1;
	}
	return 0;
}

static void __init clean_path(char *path, umode_t mode)
{
	struct stat st;

	if (!sys_newlstat(path, &st) && (st.st_mode^mode) & S_IFMT) {
		if (S_ISDIR(st.st_mode))
			sys_rmdir(path);
		else
			sys_unlink(path);
	}
}

static __initdata int wfd;

/** 20160102    
 * 파싱된 header정보를 바탕으로 파일/디렉토리/디바이스 파일에 따라 rootfs에
 * 파일시스템을 구성한다.
 **/
static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;
	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT;
			if (ml != 1)
				openflags |= O_TRUNC;
			wfd = sys_open(collected, openflags, mode);

			if (wfd >= 0) {
				sys_fchown(wfd, uid, gid);
				sys_fchmod(wfd, mode);
				if (body_len)
					sys_ftruncate(wfd, body_len);
				vcollected = kstrdup(collected, GFP_KERNEL);
				state = CopyFile;
			}
		}
	} else if (S_ISDIR(mode)) {
		/** 20160103    
		 * 파싱한 파일이 디렉토리라면
		 * - 디렉토리를 생성
		 * - owner 지정
		 * - 접근권한 지정
		 * - 디렉토리 엔트리로 만들어 파일 전역 리스트에 등록
		 **/
		sys_mkdir(collected, mode);
		sys_chown(collected, uid, gid);
		sys_chmod(collected, mode);
		dir_add(collected, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			sys_mknod(collected, mode, rdev);
			sys_chown(collected, uid, gid);
			sys_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

static int __init do_copy(void)
{
	if (count >= body_len) {
		sys_write(wfd, victim, body_len);
		sys_close(wfd);
		do_utime(vcollected, mtime);
		kfree(vcollected);
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		sys_write(wfd, victim, count);
		body_len -= count;
		eat(count);
		return 1;
	}
}

static int __init do_symlink(void)
{
	collected[N_ALIGN(name_len) + body_len] = '\0';
	clean_path(collected, 0);
	sys_symlink(collected + N_ALIGN(name_len), collected);
	sys_lchown(collected, uid, gid);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

/** 20160102    
 * state machine에 해당하는 action.
 *
 * 각 함수에서 정상 처리될 경우 이전 state에서 다른 state로 전환되고 0을 리턴한다.
 **/
static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

/** 20160102    
 * buffer를 len만큼, state machine을 돌려 rootfs에 파일 트리를 구성한다.
 **/
static int __init write_buffer(char *buf, unsigned len)
{
	count = len;
	victim = buf;

	/** 20160102    
	 * state machine이 돌아가 buf(archive 형태)의 header를 파싱해
	 * rootfs에 기록한다. (mount는 init_mount_tree에서 진행한다)
	 **/
	while (!actions[state]())
		;
	return len - count;
}

static int __init flush_buffer(void *bufv, unsigned len)
{
	char *buf = (char *) bufv;
	int written;
	int origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk in compressed archive");
	}
	return origLen;
}

static unsigned my_inptr;   /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

/** 20160102    
 * 입력 버퍼를 해석해 rootfs에 파일 트리를 구성한다.
 * 압축된 데이터라면 먼저 decompress 함수로 해제한다.
 * 
 * 입력 파라미터는 압축 해제할 원본 데이터의 위치와 크기.
 **/
static char * __init unpack_to_rootfs(char *buf, unsigned len)
{
	int written, res;
	decompress_fn decompress;
	const char *compress_name;
	static __initdata char msg_buf[64];

	/** 20160103    
	 * state machine내에서 파일 트리를 구성할 때 사용할 버퍼 할당.
	 **/
	header_buf = kmalloc(110, GFP_KERNEL);
	symlink_buf = kmalloc(PATH_MAX + N_ALIGN(PATH_MAX) + 1, GFP_KERNEL);
	name_buf = kmalloc(N_ALIGN(PATH_MAX), GFP_KERNEL);

	if (!header_buf || !symlink_buf || !name_buf)
		panic("can't allocate buffers");

	state = Start;
	this_header = 0;
	message = NULL;
	/** 20160103    
	 * message(에러 메시지)가 없고 len을 다 처리할 때까지 반복한다.
	 **/
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		decompress = decompress_method(buf, len, &compress_name);
		if (decompress) {
			res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			if (!message) {
				snprintf(msg_buf, sizeof msg_buf,
					 "compression method %s not configured",
					 compress_name);
				message = msg_buf;
			}
		} else
			error("junk in compressed archive");
		if (state != Reset)
			error("junk in compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	dir_utime();
	kfree(name_buf);
	kfree(symlink_buf);
	kfree(header_buf);
	return message;
}

static int __initdata do_retain_initrd;

/** 20160103    
 * retain_initrd 옵션이 지정된 경우, free하지 않고 리턴하기 위해 플래그 변수 설정
 **/
static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

/** 20160103    
 * initrd 버퍼용으로 사용한 메모리를 해제한다.
 **/
static void __init free_initrd(void)
{
#ifdef CONFIG_KEXEC
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);
#endif
	/** 20160103    
	 * retain_initrd 옵션이 지정된 경우 initrd 사용 메모리를 해제하지 않고 리턴.
	 **/
	if (do_retain_initrd)
		goto skip;

#ifdef CONFIG_KEXEC
	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start < crashk_end && initrd_end > crashk_start) {
		/*
		 * Initialize initrd memory region since the kexec boot does
		 * not do.
		 */
		memset((void *)initrd_start, 0, initrd_end - initrd_start);
		if (initrd_start < crashk_start)
			free_initrd_mem(initrd_start, crashk_start);
		if (initrd_end > crashk_end)
			free_initrd_mem(crashk_end, initrd_end);
	} else
#endif
		/** 20160103    
		 * initrd_start ~ initrd_end 사이의 메모리를 해제한다.
		 **/
		free_initrd_mem(initrd_start, initrd_end);
skip:
	initrd_start = 0;
	initrd_end = 0;
}

#ifdef CONFIG_BLK_DEV_RAM
#define BUF_SIZE 1024
static void __init clean_rootfs(void)
{
	int fd;
	void *buf;
	struct linux_dirent64 *dirp;
	int num;

	fd = sys_open("/", O_RDONLY, 0);
	WARN_ON(fd < 0);
	if (fd < 0)
		return;
	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	WARN_ON(!buf);
	if (!buf) {
		sys_close(fd);
		return;
	}

	dirp = buf;
	num = sys_getdents64(fd, dirp, BUF_SIZE);
	while (num > 0) {
		while (num > 0) {
			struct stat st;
			int ret;

			ret = sys_newlstat(dirp->d_name, &st);
			WARN_ON_ONCE(ret);
			if (!ret) {
				if (S_ISDIR(st.st_mode))
					sys_rmdir(dirp->d_name);
				else
					sys_unlink(dirp->d_name);
			}

			num -= dirp->d_reclen;
			dirp = (void *)dirp + dirp->d_reclen;
		}
		dirp = buf;
		memset(buf, 0, BUF_SIZE);
		num = sys_getdents64(fd, dirp, BUF_SIZE);
	}

	sys_close(fd);
	kfree(buf);
}
#endif

/** 20160103    
 *
 *
 * CONFIG_BLK_DEV_INITRD가 지정되지 않았다면 populate_rootfs 대신 default_rootfs가 호출.
 **/
static int __init populate_rootfs(void)
{
	/** 20160102    
	 * CONFIG_BLK_DEV_INITRD가 지정되었다면 __initramfs_start는 항상 포함된다.
	 * CONFIG_INITRAMFS_SOURCE을 지정하지 않은 경우에는 스크립트로 기본 정보가
	 * 생성된다 (gen_init_cpio, gen_initramfs_list.sh)
	 *
	 **/
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic(err);	/* Failed to decompress INTERNAL initramfs */
	/** 20160103    
	 * initrd_start는 arch_memblock_init에서 __phys_to_virt(phys_initrd_start)로 지정된다.
	 *	(phys_initrd_start는 atags나 param으로 설정된다)
	 *
	 * initrd_start가 지정되었다면 rootfs에 해당 영역을 unpack 시킨다.
	 **/
	if (initrd_start) {
#ifdef CONFIG_BLK_DEV_RAM
		int fd;
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
		/** 20160103    
		 * initrd_start ~ initrd_end까지 unpack을 시도하고, 성공했다면 리턴.
		 * 에러가 발생했다면 initrd라 판단하여
		 * __initramfs_start ~ __initramfs_end 영역을 다시 unpack시키고, 
		 * "/initrd.image"에 이미지를 쓴다.
		 **/
		err = unpack_to_rootfs((char *)initrd_start,
			initrd_end - initrd_start);
		if (!err) {
			free_initrd();
			return 0;
		} else {
			clean_rootfs();
			unpack_to_rootfs(__initramfs_start, __initramfs_size);
		}
		printk(KERN_INFO "rootfs image is not initramfs (%s)"
				"; looks like an initrd\n", err);
		fd = sys_open("/initrd.image",
			      O_WRONLY|O_CREAT, 0700);
		if (fd >= 0) {
			sys_write(fd, (char *)initrd_start,
					initrd_end - initrd_start);
			sys_close(fd);
			free_initrd();
		}
#else
		printk(KERN_INFO "Unpacking initramfs...\n");
		err = unpack_to_rootfs((char *)initrd_start,
			initrd_end - initrd_start);
		if (err)
			printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
		free_initrd();
#endif
	}
	return 0;
}
rootfs_initcall(populate_rootfs);
