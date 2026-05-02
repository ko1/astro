#include <stdio.h>

int zero(void){return 0;}

int main(void)
{
    int sz = 1000 * 1000 * 1000;
    int i;
    for (i = 0; i < sz; i++) {
        zero();
    }
    printf("%d\n", i);
    return 0;
}
