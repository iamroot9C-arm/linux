/*
 * ARM specific SMP header, this contains our implementation
 * details.
 */
#ifndef __ASMARM_SMP_PLAT_H
#define __ASMARM_SMP_PLAT_H

#include <asm/cputype.h>

/*
 * Return true if we are running on a SMP platform
 */
/** 20130518    
 * smp 사용 여부를 리턴하는 함수
 **/
static inline bool is_smp(void)
{
#ifndef CONFIG_SMP
	return false;
#elif defined(CONFIG_SMP_ON_UP)
	extern unsigned int smp_on_up;
/** 20121103
 * 0 or 1, False or True 중 하나의 값으로 리턴하기 위한 코드. !!
 **/
	return !!smp_on_up;
#else
	return true;
#endif
}

/* all SMP configurations have the extended CPUID registers */
/** 20131026    
 * tlb operation이 broadcast되어야 하는 경우인지 조회해 리턴.
 **/
static inline int tlb_ops_need_broadcast(void)
{
	/** 20131026    
	 * smp가 아닐 경우 바로 return.
	 **/
	if (!is_smp())
		return 0;

	/** 20131026    
	 * ARM B4.1.92
	 * ID_MMFR3, Memory Model Feature Register 3, VMSA
	 *
	 * Indicates whether Cache, TLB and branch predictor operations are broadcast. Permitted values are:
	 * 0b0000 Cache, TLB and branch predictor operations only affect local structures.
	 * 0b0001 Cache and branch predictor operations affect structures according to shareability and defined behavior of instructions. TLB operations only affect local structures.
	 * 0b0010 Cache, TLB and branch predictor operations affect structures according to shareability and defined behavior of instructions.
	 *
	 *
	 * Maintenance broadcast 필드의 값이 2보다 작은 경우,
	 * 즉 TLB operation이 local structure에 영향을 미치는 경우 true 리턴.
	 * local structure ???
	 **/
	return ((read_cpuid_ext(CPUID_EXT_MMFR3) >> 12) & 0xf) < 2;
}

#if !defined(CONFIG_SMP) || __LINUX_ARM_ARCH__ >= 7
#define cache_ops_need_broadcast()	0
#else
static inline int cache_ops_need_broadcast(void)
{
	if (!is_smp())
		return 0;

	return ((read_cpuid_ext(CPUID_EXT_MMFR3) >> 12) & 0xf) < 1;
}
#endif

/*
 * Logical CPU mapping.
 */
extern int __cpu_logical_map[];
#define cpu_logical_map(cpu)	__cpu_logical_map[cpu]

#endif
