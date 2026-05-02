#include <stdio.h>

long my_gcd(long a, long b)
{
    while (b != 0) {
        long t = b;
        b = a % b;
        a = t;
    }
    return a;
}

int main(int argc, const char *argv[])
{
    long acc = 0;
    for (int i = 0; i < 50000000; i++) {
        acc = acc + my_gcd(2147483647L, 1073741823L);
    }
    printf("%ld\n", acc);
    (void)argc; (void)argv;
    return 0;
}
