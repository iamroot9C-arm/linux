#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/errno.h>
#include <asm/thread_info.h>

@ Bad Abort numbers
@ -----------------
@
#define BAD_PREFETCH	0
#define BAD_DATA	1
#define BAD_ADDREXCPTN	2
#define BAD_IRQ		3
#define BAD_UNDEFINSTR	4

@
@ Most of the stack format comes from struct pt_regs, but with
@ the addition of 8 bytes for storing syscall args 5 and 6.
@ This _must_ remain a multiple of 8 for EABI.
@
/** 20160206
 * stack format = struct pt_regs + (args 5, args 6 )

 * EABI 프로토콜에 의해 8의 배수를 유지시켜야 한다.
 **/
#define S_OFF		8

/* 
 * The SWI code relies on the fact that R0 is at the bottom of the stack
 * (due to slow/fast restore user regs).
 */
#if S_R0 != 0
#error "Please fix"
#endif

/** 20160305
 * CONFIG_FRAME_POINTER 정의시 fp 위치에 0을 넣어준다.
 * 분석 config에는 CONFIG_FRAME_POINTER를 선언하지 않았음.
 **/
	.macro	zero_fp
#ifdef CONFIG_FRAME_POINTER
	mov	fp, #0
#endif
	.endm

	.macro	alignment_trap, rtemp
#ifdef CONFIG_ALIGNMENT_TRAP
	ldr	\rtemp, .LCcralign
	ldr	\rtemp, [\rtemp]
	mcr	p15, 0, \rtemp, c1, c0
