/*
 * drivers/base/power/wakeup.c - System wakeup events framework
 *
 * Copyright (c) 2010 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file is released under the GPLv2.
 */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <trace/events/power.h>

#include "power.h"

/*
 * If set, the suspend/hibernate code will abort transitions to a sleep state
 * if wakeup events are registered during or immediately before the transition.
 */
bool events_check_enabled __read_mostly;

/*
 * Combined counters of registered wakeup events and wakeup events in progress.
 * They need to be modified together atomically, so it's better to use one
 * atomic variable to hold them both.
 */
static atomic_t combined_event_count = ATOMIC_INIT(0);

#define IN_PROGRESS_BITS	(sizeof(int) * 4)
#define MAX_IN_PROGRESS		((1 << IN_PROGRESS_BITS) - 1)

static void split_counters(unsigned int *cnt, unsigned int *inpr)
{
	unsigned int comb = atomic_read(&combined_event_count);

	*cnt = (comb >> IN_PROGRESS_BITS);
	*inpr = comb & MAX_IN_PROGRESS;
}

/* A preserved old value of the events counter. */
static unsigned int saved_count;

static DEFINE_SPINLOCK(events_lock);

static void pm_wakeup_timer_fn(unsigned long data);

static LIST_HEAD(wakeup_sources);

static DECLARE_WAIT_QUEUE_HEAD(wakeup_count_wait_queue);

/**
 * wakeup_source_prepare - Prepare a new wakeup source for initialization.
 * @ws: Wakeup source to prepare.
 * @name: Pointer to the name of the new wakeup source.
 *
 * Callers must ensure that the @name string won't be freed when @ws is still in
 * use.
 */
/** 20151121
 * 새 wakeup source를 초기화에 사용하기 위해 준비한다.
 *
 * - 이름을 지정한다.
 **/
void wakeup_source_prepare(struct wakeup_source *ws, const char *name)
{
	if (ws) {
		memset(ws, 0, sizeof(*ws));
		ws->name = name;
	}
}
EXPORT_SYMBOL_GPL(wakeup_source_prepare);

/**
 * wakeup_source_create - Create a struct wakeup_source object.
 * @name: Name of the new wakeup source.
 */
/** 20151121
 * 주어진 이름의 새 wakeup source를 할당해 준비한다.
 **/
struct wakeup_source *wakeup_source_create(const char *name)
{
	struct wakeup_source *ws;

	ws = kmalloc(sizeof(*ws), GFP_KERNEL);
	if (!ws)
		return NULL;

	wakeup_source_prepare(ws, name ? kstrdup(name, GFP_KERNEL) : NULL);
	return ws;
}
EXPORT_SYMBOL_GPL(wakeup_source_create);

/**
 * wakeup_source_drop - Prepare a struct wakeup_source object for destruction.
 * @ws: Wakeup source to prepare for destruction.
 *
 * Callers must ensure that __pm_stay_awake() or __pm_wakeup_event() will never
 * be run in parallel with this function for the same wakeup source object.
 */
/** 20151121
 * wakeup source의 제거 전 작업을 진행한다.
 * 
 * 동작 중인 timer를 제거하고, PM core에서 해당 wakeup source가 끝났음을 알린다.
 * 
 **/
void wakeup_source_drop(struct wakeup_source *ws)
{
	if (!ws)
		return;

	del_timer_sync(&ws->timer);
	__pm_relax(ws);
}
EXPORT_SYMBOL_GPL(wakeup_source_drop);

/**
 * wakeup_source_destroy - Destroy a struct wakeup_source object.
 * @ws: Wakeup source to destroy.
 *
 * Use only for wakeup source objects created with wakeup_source_create().
 */
/** 20151121
 * wakeup source의 제거 전 처리를 진행하고 메모리를 해제한다.
 **/
void wakeup_source_destroy(struct wakeup_source *ws)
{
	if (!ws)
		return;

	wakeup_source_drop(ws);
	kfree(ws->name);
	kfree(ws);
}
EXPORT_SYMBOL_GPL(wakeup_source_destroy);

/**
 * wakeup_source_add - Add given object to the list of wakeup sources.
 * @ws: Wakeup source object to add to the list.
 */
