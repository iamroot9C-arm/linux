#ifndef __ASM_ARM_SWITCH_TO_H
#define __ASM_ARM_SWITCH_TO_H

#include <linux/thread_info.h>

/*
 * switch_to(prev, next) should switch from task `prev' to `next'
 * `prev' will never be the same as `next'.  schedule() itself
 * contains the memory barrier to tell GCC not to cache `current'.
 */
/** 20150118
 * task 전환과정의 arm specific implementation.
 *
 * 첫번째 argument : 이전 task의 struct task_struct *.
 * 두번째 argument : 이전 task의 struct thread_info *.
 * 세번째 argument : 다음 task의 struct thread_info *.
 *
 * 구현 방식 때문에 이전 task와 다음 task는 같은 task여서는 안 된다.
 **/
extern struct task_struct *__switch_to(struct task_struct *, struct thread_info *, struct thread_info *);

/** 20150118
 * 공통 함수 switch_to를 arm specific implementation으로 정의.
 * 매개변수로 전달된 task_struct을 통해 thread_info(문맥 정보)을 얻어 전환한다.
 *
 * 이전 수행하던 task에서 다음 수행할 task로 문맥(register, stack)을 전환하고,
 * 마지막 수행하던 task를 리턴한다.
 **/
#define switch_to(prev,next,last)					\
do {									\
	last = __switch_to(prev,task_thread_info(prev), task_thread_info(next));	\
} while (0)

#endif /* __ASM_ARM_SWITCH_TO_H */
