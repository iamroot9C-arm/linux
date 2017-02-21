/*
 * Derived from arch/ppc/mm/extable.c and arch/i386/mm/extable.c.
 *
 * Copyright (C) 2004 Paul Mackerras, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/sort.h>
#include <asm/uaccess.h>

#ifndef ARCH_HAS_SORT_EXTABLE
/*
 * The exception table needs to be sorted so that the binary
 * search that we use to find entries in it works properly.
 * This is used both for the kernel exception table and for
 * the exception tables of modules that get loaded.
 */
/** 20130803
 * exception table를 정렬해 두어야 binary search시에 빠르게 필요한 entry를 찾을 수 있다.
 * sort시에 사용할 compare 함수를 선언한다.
 *   - compare 기준은 insn (주소값)이 낮은 순이다.
 **/
static int cmp_ex(const void *a, const void *b)
{
	/** 20130803
	 * exception table
	 **/
	const struct exception_table_entry *x = a, *y = b;

	/* avoid overflow */
	if (x->insn > y->insn)
		return 1;
	if (x->insn < y->insn)
		return -1;
	return 0;
}

/** 20130803
 * start ~ finish까지 exception table을 insn (수행할 instruct의 주소)로 정렬한다
 **/
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish)
{
	/** 20130803
	 * exception table의 start ~ finish까지에 대해
	 * library sort 함수 호출.
	 * cmp_func에 대한 CB으로 cmp_ex 지정
	 **/
	sort(start, finish - start, sizeof(struct exception_table_entry),
	     cmp_ex, NULL);
}

#ifdef CONFIG_MODULES
/*
 * If the exception table is sorted, any referring to the module init
 * will be at the beginning or the end.
 */
void trim_init_extable(struct module *m)
{
	/*trim the beginning*/
	while (m->num_exentries && within_module_init(m->extable[0].insn, m)) {
		m->extable++;
		m->num_exentries--;
	}
	/*trim the end*/
	while (m->num_exentries &&
		within_module_init(m->extable[m->num_exentries-1].insn, m))
		m->num_exentries--;
}
#endif /* CONFIG_MODULES */
#endif /* !ARCH_HAS_SORT_EXTABLE */

#ifndef ARCH_HAS_SEARCH_EXTABLE
/*
 * Search one exception table for an entry corresponding to the
 * given instruction address, and return the address of the entry,
 * or NULL if none is found.
 * We use a binary search, and thus we assume that the table is
 * already sorted.
 */
const struct exception_table_entry *
search_extable(const struct exception_table_entry *first,
	       const struct exception_table_entry *last,
	       unsigned long value)
{
	while (first <= last) {
		const struct exception_table_entry *mid;

		mid = ((last - first) >> 1) + first;
		/*
		 * careful, the distance between value and insn
		 * can be larger than MAX_LONG:
		 */
		if (mid->insn < value)
			first = mid + 1;
		else if (mid->insn > value)
			last = mid - 1;
		else
			return mid;
        }
        return NULL;
}
#endif
