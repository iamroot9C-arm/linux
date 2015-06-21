/*
 *  linux/arch/arm/kernel/head.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Kernel startup code for all 32-bit CPUs
 */
#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/pgtable.h>

#ifdef CONFIG_DEBUG_LL
#include <mach/debug-macro.S>
#endif

/*
 * swapper_pg_dir is the virtual address of the initial page table.
 * We place the page tables 16K below KERNEL_RAM_VADDR.  Therefore, we must
 * make sure that KERNEL_RAM_VADDR is correctly set.  Currently, we expect
 * the least significant 16 bits to be 0x8000, but we could probably
 * relax this restriction to KERNEL_RAM_VADDR >= PAGE_OFFSET + 0x4000.
 */

/** 20120922 PAGE_OFFSET Define 되어있음 (0x80000000), 
# The byte offset of the kernel image in RAM from the start of RAM.
<arch/arm/Makefile>
TEXT_OFFSET := $(textofs-y)

textofs-y       := 0x00008000

**/

#define KERNEL_RAM_VADDR	(PAGE_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif


/** 20120922 
	CONFIG_ARM_LPAE는 정의되어 있지 않음 
	LPAE(Large Physical Address Extension)
**/

#ifdef CONFIG_ARM_LPAE
	/* LPAE requires an additional page for the PGD */
#define PG_DIR_SIZE	0x5000
#define PMD_ORDER	3
#else
/** 20120922 
	PG_DIR_SIZE -> 16KB
**/

#define PG_DIR_SIZE	0x4000
#define PMD_ORDER	2
#endif

/** 20121006
    swapper_pg_dir를 전역 변수로 선언
    KERNEL_RAM_VADDR에서 PG_DIR_SIZE를 뺀 값을 swapper_pg_dir에 입력 
**/
/** 20130810
+-------------+ 0x80008000   KERNEL_RAM_VADDR
|             |
+-------------+ 0x80004000   swapper_pg_dir
|             |
+-------------+ 0x80000000   PAGE_OFFSET       (TEXT_OFFSET 0x8000)
**/
	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE

	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET - PG_DIR_SIZE
	.endm

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * This is normally called from the decompressor code.  The requirements
 * are: MMU = off, D-cache = off, I-cache = dont care, r0 = 0,
 * r1 = machine nr, r2 = atags or dtb pointer.
 *
 * This code is mostly position independent, so if you link the kernel at
 * 0xc0008000, you call this at __pa(0xc0008000).
 *
 * See linux/arch/arm/tools/mach-types for the complete list of machine
 * numbers for r1.
 *
 * We're trying to keep crap to a minimum; DO NOT add any machine specific
 * crap here - that's what the boot loader (or in extreme, well justified
 * circumstances, zImage) is for.
 */
	.arm
/** 20120922 
	__HEAD(define 매크로) : .head.text 섹션
**/
	__HEAD
ENTRY(stext)

 THUMB(	adr	r9, BSYM(1f)	)	@ Kernel is always entered in ARM.
 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
 THUMB(	.thumb			)	@ switch to Thumb now.
 THUMB(1:			)

/** 20120922
	setmode(.macro 매크로) : CPSR_C의 모드를 지정해준다(FIQ,IRQ,SVC비트를 setting)
**/

/** 20121006
	/linux/arch/arm/include/asm/assembler.h 
**/	
	setmode	PSR_F_BIT | PSR_I_BIT | SVC_MODE, r9 @ ensure svc mode 
						@ and irqs disabled
/** 20120922
	코프로세서에서 processor id를 가져온다음 프로세서 타입을 찾는다
**/

	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type		@ r5=procinfo r9=cpuid
	movs	r10, r5				@ invalid processor (r5=0)?
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p			@ yes, error 'p'

#ifdef CONFIG_ARM_LPAE
	mrc	p15, 0, r3, c0, c1, 4		@ read ID_MMFR0
	and	r3, r3, #0xf			@ extract VMSA support
	cmp	r3, #5				@ long-descriptor translation table format?
 THUMB( it	lo )				@ force fixup-able long branch encoding
	blo	__error_p			@ only classic page table format
