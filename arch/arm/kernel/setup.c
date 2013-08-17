/*
 *  linux/arch/arm/kernel/setup.c
 *
 *  Copyright (C) 1995-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/bootmem.h>
#include <linux/seq_file.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/of_fdt.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/sort.h>

#include <asm/unified.h>
#include <asm/cp15.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/elf.h>
#include <asm/procinfo.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/mach-types.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/tlbflush.h>

#include <asm/prom.h>
#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/system_info.h>
#include <asm/system_misc.h>
#include <asm/traps.h>
#include <asm/unwind.h>
#include <asm/memblock.h>

#if defined(CONFIG_DEPRECATED_PARAM_STRUCT)
#include "compat.h"
#endif
#include "atags.h"
#include "tcm.h"

#ifndef MEM_SIZE
#define MEM_SIZE	(16*1024*1024)
#endif

#if defined(CONFIG_FPE_NWFPE) || defined(CONFIG_FPE_FASTFPE)
char fpe_type[8];

static int __init fpe_setup(char *line)
{
	memcpy(fpe_type, line, 8);
	return 1;
}

__setup("fpe=", fpe_setup);
#endif

extern void paging_init(struct machine_desc *desc);
extern void sanity_check_meminfo(void);
extern void reboot_setup(char *str);
extern void setup_dma_zone(struct machine_desc *desc);

unsigned int processor_id;
EXPORT_SYMBOL(processor_id);
unsigned int __machine_arch_type __read_mostly;
EXPORT_SYMBOL(__machine_arch_type);
unsigned int cacheid __read_mostly;
EXPORT_SYMBOL(cacheid);

/** 20130518    
 * head-common.S
 **/
unsigned int __atags_pointer __initdata;

unsigned int system_rev;
EXPORT_SYMBOL(system_rev);

unsigned int system_serial_low;
EXPORT_SYMBOL(system_serial_low);

unsigned int system_serial_high;
EXPORT_SYMBOL(system_serial_high);

unsigned int elf_hwcap __read_mostly;
EXPORT_SYMBOL(elf_hwcap);


#ifdef MULTI_CPU
struct processor processor __read_mostly;
#endif
#ifdef MULTI_TLB
struct cpu_tlb_fns cpu_tlb __read_mostly;
#endif
#ifdef MULTI_USER
struct cpu_user_fns cpu_user __read_mostly;
#endif
#ifdef MULTI_CACHE
struct cpu_cache_fns cpu_cache __read_mostly;
#endif
#ifdef CONFIG_OUTER_CACHE
struct outer_cache_fns outer_cache __read_mostly;
EXPORT_SYMBOL(outer_cache);
#endif

/*
 * Cached cpu_architecture() result for use by assembler code.
 * C code should use the cpu_architecture() function instead of accessing this
 * variable directly.
 */
int __cpu_architecture __read_mostly = CPU_ARCH_UNKNOWN;

struct stack {
	/** 20121215
	 * 왜 3개씩 가지고 있는 것일까???
	 **/
	 /** 20130810
	 irq, abt, und 세가지 모드가 각각 12byte의 스택 크기를
	 가지고 있는듯 한데 이정도로 충분할까???
	 **/
	u32 irq[3];
	u32 abt[3];
	u32 und[3];
	/** 20121215
	 * cacheline 단위로 align.
	 * __aligned__(1 << CONFIG_ARM_L1_CACHE_SHIFT(6))
	 **/
} ____cacheline_aligned;

static struct stack stacks[NR_CPUS];

char elf_platform[ELF_PLATFORM_SIZE];
EXPORT_SYMBOL(elf_platform);

static const char *cpu_name;
static const char *machine_name;
static char __initdata cmd_line[COMMAND_LINE_SIZE];
struct machine_desc *machine_desc __initdata;

static char default_command_line[COMMAND_LINE_SIZE] __initdata = CONFIG_CMDLINE;
static union { char c[4]; unsigned long l; } endian_test __initdata = { { 'l', '?', '?', 'b' } };
/** 20121208
 * endian 확인 코드. 
 * 	big endian 이면, 'b' return
 * 	litten endian 이면, 'l' return
 * */
#define ENDIANNESS ((char)endian_test.l)

DEFINE_PER_CPU(struct cpuinfo_arm, cpu_data);

/*
 * Standard memory resources
 */
/** 20130518    
 **/
static struct resource mem_res[] = {
	{
		.name = "Video RAM",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	}
};

#define video_ram   mem_res[0]
#define kernel_code mem_res[1]
#define kernel_data mem_res[2]

static struct resource io_res[] = {
	{
		.name = "reserved",
		.start = 0x3bc,
		.end = 0x3be,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x378,
		.end = 0x37f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x278,
		.end = 0x27f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	}
};

#define lp0 io_res[0]
#define lp1 io_res[1]
#define lp2 io_res[2]

static const char *proc_arch[] = {
	"undefined/unknown",
	"3",
	"4",
	"4T",
	"5",
	"5T",
	"5TE",
	"5TEJ",
	"6TEJ",
	"7",
	"?(11)",
	"?(12)",
	"?(13)",
	"?(14)",
	"?(15)",
	"?(16)",
	"?(17)",
};

/** 20121208
 * CPU ARCH 를 리턴.
 * */
