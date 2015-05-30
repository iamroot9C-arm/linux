#ifndef __LINUX_CPUMASK_H
#define __LINUX_CPUMASK_H

/*
 * Cpumasks provide a bitmap suitable for representing the
 * set of CPU's in a system, one bit position per CPU number.  In general,
 * only nr_cpu_ids (<= NR_CPUS) bits are valid.
 */
#include <linux/kernel.h>
#include <linux/threads.h>
#include <linux/bitmap.h>
#include <linux/bug.h>

/** 20121208
 * DECLARE_BITMAP(bits, NR_CPUS)
 * => unsigned long bits[1]   (NR_CPUS=4일경우)
 **/
typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

/**
 * cpumask_bits - get the bits in a cpumask
 * @maskp: the struct cpumask *
 *
 * You should only assume nr_cpu_ids bits of this mask are valid.  This is
 * a macro so it's const-correct.
 */
/** 20130518    
 * struct cpumask 선언시 bits라는 멤버를 macro로 정의
 **/
#define cpumask_bits(maskp) ((maskp)->bits)

#if NR_CPUS == 1
#define nr_cpu_ids		1
#else
extern int nr_cpu_ids;
#endif

#ifdef CONFIG_CPUMASK_OFFSTACK
/* Assuming NR_CPUS is huge, a runtime limit is more efficient.  Also,
 * not all bits may be allocated. */
#define nr_cpumask_bits	nr_cpu_ids
#else
/** 20130608    
 * nr_cpumask_bits = 4
 **/
#define nr_cpumask_bits	NR_CPUS
#endif

/*
 * The following particular system cpumasks and operations manage
 * possible, present, active and online cpus.
 *
 *     cpu_possible_mask- has bit 'cpu' set iff cpu is populatable
 *     cpu_present_mask - has bit 'cpu' set iff cpu is populated
 *     cpu_online_mask  - has bit 'cpu' set iff cpu available to scheduler
 *     cpu_active_mask  - has bit 'cpu' set iff cpu available to migration
 *
 *  If !CONFIG_HOTPLUG_CPU, present == possible, and active == online.
 *
 *  The cpu_possible_mask is fixed at boot time, as the set of CPU id's
 *  that it is possible might ever be plugged in at anytime during the
 *  life of that system boot.  The cpu_present_mask is dynamic(*),
 *  representing which CPUs are currently plugged in.  And
 *  cpu_online_mask is the dynamic subset of cpu_present_mask,
 *  indicating those CPUs available for scheduling.
 *
 *  If HOTPLUG is enabled, then cpu_possible_mask is forced to have
 *  all NR_CPUS bits set, otherwise it is just the set of CPUs that
 *  ACPI reports present at boot.
 *
 *  If HOTPLUG is enabled, then cpu_present_mask varies dynamically,
 *  depending on what ACPI reports as currently plugged in, otherwise
 *  cpu_present_mask is just a copy of cpu_possible_mask.
 *
 *  (*) Well, cpu_present_mask is dynamic in the hotplug case.  If not
 *      hotplug, it's a copy of cpu_possible_mask, hence fixed at boot.
 *
 * Subtleties:
 * 1) UP arch's (NR_CPUS == 1, CONFIG_SMP not defined) hardcode
 *    assumption that their single CPU is online.  The UP
 *    cpu_{online,possible,present}_masks are placebos.  Changing them
 *    will have no useful affect on the following num_*_cpus()
 *    and cpu_*() macros in the UP case.  This ugliness is a UP
 *    optimization - don't waste any instructions or memory references
 *    asking if you're online or how many CPUs there are if there is
 *    only one CPU.
 */

extern const struct cpumask *const cpu_possible_mask;
extern const struct cpumask *const cpu_online_mask;
extern const struct cpumask *const cpu_present_mask;
extern const struct cpumask *const cpu_active_mask;

/** 20130608    
 * cpumask_weight(...)
 * 각각의 bitmap에 대해 weight (1로 설정된 비트의 수)를 구함
 *
 * 20130720
 *   예를 들어 num_online_cpus는 cpu_online_mask에 설정된 1의 개수를 세서 리턴.
 **/
