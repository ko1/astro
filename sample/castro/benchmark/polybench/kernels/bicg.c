// PolyBench/C bicg — adapted for castro.
// q := A*p; s := A'*r   (BiCG sub-kernel)
// Upstream: linear-algebra/kernels/bicg/bicg.c

#define DATA_TYPE double
#define M 400
#define N 400
#define REPS 2000

DATA_TYPE A[N][M];
DATA_TYPE s[M];
DATA_TYPE q[N];
DATA_TYPE p[M];
DATA_TYPE r[N];

void init_array(void) {
    for (int i = 0; i < M; i++) p[i] = ((DATA_TYPE)(i % M)) / M;
    for (int i = 0; i < N; i++) {
        r[i] = ((DATA_TYPE)(i % N)) / N;
        for (int j = 0; j < M; j++)
            A[i][j] = ((DATA_TYPE)(i*(j+1) % N)) / N;
    }
}

void kernel_bicg(void) {
    for (int i = 0; i < M; i++) s[i] = 0;
    for (int i = 0; i < N; i++) {
        q[i] = 0.0;
        for (int j = 0; j < M; j++) {
            s[j] = s[j] + r[i] * A[i][j];
            q[i] = q[i] + A[i][j] * p[j];
        }
    }
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_bicg();
    DATA_TYPE acc = 0.0;
    for (int i = 0; i < M; i++) acc += s[i];
    for (int i = 0; i < N; i++) acc += q[i];
    return (int)acc & 0xff;
}
