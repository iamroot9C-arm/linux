/*
 * fs/sysfs/sysfs.h - sysfs internal header file
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 */

#include <linux/lockdep.h>
#include <linux/kobject_ns.h>
#include <linux/fs.h>
#include <linux/rbtree.h>

struct sysfs_open_dirent;

/* type-specific structures for sysfs_dirent->s_* union members */
/** 20150411    
 * sysfs_dirent가 directory인 경우에 해당하는 union 멤버.
 **/
struct sysfs_elem_dir {
	struct kobject		*kobj;

	unsigned long		subdirs;
	/* children rbtree starts here and goes through sd->s_rb */
	/** 20150411    
	 * children rbtree
	 **/
	struct rb_root		children;
};

struct sysfs_elem_symlink {
	struct sysfs_dirent	*target_sd;
};

struct sysfs_elem_attr {
	struct attribute	*attr;
	struct sysfs_open_dirent *open;
};

struct sysfs_elem_bin_attr {
	struct bin_attribute	*bin_attr;
	struct hlist_head	buffers;
};

/** 20150404    
 * sysfs의 inode attributes
 **/
struct sysfs_inode_attrs {
	struct iattr	ia_iattr;
	void		*ia_secdata;
	u32		ia_secdata_len;
};

/*
 * sysfs_dirent - the building block of sysfs hierarchy.  Each and
 * every sysfs node is represented by single sysfs_dirent.
 *
 * As long as s_count reference is held, the sysfs_dirent itself is
 * accessible.  Dereferencing s_elem or any other outer entity
 * requires s_active reference.
 */
/** 20150314    
 * sysfs 계층 구조에서 각 sysfs 노드의 블럭 정보.
 *
 * s_parent : parent인 sysfs_dirent를 지정한다.
 * s_count : reference count
 **/
struct sysfs_dirent {
	atomic_t		s_count;
	atomic_t		s_active;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
	struct sysfs_dirent	*s_parent;
	const char		*s_name;

	struct rb_node		s_rb;

	union {
		struct completion	*completion;
		struct sysfs_dirent	*removed_list;
	} u;

	const void		*s_ns; /* namespace tag */
	unsigned int		s_hash; /* ns + name hash */
	union {
		struct sysfs_elem_dir		s_dir;
		struct sysfs_elem_symlink	s_symlink;
		struct sysfs_elem_attr		s_attr;
		struct sysfs_elem_bin_attr	s_bin_attr;
	};

	unsigned short		s_flags;
	umode_t 		s_mode;
	unsigned int		s_ino;
	struct sysfs_inode_attrs *s_iattr;
};

#define SD_DEACTIVATED_BIAS		INT_MIN

#define SYSFS_TYPE_MASK			0x00ff
#define SYSFS_DIR			0x0001
#define SYSFS_KOBJ_ATTR			0x0002
#define SYSFS_KOBJ_BIN_ATTR		0x0004
#define SYSFS_KOBJ_LINK			0x0008
#define SYSFS_COPY_NAME			(SYSFS_DIR | SYSFS_KOBJ_LINK)
#define SYSFS_ACTIVE_REF		(SYSFS_KOBJ_ATTR | SYSFS_KOBJ_BIN_ATTR)

/* identify any namespace tag on sysfs_dirents */
#define SYSFS_NS_TYPE_MASK		0xf00
#define SYSFS_NS_TYPE_SHIFT		8

#define SYSFS_FLAG_MASK			~(SYSFS_NS_TYPE_MASK|SYSFS_TYPE_MASK)
#define SYSFS_FLAG_REMOVED		0x02000

/** 20150404    
 * sysfs_direct의 s_flags 중 TYPE 부분을 마스킹 해온다.
 **/
static inline unsigned int sysfs_type(struct sysfs_dirent *sd)
{
	return sd->s_flags & SYSFS_TYPE_MASK;
}

/*
 * Return any namespace tags on this dirent.
 * enum kobj_ns_type is defined in linux/kobject.h
 */
/** 20150411    
 * sysfs_dirent의 s_flags에 포함된 NS TYPE을 추출한다.
 **/
