/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>

#include "base.h"

/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
/** 20150912
 * 리눅스 드라이버 모델을 초기화 한다.
 **/
void __init driver_init(void)
{
	/* These are the core pieces */
	devtmpfs_init();
	/** 20150829
	 * sysfs에 kset과 kobject를 추가한다.
	 *
	 * "/sys/devices"
	 * "/sys/dev"
	 * "/sys/class", "/sys/devices/system"
	 * "/sys/firmware"
	 **/
	devices_init();
	buses_init();
	classes_init();
	firmware_init();
	hypervisor_init();

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	/** 20150912
	 * 플랫폼 버스를 추가 한다.
	 * "/sys/devices/platform"
	 * "/sys/bus/platform"
	 **/
	platform_bus_init();
	/** 20150912
	 * cpu를 버스와 디바이스에 추가 한다.
	 * "/sys/bus/cpu"
	 * "/sys/devices/system/cpu"
	 **/
	cpu_dev_init();
	memory_dev_init();
}
