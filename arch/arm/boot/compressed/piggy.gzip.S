	.section .piggydata,#alloc
	.globl	input_data
input_data:
/** 20120901
	include binary.	
	piggy.gzip 은 Image 을 압축한 코드. 
	.piggy.gzip.cmd 에서 arch/arm/boot/compressed/../Image 를 gzip으로 압축한다. 
**/
	.incbin	"arch/arm/boot/compressed/piggy.gzip"
	.globl	input_data_end
input_data_end:
