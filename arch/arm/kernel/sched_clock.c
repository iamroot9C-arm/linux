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
/** 20150103
 * sched_clock_timer를 정의한다.
 *
 * 콜백 함수는 sched_clock_poll이다.
 **/
static DEFINE_TIMER(sched_clock_timer, sched_clock_poll, 0, 0);

/** 20140426
 * clock data.
 *
 * 한 clock tick이 몇 ns인지 환산.
 * HZ           : CONFIG_HZ에 따라 100.
 * NSEC_PER_SEC : 10^9
 *
 * setup_sched_clock에서 나머지 구조체 설정.
 **/
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
 * read_sched_clock 콜백함수.
 *
 * versatile_sched_clock_init()에서
 * setup_sched_clock로 지정한 함수 versatile_read_sched_clock가 등록.
 **/
static u32 __read_mostly (*read_sched_clock)(void) = jiffy_sched_clock_read;

static inline u64 cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

/** 20140426
 * cyc 값을 sched_clock을 기준으로 한 ns로 변환.
 *
 * epoch_ns가 갱신된 이후 얼마의 ns가 흘렀는지 더해 리턴.
 **/
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
		/** 20140426
		 * clock_data로부터 설정된 epoch_cyc, epoch_ns 값을 읽어온다.
		 **/
		epoch_cyc = cd.epoch_cyc;
		smp_rmb();
		epoch_ns = cd.epoch_ns;
		smp_rmb();
	} while (epoch_cyc != cd.epoch_cyc_copy);

	/** 20140426
	 * epoch_ns + cyc값을 ns으로 변환한 값을 더해 sched clock 값을 리턴
	 **/
	return epoch_ns + cyc_to_ns((cyc - epoch_cyc) & mask, cd.mult, cd.shift);
}

/*
 * Atomically update the sched_clock epoch.
 */
/** 20130601
 * 전역 clock_data에 epoch_cyc,epoch_ns,epoch_cyc_copy를 갱신한다. 
 **/
static void notrace update_sched_clock(void)
{
	unsigned long flags;
	u32 cyc;
	u64 ns;
	/** 20130601
	 * sched_clock 값을 읽어오기 위해 등록한 콜백함수를 호출해 값을 읽어온다.
	 *
	 * versatile_sched_clock_init에서 setup_sched_clock을 부를때
	 * versatile_read_sched_clock 함수 포인터를 주고 read_sched_clock에 저장된다.
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

/** 20150103
 * wrap_ticks 이후에 timer가 만료되도록 sched_clock_timer를 설정한다.
 * sched_clock 관련 자료구조를 update한다.
 **/
static void sched_clock_poll(unsigned long wrap_ticks)
{
	/** 20150103
	 * sched_clock_timer의 timeout 시간을 변경한다.
	 * 현재 jiffies에 wrap_ticks를 더한 시간에 만료되도록 한다.
	 **/
	mod_timer(&sched_clock_timer, round_jiffies(jiffies + wrap_ticks));
	/** 20150103
	 * sched_clock으로 사용되는 epoch_XX를 갱신한다.
	 **/
	update_sched_clock();
}

/** 20130601
 * 1.clock_data의 mult,shift 값을 구한후 이값으로 한 사이클의 소요시간(ns)을 구한다.
 * 2.프리퀀시 단위 설정
 * 3.sched_clock_timer의 data을 설장
 * 4.clock_data 값 설정
 * 리눅스에서 사용할 스캐쥴링 clock 기준값을 설정
 *
 * 리눅스에서는 64비트 ns 타이머하고 arm에서는 32 비트 TCNT(cycle 단위)를 사용한다.
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

	/* calculate the mult/shift to convert counter ticks to ns. */
	/** 20130601
	 * #define NSEC_PER_SEC	1000 000 000L
	 *
	 * rate : 24000000
	 * #define HZ 100
	 * static struct clock_data cd = {
	 * .mult	= NSEC_PER_SEC / HZ,
	 * };
	 *
	 *  24MHz 는 Counter Register가 1증가 또는 감소하는 주파수이고, 1GHz는 clock source로 삼기 위한 ns 단위 resolution입니다.
	 *    1GHz resolution clock을 사용하려면 24MHz로 동작하는 counter로 몇 번 곱하고, shift 시켜줘야 하는지 구합니다.
	 **/
	clocks_calc_mult_shift(&cd.mult, &cd.shift, rate, NSEC_PER_SEC, 0);

	/** 20130601
	 * 주파수의 단위를 나타내는 r_unit을 설정
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
	 * bits : 32
	 **/
	/* calculate how many ns until we wrap */
	/** 20130601
	 * example
	 * mult : 0xa6aa_aaab
	 * shift : 0x1a
	 * wrap = ((0x1_0000_0000 - 1)*mult)>>shift;
	 *
	 * wrap : cycle의 갯수가 0xffffffff가 될 때까지 걸리는 cycle을 ns로 변환한 값
	 **/
	wrap = cyc_to_ns((1ULL << bits) - 1, cd.mult, cd.shift);
	/** 20130601
	 * nsec 기준의 wrap을 msec로 변환 
	 **/
	do_div(wrap, NSEC_PER_MSEC);
	w = wrap;

	/* calculate the ns resolution of this counter */
	/** 20150103
	 * 1 cyc에 소요되는 ns.
	 **/
	res = cyc_to_ns(1ULL, cd.mult, cd.shift);
	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lums\n",
		bits, r, r_unit, res, w);

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	/** 20130601
	 * timer_list 의 data 값에 저장
	 *
	 * data : 리눅스에서는 full 64-bit ns counter 사용하는데 
	 * vexpress의경우 기준 clock으로  24Mhz 를 사용하므로
	 * OXFFFFFFFF 의 사이클을 수행했을경우의 (OVERFLOW되었을 경우)
	 * 보정해주는 값으로 사용???
	 **/
	sched_clock_timer.data = msecs_to_jiffies(w - (w / 10));
	/** 20150103
	 * sched_clock 관련 전역변수를 갱신한다.
	 **/
	update_sched_clock();
	/*
	 * Ensure that sched_clock() starts off at 0ns
	 */
	/** 20130601
	 * update_sched_clock 에서 cd.epoch_ns = ns 를 해주는데 바로 다시 0를 세팅해주는 이유는???
	 *   -> update_schec_clock은 sched_clock값을 변경할 때마다 호출하는 함수.
	 *      초기값을 0으로 잡아 기준점으로 함.
	 **/
	cd.epoch_ns = 0;

	pr_debug("Registered %pF as sched_clock source\n", read);
}

