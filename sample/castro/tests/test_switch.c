int classify(int x) {
    switch (x) {
        case 1: return 100;
        case 2: return 200;
        case 3: return 300;
        default: return -1;
    }
}

int sum_first_n(int n) {
    int total = 0;
    for (int i = 1; i <= n; i++) {
        switch (i % 3) {
            case 0: total += i * 3; break;
            case 1: total += i * 1; break;
            case 2: total += i * 2; break;
        }
    }
    return total;
}

int fall_through(int x) {
    int s = 0;
    switch (x) {
        case 1: s += 1;
        case 2: s += 2;
        case 3: s += 3;
                break;
        case 4: s += 4;
                break;
    }
    return s;
}

int main() {
    printf("classify 1=%d 2=%d 3=%d 4=%d\n",
           classify(1), classify(2), classify(3), classify(4));
    printf("sum=%d\n", sum_first_n(10));
    printf("fall %d %d %d %d\n",
           fall_through(1), fall_through(2), fall_through(3), fall_through(4));
    return 0;
}
