// Sieve of Eratosthenes — count primes < N.
// Stresses: tight integer loops, array indexing, store/load mix.

int prime[200000];

int sieve(int n) {
    for (int i = 0; i < n; i++) prime[i] = 1;
    prime[0] = 0;
    prime[1] = 0;
    // Only walk the marker loop while i*i fits in int (and < n).
    for (int i = 2; i * i < n; i++) {
        if (prime[i]) {
            for (int j = i * i; j < n; j += i) prime[j] = 0;
        }
    }
    int count = 0;
    for (int i = 2; i < n; i++) if (prime[i]) count++;
    return count;
}

int main() {
    int total = 0;
    for (int k = 0; k < 8; k++) total += sieve(200000);
    return total & 0xFF;
}
