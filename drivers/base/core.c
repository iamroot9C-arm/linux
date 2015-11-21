/*
 * drivers/base/core.c - core driver model code (device registration, etc)
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2006 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2006 Novell, Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/genhd.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>
#include <linux/netdevice.h>

#include "base.h"
#include "power/power.h"

#ifdef CONFIG_SYSFS_DEPRECATED
#ifdef CONFIG_SYSFS_DEPRECATED_V2
long sysfs_deprecated = 1;
#else
/** 20151017    
 * sysfs는 deprecated 되지 않았다.
 **/
long sysfs_deprecated = 0;
#endif
static __init int sysfs_deprecated_setup(char *arg)
{
	return strict_strtol(arg, 10, &sysfs_deprecated);
}
early_param("sysfs.deprecated", sysfs_deprecated_setup);
#endif

/** 20150829    
 * device 추가시 platform에 notify를 주기 위한 콜백을 등록한다.
 **/
int (*platform_notify)(struct device *dev) = NULL;
int (*platform_notify_remove)(struct device *dev) = NULL;
static struct kobject *dev_kobj;
/** 20150905    
 * devices_init에서 "/sys/dev/block", "/sys/dev/char"에 대한 kobject.
 **/
struct kobject *sysfs_dev_char_kobj;
struct kobject *sysfs_dev_block_kobj;

#ifdef CONFIG_BLOCK
/** 20150905    
 * device type이 part_type이 아니면 partition되어 있지 않은 디바이스.
 **/
static inline int device_is_not_partition(struct device *dev)
{
	return !(dev->type == &part_type);
}
#else
static inline int device_is_not_partition(struct device *dev)
{
	return 1;
}
#endif

/**
 * dev_driver_string - Return a device's driver name, if at all possible
 * @dev: struct device to get the name of
 *
 * Will return the device's driver's name if it is bound to a device.  If
 * the device is not bound to a driver, it will return the name of the bus
 * it is attached to.  If it is not attached to a bus either, an empty
 * string will be returned.
 */
const char *dev_driver_string(const struct device *dev)
{
	struct device_driver *drv;

	/* dev->driver can change to NULL underneath us because of unbinding,
	 * so be careful about accessing it.  dev->bus and dev->class should
	 * never change once they are set, so they don't need special care.
	 */
	drv = ACCESS_ONCE(dev->driver);
	return drv ? drv->name :
			(dev->bus ? dev->bus->name :
			(dev->class ? dev->class->name : ""));
}
EXPORT_SYMBOL(dev_driver_string);

#define to_dev_attr(_attr) container_of(_attr, struct device_attribute, attr)

/** 20150829    
 * 디바이스 등록시 지정한 attribute show 콜백 함수를 호출한다.
 **/
static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = kobj_to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->show)
		ret = dev_attr->show(dev, dev_attr, buf);
	if (ret >= (ssize_t)PAGE_SIZE) {
		print_symbol("dev_attr_show: %s returned bad count\n",
				(unsigned long)dev_attr->show);
	}
	return ret;
}

static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = kobj_to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->store)
		ret = dev_attr->store(dev, dev_attr, buf, count);
	return ret;
}

/** 20150829    
 * dev의 sysfs 콜백함수 구조체.
 **/
static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,
	.store	= dev_attr_store,
};

#define to_ext_attr(x) container_of(x, struct dev_ext_attribute, attr)

ssize_t device_store_ulong(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	char *end;
	unsigned long new = simple_strtoul(buf, &end, 0);
	if (end == buf)
		return -EINVAL;
	*(unsigned long *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_ulong);

ssize_t device_show_ulong(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	return snprintf(buf, PAGE_SIZE, "%lx\n", *(unsigned long *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_ulong);

ssize_t device_store_int(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	char *end;
	long new = simple_strtol(buf, &end, 0);
	if (end == buf || new > INT_MAX || new < INT_MIN)
		return -EINVAL;
	*(int *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_int);

ssize_t device_show_int(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	return snprintf(buf, PAGE_SIZE, "%d\n", *(int *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_int);

/**
 *	device_release - free device structure.
 *	@kobj:	device's kobject.
 *
 *	This is called once the reference count for the object
 *	reaches 0. We forward the call to the device's release
 *	method, which should handle actually freeing the structure.
 */
static void device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct device_private *p = dev->p;

	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
	else
		WARN(1, KERN_ERR "Device '%s' does not have a release() "
			"function, it is broken and must be fixed.\n",
			dev_name(dev));
	kfree(p);
}

static const void *device_namespace(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	const void *ns = NULL;

	if (dev->class && dev->class->ns_type)
		ns = dev->class->namespace(dev);

	return ns;
}

/** 20150829    
 * device ktype.
 *
 * .sysfs_ops
 **/
static struct kobj_type device_ktype = {
	.release	= device_release,
	.sysfs_ops	= &dev_sysfs_ops,
	.namespace	= device_namespace,
};


static int dev_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);

	if (ktype == &device_ktype) {
		struct device *dev = kobj_to_dev(kobj);
		if (dev->bus)
			return 1;
		if (dev->class)
			return 1;
	}
	return 0;
}

static const char *dev_uevent_name(struct kset *kset, struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);

	if (dev->bus)
		return dev->bus->name;
	if (dev->class)
		return dev->class->name;
	return NULL;
}

/** 20150829    
 * "devices" kset의 ops에 해당하는 uevent 함수.
 * 자세한 내용은 추후분석???
 **/
static int dev_uevent(struct kset *kset, struct kobject *kobj,
		      struct kobj_uevent_env *env)
{
	struct device *dev = kobj_to_dev(kobj);
	int retval = 0;

	/* add device node properties if present */
	if (MAJOR(dev->devt)) {
		const char *tmp;
		const char *name;
		umode_t mode = 0;

		add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));
		add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));
		name = device_get_devnode(dev, &mode, &tmp);
		if (name) {
			add_uevent_var(env, "DEVNAME=%s", name);
			kfree(tmp);
			if (mode)
				add_uevent_var(env, "DEVMODE=%#o", mode & 0777);
		}
	}

	if (dev->type && dev->type->name)
		add_uevent_var(env, "DEVTYPE=%s", dev->type->name);

	if (dev->driver)
		add_uevent_var(env, "DRIVER=%s", dev->driver->name);

	/* Add common DT information about the device */
	of_device_uevent(dev, env);

	/* have the bus specific function add its stuff */
	if (dev->bus && dev->bus->uevent) {
		retval = dev->bus->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: bus uevent() returned %d\n",
				 dev_name(dev), __func__, retval);
	}

	/* have the class specific function add its stuff */
	if (dev->class && dev->class->dev_uevent) {
		retval = dev->class->dev_uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: class uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	/* have the device type specific function add its stuff */
	if (dev->type && dev->type->uevent) {
		retval = dev->type->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: dev_type uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	return retval;
}

static const struct kset_uevent_ops device_uevent_ops = {
	.filter =	dev_uevent_filter,
	.name =		dev_uevent_name,
	.uevent =	dev_uevent,
};

static ssize_t show_uevent(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct kobject *top_kobj;
	struct kset *kset;
	struct kobj_uevent_env *env = NULL;
	int i;
	size_t count = 0;
	int retval;

	/* search the kset, the device belongs to */
	top_kobj = &dev->kobj;
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;
	if (!top_kobj->kset)
		goto out;

	kset = top_kobj->kset;
	if (!kset->uevent_ops || !kset->uevent_ops->uevent)
		goto out;

	/* respect filter */
	if (kset->uevent_ops && kset->uevent_ops->filter)
		if (!kset->uevent_ops->filter(kset, &dev->kobj))
			goto out;

	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;

	/* let the kset specific function add its keys */
	retval = kset->uevent_ops->uevent(kset, &dev->kobj, env);
	if (retval)
		goto out;

	/* copy keys to file */
	for (i = 0; i < env->envp_idx; i++)
		count += sprintf(&buf[count], "%s\n", env->envp[i]);
out:
	kfree(env);
	return count;
}

static ssize_t store_uevent(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	enum kobject_action action;

	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&dev->kobj, action);
	else
		dev_err(dev, "uevent: unknown action-string\n");
	return count;
}

