#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int f(int n) {
    if (n < 1000000000) {
        return n + 1;
    } else {
        return n * 2;
    }
}

int main(void)
{
    int sz = 50 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f(42);
    }
    printf("%d\n", acc);
    return 0;
}