#endif
/** 20120922
	r3 : physical
	r4 : .(virtual)
	r8 : PAGE_OFFSET(virtual => physical)
**/
#ifndef CONFIG_XIP_KERNEL
/** 20121013
    autoconf.h 빌드시 자동으로 PAGE_OFFSET이 지정되었다.
    #define CONFIG_PAGE_OFFSET 0x80000000
	(커널 address space의 시작 주소.
	 보통은 USER/KERNEL 3G/1G로 split 하여 사용하지만, vexpress는 2G/2G로 split 하여 사용함)

    r4 : 2f - . 즉, virtual - physical 사이의 offset 값이 저장되어 있다.

	r8(PA) <= r8(VA) + r4(PA-VA)

	이후 PHYS_OFFSET은 virtual address PHYS_OFFSET이 물리주소 위치
**/
	adr	r3, 2f
	ldmia	r3, {r4, r8}
	sub	r4, r3, r4			@ (PHYS_OFFSET - PAGE_OFFSET)
	add	r8, r8, r4			@ PHYS_OFFSET
#else
	ldr	r8, =PHYS_OFFSET		@ always constant in this case
#endif

	/*
	 * r1 = machine no, r2 = atags or dtb,
	 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
	 */
	bl	__vet_atags
#ifdef CONFIG_SMP_ON_UP
	bl	__fixup_smp
#endif
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
	bl	__fixup_pv_table
#endif
	bl	__create_page_tables

	/*
	 * The following calls CPU specific code in a position independent
	 * manner.  See arch/arm/mm/proc-*.S for details.  r10 = base of
	 * xxx_proc_info structure selected by __lookup_processor_type
	 * above.  On return, the CPU will be ready for the MMU to be
	 * turned on, and r0 will hold the CPU control register value.
	 */
	ldr	r13, =__mmap_switched		@ address to jump to after
						@ mmu has been enabled
/** 20121020
	lr에 __enable_mmu label 주소 저장. 
**/
	adr	lr, BSYM(1f)			@ return (PIC) address
/** 20121020
	r4 = [PA] page table address
	r4 : 0x60004000, __create_page_tables에서 설정.
**/
	mov	r8, r4				@ set TTBR1 to swapper_pg_dir
/** 20121013
    pc에 직접 값을 넣어 W(b)    \initfunc 을 실행.
	initfunc은 __v7_proc을 호출할 때 지정한 argument

	arch/arm/mm/proc-v7.S
		__v7_proc __v7_ca9mp_setup
**/
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	mov	pc, r12				)
/**
	__v7_proc
 	ARM(	add	pc, r10, #PROCINFO_INITFUNC	)
		call ==>	__v7_ca9mp_setup: 
					__v7_setup:
						__enable__mmu로 return
**/
1:	b	__enable_mmu
ENDPROC(stext)
	.ltorg
#ifndef CONFIG_XIP_KERNEL
2:	.long	.
	.long	PAGE_OFFSET
#endif

/*
 * Setup the initial page tables.  We only setup the barest
 * amount which are required to get the kernel running, which
 * generally means mapping in the kernel code.
 *
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 *
 * Returns:
 *  r0, r3, r5-r7 corrupted
 *  r4 = physical page table address
 */
__create_page_tables:
/** 20121013
	pgtbl은 macro.
	add	r4, r8, #TEXT_OFFSET - PG_DIR_SIZE (r8: [PA]PAGE_OFFSET, 0x8000-0x4000)
	r4 = [PA] page table address
**/
	pgtbl	r4, r8				@ page table address

	/*
	 * Clear the swapper page table
	 */
	mov	r0, r4
	mov	r3, #0
	add	r6, r0, #PG_DIR_SIZE
/** 20121013
    cache도 사용하지 않는데 왜 4번으로 나눠서 클리어 해주는 것일까???
**/
1:	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	teq	r0, r6
	bne	1b
/** 20121028
	
**/

#ifdef CONFIG_ARM_LPAE
	/*
	 * Build the PGD table (first level) to point to the PMD table. A PGD
	 * entry is 64-bit wide.
	 */
	mov	r0, r4
	add	r3, r4, #0x1000			@ first PMD table address
	orr	r3, r3, #3			@ PGD block type
	mov	r6, #4				@ PTRS_PER_PGD
	mov	r7, #1 << (55 - 32)		@ L_PGD_SWAPPER