/** 20151121
 * 주어진 waekup source를 전역 리스트에 등록한다.
 **/
void wakeup_source_add(struct wakeup_source *ws)
{
	if (WARN_ON(!ws))
		return;

	spin_lock_init(&ws->lock);
	/** 20151121
	 * timer 자료구조 설정.
	 **/
	setup_timer(&ws->timer, pm_wakeup_timer_fn, (unsigned long)ws);
	ws->active = false;
	ws->last_time = ktime_get();

	spin_lock_irq(&events_lock);
	list_add_rcu(&ws->entry, &wakeup_sources);
	spin_unlock_irq(&events_lock);
}
EXPORT_SYMBOL_GPL(wakeup_source_add);

/**
 * wakeup_source_remove - Remove given object from the wakeup sources list.
 * @ws: Wakeup source object to remove from the list.
 */
/** 20151121
 * wakeup source를 리스트에서 제거한다.
 **/
void wakeup_source_remove(struct wakeup_source *ws)
{
	if (WARN_ON(!ws))
		return;

	spin_lock_irq(&events_lock);
	list_del_rcu(&ws->entry);
	spin_unlock_irq(&events_lock);
	/** 20151121
	 * rcu로 관리되는 리스트이므로 reader가 완료될 때까지 대기한다.
	 **/
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(wakeup_source_remove);

/**
 * wakeup_source_register - Create wakeup source and add it to the list.
 * @name: Name of the wakeup source to register.
 */
/** 20151121
 * 새 wakeup source를 생성하고, 생성이 완료되었다면 등록한다.
 **/
struct wakeup_source *wakeup_source_register(const char *name)
{
	struct wakeup_source *ws;

	ws = wakeup_source_create(name);
	if (ws)
		wakeup_source_add(ws);

	return ws;
}
EXPORT_SYMBOL_GPL(wakeup_source_register);

/**
 * wakeup_source_unregister - Remove wakeup source from the list and remove it.
 * @ws: Wakeup source object to unregister.
 */
/** 20151121
 * 주어진 wakeup source를 제거한다.
 **/
void wakeup_source_unregister(struct wakeup_source *ws)
{
	if (ws) {
		wakeup_source_remove(ws);
		wakeup_source_destroy(ws);
	}
}
EXPORT_SYMBOL_GPL(wakeup_source_unregister);

/**
 * device_wakeup_attach - Attach a wakeup source object to a device object.
 * @dev: Device to handle.
 * @ws: Wakeup source object to attach to @dev.
 *
 * This causes @dev to be treated as a wakeup device.
 */
/** 20151121
 * device power lock을 걸고, wakeup source를 디바이스에 지정한다.
 **/
static int device_wakeup_attach(struct device *dev, struct wakeup_source *ws)
{
	spin_lock_irq(&dev->power.lock);
	if (dev->power.wakeup) {
		spin_unlock_irq(&dev->power.lock);
		return -EEXIST;
	}
	dev->power.wakeup = ws;
	spin_unlock_irq(&dev->power.lock);
	return 0;
}

/**
 * device_wakeup_enable - Enable given device to be a wakeup source.
 * @dev: Device to handle.
 *
 * Create a wakeup source object, register it and attach it to @dev.
 */
/** 20151121
 * device가 wakeup 가능한 경우 wakeup source를 생성해 등록한다.
 **/
int device_wakeup_enable(struct device *dev)
{
	struct wakeup_source *ws;
	int ret;

	/** 20151121
	 * device는 wakeup 가능한 상태로 설정되어 있어야 한다.
	 **/
	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	/** 20151121
	 * 디바이스 이름으로 wakeup source를 생성하고 등록한다.
	 **/
	ws = wakeup_source_register(dev_name(dev));
	if (!ws)
		return -ENOMEM;

	/** 20151121
	 * device에 wakeup source를 등록한다.
	 **/
	ret = device_wakeup_attach(dev, ws);
	if (ret)
		wakeup_source_unregister(ws);

	return ret;
}
EXPORT_SYMBOL_GPL(device_wakeup_enable);

/**
 * device_wakeup_detach - Detach a device's wakeup source object from it.
 * @dev: Device to detach the wakeup source object from.
 *
 * After it returns, @dev will not be treated as a wakeup device any more.
 */
static struct wakeup_source *device_wakeup_detach(struct device *dev)
{
	struct wakeup_source *ws;

	spin_lock_irq(&dev->power.lock);
	ws = dev->power.wakeup;
	dev->power.wakeup = NULL;
	spin_unlock_irq(&dev->power.lock);
	return ws;
}

/**
 * device_wakeup_disable - Do not regard a device as a wakeup source any more.
 * @dev: Device to handle.
 *
 * Detach the @dev's wakeup source object from it, unregister this wakeup source
 * object and destroy it.
 */
int device_wakeup_disable(struct device *dev)
{
	struct wakeup_source *ws;

	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	ws = device_wakeup_detach(dev);
	if (ws)
		wakeup_source_unregister(ws);

	return 0;
}
EXPORT_SYMBOL_GPL(device_wakeup_disable);

/**
 * device_set_wakeup_capable - Set/reset device wakeup capability flag.
 * @dev: Device to handle.
 * @capable: Whether or not @dev is capable of waking up the system from sleep.
 *
 * If @capable is set, set the @dev's power.can_wakeup flag and add its
 * wakeup-related attributes to sysfs.  Otherwise, unset the @dev's
 * power.can_wakeup flag and remove its wakeup-related attributes from sysfs.
 *
 * This function may sleep and it can't be called from any context where
 * sleeping is not allowed.
 */
/** 20151121
 * device에 wakeup capable 속성을 지정한다.
 **/
void device_set_wakeup_capable(struct device *dev, bool capable)
{
	/** 20151121
	 * 지정하려고 하는 capable 상태가 이미 디바이스에 설정된 값과 같다면 리턴.
	 **/
	if (!!dev->power.can_wakeup == !!capable)
		return;

	/** 20151121
	 * 디바이스가 등록되어 있고, pm list에 등록되어 있다면 capable에 따라 처리.
	 **/
	if (device_is_registered(dev) && !list_empty(&dev->power.entry)) {
		/** 20151121
		 * capable인 경우 sysfs에 wakeup 관련 attribute들을 추가.
		 * 그렇지 않은 경우 제거.
		 **/
		if (capable) {
			if (wakeup_sysfs_add(dev))
				return;
		} else {
			wakeup_sysfs_remove(dev);
		}
	}
	/** 20151121
	 * device에 설정하고자 하는 capable을 지정한다.
	 **/
	dev->power.can_wakeup = capable;
}
EXPORT_SYMBOL_GPL(device_set_wakeup_capable);

/**
 * device_init_wakeup - Device wakeup initialization.
 * @dev: Device to handle.
 * @enable: Whether or not to enable @dev as a wakeup device.
 *
 * By default, most devices should leave wakeup disabled.  The exceptions are
 * devices that everyone expects to be wakeup sources: keyboards, power buttons,
 * possibly network interfaces, etc.  Also, devices that don't generate their
 * own wakeup requests but merely forward requests from one bus to another
 * (like PCI bridges) should have wakeup enabled by default.
 */
/** 20151121
 * device의 wakeup을 초기화 한다.
 * 
 * 지정하지 않으면 디바이스는 wakeup disabled 상태이다.
 **/
int device_init_wakeup(struct device *dev, bool enable)
{
	int ret = 0;

	/** 20151121
	 * enable인 경우device의 wake capable을 true로 지정하고,
	 * 디바이스의 wakeup source를 등록한다.
	 **/
	if (enable) {
		device_set_wakeup_capable(dev, true);
		ret = device_wakeup_enable(dev);
	} else {
		device_set_wakeup_capable(dev, false);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(device_init_wakeup);

/**
 * device_set_wakeup_enable - Enable or disable a device to wake up the system.
 * @dev: Device to handle.
 */
int device_set_wakeup_enable(struct device *dev, bool enable)
{
	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	return enable ? device_wakeup_enable(dev) : device_wakeup_disable(dev);
}
EXPORT_SYMBOL_GPL(device_set_wakeup_enable);

/*
 * The functions below use the observation that each wakeup event starts a
 * period in which the system should not be suspended.  The moment this period
 * will end depends on how the wakeup event is going to be processed after being
 * detected and all of the possible cases can be divided into two distinct
 * groups.
 *
 * First, a wakeup event may be detected by the same functional unit that will
 * carry out the entire processing of it and possibly will pass it to user space
 * for further processing.  In that case the functional unit that has detected
 * the event may later "close" the "no suspend" period associated with it
 * directly as soon as it has been dealt with.  The pair of pm_stay_awake() and
 * pm_relax(), balanced with each other, is supposed to be used in such
 * situations.
 *
 * Second, a wakeup event may be detected by one functional unit and processed
 * by another one.  In that case the unit that has detected it cannot really
 * "close" the "no suspend" period associated with it, unless it knows in
 * advance what's going to happen to the event during processing.  This
 * knowledge, however, may not be available to it, so it can simply specify time
 * to wait before the system can be suspended and pass it as the second
 * argument of pm_wakeup_event().
 *
 * It is valid to call pm_relax() after pm_wakeup_event(), in which case the
 * "no suspend" period will be ended either by the pm_relax(), or by the timer
 * function executed when the timer expires, whichever comes first.
 */

/**
 * wakup_source_activate - Mark given wakeup source as active.
 * @ws: Wakeup source to handle.
 *
 * Update the @ws' statistics and, if @ws has just been activated, notify the PM
 * core of the event by incrementing the counter of of wakeup events being
 * processed.
 */
static void wakeup_source_activate(struct wakeup_source *ws)
{
	unsigned int cec;

	ws->active = true;
	ws->active_count++;
	ws->last_time = ktime_get();
	if (ws->autosleep_enabled)
		ws->start_prevent_time = ws->last_time;

	/* Increment the counter of events in progress. */
	cec = atomic_inc_return(&combined_event_count);

	trace_wakeup_source_activate(ws->name, cec);
}

/**
 * wakeup_source_report_event - Report wakeup event using the given source.
 * @ws: Wakeup source to report the event for.
 */
static void wakeup_source_report_event(struct wakeup_source *ws)
{
	ws->event_count++;
	/* This is racy, but the counter is approximate anyway. */
	if (events_check_enabled)
		ws->wakeup_count++;

	if (!ws->active)
		wakeup_source_activate(ws);
}

/**
 * __pm_stay_awake - Notify the PM core of a wakeup event.
 * @ws: Wakeup source object associated with the source of the event.
 *
 * It is safe to call this function from interrupt context.
 */
void __pm_stay_awake(struct wakeup_source *ws)
{
	unsigned long flags;

	if (!ws)
		return;

	spin_lock_irqsave(&ws->lock, flags);

	wakeup_source_report_event(ws);
	del_timer(&ws->timer);
	ws->timer_expires = 0;

	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_stay_awake);

/**
 * pm_stay_awake - Notify the PM core that a wakeup event is being processed.
 * @dev: Device the wakeup event is related to.
 *
 * Notify the PM core of a wakeup event (signaled by @dev) by calling
 * __pm_stay_awake for the @dev's wakeup source object.
 *
 * Call this function after detecting of a wakeup event if pm_relax() is going
 * to be called directly after processing the event (and possibly passing it to
 * user space for further processing).
 */
void pm_stay_awake(struct device *dev)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_stay_awake(dev->power.wakeup);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_stay_awake);

#ifdef CONFIG_PM_AUTOSLEEP
static void update_prevent_sleep_time(struct wakeup_source *ws, ktime_t now)
{
	ktime_t delta = ktime_sub(now, ws->start_prevent_time);
	ws->prevent_sleep_time = ktime_add(ws->prevent_sleep_time, delta);
}
#else
static inline void update_prevent_sleep_time(struct wakeup_source *ws,
					     ktime_t now) {}
#endif

/**
 * wakup_source_deactivate - Mark given wakeup source as inactive.
 * @ws: Wakeup source to handle.
 *
 * Update the @ws' statistics and notify the PM core that the wakeup source has
 * become inactive by decrementing the counter of wakeup events being processed
 * and incrementing the counter of registered wakeup events.
 */
static void wakeup_source_deactivate(struct wakeup_source *ws)
{
	unsigned int cnt, inpr, cec;
	ktime_t duration;
	ktime_t now;

	ws->relax_count++;
	/*
	 * __pm_relax() may be called directly or from a timer function.
	 * If it is called directly right after the timer function has been
	 * started, but before the timer function calls __pm_relax(), it is
	 * possible that __pm_stay_awake() will be called in the meantime and
	 * will set ws->active.  Then, ws->active may be cleared immediately
	 * by the __pm_relax() called from the timer function, but in such a
	 * case ws->relax_count will be different from ws->active_count.
	 */
	if (ws->relax_count != ws->active_count) {
		ws->relax_count--;
		return;
	}

	ws->active = false;

	now = ktime_get();
	duration = ktime_sub(now, ws->last_time);
	ws->total_time = ktime_add(ws->total_time, duration);
	if (ktime_to_ns(duration) > ktime_to_ns(ws->max_time))
		ws->max_time = duration;

	ws->last_time = now;
	del_timer(&ws->timer);
	ws->timer_expires = 0;

	if (ws->autosleep_enabled)
		update_prevent_sleep_time(ws, now);

	/*
	 * Increment the counter of registered wakeup events and decrement the
	 * couter of wakeup events in progress simultaneously.
	 */
	cec = atomic_add_return(MAX_IN_PROGRESS, &combined_event_count);
	trace_wakeup_source_deactivate(ws->name, cec);

	split_counters(&cnt, &inpr);
	if (!inpr && waitqueue_active(&wakeup_count_wait_queue))
		wake_up(&wakeup_count_wait_queue);
}

/**
 * __pm_relax - Notify the PM core that processing of a wakeup event has ended.
 * @ws: Wakeup source object associated with the source of the event.
 *
 * Call this function for wakeup events whose processing started with calling
 * __pm_stay_awake().
 *
 * It is safe to call it from interrupt context.
 */
/** 20151121
 * PM core에게 wakeup event 작동이 끝났음을 통보한다.
 **/
void __pm_relax(struct wakeup_source *ws)
{
	unsigned long flags;

	if (!ws)
		return;

	spin_lock_irqsave(&ws->lock, flags);
	if (ws->active)
		wakeup_source_deactivate(ws);
	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_relax);

/**
 * pm_relax - Notify the PM core that processing of a wakeup event has ended.
 * @dev: Device that signaled the event.
 *
 * Execute __pm_relax() for the @dev's wakeup source object.
 */
void pm_relax(struct device *dev)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_relax(dev->power.wakeup);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_relax);

/**
 * pm_wakeup_timer_fn - Delayed finalization of a wakeup event.
 * @data: Address of the wakeup source object associated with the event source.
 *
 * Call wakeup_source_deactivate() for the wakeup source whose address is stored
 * in @data if it is currently active and its timer has not been canceled and
 * the expiration time of the timer is not in future.
 */
static void pm_wakeup_timer_fn(unsigned long data)
{
	struct wakeup_source *ws = (struct wakeup_source *)data;
	unsigned long flags;

	spin_lock_irqsave(&ws->lock, flags);

	if (ws->active && ws->timer_expires
	    && time_after_eq(jiffies, ws->timer_expires)) {
		wakeup_source_deactivate(ws);
		ws->expire_count++;
	}

	spin_unlock_irqrestore(&ws->lock, flags);
}

/**
 * __pm_wakeup_event - Notify the PM core of a wakeup event.
 * @ws: Wakeup source object associated with the event source.
 * @msec: Anticipated event processing time (in milliseconds).
 *
 * Notify the PM core of a wakeup event whose source is @ws that will take
 * approximately @msec milliseconds to be processed by the kernel.  If @ws is
 * not active, activate it.  If @msec is nonzero, set up the @ws' timer to
 * execute pm_wakeup_timer_fn() in future.
 *
 * It is safe to call this function from interrupt context.
 */
void __pm_wakeup_event(struct wakeup_source *ws, unsigned int msec)
{
	unsigned long flags;
	unsigned long expires;

	if (!ws)
		return;

	spin_lock_irqsave(&ws->lock, flags);

	wakeup_source_report_event(ws);

	if (!msec) {
		wakeup_source_deactivate(ws);
		goto unlock;
	}

	expires = jiffies + msecs_to_jiffies(msec);
	if (!expires)
		expires = 1;

	if (!ws->timer_expires || time_after(expires, ws->timer_expires)) {
		mod_timer(&ws->timer, expires);
		ws->timer_expires = expires;
	}

 unlock:
	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_wakeup_event);


/**
 * pm_wakeup_event - Notify the PM core of a wakeup event.
 * @dev: Device the wakeup event is related to.
 * @msec: Anticipated event processing time (in milliseconds).
 *
 * Call __pm_wakeup_event() for the @dev's wakeup source object.
 */
void pm_wakeup_event(struct device *dev, unsigned int msec)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_wakeup_event(dev->power.wakeup, msec);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_wakeup_event);

