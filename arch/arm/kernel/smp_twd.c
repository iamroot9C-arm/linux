/*
 *  linux/arch/arm/kernel/smp_twd.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/smp.h>
#include <linux/jiffies.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>

#include <asm/smp_twd.h>
#include <asm/localtimer.h>
#include <asm/hardware/gic.h>

/* set up by the platform code */
static void __iomem *twd_base;

static struct clk *twd_clk;
static unsigned long twd_timer_rate;

/** 20140913    
 * clock event device 선언.
 **/
static struct clock_event_device __percpu **twd_evt;
static int twd_ppi;

static void twd_set_mode(enum clock_event_mode mode,
			struct clock_event_device *clk)
{
	unsigned long ctrl;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		/* timer load already set up */
		ctrl = TWD_TIMER_CONTROL_ENABLE | TWD_TIMER_CONTROL_IT_ENABLE
			| TWD_TIMER_CONTROL_PERIODIC;
		__raw_writel(twd_timer_rate / HZ, twd_base + TWD_TIMER_LOAD);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		/* period set, and timer enabled in 'next_event' hook */
		ctrl = TWD_TIMER_CONTROL_IT_ENABLE | TWD_TIMER_CONTROL_ONESHOT;
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		ctrl = 0;
	}

	__raw_writel(ctrl, twd_base + TWD_TIMER_CONTROL);
}

static int twd_set_next_event(unsigned long evt,
			struct clock_event_device *unused)
{
	unsigned long ctrl = __raw_readl(twd_base + TWD_TIMER_CONTROL);

	ctrl |= TWD_TIMER_CONTROL_ENABLE;

	__raw_writel(evt, twd_base + TWD_TIMER_COUNTER);
	__raw_writel(ctrl, twd_base + TWD_TIMER_CONTROL);

	return 0;
}

/*
 * local_timer_ack: checks for a local timer interrupt.
 *
 * If a local timer interrupt has occurred, acknowledge and return 1.
 * Otherwise, return 0.
 */
/** 20140920    
 * timer watchdog interrupt가 떴다면 1로 ack를 주고, 1을 리턴한다.
 **/
static int twd_timer_ack(void)
{
	if (__raw_readl(twd_base + TWD_TIMER_INTSTAT)) {
		__raw_writel(1, twd_base + TWD_TIMER_INTSTAT);
		return 1;
	}

	return 0;
}

static void twd_timer_stop(struct clock_event_device *clk)
{
	twd_set_mode(CLOCK_EVT_MODE_UNUSED, clk);
	disable_percpu_irq(clk->irq);
}

#ifdef CONFIG_CPU_FREQ

/*
 * Updates clockevent frequency when the cpu frequency changes.
 * Called on the cpu that is changing frequency with interrupts disabled.
 */
static void twd_update_frequency(void *data)
{
	twd_timer_rate = clk_get_rate(twd_clk);

	clockevents_update_freq(*__this_cpu_ptr(twd_evt), twd_timer_rate);
}

static int twd_cpufreq_transition(struct notifier_block *nb,
	unsigned long state, void *data)
{
	struct cpufreq_freqs *freqs = data;

	/*
	 * The twd clock events must be reprogrammed to account for the new
	 * frequency.  The timer is local to a cpu, so cross-call to the
	 * changing cpu.
	 */
	if (state == CPUFREQ_POSTCHANGE || state == CPUFREQ_RESUMECHANGE)
		smp_call_function_single(freqs->cpu, twd_update_frequency,
			NULL, 1);

	return NOTIFY_OK;
}

static struct notifier_block twd_cpufreq_nb = {
	.notifier_call = twd_cpufreq_transition,
};

static int twd_cpufreq_init(void)
{
	if (twd_evt && *__this_cpu_ptr(twd_evt) && !IS_ERR(twd_clk))
		return cpufreq_register_notifier(&twd_cpufreq_nb,
			CPUFREQ_TRANSITION_NOTIFIER);

	return 0;
}
core_initcall(twd_cpufreq_init);

#endif

/** 20150606    
 * twd를 직접 조작해 twd_timer_rate를 계산한다.
 **/
