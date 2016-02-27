/*
 *  arch/arm/include/asm/thread_info.h
 *
 *  Copyright (C) 2002 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_ARM_THREAD_INFO_H
#define __ASM_ARM_THREAD_INFO_H

#ifdef __KERNEL__

#include <linux/compiler.h>
#include <asm/fpstate.h>

/** 20150118    
 * thread_info 관련 define.
 *
 *   order 1 << (1) = 2.
 *   size  8KB
 *   SP의 시작은 thread_info와 union이므로 THREAD_SIZE만큼 더한다.
 *
 *     ( -8은 왜 ??? )
 *     (추측) AAPCS를 보면 첫번째 제약으로 다음과 같은 조건이 있다.
 *     Stack-limit < SP <= stack-base
 *     stack-base가 task->stack이 가리키는 주소라면,
 *     sp는 +8KB 보다 작은 값 안에 위치해야 하고 standard로 사용하기 위해
 *     8byte 단위로 정렬되어야 하므로 8을 빼준 것이다.
 *     stack이 full descending으로 동작하여 감소하는 방향으로 자라므로
 *     THREAD_START_SP가 곧 시작 위치이다.
 **/
#define THREAD_SIZE_ORDER	1
#define THREAD_SIZE		8192
#define THREAD_START_SP		(THREAD_SIZE - 8)

#ifndef __ASSEMBLY__

struct task_struct;
struct exec_domain;

#include <asm/types.h>
#include <asm/domain.h>

typedef unsigned long mm_segment_t;

/** 20150118    
 * cpu 문맥을 저장할 구조체.
 **/
struct cpu_context_save {
	__u32	r4;
	__u32	r5;
	__u32	r6;
	__u32	r7;
	__u32	r8;
	__u32	r9;
	__u32	sl;
	__u32	fp;
	__u32	sp;
	__u32	pc;
	__u32	extra[2];		/* Xscale 'acc' register, etc */
};

/*
 * low level task data that entry.S needs immediate access to.
 * __switch_to() assumes cpu_context follows immediately after cpu_domain.
 */
/** 20150118    
 * task 구조체와 연결된 thread_info 구조체.
 *
 * cpu : 현재 task가 실행되는 cpu 정보.
 * cpu_context : 문맥 교환시 수행 중이던 문맥 저장을 위한 공간.
 **/
struct thread_info {
	unsigned long		flags;		/* low level flags */
	/** 20131005    
	 * preempt_count가 0일 경우 선점 가능, 
	 **/
	int			preempt_count;	/* 0 => preemptable, <0 => bug */
	mm_segment_t		addr_limit;	/* address limit */
	struct task_struct	*task;		/* main task structure */
	struct exec_domain	*exec_domain;	/* execution domain */
	__u32			cpu;		/* cpu */
	__u32			cpu_domain;	/* cpu domain */
	struct cpu_context_save	cpu_context;	/* cpu context */
	__u32			syscall;	/* syscall number */
	__u8			used_cp[16];	/* thread used copro */
	unsigned long		tp_value;
	struct crunch_state	crunchstate;
	union fp_state		fpstate __attribute__((aligned(8)));
	union vfp_state		vfpstate;
#ifdef CONFIG_ARM_THUMBEE
	unsigned long		thumbee_state;	/* ThumbEE Handler Base register */
#endif
	struct restart_block	restart_block;
};
/** 20121208
왜 cpu번호는 정의되지 않고, flags=0으로 정의되어 있는지 ???
 **/
#define INIT_THREAD_INFO(tsk)						\
{									\
	.task		= &tsk,						\
	.exec_domain	= &default_exec_domain,				\
	.flags		= 0,						\
	.preempt_count	= INIT_PREEMPT_COUNT,				\
	.addr_limit	= KERNEL_DS,					\
	.cpu_domain	= domain_val(DOMAIN_USER, DOMAIN_MANAGER) |	\
			  domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) |	\
			  domain_val(DOMAIN_IO, DOMAIN_CLIENT),		\
	.restart_block	= {						\
		.fn	= do_no_restart_syscall,			\
	},								\
}

#define init_thread_info	(init_thread_union.thread_info)
#define init_stack		(init_thread_union.stack)

/*
 * how to get the thread information struct from C
 */

/** 20121208
 * __attribute_const__ : 컴파일러에게 전역 변수를 access 하지 않는다고 알려줌 
 **/
static inline struct thread_info *current_thread_info(void) __attribute_const__;

/** 20121208
 * 'current' thread의 thread_info를 리턴한다.
 *
 * thread_info는 thread의 stack에 overlay 된다.
 * Kernel stack(8KB)의 첫 주소를 추출해 thread_info구조체로 리턴한다.
 * 문맥교환시(switch_to) sp 역시 교체되므로, sp로 접근하는 것이 보장된다.
 **/
