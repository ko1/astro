#include <stdio.h>

int
fib(int n)
{
    if (n < 2) {
        return 1;
    }
    else {
        return fib(n-2) + fib(n-1);
    }
}

int
main(int argc, const char *argv[])
{
    int r = fib(40);
    printf("%d\n", r);
    (void)argc; (void)argv;
    return 0;
}
