/*
 * kobject.c - library routines for handling generic kernel objects
 *
 * Copyright (c) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2006-2007 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2007 Novell Inc.
 *
 * This file is released under the GPLv2.
 *
 *
 * Please see the file Documentation/kobject.txt for critical information
 * about using the kobject interface.
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/export.h>
#include <linux/stat.h>
#include <linux/slab.h>

/*
 * populate_dir - populate directory with attributes.
 * @kobj: object we're working on.
 *
 * Most subsystems have a set of default attributes that are associated
 * with an object that registers with them.  This is a helper called during
 * object registration that loops through the default attributes of the
 * subsystem and creates attributes files for them in sysfs.
 */
/** 20150418
 * kobj로 생성된 sysfs dir에 default_attrs 속성들에 대한 file을 생성한다.
 **/
static int populate_dir(struct kobject *kobj)
{
	struct kobj_type *t = get_ktype(kobj);
	struct attribute *attr;
	int error = 0;
	int i;

	/** 20150418
	 * kobj의 ktype이 존재하면, 해당 ktype의 기본 속성(default_attrs) 들에 대해
	 * sysfs file을 생성한다.
	 **/
	if (t && t->default_attrs) {
		for (i = 0; (attr = t->default_attrs[i]) != NULL; i++) {
			error = sysfs_create_file(kobj, attr);
			if (error)
				break;
		}
	}
	return error;
}

/** 20150418
 * kobj를 위한 sysfs 디렉토리를 생성하고, attr를 파일로 생성한다.
 **/
static int create_dir(struct kobject *kobj)
{
	int error = 0;
	/** 20150418
	 * sysfs에 kobject를 위한 directory를 생성한다.
	 **/
	error = sysfs_create_dir(kobj);
	if (!error) {
		/** 20150418
		 * 생성한 디렉토리에 default_attrs를 파일로 생성한다.
		 **/
		error = populate_dir(kobj);
		if (error)
			sysfs_remove_dir(kobj);
	}
	return error;
}

static int get_kobj_path_length(struct kobject *kobj)
{
	int length = 1;
	struct kobject *parent = kobj;

	/* walk up the ancestors until we hit the one pointing to the
	 * root.
	 * Add 1 to strlen for leading '/' of each level.
	 */
	do {
		if (kobject_name(parent) == NULL)
			return 0;
		length += strlen(kobject_name(parent)) + 1;
		parent = parent->parent;
	} while (parent);
	return length;
}

static void fill_kobj_path(struct kobject *kobj, char *path, int length)
{
	struct kobject *parent;

	--length;
	for (parent = kobj; parent; parent = parent->parent) {
		int cur = strlen(kobject_name(parent));
		/* back up enough to print this name with '/' */
		length -= cur;
		strncpy(path + length, kobject_name(parent), cur);
		*(path + --length) = '/';
	}

	pr_debug("kobject: '%s' (%p): %s: path = '%s'\n", kobject_name(kobj),
		 kobj, __func__, path);
}

/**
 * kobject_get_path - generate and return the path associated with a given kobj and kset pair.
 *
 * @kobj:	kobject in question, with which to build the path
 * @gfp_mask:	the allocation type used to allocate the path
 *
 * The result must be freed by the caller with kfree().
 */
char *kobject_get_path(struct kobject *kobj, gfp_t gfp_mask)
{
	char *path;
	int len;

	len = get_kobj_path_length(kobj);
	if (len == 0)
		return NULL;
	path = kzalloc(len, gfp_mask);
	if (!path)
		return NULL;
	fill_kobj_path(kobj, path, len);

	return path;
}
EXPORT_SYMBOL_GPL(kobject_get_path);

/* add the kobject to its kset's list */
/** 20150411
 * kobj의 kset이 지정되어 있는 상태에서, kset의 리스트에 추가한다.
 **/
static void kobj_kset_join(struct kobject *kobj)
{
	/** 20150411
	 * kobj의 kset이 지정되지 않은 경우에 사용될 수 없다.
	 **/
	if (!kobj->kset)
		return;

	/** 20150411
	 * kobj가 가리키는 kset의 reference count를 증가시킨다.
	 **/
	kset_get(kobj->kset);
	/** 20150411
	 * spinlock을 걸고 kset의 list에 kobj를 등록시킨다.
	 **/
	spin_lock(&kobj->kset->list_lock);
	list_add_tail(&kobj->entry, &kobj->kset->list);
	spin_unlock(&kobj->kset->list_lock);
}

