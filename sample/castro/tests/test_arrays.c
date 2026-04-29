int sum_arr(int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

int main() {
    int a[10];
    for (int i = 0; i < 10; i++) a[i] = i * i;
    printf("sum=%d\n", sum_arr(a, 10));

    // multi-dim simulated as flat
    int b[6];
    for (int i = 0; i < 6; i++) b[i] = i + 1;
    int total = 0;
    for (int i = 0; i < 6; i++) total += b[i];
    printf("total=%d\n", total);

    // string buffer
    char buf[16];
    buf[0] = 'h'; buf[1] = 'e'; buf[2] = 'l'; buf[3] = 'l'; buf[4] = 'o'; buf[5] = 0;
    printf("%s\n", buf);
    printf("len=%d\n", (int)strlen(buf));

    return 0;
}
