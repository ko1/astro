#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f39(int64_t n) { return n; }
static A int64_t f38(int64_t n) { return f39(n); }
static A int64_t f37(int64_t n) { return f38(n); }
static A int64_t f36(int64_t n) { return f37(n); }
static A int64_t f35(int64_t n) { return f36(n); }
static A int64_t f34(int64_t n) { return f35(n); }
static A int64_t f33(int64_t n) { return f34(n); }
static A int64_t f32(int64_t n) { return f33(n); }
static A int64_t f31(int64_t n) { return f32(n); }
static A int64_t f30(int64_t n) { return f31(n); }
static A int64_t f29(int64_t n) { return f30(n); }
static A int64_t f28(int64_t n) { return f29(n); }
static A int64_t f27(int64_t n) { return f28(n); }
static A int64_t f26(int64_t n) { return f27(n); }
static A int64_t f25(int64_t n) { return f26(n); }
static A int64_t f24(int64_t n) { return f25(n); }
static A int64_t f23(int64_t n) { return f24(n); }
static A int64_t f22(int64_t n) { return f23(n); }
static A int64_t f21(int64_t n) { return f22(n); }
static A int64_t f20(int64_t n) { return f21(n); }
static A int64_t f19(int64_t n) { return f20(n); }
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

int main(void)
{
    int64_t sz = 25 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
