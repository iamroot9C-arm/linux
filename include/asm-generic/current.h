#ifndef __ASM_GENERIC_CURRENT_H
#define __ASM_GENERIC_CURRENT_H

#include <linux/thread_info.h>

/** 20140621    
 * thread_info를 받아와 task struct를 찾아온다.
 *
 * thread_info는 stack 영역에 overlay 되고, sp를 참조해 가져오므로
 * 현재 cpu 위에서 실행 중인 task에 대한 정보를 가져오게 된다.
 *
 * thread_info는 실행된 task_struct에 대한 포인터를 보유하므로
 * 이 방법으로 현재 실행 중인 task 구조체를 가져오는 것이 유효하다.
 **/
#define get_current() (current_thread_info()->task)
#define current get_current()

#endif /* __ASM_GENERIC_CURRENT_H */
