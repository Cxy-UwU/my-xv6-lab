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
        close(p[1]); // close write end
        char temp;
        read(p[0], &temp, 1);
        printf("%d: received ping\n", getpid());
        write(p[0], &temp, 1); // write back to parent
        close(p[0]);           // close read end
        exit(0);
    }
    else
    {
        close(p[0]); // close read end
        char temp = 'x';
        write(p[1], &temp, 1);
        close(p[1]); // close write end
        wait((int *)0);
        close(p[1]);          // close write end
        read(p[0], &temp, 1); // read from child
        printf("%d: received pong\n", getpid());
    }

    exit(0);
}
