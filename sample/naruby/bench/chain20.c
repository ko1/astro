#include <stdio.h>
#include <stdlib.h>

#define A __attribute__((noinline,noipa))

static A int f19(int n) { return n; }
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

int main(int argc, char **argv)
{
    int sz = argc > 1 ? atoi(argv[1]) : 50 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%d\n", acc);
    return 0;
}