1:	str	r3, [r0], #4			@ set bottom PGD entry bits
	str	r7, [r0], #4			@ set top PGD entry bits
	add	r3, r3, #0x1000			@ next PMD table
	subs	r6, r6, #1
	bne	1b

	add	r4, r4, #0x1000			@ point to the PMD tables
#endif

/** 20121013
    procinfo의 MMUFLAGS 값을 읽어옴
	  procinfo : vmlinux.lds에서 __proc_info_begin ~ __proc_info_end 사이에 정의
	  .proc.info.init 는 arch/arm/mm/proc-v7.S 에 위치

    arch/arm/include/asm/page.h
         #ifdef CONFIG_ARM_LPAE
         #include <asm/pgtable-3level-types.h>
         #else
         #include <asm/pgtable-2level-types.h>		<- 아래 속성값의 정의는 이 파일을 참고한다.
         #endif

	arch/arm/include/asm/pgtable-2level-hwdef.h
	ALT_SMP(.long	PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | \
			PMD_SECT_AF | PMD_FLAGS_SMP | \mm_mmuflags)

	각 flag에 해당하는 필드는 Cortex-A Series Programmer Guide, 
		Figure 10-3 Level 1 page table entry format

	type            PMD_TYPE_SECT
	AP              PMD_SECT_AP_WRITE | PMD_SECT_AP_READ
	AF              0
    PMD_FLAGS_SMP   PMD_SECT_WBWA|PMD_SECT_S
			#define PMD_SECT_WBWA		(PMD_SECT_TEX(1) | PMD_SECT_CACHEABLE | PMD_SECT_BUFFERABLE)
				TEX(001), C(1), B(1)에 해당하는 entry는 manual에 존재하지 않음
				20121020 PRRR, NMRR 의 인덱스로 사용됨. 
					아래 테이블 참조. proc-v7.S의  v7_ttb_setup 매크로 이후 코드. 	
					Table B4-28 Memory attributes and the n value for the PRRR field descriptions

			#define PMD_SECT_S		(_AT(pmdval_t, 1) << 16)	v6 	<-- shareable???
				이 비트에 대한 설명. Cortex-A Series Programmer Guide, Table 10-2 Memory attributes

    mm_mmuflags   argument. default 0

**/
	ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS] @ mm_mmuflags

	/*
	 * Create identity mapping to cater for __enable_mmu.
	 * This identity mapping will be removed by paging_init().
	 */
	adr	r0, __turn_mmu_on_loc
/** 20121013
	이 부분을 초기화 하는 이유??? 이게 turn_mmu_on???
   	r5 <- .long	__turn_mmu_on
	r6 <- .long	__turn_mmu_on_end

	20121027
	__turn_mmu_on ~ __turn_mmu_on_end 에 해당하는 page entry를 왜 이렇게 조작해주는지 ???
    
	이어지는 코드에서 커널 영역의 시작에서 커널 .bss 섹션의 끝까지 page entry를 조작하기 때문에., 필요없는 동작일 듯 ???
**/
	ldmia	r0, {r3, r5, r6}
	sub	r0, r0, r3			@ virt->phys offset
	add	r5, r5, r0			@ phys __turn_mmu_on
	add	r6, r6, r0			@ phys __turn_mmu_on_end
/** 20121013
    #SECTION_SHIFT 20 -> 1M의 index를 구함
**/
	mov	r5, r5, lsr #SECTION_SHIFT
	mov	r6, r6, lsr #SECTION_SHIFT

