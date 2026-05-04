#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t h(int64_t n) { return n + 1; }
static A int64_t g(int64_t n) { return h(n) * 2; }
static A int64_t f(int64_t n) { return g(n) - 3; }

int main(void)
{
    int64_t sz = 30 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f(10);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
