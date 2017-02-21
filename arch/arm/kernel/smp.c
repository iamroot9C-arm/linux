/*
 *  linux/arch/arm/kernel/smp.c
 *
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cache.h>
#include <linux/profile.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/clockchips.h>
#include <linux/completion.h>

#include <linux/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/exception.h>
#include <asm/idmap.h>
#include <asm/topology.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/processor.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/localtimer.h>
#include <asm/smp_plat.h>

/*
 * as from 2.5, kernels no longer have an init_tasks structure
 * so we need some other way of telling a new secondary core
 * where to place its SVC stack
 */
/** 20150118
 * SVC 모드 stack의 위치를 알려주기 위한 전역 변수.
 *
 * __cpu_up에서 초기화하고, head.S에서 사용한다.
 **/
struct secondary_data secondary_data;

/** 20130713
 * IPI_RESCHEDULE 사용 예)
 * load_balance()로부터 시작해서 resched_task까지 도달해 smp_cross_call로 전송
 *
 * IPI_CALL_FUNC_SINGLE 사용 예)
 * flush_tlb_page()로부터 시작해서 smp_call_function_single에서 func으로
 * ipi_flush_tlb_page 지정해 함수가 호출되도록 한다.
 *
 * [참고] http://en.wikipedia.org/wiki/Inter-processor_interrupt
 **/
enum ipi_msg_type {
	IPI_TIMER = 2,
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
};

/** 20150808
 * 부팅이 완료된 cpu의 동작완료를 대기하기 위한 wait - complete.
 **/
static DECLARE_COMPLETION(cpu_running);

/** 20150801
 * cpu를 부팅시킨다.
 *
 * cpu에게 전달할 secondary_data를 채우고, 
 * platform 에서 지정된 코드를 통해 부팅시킨다.
 **/
int __cpuinit __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	int ret;

	/*
	 * We need to tell the secondary core where to find
	 * its stack and the page tables.
	 */
	/** 20150118
	 * secondary core가 필요한 secondary_data 구조체를 초기화 한다.
	 *
	 * secondary_startup에서 사용된다.
	 **/
	secondary_data.stack = task_stack_page(idle) + THREAD_START_SP;
	secondary_data.pgdir = virt_to_phys(idmap_pgd);
	secondary_data.swapper_pg_dir = virt_to_phys(swapper_pg_dir);
	/** 20150118
	 * secondary_data 영역을 flush 한다.
	 *   L1, L2 캐시의 순서로 clean 한다.
	 **/
	__cpuc_flush_dcache_area(&secondary_data, sizeof(secondary_data));
	outer_clean_range(__pa(&secondary_data), __pa(&secondary_data + 1));

	/*
	 * Now bring the CPU into our world.
	 */
	/** 20150801
	 * cpu를 booting 시킨다.
	 *
	 * 실패시 failed to boot 메모리가 출력된다.
	 **/
	ret = boot_secondary(cpu, idle);
	if (ret == 0) {
		/*
		 * CPU was successfully started, wait for it
		 * to come online or time out.
		 */
		/** 20150801
		 * secondary_start_kernel에서 kernel 부팅이 진행되어 complete시킬 때까지
		 * timeout을 대기한다.
		 **/
		wait_for_completion_timeout(&cpu_running,
						 msecs_to_jiffies(1000));

		/** 20150801
		 * 해당 cpu가 online mask에 추가되지 않았다면 에러.
		 **/
		if (!cpu_online(cpu)) {
			pr_crit("CPU%u: failed to come online\n", cpu);
			ret = -EIO;
		}
	} else {
		pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
	}

	/** 20150801
	 * secondary_data의 stack과 pgdir을 비워준다.
	 **/
	secondary_data.stack = NULL;
	secondary_data.pgdir = 0;

	return ret;
}

#ifdef CONFIG_HOTPLUG_CPU
static void percpu_timer_stop(void);

/*
 * __cpu_disable runs on the processor to be shutdown.
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	int ret;

	ret = platform_cpu_disable(cpu);
	if (ret)
		return ret;

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	set_cpu_online(cpu, false);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	migrate_irqs();

	/*
	 * Stop the local timer for this CPU.
	 */
	percpu_timer_stop();

	/*
	 * Flush user cache and TLB mappings, and then remove this CPU
	 * from the vm mask set of all processes.
	 */
	flush_cache_all();
	local_flush_tlb_all();

	clear_tasks_mm_cpumask(cpu);

	return 0;
}

static DECLARE_COMPLETION(cpu_died);

/*
 * called on the thread which is asking for a CPU to be shutdown -
 * waits until shutdown has completed, or it is timed out.
 */