/* remove the kobject from its kset's list */
/** 20150418
 * kobj가 kset에 속해 있다면, kset 리스트에서 제거시킨다.
 **/
static void kobj_kset_leave(struct kobject *kobj)
{
	if (!kobj->kset)
		return;

	/** 20150418
	 * kobject가 속한 kset 리스트에서 kobject를 제거한다.
	 * 제거시 spinlock에 의해 kset이 보호되어야 한다.
	 **/
	spin_lock(&kobj->kset->list_lock);
	list_del_init(&kobj->entry);
	spin_unlock(&kobj->kset->list_lock);
	/** 20150418
	 * kset에서 kobject가 하나 감소했으므로 reference count를 감소시킨다.
	 **/
	kset_put(kobj->kset);
}

/** 20150411
 * kobject 구조체를 하나 초기화 한다.
 **/
static void kobject_init_internal(struct kobject *kobj)
{
	if (!kobj)
		return;
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->entry);
	kobj->state_in_sysfs = 0;
	kobj->state_add_uevent_sent = 0;
	kobj->state_remove_uevent_sent = 0;
	kobj->state_initialized = 1;
}


/** 20150418
 * kobject를 kset의 리스트에 추가하고 sysfs 디렉토리를 생성한다.
 * 함수 호출 후 state_in_sysfs 상태로 1이 된다.
 **/
static int kobject_add_internal(struct kobject *kobj)
{
	int error = 0;
	struct kobject *parent;

	if (!kobj)
		return -ENOENT;

	if (!kobj->name || !kobj->name[0]) {
		WARN(1, "kobject: (%p): attempted to be registered with empty "
			 "name!\n", kobj);
		return -EINVAL;
	}

	/** 20150411
	 * kobj의 parent의 reference count를 증가시킨다.
	 **/
	parent = kobject_get(kobj->parent);

	/* join kset if set, use it as parent if we do not already have one */
	/** 20150411
	 * kobj가 현재 kset에 포함되어 있을 경우 kobj를 kset에 조인시키고,
	 * kobj의 parent가 지정되지 않은 경우 kset을 kobj의 parent로 지정한다.
	 **/
	if (kobj->kset) {
		/** 20150411
		 * parent가 NULL인 경우, kset을 (실제로는 kset 내의 kobj) parent로 지정한다.
		 * kset이 parent로 지정되므로 kset->kobj의 reference count가 증가된다.
		 **/
		if (!parent)
			parent = kobject_get(&kobj->kset->kobj);
		/** 20150411
		 * kobj를 kset의 리스트에 추가한다.
		 * kobj의 parent가 없고 kset만 존재한다면 kset을 parent로 지정한다.
		 **/
		kobj_kset_join(kobj);
		kobj->parent = parent;
	}

	pr_debug("kobject: '%s' (%p): %s: parent: '%s', set: '%s'\n",
		 kobject_name(kobj), kobj, __func__,
		 parent ? kobject_name(parent) : "<NULL>",
		 kobj->kset ? kobject_name(&kobj->kset->kobj) : "<NULL>");

	/** 20150418
	 * kobj를 위한 sysfs 디렉토리를 생성하고, default_attrs를 파일로 생성한다.
	 **/
	error = create_dir(kobj);
	if (error) {
		kobj_kset_leave(kobj);
		kobject_put(parent);
		kobj->parent = NULL;

		/* be noisy on error issues */
		if (error == -EEXIST)
			WARN(1, "%s failed for %s with "
			     "-EEXIST, don't try to register things with "
			     "the same name in the same directory.\n",
			     __func__, kobject_name(kobj));
		else
			WARN(1, "%s failed for %s (error: %d parent: %s)\n",
			     __func__, kobject_name(kobj), error,
			     parent ? kobject_name(parent) : "'none'");
	} else
		/** 20150418
		 * 이제 kobj는 sysfs상에 존재한다.
		 **/
		kobj->state_in_sysfs = 1;

	return error;
}

