#ifndef __LINUX_DCACHE_H
#define __LINUX_DCACHE_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rculist_bl.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>

struct nameidata;
struct path;
struct vfsmount;

/*
 * linux/include/linux/dcache.h
 *
 * Dirent cache data structures
 *
 * (C) Copyright 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

#define IS_ROOT(x) ((x) == (x)->d_parent)

/* The hash is always the low bits of hash_len */
/** 20150328
 * endian에 상관없이 hash가 항상 하위비트에 오도록 한다.
 **/
#ifdef __LITTLE_ENDIAN
 #define HASH_LEN_DECLARE u32 hash; u32 len;
#else
 #define HASH_LEN_DECLARE u32 len; u32 hash;
#endif

/*
 * "quick string" -- eases parameter passing, but more importantly
 * saves "metadata" about the string (ie length and the hash).
 *
 * hash comes first so it snuggles against d_parent in the
 * dentry.
 */
/** 20150328
 * denty에서 사용하는 object로,
 * name과 name에 대한 metadata(hash 또는 길이)를 묶어놓은 자료구조.
 **/
struct qstr {
	union {
		struct {
			HASH_LEN_DECLARE;
		};
		u64 hash_len;
	};
	const unsigned char *name;
};

/** 20150328
 * qstr의 name과 length 정보를 설정한다.
 **/
#define QSTR_INIT(n,l) { { { .len = l } }, .name = n }
/** 20150328
 * hashlen의 하위비트에 위치한 hash 값을 가져온다.
 * hashlen의 상위비트에 위치한 len  값을 가져온다.
 **/
#define hashlen_hash(hashlen) ((u32) (hashlen))
#define hashlen_len(hashlen)  ((u32)((hashlen) >> 32))

/** 20150405
 **/
struct dentry_stat_t {
	int nr_dentry;
	int nr_unused;
	int age_limit;          /* age in seconds */
	int want_pages;         /* pages requested by system */
	int dummy[2];
};
extern struct dentry_stat_t dentry_stat;

/* Name hashing routines. Initial hash value */
/* Hash courtesy of the R5 hash in reiserfs modulo sign bits */
#define init_name_hash()		0

/* partial hash update function. Assume roughly 4 bits per character */
static inline unsigned long
partial_name_hash(unsigned long c, unsigned long prevhash)
{
	return (prevhash + (c << 4) + (c >> 4)) * 11;
}

/*
 * Finally: cut down the number of bits to a int value (and try to avoid
 * losing bits)
 */
static inline unsigned long end_name_hash(unsigned long hash)
{
	return (unsigned int) hash;
}

/* Compute the hash for a name string. */
extern unsigned int full_name_hash(const unsigned char *, unsigned int);

/*
 * Try to keep struct dentry aligned on 64 byte cachelines (this will
 * give reasonable cacheline footprint with larger lines without the
 * large memory footprint increase).
 */
/** 20150328
 * dentry 구조체가 64바이트 캐시라인에 정렬되도록
 * 조건에 따라 DNAME의 길이를 조절한다.
 **/
#ifdef CONFIG_64BIT
# define DNAME_INLINE_LEN 32 /* 192 bytes */
#else
# ifdef CONFIG_SMP
#  define DNAME_INLINE_LEN 36 /* 128 bytes */
# else
#  define DNAME_INLINE_LEN 40 /* 128 bytes */
# endif
#endif

/** 20150328
 * dentry : directory entry를 표현하는 오브젝트. 한 경로에서 한 컴포넌트를 표현.
 * 
 * dentry는 spinlock으로 보호된다.
 * name을 갱신할 때 사용하는 seqlock이 별도로 존재한다.
 **/
