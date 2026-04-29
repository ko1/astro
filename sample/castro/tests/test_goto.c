int main() {
    int i = 0;
    goto start;
again:
    i++;
start:
    if (i < 5) goto again;
    printf("i=%d\n", i);

    int j = 0;
    int s = 0;
loop_top:
    if (j >= 10) goto loop_end;
    s += j;
    j++;
    goto loop_top;
loop_end:
    printf("s=%d j=%d\n", s, j);

    return 0;
}
