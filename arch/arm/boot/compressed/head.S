/*
 *  linux/arch/arm/boot/compressed/head.S
 *
 *  Copyright (C) 1996-2002 Russell King
 *  Copyright (C) 2004 Hyok S. Choi (MPU support)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>

/*
 * Debugging stuff
 *
 * Note that these macros must not contain any code which is not
 * 100% relocatable.  Any attempt to do so will result in a crash.
 * Please select one of the following when turning on debugging.
 */
#ifdef DEBUG

#if defined(CONFIG_DEBUG_ICEDCC)

#if defined(CONFIG_CPU_V6) || defined(CONFIG_CPU_V6K) || defined(CONFIG_CPU_V7)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c0, c5, 0
		.endm
#elif defined(CONFIG_CPU_XSCALE)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c8, c0, 0
		.endm
#else
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c1, c0, 0
		.endm
#endif

#else

#include <mach/debug-macro.S>

		.macro	writeb,	ch, rb
		senduart \ch, \rb
		.endm

#if defined(CONFIG_ARCH_SA1100)
		.macro	loadsp, rb, tmp
		mov	\rb, #0x80000000	@ physical base address
#ifdef CONFIG_DEBUG_LL_SER3
		add	\rb, \rb, #0x00050000	@ Ser3
#else
		add	\rb, \rb, #0x00010000	@ Ser1
#endif
		.endm
#elif defined(CONFIG_ARCH_S3C24XX)
		.macro loadsp, rb, tmp
		mov	\rb, #0x50000000
		add	\rb, \rb, #0x4000 * CONFIG_S3C_LOWLEVEL_UART_PORT
		.endm
#else
		.macro	loadsp,	rb, tmp
		addruart \rb, \tmp
		.endm
#endif
#endif
#endif

		.macro	kputc,val
		mov	r0, \val
		bl	putc
		.endm

		.macro	kphex,val,len
		mov	r0, \val
		mov	r1, #\len
		bl	phex
		.endm

		.macro	debug_reloc_start
#ifdef DEBUG
		kputc	#'\n'
		kphex	r6, 8		/* processor id */
		kputc	#':'
		kphex	r7, 8		/* architecture id */
#ifdef CONFIG_CPU_CP15
		kputc	#':'
		mrc	p15, 0, r0, c1, c0
		kphex	r0, 8		/* control reg */
#endif
		kputc	#'\n'
		kphex	r5, 8		/* decompressed kernel start */
		kputc	#'-'
		kphex	r9, 8		/* decompressed kernel end  */
		kputc	#'>'
		kphex	r4, 8		/* kernel execution address */
		kputc	#'\n'
#endif
		.endm

		.macro	debug_reloc_end
#ifdef DEBUG
		kphex	r5, 8		/* end of kernel */
		kputc	#'\n'
		mov	r0, r4
		bl	memdump		/* dump 256 bytes at start of kernel */
#endif
		.endm
		/** Iamroot kernel study C team - linux source investigation start! **/
		/** 20120818 할당,실행가능(GNU-Assembler.pdf참조) **/
		.section ".start", #alloc, #execinstr  
/*
 * sort out different calling conventions
 */
		
		/** 20120818 정렬, ARM모드 **/
		.align
		.arm				@ Always enter in ARM state
start:
		/** 20120818 심볼에 대한 형과 속성을 지정**/
		.type	start,#function
		/** 20120818 .rept부터 .endr까지 무의미한 8번 반복함
		http://www.iamroot.org/xe/23099) **/
		.rept	7
		
		mov	r0, r0
		.endr
		/** 20120818 ARM, THUMB 매크로정의됨(arch/arm/include/asm/unified.h) **/
   ARM(		mov	r0, r0		)
		/** 20120818 1f(1 레이블위치의 앞으로 점프)**/
   ARM(		b	1f		)

 THUMB(		adr	r12, BSYM(1f)	)
 THUMB(		bx	r12		)

		.word	0x016f2818		@ Magic numbers to help the loader
		.word	start			@ absolute load/run zImage address
		.word	_edata			@ zImage end address
 THUMB(		.thumb			)
		/** 20120818 부트로더 정보(ID, ATAGS) 저장**/
1:		mov	r7, r1			@ save architecture ID
		mov	r8, r2			@ save atags pointer

#ifndef __ARM_ARCH_2__
		/*
		 * Booting from Angel - need to enter SVC mode and disable
		 * FIQs/IRQs (numeric definitions from angel arm.h source).
		 * We only do this if we were in user mode on entry.
		 */
		/** 20120818 만약 유저모드가 아니면 Not Angel로 진입함**/
		mrs	r2, cpsr		@ get current mode
		tst	r2, #3			@ not user?
		bne	not_angel
		mov	r0, #0x17		@ angel_SWIreason_EnterSVC
		/**
		20120818 Angel이 사용하는 SWI번호(0x123456,0xab)
		http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0040d/ch12s12s01.html
		**/
 ARM(		swi	0x123456	)	@ angel_SWI_ARM
 THUMB(		svc	0xab		)	@ angel_SWI_THUMB
not_angel:
		mrs	r2, cpsr		@ turn off interrupts to
		/**20120818 FIQ/IRQ Disable시킴**/
		orr	r2, r2, #0xc0		@ prevent angel from running
		/**
		  _c : sets the control field mask bit(bit 0)
		  : CPSR/SPSR 레지스터의 [0-7]bit만을 변경한다.
		  _x : sets the extention field mask bit(bit 1)
		  : CPSR/SPSR 레지스터의 [8-15]bit만을 변경한다.
		  _s : sets the status field mask bit(bit 2)
		  : CPSR/SPSR 레지스터의 [16-23]bit만을 변경한다.
		  _f : sets the flags field mask bit(bit 3)
		  : CPSR/SPSR 레지스터의 [24-31]bit만을 변경한다.
		 **/
		msr	cpsr_c, r2
#else
		teqp	pc, #0x0c000003		@ turn off interrupts
#endif

		/*
		 * Note that some cache flushing and other stuff may
		 * be needed here - is there an Angel SWI call for this?
		 */

		/*
		 * some architecture specific code can be inserted
		 * by the linker here, but it should preserve r7, r8, and r9.
		 */

		/** 
		20120818 섹션 지정 방법 (vmlinux.lds)
		  .text : {
		       _start = .;
		       *(.start)
		       *(.text)
		       *(.text.*)
		       *(.fixup)
		       *(.gnu.warning)
		       *(.glue_7t)
		       *(.glue_7)
		     }
		 **/
		.text

		/** 20120818 압축해제된 커널 이미지 어드레스 위치 지정 (Kconfig)
		  AUTO_ZRELADDR
		  bool "Auto calculation of the decompressed kernel image address"
		  depends on !ZBOOT_ROM && !ARCH_U300
		  help
		  ZRELADDR is the physical address where the decompressed kernel
		  image will be placed. If AUTO_ZRELADDR is selected, the address
		  will be determined at run-time by masking the current IP with
		  0xf8000000. This assumes the zImage being placed in the first 128MB
		  from start of memory.
		 **/
