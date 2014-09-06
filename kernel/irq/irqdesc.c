/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the interrupt descriptor management code
 *
 * Detailed information is available in Documentation/DocBook/genericirq
 *
 */
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/radix-tree.h>
#include <linux/bitmap.h>

#include "internals.h"

/*
 * lockdep: we want to handle all irq_desc locks as a single lock-class:
 */
static struct lock_class_key irq_desc_lock_class;

#if defined(CONFIG_SMP)
/** 20140906    
 * irq_default_affinity를 위한 cpumask를 생성하고,
 * default로 전체 mask를 지정한다.
 **/
static void __init init_irq_default_affinity(void)
{
	alloc_cpumask_var(&irq_default_affinity, GFP_NOWAIT);
	cpumask_setall(irq_default_affinity);
}
#else
static void __init init_irq_default_affinity(void)
{
}
#endif

#ifdef CONFIG_SMP
/** 20140906    
 * irq_data의 affinity용 cpumask를 할당한다.
 **/
static int alloc_masks(struct irq_desc *desc, gfp_t gfp, int node)
{
	if (!zalloc_cpumask_var_node(&desc->irq_data.affinity, gfp, node))
		return -ENOMEM;

#ifdef CONFIG_GENERIC_PENDING_IRQ
	/** 20140906    
	 * GENERIC_PENDING_IRQ가 선언되어 있지 않음.
	 **/
	if (!zalloc_cpumask_var_node(&desc->pending_mask, gfp, node)) {
		free_cpumask_var(desc->irq_data.affinity);
		return -ENOMEM;
	}
#endif
	return 0;
}

/** 20140906    
 * irq_desc 중 smp 관련된 부분을 초기화 한다.
 **/
static void desc_smp_init(struct irq_desc *desc, int node)
{
	desc->irq_data.node = node;
	/** 20140906    
	 * default affinity를 복사해 초기화.
	 **/
	cpumask_copy(desc->irq_data.affinity, irq_default_affinity);
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_clear(desc->pending_mask);
#endif
}

static inline int desc_node(struct irq_desc *desc)
{
	return desc->irq_data.node;
}

#else
static inline int
alloc_masks(struct irq_desc *desc, gfp_t gfp, int node) { return 0; }
static inline void desc_smp_init(struct irq_desc *desc, int node) { }
static inline int desc_node(struct irq_desc *desc) { return 0; }
#endif

/** 20140906    
 * irq_desc를 기본값으로 초기화 한다.
 **/
static void desc_set_defaults(unsigned int irq, struct irq_desc *desc, int node,
		struct module *owner)
{
	int cpu;

	desc->irq_data.irq = irq;
	desc->irq_data.chip = &no_irq_chip;
	desc->irq_data.chip_data = NULL;
	desc->irq_data.handler_data = NULL;
	desc->irq_data.msi_desc = NULL;
	irq_settings_clr_and_set(desc, ~0, _IRQ_DEFAULT_INIT_FLAGS);
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);
	desc->handle_irq = handle_bad_irq;
	desc->depth = 1;
	desc->irq_count = 0;
	desc->irqs_unhandled = 0;
	desc->name = NULL;
	desc->owner = owner;
	for_each_possible_cpu(cpu)
		*per_cpu_ptr(desc->kstat_irqs, cpu) = 0;
	desc_smp_init(desc, node);
}

/** 20140906    
 * irq 개수.
 **/
int nr_irqs = NR_IRQS;
EXPORT_SYMBOL_GPL(nr_irqs);

static DEFINE_MUTEX(sparse_irq_lock);
/** 20140906    
 * irq 번호가 할당되었음을 기록하는 전역 bitmap.
 * 크기는 NR_IRQS와 같다.
 **/
static DECLARE_BITMAP(allocated_irqs, IRQ_BITMAP_BITS);

#ifdef CONFIG_SPARSE_IRQ

static RADIX_TREE(irq_desc_tree, GFP_KERNEL);

static void irq_insert_desc(unsigned int irq, struct irq_desc *desc)
{
	radix_tree_insert(&irq_desc_tree, irq, desc);
}

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return radix_tree_lookup(&irq_desc_tree, irq);
}
EXPORT_SYMBOL(irq_to_desc);

static void delete_irq_desc(unsigned int irq)
{
	radix_tree_delete(&irq_desc_tree, irq);
}

