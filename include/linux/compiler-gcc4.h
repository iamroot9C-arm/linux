#ifndef __LINUX_COMPILER_H
#error "Please don't include <linux/compiler-gcc4.h> directly, include <linux/compiler.h> instead."
#endif

/* GCC 4.1.[01] miscompiles __weak */
#ifdef __KERNEL__
# if __GNUC_MINOR__ == 1 && __GNUC_PATCHLEVEL__ <= 1
#  error Your version of gcc miscompiles the __weak directive
# endif
#endif

/** 20121222
 * used
 *     This attribute, attached to a function, means that code must be emitted for the function 
 *     even if it appears that the function is not referenced. 
 *     This is useful, for example, when the function is referenced only in inline assembly. 
 *
 *     이 함수가 사용되는 부분이 없더라도 컴파일러 최적화 과정에서 제거하지 말아라..
 * */
#define __used			__attribute__((__used__))
/** 20150314
 * warn_unused_result
 *     결과물이 사용되지 않았을 경우,  경고를 출력하라.
 **/
#define __must_check 		__attribute__((warn_unused_result))
/** 20121215
 * gcc에서 builtin_offsetof 지원여부를 확인하지 못하겠음 ???
 *   (예제코드에서는 없는 것으로 나옴)
 **/
#define __compiler_offsetof(a,b) __builtin_offsetof(a,b)

#if __GNUC_MINOR__ >= 3
/* Mark functions as cold. gcc will assume any path leading to a call
   to them will be unlikely.  This means a lot of manual unlikely()s
   are unnecessary now for any paths leading to the usual suspects
   like BUG(), printk(), panic() etc. [but let's keep them for now for
   older compilers]

   Early snapshots of gcc 4.3 don't support this and we can't detect this
   in the preprocessor, but we can live with this because they're unreleased.
   Maketime probing would be overkill here.

   gcc also has a __attribute__((__hot__)) to move hot functions into
   a special section, but I don't see any sense in this right now in
   the kernel context */

/** 20121103
 * __cold__ 는 non-cold code의 locality 향상을 위해 사용된다고 함. 
 * 
 * http://www.iamroot.org/xe/7111
 * http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
 * 어떻게 사이즈에서 이득을 본다는 건지 ???  The function is optimized for size rather than speed. 
 * - 추측 #1 cold를 뺀 섹션만 캐시에 올라가므로 캐시 라인 사이즈에서 이득을 보지만, 그러면 속도도 빨라질 듯. 
 **/
#define __cold			__attribute__((__cold__))

#define __linktime_error(message) __attribute__((__error__(message)))

#if __GNUC_MINOR__ >= 5
/*
 * Mark a position in code as unreachable.  This can be used to
 * suppress control flow warnings after asm blocks that transfer
 * control elsewhere.
 *
 * Early snapshots of gcc 4.5 don't support this and we can't detect
 * this in the preprocessor, but we can live with this because they're
 * unreleased.  Really, we need to have autoconf for the kernel.
 */
#define unreachable() __builtin_unreachable()

/* Mark a function definition as prohibited from being cloned. */
#define __noclone	__attribute__((__noclone__))

#endif
#endif

#if __GNUC_MINOR__ > 0
#define __compiletime_object_size(obj) __builtin_object_size(obj, 0)
#endif
#if __GNUC_MINOR__ >= 4 && !defined(__CHECKER__)
#define __compiletime_warning(message) __attribute__((warning(message)))
#define __compiletime_error(message) __attribute__((error(message)))
#endif
