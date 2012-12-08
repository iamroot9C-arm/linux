/*
 *  arch/arm/include/asm/glue.h
 *
 *  Copyright (C) 1997-1999 Russell King
 *  Copyright (C) 2000-2002 Deep Blue Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This file provides the glue to stick the processor-specific bits
 *  into the kernel in an efficient manner.  The idea is to use branches
 *  when we're only targeting one class of TLB, or indirect calls
 *  when we're targeting multiple classes of TLBs.
 */
#ifdef __KERNEL__

#ifdef __STDC__
/** 20121208
 * gcc에서 내장되어 있음. 
 */
#define ____glue(name,fn)	name##fn
#else
/** 20121208
 * " / ** / " 는 pre process 순서에 따라 name과 fn이 하나로 합쳐지지 않도록 방지하는 역할.
 **/
#define ____glue(name,fn)	name/**/fn
#endif
#define __glue(name,fn)		____glue(name,fn)

#endif
