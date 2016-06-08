/*
 *  linux/arch/arm/mm/context.c
 *
 *  Copyright (C) 2002-2003 Deep Blue Solutions Ltd, all rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/percpu.h>

#include <asm/mmu_context.h>
#include <asm/thread_notify.h>
#include <asm/tlbflush.h>

/** 20160604
 * ASID 관련 동작(hw 조작 포함)을 보호하기 위한 spinlock
 **/
static DEFINE_RAW_SPINLOCK(cpu_asid_lock);
/** 20160604
 * last asid. 초기값은 (1 << 8)
 *
 * 각 core는 이 값이 자신의 cpu 번호 + 1을 더해 asid로 삼는다.
 * 즉, core가 4개라면  generation id, cpu0, cpu1, cpu2, cpu3 순으로 사용된다.
 **/
unsigned int cpu_last_asid = ASID_FIRST_VERSION;

#ifdef CONFIG_ARM_LPAE
void cpu_set_reserved_ttbr0(void)
{
	unsigned long ttbl = __pa(swapper_pg_dir);
	unsigned long ttbh = 0;

	/*
	 * Set TTBR0 to swapper_pg_dir which contains only global entries. The
	 * ASID is set to 0.
	 */
	asm volatile(
	"	mcrr	p15, 0, %0, %1, c2		@ set TTBR0\n"
	:
	: "r" (ttbl), "r" (ttbh));
	isb();
}
#else
/** 20160604
 * 보존된 ttbr0 값을 설정한다.
 *
 * ttbr1을 읽어 ttbr0에 저장한다.
 **/
void cpu_set_reserved_ttbr0(void)
{
	u32 ttb;
	/* Copy TTBR1 into TTBR0 */
	asm volatile(
	"	mrc	p15, 0, %0, c2, c0, 1		@ read TTBR1\n"
	"	mcr	p15, 0, %0, c2, c0, 0		@ set TTBR0\n"
	: "=r" (ttb));
	/** 20160604
	 * TTBR 설정을 변경했으므로 barrier를 두어 설정이 적용된 후 다음 명령을
	 * 실행한다.
	 **/
	isb();
}
#endif

#ifdef CONFIG_PID_IN_CONTEXTIDR
static int contextidr_notifier(struct notifier_block *unused, unsigned long cmd,
			       void *t)
{
	u32 contextidr;
	pid_t pid;
	struct thread_info *thread = t;

	if (cmd != THREAD_NOTIFY_SWITCH)
		return NOTIFY_DONE;

	pid = task_pid_nr(thread->task) << ASID_BITS;
	asm volatile(
	"	mrc	p15, 0, %0, c13, c0, 1\n"
	"	bfi	%1, %0, #0, %2\n"
	"	mcr	p15, 0, %1, c13, c0, 1\n"
	: "=r" (contextidr), "+r" (pid)
	: "I" (ASID_BITS));
	isb();

	return NOTIFY_OK;
}

static struct notifier_block contextidr_notifier_block = {
	.notifier_call = contextidr_notifier,
};

static int __init contextidr_notifier_init(void)
{
	return thread_register_notifier(&contextidr_notifier_block);
}
arch_initcall(contextidr_notifier_init);
#endif

/*
 * We fork()ed a process, and we need a new context for the child
 * to run in.
 */
/** 20160416
 * process를 fork한 뒤, child를 위해 새로운 context가 필요하다.
 * 새 context를 생성하기 위해 context.id를 초기화.
 **/
void __init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	mm->context.id = 0;
	raw_spin_lock_init(&mm->context.id_lock);
}

/** 20160604
 * 보존된 TTBR0을 사용하고, translation table이 변경되었으므로 TLB를 비운다.
 **/
static void flush_context(void)
{
	/** 20160604
	 * ttbr0를 보존된 (ttbr1에 저장해둔) 값으로 설정한다.
	 **/
	cpu_set_reserved_ttbr0();
	/** 20160604
	 * translation table이 변경되었으므로 TLB를 flush시킨다.
	 * inner-shareable 옵션이 사용되어 IS domain의 모든 코어의 TLB를 비운다.
	 **/
	local_flush_tlb_all();
	/** 20160604
	 * icache type이 vivt asid tagged인 경우 icache flush.
	 **/
	if (icache_is_vivt_asid_tagged()) {
		__flush_icache_all();
		dsb();
	}
}

#ifdef CONFIG_SMP

/** 20160604
 * mm에 현재 core의 정보로 context를 설정한다.
 * context.id는 전달받은 asid로 설정.
 **/