/** 20150829    
 * device의 sysfs에서 uevent 파일에 대한 attribute.
 **/
static struct device_attribute uevent_attr =
	__ATTR(uevent, S_IRUGO | S_IWUSR, show_uevent, store_uevent);

/** 20150905    
 * device에 attribute를 추가한다.
 * sysfs에서 각 device_attribute가 파일로 생성된다.
 **/
static int device_add_attributes(struct device *dev,
				 struct device_attribute *attrs)
{
	int error = 0;
	int i;

	/** 20150905    
	 * 속성 이름이 NULL이 아닐 때까지 device에
	 * attribute들을 각각 sysfs 파일로 생성한다.
	 **/
	if (attrs) {
		for (i = 0; attr_name(attrs[i]); i++) {
			error = device_create_file(dev, &attrs[i]);
			if (error)
				break;
		}
		if (error)
			while (--i >= 0)
				device_remove_file(dev, &attrs[i]);
	}
	return error;
}

static void device_remove_attributes(struct device *dev,
				     struct device_attribute *attrs)
{
	int i;

	if (attrs)
		for (i = 0; attr_name(attrs[i]); i++)
			device_remove_file(dev, &attrs[i]);
}

/** 20150905    
 * device에 bin attribute를 추가한다.
 * sysfs에서 각 device_attribute가 BIN 파일로 생성된다.
 **/
static int device_add_bin_attributes(struct device *dev,
				     struct bin_attribute *attrs)
{
	int error = 0;
	int i;

	/** 20150905    
	 * 속성 이름이 NULL이 아닐 때까지 device에
	 * attribute들을 각각 sysfs BIN 파일로 생성한다.
	 **/
	if (attrs) {
		for (i = 0; attr_name(attrs[i]); i++) {
			error = device_create_bin_file(dev, &attrs[i]);
			if (error)
				break;
		}
		if (error)
			while (--i >= 0)
				device_remove_bin_file(dev, &attrs[i]);
	}
	return error;
}

static void device_remove_bin_attributes(struct device *dev,
					 struct bin_attribute *attrs)
{
	int i;

	if (attrs)
		for (i = 0; attr_name(attrs[i]); i++)
			device_remove_bin_file(dev, &attrs[i]);
}

/** 20150905    
 * 디바이스 파일에 attribute 그룹을 추가한다.
 **/
static int device_add_groups(struct device *dev,
			     const struct attribute_group **groups)
{
	int error = 0;
	int i;

	/** 20150905    
	 * 각 attribute 그룹들을 순회하며 디렉토리 kobject에 추가한다.
	 **/
	if (groups) {
		for (i = 0; groups[i]; i++) {
			error = sysfs_create_group(&dev->kobj, groups[i]);
			if (error) {
				while (--i >= 0)
					sysfs_remove_group(&dev->kobj,
							   groups[i]);
				break;
			}
		}
	}
	return error;
}

static void device_remove_groups(struct device *dev,
				 const struct attribute_group **groups)
{
	int i;

	if (groups)
		for (i = 0; groups[i]; i++)
			sysfs_remove_group(&dev->kobj, groups[i]);
}

/** 20150905    
 * device에 attribute 들을 추가한다.
 * class와 type의 attribute들과 device의 attribute가 sysfs dirent로 생성된다.
 **/
static int device_add_attrs(struct device *dev)
{
	struct class *class = dev->class;
	const struct device_type *type = dev->type;
	int error;

	/** 20150905    
	 * class가 존재하면 device에 class용 attributes를 추가한다.
	 * sysfs에서 attirubte가 파일로 추가된다.
	 **/
	if (class) {
		error = device_add_attributes(dev, class->dev_attrs);
		if (error)
			return error;
		error = device_add_bin_attributes(dev, class->dev_bin_attrs);
		if (error)
			goto err_remove_class_attrs;
	}

	/** 20150905    
	 * type이 지정되어 있으면 type의 attribute 그룹들을 추가한다.
	 **/
	if (type) {
		error = device_add_groups(dev, type->groups);
		if (error)
			goto err_remove_class_bin_attrs;
	}

	/** 20150905    
	 * 디바이스에 지정된 attribute 그룹들을 추가한다.
	 **/
	error = device_add_groups(dev, dev->groups);
	if (error)
		goto err_remove_type_groups;

	return 0;

 err_remove_type_groups:
	if (type)
		device_remove_groups(dev, type->groups);
 err_remove_class_bin_attrs:
	if (class)
		device_remove_bin_attributes(dev, class->dev_bin_attrs);
 err_remove_class_attrs:
	if (class)
		device_remove_attributes(dev, class->dev_attrs);

	return error;
}

static void device_remove_attrs(struct device *dev)
{
	struct class *class = dev->class;
	const struct device_type *type = dev->type;

	device_remove_groups(dev, dev->groups);

	if (type)
		device_remove_groups(dev, type->groups);

	if (class) {
		device_remove_attributes(dev, class->dev_attrs);
		device_remove_bin_attributes(dev, class->dev_bin_attrs);
	}
}


static ssize_t show_dev(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return print_dev_t(buf, dev->devt);
}

/** 20150905    
 * device를 위한 device_attribute를 생성한다.
 **/
static struct device_attribute devt_attr =
	__ATTR(dev, S_IRUGO, show_dev, NULL);

/* /sys/devices/ */
/** 20150829    
 * "devices" kset.
 * devices_init() 함수에서 kset을 생성해 저정한다.
 **/
struct kset *devices_kset;

/**
 * device_create_file - create sysfs attribute file for device.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
/** 20150829    
 * sysfs에 device를 위한 attribute 파일을 생성한다.
 **/
int device_create_file(struct device *dev,
		       const struct device_attribute *attr)
{
	int error = 0;
	if (dev)
		error = sysfs_create_file(&dev->kobj, &attr->attr);
	return error;
}

/**
 * device_remove_file - remove sysfs attribute file.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
void device_remove_file(struct device *dev,
			const struct device_attribute *attr)
{
	if (dev)
		sysfs_remove_file(&dev->kobj, &attr->attr);
}

/**
 * device_create_bin_file - create sysfs binary attribute file for device.
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
/** 20150905    
 * device에 sysfs bin 속성파일을 생성한다.
 **/
int device_create_bin_file(struct device *dev,
			   const struct bin_attribute *attr)
{
	int error = -EINVAL;
	if (dev)
		error = sysfs_create_bin_file(&dev->kobj, attr);
	return error;
}
EXPORT_SYMBOL_GPL(device_create_bin_file);

