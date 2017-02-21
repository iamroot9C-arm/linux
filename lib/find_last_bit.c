/* find_last_bit.c: fallback find next bit implementation
 *
 * Copyright (C) 2008 IBM Corporation
 * Written by Rusty Russell <rusty@rustcorp.com.au>
 * (Inspired by David Howell's find_next_bit implementation)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/export.h>
#include <asm/types.h>
#include <asm/byteorder.h>

#ifndef find_last_bit

/** 20130608
 * addr에서 가장 첫번째 1이 나오는 bit의 위치를 리턴
 **/
unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long words;
	unsigned long tmp;

	/* Start at final word. */
	/** 20130608
	 * unsigned long 단위의 개수를 words에 저장
	 **/
	words = size / BITS_PER_LONG;

	/* Partial final word? */
	if (size & (BITS_PER_LONG-1)) {
		/** 20130608
		 * 해당 word
		 * e.g. size : 4, BITS_PER_LONG : 32
		 * addr[words] & (0xffffffff >> (32 - (4 & (32-1))))
		 **/
		tmp = (addr[words] & (~0UL >> (BITS_PER_LONG
					 - (size & (BITS_PER_LONG-1)))));
		if (tmp)
			goto found;
	}

	while (words) {
		tmp = addr[--words];
		if (tmp) {
found:
			/** 20130608
			 * 첫번째(낮은 메모리 주소) 워드에서 msb의 위치를 리턴
			 **/
			return words * BITS_PER_LONG + __fls(tmp);
		}
	}

	/* Not found */
	return size;
}
EXPORT_SYMBOL(find_last_bit);

#endif