/** 20121013
    r7 : 읽어온 MM_FLAGS값
	r5 : Section Base Address Index
	r3 : 두 값으로 Level 1 Section Entry를 생성
**/
1:	orr	r3, r7, r5, lsl #SECTION_SHIFT	@ flags + kernel base
/** 20121013
	r4 = [PA] page table address
    #if CONFIG_ARM_LPAE
	  PMD_ORDER 3
    #else
	  PMD_ORDER 2
**/
	str	r3, [r4, r5, lsl #PMD_ORDER]	@ identity mapping
	cmp	r5, r6
	addlo	r5, r5, #1			@ next section
	blo	1b


/** 20121013
    page table entry 중 커널의 VA(PAGE_OFFSET ~ BSS 섹션의 끝)에
	해당하는 구간에 PA주소와 MM_FLAGS를 저장
**/
	/*
	 * Map our RAM from the start to the end of the kernel .bss section.
	 */
/** 20121013
    커널 영역의 시작에서 커널 .bss 섹션의 끝까지
	r0 : [PA]page table address + [VA]#PAGE_OFFSET의 index 주소
	      => [PA] entry값을 써넣을 위치. 커널의 시작 주소에 해당하는 entry 주소
	r6 : [VA]bss 섹션의 마지막 주소
**/
	add	r0, r4, #PAGE_OFFSET >> (SECTION_SHIFT - PMD_ORDER)
	ldr	r6, =(_end - 1)
/** 20121013
    r8 : [PA]PAGE_OFFSET (Section Base Address로 바로 적용)
	r7 : MM_FLAGS
	r3 : entry 값을 생성
**/
	orr	r3, r8, r7
/** 20121013
    r6 : [VA]bss섹션의 끝 주소에 해당하는 entry 주소
**/
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
/** 20121013
    r0 ~ r6 사이(커널 영역)의 page table에 entry를 채워넣음
	반복문을 수행할 때마다 다음 entry에 써넣을 Base Address를 1MB씩 증가
**/
1:	str	r3, [r0], #1 << PMD_ORDER
	add	r3, r3, #1 << SECTION_SHIFT
	cmp	r0, r6
	bls	1b

#ifdef CONFIG_XIP_KERNEL
	/*
	 * Map the kernel image separately as it is not located in RAM.
	 */
#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
	mov	r3, pc
	mov	r3, r3, lsr #SECTION_SHIFT
	orr	r3, r7, r3, lsl #SECTION_SHIFT
	add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ORDER]!
	ldr	r6, =(_edata_loc - 1)
	add	r0, r0, #1 << PMD_ORDER
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
1:	cmp	r0, r6
	add	r3, r3, #1 << SECTION_SHIFT
	strls	r3, [r0], #1 << PMD_ORDER
	bls	1b
#endif

	/*
	 * Then map boot params address in r2 if specified.
	 */
	mov	r0, r2, lsr #SECTION_SHIFT
/** 20121013
    r0 : [PA]r2(atag)에 해당하는 base address
**/
	movs	r0, r0, lsl #SECTION_SHIFT
/** 20121013
    r2 [PA] - r8[PA] + [VA]#PAGE_OFFSET => [VA] atag
**/
	subne	r3, r0, r8
	addne	r3, r3, #PAGE_OFFSET
/** 20121013
    [VA] atag에 해당하는 entry의 주소를 r3에 저장
**/
	addne	r3, r4, r3, lsr #(SECTION_SHIFT - PMD_ORDER)
	orrne	r6, r7, r0
	strne	r6, [r3]

#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
	/*
	 * Map in IO space for serial debugging.
	 * This allows debug messages to be output
	 * via a serial console before paging_init.
	 */
	addruart r7, r3, r0

	mov	r3, r3, lsr #SECTION_SHIFT
	mov	r3, r3, lsl #PMD_ORDER

	add	r0, r4, r3
	mov	r3, r7, lsr #SECTION_SHIFT
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
	orr	r3, r7, r3, lsl #SECTION_SHIFT
#ifdef CONFIG_ARM_LPAE
	mov	r7, #1 << (54 - 32)		@ XN
#else
	orr	r3, r3, #PMD_SECT_XN
#endif
	str	r3, [r0], #4
#ifdef CONFIG_ARM_LPAE
	str	r7, [r0], #4
#endif

#else /* CONFIG_DEBUG_ICEDCC || CONFIG_DEBUG_SEMIHOSTING */
	/* we don't need any serial debugging mappings */
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
#endif

#if defined(CONFIG_ARCH_NETWINDER) || defined(CONFIG_ARCH_CATS)
	/*
	 * If we're using the NetWinder or CATS, we also need to map
	 * in the 16550-type serial port for the debug messages
	 */
	add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x7c000000
	str	r3, [r0]
