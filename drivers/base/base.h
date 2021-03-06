#include <linux/notifier.h>

/**
 * struct subsys_private - structure to hold the private to the driver core portions of the bus_type/class structure.
 *
 * @subsys - the struct kset that defines this subsystem
 * @devices_kset - the subsystem's 'devices' directory
 * @interfaces - list of subsystem interfaces associated
 * @mutex - protect the devices, and interfaces lists.
 *
 * @drivers_kset - the list of drivers associated
 * @klist_devices - the klist to iterate over the @devices_kset
 * @klist_drivers - the klist to iterate over the @drivers_kset
 * @bus_notifier - the bus notifier list for anything that cares about things
 *                 on this bus.
 * @bus - pointer back to the struct bus_type that this structure is associated
 *        with.
 *
 * @glue_dirs - "glue" directory to put in-between the parent device to
 *              avoid namespace conflicts
 * @class - pointer back to the struct class that this structure is associated
 *          with.
 *
 * This structure is the one that is the actual kobject allowing struct
 * bus_type/class to be statically allocated safely.  Nothing outside of the
 * driver core should ever touch these fields.
 */
/** 20150905
 * driver core에 대한 private data를 저장하는 자료구조.
 *	bus 구조체와 class 구조체 등은 공통으로 subsys_private 구조체를 가지고 있다.
 *
 * subsys : 이 subsystem에 대한 kset.
 * mutex : devices와 interfaces 리스트를 보호한다.
 *
 * drivers_kset : 이 subsys에 등록된 driver들의 kset.
 **/
struct subsys_private {
	struct kset subsys;
	struct kset *devices_kset;
	struct list_head interfaces;
	struct mutex mutex;

	struct kset *drivers_kset;
	struct klist klist_devices;
	struct klist klist_drivers;
	struct blocking_notifier_head bus_notifier;
	unsigned int drivers_autoprobe:1;
	struct bus_type *bus;

	struct kset glue_dirs;
	struct class *class;
};
#define to_subsys_private(obj) container_of(obj, struct subsys_private, subsys.kobj)

/** 20151121
 * driver_private 구조체에 kobject가 존재한다.
 *
 * .klist_devices : 이 드라이버와 연결된 디바이스 klist.
 * .knode_bus : bus 연결 knode.
 * .driver : 이 device_driver와 서로 연결된 driver 구조체
 **/
struct driver_private {
	struct kobject kobj;
	struct klist klist_devices;
	struct klist_node knode_bus;
	struct module_kobject *mkobj;
	struct device_driver *driver;
};
/** 20151121
 * kobject를 포함하는 driver_private 구조체를 리턴한다.
 **/
#define to_driver(obj) container_of(obj, struct driver_private, kobj)

/**
 * struct device_private - structure to hold the private to the driver core portions of the device structure.
 *
 * @klist_children - klist containing all children of this device
 * @knode_parent - node in sibling list
 * @knode_driver - node in driver list
 * @knode_bus - node in bus list
 * @deferred_probe - entry in deferred_probe_list which is used to retry the
 *	binding of drivers which were unable to get all the resources needed by
 *	the device; typically because it depends on another driver getting
 *	probed first.
 * @driver_data - private pointer for driver specific info.  Will turn into a
 * list soon.
 * @device - pointer back to the struct class that this structure is
 * associated with.
 *
 * Nothing outside of the driver core should ever touch these fields.
 */
/** 20150829
 * device 구조체에서 driver에 관련된 private 자료구조를 저장하는 구조체.
 *
 * klist_node는 각 리스트에 연결하기 위한 entry point.
 * .klist_children : 이 디바이스의 children이 엮인 klist
 * .device : 이 privates 구조체가 연결된 device 구조체를 가리킨다.
 **/
struct device_private {
	struct klist klist_children;
	struct klist_node knode_parent;
	struct klist_node knode_driver;
	struct klist_node knode_bus;
	struct list_head deferred_probe;
	void *driver_data;
	struct device *device;
};
/** 20150912
 * klist에 연결하기 위한 각 knode를 받아 device_private 자료구조를 찾아오는 매크로
 **/
#define to_device_private_parent(obj)	\
	container_of(obj, struct device_private, knode_parent)
#define to_device_private_driver(obj)	\
	container_of(obj, struct device_private, knode_driver)
#define to_device_private_bus(obj)	\
	container_of(obj, struct device_private, knode_bus)

extern int device_private_init(struct device *dev);

/* initialisation functions */
extern int devices_init(void);
extern int buses_init(void);
extern int classes_init(void);
extern int firmware_init(void);
#ifdef CONFIG_SYS_HYPERVISOR
extern int hypervisor_init(void);
#else
/** 20150829
 * CONFIG_SYS_HYPERVISOR가 정의되지 않음.
 **/
static inline int hypervisor_init(void) { return 0; }
#endif
extern int platform_bus_init(void);
extern void cpu_dev_init(void);

extern int bus_add_device(struct device *dev);
extern void bus_probe_device(struct device *dev);
extern void bus_remove_device(struct device *dev);

extern int bus_add_driver(struct device_driver *drv);
extern void bus_remove_driver(struct device_driver *drv);

extern void driver_detach(struct device_driver *drv);
extern int driver_probe_device(struct device_driver *drv, struct device *dev);
extern void driver_deferred_probe_del(struct device *dev);
/** 20150905
 * 드라이버와 디바이스가 매치되는지 검사해 match한다면 
 *
 * driver 버스에 match 함수가 존재하면 호출해 결과 리턴. 그렇지 않다면 1 리턴.
 * match된다면 nonzero가 리턴된다.
 **/
static inline int driver_match_device(struct device_driver *drv,
				      struct device *dev)
{
	return drv->bus->match ? drv->bus->match(dev, drv) : 1;
}

extern char *make_class_name(const char *name, struct kobject *kobj);

extern int devres_release_all(struct device *dev);

/* /sys/devices directory */
extern struct kset *devices_kset;

#if defined(CONFIG_MODULES) && defined(CONFIG_SYSFS)
extern void module_add_driver(struct module *mod, struct device_driver *drv);
extern void module_remove_driver(struct device_driver *drv);
#else
static inline void module_add_driver(struct module *mod,
				     struct device_driver *drv) { }
static inline void module_remove_driver(struct device_driver *drv) { }
#endif

#ifdef CONFIG_DEVTMPFS
extern int devtmpfs_init(void);
#else
/** 20150822
 * CONIFG_DEVTMPFS가 정의되어 있지 않다.
 **/
static inline int devtmpfs_init(void) { return 0; }
#endif
