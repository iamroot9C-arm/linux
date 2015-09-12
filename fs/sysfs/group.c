/*
 * fs/sysfs/group.c - Operations for adding/removing multiple files at once.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released undert the GPL v2. 
 *
 */

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/err.h>
#include "sysfs.h"


static void remove_files(struct sysfs_dirent *dir_sd, struct kobject *kobj,
			 const struct attribute_group *grp)
{
	struct attribute *const* attr;
	int i;

	for (i = 0, attr = grp->attrs; *attr; i++, attr++)
		sysfs_hash_and_remove(dir_sd, NULL, (*attr)->name);
}

/** 20150905    
 * 디렉토리 sysfs dirent에 attribute group을 파일로 각각 추가한다.
 **/
static int create_files(struct sysfs_dirent *dir_sd, struct kobject *kobj,
			const struct attribute_group *grp, int update)
{
	struct attribute *const* attr;
	int error = 0, i;

	/** 20150905    
	 * attribute group의 attribute를 각각 파일로 생성한다.
	 **/
	for (i = 0, attr = grp->attrs; *attr && !error; i++, attr++) {
		umode_t mode = 0;

		/* in update mode, we're changing the permissions or
		 * visibility.  Do this by first removing then
		 * re-adding (if required) the file */
		/** 20150905    
		 * update라면 기존 name을 가지는 sysfs dirent를 찾아 제거한다.
		 * is_visible이 정의되어 있다면 콜백을 호출해 결정된 mode를 사용한다.
		 **/
		if (update)
			sysfs_hash_and_remove(dir_sd, NULL, (*attr)->name);
		if (grp->is_visible) {
			mode = grp->is_visible(kobj, *attr, i);
			if (!mode)
				continue;
		}
		/** 20150905    
		 * 디렉토리 sysfs dirent에 파일을 지정한 mode로 추가한다.
		 **/
		error = sysfs_add_file_mode(dir_sd, *attr, SYSFS_KOBJ_ATTR,
					    (*attr)->mode | mode);
		if (unlikely(error))
			break;
	}
	if (error)
		remove_files(dir_sd, kobj, grp);
	return error;
}


/** 20150905    
 * kobject 에 attribute 그룹을 sysfs dirent 파일로 생성한다.
 **/
static int internal_create_group(struct kobject *kobj, int update,
				 const struct attribute_group *grp)
{
	struct sysfs_dirent *sd;
	int error;

	BUG_ON(!kobj || (!update && !kobj->sd));

	/* Updates may happen before the object has been instantiated */
	if (unlikely(update && !kobj->sd))
		return -EINVAL;
	if (!grp->attrs) {
		WARN(1, "sysfs: attrs not set by subsystem for group: %s/%s\n",
			kobj->name, grp->name ? "" : grp->name);
		return -EINVAL;
	}
	/** 20150905    
	 * attribute 그룹에 이름이 존재하면 하위디렉토리를 생성하고,
	 * 생성된 sysfs dirent가 sd에 저장되어 리턴된다.
	 **/
	if (grp->name) {
		error = sysfs_create_subdir(kobj, grp->name, &sd);
		if (error)
			return error;
	} else
		sd = kobj->sd;
	/** 20150905    
	 * sysfs dirent의 권한을 획득하고,
	 * 디렉토리 아래 attribute 그룹 속성 파일을 생성한다.
	 **/
	sysfs_get(sd);
	error = create_files(sd, kobj, grp, update);
	if (error) {
		if (grp->name)
			sysfs_remove_subdir(sd);
	}
	sysfs_put(sd);
	return error;
}

/**
 * sysfs_create_group - given a directory kobject, create an attribute group
 * @kobj:	The kobject to create the group on
 * @grp:	The attribute group to create
 *
 * This function creates a group for the first time.  It will explicitly
 * warn and error if any of the attribute files being created already exist.
 *
 * Returns 0 on success or error.
 */
/** 20150905    
 * 디렉토리 kobject에 attribute 그룹을 생성한다.
 **/
int sysfs_create_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	return internal_create_group(kobj, 0, grp);
}

/**
 * sysfs_update_group - given a directory kobject, update an attribute group
 * @kobj:	The kobject to update the group on
 * @grp:	The attribute group to update
 *
 * This function updates an attribute group.  Unlike
 * sysfs_create_group(), it will explicitly not warn or error if any
 * of the attribute files being created already exist.  Furthermore,
 * if the visibility of the files has changed through the is_visible()
 * callback, it will update the permissions and add or remove the
 * relevant files.
 *
 * The primary use for this function is to call it after making a change
 * that affects group visibility.
 *
 * Returns 0 on success or error.
 */
int sysfs_update_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	return internal_create_group(kobj, 1, grp);
}



void sysfs_remove_group(struct kobject * kobj, 
			const struct attribute_group * grp)
{
	struct sysfs_dirent *dir_sd = kobj->sd;
	struct sysfs_dirent *sd;

	if (grp->name) {
		sd = sysfs_get_dirent(dir_sd, NULL, grp->name);
		if (!sd) {
			WARN(!sd, KERN_WARNING "sysfs group %p not found for "
				"kobject '%s'\n", grp, kobject_name(kobj));
			return;
		}
	} else
		sd = sysfs_get(dir_sd);

	remove_files(sd, kobj, grp);
	if (grp->name)
		sysfs_remove_subdir(sd);

	sysfs_put(sd);
}

/**
 * sysfs_merge_group - merge files into a pre-existing attribute group.
 * @kobj:	The kobject containing the group.
 * @grp:	The files to create and the attribute group they belong to.
 *
 * This function returns an error if the group doesn't exist or any of the
 * files already exist in that group, in which case none of the new files
 * are created.
 */
/** 20150905    
 * 이미 attribute 그룹을 보유 중인 kobj에 새로운 attribute 그룹을 추가한다.
 **/
int sysfs_merge_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	struct sysfs_dirent *dir_sd;
	int error = 0;
	struct attribute *const *attr;
	int i;

	/** 20150905    
	 * kobj->sd 아래에서 group name을 갖는 sysfs dirent를 받아온다.
	 * merge는 이미 그룹이 존재해야 하기 때문에 존재하지 않는 경우 에러.
	 **/
	dir_sd = sysfs_get_dirent(kobj->sd, NULL, grp->name);
	if (!dir_sd)
		return -ENOENT;

	/** 20150905    
	 * merge시킬 group의 attribute를 sysfs에 추가한다.
	 **/
	for ((i = 0, attr = grp->attrs); *attr && !error; (++i, ++attr))
		error = sysfs_add_file(dir_sd, *attr, SYSFS_KOBJ_ATTR);
	if (error) {
		while (--i >= 0)
			sysfs_hash_and_remove(dir_sd, NULL, (*--attr)->name);
	}
	/** 20150905    
	 * 획득했던 접근을 해제한다.
	 **/
	sysfs_put(dir_sd);

	return error;
}
EXPORT_SYMBOL_GPL(sysfs_merge_group);

/**
 * sysfs_unmerge_group - remove files from a pre-existing attribute group.
 * @kobj:	The kobject containing the group.
 * @grp:	The files to remove and the attribute group they belong to.
 */
void sysfs_unmerge_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	struct sysfs_dirent *dir_sd;
	struct attribute *const *attr;

	dir_sd = sysfs_get_dirent(kobj->sd, NULL, grp->name);
	if (dir_sd) {
		for (attr = grp->attrs; *attr; ++attr)
			sysfs_hash_and_remove(dir_sd, NULL, (*attr)->name);
		sysfs_put(dir_sd);
	}
}
EXPORT_SYMBOL_GPL(sysfs_unmerge_group);


EXPORT_SYMBOL_GPL(sysfs_create_group);
EXPORT_SYMBOL_GPL(sysfs_update_group);
EXPORT_SYMBOL_GPL(sysfs_remove_group);
