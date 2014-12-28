/*
 *  linux/arch/arm/common/timer-sp.c
 *
 *  Copyright (C) 1999 - 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>

#include <asm/sched_clock.h>
#include <asm/hardware/arm_timer.h>

/** 20141227    
 * name으로 등록된 clk hierarchy에서 clk 정보를 찾아 rate를 반환한다.
 *
 * - prepare
 * - enable
 * - get_rate 순서로 호출한다.
 **/
static long __init sp804_get_clock_rate(const char *name)
{
	struct clk *clk;
	long rate;
	int err;

	/** 20141220    
	 * sp804 device의 name으로 등록된 clk을 받아온다.
	 *
	 * dev_id : sp804
	 * con_id : name (v2m-timer0, v2m-timer1)
	 **/
	clk = clk_get_sys("sp804", name);
	if (IS_ERR(clk)) {
		pr_err("sp804: %s clock not found: %d\n", name,
			(int)PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	/** 20141227    
	 * clk에 등록된 clk_ops.prepare 호출.
	 **/
	err = clk_prepare(clk);
	if (err) {
		pr_err("sp804: %s clock failed to prepare: %d\n", name, err);
		clk_put(clk);
		return err;
	}

	/** 20141227    
	 * clk에 등록된 clk_ops.enable 호출.
	 **/
	err = clk_enable(clk);
	if (err) {
		pr_err("sp804: %s clock failed to enable: %d\n", name, err);
		clk_unprepare(clk);
		clk_put(clk);
		return err;
	}

	/** 20141227    
	 * clk에 등록된 rate(frequency)를 받아온다.
	 **/
	rate = clk_get_rate(clk);
	if (rate < 0) {
		pr_err("sp804: %s clock failed to get rate: %ld\n", name, rate);
		clk_disable(clk);
		clk_unprepare(clk);
		clk_put(clk);
	}

	return rate;
}

static void __iomem *sched_clock_base;

/** 20141227    
 *
 **/
static u32 sp804_read(void)
{
	return ~readl_relaxed(sched_clock_base + TIMER_VALUE);
}

/** 20141227    
 * sp804를 clocksource로 사용하기 위해 레지스터를 설정하고,
 * clocksource 구조체를 생성해 초기화 하고,
 * 마지막 argument 유무에 따라 shced_clock을 초기화한다.
 **/
void __init __sp804_clocksource_and_sched_clock_init(void __iomem *base,
						     const char *name,
						     int use_sched_clock)
{
	/** 20141227    
	 * 이미 등록한 clk hierarchy에서 name으로 clock rate를 가져온다.
	 * (등록은 v2m_clk_init에서 이뤄졌다)
	 **/
	long rate = sp804_get_clock_rate(name);

	if (rate < 0)
		return;

	/* setup timer 0 as free-running clocksource */
	/** 20141227    
	 * 레지스터 설정. 자세한 내용은 datasheet 참조
	 **/
	writel(0, base + TIMER_CTRL);
	writel(0xffffffff, base + TIMER_LOAD);
	writel(0xffffffff, base + TIMER_VALUE);
	writel(TIMER_CTRL_32BIT | TIMER_CTRL_ENABLE | TIMER_CTRL_PERIODIC,
		base + TIMER_CTRL);

	/** 20141227    
	 * clock의 rate(주파수), rating 200, bits 32,
	 * clocksource 값을 읽기 위한 콜백함수를 지정해
	 * clocksource를 등록한다.
	 **/
	clocksource_mmio_init(base + TIMER_VALUE, name,
		rate, 200, 32, clocksource_mmio_readl_down);

	/** 20141227    
	 * 주어진 clock source를 sched_clock으로 사용한다면 
	 * sched_clock_base와 rate를 설정한다.
	 **/
	if (use_sched_clock) {
		sched_clock_base = base;
		setup_sched_clock(sp804_read, 32, rate);
	}
}


static void __iomem *clkevt_base;
static unsigned long clkevt_reload;

/*
 * IRQ handler for the timer
 */
static irqreturn_t sp804_timer_interrupt(int irq, void *dev_id)
{
	/** 20141227    
	 * clock_event_device 를 받아온다.
	 **/
	struct clock_event_device *evt = dev_id;

	/* clear the interrupt */
	writel(1, clkevt_base + TIMER_INTCLR);

	/** 20140830    
	 * event_handler 호출.
	 **/
	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static void sp804_set_mode(enum clock_event_mode mode,
	struct clock_event_device *evt)
{
	unsigned long ctrl = TIMER_CTRL_32BIT | TIMER_CTRL_IE;

	writel(ctrl, clkevt_base + TIMER_CTRL);

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		writel(clkevt_reload, clkevt_base + TIMER_LOAD);
		ctrl |= TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE;
		break;

	case CLOCK_EVT_MODE_ONESHOT:
		/* period set, and timer enabled in 'next_event' hook */
		ctrl |= TIMER_CTRL_ONESHOT;
		break;

	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		break;
	}

	writel(ctrl, clkevt_base + TIMER_CTRL);
}

static int sp804_set_next_event(unsigned long next,
	struct clock_event_device *evt)
{
	unsigned long ctrl = readl(clkevt_base + TIMER_CTRL);

	writel(next, clkevt_base + TIMER_LOAD);
	writel(ctrl | TIMER_CTRL_ENABLE, clkevt_base + TIMER_CTRL);

	return 0;
}

/** 20141002
 * sp804는 PERIODIC과 ONESHOT의 속성을 다 가진다.
 *
 * cpumask : 이 디바이스가 동작할 수 있는 cpu 지정.
 *           cpu_all_mask이므로 모든 cpu에서 동작할 수 있다.
 **/
static struct clock_event_device sp804_clockevent = {
	.features       = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.set_mode	= sp804_set_mode,
	.set_next_event	= sp804_set_next_event,
	.rating		= 300,
	.cpumask	= cpu_all_mask,
};

/** 20140830    
 * handler는 sp804_timer_interrupt.
 **/
static struct irqaction sp804_timer_irq = {
	.name		= "timer",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= sp804_timer_interrupt,
	.dev_id		= &sp804_clockevent,
};

/** 20141220    
 * sp804_clockevents_init(base + TIMER_1_BASE, irq, "v2m-timer0");
 *   name == con_id ==> v2m-timer0
 **/
void __init sp804_clockevents_init(void __iomem *base, unsigned int irq,
	const char *name)
{
	/** 20141227    
	 * sp804 clockevent 구조체를 가져온다.
	 * name으로 clock rate를 받아온다.
	 **/
	struct clock_event_device *evt = &sp804_clockevent;
	long rate = sp804_get_clock_rate(name);

	if (rate < 0)
		return;

	/** 20141227    
	 * clockevent의 base와 reload
	 **/
	clkevt_base = base;
	clkevt_reload = DIV_ROUND_CLOSEST(rate, HZ);
	/** 20141227    
	 * clockevent 구조체의 name과 irq를 설정한다.
	 **/
	evt->name = name;
	evt->irq = irq;

	/** 20140830    
	 * irq action 등록.
	 **/
	setup_irq(irq, &sp804_timer_irq);
	/** 20141122    
	 * sp804 clock event device를 설정하고 등록한다.
	 * rate(frequency)
	 **/
	clockevents_config_and_register(evt, rate, 0xf, 0xffffffff);
}
