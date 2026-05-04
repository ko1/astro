#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f9(int64_t n) { return n + 1; }
static A int64_t f8(int64_t n) { return f9(n + 1); }
static A int64_t f7(int64_t n) { return f8(n + 1); }
static A int64_t f6(int64_t n) { return f7(n + 1); }
static A int64_t f5(int64_t n) { return f6(n + 1); }
static A int64_t f4(int64_t n) { return f5(n + 1); }
static A int64_t f3(int64_t n) { return f4(n + 1); }
static A int64_t f2(int64_t n) { return f3(n + 1); }
static A int64_t f1(int64_t n) { return f2(n + 1); }
static A int64_t f0(int64_t n) { return f1(n + 1); }

int main(void)
{
    int64_t sz = 10 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f0(0);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
