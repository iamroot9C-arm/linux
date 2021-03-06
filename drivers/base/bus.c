/*
 * bus.c - bus driver management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007 Novell Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include "base.h"
#include "power/power.h"

/* /sys/devices/system */
/** 20150912
 * buses_init에서 /sys/devices/system을 생성한다.
 **/
static struct kset *system_kset;

#define to_bus_attr(_attr) container_of(_attr, struct bus_attribute, attr)

/*
 * sysfs bindings for drivers
 */

#define to_drv_attr(_attr) container_of(_attr, struct driver_attribute, attr)


static int __must_check bus_rescan_devices_helper(struct device *dev,
						void *data);

/** 20150905
 * bus의 참조를 시작한다.
 *
 * bus_type 내 subsys의 reference count를 증가시킨다.
 **/
static struct bus_type *bus_get(struct bus_type *bus)
{
	if (bus) {
		kset_get(&bus->p->subsys);
		return bus;
	}
	return NULL;
}

/** 20151121
 * bus의 참조를 끝낸다.
 *
 * bus_type 내 subsys의 reference count를 감소시킨다.
 **/
static void bus_put(struct bus_type *bus)
{
	if (bus)
		kset_put(&bus->p->subsys);
}

static ssize_t drv_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct driver_attribute *drv_attr = to_drv_attr(attr);
	struct driver_private *drv_priv = to_driver(kobj);
	ssize_t ret = -EIO;

	if (drv_attr->show)
		ret = drv_attr->show(drv_priv->driver, buf);
	return ret;
}

static ssize_t drv_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct driver_attribute *drv_attr = to_drv_attr(attr);
	struct driver_private *drv_priv = to_driver(kobj);
	ssize_t ret = -EIO;

	if (drv_attr->store)
		ret = drv_attr->store(drv_priv->driver, buf, count);
	return ret;
}

static const struct sysfs_ops driver_sysfs_ops = {
	.show	= drv_attr_show,
	.store	= drv_attr_store,
};

static void driver_release(struct kobject *kobj)
{
	struct driver_private *drv_priv = to_driver(kobj);

	pr_debug("driver: '%s': %s\n", kobject_name(kobj), __func__);
	kfree(drv_priv);
}

static struct kobj_type driver_ktype = {
	.sysfs_ops	= &driver_sysfs_ops,
	.release	= driver_release,
};

/*
 * sysfs bindings for buses
 */
static ssize_t bus_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct bus_attribute *bus_attr = to_bus_attr(attr);
	struct subsys_private *subsys_priv = to_subsys_private(kobj);
	ssize_t ret = 0;

	if (bus_attr->show)
		ret = bus_attr->show(subsys_priv->bus, buf);
	return ret;
}

static ssize_t bus_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct bus_attribute *bus_attr = to_bus_attr(attr);
	struct subsys_private *subsys_priv = to_subsys_private(kobj);
	ssize_t ret = 0;

	if (bus_attr->store)
		ret = bus_attr->store(subsys_priv->bus, buf, count);
	return ret;
}

static const struct sysfs_ops bus_sysfs_ops = {
	.show	= bus_attr_show,
	.store	= bus_attr_store,
};

/** 20150905
 * bus에 attribute로 지정된 파일을 sysfs에 생성한다.
 **/
int bus_create_file(struct bus_type *bus, struct bus_attribute *attr)
{
	int error;
	/** 20150905
	 * bus 사용권을 획득하고,
	 * bus kobject 아래 attribute에 해당하는 파일을 생성한다.
	 **/
	if (bus_get(bus)) {
		error = sysfs_create_file(&bus->p->subsys.kobj, &attr->attr);
		bus_put(bus);
	} else
		error = -EINVAL;
	return error;
}
EXPORT_SYMBOL_GPL(bus_create_file);

void bus_remove_file(struct bus_type *bus, struct bus_attribute *attr)
{
	if (bus_get(bus)) {
		sysfs_remove_file(&bus->p->subsys.kobj, &attr->attr);
		bus_put(bus);
	}
}
EXPORT_SYMBOL_GPL(bus_remove_file);

/** 20150905
 * bus ktype.
 **/
static struct kobj_type bus_ktype = {
	.sysfs_ops	= &bus_sysfs_ops,
};

static int bus_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);

	if (ktype == &bus_ktype)
		return 1;
	return 0;
}

/** 20150829
 * bus kset의 uevent ops.
 * filter만 정의하고 있다.
 *
 * bus의 uevent 콜백은 struct bus_type 구조체 내의 uevent 함수 포인터로 가리킨다.
 **/
static const struct kset_uevent_ops bus_uevent_ops = {
	.filter = bus_uevent_filter,
};

/** 20150829
 * bus kset. buses_init()에서 kset을 할당받아 저장한다.
 **/
static struct kset *bus_kset;


#ifdef CONFIG_HOTPLUG
/* Manually detach a device from its associated driver. */
static ssize_t driver_unbind(struct device_driver *drv,
			     const char *buf, size_t count)
{
	struct bus_type *bus = bus_get(drv->bus);
	struct device *dev;
	int err = -ENODEV;

	dev = bus_find_device_by_name(bus, NULL, buf);
	if (dev && dev->driver == drv) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
		device_release_driver(dev);
		if (dev->parent)
			device_unlock(dev->parent);
		err = count;
	}
	put_device(dev);
	bus_put(bus);
	return err;
}
static DRIVER_ATTR(unbind, S_IWUSR, NULL, driver_unbind);

/*
 * Manually attach a device to a driver.
 * Note: the driver must want to bind to the device,
 * it is not possible to override the driver's id table.
 */
/** 20151121
 * 수동으로 드라이버를 구동할 드라이버를 붙인다 (probe -> bind)
 **/