#ifdef CONFIG_SMP
static void free_masks(struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_PENDING_IRQ
	free_cpumask_var(desc->pending_mask);
#endif
	free_cpumask_var(desc->irq_data.affinity);
}
#else
static inline void free_masks(struct irq_desc *desc) { }
#endif

static struct irq_desc *alloc_desc(int irq, int node, struct module *owner)
{
	struct irq_desc *desc;
	gfp_t gfp = GFP_KERNEL;

	desc = kzalloc_node(sizeof(*desc), gfp, node);
	if (!desc)
		return NULL;
	/* allocate based on nr_cpu_ids */
	desc->kstat_irqs = alloc_percpu(unsigned int);
	if (!desc->kstat_irqs)
		goto err_desc;

	if (alloc_masks(desc, gfp, node))
		goto err_kstat;

	raw_spin_lock_init(&desc->lock);
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);

	desc_set_defaults(irq, desc, node, owner);

	return desc;

err_kstat:
	free_percpu(desc->kstat_irqs);
err_desc:
	kfree(desc);
	return NULL;
}

static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	unregister_irq_proc(irq, desc);

	mutex_lock(&sparse_irq_lock);
	delete_irq_desc(irq);
	mutex_unlock(&sparse_irq_lock);

	free_masks(desc);
	free_percpu(desc->kstat_irqs);
	kfree(desc);
}

static int alloc_descs(unsigned int start, unsigned int cnt, int node,
		       struct module *owner)
{
	struct irq_desc *desc;
	int i;

	for (i = 0; i < cnt; i++) {
		desc = alloc_desc(start + i, node, owner);
		if (!desc)
			goto err;
		mutex_lock(&sparse_irq_lock);
		irq_insert_desc(start + i, desc);
		mutex_unlock(&sparse_irq_lock);
	}
	return start;

err:
	for (i--; i >= 0; i--)
		free_desc(start + i);

	mutex_lock(&sparse_irq_lock);
	bitmap_clear(allocated_irqs, start, cnt);
	mutex_unlock(&sparse_irq_lock);
	return -ENOMEM;
}

static int irq_expand_nr_irqs(unsigned int nr)
{
	if (nr > IRQ_BITMAP_BITS)
		return -ENOMEM;
	nr_irqs = nr;
	return 0;
}

int __init early_irq_init(void)
{
	int i, initcnt, node = first_online_node;
	struct irq_desc *desc;

	init_irq_default_affinity();

	/* Let arch update nr_irqs and return the nr of preallocated irqs */
	initcnt = arch_probe_nr_irqs();
	printk(KERN_INFO "NR_IRQS:%d nr_irqs:%d %d\n", NR_IRQS, nr_irqs, initcnt);

	if (WARN_ON(nr_irqs > IRQ_BITMAP_BITS))
		nr_irqs = IRQ_BITMAP_BITS;

	if (WARN_ON(initcnt > IRQ_BITMAP_BITS))
		initcnt = IRQ_BITMAP_BITS;

	if (initcnt > nr_irqs)
		nr_irqs = initcnt;

	for (i = 0; i < initcnt; i++) {
		desc = alloc_desc(i, node, NULL);
		set_bit(i, allocated_irqs);
		irq_insert_desc(i, desc);
	}
	return arch_early_irq_init();
}

#else /* !CONFIG_SPARSE_IRQ */

/** 20140906    
 * IRQ수만큼 irq_desc 배열을 정의. 초기값은 모두 handle_bad_irq.
 **/
struct irq_desc irq_desc[NR_IRQS] __cacheline_aligned_in_smp = {
	[0 ... NR_IRQS-1] = {
		.handle_irq	= handle_bad_irq,
		.depth		= 1,
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(irq_desc->lock),
	}
};

/** 20140906    
 * irq 관련 자료구조, 특히 irq_desc를 초기화 한다.
 **/
