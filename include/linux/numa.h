#ifndef _LINUX_NUMA_H
#define _LINUX_NUMA_H


#ifdef CONFIG_NODES_SHIFT
#define NODES_SHIFT     CONFIG_NODES_SHIFT
#else
#define NODES_SHIFT     0
#endif

/** 20130629    
 * vexpress에서는 NODES_SHIFT가 0.
 * 따라서 MAX_NUMNODES는 1.
 **/
#define MAX_NUMNODES    (1 << NODES_SHIFT)

#define	NUMA_NO_NODE	(-1)

#endif /* _LINUX_NUMA_H */
