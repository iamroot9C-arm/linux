/*
 * kref.h - library routines for handling generic reference counted objects
 *
 * Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2004 IBM Corp.
 *
 * based on kobject.h which was:
 * Copyright (C) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (C) 2002-2003 Open Source Development Labs
 *
 * This file is released under the GPLv2.
 *
 */

#ifndef _KREF_H_
#define _KREF_H_

#include <linux/bug.h>
#include <linux/atomic.h>
#include <linux/kernel.h>

/** 20150912
 * 참조카운터용 구조체.
 **/
struct kref {
	atomic_t refcount;
};

/**
 * kref_init - initialize object.
 * @kref: object in question.
 */
/** 20150124
 * (주로 object 내에 선언된) kref 구조체를 초기화 한다.
 *
 * refcount 를 1로 설정한다.
 **/
static inline void kref_init(struct kref *kref)
{
	atomic_set(&kref->refcount, 1);
}

/**
 * kref_get - increment refcount for object.
 * @kref: object.
 */
/** 20150912
 * 오브젝트의 kref를 받아 참조 카운터를 증가시킨다.
 **/
static inline void kref_get(struct kref *kref)
{
	WARN_ON(!atomic_read(&kref->refcount));
	atomic_inc(&kref->refcount);
}

/**
 * kref_sub - subtract a number of refcounts for object.
 * @kref: object.
 * @count: Number of recounts to subtract.
 * @release: pointer to the function that will clean up the object when the
 *	     last reference to the object is released.
 *	     This pointer is required, and it is not acceptable to pass kfree
 *	     in as this function.  If the caller does pass kfree to this
 *	     function, you will be publicly mocked mercilessly by the kref
 *	     maintainer, and anyone else who happens to notice it.  You have
 *	     been warned.
 *
 * Subtract @count from the refcount, and if 0, call release().
 * Return 1 if the object was removed, otherwise return 0.  Beware, if this
 * function returns 0, you still can not count on the kref from remaining in
 * memory.  Only use the return value if you want to see if the kref is now
 * gone, not present.
 */
static inline int kref_sub(struct kref *kref, unsigned int count,
	     void (*release)(struct kref *kref))
{
	WARN_ON(release == NULL);

	/** 20150418
	 * kref의 reference count를 count만큼 감소시키고, 그 결과가 0이면 
	 * 전달받은 release 함수를 호출하여 해제한다.
	 **/
	if (atomic_sub_and_test((int) count, &kref->refcount)) {
		release(kref);
		return 1;
	}
	return 0;
}

/**
 * kref_put - decrement refcount for object.
 * @kref: object.
 * @release: pointer to the function that will clean up the object when the
 *	     last reference to the object is released.
 *	     This pointer is required, and it is not acceptable to pass kfree
 *	     in as this function.  If the caller does pass kfree to this
 *	     function, you will be publicly mocked mercilessly by the kref
 *	     maintainer, and anyone else who happens to notice it.  You have
 *	     been warned.
 *
 * Decrement the refcount, and if 0, call release().
 * Return 1 if the object was removed, otherwise return 0.  Beware, if this
 * function returns 0, you still can not count on the kref from remaining in
 * memory.  Only use the return value if you want to see if the kref is now
 * gone, not present.
 */
static inline int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	return kref_sub(kref, 1, release);
}
#endif /* _KREF_H_ */
