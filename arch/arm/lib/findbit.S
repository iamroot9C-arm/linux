/*
 *  linux/arch/arm/lib/findbit.S
 *
 *  Copyright (C) 1995-2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * 16th March 2001 - John Ripley <jripley@sonicblue.com>
 *   Fixed so that "size" is an exclusive not an inclusive quantity.
 *   All users of these functions expect exclusive sizes, and may
 *   also call with zero size.
 * Reworked by rmk.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
                .text

/*
 * Purpose  : Find a 'zero' bit
 * Prototype: int find_first_zero_bit(void *addr, unsigned int maxbit);
 */
ENTRY(_find_first_zero_bit_le)
		teq	r1, #0	
		beq	3f
		mov	r2, #0
/** 20130420    
	_find_first_zero_bit_le, _find_next_zero_bit_le 에서 공통으로 사용.
	_find_first_zero_bit_le에서 올 경우 r2에 0을 넣어줌
 **/
1:
/** 20130420
  _find_next_zero_bit_le에서 하위 3비트가 0일 경우 넘어옴
   r3 = *(char *)(addr + (offset >> 3))
 **/
 ARM(		ldrb	r3, [r0, r2, lsr #3]	)
 THUMB(		lsr	r3, r2, #3		)
 THUMB(		ldrb	r3, [r0, r3]		)
/** 20130420
  r3 ~= r3
 **/
		eors	r3, r3, #0xff		@ invert bits
/** 20130420
  LSB부터 처음 0이 아닌 위치를 찾아 리턴.
  찾지 못했을 경우 다음 바이트부터 검색
 **/
		bne	.L_found		@ any now set - found zero bit
		add	r2, r2, #8		@ next bit pointer
/** 20130420
  maxbit보다 작으면 계속 반복.
 **/
2:		cmp	r2, r1			@ any more?
		blo	1b
/** 20130420    
  찾지 못하고 max까지 도달한 경우 maxbit를 리턴
 **/
3:		mov	r0, r1			@ no free bits
		mov	pc, lr
ENDPROC(_find_first_zero_bit_le)

/*
 * Purpose  : Find next 'zero' bit
 * Prototype: int find_next_zero_bit(void *addr, unsigned int maxbit, int offset)
 */
ENTRY(_find_next_zero_bit_le)
/** 20130420    
 *      maxbit가 0으로 넘어오면 _find_first_zero_bit_le의 3:의 리턴 코드를 수행
 **/
		teq	r1, #0
		beq	3b
/** 20130420    
		maxbit가 0이 아닌 경우, offset의 하위 3비트를 ip에 저장
		하위 3비트가 0인 경우 _find_first_zero_bit_le의 1:부터 수행
 **/
		ands	ip, r2, #7
		beq	1b			@ If new byte, goto old routine
 ARM(		ldrb	r3, [r0, r2, lsr #3]	)
 THUMB(		lsr	r3, r2, #3		)
 THUMB(		ldrb	r3, [r0, r3]		)
		eor	r3, r3, #0xff		@ now looking for a 1 bit
/** 20130420
		r2의 하위 3비트 값으로 shift를 해서 버림.
 **/
		movs	r3, r3, lsr ip		@ shift off unused bits
		bne	.L_found
/** 20130420    
		해당 바이트에서 0을 찾지 못하면
		다음 바이트부터 정렬된 상태에서 찾기 위해 r2 값을 조정.
		정렬을 맞춘 뒤 2b부터 수행
 **/
		orr	r2, r2, #7		@ if zero, then no bits here
		add	r2, r2, #1		@ align bit pointer
		b	2b			@ loop for next bit
ENDPROC(_find_next_zero_bit_le)

/*
 * Purpose  : Find a 'one' bit
 * Prototype: int find_first_bit(const unsigned long *addr, unsigned int maxbit);
 */
ENTRY(_find_first_bit_le)
		teq	r1, #0	
		beq	3f
		mov	r2, #0
1:
 ARM(		ldrb	r3, [r0, r2, lsr #3]	)
 THUMB(		lsr	r3, r2, #3		)
 THUMB(		ldrb	r3, [r0, r3]		)
		movs	r3, r3
		bne	.L_found		@ any now set - found zero bit
		add	r2, r2, #8		@ next bit pointer
2:		cmp	r2, r1			@ any more?
		blo	1b
3:		mov	r0, r1			@ no free bits
		mov	pc, lr
ENDPROC(_find_first_bit_le)

/*
 * Purpose  : Find next 'one' bit
 * Prototype: int find_next_zero_bit(void *addr, unsigned int maxbit, int offset)
 */
ENTRY(_find_next_bit_le)
		teq	r1, #0
		beq	3b
		ands	ip, r2, #7
		beq	1b			@ If new byte, goto old routine
 ARM(		ldrb	r3, [r0, r2, lsr #3]	)
 THUMB(		lsr	r3, r2, #3		)
 THUMB(		ldrb	r3, [r0, r3]		)
		movs	r3, r3, lsr ip		@ shift off unused bits
		bne	.L_found
		orr	r2, r2, #7		@ if zero, then no bits here
		add	r2, r2, #1		@ align bit pointer
		b	2b			@ loop for next bit
ENDPROC(_find_next_bit_le)

#ifdef __ARMEB__

ENTRY(_find_first_zero_bit_be)
		teq	r1, #0
		beq	3f
		mov	r2, #0
1:		eor	r3, r2, #0x18		@ big endian byte ordering
 ARM(		ldrb	r3, [r0, r3, lsr #3]	)
 THUMB(		lsr	r3, #3			)
 THUMB(		ldrb	r3, [r0, r3]		)
		eors	r3, r3, #0xff		@ invert bits
		bne	.L_found		@ any now set - found zero bit
		add	r2, r2, #8		@ next bit pointer
2:		cmp	r2, r1			@ any more?
		blo	1b
3:		mov	r0, r1			@ no free bits
		mov	pc, lr
ENDPROC(_find_first_zero_bit_be)

ENTRY(_find_next_zero_bit_be)
		teq	r1, #0
		beq	3b
		ands	ip, r2, #7
		beq	1b			@ If new byte, goto old routine
		eor	r3, r2, #0x18		@ big endian byte ordering
 ARM(		ldrb	r3, [r0, r3, lsr #3]	)
 THUMB(		lsr	r3, #3			)
 THUMB(		ldrb	r3, [r0, r3]		)
		eor	r3, r3, #0xff		@ now looking for a 1 bit
		movs	r3, r3, lsr ip		@ shift off unused bits
		bne	.L_found
		orr	r2, r2, #7		@ if zero, then no bits here
		add	r2, r2, #1		@ align bit pointer
		b	2b			@ loop for next bit
ENDPROC(_find_next_zero_bit_be)

ENTRY(_find_first_bit_be)
		teq	r1, #0
		beq	3f
		mov	r2, #0
1:		eor	r3, r2, #0x18		@ big endian byte ordering
 ARM(		ldrb	r3, [r0, r3, lsr #3]	)
 THUMB(		lsr	r3, #3			)
 THUMB(		ldrb	r3, [r0, r3]		)
		movs	r3, r3
		bne	.L_found		@ any now set - found zero bit
		add	r2, r2, #8		@ next bit pointer
2:		cmp	r2, r1			@ any more?
		blo	1b
3:		mov	r0, r1			@ no free bits
		mov	pc, lr
ENDPROC(_find_first_bit_be)

ENTRY(_find_next_bit_be)
		teq	r1, #0
		beq	3b
		ands	ip, r2, #7
		beq	1b			@ If new byte, goto old routine
		eor	r3, r2, #0x18		@ big endian byte ordering
 ARM(		ldrb	r3, [r0, r3, lsr #3]	)
 THUMB(		lsr	r3, #3			)
 THUMB(		ldrb	r3, [r0, r3]		)
		movs	r3, r3, lsr ip		@ shift off unused bits
		bne	.L_found
		orr	r2, r2, #7		@ if zero, then no bits here
		add	r2, r2, #1		@ align bit pointer
		b	2b			@ loop for next bit
ENDPROC(_find_next_bit_be)

#endif

/*
 * One or more bits in the LSB of r3 are assumed to be set.
 */
.L_found:
#if __LINUX_ARM_ARCH__ >= 5
/** 20130420    
		ex) r3 : 0x18
		rsb r0, r3, #0  ->  r3의  2의 보수를 취한다.
		    r3 : 0xE8
		and	r3, r3, r0  ->  (0-r3) & r3로 가장 오른쪽 1을 취한다.
			r3 : 0xE8 & 0x18 = 0x08
		clz	r3, r3		->  r3에서 MSB부터 0의 개수를 센다.
			r3 : 28
		rsb	r3, r3, #31	->  LSB 기준으로 변경한다.
			r3 : 3
		add	r0, r2, r3	->  offset(기준점)에서부터의 위치로 리턴한다.
**/
		rsb	r0, r3, #0
		and	r3, r3, r0
		clz	r3, r3
		rsb	r3, r3, #31
		add	r0, r2, r3
#else
		tst	r3, #0x0f
		addeq	r2, r2, #4
		movne	r3, r3, lsl #4
		tst	r3, #0x30
		addeq	r2, r2, #2
		movne	r3, r3, lsl #2
		tst	r3, #0x40
		addeq	r2, r2, #1
		mov	r0, r2
#endif
/** 20130420    
		찾은 값이 max보다 크다면 max를 리턴
 **/
		cmp	r1, r0			@ Clamp to maxbit
		movlo	r0, r1
		mov	pc, lr

