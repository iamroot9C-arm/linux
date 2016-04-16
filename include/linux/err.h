#ifndef _LINUX_ERR_H
#define _LINUX_ERR_H

#include <linux/compiler.h>

#include <asm/errno.h>

/*
 * Kernel pointers have redundant information, so we can use a
 * scheme where we can return either an error code or a dentry
 * pointer with the same return value.
 *
 * This should be a per-architecture thing, to allow different
 * error and pointer decisions.
 */
#define MAX_ERRNO	4095

#ifndef __ASSEMBLY__

/** 20140412    
 * MAX_ERRNO -4095를 unsigned로 type casting.
 * IS_ERR_VALUE에 넘겨준 x 역시 unsigned로 type casting.
 * 음수는 0에 가까운 값일수록 절대값이 큰 값을 가진다는 원리를 이용해 error를 판별
 * 포인터의 경우 MAX_ERRNO보다 작은 값이므로 error 값이 아님.
 * 
 * x가 0인 경우 false.
 **/
#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-MAX_ERRNO)

/** 20150411    
 * error를 pointer로 casting 해 리턴.
 **/
static inline void * __must_check ERR_PTR(long error)
{
	return (void *) error;
}

/** 20160416    
 * ptr 값을 반환.
 **/
static inline long __must_check PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

/** 20140412    
 * 이전에 수행한 값이 ERR인지 판단하는 함수.
 **/
static inline long __must_check IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline long __must_check IS_ERR_OR_NULL(const void *ptr)
{
	return !ptr || IS_ERR_VALUE((unsigned long)ptr);
}

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
/** 20150314    
 * ERR값에서 const 속성을 제거.
 **/
static inline void * __must_check ERR_CAST(const void *ptr)
{
	/* cast away the const */
	return (void *) ptr;
}

static inline int __must_check PTR_RET(const void *ptr)
{
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	else
		return 0;
}

#endif

#endif /* _LINUX_ERR_H */
