// 10-level pass-through chain.  Each fk(n) just calls fk+1(n), and f9
// returns its argument unchanged.  Same shape as chain20.c / chain40.c
// but at depth 10 — the synthetic call-overhead micro-bench at small
// depth.  Functions are marked noinline+noipa so gcc cannot collapse
// the chain and we actually measure call cost (matches the naruby case
// where each call goes through a dispatcher).
#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f9(int64_t n) { return n; }
static A int64_t f8(int64_t n) { return f9(n); }
static A int64_t f7(int64_t n) { return f8(n); }
static A int64_t f6(int64_t n) { return f7(n); }
static A int64_t f5(int64_t n) { return f6(n); }
static A int64_t f4(int64_t n) { return f5(n); }
static A int64_t f3(int64_t n) { return f4(n); }
static A int64_t f2(int64_t n) { return f3(n); }
static A int64_t f1(int64_t n) { return f2(n); }
static A int64_t f0(int64_t n) { return f1(n); }

int main(void)
{
    int64_t sz = 100 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
