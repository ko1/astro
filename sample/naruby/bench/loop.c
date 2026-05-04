#include <stdio.h>
#include <stdint.h>

int
main(int argc, const char *argv[])
{
    int64_t i;
    for (i=0; i<1000*1000*100; i++) {
    }
    printf("%ld\n", (long)i);
    (void)argc; (void)argv;
    return 0;
}