static ssize_t driver_bind(struct device_driver *drv,
			   const char *buf, size_t count)
{
	struct bus_type *bus = bus_get(drv->bus);
	struct device *dev;
	int err = -ENODEV;

	/** 20151121
	 * 드라이버가 속한 버스에서 buf에 해당하는 디바이스를 검색.
	 **/
	dev = bus_find_device_by_name(bus, NULL, buf);
	/** 20151121
	 * 검색된 디바이스에 드라이버가 존재하지 않고 해당 드라이버와 match되면
	 * 드라이버로 해당 디바이스를 구동가능한지 검사해 bind시킨다.
	 **/
	if (dev && dev->driver == NULL && driver_match_device(drv, dev)) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
		device_lock(dev);
		err = driver_probe_device(drv, dev);
		device_unlock(dev);
		if (dev->parent)
			device_unlock(dev->parent);

		if (err > 0) {
			/* success */
			err = count;
		} else if (err == 0) {
			/* driver didn't accept device */
			err = -ENODEV;
		}
	}
	put_device(dev);
	bus_put(bus);
	return err;
}
static DRIVER_ATTR(bind, S_IWUSR, NULL, driver_bind);

static ssize_t show_drivers_autoprobe(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "%d\n", bus->p->drivers_autoprobe);
}

static ssize_t store_drivers_autoprobe(struct bus_type *bus,
				       const char *buf, size_t count)
{
	if (buf[0] == '0')
		bus->p->drivers_autoprobe = 0;
	else
		bus->p->drivers_autoprobe = 1;
	return count;
}

static ssize_t store_drivers_probe(struct bus_type *bus,
				   const char *buf, size_t count)
{
	struct device *dev;

	/** 20150912
	 * bus에 속한 디바이스들 중 buf라는 이름을 가진 device를 찾는다.
	 **/
	dev = bus_find_device_by_name(bus, NULL, buf);
	if (!dev)
		return -ENODEV;
	if (bus_rescan_devices_helper(dev, NULL) != 0)
		return -EINVAL;
	return count;
}
#endif

/** 20150912
 * device - device_private - knode 포함 구조에서
 * klist_iter로 다음 knode를 찾고, 이를 포함하는 device 자료구조를 찾아 리턴한다.
 **/
static struct device *next_device(struct klist_iter *i)
{
	/** 20150912
	 * iterator로부터 다음 klist 노드를 받아온다.
	 **/
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct device_private *dev_prv;

	/** 20150912
	 * 다음 klist 노드가 존재하면, 해당 노드를 사용하는 dev_private를 찾아온다.
	 * dev_prv가 속한 device를 찾아 리턴한다.
	 **/
	if (n) {
		dev_prv = to_device_private_bus(n);
		dev = dev_prv->device;
	}
	return dev;
}

/**
 * bus_for_each_dev - device iterator.
 * @bus: bus type.
 * @start: device to start iterating from.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @bus's list of devices, and call @fn for each,
 * passing it @data. If @start is not NULL, we use that device to
 * begin iterating from.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 *
 * NOTE: The device that returns a non-zero value is not retained
 * in any way, nor is its refcount incremented. If the caller needs
 * to retain this data, it should do so, and increment the reference
 * count in the supplied callback.
 */
/** 20151121
 * 버스에 등록된 각 디바이스에 대해 fn을 호출한다.
 **/
int bus_for_each_dev(struct bus_type *bus, struct device *start,
		     void *data, int (*fn)(struct device *, void *))
{
	struct klist_iter i;
	struct device *dev;
	int error = 0;

	if (!bus)
		return -EINVAL;

	/** 20151121
	 * 버스의 device 목록을 유지하는 klist를 탐색하기 위한 준비를 한다.
	 **/
	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));
	/** 20151121
	 * 각 디바이스에 대해 fn 콜백을 진행한다.
	 **/
	while ((dev = next_device(&i)) && !error)
		error = fn(dev, data);
	/** 20151121
	 * klist 탐색을 종료한다.
	 **/
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(bus_for_each_dev);

/**
 * bus_find_device - device iterator for locating a particular device.
 * @bus: bus type
 * @start: Device to begin with
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the bus_for_each_dev() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 */
/** 20150912
 * 버스에 속하는 device 리스트에서 start 이후부터 match 조건에 해당하는
 * 디바이스를 찾아 리턴한다.
 **/
struct device *bus_find_device(struct bus_type *bus,
			       struct device *start, void *data,
			       int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *dev;

	if (!bus)
		return NULL;

	/** 20150912
	 * klist_devices 리스트를 순회하기 위한 iterator를 초기화 한다.
	 **/
	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));
	/** 20150912
	 * 다음 device가 존재하는 동안 계속 찾는다.
	 * match 함수로 비교해 일치하면 device의 참조 카운터를 증가시키고 리턴한다. 
	 **/
	while ((dev = next_device(&i)))
		if (match(dev, data) && get_device(dev))
			break;
	klist_iter_exit(&i);
	return dev;
}
EXPORT_SYMBOL_GPL(bus_find_device);

/** 20150912
 * name을 비교대상으로 삼는 비교함수.
 **/
static int match_name(struct device *dev, void *data)
{
	const char *name = data;

	return sysfs_streq(name, dev_name(dev));
}

/**
 * bus_find_device_by_name - device iterator for locating a particular device of a specific name
 * @bus: bus type
 * @start: Device to begin with
 * @name: name of the device to match
 *
 * This is similar to the bus_find_device() function above, but it handles
 * searching by a name automatically, no need to write another strcmp matching
 * function.
 */
/** 20150912
 * bus에서 start 이후의 device 중 name과 이름을 비교해 동일한 device를 찾아 리턴.
 **/
