// Quicksort over a deterministically-generated 5000-element int array.
// Stresses: recursion + array swaps + branch-heavy inner partition.

int data[5000];

void qs(int lo, int hi) {
    if (lo >= hi) return;
    int pivot = data[hi];
    int i = lo - 1;
    for (int j = lo; j < hi; j++) {
        if (data[j] < pivot) {
            i++;
            int t = data[i]; data[i] = data[j]; data[j] = t;
        }
    }
    i++;
    int t = data[i]; data[i] = data[hi]; data[hi] = t;
    qs(lo, i - 1);
    qs(i + 1, hi);
}

int main() {
    int n = 5000;
    for (int i = 0; i < n; i++) data[i] = (i * 137 + 17) % 9973;
    for (int k = 0; k < 4; k++) qs(0, n - 1);   // re-sort an already-sorted array, x4
    int s = 0;
    for (int i = 0; i < n; i++) s = s * 31 + data[i];
    return s & 0xFF;
}
