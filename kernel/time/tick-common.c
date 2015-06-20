/*
 * linux/kernel/time/tick-common.c
 *
 * This file contains the base functions to manage periodic tick
 * related events.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 *
 * This code is licenced under the GPL version 2. For details see
 * kernel-base/COPYING.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Tick devices
 */
/** 20141115    
 * cpu별로 tick_device 자료구조를 갖는다.
 **/
DEFINE_PER_CPU(struct tick_device, tick_cpu_device);
/*
 * Tick next event: keeps track of the tick time
 */
ktime_t tick_next_period;
/** 20141115    
 * tick_setup_device() 에서 HZ주기로 동작하도록 설정한다.
 **/
ktime_t tick_period;
/** 20141129    
 * do_timer를 수행하는 cpu를 저장하는 전역변수.
 **/
int tick_do_timer_cpu __read_mostly = TICK_DO_TIMER_BOOT;
static DEFINE_RAW_SPINLOCK(tick_device_lock);

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_device(int cpu)
{
	return &per_cpu(tick_cpu_device, cpu);
}

/**
 * tick_is_oneshot_available - check for a oneshot capable event device
 */
int tick_is_oneshot_available(void)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return 0;
	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 1;
	return tick_broadcast_oneshot_available();
}

/*
 * Periodic tick
 */
static void tick_periodic(int cpu)
{
	/** 20140913    
	 * do_timer를 호출하도록 지정된 cpu만 jiffies를 증가시킨다.
	 **/
	if (tick_do_timer_cpu == cpu) {
		write_seqlock(&xtime_lock);

		/* Keep track of the next tick event */
		tick_next_period = ktime_add(tick_next_period, tick_period);

		do_timer(1);
		write_sequnlock(&xtime_lock);
	}

	/** 20141115    
	 * 모든 cpu가 tick을 주기적으로 처리하는 작업을 수행한다.
	 **/
	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
void tick_handle_periodic(struct clock_event_device *dev)
{
	int cpu = smp_processor_id();
	ktime_t next;

	/** 20140920    
	 * 현재 cpu에 대해 tick_periodic 실행
	 **/
	tick_periodic(cpu);

	if (dev->mode != CLOCK_EVT_MODE_ONESHOT)
		return;
	/*
	 * Setup the next period for devices, which do not have
	 * periodic mode:
	 */
	next = ktime_add(dev->next_event, tick_period);
	for (;;) {
		if (!clockevents_program_event(dev, next, false))
			return;
		/*
		 * Have to be careful here. If we're in oneshot mode,
		 * before we call tick_periodic() in a loop, we need
		 * to be sure we're using a real hardware clocksource.
		 * Otherwise we could get trapped in an infinite
		 * loop, as the tick_periodic() increments jiffies,
		 * when then will increment time, posibly causing
		 * the loop to trigger again and again.
		 */
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
		next = ktime_add(next, tick_period);
	}
}

/*
 * Setup the device for a periodic tick
 */
/** 20141129    
 *
 * vexpress의 경우 sp804, twd 모두 broadcast 0으로 호출된다.
 **/
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	/** 20141002
	 * event_handler를 broadcast에 따라 지정.
	 **/
	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	/** 20141002
	 * broadcast인 경우 이 함수에서 처리하지 않는다.
	 **/
	if (!tick_device_is_functional(dev))
		return;

	/** 20141002
	 * clock_event_device가 PERIODIC 속성이며 , broadcast device가 ONESHOT으로 동작 중이지 않으므로
	 * mode를 CLOCK_EVT_MODE_PERIODIC로 설정한다.
	 **/
	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
		clockevents_set_mode(dev, CLOCK_EVT_MODE_PERIODIC);
	} else {
	/** 20141002
	 * 그렇지 않은 경우 ONESHOT mode로 동작시킨다.
	 **/
		unsigned long seq;
		ktime_t next;

		do {
			seq = read_seqbegin(&xtime_lock);
			next = tick_next_period;
		} while (read_seqretry(&xtime_lock, seq));

		clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);

		for (;;) {
			if (!clockevents_program_event(dev, next, false))
				return;
			next = ktime_add(next, tick_period);
		}
	}
}

/*
 * Setup the tick device
 */