static void __cpuinit twd_calibrate_rate(void)
{
	unsigned long count;
	u64 waitjiffies;

	/*
	 * If this is the first time round, we need to work out how fast
	 * the timer ticks
	 */
	/** 20150606    
	 * local timer를 설정하기 위해 TWD TIMER COUNTER를 최대치로 설정해두고,
	 * 5개 jiffies 뒤에 읽어 지나간 COUNTER값을 환산해 rate를 계산한다.
	 **/
	if (twd_timer_rate == 0) {
		printk(KERN_INFO "Calibrating local timer... ");

		/* Wait for a tick to start */
		waitjiffies = get_jiffies_64() + 1;

		while (get_jiffies_64() < waitjiffies)
			udelay(10);

		/* OK, now the tick has started, let's get the timer going */
		waitjiffies += 5;

				 /* enable, no interrupt or reload */
		__raw_writel(0x1, twd_base + TWD_TIMER_CONTROL);

				 /* maximum value */
		__raw_writel(0xFFFFFFFFU, twd_base + TWD_TIMER_COUNTER);

		while (get_jiffies_64() < waitjiffies)
			udelay(10);

		count = __raw_readl(twd_base + TWD_TIMER_COUNTER);

		twd_timer_rate = (0xFFFFFFFFU - count) * (HZ / 5);

		/** 20150606     
		 * vexpress on QEMU...
		 * Calibrating local timer... 97.32MHz.
		 **/
		printk("%lu.%02luMHz.\n", twd_timer_rate / 1000000,
			(twd_timer_rate / 10000) % 100);
	}
}

/** 20140920    
 * twd interrupt handler.
 * clock_event_device에 지정한 event_handler를 호출한다.
 **/
