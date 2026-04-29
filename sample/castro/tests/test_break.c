int main() {
    int s = 0;
    for (int i = 0; i < 100; i++) {
        if (i == 50) break;
        if (i % 2 == 0) continue;
        s += i;
    }
    printf("for: s=%d\n", s);

    int j = 0;
    while (j < 1000) {
        j++;
        if (j > 5) break;
    }
    printf("while: j=%d\n", j);

    int k = 0;
    do {
        k++;
        if (k == 3) continue;
        if (k > 5) break;
    } while (1);
    printf("dowhile: k=%d\n", k);

    // nested loops: break only exits inner
    int outer = 0;
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 10; b++) {
            if (b == 2) break;
            outer++;
        }
    }
    printf("nested: outer=%d\n", outer);

    return 0;
}
