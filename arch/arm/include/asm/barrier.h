#ifndef __ASM_BARRIER_H
#define __ASM_BARRIER_H

#ifndef __ASSEMBLY__
#include <asm/outercache.h>

#define nop() __asm__ __volatile__("mov\tr0,r0\t@ nop\n\t");

#if __LINUX_ARM_ARCH__ >= 7 ||		\
	(__LINUX_ARM_ARCH__ == 6 && defined(CONFIG_CPU_32v6K))
#define sev()	__asm__ __volatile__ ("sev" : : : "memory")
/** 20121124
 *  ARM B1.8.13 Wait For Event and Send Event
 *
 *  SEV (대기) <-> WFE (호출)
 *
 *  wfe() 사용 예
 *  => spinlock에서 ticket을 획득하지 못한 상태로 대기할 때.
 *
 * [출처] http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0204ik/Cjafcggi.html
 *
 *   * SEV
 *	 SEV는 다중 프로세서 시스템 내의 모든 코어에 신호를 보낼 이벤트를 발생시킵니다. SEV가 구현될 경우 WFE도 구현되어야 합니다.
 *
 *	 - WFE
 *	 이벤트 레지스터가 설정되지 않은 경우 WFE는 다음 이벤트 중 하나가 발생할 때까지 실행을 일시 중단합니다.
 *	 CPSR I 비트로 마스킹되지 않은 경우, IRQ 인터럽트
 *	 CPSR F 비트로 마스킹되지 않은 경우, FIQ 인터럽트
 *	 CPSR A 비트로 마스킹되지 않은 경우, 부정확한 데이터 어보트
 *	 디버그를 사용하는 경우, 디버그 시작 요청
 *	 SEV 명령어를 통해 다른 프로세서의 신호를 받는 이벤트
 *
 *	 이벤트 레지스터가 설정될 경우 WFE는 이를 해제하고 즉시 원래 상태로 되돌립니다.
 *	 WFE가 구현될 경우 SEV도 구현되어야 합니다.
 *
 *	 - WFI
 *	 WFI는 다음 이벤트 중 하나가 발생할 때까지 실행을 일시 중단합니다.
 *	 CPSR I 비트에 관계없이 IRQ 인터럽트
 *	 CPSR F 비트에 관계없이 FIQ 인터럽트
 *	 CPSR A 비트로 마스킹되지 않은 경우, 부정확한 데이터 어보트
 *	 디버그를 사용하는지 여부에 관계없이 디버그 시작 요청
 **/
#define wfe()	__asm__ __volatile__ ("wfe" : : : "memory")
#define wfi()	__asm__ __volatile__ ("wfi" : : : "memory")
#endif

#if __LINUX_ARM_ARCH__ >= 7
/** 20140920    
 * 아키텍처에서 지원하는 명령을 사용한 barrier oepration.
 *
 * 현재 버전(v3.17)에서는 더 세밀한 옵션을 사용한다.
 * #define smp_mb()    dmb(ish)
 * #define smp_rmb()   smp_mb()
 * #define smp_wmb()   dmb(ishst)
 *
 * 20141229
 * ISB
 * 프로세서의 파이프라인을 플러시하여, ISB 다음의 명령들이 캐시나 메모리로부터 fetch되도록 한다.
 * CP15 레지스터에 대한 변경, ASID 변경 같은 컨텍스트 변경작업, 완료된 TLB 관리작업, 분기예측 관리작업 등
 * 상태에 따라 명령의 동작이 달라질 수 있는 상황에서 사용한다.
 **/
#define isb() __asm__ __volatile__ ("isb" : : : "memory")
#define dsb() __asm__ __volatile__ ("dsb" : : : "memory")
#define dmb() __asm__ __volatile__ ("dmb" : : : "memory")
#elif defined(CONFIG_CPU_XSC3) || __LINUX_ARM_ARCH__ == 6
#define isb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c5, 4" \
				    : : "r" (0) : "memory")
#define dsb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 4" \
				    : : "r" (0) : "memory")
#define dmb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 5" \
				    : : "r" (0) : "memory")
#elif defined(CONFIG_CPU_FA526)
#define isb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c5, 4" \
				    : : "r" (0) : "memory")
#define dsb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 4" \
				    : : "r" (0) : "memory")
#define dmb() __asm__ __volatile__ ("" : : : "memory")
#else
#define isb() __asm__ __volatile__ ("" : : : "memory")
#define dsb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 4" \
				    : : "r" (0) : "memory")
#define dmb() __asm__ __volatile__ ("" : : : "memory")
#endif

#ifdef CONFIG_ARCH_HAS_BARRIERS
#include <mach/barriers.h>
/** 20140920    
 * dsb()로 L1 cache를 포함한 core(Inner shareability)까지의 sync.
 * outer_sync()로 L2 cache까지의 sync. 수행
 *
 * 현재 커널(3.17)에서는 더 세분화된 옵션을 사용
 * #define mb()        do { dsb(); outer_sync(); } while (0)
 * #define rmb()       dsb()
 * #define wmb()       do { dsb(st); outer_sync(); } while (0)
 *
 * 세부 옵션 참고
 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0204ik/CIHJFGFE.html
 **/
#elif defined(CONFIG_ARM_DMA_MEM_BUFFERABLE) || defined(CONFIG_SMP)
#define mb()		do { dsb(); outer_sync(); } while (0)
#define rmb()		dsb()
#define wmb()		mb()
#else
#include <asm/memory.h>
#define mb()	do { if (arch_is_coherent()) dmb(); else barrier(); } while (0)
#define rmb()	do { if (arch_is_coherent()) dmb(); else barrier(); } while (0)
#define wmb()	do { if (arch_is_coherent()) dmb(); else barrier(); } while (0)
#endif

#ifndef CONFIG_SMP
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#else
/** 20130706    
 * SMP일 때는 dmb() 호출.
 **/
#define smp_mb()	dmb()
#define smp_rmb()	dmb()
#define smp_wmb()	dmb()
#endif

/** 20150404    
 **/
#define read_barrier_depends()		do { } while(0)
#define smp_read_barrier_depends()	do { } while(0)

/** 20160213    
 **/
#define set_mb(var, value)	do { var = value; smp_mb(); } while (0)

#endif /* !__ASSEMBLY__ */
#endif /* __ASM_BARRIER_H */