struct dentry {
	/* RCU lookup touched fields */
	unsigned int d_flags;		/* protected by d_lock */
	seqcount_t d_seq;		/* per dentry seqlock */
	/** 20151024
	 * hashtable의 각 hash list에 등록하기 위한 entry.
	 **/
	struct hlist_bl_node d_hash;	/* lookup hash list */
	struct dentry *d_parent;	/* parent directory */
	struct qstr d_name;
	struct inode *d_inode;		/* Where the name belongs to - NULL is
					 * negative */
	/** 20150328
	 * dentry inline name.
	 * 
	 * 길이가 짧다면 dentry에 바로 저장하고, 길다면 별도의 메모리에 저장한다.
	 **/
	unsigned char d_iname[DNAME_INLINE_LEN];	/* small names */

	/* Ref lookup also touches following */
	/** 20150404
	 * dentry object의 상태를 나타낸다.
	 * positive : used   - 현재 사용 중
	 * zero     : unused - 유효하지만 현재 사용 중이 아님
	 * negative : negative - 유효한 inode와 연결되어 있지 않음 (d_inode is NULL).
	 *
	 * 이 속성은 d_lock에 의해 보호된다.
	 **/
	unsigned int d_count;		/* protected by d_lock */
	spinlock_t d_lock;		/* per dentry lock */
	const struct dentry_operations *d_op;
	struct super_block *d_sb;	/* The root of the dentry tree */
	unsigned long d_time;		/* used by d_revalidate */
	/** 20150404
	 * fs specific data.
	 **/
	void *d_fsdata;			/* fs-specific data */

	/** 20150404
	 * 'unused', 'negative' 상태의 dentry가 연결되는 리스트.
	 * dentry_lru_add에서 d_sb의 s_dentry_lru에 등록시킨다.
	 **/
	struct list_head d_lru;		/* LRU list */
	/*
	 * d_child and d_rcu can share memory
	 */
	union {
		struct list_head d_child;	/* child of parent list */
	 	struct rcu_head d_rcu;
	} d_u;
	struct list_head d_subdirs;	/* our children */
	/** 20150328
	 * inode의 i_dentry 리스트에 연결하는 자료구조.
	 **/
	struct hlist_node d_alias;	/* inode alias list */
};

/*
 * dentry->d_lock spinlock nesting subclasses:
 *
 * 0: normal
 * 1: nested
 */
enum dentry_d_lock_class
{
	DENTRY_D_LOCK_NORMAL, /* implicitly used by plain spin_lock() APIs. */
	DENTRY_D_LOCK_NESTED
};

struct dentry_operations {
	int (*d_revalidate)(struct dentry *, unsigned int);
	int (*d_hash)(const struct dentry *, const struct inode *,
			struct qstr *);
	int (*d_compare)(const struct dentry *, const struct inode *,
			const struct dentry *, const struct inode *,
			unsigned int, const char *, const struct qstr *);
	int (*d_delete)(const struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_prune)(struct dentry *);
	void (*d_iput)(struct dentry *, struct inode *);
	char *(*d_dname)(struct dentry *, char *, int);
	struct vfsmount *(*d_automount)(struct path *);
	int (*d_manage)(struct dentry *, bool);
} ____cacheline_aligned;

/*
 * Locking rules for dentry_operations callbacks are to be found in
 * Documentation/filesystems/Locking. Keep it updated!
 *
 * FUrther descriptions are found in Documentation/filesystems/vfs.txt.
 * Keep it updated too!
 */

/* d_flags entries */
/** 20150404
 * dentry_operations에 해당 op이 존재함을 dentry의 d_flags에 표시한다.
 **/
#define DCACHE_OP_HASH		0x0001
#define DCACHE_OP_COMPARE	0x0002
#define DCACHE_OP_REVALIDATE	0x0004
#define DCACHE_OP_DELETE	0x0008
#define DCACHE_OP_PRUNE         0x0010

#define	DCACHE_DISCONNECTED	0x0020
     /* This dentry is possibly not currently connected to the dcache tree, in
      * which case its parent will either be itself, or will have this flag as
      * well.  nfsd will not use a dentry with this bit set, but will first
      * endeavour to clear the bit either by discovering that it is connected,
      * or by performing lookup operations.   Any filesystem which supports
      * nfsd_operations MUST have a lookup function which, if it finds a
      * directory inode with a DCACHE_DISCONNECTED dentry, will d_move that
      * dentry into place and return that dentry rather than the passed one,
      * typically using d_splice_alias. */