/**
 * device_remove_bin_file - remove sysfs binary attribute file
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
void device_remove_bin_file(struct device *dev,
			    const struct bin_attribute *attr)
{
	if (dev)
		sysfs_remove_bin_file(&dev->kobj, attr);
}
EXPORT_SYMBOL_GPL(device_remove_bin_file);

/**
 * device_schedule_callback_owner - helper to schedule a callback for a device
 * @dev: device.
 * @func: callback function to invoke later.
 * @owner: module owning the callback routine
 *
 * Attribute methods must not unregister themselves or their parent device
 * (which would amount to the same thing).  Attempts to do so will deadlock,
 * since unregistration is mutually exclusive with driver callbacks.
 *
 * Instead methods can call this routine, which will attempt to allocate
 * and schedule a workqueue request to call back @func with @dev as its
 * argument in the workqueue's process context.  @dev will be pinned until
 * @func returns.
 *
 * This routine is usually called via the inline device_schedule_callback(),
 * which automatically sets @owner to THIS_MODULE.
 *
 * Returns 0 if the request was submitted, -ENOMEM if storage could not
 * be allocated, -ENODEV if a reference to @owner isn't available.
 *
 * NOTE: This routine won't work if CONFIG_SYSFS isn't set!  It uses an
 * underlying sysfs routine (since it is intended for use by attribute
 * methods), and if sysfs isn't available you'll get nothing but -ENOSYS.
 */
int device_schedule_callback_owner(struct device *dev,
		void (*func)(struct device *), struct module *owner)
{
	return sysfs_schedule_callback(&dev->kobj,
			(void (*)(void *)) func, dev, owner);
}
EXPORT_SYMBOL_GPL(device_schedule_callback_owner);

/** 20150829    
 * children klist_node로 children을 가져오고,
 * 그 children의 device의 reference count를 증가시킨다.
 **/
static void klist_children_get(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);
	struct device *dev = p->device;

	get_device(dev);
}

/** 20150829    
 * children klist_node로 children을 가져오고,
 * 그 children의 device의 reference count를 감소시킨다.
 **/
static void klist_children_put(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);
	struct device *dev = p->device;

	put_device(dev);
}

/**
 * device_initialize - init device structure.
 * @dev: device.
 *
 * This prepares the device for use by other layers by initializing
 * its fields.
 * It is the first half of device_register(), if called by
 * that function, though it can also be called separately, so one
 * may use @dev's fields. In particular, get_device()/put_device()
 * may be used for reference counting of @dev after calling this
 * function.
 *
 * All fields in @dev must be initialized by the caller to 0, except
 * for those explicitly set to some other value.  The simplest
 * approach is to use kzalloc() to allocate the structure containing
 * @dev.
 *
 * NOTE: Use put_device() to give up your reference instead of freeing
 * @dev directly once you have called this function.
 */
/** 20150829    
 * device 구조체를 초기화 한다.
 * - kobject 초기화 (device에 해당하는 kset과 ktype 지정)
 * - 자료구조 초기화
 **/
void device_initialize(struct device *dev)
{
	/** 20150829    
	 * device kobject는 devices_kset kset 내에 등록한다.
	 **/
	dev->kobj.kset = devices_kset;
	/** 20150829    
	 * device_ktype로 kobject를 추가한다.
	 **/
	kobject_init(&dev->kobj, &device_ktype);
	INIT_LIST_HEAD(&dev->dma_pools);
	mutex_init(&dev->mutex);
	lockdep_set_novalidate_class(&dev->mutex);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
	device_pm_init(dev);
	set_dev_node(dev, -1);
}

/** 20150829    
 * device에 상관없이 virtual device에 해당하는 "virtual"이 리턴된다.
 **/
static struct kobject *virtual_device_parent(struct device *dev)
{
	static struct kobject *virtual_dir = NULL;

	/** 20150829    
	 * virtual_dir이 지정되어 않았다면 "virtual" kobject를 생성해 devices 아래 추가한다.
	 * "/sys/devices/virtual"
	 **/
	if (!virtual_dir)
		virtual_dir = kobject_create_and_add("virtual",
						     &devices_kset->kobj);

	return virtual_dir;
}

struct class_dir {
	struct kobject kobj;
	struct class *class;
};

#define to_class_dir(obj) container_of(obj, struct class_dir, kobj)

static void class_dir_release(struct kobject *kobj)
{
	struct class_dir *dir = to_class_dir(kobj);
	kfree(dir);
}

static const
struct kobj_ns_type_operations *class_dir_child_ns_type(struct kobject *kobj)
{
	struct class_dir *dir = to_class_dir(kobj);
	return dir->class->ns_type;
}

static struct kobj_type class_dir_ktype = {
	.release	= class_dir_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.child_ns_type	= class_dir_child_ns_type
};

/** 20150829    
 * device를 class hierarchy에 추가한다.
 **/
static struct kobject *
class_dir_create_and_add(struct class *class, struct kobject *parent_kobj)
{
	struct class_dir *dir;
	int retval;

	dir = kzalloc(sizeof(*dir), GFP_KERNEL);
	if (!dir)
		return NULL;

	dir->class = class;
	kobject_init(&dir->kobj, &class_dir_ktype);

	dir->kobj.kset = &class->p->glue_dirs;

	retval = kobject_add(&dir->kobj, parent_kobj, "%s", class->name);
	if (retval < 0) {
		kobject_put(&dir->kobj);
		return NULL;
	}
	return &dir->kobj;
}


/** 20150829    
 * device의 parent 디바이스의 kobject를 리턴한다.
 *
 * class device인지 먼저 판단해 처리한다.
 **/
static struct kobject *get_device_parent(struct device *dev,
					 struct device *parent)
{
	if (dev->class) {
		static DEFINE_MUTEX(gdp_mutex);
		struct kobject *kobj = NULL;
		struct kobject *parent_kobj;
		struct kobject *k;

#ifdef CONFIG_BLOCK
		/* block disks show up in /sys/block */
		if (sysfs_deprecated && dev->class == &block_class) {
			if (parent && parent->class == &block_class)
				return &parent->kobj;
			return &block_class.p->subsys.kobj;
		}
#endif

		/*
		 * If we have no parent, we live in "virtual".
		 * Class-devices with a non class-device as parent, live
		 * in a "glue" directory to prevent namespace collisions.
		 */
		/** 20150829    
		 * parent가 지정되지 않았다면 virtual 디바이스. 예를 들어 cpuid.
		 * parent_kobj로 virtual root에 해당하는 "virtual"이 지정된다.
		 *
		 * parent의 class가 정의되어 있고 ns_type이 지정되지 않았다면
		 * parent의 kobj를 리턴한다.
		 *
		 * class devices가 parent로 non-class 디바이스를 가진다면
		 * namespace 충돌을 방지하기 위해 "glue" 디렉토리를 사용한다.
		 **/
		if (parent == NULL)
			parent_kobj = virtual_device_parent(dev);
		else if (parent->class && !dev->class->ns_type)
			return &parent->kobj;
		else
			parent_kobj = &parent->kobj;

		mutex_lock(&gdp_mutex);

		/* find our class-directory at the parent and reference it */
		spin_lock(&dev->class->p->glue_dirs.list_lock);
		/** 20150829    
		 * device가 속한 class의 subsystem의 glue_dirs를 순회하며
		 * parent_kobj를 parent로 가지는 entry를 찾아 kobj로 삼는다.
		 **/
		list_for_each_entry(k, &dev->class->p->glue_dirs.list, entry)
			if (k->parent == parent_kobj) {
				kobj = kobject_get(k);
				break;
			}
		spin_unlock(&dev->class->p->glue_dirs.list_lock);
		if (kobj) {
			mutex_unlock(&gdp_mutex);
			return kobj;
		}

		/* or create a new class-directory at the parent device */
		k = class_dir_create_and_add(dev->class, parent_kobj);
		/* do not emit an uevent for this simple "glue" directory */
		mutex_unlock(&gdp_mutex);
		return k;
	}

	/* subsystems can specify a default root directory for their devices */
	if (!parent && dev->bus && dev->bus->dev_root)
		return &dev->bus->dev_root->kobj;

	if (parent)
		return &parent->kobj;
	return NULL;
}

static void cleanup_glue_dir(struct device *dev, struct kobject *glue_dir)
{
	/* see if we live in a "glue" directory */
	if (!glue_dir || !dev->class ||
	    glue_dir->kset != &dev->class->p->glue_dirs)
		return;

	kobject_put(glue_dir);
}

static void cleanup_device_parent(struct device *dev)
{
	cleanup_glue_dir(dev, dev->kobj.parent);
}

/** 20150905    
 * 새 디바이스의 class 관련 심볼릭 링크를 생성한다.
 *
 * sysfs 상에서 subsystem과 서로 심볼릭 링크로 가리킨다.
 **/
static int device_add_class_symlinks(struct device *dev)
{
	int error;

	if (!dev->class)
		return 0;

	/** 20150905    
	 * device 디렉토리 아래에 class의 "subsystem"에 대한 링크를 만든다.
	 *
	 * 즉, sysfs에서 이 디바이스가 어떤 클래스의 subsystem에 속하는지 알 수 있다.
	 **/
	error = sysfs_create_link(&dev->kobj,
				  &dev->class->p->subsys.kobj,
				  "subsystem");
	if (error)
		goto out;

	/** 20150905    
	 * parent가 존재하고,
	 * partition 디바이스가 아닐 경우 "device"에 대한 링크를 만든다.
	 **/
	if (dev->parent && device_is_not_partition(dev)) {
		error = sysfs_create_link(&dev->kobj, &dev->parent->kobj,
					  "device");
		if (error)
			goto out_subsys;
	}

#ifdef CONFIG_BLOCK
	/* /sys/block has directories and does not need symlinks */
	/** 20150905    
	 * sysfs_deprecated이고 block class인 경우 리턴.
	 **/
	if (sysfs_deprecated && dev->class == &block_class)
		return 0;
#endif

	/* link in the class directory pointing to the device */
	/** 20150905    
	 * 디바이스가 속한 subsystem에 지정된 디바이스에 대한 링크를 걸어준다.
	 **/
	error = sysfs_create_link(&dev->class->p->subsys.kobj,
				  &dev->kobj, dev_name(dev));
	if (error)
		goto out_device;

	return 0;

out_device:
	sysfs_remove_link(&dev->kobj, "device");

out_subsys:
	sysfs_remove_link(&dev->kobj, "subsystem");
out:
	return error;
}

static void device_remove_class_symlinks(struct device *dev)
{
	if (!dev->class)
		return;

	if (dev->parent && device_is_not_partition(dev))
		sysfs_remove_link(&dev->kobj, "device");
	sysfs_remove_link(&dev->kobj, "subsystem");
#ifdef CONFIG_BLOCK
	if (sysfs_deprecated && dev->class == &block_class)
		return;
#endif
	sysfs_delete_link(&dev->class->p->subsys.kobj, &dev->kobj, dev_name(dev));
}

/**
 * dev_set_name - set a device name
 * @dev: device
 * @fmt: format string for the device's name
 */
/** 20150829    
 * device의 포맷스트링을 파싱해 kobject의 name에 저장한다.
 **/
int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;
	int err;

	va_start(vargs, fmt);
	err = kobject_set_name_vargs(&dev->kobj, fmt, vargs);
	va_end(vargs);
	return err;
}
EXPORT_SYMBOL_GPL(dev_set_name);

/**
 * device_to_dev_kobj - select a /sys/dev/ directory for the device
 * @dev: device
 *
 * By default we select char/ for new entries.  Setting class->dev_obj
 * to NULL prevents an entry from being created.  class->dev_kobj must
 * be set (or cleared) before any devices are registered to the class
 * otherwise device_create_sys_dev_entry() and
 * device_remove_sys_dev_entry() will disagree about the presence of
 * the link.
 */
/** 20150905    
 * device를 위한 "/sys/dev/" 디렉토리를 선택한다.
 *
 * device에 class가 존재하면 class의 dev_kobj를 가져오고,
 * 그렇지 않으면 character device의 kobj를 가져온다.
 **/
static struct kobject *device_to_dev_kobj(struct device *dev)
{
	struct kobject *kobj;

	if (dev->class)
		kobj = dev->class->dev_kobj;
	else
		kobj = sysfs_dev_char_kobj;

	return kobj;
}

/** 20150829    
 * "/sys/dev" 아래 device를 위한 심볼릭 링크를 생성한다.
 **/
static int device_create_sys_dev_entry(struct device *dev)
{
	struct kobject *kobj = device_to_dev_kobj(dev);
	int error = 0;
	char devt_str[15];

	/** 20150905    
	 * kobj가 존재하면 "MAJOR:MINOR" 번호로 심볼릭 링크를 생성한다.
	 **/
	if (kobj) {
		format_dev_t(devt_str, dev->devt);
		error = sysfs_create_link(kobj, &dev->kobj, devt_str);
	}

	return error;
}

static void device_remove_sys_dev_entry(struct device *dev)
{
	struct kobject *kobj = device_to_dev_kobj(dev);
	char devt_str[15];

	if (kobj) {
		format_dev_t(devt_str, dev->devt);
		sysfs_remove_link(kobj, devt_str);
	}
}

/** 20150829    
 * device 내의 device_private 구조체를 초기화 한다.
 **/
int device_private_init(struct device *dev)
{
	/** 20150829    
	 * device_private 구조체로 사용할 메모리를 할당 받는다.
	 **/
	dev->p = kzalloc(sizeof(*dev->p), GFP_KERNEL);
	if (!dev->p)
		return -ENOMEM;
	dev->p->device = dev;
	/** 20150829    
	 * klist_children klist를 초기화 한다.
	 * children용 get/put 함수가 전달된다.
	 **/
	klist_init(&dev->p->klist_children, klist_children_get,
		   klist_children_put);
	INIT_LIST_HEAD(&dev->p->deferred_probe);
	return 0;
}

/**
 * device_add - add device to device hierarchy.
 * @dev: device.
 *
 * This is part 2 of device_register(), though may be called
 * separately _iff_ device_initialize() has been called separately.
 *
 * This adds @dev to the kobject hierarchy via kobject_add(), adds it
 * to the global and sibling lists for the device, then
 * adds it to the other relevant subsystems of the driver model.
 *
 * Do not call this routine or device_register() more than once for
 * any device structure.  The driver model core is not designed to work
 * with devices that get unregistered and then spring back to life.
 * (Among other things, it's very hard to guarantee that all references
 * to the previous incarnation of @dev have been dropped.)  Allocate
 * and register a fresh new struct device instead.
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up your
 * reference instead.
 */
/** 20150912    
 * 디바이스를 디바이스 hierarchy에 등록한다(kobject에 추가).
 * device_register()에서는 device_initailize() 된 상태에서 호출된다.
 **/
int device_add(struct device *dev)
{
	struct device *parent = NULL;
	struct kobject *kobj;
	struct class_interface *class_intf;
	int error = -EINVAL;

	/** 20150829    
	 * device를 참조한다.
	 **/
	dev = get_device(dev);
	if (!dev)
		goto done;

	/** 20150829    
	 * device_private이 정의되지 않았다면 할당받아 초기화 한다.
	 **/
	if (!dev->p) {
		error = device_private_init(dev);
		if (error)
			goto done;
	}

	/*
	 * for statically allocated devices, which should all be converted
	 * some day, we need to initialize the name. We prevent reading back
	 * the name, and force the use of dev_name()
	 */
	/** 20150829    
	 * device의 init_name이 지정되었다면 device 내의 kobject 이름으로 저장하고
	 * init_name을 초기화 한다.
	 **/
	if (dev->init_name) {
		dev_set_name(dev, "%s", dev->init_name);
		dev->init_name = NULL;
	}

	/* subsystems can specify simple device enumeration */
	/** 20150829    
	 * device의 name이 지정되지 않고 bus가 존재하면 bus의 이름을 device 이름으로
	 * 지정한다.
	 **/
	if (!dev_name(dev) && dev->bus && dev->bus->dev_name)
		dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);

	/** 20150829    
	 * 여전히 device의 이름이 없다면 error.
	 **/
	if (!dev_name(dev)) {
		error = -EINVAL;
		goto name_error;
	}

	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);

	/** 20150829    
	 * parent의 reference count를 증가시키고 가져와
	 * 추가하는 device의 parent로 지정한다.
	 **/
	parent = get_device(dev->parent);
	kobj = get_device_parent(dev, parent);
	if (kobj)
		dev->kobj.parent = kobj;

	/* use parent numa_node */
	/** 20150829    
	 * parent가 존재하는 경우, 추가하는 device는 parent의 node를 따른다.
	 **/
	if (parent)
		set_dev_node(dev, dev_to_node(parent));

	/* first, register with generic layer. */
	/* we require the name to be set before, and pass NULL */
	/** 20150829    
	 * kobject hierarchy에 device의 parent의 child entry로 추가한다.
	 **/
	error = kobject_add(&dev->kobj, dev->kobj.parent, NULL);
	if (error)
		goto Error;

	/* notify platform of device entry */
	/** 20150829    
	 * 플랫폼 notify 콜백이 지정되었다면 디바이스가 추가된 시점이므로 추가한다.
	 **/
	if (platform_notify)
		platform_notify(dev);

	/** 20150829    
	 * device의 uevent attribute로 파일을 생성한다.
	 **/
	error = device_create_file(dev, &uevent_attr);
	if (error)
		goto attrError;

	/** 20150829    
	 * 디바이스의 major 번호가 지정된 경우라면
	 * device의 dev attribute로 파일을 생성한다. ("dev"파일)
	 * 
	 **/
	if (MAJOR(dev->devt)) {
		error = device_create_file(dev, &devt_attr);
		if (error)
			goto ueventattrError;

		/** 20150905    
		 * "/sys/dev/XXXX" 아래 dev를 위한 심볼릭 링크를 생성한다
		 **/
		error = device_create_sys_dev_entry(dev);
		if (error)
			goto devtattrError;

		/** 20150905    
		 * devtmpfs 를 위한 node를 생성한다.
		 **/
		devtmpfs_create_node(dev);
	}

	/** 20150905    
	 * device를 위한 class 관련 심볼릭 링크를 생성한다.
	 **/
	error = device_add_class_symlinks(dev);
	if (error)
		goto SymlinkError;
	/** 20150905    
	 * device의 class, type, device 자신의 attribute들이 추가된다.
	 **/
	error = device_add_attrs(dev);
	if (error)
		goto AttrsError;
	/** 20150905    
	 * device를 bus에 추가한다.
	 **/
	error = bus_add_device(dev);
	if (error)
		goto BusError;
	/** 20150905    
	 * device에 PM 관련 attirubte 그룹을 추가한다.
	 **/
	error = dpm_sysfs_add(dev);
	if (error)
		goto DPMError;
	/** 20150905    
	 * device를 PM list에 추가한다.
	 **/
	device_pm_add(dev);

	/* Notify clients of device addition.  This call must come
	 * after dpm_sysfs_add() and before kobject_uevent().
	 */
	/** 20150905    
	 * device의 bus가 존재하면
	 * bus notifier 리스트에 새로운 device가 추가되었음을 통보한다.
	 **/
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_ADD_DEVICE, dev);

	/** 20150905    
	 * device의 kobj가 추가되었음을 userspace에 알린다.
	 **/
	kobject_uevent(&dev->kobj, KOBJ_ADD);
	/** 20150905    
	 * 새로운 device에 대해 bus가 지정되어 있다면 버스의 드라이버를 probe 한다.
	 **/
	bus_probe_device(dev);
	/** 20150912    
	 * parent device가 존재하면 디바이스를 parent의 children 리스트에 등록한다.
	 **/
	if (parent)
		klist_add_tail(&dev->p->knode_parent,
			       &parent->p->klist_children);

	/** 20150912    
	 * class가 존재하면 클래스의 디바이스 리스트에 등록한다.
	 **/
	if (dev->class) {
		mutex_lock(&dev->class->p->mutex);
		/* tie the class to the device */
		klist_add_tail(&dev->knode_class,
			       &dev->class->p->klist_devices);

		/* notify any interfaces that the device is here */
		/** 20150912    
		 * class의 인터페이스 리스트의 인터페이스들을 순회하며
		 * add_dev 콜백들을 호출한다.
		 **/
		list_for_each_entry(class_intf,
				    &dev->class->p->interfaces, node)
			if (class_intf->add_dev)
				class_intf->add_dev(dev, class_intf);
		mutex_unlock(&dev->class->p->mutex);
	}
done:
	put_device(dev);
	return error;
 DPMError:
	bus_remove_device(dev);
 BusError:
	device_remove_attrs(dev);
 AttrsError:
	device_remove_class_symlinks(dev);
 SymlinkError:
	if (MAJOR(dev->devt))
		devtmpfs_delete_node(dev);
	if (MAJOR(dev->devt))
		device_remove_sys_dev_entry(dev);
 devtattrError:
	if (MAJOR(dev->devt))
		device_remove_file(dev, &devt_attr);
 ueventattrError:
	device_remove_file(dev, &uevent_attr);
 attrError:
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	kobject_del(&dev->kobj);
 Error:
	cleanup_device_parent(dev);
	if (parent)
		put_device(parent);
name_error:
	kfree(dev->p);
	dev->p = NULL;
	goto done;
}

/**
 * device_register - register a device with the system.
 * @dev: pointer to the device structure
 *
 * This happens in two clean steps - initialize the device
 * and add it to the system. The two steps can be called
 * separately, but this is the easiest and most common.
 * I.e. you should only call the two helpers separately if
 * have a clearly defined need to use and refcount the device
 * before it is added to the hierarchy.
 *
 * For more information, see the kerneldoc for device_initialize()
 * and device_add().
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up the
 * reference initialized in this function instead.
 */
/** 20150912    
 * 디바이스를 시스템에 등록한다.
 *
 * device 초기화를 수행하는 device_initialize와
 * 생성된 device를 등록하는 device_add 과정을 거친다.
 **/
