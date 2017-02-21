/***********************************************************************
* linux/kernel/time/jiffies.c
*
* This file contains the jiffies based clocksource.
*
* Copyright (C) 2004, 2005 IBM, John Stultz (johnstul@us.ibm.com)
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
************************************************************************/
#include <linux/clocksource.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/init.h>

#include "tick-internal.h"

/* The Jiffies based clocksource is the lowest common
 * denominator clock source which should function on
 * all systems. It has the same coarse resolution as
 * the timer interrupt frequency HZ and it suffers
 * inaccuracies caused by missed or lost timer
 * interrupts and the inability for the timer
 * interrupt hardware to accuratly tick at the
 * requested HZ value. It is also not recommended
 * for "tick-less" systems.
 */
#define NSEC_PER_JIFFY	((u32)((((u64)NSEC_PER_SEC)<<8)/ACTHZ))

/* Since jiffies uses a simple NSEC_PER_JIFFY multiplier
 * conversion, the .shift value could be zero. However
 * this would make NTP adjustments impossible as they are
 * in units of 1/2^.shift. Thus we use JIFFIES_SHIFT to
 * shift both the nominator and denominator the same
 * amount, and give ntp adjustments in units of 1/2^8
 *
 * The value 8 is somewhat carefully chosen, as anything
 * larger can result in overflows. NSEC_PER_JIFFY grows as
 * HZ shrinks, so values greater than 8 overflow 32bits when
 * HZ=100.
 */
#define JIFFIES_SHIFT	8

static cycle_t jiffies_read(struct clocksource *cs)
{
	return (cycle_t) jiffies;
}

/** 20141227
 * clocksource jiffies.
 *
 * jiffies를 사용한 클럭소스는 모든 시스템에서 항상 제공하고 있다.
 * 따라서 default clock으로 사용된다.
 *
 * rating이 1로 가장 낮게 설정되어 높은 rating을 가진 clocksource를 등록한다면
 * 해당 clocksource가 사용된다.
 **/
struct clocksource clocksource_jiffies = {
	.name		= "jiffies",
	.rating		= 1, /* lowest valid rating*/
	.read		= jiffies_read,
	.mask		= 0xffffffff, /*32bits*/
	.mult		= NSEC_PER_JIFFY << JIFFIES_SHIFT, /* details above */
	.shift		= JIFFIES_SHIFT,
};

#if (BITS_PER_LONG < 64)
u64 get_jiffies_64(void)
{
	unsigned long seq;
	u64 ret;

	do {
		seq = read_seqbegin(&xtime_lock);
		ret = jiffies_64;
	} while (read_seqretry(&xtime_lock, seq));
	return ret;
}
EXPORT_SYMBOL(get_jiffies_64);
#endif

EXPORT_SYMBOL(jiffies);

/** 20151010
 * 시스템 공통으로 제공되는 jiffies를 기준으로 한 클럭소스를 등록한다.
 **/
static int __init init_jiffies_clocksource(void)
{
	return clocksource_register(&clocksource_jiffies);
}

core_initcall(init_jiffies_clocksource);

/** 20150103
 * clocksource default clock은 clocksource_jiffies이다.
 **/
struct clocksource * __init __weak clocksource_default_clock(void)
{
	return &clocksource_jiffies;
}
