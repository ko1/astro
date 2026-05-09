// PolyBench/C mvt — adapted for castro.
// x1 := x1 + A * y1; x2 := x2 + A' * y2
// Upstream: linear-algebra/kernels/mvt/mvt.c

#define DATA_TYPE double
#define N 250
#define REPS 4000

DATA_TYPE A[N][N];
DATA_TYPE x1[N];
DATA_TYPE x2[N];
DATA_TYPE y1[N];
DATA_TYPE y2[N];

void init_array(void) {
    for (int i = 0; i < N; i++) {
        x1[i] = ((DATA_TYPE)(i % N)) / N;
        x2[i] = ((DATA_TYPE)((i+1) % N)) / N;
        y1[i] = ((DATA_TYPE)((i+3) % N)) / N;
        y2[i] = ((DATA_TYPE)((i+4) % N)) / N;
        for (int j = 0; j < N; j++)
            A[i][j] = ((DATA_TYPE)(i*j % N)) / N;
    }
}

void kernel_mvt(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            x1[i] = x1[i] + A[i][j] * y1[j];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            x2[i] = x2[i] + A[j][i] * y2[j];
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_mvt();
    DATA_TYPE s = 0.0;
    for (int i = 0; i < N; i++) s += x1[i] + x2[i];
    return (int)s & 0xff;
}
