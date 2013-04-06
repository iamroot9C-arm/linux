/*
 * list_for_each_entry
 * list_head가 초기화 되지 않았을 경우에 대한 테스트
 *
 * $ gcc list_for_each_entry.c
 * $ ./a.out
 */
#include <stdio.h>

#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#define __compiler_offsetof(a,b) __builtin_offsetof(a,b)

#define container_of(ptr, type, member) ({          \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type,member) );})

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define PRINT(pos, member, head)		\
	printf ("&pos->member: %p, head: %p\n", &pos->member, head)
#define list_for_each_entry(pos, head, member)              \
	for (pos = list_entry((head)->next, typeof(*pos), member), PRINT(pos, member, head) ; \
			&pos->member != (head);    \
			pos = list_entry(pos->member.next, typeof(*pos), member))

#define LIST_HEAD_INIT(name) { &(name), &(name) }

struct list_head {
	struct list_head *next, *prev;
};


typedef struct bootmem_data {
    unsigned long node_min_pfn;
    unsigned long node_low_pfn;
    void *node_bootmem_map;
    unsigned long last_end_off;
    unsigned long hint_idx;
    struct list_head list;
} bootmem_data_t;

static struct list_head bdata_list = LIST_HEAD_INIT(bdata_list);


int main(void)
{
#if 0
	printf ("%d\n", sizeof (unsigned long));
	printf ("%d\n", offsetof(bootmem_data_t, list));
#endif

    bootmem_data_t *ent;
	ent = list_entry((&bdata_list)->next, bootmem_data_t, list);
	printf ("%p\n", &(ent->list));
	printf ("%p\n", &bdata_list);

#if 1
	list_for_each_entry(ent, &bdata_list, list) {
    }
#endif

	return 0;
}

