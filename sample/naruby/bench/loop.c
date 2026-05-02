#include <stdio.h>

int
main(int argc, const char *argv[])
{
    int i;
    for (i=0; i<1000*1000*100; i++) {
    }
    printf("%d\n", i);
    (void)argc; (void)argv;
    return 0;
}
