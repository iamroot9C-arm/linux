#ifndef __ASM_GENERIC_GETORDER_H
#define __ASM_GENERIC_GETORDER_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/log2.h>

/*
 * Runtime evaluation of get_order()
 */
/** 20130907    
 * get_order()의 runtime 계산 버전.
 **/
static inline __attribute_const__
int __get_order(unsigned long size)
{
	int order;

	size--;
	size >>= PAGE_SHIFT;
#if BITS_PER_LONG == 32
	order = fls(size);
#else
	order = fls64(size);
#endif
	return order;
}

/**
 * get_order - Determine the allocation order of a memory size
 * @size: The size for which to get the order
 *
 * Determine the allocation order of a particular sized block of memory.  This
 * is on a logarithmic scale, where:
 *
 *	0 -> 2^0 * PAGE_SIZE and below
 *	1 -> 2^1 * PAGE_SIZE to 2^0 * PAGE_SIZE + 1
 *	2 -> 2^2 * PAGE_SIZE to 2^1 * PAGE_SIZE + 1
 *	3 -> 2^3 * PAGE_SIZE to 2^2 * PAGE_SIZE + 1
 *	4 -> 2^4 * PAGE_SIZE to 2^3 * PAGE_SIZE + 1
 *	...
 *
 * The order returned is used to find the smallest allocation granule required
 * to hold an object of the specified size.
 *
 * The result is undefined if the size is 0.
 *
 * This function may be used to initialise variables with compile time
 * evaluations of constants.
 */
/** 20130907    
 * n이라는 크기가 필요할 때
 * PAGE_SIZE * (2 ** x) 로 표현할 수 있는 x의 최소 크기가 order로 리턴된다.
 * 
 * 0    -> undefined. 컴파일시에 계산되는 값이면 20
 * 4096 -> 0
 * 8193 -> 2
 *
 *  2^0 * PAGE_SIZE and below                 ->   0
 *	2^0 * PAGE_SIZE + 1  ~  2^1 * PAGE_SIZE   ->   1
 *	2^1 * PAGE_SIZE + 1  ~  2^2 * PAGE_SIZE   ->   2
 *	2^2 * PAGE_SIZE + 1  ~  2^3 * PAGE_SIZE   ->   3
 *	2^3 * PAGE_SIZE + 1  ~  2^4 * PAGE_SIZE   ->   4
 *
 **/
#define get_order(n)						\
(								\
	__builtin_constant_p(n) ? (				\
		((n) == 0UL) ? BITS_PER_LONG - PAGE_SHIFT :	\
		(((n) < (1UL << PAGE_SHIFT)) ? 0 :		\
		 ilog2((n) - 1) - PAGE_SHIFT + 1)		\
	) :							\
	__get_order(n)						\
)

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_GENERIC_GETORDER_H */
