/*
 * arch/arm/mm/proc-v7-2level.S
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define TTB_S		(1 << 1)
#define TTB_RGN_NC	(0 << 3)
#define TTB_RGN_OC_WBWA	(1 << 3)
#define TTB_RGN_OC_WT	(2 << 3)
#define TTB_RGN_OC_WB	(3 << 3)
#define TTB_NOS		(1 << 5)
#define TTB_IRGN_NC	((0 << 0) | (0 << 6))
#define TTB_IRGN_WBWA	((0 << 0) | (1 << 6))
#define TTB_IRGN_WT	((1 << 0) | (0 << 6))
#define TTB_IRGN_WB	((1 << 0) | (1 << 6))

/* PTWs cacheable, inner WB not shareable, outer WB not shareable */
#define TTB_FLAGS_UP	TTB_IRGN_WB|TTB_RGN_OC_WB
#define PMD_FLAGS_UP	PMD_SECT_WB

/* PTWs cacheable, inner WBWA shareable, outer WBWA not shareable */
/** 20121020
 * TTB_FLAGS_SMP : 0b0110_1010
 **/
#define TTB_FLAGS_SMP	TTB_IRGN_WBWA|TTB_S|TTB_NOS|TTB_RGN_OC_WBWA
#define PMD_FLAGS_SMP	PMD_SECT_WBWA|PMD_SECT_S

/*
 *	cpu_v7_switch_mm(pgd_phys, tsk)
 *
 *	Set the translation table base pointer to be pgd_phys
 *
 *	- pgd_phys - physical address of new TTB
 *
 *	It is assumed that:
 *	- we are not using split page tables
 */
/** 20150801, 20160604
 * 새로운 mm으로 context를 시작하기 위해 TTB0와 context ID를 설정한다.
 *
 * cpu_switch_mm 함수 호출시 호출됨 (mm->pgd, mm)
 * r0: pgd의 물리주소
 * r1: mm_struct (context.id를 추출)
 *
 * MM_CONTEXT_ID : offset은 arch/arm/kernel/asm-offsets.c 에 의해 생성되는 파일에 정의된다.
 *
 **/
ENTRY(cpu_v7_switch_mm)
#ifdef CONFIG_MMU
	/** 20150801
	 * r0 <- pgd의 물리주소 | TTB_FLAGS_SMP
	 * r1 <- mm->context.id
	 * r2 <- 0
	 **/
	mov	r2, #0
	ldr	r1, [r1, #MM_CONTEXT_ID]	@ get mm->context.id
	ALT_SMP(orr	r0, r0, #TTB_FLAGS_SMP)
	ALT_UP(orr	r0, r0, #TTB_FLAGS_UP)
#ifdef CONFIG_ARM_ERRATA_430973
	mcr	p15, 0, r2, c7, c5, 6		@ flush BTAC/BTB
#endif
#ifdef CONFIG_PID_IN_CONTEXTIDR
	mrc	p15, 0, r2, c13, c0, 1		@ read current context ID
	lsr	r2, r2, #8			@ extract the PID
	bfi	r1, r2, #8, #24			@ insert into new context ID
#endif
#ifdef CONFIG_ARM_ERRATA_754322
	dsb
#endif
	/** 20150801
	 * B4.1.36 CONTEXTIDR, Context ID Register, VMSA
	 * 위에서 준비한 r1 값을 context.id를 CONTEXTIDR에 설정
	 * 위에서 준비한 r0 값을 TTBR0 레지스터에 설정
	 **/
	mcr	p15, 0, r1, c13, c0, 1		@ set context ID
	isb
	mcr	p15, 0, r0, c2, c0, 0		@ set TTB 0
	isb
#endif
	mov	pc, lr
ENDPROC(cpu_v7_switch_mm)

/*
 *	cpu_v7_set_pte_ext(ptep, pte)
 *
 *	Set a level 2 translation table entry.
 *
 *	- ptep  - pointer to level 2 translation table entry
 *		  (hardware version is stored at +2048 bytes)
 *	- pte   - PTE value to store
 *	- ext	- value for extended PTE bits
 */
/** 20140526
 * page table entry의 Linux, HW 부분을 각각 채운다.
 * r0 : ptep - level 2 변환 테이블의 위치
 * r1 : pte  - 저장할 pte 값
 * r2 : ext  - HW pt에 추가할 속성
 **/
ENTRY(cpu_v7_set_pte_ext)
#ifdef CONFIG_MMU
	/** 20130309
		pte의 linux pt에 linux 속성을 가진 pte value 저장
	 **/
	str	r1, [r0]			@ linux version

	/** 20130309
		pte의 값 중 b4 ~ b9까지 클리어 (Linux용 attribute의 일부)
	**/
	bic	r3, r1, #0x000003f0
	/** 20130309
		pte의 값 중 b0 ~ b1까지 클리어
	**/
	bic	r3, r3, #PTE_TYPE_MASK
	/** 20130309
		r2는 ext
	**/
	orr	r3, r3, r2
	/** 20130309
		b1, b4를 set
	**/
	orr	r3, r3, #PTE_EXT_AP0 | 2

	/** 20130309
		r1의 b4가 세팅되어 있는지 체크(bit and)
	**/
	tst	r1, #1 << 4
	/** 20130309
		같으면(set되어 있으면) z flag가 0으로 ne 수행
		(v5에 해당)
	**/
	orrne	r3, r3, #PTE_EXT_TEX(1)

	/** 20130309
		DIRTY 비트를 반전
	**/
	eor	r1, r1, #L_PTE_DIRTY
	/** 20130309
		r1의 두 비트 중 하나라도 켜져 있는지 검사
		(v6에 해당)
	**/
	tst	r1, #L_PTE_RDONLY | L_PTE_DIRTY
	orrne	r3, r3, #PTE_EXT_APX

	/** 20130309
		PTE_USER가 set되어 있으면 PTE_EXT_AP1을 켜줌
	**/
	tst	r1, #L_PTE_USER
	orrne	r3, r3, #PTE_EXT_AP1
#ifdef CONFIG_CPU_USE_DOMAINS
	/** 20130309
	Cortex-A Series PG 10.7.3 Domain
	  The use of domains is deprecated in the ARMv7 architecture,
	**/
	@ allow kernel read/write access to read-only user pages
	tstne	r3, #PTE_EXT_APX
	bicne	r3, r3, #PTE_EXT_APX | PTE_EXT_AP0
#endif

	/** 20130309
		PTE_XN 비트를 검사해 설정되어 있다면 PTE_EXT_NX을 켜줌
	 **/
	tst	r1, #L_PTE_XN
	orrne	r3, r3, #PTE_EXT_XN

	/** 20130309
		L_PTE_YOUNG이 켜져 있을 때
			L_PTE_PRESENT가 켜져 있을 때
				r3 유지
			L_PTE_PRESENT가 꺼져 있을 때
				r3 <- 0
		L_PTE_YOUNG이 꺼져 있을 때
				r3 <- 0
	**/
	tst	r1, #L_PTE_YOUNG
	tstne	r1, #L_PTE_PRESENT
	moveq	r3, #0

/** 20130309
	pte의 h/w pt 위치에 위에서 설정한 r3값을 저장. r0의 값이 +2048로 업데이트
**/
 ARM(	str	r3, [r0, #2048]! )
 THUMB(	add	r0, r0, #2048 )
 THUMB(	str	r3, [r0] )
 /** 20130309
   DCCMVAC, Data Cache Clean by MVA to PoC
	  Clean and Invalidate data or unified cache line by MVA to PoC.

	h/w를 가리키는 r0 주소를 전달해 cache를 clear 하는 명령
**/
	mcr	p15, 0, r0, c7, c10, 1		@ flush_pte
#endif
	mov	pc, lr
ENDPROC(cpu_v7_set_pte_ext)

	/*
	 * Memory region attributes with SCTLR.TRE=1
	 *
	 *   n = TEX[0],C,B
	 *   TR = PRRR[2n+1:2n]		- memory type
	 *   IR = NMRR[2n+1:2n]		- inner cacheable property
	 *   OR = NMRR[2n+17:2n+16]	- outer cacheable property
	 *
	 *			n	TR	IR	OR
	 *   UNCACHED		000	00
	 *   BUFFERABLE		001	10	00	00
	 *   WRITETHROUGH	010	10	10	10
	 *   WRITEBACK		011	10	11	11
	 *   reserved		110
	 *   WRITEALLOC		111	10	01	01
	 *   DEV_SHARED		100	01
	 *   DEV_NONSHARED	100	01
	 *   DEV_WC		001	10
	 *   DEV_CACHED		011	10
	 *
	 * Other attributes:
	 *
	 *   DS0 = PRRR[16] = 0		- device shareable property
	 *   DS1 = PRRR[17] = 1		- device shareable property
	 *   NS0 = PRRR[18] = 0		- normal shareable property
	 *   NS1 = PRRR[19] = 1		- normal shareable property
	 *   NOS = PRRR[24+n] = 1	- not outer shareable
	 */
.equ	PRRR,	0xff0a81a8
.equ	NMRR,	0x40e040e0

	/*
	 * Macro for setting up the TTBRx and TTBCR registers.
	 * - \ttb0 and \ttb1 updated with the corresponding flags.
	 */
	 /** 20150620
	  * ttbcr 레지스터를 설정. 0을 쓴다.
	  *
	  *  ARM Document
	  *  TTBCR format when using the Short-descriptor translation table format
	  *
	  *  ARM Document
	  *  Selecting between TTBR0 and TTBR1, Short-descriptor translation table format
	  *    N : 0일 때, TTBR1 not used.
	  **/
	.macro	v7_ttb_setup, zero, ttbr0, ttbr1, tmp
	mcr	p15, 0, \zero, c2, c0, 2	@ TTB control register
	ALT_SMP(orr	\ttbr0, \ttbr0, #TTB_FLAGS_SMP)
	ALT_UP(orr	\ttbr0, \ttbr0, #TTB_FLAGS_UP)
	ALT_SMP(orr	\ttbr1, \ttbr1, #TTB_FLAGS_SMP)
	ALT_UP(orr	\ttbr1, \ttbr1, #TTB_FLAGS_UP)
	mcr	p15, 0, \ttbr1, c2, c0, 1	@ load TTB1
	.endm

	__CPUINIT

	/*   AT
	 *  TFR   EV X F   I D LR    S
	 * .EEE ..EE PUI. .T.T 4RVI ZWRS BLDP WCAM
	 * rxxx rrxx xxx0 0101 xxxx xxxx x111 xxxx < forced
	 *    1    0 110       0011 1100 .111 1101 < we want
	 */
	.align	2
	.type	v7_crval, #object
v7_crval:
	crval	clear=0x0120c302, mmuset=0x10c03c7d, ucset=0x00c01c7c

	.previous