struct device *bus_find_device_by_name(struct bus_type *bus,
				       struct device *start, const char *name)
{
	return bus_find_device(bus, start, (void *)name, match_name);
}
EXPORT_SYMBOL_GPL(bus_find_device_by_name);

/**
 * subsys_find_device_by_id - find a device with a specific enumeration number
 * @subsys: subsystem
 * @id: index 'id' in struct device
 * @hint: device to check first
 *
 * Check the hint's next object and if it is a match return it directly,
 * otherwise, fall back to a full list search. Either way a reference for
 * the returned object is taken.
 */
struct device *subsys_find_device_by_id(struct bus_type *subsys, unsigned int id,
					struct device *hint)
{
	struct klist_iter i;
	struct device *dev;

	if (!subsys)
		return NULL;

	if (hint) {
		klist_iter_init_node(&subsys->p->klist_devices, &i, &hint->p->knode_bus);
		dev = next_device(&i);
		if (dev && dev->id == id && get_device(dev)) {
			klist_iter_exit(&i);
			return dev;
		}
		klist_iter_exit(&i);
	}

	klist_iter_init_node(&subsys->p->klist_devices, &i, NULL);
	while ((dev = next_device(&i))) {
		if (dev->id == id && get_device(dev)) {
			klist_iter_exit(&i);
			return dev;
		}
	}
	klist_iter_exit(&i);
	return NULL;
}
EXPORT_SYMBOL_GPL(subsys_find_device_by_id);

static struct device_driver *next_driver(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);
	struct driver_private *drv_priv;

	if (n) {
		drv_priv = container_of(n, struct driver_private, knode_bus);
		return drv_priv->driver;
	}
	return NULL;
}

/**
 * bus_for_each_drv - driver iterator
 * @bus: bus we're dealing with.
 * @start: driver to start iterating on.
 * @data: data to pass to the callback.
 * @fn: function to call for each driver.
 *
 * This is nearly identical to the device iterator above.
 * We iterate over each driver that belongs to @bus, and call
 * @fn for each. If @fn returns anything but 0, we break out
 * and return it. If @start is not NULL, we use it as the head
 * of the list.
 *
 * NOTE: we don't return the driver that returns a non-zero
 * value, nor do we leave the reference count incremented for that
 * driver. If the caller needs to know that info, it must set it
 * in the callback. It must also be sure to increment the refcount
 * so it doesn't disappear before returning to the caller.
 */
