#include <stdio.h>

long find_first_factor(long n)
{
    long i = 2;
    while (i * i <= n) {
        if (n % i == 0) {
            return i;
        }
        i++;
    }
    return n;
}

int main(int argc, const char *argv[])
{
    long sum = 0;
    for (long n = 2; n < 2000000; n++) {
        sum += find_first_factor(n);
    }
    printf("%ld\n", sum);
    (void)argc; (void)argv;
    return 0;
}