int __init early_irq_init(void)
{
	int count, i, node = first_online_node;
	struct irq_desc *desc;

	init_irq_default_affinity();

	/** 20140906    
	 * IRQS 개수 출력.
	 *   - architecture마다 다르다.
	 **/
	printk(KERN_INFO "NR_IRQS:%d\n", NR_IRQS);

	desc = irq_desc;
	count = ARRAY_SIZE(irq_desc);

	/** 20140906    
	 * IRQ 개수만큼 순회하며 irq_desc를 초기화 한다.
	 **/
	for (i = 0; i < count; i++) {
		desc[i].kstat_irqs = alloc_percpu(unsigned int);
		alloc_masks(&desc[i], GFP_KERNEL, node);
		raw_spin_lock_init(&desc[i].lock);
		lockdep_set_class(&desc[i].lock, &irq_desc_lock_class);
		desc_set_defaults(i, &desc[i], node, NULL);
	}
	/** 20140906    
	 * architecture specific 한 irq init 함수를 호출한다.
	 **/
	return arch_early_irq_init();
}

/** 20140906    
 * irq번째 irq_desc의 위치를 리턴한다.
 **/
struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}

static void free_desc(unsigned int irq)
{
	dynamic_irq_cleanup(irq);
}

/** 20140906    
 * irq_desc의 owner를 지정하고 시작 위치를 리턴한다.
 **/
static inline int alloc_descs(unsigned int start, unsigned int cnt, int node,
			      struct module *owner)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		struct irq_desc *desc = irq_to_desc(start + i);

		desc->owner = owner;
	}
	return start;
}

/** 20140906    
 * SPARSE_IRQ가 아닌 경우 irq의 수를 확장되지 않는다.
 **/
static int irq_expand_nr_irqs(unsigned int nr)
{
	return -ENOMEM;
}

#endif /* !CONFIG_SPARSE_IRQ */

/**
 * generic_handle_irq - Invoke the handler for a particular irq
 * @irq:	The irq number to handle
 *
 */
int generic_handle_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return -EINVAL;
	generic_handle_irq_desc(irq, desc);
	return 0;
}
EXPORT_SYMBOL_GPL(generic_handle_irq);

/* Dynamic interrupt handling */

/**
 * irq_free_descs - free irq descriptors
 * @from:	Start of descriptor range
 * @cnt:	Number of consecutive irqs to free
 */
void irq_free_descs(unsigned int from, unsigned int cnt)
{
	int i;

	if (from >= nr_irqs || (from + cnt) > nr_irqs)
		return;

	for (i = 0; i < cnt; i++)
		free_desc(from + i);

	mutex_lock(&sparse_irq_lock);
	bitmap_clear(allocated_irqs, from, cnt);
	mutex_unlock(&sparse_irq_lock);
}
EXPORT_SYMBOL_GPL(irq_free_descs);

/**
 * irq_alloc_descs - allocate and initialize a range of irq descriptors
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate.
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 *
 * Returns the first irq number or error code
 */
/** 20140906    
 * irq descriptor를 cnt만큼 할당하고 초기화 한다.
 **/
int __ref
__irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node,
		  struct module *owner)
{
	int start, ret;

	if (!cnt)
		return -EINVAL;

	/** 20140906    
	 * irq가 -1이 아닌 지정값인 경우, from은 irq로 조정된다.
	 **/
	if (irq >= 0) {
		if (from > irq)
			return -EINVAL;
		from = irq;
	}

	mutex_lock(&sparse_irq_lock);

	/** 20140906    
	 * allocated_irqs에서 from부터 IRQ_BITMAP_BITS까지 nr개의 연속적인 0이 나오는 위치를 가져온다.
	 **/
	start = bitmap_find_next_zero_area(allocated_irqs, IRQ_BITMAP_BITS,
					   from, cnt, 0);
	ret = -EEXIST;
	/** 20140906    
	 * irq가 -1이 아닌 지정값이고, 찾은 start와 irq가 다르면 실패이다.
	 **/
	if (irq >=0 && start != irq)
		goto err;

	/** 20140906    
	 * start부터 cnt만큼 확보한 크기가 nr_irqs(NR_IRQS)를 초과하면 확장하고,
	 * 실패하면 에러처리 한다.
	 **/
	if (start + cnt > nr_irqs) {
		ret = irq_expand_nr_irqs(start + cnt);
		if (ret)
			goto err;
	}

