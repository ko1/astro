#include <stdio.h>

long collatz_steps(long n)
{
    long steps = 0;
    while (n != 1) {
        if (n % 2 == 0) {
            n = n / 2;
        } else {
            n = n * 3 + 1;
        }
        steps++;
    }
    return steps;
}

int main(int argc, const char *argv[])
{
    long sum = 0;
    for (long i = 1; i < 1000000; i++) {
        sum += collatz_steps(i);
    }
    printf("%ld\n", sum);
    (void)argc; (void)argv;
    return 0;
}
