int g_counter = 0;
int g_arr[5] = {10, 20, 30, 40, 50};
double g_pi = 3.14159;

int bump() {
    g_counter++;
    return g_counter;
}

int main() {
    bump(); bump(); bump();
    printf("counter=%d\n", g_counter);
    int s = 0;
    for (int i = 0; i < 5; i++) s += g_arr[i];
    printf("g_arr_sum=%d\n", s);
    printf("g_pi=%.2f\n", g_pi);
    g_arr[2] = 99;
    printf("g_arr[2]=%d\n", g_arr[2]);
    return 0;
}
