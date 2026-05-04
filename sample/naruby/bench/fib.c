#include <stdio.h>
#include <stdint.h>

int64_t
fib(int64_t n)
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
    int64_t r = fib(40);
    printf("%ld\n", (long)r);
    (void)argc; (void)argv;
    return 0;
}
