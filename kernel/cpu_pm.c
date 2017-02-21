/*
 * Copyright (C) 2011 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/cpu_pm.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>

static DEFINE_RWLOCK(cpu_pm_notifier_lock);
static RAW_NOTIFIER_HEAD(cpu_pm_notifier_chain);

/** 20151010
 * cpu_pm_notifier_chain에 등록된 nb에 event를 보낸다.
 **/
static int cpu_pm_notify(enum cpu_pm_event event, int nr_to_call, int *nr_calls)
{
	int ret;

	ret = __raw_notifier_call_chain(&cpu_pm_notifier_chain, event, NULL,
		nr_to_call, nr_calls);

	return notifier_to_errno(ret);
}

/**
 * cpu_pm_register_notifier - register a driver with cpu_pm
 * @nb: notifier block to register
 *
 * Add a driver to a list of drivers that are notified about
 * CPU and CPU cluster low power entry and exit.
 *
 * This function may sleep, and has the same return conditions as
 * raw_notifier_chain_register.
 */
/** 20140913
 * cpu_pm_notifier_chain에 write lock을 걸고 nb를 등록한다.
 **/
int cpu_pm_register_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	write_lock_irqsave(&cpu_pm_notifier_lock, flags);
	ret = raw_notifier_chain_register(&cpu_pm_notifier_chain, nb);
	write_unlock_irqrestore(&cpu_pm_notifier_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_register_notifier);

/**
 * cpu_pm_unregister_notifier - unregister a driver with cpu_pm
 * @nb: notifier block to be unregistered
 *
 * Remove a driver from the CPU PM notifier list.
 *
 * This function may sleep, and has the same return conditions as
 * raw_notifier_chain_unregister.
 */
int cpu_pm_unregister_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	write_lock_irqsave(&cpu_pm_notifier_lock, flags);
	ret = raw_notifier_chain_unregister(&cpu_pm_notifier_chain, nb);
	write_unlock_irqrestore(&cpu_pm_notifier_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_unregister_notifier);

/**
 * cpu_pm_enter - CPU low power entry notifier
 *
 * Notifies listeners that a single CPU is entering a low power state that may
 * cause some blocks in the same power domain as the cpu to reset.
 *
 * Must be called on the affected CPU with interrupts disabled.  Platform is
 * responsible for ensuring that cpu_pm_enter is not called twice on the same
 * CPU before cpu_pm_exit is called. Notified drivers can include VFP
 * co-processor, interrupt controller and its PM extensions, local CPU
 * timers context save/restore which shouldn't be interrupted. Hence it
 * must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
/** 20151010
 * cpu가 pm enter 상태로 진입한다. notifier chain에 notify한다.
 **/
int cpu_pm_enter(void)
{
	int nr_calls;
	int ret = 0;

	/** 20151010
	 * read_lock구간에서 cpu_pm_notifier chain에 CPU_PM_ENTER notify를 날린다.
	 **/
	read_lock(&cpu_pm_notifier_lock);
	ret = cpu_pm_notify(CPU_PM_ENTER, -1, &nr_calls);
	if (ret)
		/*
		 * Inform listeners (nr_calls - 1) about failure of CPU PM
		 * PM entry who are notified earlier to prepare for it.
		 */
		cpu_pm_notify(CPU_PM_ENTER_FAILED, nr_calls - 1, NULL);
	read_unlock(&cpu_pm_notifier_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_enter);

/**
 * cpu_pm_exit - CPU low power exit notifier
 *
 * Notifies listeners that a single CPU is exiting a low power state that may
 * have caused some blocks in the same power domain as the cpu to reset.
 *
 * Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_pm_exit(void)
{
	int ret;

	read_lock(&cpu_pm_notifier_lock);
	ret = cpu_pm_notify(CPU_PM_EXIT, -1, NULL);
	read_unlock(&cpu_pm_notifier_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_exit);

/**
 * cpu_cluster_pm_enter - CPU cluster low power entry notifier
 *
 * Notifies listeners that all cpus in a power domain are entering a low power
 * state that may cause some blocks in the same power domain to reset.
 *
 * Must be called after cpu_pm_enter has been called on all cpus in the power
 * domain, and before cpu_pm_exit has been called on any cpu in the power
 * domain. Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
/** 20151010
 * cpu_pm notifier chain에 CPU_CLUSTER_PM_ENTER를 날린다.
 **/
int cpu_cluster_pm_enter(void)
{
	int nr_calls;
	int ret = 0;

	read_lock(&cpu_pm_notifier_lock);
	ret = cpu_pm_notify(CPU_CLUSTER_PM_ENTER, -1, &nr_calls);
	if (ret)
		/*
		 * Inform listeners (nr_calls - 1) about failure of CPU cluster
		 * PM entry who are notified earlier to prepare for it.
		 */
		cpu_pm_notify(CPU_CLUSTER_PM_ENTER_FAILED, nr_calls - 1, NULL);
	read_unlock(&cpu_pm_notifier_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_cluster_pm_enter);

/**
 * cpu_cluster_pm_exit - CPU cluster low power exit notifier
 *
 * Notifies listeners that all cpus in a power domain are exiting form a
 * low power state that may have caused some blocks in the same power domain
 * to reset.
 *
 * Must be called after cpu_pm_exit has been called on all cpus in the power
 * domain, and before cpu_pm_exit has been called on any cpu in the power
 * domain. Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_cluster_pm_exit(void)
{
	int ret;

	read_lock(&cpu_pm_notifier_lock);
	ret = cpu_pm_notify(CPU_CLUSTER_PM_EXIT, -1, NULL);
	read_unlock(&cpu_pm_notifier_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(cpu_cluster_pm_exit);

#ifdef CONFIG_PM
/** 20151010
 * cpu pm의 suspend 과정을 수행한다.
 **/
static int cpu_pm_suspend(void)
{
	int ret;

	/** 20151010
	 * cpu를 pm enter 상태로 진입한다.
	 **/
	ret = cpu_pm_enter();
	if (ret)
		return ret;

	/** 20151010
	 * cpu cluster를 pm enter 상태로 진입한다.
	 **/
	ret = cpu_cluster_pm_enter();
	return ret;
}

/** 20151010
 * cpu pm의 resume 과정을 수행한다.
 **/
static void cpu_pm_resume(void)
{
	cpu_cluster_pm_exit();
	cpu_pm_exit();
}

static struct syscore_ops cpu_pm_syscore_ops = {
	.suspend = cpu_pm_suspend,
	.resume = cpu_pm_resume,
};

/** 20151010
 * cpu pm 관련 초기화로 cpu pm syscore_ops를 등록한다.
 **/
static int cpu_pm_init(void)
{
	/** 20151010
	 * cpu_pm_syscore_ops라는 syscore_ops를 등록한다.
	 **/
	register_syscore_ops(&cpu_pm_syscore_ops);
	return 0;
}
core_initcall(cpu_pm_init);
#endif
