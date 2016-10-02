/*
 *  arch/arm/include/asm/mmu_context.h
 *
 *  Copyright (C) 1996 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Changelog:
 *   27-06-1996	RMK	Created
 */
#ifndef __ASM_ARM_MMU_CONTEXT_H
#define __ASM_ARM_MMU_CONTEXT_H

#include <linux/compiler.h>
#include <linux/sched.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/proc-fns.h>
#include <asm-generic/mm_hooks.h>

void __check_kvm_seq(struct mm_struct *mm);

/** 20160604
 * Cortex-A 시리즈는 ASID를 설정할 수 있는 CONTEXTIDR 레지스터를 제공한다.
 **/
#ifdef CONFIG_CPU_HAS_ASID

/*
 * On ARMv6, we have the following structure in the Context ID:
 *
 * 31                         7          0
 * +-------------------------+-----------+
 * |      process ID         |   ASID    |
 * +-------------------------+-----------+
 * |              context ID             |
 * +-------------------------------------+
 *
 * The ASID is used to tag entries in the CPU caches and TLBs.
 * The context ID is used by debuggers and trace logic, and
 * should be unique within all running processes.
 */
#define ASID_BITS		8
#define ASID_MASK		((~0) << ASID_BITS)
#define ASID_FIRST_VERSION	(1 << ASID_BITS)
/** 20160604
 * ARMv7은 hw에서 ASID로 8비트를 제공한다.
 * 상위 비트는 generation으로 사용하여 context switch시 다른 generation일 경우
 * 새로운 ASID를 발급 받는다.
 *
 * 새 프로세스에게 마지막으로 사용된 ASID를 하나씩 증가시켜 배정하며,
 * 마지막 값에 도달하면 TLB를 flush하고 이전 generation의 번호는 invalid 시킨다.
 *
 * percpu를 포함해 ASID 관리방식이 이후 커널 버전에서 많이 변경되었다.
 * 새로운 커널 버전을 참고.
 **/

extern unsigned int cpu_last_asid;

void __init_new_context(struct task_struct *tsk, struct mm_struct *mm);
void __new_context(struct mm_struct *mm);
void cpu_set_reserved_ttbr0(void);

/** 20160604
 * mm에 새로운 context를 설정하고, 레지스터에 그 정보를 설정한다.
 **/
static inline void switch_new_context(struct mm_struct *mm)
{
	unsigned long flags;

	/** 20160604
	 * mm에 새로운 context를 생성한다.
	 **/
	__new_context(mm);

	/** 20160604
	 * interrupt를 막은 상태에서 (IPI로 reset_context가 실행되지 않도록)
	 * mm의 context 정보를 레지스터에 설정한다.
	 **/
	local_irq_save(flags);
	cpu_switch_mm(mm->pgd, mm);
	local_irq_restore(flags);
}

/** 20160604
 * context의 generation이 바뀌었는지 살펴보고,
 * 동일하다면 현재 mm의 context 정보로 레지스터를 설정하고,
 * 그렇지 않다면 새로 context를 생성해 레지스터를 설정한다.
 **/
static inline void check_and_switch_context(struct mm_struct *mm,
					    struct task_struct *tsk)
{
	if (unlikely(mm->context.kvm_seq != init_mm.context.kvm_seq))
		__check_kvm_seq(mm);

	/*
	 * Required during context switch to avoid speculative page table
	 * walking with the wrong TTBR.
	 */
	/**
	 * context switch시 잘못된 TTBR을 통해 page table walking을 하지 않도록
	 * ttbr1의 값을 ttbr0로 사용한다.
	 **/
	cpu_set_reserved_ttbr0();

	/** 20160604
	 * mm의 context.id와 cpu_last_asid의 generation을 비교해 같다면
	 * mm의 context를 그대로 사용하면 된다.
	 * mm의 pgd로 TTB를 변경하고 CONTEXTIDR 역시 새로 지정한다.
	 **/
	if (!((mm->context.id ^ cpu_last_asid) >> ASID_BITS))
		/*
		 * The ASID is from the current generation, just switch to the
		 * new pgd. This condition is only true for calls from
		 * context_switch() and interrupts are already disabled.
		 */
		cpu_switch_mm(mm->pgd, mm);
	/** 20160604
	 * generation이 다르다면 새로운 context로 실행해야 한다.
	 *
	 * interrupt disabled면 __new_context에서 IPI를 발송하지 못하므로
	 * thread info에 deferred 되었음을 표시만 한다.
	 **/
	else if (irqs_disabled())
		/*
		 * Defer the new ASID allocation until after the context
		 * switch critical region since __new_context() cannot be
		 * called with interrupts disabled (it sends IPIs).
		 */
		set_ti_thread_flag(task_thread_info(tsk), TIF_SWITCH_MM);
	/** 20160604
	 * generation이 달라 새로운 context를 생성해 전환시킨다.
	 **/
	else
		/*
		 * That is a direct call to switch_mm() or activate_mm() with
		 * interrupts enabled and a new context.
		 */
		switch_new_context(mm);
}