static irqreturn_t twd_handler(int irq, void *dev_id)
{
	/** 20140920    
	 * clock event device를 받아온다.
	 **/
	struct clock_event_device *evt = *(struct clock_event_device **)dev_id;

	/** 20140920    
	 * twd interrupt 발생시 응답하고, event_handler를 호출해 interrupt를 처리한다.
	 * 
	 * event_handler는 tick_handle_periodic.
	 **/
	if (twd_timer_ack()) {
		evt->event_handler(evt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

/** 20150606    
 * "smp_twd"를 위한 clock을 준비하고 공급한다.
 **/
static struct clk *twd_get_clock(void)
{
	struct clk *clk;
	int err;

	/** 20150606    
	 * "smp_twd"으로 등록된 struct clk을 찾아온다.
	 **/
	clk = clk_get_sys("smp_twd", NULL);
	if (IS_ERR(clk)) {
		pr_err("smp_twd: clock not found: %d\n", (int)PTR_ERR(clk));
		return clk;
	}

	/** 20150606    
	 * clkops의 prepare 함수를 호출한다.
	 **/
	err = clk_prepare(clk);
	if (err) {
		pr_err("smp_twd: clock failed to prepare: %d\n", err);
		clk_put(clk);
		return ERR_PTR(err);
	}

	/** 20150606    
	 * clkops의 enable 함수를 호출한다.
	 **/
	err = clk_enable(clk);
	if (err) {
		pr_err("smp_twd: clock failed to enable: %d\n", err);
		clk_unprepare(clk);
		clk_put(clk);
		return ERR_PTR(err);
	}

	return clk;
}

/*
 * Setup the local clock events for a CPU.
 */
/** 20150606    
 * twd timer를 setup한다.
 *
 * clock_event_device를 설정하고 등록한다.
 **/
static int __cpuinit twd_timer_setup(struct clock_event_device *clk)
{
	struct clock_event_device **this_cpu_clk;

	/** 20150606    
	 * twd_clk이 초기화되지 않았으면 twd_get_clock으로 clock 값을 읽어와 설정한다.
	 **/
	if (!twd_clk)
		twd_clk = twd_get_clock();

	/** 20150606    
	 * twd에 해당하는 struct clk 구조체를 가져왔다면 get_rate를 호출하고,
	 * 그렇지 않다면 calibrate rate를 통해 설정한다.
	 **/
	if (!IS_ERR_OR_NULL(twd_clk))
		twd_timer_rate = clk_get_rate(twd_clk);
	else
		twd_calibrate_rate();

	__raw_writel(0, twd_base + TWD_TIMER_CONTROL);

	/** 20150606    
	 * clock_event_device 구조체를 설정한 뒤 percpu 변수 twd_evt에 저장한다.
	 *
	 * cat /proc/timer_list로 확인
	 **/
	clk->name = "local_timer";
	clk->features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT |
			CLOCK_EVT_FEAT_C3STOP;
	clk->rating = 350;
	clk->set_mode = twd_set_mode;
	clk->set_next_event = twd_set_next_event;
	/** 20150613    
	 * twd_local_timer_common_register에서 등록한 percpu irq.
	 **/
	clk->irq = twd_ppi;

	this_cpu_clk = __this_cpu_ptr(twd_evt);
	*this_cpu_clk = clk;

	/** 20150606    
	 * clockevent를 설정하고 등록한다.
	 **/
	clockevents_config_and_register(clk, twd_timer_rate,
					0xf, 0xffffffff);
	/** 20150606    
	 * percpu interrupt를 활성화 한다.
	 **/
	enable_percpu_irq(clk->irq, 0);

	return 0;
}

/** 20140920    
 * timer watchdog local timer operations.
 **/
static struct local_timer_ops twd_lt_ops __cpuinitdata = {
	.setup	= twd_timer_setup,
	.stop	= twd_timer_stop,
};

/** 20140920    
 * twd를 percpu irq로 등록하고, local timer로 등록한다.
 **/
static int __init twd_local_timer_common_register(void)
{
	int err;

	/** 20140913    
	 * clock_event_device용 percpu 변수 할당.
	 **/
	twd_evt = alloc_percpu(struct clock_event_device *);
	if (!twd_evt) {
		err = -ENOMEM;
		goto out_free;
	}

	/** 20140920    
	 * percpu irq로 twd_ppi(IRQ_LOCALTIMER, 29) 등록.
	 * handler는 twd_handler
	 * dev_id는 twd_evt
	 *
	 * cat /proc/interrupts에서 확인 가능.
	 **/
	err = request_percpu_irq(twd_ppi, twd_handler, "twd", twd_evt);
	if (err) {
		pr_err("twd: can't register interrupt %d (%d)\n", twd_ppi, err);
		goto out_free;
	}

	/** 20140920    
	 * twd_lt_ops를 local timer operations (lt_ops)로 지정한다.
	 **/
	err = local_timer_register(&twd_lt_ops);
	if (err)
		goto out_irq;

	return 0;

out_irq:
	free_percpu_irq(twd_ppi, twd_evt);
out_free:
	iounmap(twd_base);
	twd_base = NULL;
	free_percpu(twd_evt);

	return err;
}

/** 20140920    
 * twd local timer를 등록한다.
 **/
int __init twd_local_timer_register(struct twd_local_timer *tlt)
{
	if (twd_base || twd_evt)
		return -EBUSY;

	/** 20140913    
	 * twd irq resource를 가져와 twd_ppi에 저장한다.
	 **/
	twd_ppi	= tlt->res[1].start;

	/** 20140913    
	 * twd mem resource에 지정된 메모리를 page table에 매핑.
	 **/
	twd_base = ioremap(tlt->res[0].start, resource_size(&tlt->res[0]));
	if (!twd_base)
		return -ENOMEM;

	/** 20140920    
	 * twd를 local timer로 등록한다.
	 *   - interrupt 등록(핸들러 지정)
	 *   - register 설정
	 **/
	return twd_local_timer_common_register();
}

#ifdef CONFIG_OF
const static struct of_device_id twd_of_match[] __initconst = {
	{ .compatible = "arm,cortex-a9-twd-timer",	},
	{ .compatible = "arm,cortex-a5-twd-timer",	},
	{ .compatible = "arm,arm11mp-twd-timer",	},
	{ },
};

void __init twd_local_timer_of_register(void)
{
	struct device_node *np;
	int err;

	np = of_find_matching_node(NULL, twd_of_match);
	if (!np) {
		err = -ENODEV;
		goto out;
	}

	twd_ppi = irq_of_parse_and_map(np, 0);
	if (!twd_ppi) {
		err = -EINVAL;
		goto out;
	}

	twd_base = of_iomap(np, 0);
	if (!twd_base) {
		err = -ENOMEM;
		goto out;
	}

	err = twd_local_timer_common_register();

out:
	WARN(err, "twd_local_timer_of_register failed (%d)\n", err);
}
#endif