int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

/**
 * get_device - increment reference count for device.
 * @dev: device.
 *
 * This simply forwards the call to kobject_get(), though
 * we do take care to provide for the case that we get a NULL
 * pointer passed in.
 */
/** 20150829    
 * device가 존재하면 kobject 레퍼런스 카운트를 증가시키고 device를 리턴한다.
 **/
struct device *get_device(struct device *dev)
{
	return dev ? kobj_to_dev(kobject_get(&dev->kobj)) : NULL;
}

/**
 * put_device - decrement reference count.
 * @dev: device in question.
 */
void put_device(struct device *dev)
{
	/* might_sleep(); */
	if (dev)
		kobject_put(&dev->kobj);
}

/**
 * device_del - delete device from system.
 * @dev: device.
 *
 * This is the first part of the device unregistration
 * sequence. This removes the device from the lists we control
 * from here, has it removed from the other driver model
 * subsystems it was added to in device_add(), and removes it
 * from the kobject hierarchy.
 *
 * NOTE: this should be called manually _iff_ device_add() was
 * also called manually.
 */
void device_del(struct device *dev)
{
	struct device *parent = dev->parent;
	struct class_interface *class_intf;

	/* Notify clients of device removal.  This call must come
	 * before dpm_sysfs_remove().
	 */
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DEL_DEVICE, dev);
	device_pm_remove(dev);
	dpm_sysfs_remove(dev);
	if (parent)
		klist_del(&dev->p->knode_parent);
	if (MAJOR(dev->devt)) {
		devtmpfs_delete_node(dev);
		device_remove_sys_dev_entry(dev);
		device_remove_file(dev, &devt_attr);
	}
	if (dev->class) {
		device_remove_class_symlinks(dev);

		mutex_lock(&dev->class->p->mutex);
		/* notify any interfaces that the device is now gone */
		list_for_each_entry(class_intf,
				    &dev->class->p->interfaces, node)
			if (class_intf->remove_dev)
				class_intf->remove_dev(dev, class_intf);
		/* remove the device from the class list */
		klist_del(&dev->knode_class);
		mutex_unlock(&dev->class->p->mutex);
	}
	device_remove_file(dev, &uevent_attr);
	device_remove_attrs(dev);
	bus_remove_device(dev);
	driver_deferred_probe_del(dev);

	/*
	 * Some platform devices are driven without driver attached
	 * and managed resources may have been acquired.  Make sure
	 * all resources are released.
	 */
	devres_release_all(dev);

	/* Notify the platform of the removal, in case they
	 * need to do anything...
	 */
	if (platform_notify_remove)
		platform_notify_remove(dev);
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	cleanup_device_parent(dev);
	kobject_del(&dev->kobj);
	put_device(parent);
}

/**
 * device_unregister - unregister device from system.
 * @dev: device going away.
 *
 * We do this in two parts, like we do device_register(). First,
 * we remove it from all the subsystems with device_del(), then
 * we decrement the reference count via put_device(). If that
 * is the final reference count, the device will be cleaned up
 * via device_release() above. Otherwise, the structure will
 * stick around until the final reference to the device is dropped.
 */
void device_unregister(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
	device_del(dev);
	put_device(dev);
}

static struct device *next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct device_private *p;

	if (n) {
		p = to_device_private_parent(n);
		dev = p->device;
	}
	return dev;
}

/**
 * device_get_devnode - path of device node file
 * @dev: device
 * @mode: returned file access mode
 * @tmp: possibly allocated string
 *
 * Return the relative path of a possible device node.
 * Non-default names may need to allocate a memory to compose
 * a name. This memory is returned in tmp and needs to be
 * freed by the caller.
 */
const char *device_get_devnode(struct device *dev,
			       umode_t *mode, const char **tmp)
{
	char *s;

	*tmp = NULL;

	/* the device type may provide a specific name */
	if (dev->type && dev->type->devnode)
		*tmp = dev->type->devnode(dev, mode);
	if (*tmp)
		return *tmp;

	/* the class may provide a specific name */
	if (dev->class && dev->class->devnode)
		*tmp = dev->class->devnode(dev, mode);
	if (*tmp)
		return *tmp;

	/* return name without allocation, tmp == NULL */
	if (strchr(dev_name(dev), '!') == NULL)
		return dev_name(dev);

	/* replace '!' in the name with '/' */
	*tmp = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!*tmp)
		return NULL;
	while ((s = strchr(*tmp, '!')))
		s[0] = '/';
	return *tmp;
}

/**
 * device_for_each_child - device child iterator.
 * @parent: parent struct device.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @parent's child devices, and call @fn for each,
 * passing it @data.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 */
int device_for_each_child(struct device *parent, void *data,
			  int (*fn)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *child;
	int error = 0;

	if (!parent->p)
		return 0;

	klist_iter_init(&parent->p->klist_children, &i);
	while ((child = next_device(&i)) && !error)
		error = fn(child, data);
	klist_iter_exit(&i);
	return error;
}

/**
 * device_find_child - device iterator for locating a particular device.
 * @parent: parent struct device
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the device_for_each_child() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero and a reference to the
 * current device can be obtained, this function will return to the caller
 * and not iterate over any more devices.
 */
struct device *device_find_child(struct device *parent, void *data,
				 int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *child;

	if (!parent)
		return NULL;

	klist_iter_init(&parent->p->klist_children, &i);
	while ((child = next_device(&i)))
		if (match(child, data) && get_device(child))
			break;
	klist_iter_exit(&i);
	return child;
}

/** 20150829    
 * device 등록을 위해 필요한 "devices"와 "dev"를 생성하고 sysfs에 추가한다.
 **/
int __init devices_init(void)
{
	/** 20150829    
	 * "devices"라는 kset을 동적으로 생성하고, sysfs에 붙여준다.
	 * kset에 해당하는 uevent ops를 지정한다.
	 **/
	devices_kset = kset_create_and_add("devices", &device_uevent_ops, NULL);
	if (!devices_kset)
		return -ENOMEM;
	/** 20150829    
	 * "dev"라는 kobject를 할당받아 sysfs에 붙여준다.
	 *     dev 아래 "block", "char"라는 kobject를 생성한다.
	 **/
	dev_kobj = kobject_create_and_add("dev", NULL);
	if (!dev_kobj)
		goto dev_kobj_err;
	sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
	if (!sysfs_dev_block_kobj)
		goto block_kobj_err;
	sysfs_dev_char_kobj = kobject_create_and_add("char", dev_kobj);
	if (!sysfs_dev_char_kobj)
		goto char_kobj_err;

	return 0;

 char_kobj_err:
	kobject_put(sysfs_dev_block_kobj);
 block_kobj_err:
	kobject_put(dev_kobj);
 dev_kobj_err:
	kset_unregister(devices_kset);
	return -ENOMEM;
}

EXPORT_SYMBOL_GPL(device_for_each_child);
EXPORT_SYMBOL_GPL(device_find_child);

EXPORT_SYMBOL_GPL(device_initialize);
EXPORT_SYMBOL_GPL(device_add);
EXPORT_SYMBOL_GPL(device_register);

EXPORT_SYMBOL_GPL(device_del);
EXPORT_SYMBOL_GPL(device_unregister);
EXPORT_SYMBOL_GPL(get_device);
EXPORT_SYMBOL_GPL(put_device);

