/*
 *  arch/arm/include/asm/atomic.h
 *
 *  Copyright (C) 1996 Russell King.
 *  Copyright (C) 2002 Deep Blue Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_ARM_ATOMIC_H
#define __ASM_ARM_ATOMIC_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/irqflags.h>
#include <asm/barrier.h>
#include <asm/cmpxchg.h>

/** 20130706    
 * 단순히 i를 리턴.
 **/
#define ATOMIC_INIT(i)	{ (i) }

#ifdef __KERNEL__

/** 20121103
	ARMv6K부터 atomic 연산을 위해서 ldrex, strex 명령을 제공한다.
	
	The L1 memory system of the Cortex-A9 processor has a local monitor. This is a 2-state, open
	and exclusive, state machine that manages load/store exclusive (LDREXB, LDREXH, LDREX, LDREXD,
	STREXB, STREXH, STREX and STREXD) accesses and clear exclusive (CLREX) instructions.

		exclusive access state
		open access state 

	ldrex (load exclusive)
	strex (store exclusive)
		ldrex, strex는 항상 짝으로 사용되어 동기화 연산을 보장한다. 
		특정 address 에 대한 ldrex는 해당 메모리의 상태를 exclusive access state 로 변경한다. 
		이후, 그 주소에 대한 strex는 해당 메모리의 상태가 
			- exclusive 이면, str를 수행하고 0(정상)을 리턴한다.  
			- open 이면, 1(실패)을 리턴한다. 아래 atomic_add 등에서는 이 경우, 다시 ldrex 부터 수행한다. 
		
		만약, 
			context1 				context2 		 
		  1 atomic_add(a, 1)  |  1 atomic_add(a, 2) |
		  2                   |  2                  |
		  3                   |  3                  |
		  4                   |  4                  |
		  5 ldrex             |  5                  |
		  6                   |  6 ldrex            |
		  7                   |  7 strex            |
		  8 strex             |~                    | open state이므로 fail, reload시, a = 1 + 2 = 3이 저장됨.
			두 개의 thread(process)에서 atomic_add의 접근시에 원자성이 보장된다.

	clrex(exclusive -> open) 의 필요성.
			context1 				context2 			context3
		  1 atomic_add(a, 1)  |  1 atomic_add(a, 2) |  1 atomic_add(a, 3)|
		  2                   |  2                  |  2                 |
		  3                   |  3                  |  3                 |
		  4                   |  4                  |  4                 |
		  5 ldrex             |  5                  |  5 				 |  exclusive state 
		  6                   |  6 ldrex            |  6                 |	exclusive state
		  7                   |  7 strex            |  7                 |	open state			a = 2
		  8                   |~                    |  8 ldrex           |	exclusive state
		  9 strex             |~                    |  9                 |	open state			a = 1
		~                     |~                    | 10 strex           |	open state 이므로 fail, reload 시, a = 1 + 3 = 4가 저장됨.
	
		이런 문제를 방지하기 위해서 context switching시에 clrex(exclusive -> open)를 수행하여, 데이터의 원자성이 깨지지 않도록 한다. 

 * 참고.
	TRM: 7.4 About the L1 data side memory system
	ARM: A3.4.1 Exclusive access instructions and Non-shareable memory regions
	http://www.iamroot.org/xe/66152
 */
/** 20130706    
 * 코드에서 clrex가 실제 호출되는 곳은 arch/arm/kernel/entry-header.S의
 * svc_exit, restore_user_regs이다.
 *     -> context switching이 발생하는 시점은 scheduler가 불리는 시점이므로 svc_exit를 호출할 것이므로 svc_exit 안에 clrex를 넣어준다.
 **/
 /** 20130706    
  * ARM에서 일반적인 str 명령은 local exclusive monitor를 clear하지 않는다.
  * ldr/str 명령은 그 자체로 atomic 하다.
  * 모든 exception return시에 clrex나 dummy strex (ARMv6 이전)가 수행된다.
  **/
/*
 * On ARM, ordinary assignment (str instruction) doesn't clear the local
 * strex/ldrex monitor on some implementations. The reason we can use it for
 * atomic_set() is the clrex or dummy strex done on every exception return.
 */
#define atomic_read(v)	(*(volatile int *)&(v)->counter)
#define atomic_set(v,i)	(((v)->counter) = (i))

#if __LINUX_ARM_ARCH__ >= 6

/*
 * ARMv6 UP and SMP safe atomic ops.  We use load exclusive and
 * store exclusive to ensure that these are atomic.  We may loop
 * to ensure that the update happens.
 */
