// C reference for poly_redef.na.rb.  No redef here (C doesn't have
// runtime function redefinition); just the loop with f(x) = x + 1.
#include <stdio.h>
#include <stdint.h>

#define A __attribute__((noinline,noipa))

static A int64_t f(int64_t x) { return x + 1; }

int main(void)
{
    int64_t sz = 50 * 1000 * 1000;
    int64_t acc = 0;
    for (int64_t i = 0; i < sz; i++) {
        acc += f(10);
    }
    printf("%ld\n", (long)acc);
    return 0;
}
