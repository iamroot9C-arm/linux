/*
 * linux/kernel/time/tick-broadcast.c
 *
 * This file contains functions which emulate a local clock-event
 * device via a broadcast event source.
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

#include "tick-internal.h"

/*
 * Broadcast support for broken x86 hardware, where the local apic
 * timer stops in C3 state.
 */

/** 20141122    
 * tick broadcast device용 변수는 전역으로 설정.
 **/
static struct tick_device tick_broadcast_device;
/* FIXME: Use cpumask_var_t. */
/** 20141129    
 * cpu 개수만큼 설정된 tick_broadcast_mask 비트맵을 선언한다.
 **/
static DECLARE_BITMAP(tick_broadcast_mask, NR_CPUS);
static DECLARE_BITMAP(tmpmask, NR_CPUS);
static DEFINE_RAW_SPINLOCK(tick_broadcast_lock);
static int tick_broadcast_force;

#ifdef CONFIG_TICK_ONESHOT
static void tick_broadcast_clear_oneshot(int cpu);
#else
static inline void tick_broadcast_clear_oneshot(int cpu) { }
#endif

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_broadcast_device(void)
{
	return &tick_broadcast_device;
}

/** 20141129    
 * tick broadcast mask를 가져온다.
 * broadcast로 동작하는 cpu들의 mask.
 **/
struct cpumask *tick_get_broadcast_mask(void)
{
	return to_cpumask(tick_broadcast_mask);
}

/*
 * Start the device in periodic mode
 */
/** 20141129    
 * clock_event_device를 periodic broadcast용으로 설정한다.
 **/
static void tick_broadcast_start_periodic(struct clock_event_device *bc)
{
	if (bc)
		tick_setup_periodic(bc, 1);
}

/*
 * Check, if the device can be utilized as broadcast device:
 */
/** 20141129    
 * dev가 새로운 broadcast device가 될 수 있는지 검사하고,
 * 가능하다면 새로운 broadcast device로 설정한다.
 **/
