int sum_to(int n) {
    int s = 0;
    for (int i = 1; i <= n; i++) s += i;
    return s;
}

int main() {
    int total = 0;
    for (int i = 0; i < 36000; i++) total += sum_to(1000);
    return total;
}
