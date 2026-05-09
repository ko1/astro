// PolyBench/C seidel-2d — adapted for castro.
// 2D Gauss-Seidel iterative stencil
// Upstream: stencils/seidel-2d/seidel-2d.c

#define DATA_TYPE double
#define N 150
#define TSTEPS 80
#define REPS 100

DATA_TYPE A[N][N];

void init_array(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            A[i][j] = ((DATA_TYPE)(i*(j+2)+2)) / N;
}

void kernel_seidel_2d(void) {
    for (int t = 0; t < TSTEPS; t++)
        for (int i = 1; i < N - 1; i++)
            for (int j = 1; j < N - 1; j++)
                A[i][j] = (A[i-1][j-1] + A[i-1][j] + A[i-1][j+1]
                         + A[i][j-1]   + A[i][j]   + A[i][j+1]
                         + A[i+1][j-1] + A[i+1][j] + A[i+1][j+1]) / 9.0;
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_seidel_2d();
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += A[i][j];
    return (int)s & 0xff;
}
