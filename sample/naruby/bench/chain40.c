#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int f39(int n) { return n; }
static A int f38(int n) { return f39(n); }
static A int f37(int n) { return f38(n); }
static A int f36(int n) { return f37(n); }
static A int f35(int n) { return f36(n); }
static A int f34(int n) { return f35(n); }
static A int f33(int n) { return f34(n); }
static A int f32(int n) { return f33(n); }
static A int f31(int n) { return f32(n); }
static A int f30(int n) { return f31(n); }
static A int f29(int n) { return f30(n); }
static A int f28(int n) { return f29(n); }
static A int f27(int n) { return f28(n); }
static A int f26(int n) { return f27(n); }
static A int f25(int n) { return f26(n); }
static A int f24(int n) { return f25(n); }
static A int f23(int n) { return f24(n); }
static A int f22(int n) { return f23(n); }
static A int f21(int n) { return f22(n); }
static A int f20(int n) { return f21(n); }
static A int f19(int n) { return f20(n); }
static A int f18(int n) { return f19(n); }
static A int f17(int n) { return f18(n); }
static A int f16(int n) { return f17(n); }
static A int f15(int n) { return f16(n); }
static A int f14(int n) { return f15(n); }
static A int f13(int n) { return f14(n); }
static A int f12(int n) { return f13(n); }
static A int f11(int n) { return f12(n); }
static A int f10(int n) { return f11(n); }
static A int f9(int n)  { return f10(n); }
static A int f8(int n)  { return f9(n); }
static A int f7(int n)  { return f8(n); }
static A int f6(int n)  { return f7(n); }
static A int f5(int n)  { return f6(n); }
static A int f4(int n)  { return f5(n); }
static A int f3(int n)  { return f4(n); }
static A int f2(int n)  { return f3(n); }
static A int f1(int n)  { return f2(n); }
static A int f0(int n)  { return f1(n); }

int main(void)
{
    int sz = 25 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%d\n", acc);
    return 0;
}