/** 20150905
 * bus에 등록된 driver 리스트를 start부터 순회하며 fn() 함수를 수행한다.
 * error 값이 0이 아닐 경우 iterate를 중단하고 결과를 리턴한다.
 **/
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start,
		     void *data, int (*fn)(struct device_driver *, void *))
{
	struct klist_iter i;
	struct device_driver *drv;
	int error = 0;

	if (!bus)
		return -EINVAL;

	/** 20150905
	 * klist_drivers를 iterate하기 위해 klist_iter를 초기화 한다.
	 * klist_iter로 순회하며 fn()을 호출해 driver가 data(device)를 구동 가능한지
	 * 검사한다.
	 *
	 * 정상적으로 probe 되었다면 error가 1로 리턴되어 iterate가 종료된다.
	 **/
	klist_iter_init_node(&bus->p->klist_drivers, &i,
			     start ? &start->p->knode_bus : NULL);
	while ((drv = next_driver(&i)) && !error)
		error = fn(drv, data);
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(bus_for_each_drv);

/** 20150905
 * device에 bus attribute를 추가한다.
 **/
static int device_add_attrs(struct bus_type *bus, struct device *dev)
{
	int error = 0;
	int i;

	if (!bus->dev_attrs)
		return 0;

	for (i = 0; attr_name(bus->dev_attrs[i]); i++) {
		error = device_create_file(dev, &bus->dev_attrs[i]);
		if (error) {
			while (--i >= 0)
				device_remove_file(dev, &bus->dev_attrs[i]);
			break;
		}
	}
	return error;
}

static void device_remove_attrs(struct bus_type *bus, struct device *dev)
{
	int i;

	if (bus->dev_attrs) {
		for (i = 0; attr_name(bus->dev_attrs[i]); i++)
			device_remove_file(dev, &bus->dev_attrs[i]);
	}
}

/**
 * bus_add_device - add device to bus
 * @dev: device being added
 *
 * - Add device's bus attributes.
 * - Create links to device's bus.
 * - Add the device to its bus's list of devices.
 */
/** 20150905
 * device를 bus에 추가한다.
 * - device에 bus attribute를 추가한다.
 * - bus와 device에 각각 심볼릭 링크를 추가한다.
 * - bus에 속한 디바이스 리스트의 마지막에 디바이스를 추가한다.
 *
 * ex) devices/subsystem/2-2/subsystem -> ../../../../../../bus/usb
 **/
int bus_add_device(struct device *dev)
{
	struct bus_type *bus = bus_get(dev->bus);
	int error = 0;

	/** 20150905
	 * device의 bus가 존재하면 bus 에 등록한다.
	 **/
	if (bus) {
		pr_debug("bus: '%s': add device %s\n", bus->name, dev_name(dev));
		/** 20150905
		 * device에 bus attribute들을 추가한다.
		 **/
		error = device_add_attrs(bus, dev);
		if (error)
			goto out_put;
		/** 20150905
		 * bus 디렉토리에 device에 대한 심블릭 링크를 device 이름으로 생성하고,
		 * 디바이스 디렉토리에 bus에 대한 심볼릭 링크를 "subsystem"으로 생성한다.
		 **/
		error = sysfs_create_link(&bus->p->devices_kset->kobj,
						&dev->kobj, dev_name(dev));
		if (error)
			goto out_id;
		error = sysfs_create_link(&dev->kobj,
				&dev->bus->p->subsys.kobj, "subsystem");
		if (error)
			goto out_subsys;
		/** 20150905
		 * bus의 klist_devices 리스트의 마지막에 device를 노드로 추가한다.
		 **/
		klist_add_tail(&dev->p->knode_bus, &bus->p->klist_devices);
	}
	return 0;

out_subsys:
	sysfs_remove_link(&bus->p->devices_kset->kobj, dev_name(dev));
out_id:
	device_remove_attrs(bus, dev);
out_put:
	bus_put(dev->bus);
	return error;
}

/**
 * bus_probe_device - probe drivers for a new device
 * @dev: device to probe
 *
 * - Automatically probe for a driver if the bus allows it.
 */
/** 20150905
 * 새로운 디바이스를 구동하기 위해 driver를 probe한다.
 *
 * bus가 autoprobe로 되어 있다면 driver를 attach한다.
 * driver attach 여부와 상관 없이 bus의 인터페이스 함수들에 대해 add_dev() 한다.
 **/
void bus_probe_device(struct device *dev)
{
	struct bus_type *bus = dev->bus;
	struct subsys_interface *sif;
	int ret;

	/** 20150905
	 * device가 bus에 연결되지 않았다면 리턴.
	 **/
	if (!bus)
		return;

	/** 20150905
	 * bus가 driver를 autoprobe 하도록 설정되어 있다면
	 * device를 구동가능한 드라이버를 찾고 bind한다.
	 **/
	if (bus->p->drivers_autoprobe) {
		ret = device_attach(dev);
		WARN_ON(ret < 0);
	}

	mutex_lock(&bus->p->mutex);
	/** 20150905
	 * bus에 등록된 인터페이스 리스트들을 순회하며
	 * 각각의 add_dev 콜백을 호출한다.
	 **/
	list_for_each_entry(sif, &bus->p->interfaces, node)
		if (sif->add_dev)
			sif->add_dev(dev, sif);
	mutex_unlock(&bus->p->mutex);
}

/**
 * bus_remove_device - remove device from bus
 * @dev: device to be removed
 *
 * - Remove device from all interfaces.
 * - Remove symlink from bus' directory.
 * - Delete device from bus's list.
 * - Detach from its driver.
 * - Drop reference taken in bus_add_device().
 */
void bus_remove_device(struct device *dev)
{
	struct bus_type *bus = dev->bus;
	struct subsys_interface *sif;

	if (!bus)
		return;

	mutex_lock(&bus->p->mutex);
	list_for_each_entry(sif, &bus->p->interfaces, node)
		if (sif->remove_dev)
			sif->remove_dev(dev, sif);
	mutex_unlock(&bus->p->mutex);

	sysfs_remove_link(&dev->kobj, "subsystem");
	sysfs_remove_link(&dev->bus->p->devices_kset->kobj,
			  dev_name(dev));
	device_remove_attrs(dev->bus, dev);
	if (klist_node_attached(&dev->p->knode_bus))
		klist_del(&dev->p->knode_bus);

	pr_debug("bus: '%s': remove device %s\n",
		 dev->bus->name, dev_name(dev));
	device_release_driver(dev);
	bus_put(dev->bus);
}

/** 20151121
 * 드라이버에 버스용 드라이버 attribute들을 추가한다.
 **/
static int driver_add_attrs(struct bus_type *bus, struct device_driver *drv)
{
	int error = 0;
	int i;

	/** 20151121
	 * bus의 드라이버 attribute 항목이 존재하면 driver에 추가한다.
	 **/
	if (bus->drv_attrs) {
		for (i = 0; attr_name(bus->drv_attrs[i]); i++) {
			error = driver_create_file(drv, &bus->drv_attrs[i]);
			if (error)
				goto err;
		}
	}
done:
	return error;
err:
	while (--i >= 0)
		driver_remove_file(drv, &bus->drv_attrs[i]);
	goto done;
}

static void driver_remove_attrs(struct bus_type *bus,
				struct device_driver *drv)
{
	int i;

	if (bus->drv_attrs) {
		for (i = 0; attr_name(bus->drv_attrs[i]); i++)
			driver_remove_file(drv, &bus->drv_attrs[i]);
	}
}

#ifdef CONFIG_HOTPLUG
/*
 * Thanks to drivers making their tables __devinit, we can't allow manual
 * bind and unbind from userspace unless CONFIG_HOTPLUG is enabled.
 */
/** 20151121
 * 드라이버에 bind/unbind attribute 파일을 생성한다.
 **/
static int __must_check add_bind_files(struct device_driver *drv)
{
	int ret;

	ret = driver_create_file(drv, &driver_attr_unbind);
	if (ret == 0) {
		ret = driver_create_file(drv, &driver_attr_bind);
		if (ret)
			driver_remove_file(drv, &driver_attr_unbind);
	}
	return ret;
}

static void remove_bind_files(struct device_driver *drv)
{
	driver_remove_file(drv, &driver_attr_bind);
	driver_remove_file(drv, &driver_attr_unbind);
}

/** 20150912
 * 매크로로 BUS attribute 정의
 * struct bus_attribute bus_attr_drivers_probe
 * struct bus_attribute bus_attr_drivers_autoprobe
 *
 * ex) "/sys/bus/usb" 아래 drivers_probe, drivers_autoprobe 생성
 **/
static BUS_ATTR(drivers_probe, S_IWUSR, NULL, store_drivers_probe);
static BUS_ATTR(drivers_autoprobe, S_IWUSR | S_IRUGO,
		show_drivers_autoprobe, store_drivers_autoprobe);

/** 20150912
 * 버스에 drivers_probe, drivers_autoprobe 파일을 생성한다.
 **/
static int add_probe_files(struct bus_type *bus)
{
	int retval;

	retval = bus_create_file(bus, &bus_attr_drivers_probe);
	if (retval)
		goto out;

	retval = bus_create_file(bus, &bus_attr_drivers_autoprobe);
	if (retval)
		bus_remove_file(bus, &bus_attr_drivers_probe);
out:
	return retval;
}

static void remove_probe_files(struct bus_type *bus)
{
	bus_remove_file(bus, &bus_attr_drivers_autoprobe);
	bus_remove_file(bus, &bus_attr_drivers_probe);
}
#else
static inline int add_bind_files(struct device_driver *drv) { return 0; }
static inline void remove_bind_files(struct device_driver *drv) {}
static inline int add_probe_files(struct bus_type *bus) { return 0; }
static inline void remove_probe_files(struct bus_type *bus) {}
#endif

static ssize_t driver_uevent_store(struct device_driver *drv,
				   const char *buf, size_t count)
{
	enum kobject_action action;

	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&drv->p->kobj, action);
	return count;
}
static DRIVER_ATTR(uevent, S_IWUSR, NULL, driver_uevent_store);

