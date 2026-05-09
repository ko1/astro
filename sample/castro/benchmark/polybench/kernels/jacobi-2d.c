// PolyBench/C jacobi-2d — adapted for castro.
// 2D Jacobi iterative stencil
// Upstream: stencils/jacobi-2d/jacobi-2d.c

#define DATA_TYPE double
#define N 150
#define TSTEPS 80
#define REPS 200

DATA_TYPE A[N][N];
DATA_TYPE B[N][N];

void init_array(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = ((DATA_TYPE)(i*(j+2)+2)) / N;
            B[i][j] = ((DATA_TYPE)(i*(j+3)+3)) / N;
        }
}

void kernel_jacobi_2d(void) {
    for (int t = 0; t < TSTEPS; t++) {
        for (int i = 1; i < N - 1; i++)
            for (int j = 1; j < N - 1; j++)
                B[i][j] = 0.2 * (A[i][j] + A[i][j-1] + A[i][1+j] + A[1+i][j] + A[i-1][j]);
        for (int i = 1; i < N - 1; i++)
            for (int j = 1; j < N - 1; j++)
                A[i][j] = 0.2 * (B[i][j] + B[i][j-1] + B[i][1+j] + B[1+i][j] + B[i-1][j]);
    }
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_jacobi_2d();
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += A[i][j];
    return (int)s & 0xff;
}