#endif
#ifdef CONFIG_ARCH_RPC
	/*
	 * Map in screen at 0x02000000 & SCREEN2_BASE
	 * Similar reasons here - for debug.  This is
	 * only for Acorn RiscPC architectures.
	 */
	add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x02000000
	str	r3, [r0]
	add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0]
#endif
#endif
#ifdef CONFIG_ARM_LPAE
	sub	r4, r4, #0x1000		@ point to the PGD table
#endif
	mov	pc, lr
ENDPROC(__create_page_tables)
	.ltorg
	.align
__turn_mmu_on_loc:
	.long	.
	.long	__turn_mmu_on
	.long	__turn_mmu_on_end

#if defined(CONFIG_SMP)
	__CPUINIT
ENTRY(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 *
	 * Ensure that we're in SVC mode, and IRQs are disabled.  Lookup
	 * the processor type - there is no need to check the machine type
	 * as it has already been validated by the primary processor.
	 */
	setmode	PSR_F_BIT | PSR_I_BIT | SVC_MODE, r9
	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type
	/** 20150117
	 * __lookup_processor_type의 리턴 결과로 r5 : procinfo
	 **/
	movs	r10, r5				@ invalid processor?
	moveq	r0, #'p'			@ yes, error 'p'
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p

	/*
	 * Use the page tables supplied from  __cpu_up.
	 */
	/** 20150118    
		r7  <- secondary_data
		r13 = r12 <- __secondary_switched
		__enable_mmu 호출 후 r13의 함수로 이동한다.

		__enable_mmu
			__secondary_switched
	**/
	adr	r4, __secondary_data
	ldmia	r4, {r5, r7, r12}		@ address to jump to after
	sub	lr, r4, r5			@ mmu has been enabled
	ldr	r4, [r7, lr]			@ get secondary_data.pgdir
	add	r7, r7, #4
	ldr	r8, [r7, lr]			@ get secondary_data.swapper_pg_dir
	adr	lr, BSYM(__enable_mmu)		@ return address
	mov	r13, r12			@ __secondary_switched address
	/** 20150117    
		arch/arm/mm/proc-v7.S
		r10:
			__v7_proc __v7_ca9mp_setup

		branch가 아니므로 #PROCINFO_INITFUNC 이후 리턴 주소는
		lr에 넣어둔 __enable_mmu.
	**/
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	) @ initialise processor
						  @ (return control reg)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	mov	pc, r12				)
ENDPROC(secondary_startup)

	/*
	 * r6  = &secondary_data
	 */
/** 20150118    
 * sp <- secondary_data.stack
 * fp <- 0
 **/
ENTRY(__secondary_switched)
	ldr	sp, [r7, #4]			@ get secondary_data.stack
	mov	fp, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)

	.align

/** 20150620    
 * __secondary_data
 **/
	.type	__secondary_data, %object
__secondary_data:
	.long	.
	.long	secondary_data
	.long	__secondary_switched
#endif /* defined(CONFIG_SMP) */



/*
 * Setup common bits before finally enabling the MMU.  Essentially
 * this is just loading the page table pointer and domain access
 * registers.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r4  = page table pointer
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 */
__enable_mmu:
/** 20121020
  	__LINUX_ARM_ARCH__ 어디에 정의 되어 있는지 ??? 
	arch/arm/Makefile에서 정의됨. vexpress에서는  __LINUX_ARM_ARCH__=7
**/
#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
	orr	r0, r0, #CR_A
#else
	bic	r0, r0, #CR_A
#endif
#ifdef CONFIG_CPU_DCACHE_DISABLE
	bic	r0, r0, #CR_C
#endif
#ifdef CONFIG_CPU_BPREDICT_DISABLE
	bic	r0, r0, #CR_Z
#endif
#ifdef CONFIG_CPU_ICACHE_DISABLE
	bic	r0, r0, #CR_I
#endif
#ifdef CONFIG_ARM_LPAE
	mov	r5, #0
	mcrr	p15, 0, r4, r5, c2		@ load TTBR0