#if NR_CPUS > 1
#define num_online_cpus()	cpumask_weight(cpu_online_mask)
#define num_possible_cpus()	cpumask_weight(cpu_possible_mask)
#define num_present_cpus()	cpumask_weight(cpu_present_mask)
#define num_active_cpus()	cpumask_weight(cpu_active_mask)
#define cpu_online(cpu)		cpumask_test_cpu((cpu), cpu_online_mask)
#define cpu_possible(cpu)	cpumask_test_cpu((cpu), cpu_possible_mask)
#define cpu_present(cpu)	cpumask_test_cpu((cpu), cpu_present_mask)
#define cpu_active(cpu)		cpumask_test_cpu((cpu), cpu_active_mask)
#else
#define num_online_cpus()	1U
#define num_possible_cpus()	1U
#define num_present_cpus()	1U
#define num_active_cpus()	1U
#define cpu_online(cpu)		((cpu) == 0)
#define cpu_possible(cpu)	((cpu) == 0)
#define cpu_present(cpu)	((cpu) == 0)
#define cpu_active(cpu)		((cpu) == 0)
#endif

/* verify cpu argument to cpumask_* operators */
/** 20130518    
 * argument cpu에 대한 DEBUG용 함수
 **/
static inline unsigned int cpumask_check(unsigned int cpu)
{
#ifdef CONFIG_DEBUG_PER_CPU_MAPS
	WARN_ON_ONCE(cpu >= nr_cpumask_bits);
#endif /* CONFIG_DEBUG_PER_CPU_MAPS */
	return cpu;
}

#if NR_CPUS == 1
/* Uniprocessor.  Assume all masks are "1". */
static inline unsigned int cpumask_first(const struct cpumask *srcp)
{
	return 0;
}

/* Valid inputs for n are -1 and 0. */
static inline unsigned int cpumask_next(int n, const struct cpumask *srcp)
{
	return n+1;
}

static inline unsigned int cpumask_next_zero(int n, const struct cpumask *srcp)
{
	return n+1;
}

static inline unsigned int cpumask_next_and(int n,
					    const struct cpumask *srcp,
					    const struct cpumask *andp)
{
	return n+1;
}

/* cpu must be a valid cpu, ie 0, so there's no other choice. */
static inline unsigned int cpumask_any_but(const struct cpumask *mask,
					   unsigned int cpu)
{
	return 1;
}

#define for_each_cpu(cpu, mask)			\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask)
#define for_each_cpu_not(cpu, mask)		\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask)
#define for_each_cpu_and(cpu, mask, and)	\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask, (void)and)
#else
/**
 * cpumask_first - get the first cpu in a cpumask
 * @srcp: the cpumask pointer
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
static inline unsigned int cpumask_first(const struct cpumask *srcp)
{
	return find_first_bit(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_next - get the next cpu in a cpumask
 * @n: the cpu prior to the place to search (ie. return will be > @n)
 * @srcp: the cpumask pointer
 *
 * Returns >= nr_cpu_ids if no further cpus set.
 */
/** 20130608    
 * 제공된 cpu 마스크에서 index n 이후 처음으로 세팅된 bit를 찾아 인덱스를 리턴.
 * 최초 호출시에는 n에 -1을 넣는다.
 *
 * NR_CPUS = 4 (SMP) 이므로 이 부분 수행
 **/
static inline unsigned int cpumask_next(int n, const struct cpumask *srcp)
{
	/* -1 is a legal arg here. */
	if (n != -1)
		cpumask_check(n);
	/** 20130608    
	 * _find_next_bit_le : lsb부터 처음 1이 나오는 bit index 리턴.
	 **/
	return find_next_bit(cpumask_bits(srcp), nr_cpumask_bits, n+1);
}

/**
 * cpumask_next_zero - get the next unset cpu in a cpumask
 * @n: the cpu prior to the place to search (ie. return will be > @n)
 * @srcp: the cpumask pointer
 *
 * Returns >= nr_cpu_ids if no further cpus unset.
 */
static inline unsigned int cpumask_next_zero(int n, const struct cpumask *srcp)
{
	/* -1 is a legal arg here. */
	if (n != -1)
		cpumask_check(n);
	return find_next_zero_bit(cpumask_bits(srcp), nr_cpumask_bits, n+1);
}

int cpumask_next_and(int n, const struct cpumask *, const struct cpumask *);
int cpumask_any_but(const struct cpumask *mask, unsigned int cpu);

/**
 * for_each_cpu - iterate over every cpu in a mask
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the cpumask pointer
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
/** 20130608    
 * NR_CPUS = 4이므로 이 부분이 수행됨
 * 0, 1, 2, 3 까지 반복
 **/
