int sum_to(int n) {
    int s = 0;
    for (int i = 1; i <= n; i++) s += i;
    return s;
}

int factorial(int n) {
    int r = 1;
    while (n > 0) { r *= n; n--; }
    return r;
}

int main() {
    return sum_to(100) + factorial(5);  // 5050 + 120 = 5170
}