int tick_check_broadcast_device(struct clock_event_device *dev)
{
	/** 20141129    
	 * 이미 tick_broadcast_device에 clock_event_device가 등록되어 있고,
	 * 새롭게 추가된 dev의 rating이 높지 않다면 등록하지 않고 리턴한다.
	 **/
	if ((tick_broadcast_device.evtdev &&
	     tick_broadcast_device.evtdev->rating >= dev->rating) ||
	     (dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 0;

	/** 20141129    
	 * 새로운 device를 tick_broadcast_device에 clock_event_device로 지정한다.
	 **/
	clockevents_exchange_device(tick_broadcast_device.evtdev, dev);
	tick_broadcast_device.evtdev = dev;
	/** 20141129    
	 * boradcast mask에 대상이 존재하면
	 * 새로운 device를 periodic broadcast용으로 설정한다.
	 **/
	if (!cpumask_empty(tick_get_broadcast_mask()))
		tick_broadcast_start_periodic(dev);
	return 1;
}

/*
 * Check, if the device is the broadcast device
 */
/** 20141002
 * dev가 존재하고, 현재 broadcast로 사용 중인 device와 같다면 참.
 **/
int tick_is_broadcast_device(struct clock_event_device *dev)
{
	return (dev && tick_broadcast_device.evtdev == dev);
}

/*
 * Check, if the device is disfunctional and a place holder, which
 * needs to be handled by the broadcast device.
 */
/** 20141129    
 * clock_event_device가 functional하지 않다면 broadcast를 위한 것일 뿐이므로
 * cpu를 broadcast mask에 등록시킨다.
 * 그렇지 않다면 broadcast mask에서 제거한다.
 *
 * vexpress의 경우 sp804, twd clock_event_device는 broadcast에 포함되지 않는다.
 **/
int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu)
{
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Devices might be registered with both periodic and oneshot
	 * mode disabled. This signals, that the device needs to be
	 * operated from the broadcast device and is a placeholder for
	 * the cpu local device.
	 */
	/** 20141129    
	 * clock_event_device가 periodic이나 oneshot으로도 동작하지 않을 경우,
	 * broadcast dummy인 경우
	 *   broadcast mask에 전달받은 cpu를 등록하고 periodic으로 동작시킨다.
	 * broadcast dummy가 아닌 경우
	 *   
	 **/
	if (!tick_device_is_functional(dev)) {
		dev->event_handler = tick_handle_periodic;
		cpumask_set_cpu(cpu, tick_get_broadcast_mask());
		tick_broadcast_start_periodic(tick_broadcast_device.evtdev);
		ret = 1;
	} else {
		/*
		 * When the new device is not affected by the stop
		 * feature and the cpu is marked in the broadcast mask
		 * then clear the broadcast bit.
		 */
		if (!(dev->features & CLOCK_EVT_FEAT_C3STOP)) {
			int cpu = smp_processor_id();

			cpumask_clear_cpu(cpu, tick_get_broadcast_mask());
			tick_broadcast_clear_oneshot(cpu);
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
	return ret;
}

/*
 * Broadcast the event to the cpus, which are set in the mask (mangled).
 */
static void tick_do_broadcast(struct cpumask *mask)
{
	int cpu = smp_processor_id();
	struct tick_device *td;

	/*
	 * Check, if the current cpu is in the mask
	 */
	if (cpumask_test_cpu(cpu, mask)) {
		cpumask_clear_cpu(cpu, mask);
		td = &per_cpu(tick_cpu_device, cpu);
		td->evtdev->event_handler(td->evtdev);
	}

	if (!cpumask_empty(mask)) {
		/*
		 * It might be necessary to actually check whether the devices
		 * have different broadcast functions. For now, just use the
		 * one of the first device. This works as long as we have this
		 * misfeature only on x86 (lapic)
		 */
		td = &per_cpu(tick_cpu_device, cpumask_first(mask));
		td->evtdev->broadcast(mask);
	}
}

/*
 * Periodic broadcast:
 * - invoke the broadcast handlers
 */
static void tick_do_periodic_broadcast(void)
{
	raw_spin_lock(&tick_broadcast_lock);

	cpumask_and(to_cpumask(tmpmask),
		    cpu_online_mask, tick_get_broadcast_mask());
	tick_do_broadcast(to_cpumask(tmpmask));

	raw_spin_unlock(&tick_broadcast_lock);
}

/*
 * Event handler for periodic broadcast ticks
 */
static void tick_handle_periodic_broadcast(struct clock_event_device *dev)
{
	ktime_t next;

	tick_do_periodic_broadcast();

	/*
	 * The device is in periodic mode. No reprogramming necessary:
	 */
	if (dev->mode == CLOCK_EVT_MODE_PERIODIC)
		return;

	/*
	 * Setup the next period for devices, which do not have
	 * periodic mode. We read dev->next_event first and add to it
	 * when the event already expired. clockevents_program_event()
	 * sets dev->next_event only when the event is really
	 * programmed to the device.
	 */
	for (next = dev->next_event; ;) {
		next = ktime_add(next, tick_period);

		if (!clockevents_program_event(dev, next, false))
			return;
		tick_do_periodic_broadcast();
	}
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop
 */
static void tick_do_broadcast_on_off(unsigned long *reason)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	unsigned long flags;
	int cpu, bc_stopped;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	cpu = smp_processor_id();
	td = &per_cpu(tick_cpu_device, cpu);
	dev = td->evtdev;
	bc = tick_broadcast_device.evtdev;

	/*
	 * Is the device not affected by the powerstate ?
	 */
	if (!dev || !(dev->features & CLOCK_EVT_FEAT_C3STOP))
		goto out;

	if (!tick_device_is_functional(dev))
		goto out;

	bc_stopped = cpumask_empty(tick_get_broadcast_mask());

	switch (*reason) {
	case CLOCK_EVT_NOTIFY_BROADCAST_ON:
	case CLOCK_EVT_NOTIFY_BROADCAST_FORCE:
		if (!cpumask_test_cpu(cpu, tick_get_broadcast_mask())) {
			cpumask_set_cpu(cpu, tick_get_broadcast_mask());
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				clockevents_shutdown(dev);
		}
		if (*reason == CLOCK_EVT_NOTIFY_BROADCAST_FORCE)
			tick_broadcast_force = 1;
		break;
	case CLOCK_EVT_NOTIFY_BROADCAST_OFF:
		if (!tick_broadcast_force &&
		    cpumask_test_cpu(cpu, tick_get_broadcast_mask())) {
			cpumask_clear_cpu(cpu, tick_get_broadcast_mask());
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				tick_setup_periodic(dev, 0);
		}
		break;
	}

	if (cpumask_empty(tick_get_broadcast_mask())) {
		if (!bc_stopped)
			clockevents_shutdown(bc);
	} else if (bc_stopped) {
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
			tick_broadcast_start_periodic(bc);
		else
			tick_broadcast_setup_oneshot(bc);
	}
out:
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop.
 */
void tick_broadcast_on_off(unsigned long reason, int *oncpu)
{
	if (!cpumask_test_cpu(*oncpu, cpu_online_mask))
		printk(KERN_ERR "tick-broadcast: ignoring broadcast for "
		       "offline CPU #%d\n", *oncpu);
	else
		tick_do_broadcast_on_off(&reason);
}

/*
 * Set the periodic handler depending on broadcast on/off
 */
/** 20140920    
 * tick periodic handler를 지정한다.
 *
 * 최초 sp804_clockevent, broadcast 0으로 호출.
 * 이후 twd_timer_setup에서 "local_timer"를 지정해 broadcast 0으로 호출.
 **/
void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast)
{
	if (!broadcast)
		dev->event_handler = tick_handle_periodic;
	else
		dev->event_handler = tick_handle_periodic_broadcast;
}

/*
 * Remove a CPU from broadcasting
 */
void tick_shutdown_broadcast(unsigned int *cpup)
{
	struct clock_event_device *bc;
	unsigned long flags;
	unsigned int cpu = *cpup;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;
	cpumask_clear_cpu(cpu, tick_get_broadcast_mask());

	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
		if (bc && cpumask_empty(tick_get_broadcast_mask()))
			clockevents_shutdown(bc);
	}

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

void tick_suspend_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;
	if (bc)
		clockevents_shutdown(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

int tick_resume_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;
	int broadcast = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;

	if (bc) {
		clockevents_set_mode(bc, CLOCK_EVT_MODE_RESUME);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_PERIODIC:
			if (!cpumask_empty(tick_get_broadcast_mask()))
				tick_broadcast_start_periodic(bc);
			broadcast = cpumask_test_cpu(smp_processor_id(),
						     tick_get_broadcast_mask());
			break;
		case TICKDEV_MODE_ONESHOT:
			if (!cpumask_empty(tick_get_broadcast_mask()))
				broadcast = tick_resume_broadcast_oneshot(bc);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);

	return broadcast;
}


#ifdef CONFIG_TICK_ONESHOT

/* FIXME: use cpumask_var_t. */
/** 20141129    
 * tick broadcast oneshot 비트맵 마스크를 선언한다.
 **/
static DECLARE_BITMAP(tick_broadcast_oneshot_mask, NR_CPUS);

/*
 * Exposed for debugging: see timer_list.c
 */
/** 20141129    
 * tick broadcast oneshot 비트맵 마스크를 (cpumask *) 타입으로 리턴한다.
 **/
struct cpumask *tick_get_broadcast_oneshot_mask(void)
{
	return to_cpumask(tick_broadcast_oneshot_mask);
}

/** 20141129    
 * tick_broadcast_device의 mode를 ONESHOT으로 하고 program 한다.
 **/
static int tick_broadcast_set_event(ktime_t expires, int force)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	if (bc->mode != CLOCK_EVT_MODE_ONESHOT)
		clockevents_set_mode(bc, CLOCK_EVT_MODE_ONESHOT);

	return clockevents_program_event(bc, expires, force);
}

int tick_resume_broadcast_oneshot(struct clock_event_device *bc)
{
	clockevents_set_mode(bc, CLOCK_EVT_MODE_ONESHOT);
	return 0;
}

/*
 * Called from irq_enter() when idle was interrupted to reenable the
 * per cpu device.
 */
void tick_check_oneshot_broadcast(int cpu)
{
	if (cpumask_test_cpu(cpu, to_cpumask(tick_broadcast_oneshot_mask))) {
		struct tick_device *td = &per_cpu(tick_cpu_device, cpu);

		clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_ONESHOT);
	}
}

/*
 * Handle oneshot mode broadcasting
 */
static void tick_handle_oneshot_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td;
	ktime_t now, next_event;
	int cpu;

	raw_spin_lock(&tick_broadcast_lock);
again:
	dev->next_event.tv64 = KTIME_MAX;
	next_event.tv64 = KTIME_MAX;
	cpumask_clear(to_cpumask(tmpmask));
	now = ktime_get();
	/* Find all expired events */
	for_each_cpu(cpu, tick_get_broadcast_oneshot_mask()) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev->next_event.tv64 <= now.tv64)
			cpumask_set_cpu(cpu, to_cpumask(tmpmask));
		else if (td->evtdev->next_event.tv64 < next_event.tv64)
			next_event.tv64 = td->evtdev->next_event.tv64;
	}

	/*
	 * Wakeup the cpus which have an expired event.
	 */
	tick_do_broadcast(to_cpumask(tmpmask));

	/*
	 * Two reasons for reprogram:
	 *
	 * - The global event did not expire any CPU local
	 * events. This happens in dyntick mode, as the maximum PIT
	 * delta is quite small.
	 *
	 * - There are pending events on sleeping CPUs which were not
	 * in the event mask
	 */
	if (next_event.tv64 != KTIME_MAX) {
		/*
		 * Rearm the broadcast device. If event expired,
		 * repeat the above
		 */
		if (tick_broadcast_set_event(next_event, 0))
			goto again;
	}
	raw_spin_unlock(&tick_broadcast_lock);
}