void __cpu_die(unsigned int cpu)
{
	if (!wait_for_completion_timeout(&cpu_died, msecs_to_jiffies(5000))) {
		pr_err("CPU%u: cpu didn't die\n", cpu);
		return;
	}
	printk(KERN_NOTICE "CPU%u: shutdown\n", cpu);

	if (!platform_cpu_kill(cpu))
		printk("CPU%u: unable to kill\n", cpu);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 * Note that we disable IRQs here, but do not re-enable them
 * before returning to the caller. This is also the behaviour
 * of the other hotplug-cpu capable cores, so presumably coming
 * out of idle fixes this.
 */
void __ref cpu_die(void)
{
	unsigned int cpu = smp_processor_id();

	idle_task_exit();

	local_irq_disable();
	mb();

	/* Tell __cpu_die() that this CPU is now safe to dispose of */
	RCU_NONIDLE(complete(&cpu_died));

	/*
	 * actual CPU shutdown procedure is at least platform (if not
	 * CPU) specific.
	 */
	platform_cpu_die(cpu);

	/*
	 * Do not return to the idle loop - jump back to the secondary
	 * cpu initialisation.  There's some initialisation which needs
	 * to be repeated to undo the effects of taking the CPU offline.
	 */
	__asm__("mov	sp, %0\n"
	"	mov	fp, #0\n"
	"	b	secondary_start_kernel"
		:
		: "r" (task_stack_page(current) + THREAD_SIZE - 8));
}
#endif /* CONFIG_HOTPLUG_CPU */

/*
 * Called by both boot and secondaries to move global data into
 * per-processor storage.
 */
/** 20150606
 * 지정된 cpu의 percpu 변수인 cpu_info에 loops_per_jiffy를 저장한다.
 * cpuid의 cpu topology 정보를 저장하고, 다른 cpu의 정보에 업데이트 한다.
 **/
static void __cpuinit smp_store_cpu_info(unsigned int cpuid)
{
	struct cpuinfo_arm *cpu_info = &per_cpu(cpu_data, cpuid);

	cpu_info->loops_per_jiffy = loops_per_jiffy;

	store_cpu_topology(cpuid);
}

static void percpu_timer_setup(void);

/*
 * This is the secondary CPU boot entry.  We're using this CPUs
 * idle thread stack, but a set of temporary page tables.
 */
/** 20150118
 * 부팅된 secondary cpu에서 실행하는 함수.
 *
 * mmu가 enable 된 상태이며, 가장 먼저 init_mm으로 translation table을 교체한다.
 * 이후 cpu 초기화 등 커널 수준에서 실행해야 하는 작업을 진행한다.
 *
 * 이후 interrupt를 활성화 시키고 idle 상태로 진입해 scheduler에 의해 동작한다.
 **/
asmlinkage void __cpuinit secondary_start_kernel(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int cpu = smp_processor_id();

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	/** 20150801
	 * mm의 reference count를 증가시키고,
	 * current task의 active_mm으로 mm을 지정한다.
	 **/
	atomic_inc(&mm->mm_count);
	current->active_mm = mm;
	/** 20150801
	 * mm의 cpumask에 cpu를 지정한다.
	 **/
	cpumask_set_cpu(cpu, mm_cpumask(mm));
	/** 20150801
	 * translation table base 레지스터를 설정한다.
	 *
	 * init_mm의 pgd는 swapper_pg_dir. 
	 **/
	cpu_switch_mm(mm->pgd, mm);
	enter_lazy_tlb(mm, current);
	/** 20150801
	 * pgd가 변경되었으니 이전 tlb를 flush 한다.
	 **/
	local_flush_tlb_all();

	/** 20150801
	 * 해당 cpu가 부팅되었음을 메시지로 출력한다.
	 **/
	printk("CPU%u: Booted secondary processor\n", cpu);

	/** 20150808
	 * 부팅 cpu의 초기화.
	 *
	 * per-cpu로 und, irq, abr 상태의 stacks 주소를 설정한다.
	 **/
	cpu_init();
	/** 20150808
	 * 선점 불가 상태로 진행
	 **/
	preempt_disable();
	trace_hardirqs_off();

	/*
	 * Give the platform a chance to do its own initialisation.
	 */
	/** 20150808
	 * platform에서 제공하는 부팅 이후 초기화 작업을 수행한다.
	 **/
	platform_secondary_init(cpu);

	/** 20150808
	 * 이 cpu가 깨어났음을 cpu_notify로 통보한다.
	 *
	 * - sched는 active mask에 이 cpu를 추가한다.
	 **/
	notify_cpu_starting(cpu);

	/** 20150808
	 * 해당 core의 bogoMips를 계산한다. 출력은 booting core에서만 호출한다.
	 **/
	calibrate_delay();

	/** 20150808
	 * loops_per_jiffy와 topology 등 cpu 관련 정보를 해당 core 변수에 저장한다.
	 **/
	smp_store_cpu_info(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue.  Wait for
	 * the CPU migration code to notice that the CPU is online
	 * before we continue - which happens after __cpu_up returns.
	 */
	/** 20150808
	 * 해당 cpu를 online mask에 추가한다.
	 * kernel 함수로 부팅 완료를 대기 중이던 boot core에게 완료를 통지한다.
	 **/
	set_cpu_online(cpu, true);
	complete(&cpu_running);

	/*
	 * Setup the percpu timer for this CPU.
	 */
	/** 20150808
	 * booting cpu를 제외한 smp의 각 cpu마다 수행되어
	 * 이 cpu를 위한 percpu timer를 설정한다.
	 **/
	percpu_timer_setup();

	/** 20150808
	 * cpu에서 irq와 fiq 신호를 받도록 설정한다.
	 **/
	local_irq_enable();
	local_fiq_enable();

	/*
	 * OK, it's off to the idle thread for us
	 */
	/** 20150808
	 * preempt_disable 상태로 idle 상태로 진입해 스케쥴링을 시작한다.
	 **/
	cpu_idle();
}

/** 20150808
 * smp 작업이 완료된 뒤 호출되는 함수로 bogomips를 합산해 출력한다.
 **/
void __init smp_cpus_done(unsigned int max_cpus)
{
	int cpu;
	unsigned long bogosum = 0;

	/** 20150808
	 * online 상태의 cpu들을 순회하며 각 core에서 가지고 있는
	 * loops_per_jiffy를 update하여 bogosum을 계산한다.
	 *
	 * 부팅된 cpu가 percpu 변수에 전역변수를 복사해 가지고 있는 상태이다.
	 **/
	for_each_online_cpu(cpu)
		bogosum += per_cpu(cpu_data, cpu).loops_per_jiffy;

	/** 20150808
	 * qemu vexpress 출력 예:
	 * SMP: Total of 4 processors activated (1216.10 BogoMIPS).
	 **/
	printk(KERN_INFO "SMP: Total of %d processors activated "
	       "(%lu.%02lu BogoMIPS).\n",
	       num_online_cpus(),
	       bogosum / (500000/HZ),
	       (bogosum / (5000/HZ)) % 100);
}

void __init smp_prepare_boot_cpu(void)
{
}

/** 20150613
 * smp_init 수행 전에 준비 작업을 한다.
 **/
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	/** 20150606
	 * possible cpu의 수를 구한다.
	 **/
	unsigned int ncores = num_possible_cpus();

	/** 20150606
	 * cpu topology 구조체 초기화.
	 * 하나의 cpu만 돌고 있을 때 호출한다.
	 **/
	init_cpu_topology();

	/** 20150606
	 * 현재 init이 수행 중인 cpu의 정보를 저장한다.
	 **/
	smp_store_cpu_info(smp_processor_id());

	/*
	 * are we trying to boot more cores than exist?
	 */
	if (max_cpus > ncores)
		max_cpus = ncores;
	/** 20150606
	 * 동작할 수 있는 core가 2개 이상일 경우
	 **/
	if (ncores > 1 && max_cpus) {
		/*
		 * Enable the local timer or broadcast device for the
		 * boot CPU, but only if we have more than one CPU.
		 */
		/** 20150606
		 * boot cpu에 대한 local timer로 설정을 시도하고,
		 * 실패하면 broadcast timer로 설정한다.
		 **/
		percpu_timer_setup();

		/*
		 * Initialise the present map, which describes the set of CPUs
		 * actually populated at the present time. A platform should
		 * re-initialize the map in platform_smp_prepare_cpus() if
		 * present != possible (e.g. physical hotplug).
		 */
		/** 20150613
		 * possible mask를 복사해 present mask를 채운다.
		 *
		 * present와 possible이 같지 않는 platform인 경우
		 * platform_smp_prepare_cpus에서 재 초기화.
		 **/
		init_cpu_present(cpu_possible_mask);

		/*
		 * Initialise the SCU if there are more than one CPU
		 * and let them know where to start.
		 */
		platform_smp_prepare_cpus(max_cpus);
	}
}

/** 20130518
 * vexpress의 경우 ct_ca9x4_init_cpu_map 에서 gic_raise_softirq 등록
 **/
static void (*smp_cross_call)(const struct cpumask *, unsigned int);

/** 20130518
 * smp_cross_call 에 함수 포인터 등록
 *   vexpress의 경우 gic_raise_softirq
 **/
void __init set_smp_cross_call(void (*fn)(const struct cpumask *, unsigned int))
{
	smp_cross_call = fn;
}

/** 20140621
 * smp_cross_call 함수로 IPI_CALL_FUNC irq를 mask에 해당하는 cpu에 날린다. 
 **/
void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	/** 20130713
	 * vexpress의 경우 gic_raise_softirq 호출됨
	 **/
	smp_cross_call(mask, IPI_CALL_FUNC);
}

