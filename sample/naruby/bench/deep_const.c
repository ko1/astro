#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t a(void) { return 1; }
static A int64_t b(void) { return a(); }
static A int64_t c(void) { return b(); }
static A int64_t d(void) { return c(); }
static A int64_t e(void) { return d(); }
static A int64_t f(void) { return e(); }
static A int64_t g(void) { return f(); }
static A int64_t h(void) { return g(); }
static A int64_t i_fn(void) { return h(); }
static A int64_t j(void) { return i_fn(); }

int main(void)
{
    int64_t sz = 100 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t k = 0; k < sz; k++) {
        acc += j();
    }
    printf("%ld\n", (long)acc);
    return 0;
}
