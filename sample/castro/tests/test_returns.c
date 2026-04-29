// Exercise return-lifting: tail returns, early returns, mixed.
int simple(int n) {
    return n * 2;
}

int with_early(int n) {
    if (n < 0) return -1;
    if (n == 0) return 0;
    return n + 1;
}

int nested_if(int x, int y) {
    if (x > 0) {
        if (y > 0) return 1;
        if (y < 0) return -1;
        return 0;
    }
    return -2;
}

int loop_then_return(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += i;
    return s;
}

// Loop with mid-body return — needs setjmp.
int loop_with_return(int target, int n) {
    for (int i = 0; i < n; i++) {
        if (i == target) return i * 10;
    }
    return -1;
}

int main() {
    printf("%d %d %d %d\n", simple(5), with_early(-3), with_early(0), with_early(7));
    printf("%d %d %d\n", nested_if(1, 1), nested_if(1, -1), nested_if(-1, 0));
    printf("%d\n", loop_then_return(10));
    printf("%d %d\n", loop_with_return(3, 100), loop_with_return(99, 5));
    return 0;
}