/**
 * pm_wakeup_pending - Check if power transition in progress should be aborted.
 *
 * Compare the current number of registered wakeup events with its preserved
 * value from the past and return true if new wakeup events have been registered
 * since the old value was stored.  Also return true if the current number of
 * wakeup events being processed is different from zero.
 */
bool pm_wakeup_pending(void)
{
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&events_lock, flags);
	if (events_check_enabled) {
		unsigned int cnt, inpr;

		split_counters(&cnt, &inpr);
		ret = (cnt != saved_count || inpr > 0);
		events_check_enabled = !ret;
	}
	spin_unlock_irqrestore(&events_lock, flags);
	return ret;
}

/**
 * pm_get_wakeup_count - Read the number of registered wakeup events.
 * @count: Address to store the value at.
 * @block: Whether or not to block.
 *
 * Store the number of registered wakeup events at the address in @count.  If
 * @block is set, block until the current number of wakeup events being
 * processed is zero.
 *
 * Return 'false' if the current number of wakeup events being processed is
 * nonzero.  Otherwise return 'true'.
 */
bool pm_get_wakeup_count(unsigned int *count, bool block)
{
	unsigned int cnt, inpr;

	if (block) {
		DEFINE_WAIT(wait);

		for (;;) {
			prepare_to_wait(&wakeup_count_wait_queue, &wait,
					TASK_INTERRUPTIBLE);
			split_counters(&cnt, &inpr);
			if (inpr == 0 || signal_pending(current))
				break;

			schedule();
		}
		finish_wait(&wakeup_count_wait_queue, &wait);
	}

	split_counters(&cnt, &inpr);
	*count = cnt;
	return !inpr;
}

