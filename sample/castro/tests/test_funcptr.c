int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int apply(int (*op)(int, int), int a, int b) {
    return op(a, b);
}

int main() {
    printf("add=%d mul=%d\n", apply(add, 3, 4), apply(mul, 3, 4));

    int (*fp)(int, int) = add;
    printf("fp(5,6)=%d\n", fp(5, 6));
    fp = mul;
    printf("fp(5,6)=%d\n", fp(5, 6));
    return 0;
}
