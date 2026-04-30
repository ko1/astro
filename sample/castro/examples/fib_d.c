double fib(double n) {
    if (n < 2.0) return n;
    return fib(n - 1.0) + fib(n - 2.0);
}

int main() {
    return (int)fib(35.0);
}