/** 20140621
 * cpu에게 smp_cross_call (vexpress의 경우 gic_raise_softirq)을 통해
 * IPI_CALL_FUNC_SINGLE irq를 날린다.
 **/
void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_CALL_FUNC_SINGLE);
}

static const char *ipi_types[NR_IPI] = {
#define S(x,s)	[x - IPI_TIMER] = s
	S(IPI_TIMER, "Timer broadcast interrupts"),
	S(IPI_RESCHEDULE, "Rescheduling interrupts"),
	S(IPI_CALL_FUNC, "Function call interrupts"),
	S(IPI_CALL_FUNC_SINGLE, "Single function call interrupts"),
	S(IPI_CPU_STOP, "CPU stop interrupts"),
};

void show_ipi_list(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < NR_IPI; i++) {
		seq_printf(p, "%*s%u: ", prec - 1, "IPI", i);

		for_each_present_cpu(cpu)
			seq_printf(p, "%10u ",
				   __get_irq_stat(cpu, ipi_irqs[i]));

		seq_printf(p, " %s\n", ipi_types[i]);
	}
}

u64 smp_irq_stat_cpu(unsigned int cpu)
{
	u64 sum = 0;
	int i;

	for (i = 0; i < NR_IPI; i++)
		sum += __get_irq_stat(cpu, ipi_irqs[i]);

	return sum;
}

