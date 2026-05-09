// PolyBench/C atax — adapted for castro.
// y := A' * (A * x)    (rectangular)
// Upstream: linear-algebra/kernels/atax/atax.c

#define DATA_TYPE double
#define M 400
#define N 400
#define REPS 2000

DATA_TYPE A[M][N];
DATA_TYPE x[N];
DATA_TYPE y[N];
DATA_TYPE tmp[M];

void init_array(void) {
    DATA_TYPE fn = (DATA_TYPE)N;
    for (int i = 0; i < N; i++) x[i] = 1 + (i / fn);
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++)
            A[i][j] = ((DATA_TYPE)((i + j) % N)) / (5 * M);
}

void kernel_atax(void) {
    for (int i = 0; i < N; i++) y[i] = 0;
    for (int i = 0; i < M; i++) {
        tmp[i] = 0.0;
        for (int j = 0; j < N; j++)
            tmp[i] = tmp[i] + A[i][j] * x[j];
        for (int j = 0; j < N; j++)
            y[j] = y[j] + A[i][j] * tmp[i];
    }
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_atax();
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++) s += y[i];
    return (int)s & 0xff;
}
