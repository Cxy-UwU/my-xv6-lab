#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(2, "Usage: xargs command [initial-args...]\n");
        exit(1);
    }

    char buf[512];
    char *args[MAXARG];
    int argi;

    // Copy initial arguments
    for (argi = 1; argi < argc && argi < MAXARG; argi++)
    {
        args[argi - 1] = argv[argi];
    }

    char *p = buf;
    int n;

    while ((n = read(0, p, sizeof(char))) > 0)
    {
        /*
        一次只读一个字符。
        如果读到了换行，直接新开一个子进程，用exec执行这个带参数的命令。
        */
        if (*p == '\n')
        {
            *p = '\0';
            args[argi - 1] = buf;
            args[argi] = 0;

            if (fork() == 0)
            {
                exec(args[0], args);
                fprintf(2, "exec %s failed\n", args[0]);
                exit(1);
            }
            else
            {
                wait(0);
            }

            p = buf;
        }
        else
        {
            p++;
        }
    }

    exit(0);
}