/**
 * bus_add_driver - Add a driver to the bus.
 * @drv: driver.
 */
/** 20151121
 * 버스에 드라이버를 추가한다.
 * 추가 후 autoprobe 가능하면 바로 probe까지 진행한다.
 **/
int bus_add_driver(struct device_driver *drv)
{
	struct bus_type *bus;
	struct driver_private *priv;
	int error = 0;

	bus = bus_get(drv->bus);
	if (!bus)
		return -EINVAL;

	pr_debug("bus: '%s': add driver %s\n", bus->name, drv->name);

	/** 20151121
	 * driver_private 구조체 할당.
	 **/
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		error = -ENOMEM;
		goto out_put_bus;
	}
	/** 20151121
	 * 드라이버를 사용하는 devices 리스트 초기화.
	 * 드라이버와 드라이버 private 서로 연결.
	 * 드라이버의 kset은 버스에 해당하는 드라이버 kset.
	 **/
	klist_init(&priv->klist_devices, NULL, NULL);
	priv->driver = drv;
	drv->p = priv;
	priv->kobj.kset = bus->p->drivers_kset;
	/** 20151121
	 * kobject를 초기화 하고 추가한다.
	 **/
	error = kobject_init_and_add(&priv->kobj, &driver_ktype, NULL,
				     "%s", drv->name);
	if (error)
		goto out_unregister;

	/** 20151121
	 * 버스가 driver autoprobe로 설정되어 있다면 attach 시킨다.
	 **/
	if (drv->bus->p->drivers_autoprobe) {
		error = driver_attach(drv);
		if (error)
			goto out_unregister;
	}
	/** 20151121
	 * bus의 driver klist에 드라이버를 추가한다.
	 **/
	klist_add_tail(&priv->knode_bus, &bus->p->klist_drivers);
	module_add_driver(drv->owner, drv);

	/** 20151121
	 * 드라이버에 uevent attribute를 추가한다.
	 **/
	error = driver_create_file(drv, &driver_attr_uevent);
	if (error) {
		printk(KERN_ERR "%s: uevent attr (%s) failed\n",
			__func__, drv->name);
	}
	/** 20151121
	 * 드라이버에 버스용 드라이버 attribute 들을 추가한다.
	 **/
	error = driver_add_attrs(bus, drv);
	if (error) {
		/* How the hell do we get out of this pickle? Give up */
		printk(KERN_ERR "%s: driver_add_attrs(%s) failed\n",
			__func__, drv->name);
	}

	/** 20151121
	 * 드라이버의 sysfs를 통한 bind/unbind를 막지 않았다면 bind/unbind 파일을 추가
	 **/
	if (!drv->suppress_bind_attrs) {
		error = add_bind_files(drv);
		if (error) {
			/* Ditto */
			printk(KERN_ERR "%s: add_bind_files(%s) failed\n",
				__func__, drv->name);
		}
	}

	return 0;

out_unregister:
	kobject_put(&priv->kobj);
	kfree(drv->p);
	drv->p = NULL;
out_put_bus:
	bus_put(bus);
	return error;
}

/**
 * bus_remove_driver - delete driver from bus's knowledge.
 * @drv: driver.
 *
 * Detach the driver from the devices it controls, and remove
 * it from its bus's list of drivers. Finally, we drop the reference
 * to the bus we took in bus_add_driver().
 */
void bus_remove_driver(struct device_driver *drv)
{
	if (!drv->bus)
		return;

	if (!drv->suppress_bind_attrs)
		remove_bind_files(drv);
	driver_remove_attrs(drv->bus, drv);
	driver_remove_file(drv, &driver_attr_uevent);
	klist_remove(&drv->p->knode_bus);
	pr_debug("bus: '%s': remove driver %s\n", drv->bus->name, drv->name);
	driver_detach(drv);
	module_remove_driver(drv);
	kobject_put(&drv->p->kobj);
	bus_put(drv->bus);
}

/* Helper for bus_rescan_devices's iter */
static int __must_check bus_rescan_devices_helper(struct device *dev,
						  void *data)
{
	int ret = 0;

	if (!dev->driver) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
		ret = device_attach(dev);
		if (dev->parent)
			device_unlock(dev->parent);
	}
	return ret < 0 ? ret : 0;
}

/**
 * bus_rescan_devices - rescan devices on the bus for possible drivers
 * @bus: the bus to scan.
 *
 * This function will look for devices on the bus with no driver
 * attached and rescan it against existing drivers to see if it matches
 * any by calling device_attach() for the unbound devices.
 */
int bus_rescan_devices(struct bus_type *bus)
{
	return bus_for_each_dev(bus, NULL, NULL, bus_rescan_devices_helper);
}
EXPORT_SYMBOL_GPL(bus_rescan_devices);

