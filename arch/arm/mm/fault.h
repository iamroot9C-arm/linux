#ifndef __ARCH_ARM_FAULT_H
#define __ARCH_ARM_FAULT_H

/*
 * Fault status register encodings.  We steal bit 31 for our own purposes.
 */
/** 20161206
 * FSR_LNX_PF는 reserved인데, 커널에서 추가해 do_DataAbort, do_PrefetchAbort를
 * 구분하기 위한 용도로 사용한다.
 *
 * 그 외 DFSR 레지스터대로 비트 선언.
 **/
#define FSR_LNX_PF		(1 << 31)
#define FSR_WRITE		(1 << 11)
#define FSR_FS4			(1 << 10)
#define FSR_FS3_0		(15)
#define FSR_FS5_0		(0x3f)

#ifdef CONFIG_ARM_LPAE
static inline int fsr_fs(unsigned int fsr)
{
	return fsr & FSR_FS5_0;
}
#else
/** 20151121
 * FSR에서 fault status를 추출.
 *
 * encoding의 의미는 데이터시트의 표 참고.
 * Table B3-23 Short-descriptor format FSR encodings
 *
 * FS		Source									Notes
 * -----------------------------------------------------------------------------------------------------------------------------
 * 00001	Alignment fault								DFSR only. Fault on first lookup
 * 00100	Fault on instruction cache maintenance					DFSR only
 * 01100	Synchronous external abort on translation table walk	First level	-
 * 01110								Second level
 * 11100	Synchronous parity error on translation table walk	First level	-
 * 11110								Second level
 * 00101	Translation fault					First level	MMU fault
 * 00111								Second level	MMU fault
 * 00011	Access flag fault					First level	MMU fault
 * 00110								Second level	MMU fault
 * 01001	Domain fault						First level	MMU fault
 * 01011								Second level
 * 01101	Permission fault					Second level	MMU fault
 * 01111								First level
 * 00010	Debug event								See About debug events on page C3-2008
 * 01000	Synchronous external abort						-
 * 10000	TLB conflict abort							See TLB conflict aborts on page B3-1371
 * 10100	IMPLEMENTATION DEFINED							Lockdown
 * 11010	IMPLEMENTATION DEFINED Coprocessor					abort
 * 11001	Synchronous parity error on memory access				-
 * 10110	Asynchronous external abort						DFSR only
 * 11000	Asynchronous parity error on memory access				DFSR only
 **/
static inline int fsr_fs(unsigned int fsr)
{
	return (fsr & FSR_FS3_0) | (fsr & FSR_FS4) >> 6;
}
#endif

void do_bad_area(unsigned long addr, unsigned int fsr, struct pt_regs *regs);
unsigned long search_exception_table(unsigned long addr);

#endif	/* __ARCH_ARM_FAULT_H */
