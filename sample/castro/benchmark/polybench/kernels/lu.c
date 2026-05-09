// PolyBench/C lu — adapted for castro.
// LU decomposition (no pivoting)
// Upstream: linear-algebra/solvers/lu/lu.c

#define DATA_TYPE double
#define N 120
#define REPS 500

DATA_TYPE A[N][N];

void init_array(void) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j <= i; j++) A[i][j] = ((DATA_TYPE)(-(j % N)) / N) + 1.0;
        for (int j = i + 1; j < N; j++) A[i][j] = 0.0;
        A[i][i] = 1.0;
    }
    // make positive definite-ish so the kernel is numerically stable.
    DATA_TYPE B[N][N];
    for (int r = 0; r < N; r++)
        for (int s = 0; s < N; s++) {
            DATA_TYPE acc = 0;
            for (int t = 0; t < N; t++) acc += A[r][t] * A[s][t];
            B[r][s] = acc;
        }
    for (int r = 0; r < N; r++)
        for (int s = 0; s < N; s++)
            A[r][s] = B[r][s];
}

void kernel_lu(void) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < i; j++) {
            for (int k = 0; k < j; k++)
                A[i][j] -= A[i][k] * A[k][j];
            A[i][j] /= A[j][j];
        }
        for (int j = i; j < N; j++) {
            for (int k = 0; k < i; k++)
                A[i][j] -= A[i][k] * A[k][j];
        }
    }
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) {
        init_array();
        kernel_lu();
    }
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += A[i][j];
    return (int)s & 0xff;
}