#ifdef CONFIG_AUTO_ZRELADDR
		@ determine final kernel image address
		mov	r4, pc
		and	r4, r4, #0xf8000000
		add	r4, r4, #TEXT_OFFSET
#else

		/** 20120818 vexpress보드일때 zreladdr의 값 
		arch/arm/mach-vexpress/Makefile.boot:   zreladdr-y	+= 0x60008000
		**/
		ldr	r4, =zreladdr
#endif

		bl	cache_on

/**
  20120901
  adr 에서 상대주소의 개념.
  objdump 결과.. 
 	155390 00000198 <LC0>:
	155391      198:   00000198 00255c88 00255ca0 00255c88     .....\%..\%..\%.
	155392      1a8:   00255c4d 00255c60 00255c84 00256ca0     M\%.`\%..\%..l%.
	155393      1b8:   e320f000 e320f000                       .. ... .

	155164 restart:    adr r0, LC0
	155165       68:   e28f0f4a    add r0, pc, #296    ; 0x128
	
	adr 은 add 로 변환됨. 
	68 라인이 수행될 때, pc 값은  0x70. 이 되고, 
		0x128과 pc값을 더하면, 0x198이 되어서 LC0의 위치를 지정하게 됨. 
 **/
restart:	adr	r0, LC0
		ldmia	r0, {r1, r2, r3, r6, r10, r11, r12}