/** 20121110
 *
 * UP/ SMP에서 모두 사용할 수 있는 원자적 명령 (atomic operation)
  참조 사이트 
http://www.ethernut.de/en/documents/arm-inline-asm.html
http://ibiblio.org/gferg/ldp/GCC-Inline-Assembly-HOWTO.html#ss5.3

아래 코드를 풀어보면..
ldrex result, &v->counter
add result, result, i
strex tmp, result, &v->counter

Constraints
= : 쓰기전용
& : ??? 
+ : read / write 속성
Q :  (arm specific) ???
o : 오프셋화 가능한 주소를 나타난다???
r : general register ( r0 ~ r15)
I : Immediate value in data processing instructions(Arm state)
e.g. ORR R0, R0, #operand
Ir 같이 쓰이고 있는데 어떤 의미일까???

cc : 명령어가 condition 코드 레지스터를 변경할 경우에 사용한다.
eg) teq,subs 

+Qo : 만들어놓고 안쓰고 있다 어떤 의미가 있을까???

http://gcc.gnu.org/onlinedocs/gcc/Machine-Constraints.html#Machine-Constraints

 **/
static inline void atomic_add(int i, atomic_t *v)
{
	unsigned long tmp;
	int result;

	__asm__ __volatile__("@ atomic_add\n"
"1:	ldrex	%0, [%3]\n"
"	add	%0, %0, %4\n"
"	strex	%1, %0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "Ir" (i)
	: "cc");
}

/** 20140517    
 * ldrex/strex 로 atomic 하게 증가시키고 이전 값 리턴.
 *
 * 연산 전,후로 memory barrier를 두어 메모리 접근 연산의 순서를 보장한다.
 **/
static inline int atomic_add_return(int i, atomic_t *v)
{
	unsigned long tmp;
	int result;

	smp_mb();

	__asm__ __volatile__("@ atomic_add_return\n"
"1:	ldrex	%0, [%3]\n"
"	add	%0, %0, %4\n"
"	strex	%1, %0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "Ir" (i)
	: "cc");

	smp_mb();

	return result;
}

static inline void atomic_sub(int i, atomic_t *v)
{
	unsigned long tmp;
	int result;

	__asm__ __volatile__("@ atomic_sub\n"
"1:	ldrex	%0, [%3]\n"
"	sub	%0, %0, %4\n"
"	strex	%1, %0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "Ir" (i)
	: "cc");
}

/** 20130803    
 * ARMv6 이상
 *
 * %0 : result
 * %1 : tmp    (ex 검사용)
 * %2 : v->counter
 * %3 : &v->counter
 * %4 : i
 *
 * LOCK();
 * result = *(&v->counter);
 * result = result - i;
 * *(&v->counter) = result;
 * UNLOCK();
 *
 **/
static inline int atomic_sub_return(int i, atomic_t *v)
{
	unsigned long tmp;
	int result;

	smp_mb();

	__asm__ __volatile__("@ atomic_sub_return\n"
"1:	ldrex	%0, [%3]\n"
"	sub	%0, %0, %4\n"
"	strex	%1, %0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "Ir" (i)
	: "cc");

	smp_mb();

	return result;
}

/** 20130706    
 * vexpress는 __LINUX_ARM_ARCH__가 7.
 * old->new 변경을 원자적으로 실행하고, ptr->counter를 리턴.
 **/
static inline int atomic_cmpxchg(atomic_t *ptr, int old, int new)
{
	unsigned long oldval, res;

	/** 20130706    
	 * memory barrier를 수행
	 **/
	smp_mb();

	/** 20130706    
	 * http://gcc.gnu.org/onlinedocs/gcc/Machine-Constraints.html#Machine-Constraints
	 * %0 : res
	 * %1 : oldval
	 * %2 : ptr->counter
	 * %3 : &ptr->counter
	 * %4 : old
	 * %5 : new
	 * do  {
	 *		oldval = *(&ptr->counter);
	 *		res = 0;
	 *		if (oldval == old) {			// compare
	 *			*(&ptr->counter) = new;
	 *			res = (is_touched);
	 *		}
	 *	} while (res);
	 *
	 * do {
	 *		"ldrex	oldval, [&ptr->counter]\n"
	 *		"mov	res, #0\n"
	 *		"teq	oldval, old\n"
	 *		"strexeq res, new, [&ptr->counter]\n"
	 * } while (res);
	 *
	 * 만약 첫번째 수행에서 oldval이 old와 다르다면,
	 * res는 0이므로 loop을 돌지 않고 빠져나간다.
	 **/
	do {
		__asm__ __volatile__("@ atomic_cmpxchg\n"
		"ldrex	%1, [%3]\n"
		"mov	%0, #0\n"
		"teq	%1, %4\n"
		"strexeq %0, %5, [%3]\n"
		    : "=&r" (res), "=&r" (oldval), "+Qo" (ptr->counter)
		    : "r" (&ptr->counter), "Ir" (old), "r" (new)
		    : "cc");
	} while (res);

	/** 20130706    
	 * memory barrier를 수행
	 **/
	smp_mb();

	/** 20130706    
	 * 이전 값 리턴
	 **/
	return oldval;
}