static int __get_cpu_architecture(void)
{
	int cpu_arch;

	if ((read_cpuid_id() & 0x0008f000) == 0) {
		cpu_arch = CPU_ARCH_UNKNOWN;
	} else if ((read_cpuid_id() & 0x0008f000) == 0x00007000) {
		cpu_arch = (read_cpuid_id() & (1 << 23)) ? CPU_ARCH_ARMv4T : CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x00080000) == 0x00000000) {
		cpu_arch = (read_cpuid_id() >> 16) & 7;
		if (cpu_arch)
			cpu_arch += CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x000f0000) == 0x000f0000) {
		unsigned int mmfr0;

		/* Revised CPUID format. Read the Memory Model Feature
		 * Register 0 and check for VMSAv7 or PMSAv7 */
		/** 20121208
		 * B4.1.89 ID_MMFR0, Memory Model Feature Register 0, VMSA
		 * */
		asm("mrc	p15, 0, %0, c0, c1, 4"
		    : "=r" (mmfr0));
		if ((mmfr0 & 0x0000000f) >= 0x00000003 ||
		    (mmfr0 & 0x000000f0) >= 0x00000030)
			cpu_arch = CPU_ARCH_ARMv7;
		else if ((mmfr0 & 0x0000000f) == 0x00000002 ||
			 (mmfr0 & 0x000000f0) == 0x00000020)
			cpu_arch = CPU_ARCH_ARMv6;
		else
			cpu_arch = CPU_ARCH_UNKNOWN;
	} else
		cpu_arch = CPU_ARCH_UNKNOWN;

	return cpu_arch;
}

int __pure cpu_architecture(void)
{
	BUG_ON(__cpu_architecture == CPU_ARCH_UNKNOWN);

	return __cpu_architecture;
}

static int cpu_has_aliasing_icache(unsigned int arch)
{
	int aliasing_icache;
	unsigned int id_reg, num_sets, line_size;

	/* PIPT caches never alias. */
	if (icache_is_pipt())
		return 0;

	/* arch specifies the register format */
	switch (arch) {
	case CPU_ARCH_ARMv7:
		/** 20121215
		 * Cache Size Selection Register
		 *   0: Data or unified cache
		 *   1: Instruction cache
		 **/
		asm("mcr	p15, 2, %0, c0, c0, 0 @ set CSSELR"
		    : /* No output operands */
		    : "r" (1));
		isb();
		/** 20121215
		 * Cache Size ID Registers
		 **/
		asm("mrc	p15, 1, %0, c0, c0, 0 @ read CCSIDR"
		    : "=r" (id_reg));
		line_size = 4 << ((id_reg & 0x7) + 2);
		num_sets = ((id_reg >> 13) & 0x7fff) + 1;
		/** 20121215
		 * TRM에 따르면
		 * line_size : 4 << (1 + 2)
		 * num_sets  : 32KB라 가정하면, (0xFF) + 1
		 *	0x7F	16KB cache size
		 *	0xFF	32KB cache size
		 *	0x1FF	64KB cache size
		 *
		 *	PAGE_SIZE: 4KB
		 *
		 *	((4 << 3) * (0x100)) > 4 KB       : TRUE
		 **/
		aliasing_icache = (line_size * num_sets) > PAGE_SIZE;
		break;
	case CPU_ARCH_ARMv6:
		aliasing_icache = read_cpuid_cachetype() & (1 << 11);
		break;
	default:
		/* I-cache aliases will be handled by D-cache aliasing code */
		aliasing_icache = 0;
	}

	return aliasing_icache;
}

/** 20121215
 * cachetype과 architecture를 읽어와 전역변수 cacheid를 설정하고, printk로 출력
 **/
static void __init cacheid_init(void)
{
	/** 20121208
	 * B4.1.42 CTR, Cache Type Register, VMSA (from ARM)
	 *  cachetype: 0x83338003                 (from TRM)
	 *
	 * */
	unsigned int cachetype = read_cpuid_cachetype();
	unsigned int arch = cpu_architecture();

	if (arch >= CPU_ARCH_ARMv6) {
		if ((cachetype & (7 << 29)) == 4 << 29) {
			/* ARMv7 register format */
			arch = CPU_ARCH_ARMv7;
			/** 20121215
			 *  NONALIASING ??? 하드웨어적인 설계인가?
			 **/
			cacheid = CACHEID_VIPT_NONALIASING;
			/** 20121208
			 * cachetype에서 (3 << 14) : L1Ip, bits[15:14]
			 * */
			switch (cachetype & (3 << 14)) {
			case (1 << 14):
				cacheid |= CACHEID_ASID_TAGGED;
				break;
			case (3 << 14):
				cacheid |= CACHEID_PIPT;
				break;
			}
		} else {
			arch = CPU_ARCH_ARMv6;
			if (cachetype & (1 << 23))
				cacheid = CACHEID_VIPT_ALIASING;
			else
				cacheid = CACHEID_VIPT_NONALIASING;
		}
		/** 20121215
		 * vexpress의 경우 아래 조건 만족
		 **/
		if (cpu_has_aliasing_icache(arch))
			cacheid |= CACHEID_VIPT_I_ALIASING;
	} else {
		cacheid = CACHEID_VIVT;
	}

	/** 20121215
	 * vexpress qemu에서 수행시 출력결과
	 *	CPU: PIPT / VIPT nonaliasing data cache, VIPT aliasing instruction cache
	 **/
	printk("CPU: %s data cache, %s instruction cache\n",
		cache_is_vivt() ? "VIVT" :
		cache_is_vipt_aliasing() ? "VIPT aliasing" :
		cache_is_vipt_nonaliasing() ? "PIPT / VIPT nonaliasing" : "unknown",
		cache_is_vivt() ? "VIVT" :
		icache_is_vivt_asid_tagged() ? "VIVT ASID tagged" :
		icache_is_vipt_aliasing() ? "VIPT aliasing" :
		icache_is_pipt() ? "PIPT" :
		cache_is_vipt_nonaliasing() ? "VIPT nonaliasing" : "unknown");
}

/*
 * These functions re-use the assembly code in head.S, which
 * already provide the required functionality.
 */
extern struct proc_info_list *lookup_processor_type(unsigned int);

