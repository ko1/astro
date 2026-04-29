int main() {
    int x = 42;
    int *p = &x;
    *p = 100;
    printf("x=%d\n", x);

    int y = 5;
    p = &y;
    *p += 3;
    printf("y=%d *p=%d\n", y, *p);

    // pointer arithmetic
    int a[5] = {10, 20, 30, 40, 50};
    int *q = a;
    printf("%d %d %d\n", *q, *(q+1), q[2]);
    q++;
    printf("%d %d\n", q[0], q[3]);

    // pointer diff
    int *r = a + 4;
    printf("diff=%ld\n", r - a);

    return 0;
}
