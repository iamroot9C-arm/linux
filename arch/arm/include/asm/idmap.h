#ifndef __ASM_IDMAP_H
#define __ASM_IDMAP_H

#include <linux/compiler.h>
#include <asm/pgtable.h>

/* Tag a function as requiring to be executed via an identity mapping. */
/** 20150620
 * identity mapping을 통해 실행될 함수들에 대한 tag.
 * __idmap_text_start ~ __idmap_text_end 사이에 위치한다.
 **/
#define __idmap __section(.idmap.text) noinline notrace

extern pgd_t *idmap_pgd;

void setup_mm_for_reboot(void);

#endif	/* __ASM_IDMAP_H */
