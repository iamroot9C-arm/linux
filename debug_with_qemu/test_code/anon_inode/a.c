/**
 * anon inode를 생성하는 예제
 *	=> 핸들을 닫기 전까지 접근은 가능하지만, 파일시스템에서는 접근할 수 없다.
 *
 * [참고] http://stackoverflow.com/questions/4508998/what-is-anonymous-inode
 **/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <stdio.h>



char buf[256] = { 0, };

int main(void)
{
	int c, ret;
	int fd = open( "./file", O_CREAT | O_RDWR, 0666 );

	c = getchar();

	unlink( "./file" );

	ret = write(fd, "hello\n", 6);
	printf("write ret:%d\n", ret);

	lseek(fd, 0, SEEK_SET);
	ret = read(fd, buf, 6);

	printf("read ret:%d\n", ret);
	c = getchar();

	printf("aaa %s\n", buf);

	c = getchar();

	return 0;
}