/**
 * device_reprobe - remove driver for a device and probe for a new driver
 * @dev: the device to reprobe
 *
 * This function detaches the attached driver (if any) for the given
 * device and restarts the driver probing process.  It is intended
 * to use if probing criteria changed during a devices lifetime and
 * driver attachment should change accordingly.
 */
int device_reprobe(struct device *dev)
{
	if (dev->driver) {
		if (dev->parent)        /* Needed for USB */
			device_lock(dev->parent);
		device_release_driver(dev);
		if (dev->parent)
			device_unlock(dev->parent);
	}
	return bus_rescan_devices_helper(dev, NULL);
}
EXPORT_SYMBOL_GPL(device_reprobe);

/**
 * find_bus - locate bus by name.
 * @name: name of bus.
 *
 * Call kset_find_obj() to iterate over list of buses to
 * find a bus by name. Return bus if found.
 *
 * Note that kset_find_obj increments bus' reference count.
 */
#if 0
struct bus_type *find_bus(char *name)
{
	struct kobject *k = kset_find_obj(bus_kset, name);
	return k ? to_bus(k) : NULL;
}
#endif  /*  0  */


/**
 * bus_add_attrs - Add default attributes for this bus.
 * @bus: Bus that has just been registered.
 */

/** 20150912
 * 버스에 대한 기본 attribute를 파일로 추가한다.
 **/
static int bus_add_attrs(struct bus_type *bus)
{
	int error = 0;
	int i;

	/** 20150912
	 * 버스의 default attribute들을 순회하며 각 attribute를 파일로 생성한다.
	 **/
	if (bus->bus_attrs) {
		for (i = 0; attr_name(bus->bus_attrs[i]); i++) {
			error = bus_create_file(bus, &bus->bus_attrs[i]);
			if (error)
				goto err;
		}
	}
done:
	return error;
err:
	while (--i >= 0)
		bus_remove_file(bus, &bus->bus_attrs[i]);
	goto done;
}

static void bus_remove_attrs(struct bus_type *bus)
{
	int i;

	if (bus->bus_attrs) {
		for (i = 0; attr_name(bus->bus_attrs[i]); i++)
			bus_remove_file(bus, &bus->bus_attrs[i]);
	}
}

static void klist_devices_get(struct klist_node *n)
{
	struct device_private *dev_prv = to_device_private_bus(n);
	struct device *dev = dev_prv->device;

	get_device(dev);
}

static void klist_devices_put(struct klist_node *n)
{
	struct device_private *dev_prv = to_device_private_bus(n);
	struct device *dev = dev_prv->device;

	put_device(dev);
}

static ssize_t bus_uevent_store(struct bus_type *bus,
				const char *buf, size_t count)
{
	enum kobject_action action;

	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&bus->p->subsys.kobj, action);
	return count;
}
static BUS_ATTR(uevent, S_IWUSR, NULL, bus_uevent_store);

/**
 * __bus_register - register a driver-core subsystem
 * @bus: bus to register
 * @key: lockdep class key
 *
 * Once we have that, we register the bus with the kobject
 * infrastructure, then register the children subsystems it has:
 * the devices and drivers that belong to the subsystem.
 */
/** 20150912
 * 버스를 드라이버 코어 서브시스템에 등록한다.
 *
 * 내부 관리용 subsys_private을 생성하고, subsys를 초기화 한다.
 * 속성 파일과 probe 파일을 생성한다.
 **/