/** 20141122    
 * tick device에 새로운 clock event device를 등록한다.
 **/
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	ktime_t next_event;
	void (*handler)(struct clock_event_device *) = NULL;

	/*
	 * First device setup ?
	 */
	/** 20141002
	 * 현재 cpu의 tick_cpu_device가 비어있다면 최초 device를 등록하는 것이다.
	 **/
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
		/** 20141002
		 * 현재 tick_do_timer_cpu로 지정된 cpu가 없다면 (BOOT 초기값)
		 * 전달받은 cpu를 새로운 do_timer를 수행하는 cpu로 지정한다.
		 *
		 * tick_period는 HZ주기로 동작하도록 설정한다.
		 **/
		if (tick_do_timer_cpu == TICK_DO_TIMER_BOOT) {
			tick_do_timer_cpu = cpu;
			tick_next_period = ktime_get();
			tick_period = ktime_set(0, NSEC_PER_SEC / HZ);
		}

		/*
		 * Startup in periodic mode first.
		 */
		/** 20141002
		 * tick device의 mode를 periodic로 지정, 아래에서 periodic으로 설정하기 위함.
		 **/
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		/** 20141002
		 * 이전에 tick event device가 등록된 상태라면
		 * 이전 event_handler를 clockevents_handle_noop로 지정해 제거한다.
		 **/
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}

	/** 20141002
	 * 새로운 device를 전달받은 tick_device의 event device로 지정한다. 
	 **/
	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	/** 20141002
	 * 새로운 device의 cpumask가 전달받은 cpumask가 같지 않은지 비교.
	 * 이 때 cpumask는 현재 cpu의 mask가 넘어오도록 되어 있으므로,
	 * 같은 경우는 device가 cpu마다 지정되는 경우이다.
	 *
	 * cpu마다 지정되는 device가 아니라면 interrupt를 현재 cpu로 고정시킨다.
	 **/
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way.
	 */
    /** 20141122    
	 * newdev가 dummy라면 cpu를 broadcast에 의한 handle 대상으로 등록하고,
	 * 그렇지 않다면 broadcast mask에서 제거한다.
     **/
	if (tick_device_uses_broadcast(newdev, cpu))
		return;

	/** 20141002
	 * tick device의 mode에 따라 periodic / oneshot 동작을 설정.
	 * periodic의 경우 tick용으로 사용되는 것이므로 broadcast하지 않는다.
	 **/
	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);
	else
		tick_setup_oneshot(newdev, handler, next_event);
}

/*
 * Check, if the new registered device should be used.
 */
/** 20141122    
 * 전역리스트에 새로운 clock event device를 등록하고,
 * CLOCK_EVT_NOTIFY_ADD notify를 날리면 이 notify handler가 호출된다.
 *
 * 성공적으로 등록한 경우 NOTIFY_STOP이 리턴된다. 
 **/
static int tick_check_new_device(struct clock_event_device *newdev)
{
	struct clock_event_device *curdev;
	struct tick_device *td;
	int cpu, ret = NOTIFY_OK;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);

	/** 20141002
	 * 현재 cpu가 새로운 device의 cpumask에 포함되어 있지 않다면 out_bc로 이동.
	 **/
	cpu = smp_processor_id();
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		goto out_bc;

	/** 20141002
	 * percpu인 tick_cpu_device를 받아와 현재 clock_event_device 를 받아옴.
	 **/
	td = &per_cpu(tick_cpu_device, cpu);
	curdev = td->evtdev;

	/* cpu local device ? */
	/** 20141122    
	 * 디바이스가 동작할 cpumask가 현재 cpu의 cpumask와 동일하면
	 * 현재 cpu만의 local device이다. 그렇지 않다면
	 **/
	if (!cpumask_equal(newdev->cpumask, cpumask_of(cpu))) {

		/*
		 * If the cpu affinity of the device interrupt can not
		 * be set, ignore it.
		 */
        /** 20141122    
         * device interrupt에 대해 cpu affinity를 설정이 불가능하면
         * out_bc로 이동.
         **/
		if (!irq_can_set_affinity(newdev->irq))
			goto out_bc;

		/*
		 * If we have a cpu local device already, do not replace it
		 * by a non cpu local device
		 */
        /** 20141122    
         * 현재 cpu에 local device가 등록되어 있다면
         * 새로 지정되는 디바이스는 local device가 아니므로 교체하지 않는다.
         **/
		if (curdev && cpumask_equal(curdev->cpumask, cpumask_of(cpu)))
			goto out_bc;
	}

	/*
	 * If we have an active device, then check the rating and the oneshot
	 * feature.
	 */
	/** 20141002
	 * 최초 periodic용 등록을 위해 호출되었을 때 curdev는 NULL.
	 * 다음 호출되었을 때 curdev는 NOT NULL.
     *
     * 20141122
     * curdev는 cpu local device이다. 이미 존재하면 새 디바이스와 속성을 비교한다.
	 **/
	if (curdev) {
		/*
		 * Prefer one shot capable devices !
		 */
		/** 20150606    
		 * 현재 등록된 디바이스가 ONESHOT이고, 새 디바이스는 ONESHOT이 아니면
		 * 교체하지 않는다.
		 **/
		if ((curdev->features & CLOCK_EVT_FEAT_ONESHOT) &&
		    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
			goto out_bc;
		/*
		 * Check the rating
		 */
		/** 20150103    
		 * 새로 추가하는 device의 rating이 더 높을 때만 교체한다
		 **/
		if (curdev->rating >= newdev->rating)
			goto out_bc;
	}

	/*
	 * Replace the eventually existing device by the new
	 * device. If the current device is the broadcast device, do
	 * not give it back to the clockevents layer !
	 */
	/** 20141002
	 * 현재 등록된 clock_event_device가 broadcast로 사용 중인 dev라면
	 * shutdown 시키고 curdev를 비움. curdev가 NULL이면 거짓.
	 **/
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}
	/** 20141115    
	 * 이전 device를 해제하고, 새로운 device를 등록한다.
	 **/
	clockevents_exchange_device(curdev, newdev);
    /** 20141122    
     * tick device에 새로운 clock_event_device를 등록한다.
     * 새 clock_event_device에 ONESHOT 속성이 있으면 oneshot notify를 준다.
     **/
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();

	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
	/** 20141129    
	 * 정상적으로 tick device에 새로운 clock_event_device를 등록했다면 벗어난다.
	 **/
	return NOTIFY_STOP;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	if (tick_check_broadcast_device(newdev))
		ret = NOTIFY_STOP;

	raw_spin_unlock_irqrestore(&tick_device_lock, flags);

	return ret;
}

