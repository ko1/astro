#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int a(void) { return 1; }
static A int b(void) { return a(); }
static A int c(void) { return b(); }
static A int d(void) { return c(); }
static A int e(void) { return d(); }
static A int f(void) { return e(); }
static A int g(void) { return f(); }
static A int h(void) { return g(); }
static A int i_fn(void) { return h(); }
static A int j(void) { return i_fn(); }

int main(void)
{
    int sz = 100 * 1000 * 1000;
    int acc = 0;
    for (int k = 0; k < sz; k++) {
        acc += j();
    }
    printf("%d\n", acc);
    return 0;
}
