#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f(int64_t n) {
    if (n < 1000000000) {
        return n + 1;
    } else {
        return n * 2;
    }
}

int main(void)
{
    int64_t sz = 50 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f(42);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
