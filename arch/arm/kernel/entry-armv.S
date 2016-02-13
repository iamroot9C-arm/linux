/*
 *  linux/arch/arm/kernel/entry-armv.S
 *
 *  Copyright (C) 1996,1997,1998 Russell King.
 *  ARM700 fix by Matthew Godbolt (linux-user@willothewisp.demon.co.uk)
 *  nommu support by Hyok S. Choi (hyok.choi@samsung.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Low-level vector interface routines
 *
 *  Note:  there is a StrongARM bug in the STMIA rn, {regs}^ instruction
 *  that causes it to save wrong values...  Be aware!
 */

#include <asm/assembler.h>
#include <asm/memory.h>
#include <asm/glue-df.h>
#include <asm/glue-pf.h>
#include <asm/vfpmacros.h>
#ifndef CONFIG_MULTI_IRQ_HANDLER
#include <mach/entry-macro.S>
#endif
#include <asm/thread_notify.h>
#include <asm/unwind.h>
#include <asm/unistd.h>
#include <asm/tls.h>
#include <asm/system_info.h>

#include "entry-header.S"
#include <asm/entry-macro-multi.S>

/*
 * Interrupt handling.
 */
	.macro	irq_handler
#ifdef CONFIG_MULTI_IRQ_HANDLER
	/** 20140816    
	 * handle_arch_irq에 저장된 irq handler의 주소를 가져와 실행한다.
	 * 핸들러에 stack의 시작 주소를 인자로 전달한다.
	 * 복귀주소는 이 매크로를 호출한 다음 명령의 주소이다.
	 **/
	ldr	r1, =handle_arch_irq
	mov	r0, sp
	adr	lr, BSYM(9997f)
	ldr	pc, [r1]
#else
	arch_irq_handler_default
#endif
9997:
	.endm

	.macro	pabt_helper
	@ PABORT handler takes pt_regs in r2, fault address in r4 and psr in r5