static inline void atomic_clear_mask(unsigned long mask, unsigned long *addr)
{
	unsigned long tmp, tmp2;

	__asm__ __volatile__("@ atomic_clear_mask\n"
"1:	ldrex	%0, [%3]\n"
"	bic	%0, %0, %4\n"
"	strex	%1, %0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (tmp), "=&r" (tmp2), "+Qo" (*addr)
	: "r" (addr), "Ir" (mask)
	: "cc");
}

#else /* ARM_ARCH_6 */

#ifdef CONFIG_SMP
#error SMP not supported on pre-ARMv6 CPUs
#endif

static inline int atomic_add_return(int i, atomic_t *v)
{
	unsigned long flags;
	int val;

	raw_local_irq_save(flags);
	val = v->counter;
	v->counter = val += i;
	raw_local_irq_restore(flags);

	return val;
}
#define atomic_add(i, v)	(void) atomic_add_return(i, v)

static inline int atomic_sub_return(int i, atomic_t *v)
{
	unsigned long flags;
	int val;

	raw_local_irq_save(flags);
	val = v->counter;
	v->counter = val -= i;
	raw_local_irq_restore(flags);

	return val;
}
#define atomic_sub(i, v)	(void) atomic_sub_return(i, v)

static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	int ret;
	unsigned long flags;

	raw_local_irq_save(flags);
	ret = v->counter;
	if (likely(ret == old))
		v->counter = new;
	raw_local_irq_restore(flags);

	return ret;
}

static inline void atomic_clear_mask(unsigned long mask, unsigned long *addr)
{
	unsigned long flags;

	raw_local_irq_save(flags);
	*addr &= ~mask;
	raw_local_irq_restore(flags);
}

#endif /* __LINUX_ARM_ARCH__ */

/** 20130713    
 **/
#define atomic_xchg(v, new) (xchg(&((v)->counter), new))

/** 20140111
 * v가 u와 다른경우, v값에 a만큼을 더해서 저장하고 이전값(v)을 리턴한다.
 **/
static inline int __atomic_add_unless(atomic_t *v, int a, int u)
{
	int c, old;

	c = atomic_read(v);
	while (c != u && (old = atomic_cmpxchg((v), c, c + a)) != c)
		c = old;
	return c;
}

/** 20140111
 **/
#define atomic_inc(v)		atomic_add(1, v)
#define atomic_dec(v)		atomic_sub(1, v)

#define atomic_inc_and_test(v)	(atomic_add_return(1, v) == 0)
/** 20130803    
 * atomic_sub_result을 이용해 v를 1 감소시키고, 감소시킨 결과가 0인지 리턴
 **/
#define atomic_dec_and_test(v)	(atomic_sub_return(1, v) == 0)
#define atomic_inc_return(v)    (atomic_add_return(1, v))
#define atomic_dec_return(v)    (atomic_sub_return(1, v))
#define atomic_sub_and_test(i, v) (atomic_sub_return(i, v) == 0)

#define atomic_add_negative(i,v) (atomic_add_return(i, v) < 0)

#define smp_mb__before_atomic_dec()	smp_mb()
#define smp_mb__after_atomic_dec()	smp_mb()
#define smp_mb__before_atomic_inc()	smp_mb()
#define smp_mb__after_atomic_inc()	smp_mb()

#ifndef CONFIG_GENERIC_ATOMIC64
typedef struct {
	u64 __aligned(8) counter;
} atomic64_t;

#define ATOMIC64_INIT(i) { (i) }

static inline u64 atomic64_read(const atomic64_t *v)
{
	u64 result;

	__asm__ __volatile__("@ atomic64_read\n"
"	ldrexd	%0, %H0, [%1]"
	: "=&r" (result)
	: "r" (&v->counter), "Qo" (v->counter)
	);

	return result;
}

static inline void atomic64_set(atomic64_t *v, u64 i)
{
	u64 tmp;

	__asm__ __volatile__("@ atomic64_set\n"
"1:	ldrexd	%0, %H0, [%2]\n"
"	strexd	%0, %3, %H3, [%2]\n"
"	teq	%0, #0\n"
"	bne	1b"
	: "=&r" (tmp), "=Qo" (v->counter)
	: "r" (&v->counter), "r" (i)
	: "cc");
}

static inline void atomic64_add(u64 i, atomic64_t *v)
{
	u64 result;
	unsigned long tmp;

	__asm__ __volatile__("@ atomic64_add\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	adds	%0, %0, %4\n"
"	adc	%H0, %H0, %H4\n"
"	strexd	%1, %0, %H0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "r" (i)
	: "cc");
}