/** 20160416
 * 새 context를 초기화 한다.
 **/
#define init_new_context(tsk,mm)	(__init_new_context(tsk,mm),0)

/** 20160625
 * SWITCH_MM 플래그를 조회하고 클리어한다.
 * 지연되어 있었다면 context switch 한다.
 **/
#define finish_arch_post_lock_switch \
	finish_arch_post_lock_switch
static inline void finish_arch_post_lock_switch(void)
{
	/** 20160625
	 * TIF_SWITCH_MM 플래그를 리턴하고 클리어한다.
	 * 설정되어 있다면 해당 mm으로 switch 한다.
	 **/
	if (test_and_clear_thread_flag(TIF_SWITCH_MM))
		switch_new_context(current->mm);
}

#else	/* !CONFIG_CPU_HAS_ASID */

#ifdef CONFIG_MMU

static inline void check_and_switch_context(struct mm_struct *mm,
					    struct task_struct *tsk)
{
	if (unlikely(mm->context.kvm_seq != init_mm.context.kvm_seq))
		__check_kvm_seq(mm);

	if (irqs_disabled())
		/*
		 * cpu_switch_mm() needs to flush the VIVT caches. To avoid
		 * high interrupt latencies, defer the call and continue
		 * running with the old mm. Since we only support UP systems
		 * on non-ASID CPUs, the old mm will remain valid until the
		 * finish_arch_post_lock_switch() call.
		 */
		set_ti_thread_flag(task_thread_info(tsk), TIF_SWITCH_MM);
	else
		cpu_switch_mm(mm->pgd, mm);
}

#define finish_arch_post_lock_switch \
	finish_arch_post_lock_switch
static inline void finish_arch_post_lock_switch(void)
{
	if (test_and_clear_thread_flag(TIF_SWITCH_MM)) {
		struct mm_struct *mm = current->mm;
		cpu_switch_mm(mm->pgd, mm);
	}
}

#endif	/* CONFIG_MMU */

#define init_new_context(tsk,mm)	0

#endif	/* CONFIG_CPU_HAS_ASID */

#define destroy_context(mm)		do { } while(0)

/*
 * This is called when "tsk" is about to enter lazy TLB mode.
 *
 * mm:  describes the currently active mm context
 * tsk: task which is entering lazy tlb
 * cpu: cpu number which is entering lazy tlb
 *
 * tsk->mm will be NULL
 */
/** 20140426
 * NULL 함수
 **/
static inline void
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}

/*
 * This is the actual mm switch as far as the scheduler
 * is concerned.  No registers are touched.  We avoid
 * calling the CPU specific function when the mm hasn't
 * actually changed.
 */
/** 20160604
 * mm 설정을 prev에서 next로 교체한다.
 **/
static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
#ifdef CONFIG_MMU
	/** 20160604
	 * 이 코드를 실행하는 cpu 번호를 얻어온다.
	 **/
	unsigned int cpu = smp_processor_id();

#ifdef CONFIG_SMP
	/* check for possible thread migration */
	/** 20160604
	 * next mm의 cpumask에 설정되어 있고, 현재 cpu는 거기에 속해 있지 않다면
	 * 다른 core들까지 icache를 모두 flush시킨다.
	 **/
	if (!cpumask_empty(mm_cpumask(next)) &&
	    !cpumask_test_cpu(cpu, mm_cpumask(next)))
		__flush_icache_all();
#endif
	/** 20160604
	 * next의 cpumask에 현재 cpu가 설정되지 않았다면 cpumask로 새로 설정하고
	 * if문을 실행.
	 * 또는 이전에 설정되었을 경우라도 prev와 next가 같지 않을 경우
	 *   새로운 context로 변경한다.
	 **/
	if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next)) || prev != next) {
		check_and_switch_context(next, tsk);
		/** 20160604
		 * cache가 vivt 타입이면 cpu를 prev mmv에서 제거한다.
		 **/
		if (cache_is_vivt())
			cpumask_clear_cpu(cpu, mm_cpumask(prev));
	}
#endif
}

#define deactivate_mm(tsk,mm)	do { } while (0)
#define activate_mm(prev,next)	switch_mm(prev, next, NULL)

#endif