/**
 * kobject_set_name_vargs - Set the name of an kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 * @vargs: vargs to format the string.
 */
/** 20150411
 * vargs를 파싱해 kobject의 name을 채운다.
 **/
int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
				  va_list vargs)
{
	const char *old_name = kobj->name;
	char *s;

	if (kobj->name && !fmt)
		return 0;

	/** 20150411
	 * fmt대로 argument를 파싱해 kobj의 이름을 지정한다.
	 **/
	kobj->name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!kobj->name)
		return -ENOMEM;

	/* ewww... some of these buggers have '/' in the name ... */
	/** 20150411
	 * name에 '/'가 존재하면 '!'로 치환한다.
	 **/
	while ((s = strchr(kobj->name, '/')))
		s[0] = '!';

	/** 20150411
	 * 이전 name을 해제한다.
	 **/
	kfree(old_name);
	return 0;
}

/**
 * kobject_set_name - Set the name of a kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 *
 * This sets the name of the kobject.  If you have already added the
 * kobject to the system, you must call kobject_rename() in order to
 * change the name of the kobject.
 */
/** 20150829
 * fmt를 파싱해 kobj의 name을 채운다.
 **/
int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list vargs;
	int retval;

	va_start(vargs, fmt);
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	va_end(vargs);

	return retval;
}
EXPORT_SYMBOL(kobject_set_name);

/**
 * kobject_init - initialize a kobject structure
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 *
 * This function will properly initialize a kobject such that it can then
 * be passed to the kobject_add() call.
 *
 * After this function is called, the kobject MUST be cleaned up by a call
 * to kobject_put(), not by a call to kfree directly to ensure that all of
 * the memory is cleaned up properly.
 */
/** 20150411
 * 'ktype' 타입인 kobject를 하나 초기화 한다.
 **/
void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
	char *err_str;

	if (!kobj) {
		err_str = "invalid kobject pointer!";
		goto error;
	}
	if (!ktype) {
		err_str = "must have a ktype to be initialized properly!\n";
		goto error;
	}
	if (kobj->state_initialized) {
		/* do not error out as sometimes we can recover */
		printk(KERN_ERR "kobject (%p): tried to init an initialized "
		       "object, something is seriously wrong.\n", kobj);
		dump_stack();
	}

	kobject_init_internal(kobj);
	/** 20150411
	 * kobj의 ktype을 지정한다.
	 **/
	kobj->ktype = ktype;
	return;

error:
	printk(KERN_ERR "kobject (%p): %s\n", kobj, err_str);
	dump_stack();
}
EXPORT_SYMBOL(kobject_init);

/** 20150418
 * parent에 주어진 이름을 가지는 kobject를 추가한다.
 *
 * kobject에 따라 특별한 이름이 존재할 수 있도록 포맷스트링으로 name을 받는다.
 *   ex) "pci0000:00"
 **/
static int kobject_add_varg(struct kobject *kobj, struct kobject *parent,
			    const char *fmt, va_list vargs)
{
	int retval;

	/** 20150411
	 * vargs를 fmt 형태로 파싱해 kobject의 name을 채운다.
	 **/
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	if (retval) {
		printk(KERN_ERR "kobject: can not set name properly!\n");
		return retval;
	}
	/** 20150411
	 * kobject의 parent를 지정하고, kobj를 위한 sysfs 디렉토리를 생성한다.
	 * parent는 NULL 지정이 가능하다.
	 **/
	kobj->parent = parent;
	return kobject_add_internal(kobj);
}

/**
 * kobject_add - the main kobject add function
 * @kobj: the kobject to add
 * @parent: pointer to the parent of the kobject.
 * @fmt: format to name the kobject with.
 *
 * The kobject name is set and added to the kobject hierarchy in this
 * function.
 *
 * If @parent is set, then the parent of the @kobj will be set to it.
 * If @parent is NULL, then the parent of the @kobj will be set to the
 * kobject associted with the kset assigned to this kobject.  If no kset
 * is assigned to the kobject, then the kobject will be located in the
 * root of the sysfs tree.
 *
 * If this function returns an error, kobject_put() must be called to
 * properly clean up the memory associated with the object.
 * Under no instance should the kobject that is passed to this function
 * be directly freed with a call to kfree(), that can leak memory.
 *
 * Note, no "add" uevent will be created with this call, the caller should set
 * up all of the necessary sysfs files for the object and then call
 * kobject_uevent() with the UEVENT_ADD parameter to ensure that
 * userspace is properly notified of this kobject's creation.
 */
