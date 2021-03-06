/*
 * Specialised local-global spinlock. Can only be declared as global variables
 * to avoid overhead and keep things simple (and we don't want to start using
 * these inside dynamically allocated structures).
 *
 * "local/global locks" (lglocks) can be used to:
 *
 * - Provide fast exclusive access to per-CPU data, with exclusive access to
 *   another CPU's data allowed but possibly subject to contention, and to
 *   provide very slow exclusive access to all per-CPU data.
 * - Or to provide very fast and scalable read serialisation, and to provide
 *   very slow exclusive serialisation of data (not necessarily per-CPU data).
 *
 * Brlocks are also implemented as a short-hand notation for the latter use
 * case.
 *
 * Copyright 2009, 2010, Nick Piggin, Novell Inc.
 */
#ifndef __LINUX_LGLOCK_H
#define __LINUX_LGLOCK_H

#include <linux/spinlock.h>
#include <linux/lockdep.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>

/* can make br locks by using local lock for read side, global lock for write */
/** 20150221
 * big-reader lock을 lg lock (local global lock)으로 구현한다.
 *   read side는 local lock을, write side는 global lock을 사용한다.
 **/
#define br_lock_init(name)	lg_lock_init(name, #name)
#define br_read_lock(name)	lg_local_lock(name)
#define br_read_unlock(name)	lg_local_unlock(name)
#define br_write_lock(name)	lg_global_lock(name)
#define br_write_unlock(name)	lg_global_unlock(name)

#define DEFINE_BRLOCK(name)	DEFINE_LGLOCK(name)

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define LOCKDEP_INIT_MAP lockdep_init_map

#define DEFINE_LGLOCK_LOCKDEP(name)					\
 struct lock_class_key name##_lock_key;					\
 struct lockdep_map name##_lock_dep_map;				\
 EXPORT_SYMBOL(name##_lock_dep_map)

#else
/** 20150214
 * CONFIG_DEBUG_LOCK_ALLOC를 설정하지 않았음.
 **/
#define LOCKDEP_INIT_MAP(a, b, c, d)

#define DEFINE_LGLOCK_LOCKDEP(name)
#endif

/** 20150221
 * 각 core마다 spinlock을 걸어 전체 global lock을 건다.
 **/
struct lglock {
	arch_spinlock_t __percpu *lock;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lock_class_key lock_key;
	struct lockdep_map    lock_dep_map;
#endif
};

/** 20150214
 * lglock (local/global lock)을 정의한다.
 *
 * percpu lock과 percpu lock을 가리키는 global lock을 선언한다.
 **/
#define DEFINE_LGLOCK(name)						\
	DEFINE_LGLOCK_LOCKDEP(name);					\
	DEFINE_PER_CPU(arch_spinlock_t, name ## _lock)			\
	= __ARCH_SPIN_LOCK_UNLOCKED;					\
	struct lglock name = { .lock = &name ## _lock }

void lg_lock_init(struct lglock *lg, char *name);
void lg_local_lock(struct lglock *lg);
void lg_local_unlock(struct lglock *lg);
void lg_local_lock_cpu(struct lglock *lg, int cpu);
void lg_local_unlock_cpu(struct lglock *lg, int cpu);
void lg_global_lock(struct lglock *lg);
void lg_global_unlock(struct lglock *lg);

#endif