#define for_each_cpu(cpu, mask)				\
	for ((cpu) = -1;				\
		(cpu) = cpumask_next((cpu), (mask)),	\
		(cpu) < nr_cpu_ids;)

/**
 * for_each_cpu_not - iterate over every cpu in a complemented mask
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the cpumask pointer
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
#define for_each_cpu_not(cpu, mask)				\
	for ((cpu) = -1;					\
		(cpu) = cpumask_next_zero((cpu), (mask)),	\
		(cpu) < nr_cpu_ids;)

/**
 * for_each_cpu_and - iterate over every cpu in both masks
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the first cpumask pointer
 * @and: the second cpumask pointer
 *
 * This saves a temporary CPU mask in many places.  It is equivalent to:
 *	struct cpumask tmp;
 *	cpumask_and(&tmp, &mask, &and);
 *	for_each_cpu(cpu, &tmp)
 *		...
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
#define for_each_cpu_and(cpu, mask, and)				\
	for ((cpu) = -1;						\
		(cpu) = cpumask_next_and((cpu), (mask), (and)),		\
		(cpu) < nr_cpu_ids;)
#endif /* SMP */

#define CPU_BITS_NONE						\
{								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-1] = 0UL			\
}

#define CPU_BITS_CPU0						\
{								\
	[0] =  1UL						\
}

/**
 * cpumask_set_cpu - set a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @dstp: the cpumask pointer
 */
/** 20130518    
 * unsigned long 배열 dstp 의 cpu 번째 bit를 set 하는 함수.
 **/
