#ifndef _LINUX_THREADS_H
#define _LINUX_THREADS_H


/*
 * The default limit for the nr of threads is now in
 * /proc/sys/kernel/threads-max.
 */

/*
 * Maximum supported processors.  Setting this smaller saves quite a
 * bit of memory.  Use nr_cpu_ids instead of this except for static bitmaps.
 */
#ifndef CONFIG_NR_CPUS
/* FIXME: This should be fixed in the arch's Kconfig */
#define CONFIG_NR_CPUS	1
#endif

/* Places which use this should consider cpumask_var_t. */
/** 20130518
 * vexpress의 경우 4
 **/
#define NR_CPUS		CONFIG_NR_CPUS

#define MIN_THREADS_LEFT_FOR_ROOT 4

/*
 * This controls the default maximum pid allocated to a process
 */
/** 20150131
 * 하나의 process에 할당된 최대 PID를 설정할 default 값.
 **/
#define PID_MAX_DEFAULT (CONFIG_BASE_SMALL ? 0x1000 : 0x8000)

/*
 * A maximum of 4 million PIDs should be enough for a while.
 * [NOTE: PID/TIDs are limited to 2^29 ~= 500+ million, see futex.h.]
 */
/** 20150131
 * PID 최대치의 한계값을 계산한다.
 **/
#define PID_MAX_LIMIT (CONFIG_BASE_SMALL ? PAGE_SIZE * 8 : \
	(sizeof(long) > 4 ? 4 * 1024 * 1024 : PID_MAX_DEFAULT))

/*
 * Define a minimum number of pids per cpu.  Heuristically based
 * on original pid max of 32k for 32 cpus.  Also, increase the
 * minimum settable value for pid_max on the running system based
 * on similar defaults.  See kernel/pid.c:pidmap_init() for details.
 */
/** 20150131
 * CPU당 할당할 PID의 최소치를 정의한다.
 **/
#define PIDS_PER_CPU_DEFAULT	1024
#define PIDS_PER_CPU_MIN	8

#endif