static inline enum kobj_ns_type sysfs_ns_type(struct sysfs_dirent *sd)
{
	return (sd->s_flags & SYSFS_NS_TYPE_MASK) >> SYSFS_NS_TYPE_SHIFT;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define sysfs_dirent_init_lockdep(sd)				\
do {								\
	struct attribute *attr = sd->s_attr.attr;		\
	struct lock_class_key *key = attr->key;			\
	if (!key)						\
		key = &attr->skey;				\
								\
	lockdep_init_map(&sd->dep_map, "s_active", key, 0);	\
} while(0)
#else
#define sysfs_dirent_init_lockdep(sd) do {} while(0)
#endif

/*
 * Context structure to be used while adding/removing nodes.
 */
/** 20150411    
 * sysfs에 add/rm시 사용되는 context.
 * parent와 removed sysfs_dirent가 저장된다.
 **/
struct sysfs_addrm_cxt {
	struct sysfs_dirent	*parent_sd;
	struct sysfs_dirent	*removed;
};

/*
 * mount.c
 */

/*
 * Each sb is associated with a set of namespace tags (i.e.
 * the network namespace of the task which mounted this sysfs
 * instance).
 */
/** 20150307    
 * kobjs의 namespace 타입별 정보를 저장하는 구조체.
 **/
struct sysfs_super_info {
	void *ns[KOBJ_NS_TYPES];
};
/** 20150307    
 * sb의 fs private 정보를 추출한다.
 **/
#define sysfs_info(SB) ((struct sysfs_super_info *)(SB->s_fs_info))
extern struct sysfs_dirent sysfs_root;
extern struct kmem_cache *sysfs_dir_cachep;

/*
 * dir.c
 */
extern struct mutex sysfs_mutex;
extern spinlock_t sysfs_assoc_lock;
extern const struct dentry_operations sysfs_dentry_ops;

extern const struct file_operations sysfs_dir_operations;
extern const struct inode_operations sysfs_dir_inode_operations;

struct dentry *sysfs_get_dentry(struct sysfs_dirent *sd);
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd);
void sysfs_put_active(struct sysfs_dirent *sd);
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd);
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt);

struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const void *ns,
				       const unsigned char *name);
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const void *ns,
				      const unsigned char *name);
struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type);

void release_sysfs_dirent(struct sysfs_dirent *sd);

int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd);
void sysfs_remove_subdir(struct sysfs_dirent *sd);

int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const void *ns, const char *new_name);

/** 20150321    
 * sysfs_dirent의 s_count를 증가시키고 sysfs_dirent를 리턴한다.
 **/
static inline struct sysfs_dirent *__sysfs_get(struct sysfs_dirent *sd)
{
	if (sd) {
		WARN_ON(!atomic_read(&sd->s_count));
		atomic_inc(&sd->s_count);
	}
	return sd;
}
#define sysfs_get(sd) __sysfs_get(sd)

/** 20150404    
 * sysfs_dirent의 s_count를 감소시키고, 0이 되었다면 해당 dirent를 제거한다.
 *
 * sd를 제거함으로써 parent의 reference count도 0이 된다면 반복해 호출한다.
 **/
static inline void __sysfs_put(struct sysfs_dirent *sd)
{
	if (sd && atomic_dec_and_test(&sd->s_count))
		release_sysfs_dirent(sd);
}
#define sysfs_put(sd) __sysfs_put(sd)

/*
 * inode.c
 */
struct inode *sysfs_get_inode(struct super_block *sb, struct sysfs_dirent *sd);
void sysfs_evict_inode(struct inode *inode);
int sysfs_sd_setattr(struct sysfs_dirent *sd, struct iattr *iattr);
int sysfs_permission(struct inode *inode, int mask);
int sysfs_setattr(struct dentry *dentry, struct iattr *iattr);
int sysfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
int sysfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		size_t size, int flags);
int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const void *ns, const char *name);
int sysfs_inode_init(void);

/*
 * file.c
 */
extern const struct file_operations sysfs_file_operations;

int sysfs_add_file(struct sysfs_dirent *dir_sd,
		   const struct attribute *attr, int type);

int sysfs_add_file_mode(struct sysfs_dirent *dir_sd,
			const struct attribute *attr, int type, umode_t amode);
/*
 * bin.c
 */
extern const struct file_operations bin_fops;
void unmap_bin_file(struct sysfs_dirent *attr_sd);

/*
 * symlink.c
 */
extern const struct inode_operations sysfs_symlink_inode_operations;