/**
 * pm_save_wakeup_count - Save the current number of registered wakeup events.
 * @count: Value to compare with the current number of registered wakeup events.
 *
 * If @count is equal to the current number of registered wakeup events and the
 * current number of wakeup events being processed is zero, store @count as the
 * old number of registered wakeup events for pm_check_wakeup_events(), enable
 * wakeup events detection and return 'true'.  Otherwise disable wakeup events
 * detection and return 'false'.
 */
bool pm_save_wakeup_count(unsigned int count)
{
	unsigned int cnt, inpr;

	events_check_enabled = false;
	spin_lock_irq(&events_lock);
	split_counters(&cnt, &inpr);
	if (cnt == count && inpr == 0) {
		saved_count = count;
		events_check_enabled = true;
	}
	spin_unlock_irq(&events_lock);
	return events_check_enabled;
}

#ifdef CONFIG_PM_AUTOSLEEP
/**
 * pm_wakep_autosleep_enabled - Modify autosleep_enabled for all wakeup sources.
 * @enabled: Whether to set or to clear the autosleep_enabled flags.
 */
void pm_wakep_autosleep_enabled(bool set)
{
	struct wakeup_source *ws;
	ktime_t now = ktime_get();

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
		spin_lock_irq(&ws->lock);
		if (ws->autosleep_enabled != set) {
			ws->autosleep_enabled = set;
			if (ws->active) {
				if (set)
					ws->start_prevent_time = now;
				else
					update_prevent_sleep_time(ws, now);
			}
		}
		spin_unlock_irq(&ws->lock);
	}
	rcu_read_unlock();
}
#endif /* CONFIG_PM_AUTOSLEEP */