static void set_mm_context(struct mm_struct *mm, unsigned int asid)
{
	unsigned long flags;

	/*
	 * Locking needed for multi-threaded applications where the
	 * same mm->context.id could be set from different CPUs during
	 * the broadcast. This function is also called via IPI so the
	 * mm->context.id_lock has to be IRQ-safe.
	 */
	/** 20160604
	 * context 설정 코드에 대해 spinlock으로 임계구역을 설정한다.
	 **/
	raw_spin_lock_irqsave(&mm->context.id_lock, flags);
	/** 20160604
	 * mm의 현재 context.id가 cpu_last_asid가 아닌 값으로 설정되어 있을 경우
	 * 즉, 다른 generation으로 설정되었을 경우
	 *   mm의 context.id를 넘겨받은 asid로 설정하고, cpumask를 클리어한다.
	 **/
	if (likely((mm->context.id ^ cpu_last_asid) >> ASID_BITS)) {
		/*
		 * Old version of ASID found. Set the new one and
		 * reset mm_cpumask(mm).
		 */
		mm->context.id = asid;
		cpumask_clear(mm_cpumask(mm));
	}
	raw_spin_unlock_irqrestore(&mm->context.id_lock, flags);

	/*
	 * Set the mm_cpumask(mm) bit for the current CPU.
	 */
	/** 20160604
	 * mm의 cpumask에 현재 cpu를 설정한다.
	 **/
	cpumask_set_cpu(smp_processor_id(), mm_cpumask(mm));
}

/*
 * Reset the ASID on the current CPU. This function call is broadcast
 * from the CPU handling the ASID rollover and holding cpu_asid_lock.
 */
/** 20160604
 * 현재 CPU에 ttbr0와 ASID를 재설정한다.
 *
 * ASID (8bit) rollover시 호출된다.
 **/
static void reset_context(void *info)
{
	/** 20160604
	 * 현재 cpu 번호를 받아오고, active_mm에 저장된 커널 mm을 받아온다.
	 **/
	unsigned int asid;
	unsigned int cpu = smp_processor_id();
	struct mm_struct *mm = current->active_mm;

	/** 20160604
	 * read memory barrier를 둔다.
	 *
	 * asid를 읽어온다.
	 **/
	smp_rmb();
	asid = cpu_last_asid + cpu + 1;

	/** 20160604
	 * 현재 context를 flush 한다 - ttbr0 설정 후 TLB flush.
	 * asid를 context.id로 지정한다.
	 **/
	flush_context();
	set_mm_context(mm, asid);

	/* set the new ASID */
	/** 20160604
	 * ttbr0 레지스터에 mm->pgd를 설정하고, CONTEXTIDR에 context.id를 쓴다.
	 **/
	cpu_switch_mm(mm->pgd, mm);
}

#else

static inline void set_mm_context(struct mm_struct *mm, unsigned int asid)
{
	mm->context.id = asid;
	cpumask_copy(mm_cpumask(mm), cpumask_of(smp_processor_id()));
}

#endif

/** 20160604
 * mm에 대해 새로운 context를 생성한다.
 **/
void __new_context(struct mm_struct *mm)
{
	unsigned int asid;

	raw_spin_lock(&cpu_asid_lock);
#ifdef CONFIG_SMP
	/*
	 * Check the ASID again, in case the change was broadcast from
	 * another CPU before we acquired the lock.
	 */
	/** 20160528
	 * 위의 spinlock을 얻기 전에 다른 CPU에서 asid change가 broadcast되었는지
	 * 체크한다.
	 **/
	if (unlikely(((mm->context.id ^ cpu_last_asid) >> ASID_BITS) == 0)) {
		cpumask_set_cpu(smp_processor_id(), mm_cpumask(mm));
		raw_spin_unlock(&cpu_asid_lock);
		return;
	}
#endif
	/*
	 * At this point, it is guaranteed that the current mm (with
	 * an old ASID) isn't active on any other CPU since the ASIDs
	 * are changed simultaneously via IPI.
	 */
	/** 20160604
	 * last asid를 하나 증가시킨다.
	 * 왜 cpu_last_asid를 1만 증가시키는 것일까??? smp_processor_id()???
	 **/
	asid = ++cpu_last_asid;
	/** 20160604
	 * asid가 overflow되어 0이 되면 초기값으로 설정한다.
	 **/
	if (asid == 0)
		asid = cpu_last_asid = ASID_FIRST_VERSION;

	/*
	 * If we've used up all our ASIDs, we need
	 * to start a new version and flush the TLB.
	 */
	/** 20160604
	 * asid의 ASID 파트를 검사해 0이 되었다면 ASID를 모두 소모하였으므로
	 * 새로운 generation을 시작한다 (rollover).
	 **/
	if (unlikely((asid & ~ASID_MASK) == 0)) {
		asid = cpu_last_asid + smp_processor_id() + 1;
		flush_context();
#ifdef CONFIG_SMP
		smp_wmb();
		/** 20160604
		 * IPI를 이용해 SMP의 다른 코어들이 reset_context 함수를
		 * 호출하게 한다. wait을 설정해 return을 받을 때까지 대기한다.
		 **/
		smp_call_function(reset_context, NULL, 1);
#endif
		cpu_last_asid += NR_CPUS;
	}

	set_mm_context(mm, asid);
	raw_spin_unlock(&cpu_asid_lock);
}