/** 20150418
 * parent에 포맷스트링으로 생성된 이름을 갖는 kobject를 추가한다.
 * 
 * 추가된 kobject는 sysfs 내에 디렉토리와 파일이 추가된다.
 **/
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...)
{
	va_list args;
	int retval;

	if (!kobj)
		return -EINVAL;

	/** 20150411
	 * kobj는 초기화 되어 있어야 한다.
	 **/
	if (!kobj->state_initialized) {
		printk(KERN_ERR "kobject '%s' (%p): tried to add an "
		       "uninitialized object, something is seriously wrong.\n",
		       kobject_name(kobj), kobj);
		dump_stack();
		return -EINVAL;
	}
	/** 20150411
	 * 포맷스트링의 이름을 갖는 kobject를 parent에 추가한다.
	 * kobject에 대한 sysfs 디렉토리와 파일이 생성된다.
	 **/
	va_start(args, fmt);
	retval = kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);

	return retval;
}
EXPORT_SYMBOL(kobject_add);

/**
 * kobject_init_and_add - initialize a kobject structure and add it to the kobject hierarchy
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 * @parent: pointer to the parent of this kobject.
 * @fmt: the name of the kobject.
 *
 * This function combines the call to kobject_init() and
 * kobject_add().  The same type of error handling after a call to
 * kobject_add() and kobject lifetime rules are the same here.
 */
/** 20151121
 * kobj를 초기화 해 주어진 이름으로 parent에 추가.
 **/
int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype,
			 struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	int retval;

	/** 20151121
	 * kobj 초기화.
	 **/
	kobject_init(kobj, ktype);

	/** 20151121
	 * parent에 주어진 이름을 가지는 kobject 추가.
	 **/
	va_start(args, fmt);
	retval = kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);

	return retval;
}
EXPORT_SYMBOL_GPL(kobject_init_and_add);

/**
 * kobject_rename - change the name of an object
 * @kobj: object in question.
 * @new_name: object's new name
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of kobject_rename
 * on the same kobject and to ensure that new_name is valid and
 * won't conflict with other kobjects.
 */
int kobject_rename(struct kobject *kobj, const char *new_name)
{
	int error = 0;
	const char *devpath = NULL;
	const char *dup_name = NULL, *name;
	char *devpath_string = NULL;
	char *envp[2];

	kobj = kobject_get(kobj);
	if (!kobj)
		return -EINVAL;
	if (!kobj->parent)
		return -EINVAL;

	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		error = -ENOMEM;
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL);
	if (!devpath_string) {
		error = -ENOMEM;
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath);
	envp[0] = devpath_string;
	envp[1] = NULL;

	name = dup_name = kstrdup(new_name, GFP_KERNEL);
	if (!name) {
		error = -ENOMEM;
		goto out;
	}

	error = sysfs_rename_dir(kobj, new_name);
	if (error)
		goto out;

	/* Install the new kobject name */
	dup_name = kobj->name;
	kobj->name = name;

	/* This function is mostly/only used for network interface.
	 * Some hotplug package track interfaces by their name and
	 * therefore want to know when the name is changed by the user. */
	kobject_uevent_env(kobj, KOBJ_MOVE, envp);

out:
	kfree(dup_name);
	kfree(devpath_string);
	kfree(devpath);
	kobject_put(kobj);

	return error;
}
EXPORT_SYMBOL_GPL(kobject_rename);

/**
 * kobject_move - move object to another parent
 * @kobj: object in question.
 * @new_parent: object's new parent (can be NULL)
 */
