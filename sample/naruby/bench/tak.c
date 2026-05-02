#include <stdio.h>

long tak(long x, long y, long z)
{
    if (y < x) {
        return tak(tak(x - 1, y, z),
                   tak(y - 1, z, x),
                   tak(z - 1, x, y));
    } else {
        return z;
    }
}

int main(int argc, const char *argv[])
{
    long r = tak(30, 22, 12);
    printf("%ld\n", r);
    (void)argc; (void)argv;
    return 0;
}
