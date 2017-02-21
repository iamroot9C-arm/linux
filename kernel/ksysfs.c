/*
 * kernel/ksysfs.c - sysfs attributes in /sys/kernel, which
 * 		     are not related to any other subsystem
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 * 
 * This file is release under the GPLv2
 *
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/profile.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/capability.h>

/** 20151010
 * KERNEL 관련 read-only attribute를 선언한다.
 **/
#define KERNEL_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0644, _name##_show, _name##_store)

#if defined(CONFIG_HOTPLUG)
/* current uevent sequence number */
static ssize_t uevent_seqnum_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)uevent_seqnum);
}
KERNEL_ATTR_RO(uevent_seqnum);

/* uevent helper program, used during early boot */
static ssize_t uevent_helper_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", uevent_helper);
}
static ssize_t uevent_helper_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (count+1 > UEVENT_HELPER_PATH_LEN)
		return -ENOENT;
	memcpy(uevent_helper, buf, count);
	uevent_helper[count] = '\0';
	if (count && uevent_helper[count-1] == '\n')
		uevent_helper[count-1] = '\0';
	return count;
}
KERNEL_ATTR_RW(uevent_helper);
#endif

#ifdef CONFIG_PROFILING
static ssize_t profiling_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", prof_on);
}
static ssize_t profiling_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;

	if (prof_on)
		return -EEXIST;
	/*
	 * This eventually calls into get_option() which
	 * has a ton of callers and is not const.  It is
	 * easiest to cast it away here.
	 */
	profile_setup((char *)buf);
	ret = profile_init();
	if (ret)
		return ret;
	ret = create_proc_profile();
	if (ret)
		return ret;
	return count;
}
KERNEL_ATTR_RW(profiling);
#endif

#ifdef CONFIG_KEXEC
static ssize_t kexec_loaded_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", !!kexec_image);
}
KERNEL_ATTR_RO(kexec_loaded);

static ssize_t kexec_crash_loaded_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", !!kexec_crash_image);
}
KERNEL_ATTR_RO(kexec_crash_loaded);

static ssize_t kexec_crash_size_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%zu\n", crash_get_memory_size());
}
static ssize_t kexec_crash_size_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long cnt;
	int ret;

	if (strict_strtoul(buf, 0, &cnt))
		return -EINVAL;

	ret = crash_shrink_memory(cnt);
	return ret < 0 ? ret : count;
}
KERNEL_ATTR_RW(kexec_crash_size);

static ssize_t vmcoreinfo_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lx %x\n",
		       paddr_vmcoreinfo_note(),
		       (unsigned int)vmcoreinfo_max_size);
}
KERNEL_ATTR_RO(vmcoreinfo);

#endif /* CONFIG_KEXEC */

/* whether file capabilities are enabled */
static ssize_t fscaps_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", file_caps_enabled);
}
KERNEL_ATTR_RO(fscaps);

/*
 * Make /sys/kernel/notes give the raw contents of our kernel .notes section.
 */
/** 20151010
 * kernel의 .notes 섹션을 binary로 제공하는 "/sys/kernel/notes" 파일을 정의한다.
 **/
extern const void __start_notes __attribute__((weak));
extern const void __stop_notes __attribute__((weak));
#define	notes_size (&__stop_notes - &__start_notes)

/** 20151010
 * elf 섹션 중 "notes"를 buffer에 복사한다.
 **/
static ssize_t notes_read(struct file *filp, struct kobject *kobj,
			  struct bin_attribute *bin_attr,
			  char *buf, loff_t off, size_t count)
{
	memcpy(buf, &__start_notes + off, count);
	return count;
}

/** 20151010
 * "notes"라는 이름의 bin_attribute를 선언한다.
 *
 * attribute용 read 함수는 notes_read로 지정한다.
 *
 * http://kernelnewbies.org/vmlinux/asm-notes
 * https://lwn.net/Articles/531148/
 **/
static struct bin_attribute notes_attr = {
	.attr = {
		.name = "notes",
		.mode = S_IRUGO,
	},
	.read = &notes_read,
};

struct kobject *kernel_kobj;
EXPORT_SYMBOL_GPL(kernel_kobj);

/** 20151010
 * kernel 관련 struct attribute 배열.
 *
 * 각 attribute는 KERNEL_ATTR_RW(), KERNEL_ATTR_RO() 등으로 선언한
 * kobj_attribute의 attribute이다.
 **/
static struct attribute * kernel_attrs[] = {
	&fscaps_attr.attr,
#if defined(CONFIG_HOTPLUG)
	&uevent_seqnum_attr.attr,
	&uevent_helper_attr.attr,
#endif
#ifdef CONFIG_PROFILING
	&profiling_attr.attr,
#endif
	/** 20151010
	 * kexec: 현재 커널을 shutdown 시키고, 새로운 kernel을 시작하는 기법.
	 *
	 * UEFI secure boot을 kexec가 우회하는 문제가 있는데, 3.17 버전부터
	 * 서명된 kernel만 올릴 수 있도록 개선되었다.
	 **/
#ifdef CONFIG_KEXEC
	&kexec_loaded_attr.attr,
	&kexec_crash_loaded_attr.attr,
	&kexec_crash_size_attr.attr,
	&vmcoreinfo_attr.attr,
#endif
	NULL
};

/** 20151010
 * kernel attribute group 설정.
 * 
 * .name이 지정되지 않았으므로 kernel 디렉토리 아래 attribute가 바로 생성된다.
 * .name이 지정된다면 해당 이름으로 디렉토리가 생성되고, attribute가 그 아래 생성.
 **/
static struct attribute_group kernel_attr_group = {
	.attrs = kernel_attrs,
};

/** 20151010
 * "kernel" kobject를 생성해 등록하고, 하위 attribute 파일들을 만든다.
 **/
static int __init ksysfs_init(void)
{
	int error;

	/** 20151010
	 * "kernel" kobject를 생성하고 sysfs에 등록한다.
	 **/
	kernel_kobj = kobject_create_and_add("kernel", NULL);
	if (!kernel_kobj) {
		error = -ENOMEM;
		goto exit;
	}
	/** 20151010
	 * "kernel" kobject아래 kernel attribute group을 등록한다.
	 **/
	error = sysfs_create_group(kernel_kobj, &kernel_attr_group);
	if (error)
		goto kset_exit;

	/** 20151010
	 * notes 섹션에 데이터가 들어 있다면 notes를 위한 bin attribute를 만든다.
	 **/
	if (notes_size > 0) {
		notes_attr.size = notes_size;
		error = sysfs_create_bin_file(kernel_kobj, &notes_attr);
		if (error)
			goto group_exit;
	}

	return 0;

group_exit:
	sysfs_remove_group(kernel_kobj, &kernel_attr_group);
kset_exit:
	kobject_put(kernel_kobj);
exit:
	return error;
}

core_initcall(ksysfs_init);