int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
	int error;
	struct kobject *old_parent;
	const char *devpath = NULL;
	char *devpath_string = NULL;
	char *envp[2];

	kobj = kobject_get(kobj);
	if (!kobj)
		return -EINVAL;
	new_parent = kobject_get(new_parent);
	if (!new_parent) {
		if (kobj->kset)
			new_parent = kobject_get(&kobj->kset->kobj);
	}
	/* old object path */
	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		error = -ENOMEM;
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL);
	if (!devpath_string) {
		error = -ENOMEM;
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath);
	envp[0] = devpath_string;
	envp[1] = NULL;
	error = sysfs_move_dir(kobj, new_parent);
	if (error)
		goto out;
	old_parent = kobj->parent;
	kobj->parent = new_parent;
	new_parent = NULL;
	kobject_put(old_parent);
	kobject_uevent_env(kobj, KOBJ_MOVE, envp);
out:
	kobject_put(new_parent);
	kobject_put(kobj);
	kfree(devpath_string);
	kfree(devpath);
	return error;
}

/**
 * kobject_del - unlink kobject from hierarchy.
 * @kobj: object.
 */
/** 20150418
 * kobject를 kobject hiererachy에서 제거한다.
 **/
void kobject_del(struct kobject *kobj)
{
	if (!kobj)
		return;

	/** 20150418
	 * sysfs_dirent 상에서 kobj와 하위 attribute를 제거한다.
	 **/
	sysfs_remove_dir(kobj);
	kobj->state_in_sysfs = 0;
	/** 20150418
	 * kobj가 kset에 속해 있다면 kset에서 제거한다.
	 **/
	kobj_kset_leave(kobj);
	/** 20150418
	 * kobj의 parent의 reference count를 감소시킨다.
	 **/
	kobject_put(kobj->parent);
	kobj->parent = NULL;
}

/**
 * kobject_get - increment refcount for object.
 * @kobj: object.
 */
/** 20150411
 * 주어진 kobj의 kref를 증가시킨다.
 **/
struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj)
		kref_get(&kobj->kref);
	return kobj;
}

/*
 * kobject_cleanup - free kobject resources.
 * @kobj: object to cleanup
 */
/** 20150418
 * kobj와 관련된 자료구조와 사용한 자원(name)을 모두 정리한다.
 **/
static void kobject_cleanup(struct kobject *kobj)
{
	/** 20150418
	 * kobject의 ktype과 name을 받아온다.
	 **/
	struct kobj_type *t = get_ktype(kobj);
	const char *name = kobj->name;

	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);

	if (t && !t->release)
		pr_debug("kobject: '%s' (%p): does not have a release() "
			 "function, it is broken and must be fixed.\n",
			 kobject_name(kobj), kobj);

	/* send "remove" if the caller did not do it but sent "add" */
	/** 20150418
	 * state_add_uevent_sent로 설정되어 있고 아직 state_remove_uevent_sent가
	 * 설정되지 않은 상태라면
	 * kobject_uevent를 통해 KOBJ_REMOVE 이벤트를 보낸다.
	 **/
	if (kobj->state_add_uevent_sent && !kobj->state_remove_uevent_sent) {
		pr_debug("kobject: '%s' (%p): auto cleanup 'remove' event\n",
			 kobject_name(kobj), kobj);
		kobject_uevent(kobj, KOBJ_REMOVE);
	}

	/* remove from sysfs if the caller did not do it */
	/** 20150418
	 * kobject가 sysfs에 있으면 해당 kobject를 제거한다.
	 **/
	if (kobj->state_in_sysfs) {
		pr_debug("kobject: '%s' (%p): auto cleanup kobject_del\n",
			 kobject_name(kobj), kobj);
		kobject_del(kobj);
	}

	/** 20150418
	 * ktype이 존재하고 release 처리 함수가 존재하면 호출한다.
	 *
	 * ex) driver_ktype의 .release
	 *     device_ktype의 .release
	 **/
	if (t && t->release) {
		pr_debug("kobject: '%s' (%p): calling ktype release\n",
			 kobject_name(kobj), kobj);
		t->release(kobj);
	}

	/* free name if we allocated it */
	/** 20150411
	 * kobject_set_name_vargs 에서 동적 할당 받은 name을 해제한다.
	 **/
	if (name) {
		pr_debug("kobject: '%s': free name\n", name);
		kfree(name);
	}
}

