// 128×128 integer matrix multiplication.
// Stresses: triple-nested loop + array stride access (row × col).

int A[16384];
int B[16384];
int C[16384];

void matmul(int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int s = 0;
            for (int k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j];
            C[i * n + j] = s;
        }
    }
}

int main() {
    int n = 128;
    int total = n * n;
    for (int i = 0; i < total; i++) {
        A[i] = (i * 13 + 1) & 0xFF;
        B[i] = (i * 7  + 3) & 0xFF;
    }
    for (int k = 0; k < 8; k++) matmul(n);
    int s = 0;
    for (int i = 0; i < total; i++) s = s * 31 + C[i];
    return s & 0xFF;
}
