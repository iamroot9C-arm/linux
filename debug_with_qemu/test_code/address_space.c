#if 0
	usage:
		sparse address_space.c -D__CHECK__
#endif
#define NULL 0

#ifdef __CHECK__
# define __noderef __attribute__((noderef))
# define __percpu   __attribute__((noderef, address_space(3)))
#else
# define __noderef 
# define __percpu
#endif

#if 0
// for test gcc warning
// # gcc address_space.c -Wall
// address_space.c:39:2: warning: unused variable ‘__vpp_verify’ [-Wunused-variable]
    (void)__vpp_verify;

#define __verify_pcpu_ptr(ptr)  do {                    \
    void __percpu *__vpp_verify = (typeof(ptr))NULL; \
} while (0)
#else
#define __verify_pcpu_ptr(ptr)  do {                    \
    const void __percpu *__vpp_verify = (typeof(ptr))NULL;      \
	    (void)__vpp_verify;                     \
} while (0)
#endif

struct per_cpu_pageset {
	int x;
};

int main(int argc, char **argv)
{
#if 1
	void * __percpu boot_pageset;
#else
	int *boot_pageset = (int *)0x1234567;
	/* address_space.c:37:9: warning: Using plain integer as NULL pointer */
	// int boot_pageset = 0x1234567;
#endif

	__verify_pcpu_ptr (boot_pageset);
#if 0
	int a = 3;
	int __noderef *pa_noderef;
	int *pa;

	pa_noderef = (int __noderef *)&a;
	pa = &a;

	*pa = 0;
	*pa_noderef = 0;
#endif

	return 0;
}