	/** 20140906    
	 * allocated_irqs에 start부터 cnt개를 설정한다.
	 **/
	bitmap_set(allocated_irqs, start, cnt);
	mutex_unlock(&sparse_irq_lock);
	return alloc_descs(start, cnt, node, owner);

err:
	mutex_unlock(&sparse_irq_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(__irq_alloc_descs);

/**
 * irq_reserve_irqs - mark irqs allocated
 * @from:	mark from irq number
 * @cnt:	number of irqs to mark
 *
 * Returns 0 on success or an appropriate error code
 */
/** 20140906    
 * from부터 cnt개의 irq 번호를 할당되었다고 기록한다.
 **/
int irq_reserve_irqs(unsigned int from, unsigned int cnt)
{
	unsigned int start;
	int ret = 0;

	if (!cnt || (from + cnt) > nr_irqs)
		return -EINVAL;

	mutex_lock(&sparse_irq_lock);
	/** 20140906    
	 * allocated_irqs에서 start를 다시 찾아 from과 같다면 여전히 사용할 수 있는
	 * 상태이므로 bitmap에 start부터 cnt개만큼 설정한다.
	 *
	 * 그렇지 않다면 irq 번호가 다른 곳에서 사용 중이므로 에러를 리턴한다.
	 **/
	start = bitmap_find_next_zero_area(allocated_irqs, nr_irqs, from, cnt, 0);
	if (start == from)
		bitmap_set(allocated_irqs, start, cnt);
	else
		ret = -EEXIST;
	mutex_unlock(&sparse_irq_lock);
	return ret;
}

/**
 * irq_get_next_irq - get next allocated irq number
 * @offset:	where to start the search
 *
 * Returns next irq number after offset or nr_irqs if none is found.
 */
unsigned int irq_get_next_irq(unsigned int offset)
{
	return find_next_bit(allocated_irqs, nr_irqs, offset);
}

/** 20140906    
 * irq에 해당하는 irq_desc를 찾아 spinlock을 걸고 리턴한다.
 **/
struct irq_desc *
__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
		    unsigned int check)
{
	/** 20140906    
	 * irq에 해당하는 irq desc를 얻어온다.
	 **/
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		/** 20140906    
		 * check에 _IRQ_DESC_CHECK 요청이 포함되어 있다면
		 * _IRQ_DESC_PERCPU 체크인데 status에 devid가 포함되어 있지 않거나
		 * 또는 그 반대의 경우라면 NULL을 리턴한다.
		 **/
		if (check & _IRQ_DESC_CHECK) {
			if ((check & _IRQ_DESC_PERCPU) &&
			    !irq_settings_is_per_cpu_devid(desc))
				return NULL;

			if (!(check & _IRQ_DESC_PERCPU) &&
			    irq_settings_is_per_cpu_devid(desc))
				return NULL;
		}

		/** 20140906    
		 * bus가 지정된 경우 irq chip의 bus lock을 건다. 
		 **/
		if (bus)
			chip_bus_lock(desc);
		/** 20140906    
		 * irq_desc에 spinlock을 건다.
		 **/
		raw_spin_lock_irqsave(&desc->lock, *flags);
	}
	return desc;
}

/** 20140906    
 * irq_desc의 spinlock을 해제한다.
 *
 * __irq_get_desc_lock에 대응.
 **/
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus)
{
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	if (bus)
		chip_bus_sync_unlock(desc);
}

/** 20140906    
 * irq가 percpu devid를 갖도록 설정한다.
 **/
int irq_set_percpu_devid(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return -EINVAL;

	if (desc->percpu_enabled)
		return -EINVAL;

	/** 20140906    
	 * percpu_enabled를 동적할당 받아 0으로 초기화 한다.
	 **/
	desc->percpu_enabled = kzalloc(sizeof(*desc->percpu_enabled), GFP_KERNEL);

	if (!desc->percpu_enabled)
		return -ENOMEM;

	/** 20140906    
	 * irq의 속성에 devid가 per_cpu 임을 설정한다.
	 **/
	irq_set_percpu_devid_flags(irq);
	return 0;
}

/**
 * dynamic_irq_cleanup - cleanup a dynamically allocated irq
 * @irq:	irq number to initialize
 */
void dynamic_irq_cleanup(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	unsigned long flags;

	raw_spin_lock_irqsave(&desc->lock, flags);
	desc_set_defaults(irq, desc, desc_node(desc), NULL);
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc && desc->kstat_irqs ?
			*per_cpu_ptr(desc->kstat_irqs, cpu) : 0;
}

unsigned int kstat_irqs(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	int cpu;
	int sum = 0;

	if (!desc || !desc->kstat_irqs)
		return 0;
	for_each_possible_cpu(cpu)
		sum += *per_cpu_ptr(desc->kstat_irqs, cpu);
	return sum;
}