#else
/** 20121020
	#define DOMAIN_KERNEL	0
	#define DOMAIN_TABLE	0
	#define DOMAIN_USER	1
	#define DOMAIN_IO	2
	r5 = 1<<2 | 1<<0 | 1<<0 | 1<<(2*2) = 0b10101 = 0x15

	항상 아래 두 값이 같은데., 왜 이렇게 계산을 하지 ???
	domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) == domain_val(DOMAIN_TABLE, DOMAIN_MANAGER) 
	그리고 DOMAIN_TABLE 의 의미는 ???
	DOMAIN_CLIENT와 DOMAIN_MANAGER가 같은 값을 가지게 되는데., 이래도 되나 ??? 
**/
	mov	r5, #(domain_val(DOMAIN_USER, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_TABLE, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_IO, DOMAIN_CLIENT))
	mcr	p15, 0, r5, c3, c0, 0		@ load domain access register
/** 20121020
  	TTBR0 setting

	TTBR1은 proc-v7.S 에서 아래와 같이 설정했음. 
	r4 = r8 = 0x60004000 [PA] page table address
	orr	r4, r4, #TTB_FLAGS_SMP
	orr	r8, r8, #TTB_FLAGS_SMP
	mcr	p15, 0, r8, c2, c0, 1	@ load TTB1

	현 시점에서 TTBR0, TTBR1 는 같은 값임.

	20150620
	secondary_startup에서 r4: idmap_pgd, r8: swapper_pg_dir이 들어간다.

	__v7_ca9mp_setup에서 
		mcr	p15, 0, r8, c2, c0, 1	@ load TTB1
	아래에서
		mcr	p15, 0, r4, c2, c0, 0		@ load page table pointer
**/
	mcr	p15, 0, r4, c2, c0, 0		@ load page table pointer
#endif
	b	__turn_mmu_on
ENDPROC(__enable_mmu)

/** 20121027
	???
  	아래 주석에서 얘기하는 그 메일이 여기인듯...
	http://www.arm.linux.org.uk/mailinglists/faq.php#f6
**/
/*
 * Enable the MMU.  This completely changes the structure of the visible
 * memory space.  You will not be able to trace execution through this.
 * If you have an enquiry about this, *please* check the linux-arm-kernel
 * mailing list archives BEFORE sending another post to the list.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 *
 * other registers depend on the function called upon completion
 */
	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(__turn_mmu_on)
/** 20121020
	mov r0, r0, mov r3, r3 같은 의미없는 인스트럭션이 instr_sync 명령 전후에 있는지???
	
	20121027
	instr_sync 에 대한 사용 가이드가 어디 있는지 ???

	SCTLR  설정 이후에 의미없어 보이는 MIDR ???
**/
	mov	r0, r0
	instr_sync
	/** 20150620    
	 * SCTLR 레지스터에 r0 레지스터의 값을 써준다.
	 * r0 레지스터의 값은 __enable_mmu에서부터 넘어온다.
	 * SCTLR 0번 비트가 Enables the MMU를 조작.
	 **/
	mcr	p15, 0, r0, c1, c0, 0		@ write control reg
	mrc	p15, 0, r3, c0, c0, 0		@ read id reg
	instr_sync
	mov	r3, r3
	/** 20121020
	 * r13 : __mmap_switched		
	 * __mmap_switched 로 b.
	 *
	 * 20150620
	 * SMP로 secondary_startup에서 들어올 때,
	 * r13은 [VA] __secondary_switched이다.
	 **/
	mov	r3, r13
	mov	pc, r3
__turn_mmu_on_end:
ENDPROC(__turn_mmu_on)
	.popsection

/** 20120922 
 * ARM Ltd. Cortex A9 processor.
	.long   0x410fc090 (r9)
        .long   0xff0ffff0

*** DDI0406C_arm_architecture_reference_manual <p.1631>
 Bits[31], in an implementation that includes the Multiprocessing Extensions
 RAO. Indicates that the implementation uses the Multiprocessing Extensions register format.

 U, bit[30], in an implementation that includes the Multiprocessing Extensions
 Indicates a Uniprocessor system, as distinct from processor 0 in a multiprocessor system. The
 possible values of this bit are:
	0 : Processor is part of a multiprocessor system.
	1 : Processor is part of a uniprocessor system.

 =>MPIDR을 읽어들여 SMP인지 검사
**/

#ifdef CONFIG_SMP_ON_UP
	__INIT