static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *dstp)
{
	set_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

/**
 * cpumask_clear_cpu - clear a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @dstp: the cpumask pointer
 */
/** 20140426    
 * cpumask bits에서 특정 cpu를 제거.
 **/
static inline void cpumask_clear_cpu(int cpu, struct cpumask *dstp)
{
	clear_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

/**
 * cpumask_test_cpu - test for a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in @cpumask, else returns 0
 *
 * No static inline type checking - see Subtlety (1) above.
 */
/** 20140426    
 * cpumask 안에 cpu에 해당하는 bit가 설정되어 있는지 검사하는 매크로
 **/
#define cpumask_test_cpu(cpu, cpumask) \
	test_bit(cpumask_check(cpu), cpumask_bits((cpumask)))

/**
 * cpumask_test_and_set_cpu - atomically test and set a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in old bitmap of @cpumask, else returns 0
 *
 * test_and_set_bit wrapper for cpumasks.
 */
static inline int cpumask_test_and_set_cpu(int cpu, struct cpumask *cpumask)
{
	return test_and_set_bit(cpumask_check(cpu), cpumask_bits(cpumask));
}

/**
 * cpumask_test_and_clear_cpu - atomically test and clear a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in old bitmap of @cpumask, else returns 0
 *
 * test_and_clear_bit wrapper for cpumasks.
 */
/** 20140621    
 * cpu가 cpumask에 속했는지 검사해 리턴하고, 설정되어 있다면 clear 한다.
 **/
static inline int cpumask_test_and_clear_cpu(int cpu, struct cpumask *cpumask)
{
	return test_and_clear_bit(cpumask_check(cpu), cpumask_bits(cpumask));
}

/**
 * cpumask_setall - set all cpus (< nr_cpu_ids) in a cpumask
 * @dstp: the cpumask pointer
 */
/** 20140913    
 * 전달받은 cpumask를 nr_cpumask_bits (CPU 개수)만큼 1로 채워 설정한다.
 **/
static inline void cpumask_setall(struct cpumask *dstp)
{
	bitmap_fill(cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_clear - clear all cpus (< nr_cpu_ids) in a cpumask
 * @dstp: the cpumask pointer
 */
/** 20140419    
 * cpumask bitmap을 모두 0으로 초기화
 **/
static inline void cpumask_clear(struct cpumask *dstp)
{
	bitmap_zero(cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_and - *dstp = *src1p & *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 *
 * If *@dstp is empty, returns 0, else returns 1
 */
/** 20140621    
 * 두 개의 cpumask 에서 공통 비트를 추출해 새로운 cpumask를 만듦.
 * *dstp = *src1p & *src2p.
 **/
static inline int cpumask_and(struct cpumask *dstp,
			       const struct cpumask *src1p,
			       const struct cpumask *src2p)
{
	return bitmap_and(cpumask_bits(dstp), cpumask_bits(src1p),
				       cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_or - *dstp = *src1p | *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 */
/** 20141129    
 * 두 개의 cpumask를 or해 새로운 cpumask로 만들어 리턴한다.
 **/
static inline void cpumask_or(struct cpumask *dstp, const struct cpumask *src1p,
			      const struct cpumask *src2p)
{
	bitmap_or(cpumask_bits(dstp), cpumask_bits(src1p),
				      cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_xor - *dstp = *src1p ^ *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 */
static inline void cpumask_xor(struct cpumask *dstp,
			       const struct cpumask *src1p,
			       const struct cpumask *src2p)
{
	bitmap_xor(cpumask_bits(dstp), cpumask_bits(src1p),
				       cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_andnot - *dstp = *src1p & ~*src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 *
 * If *@dstp is empty, returns 0, else returns 1
 */
static inline int cpumask_andnot(struct cpumask *dstp,
				  const struct cpumask *src1p,
				  const struct cpumask *src2p)
{
	return bitmap_andnot(cpumask_bits(dstp), cpumask_bits(src1p),
					  cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_complement - *dstp = ~*srcp
 * @dstp: the cpumask result
 * @srcp: the input to invert
 */
static inline void cpumask_complement(struct cpumask *dstp,
				      const struct cpumask *srcp)
{
	bitmap_complement(cpumask_bits(dstp), cpumask_bits(srcp),
					      nr_cpumask_bits);
}

/**
 * cpumask_equal - *src1p == *src2p
 * @src1p: the first input
 * @src2p: the second input
 */
/** 20141122    
 * 두 cpumask의 비트맵이 동일한 비트들을 표현하는지 검사한다.
 **/
static inline bool cpumask_equal(const struct cpumask *src1p,
				const struct cpumask *src2p)
{
	return bitmap_equal(cpumask_bits(src1p), cpumask_bits(src2p),
						 nr_cpumask_bits);
}

/**
 * cpumask_intersects - (*src1p & *src2p) != 0
 * @src1p: the first input
 * @src2p: the second input
 */
/** 20150523    
 * 두 cpumask의 비트맵 중 교차되는 비트가 하나라도 있는지 검사한다.
 **/
static inline bool cpumask_intersects(const struct cpumask *src1p,
				     const struct cpumask *src2p)
{
	return bitmap_intersects(cpumask_bits(src1p), cpumask_bits(src2p),
						      nr_cpumask_bits);
}

/**
 * cpumask_subset - (*src1p & ~*src2p) == 0
 * @src1p: the first input
 * @src2p: the second input
 *
 * Returns 1 if *@src1p is a subset of *@src2p, else returns 0
 */
static inline int cpumask_subset(const struct cpumask *src1p,
				 const struct cpumask *src2p)
{
	return bitmap_subset(cpumask_bits(src1p), cpumask_bits(src2p),
						  nr_cpumask_bits);
}

/**
 * cpumask_empty - *srcp == 0
 * @srcp: the cpumask to that all cpus < nr_cpu_ids are clear.
 */
static inline bool cpumask_empty(const struct cpumask *srcp)
{
	return bitmap_empty(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_full - *srcp == 0xFFFFFFFF...
 * @srcp: the cpumask to that all cpus < nr_cpu_ids are set.
 */
static inline bool cpumask_full(const struct cpumask *srcp)
{
	return bitmap_full(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_weight - Count of bits in *srcp
 * @srcp: the cpumask to count bits (< nr_cpu_ids) in.
 */
/** 20130608    
 * mask영역에 대해 1로 설정된 bit의 수를 구함.
 **/
static inline unsigned int cpumask_weight(const struct cpumask *srcp)
{
	return bitmap_weight(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_shift_right - *dstp = *srcp >> n
 * @dstp: the cpumask result
 * @srcp: the input to shift
 * @n: the number of bits to shift by
 */
static inline void cpumask_shift_right(struct cpumask *dstp,
				       const struct cpumask *srcp, int n)
{
	bitmap_shift_right(cpumask_bits(dstp), cpumask_bits(srcp), n,
					       nr_cpumask_bits);
}

/**
 * cpumask_shift_left - *dstp = *srcp << n
 * @dstp: the cpumask result
 * @srcp: the input to shift
 * @n: the number of bits to shift by
 */
static inline void cpumask_shift_left(struct cpumask *dstp,
				      const struct cpumask *srcp, int n)
{
	bitmap_shift_left(cpumask_bits(dstp), cpumask_bits(srcp), n,
					      nr_cpumask_bits);
}

/**
 * cpumask_copy - *dstp = *srcp
 * @dstp: the result
 * @srcp: the input cpumask
 */
/** 20140426    
 * srcp -> dstp로 cpumask copy.
 **/
static inline void cpumask_copy(struct cpumask *dstp,
				const struct cpumask *srcp)
{
	bitmap_copy(cpumask_bits(dstp), cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_any - pick a "random" cpu from *srcp
 * @srcp: the input cpumask
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
#define cpumask_any(srcp) cpumask_first(srcp)

/**
 * cpumask_first_and - return the first cpu from *srcp1 & *srcp2
 * @src1p: the first input
 * @src2p: the second input
 *
 * Returns >= nr_cpu_ids if no cpus set in both.  See also cpumask_next_and().
 */
/** 20140621    
 * 두 개의 cpumask에 모두 포함되는 첫번째 cpu를 찾아 리턴한다.
 **/
#define cpumask_first_and(src1p, src2p) cpumask_next_and(-1, (src1p), (src2p))

/**
 * cpumask_any_and - pick a "random" cpu from *mask1 & *mask2
 * @mask1: the first input cpumask
 * @mask2: the second input cpumask
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
/** 20150523    
 * 두 cpumask 에 공통으로 포함된 비트를 찾는 방식으로
 * 첫번째 cpu를 선택하는 방식을 취한다.
 **/
#define cpumask_any_and(mask1, mask2) cpumask_first_and((mask1), (mask2))

/**
 * cpumask_of - the cpumask containing just a given cpu
 * @cpu: the cpu (<= nr_cpu_ids)
 */
/** 20130713    
 * cpu만 포함되어 있는 cpumask를 가져온다.
 **/
#define cpumask_of(cpu) (get_cpu_mask(cpu))

/**
 * cpumask_scnprintf - print a cpumask into a string as comma-separated hex
 * @buf: the buffer to sprintf into
 * @len: the length of the buffer
 * @srcp: the cpumask to print
 *
 * If len is zero, returns zero.  Otherwise returns the length of the
 * (nul-terminated) @buf string.
 */
static inline int cpumask_scnprintf(char *buf, int len,
				    const struct cpumask *srcp)
{
	return bitmap_scnprintf(buf, len, cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_parse_user - extract a cpumask from a user string
 * @buf: the buffer to extract from
 * @len: the length of the buffer
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpumask_parse_user(const char __user *buf, int len,
				     struct cpumask *dstp)
{
	return bitmap_parse_user(buf, len, cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_parselist_user - extract a cpumask from a user string
 * @buf: the buffer to extract from
 * @len: the length of the buffer
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpumask_parselist_user(const char __user *buf, int len,
				     struct cpumask *dstp)
{
	return bitmap_parselist_user(buf, len, cpumask_bits(dstp),
							nr_cpumask_bits);
}

/**
 * cpulist_scnprintf - print a cpumask into a string as comma-separated list
 * @buf: the buffer to sprintf into
 * @len: the length of the buffer
 * @srcp: the cpumask to print
 *
 * If len is zero, returns zero.  Otherwise returns the length of the
 * (nul-terminated) @buf string.
 */
static inline int cpulist_scnprintf(char *buf, int len,
				    const struct cpumask *srcp)
{
	return bitmap_scnlistprintf(buf, len, cpumask_bits(srcp),
				    nr_cpumask_bits);
}

/**
 * cpulist_parse - extract a cpumask from a user string of ranges
 * @buf: the buffer to extract from
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpulist_parse(const char *buf, struct cpumask *dstp)
{
	return bitmap_parselist(buf, cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_size - size to allocate for a 'struct cpumask' in bytes
 *
 * This will eventually be a runtime variable, depending on nr_cpu_ids.
 */
/** 20140426    
 * COMPILE시 지정된 CPU의 수만큼을 처리할 수 있는 long 변수의 크기를 구함.
 **/
static inline size_t cpumask_size(void)
{
	/* FIXME: Once all cpumask assignments are eliminated, this
	 * can be nr_cpumask_bits */
	return BITS_TO_LONGS(NR_CPUS) * sizeof(long);
}

/*
 * cpumask_var_t: struct cpumask for stack usage.
 *
 * Oh, the wicked games we play!  In order to make kernel coding a
 * little more difficult, we typedef cpumask_var_t to an array or a
 * pointer: doing &mask on an array is a noop, so it still works.
 *
 * ie.
 *	cpumask_var_t tmpmask;
 *	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
 *		return -ENOMEM;
 *
 *	  ... use 'tmpmask' like a normal struct cpumask * ...
 *
 *	free_cpumask_var(tmpmask);
 *
 *
 * However, one notable exception is there. alloc_cpumask_var() allocates
 * only nr_cpumask_bits bits (in the other hand, real cpumask_t always has
 * NR_CPUS bits). Therefore you don't have to dereference cpumask_var_t.
 *
 *	cpumask_var_t tmpmask;
 *	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
 *		return -ENOMEM;
 *
 *	var = *tmpmask;
 *
 * This code makes NR_CPUS length memcopy and brings to a memory corruption.
 * cpumask_copy() provide safe copy functionality.
 */
#ifdef CONFIG_CPUMASK_OFFSTACK
typedef struct cpumask *cpumask_var_t;

bool alloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags, int node);
bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
bool zalloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags, int node);
bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
void alloc_bootmem_cpumask_var(cpumask_var_t *mask);
void free_cpumask_var(cpumask_var_t mask);
void free_bootmem_cpumask_var(cpumask_var_t mask);

#else
typedef struct cpumask cpumask_var_t[1];

/** 20140419    
 * default로 CPUMASK_OFFSTACK가 정의되어 있지 않아 true 리턴.
 *
 * CPUMASK_OFFSTACK은 CPUMASK를 stack에 저장하지 않고 동적 메모리를 할당해 저장하는 것을 의미.
 **/
static inline bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	return true;
}

/** 20140426    
 * true 리턴
 **/
static inline bool alloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags,
					  int node)
{
	return true;
}

/** 20140419    
 * OFFSTACK이 아니므로 주어진 mask를 0으로 클리어 한다.
 **/
static inline bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	cpumask_clear(*mask);
	return true;
}

/** 20140906    
 * UMA인 경우 일반적인 cpumask clear.
 **/
static inline bool zalloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags,
					  int node)
{
	cpumask_clear(*mask);
	return true;
}

static inline void alloc_bootmem_cpumask_var(cpumask_var_t *mask)
{
}

/** 20150110    
 **/
static inline void free_cpumask_var(cpumask_var_t mask)
{
}

static inline void free_bootmem_cpumask_var(cpumask_var_t mask)
{
}
#endif /* CONFIG_CPUMASK_OFFSTACK */

/* It's common to want to use cpu_all_mask in struct member initializers,
 * so it has to refer to an address rather than a pointer. */
/** 20141122    
 * kernel/cpu.c에서 정의된 cpu_all_bits 선언.
 * bitmap mask를 cpumask로 변환해 cpu_all_mask라 선언한다.
 **/
extern const DECLARE_BITMAP(cpu_all_bits, NR_CPUS);
/** 20150523    
 * SMP에서 존재하는 모든 cpu에서 실행될 수 있도록 설정하는 mask.
 **/
#define cpu_all_mask to_cpumask(cpu_all_bits)

/* First bits of cpu_bit_bitmap are in fact unset. */
#define cpu_none_mask to_cpumask(cpu_bit_bitmap[0])

/** 20130608    
 * cpu_possible_mask에 설정된 각 cpu에 대해 루프 수행
 **/
#define for_each_possible_cpu(cpu) for_each_cpu((cpu), cpu_possible_mask)
#define for_each_online_cpu(cpu)   for_each_cpu((cpu), cpu_online_mask)
#define for_each_present_cpu(cpu)  for_each_cpu((cpu), cpu_present_mask)

/* Wrappers for arch boot code to manipulate normally-constant masks */
void set_cpu_possible(unsigned int cpu, bool possible);
void set_cpu_present(unsigned int cpu, bool present);
void set_cpu_online(unsigned int cpu, bool online);
void set_cpu_active(unsigned int cpu, bool active);
void init_cpu_present(const struct cpumask *src);
void init_cpu_possible(const struct cpumask *src);
void init_cpu_online(const struct cpumask *src);

/**
 * to_cpumask - convert an NR_CPUS bitmap to a struct cpumask *
 * @bitmap: the bitmap
 *
 * There are a few places where cpumask_var_t isn't appropriate and
 * static cpumasks must be used (eg. very early boot), yet we don't
 * expose the definition of 'struct cpumask'.
 *
 * This does the conversion, and can be used as a constant initializer.
 */
/** 20121208
 * bitmap을 struct cpumask *로 변환한다.
 *
 * 삼항 연산자가 필요한 이유 ???
 * 20130518
 * syntax 검사용인듯...
 **/
#define to_cpumask(bitmap)						\
	((struct cpumask *)(1 ? (bitmap)				\
			    : (void *)sizeof(__check_is_bitmap(bitmap))))

static inline int __check_is_bitmap(const unsigned long *bitmap)
{
	return 1;
}

/*
 * Special-case data structure for "single bit set only" constant CPU masks.
 *
 * We pre-generate all the 64 (or 32) possible bit positions, with enough
 * padding to the left and the right, and return the constant pointer
 * appropriately offset.
 */
extern const unsigned long
	cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)];

/** 20130713    
 * NR_CPUS의 비트맵에서 cpu만 설정되어 있는 cpu_mask를 가져온다.
 *
 * 비트가 설정된 비트맵을 왼쪽으로 리오더링된 테이블에서 p를 조정하여
 * 0으로 채워진 비트맵을 온전히 가져올 수 있다.
 *
 * cpu_bit_bitmap 참고.
 **/
static inline const struct cpumask *get_cpu_mask(unsigned int cpu)
{
	const unsigned long *p = cpu_bit_bitmap[1 + cpu % BITS_PER_LONG];
	p -= cpu / BITS_PER_LONG;
	return to_cpumask(p);
}

/** 20140809    
 * 주어진 cpu가 online mask에 속해있지 않으면 offline.
 **/
#define cpu_is_offline(cpu)	unlikely(!cpu_online(cpu))

#if NR_CPUS <= BITS_PER_LONG
/** 20141122    
 * 하나의 워드(long 변수)로 표현가능하므로 CPU_BITS_ALL은
 * 비트맵을 NR_CPUS만큼 1로 채워 cpu_mask로 리턴한다.
 **/
#define CPU_BITS_ALL						\
{								\
	[BITS_TO_LONGS(NR_CPUS)-1] = CPU_MASK_LAST_WORD	\
}

#else /* NR_CPUS > BITS_PER_LONG */

#define CPU_BITS_ALL						\
{								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-2] = ~0UL,		\
	[BITS_TO_LONGS(NR_CPUS)-1] = CPU_MASK_LAST_WORD		\
}
#endif /* NR_CPUS > BITS_PER_LONG */

/*
 *
 * From here down, all obsolete.  Use cpumask_ variants!
 *
 */
#ifndef CONFIG_DISABLE_OBSOLETE_CPUMASK_FUNCTIONS
#define cpumask_of_cpu(cpu) (*get_cpu_mask(cpu))

/** 20141122    
 * NR_CPUS 개수만큼 1로 설정된 bitmap의 마지막 word를 취한다.
 **/
#define CPU_MASK_LAST_WORD BITMAP_LAST_WORD_MASK(NR_CPUS)

#if NR_CPUS <= BITS_PER_LONG

#define CPU_MASK_ALL							\
(cpumask_t) { {								\
	[BITS_TO_LONGS(NR_CPUS)-1] = CPU_MASK_LAST_WORD			\
} }

#else

#define CPU_MASK_ALL							\
(cpumask_t) { {								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-2] = ~0UL,			\
	[BITS_TO_LONGS(NR_CPUS)-1] = CPU_MASK_LAST_WORD			\
} }

#endif

#define CPU_MASK_NONE							\
(cpumask_t) { {								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-1] =  0UL				\
} }

#define CPU_MASK_CPU0							\
(cpumask_t) { {								\
	[0] =  1UL							\
} }

#if NR_CPUS == 1
#define first_cpu(src)		({ (void)(src); 0; })
#define next_cpu(n, src)	({ (void)(src); 1; })
#define any_online_cpu(mask)	0
#define for_each_cpu_mask(cpu, mask)	\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask)
#else /* NR_CPUS > 1 */
int __first_cpu(const cpumask_t *srcp);
int __next_cpu(int n, const cpumask_t *srcp);

#define first_cpu(src)		__first_cpu(&(src))
#define next_cpu(n, src)	__next_cpu((n), &(src))
#define any_online_cpu(mask) cpumask_any_and(&mask, cpu_online_mask)
#define for_each_cpu_mask(cpu, mask)			\
	for ((cpu) = -1;				\
		(cpu) = next_cpu((cpu), (mask)),	\
		(cpu) < NR_CPUS; )
#endif /* SMP */

#if NR_CPUS <= 64

#define for_each_cpu_mask_nr(cpu, mask)	for_each_cpu_mask(cpu, mask)

#else /* NR_CPUS > 64 */

int __next_cpu_nr(int n, const cpumask_t *srcp);
#define for_each_cpu_mask_nr(cpu, mask)			\
	for ((cpu) = -1;				\
		(cpu) = __next_cpu_nr((cpu), &(mask)),	\
		(cpu) < nr_cpu_ids; )

#endif /* NR_CPUS > 64 */

#define cpus_addr(src) ((src).bits)

#define cpu_set(cpu, dst) __cpu_set((cpu), &(dst))
static inline void __cpu_set(int cpu, volatile cpumask_t *dstp)
{
	set_bit(cpu, dstp->bits);
}

#define cpu_clear(cpu, dst) __cpu_clear((cpu), &(dst))
static inline void __cpu_clear(int cpu, volatile cpumask_t *dstp)
{
	clear_bit(cpu, dstp->bits);
}

#define cpus_setall(dst) __cpus_setall(&(dst), NR_CPUS)
static inline void __cpus_setall(cpumask_t *dstp, int nbits)
{
	bitmap_fill(dstp->bits, nbits);
}

#define cpus_clear(dst) __cpus_clear(&(dst), NR_CPUS)
static inline void __cpus_clear(cpumask_t *dstp, int nbits)
{
	bitmap_zero(dstp->bits, nbits);
}

/* No static inline type checking - see Subtlety (1) above. */
#define cpu_isset(cpu, cpumask) test_bit((cpu), (cpumask).bits)

#define cpu_test_and_set(cpu, cpumask) __cpu_test_and_set((cpu), &(cpumask))
static inline int __cpu_test_and_set(int cpu, cpumask_t *addr)
{
	return test_and_set_bit(cpu, addr->bits);
}

#define cpus_and(dst, src1, src2) __cpus_and(&(dst), &(src1), &(src2), NR_CPUS)
static inline int __cpus_and(cpumask_t *dstp, const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	return bitmap_and(dstp->bits, src1p->bits, src2p->bits, nbits);
}

#define cpus_or(dst, src1, src2) __cpus_or(&(dst), &(src1), &(src2), NR_CPUS)
static inline void __cpus_or(cpumask_t *dstp, const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	bitmap_or(dstp->bits, src1p->bits, src2p->bits, nbits);
}

#define cpus_xor(dst, src1, src2) __cpus_xor(&(dst), &(src1), &(src2), NR_CPUS)
static inline void __cpus_xor(cpumask_t *dstp, const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	bitmap_xor(dstp->bits, src1p->bits, src2p->bits, nbits);
}

#define cpus_andnot(dst, src1, src2) \
				__cpus_andnot(&(dst), &(src1), &(src2), NR_CPUS)
static inline int __cpus_andnot(cpumask_t *dstp, const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	return bitmap_andnot(dstp->bits, src1p->bits, src2p->bits, nbits);
}

#define cpus_equal(src1, src2) __cpus_equal(&(src1), &(src2), NR_CPUS)
static inline int __cpus_equal(const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	return bitmap_equal(src1p->bits, src2p->bits, nbits);
}

#define cpus_intersects(src1, src2) __cpus_intersects(&(src1), &(src2), NR_CPUS)
static inline int __cpus_intersects(const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	return bitmap_intersects(src1p->bits, src2p->bits, nbits);
}

#define cpus_subset(src1, src2) __cpus_subset(&(src1), &(src2), NR_CPUS)
static inline int __cpus_subset(const cpumask_t *src1p,
					const cpumask_t *src2p, int nbits)
{
	return bitmap_subset(src1p->bits, src2p->bits, nbits);
}

#define cpus_empty(src) __cpus_empty(&(src), NR_CPUS)
static inline int __cpus_empty(const cpumask_t *srcp, int nbits)
{
	return bitmap_empty(srcp->bits, nbits);
}

#define cpus_weight(cpumask) __cpus_weight(&(cpumask), NR_CPUS)
static inline int __cpus_weight(const cpumask_t *srcp, int nbits)
{
	return bitmap_weight(srcp->bits, nbits);
}

#define cpus_shift_left(dst, src, n) \
			__cpus_shift_left(&(dst), &(src), (n), NR_CPUS)
static inline void __cpus_shift_left(cpumask_t *dstp,
					const cpumask_t *srcp, int n, int nbits)
{
	bitmap_shift_left(dstp->bits, srcp->bits, n, nbits);
}
#endif /* !CONFIG_DISABLE_OBSOLETE_CPUMASK_FUNCTIONS */

#endif /* __LINUX_CPUMASK_H */
