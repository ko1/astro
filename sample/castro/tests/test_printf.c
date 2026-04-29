int main() {
    printf("hello\n");
    printf("%d\n", 42);
    printf("%d %d\n", 1, 2);
    printf("%s=%d\n", "x", 42);
    printf("%5d\n", 7);
    printf("%-5d|\n", 7);
    printf("%05d\n", 7);
    printf("%lld\n", 1234567890123LL);
    printf("%x %X\n", 255, 255);
    printf("%c%c%c\n", 65, 66, 67);
    printf("%.3f\n", 3.14159);
    printf("%g\n", 1.5);
    printf("%%done\n");

    // putchar / puts
    putchar(72);  // 'H'
    putchar(105); // 'i'
    putchar(10);  // '\n'
    puts("via puts");
    return 0;
}
