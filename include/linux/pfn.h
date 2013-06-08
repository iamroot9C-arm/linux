#ifndef _LINUX_PFN_H_
#define _LINUX_PFN_H_

#ifndef __ASSEMBLY__
#include <linux/types.h>
#endif

/** 20130608    
 * PAGE_SIZE 단위로 정렬되어 있으면 그대로 리턴.
 * 정렬되어 있지 않으면 다음 PAGE_SIZE 단위로 정렬해 리턴.
 **/
#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
/** 20130330    
 * PFN_UP  : address x의 다음 pfn을 가리킴. round up
 * PFN_DOWN: round down한 pfn값.
 **/
#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
/** 20130330    
 * pfn을 PAGE_SHIFT 해서 물리주소를 리턴
 **/
#define PFN_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT)

#endif