#define DCACHE_REFERENCED	0x0040  /* Recently used, don't discard. */
#define DCACHE_RCUACCESS	0x0080	/* Entry has ever been RCU-visible */

#define DCACHE_CANT_MOUNT	0x0100
#define DCACHE_GENOCIDE		0x0200
#define DCACHE_SHRINK_LIST	0x0400

#define DCACHE_NFSFS_RENAMED	0x1000
     /* this dentry has been "silly renamed" and has to be deleted on the last
      * dput() */
#define DCACHE_COOKIE		0x2000	/* For use by dcookie subsystem */
#define DCACHE_FSNOTIFY_PARENT_WATCHED 0x4000
     /* Parent inode is watched by some fsnotify listener */

#define DCACHE_MOUNTED		0x10000	/* is a mountpoint */
#define DCACHE_NEED_AUTOMOUNT	0x20000	/* handle automount on this dir */
#define DCACHE_MANAGE_TRANSIT	0x40000	/* manage transit from this dirent */
#define DCACHE_NEED_LOOKUP	0x80000 /* dentry requires i_op->lookup */
#define DCACHE_MANAGED_DENTRY \
	(DCACHE_MOUNTED|DCACHE_NEED_AUTOMOUNT|DCACHE_MANAGE_TRANSIT)

extern seqlock_t rename_lock;

static inline int dname_external(struct dentry *dentry)
{
	return dentry->d_name.name != dentry->d_iname;
}

/*
 * These are the low-level FS interfaces to the dcache..
 */
extern void d_instantiate(struct dentry *, struct inode *);
extern struct dentry * d_instantiate_unique(struct dentry *, struct inode *);
extern struct dentry * d_materialise_unique(struct dentry *, struct inode *);
extern void __d_drop(struct dentry *dentry);
extern void d_drop(struct dentry *dentry);
extern void d_delete(struct dentry *);
extern void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op);

/* allocate/de-allocate */
extern struct dentry * d_alloc(struct dentry *, const struct qstr *);
extern struct dentry * d_alloc_pseudo(struct super_block *, const struct qstr *);
extern struct dentry * d_splice_alias(struct inode *, struct dentry *);
extern struct dentry * d_add_ci(struct dentry *, struct inode *, struct qstr *);
extern struct dentry *d_find_any_alias(struct inode *inode);
extern struct dentry * d_obtain_alias(struct inode *);
extern void shrink_dcache_sb(struct super_block *);
extern void shrink_dcache_parent(struct dentry *);
extern void shrink_dcache_for_umount(struct super_block *);
extern int d_invalidate(struct dentry *);

/* only used at mount-time */
extern struct dentry * d_make_root(struct inode *);

/* <clickety>-<click> the ramfs-type tree */
extern void d_genocide(struct dentry *);

extern struct dentry *d_find_alias(struct inode *);
extern void d_prune_aliases(struct inode *);

/* test whether we have any submounts in a subdir tree */
extern int have_submounts(struct dentry *);

/*
 * This adds the entry to the hash queues.
 */
extern void d_rehash(struct dentry *);

/**
 * d_add - add dentry to hash queues
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
 
static inline void d_add(struct dentry *entry, struct inode *inode)
{
	d_instantiate(entry, inode);
	d_rehash(entry);
}

/**
 * d_add_unique - add dentry to hash queues without aliasing
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
static inline struct dentry *d_add_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *res;

	res = d_instantiate_unique(entry, inode);
	d_rehash(res != NULL ? res : entry);
	return res;
}

extern void dentry_update_name_case(struct dentry *, struct qstr *);

/* used for rename() and baskets */
extern void d_move(struct dentry *, struct dentry *);
extern struct dentry *d_ancestor(struct dentry *, struct dentry *);

