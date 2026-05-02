#include <stdio.h>
#include <stdbool.h>

static bool is_prime(long n) {
    if (n < 2) return false;
    for (long i = 2; i * i <= n; i++)
        if (n % i == 0) return false;
    return true;
}

long prime_count(long limit) {
    long count = 0;
    for (long n = 2; n <= limit; n++)
        if (is_prime(n)) count++;
    return count;
}

int main(void) {
    for (int i=0; i<100; i++) {
        const long N = 100000;
        prime_count(N);
    }
    printf("%ld\n", prime_count(100000));
    return 0;
}