/*
 * Timer (local or broadcast) support
 */
/** 20150606
 * percpu clock_event_device.
 **/
static DEFINE_PER_CPU(struct clock_event_device, percpu_clockevent);

/** 20140830
 * IPI_TIMER 발생시 호출 함수.
 *
 * clock_event_device를 가져와 처리한다.
 **/
static void ipi_timer(void)
{
	struct clock_event_device *evt = &__get_cpu_var(percpu_clockevent);
	/** 20140830
	 * event_handler 호출.
	 *
	 * tick_handle_periodic가 지정되어 있음.
	 **/
	evt->event_handler(evt);
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/** 20141129
 * clockevents를 broadcast 할 때 IPI_TIMER를 smp call 한다.
 * vexpress에서 등록이 되지 않아 호출되지 않았다.
 **/
static void smp_timer_broadcast(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_TIMER);
}
#else
#define smp_timer_broadcast	NULL
#endif

static void broadcast_timer_set_mode(enum clock_event_mode mode,
	struct clock_event_device *evt)
{
}

/** 20150613
 * broadcast event timer로 dummy_timer를 설정하고 등록시킨다.
 **/
static void __cpuinit broadcast_timer_setup(struct clock_event_device *evt)
{
	/** 20150606
	 * dummy_timer clock event device를 선언하고 등록한다. 
	 **/
	evt->name	= "dummy_timer";
	evt->features	= CLOCK_EVT_FEAT_ONESHOT |
			  CLOCK_EVT_FEAT_PERIODIC |
			  CLOCK_EVT_FEAT_DUMMY;
	evt->rating	= 400;
	evt->mult	= 1;
	evt->set_mode	= broadcast_timer_set_mode;

	clockevents_register_device(evt);
}

/** 20140920
 * vexpress의 twd의 경우 twd_local_timer_common_register에서 twd_lt_ops가 등록됨.
 **/
static struct local_timer_ops *lt_ops;

#ifdef CONFIG_LOCAL_TIMERS
/** 20140920
 * 전달받은 local timer ops를 등록한다.
 **/
int local_timer_register(struct local_timer_ops *ops)
{
	if (!is_smp() || !setup_max_cpus)
		return -ENXIO;

	if (lt_ops)
		return -EBUSY;

	lt_ops = ops;
	return 0;
}
#endif

/** 20150606
 * 각 프로세서에서 percpu_clockevent를 설정하고,
 * local timer로 설정하여 실패하면 broadcast timer로 등록한다.
 **/
static void __cpuinit percpu_timer_setup(void)
{
	/** 20150606
	 * 현재 함수가 호출되는 cpu의 percpu_clockevent 구조체를 설정한다.
	 **/
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(percpu_clockevent, cpu);

	/** 20150606
	 * cpumask를 현재 cpu로 지정해 설정한다.
	 **/
	evt->cpumask = cpumask_of(cpu);
	evt->broadcast = smp_timer_broadcast;

	/** 20150606
	 * lt_ops가 없거나, lt_ops가 존재하지만 setup 실행이 실패할 경우 (0이 아닌값)
	 * broadcast_timer_setup을 호출한다.
	 *
	 * vexpress의 경우 twd_lt_ops
	 *	.setup	= twd_timer_setup,
	 *  .stop	= twd_timer_stop,
	 **/
	if (!lt_ops || lt_ops->setup(evt))
		broadcast_timer_setup(evt);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The generic clock events code purposely does not stop the local timer
 * on CPU_DEAD/CPU_DEAD_FROZEN hotplug events, so we have to do it
 * manually here.
 */
static void percpu_timer_stop(void)
{
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(percpu_clockevent, cpu);

	if (lt_ops)
		lt_ops->stop(evt);
}
#endif

static DEFINE_RAW_SPINLOCK(stop_lock);

/*
 * ipi_cpu_stop - handle IPI from smp_send_stop()
 */
static void ipi_cpu_stop(unsigned int cpu)
{
	if (system_state == SYSTEM_BOOTING ||
	    system_state == SYSTEM_RUNNING) {
		raw_spin_lock(&stop_lock);
		printk(KERN_CRIT "CPU%u: stopping\n", cpu);
		dump_stack();
		raw_spin_unlock(&stop_lock);
	}

	set_cpu_online(cpu, false);

	local_fiq_disable();
	local_irq_disable();

	while (1)
		cpu_relax();
}

/*
 * Main handler for inter-processor interrupts
 */
/** 20130713
 * IPI Message handler 호출
 **/
asmlinkage void __exception_irq_entry do_IPI(int ipinr, struct pt_regs *regs)
{
	handle_IPI(ipinr, regs);
}

void handle_IPI(int ipinr, struct pt_regs *regs)
{
	unsigned int cpu = smp_processor_id();
	struct pt_regs *old_regs = set_irq_regs(regs);

	if (ipinr >= IPI_TIMER && ipinr < IPI_TIMER + NR_IPI)
		__inc_irq_stat(cpu, ipi_irqs[ipinr - IPI_TIMER]);

	switch (ipinr) {
	case IPI_TIMER:
		irq_enter();
		ipi_timer();
		irq_exit();
		break;

	case IPI_RESCHEDULE:
		scheduler_ipi();
		break;

	/** 20140621
	 * 여러 cpu들이 동시에 수행해야 하는 함수를 전달하는 IPI인 경우.
	 **/
	case IPI_CALL_FUNC:
		irq_enter();
		generic_smp_call_function_interrupt();
		irq_exit();
		break;

	/** 20140621
	 * 하나의 cpu가 수행해야 하는 함수를 전달하는 IPI인 경우.
	 **/
	case IPI_CALL_FUNC_SINGLE:
		irq_enter();
		generic_smp_call_function_single_interrupt();
		irq_exit();
		break;

	case IPI_CPU_STOP:
		irq_enter();
		ipi_cpu_stop(cpu);
		irq_exit();
		break;

	default:
		printk(KERN_CRIT "CPU%u: Unknown IPI message 0x%x\n",
		       cpu, ipinr);
		break;
	}
	set_irq_regs(old_regs);
}

/** 20130713
 * 특정 cpu에 IPI_RESCHEDULE message를 전달.
 **/
void smp_send_reschedule(int cpu)
{
	/** 20130713
	 * smp 간의 인터럽트 통신 (IPI)을 호출
	 **/
	smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}

#ifdef CONFIG_HOTPLUG_CPU
static void smp_kill_cpus(cpumask_t *mask)
{
	unsigned int cpu;
	for_each_cpu(cpu, mask)
		platform_cpu_kill(cpu);
}
#else
static void smp_kill_cpus(cpumask_t *mask) { }
#endif

void smp_send_stop(void)
{
	unsigned long timeout;
	struct cpumask mask;

	cpumask_copy(&mask, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &mask);
	if (!cpumask_empty(&mask))
		smp_cross_call(&mask, IPI_CPU_STOP);

	/* Wait up to one second for other CPUs to stop */
	timeout = USEC_PER_SEC;
	while (num_online_cpus() > 1 && timeout--)
		udelay(1);

	if (num_online_cpus() > 1)
		pr_warning("SMP: failed to stop secondary CPUs\n");

	smp_kill_cpus(&mask);
}

/*
 * not supported here
 */
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}
