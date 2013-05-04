#ifndef LINUX_MM_DEBUG_H
#define LINUX_MM_DEBUG_H 1

/** 20130504
COMFIG_DEBUG_VM이 define되어 있으면 BUG_ON
아니면 BUILD_BUG_ON_INVALID define
**/
#ifdef CONFIG_DEBUG_VM
#define VM_BUG_ON(cond) BUG_ON(cond)
#else
#define VM_BUG_ON(cond) BUILD_BUG_ON_INVALID(cond)
#endif

#ifdef CONFIG_DEBUG_VIRTUAL
#define VIRTUAL_BUG_ON(cond) BUG_ON(cond)
#else
#define VIRTUAL_BUG_ON(cond) do { } while (0)
#endif

#endif