static struct dentry *wakeup_sources_stats_dentry;

/**
 * print_wakeup_source_stats - Print wakeup source statistics information.
 * @m: seq_file to print the statistics into.
 * @ws: Wakeup source object to print the statistics for.
 */
static int print_wakeup_source_stats(struct seq_file *m,
				     struct wakeup_source *ws)
{
	unsigned long flags;
	ktime_t total_time;
	ktime_t max_time;
	unsigned long active_count;
	ktime_t active_time;
	ktime_t prevent_sleep_time;
	int ret;

	spin_lock_irqsave(&ws->lock, flags);

	total_time = ws->total_time;
	max_time = ws->max_time;
	prevent_sleep_time = ws->prevent_sleep_time;
	active_count = ws->active_count;
	if (ws->active) {
		ktime_t now = ktime_get();

		active_time = ktime_sub(now, ws->last_time);
		total_time = ktime_add(total_time, active_time);
		if (active_time.tv64 > max_time.tv64)
			max_time = active_time;

		if (ws->autosleep_enabled)
			prevent_sleep_time = ktime_add(prevent_sleep_time,
				ktime_sub(now, ws->start_prevent_time));
	} else {
		active_time = ktime_set(0, 0);
	}

	ret = seq_printf(m, "%-12s\t%lu\t\t%lu\t\t%lu\t\t%lu\t\t"
			"%lld\t\t%lld\t\t%lld\t\t%lld\t\t%lld\n",
			ws->name, active_count, ws->event_count,
			ws->wakeup_count, ws->expire_count,
			ktime_to_ms(active_time), ktime_to_ms(total_time),
			ktime_to_ms(max_time), ktime_to_ms(ws->last_time),
			ktime_to_ms(prevent_sleep_time));