#endif
	.endm

	@
	@ Store/load the USER SP and LR registers by switching to the SYS
	@ mode. Useful in Thumb-2 mode where "stm/ldm rd, {sp, lr}^" is not
	@ available. Should only be called from SVC mode
	@
	.macro	store_user_sp_lr, rd, rtemp, offset = 0
	mrs	\rtemp, cpsr
	eor	\rtemp, \rtemp, #(SVC_MODE ^ SYSTEM_MODE)
	msr	cpsr_c, \rtemp			@ switch to the SYS mode

	str	sp, [\rd, #\offset]		@ save sp_usr
	str	lr, [\rd, #\offset + 4]		@ save lr_usr

	eor	\rtemp, \rtemp, #(SVC_MODE ^ SYSTEM_MODE)
	msr	cpsr_c, \rtemp			@ switch back to the SVC mode
	.endm

	.macro	load_user_sp_lr, rd, rtemp, offset = 0
	mrs	\rtemp, cpsr
	eor	\rtemp, \rtemp, #(SVC_MODE ^ SYSTEM_MODE)
	msr	cpsr_c, \rtemp			@ switch to the SYS mode

	ldr	sp, [\rd, #\offset]		@ load sp_usr
	ldr	lr, [\rd, #\offset + 4]		@ load lr_usr

	eor	\rtemp, \rtemp, #(SVC_MODE ^ SYSTEM_MODE)
	msr	cpsr_c, \rtemp			@ switch back to the SVC mode
	.endm

#ifndef CONFIG_THUMB2_KERNEL
	/** 20140824
	 * supervisor mode에서 복귀하여 exception code로 진입하는 매크로.
	 **/
	.macro	svc_exit, rpsr
	/** 20150919
	 * 매개변수로 받은 rpsr을 spsr_cxsf에 복사한다.
	 **/
	msr	spsr_cxsf, \rpsr
#if defined(CONFIG_CPU_V6)
	ldr	r0, [sp]
	strex	r1, r2, [sp]			@ clear the exclusive monitor
	ldmib	sp, {r1 - pc}^			@ load r1 - pc, cpsr
#elif defined(CONFIG_CPU_32v6K)
	/** 20140824
	 * clrex로 exclusive monitor를 초기화 해준다.
	 * stack에 저장해둔 r0 - pc 레지스터를 복원하고, spsr을 cpsr로 복사한다.
	 *
	 * svc mode 스택의 pc 레지스터에 저장된 주소는 lr_<exception>.
	 **/
	clrex					@ clear the exclusive monitor
	ldmia	sp, {r0 - pc}^			@ load r0 - pc, cpsr
#else
	ldmia	sp, {r0 - pc}^			@ load r0 - pc, cpsr
#endif
	.endm

	/** 20160305
	 * user mode context를 복원하고 user mode로 돌아간다.
	 *
	 * fast ret인지 구분하는 정보와 stack 보정값 offset을 argument로 받는다.
	 *
	 * kernel_execve에서 ret_to_user인 경우는 호출시 fast가 0.
	 * syscall에서 돌아가는 경우는 fast가 1.
	 **/
	.macro	restore_user_regs, fast = 0, offset = 0
	/** 20160305
	 * pt_regs에서 user context를 꺼내온다.
	 * offset은 r4, r5 등 저장 여부에 따라 호출시 주어진다.
	 *
	 * r1 = calling cpsr; (user mode cpsr)
	 * lr = pc, sp = sp + S_PC;
	 * spsr_svc = r1;
	 **/
	ldr	r1, [sp, #\offset + S_PSR]	@ get calling cpsr
	ldr	lr, [sp, #\offset + S_PC]!	@ get pc
	msr	spsr_cxsf, r1			@ save in spsr_svc
#if defined(CONFIG_CPU_V6)
	strex	r1, r2, [sp]			@ clear the exclusive monitor
#elif defined(CONFIG_CPU_32v6K)
	/** 20160305
	 * exclusive monitor clear.
	 * 문맥전환 전에 exclusive monitor를 클리어한다.
	 **/
	clrex					@ clear the exclusive monitor
#endif
	/** 20160305
	 * LDM{IA|IB|DA|DB} Rn, <reglist-PC>^
	 *   [Rn]에서 사용자 모드 레지스터 목록 로드. 권한 모드 전용.
	 *
	 * sp의 현재 위치: ARM_pc
	 * stack에서 꺼내 user mode register에 저장.
	 * fast인 경우 r1 ~ lr까지, 
	 * fast가 아닌 경우 r0 ~ lr까지.
	 **/
	.if	\fast
	ldmdb	sp, {r1 - lr}^			@ get calling r1 - lr
	.else
	ldmdb	sp, {r0 - lr}^			@ get calling r0 - lr
	.endif
	/** 20160305
	 * 주석에 의하면 ldm {}^ 명령을 사용한 경우,
	 * ARMv5T 이전 architecture를 위해 nop이 필요하다.
	 **/
	mov	r0, r0				@ ARMv5T and earlier require a nop
						@ after ldm {}^
	/** 20160305
	 * sp의 현재 위치: ARM_pc.
	 * sp를 증가시켜 ARM_cpsr, ARM_ORIG_r0 위의 시작 위치로 변경시킨다.
	 *
	 * lr을 pc에 저장하며 spsr_svc가 cpsr로 들어간다.
	 * mode 비트가 변경되므로 USER MODE가 호출된다.
	 **/
	add	sp, sp, #S_FRAME_SIZE - S_PC
	movs	pc, lr				@ return & move spsr_svc into cpsr
	.endm

	/** 20140816
	 * thread_info는 stack에 overlay된다.
	 * sp를 8KB로 내림해 thread_info의 위치를 rd에 받아온다.
	 **/
	.macro	get_thread_info, rd
	mov	\rd, sp, lsr #13
	mov	\rd, \rd, lsl #13
	.endm

	@
	@ 32-bit wide "mov pc, reg"
	@
	/** 20151003
	 * mov 명령으로 reg값을 pc에 넣어 이동한다.
	 **/
	.macro	movw_pc, reg
	mov	pc, \reg
	.endm
#else	/* CONFIG_THUMB2_KERNEL */
	.macro	svc_exit, rpsr
	ldr	lr, [sp, #S_SP]			@ top of the stack
	ldrd	r0, r1, [sp, #S_LR]		@ calling lr and pc
	clrex					@ clear the exclusive monitor
	stmdb	lr!, {r0, r1, \rpsr}		@ calling lr and rfe context
	ldmia	sp, {r0 - r12}
	mov	sp, lr
	ldr	lr, [sp], #4
	rfeia	sp!
	.endm

	.macro	restore_user_regs, fast = 0, offset = 0
	clrex					@ clear the exclusive monitor
	mov	r2, sp
	load_user_sp_lr r2, r3, \offset + S_SP	@ calling sp, lr
	ldr	r1, [sp, #\offset + S_PSR]	@ get calling cpsr
	ldr	lr, [sp, #\offset + S_PC]	@ get pc
	add	sp, sp, #\offset + S_SP
	msr	spsr_cxsf, r1			@ save in spsr_svc
	.if	\fast
	ldmdb	sp, {r1 - r12}			@ get calling r1 - r12
	.else
	ldmdb	sp, {r0 - r12}			@ get calling r0 - r12
	.endif
	add	sp, sp, #S_FRAME_SIZE - S_SP
	movs	pc, lr				@ return & move spsr_svc into cpsr
	.endm

	.macro	get_thread_info, rd
	mov	\rd, sp
	lsr	\rd, \rd, #13
	mov	\rd, \rd, lsl #13
	.endm

	@
	@ 32-bit wide "mov pc, reg"
	@
	.macro	movw_pc, reg
	mov	pc, \reg
	nop
	.endm
#endif	/* !CONFIG_THUMB2_KERNEL */

/*
 * These are the registers used in the syscall handler, and allow us to
 * have in theory up to 7 arguments to a function - r0 to r6.
 *
 * r7 is reserved for the system call number for thumb mode.
 *
 * Note that tbl == why is intentional.
 *
 * We must set at least "tsk" and "why" when calling ret_with_reschedule.
 */
/** 20160305
 *  syscall number          r7
 *  syscall table pointer   r8
 *  linux syscall에 의한 경우이면 syscall table pointer로 설정되었으므로 != 0
 *    0이면 syscall이 아닌 이유. if or if not을 나타내기 위해 같은 r8로 설정.
 *  current thread_info     r9
 **/
scno	.req	r7		@ syscall number
tbl	.req	r8		@ syscall table pointer
why	.req	r8		@ Linux syscall (!= 0)
tsk	.req	r9		@ current thread_info
