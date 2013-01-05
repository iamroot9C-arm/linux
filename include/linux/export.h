#ifndef _LINUX_EXPORT_H
#define _LINUX_EXPORT_H
/*
 * Export symbols from the kernel to modules.  Forked from module.h
 * to reduce the amount of pointless cruft we feed to gcc when only
 * exporting a simple symbol or two.
 *
 * If you feel the need to add #include <linux/foo.h> to this file
 * then you are doing something wrong and should go away silently.
 */

/* Some toolchains use a `_' prefix for all user symbols. */
#ifdef CONFIG_SYMBOL_PREFIX
#define MODULE_SYMBOL_PREFIX CONFIG_SYMBOL_PREFIX
#else
#define MODULE_SYMBOL_PREFIX ""
#endif

struct kernel_symbol
{
	unsigned long value;
	const char *name;
};

#ifdef MODULE
extern struct module __this_module;
#define THIS_MODULE (&__this_module)
#else
#define THIS_MODULE ((struct module *)0)
#endif

#ifdef CONFIG_MODULES

#ifndef __GENKSYMS__
#ifdef CONFIG_MODVERSIONS
/* Mark the CRC weak since genksyms apparently decides not to
 * generate a checksums for some symbols */
#define __CRC_SYMBOL(sym, sec)					\
	extern void *__crc_##sym __attribute__((weak));		\
	static const unsigned long __kcrctab_##sym		\
	__used							\
	__attribute__((section("___kcrctab" sec "+" #sym), unused))	\
	= (unsigned long) &__crc_##sym;
#else
#define __CRC_SYMBOL(sym, sec)
#endif

/* For every exported symbol, place a struct in the __ksymtab section */
/** 20130105
 * extern typeof(sym) sym
 * 	: 외부 모듈에서 이 sym(함수, 변수)을 사용하기 위하여 외부 참조 선언을 한다.
 *
 * __CRC_SYMBOL : null
 *
 * __ksymtab_strings section에 symbol의 이름(문자열) 기록.
 * "___ksymtab" sec "+" symbol section에 해당 symbol의 주소와 __ksymtab_strings에서의 symbol 이름의 주소를 기록.
 *
 * EXPORT_SYMBOL 을 사용하여 export 된 symbol을 찾는 과정.. 추측 ??? 
 *	1. symbol이름으로 __ksymtab_strings에서의 위치를 찾는다. 
 *	2. 1번에서 찾은 위치를 이용하여 "___ksymtab + *" section의 주소를 찾고, 해당 심볼에 접근한다.
 *	다른 추측 : 2번에서 찾은 section주소를 가지고 kernel_symbol 구조체의 내용을 참조하여
 *				실제 symbol의 주소를 가져오고 실제 스트링의 주소 또는 스트링을 비교하여 검증한다. 
 *
 * ksymtab은 kernel내부에서의 함수 호출에서는 불필요하고, kernel module에서 kernel의 symbol 을 접근하기 위해 사용된다. 
 *
 * unused의 의미:
 * 	    This attribute, attached to a function, means that the function is meant to be possibly unused. 
 * 	    GCC does not produce a warning for this function. 
 * 	    이 심볼을 참조하는 코드가 없더라도 warning을 출력하지 말아라.
 * */
#define __EXPORT_SYMBOL(sym, sec)				\
	extern typeof(sym) sym;					\
	__CRC_SYMBOL(sym, sec)					\
	static const char __kstrtab_##sym[]			\
	__attribute__((section("__ksymtab_strings"), aligned(1))) \
	= MODULE_SYMBOL_PREFIX #sym;				\
	static const struct kernel_symbol __ksymtab_##sym	\
	__used							\
	__attribute__((section("___ksymtab" sec "+" #sym), unused))	\
	= { (unsigned long)&sym, __kstrtab_##sym }

#define EXPORT_SYMBOL(sym)					\
	__EXPORT_SYMBOL(sym, "")

#define EXPORT_SYMBOL_GPL(sym)					\
	__EXPORT_SYMBOL(sym, "_gpl")

#define EXPORT_SYMBOL_GPL_FUTURE(sym)				\
	__EXPORT_SYMBOL(sym, "_gpl_future")

#ifdef CONFIG_UNUSED_SYMBOLS
#define EXPORT_UNUSED_SYMBOL(sym) __EXPORT_SYMBOL(sym, "_unused")
#define EXPORT_UNUSED_SYMBOL_GPL(sym) __EXPORT_SYMBOL(sym, "_unused_gpl")
#else
#define EXPORT_UNUSED_SYMBOL(sym)
#define EXPORT_UNUSED_SYMBOL_GPL(sym)
#endif

#endif	/* __GENKSYMS__ */

#else /* !CONFIG_MODULES... */

#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_GPL_FUTURE(sym)
#define EXPORT_UNUSED_SYMBOL(sym)
#define EXPORT_UNUSED_SYMBOL_GPL(sym)

#endif /* CONFIG_MODULES */

#endif /* _LINUX_EXPORT_H */
