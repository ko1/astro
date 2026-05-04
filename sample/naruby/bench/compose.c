#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int h(int n) { return n + 1; }
static A int g(int n) { return h(n) * 2; }
static A int f(int n) { return g(n) - 3; }

int main(void)
{
    int sz = 30 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f(10);
    }
    printf("%d\n", acc);
    return 0;
}