/** 20150418
 * 주어진 kref를 가지는 kobject를 kobject_cleanup 함수로 해제한다.
 *
 * kobject가 reference count는 kref라는 내부 구조체로 관리하므로
 * 이런 구조를 가진다.
 **/
static void kobject_release(struct kref *kref)
{
	kobject_cleanup(container_of(kref, struct kobject, kref));
}

/**
 * kobject_put - decrement refcount for object.
 * @kobj: object.
 *
 * Decrement the refcount, and if 0, call kobject_cleanup().
 */
/** 20150418
 * kobject의 kref를 감소시키고, 그 결과 0이라면 kobject_release를 사용해
 * kobject를 cleanup시킨다.
 **/
void kobject_put(struct kobject *kobj)
{
	if (kobj) {
		if (!kobj->state_initialized)
			WARN(1, KERN_WARNING "kobject: '%s' (%p): is not "
			       "initialized, yet kobject_put() is being "
			       "called.\n", kobject_name(kobj), kobj);
		/** 20150418
		 * kref의 reference count를 감소시키고, 그 결과 0이라면
		 * kobject_release를 사용해 kobject를 해제한다.
		 **/
		kref_put(&kobj->kref, kobject_release);
	}
}

static void dynamic_kobj_release(struct kobject *kobj)
{
	pr_debug("kobject: (%p): %s\n", kobj, __func__);
	kfree(kobj);
}

/** 20150411
 **/
static struct kobj_type dynamic_kobj_ktype = {
	.release	= dynamic_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
};

/**
 * kobject_create - create a struct kobject dynamically
 *
 * This function creates a kobject structure dynamically and sets it up
 * to be a "dynamic" kobject with a default release function set up.
 *
 * If the kobject was not able to be created, NULL will be returned.
 * The kobject structure returned from here must be cleaned up with a
 * call to kobject_put() and not kfree(), as kobject_init() has
 * already been called on this structure.
 */
/** 20150411
 * kobject를 할당받고 초기화해 리턴한다.
 **/
struct kobject *kobject_create(void)
{
	struct kobject *kobj;

	/** 20150411
	 * kobject를 할당받는다.
	 **/
	kobj = kzalloc(sizeof(*kobj), GFP_KERNEL);
	if (!kobj)
		return NULL;

	kobject_init(kobj, &dynamic_kobj_ktype);
	return kobj;
}

/**
 * kobject_create_and_add - create a struct kobject dynamically and register it with sysfs
 *
 * @name: the name for the kobject
 * @parent: the parent kobject of this kobject, if any.
 *
 * This function creates a kobject structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kobject_put() and the structure will be dynamically freed when
 * it is no longer being used.
 *
 * If the kobject was not able to be created, NULL will be returned.
 */
/** 20150418
 * parent에 name이름을 갖는 새로운 kobject를 할당받아 추가한다.
 *
 * parnet가 NULL로 지정되면 최상단에 생성된다. ("/sys/{name}")
 **/
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj;
	int retval;

	/** 20150411
	 * kobject 할당 및 초기화.
	 **/
	kobj = kobject_create();
	if (!kobj)
		return NULL;

	/** 20150418
	 * 생성 및 초기화된 kobj를 parent 아래 kobject hierarchy에 name으로 추가한다.
	 * sysfs에 디렉토리 및 파일도 추가된다.
	 **/
	retval = kobject_add(kobj, parent, "%s", name);
	if (retval) {
		printk(KERN_WARNING "%s: kobject_add error: %d\n",
		       __func__, retval);
		kobject_put(kobj);
		kobj = NULL;
	}
	return kobj;
}
EXPORT_SYMBOL_GPL(kobject_create_and_add);

/**
 * kset_init - initialize a kset for use
 * @k: kset
 */
/** 20150829
 * kset의 각 멤버를 초기화 한다.
 **/
void kset_init(struct kset *k)
{
	kobject_init_internal(&k->kobj);
	INIT_LIST_HEAD(&k->list);
	spin_lock_init(&k->list_lock);
}

/* default kobject attribute operations */
static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->show)
		ret = kattr->show(kobj, kattr, buf);
	return ret;
}

static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->store)
		ret = kattr->store(kobj, kattr, buf, count);
	return ret;
}

