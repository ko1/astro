// PolyBench/C 2mm — adapted for castro.
// D := alpha * A * B * C + beta * D
// Upstream: linear-algebra/kernels/2mm/2mm.c

#define DATA_TYPE double
#define NI 70
#define NJ 80
#define NK 100
#define NL 120
#define REPS 1500

DATA_TYPE tmp[NI][NJ];
DATA_TYPE A[NI][NK];
DATA_TYPE B[NK][NJ];
DATA_TYPE C[NJ][NL];
DATA_TYPE D[NI][NL];

void init_array(DATA_TYPE *alpha, DATA_TYPE *beta) {
    *alpha = 1.5;
    *beta = 1.2;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NK; j++)
            A[i][j] = ((DATA_TYPE)((i*j+1) % NI)) / NI;
    for (int i = 0; i < NK; i++)
        for (int j = 0; j < NJ; j++)
            B[i][j] = ((DATA_TYPE)(i*(j+1) % NJ)) / NJ;
    for (int i = 0; i < NJ; i++)
        for (int j = 0; j < NL; j++)
            C[i][j] = ((DATA_TYPE)((i*(j+3)+1) % NL)) / NL;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NL; j++)
            D[i][j] = ((DATA_TYPE)(i*(j+2) % NK)) / NK;
}

void kernel_2mm(DATA_TYPE alpha, DATA_TYPE beta) {
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NJ; j++) {
            tmp[i][j] = 0.0;
            for (int k = 0; k < NK; k++)
                tmp[i][j] += alpha * A[i][k] * B[k][j];
        }
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NL; j++) {
            D[i][j] *= beta;
            for (int k = 0; k < NJ; k++)
                D[i][j] += tmp[i][k] * C[k][j];
        }
}

int main() {
    DATA_TYPE alpha, beta;
    init_array(&alpha, &beta);
    for (int rep = 0; rep < REPS; rep++) kernel_2mm(alpha, beta);
    DATA_TYPE s = 0.0;
    for (int i = 0; i < NI; i++)
        for (int j = 0; j < NL; j++)
            s += D[i][j];
    return (int)s & 0xff;
}
