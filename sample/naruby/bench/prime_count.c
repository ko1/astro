#include <stdio.h>
#include <stdbool.h>

static bool is_prime(long n) {
    if (n < 2) return false;
    for (long i = 2; i * i <= n; i++)
        if (n % i == 0) return false;
    return true;
}

__attribute__((noinline,noipa))
long prime_count(long limit) {
    long count = 0;
    for (long n = 2; n <= limit; n++)
        if (is_prime(n)) count++;
    return count;
}

int main(void) {
    long acc = 0;
    for (int i=0; i<100; i++) {
        const long N = 100000;
        acc += prime_count(N);
    }
    printf("%ld\n", acc);
    return 0;
}