/** 20150829
 *
 **/
const struct sysfs_ops kobj_sysfs_ops = {
	.show	= kobj_attr_show,
	.store	= kobj_attr_store,
};

/**
 * kset_register - initialize and add a kset.
 * @k: kset.
 */
/** 20150829
 * kset을 초기화 하고, kset 내의 kobject를 hierarchy에 등록한다.
 * 등록 후 추가에 대한 uevent를 호출한다.
 **/
int kset_register(struct kset *k)
{
	int err;

	if (!k)
		return -EINVAL;

	/** 20150829
	 * kset을 등록하기 전 초기화 한다.
	 * kset 내부의 kobject를 초기화 한다.
	 **/
	kset_init(k);
	/** 20150829
	 * kset에 해당하는 kobject를 추가한다. (sys 상의 디렉토리 생성 포함)
	 **/
	err = kobject_add_internal(&k->kobj);
	if (err)
		return err;
	/** 20150829
	 * kset의 kobj에 KOBJ_ADD가 발생했다는 uevent를 보낸다.
	 **/
	kobject_uevent(&k->kobj, KOBJ_ADD);
	return 0;
}

/**
 * kset_unregister - remove a kset.
 * @k: kset.
 */
void kset_unregister(struct kset *k)
{
	if (!k)
		return;
	kobject_put(&k->kobj);
}

/**
 * kset_find_obj - search for object in kset.
 * @kset: kset we're looking in.
 * @name: object's name.
 *
 * Lock kset via @kset->subsys, and iterate over @kset->list,
 * looking for a matching kobject. If matching object is found
 * take a reference and return the object.
 */
/** 20151121
 * kset에서 name이라는 이름을 지닌 kobject를 찾아 리턴한다.
 **/
struct kobject *kset_find_obj(struct kset *kset, const char *name)
{
	struct kobject *k;
	struct kobject *ret = NULL;

	/** 20151121
	 * kset list 접근은 spinlock으로 보호된다.
	 **/
	spin_lock(&kset->list_lock);

	/** 20151121
	 * kset (list)에 등록된 kobject를 순회하며 전달된 이름과 같은 object를 찾는다.
	 **/
	list_for_each_entry(k, &kset->list, entry) {
		if (kobject_name(k) && !strcmp(kobject_name(k), name)) {
			ret = kobject_get(k);
			break;
		}
	}

	spin_unlock(&kset->list_lock);
	return ret;
}

static void kset_release(struct kobject *kobj)
{
	struct kset *kset = container_of(kobj, struct kset, kobj);
	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);
	kfree(kset);
}

/** 20150829
 * kset에 해당하는 ktype 정의.
 **/
static struct kobj_type kset_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.release = kset_release,
};

/**
 * kset_create - create a struct kset dynamically
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically.  This structure can
 * then be registered with the system and show up in sysfs with a call to
 * kset_register().  When you are finished with this structure, if
 * kset_register() has been called, call kset_unregister() and the
 * structure will be dynamically freed when it is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/** 20150829
 * kset을 동적 할당받아 내부 kobject와 관련 자료구조를 초기화 한다.
 **/
static struct kset *kset_create(const char *name,
				const struct kset_uevent_ops *uevent_ops,
				struct kobject *parent_kobj)
{
	struct kset *kset;
	int retval;

	/** 20150829
	 * kset으로 사용할 메모리를 동적 할당 받는다.
	 **/
	kset = kzalloc(sizeof(*kset), GFP_KERNEL);
	if (!kset)
		return NULL;
	/** 20150829
	 * kset의 이름은 kset내의 kobject에 저장한다.
	 **/
	retval = kobject_set_name(&kset->kobj, name);
	if (retval) {
		kfree(kset);
		return NULL;
	}
	/** 20150829
	 * kset의 uevent_ops를 채우고,
	 * 넘어온 parent를 kset의 kobject의 parent로 지정한다. (NULL도 포함)
	 **/
	kset->uevent_ops = uevent_ops;
	kset->kobj.parent = parent_kobj;