/*
 * Powerstate information: The system enters/leaves a state, where
 * affected devices might stop
 */
void tick_broadcast_oneshot_control(unsigned long reason)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	unsigned long flags;
	int cpu;

	/*
	 * Periodic mode does not care about the enter/exit of power
	 * states
	 */
	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
		return;

	/*
	 * We are called with preemtion disabled from the depth of the
	 * idle code, so we can't be moved away.
	 */
	cpu = smp_processor_id();
	td = &per_cpu(tick_cpu_device, cpu);
	dev = td->evtdev;

	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		return;

	bc = tick_broadcast_device.evtdev;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	if (reason == CLOCK_EVT_NOTIFY_BROADCAST_ENTER) {
		if (!cpumask_test_cpu(cpu, tick_get_broadcast_oneshot_mask())) {
			cpumask_set_cpu(cpu, tick_get_broadcast_oneshot_mask());
			clockevents_set_mode(dev, CLOCK_EVT_MODE_SHUTDOWN);
			if (dev->next_event.tv64 < bc->next_event.tv64)
				tick_broadcast_set_event(dev->next_event, 1);
		}
	} else {
		if (cpumask_test_cpu(cpu, tick_get_broadcast_oneshot_mask())) {
			cpumask_clear_cpu(cpu,
					  tick_get_broadcast_oneshot_mask());
			clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);
			if (dev->next_event.tv64 != KTIME_MAX)
				tick_program_event(dev->next_event, 1);
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Reset the one shot broadcast for a cpu
 *
 * Called with tick_broadcast_lock held
 */
/** 20141129    
 * cpu를 broadcast_oneshot mask에서 지운다.
 **/
static void tick_broadcast_clear_oneshot(int cpu)
{
	cpumask_clear_cpu(cpu, tick_get_broadcast_oneshot_mask());
}

/** 20141129    
 * broadcast mask되어 있는 각 cpu를 순회하며
 * clock_event_device가 등록되어 있는 경우 next_event(tick_next_period)를 설정한다.
 **/
static void tick_broadcast_init_next_event(struct cpumask *mask,
					   ktime_t expires)
{
	struct tick_device *td;
	int cpu;

	/** 20141129    
	 * mask에 속하는 각 cpu의 tick_cpu_device에 접근하여
	 * clock_event_device가 존재한다면 expires를 다음 이벤트 발생시간으로 설정한다.
	 **/
	for_each_cpu(cpu, mask) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev)
			td->evtdev->next_event = expires;
	}
}

