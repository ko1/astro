// PolyBench/C jacobi-1d — adapted for castro.
// 1D Jacobi iterative stencil
// Upstream: stencils/jacobi-1d/jacobi-1d.c

#define DATA_TYPE double
#define N 4000
#define TSTEPS 200
#define REPS 200

DATA_TYPE A[N];
DATA_TYPE B[N];

void init_array(void) {
    for (int i = 0; i < N; i++) {
        A[i] = ((DATA_TYPE)(i + 2)) / N;
        B[i] = ((DATA_TYPE)(i + 3)) / N;
    }
}

void kernel_jacobi_1d(void) {
    for (int t = 0; t < TSTEPS; t++) {
        for (int i = 1; i < N - 1; i++)
            B[i] = 0.33333 * (A[i-1] + A[i] + A[i+1]);
        for (int i = 1; i < N - 1; i++)
            A[i] = 0.33333 * (B[i-1] + B[i] + B[i+1]);
    }
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_jacobi_1d();
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++) s += A[i];
    return (int)s & 0xff;
}
