// Computer Language Benchmarks Game — fannkuch-redux.
// Stresses: int-only, array permutations, branch-heavy inner loop.

#define N 10
#define REPS 1

int perm[N];
int perm1[N];
int count[N];

int kernel_fannkuch_redux(int n) {
    int max_flips = 0;
    int checksum = 0;
    int sign = 1;

    for (int i = 0; i < n; i++) perm1[i] = i;
    int r = n;

    while (1) {
        for (; r != 1; r--) count[r - 1] = r;

        // Copy perm1 to perm
        for (int i = 0; i < n; i++) perm[i] = perm1[i];

        // Count flips
        int flips_count = 0;
        int k;
        while ((k = perm[0]) != 0) {
            int k2 = (k + 1) >> 1;
            for (int i = 0; i < k2; i++) {
                int tmp = perm[i];
                perm[i] = perm[k - i];
                perm[k - i] = tmp;
            }
            flips_count++;
        }

        if (flips_count > max_flips) max_flips = flips_count;
        checksum += sign * flips_count;

        // Generate next permutation
        while (1) {
            if (r == n) {
                // Done
                int result = (checksum + max_flips * 1000) & 0xff;
                return result;
            }
            int perm0 = perm1[0];
            int i = 0;
            while (i < r) {
                int j = i + 1;
                perm1[i] = perm1[j];
                i = j;
            }
            perm1[r] = perm0;
            count[r]--;
            if (count[r] > 0) break;
            r++;
        }
        sign = -sign;
    }
}

int main() {
    int s = 0;
    for (int rep = 0; rep < REPS; rep++)
        s += kernel_fannkuch_redux(N);
    return s & 0xff;
}
