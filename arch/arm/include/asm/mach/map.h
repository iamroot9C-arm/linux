/*
 *  arch/arm/include/asm/map.h
 *
 *  Copyright (C) 1999-2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Page table mapping constructs and function prototypes
 */
#include <asm/io.h>

/** 20130309
 * page table mapping을 위한 구조체
 *   virtual : mapping될 가상 address 주소
 *   pfn     : mapping할 물리 page frame number (physical address로 변환해 사용)
 *   length  : mapping시킬 크기
 *   type    : 속성 테이블 mem_types의 멤버 지정 (arch/arm/mm/mmu.c)
 **/
struct map_desc {
	unsigned long virtual;
	unsigned long pfn;
	unsigned long length;
	unsigned int type;
};

/* types 0-3 are defined in asm/io.h */
#define MT_UNCACHED		4
#define MT_CACHECLEAN		5
#define MT_MINICLEAN		6
#define MT_LOW_VECTORS		7
#define MT_HIGH_VECTORS		8
#define MT_MEMORY		9
#define MT_ROM			10
#define MT_MEMORY_NONCACHED	11
#define MT_MEMORY_DTCM		12
#define MT_MEMORY_ITCM		13
#define MT_MEMORY_SO		14
#define MT_MEMORY_DMA_READY	15

#ifdef CONFIG_MMU
extern void iotable_init(struct map_desc *, int);

struct mem_type;
extern const struct mem_type *get_mem_type(unsigned int type);
/*
 * external interface to remap single page with appropriate type
 */
extern int ioremap_page(unsigned long virt, unsigned long phys,
			const struct mem_type *mtype);
#else
#define iotable_init(map,num)	do { } while (0)
#endif
