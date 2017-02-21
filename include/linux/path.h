#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

/** 20150425
 * file에 대한 path는 특정 vfsmount로부터 생성된 특정 dentry인 경우 unique하다.
 **/
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

extern void path_get(struct path *);
extern void path_put(struct path *);

static inline int path_equal(const struct path *path1, const struct path *path2)
{
	return path1->mnt == path2->mnt && path1->dentry == path2->dentry;
}

#endif  /* _LINUX_PATH_H */
