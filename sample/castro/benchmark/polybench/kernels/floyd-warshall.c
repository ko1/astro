// PolyBench/C floyd-warshall — adapted for castro.
// All-pairs shortest paths
// Upstream: medley/floyd-warshall/floyd-warshall.c

#define DATA_TYPE int
#define N 180
#define REPS 60

DATA_TYPE path[N][N];

void init_array(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            path[i][j] = i*j % 7 + 1;
            if ((i + j) % 13 == 0 || (i + j) % 7 == 0 || (i + j) % 11 == 0)
                path[i][j] = 999;
        }
}

void kernel_floyd_warshall(void) {
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                path[i][j] = path[i][j] < path[i][k] + path[k][j]
                           ? path[i][j] : path[i][k] + path[k][j];
}

int main() {
    init_array();
    for (int rep = 0; rep < REPS; rep++) kernel_floyd_warshall();
    DATA_TYPE s = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += path[i][j];
    return s & 0xff;
}
