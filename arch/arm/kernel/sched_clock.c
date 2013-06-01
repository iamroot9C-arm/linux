/*
 * sched_clock.c: support for extending counters to full 64-bit ns counter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/syscore_ops.h>
#include <linux/timer.h>

#include <asm/sched_clock.h>

struct clock_data {
	u64 epoch_ns;
	u32 epoch_cyc;
	u32 epoch_cyc_copy;
	u32 mult;
	u32 shift;
};

static void sched_clock_poll(unsigned long wrap_ticks);
static DEFINE_TIMER(sched_clock_timer, sched_clock_poll, 0, 0);

static struct clock_data cd = {
	.mult	= NSEC_PER_SEC / HZ,
};

static u32 __read_mostly sched_clock_mask = 0xffffffff;

static u32 notrace jiffy_sched_clock_read(void)
{
	/** 20130518    
	 * vmlinux.lds에서
	 * jiffies = jiffies_64
	 **/
	return (u32)(jiffies - INITIAL_JIFFIES);
}

/** 20130518    
 * read_sched_clock 함수 포인터 초기화
 **/
static u32 __read_mostly (*read_sched_clock)(void) = jiffy_sched_clock_read;

static inline u64 cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

static unsigned long long cyc_to_sched_clock(u32 cyc, u32 mask)
{
	u64 epoch_ns;
	u32 epoch_cyc;

	/*
	 * Load the epoch_cyc and epoch_ns atomically.  We do this by
	 * ensuring that we always write epoch_cyc, epoch_ns and
	 * epoch_cyc_copy in strict order, and read them in strict order.
	 * If epoch_cyc and epoch_cyc_copy are not equal, then we're in
	 * the middle of an update, and we should repeat the load.
	 */
	do {
		epoch_cyc = cd.epoch_cyc;
		smp_rmb();
		epoch_ns = cd.epoch_ns;
		smp_rmb();
	} while (epoch_cyc != cd.epoch_cyc_copy);

	return epoch_ns + cyc_to_ns((cyc - epoch_cyc) & mask, cd.mult, cd.shift);
}

/*
 * Atomically update the sched_clock epoch.
 */
/** 20130601
clock_data에 epoch_cyc,epoch_ns,epoch_cyc_copy를 채워준다. 
**/
static void notrace update_sched_clock(void)
{
	unsigned long flags;
	u32 cyc;
	u64 ns;
/** 20130601
versatile_sched_clock_init에서 setup_sched_clock을 부를때
versatile_read_sched_clock 함수 포인터를 주고 read_sched_clock에 저장된다.
**/
	cyc = read_sched_clock();
	ns = cd.epoch_ns +
		cyc_to_ns((cyc - cd.epoch_cyc) & sched_clock_mask,
			  cd.mult, cd.shift);
	/*
	 * Write epoch_cyc and epoch_ns in a way that the update is
	 * detectable in cyc_to_fixed_sched_clock().
	 */
	raw_local_irq_save(flags);
	cd.epoch_cyc = cyc;
	smp_wmb();
	cd.epoch_ns = ns;
	smp_wmb();
	cd.epoch_cyc_copy = cyc;
	raw_local_irq_restore(flags);
}

