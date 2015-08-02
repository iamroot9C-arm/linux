/*
 *  linux/arch/arm/plat-versatile/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/hardware/gic.h>

/*
 * control for which core is the next to come out of the secondary
 * boot "holding pen"
 */
volatile int __cpuinitdata pen_release = -1;

/*
 * Write pen_release in a way that is guaranteed to be visible to all
 * observers, irrespective of whether they're taking part in coherency
 * or not.  This is necessary for the hotplug code to work reliably.
 */
/** 20150118    
 * pen_release에 val로 넘어온 cpu번호를 쓰고, cache를 flush한다.
 *
 * 다른 core에서 실행 중인 versatile_secondary_startup에서 pen_release에
 * 자신의 cpu번호가 쓰일 때까지 check한다.
 *
 * coherency에 관계없이 다른 observers에게 보여지도록 보장한다.
 **/
static void __cpuinit write_pen_release(int val)
{
	pen_release = val;
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static DEFINE_SPINLOCK(boot_lock);

void __cpuinit platform_secondary_init(unsigned int cpu)
{
	/*
	 * if any interrupts are already enabled for the primary
	 * core (e.g. timer irq), then they will not have been enabled
	 * for us: do so
	 */
	gic_secondary_init(0);

	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	/** 20150124    
	 * pen_release에 -1을 넣어 primary processor (boot cpu)에서
	 * 다음 부분을 수행한다.
	 **/
	write_pen_release(-1);

	/*
	 * Synchronise with the boot thread.
	 */
	/** 20150124    
	 * pen_release 이후 boot thread가 먼저 걸어둔 spinlock을 해제하고 진행한다.
	 **/
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

/** 20150124    
 * 특정 cpu를 pen_release 방식으로 깨우고,
 * 깨어날 때까지 timeout을 두고 대기한다.
 **/
int __cpuinit boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one
	 */
	spin_lock(&boot_lock);

	/*
	 * This is really belt and braces; we hold unintended secondary
	 * CPUs in the holding pen until we're ready for them.  However,
	 * since we haven't sent them a soft interrupt, they shouldn't
	 * be there.
	 */
	/** 20150124    
	 * 깨울 cpu에 해당하는 물리번호를 가져와 pen_release 위치에 쓴다.
	 * 쓰고 나서 cache를 clean 시킨다.
	 **/
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */
	/** 20150124    
	 * 깨울 cpu로 cpumask로 생성해 그 mask에 gic를 통해 irq 0(SGI)을 날린다.
	 **/
	gic_raise_softirq(cpumask_of(cpu), 0);

	/** 20150124    
	 * 깨어난 cpu가 init 과정을 마치고 pen_release에 -1을 넣어줄 때까지 대기한다.
	 **/
	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();
		if (pen_release == -1)
			break;

		udelay(10);
	}

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	spin_unlock(&boot_lock);

	/** 20150801    
	 * pen_release가 -1이면 up시킬 cpu가 정상 부팅이 된 것이다.
	 **/
	return pen_release != -1 ? -ENOSYS : 0;
}
