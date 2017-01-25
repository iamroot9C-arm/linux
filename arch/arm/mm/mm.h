#ifdef CONFIG_MMU

/* the upper-most page table pointer */
extern pmd_t *top_pmd;

/*
 * 0xffff8000 to 0xffffffff is reserved for any ARM architecture
 * specific hacks for copying pages efficiently, while 0xffff4000
 * is reserved for VIPT aliasing flushing by generic code.
 *
 * Note that we don't allow VIPT aliasing caches with SMP.
 */
#define COPYPAGE_MINICACHE	0xffff8000
#define COPYPAGE_V6_FROM	0xffff8000
#define COPYPAGE_V6_TO		0xffffc000
/* PFN alias flushing, for VIPT caches */
#define FLUSH_ALIAS_START	0xffff4000

/** 20130518
 * va에 해당하는 top_pte 주소에 pte를 써주고,
 * va에 해당하는 tlb를 flush 하는 함수
 **/
static inline void set_top_pte(unsigned long va, pte_t pte)
{
/** 20130511
	high vector에 해당하는 pmd의 주소 
	top_pmd = pmd_off_k(0xffff0000) 
	pte_offset_kernel로 va에 대한 pte 주소를 가져옴.
**/
	pte_t *ptep = pte_offset_kernel(top_pmd, va);
/** 20130511
	ptep에 넘어온 pte 값을 써준다. 
**/
	set_pte_ext(ptep, pte, 0);
/** 20130518
 *  pte를 새로 써주었으므로 tlb를 flush하고, barrier 를 수행한다.
**/
	local_flush_tlb_kernel_page(va);
}

static inline pte_t get_top_pte(unsigned long va)
{
	pte_t *ptep = pte_offset_kernel(top_pmd, va);
	return *ptep;
}

/** 20130216
 * 해당 pmd entry의 주소. (2 level 사용시에는 pgd=pmd)
 *
 * 20131019
 * virt가 pgd, pud, pmd를 거치는 과정에서 변환되는 pmd entry의 주소를 리턴
 **/
static inline pmd_t *pmd_off_k(unsigned long virt)
{
	/** 20130216
	 * Q. pmd_offset, pud_offset 은 무슨 역할을 할까요???
	 * A. pgd -> pud -> pmd를 따라가 virtual address에 해당하는 entry를
	 * 가져옴
	 * */
	return pmd_offset(pud_offset(pgd_offset_k(virt), virt), virt);
}

struct mem_type {
	pteval_t prot_pte;
	pmdval_t prot_l1;
	pmdval_t prot_sect;
	unsigned int domain;
};

const struct mem_type *get_mem_type(unsigned int type);

extern void __flush_dcache_page(struct address_space *mapping, struct page *page);

/*
 * ARM specific vm_struct->flags bits.
 */

/* (super)section-mapped I/O regions used by ioremap()/iounmap() */
#define VM_ARM_SECTION_MAPPING	0x80000000

/* permanent static mappings from iotable_init() */
#define VM_ARM_STATIC_MAPPING	0x40000000

/* mapping type (attributes) for permanent static mappings */
#define VM_ARM_MTYPE(mt)		((mt) << 20)
#define VM_ARM_MTYPE_MASK	(0x1f << 20)

/* consistent regions used by dma_alloc_attrs() */
#define VM_ARM_DMA_CONSISTENT	0x20000000

#endif

#ifdef CONFIG_ZONE_DMA
extern phys_addr_t arm_dma_limit;
#else
#define arm_dma_limit ((phys_addr_t)~0)
#endif

extern phys_addr_t arm_lowmem_limit;

void __init bootmem_init(void);
void arm_mm_memblock_reserve(void);
void dma_contiguous_remap(void);