/**
 * tick_broadcast_setup_oneshot - setup the broadcast device
 */
/** 20141129    
 * tick broadcast device를 oneshot 방식으로 설정한다.
 **/
void tick_broadcast_setup_oneshot(struct clock_event_device *bc)
{
	int cpu = smp_processor_id();

	/* Set it up only once ! */
	/** 20141129    
	 * 현재 event_handler가 tick_handle_oneshot_broadcast가 아니라면
	 * event_handler를 tick_handle_oneshot_broadcast로 설정한다.
	 **/
	if (bc->event_handler != tick_handle_oneshot_broadcast) {
		int was_periodic = bc->mode == CLOCK_EVT_MODE_PERIODIC;

		bc->event_handler = tick_handle_oneshot_broadcast;

		/* Take the do_timer update */
		/** 20141129    
		 * 현재 코드를 수행하는 cpu가 do_timer를 수행하는 cpu이다.
		 **/
		tick_do_timer_cpu = cpu;

		/*
		 * We must be careful here. There might be other CPUs
		 * waiting for periodic broadcast. We need to set the
		 * oneshot_mask bits for those and program the
		 * broadcast device to fire.
		 */
		/** 20141129    
		 * tick broadcast mask를 tmpmask로 복사해 와서 현재 cpu는 제거한다.
		 * tick broadcast oneshot 마스크에 tick broadcast 마스크를 or한다.
		 **/
		cpumask_copy(to_cpumask(tmpmask), tick_get_broadcast_mask());
		cpumask_clear_cpu(cpu, to_cpumask(tmpmask));
		cpumask_or(tick_get_broadcast_oneshot_mask(),
			   tick_get_broadcast_oneshot_mask(),
			   to_cpumask(tmpmask));

		/** 20141129    
		 * tick이 periodic이었고, tick broadcast mask가 현재 cpu를 제외했을 때
		 * 비어있지 않았다면
		 *   clock_event_device의 mode를 ONESHOT 모드로 설정하고,
		 *   다음 event가 tick_next_period가 되도록 한다.
		 *   (tick_cpu_device와 tick_broadcast_device 모두.)
		 **/
		if (was_periodic && !cpumask_empty(to_cpumask(tmpmask))) {
			clockevents_set_mode(bc, CLOCK_EVT_MODE_ONESHOT);
			tick_broadcast_init_next_event(to_cpumask(tmpmask),
						       tick_next_period);
			tick_broadcast_set_event(tick_next_period, 1);
		} else
			bc->next_event.tv64 = KTIME_MAX;
	} else {
		/*
		 * The first cpu which switches to oneshot mode sets
		 * the bit for all other cpus which are in the general
		 * (periodic) broadcast mask. So the bit is set and
		 * would prevent the first broadcast enter after this
		 * to program the bc device.
		 */
		/** 20141129    
		 * 첫번째 수행에서 각 cpu의 device에 대한 설정까지 되었으므로
		 * 이후 호출시에는 broadcast oneshot mask에서 지운다.
		 **/
		tick_broadcast_clear_oneshot(cpu);
	}
}