int __bus_register(struct bus_type *bus, struct lock_class_key *key)
{
	int retval;
	struct subsys_private *priv;

	/** 20150905
	 * subsys_private를 위한 메모리를 할당 받는다.
	 **/
	priv = kzalloc(sizeof(struct subsys_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/** 20150905
	 * bus와 subsys가 서로를 가리키도록 연결한다.
	 **/
	priv->bus = bus;
	bus->p = priv;

	/** 20150905
	 * bus notifier 를 등록할 리스트를 초기화 한다.
	 **/
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->bus_notifier);

	/** 20150905
	 * subsys_private의 kobject에 버스 이름을 등록한다.
	 **/
	retval = kobject_set_name(&priv->subsys.kobj, "%s", bus->name);
	if (retval)
		goto out;

	/** 20150905
	 * subsys_private의 subsys kset의 정보를 채운다.
	 * 각 bus의 kset은 /sys/bus로 나타나는 전역 bus_kset이다.
	 **/
	priv->subsys.kobj.kset = bus_kset;
	priv->subsys.kobj.ktype = &bus_ktype;
	priv->drivers_autoprobe = 1;

	/** 20150905
	 * kset 구조체를 초기화 하고, kobject hierarchy에 등록한다.
	 **/
	retval = kset_register(&priv->subsys);
	if (retval)
		goto out;

	/** 20150905
	 * bus 아래 bus_attr_uevent 파일을 생성한다.
	 *
	 * bus_attr_uevent는 BUS_ATTR(uevent, ...) 매크로롤 생성되는 심볼.
	 **/
	retval = bus_create_file(bus, &bus_attr_uevent);
	if (retval)
		goto bus_uevent_fail;

	/** 20150905
	 * bus subsys 아래 "devices" kset을 생성하고 devices_kset에 저장한다.
	 * 예 "/sys/bus/i2c/devices"
	 **/
	priv->devices_kset = kset_create_and_add("devices", NULL,
						 &priv->subsys.kobj);
	if (!priv->devices_kset) {
		retval = -ENOMEM;
		goto bus_devices_fail;
	}

	/** 20150905
	 * bus subsys 아래 "drivers" kset을 생성하고 devices_kset에 저장한다.
	 * 예 "/sys/bus/i2c/drivers"
	 **/
	priv->drivers_kset = kset_create_and_add("drivers", NULL,
						 &priv->subsys.kobj);
	if (!priv->drivers_kset) {
		retval = -ENOMEM;
		goto bus_drivers_fail;
	}

	/** 20150905
	 * subsys interfaces 리스트를 초기화 한다.
	 * mutex를 초기화 한다.
	 * klist_devices, klist_drivers 리스트를 초기화 한다.
	 **/
	INIT_LIST_HEAD(&priv->interfaces);
	__mutex_init(&priv->mutex, "subsys mutex", key);
	klist_init(&priv->klist_devices, klist_devices_get, klist_devices_put);
	klist_init(&priv->klist_drivers, NULL, NULL);

	/** 20150912
	 * bus에 probe 파일을 생성한다.
	 **/
	retval = add_probe_files(bus);
	if (retval)
		goto bus_probe_files_fail;

	/** 20150912
	 * 버스에 기본 attribute를 생성한다.
	 **/
	retval = bus_add_attrs(bus);
	if (retval)
		goto bus_attrs_fail;

	pr_debug("bus: '%s': registered\n", bus->name);
	return 0;

bus_attrs_fail:
	remove_probe_files(bus);
bus_probe_files_fail:
	kset_unregister(bus->p->drivers_kset);
bus_drivers_fail:
	kset_unregister(bus->p->devices_kset);
bus_devices_fail:
	bus_remove_file(bus, &bus_attr_uevent);
bus_uevent_fail:
	kset_unregister(&bus->p->subsys);
out:
	kfree(bus->p);
	bus->p = NULL;
	return retval;
}
EXPORT_SYMBOL_GPL(__bus_register);

/**
 * bus_unregister - remove a bus from the system
 * @bus: bus.
 *
 * Unregister the child subsystems and the bus itself.
 * Finally, we call bus_put() to release the refcount
 */
void bus_unregister(struct bus_type *bus)
{
	pr_debug("bus: '%s': unregistering\n", bus->name);
	if (bus->dev_root)
		device_unregister(bus->dev_root);
	bus_remove_attrs(bus);
	remove_probe_files(bus);
	kset_unregister(bus->p->drivers_kset);
	kset_unregister(bus->p->devices_kset);
	bus_remove_file(bus, &bus_attr_uevent);
	kset_unregister(&bus->p->subsys);
	kfree(bus->p);
	bus->p = NULL;
}
EXPORT_SYMBOL_GPL(bus_unregister);

int bus_register_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_register_notifier);

int bus_unregister_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_unregister_notifier);

struct kset *bus_get_kset(struct bus_type *bus)
{
	return &bus->p->subsys;
}
EXPORT_SYMBOL_GPL(bus_get_kset);

struct klist *bus_get_device_klist(struct bus_type *bus)
{
	return &bus->p->klist_devices;
}
EXPORT_SYMBOL_GPL(bus_get_device_klist);

/*
 * Yes, this forcibly breaks the klist abstraction temporarily.  It
 * just wants to sort the klist, not change reference counts and
 * take/drop locks rapidly in the process.  It does all this while
 * holding the lock for the list, so objects can't otherwise be
 * added/removed while we're swizzling.
 */
static void device_insertion_sort_klist(struct device *a, struct list_head *list,
					int (*compare)(const struct device *a,
							const struct device *b))
{
	struct list_head *pos;
	struct klist_node *n;
	struct device_private *dev_prv;
	struct device *b;

	list_for_each(pos, list) {
		n = container_of(pos, struct klist_node, n_node);
		dev_prv = to_device_private_bus(n);
		b = dev_prv->device;
		if (compare(a, b) <= 0) {
			list_move_tail(&a->p->knode_bus.n_node,
				       &b->p->knode_bus.n_node);
			return;
		}
	}
	list_move_tail(&a->p->knode_bus.n_node, list);
}

void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b))
{
	LIST_HEAD(sorted_devices);
	struct list_head *pos, *tmp;
	struct klist_node *n;
	struct device_private *dev_prv;
	struct device *dev;
	struct klist *device_klist;

	device_klist = bus_get_device_klist(bus);

	spin_lock(&device_klist->k_lock);
	list_for_each_safe(pos, tmp, &device_klist->k_list) {
		n = container_of(pos, struct klist_node, n_node);
		dev_prv = to_device_private_bus(n);
		dev = dev_prv->device;
		device_insertion_sort_klist(dev, &sorted_devices, compare);
	}
	list_splice(&sorted_devices, &device_klist->k_list);
	spin_unlock(&device_klist->k_lock);
}
EXPORT_SYMBOL_GPL(bus_sort_breadthfirst);

/**
 * subsys_dev_iter_init - initialize subsys device iterator
 * @iter: subsys iterator to initialize
 * @subsys: the subsys we wanna iterate over
 * @start: the device to start iterating from, if any
 * @type: device_type of the devices to iterate over, NULL for all
 *
 * Initialize subsys iterator @iter such that it iterates over devices
 * of @subsys.  If @start is set, the list iteration will start there,
 * otherwise if it is NULL, the iteration starts at the beginning of
 * the list.
 */
void subsys_dev_iter_init(struct subsys_dev_iter *iter, struct bus_type *subsys,
			  struct device *start, const struct device_type *type)
{
	struct klist_node *start_knode = NULL;

	if (start)
		start_knode = &start->p->knode_bus;
	klist_iter_init_node(&subsys->p->klist_devices, &iter->ki, start_knode);
	iter->type = type;
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_init);

/**
 * subsys_dev_iter_next - iterate to the next device
 * @iter: subsys iterator to proceed
 *
 * Proceed @iter to the next device and return it.  Returns NULL if
 * iteration is complete.
 *
 * The returned device is referenced and won't be released till
 * iterator is proceed to the next device or exited.  The caller is
 * free to do whatever it wants to do with the device including
 * calling back into subsys code.
 */
