#ifndef _LINUX_TIMERQUEUE_H
#define _LINUX_TIMERQUEUE_H

#include <linux/rbtree.h>
#include <linux/ktime.h>


/** 20140419
 * timerqueue_node
 *
 * expires를 기준으로 rb_tree를 구성하기 위한 자료구조.
 **/
struct timerqueue_node {
	struct rb_node node;
	ktime_t expires;
};

/** 20141108
 * hrtimer_clock_base의 active.
 *
 * head : rb tree의 root
 * next : 다음 expire될 timerqueue_node.
 **/
struct timerqueue_head {
	struct rb_root head;
	struct timerqueue_node *next;
};


extern void timerqueue_add(struct timerqueue_head *head,
				struct timerqueue_node *node);
extern void timerqueue_del(struct timerqueue_head *head,
				struct timerqueue_node *node);
extern struct timerqueue_node *timerqueue_iterate_next(
						struct timerqueue_node *node);

/**
 * timerqueue_getnext - Returns the timer with the earliest expiration time
 *
 * @head: head of timerqueue
 *
 * Returns a pointer to the timer node that has the
 * earliest expiration time.
 */
/** 20141108
 * 만료시간이 가장 가까운 timer를 리턴한다.
 **/
static inline
struct timerqueue_node *timerqueue_getnext(struct timerqueue_head *head)
{
	return head->next;
}

/** 20141108
 * timerqueue_node를 초기화 한다.
 *
 * 초기화할 주요 자료구조는 rb_node.
 **/
static inline void timerqueue_init(struct timerqueue_node *node)
{
	rb_init_node(&node->node);
}

static inline void timerqueue_init_head(struct timerqueue_head *head)
{
	head->head = RB_ROOT;
	head->next = NULL;
}
#endif /* _LINUX_TIMERQUEUE_H */