void __init early_print(const char *str, ...)
{
	extern void printascii(const char *);
	char buf[256];
	va_list ap;

	va_start(ap, str);
	vsnprintf(buf, sizeof(buf), str, ap);
	va_end(ap);

#ifdef CONFIG_DEBUG_LL
	printascii(buf);
#endif
	printk("%s", buf);
}

static void __init feat_v6_fixup(void)
{
	int id = read_cpuid_id();

	/** 20121208
	 * MIDR에서 0x41070000 의 의미는 ARM v6 임.
	 * ARMv6 가 아닌 경우에 바로 리턴.
	 * */
	if ((id & 0xff0f0000) != 0x41070000)
		return;

	/*
	 * HWCAP_TLS is available only on 1136 r1p0 and later,
	 * see also kuser_get_tls_init.
	 */
	if ((((id >> 4) & 0xfff) == 0xb36) && (((id >> 20) & 3) == 0))
		elf_hwcap &= ~HWCAP_TLS;
}

/*
 * cpu_init - initialise one CPU.
 *
 * cpu_init sets up the per-CPU stacks.
 */
/** 20121215
 * CPU 0번에 대해 irq, abt, und 모드의 stack pointer를 설정한다.
 **/
void cpu_init(void)
{
	/** 20121215
	 * current_thread_info()->cpu는 0.
	 * 부트 프로세스에서 실행되는 cpu를 의미함
	 **/
	unsigned int cpu = smp_processor_id();
	/** 20121215
	 * cpu 0의 stack 주소를 가져옴
	 **/
	struct stack *stk = &stacks[cpu];

	if (cpu >= NR_CPUS) {
		printk(KERN_CRIT "CPU%u: bad primary CPU number\n", cpu);
		BUG();
	}

	/** 20121215
	 * cpu_v7_proc_init
	 **/
	cpu_proc_init();

	/*
	 * Define the placement constraint for the inline asm directive below.
	 * In Thumb-2, msr with an immediate value is not allowed.
	 */
#ifdef CONFIG_THUMB2_KERNEL
#define PLC	"r"
#else
#define PLC	"I"
#endif

	/*
	 * setup stacks for re-entrant exception handlers
	 */
	/** 20121215
	 * 0 : stk
	 * 1 : "I" (PSR_F_BIT | PSR_I_BIT | IRQ_MODE),
	 * 2 : "I" (offsetof(struct stack, irq[0])),
	 * 3 : "I" (PSR_F_BIT | PSR_I_BIT | ABT_MODE),
	 * 4 : "I" (offsetof(struct stack, abt[0])),
	 * 5 : "I" (PSR_F_BIT | PSR_I_BIT | UND_MODE),
	 * 6 : "I" (offsetof(struct stack, und[0])),
	 * 7 : "I" (PSR_F_BIT | PSR_I_BIT | SVC_MODE)
	 *
	 *  1. IRQ, ABT, UND 모드로 각각 전환
	 *  2. 각 모드에서 사용하는 stack의 주소를 계산 (stk + offset)
	 *  3. 해당 모드의 sp 레지스터에 저장
	 *  4. 위 세 가지 모드의 설정이 끝난 뒤 SVC로 돌아옴
	 **/
	__asm__ (
	"msr	cpsr_c, %1\n\t"
	"add	r14, %0, %2\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %3\n\t"
	"add	r14, %0, %4\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %5\n\t"
	"add	r14, %0, %6\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %7"
	    :
	    : "r" (stk),
	      PLC (PSR_F_BIT | PSR_I_BIT | IRQ_MODE),
	      "I" (offsetof(struct stack, irq[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | ABT_MODE),
	      "I" (offsetof(struct stack, abt[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | UND_MODE),
	      "I" (offsetof(struct stack, und[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | SVC_MODE)
	    : "r14");
}

int __cpu_logical_map[NR_CPUS];

void __init smp_setup_processor_id(void)
{
	int i;
	/** 20121103
	 * TRM. p68. Multiprocessor Affinity Register. CPUID를 읽는다. 
	 *
	 * cpu_logical_map[0,1.. NR_CPUS] 에 논리 cpu 번호를 기록한다.
	 * 만약 MPIDR에서 읽은 cpu가 2라면, 
	 * 	 cpu_logical_map[0,1,2,3] = {2,1,0,3} 으로 기록된다.
	 **/
	u32 cpu = is_smp() ? read_cpuid_mpidr() & 0xff : 0;

	cpu_logical_map(0) = cpu;
	for (i = 1; i < NR_CPUS; ++i)
		cpu_logical_map(i) = i == cpu ? 0 : i;

	printk(KERN_INFO "Booting Linux on physical CPU %d\n", cpu);
}

/** 20121215
 * cpuid를 읽어 architecture 관련 전역변수를 초기화
 *   - cacheid 및 cpu stack 등 프로세서 관련 변수 초기화
 **/
static void __init setup_processor(void)
{
	struct proc_info_list *list;

	/*
	 * locate processor in the list of supported processor
	 * types.  The linker builds this table for us from the
	 * entries in arch/arm/mm/proc-*.S
	 */
	/** 20130608    
	 * arch/arm/mm/proc-v7.S
	 * __v7_proc_info:
	 **/
	list = lookup_processor_type(read_cpuid_id());
	if (!list) {
		printk("CPU configuration botched (ID %08x), unable "
		       "to continue.\n", read_cpuid_id());
		while (1);
	}

	cpu_name = list->cpu_name;
	/** 20121208
	 * __cpu_architecture = CPU_ARCH_ARMv7 (9)
	 * */
	__cpu_architecture = __get_cpu_architecture();

#ifdef MULTI_CPU
	/** 20121208
	 * 수행안됨.
	 * */
	processor = *list->proc;
#endif
#ifdef MULTI_TLB
	/** 20121208
	 * 수행됨.
	 * */
	cpu_tlb = *list->tlb;
#endif
#ifdef MULTI_USER
	/** 20121208
	 * 수행됨.
	 * */
	cpu_user = *list->user;
#endif
#ifdef MULTI_CACHE
	/** 20121208
	 * 수행됨.
	 * */
	cpu_cache = *list->cache;
#endif

	/** 20121208
	 * cr_alignment는 head-common.S 에 __mmap_switched에서 설정함. 
	 * */
	printk("CPU: %s [%08x] revision %d (ARMv%s), cr=%08lx\n",
	       cpu_name, read_cpuid_id(), read_cpuid_id() & 15,
	       proc_arch[cpu_architecture()], cr_alignment);

	/** 20121208
	 * init_utsname()->machine에 이미 설정되어 있는 값을 
	 * 		lookup_processor_type에서 읽어온 값으로 갱신.
	 * */
	snprintf(init_utsname()->machine, __NEW_UTS_LEN + 1, "%s%c",
		 list->arch_name, ENDIANNESS);
	snprintf(elf_platform, ELF_PLATFORM_SIZE, "%s%c",
		 list->elf_name, ENDIANNESS);
	elf_hwcap = list->elf_hwcap;
#ifndef CONFIG_ARM_THUMB
	elf_hwcap &= ~HWCAP_THUMB;
#endif

	feat_v6_fixup();

	/** 20121215
	 * cachetype에 따라 전역변수 cacheid를 init 해주는 함수
	 *   CACHEID_VIPT_NONALIASING | CACHEID_VIPT_I_ALIASING
	 **/
	cacheid_init();
	/** 20121215
	 * architecture specific한 초기화 작업이 있다면 호출
	 * cpu에 대한 모드별 stack pointer 초기화
	 **/
	cpu_init();
}

void __init dump_machine_table(void)
{
	struct machine_desc *p;

	early_print("Available machine support:\n\nID (hex)\tNAME\n");
	for_each_machine_desc(p)
		early_print("%08x\t%s\n", p->nr, p->name);

	early_print("\nPlease check your kernel config and/or bootloader.\n");

	while (true)
		/* can't use cpu_relax() here as it may require MMU setup */;
}

/** 20121222
 * physical memory에 대한 start, size 를 align한다. 
 * */
int __init arm_add_memory(phys_addr_t start, phys_addr_t size)
{
	/** 20121222
	 * meminfo.nr_banks : 0
	 * */
	struct membank *bank = &meminfo.bank[meminfo.nr_banks];
	if (meminfo.nr_banks >= NR_BANKS) {
		printk(KERN_CRIT "NR_BANKS too low, "
			"ignoring memory at 0x%08llx\n", (long long)start);
		return -EINVAL;
	}

	/*
	 * Ensure that start/size are aligned to a page boundary.
	 * Size is appropriately rounded down, start is rounded up.
	 */
	/** 20121222
	 * 예를 들어, start : 0x200, size : 0x5000 이라고 가정하자. 
	 * 	size 는 0x5000 - (0x200 & (0xFFF) = 0x4E00
	 * 	start 는 (0x200 + 0xFFF) & (0xFFFFF000) = 0x1000 
	 * */
	size -= start & ~PAGE_MASK;
	bank->start = PAGE_ALIGN(start);

#ifndef CONFIG_LPAE
	/** 20121222
	 * size가 32bit을 넘어가는 경우, size를 unsigned long - start으로 제한한다.
	 * */
	if (bank->start + size < bank->start) {
		printk(KERN_CRIT "Truncating memory at 0x%08llx to fit in "
			"32-bit physical address space\n", (long long)start);
		/*
		 * To ensure bank->start + bank->size is representable in
		 * 32 bits, we use ULONG_MAX as the upper limit rather than 4GB.
		 * This means we lose a page after masking.
		 */
		size = ULONG_MAX - bank->start;
	}
#endif

	/** 20121222
	 * size도 PAGE_SIZE로 round down 해준다. 
	 * 위에서 가정한 예에서..
	 *  bank->size = 0x4E00 & (~(0x1000-1)) = 0x4000
	 * */
	bank->size = size & ~(phys_addr_t)(PAGE_SIZE - 1);

	/*
	 * Check whether this memory region has non-zero size or
	 * invalid node number.
	 */
	if (bank->size == 0)
		return -EINVAL;

	/** 20121222
	 * bank->size 가 0보다 큰 경우, meminfo.nr_banks를 증가시켜준다. 
	 * */
	meminfo.nr_banks++;
	return 0;
}

/*
 * Pick out the memory size.  We look for mem=size@start,
 * where start and size are "size[KkMm]"
 */
/** 20130105
 * parse_early 단계에서 cmdline의 mem을 분석하여 mem bank를 설정한다.  
 **/
static int __init early_mem(char *p)
{
	/** 20130105
	 * cmdline에서 mem= 이 여러번 지정되는 경우, 
	 * 	nr_banks 를 늘려가며 meminfo.bank[nr_banks]의 값을 설정한다. 
	 * */
	static int usermem __initdata = 0;
	phys_addr_t size;
	phys_addr_t start;
	char *endp;

	/*
	 * If the user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	/** 20130810
	ATAG로 meminfo를 세팅을 해줘도 early_mem으로 
	세팅을 하면 기존 ATAG meminfo는 오버라이드 된다.
	**/
	if (usermem == 0) {
		usermem = 1;
		meminfo.nr_banks = 0;
	}

	start = PHYS_OFFSET;
	size  = memparse(p, &endp);
	/** 20130105
	 * "mem=128M@0x10000" 이 들어온 경우, 
	 * 	start로 0x10000 이 됨.
	 * */
	if (*endp == '@')
		start = memparse(endp + 1, NULL);

	arm_add_memory(start, size);

	return 0;
}
early_param("mem", early_mem);

static void __init
setup_ramdisk(int doload, int prompt, int image_start, unsigned int rd_sz)
{
#ifdef CONFIG_BLK_DEV_RAM
	extern int rd_size, rd_image_start, rd_prompt, rd_doload;

	rd_image_start = image_start;
	rd_prompt = prompt;
	rd_doload = doload;

	if (rd_sz)
		rd_size = rd_sz;
#endif
}

/** 20130518    
 * resource 구조체를 이용한 메모리 계층 생성. (root는 iomem_resource)
 **/
static void __init request_standard_resources(struct machine_desc *mdesc)
{
	struct memblock_region *region;
	struct resource *res;

	/** 20130518    
	 * arch/arm/kernel/vmlinux.lds 정의된 가상주소.
	 *
	 * struct resource mem_res[1], [2]의 주소 값을 채움.
	 **/
	kernel_code.start   = virt_to_phys(_text);
	kernel_code.end     = virt_to_phys(_etext - 1);
	kernel_data.start   = virt_to_phys(_sdata);
	kernel_data.end     = virt_to_phys(_end - 1);

	/** 20130518    
	 * memblock의 memory의 각 region을 resource 구조체로 만들어
	 * iomem_resource에 등록.
	 **/
	for_each_memblock(memory, region) {
		res = alloc_bootmem_low(sizeof(*res));
		res->name  = "System RAM";
		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

		/** 20130518    
		 * "System RAM"이라는 이름으로 새로운 resource를 등록.
		 **/
		request_resource(&iomem_resource, res);

		/** 20130518    
		 * kernel_code 영역이 새로운 res 영역에 포함될 경우
		 *   kernel_code를 child로 등록함.
		 *   kernel_data를 child로 등록함.
		 **/
		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
	}

	/** 20130518    
	 * video_start가 지정되어 있다면 iomem_resource에 video_ram으로 등록한다.
	 * vexpress의 경우 정의되어 있지 않음.
	 **/
	if (mdesc->video_start) {
		video_ram.start = mdesc->video_start;
		video_ram.end   = mdesc->video_end;
		request_resource(&iomem_resource, &video_ram);
	}

	/*
	 * Some machines don't have the possibility of ever
	 * possessing lp0, lp1 or lp2
	 */
	/** 20130518    
	 * 예약된 lp가 존재한다면 resource로 등록한다.
	 **/
	if (mdesc->reserve_lp0)
		request_resource(&ioport_resource, &lp0);
	if (mdesc->reserve_lp1)
		request_resource(&ioport_resource, &lp1);
	if (mdesc->reserve_lp2)
		request_resource(&ioport_resource, &lp2);
}

/*
 *  Tag parsing.
 *
 * This is the new way of passing data to the kernel at boot time.  Rather
 * than passing a fixed inflexible structure to the kernel, we pass a list
 * of variable-sized tags to the kernel.  The first tag must be a ATAG_CORE
 * tag for the list to be recognised (to distinguish the tagged list from
 * a param_struct).  The list is terminated with a zero-length tag (this tag
 * is not parsed in any way).
 */
static int __init parse_tag_core(const struct tag *tag)
{
	/** 20121222
	 * hdr 크기는 4바이트 단위를 1로 표현하는 듯.. 이유는 모름 ???
	 * hdr.size > 2 라는 것은 tag header 외의 정보가 있다는 의미임.
	 * */
	if (tag->hdr.size > 2) {
		if ((tag->u.core.flags & 1) == 0)
			root_mountflags &= ~MS_RDONLY;
		/** 20121222
		 * CORE의 rootdev값에 따라 device number를 만든다. 
		 * ROOT_DEV는 root file system이 마운트될 device number인 듯 ???
		 * */
		ROOT_DEV = old_decode_dev(tag->u.core.rootdev);
	}
	return 0;
}

__tagtable(ATAG_CORE, parse_tag_core);

/** 20130810
ATAG_MEM으로 meminfo가 넘어왔을때 파싱하는 부분
**/
static int __init parse_tag_mem32(const struct tag *tag)
{
	return arm_add_memory(tag->u.mem.start, tag->u.mem.size);
}

__tagtable(ATAG_MEM, parse_tag_mem32);

#if defined(CONFIG_VGA_CONSOLE) || defined(CONFIG_DUMMY_CONSOLE)
struct screen_info screen_info = {
 .orig_video_lines	= 30,
 .orig_video_cols	= 80,
 .orig_video_mode	= 0,
 .orig_video_ega_bx	= 0,
 .orig_video_isVGA	= 1,
 .orig_video_points	= 8
};

static int __init parse_tag_videotext(const struct tag *tag)
{
	screen_info.orig_x            = tag->u.videotext.x;
	screen_info.orig_y            = tag->u.videotext.y;
	screen_info.orig_video_page   = tag->u.videotext.video_page;
	screen_info.orig_video_mode   = tag->u.videotext.video_mode;
	screen_info.orig_video_cols   = tag->u.videotext.video_cols;
	screen_info.orig_video_ega_bx = tag->u.videotext.video_ega_bx;
	screen_info.orig_video_lines  = tag->u.videotext.video_lines;
	screen_info.orig_video_isVGA  = tag->u.videotext.video_isvga;
	screen_info.orig_video_points = tag->u.videotext.video_points;
	return 0;
}

__tagtable(ATAG_VIDEOTEXT, parse_tag_videotext);
#endif

static int __init parse_tag_ramdisk(const struct tag *tag)
{
	setup_ramdisk((tag->u.ramdisk.flags & 1) == 0,
		      (tag->u.ramdisk.flags & 2) == 0,
		      tag->u.ramdisk.start, tag->u.ramdisk.size);
	return 0;
}

__tagtable(ATAG_RAMDISK, parse_tag_ramdisk);

static int __init parse_tag_serialnr(const struct tag *tag)
{
	system_serial_low = tag->u.serialnr.low;
	system_serial_high = tag->u.serialnr.high;
	return 0;
}

__tagtable(ATAG_SERIAL, parse_tag_serialnr);

static int __init parse_tag_revision(const struct tag *tag)
{
	system_rev = tag->u.revision.rev;
	return 0;
}

__tagtable(ATAG_REVISION, parse_tag_revision);

static int __init parse_tag_cmdline(const struct tag *tag)
{
#if defined(CONFIG_CMDLINE_EXTEND)
	strlcat(default_command_line, " ", COMMAND_LINE_SIZE);
	strlcat(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#elif defined(CONFIG_CMDLINE_FORCE)
	pr_warning("Ignoring tag cmdline (using the default kernel command line)\n");
#else
	/** 20121222
	 * ATAG로 넘어온 cmdline을 default_command_line으로 복사. 
	 * */
	strlcpy(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#endif
	return 0;
}

__tagtable(ATAG_CMDLINE, parse_tag_cmdline);

/*
 * Scan the tag table for this tag, and call its parse function.
 * The tag table is built by the linker from all the __tagtable
 * declarations.
 */
static int __init parse_tag(const struct tag *tag)
{
	/** 20121215
	 * vmlinux.lds에 __tagtable_begin, __tagtable_end 사이에 .taglist.init이 저장됨.
	 * __tag 속성이 붙은 구조체 변수가 .taglist.init에 저장됨.
	 * 이 함수 여기저기서 __tagtable(tag, fn) 매크로를 호출하고 있다.
	 **/
	extern struct tagtable __tagtable_begin, __tagtable_end;
	struct tagtable *t;

	/** 20121222
	 * 아래와 같이 .taglist.init 섹션에 tag type, parse function pointer 가 채워짐.
		__tagtable(ATAG_CMDLINE, parse_tag_cmdline);
		__tagtable(ATAG_MEM, parse_tag_mem32);
		...
	 * tag table에서 tag->hdr.tag에 해당하는 항목을 찾아서 등록되어 있는 parse 함수를 실행시킨다. 
	 **/
	for (t = &__tagtable_begin; t < &__tagtable_end; t++)
		if (tag->hdr.tag == t->tag) {
			t->parse(tag);
			break;
		}

	/** 20121222
	 * tagtable에 해당하는 tag->hdr.tag가 없는 경우, FALSE 을 리턴. 
	 * FALSE 리턴시, parse_tags에서는 에러 코드 출력. 
	 * */
	return t < &__tagtable_end;
}

/*
 * Parse all tags in the list, checking both the global and architecture
 * specific tag tables.
 */
/** 20121222
 * 각 ATAG 정보를 kernel 의 해당 자료구조에 넣는다. 
 * */
static void __init parse_tags(const struct tag *t)
{
	for (; t->hdr.size; t = tag_next(t))
		if (!parse_tag(t))
			printk(KERN_WARNING
				"Ignoring unrecognised tag 0x%08x\n",
				t->hdr.tag);
}

/*
 * This holds our defaults.
 */
static struct init_tags {
	struct tag_header hdr1;
	struct tag_core   core;
	struct tag_header hdr2;
	struct tag_mem32  mem;
	struct tag_header hdr3;
} init_tags __initdata = {
	{ tag_size(tag_core), ATAG_CORE },
	{ 1, PAGE_SIZE, 0xff },
	{ tag_size(tag_mem32), ATAG_MEM },
	{ MEM_SIZE },
	{ 0, ATAG_NONE }
};

static int __init customize_machine(void)
{
	/* customizes platform devices, or adds new ones */
	if (machine_desc->init_machine)
		machine_desc->init_machine();
	return 0;
}
arch_initcall(customize_machine);

static int __init init_machine_late(void)
{
	if (machine_desc->init_late)
		machine_desc->init_late();
	return 0;
}
late_initcall(init_machine_late);

#ifdef CONFIG_KEXEC
static inline unsigned long long get_total_mem(void)
{
	unsigned long total;

	total = max_low_pfn - min_low_pfn;
	return total << PAGE_SHIFT;
}

/**
 * reserve_crashkernel() - reserves memory are for crash kernel
 *
 * This function reserves memory area given in "crashkernel=" kernel command
 * line parameter. The memory reserved is used by a dump capture kernel when
 * primary kernel is crashing.
 */
static void __init reserve_crashkernel(void)
{
	unsigned long long crash_size, crash_base;
	unsigned long long total_mem;
	int ret;

	total_mem = get_total_mem();
	ret = parse_crashkernel(boot_command_line, total_mem,
				&crash_size, &crash_base);
	if (ret)
		return;

	ret = reserve_bootmem(crash_base, crash_size, BOOTMEM_EXCLUSIVE);
	if (ret < 0) {
		printk(KERN_WARNING "crashkernel reservation failed - "
		       "memory is in use (0x%lx)\n", (unsigned long)crash_base);
		return;
	}

	printk(KERN_INFO "Reserving %ldMB of memory at %ldMB "
	       "for crashkernel (System RAM: %ldMB)\n",
	       (unsigned long)(crash_size >> 20),
	       (unsigned long)(crash_base >> 20),
	       (unsigned long)(total_mem >> 20));

	crashk_res.start = crash_base;
	crashk_res.end = crash_base + crash_size - 1;
	insert_resource(&iomem_resource, &crashk_res);
}
#else
static inline void reserve_crashkernel(void) {}
#endif /* CONFIG_KEXEC */

static void __init squash_mem_tags(struct tag *tag)
{
	for (; tag->hdr.size; tag = tag_next(tag))
		if (tag->hdr.tag == ATAG_MEM)
			tag->hdr.tag = ATAG_NONE;
}

/** 20121222
 * machine type을 찾아서 return 하고,
 * ATAG 정보를 읽는다. 
 * */
static struct machine_desc * __init setup_machine_tags(unsigned int nr)
{
	struct tag *tags = (struct tag *)&init_tags;
	struct machine_desc *mdesc = NULL, *p;
	char *from = default_command_line;

	/** 20121215
	 * PHYS_OFFSET은 head.S에서 지정한 __pv_phys_offset값
	 **/
	init_tags.mem.start = PHYS_OFFSET;

	/*
	 * locate machine in the list of supported machines.
	 */
	/** 20121215
	 * machine_desc는 .arch.info.init에 저장됨.
	 * .arch.info.init의 값은 arch/arm/mach-vexpress/v2m.c 에서 MACHINE_START에 의해 .arch.info.init 섹션에 채워짐
	 *
	 **/
	for_each_machine_desc(p)
		if (nr == p->nr) {
			printk("Machine: %s\n", p->name);
			mdesc = p;
			break;
		}

	if (!mdesc) {
		early_print("\nError: unrecognized/unsupported machine ID"
			" (r1 = 0x%08x).\n\n", nr);
		dump_machine_table(); /* does not return */
	}

	/** 20121215
	 * head-common.S 에서 부트로더에서 넘어온 atags 시작 주소(PA)를 저장.
	 **/
	if (__atags_pointer)
		tags = phys_to_virt(__atags_pointer);
	else if (mdesc->atag_offset)
		tags = (void *)(PAGE_OFFSET + mdesc->atag_offset);

#if defined(CONFIG_DEPRECATED_PARAM_STRUCT)
	/*
	 * If we have the old style parameters, convert them to
	 * a tag list.
	 */
	if (tags->hdr.tag != ATAG_CORE)
		convert_to_tag_list(tags);
#endif

	/** 20121215
	 * 넘어온 tags의 첫번째 tag가 ATAG_CORE가 아니라면,
	 * 즉 정상적인 설정값이 아니라면 init_tags로 대체
	 **/
	if (tags->hdr.tag != ATAG_CORE) {
#if defined(CONFIG_OF)
		/*
		 * If CONFIG_OF is set, then assume this is a reasonably
		 * modern system that should pass boot parameters
		 */
		early_print("Warning: Neither atags nor dtb found\n");
#endif
		tags = (struct tag *)&init_tags;
	}

	/** 20121215
	 * function pointer가 지정되어 있지 않으면 실행 안 함
	 **/
	if (mdesc->fixup)
		mdesc->fixup(tags, &from, &meminfo);

	if (tags->hdr.tag == ATAG_CORE) {
		if (meminfo.nr_banks != 0)
			squash_mem_tags(tags);
		/** 20121215
		 * NULL 함수
		 **/
		save_atags(tags);
		parse_tags(tags);
	}

	/* parse_early_param needs a boot_command_line */
	/** 20121222
	 * from
	 *	- ATAG에서 command line 정보를 받는 경우: ATAG에서 넘어온 cmd line
	 * */
	strlcpy(boot_command_line, from, COMMAND_LINE_SIZE);

	return mdesc;
}

static int __init meminfo_cmp(const void *_a, const void *_b)
{
	const struct membank *a = _a, *b = _b;
	long cmp = bank_pfn_start(a) - bank_pfn_start(b);
	return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
}

/** 20130608 
	1. processor 관련 자료구조 초기화 (함수 포인터 등)
	2. MACHINE에 대한 machine descriptor 를 채워준다.
	3. boot command line 파싱해 early 함수 실행
	4. meminfo와 mdesc를 바탕으로 memblock 정보를 채움
	5. page table 생성 및 부팅시 사용할 메모리 할당자 생성
	6. iomem_resource를 root로 하는 resources 메모리 계층 생성 (kernel code 등)
	7. smp에서 사용하는 cpu를 bitmap에 사용함을 셋업
	8. machine에 따른 init_early 함수 실행
	   (vexpress의 경우 sched clock 초기화)
**/
void __init setup_arch(char **cmdline_p)
{
	struct machine_desc *mdesc;

	/** 20121215
	 * processor 관련 자료구조 초기화
	 **/
	setup_processor();
	/** 20121215
	 * NULL 리턴
	 **/
	mdesc = setup_machine_fdt(__atags_pointer);
	/** 20121215
	 * mdesc가 NULL이므로 실행
	 *
	 * machine_arch_type은 CONFIG_MACH_XXX가 정의되어 있지 않은 경우
	 * decompress_kernel()의 argument로 넘어온 값을 사용
	 **/
	if (!mdesc)
		mdesc = setup_machine_tags(machine_arch_type);
	/** 20121222
	 * machine_name : "ARM-Versatile Express"
	 * */
	machine_desc = mdesc;
	machine_name = mdesc->name;

	/** 20121222
	 * vexpress에서는 empty function.
	 * */
	setup_dma_zone(mdesc);

	/** 20121222
	 * vexpress에서는 restart_mode는 정의되지 않음.
	 * */
	if (mdesc->restart_mode)
		reboot_setup(&mdesc->restart_mode);

	init_mm.start_code = (unsigned long) _text;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	/** 20121222
	 * brk 는 break 를 의미하는 듯. 
	 * man brk에서.. 
	 * 		brk() and sbrk() change  the  location  of  the  program  break,  which
	 *      defines  the end of the process's data segment (i.e., the program break
	 *      is the first location after the end of the uninitialized data segment).
	 *      Increasing the program break has the effect of allocating memory to the
	 *      process; decreasing the break deallocates memory.
	 * */
	init_mm.brk	   = (unsigned long) _end;

	/* populate cmd_line too for later use, preserving boot_command_line */
	strlcpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = cmd_line;

	parse_early_param();

/** 20130112
	meminfo 의 각bank의 start주소를 페이지프레임 인덱스로 변환해 비교하는
	meminfo_cmp 결과를 기준으로 정렬한다. 	
**/
	sort(&meminfo.bank, meminfo.nr_banks, sizeof(meminfo.bank[0]), meminfo_cmp, NULL);
/** 20130119
  메모리 뱅크들에 대한 적정한 설정이 되어 있는지 조사하고 수정한다
**/
    sanity_check_meminfo();
/** 20130126    
 * memblock 자료구조 초기화
**/
	arm_memblock_init(&meminfo, mdesc);

/** 20130518    
 * page table 생성 및 bootmem_init
 **/
	paging_init(mdesc);
/** 20130518    
 * machine에 대한 resource 계층도 생성
 **/
	request_standard_resources(mdesc);

/** 20130518    
 * restart 함수 포인터가 정의되어 있으면 arm_pm_restart 전역변수에 저장
 * vexpress의 경우 v2m_restart 함수.
 **/
	if (mdesc->restart)
		arm_pm_restart = mdesc->restart;

/** 20130518    
 * vexpress의 경우 NULL 함수.
 **/
	unflatten_device_tree();

#ifdef CONFIG_SMP
	if (is_smp())
		smp_init_cpus();
#endif
/** 20130518    
 * vepress에서 NULL 함수.
 **/
	reserve_crashkernel();

/** 20130518    
 * vexpress에서 NULL 함수.
 **/
	tcm_init();

#ifdef CONFIG_MULTI_IRQ_HANDLER
/** 20130518    
 * vexpress의 경우 .handle_irq = gic_handle_irq 가 등록
 **/
	handle_arch_irq = mdesc->handle_irq;
#endif

/** 20130518    
 * VIRTUAL TERMINAL 함수 등록
 **/
#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
/** 20130518    
 * console switcher 지정.
 * vexpress의 경우 dummy_con.
 **/
	conswitchp = &dummy_con;
#endif
#endif

/** 20130518    
 * vexpress의 경우
 * .init_early = v2m_init_early,
 * 함수 실행
 **/
	if (mdesc->init_early)
		mdesc->init_early();
}


static int __init topology_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cpuinfo_arm *cpuinfo = &per_cpu(cpu_data, cpu);
		cpuinfo->cpu.hotpluggable = 1;
		register_cpu(&cpuinfo->cpu, cpu);
	}

	return 0;
}
subsys_initcall(topology_init);

#ifdef CONFIG_HAVE_PROC_CPU
static int __init proc_cpu_init(void)
{
	struct proc_dir_entry *res;

	res = proc_mkdir("cpu", NULL);
	if (!res)
		return -ENOMEM;
	return 0;
}
fs_initcall(proc_cpu_init);
#endif

static const char *hwcap_str[] = {
	"swp",
	"half",
	"thumb",
	"26bit",
	"fastmult",
	"fpa",
	"vfp",
	"edsp",
	"java",
	"iwmmxt",
	"crunch",
	"thumbee",
	"neon",
	"vfpv3",
	"vfpv3d16",
	"tls",
	"vfpv4",
	"idiva",
	"idivt",
	NULL
};

static int c_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "Processor\t: %s rev %d (%s)\n",
		   cpu_name, read_cpuid_id() & 15, elf_platform);

#if defined(CONFIG_SMP)
	for_each_online_cpu(i) {
		/*
		 * glibc reads /proc/cpuinfo to determine the number of
		 * online processors, looking for lines beginning with
		 * "processor".  Give glibc what it expects.
		 */
		seq_printf(m, "processor\t: %d\n", i);
		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n\n",
			   per_cpu(cpu_data, i).loops_per_jiffy / (500000UL/HZ),
			   (per_cpu(cpu_data, i).loops_per_jiffy / (5000UL/HZ)) % 100);
	}
#else /* CONFIG_SMP */
	seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
		   loops_per_jiffy / (500000/HZ),
		   (loops_per_jiffy / (5000/HZ)) % 100);
#endif

	/* dump out the processor features */
	seq_puts(m, "Features\t: ");

	for (i = 0; hwcap_str[i]; i++)
		if (elf_hwcap & (1 << i))
			seq_printf(m, "%s ", hwcap_str[i]);

	seq_printf(m, "\nCPU implementer\t: 0x%02x\n", read_cpuid_id() >> 24);
	seq_printf(m, "CPU architecture: %s\n", proc_arch[cpu_architecture()]);

	if ((read_cpuid_id() & 0x0008f000) == 0x00000000) {
		/* pre-ARM7 */
		seq_printf(m, "CPU part\t: %07x\n", read_cpuid_id() >> 4);
	} else {
		if ((read_cpuid_id() & 0x0008f000) == 0x00007000) {
			/* ARM7 */
			seq_printf(m, "CPU variant\t: 0x%02x\n",
				   (read_cpuid_id() >> 16) & 127);
		} else {
			/* post-ARM7 */
			seq_printf(m, "CPU variant\t: 0x%x\n",
				   (read_cpuid_id() >> 20) & 15);
		}
		seq_printf(m, "CPU part\t: 0x%03x\n",
			   (read_cpuid_id() >> 4) & 0xfff);
	}
	seq_printf(m, "CPU revision\t: %d\n", read_cpuid_id() & 15);

	seq_puts(m, "\n");

	seq_printf(m, "Hardware\t: %s\n", machine_name);
	seq_printf(m, "Revision\t: %04x\n", system_rev);
	seq_printf(m, "Serial\t\t: %08x%08x\n",
		   system_serial_high, system_serial_low);

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};