static inline struct thread_info *current_thread_info(void)
{
	/** 20121208
      5.37.1 Defining Global Register Variables 
      => http://gcc.gnu.org/onlinedocs/gcc-4.1.2/gcc/Global-Reg-Vars.html
    **/
    register unsigned long sp asm ("sp");
	return (struct thread_info *)(sp & ~(THREAD_SIZE - 1));
}

/** 20150118    
 * 특정 task가 보관 중인 문맥 중 pc, sp, fp를 가져온다.
 *
 * thread_info의 cpu_context에 문맥전환시 사용되는 레지스터들을 보관하므로
 * 여기에 접근해서 읽어온다.
 **/
#define thread_saved_pc(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.pc))
#define thread_saved_sp(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.sp))
#define thread_saved_fp(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.fp))

extern void crunch_task_disable(struct thread_info *);
extern void crunch_task_copy(struct thread_info *, void *);
extern void crunch_task_restore(struct thread_info *, void *);
extern void crunch_task_release(struct thread_info *);

extern void iwmmxt_task_disable(struct thread_info *);
extern void iwmmxt_task_copy(struct thread_info *, void *);
extern void iwmmxt_task_restore(struct thread_info *, void *);
extern void iwmmxt_task_release(struct thread_info *);
extern void iwmmxt_task_switch(struct thread_info *);

extern void vfp_sync_hwstate(struct thread_info *);
extern void vfp_flush_hwstate(struct thread_info *);

struct user_vfp;
struct user_vfp_exc;

extern int vfp_preserve_user_clear_hwstate(struct user_vfp __user *,
					   struct user_vfp_exc __user *);
extern int vfp_restore_user_hwstate(struct user_vfp __user *,
				    struct user_vfp_exc __user *);
#endif

/*
 * We use bit 30 of the preempt_count to indicate that kernel
 * preemption is occurring.  See <asm/hardirq.h>.
 */
/** 20140622    
 * kernel premption 중임을 나타내기 위한 상수값. preempt_count에 저장된다.
 **/
#define PREEMPT_ACTIVE	0x40000000

/*
 * thread information flags:
 *  TIF_SYSCALL_TRACE	- syscall trace active
 *  TIF_SYSCAL_AUDIT	- syscall auditing active
 *  TIF_SIGPENDING	- signal pending
 *  TIF_NEED_RESCHED	- rescheduling necessary
 *  TIF_NOTIFY_RESUME	- callback before returning to user
 *  TIF_USEDFPU		- FPU was used by this task this quantum (SMP)
 *  TIF_POLLING_NRFLAG	- true if poll_idle() is polling TIF_NEED_RESCHED
 */
/** 20140816    
 * thread info의 flags 속성값
 *  TIF_SYSCALL_TRACE	- syscall trace가 활성화 되어 있다.
 *  TIF_SYSCAL_AUDIT	- syscall 감사가 활성화 되어 있다.
 *  TIF_SIGPENDING		- 시그널이 대기(펜딩) 중이다.
 *  TIF_NEED_RESCHED	- rescheduling이 필요하다.
 *  TIF_NOTIFY_RESUME	- user mode 진입 전 콜백함수 호출이 필요하다.
 *  TIF_USEDFPU			- FPU was used by this task this quantum (SMP)
 *  TIF_POLLING_NRFLAG	- poll_idle()이 폴링으로 TIF_NEED_RESCHED를 검사한다.
 *
 *  TIF_NEED_RESCHED 설정되는 경우 (resched_task())
 *		- scheduler_tick()에서 프로세스가 타임슬라이스를 모두 소비한 경우
 *			: sched_class->task_tick()
 *		- try_to_wake_up()에서 현재 프로세스보다 높은 우선 순위를 가진 프로세스가
 *		  깨어난 경우
 *		    : check_preempt_curr()
 *
 **/
#define TIF_SIGPENDING		0
#define TIF_NEED_RESCHED	1
#define TIF_NOTIFY_RESUME	2	/* callback before returning to user */
#define TIF_SYSCALL_TRACE	8
#define TIF_SYSCALL_AUDIT	9
#define TIF_POLLING_NRFLAG	16
#define TIF_USING_IWMMXT	17
#define TIF_MEMDIE		18	/* is terminating due to OOM killer */
#define TIF_RESTORE_SIGMASK	20
#define TIF_SECCOMP		21
#define TIF_SWITCH_MM		22	/* deferred switch_mm */

#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_POLLING_NRFLAG	(1 << TIF_POLLING_NRFLAG)
#define _TIF_USING_IWMMXT	(1 << TIF_USING_IWMMXT)
#define _TIF_SECCOMP		(1 << TIF_SECCOMP)

/* Checks for any syscall work in entry-common.S */
#define _TIF_SYSCALL_WORK (_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT)

/*
 * Change these and you break ASM code in entry-common.S
 */
/** 20160220    
 * thread_info 중 work 관련 mask.
 **/
#define _TIF_WORK_MASK		(_TIF_NEED_RESCHED | _TIF_SIGPENDING | _TIF_NOTIFY_RESUME)

#endif /* __KERNEL__ */
#endif /* __ASM_ARM_THREAD_INFO_H */