static inline u64 atomic64_add_return(u64 i, atomic64_t *v)
{
	u64 result;
	unsigned long tmp;

	smp_mb();

	__asm__ __volatile__("@ atomic64_add_return\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	adds	%0, %0, %4\n"
"	adc	%H0, %H0, %H4\n"
"	strexd	%1, %0, %H0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "r" (i)
	: "cc");

	smp_mb();

	return result;
}

static inline void atomic64_sub(u64 i, atomic64_t *v)
{
	u64 result;
	unsigned long tmp;

	__asm__ __volatile__("@ atomic64_sub\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	subs	%0, %0, %4\n"
"	sbc	%H0, %H0, %H4\n"
"	strexd	%1, %0, %H0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "r" (i)
	: "cc");
}

static inline u64 atomic64_sub_return(u64 i, atomic64_t *v)
{
	u64 result;
	unsigned long tmp;

	smp_mb();

	__asm__ __volatile__("@ atomic64_sub_return\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	subs	%0, %0, %4\n"
"	sbc	%H0, %H0, %H4\n"
"	strexd	%1, %0, %H0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "r" (i)
	: "cc");

	smp_mb();

	return result;
}

static inline u64 atomic64_cmpxchg(atomic64_t *ptr, u64 old, u64 new)
{
	u64 oldval;
	unsigned long res;

	smp_mb();

	do {
		__asm__ __volatile__("@ atomic64_cmpxchg\n"
		"ldrexd		%1, %H1, [%3]\n"
		"mov		%0, #0\n"
		"teq		%1, %4\n"
		"teqeq		%H1, %H4\n"
		"strexdeq	%0, %5, %H5, [%3]"
		: "=&r" (res), "=&r" (oldval), "+Qo" (ptr->counter)
		: "r" (&ptr->counter), "r" (old), "r" (new)
		: "cc");
	} while (res);

	smp_mb();

	return oldval;
}

static inline u64 atomic64_xchg(atomic64_t *ptr, u64 new)
{
	u64 result;
	unsigned long tmp;

	smp_mb();

	__asm__ __volatile__("@ atomic64_xchg\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	strexd	%1, %4, %H4, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (ptr->counter)
	: "r" (&ptr->counter), "r" (new)
	: "cc");

	smp_mb();

	return result;
}

static inline u64 atomic64_dec_if_positive(atomic64_t *v)
{
	u64 result;
	unsigned long tmp;

	smp_mb();

	__asm__ __volatile__("@ atomic64_dec_if_positive\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	subs	%0, %0, #1\n"
"	sbc	%H0, %H0, #0\n"
"	teq	%H0, #0\n"
"	bmi	2f\n"
"	strexd	%1, %0, %H0, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b\n"
"2:"
	: "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter)
	: "cc");

	smp_mb();

	return result;
}

static inline int atomic64_add_unless(atomic64_t *v, u64 a, u64 u)
{
	u64 val;
	unsigned long tmp;
	int ret = 1;

	smp_mb();

	__asm__ __volatile__("@ atomic64_add_unless\n"
"1:	ldrexd	%0, %H0, [%4]\n"
"	teq	%0, %5\n"
"	teqeq	%H0, %H5\n"
"	moveq	%1, #0\n"
"	beq	2f\n"
"	adds	%0, %0, %6\n"
"	adc	%H0, %H0, %H6\n"
"	strexd	%2, %0, %H0, [%4]\n"
"	teq	%2, #0\n"
"	bne	1b\n"
"2:"
	: "=&r" (val), "+r" (ret), "=&r" (tmp), "+Qo" (v->counter)
	: "r" (&v->counter), "r" (u), "r" (a)
	: "cc");

	if (ret)
		smp_mb();

	return ret;
}

#define atomic64_add_negative(a, v)	(atomic64_add_return((a), (v)) < 0)
#define atomic64_inc(v)			atomic64_add(1LL, (v))
#define atomic64_inc_return(v)		atomic64_add_return(1LL, (v))
#define atomic64_inc_and_test(v)	(atomic64_inc_return(v) == 0)
#define atomic64_sub_and_test(a, v)	(atomic64_sub_return((a), (v)) == 0)
#define atomic64_dec(v)			atomic64_sub(1LL, (v))
#define atomic64_dec_return(v)		atomic64_sub_return(1LL, (v))
#define atomic64_dec_and_test(v)	(atomic64_dec_return((v)) == 0)
#define atomic64_inc_not_zero(v)	atomic64_add_unless((v), 1LL, 0LL)

#endif /* !CONFIG_GENERIC_ATOMIC64 */
#endif
#endif