	spin_unlock_irqrestore(&ws->lock, flags);

	return ret;
}

/**
 * wakeup_sources_stats_show - Print wakeup sources statistics information.
 * @m: seq_file to print the statistics into.
 */
/** 20151121
 * wakeup_sources 목록을 순회하며 정보를 출력한다.
 *
 * wakeup_source_add 함수로 등록.
 * 
 * # cat wakeup_sources
 * name            active_count    event_count     wakeup_count    expire_count    active_since    total_time      max_time        last_change     prevent_suspend_time
 * 00:03           0               0               0               0               0               0               0               14077           0
 * 0000:02:00.0    0               0               0               0               0               0               0               14072           0
 * 0000:02:03.0    0               0               0               0               0               0               0               14070           0
 * alarmtimer      0               0               0               0               0               0               0               10910           0
 * LNXPWRBN:00     0               0               0               0               0               0               0               6567            0
 **/
static int wakeup_sources_stats_show(struct seq_file *m, void *unused)
{
	struct wakeup_source *ws;

	seq_puts(m, "name\t\tactive_count\tevent_count\twakeup_count\t"
		"expire_count\tactive_since\ttotal_time\tmax_time\t"
		"last_change\tprevent_suspend_time\n");

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry)
		print_wakeup_source_stats(m, ws);
	rcu_read_unlock();

	return 0;
}

/** 20151114
 * seq_file을 사용하므로 wakeup_sources_stats_open은 seq_open을 호출해야 한다.
 * single interface를 사용해 seq_open과 관련된 함수 구현을 대체하였다.
 **/
static int wakeup_sources_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, wakeup_sources_stats_show, NULL);
}

/** 20151114
 * wakeup_source_stats 파일에 대한 vfs 콜백 함수를 정의한다.
 * seq_file 인터페이스를 사용한다.
 **/
static const struct file_operations wakeup_sources_stats_fops = {
	.owner = THIS_MODULE,
	.open = wakeup_sources_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/** 20151114
 * "wakeup_source"라는 debugfs 파일을 생성한다.
 * parent가 지정되지 않았으므로 루트에 생성된다.
 **/
static int __init wakeup_sources_debugfs_init(void)
{
	wakeup_sources_stats_dentry = debugfs_create_file("wakeup_sources",
			S_IRUGO, NULL, NULL, &wakeup_sources_stats_fops);
	return 0;
}

postcore_initcall(wakeup_sources_debugfs_init);
