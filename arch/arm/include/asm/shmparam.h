#ifndef _ASMARM_SHMPARAM_H
#define _ASMARM_SHMPARAM_H

/*
 * This should be the size of the virtually indexed cache/ways,
 * or page size, whichever is greater since the cache aliases
 * every size/ways bytes.
 */

/** 20130511 
SHMLBA : Segment(공유 메모리) low boundary address
참고 - http://www.spinics.net/lists/arm-kernel/msg19798.html
**/
#define	SHMLBA	(4 * PAGE_SIZE)		 /* attach addr a multiple of this */

/*
 * Enforce SHMLBA in shmat
 */
#define __ARCH_FORCE_SHMLBA

#endif /* _ASMARM_SHMPARAM_H */