__fixup_smp:
	and	r3, r9, #0x000f0000	@ architecture version
	teq	r3, #0x000f0000		@ CPU ID supported?
	bne	__fixup_smp_on_up	@ no, assume UP

	bic	r3, r9, #0x00ff0000
	bic	r3, r3, #0x0000000f	@ mask 0xff00fff0
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000b000
	orr	r4, r4, #0x00000020	@ val 0x4100b020
	teq	r3, r4			@ ARM 11MPCore?
	moveq	pc, lr			@ yes, assume SMP

	mrc	p15, 0, r0, c0, c0, 5	@ read MPIDR
	and	r0, r0, #0xc0000000	@ multiprocessing extensions and
	teq	r0, #0x80000000		@ not part of a uniprocessor system?
	moveq	pc, lr			@ yes, assume SMP

/** 20120922
	__smpalt_begin(r4), __smpalt_end(r5)의 physical address 계산
**/

/** 20121006
	참조 : http://www.iamroot.org/xe/61099#comment_61348
	CONFIG_SMP_ON_UP
	출처 : http://cateee.net/lkddb/web-lkddb/SMP_ON_UP.html
	SMP 용으로 컴파일 된 커널이 UP에서 돌아가도 문제가 생기지 않도록 해주는 기능인것 같습니다.
	arch/arm/Kconfig 1334라인에 SMP_ON_UP 컴파일 옵션으로 들어가있습니다.
	이 기능(컴파일 옵션)은 EXPERIMENTAL로 명시 되어있습니다.
**/
__fixup_smp_on_up:
	adr	r0, 1f
	ldmia	r0, {r3 - r5}
	sub	r3, r0, r3
	add	r4, r4, r3
	add	r5, r5, r3
	b	__do_fixup_smp_on_up
ENDPROC(__fixup_smp)

	.align
1:	.word	.
	.word	__smpalt_begin
	.word	__smpalt_end

/** 20130518
	smp_on_up
**/
	.pushsection .data
	.globl	smp_on_up
smp_on_up:
	ALT_SMP(.long	1)
	ALT_UP(.long	0)
	.popsection
#endif

	.text
__do_fixup_smp_on_up:
	cmp	r4, r5
	movhs	pc, lr
	ldmia	r4!, {r0, r6}
 ARM(	str	r6, [r0, r3]	)
 THUMB(	add	r0, r0, r3	)
