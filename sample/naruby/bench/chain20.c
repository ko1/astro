#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f19(int64_t n) { return n; }
static A int64_t f18(int64_t n) { return f19(n); }
static A int64_t f17(int64_t n) { return f18(n); }
static A int64_t f16(int64_t n) { return f17(n); }
static A int64_t f15(int64_t n) { return f16(n); }
static A int64_t f14(int64_t n) { return f15(n); }
static A int64_t f13(int64_t n) { return f14(n); }
static A int64_t f12(int64_t n) { return f13(n); }
static A int64_t f11(int64_t n) { return f12(n); }
static A int64_t f10(int64_t n) { return f11(n); }
static A int64_t f9(int64_t n)  { return f10(n); }
static A int64_t f8(int64_t n)  { return f9(n); }
static A int64_t f7(int64_t n)  { return f8(n); }
static A int64_t f6(int64_t n)  { return f7(n); }
static A int64_t f5(int64_t n)  { return f6(n); }
static A int64_t f4(int64_t n)  { return f5(n); }
static A int64_t f3(int64_t n)  { return f4(n); }
static A int64_t f2(int64_t n)  { return f3(n); }
static A int64_t f1(int64_t n)  { return f2(n); }
static A int64_t f0(int64_t n)  { return f1(n); }

int main(int argc, char **argv)
{
    int64_t sz = argc > 1 ? atoll(argv[1]) : 50 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