/* appendix may either be NULL or be used for transname suffixes */
extern struct dentry *d_lookup(struct dentry *, struct qstr *);
extern struct dentry *d_hash_and_lookup(struct dentry *, struct qstr *);
extern struct dentry *__d_lookup(struct dentry *, struct qstr *);
extern struct dentry *__d_lookup_rcu(const struct dentry *parent,
				const struct qstr *name,
				unsigned *seq, struct inode *inode);

/**
 * __d_rcu_to_refcount - take a refcount on dentry if sequence check is ok
 * @dentry: dentry to take a ref on
 * @seq: seqcount to verify against
 * Returns: 0 on failure, else 1.
 *
 * __d_rcu_to_refcount operates on a dentry,seq pair that was returned
 * by __d_lookup_rcu, to get a reference on an rcu-walk dentry.
 */
static inline int __d_rcu_to_refcount(struct dentry *dentry, unsigned seq)
{
	int ret = 0;

	assert_spin_locked(&dentry->d_lock);
	if (!read_seqcount_retry(&dentry->d_seq, seq)) {
		ret = 1;
		dentry->d_count++;
	}

	return ret;
}

/* validate "insecure" dentry pointer */
extern int d_validate(struct dentry *, struct dentry *);

/*
 * helper function for dentry_operations.d_dname() members
 */
extern char *dynamic_dname(struct dentry *, char *, int, const char *, ...);

extern char *__d_path(const struct path *, const struct path *, char *, int);
extern char *d_absolute_path(const struct path *, char *, int);
extern char *d_path(const struct path *, char *, int);
extern char *d_path_with_unreachable(const struct path *, char *, int);
extern char *dentry_path_raw(struct dentry *, char *, int);
extern char *dentry_path(struct dentry *, char *, int);

/* Allocation counts.. */

/**
 *	dget, dget_dlock -	get a reference to a dentry
 *	@dentry: dentry to get a reference to
 *
 *	Given a dentry or %NULL pointer increment the reference count
 *	if appropriate and return the dentry. A dentry will not be 
 *	destroyed when it has references.
 */
/** 20150404
 * dentry를 받아 NULL이 아니라면 reference count를 증가시켜 리턴한다.
 **/
static inline struct dentry *dget_dlock(struct dentry *dentry)
{
	if (dentry)
		dentry->d_count++;
	return dentry;
}

/** 20150404
 * dentry의 reference count를 증가시킨다.
 **/
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry) {
		/** 20150404
		 * dentry의 d_count를 변경하기 위해 d_lock을 걸고
		 * reference count를 증가시켜 리턴한다.
		 **/
		spin_lock(&dentry->d_lock);
		dget_dlock(dentry);
		spin_unlock(&dentry->d_lock);
	}
	return dentry;
}

extern struct dentry *dget_parent(struct dentry *dentry);

/**
 *	d_unhashed -	is dentry hashed
 *	@dentry: entry to check
 *
 *	Returns true if the dentry passed is not currently hashed.
 */
 
/** 20150404
 * dentry가 unhashed인지(hashtable에 존재하지 않는 경우) 조회해 리턴한다.
 **/
static inline int d_unhashed(struct dentry *dentry)
{
	return hlist_bl_unhashed(&dentry->d_hash);
}

static inline int d_unlinked(struct dentry *dentry)
{
	return d_unhashed(dentry) && !IS_ROOT(dentry);
}

static inline int cant_mount(struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_CANT_MOUNT);
}

static inline void dont_mount(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags |= DCACHE_CANT_MOUNT;
	spin_unlock(&dentry->d_lock);
}

extern void dput(struct dentry *);

static inline bool d_managed(struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MANAGED_DENTRY;
}

static inline bool d_mountpoint(struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MOUNTED;
}

/** 20150404
 * dentry의 flags를 검사해 'dentry requires i_op->lookup'인지 검사한다.
 **/
static inline bool d_need_lookup(struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_NEED_LOOKUP;
}

extern void d_clear_need_lookup(struct dentry *dentry);

extern int sysctl_vfs_cache_pressure;

#endif	/* __LINUX_DCACHE_H */
