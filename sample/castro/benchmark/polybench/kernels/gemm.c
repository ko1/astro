// PolyBench/C gemm — adapted for castro.
// C := alpha*A*B + beta*C    (ni x nj = ni x nk * nk x nj)
// Upstream: linear-algebra/blas/gemm/gemm.c

#define DATA_TYPE double
#define NI 100
#define NJ 110
#define NK 120
#define REPS 1500

DATA_TYPE C[NI][NJ];
DATA_TYPE A[NI][NK];
DATA_TYPE B[NK][NJ];

void init_array(DATA_TYPE *alpha, DATA_TYPE *beta) {
    *alpha = 1.5;
    *beta = 1.2;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NJ; j++)
            C[i][j] = ((DATA_TYPE)((i*j+1) % NI)) / NI;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NK; j++)
            A[i][j] = ((DATA_TYPE)(i*(j+1) % NK)) / NK;
    for (int i = 0; i < NK; i++)
        for (int j = 0; j < NJ; j++)
            B[i][j] = ((DATA_TYPE)(i*(j+2) % NJ)) / NJ;
}

void kernel_gemm(DATA_TYPE alpha, DATA_TYPE beta) {
    for (int i = 0; i < NI; i++) {
        for (int j = 0; j < NJ; j++)
            C[i][j] *= beta;
        for (int k = 0; k < NK; k++)
            for (int j = 0; j < NJ; j++)
                C[i][j] += alpha * A[i][k] * B[k][j];
    }
}

int main() {
    DATA_TYPE alpha, beta;
    init_array(&alpha, &beta);
    for (int rep = 0; rep < REPS; rep++) kernel_gemm(alpha, beta);
    DATA_TYPE s = 0.0;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NJ; j++)
            s += C[i][j];
    return (int)s & 0xff;
}
