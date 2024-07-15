#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main()
{
    int p[2];
    if (pipe(p) != 0)
    {
        printf("pipe err");
        exit(1);
    }

    if (fork() == 0) // child
    {
        close(p[1]); // close write
        char temp;
        read(p[0], &temp, 1);
        printf("%d: received ping\n", getpid());
        close(p[0]); // close read
    }
    else
    {
        close(p[0]); // close read
        char temp = 'x';
        write(p[1], &temp, 1);
        close(p[1]); // close write
        wait((int *)0);
        printf("%d: received pong\n", getpid());
    }

    exit(0);
}