static void sched_clock_poll(unsigned long wrap_ticks)
{
	mod_timer(&sched_clock_timer, round_jiffies(jiffies + wrap_ticks));
	update_sched_clock();
}
/** 20130601

1.clock_data의 mult,shift 값을 구한후 이값으로 한 사이클의 소요시간(ns)
을 구한다.
2.프리퀀시 단위 설정
3.sched_clock_timer의 data을 설장
4.clock_data 값 설정
리눅스에서 사용할 스캐쥴링 clock 기준값을 설정???

리눅스에서는 64비트 ns 타이머하고 arm에서는 32 비트 TCNT(cycle 단위)를 사용한다.

**/
void __init setup_sched_clock(u32 (*read)(void), int bits, unsigned long rate)
{
	unsigned long r, w;
	u64 res, wrap;
	char r_unit;

	BUG_ON(bits > 32);
	/** 20130518    
	 * irq가 disabled 되어 있지 않을 경우 WARN()
	 **/
	WARN_ON(!irqs_disabled());
	/** 20130518    
	 * 함수 포인터 초기값이 변경되었다면 WARN()
	 **/
	WARN_ON(read_sched_clock != jiffy_sched_clock_read);
	/** 20130518    
	 * read 함수로 함수 포인터 변경.
	 *   vexpress의 경우 versatile_read_sched_clock
	 **/
	read_sched_clock = read;
	/** 20130518    
	 * bits가 32로 넘어온 경우: 0 - 1
	 **/
	sched_clock_mask = (1 << bits) - 1;

	/** 20130601 여기부터...
	 **/
	/* calculate the mult/shift to convert counter ticks to ns. */
	/** 20130601
	
	#define NSEC_PER_SEC	1000 000 000L
	
	rate : 24000000
	#define HZ 100
	static struct clock_data cd = {
	.mult	= NSEC_PER_SEC / HZ,
	};
	**/
	clocks_calc_mult_shift(&cd.mult, &cd.shift, rate, NSEC_PER_SEC, 0);

	/** 20130601
	주파수의 단위를 나타내는 r_unit을 설정
	**/
	r = rate;
	if (r >= 4000000) {
		r /= 1000000;
		r_unit = 'M';
	} else if (r >= 1000) {
		r /= 1000;
		r_unit = 'k';
	} else
		r_unit = ' ';

	/** 20130601
	bits : 32
	**/
	/* calculate how many ns until we wrap */
	/** 20130601
	example
	mult : 0xa6aa_aaab
	shift : 0x1a
	wrap = ((0x1_0000_0000 - 1)*mult)>>shift;
	
	wrap : cycle의 갯수가 0xffffffff가 되었때까지 걸리는 cycle을 ns로 변환한 값
	**/
	wrap = cyc_to_ns((1ULL << bits) - 1, cd.mult, cd.shift);
	/** 20130601
	wrap을 msec로 변환 
	**/
	do_div(wrap, NSEC_PER_MSEC);
	w = wrap;

	/* calculate the ns resolution of this counter */
	res = cyc_to_ns(1ULL, cd.mult, cd.shift);
	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lums\n",
		bits, r, r_unit, res, w);

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	/** 20130601
	timer_list 의 data 값에 저장
	
	
	data : 리눅스에서는 full 64-bit ns counter 사용하는데 
	vexpress의경우 기준 clock으로  24Mhz 를 사용하므로
	OXFFFFFFFF 의 사이클을 수행했을경우의 (OVERFLOW되었을 경우)
	보정해주는 값으로 사용???
	**/
	sched_clock_timer.data = msecs_to_jiffies(w - (w / 10));
	update_sched_clock();
	/*
	 * Ensure that sched_clock() starts off at 0ns
	 */
	/** 20130601
	update_sched_clock 에서 cd.epoch_ns = ns 를 해주는데 바로 다시 0를 세팅해주는 이유는???
	**/
	cd.epoch_ns = 0;

	pr_debug("Registered %pF as sched_clock source\n", read);
}

unsigned long long notrace sched_clock(void)
{
	u32 cyc = read_sched_clock();
	return cyc_to_sched_clock(cyc, sched_clock_mask);
}

void __init sched_clock_postinit(void)
{
	/*
	 * If no sched_clock function has been provided at that point,
	 * make it the final one one.
	 */
	if (read_sched_clock == jiffy_sched_clock_read)
		setup_sched_clock(jiffy_sched_clock_read, 32, HZ);

	sched_clock_poll(sched_clock_timer.data);
}

static int sched_clock_suspend(void)
{
	sched_clock_poll(sched_clock_timer.data);
	return 0;
}

static struct syscore_ops sched_clock_ops = {
	.suspend = sched_clock_suspend,
};

static int __init sched_clock_syscore_init(void)
{
	register_syscore_ops(&sched_clock_ops);
	return 0;
}
device_initcall(sched_clock_syscore_init);
