// PolyBench/C syrk — adapted for castro.
// C := alpha*A*A' + beta*C    (symmetric rank-k update)
// Upstream: linear-algebra/blas/syrk/syrk.c

#define DATA_TYPE double
#define N 140
#define M 100
#define REPS 1000

DATA_TYPE C[N][N];
DATA_TYPE A[N][M];

void init_array(DATA_TYPE *alpha, DATA_TYPE *beta) {
    *alpha = 1.5;
    *beta = 1.2;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < M; j++)
            A[i][j] = ((DATA_TYPE)((i*j+1) % N)) / N;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            C[i][j] = ((DATA_TYPE)((i*j+2) % M)) / M;
}

void kernel_syrk(DATA_TYPE alpha, DATA_TYPE beta) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j <= i; j++)
            C[i][j] *= beta;
        for (int k = 0; k < M; k++)
            for (int j = 0; j <= i; j++)
                C[i][j] += alpha * A[i][k] * A[j][k];
    }
}

int main() {
    DATA_TYPE alpha, beta;
    init_array(&alpha, &beta);
    for (int rep = 0; rep < REPS; rep++) kernel_syrk(alpha, beta);
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += C[i][j];
    return (int)s & 0xff;
}
