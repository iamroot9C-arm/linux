#ifndef __LINUX_CACHE_H
#define __LINUX_CACHE_H

#include <linux/kernel.h>
#include <asm/cache.h>

#ifndef L1_CACHE_ALIGN
#define L1_CACHE_ALIGN(x) ALIGN(x, L1_CACHE_BYTES)
#endif

/** 20130518    
 * SMP cache 크기는 L1_CACHE_BYTES
 **/
#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES L1_CACHE_BYTES
#endif

#ifndef __read_mostly
#define __read_mostly
#endif

/** 20140920    
 * SMP cache bytes로 align시켜 데이터가 하나의 cacheline에 들어올 수 있도록 한다.
 **/
#ifndef ____cacheline_aligned
#define ____cacheline_aligned __attribute__((__aligned__(SMP_CACHE_BYTES)))
#endif

#ifndef ____cacheline_aligned_in_smp
#ifdef CONFIG_SMP
#define ____cacheline_aligned_in_smp ____cacheline_aligned
#else
#define ____cacheline_aligned_in_smp
#endif /* CONFIG_SMP */
#endif

/** 20140426    
 * ".data..cacheline_aligned" section이 저장된다.
 * 이 섹션의 데이터는 SMP_CACHE_BYTES (1<<6)으로 저장된다.
 **/
#ifndef __cacheline_aligned
#define __cacheline_aligned					\
  __attribute__((__aligned__(SMP_CACHE_BYTES),			\
		 __section__(".data..cacheline_aligned")))
#endif /* __cacheline_aligned */

#ifndef __cacheline_aligned_in_smp
#ifdef CONFIG_SMP
/** 20140426    
 **/
#define __cacheline_aligned_in_smp __cacheline_aligned
#else
#define __cacheline_aligned_in_smp
#endif /* CONFIG_SMP */
#endif

/*
 * The maximum alignment needed for some critical structures
 * These could be inter-node cacheline sizes/L3 cacheline
 * size etc.  Define this in asm/cache.h for your arch
 */
#ifndef INTERNODE_CACHE_SHIFT
#define INTERNODE_CACHE_SHIFT L1_CACHE_SHIFT
#endif

#if !defined(____cacheline_internodealigned_in_smp)
#if defined(CONFIG_SMP)
#define ____cacheline_internodealigned_in_smp \
	__attribute__((__aligned__(1 << (INTERNODE_CACHE_SHIFT))))
#else
#define ____cacheline_internodealigned_in_smp
#endif
#endif

/** 20130907    
 * cache line 크기는 L1 캐시 크기
 **/
#ifndef CONFIG_ARCH_HAS_CACHE_LINE_SIZE
#define cache_line_size()	L1_CACHE_BYTES
#endif

#endif /* __LINUX_CACHE_H */