/*
 * Transfer the do_timer job away from a dying cpu.
 *
 * Called with interrupts disabled.
 */
static void tick_handover_do_timer(int *cpup)
{
	if (*cpup == tick_do_timer_cpu) {
		int cpu = cpumask_first(cpu_online_mask);

		tick_do_timer_cpu = (cpu < nr_cpu_ids) ? cpu :
			TICK_DO_TIMER_NONE;
	}
}

/*
 * Shutdown an event device on a given cpu:
 *
 * This is called on a life CPU, when a CPU is dead. So we cannot
 * access the hardware device itself.
 * We just set the mode and remove it from the lists.
 */
static void tick_shutdown(unsigned int *cpup)
{
	struct tick_device *td = &per_cpu(tick_cpu_device, *cpup);
	struct clock_event_device *dev = td->evtdev;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	td->mode = TICKDEV_MODE_PERIODIC;
	if (dev) {
		/*
		 * Prevent that the clock events layer tries to call
		 * the set mode function!
		 */
		dev->mode = CLOCK_EVT_MODE_UNUSED;
		clockevents_exchange_device(dev, NULL);
		td->evtdev = NULL;
	}
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

static void tick_suspend(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	clockevents_shutdown(td->evtdev);
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

static void tick_resume(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	unsigned long flags;
	int broadcast = tick_resume_broadcast();

	raw_spin_lock_irqsave(&tick_device_lock, flags);
	clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_RESUME);

	if (!broadcast) {
		if (td->mode == TICKDEV_MODE_PERIODIC)
			tick_setup_periodic(td->evtdev, 0);
		else
			tick_resume_oneshot();
	}
	raw_spin_unlock_irqrestore(&tick_device_lock, flags);
}

/*
 * Notification about clock event devices
 */
/** 20140913    
 * tick_notify event handler.
 *
 * tick_init에서 clockevents_register_notifier 로 등록한 notify handler.
 **/
static int tick_notify(struct notifier_block *nb, unsigned long reason,
			       void *dev)
{
	switch (reason) {

	/** 20141115    
	 * clockevent_devices 리스트에 새로운 clock event device를 추가한 뒤 보낸다.
	 * clockevents_register_device, clockevents_notify_released 에서 호출.
	 **/
	case CLOCK_EVT_NOTIFY_ADD:
		return tick_check_new_device(dev);

	case CLOCK_EVT_NOTIFY_BROADCAST_ON:
	case CLOCK_EVT_NOTIFY_BROADCAST_OFF:
	case CLOCK_EVT_NOTIFY_BROADCAST_FORCE:
		tick_broadcast_on_off(reason, dev);
		break;

	case CLOCK_EVT_NOTIFY_BROADCAST_ENTER:
	case CLOCK_EVT_NOTIFY_BROADCAST_EXIT:
		tick_broadcast_oneshot_control(reason);
		break;

	case CLOCK_EVT_NOTIFY_CPU_DYING:
		tick_handover_do_timer(dev);
		break;

	case CLOCK_EVT_NOTIFY_CPU_DEAD:
		tick_shutdown_broadcast_oneshot(dev);
		tick_shutdown_broadcast(dev);
		tick_shutdown(dev);
		break;

	case CLOCK_EVT_NOTIFY_SUSPEND:
		tick_suspend();
		tick_suspend_broadcast();
		break;

	case CLOCK_EVT_NOTIFY_RESUME:
		tick_resume();
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

/** 20121201
 * tick_notifier 선언.
 **/
static struct notifier_block tick_notifier = {
	.notifier_call = tick_notify,
};

/**
 * tick_init - initialize the tick control
 *
 * Register the notifier with the clockevents framework
 */
/** 20140825    
 * tick notifier 등록.
 **/
void __init tick_init(void)
{
	clockevents_register_notifier(&tick_notifier);
}
