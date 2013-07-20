#ifndef __ASM_ARM_CMPXCHG_H
#define __ASM_ARM_CMPXCHG_H

#include <linux/irqflags.h>
#include <asm/barrier.h>

#if defined(CONFIG_CPU_SA1100) || defined(CONFIG_CPU_SA110)
/*
 * On the StrongARM, "swp" is terminally broken since it bypasses the
 * cache totally.  This means that the cache becomes inconsistent, and,
 * since we use normal loads/stores as well, this is really bad.
 * Typically, this causes oopsen in filp_close, but could have other,
 * more disastrous effects.  There are two work-arounds:
 *  1. Disable interrupts and emulate the atomic swap
 *  2. Clean the cache, perform atomic swap, flush the cache
 *
 * We choose (1) since its the "easiest" to achieve here and is not
 * dependent on the processor type.
 *
 * NOTE that this solution won't work on an SMP system, so explcitly
 * forbid it here.
 */
#define swp_is_buggy
#endif

/** 20130713    
 * ptr를 x로 변경하고 이전 값을 return.
 *
 * x   : 변경할 값
 * ptr : 변경될 메모리 주소
 * size: ptr의 크기
 **/
static inline unsigned long __xchg(unsigned long x, volatile void *ptr, int size)
{
	extern void __bad_xchg(volatile void *, int);
	unsigned long ret;
#ifdef swp_is_buggy
	unsigned long flags;
#endif
#if __LINUX_ARM_ARCH__ >= 6
	unsigned int tmp;
#endif

	/** 20130713    
	 * dmb();
	 **/
	smp_mb();

	switch (size) {
#if __LINUX_ARM_ARCH__ >= 6
	case 1:
		asm volatile("@	__xchg1\n"
		"1:	ldrexb	%0, [%3]\n"
		"	strexb	%1, %2, [%3]\n"
		"	teq	%1, #0\n"
		"	bne	1b"
			: "=&r" (ret), "=&r" (tmp)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
	/** 20130713    
	 * ex) atomic_t 
	 *
	 * %0  : ret   - ptr에서 읽어온 값(이전값)
	 * %1  : tmp   - exclusive 상태값
	 * %2  : x     - *ptr에 저장할 값(새로운 값)
	 * %3  : ptr
	 **/
	case 4:
		asm volatile("@	__xchg4\n"
		"1:	ldrex	%0, [%3]\n"
		"	strex	%1, %2, [%3]\n"
		"	teq	%1, #0\n"
		"	bne	1b"
			: "=&r" (ret), "=&r" (tmp)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
#elif defined(swp_is_buggy)
#ifdef CONFIG_SMP
#error SMP is not supported on this platform
#endif
	case 1:
		raw_local_irq_save(flags);
		ret = *(volatile unsigned char *)ptr;
		*(volatile unsigned char *)ptr = x;
		raw_local_irq_restore(flags);
		break;

	case 4:
		raw_local_irq_save(flags);
		ret = *(volatile unsigned long *)ptr;
		*(volatile unsigned long *)ptr = x;
		raw_local_irq_restore(flags);
		break;
#else
	case 1:
		asm volatile("@	__xchg1\n"
		"	swpb	%0, %1, [%2]"
			: "=&r" (ret)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
	case 4:
		asm volatile("@	__xchg4\n"
		"	swp	%0, %1, [%2]"
			: "=&r" (ret)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
#endif
	default:
		__bad_xchg(ptr, size), ret = 0;
		break;
	}
	/** 20130713    
	 * dmb()
	 **/
	smp_mb();

	return ret;
}

/** 20130713    
 * __xchg()를 수행하고, 이전 값을 *ptr의 자료형으로 리턴.
 **/
#define xchg(ptr,x) \
	((__typeof__(*(ptr)))__xchg((unsigned long)(x),(ptr),sizeof(*(ptr))))

#include <asm-generic/cmpxchg-local.h>

#if __LINUX_ARM_ARCH__ < 6
/* min ARCH < ARMv6 */

#ifdef CONFIG_SMP
#error "SMP is not supported on this platform"
#endif

/*
 * cmpxchg_local and cmpxchg64_local are atomic wrt current CPU. Always make
 * them available.
 */
#define cmpxchg_local(ptr, o, n)				  	       \
	((__typeof__(*(ptr)))__cmpxchg_local_generic((ptr), (unsigned long)(o),\
			(unsigned long)(n), sizeof(*(ptr))))
#define cmpxchg64_local(ptr, o, n) __cmpxchg64_local_generic((ptr), (o), (n))

#ifndef CONFIG_SMP
#include <asm-generic/cmpxchg.h>
#endif

#else	/* min ARCH >= ARMv6 */

extern void __bad_cmpxchg(volatile void *ptr, int size);

/*
 * cmpxchg only support 32-bits operands on ARMv6.
 */

/** 20130720    
 * compare and exchange - 원자적 수행
 **/
static inline unsigned long __cmpxchg(volatile void *ptr, unsigned long old,
				      unsigned long new, int size)
{
	unsigned long oldval, res;

	switch (size) {
#ifndef CONFIG_CPU_V6	/* min ARCH >= ARMv6K */
	case 1:
		do {
			asm volatile("@ __cmpxchg1\n"
			"	ldrexb	%1, [%2]\n"
			"	mov	%0, #0\n"
			"	teq	%1, %3\n"
			"	strexbeq %0, %4, [%2]\n"
				: "=&r" (res), "=&r" (oldval)
				: "r" (ptr), "Ir" (old), "r" (new)
				: "memory", "cc");
		} while (res);
		break;
	case 2:
		do {
			asm volatile("@ __cmpxchg1\n"
			"	ldrexh	%1, [%2]\n"
			"	mov	%0, #0\n"
			"	teq	%1, %3\n"
			"	strexheq %0, %4, [%2]\n"
				: "=&r" (res), "=&r" (oldval)
				: "r" (ptr), "Ir" (old), "r" (new)
				: "memory", "cc");
		} while (res);
		break;
#endif
	case 4:
		/** 20130720    
		 * %0  : res
		 * %1  : oldval
		 * %2  : ptr
		 * %3  : old
		 * %4  : new
		 *
		 *                    do {
		 * ldrex	%1, [%2]      oldval = *ptr;
		 * mov	%0, #0            res = 0;
		 * teq	%1, %3            if (oldval == old)
		 * strexeq %0, %4, [%2]     *ptr = new;
		 *                    } while (res)
		 * 1. *ptr의 값이 old와 같지 않을 경우, res = 0으로 저장하지 않고 리턴
		 * 2. 같을 경우,
		 *      exclusive가 보장될 경우 - new 값으로 *ptr에 저장
		 *      보장되지 않을 경우      - 다시 시도 while(1)
		 **/
		do {
			asm volatile("@ __cmpxchg4\n"
			"	ldrex	%1, [%2]\n"
			"	mov	%0, #0\n"
			"	teq	%1, %3\n"
			"	strexeq %0, %4, [%2]\n"
				: "=&r" (res), "=&r" (oldval)
				: "r" (ptr), "Ir" (old), "r" (new)
				: "memory", "cc");
		} while (res);
		break;
	default:
		__bad_cmpxchg(ptr, size);
		oldval = 0;
	}

	/** 20130720    
	 * *ptr에서 읽은 값 리턴
	 **/
	return oldval;
}

/** 20130720    
 * __cmpxchg 전후에 memory barrier를 두는 함수.
 * 리턴 값은 *ptr에 현재 저장된 값
 **/
static inline unsigned long __cmpxchg_mb(volatile void *ptr, unsigned long old,
					 unsigned long new, int size)
{
	unsigned long ret;

	/** 20130720    
	 * __cmpxchg 전후에 dmb()
	 **/
	smp_mb();
	ret = __cmpxchg(ptr, old, new, size);
	smp_mb();

	return ret;
}

/** 20130720    
 * __cmpxchg_mb 호출.
 **/
#define cmpxchg(ptr,o,n)						\
	((__typeof__(*(ptr)))__cmpxchg_mb((ptr),			\
					  (unsigned long)(o),		\
					  (unsigned long)(n),		\
					  sizeof(*(ptr))))

static inline unsigned long __cmpxchg_local(volatile void *ptr,
					    unsigned long old,
					    unsigned long new, int size)
{
	unsigned long ret;

	switch (size) {
#ifdef CONFIG_CPU_V6	/* min ARCH == ARMv6 */
	case 1:
	case 2:
		ret = __cmpxchg_local_generic(ptr, old, new, size);
		break;
#endif
	default:
		ret = __cmpxchg(ptr, old, new, size);
	}

	return ret;
}

#define cmpxchg_local(ptr,o,n)						\
	((__typeof__(*(ptr)))__cmpxchg_local((ptr),			\
				       (unsigned long)(o),		\
				       (unsigned long)(n),		\
				       sizeof(*(ptr))))

#define cmpxchg64(ptr, o, n)						\
	((__typeof__(*(ptr)))atomic64_cmpxchg(container_of((ptr),	\
						atomic64_t,		\
						counter),		\
					      (unsigned long)(o),	\
					      (unsigned long)(n)))

#define cmpxchg64_local(ptr, o, n)					\
	((__typeof__(*(ptr)))local64_cmpxchg(container_of((ptr),	\
						local64_t,		\
						a),			\
					     (unsigned long)(o),	\
					     (unsigned long)(n)))

#endif	/* __LINUX_ARM_ARCH__ >= 6 */

#endif /* __ASM_ARM_CMPXCHG_H */
