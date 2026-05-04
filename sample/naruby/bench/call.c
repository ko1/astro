// 10-level pass-through chain.  Each fk(n) just calls fk+1(n), and f9
// returns its argument unchanged.  Same shape as chain20.c / chain40.c
// but at depth 10 — the synthetic call-overhead micro-bench at small
// depth.  Functions are marked noinline+noipa so gcc cannot collapse
// the chain and we actually measure call cost (matches the naruby case
// where each call goes through a dispatcher).
#include <stdio.h>

#define A __attribute__((noinline,noipa))

static A int f9(int n) { return n; }
static A int f8(int n) { return f9(n); }
static A int f7(int n) { return f8(n); }
static A int f6(int n) { return f7(n); }
static A int f5(int n) { return f6(n); }
static A int f4(int n) { return f5(n); }
static A int f3(int n) { return f4(n); }
static A int f2(int n) { return f3(n); }
static A int f1(int n) { return f2(n); }
static A int f0(int n) { return f1(n); }

int main(void)
{
    int sz = 100 * 1000 * 1000;
    int acc = 0;
    for (int i = 0; i < sz; i++) {
        acc += f0(42);
    }
    printf("%d\n", acc);
    return 0;
}
