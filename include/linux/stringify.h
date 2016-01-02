#ifndef __LINUX_STRINGIFY_H
#define __LINUX_STRINGIFY_H

/* Indirect stringification.  Doing two levels allows the parameter to be a
 * macro itself.  For example, compile with -DFOO=bar, __stringify(FOO)
 * converts to "bar".
 */
/** 20121208
 * symbol -> "symbol"
 *
 * 2중 macro를 사용한 이유는 AAA라는 매크로가 정의된 경우,
 * AAA라는 심볼이 아니라 AAA가 가리키는 심볼을 문자열화 시켜주기 위함이다.
 *
 * #define AAA	abc
 * stringify(AAA)	-> "abc"
 **/
#define __stringify_1(x...)	#x
#define __stringify(x...)	__stringify_1(x)

#endif	/* !__LINUX_STRINGIFY_H */
