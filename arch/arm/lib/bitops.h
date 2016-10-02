#include <asm/unwind.h>

#if __LINUX_ARM_ARCH__ >= 6
	/** 20121208
	 * _set_bit일 경우 r0 : nr, r1 : p, instr : orr 
	 * #define ATOMIC_BITOP(name,nr,p)		_##name(nr,p)
	 **/
	.macro	bitop, name, instr
ENTRY(	\name		)
  /** 20121208
   * UNWIND : unwind table에 등록해서 exeption이 발생했을 때 backtrace할 수 있는 정보를 제공하는 듯 ???
   * [참고] http://sourceware.org/binutils/docs/as/ARM-Unwinding-Tutorial.html
   *   https://wiki.linaro.org/KenWerner/Sandbox/libunwind#exception_index_table_entry
   **/
  /** 20121208
   * 1. word-aligned를 체크하여 data abort를 발생 시키는 듯 ???
   * 2. 32비트 word 단위연산을 위해 and r3, r0, #31을 수행한다
   * 3. r1에서 해당되는 비트를 찾는다
   * 4. 비트 연산을 atomic하게 수행한다
   **/
UNWIND(	.fnstart	)
	ands	ip, r1, #3
	strneb	r1, [ip]		@ assert word-aligned
	mov	r2, #1
	and	r3, r0, #31		@ Get bit offset
	mov	r0, r0, lsr #5
	add	r1, r1, r0, lsl #2	@ Get word offset
	mov	r3, r2, lsl r3
1:	ldrex	r2, [r1]
	\instr	r2, r2, r3
	strex	r0, r2, [r1]
	cmp	r0, #0
	bne	1b
	bx	lr
UNWIND(	.fnend		)
ENDPROC(\name		)
	.endm

	.macro	testop, name, instr, store
	/** 20130406    
	 * [참고] arm assembly 명령어 http://downrg.com/417
	 *
	 *      test_and_set_bit(idx, bdata->node_bootmem_map)
	 * c.g. testop	_test_and_set_bit, orreq, streq
	 **/
ENTRY(	\name		)
UNWIND(	.fnstart	)
	/** 20130406    
	 * 0,1 번 비트를 검사해 ip에 저장
	 * 정렬되어 있다면 0이므로 z가 1, 정렬되어 있지 않다면 z가 0
	 **/
	ands	ip, r1, #3
	/** 20130406    
	 * 정렬되어 있지 않다면 수행됨. word-align 위반을 표현하기 위한 assertion
	 **/
	strneb	r1, [ip]		@ assert word-aligned
	/** 20130406    
	 * r2에 bit shift 하기 위한 1을 넣어둠
	 **/
	mov	r2, #1
	/** 20130406    
	 * r0의 하위 5비트 추출해 r3에 저장. 특정 word 상의 offset으로 삼기 위함
	 **/
	and	r3, r0, #31		@ Get bit offset
	/** 20130406    
	 * r0를 32로 나눈 인덱스로 변환. 몇 번째 word를 찾아내야 하는지 구함
	 **/
	mov	r0, r0, lsr #5
	/** 20130406    
	 * r1를 r0가 가리키는 워드로 변경함
	 **/
	add	r1, r1, r0, lsl #2	@ Get word offset
	/** 20130406    
	 * 해당 bit의 위치에 대한 mask 생성
	 **/
	mov	r3, r2, lsl r3		@ create mask
	smp_dmb
	/** 20130406    
	 * 원자적으로 r1이 가리키는 메모리의 값을 r2에 저장
	 **/
1:	ldrex	r2, [r1]
	/** 20130406    
	 * 이전 비트값을 r0에 저장
	 **/
	ands	r0, r2, r3		@ save old value of bit
	/** 20130406    
	 * test_and_set
	 * instr이 orreq라면, r0의 값이 0일 때 수행됨
	 * (r2의 r3 위치의 비트값이 0이었다면 1로 변경)
	 *
	 * test_and_clear
	 * instr이 bicne
	 **/
	\instr	r2, r2, r3		@ toggle bit
	strex	ip, r2, [r1]
	/** 20130406    
	 * 원자적 실행을 검사
	 **/
	cmp	ip, #0
	bne	1b
	smp_dmb
	cmp	r0, #0
	/** 20130406    
	 * r0가 0이 아니면 리턴값 (r0)을 1로 설정
	 **/
	movne	r0, #1
	/** 20130406    
	 * mov pc, lr과 같은 역할 (subroutine에서 복귀)
	 * cycle 이 조금 더 적게 든다.
	 * http://newsgroups.derkeiler.com/Archive/Comp/comp.sys.arm/2006-04/msg00031.html
	 **/
2:	bx	lr
UNWIND(	.fnend		)
ENDPROC(\name		)
	.endm
#else
	.macro	bitop, name, instr
ENTRY(	\name		)
UNWIND(	.fnstart	)
	ands	ip, r1, #3
	strneb	r1, [ip]		@ assert word-aligned
	and	r2, r0, #31
	mov	r0, r0, lsr #5
	mov	r3, #1
	mov	r3, r3, lsl r2
	save_and_disable_irqs ip
	ldr	r2, [r1, r0, lsl #2]
	\instr	r2, r2, r3
	str	r2, [r1, r0, lsl #2]
	restore_irqs ip
	mov	pc, lr
UNWIND(	.fnend		)
ENDPROC(\name		)
	.endm

/**
 * testop - implement a test_and_xxx_bit operation.
 * @instr: operational instruction
 * @store: store instruction
 *
 * Note: we can trivially conditionalise the store instruction
 * to avoid dirtying the data cache.
 */
	.macro	testop, name, instr, store
ENTRY(	\name		)
UNWIND(	.fnstart	)
	ands	ip, r1, #3
	strneb	r1, [ip]		@ assert word-aligned
	and	r3, r0, #31
	mov	r0, r0, lsr #5
	save_and_disable_irqs ip
	ldr	r2, [r1, r0, lsl #2]!
	mov	r0, #1
	tst	r2, r0, lsl r3
	\instr	r2, r2, r0, lsl r3
	\store	r2, [r1]
	moveq	r0, #0
	restore_irqs ip
	mov	pc, lr
UNWIND(	.fnend		)
ENDPROC(\name		)
	.endm
#endif