	/*
	 * The kobject of this kset will have a type of kset_ktype and belong to
	 * no kset itself.  That way we can properly free it when it is
	 * finished being used.
	 */
	/** 20150829
	 * kset의 kobject의 ktype은 kset_ktype으로 지정.
	 **/
	kset->kobj.ktype = &kset_ktype;
	kset->kobj.kset = NULL;

	return kset;
}

/**
 * kset_create_and_add - create a struct kset dynamically and add it to sysfs
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kset_unregister() and the structure will be dynamically freed when it
 * is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/** 20150829
 * name이라는 kset을 동적 생성하고, 내부 자료구조체 추가하고 리턴한다.
 * parent_kobj 아래에 추가된다.
 **/
struct kset *kset_create_and_add(const char *name,
				 const struct kset_uevent_ops *uevent_ops,
				 struct kobject *parent_kobj)
{
	struct kset *kset;
	int error;

	/** 20150829
	 * name이라는 kset을 동적 할당 받고 매개변수로 멤버를 채운다.
	 **/
	kset = kset_create(name, uevent_ops, parent_kobj);
	if (!kset)
		return NULL;
	/** 20150829
	 * kset을 내부 hierarchy에 등록하고 uevent를 보낸다.
	 **/
	error = kset_register(kset);
	if (error) {
		kfree(kset);
		return NULL;
	}
	return kset;
}
EXPORT_SYMBOL_GPL(kset_create_and_add);


static DEFINE_SPINLOCK(kobj_ns_type_lock);
static const struct kobj_ns_type_operations *kobj_ns_ops_tbl[KOBJ_NS_TYPES];

int kobj_ns_type_register(const struct kobj_ns_type_operations *ops)
{
	enum kobj_ns_type type = ops->type;
	int error;

	spin_lock(&kobj_ns_type_lock);

	error = -EINVAL;
	if (type >= KOBJ_NS_TYPES)
		goto out;

	error = -EINVAL;
	if (type <= KOBJ_NS_TYPE_NONE)
		goto out;

	error = -EBUSY;
	if (kobj_ns_ops_tbl[type])
		goto out;

	error = 0;
	kobj_ns_ops_tbl[type] = ops;

out:
	spin_unlock(&kobj_ns_type_lock);
	return error;
}

int kobj_ns_type_registered(enum kobj_ns_type type)
{
	int registered = 0;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES))
		registered = kobj_ns_ops_tbl[type] != NULL;
	spin_unlock(&kobj_ns_type_lock);

	return registered;
}

/** 20150411
 * parent의 child_ns_type을 가져온다.
 **/
const struct kobj_ns_type_operations *kobj_child_ns_ops(struct kobject *parent)
{
	const struct kobj_ns_type_operations *ops = NULL;

	/** 20150411
	 * parent의 ktype에서 child_ns_type을 받아온다.
	 **/
	if (parent && parent->ktype->child_ns_type)
		ops = parent->ktype->child_ns_type(parent);

	return ops;
}

const struct kobj_ns_type_operations *kobj_ns_ops(struct kobject *kobj)
{
	return kobj_child_ns_ops(kobj->parent);
}


/** 20150307
 * 현재 type에 해당하는 kobj ns의 grab 콜백을 호출한다.
 **/
void *kobj_ns_grab_current(enum kobj_ns_type type)
{
	void *ns = NULL;

	/** 20150307
	 * kobj_ns_type은 spinlock으로 보호 받는다.
	 **/
	spin_lock(&kobj_ns_type_lock);
	/** 20150307
	 * type에 해당하는 ops table의 grab_current_ns를 호출한다.
	 **/
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->grab_current_ns();
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

const void *kobj_ns_netlink(enum kobj_ns_type type, struct sock *sk)
{
	const void *ns = NULL;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->netlink_ns(sk);
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

const void *kobj_ns_initial(enum kobj_ns_type type)
{
	const void *ns = NULL;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->initial_ns();
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

void kobj_ns_drop(enum kobj_ns_type type, void *ns)
{
	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type] && kobj_ns_ops_tbl[type]->drop_ns)
		kobj_ns_ops_tbl[type]->drop_ns(ns);
	spin_unlock(&kobj_ns_type_lock);
}

EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_del);

EXPORT_SYMBOL(kset_register);
EXPORT_SYMBOL(kset_unregister);
