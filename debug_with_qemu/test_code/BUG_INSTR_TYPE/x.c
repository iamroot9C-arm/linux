#include <stdio.h>

int main(void) {
	asm volatile (	"1:\t" ".word" " 0xe7f001f2" "\n"
					"2:nop\n"
					"3:\t .word 1b, 2b\n"
				 );
	return 0;
}