#ifdef __ARMEB__
 THUMB(	mov	r6, r6, ror #16	)	@ Convert word order for big-endian.
#endif
 THUMB(	strh	r6, [r0], #2	)	@ For Thumb-2, store as two halfwords
 THUMB(	mov	r6, r6, lsr #16	)	@ to be robust against misaligned r3.
 THUMB(	strh	r6, [r0]	)
	b	__do_fixup_smp_on_up
ENDPROC(__do_fixup_smp_on_up)

ENTRY(fixup_smp)
	stmfd	sp!, {r4 - r6, lr}
	mov	r4, r0
	add	r5, r0, r1
	mov	r3, #0
	bl	__do_fixup_smp_on_up
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(fixup_smp)
/** 20121006
arch/arm/Kconfig
	config ARM_PATCH_PHYS_VIRT
    	bool "Patch physical to virtual translations at runtime" if EMBEDDED
    	default y

**/
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT

/* __fixup_pv_table - patch the stub instructions with the delta between
 * PHYS_OFFSET and PAGE_OFFSET, which is assumed to be 16MiB aligned and
 * can be expressed by an immediate shifter operand. The stub instruction
 * has a form of '(add|sub) rd, rn, #imm'.
 */

/**20121006
	좀더 확인 필요함???		
	참고  : https://github.com/iamroot9C-arm/linux/commit/dc21af99fadcfa0ae65b52fd0895f85824f0c288	
**/
	__HEAD
__fixup_pv_table:
	adr	r0, 1f
	ldmia	r0, {r3-r5, r7}
	sub	r3, r0, r3	@ PHYS_OFFSET - PAGE_OFFSET
	add	r4, r4, r3	@ adjust table start address
	add	r5, r5, r3	@ adjust table end address
	add	r7, r7, r3	@ adjust __pv_phys_offset address
	/** 20121215
	  이곳에서 __pv_phys_offset을 처음 저장.
	  r8 값은 __fixup_pv_table 호출 전에 PAGE_OFFSET(VA)를 PA로 변환한 값
	**/
	str	r8, [r7]	@ save computed PHYS_OFFSET to __pv_phys_offset
	mov	r6, r3, lsr #24	@ constant for add/sub instructions
/**20121006
	어떻게 16MB 정렬이 되어 있나??? 그리고 왜???
	instruction format을 보면 8비트가 immediate 값으로 사용
**/
	teq	r3, r6, lsl #24 @ must be 16MiB aligned
THUMB(	it	ne		@ cross section branch )
	bne	__error
	str	r6, [r7, #4]	@ save to __pv_offset
	b	__fixup_a_pv_table
ENDPROC(__fixup_pv_table)

	.align
/** 20121006
__pv_stub에서 .pv_table section만들어짐.
arch/arm/include/asm/memory.h
**/
1:	.long	.
	.long	__pv_table_begin
	.long	__pv_table_end
2:	.long	__pv_phys_offset

	.text
__fixup_a_pv_table:
#ifdef CONFIG_THUMB2_KERNEL
	lsls	r6, #24
	beq	2f
	clz	r7, r6
	lsr	r6, #24
	lsl	r6, r7
	bic	r6, #0x0080
	lsrs	r7, #1
	orrcs	r6, #0x0080
	orr	r6, r6, r7, lsl #12
	orr	r6, #0x4000
	b	2f
1:	add     r7, r3
	ldrh	ip, [r7, #2]
	and	ip, 0x8f00
	orr	ip, r6	@ mask in offset bits 31-24
	strh	ip, [r7, #2]
2:	cmp	r4, r5
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	bx	lr
#else
	b	2f
/** 20121006
	1. r7인덱스가 가르키고 있는 명령어를 가져와서 ip에 저장
	2. 명령어 코드의 하위 8비트를 clear
	3. physical addr의 상위 8비트를 명령어 코드의 하위 8비트에
	   저장 

	eg)
	add r5,r5,0x81000000
	micro code : 0x00080
	
	0x00080 -> 0x00060 physical addr 로 변환
	add r5,r5,0x60000000

	참조 : arm/include/asm/memory.h
		- pv_table 의 비밀	       
		   http://www.scribd.com/doc/24366257/Arm-Instruction-Set#outer_page_10

	이렇게 하는 이유 : 실제 바이너리는 VA와 작동하는 PA 차이를 보정해주기 위해서 인것 같은데..
	CONFIG_ARM_PATCH_PHYS_VIRT define이  없으면 어떻게 될까?
**/
1:	ldr	ip, [r7, r3]
	bic	ip, ip, #0x000000ff
	orr	ip, ip, r6	@ mask in offset bits 31-24
	str	ip, [r7, r3]
2:	cmp	r4, r5
/** 20121006
	cc prefix : carry clear

	if(r4 < r5)
	{
		ldrcc ....
		bcc ....
	}
	else
	{
		mov ....
	}
**/

/** 20121006
	pv_table 의 명령어 인덱스 주소를 가져와서 r7에 저장.
**/
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	mov	pc, lr
#endif
/** 20121013
	__fixup_pv_table 에서 b로 들어왔을 때에는 __fixup_pv_table을 호출한 곳으로 리턴.
 */
ENDPROC(__fixup_a_pv_table)

ENTRY(fixup_pv_table)
	stmfd	sp!, {r4 - r7, lr}
	ldr	r2, 2f			@ get address of __pv_phys_offset
	mov	r3, #0			@ no offset
	mov	r4, r0			@ r0 = table start
	add	r5, r0, r1		@ r1 = table size
	ldr	r6, [r2, #4]		@ get __pv_offset
	bl	__fixup_a_pv_table
	ldmfd	sp!, {r4 - r7, pc}
ENDPROC(fixup_pv_table)

	.align
2:	.long	__pv_phys_offset

	.data
	.globl	__pv_phys_offset
	.type	__pv_phys_offset, %object
__pv_phys_offset:
	.long	0
	.size	__pv_phys_offset, . - __pv_phys_offset
__pv_offset:
	.long	0
#endif

#include "head-common.S"
