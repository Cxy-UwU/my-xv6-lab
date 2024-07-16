#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/*
多进程素数筛的工作原理：https://swtch.com/~rsc/thread/
p = get a number from left neighbor
print p
loop:
    n = get a number from left neighbor
    if (p does not divide n)
        send n to right neighbor
*/
int main(void)
{
    int pleft[2];
    pipe(pleft);

    if (fork() == 0)
    {
        // 子进程处理筛选
        int prime;
        close(pleft[1]); // 子进程不需要写入初始管道

        while (read(pleft[0], &prime, sizeof(prime)) != 0)
        {
            printf("prime %d\n", prime);

            int pright[2];
            pipe(pright);

            if (fork() == 0)
            {
                // 下一个子进程处理剩余的数字
                close(pright[1]);
                close(pleft[0]);
                pleft[0] = pright[0]; // 把当前的读端换成新管道的读端
                continue;
            }
            else
            {
                // 当前子进程过滤数字并传递给下一个子进程
                close(pright[0]);
                int num;
                while (read(pleft[0], &num, sizeof(num)) != 0)
                {
                    if (num % prime != 0)
                    {
                        write(pright[1], &num, sizeof(num));
                    }
                }
                close(pright[1]);
                close(pleft[0]);
                wait(0); // 等待子进程完成
                exit(0);
            }
        }
        close(pleft[0]);
        exit(0);
    }
    else
    {
        // 父进程写入数字 2 到 35
        close(pleft[0]);
        for (int i = 2; i <= 35; i++)
        {
            write(pleft[1], &i, sizeof(i));
        }
        close(pleft[1]);
        wait(0); // 等待所有子进程完成
        exit(0);
    }
}