/** 20120901
	sp에 L_user_stack_end의 주소를 저장시킴
	(L_user_stack_end는 소스의 끝, stack section에 해당됨)
**/
		ldr	sp, [r0, #28]

		/*
		 * We might be running at a different address.  We need
		 * to fix up various pointers.
		 */
/** 20120901
	r0, r1 에 다른 값이 있을 수 있는지 ???

	20120915
	r0, r1에 있는 값이 overlap을 처리하기 전과 처리한 후에 바뀔수도 있을듯???
	==> 아니었음. 
	r0, r1에 있는 값은 location counter기준의 LC0값과, 실제 메모리에 로딩된 LC0 값임. 
	__bss_start, _edata 등의 실제 메모리 주소 값을 알아내기 위해 delta 를 구해야 함. 
**/
		sub	r0, r0, r1		@ calculate the delta offset
		add	r6, r6, r0		@ _edata
		add	r10, r10, r0		@ inflated kernel size location

		/*
		 * The kernel build system appends the size of the
		 * decompressed kernel at the end of the compressed data
		 * in little-endian form.
		 */
/** 20120908
	arch/arm/Makefile 에서 CONFIG_CPU_BIG_ENDIAN 따라 kernel의 endian이 결정됨.

	... from ARM architecture reference manual
	In ARMv7-A, the mapping of instruction memory is always little-endian.
	In ARMv7-R, instruction endianness can be controlled at the system level,

	여기서는 cpu의 endian과 별개로 little endian으로 저장된 size 값을 레지스터로 읽어온다.
**/
		ldrb	r9, [r10, #0]
		ldrb	lr, [r10, #1]
		orr	r9, r9, lr, lsl #8
		ldrb	lr, [r10, #2]

		ldrb	r10, [r10, #3]
		orr	r9, r9, lr, lsl #16
		orr	r9, r9, r10, lsl #24

#ifndef CONFIG_ZBOOT_ROM
		/* malloc space is above the relocated stack (64k max) */
		add	sp, sp, r0
/** 20120908
	sp는 user stack의 값. 여기에 +64KB한 결과를 r10에 저장.
**/
		add	r10, sp, #0x10000
#else
		/*
		 * With ZBOOT_ROM the bss/stack is non relocatable,
		 * but someone could still run this code from RAM,
		 * in which case our reference is _edata.
		 */
		mov	r10, r6
#endif

		mov	r5, #0			@ init dtb size to 0
/** 20120901
	DTB 블럭은 수행하지 않음. 
**/
#ifdef CONFIG_ARM_APPENDED_DTB
/*
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = final kernel address
 *   r5  = appended dtb size (still unknown)
 *   r6  = _edata
 *   r7  = architecture ID
 *   r8  = atags/device tree pointer
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 *
 * if there are device trees (dtb) appended to zImage, advance r10 so that the
 * dtb data will get relocated along with the kernel if necessary.
 */

		ldr	lr, [r6, #0]
#ifndef __ARMEB__
		ldr	r1, =0xedfe0dd0		@ sig is 0xd00dfeed big endian
#else
		ldr	r1, =0xd00dfeed
#endif
		cmp	lr, r1
		bne	dtb_check_done		@ not found

#ifdef CONFIG_ARM_ATAG_DTB_COMPAT
		/*
		 * OK... Let's do some funky business here.
		 * If we do have a DTB appended to zImage, and we do have
		 * an ATAG list around, we want the later to be translated
		 * and folded into the former here.  To be on the safe side,
		 * let's temporarily move  the stack away into the malloc
		 * area.  No GOT fixup has occurred yet, but none of the
		 * code we're about to call uses any global variable.
		*/
		add	sp, sp, #0x10000
		stmfd	sp!, {r0-r3, ip, lr}
		mov	r0, r8
		mov	r1, r6
		sub	r2, sp, r6
		bl	atags_to_fdt

		/*
		 * If returned value is 1, there is no ATAG at the location
		 * pointed by r8.  Try the typical 0x100 offset from start
		 * of RAM and hope for the best.
		 */
		cmp	r0, #1
		sub	r0, r4, #TEXT_OFFSET
		add	r0, r0, #0x100
		mov	r1, r6
		sub	r2, sp, r6
		bleq	atags_to_fdt

		ldmfd	sp!, {r0-r3, ip, lr}
		sub	sp, sp, #0x10000
#endif

		mov	r8, r6			@ use the appended device tree

		/*
		 * Make sure that the DTB doesn't end up in the final
		 * kernel's .bss area. To do so, we adjust the decompressed
		 * kernel size to compensate if that .bss size is larger
		 * than the relocated code.
		 */
		ldr	r5, =_kernel_bss_size
		adr	r1, wont_overwrite
		sub	r1, r6, r1
		subs	r1, r5, r1
		addhi	r9, r9, r1

		/* Get the dtb's size */
		ldr	r5, [r6, #4]
#ifndef __ARMEB__
		/* convert r5 (dtb size) to little endian */
		eor	r1, r5, r5, ror #16
		bic	r1, r1, #0x00ff0000
		mov	r5, r5, ror #8
		eor	r5, r5, r1, lsr #8
#endif

		/* preserve 64-bit alignment */
		add	r5, r5, #7
		bic	r5, r5, #7

		/* relocate some pointers past the appended dtb */
		add	r6, r6, r5
		add	r10, r10, r5
		add	sp, sp, r5
dtb_check_done:
#endif

/*
 * Check to see if we will overwrite ourselves.
 *   r4  = final kernel address
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 * We basically want:
 *   r4 - 16k page directory >= r10 -> OK
 *   r4 + image length <= address of wont_overwrite -> OK
 */
/** 20120901 
	r4 			: 0x60008000
	r4 - 16k	: 0x60004000
	XIP : eXecute In Place.
**/
		add	r10, r10, #16384
		cmp	r4, r10
/** 20120901 
	r4(final kernel address)보다 r10(현재 이미지의 끝 주소+16k)가 작다면,  overwrite 되지 않음. 
	if(r4 >= r10) 
		b wont_overwrite
**/
		bhs	wont_overwrite
		add	r10, r4, r9
		adr	r9, wont_overwrite
		cmp	r10, r9
/** 20120901
	r10(커널이 풀리는 주소의 마지막 위치)가 r9(wont_overwrite의 주소)보다 작다면, overwrite 되지 않음.
	wont_overwrite 이전의 메모리는 겹쳐도 상관하지 않음. (다시 호출되지 않으므로)
	if(r10 <= r9)
		b wont_overwrite
**/
		bls	wont_overwrite

/*
 * Relocate ourselves past the end of the decompressed kernel.
 *   r6  = _edata
 *   r10 = end of the decompressed kernel
 * Because we always copy ahead, we need to do it from the end and go
 * backward in case the source and destination overlap.
 */
		/*
		 * Bump to the next 256-byte boundary with the size of
		 * the relocation code added. This avoids overwriting
		 * ourself when the offset is small.
		 */
/** 20120901
	(reloc_code_end - restart) 이 0x50  라면, 결과는 0x100
	(reloc_code_end - restart) 이 0x150  라면, 결과는 0x200
	왜 256-byte 단위로 정렬했는지???
**/
		add	r10, r10, #((reloc_code_end - restart + 256) & ~255)
		bic	r10, r10, #255

		/* Get start of code we want to copy and align it down. */
		adr	r5, restart
		bic	r5, r5, #31

/** 20120908
	r9 복사할 크기 주소
	r5 정렬한 restart 주소
	r6 정렬된 _edata 주소
	r10 정렬한 커널해제 위치의 끝주소

	r9 복사할 크기
	8개의 레지스터로 복사하기에 32 바이트로 맞춰줌 (캐시 라인의 효과도...)
**/
		sub	r9, r6, r5		@ size to copy
		add	r9, r9, #31		@ rounded up to a multiple
		bic	r9, r9, #31		@ ... of 32 bytes
/** 20120908
	r6은 이제 복사할(소스) 이미지의 끝 주소 (복사할 크기 + 정렬한 restart 주소)
**/
		add	r6, r9, r5
/** 20120908
	r9은 이제 복사대상(목적지) 주소 (복사할 크기 + 압축해제될 위치의 끝 주소)
	뒤에서부터 복사할 때 사용
**/
		add	r9, r9, r10

/** 20120908
	r6 ~ r5 사이의 이미지를 r9에 저장 (중복되지 않게 뒤에서 앞으로 복사)
**/
1:		ldmdb	r6!, {r0 - r3, r10 - r12, lr}
		cmp	r6, r5
		stmdb	r9!, {r0 - r3, r10 - r12, lr}
		bhi	1b

		/* Preserve offset to relocated code. */
/** 20120908
	r9는 복사된 위치의 시작 주소 (기존 restart에서 정렬한 위치)
	r6는 복사한 위치의 시작 주소
	r6  <- r9-r6 (offset 주소)
**/
		sub	r6, r9, r6

#ifndef CONFIG_ZBOOT_ROM
		/* cache_clean_flush may use the stack, so relocate it */
/** 20120908
	sp 위치를 새롭게 복사된 위치로 보정
**/
		add	sp, sp, r6
#endif

		bl	cache_clean_flush
/** 20120915
	바로 wont_overwrite로 브랜치 하지 않고 복사된 restart 주소로 이동하는 이유는???

	==> offset을 다시 계산해야 하기 때문. restart: label 참고. 
**/
		adr	r0, BSYM(restart)
		add	r0, r0, r6
		mov	pc, r0

wont_overwrite:
/*
 * If delta is zero, we are running at the address we were linked at.
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = kernel execution address
 *   r5  = appended dtb size (0 if not present)
 *   r7  = architecture ID
 *   r8  = atags pointer
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 */
/** 20120915
  r0, r5 값이 같다면, not_relocated로 브랜치한다. 
  이건 뭐지 ??? 
**/
		orrs	r1, r0, r5
		beq	not_relocated

		add	r11, r11, r0
		add	r12, r12, r0

#ifndef CONFIG_ZBOOT_ROM
		/*
		 * If we're running fully PIC === CONFIG_ZBOOT_ROM = n,
		 * we need to fix up pointers into the BSS region.
		 * Note that the stack pointer has already been fixed up.
		 */
		add	r2, r2, r0
		add	r3, r3, r0

		/*
		 * Relocate all entries in the GOT table.
		 * Bump bss entries to _edata + dtb size
		 */
/** 20120915
	GOT의 각 entry의 값에 offset을 더해 실제 메모리 주소를 넣어준다. 
	여기서의 GOT는 compressed/* 에 해당하는 GOT 이다. 
	bss start ~ end 에 해당하는 공간에는 DTB size를 더해서 넣어준다. 왜???
**/
1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		add	r1, r1, r0		@ This fixes up C references
		cmp	r1, r2			@ if entry >= bss_start &&
		cmphs	r3, r1			@       bss_end > entry
		addhi	r1, r1, r5		@    entry += dtb size
		str	r1, [r11], #4		@ next entry
		cmp	r11, r12
		blo	1b

		/* bump our bss pointers too */
		add	r2, r2, r5
		add	r3, r3, r5

#else

		/*
		 * Relocate entries in the GOT table.  We only relocate
		 * the entries that are outside the (relocated) BSS region.
		 */
1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		cmp	r1, r2			@ entry < bss_start ||
		cmphs	r3, r1			@ _end < entry
		addlo	r1, r1, r0		@ table.  This fixes up the
		str	r1, [r11], #4		@ C references.
		cmp	r11, r12
		blo	1b
#endif

/** 20120915
	BSS start ~ BSS end 를 0으로 채워준다. 
**/
not_relocated:	mov	r0, #0
1:		str	r0, [r2], #4		@ clear bss
		str	r0, [r2], #4
		str	r0, [r2], #4
		str	r0, [r2], #4
		cmp	r2, r3
		blo	1b

/*
 * The C runtime environment should now be setup sufficiently.
 * Set up some pointers, and start decompressing.
 *   r4  = kernel execution address
 *   r7  = architecture ID
 *   r8  = atags pointer
 */
		mov	r0, r4
		mov	r1, sp			@ malloc space above stack
		add	r2, sp, #0x10000	@ 64k max
		mov	r3, r7
/** 20120915
	r0 : output start 
	r1 : free_mem_ptr_p
	r2 : free_mem_ptr_end_p
	r3 : arch_id
**/
		bl	decompress_kernel
		bl	cache_clean_flush
		bl	cache_off
		mov	r0, #0			@ must be zero
		mov	r1, r7			@ restore architecture number
		mov	r2, r8		 	@ restore atags pointer
/** 20120915
	arch/arm/kernel/head.S 로 go go
**/
 ARM(		mov	pc, r4	)		@ call kernel
 THUMB(		bx	r4	)		@ entry point is always ARM

/**
 **/
		.align	2
		.type	LC0, #object
LC0:		.word	LC0			@ r1
		.word	__bss_start		@ r2
		.word	_end			@ r3
		.word	_edata			@ r6
/** 20120901 
	input_data_end : 에 압축된 binary image address. 
	inflated라는 표현은 piggy 가 끼워졌다는 뜻인듯. 
**/
		.word	input_data_end - 4	@ r10 (inflated size location)
		.word	_got_start		@ r11
		.word	_got_end		@ ip
		.word	.L_user_stack_end	@ sp
		.size	LC0, . - LC0

#ifdef CONFIG_ARCH_RPC
		.globl	params
params:		ldr	r0, =0x10000100		@ params_phys for RPC
		mov	pc, lr
		.ltorg
		.align
#endif

/*
 * Turn on the cache.  We need to setup some page tables so that we
 * can have both the I and D caches on.
 *
 * We place the page tables 16k down from the kernel execution address,
 * and we hope that nothing else is using it.  If we're using it, we
 * will go pop!
 *
 * On entry,
 *  r4 = kernel execution address
 *  r7 = architecture number
 *  r8 = atags pointer
 * On exit,
 *  r0, r1, r2, r3, r9, r10, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		/** 20120818 캐쉬라인을 고려하여 32바이트로 정렬 **/
		.align	5
cache_on:	mov	r3, #8			@ cache_on function
		b	call_cache_fn

/*
 * Initialize the highest priority protection region, PR7
 * to cover all 32bit address and cacheable and bufferable.
 */
__armv4_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting
		mcr 	p15, 0, r0, c6, c7, 1

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ D-cache on
		mcr	p15, 0, r0, c2, c0, 1	@ I-cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 1	@ I-access permission
		mcr	p15, 0, r0, c5, c0, 0	@ D-access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ ...I .... ..D. WC.M
		orr	r0, r0, #0x002d		@ .... .... ..1. 11.1
		orr	r0, r0, #0x1000		@ ...1 .... .... ....

		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mov	pc, lr

__armv3_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 0	@ access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		/*
		 * ?? ARMv3 MMU does not allow reading the control register,
		 * does this really work on ARMv3 MPU?
		 */
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ .... .... .... WC.M
		orr	r0, r0, #0x000d		@ .... .... .... 11.1
		/* ?? this overwrites the value constructed above? */
		mov	r0, #0
		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		/* ?? invalidate for the second time? */
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
#define CB_BITS 0x08
#else
#define CB_BITS 0x0c
#endif

/**
	20120825 __setup_mmu
	mmu enable하기 전에 page directory를 구성하는 부분.
	램 영역(압축 해제된 커널이 실행될 영역포함)에 대한 cache와 write buffer 속성 설정 (256MB 사이)

	label 1 ~ b 1b 루프 다음의 코드는 현재 코드가 flash에서 실행될 경우 속성을 동일하게 처리하는 부분

	 r4에는 zreladdr 값이 저장되어 있음 (_start)
	AUTO_ZRELADDR
	bool "Auto calculation of the decompressed kernel image address"

	r3에 압축 해제된 커널이 위치할 16k 아래부터 page directory를 저장한다 
	  -- 페이지 테이블이 시작할 위치
**/
__setup_mmu:	sub	r3, r4, #16384		@ Page directory size
/**
	20120825 16kb alignment를 맞춰주는 작업
	bic의 immed 표현은 8비트까지 가능하기 때문에 두 번의 bic로 나눠 수행
**/
		bic	r3, r3, #0xff		@ Align the pointer
		bic	r3, r3, #0x3f00
/*
 * Initialise the page tables, turning on the cacheable and bufferable
 * bits for the RAM area only.
 */
/**
	20120825
	Cortex-A Programmer Guide
		Figure 8-3 L1 page table entry format 참고

	r3->r0에 복사 vexpress는0x60004000
 **/
		mov	r0, r3
/** 20120825 압축 해제될 위치 하위 256KB까지의 주소를 날린다 ... 왜 18bit인지??? 약속이 있는지??? **/
		mov	r9, r0, lsr #18
		mov	r9, r9, lsl #18		@ start of RAM
/** 20120825 RAM 시작주소 + 256MB (최근의 플랫폼은 256MB를 기본으로 한다) **/
		add	r10, r9, #0x10000000	@ a reasonable RAM size
/** 20120825 r1: 0xc12. XN|U + section mapping ??? **/
		mov	r1, #0x12		@ XN|U + section mapping
		orr	r1, r1, #3 << 10	@ AP=11
/** 20120825 r2는 page directory의 끝 주소 **/
		add	r2, r3, #16384
/** 20120825 r1 0xc12로, r9 0x60000000부터 시작 */
1:		cmp	r1, r9			@ if virt > start of RAM
		cmphs	r10, r1			@   && end of RAM > virt
/**
	20120825
XN
The Execute-never bit. Determines whether the processor can execute software from the addressed region, see Execute-never restrictions on instruction fetching on page B3-1350.
This bit is not present in a Page table entry.   ... from ARM Architecture Reference Manual
**/
		bic	r1, r1, #0x1c		@ clear XN|U + C + B
		orrlo	r1, r1, #0x10		@ Set XN|U for non-RAM
/**
	20120825  r6 <- CB_BITS | 0x02
	  현재 target으로 삼은 vexpress 에서는 define 되어 있지 않음으로 0x0c | 0x02
**/
		orrhs	r1, r1, r6		@ set RAM section settings
/** 20120825 r0는 page directory의 시작 주소 **/
		str	r1, [r0], #4		@ 1:1 mapping
		add	r1, r1, #1048576	/** 20120528 r1은 1MB씩 증가 **/
/** 20120825 page directory의 끝까지 도달했는지 검사 **/
		teq	r0, r2
		bne	1b
/** 20120902 
	설정 이후, page table 
		0x70000000 		-	0x70000000

		0x60008000 		-	0x60008000	|---------------------|
			.			|	0x600047FC	|---------------------|		0xFFF00C12
			.			|					...
		 	.			|	0x60004700	|---------------------|		0x70000C12
			.			|	0x600046FC	|---------------------|		0x6FF00C0E
		 page directory	|					...
			.			|	0x60004600	|---------------------|		0x60000C0E
			.			|	0x600045FC	|---------------------|		0x5FC00C12
			.			|					...
			.			|	0x6000400C	|---------------------|		0x00300C12
			.			|	0x60004008	|---------------------|		0x00200C12
			.			|	0x60004004	|---------------------|		0x00100C12
		0x60004000 		-	0x60004000	|---------------------|		0x00000C12
**/
/*
 * If ever we are running from Flash, then we surely want the cache
 * to be enabled also for our execution instance...  We map 2MB of it
 * so there is no map overlap problem for up to 1 MB compressed kernel.
 * If the execution is in RAM then we would only be duplicating the above.
 */
/** 20120825 r1 <- r6(CB_BITS|0x02) | B **/
		orr	r1, r6, #0x04		@ ensure B is set for this
/** 20120825 r1 <- r1 + AP(11) **/
		orr	r1, r1, #3 << 10
		mov	r2, pc
/** 20120825 r2(pc)에서 1MB 아래를 날린다 - page table에 쓸 base address의 index를 구함  **/
		mov	r2, r2, lsr #20
/**
	20120825 r1 <- r2(base address index) * 1MB + r1(속성값)
	결국 r1에 page directory entry에 넣어줄 값 넣어둠
 **/
		orr	r1, r1, r2, lsl #20
/**
	20120825
	r3 page directory start address. vexpress는 (DRAM)  0x60004000
	r2 base address index
	r0 <- r3 + r2 * 4(entry의 크기)
		r0는 page directory entry의 절대주소 (pc가 위치한 page directory entry 위치)
 **/
		add	r0, r3, r2, lsl #2
		str	r1, [r0], #4
/** 20120825 그 다음 1MB에 대해서도 동일한 방법으로(base address는 1MB 증가) entry를 씀 **/
		add	r1, r1, #1048576
		str	r1, [r0]
		mov	pc, lr
/** 20120825
  일단 debugger 등에서 사용될 정보를 넣어주는 부분. assembly에서의 역할은???

#define ENDPROC(name) \
  .type name, @function; \
  END(name)

#define END(name) \
  .size name, .-name
 **/
ENDPROC(__setup_mmu)

__arm926ejs_mmu_cache_on:
#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
		mov	r0, #4			@ put dcache in WT mode
		mcr	p15, 7, r0, c15, c0, 0
#endif

__armv4_mmu_cache_on:
		mov	r12, lr
#ifdef CONFIG_MMU
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x0030
#ifdef CONFIG_CPU_ENDIAN_BE8
		orr	r0, r0, #1 << 25	@ big-endian page tables
#endif
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
		mov	pc, r12

__armv7_mmu_cache_on:
/** 20120825 r12에 복귀주소 저장 **/
		mov	r12, lr
#ifdef CONFIG_MMU
/**
	20120825 memory model feature register
	MMFR0 레지스터에서 c0, c1 4 값을 읽어온다
**/
		mrc	p15, 0, r11, c0, c1, 4	@ read ID_MMFR0
/**
	20120825 virtual memory system architecture
	VMSA support 확인
 **/
		tst	r11, #0xf		@ VMSA
/**
	20120825  CB_BITS 값이 2를 bit or해 __setup_mmu에서 사용할 r6 값을 만들어 준다.
	  CB_BITS는 CONFIG_CPU_DCACHE_WRITETHROUGH 여부에 따라 값이 달라진다.
	  현재 target으로 삼은 vexpress 에서는 define 되어 있지 않음으로 0x0c | 0x02
	 
	#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
	#define CB_BITS 0x08
	#else
	#define CB_BITS 0x0c
	#endif
**/
		movne	r6, #CB_BITS | 0x02	@ !XN
/**
	20120825 마찬가지로 ne이므로 __setup_mmu로 bl
**/
		blne	__setup_mmu
		mov	r0, #0
/**
	20120825 mmu enable 전의 준비 과정
	p15, c7, c10, 4는 Drain Write Buffer (DWB) 였으나 ARMv6부터 Data Synchronization Barrier(DSB)로 이름이 변경됨
 **/
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
/** 20120901	r11의 하위 네 비트가 0인지 검사 
 	0인 경우, VMSA를 지원하지 않음. 
**/
		tst	r11, #0xf		@ VMSA
/** 20120901	VMSA를 지원하는 경우, I, D TLB를 flush 한다 **/
		mcrne	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
/** 20120901	
  SCTLR, System Control Register, VMSA
	RR[bit 14] : round robin replacement. 
	I[bit 12] : I-cache enable
	CP15BEN[bit 5] : Barrier enable, CP15 DMB, DSB, and ISB barrier
	C[bit 2] : cache enable 
		0: Data and unified caches disabled.
		1: Data and unified caches enabled.
**/
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x003c		@ write buffer
#ifdef CONFIG_MMU
#ifdef CONFIG_CPU_ENDIAN_BE8
		orr	r0, r0, #1 << 25	@ big-endian page tables
#endif
/** 20120901	VMSA를 지원하는 경우, 
  				mmu enable 
**/
		orrne	r0, r0, #1		@ MMU enabled
		movne	r1, #0xfffffffd		@ domain 0 = client
/**	20120901 r3: page directory address 0x60004000 
 TTBR0, Translation Table Base Register 0, VMSA
 TTBR0 에 page table pointer(page directory address)를 넣는다. 
 **/
		mcrne	p15, 0, r3, c2, c0, 0	@ load page table pointer
/** 20120901
  DACR, Domain Access Control Register, VMSA
	DDI0406C, p1541
	setup_mmu에서 page directory 설정 시에,	
		모든 page에 대해서 domain을 0으로 설정시킴.
		모든 page에 대해서 AP를 11(Full access)로 설정시킴.
	domain 15 ~ 1 까지는 DACR:Manager : AP bit 에 상관없이 접근 
	domain 0 : Client : Page table entry 에 설정된 AP 값을 따른다. 
 **/
		mcrne	p15, 0, r1, c3, c0, 0	@ load domain access control
#endif
/** 20120901
  CP15ISB, CP15 Instruction Synchronization Barrier operation, VMSA
  ISB Operation. r0는 의미 없음.
  **/
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
/** 20120901
  실제 컨트롤 레지스터에 세팅! 두둥.
  **/
		mcr	p15, 0, r0, c1, c0, 0	@ load control register
/** 20120901 
  왜 다시 읽어오는지... 알 수 없음. 
  **/
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back
		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
/** 20120901
  bl cache_on의 다음 주소로 리턴.
 **/
		mov	pc, r12

__fa526_cache_on:
		mov	r12, lr
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7, 0	@ Invalidate whole cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x1000		@ I-cache enable
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mov	pc, r12

__common_mmu_cache_on:
#ifndef CONFIG_THUMB2_KERNEL
#ifndef DEBUG
		orr	r0, r0, #0x000d		@ Write buffer, mmu
#endif
		mov	r1, #-1
		mcr	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcr	p15, 0, r1, c3, c0, 0	@ load domain access control
		b	1f
		.align	5			@ cache line aligned
1:		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back to
		sub	pc, lr, r0, lsr #32	@ properly flush pipeline
#endif

#define PROC_ENTRY_SIZE (4*5)

/*
 * Here follow the relocatable cache support functions for the
 * various processors.  This is a generic hook for locating an
 * entry and jumping to an instruction at the specified offset
 * from the start of the block.  Please note this is all position
 * independent code.
 *
 *  r1  = corrupted
 *  r2  = corrupted
 *  r3  = block offset
 *  r9  = corrupted
 *  r12 = corrupted
 */

/**
	20120825 코프로세서로부터 processor ID를 가져오고
	proc_types 을 순회하며 processor ID와 비교해 해당하는 엔트리를 찾는다.
	r12에 해당 엔트리 주소가 있고, r3에 넣어준 offset 값의 함수를 가져온다.
	함수 호출시 r3에 호출을 원하는 함수를 위한 offset을 미리 넣어줘야 한다.
		8 => cache on
		12 => cache off
		16 => cache flush

	W매크로는 thumb 모드에서 인스트럭션 뒤에 .w를 붙여준다.
		.word	0x000f0000		@ new CPU Id ... value
		.word	0x000f0000		@ ... mask
		W(b)	__armv7_mmu_cache_on 	@ #define W(instr)	instr
		W(b)	__armv7_mmu_cache_off
		W(b)	__armv7_mmu_cache_flush
	
	20120901
	adr : ADR은 즉치값을 pc 값에 더하고 결과를 대상 레지스터에 기록합니다.
	proc_types: label이 갖는 값은 상대주소이므로, 
		실행시에 proc_types의 절대주소를 구하기 위해서 pc를 더해주어야 함. 
 **/
call_cache_fn:	adr	r12, proc_types
#ifdef CONFIG_CPU_CP15
		mrc	p15, 0, r9, c0, c0	@ get processor ID
#else
		ldr	r9, =CONFIG_PROCESSOR_ID
#endif
1:		ldr	r1, [r12, #0]		@ get value
		ldr	r2, [r12, #4]		@ get mask
		eor	r1, r1, r9		@ (real ^ match)
		tst	r1, r2			@       & mask
 ARM(		addeq	pc, r12, r3		) @ call cache function
 THUMB(		addeq	r12, r3			)
 THUMB(		moveq	pc, r12			) @ call cache function
		add	r12, r12, #PROC_ENTRY_SIZE
		b	1b

/*
 * Table for cache operations.  This is basically:
 *   - CPU ID match
 *   - CPU ID mask
 *   - 'cache on' method instruction
 *   - 'cache off' method instruction
 *   - 'cache flush' method instruction
 *
 * We match an entry using: ((real_id ^ match) & mask) == 0
 *
 * Writethrough caches generally only need 'on' and 'off'
 * methods.  Writeback caches _must_ have the flush method
 * defined.
 */
		.align	2
	/** 20120818 ARM CPU Type (Data Object)**/
		.type	proc_types,#object
proc_types:
		.word	0x00000000		@ old ARM ID
		.word	0x0000f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007000		@ ARM7/710
		.word	0xfff8fe00
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41807200		@ ARM720T (writethrough)
		.word	0xffffff00
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007400		@ ARM74x
		.word	0xff00ff00
		W(b)	__armv3_mpu_cache_on
		W(b)	__armv3_mpu_cache_off
		W(b)	__armv3_mpu_cache_flush
		
		.word	0x41009400		@ ARM94x
		.word	0xff00ff00
		W(b)	__armv4_mpu_cache_on
		W(b)	__armv4_mpu_cache_off
		W(b)	__armv4_mpu_cache_flush

		.word	0x41069260		@ ARM926EJ-S (v5TEJ)
		.word	0xff0ffff0
		W(b)	__arm926ejs_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x00007000		@ ARM7 IDs
		.word	0x0000f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		@ Everything from here on will be the new ID system.

		.word	0x4401a100		@ sa110 / sa1100
		.word	0xffffffe0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x6901b110		@ sa1110
		.word	0xfffffff0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56056900
		.word	0xffffff00		@ PXA9xx
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56158000		@ PXA168
		.word	0xfffff000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x56050000		@ Feroceon
		.word	0xff0f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

#ifdef CONFIG_CPU_FEROCEON_OLD_ID
		/* this conflicts with the standard ARMv5TE entry */
		.long	0x41009260		@ Old Feroceon
		.long	0xff00fff0
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv5tej_mmu_cache_flush
#endif

		.word	0x66015261		@ FA526
		.word	0xff01fff1
		W(b)	__fa526_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__fa526_cache_flush

		@ These match on the architecture ID

		.word	0x00020000		@ ARMv4T
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00050000		@ ARMv5TE
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00060000		@ ARMv5TEJ
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x0007b000		@ ARMv6
		.word	0x000ff000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv6_mmu_cache_flush

		.word	0x000f0000		@ new CPU Id
		.word	0x000f0000
		W(b)	__armv7_mmu_cache_on
		W(b)	__armv7_mmu_cache_off
		W(b)	__armv7_mmu_cache_flush

		.word	0			@ unrecognised type
		.word	0
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.size	proc_types, . - proc_types

		/*
		 * If you get a "non-constant expression in ".if" statement"
		 * error from the assembler on this line, check that you have
		 * not accidentally written a "b" instruction where you should
		 * have written W(b).
		 */
		.if (. - proc_types) % PROC_ENTRY_SIZE != 0
		.error "The size of one or more proc_types entries is wrong."
		.endif

/*
 * Turn off the Cache and MMU.  ARMv3 does not support
 * reading the control register, but ARMv4 does.
 *
 * On exit,
 *  r0, r1, r2, r3, r9, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		.align	5
cache_off:	mov	r3, #12			@ cache_off function
		b	call_cache_fn

__armv4_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c6, 0	@ flush D-Cache
		mcr	p15, 0, r0, c7, c5, 0	@ flush I-Cache
		mov	pc, lr

__armv3_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0, 0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

__armv4_mmu_cache_off:
#ifdef CONFIG_MMU
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7	@ invalidate whole cache v4
		mcr	p15, 0, r0, c8, c7	@ invalidate whole TLB v4
#endif
		mov	pc, lr

__armv7_mmu_cache_off:
/** 20120915
	SCTLR, System Control Register, VMSA
	cache, mmu에 해당하는 비트를 클리어. 
**/
		mrc	p15, 0, r0, c1, c0
#ifdef CONFIG_MMU
		bic	r0, r0, #0x000d
#else
		bic	r0, r0, #0x000c
#endif
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r12, lr
/** 20120915
	cache 를 끄고 나서도 flush가 가능하구나..
**/
		bl	__armv7_mmu_cache_flush
		mov	r0, #0
#ifdef CONFIG_MMU
		mcr	p15, 0, r0, c8, c7, 0	@ invalidate whole TLB
#endif
/** 20120915
	BPIALL, Branch Predictor Invalidate All, VMS
	BTC : Branch Target (Address) Cache 
	BTB : Branch Target Buffer 라면.. 
	BTC, BTB는 같은 대상을 지칭하는듯.
**/
		mcr	p15, 0, r0, c7, c5, 6	@ invalidate BTC
		mcr	p15, 0, r0, c7, c10, 4	@ DSB
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

/*
 * Clean and flush the cache to maintain consistency.
 *
 * On exit,
 *  r1, r2, r3, r9, r10, r11, r12 corrupted
 * This routine must preserve:
 *  r4, r6, r7, r8
 */
		.align	5
cache_clean_flush:
		mov	r3, #16
		b	call_cache_fn

__armv4_mpu_cache_flush:
		mov	r2, #1
		mov	r3, #0
		mcr	p15, 0, ip, c7, c6, 0	@ invalidate D cache
		mov	r1, #7 << 5		@ 8 segments
1:		orr	r3, r1, #63 << 26	@ 64 entries
2:		mcr	p15, 0, r3, c7, c14, 2	@ clean & invalidate D index
		subs	r3, r3, #1 << 26
		bcs	2b			@ entries 63 to 0
		subs 	r1, r1, #1 << 5
		bcs	1b			@ segments 7 to 0

		teq	r2, #0
		mcrne	p15, 0, ip, c7, c5, 0	@ invalidate I cache
		mcr	p15, 0, ip, c7, c10, 4	@ drain WB
		mov	pc, lr
		
__fa526_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean and invalidate D cache
		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv6_mmu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean+invalidate D
		mcr	p15, 0, r1, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r1, c7, c15, 0	@ clean+invalidate unified
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv7_mmu_cache_flush:
		mrc	p15, 0, r10, c0, c1, 5	@ read ID_MMFR1
/** 20120908
		0b0000 None supported. This is the required setting for ARMv7, because ARMv7 requires a
		       hierarchical cache implementation.
**/
		tst	r10, #0xf << 16		@ hierarchical cache (ARMv7)
		mov	r10, #0
		beq	hierarchical
		mcr	p15, 0, r10, c7, c14, 0	@ clean+invalidate D
		b	iflush
hierarchical:
/** 20120908
		This operation performs the 'full system barrier' performed by the DMB instruction.
		ARM Architecture Reference Manual A8.8.43
**/
		mcr	p15, 0, r10, c7, c10, 5	@ DMB
		stmfd	sp!, {r0-r7, r9-r11}
		mrc	p15, 1, r0, c0, c0, 1	@ read clidr
/** 20120908
	LoC, Level of coherence
		This field defines the last level of cache that must be cleaned or invalidated
		when cleaning or invalidating to the point of coherency.
**/
		ands	r3, r0, #0x7000000	@ extract loc from clidr
		mov	r3, r3, lsr #23		@ left align loc bit field
		beq	finished		@ if loc is 0, then no need to clean
		mov	r10, #0			@ start clean at cache level 0
/** 20120915
	loop1,loop2,loop3
	loop1 : 캐쉬 레벨을 증가
	loop2 : 인덱스를 감소
	loop3 : Way를 감소

	loop1{
		cache level 순회하면서 d-cache인경우 loop2,loop3를 순회
		loop2{
			way를 감소시키면서 적용
			loop3{
				index를 증가시키면서 적용
				data캐쉬를 invalidate
				}
		}
	}

**/

loop1:
		add	r2, r10, r10, lsr #1	@ work out 3x current cache level
		mov	r1, r0, lsr r2		@ extract cache type bits from clidr
		and	r1, r1, #7		@ mask of the bits for current cache only
		cmp	r1, #2			@ see what cache we have at this level
		blt	skip			@ skip if no cache, or just i-cache
/** 20120908
	CSSELR, Cache Size Selection Register, VMSA
		-> Cache Size 채우기

	CCSIDR, Cache Size ID Registers, VMSA (p.1512)
		The implementation includes one CCSIDR for each cache that it can access. CSSELR selects which Cache Size ID Register is accessible.

	-> CSSELR 에서 선택된 캐시 레벨에 해당하는 캐시 정보가 CCSIDR에 채워짐.
	   다시 정보를 r1에 읽음
**/
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
		mcr	p15, 0, r10, c7, c5, 4	@ isb to sych the new cssr&csidr
		mrc	p15, 1, r1, c0, c0, 0	@ read the new csidr
/** 20120908
	r2 <- cache line 크기 (log2 (n) - 2)
**/
		and	r2, r1, #7		@ extract the length of the cache lines
		add	r2, r2, #4		@ add 4 (line length offset)
		ldr	r4, =0x3ff
/** 20120908
	읽어온 CCSIDR에서 Associativity 추출
**/
		ands	r4, r4, r1, lsr #3	@ find maximum number on the way size
/** 20120908
	r5 <- r4에서 선행 0의 개수
**/
		clz	r5, r4			@ find bit position of way size increment
		ldr	r7, =0x7fff
/** 20120908
	r7 <- 읽어온 CCSIDR에서 NumSets을 추출
**/
		ands	r7, r7, r1, lsr #13	@ extract max number of the index size
loop2:
/** 20120908
	r9 <- r4 ( 읽어온 Associativity)
**/
		mov	r9, r4			@ create working copy of max way size
loop3:
 ARM(		orr	r11, r10, r9, lsl r5	) @ factor way and cache number into r11
 ARM(		orr	r11, r11, r7, lsl r2	) @ factor index number into r11
 THUMB(		lsl	r6, r9, r5		)
 THUMB(		orr	r11, r10, r6		) @ factor way and cache number into r11
 THUMB(		lsl	r6, r7, r2		)
 THUMB(		orr	r11, r11, r6		) @ factor index number into r11
/** 20120908
	DCCISW, Data Cache Clean and Invalidate by Set/Way, VMSA only
		Data formats for the cache and branch predictor operations
	(p.1724)

	Log2(ASSOCIATIVITY) <- SBZ에 의해 0으로 구분해서 Way와 Set을 추출하는듯???
**/
		mcr	p15, 0, r11, c7, c14, 2	@ clean & invalidate by set/way
		subs	r9, r9, #1		@ decrement the way
		bge	loop3
		subs	r7, r7, #1		@ decrement the index
		bge	loop2
skip:
		add	r10, r10, #2		@ increment cache number
		cmp	r3, r10
		bgt	loop1
finished:

/** 20120915 
	cache clean & invalidate에서 사용한 r0-r11을 다시 이전 상태로 복구
	루프돌면서 증가시킨 캐쉬 레벨을 level 0으로 되돌림
**/
		ldmfd	sp!, {r0-r7, r9-r11}
		mov	r10, #0			@ swith back to cache level 0
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr

/** 20120915

CP15DSB, Data Synchronization Barrier operation
In ARMv7, the DSB instruction performs a Data Synchronization Barrier, see DSB on page A8-378.
The deprecated CP15 c7 encoding for a Data Synchronization Barrier is an MCR instruction with <opc1> set to 0, <CRn>
set to c7, <CRm> set to c10, and <opc2> set to 4. This operation performs the full system barrier performed by the DSB
instruction.

1. 캐쉬레벨이 0인 상태임.
2. Write Buffer에 있는 내용에 대해 Barrier Operation을 적용시킴
3. cache level 0으로 설정한 부분과 DSB 설정 부분과의 상관관계에 대하여(?)
	=> This operation performs the full system barrier performed by the DSB instruction.

=> DSB Operation - DDI0406 p.378참고

**/

iflush:
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
/** 20120915
	Cache and branch predictor maintenance operations, VMSA
	ICIALLU (c c7 0 c5 0) 32-bit (Write Only) Instruction cache invalidate all PoU(to Point of Unification)
	arm v7에서 iflush 부분에서 DSB 이후 invalidate I+BTB이후 다시 DSB를 실행하는 이유???

**/
		mcr	p15, 0, r10, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
		mcr	p15, 0, r10, c7, c5, 4	@ ISB
		mov	pc, lr


__armv5tej_mmu_cache_flush:
1:		mrc	p15, 0, r15, c7, c14, 3	@ test,clean,invalidate D cache
		bne	1b
		mcr	p15, 0, r0, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv4_mmu_cache_flush:
		mov	r2, #64*1024		@ default: 32K dcache size (*2)
		mov	r11, #32		@ default: 32 byte line size
		mrc	p15, 0, r3, c0, c0, 1	@ read cache type
		teq	r3, r9			@ cache ID register present?
		beq	no_cache_id
		mov	r1, r3, lsr #18
		and	r1, r1, #7
		mov	r2, #1024
		mov	r2, r2, lsl r1		@ base dcache size *2
		tst	r3, #1 << 14		@ test M bit
		addne	r2, r2, r2, lsr #1	@ +1/2 size if M == 1
		mov	r3, r3, lsr #12
		and	r3, r3, #3
		mov	r11, #8
		mov	r11, r11, lsl r3	@ cache line size in bytes
no_cache_id:
		mov	r1, pc
		bic	r1, r1, #63		@ align to longest cache line
		add	r2, r1, r2
1:
 ARM(		ldr	r3, [r1], r11		) @ s/w flush D cache
 THUMB(		ldr     r3, [r1]		) @ s/w flush D cache
 THUMB(		add     r1, r1, r11		)
		teq	r1, r2
		bne	1b

		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c6, 0	@ flush D cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv3_mmu_cache_flush:
__armv3_mpu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

/*
 * Various debugging routines for printing hex characters and
 * memory, which again must be relocatable.
 */
#ifdef DEBUG
		.align	2
		.type	phexbuf,#object
phexbuf:	.space	12
		.size	phexbuf, . - phexbuf

@ phex corrupts {r0, r1, r2, r3}
phex:		adr	r3, phexbuf
		mov	r2, #0
		strb	r2, [r3, r1]
1:		subs	r1, r1, #1
		movmi	r0, r3
		bmi	puts
		and	r2, r0, #15
		mov	r0, r0, lsr #4
		cmp	r2, #10
		addge	r2, r2, #7
		add	r2, r2, #'0'
		strb	r2, [r3, r1]
		b	1b

@ puts corrupts {r0, r1, r2, r3}
puts:		loadsp	r3, r1
1:		ldrb	r2, [r0], #1
		teq	r2, #0
		moveq	pc, lr
2:		writeb	r2, r3
		mov	r1, #0x00020000
3:		subs	r1, r1, #1
		bne	3b
		teq	r2, #'\n'
		moveq	r2, #'\r'
		beq	2b
		teq	r0, #0
		bne	1b
		mov	pc, lr
@ putc corrupts {r0, r1, r2, r3}
putc:
		mov	r2, r0
		mov	r0, #0
		loadsp	r3, r1
		b	2b

@ memdump corrupts {r0, r1, r2, r3, r10, r11, r12, lr}
memdump:	mov	r12, r0
		mov	r10, lr
		mov	r11, #0
2:		mov	r0, r11, lsl #2
		add	r0, r0, r12
		mov	r1, #8
		bl	phex
		mov	r0, #':'
		bl	putc
1:		mov	r0, #' '
		bl	putc
		ldr	r0, [r12, r11, lsl #2]
		mov	r1, #8
		bl	phex
		and	r0, r11, #7
		teq	r0, #3
		moveq	r0, #' '
		bleq	putc
		and	r0, r11, #7
		add	r11, r11, #1
		teq	r0, #7
		bne	1b
		mov	r0, #'\n'
		bl	putc
		cmp	r11, #64
		blt	2b
		mov	pc, r10
#endif

/**	20120901
	.ltorg:
		http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0473c/Bgbccbdi.html
		http://sourceware.org/binutils/docs-2.22/as/ARM-Directives.html#ARM-Directives
	.ltorg가 없더라도 section 의 마지막이므로 literal pool이 자동으로 생길텐데..,
		여기에 왜 넣었을까???
		==> reloc_code_end label로 code section의 마지막(literal pool을 포함한)을 명시해주기 위한 목적임.
**/
		.ltorg
reloc_code_end:

/** 20120901

. space
	This directive emits size bytes, each of value fill. Both size and fill are absolute expressions. If the comma and fill are omitted, fill is assumed to be zero.
**/
		.align
		.section ".stack", "aw", %nobits
.L_user_stack:	.space	4096
.L_user_stack_end:
