/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org> 
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/tty.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_cgroup.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/kmemcheck.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/file.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#ifdef CONFIG_X86_LOCAL_APIC
#include <asm/smp.h>
#endif

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(unsigned long);
extern void mca_init(void);
extern void sbus_init(void);
extern void prio_tree_init(void);
extern void radix_tree_init(void);
#ifndef CONFIG_DEBUG_RODATA
static inline void mark_rodata_ro(void) { }
#endif

#ifdef CONFIG_TC
extern void tc_init(void);
#endif

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

/** 20130629    
 * system_state : 전역 변수이므로 초기값 0 (SYSTEM_BOOTING)
 **/
enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);
extern void softirq_init(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situaiton where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char * argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char * envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static int __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	int had_early_param = 0;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = 1;
			} else if (!p->setup_func) {
				printk(KERN_WARNING "Parameter %s is obsolete,"
				       " ignored\n", p->str);
				return 1;
			} else if (p->setup_func(line + n))
				return 1;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
/** 20150606    
 * calibrate_delay 함수에서 delay loop 횟수를 저장한다.
 **/
unsigned long loops_per_jiffy = (1<<12);

EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = 10;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = 4;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val, const char *unused)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val, const char *unused)
{
	repair_env_string(param, val, unused);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "Too many boot env vars at `%s'";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "Too many boot init vars at `%s'";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
#ifdef CONFIG_X86_LOCAL_APIC
static void __init smp_init(void)
{
	APIC_init_uniprocessor();
}
#else
#define smp_init()	do { } while (0)
#endif

static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
/** 20130608    
 * static_command_line, saved_command_line에 각각 복사.
 * saved_command_line은 추후 /proc/cmdline에 보여주는 용도 등으로 저장.
 **/
static void __init setup_command_line(char *command_line)
{
	/** 20130608    
	 * boot_command_line은 ATAG에서 넘어온 command line.
	 * command_line은 setup_arch에서 동일한 내용을 copy.
	 **/
	saved_command_line = alloc_bootmem(strlen (boot_command_line)+1);
	static_command_line = alloc_bootmem(strlen (command_line)+1);
	strcpy (saved_command_line, boot_command_line);
	strcpy (static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

/** 20150523    
 * kthreadd_done completion 선언 및 초기화.
 **/
static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __init_refok rest_init(void)
{
	int pid;

	/** 20150523    
	 * rcu scheduler가 동작하도록 한다.
	 **/
	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_idle();
}

/* Check for early params. */
/** 20130105
 * early param 처리.
 **/
static int __init do_early_param(char *param, char *val, const char *unused)
{
	const struct obs_kernel_param *p;

	/** 20130105
	 * __setup_start, __setup_end 는 early_param macro를 통해 .init.setup section에 들어간다.
	 * vexpress에서 earlycon 정의되어 있는 부분은 없음. early 단계에서 console 옵션이 활성화되지 않음.
	 *
	 * early_param(...)으로 선언하면 obs_kernel_param으로 .init.setup 섹션에
	 *
 	 * param와 .init.setup section에 있는 early bit == 1 인 경우, 또는.. 
	 * 	param에 "console"이 지정되어 있고, .init.setup section에 "earlycon"이 포함된 경우,
	 * 	==> 해당 section에서 setup_func 핸들러를 수행한다. 
	 *
	 * .init.setup section을 확인하기 위해서는 objdump -t vmlinux.o 
	 **/
	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				printk(KERN_WARNING
				       "Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

/** 20130105
 * cmdline을 조사하여 early field가 정의되어 있는 파라미터만 처리한다.
 **/
void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
/** 20130105
 * setup_arch에서 호출
 * boot_command_line 을 parsing 하여, early 관련 함수 실행.
 *
 * 20130727
 * start_kernel에서 직접 호출
 *   done이 설정되어 있다면 바로 return
 * */
void __init parse_early_param(void)
{
	static __initdata int done = 0;
	static __initdata char tmp_cmdline[COMMAND_LINE_SIZE];

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

/*
 *	Activate the first processor.
 */

/** 20140426    
 * 부팅시 사용된 cpu를 cpu mask에 추가한다.
 **/
static void __init boot_cpu_init(void)
{
	/** 20121208
	 * 현재 Processor의 번호를 얻어온다
	 **/
	int cpu = smp_processor_id();
	/* Mark the boot cpu "present", "online" etc for SMP and UP case */
	
    /** 20121208
	 * cpu_online, cpu_active, cpu_present, cpu_possible 비트맵에 해당 비트를 1로 세팅한다.
	 * 20130810
	 * http://studyfoss.egloos.com/5444259 참조
	 **/
    set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);
}

/** 20121103
 * smp_setup_processor_id()는 arch/arm/kernel/setup.c에 있는 함수가 호출됨. 
 **/
void __init __weak smp_setup_processor_id(void)
{
}

/** 20150207    
 * NULL 함수.
 * thread_info의 크기가 PAGE_SIZE 이상이 아니다.
 **/
void __init __weak thread_info_cache_init(void)
{
}

/*
 * Set up kernel memory allocators
 */
/** 20140322    
 * mm_init
 *	커널이 사용하는 메모리 관련 자료구조를 초기화 한다.
 *
 * o mem_init
 *		bootmem에서 buddy allocator 전환
 * o kmem_cache_init
 *		slab(slub) allocator 사용을 위한 초기화
 * o vmalloc_init
 *		vmlist에 등록되어 있던 정보를 vmap_area으로 등록
 *
 **/
static void __init mm_init(void)
{
	/*
	 * page_cgroup requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	/** 20130803    
	 * CONFIG_MEMCG 가 define되지 않아 바로 return
	 **/
	page_cgroup_init_flatmem();
	mem_init();
	/** 20130907    
	 * CONFIG_SLUB이 정의되어 있으므로 mm/Makefile에서 slub.o가 생성된다.
	 *
	 * 20140322
	 * slab allocator 동작을 위한 kmem_cache, kmem_cache_node 자료구조를 초기화
	 **/
	kmem_cache_init();
	percpu_init_late();
	pgtable_cache_init();
	vmalloc_init();
#ifdef CONFIG_X86
	if (efi_enabled)
		efi_enter_virtual_mode();
#endif
}

/** 20121103
	#define __init		__section(.init.text) __cold notrace
	__init
		. 초기화 이후, unload 됨. 
		. cold section 으로 들어가게 됨. 
 **/
asmlinkage void __init start_kernel(void)
{
	char * command_line;
	extern const struct kernel_param __start___param[], __stop___param[];

	/*
	 * Need to run as early as possible, to initialize the
	 * lockdep hash:
	 */
	/** 20121103
	 * lockdep_init() 
	 * 	lock dependency check를 위한 debug에서 사용하는 기능. 
	 * 	기본 옵션에서는 빠져있음. 
	 * 	참고: http://studyfoss.egloos.com/5342153
	 **/
	lockdep_init();
	/** 20121103
	 * processor id를 설정한다.
	 * cpu_logical_map을 설정한다. booting된 cpu 번호를 cpu_logical_map의 첫 항목으로 넣는다. 
	 */
	smp_setup_processor_id();
	/** 20121103
	 * CONFIG_DEBUG_OBJECTS 이 정의된 경우, early boot 단계에서 디버깅을 위한 자료구조 초기화인 듯. 
	 */
	debug_objects_early_init();

	/*
	 * Set up the the initial canary ASAP:
	 */
	/** 20121103
	 * http://www.iamroot.org/xe/66002#comment_66008
	 * CONFIG_CC_STACKPROTECTOR 이 정의된 경우, stack overflow를 감지할 수 있음. 
	 */
	boot_init_stack_canary();

	/** 20121103
	 */
	/** 20121124
	 * init 과정에서 먼저 초기화가 이루어져야 하는 자료구조를 초기화 한다.
	 *   (init task에 대한 cgroup 자료구조)
	 **/
	cgroup_init_early();

	/** 20121124
	 * arch_local_irq_disable
	 *   #if __LINUX_ARM_ARCH__ >= 6 일 때
	 *		CPSID i instruction으로 호출
	 *   #else
	 *		cpsr 읽어 orr로 변경
	 *
	 *  왜 irq disable을 cgroup 등의 다음에 하는 걸까???
	 **/
	local_irq_disable();
	/** 20121124
	 * 전역 함수에 disable 처리를 기록한다. __read_mostly로 선언됨
	 **/
	early_boot_irqs_disabled = true;

/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
    /** 20121208
	 * TICK 초기화 함수
     **/
	tick_init();
    /** 20121208
	 * boot시 사용된 cpu 비트맵 변수들을 초기화
     **/
	boot_cpu_init();
    /** 20121208
	 * vexpress에서는 NULL함수임
     **/
	/** 20131026    
	 * CONFIG_HIGHMEM인 경우 호출됨
	 **/
	page_address_init();

	printk(KERN_NOTICE "%s", linux_banner);
	setup_arch(&command_line);
	/** 20130608    
	 * vexpress에서 NULL 함수
	 **/
	mm_init_owner(&init_mm, &init_task);
	/** 20130608    
	 * vexpress에서 NULL 함수
	 **/
	mm_init_cpumask(&init_mm);
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	/** 20130629    
	 * percpu를 사용하기 위한 자료구조 초기화
	 **/
	setup_per_cpu_areas();
	/** 20130629    
	 * vexpress 에서 NULL 함수
	 **/
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */

	/** 20130727    
	 * zonelists 자료구조 생성
	 **/
	build_all_zonelists(NULL, NULL);
	page_alloc_init();

	/** 20130727    
	 * ATAG에서 넘어온 command line 문자열을 출력한다.
	 **/
	printk(KERN_NOTICE "Kernel command line: %s\n", boot_command_line);
	parse_early_param();
	/** 20130727    
	 * setup_command_line에서 복사해둔 static_command_line 파라미터에서 
	 *
	 * 파싱되지 못한 구식 argument들은 unknown_bootoption으로 처리.
	 **/
	parse_args("Booting kernel", static_command_line, __start___param,
		   __stop___param - __start___param,
		   -1, -1, &unknown_bootoption);

	/** 20130727    
	 * vexpress에서는 NULL 함수
	 **/
	jump_label_init();

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	/** 20130727    
	 * early_param("log_buf_len", log_buf_len_setup)이 호출되지 않으면
	 * new_log_buf_len이 0이 되어 바로 리턴됨.
	 **/
	setup_log_buf(0);
	pidhash_init();
	/** 20130803    
	 * vfs에서 cache로 사용할 hash table 초기화
	 **/
	vfs_caches_init_early();
	sort_main_extable();
	/** 20130803    
	 * NULL 함수
	 **/
	trap_init();
	/** 20140419    
	 * memory management 초기화 (kernel space)
	 **/
	mm_init();

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	/** 20140510    
	 * scheduling 관련 자료구조 초기화.
	 *   - runqueue
	 *   - sched class 지정
	 **/
	sched_init();
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	/** 20140510    
	 * 명시적으로 선점 불가 상태로 만든다.
	 **/
	preempt_disable();
	/** 20140510    
	 * cpsr에서 irq flags를 검사해 irq가 발생 가능하다면
	 * 경고 메시지를 출력하고 irq disable 상태로 만든다.
	 **/
	if (!irqs_disabled()) {
		printk(KERN_WARNING "start_kernel(): bug: interrupts were "
				"enabled *very* early, fixing it\n");
		local_irq_disable();
	}
	/** 20140705
	 * idr_layer용 kmem_cache 생성
	 */
	idr_init_cache();
	/** 20140719
	 * perf_event 관련 자료 구조 초기화
	 **/
	perf_event_init();
	/** 20140906    
	 * rcu_state 생성 등 rcu 초기화
	 **/
	rcu_init();
	/** 20140906    
	 * radix tree 자료구조 관련 초기화 - page cache용.
	 **/
	radix_tree_init();
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	/** 20140920    
	 * machine의 irq 초기화 함수를 호출한다.
	 **/
	init_IRQ();
	/** 20140920    
	 * prio tree를 초기화 한다.
	 **/
	prio_tree_init();
	/** 20140920    
	 * timer를 사용하기 위한 초기화를 수행한다.
	 **/
	init_timers();
	hrtimers_init();
	/** 20141213
	 * softirq 자료구조를 초기화 한다.
	 **/
	softirq_init();
	/** 20141213
	 * timekeeper관련 변수들을 초기화한다.
	 **/
	timekeeping_init();
	/** 20150103    
	 * machine speicific한 timer를 초기화하고, sched_clock timer를 설정한다.
	 **/
	time_init();
	/** 20150103    
	 * profile을 동작시키기 위한 초기화.
	 **/
	profile_init();
	call_function_init();
	/** 20150117    
	 * 인터럽트는 disabled 상태여야 한다.
	 **/
	if (!irqs_disabled())
		printk(KERN_CRIT "start_kernel(): bug: interrupts were "
				 "enabled early\n");
	early_boot_irqs_disabled = false;
	/** 20150124    
	 * boot cpu의 interrupt를 활성화 한다.
	 **/
	local_irq_enable();

	/** 20150124    
	 * slub은 특별한 동작을 취하지 않는다.
	 **/
	kmem_cache_init_late();

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	/** 20150124    
	 * console을 초기화 한다.
	 *
	 * console이 초기화 된 상태이므로 panic이 발생할 상황이면 발생시킨다.
	 **/
	console_init();
	if (panic_later)
		panic(panic_later, panic_param);

	/** 20150124    
	 * LOCKDEP 설정 정보를 출력한다.
	 * 분석 생략.
	 **/
	lockdep_info();

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	/** 20150124    
	 * locking API를 boot-time에 테스트 한다.
	 * 분석 생략.
	 **/
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	/** 20150124    
	 * INITRD가 잘못 설정되어 있는 경우 (min_low_pfn보다 낮은 경우)
	 * initrd를 disable 한다.
	 *
	 * INITRAMFS가 주어지면, INITRAMFS인지 INITRD인지 검사해 처리된다.
	 **/
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		printk(KERN_CRIT "initrd overwritten (0x%08lx < 0x%08lx) - "
		    "disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	/** 20150124    
	 * CONFIG_SPARSEMEM을 설정하지 않아 NULL 함수.
	 **/
	page_cgroup_init();
	/** 20150124    
	 * CONFIG_DEBUG_OBJECTS를 설정하지 않아 NULL 함수.
	 **/
	debug_objects_mem_init();
	/** 20150124    
	 * CONFIG_DEBUG_KMEMLEAK를 설정하지 않아 NULL 함수.
	 **/
	kmemleak_init();
	setup_per_cpu_pageset();
	/** 20150124    
	 * CONFIG_NUMA가 아니므로 NULL 함수.
	 **/
	numa_policy_init();
	/** 20150124    
	 * machine의 timer init 함수에서 late_time_init을 등록했다면 실행한다.
	 * vexpress는 지정하지 않음.
	 **/
	if (late_time_init)
		late_time_init();
	/** 20150124    
	 * sched_clock이 동작 중임을 표시한다.
	 **/
	sched_clock_init();
	/** 20150131    
	 * BogoMIPS를 계산한다.
	 **/
	calibrate_delay();
	/** 20150207    
	 * pidmap 관련 변수를 구하고, init_pid_ns를 위한 초기화를 한다.
	 **/
	pidmap_init();
	/** 20150207    
	 * anon_vma를 위한 초기화로 kmem_cache를 생성한다.
	 **/
	anon_vma_init();
	thread_info_cache_init();
	/** 20150207    
	 * credentials 초기화 : kmem_cache 생성
	 **/
	cred_init();
	/** 20150207    
	 * fork 초기화 : kmem_cache 생성, max_threads 설정
	 **/
	fork_init(totalram_pages);
	/** 20150207    
	 * process 초기화 : kmem_cache 생성, VMA 관련 초기화.
	 **/
	proc_caches_init();
	/** 20150214    
	 * buffer head 초기화 : kmem_cache 생성, max_buffer_heads 설정 등
	 **/
	buffer_init();
	/** 20150214    
	 * CONFIG_KEYS가 설정되지 않아 NULL 함수.
	 **/
	key_init();
	/** 20150214    
	 * CONFIG_SECURITY가 설정되지 않아 NULL 함수.
	 **/
	security_init();
	/** 20150214    
	 * CONFIG_KGDB가 설정되지 않아 NULL 함수.
	 **/
	dbg_late_init();
	/** 20150502    
	 * VFS에서 사용되는 kmem_cache 초기화, rootfs 초기화 등을 수행하는 함수.
	 **/
	vfs_caches_init(totalram_pages);
	/** 20150502    
	 * 시그널 초기화 함수.
	 **/
	signals_init();
	/* rootfs populating might need page-writeback */
	page_writeback_init();
#ifdef CONFIG_PROC_FS
	/** 20150516    
	 * proc 파일시스템을 초기화 한다.
	 **/
	proc_root_init();
#endif
	/** 20150523    
	 * cgroup_init, cpuset_init 추후 분석 ???
	 **/
	cgroup_init();
	cpuset_init();
	taskstats_init_early();
	delayacct_init();

	/** 20150523    
	 * MMU를 사용하는 경우 writebuffer bug를 체크한다.
	 **/
	check_bugs();

	/** 20150523
	 * 아래 함수들은 config에 따라 분석하지 않음.
	 **/
	acpi_early_init(); /* before LAPIC and SMP init */
	sfi_init_late();

	ftrace_init();

	/* Do the rest non-__init'ed, we're now alive */
	/** 20150523    
	 **/
	rest_init();
}

/* Call all constructor functions linked into the kernel. */
/** 20150912    
 * CONFIG_CONSTRUCTORS 를 설정하지 않아 NULL 함수.
 **/
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

/** 20150613    
 **/
bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

static char msgbuf[64];

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	ktime_t calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	printk(KERN_DEBUG "calling  %pF @ %i\n", fn, task_pid_nr(current));
	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	printk(KERN_DEBUG "initcall %pF returned %d after %lld usecs\n", fn,
		ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;

	/** 20150613    
	 * fn()을 수행하고 결과를 받아온다.
	 **/
	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();

	msgbuf[0] = 0;

	if (ret && ret != -ENODEV && initcall_debug)
		sprintf(msgbuf, "error code %d ", ret);

	if (preempt_count() != count) {
		strlcat(msgbuf, "preemption imbalance ", sizeof(msgbuf));
		preempt_count() = count;
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	if (msgbuf[0]) {
		printk("initcall %pF returned with %s\n", fn, msgbuf);
	}

	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

/** 20150912    
 * initcall level
 * 0 - ealry
 * 1 - core
 * 2 - postcore
 * 3 - arch
 * 4 - subsys
 * 5 - fs
 * 6 - device
 * 7 - late
 **/
static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	/** 20151226    
	 * include/asm-generic/vmlinux.lds.h에 선언.
	 **/
	extern const struct kernel_param __start___param[], __stop___param[];
	initcall_t *fn;

	strcpy(static_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   static_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   &repair_env_string);

	/** 20151226    
	 * level이 5라면, __initcall5_start <= fn < __initcall6_start 이므로
	 * initcall 5s와 rootfs까지도 수행된다.
	 **/
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(*fn);
}

static void __init do_initcalls(void)
{
	int level;

	/** 20150912    
	 * initcall 0부터 initcall 7까지 초기화 함수를 수행한다.
	 **/
	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	/** 20150829    
	 * cpuset_init_smp 추후분석???
	 **/
	cpuset_init_smp();
	/** 20150822    
	 * "khelper" workqueue를 생성한다.
	 **/
	usermodehelper_init();
	/** 20150822    
	 * shmem 초기화를 수행한다.
	 **/
	shmem_init();
	/** 20150912    
	 * device model을 초기화 한다.
	 **/
	driver_init();
	/** 20150912    
	 * "/proc/irq/" 아래 irq 번호에 해당하는 파일을 생성한다.
	 **/
	init_irq_proc();
	do_ctors();
	/** 20150912    
	 * usermodehelper 상태를 enable로 변경한다.
	 **/
	usermodehelper_enable();
	do_initcalls();
}

/** 20150613    
 * __initcall_start ~ __initcall0_start 사이에 배치된 함수들을 호출한다.
 * 배치된 함수는 System.map에서 __initcall_start로 검색해 찾을 수 있다.
 *
 * 이곳에 함수를 배치시키려면 early_initcall(...)을 사용한다.
 **/
static void __init do_pre_smp_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(*fn);
}

static void run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	kernel_execve(init_filename, argv_init, envp_init);
}

/* This is a non __init function. Force it to be noinline otherwise gcc
 * makes it inline to init() and it becomes part of init.text section
 */
static noinline int init_post(void)
{
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();
	free_initmem();
	mark_rodata_ro();
	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	current->signal->flags |= SIGNAL_UNKILLABLE;
	flush_delayed_fput();

	if (ramdisk_execute_command) {
		run_init_process(ramdisk_execute_command);
		printk(KERN_WARNING "Failed to execute %s\n",
				ramdisk_execute_command);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		run_init_process(execute_command);
		printk(KERN_WARNING "Failed to execute %s.  Attempting "
					"defaults...\n", execute_command);
	}
	run_init_process("/sbin/init");
	run_init_process("/etc/init");
	run_init_process("/bin/init");
	run_init_process("/bin/sh");

	panic("No init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}

static int __init kernel_init(void * unused)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	/** 20150523    
	 * kthreadd가 설정이 완료된 뒤에 동작하도록 대기시킨다.
	 **/
	wait_for_completion(&kthreadd_done);

	/* Now the scheduler is fully set up and can do blocking allocations */
	/** 20150523    
	 * 커널이 동작할 수 있도록 준비를 마쳤기 때문에
	 * GFP_BOOT_MASK로 갖고 있던 default 값을 바꿔준다.
	 **/
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	/** 20150523    
	 * 현재 task의 mems_allowed를 변경한다.
	 **/
	set_mems_allowed(node_states[N_HIGH_MEMORY]);
	/*
	 * init can run on any cpu.
	 */
	/** 20150530    
	 * 현재 task(init)은 모든 cpu에서 실행될 수 있다.
	 **/
	set_cpus_allowed_ptr(current, cpu_all_mask);

	/** 20150530    
	 * cad_pid에 현재 태스크의 pid를 저장한다.
	 **/
	cad_pid = task_pid(current);

	/** 20150530    
	 * smp_init 수행 전에 준비 작업을 한다.
	 **/
	smp_prepare_cpus(setup_max_cpus);

	/** 20150808    
	 * __initcall_start ~ __initcall0_start 사이에 배치된 함수를 호출한다.
	 **/
	do_pre_smp_initcalls();
	lockup_detector_init();

	/** 20150822    
	 * boot core로 나머지들을 깨운다.
	 **/
	smp_init();
	/** 20150822    
	 * SMP 환경에서 sched 관련 초기화를 호출한다
	 **/
	sched_init_smp();

	do_basic_setup();

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		printk(KERN_WARNING "Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 */

	init_post();
	return 0;
}
