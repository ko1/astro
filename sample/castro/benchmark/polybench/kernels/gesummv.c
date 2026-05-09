// PolyBench/C gesummv — adapted for castro.
// y := alpha*A*x + beta*B*x
// Upstream: linear-algebra/blas/gesummv/gesummv.c

#define DATA_TYPE double
#define N 250
#define REPS 4000

DATA_TYPE A[N][N];
DATA_TYPE B[N][N];
DATA_TYPE tmp[N];
DATA_TYPE x[N];
DATA_TYPE y[N];

void init_array(DATA_TYPE *alpha, DATA_TYPE *beta) {
    *alpha = 1.5;
    *beta = 1.2;
    for (int i = 0; i < N; i++) {
        x[i] = (DATA_TYPE)(i % N) / N;
        for (int j = 0; j < N; j++) {
            A[i][j] = ((DATA_TYPE)(i*j+1)) / N;
            B[i][j] = ((DATA_TYPE)(i*j+2)) / N;
        }
    }
}

void kernel_gesummv(DATA_TYPE alpha, DATA_TYPE beta) {
    for (int i = 0; i < N; i++) {
        tmp[i] = 0.0;
        y[i]   = 0.0;
        for (int j = 0; j < N; j++) {
            tmp[i] = A[i][j] * x[j] + tmp[i];
            y[i]   = B[i][j] * x[j] + y[i];
        }
        y[i] = alpha * tmp[i] + beta * y[i];
    }
}

int main() {
    DATA_TYPE alpha, beta;
    init_array(&alpha, &beta);
    for (int rep = 0; rep < REPS; rep++) kernel_gesummv(alpha, beta);
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++) s += y[i];
    return (int)s & 0xff;
}
