#include <stdio.h>

int zero(void){return 0;}

int main(void)
{
    int sz = 1000 * 1000 * 1000;
    for (int i=0; i<sz; i++) {
        zero();
    }
    printf("%d\n", zero());
    return 0;
}