/*
 * Select oneshot operating mode for the broadcast device
 */
void tick_broadcast_switch_to_oneshot(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/** 20141129    
	 * tick broadcast device의 mode를 ONESHOT으로 변경하고,
	 * clock_event_device가 등록되어 있다면 tick broadcast를 oneshot으로 설정한다.
	 **/
	tick_broadcast_device.mode = TICKDEV_MODE_ONESHOT;
	bc = tick_broadcast_device.evtdev;
	if (bc)
		tick_broadcast_setup_oneshot(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}


/*
 * Remove a dead CPU from broadcasting
 */
void tick_shutdown_broadcast_oneshot(unsigned int *cpup)
{
	unsigned long flags;
	unsigned int cpu = *cpup;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Clear the broadcast mask flag for the dead cpu, but do not
	 * stop the broadcast device!
	 */
	cpumask_clear_cpu(cpu, tick_get_broadcast_oneshot_mask());

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Check, whether the broadcast device is in one shot mode
 */
/** 20141002
 * 현재 tick_broadcast_device가 ONESHOT으로 동작 중인 경우.
 **/
int tick_broadcast_oneshot_active(void)
{
	return tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT;
}

/*
 * Check whether the broadcast device supports oneshot.
 */
bool tick_broadcast_oneshot_available(void)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	return bc ? bc->features & CLOCK_EVT_FEAT_ONESHOT : false;
}

#endif
