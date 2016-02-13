#ifndef _ASMARM_CURRENT_H
#define _ASMARM_CURRENT_H

#include <linux/thread_info.h>

static inline struct task_struct *get_current(void) __attribute_const__;

/** 20160206    
 * 현재 thread_info를 통해 task_struct를 찾아온다.
 **/
static inline struct task_struct *get_current(void)
{
	/** 20130629    
	 * current_thread_info()로 task_struct을 접근해 현재 task 정보를 구해온다.
	 **/
	return current_thread_info()->task;
}

/** 20130629    
 * 현재 task 구조체를 찾아온다.
 **/
#define current (get_current())

#endif /* _ASMARM_CURRENT_H */