struct device *subsys_dev_iter_next(struct subsys_dev_iter *iter)
{
	struct klist_node *knode;
	struct device *dev;

	for (;;) {
		knode = klist_next(&iter->ki);
		if (!knode)
			return NULL;
		dev = container_of(knode, struct device_private, knode_bus)->device;
		if (!iter->type || iter->type == dev->type)
			return dev;
	}
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_next);

/**
 * subsys_dev_iter_exit - finish iteration
 * @iter: subsys iterator to finish
 *
 * Finish an iteration.  Always call this function after iteration is
 * complete whether the iteration ran till the end or not.
 */
void subsys_dev_iter_exit(struct subsys_dev_iter *iter)
{
	klist_iter_exit(&iter->ki);
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_exit);

int subsys_interface_register(struct subsys_interface *sif)
{
	struct bus_type *subsys;
	struct subsys_dev_iter iter;
	struct device *dev;

	if (!sif || !sif->subsys)
		return -ENODEV;

	subsys = bus_get(sif->subsys);
	if (!subsys)
		return -EINVAL;

	mutex_lock(&subsys->p->mutex);
	list_add_tail(&sif->node, &subsys->p->interfaces);
	if (sif->add_dev) {
		subsys_dev_iter_init(&iter, subsys, NULL, NULL);
		while ((dev = subsys_dev_iter_next(&iter)))
			sif->add_dev(dev, sif);
		subsys_dev_iter_exit(&iter);
	}
	mutex_unlock(&subsys->p->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(subsys_interface_register);

void subsys_interface_unregister(struct subsys_interface *sif)
{
	struct bus_type *subsys;
	struct subsys_dev_iter iter;
	struct device *dev;

	if (!sif || !sif->subsys)
		return;

	subsys = sif->subsys;

	mutex_lock(&subsys->p->mutex);
	list_del_init(&sif->node);
	if (sif->remove_dev) {
		subsys_dev_iter_init(&iter, subsys, NULL, NULL);
		while ((dev = subsys_dev_iter_next(&iter)))
			sif->remove_dev(dev, sif);
		subsys_dev_iter_exit(&iter);
	}
	mutex_unlock(&subsys->p->mutex);

	bus_put(subsys);
}
EXPORT_SYMBOL_GPL(subsys_interface_unregister);

/** 20150912
 * system의 root device release 콜백.
 **/
static void system_root_device_release(struct device *dev)
{
	kfree(dev);
}
/**
 * subsys_system_register - register a subsystem at /sys/devices/system/
 * @subsys: system subsystem
 * @groups: default attributes for the root device
 *
 * All 'system' subsystems have a /sys/devices/system/<name> root device
 * with the name of the subsystem. The root device can carry subsystem-
 * wide attributes. All registered devices are below this single root
 * device and are named after the subsystem with a simple enumeration
 * number appended. The registered devices are not explicitely named;
 * only 'id' in the device needs to be set.
 *
 * Do not use this interface for anything new, it exists for compatibility
 * with bad ideas only. New subsystems should use plain subsystems; and
 * add the subsystem-wide attributes should be added to the subsystem
 * directory itself and not some create fake root-device placed in
 * /sys/devices/system/<name>.
 */
/** 20150912
 * bus를 "/sys/bus" 아래 등록한다.
 * "/sys/devices/system/" 아래 등록한다.
 **/
int subsys_system_register(struct bus_type *subsys,
			   const struct attribute_group **groups)
{
	struct device *dev;
	int err;

	/** 20150912
	 * subsys 버스를 등록한다.
	 **/
	err = bus_register(subsys);
	if (err < 0)
		return err;

	/** 20150912
	 * device를 위한 메모리를 할당 받는다.
	 **/
	dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err_dev;
	}

	/** 20150912
	 * device 이름을 bus의 이름으로 설정한다.
	 **/
	err = dev_set_name(dev, "%s", subsys->name);
	if (err < 0)
		goto err_name;

	/** 20150912
	 * system을 새로 추가된 system 디바이스의 parent으로 지정한다.
	 * 매개변수로 넘어온 attributes 그룹을 디바이스에 지정한다.
	 * device의 release 함수를 지정한다.
	 **/
	dev->kobj.parent = &system_kset->kobj;
	dev->groups = groups;
	dev->release = system_root_device_release;

	/** 20150912
	 * 위에서 설정한 디바이스를 "/sys/devices/system/"아래 추가한다.
	 **/
	err = device_register(dev);
	if (err < 0)
		goto err_dev_reg;

	/** 20150912
	 * 버스와 동일한 이름으로 생성한 디바이스를 dev_root로 지정한다.
	 **/
	subsys->dev_root = dev;
	return 0;

err_dev_reg:
	put_device(dev);
	dev = NULL;
err_name:
	kfree(dev);
err_dev:
	bus_unregister(subsys);
	return err;
}
EXPORT_SYMBOL_GPL(subsys_system_register);

/** 20150829
 * device의 bus관련 kset을 추가한다.
 *
 * system은 왜 buses_init에서 해주는 것일까???
 **/
int __init buses_init(void)
{
	/** 20150829
	 * "bus" kset을 생성해 sysfs에 등록한다.
	 * "/sys/bus"
	 *
	 * uevent ops가 별도로 지정된다.
	 **/
	bus_kset = kset_create_and_add("bus", &bus_uevent_ops, NULL);
	if (!bus_kset)
		return -ENOMEM;

	/** 20150829
	 * "system" kset을 생성해 sysfs에 등록한다.
	 * "/sys/devices/system"
	 **/
	system_kset = kset_create_and_add("system", NULL, &devices_kset->kobj);
	if (!system_kset)
		return -ENOMEM;

	return 0;
}
