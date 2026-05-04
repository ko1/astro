#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int f9(int n) { return n + 1; }
static A int f8(int n) { return f9(n + 1); }
static A int f7(int n) { return f8(n + 1); }
static A int f6(int n) { return f7(n + 1); }
static A int f5(int n) { return f6(n + 1); }
static A int f4(int n) { return f5(n + 1); }
static A int f3(int n) { return f4(n + 1); }
static A int f2(int n) { return f3(n + 1); }
static A int f1(int n) { return f2(n + 1); }
static A int f0(int n) { return f1(n + 1); }

int main(void)
{
    int sz = 10 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f0(0);
    }
    printf("%d\n", acc);
    return 0;
}