EXPORT_SYMBOL_GPL(device_create_file);
EXPORT_SYMBOL_GPL(device_remove_file);

struct root_device {
	struct device dev;
	struct module *owner;
};

inline struct root_device *to_root_device(struct device *d)
{
	return container_of(d, struct root_device, dev);
}

static void root_device_release(struct device *dev)
{
	kfree(to_root_device(dev));
}

/**
 * __root_device_register - allocate and register a root device
 * @name: root device name
 * @owner: owner module of the root device, usually THIS_MODULE
 *
 * This function allocates a root device and registers it
 * using device_register(). In order to free the returned
 * device, use root_device_unregister().
 *
 * Root devices are dummy devices which allow other devices
 * to be grouped under /sys/devices. Use this function to
 * allocate a root device and then use it as the parent of
 * any device which should appear under /sys/devices/{name}
 *
 * The /sys/devices/{name} directory will also contain a
 * 'module' symlink which points to the @owner directory
 * in sysfs.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: You probably want to use root_device_register().
 */
struct device *__root_device_register(const char *name, struct module *owner)
{
	struct root_device *root;
	int err = -ENOMEM;

	root = kzalloc(sizeof(struct root_device), GFP_KERNEL);
	if (!root)
		return ERR_PTR(err);

	err = dev_set_name(&root->dev, "%s", name);
	if (err) {
		kfree(root);
		return ERR_PTR(err);
	}

	root->dev.release = root_device_release;

	err = device_register(&root->dev);
	if (err) {
		put_device(&root->dev);
		return ERR_PTR(err);
	}

#ifdef CONFIG_MODULES	/* gotta find a "cleaner" way to do this */
	if (owner) {
		struct module_kobject *mk = &owner->mkobj;

		err = sysfs_create_link(&root->dev.kobj, &mk->kobj, "module");
		if (err) {
			device_unregister(&root->dev);
			return ERR_PTR(err);
		}
		root->owner = owner;
	}
#endif

	return &root->dev;
}
EXPORT_SYMBOL_GPL(__root_device_register);

/**
 * root_device_unregister - unregister and free a root device
 * @dev: device going away
 *
 * This function unregisters and cleans up a device that was created by
 * root_device_register().
 */
void root_device_unregister(struct device *dev)
{
	struct root_device *root = to_root_device(dev);

	if (root->owner)
		sysfs_remove_link(&root->dev.kobj, "module");

	device_unregister(dev);
}
EXPORT_SYMBOL_GPL(root_device_unregister);


static void device_create_release(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
	kfree(dev);
}

/**
 * device_create_vargs - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 * @args: va_list for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/** 20151114    
 * device 구조체를 생성해 이름과 속성을 지정하고, hierarchy에 추가한다.
 **/
struct device *device_create_vargs(struct class *class, struct device *parent,
				   dev_t devt, void *drvdata, const char *fmt,
				   va_list args)
{
	struct device *dev = NULL;
	int retval = -ENODEV;

	if (class == NULL || IS_ERR(class))
		goto error;

	/** 20151114    
	 * device 구조체 메모리를 할당 받고, 일부 멤버를 초기화 한다.
	 **/
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		retval = -ENOMEM;
		goto error;
	}

	dev->devt = devt;
	dev->class = class;
	dev->parent = parent;
	dev->release = device_create_release;
	dev_set_drvdata(dev, drvdata);

	/** 20151114    
	 * kobject에 device name을 채운다.
	 **/
	retval = kobject_set_name_vargs(&dev->kobj, fmt, args);
	if (retval)
		goto error;

	/** 20151114    
	 * 디바이스의 다른 멤버들을 초기화 하고, device hierarchy에 추가한다.
	 **/
	retval = device_register(dev);
	if (retval)
		goto error;

	return dev;

error:
	put_device(dev);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(device_create_vargs);

/**
 * device_create - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/** 20151114    
 * 디바이스 객체를 생성하고, hierarchy에 추가한다.
 *
 * class : 이 디바이스가 속할 클래스를 정의한다.
 * parent : 이 디바이스의 부모 디바이스를 정의한다.
 **/