#ifdef MULTI_PABORT
	ldr	ip, .LCprocfns
	mov	lr, pc
	ldr	pc, [ip, #PROCESSOR_PABT_FUNC]
#else
	bl	CPU_PABORT_HANDLER
#endif
	.endm

	.macro	dabt_helper

	@
	@ Call the processor-specific abort handler:
	@
	@  r2 - pt_regs
	@  r4 - aborted context pc
	@  r5 - aborted context psr
	@
	@ The abort handler must return the aborted address in r0, and
	@ the fault status register in r1.  r9 must be preserved.
	@
	/** 20151121    
	 * MULTI_DABORT가 정의되지 않으므로 (glue-df.h 참고)
	 * CPU_DABORT_HANDLER로 지정된 v7_early_abort로 브랜치.
	 **/
#ifdef MULTI_DABORT
	ldr	ip, .LCprocfns
	mov	lr, pc
	ldr	pc, [ip, #PROCESSOR_DABT_FUNC]
#else
	bl	CPU_DABORT_HANDLER
#endif
	.endm

#ifdef CONFIG_KPROBES
	.section	.kprobes.text,"ax",%progbits
#else
	.text
#endif

/*
 * Invalid mode handlers
 */
	.macro	inv_entry, reason
	sub	sp, sp, #S_FRAME_SIZE
 ARM(	stmib	sp, {r1 - lr}		)
 THUMB(	stmia	sp, {r0 - r12}		)
 THUMB(	str	sp, [sp, #S_SP]		)
 THUMB(	str	lr, [sp, #S_LR]		)
	mov	r1, #\reason
	.endm

__pabt_invalid:
	inv_entry BAD_PREFETCH
	b	common_invalid
ENDPROC(__pabt_invalid)

__dabt_invalid:
	inv_entry BAD_DATA
	b	common_invalid
ENDPROC(__dabt_invalid)

__irq_invalid:
	inv_entry BAD_IRQ
	b	common_invalid
ENDPROC(__irq_invalid)

__und_invalid:
	inv_entry BAD_UNDEFINSTR

	@
	@ XXX fall through to common_invalid
	@

@
@ common_invalid - generic code for failed exception (re-entrant version of handlers)
@
common_invalid:
	zero_fp

	ldmia	r0, {r4 - r6}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r7, #-1			@  ""   ""    ""        ""
	str	r4, [sp]		@ save preserved r0
	stmia	r0, {r5 - r7}		@ lr_<exception>,
					@ cpsr_<exception>, "old_r0"

	mov	r0, sp
	b	bad_mode
ENDPROC(__und_invalid)

/*
 * SVC mode handlers
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5)
#define SPFIX(code...) code
#else
#define SPFIX(code...)
#endif

	/** 20150919    
	 * SVC mode.
	 *
	 * ex)
	 *	user mode -> (exception 발생) -> und mode -> (vector_stub) -> svc mode
	 *
	 *  arm이 exception 발생시 und mode로 전환해 주지만, kernel은 svc mode에서
	 *	실제 필요한 handler 코드를 동작시킨다.
	 *
	 *  svc_entry 매크로에서 svc mode 진입시 svc의 stack에 exception 발생 전
	 *  사용 중이던 register들을 저장하는 동작을 한다.
	 **/
	.macro	svc_entry, stack_hole=0
 UNWIND(.fnstart		)
 UNWIND(.save {r0 - pc}		)
	/** 20150919    
	 * pt_regs를 저장하기 위한 공간을 확보한다.
	 **/
	sub	sp, sp, #(S_FRAME_SIZE + \stack_hole - 4)
#ifdef CONFIG_THUMB2_KERNEL
 SPFIX(	str	r0, [sp]	)	@ temporarily saved
 SPFIX(	mov	r0, sp		)
 SPFIX(	tst	r0, #4		)	@ test original stack alignment
 SPFIX(	ldr	r0, [sp]	)	@ restored
#else
 SPFIX(	tst	sp, #4		)
#endif
 /** 20150919    
  * tst : Works like ands, but does not store the result.
  * sp의 마지막이 0x0, 0x8인 경우 Z flag가 set.
  * subeq는 z set일 경우 4를 빼 alignment를 맞춘다.
  *
  * 왜 sp의 alignment를 0 또는 8로 맞추는 것일까???
  * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.faqs/14269.html
  **/
 SPFIX(	subeq	sp, sp, #4	)
	/** 20150919    
	 * svc stack에 exception 발생 전 mode의 r1-r12를 svc 스택에 저장한다.
	 * sp는 갱신되지 않았다.
	 **/
	stmia	sp, {r1 - r12}

	/** 20150919    
	 * vector_stub에서 exception mode의 stack에 
	 * r0, lr, spsr을 저장하고, r0에 그 sp를 복사해 두었다.
	 * 그 내용을 가져온다.
	 **/
	ldmia	r0, {r3 - r5}
	add	r7, sp, #S_SP - 4	@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""      ""       ""
	add	r2, sp, #(S_FRAME_SIZE + \stack_hole - 4)
 SPFIX(	addeq	r2, r2, #4	)
	/** 20150919    
	 * exception mode의 stack에서 exception 발생 전 mode의 r0 레지스터값을 가져와
	 * sp의 pt_regs[0] 에 저장한다.
	 **/
	str	r3, [sp, #-4]!		@ save the "real" r0 copied
					@ from the exception stack

	/** 20150919    
	 * r3에 svc mode의 lr 레지스터를 저장한다.
	 **/
	mov	r3, lr

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r2 - sp_svc
	@  r3 - lr_svc
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	/** 20150919    
	 * ptrace.h에 정의된 pt_regs layout만큼 준비한 스택의 나머지 위치에 저장한다.
	 * 
	 * 구조체 pt_regs의 #S_SP, #S_LR, #S_PC, #S_PSR, #S_OLD_R0에
	 * sp_svc, lr_svc, lr_<exception>, spsr_<exception>, orig_r0를 저장한다.
	 **/
	stmia	r7, {r2 - r6}

#ifdef CONFIG_TRACE_IRQFLAGS
	bl	trace_hardirqs_off
#endif
	.endm

	.align	5
__dabt_svc:
	svc_entry
	mov	r2, sp
	dabt_helper

	@
	@ IRQs off again before pulling preserved data off the stack
	@
	disable_irq_notrace

#ifdef CONFIG_TRACE_IRQFLAGS
	tst	r5, #PSR_I_BIT
	bleq	trace_hardirqs_on
	tst	r5, #PSR_I_BIT
	blne	trace_hardirqs_off
#endif
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__dabt_svc)

	.align	5
__irq_svc:
	svc_entry
	irq_handler

#ifdef CONFIG_PREEMPT
	/** 20140816    
	 * tsk에 thread_info를 받아와 선점 카운트와 flags를 추출한다.
	 * 선점 카운트가 0이 아니라면, 즉 선점불가라면 가져온 flags를 0으로 만든다.
	 * flags를 검사해 _TIF_NEED_RESCHED가 켜있다면 svc_preempt 루틴을 실행하고 돌아온다.
	 **/
	get_thread_info tsk
	ldr	r8, [tsk, #TI_PREEMPT]		@ get preempt count
	ldr	r0, [tsk, #TI_FLAGS]		@ get flags
	teq	r8, #0				@ if preempt count != 0
	movne	r0, #0				@ force flags to 0
	tst	r0, #_TIF_NEED_RESCHED
	blne	svc_preempt
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
	@ The parent context IRQs must have been enabled to get here in
	@ the first place, so there's no point checking the PSR I bit.
	bl	trace_hardirqs_on
#endif
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__irq_svc)

	/** 20150801
	 * literal pool의 현재 contents를 이 위치로 dump 시킨다.
	 *
	 * This directive causes the current contents of the literal pool to be dumped into the current section
	 * (which is assumed to be the .text section) at the current location (aligned to a word boundary). 
	 **/
	.ltorg

#ifdef CONFIG_PREEMPT
	/** 20140816    
	 * bl이 호출되어 lr이 변경되기 전에 백업을 받아둔다.
	 * preempt_schedule_irq에서 선점을 위한 __schedule을 호출한 뒤,
	 * 다시 flags를 가져와 TIF_NEED_RESCHED가 켜있다면 선점함수를 호출하고,
	 * 그렇지 않다면 복귀한다.
	 **/
svc_preempt:
	mov	r8, lr
1:	bl	preempt_schedule_irq		@ irq en/disable is done inside
	ldr	r0, [tsk, #TI_FLAGS]		@ get new tasks TI_FLAGS
	tst	r0, #_TIF_NEED_RESCHED
	moveq	pc, r8				@ go again
	b	1b
#endif

__und_fault:
	@ Correct the PC such that it is pointing at the instruction
	@ which caused the fault.  If the faulting instruction was ARM
	@ the PC will be pointing at the next instruction, and have to
	@ subtract 4.  Otherwise, it is Thumb, and the PC will be
	@ pointing at the second half of the Thumb instruction.  We
	@ have to subtract 2.
	/** 20151003    
	 * svc_entry에서 저장해둔 lr_<exception>를 가져와 r1(4)을 빼 다시 저장한다.
	 **/
	ldr	r2, [r0, #S_PC]
	sub	r2, r2, r1
	str	r2, [r0, #S_PC]
	b	do_undefinstr
ENDPROC(__und_fault)

	/** 20150919    
	 * 공통으로 svc_entry를 호출한다.
	 **/
	.align	5
__und_svc:
#ifdef CONFIG_KPROBES
	@ If a kprobe is about to simulate a "stmdb sp..." instruction,
	@ it obviously needs free stack space which then will belong to
	@ the saved context.
	svc_entry 64
#else
	svc_entry
#endif
	@
	@ call emulation code, which returns using r9 if it has emulated
	@ the instruction, or the more conventional lr if we are to treat
	@ this as a real undefined instruction
	@
	@  r0 - instruction
	@
	/** 20150919    
	 * svc_entry에서 r4에는 correction이 적용된 lr_<exception>를 저장해 두었다.
	 *  (exception을 발생시킨 다음 명령)
	 * exception을 발생시킨 instruction을 r0에 받아온다.
	 **/
#ifndef CONFIG_THUMB2_KERNEL
	ldr	r0, [r4, #-4]
#else
	mov	r1, #2
	ldrh	r0, [r4, #-2]			@ Thumb instruction at LR - 2
	cmp	r0, #0xe800			@ 32-bit instruction if xx >= 0
	blo	__und_svc_fault
	ldrh	r9, [r4]			@ bottom 16 bits
	add	r4, r4, #2
	str	r4, [sp, #S_PC]
	orr	r0, r9, r0, lsl #16
#endif
	/** 20151003    
	 * emulated code가 처리된 뒤 리턴할 심볼의 주소를 r9에 저장한다.
	 * svc_entry에서 받아온 lr_<exception> 주소를 r2에 저장한다.
	 **/
	adr	r9, BSYM(__und_svc_finish)
	mov	r2, r4
	/** 20151003    
	 * float point처리가 가능하면 handler로 처리하고 __und_svc_finish로 리턴한다.
	 * (VFP, FPE에 해당)
	 * 그렇지 않은 경우 __und_fault로 이동.
	 **/
	bl	call_fpe

	mov	r1, #4				@ PC correction to apply
__und_svc_fault:
	/** 20151003    
	 * svc_entry에서 sp에 pt_regs 구조체에 해당하는 레지스터 값을 저장해 두었다.
	 * r0에 넣어 argument로 전달.
	 *
	 * 리턴된 이후 다음 라인부터 수행.
	 **/
	mov	r0, sp				@ struct pt_regs *regs
	bl	__und_fault

	@
	@ IRQs off again before pulling preserved data off the stack
	@
__und_svc_finish:
	disable_irq_notrace

	@
	@ restore SPSR and restart the instruction
	@
	/** 20151003    
	 * svc_entry에서 저장한 spsr_<exception>을 r5에 저장한다.
	 **/
	ldr	r5, [sp, #S_PSR]		@ Get SVC cpsr
#ifdef CONFIG_TRACE_IRQFLAGS
	tst	r5, #PSR_I_BIT
	bleq	trace_hardirqs_on
	tst	r5, #PSR_I_BIT
	blne	trace_hardirqs_off
#endif
	/** 20151003    
	 * svc_exit를 호출해 exception 처리 후 복귀주소로 돌아간다.
	 **/
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__und_svc)

	.align	5
__pabt_svc:
	svc_entry
	mov	r2, sp				@ regs
	pabt_helper

	@
	@ IRQs off again before pulling preserved data off the stack
	@
	disable_irq_notrace

#ifdef CONFIG_TRACE_IRQFLAGS
	tst	r5, #PSR_I_BIT
	bleq	trace_hardirqs_on
	tst	r5, #PSR_I_BIT
	blne	trace_hardirqs_off
#endif
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__pabt_svc)

	.align	5
.LCcralign:
	.word	cr_alignment
#ifdef MULTI_DABORT
.LCprocfns:
	.word	processor
#endif
	/** 20151003    
	 * fp_enter 주소를 저장하는 포인터 변수.
	 * 기본값은 no_fp이지만,
	 * nwfpe가 선택된 경우 nwfpe_enter 주소가 fp_enter 포인터 변수에 저장된다.
	 **/
.LCfp:
	.word	fp_enter

/*
 * User mode handlers
 *
 * EABI note: sp_svc is always 64-bit aligned here, so should S_FRAME_SIZE
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5) && (S_FRAME_SIZE & 7)
#error "sizeof(struct pt_regs) must be a multiple of 8"
#endif

	.macro	usr_entry
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)	@ don't unwind the user space
	sub	sp, sp, #S_FRAME_SIZE
 ARM(	stmib	sp, {r1 - r12}	)
 THUMB(	stmia	sp, {r0 - r12}	)

	ldmia	r0, {r3 - r5}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""     ""        ""

	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	@ Also, separately save sp_usr and lr_usr
	@
	stmia	r0, {r4 - r6}
 ARM(	stmdb	r0, {sp, lr}^			)
 THUMB(	store_user_sp_lr r0, r1, S_SP - S_PC	)

	@
	@ Enable the alignment trap while in kernel mode
	@
	alignment_trap r0

	@
	@ Clear FP to mark the first stack frame
	@
	zero_fp

#ifdef CONFIG_IRQSOFF_TRACER
	bl	trace_hardirqs_off
#endif
	.endm

	.macro	kuser_cmpxchg_check
#if !defined(CONFIG_CPU_32v6K) && !defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)
#ifndef CONFIG_MMU
#warning "NPTL on non MMU needs fixing"
#else
	@ Make sure our user space atomic helper is restarted
	@ if it was interrupted in a critical region.  Here we
	@ perform a quick test inline since it should be false
	@ 99.9999% of the time.  The rest is done out of line.
	cmp	r4, #TASK_SIZE
	blhs	kuser_cmpxchg64_fixup
#endif
#endif
	.endm

	.align	5
__dabt_usr:
	usr_entry
	kuser_cmpxchg_check
	mov	r2, sp
	dabt_helper
	b	ret_from_exception
 UNWIND(.fnend		)
ENDPROC(__dabt_usr)

	.align	5
__irq_usr:
	usr_entry
	kuser_cmpxchg_check
	irq_handler
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user_from_irq
 UNWIND(.fnend		)
ENDPROC(__irq_usr)

	.ltorg

	.align	5
__und_usr:
	usr_entry

	mov	r2, r4
	mov	r3, r5

	@ r2 = regs->ARM_pc, which is either 2 or 4 bytes ahead of the
	@      faulting instruction depending on Thumb mode.
	@ r3 = regs->ARM_cpsr
	@
	@ The emulation code returns using r9 if it has emulated the
	@ instruction, or the more conventional lr if we are to treat
	@ this as a real undefined instruction
	@
	adr	r9, BSYM(ret_from_exception)

	tst	r3, #PSR_T_BIT			@ Thumb mode?
	bne	__und_usr_thumb
	sub	r4, r2, #4			@ ARM instr at LR - 4
1:	ldrt	r0, [r4]
#ifdef CONFIG_CPU_ENDIAN_BE8
	rev	r0, r0				@ little endian instruction
#endif
	@ r0 = 32-bit ARM instruction which caused the exception
	@ r2 = PC value for the following instruction (:= regs->ARM_pc)
	@ r4 = PC value for the faulting instruction
	@ lr = 32-bit undefined instruction function
	adr	lr, BSYM(__und_usr_fault_32)
	b	call_fpe

__und_usr_thumb:
	@ Thumb instruction
	sub	r4, r2, #2			@ First half of thumb instr at LR - 2
#if CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7
/*
 * Thumb-2 instruction handling.  Note that because pre-v6 and >= v6 platforms
 * can never be supported in a single kernel, this code is not applicable at
 * all when __LINUX_ARM_ARCH__ < 6.  This allows simplifying assumptions to be
 * made about .arch directives.
 */
#if __LINUX_ARM_ARCH__ < 7
/* If the target CPU may not be Thumb-2-capable, a run-time check is needed: */
#define NEED_CPU_ARCHITECTURE
	ldr	r5, .LCcpu_architecture
	ldr	r5, [r5]
	cmp	r5, #CPU_ARCH_ARMv7
	blo	__und_usr_fault_16		@ 16bit undefined instruction
/*
 * The following code won't get run unless the running CPU really is v7, so
 * coding round the lack of ldrht on older arches is pointless.  Temporarily
 * override the assembler target arch with the minimum required instead:
 */
	.arch	armv6t2
#endif
2:	ldrht	r5, [r4]
	cmp	r5, #0xe800			@ 32bit instruction if xx != 0
	blo	__und_usr_fault_16		@ 16bit undefined instruction
3:	ldrht	r0, [r2]
	add	r2, r2, #2			@ r2 is PC + 2, make it PC + 4
	str	r2, [sp, #S_PC]			@ it's a 2x16bit instr, update
	orr	r0, r0, r5, lsl #16
	adr	lr, BSYM(__und_usr_fault_32)
	@ r0 = the two 16-bit Thumb instructions which caused the exception
	@ r2 = PC value for the following Thumb instruction (:= regs->ARM_pc)
	@ r4 = PC value for the first 16-bit Thumb instruction
	@ lr = 32bit undefined instruction function

#if __LINUX_ARM_ARCH__ < 7
/* If the target arch was overridden, change it back: */
#ifdef CONFIG_CPU_32v6K
	.arch	armv6k
#else
	.arch	armv6
#endif
#endif /* __LINUX_ARM_ARCH__ < 7 */
#else /* !(CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7) */
	b	__und_usr_fault_16
#endif
 UNWIND(.fnend)
ENDPROC(__und_usr)

/*
 * The out of line fixup for the ldrt instructions above.
 */
	.pushsection .fixup, "ax"
	.align	2
4:	mov	pc, r9
	.popsection
	.pushsection __ex_table,"a"
	.long	1b, 4b
#if CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7
	.long	2b, 4b
	.long	3b, 4b
#endif
	.popsection

/*
 * Check whether the instruction is a co-processor instruction.
 * If yes, we need to call the relevant co-processor handler.
 *
 * Note that we don't do a full check here for the co-processor
 * instructions; all instructions with bit 27 set are well
 * defined.  The only instructions that should fault are the
 * co-processor instructions.  However, we have to watch out
 * for the ARM6/ARM7 SWI bug.
 *
 * NEON is a special case that has to be handled here. Not all
 * NEON instructions are co-processor instructions, so we have
 * to make a special case of checking for them. Plus, there's
 * five groups of them, so we have a table of mask/opcode pairs
 * to check against, and if any match then we branch off into the
 * NEON handler code.
 *
 * Emulators may wish to make use of the following registers:
 *  r0  = instruction opcode (32-bit ARM or two 16-bit Thumb)
 *  r2  = PC value to resume execution after successful emulation
 *  r9  = normal "successful" return address
 *  r10 = this threads thread_info structure
 *  lr  = unrecognised instruction return address
 * IRQs disabled, FIQs enabled.
 */
	@
	@ Fall-through from Thumb-2 __und_usr
	@
#ifdef CONFIG_NEON
	adr	r6, .LCneon_thumb_opcodes
	b	2f
#endif
	/** 20151003    
	 * FPE : floating point emulator
	 **/
call_fpe:
#ifdef CONFIG_NEON
	/** 20151003    
	 * .LCneon_arm_opcodes에서 값을 가져와 mask값이 0이 아닐 때까지 반복.
	 * r0에는 instruction이 들어 있고, neon_opcode mask와 연산해 opcode가 나오면
	 * r10에 thread info를 받아온다.
	 *
	 *  arm neon pipeline
	 *  http://i.stack.imgur.com/9dQt4.png
	 **/
	adr	r6, .LCneon_arm_opcodes
2:
	ldr	r7, [r6], #4			@ mask value
	cmp	r7, #0				@ end mask?
	beq	1f
	and	r8, r0, r7
	ldr	r7, [r6], #4			@ opcode bits matching in mask
	cmp	r8, r7				@ NEON instruction?
	bne	2b
	get_thread_info r10
	/** 20151003    
	 * struct thread_info의 used_cp 10, 11번에 mark를 하고 
	 * do_vfp로 이동한다.
	 **/
	mov	r7, #1
	strb	r7, [r10, #TI_USED_CP + 10]	@ mark CP#10 as used
	strb	r7, [r10, #TI_USED_CP + 11]	@ mark CP#11 as used
	b	do_vfp				@ let VFP handler handle this
1:
#endif
	/** 20151003    
	 * instruction의 bit 27, 26을 검사해 해당하지 않으면 리턴.
	 *
	 * 해당하면 instruction을 decoding 해 몇 번 cp를 사용하는지 식별하고,
	 * thread_info의 used_cp의 해당 번호를 설정한 뒤,
	 * 아래 테이블에서 coprocessor에 해당하는 entry로 이동해 실행한다.
	 **/
	tst	r0, #0x08000000			@ only CDP/CPRT/LDC/STC have bit 27
	tstne	r0, #0x04000000			@ bit 26 set on both ARM and Thumb-2
	moveq	pc, lr
	get_thread_info r10			@ get current thread
	and	r8, r0, #0x00000f00		@ mask out CP number
 THUMB(	lsr	r8, r8, #8		)
	mov	r7, #1
	add	r6, r10, #TI_USED_CP
 ARM(	strb	r7, [r6, r8, lsr #8]	)	@ set appropriate used_cp[]
 THUMB(	strb	r7, [r6, r8]		)	@ set appropriate used_cp[]
#ifdef CONFIG_IWMMXT
	@ Test if we need to give access to iWMMXt coprocessors
	ldr	r5, [r10, #TI_FLAGS]
	rsbs	r7, r8, #(1 << 8)		@ CP 0 or 1 only
	movcss	r7, r5, lsr #(TIF_USING_IWMMXT + 1)
	bcs	iwmmxt_task_enable
#endif
 ARM(	add	pc, pc, r8, lsr #6	)
 THUMB(	lsl	r8, r8, #2		)
 THUMB(	add	pc, r8			)
	nop

	/** 20151003    
	 * coprocessor 에 따른 branch instruction table.
	 *
	 * 1,2는 do_fpe로, 10, 11은 do_vfp로 처리하고, 그 외 리턴 주소로 점프.
	 **/
	movw_pc	lr				@ CP#0
	W(b)	do_fpe				@ CP#1 (FPE)
	W(b)	do_fpe				@ CP#2 (FPE)
	movw_pc	lr				@ CP#3
#ifdef CONFIG_CRUNCH
	b	crunch_task_enable		@ CP#4 (MaverickCrunch)
	b	crunch_task_enable		@ CP#5 (MaverickCrunch)
	b	crunch_task_enable		@ CP#6 (MaverickCrunch)
#else
	movw_pc	lr				@ CP#4
	movw_pc	lr				@ CP#5
	movw_pc	lr				@ CP#6
#endif
	movw_pc	lr				@ CP#7
	movw_pc	lr				@ CP#8
	movw_pc	lr				@ CP#9
#ifdef CONFIG_VFP
	W(b)	do_vfp				@ CP#10 (VFP)
	W(b)	do_vfp				@ CP#11 (VFP)
#else
	movw_pc	lr				@ CP#10 (VFP)
	movw_pc	lr				@ CP#11 (VFP)
#endif
	movw_pc	lr				@ CP#12
	movw_pc	lr				@ CP#13
	movw_pc	lr				@ CP#14 (Debug)
	movw_pc	lr				@ CP#15 (Control)

#ifdef NEED_CPU_ARCHITECTURE
	.align	2
.LCcpu_architecture:
	.word	__cpu_architecture
#endif

#ifdef CONFIG_NEON
	.align	6

.LCneon_arm_opcodes:
	.word	0xfe000000			@ mask
	.word	0xf2000000			@ opcode

	.word	0xff100000			@ mask
	.word	0xf4000000			@ opcode

	.word	0x00000000			@ mask
	.word	0x00000000			@ opcode

.LCneon_thumb_opcodes:
	.word	0xef000000			@ mask
	.word	0xef000000			@ opcode

	.word	0xff100000			@ mask
	.word	0xf9000000			@ opcode

	.word	0x00000000			@ mask
	.word	0x00000000			@ opcode
#endif

do_fpe:
	enable_irq
	ldr	r4, .LCfp
	add	r10, r10, #TI_FPSTATE		@ r10 = workspace
	ldr	pc, [r4]			@ Call FP module USR entry point

/*
 * The FP module is called with these registers set:
 *  r0  = instruction
 *  r2  = PC+4
 *  r9  = normal "successful" return address
 *  r10 = FP workspace
 *  lr  = unrecognised FP instruction return address
 */

	/** 20151003    
	 * fp_enter의 기본값은 no_fp.
	 **/
	.pushsection .data
ENTRY(fp_enter)
	.word	no_fp
	.popsection

ENTRY(no_fp)
	mov	pc, lr
ENDPROC(no_fp)

__und_usr_fault_32:
	mov	r1, #4
	b	1f
__und_usr_fault_16:
	mov	r1, #2
1:	enable_irq
	mov	r0, sp
	adr	lr, BSYM(ret_from_exception)
	b	__und_fault
ENDPROC(__und_usr_fault_32)
ENDPROC(__und_usr_fault_16)

	.align	5
__pabt_usr:
	usr_entry
	mov	r2, sp				@ regs
	pabt_helper
 UNWIND(.fnend		)
	/* fall through */
/*
 * This is the return code to user mode for abort handlers
 */
ENTRY(ret_from_exception)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user
 UNWIND(.fnend		)
ENDPROC(__pabt_usr)
ENDPROC(ret_from_exception)

/*
 * Register switch for ARMv3 and ARMv4 processors
 * r0 = previous task_struct, r1 = previous thread_info, r2 = next thread_info
 * previous and next are guaranteed not to be the same.
 */
/** 20150118    
 * 두 task 사이의 문맥 전환 함수. register를 교환한다.
 *
 *   r0 : 이전 task_struct, r1 : 이전 thread_info, r2 : 다음 thread_info
 *
 *   struct cpu_context_save * prev_ti, * next_ti;
 *   prev_ti = r1.cpu_context;
 *   *prev_ti <= {r4 ~ sl, fp, sp, lr(pc)};
 *   set_tls(r3, r4, r5);
 *   tmp = r0;
 *   next_ti = r2.cpu_context;
 *   atomic_notifier_call_chain(thread_notify_head, THREAD_NOTIFY_SWITCH, r2);
 *   r0 = tmp;
 *   *text_ti => { r4 ~ sl, fp, sp, pc };
 * 
 * [APCS registers]
 * r0	a1	    argument 1/scratch register/result
 * r1	a2	    argument 2/scratch register/result
 * r2	a3	    argument 3/scratch register/result
 * r3	a4	    argument 4/scratch register/result
 * r4	v1	    register variable
 * r5	v2	    register variable
 * r6	v3	    register variable
 * r7	v4	    register variable
 * r8	v5	    register variable
 * r9	sb/v6	static base/register variable
 * r10	sl/v7	stack limit/stack chunk handle/register variable
 * r11	fp/v8	frame pointer/register variable
 * r12	ip	    scratch register/new -sb in inter-link-unit calls
 * r13	sp	    lower end of the current stack frame
 * r14	lr	    link register/scratch register
 * r15	pc	    program counter
 **/
ENTRY(__switch_to)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	add	ip, r1, #TI_CPU_SAVE
	ldr	r3, [r2, #TI_TP_VALUE]
 ARM(	stmia	ip!, {r4 - sl, fp, sp, lr} )	@ Store most regs on stack
 THUMB(	stmia	ip!, {r4 - sl, fp}	   )	@ Store most regs on stack
 THUMB(	str	sp, [ip], #4		   )
 THUMB(	str	lr, [ip], #4		   )
#ifdef CONFIG_CPU_USE_DOMAINS
	ldr	r6, [r2, #TI_CPU_DOMAIN]
#endif
	set_tls	r3, r4, r5
#if defined(CONFIG_CC_STACKPROTECTOR) && !defined(CONFIG_SMP)
	ldr	r7, [r2, #TI_TASK]
	ldr	r8, =__stack_chk_guard
	ldr	r7, [r7, #TSK_STACK_CANARY]
#endif
#ifdef CONFIG_CPU_USE_DOMAINS
	mcr	p15, 0, r6, c3, c0, 0		@ Set domain register
#endif
	mov	r5, r0
	add	r4, r2, #TI_CPU_SAVE
	ldr	r0, =thread_notify_head
	mov	r1, #THREAD_NOTIFY_SWITCH
	bl	atomic_notifier_call_chain
#if defined(CONFIG_CC_STACKPROTECTOR) && !defined(CONFIG_SMP)
	str	r7, [r8]
#endif
 THUMB(	mov	ip, r4			   )
	mov	r0, r5
 ARM(	ldmia	r4, {r4 - sl, fp, sp, pc}  )	@ Load all regs saved previously
 THUMB(	ldmia	ip!, {r4 - sl, fp}	   )	@ Load all regs saved previously
 THUMB(	ldr	sp, [ip], #4		   )
 THUMB(	ldr	pc, [ip]		   )
 UNWIND(.fnend		)
ENDPROC(__switch_to)

	__INIT

/*
 * User helpers.
 *
 * Each segment is 32-byte aligned and will be moved to the top of the high
 * vector page.  New segments (if ever needed) must be added in front of
 * existing ones.  This mechanism should be used only for things that are
 * really small and justified, and not be abused freely.
 *
 * See Documentation/arm/kernel_user_helpers.txt for formal definitions.
 */
/** 20130316
 * kuser_helper 동작을 끝내고 user_space로 return	
 **/
 THUMB(	.arm	)

	.macro	usr_ret, reg
#ifdef CONFIG_ARM_THUMB
	bx	\reg
#else
	mov	pc, \reg
#endif
	.endm

	/** 20130309    
	 * instruction의 align 조정 (cache line)
	 **/
	.align	5
	.globl	__kuser_helper_start
__kuser_helper_start:

/*
 * Due to the length of some sequences, __kuser_cmpxchg64 spans 2 regular
 * kuser "slots", therefore 0xffff0f80 is not used as a valid entry point.
 */

__kuser_cmpxchg64:				@ 0xffff0f60

#if defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)

	/*
	 * Poor you.  No fast solution possible...
	 * The kernel itself must perform the operation.
	 * A special ghost syscall is used for that (see traps.c).
	 */
	stmfd	sp!, {r7, lr}
	ldr	r7, 1f			@ it's 20 bits
	swi	__ARM_NR_cmpxchg64
	ldmfd	sp!, {r7, pc}
1:	.word	__ARM_NR_cmpxchg64

#elif defined(CONFIG_CPU_32v6K)

	stmfd	sp!, {r4, r5, r6, r7}
	ldrd	r4, r5, [r0]			@ load old val
	ldrd	r6, r7, [r1]			@ load new val
	smp_dmb	arm
1:	ldrexd	r0, r1, [r2]			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eoreqs	r3, r1, r5			@ compare with oldval (2)
	strexdeq r3, r6, r7, [r2]		@ store newval if eq
	teqeq	r3, #1				@ success?
	beq	1b				@ if no then retry
	smp_dmb	arm
	rsbs	r0, r3, #0			@ set returned val and C flag
	ldmfd	sp!, {r4, r5, r6, r7}
	usr_ret	lr

#elif !defined(CONFIG_SMP)

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg64
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle of
	 * the critical sequence.  The same strategy as for cmpxchg is used.
	 */
	stmfd	sp!, {r4, r5, r6, lr}
	ldmia	r0, {r4, r5}			@ load old val
	ldmia	r1, {r6, lr}			@ load new val
1:	ldmia	r2, {r0, r1}			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eoreqs	r3, r1, r5			@ compare with oldval (2)
2:	stmeqia	r2, {r6, lr}			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	ldmfd	sp!, {r4, r5, r6, pc}

	.text
kuser_cmpxchg64_fixup:
	@ Called from kuser_cmpxchg_fixup.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0f60 + (1b - __kuser_cmpxchg64)))
	subs	r8, r4, r7
	rsbcss	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
#if __LINUX_ARM_ARCH__ < 6
	bcc	kuser_cmpxchg32_fixup
#endif
	mov	pc, lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else
#error "incoherent kernel configuration"
#endif

	/* pad to next slot */
	.rept	(16 - (. - __kuser_cmpxchg64)/4)
	.word	0
	.endr

	.align	5

__kuser_memory_barrier:				@ 0xffff0fa0
	smp_dmb	arm
	usr_ret	lr

	.align	5

__kuser_cmpxchg:				@ 0xffff0fc0

#if defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)

	/*
	 * Poor you.  No fast solution possible...
	 * The kernel itself must perform the operation.
	 * A special ghost syscall is used for that (see traps.c).
	 */
	/** 20130309    
	 * 이전 버전 architecture 에서는 memory barrier instruction이 지원되지 않으므로
	 * software interrupt를 발생시켜 처리한다.
	**/
	stmfd	sp!, {r7, lr}
	ldr	r7, 1f			@ it's 20 bits
	swi	__ARM_NR_cmpxchg
	ldmfd	sp!, {r7, pc}
1:	.word	__ARM_NR_cmpxchg

#elif __LINUX_ARM_ARCH__ < 6

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle
	 * of the critical sequence.  To prevent this, code is added to
	 * the IRQ and data abort exception handlers to set the pc back
	 * to the beginning of the critical section if it is found to be
	 * within that critical section (see kuser_cmpxchg_fixup).
	 */
1:	ldr	r3, [r2]			@ load current val
	subs	r3, r3, r0			@ compare with oldval
2:	streq	r1, [r2]			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	usr_ret	lr

	.text
kuser_cmpxchg32_fixup:
	@ Called from kuser_cmpxchg_check macro.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0fc0 + (1b - __kuser_cmpxchg)))
	subs	r8, r4, r7
	rsbcss	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
	mov	pc, lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else

	/** 20130309    
	 * __kuser_cmpxchg 에 대해
	 * CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG 도 아니고, 
	 * v6 이상일 경우 실행
	 *
	 * 20130316
	 * r2 메모리 값과 r0 레지스터 값이 같을 때까지 비교하고 같을 때 r1 메모리
	 * 값을 r2 메모리 값에 저장.		 
	 *
	 * ldrex, strex 는 atomic 동작을 보장하기 위한 명령어.		
	 * [참고] http://en.wikipedia.org/wiki/Load-link/store-conditional
	 * 
	 * include/asm/atomic.h 에 정리해 두었음.
	 **/

	smp_dmb	arm
1:	ldrex	r3, [r2]
	subs	r3, r3, r0
	strexeq	r3, r1, [r2]
	teqeq	r3, #1
	beq	1b
	rsbs	r0, r3, #0
	/* beware -- each __kuser slot must be 8 instructions max */
	/** 20130316 
	 * memory_barrier 함수 호출하고 user_space로 복귀.
	 **/
	ALT_SMP(b	__kuser_memory_barrier)
	ALT_UP(usr_ret	lr)

#endif

	.align	5
	/** 20130316 
	 * get_tls의 return value (r0)는 tls value가 된다. 
	 *
	 * ldr	r0, [pc, #(16 - 8)]	@ read TLS, set in kuser_get_tls_init
	 * usr_ret	lr
	 * mrc	p15, 0, r0, c13, c0, 3	@ 0xffff0fe8 hardware TLS code
	 * .word	0
	 * .word	0  <- ldr r0, ... 실행시 pc+8 위치.
	 * .word	0
	 * .word	0
	 *
	 * hardware TLS code는
	 * ARM 참조 - B4.1.151 TPIDRURO, User Read-Only Thread ID Register, VMSA
	 **/
__kuser_get_tls:				@ 0xffff0fe0
	ldr	r0, [pc, #(16 - 8)]	@ read TLS, set in kuser_get_tls_init
	usr_ret	lr
	mrc	p15, 0, r0, c13, c0, 3	@ 0xffff0fe8 hardware TLS code
	.rep	4
	.word	0			@ 0xffff0ff0 software TLS value, then
	.endr				@ pad up to __kuser_helper_version

__kuser_helper_version:				@ 0xffff0ffc
	.word	((__kuser_helper_end - __kuser_helper_start) >> 5)

	.globl	__kuser_helper_end
__kuser_helper_end:

 THUMB(	.thumb	)

/*
 * Vector stubs.
 *
 * This code is copied to 0xffff0200 so we can use branches in the
 * vectors, rather than ldr's.  Note that this code must not
 * exceed 0x300 bytes.
 *
 * Common stub entry macro:
 *   Enter in IRQ mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 *
 * SP points to a minimal amount of processor-private memory, the address
 * of which is copied into r0 for the mode specific abort handler.
 */
	/** 20130309    
	 * 각 vector mode에 대한 공통 코드
	 *
	 *   vector_stub 진행 중 svc 모드로 전환된다.
	 *
	 *   name : exception mode 이름. __vectors_start에서 호출. 예) irq, dabt, ...
	 *   mode : macro로 정의하고자 하는 mode
	 *   correction : exception에 따른 복귀 주소 보정
	 *
	 *   -----------------------------------------------------------------
	 *   Exception               Address    Use
	 *   -----------------------------------------------------------------
	 *   Reset                   -          -
	 *   Data Abort              lr-8       Data Abort 유발 명령으로 복귀
	 *   FIQ                     lr-4       FIQ 발생 후 실행예정 명령으로 복귀
	 *   IRQ                     lr-4       IRQ 발생 후 실행예정 명령으로 복귀
	 *   Prefetch Abort          lr-4       Prefetch Abort 유발 명령으로 복귀
	 *   SWI                     lr         SWI(svc) 다음 명령으로 복귀
	 *   Undefined Instruction   lr         undefined instruction 다음 명령으로 복귀
	 *   -----------------------------------------------------------------
	 *     [출처] ARM System Developer's Guide
	 *       9.1 Exception Handling
	 **/
	.macro	vector_stub, name, mode, correction=0
	/** 20130309    
	 * 2 ** 5 (cache line 단위) align
	 **/
	.align	5

vector_\name:
	.if \correction
	/** 20130309    
	 * 복귀 주소에 대한 correction 보상
	 **/
	sub	lr, lr, #\correction
	.endif

	@
	@ Save r0, lr_<exception> (parent PC) and spsr_<exception>
	@ (parent CPSR)
	@
	/** 20130309    
	 * r0와 lr을 해당 모드의 stack에 저장 (sp는 증가하지 않은 상태)
	 **/
	stmia	sp, {r0, lr}		@ save r0, lr
	/** 20130309    
	 * 이전 모드의 cpsr을 sp+8 위치에 저장
	 *  |      |   
	 *  +------+    <- sp
	 *  | r0   |
	 *  +------+
	 *  | lr   |
	 *  +------+    <- sp + 8
	 *  | spsr |
	 *  +------+
	 **/
	mrs	lr, spsr
	str	lr, [sp, #8]		@ save spsr

	@
	@ Prepare for SVC32 mode.  IRQs remain disabled.
	@
	mrs	r0, cpsr
	/** 20130309    
	 * 해당 \mode를 clear하고, SVC_MODE로 전환
	 * spsr_cxsf에 저장하는 이유는 movs pc, lr시에 cpsr로 채우기 위함
	 *
	 * svc_exit에서 
	 * msr	spsr_cxsf, \rpsr
	 * ldmia	sp, {r0 - pc}^			@ load r0 - pc, cpsr
	 **/
	eor	r0, r0, #(\mode ^ SVC_MODE | PSR_ISETSTATE)
	msr	spsr_cxsf, r0

	@
	@ the branch table must immediately follow this code
	@
	/** 20130309    
	 * spsr로부터 이전 모드를 추출한다.
	 **/
	and	lr, lr, #0x0f
 THUMB(	adr	r0, 1f			)
 THUMB(	ldr	lr, [r0, lr, lsl #2]	)
	/** 20130309    
	 * sp를 r0에 복사
	 **/
	mov	r0, sp
	/** 20130309    
	 * exception 발생 당시의 mode에 따라 호출할 routine을 찾는다.
	 *
	 * lr <- pc + (mode * 4)	; pc는 vector_stub macro 호출 다음 instruction을 가리키고 있다.
	 * 실행할 instruction을 lr에 저장
	 **/
 ARM(	ldr	lr, [pc, lr, lsl #2]	)
	/** 20130309    
	 * 찾은 routine으로 이동하면서 s접미사로 pc를 변경하므로 spsr이 cpsr로 복사됨.
	 **/
	movs	pc, lr			@ branch to handler in SVC mode
ENDPROC(vector_\name)

	/** 20130309    
	 * vector_stub 함수 선언 아래 handler address에 table에 대한 align을 정의한다.
	 * 2 ** 2 (4 byte) align
	 **/
	.align	2
	@ handler addresses follow this label
1:
	.endm

	.globl	__stubs_start
__stubs_start:
/*
 * Interrupt dispatcher
 */
 /** 20130309    
  * vector_stub macro에 irq 관련 argument를 이용해 exception handler를 정의.
  * 이후 exception 발생시점의 mode에 따라 exception handler table을 나열한다.
  *
  * entry가 16개인 이유는 4비트로 16개의 mode가 가능하기 때문이다.
  **/
	vector_stub	irq, IRQ_MODE, 4

	.long	__irq_usr			@  0  (USR_26 / USR_32)
	.long	__irq_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__irq_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__irq_svc			@  3  (SVC_26 / SVC_32)
	.long	__irq_invalid			@  4
	.long	__irq_invalid			@  5
	.long	__irq_invalid			@  6
	.long	__irq_invalid			@  7
	.long	__irq_invalid			@  8
	.long	__irq_invalid			@  9
	.long	__irq_invalid			@  a
	.long	__irq_invalid			@  b
	.long	__irq_invalid			@  c
	.long	__irq_invalid			@  d
	.long	__irq_invalid			@  e
	.long	__irq_invalid			@  f

/*
 * Data abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */
	vector_stub	dabt, ABT_MODE, 8

	.long	__dabt_usr			@  0  (USR_26 / USR_32)
	.long	__dabt_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__dabt_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__dabt_svc			@  3  (SVC_26 / SVC_32)
	.long	__dabt_invalid			@  4
	.long	__dabt_invalid			@  5
	.long	__dabt_invalid			@  6
	.long	__dabt_invalid			@  7
	.long	__dabt_invalid			@  8
	.long	__dabt_invalid			@  9
	.long	__dabt_invalid			@  a
	.long	__dabt_invalid			@  b
	.long	__dabt_invalid			@  c
	.long	__dabt_invalid			@  d
	.long	__dabt_invalid			@  e
	.long	__dabt_invalid			@  f

/*
 * Prefetch abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */
	vector_stub	pabt, ABT_MODE, 4

	.long	__pabt_usr			@  0 (USR_26 / USR_32)
	.long	__pabt_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__pabt_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__pabt_svc			@  3 (SVC_26 / SVC_32)
	.long	__pabt_invalid			@  4
	.long	__pabt_invalid			@  5
	.long	__pabt_invalid			@  6
	.long	__pabt_invalid			@  7
	.long	__pabt_invalid			@  8
	.long	__pabt_invalid			@  9
	.long	__pabt_invalid			@  a
	.long	__pabt_invalid			@  b
	.long	__pabt_invalid			@  c
	.long	__pabt_invalid			@  d
	.long	__pabt_invalid			@  e
	.long	__pabt_invalid			@  f

/*
 * Undef instr entry dispatcher
 * Enter in UND mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 */
	vector_stub	und, UND_MODE

	.long	__und_usr			@  0 (USR_26 / USR_32)
	.long	__und_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__und_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__und_svc			@  3 (SVC_26 / SVC_32)
	.long	__und_invalid			@  4
	.long	__und_invalid			@  5
	.long	__und_invalid			@  6
	.long	__und_invalid			@  7
	.long	__und_invalid			@  8
	.long	__und_invalid			@  9
	.long	__und_invalid			@  a
	.long	__und_invalid			@  b
	.long	__und_invalid			@  c
	.long	__und_invalid			@  d
	.long	__und_invalid			@  e
	.long	__und_invalid			@  f

	.align	5

/*=============================================================================
 * Undefined FIQs
 *-----------------------------------------------------------------------------
 * Enter in FIQ mode, spsr = ANY CPSR, lr = ANY PC
 * MUST PRESERVE SVC SPSR, but need to switch to SVC mode to show our msg.
 * Basically to switch modes, we *HAVE* to clobber one register...  brain
 * damage alert!  I don't think that we can execute any code in here in any
 * other mode than FIQ...  Ok you can switch to another mode, but you can't
 * get out of that mode without clobbering one register.
 */
vector_fiq:
	subs	pc, lr, #4

/*=============================================================================
 * Address exception handler
 *-----------------------------------------------------------------------------
 * These aren't too critical.
 * (they're not supposed to happen, and won't happen in 32-bit data mode).
 */

vector_addrexcptn:
	b	vector_addrexcptn

/*
 * We group all the following data together to optimise
 * for CPUs with separate I & D caches.
 */
	.align	5

.LCvswi:
	.word	vector_swi

	.globl	__stubs_end
__stubs_end:

	.equ	stubs_offset, __vectors_start + 0x200 - __stubs_start

	/** 20130309    
	 * vector table 선언 
	 *
	 *   - Exception과 연관된 Processor Mode
	 *   -----------------------------------------------------------------
	 *   Exception                Mode         Main purpose
	 *   -----------------------------------------------------------------
	 *   Fast Interrupt Request   FIQ          fast interrupt request handling 
	 *   Interrupt Request        IRQ          interrupt request handling
	 *   SWI and Reset            SVC          protected mode for operating systems
	 *   Prefetch/Data Abort      abort        virtual memory and/or memory protection handling
	 *   Undefined Instruction    undefined    software emulation of hardware coprocessors
	 *   -----------------------------------------------------------------
	 *     [출처] ARM System Developer's Guide
	 *       9.1 Exception Handling
	 **/
	.globl	__vectors_start
__vectors_start:
 ARM(	swi	SYS_ERROR0	)
 THUMB(	svc	#0		)
 THUMB(	nop			)
	W(b)	vector_und + stubs_offset
	W(ldr)	pc, .LCvswi + stubs_offset
	W(b)	vector_pabt + stubs_offset
	W(b)	vector_dabt + stubs_offset
	W(b)	vector_addrexcptn + stubs_offset
	/** 20130309    
	 *		(stubs_start + 0) + (__vectors_start + 0x200 - __stubs_start)
	 *			=> __vectors_start + 0x200 + 0
	 *
	 *		early_trap_init에서 __stubs_start부터 vectors + 0x200에 복사.
	 **/	
	W(b)	vector_irq + stubs_offset
	W(b)	vector_fiq + stubs_offset

	.globl	__vectors_end
__vectors_end:

	.data

	.globl	cr_alignment
	.globl	cr_no_alignment
cr_alignment:
	.space	4
cr_no_alignment:
	.space	4

#ifdef CONFIG_MULTI_IRQ_HANDLER
	/** 20140816    
	 * architecture의 irq 핸들러의 위치를 저장할 전역변수.
	 *
	 * setup_arch에서 machine의 handle_irq를 저장한다.
	 * vexpress의 경우 gic_handle_irq가 저장
	 **/
	.globl	handle_arch_irq
handle_arch_irq:
	.space	4
#endif
