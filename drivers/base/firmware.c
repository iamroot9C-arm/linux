/*
 * firmware.c - firmware subsystem hoohaw.
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007 Novell Inc.
 *
 * This file is released under the GPLv2
 */
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>

#include "base.h"

/** 20150829
 * firmware_init()에서 kobject를 할당받아 저장한다.
 **/
struct kobject *firmware_kobj;
EXPORT_SYMBOL_GPL(firmware_kobj);

/** 20150829
 * firmware kobject를 생성하고 구조에 추가한다.
 * "/sys/firmware" 생성
 **/
int __init firmware_init(void)
{
	firmware_kobj = kobject_create_and_add("firmware", NULL);
	if (!firmware_kobj)
		return -ENOMEM;
	return 0;
}
