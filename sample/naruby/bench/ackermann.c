#include <stdio.h>

long ack(long m, long n)
{
    if (m == 0) {
        return n + 1;
    } else if (n == 0) {
        return ack(m - 1, 1);
    } else {
        return ack(m - 1, ack(m, n - 1));
    }
}

int main(int argc, const char *argv[])
{
    long r = ack(3, 11);
    printf("%ld\n", r);
    (void)argc; (void)argv;
    return 0;
}
