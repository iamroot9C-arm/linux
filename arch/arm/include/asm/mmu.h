#ifndef __ARM_MMU_H
#define __ARM_MMU_H

#ifdef CONFIG_MMU

/** 20160604
 * mm_struct 중 context에 해당하는 정보를 담은 구조체.
 **/
typedef struct {
	/** 20150801
	 * CONFIG_CPU_HAS_ASID가 정의되어 있다.
	 **/
#ifdef CONFIG_CPU_HAS_ASID
	unsigned int id;
	raw_spinlock_t id_lock;
#endif
	unsigned int kvm_seq;
} mm_context_t;

/** 20160528
 * CPU가 ASID를 가진다면
 * context.id의 8bit를 ASID로 취한다.
 **/
#ifdef CONFIG_CPU_HAS_ASID
#define ASID(mm)	((mm)->context.id & 255)

/* init_mm.context.id_lock should be initialized. */
#define INIT_MM_CONTEXT(name)                                                 \
	.context.id_lock    = __RAW_SPIN_LOCK_UNLOCKED(name.context.id_lock),
#else
#define ASID(mm)	(0)
#endif

#else

/*
 * From nommu.h:
 *  Copyright (C) 2002, David McCullough <davidm@snapgear.com>
 *  modified for 2.6 by Hyok S. Choi <hyok.choi@samsung.com>
 */
typedef struct {
	unsigned long		end_brk;
} mm_context_t;

#endif

#endif