/** 20140426
 * 시스템이 시작된 후, 몇 ns가 지났는지 리턴한다.
 *
 * sched clock 레지스터를 읽어 ns 단위의 값으로 리턴.
 **/
unsigned long long notrace sched_clock(void)
{
	/** 20140426
	 * scheduling clock 값을 읽어온다.
	 * read_sched_clock는 machine에 따라 정의된다.
	 **/
	u32 cyc = read_sched_clock();
	return cyc_to_sched_clock(cyc, sched_clock_mask);
}

/** 20150103
 * sched_clock을 새로 읽고, timer를 동록시킨다.
 **/
void __init sched_clock_postinit(void)
{
	/*
	 * If no sched_clock function has been provided at that point,
	 * make it the final one one.
	 */
	/** 20150103
	 * read_sched_clock가 초기값이라면,
	 * 즉, sched_clock를 읽기 위한 콜백으로 어떠한 함수도 등록되지 않았다면
	 * jiffy_sched_clock_read를 등록한다.
	 **/
	if (read_sched_clock == jiffy_sched_clock_read)
		setup_sched_clock(jiffy_sched_clock_read, 32, HZ);

	/** 20150103
	 * setup_sched_clock에서 sched_clock_timer.data를 jiffies로 계산해 넣었다.
	 *
	 * sched_clock_timer를 등록시키고 시작시킨다.
	 **/
	sched_clock_poll(sched_clock_timer.data);
}

/** 20151003
 * sched_clock이 suspend될 때, sched_clock timer를 재설정해
 * resume 되고 data 시간 후 scheduling이 되도록 한다.
 **/
static int sched_clock_suspend(void)
{
	sched_clock_poll(sched_clock_timer.data);
	return 0;
}

/** 20151003
 * sched clock ops.
 **/
static struct syscore_ops sched_clock_ops = {
	.suspend = sched_clock_suspend,
};

/** 20151003
 * syscore ops에 sched_clock ops를 등록한다.
 **/
static int __init sched_clock_syscore_init(void)
{
	register_syscore_ops(&sched_clock_ops);
	return 0;
}
device_initcall(sched_clock_syscore_init);