struct device *device_create(struct class *class, struct device *parent,
			     dev_t devt, void *drvdata, const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

	va_start(vargs, fmt);
	dev = device_create_vargs(class, parent, devt, drvdata, fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL_GPL(device_create);

static int __match_devt(struct device *dev, void *data)
{
	dev_t *devt = data;

	return dev->devt == *devt;
}

/**
 * device_destroy - removes a device that was created with device_create()
 * @class: pointer to the struct class that this device was registered with
 * @devt: the dev_t of the device that was previously registered
 *
 * This call unregisters and cleans up a device that was created with a
 * call to device_create().
 */
void device_destroy(struct class *class, dev_t devt)
{
	struct device *dev;

	dev = class_find_device(class, NULL, &devt, __match_devt);
	if (dev) {
		put_device(dev);
		device_unregister(dev);
	}
}
EXPORT_SYMBOL_GPL(device_destroy);

/**
 * device_rename - renames a device
 * @dev: the pointer to the struct device to be renamed
 * @new_name: the new name of the device
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of device_rename
 * on the same device to ensure that new_name is valid and
 * won't conflict with other devices.
 *
 * Note: Don't call this function.  Currently, the networking layer calls this
 * function, but that will change.  The following text from Kay Sievers offers
 * some insight:
 *
 * Renaming devices is racy at many levels, symlinks and other stuff are not
 * replaced atomically, and you get a "move" uevent, but it's not easy to
 * connect the event to the old and new device. Device nodes are not renamed at
 * all, there isn't even support for that in the kernel now.
 *
 * In the meantime, during renaming, your target name might be taken by another
 * driver, creating conflicts. Or the old name is taken directly after you
 * renamed it -- then you get events for the same DEVPATH, before you even see
 * the "move" event. It's just a mess, and nothing new should ever rely on
 * kernel device renaming. Besides that, it's not even implemented now for
 * other things than (driver-core wise very simple) network devices.
 *
 * We are currently about to change network renaming in udev to completely
 * disallow renaming of devices in the same namespace as the kernel uses,
 * because we can't solve the problems properly, that arise with swapping names
 * of multiple interfaces without races. Means, renaming of eth[0-9]* will only
 * be allowed to some other name than eth[0-9]*, for the aforementioned
 * reasons.
 *
 * Make up a "real" name in the driver before you register anything, or add
 * some other attributes for userspace to find the device, or use udev to add
 * symlinks -- but never rename kernel devices later, it's a complete mess. We
 * don't even want to get into that and try to implement the missing pieces in
 * the core. We really have other pieces to fix in the driver core mess. :)
 */
int device_rename(struct device *dev, const char *new_name)
{
	char *old_class_name = NULL;
	char *new_class_name = NULL;
	char *old_device_name = NULL;
	int error;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	pr_debug("device: '%s': %s: renaming to '%s'\n", dev_name(dev),
		 __func__, new_name);

	old_device_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!old_device_name) {
		error = -ENOMEM;
		goto out;
	}

	if (dev->class) {
		error = sysfs_rename_link(&dev->class->p->subsys.kobj,
			&dev->kobj, old_device_name, new_name);
		if (error)
			goto out;
	}

	error = kobject_rename(&dev->kobj, new_name);
	if (error)
		goto out;

out:
	put_device(dev);

	kfree(new_class_name);
	kfree(old_class_name);
	kfree(old_device_name);

	return error;
}
EXPORT_SYMBOL_GPL(device_rename);

static int device_move_class_links(struct device *dev,
				   struct device *old_parent,
				   struct device *new_parent)
{
	int error = 0;

	if (old_parent)
		sysfs_remove_link(&dev->kobj, "device");
	if (new_parent)
		error = sysfs_create_link(&dev->kobj, &new_parent->kobj,
					  "device");
	return error;
}

/**
 * device_move - moves a device to a new parent
 * @dev: the pointer to the struct device to be moved
 * @new_parent: the new parent of the device (can by NULL)
 * @dpm_order: how to reorder the dpm_list
 */
int device_move(struct device *dev, struct device *new_parent,
		enum dpm_order dpm_order)
{
	int error;
	struct device *old_parent;
	struct kobject *new_parent_kobj;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	device_pm_lock();
	new_parent = get_device(new_parent);
	new_parent_kobj = get_device_parent(dev, new_parent);

	pr_debug("device: '%s': %s: moving to '%s'\n", dev_name(dev),
		 __func__, new_parent ? dev_name(new_parent) : "<NULL>");
	error = kobject_move(&dev->kobj, new_parent_kobj);
	if (error) {
		cleanup_glue_dir(dev, new_parent_kobj);
		put_device(new_parent);
		goto out;
	}
	old_parent = dev->parent;
	dev->parent = new_parent;
	if (old_parent)
		klist_remove(&dev->p->knode_parent);
	if (new_parent) {
		klist_add_tail(&dev->p->knode_parent,
			       &new_parent->p->klist_children);
		set_dev_node(dev, dev_to_node(new_parent));
	}

	if (dev->class) {
		error = device_move_class_links(dev, old_parent, new_parent);
		if (error) {
			/* We ignore errors on cleanup since we're hosed anyway... */
			device_move_class_links(dev, new_parent, old_parent);
			if (!kobject_move(&dev->kobj, &old_parent->kobj)) {
				if (new_parent)
					klist_remove(&dev->p->knode_parent);
				dev->parent = old_parent;
				if (old_parent) {
					klist_add_tail(&dev->p->knode_parent,
						       &old_parent->p->klist_children);
					set_dev_node(dev, dev_to_node(old_parent));
				}
			}
			cleanup_glue_dir(dev, new_parent_kobj);
			put_device(new_parent);
			goto out;
		}
	}
	switch (dpm_order) {
	case DPM_ORDER_NONE:
		break;
	case DPM_ORDER_DEV_AFTER_PARENT:
		device_pm_move_after(dev, new_parent);
		break;
	case DPM_ORDER_PARENT_BEFORE_DEV:
		device_pm_move_before(new_parent, dev);
		break;
	case DPM_ORDER_DEV_LAST:
		device_pm_move_last(dev);
		break;
	}

	put_device(old_parent);
out:
	device_pm_unlock();
	put_device(dev);
	return error;
}
EXPORT_SYMBOL_GPL(device_move);

/**
 * device_shutdown - call ->shutdown() on each device to shutdown.
 */
void device_shutdown(void)
{
	struct device *dev;

	spin_lock(&devices_kset->list_lock);
	/*
	 * Walk the devices list backward, shutting down each in turn.
	 * Beware that device unplug events may also start pulling
	 * devices offline, even as the system is shutting down.
	 */
	while (!list_empty(&devices_kset->list)) {
		dev = list_entry(devices_kset->list.prev, struct device,
				kobj.entry);

		/*
		 * hold reference count of device's parent to
		 * prevent it from being freed because parent's
		 * lock is to be held
		 */
		get_device(dev->parent);
		get_device(dev);
		/*
		 * Make sure the device is off the kset list, in the
		 * event that dev->*->shutdown() doesn't remove it.
		 */
		list_del_init(&dev->kobj.entry);
		spin_unlock(&devices_kset->list_lock);

		/* hold lock to avoid race with probe/release */
		if (dev->parent)
			device_lock(dev->parent);
		device_lock(dev);

		/* Don't allow any more runtime suspends */
		pm_runtime_get_noresume(dev);
		pm_runtime_barrier(dev);

		if (dev->bus && dev->bus->shutdown) {
			dev_dbg(dev, "shutdown\n");
			dev->bus->shutdown(dev);
		} else if (dev->driver && dev->driver->shutdown) {
			dev_dbg(dev, "shutdown\n");
			dev->driver->shutdown(dev);
		}

		device_unlock(dev);
		if (dev->parent)
			device_unlock(dev->parent);

		put_device(dev);
		put_device(dev->parent);

		spin_lock(&devices_kset->list_lock);
	}
	spin_unlock(&devices_kset->list_lock);
	async_synchronize_full();
}

/*
 * Device logging functions
 */

#ifdef CONFIG_PRINTK
int __dev_printk(const char *level, const struct device *dev,
		 struct va_format *vaf)
{
	char dict[128];
	size_t dictlen = 0;
	const char *subsys;

	if (!dev)
		return printk("%s(NULL device *): %pV", level, vaf);

	if (dev->class)
		subsys = dev->class->name;
	else if (dev->bus)
		subsys = dev->bus->name;
	else
		goto skip;

	dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
			    "SUBSYSTEM=%s", subsys);

	/*
	 * Add device identifier DEVICE=:
	 *   b12:8         block dev_t
	 *   c127:3        char dev_t
	 *   n8            netdev ifindex
	 *   +sound:card0  subsystem:devname
	 */
	if (MAJOR(dev->devt)) {
		char c;

		if (strcmp(subsys, "block") == 0)
			c = 'b';
		else
			c = 'c';
		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				   "DEVICE=%c%u:%u",
				   c, MAJOR(dev->devt), MINOR(dev->devt));
	} else if (strcmp(subsys, "net") == 0) {
		struct net_device *net = to_net_dev(dev);

		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				    "DEVICE=n%u", net->ifindex);
	} else {
		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				    "DEVICE=+%s:%s", subsys, dev_name(dev));
	}
skip:
	return printk_emit(0, level[1] - '0',
			   dictlen ? dict : NULL, dictlen,
			   "%s %s: %pV",
			   dev_driver_string(dev), dev_name(dev), vaf);
}
EXPORT_SYMBOL(__dev_printk);

int dev_printk(const char *level, const struct device *dev,
	       const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	int r;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	r = __dev_printk(level, dev, &vaf);
	va_end(args);

	return r;
}
EXPORT_SYMBOL(dev_printk);

#define define_dev_printk_level(func, kern_level)		\
int func(const struct device *dev, const char *fmt, ...)	\
{								\
	struct va_format vaf;					\
	va_list args;						\
	int r;							\
								\
	va_start(args, fmt);					\
								\
	vaf.fmt = fmt;						\
	vaf.va = &args;						\
								\
	r = __dev_printk(kern_level, dev, &vaf);		\
	va_end(args);						\
								\
	return r;						\
}								\
EXPORT_SYMBOL(func);

define_dev_printk_level(dev_emerg, KERN_EMERG);
define_dev_printk_level(dev_alert, KERN_ALERT);
define_dev_printk_level(dev_crit, KERN_CRIT);
define_dev_printk_level(dev_err, KERN_ERR);
define_dev_printk_level(dev_warn, KERN_WARNING);
define_dev_printk_level(dev_notice, KERN_NOTICE);
define_dev_printk_level(_dev_info, KERN_INFO);

#endif
