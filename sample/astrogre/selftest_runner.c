/*
 * Tiny CLI shim around the engine's self-test / microbench / file-bench
 * harnesses.  Built as a separate binary (`selftest_runner`) by the
 * Makefile and invoked by `make self-test`, `make bench`, `make
 * bench-file FILE=… PAT=…`.
 *
 * Keeping these out of the user-facing `are` CLI means: (a) a hot grep
 * binary doesn't link the test corpus, (b) `are --help` stays a clean
 * grep manual page, (c) a CI run that just wants the engine result
 * doesn't need to know any grep flags.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "node.h"
#include "context.h"

/* The framework's global option struct.  Normally lives in main.c;
 * we provide our own here so we don't need to link main.c. */
struct astrogre_option OPTION = {0};

extern int astrogre_run_self_tests(void);
extern int astrogre_run_microbench(void);
extern int astrogre_run_file_bench(const char *file, const char *pat,
                                    int iters, bool aot, bool plain);

static int
usage(void)
{
    fputs(
        "Usage:\n"
        "  selftest_runner test\n"
        "  selftest_runner bench\n"
        "  selftest_runner bench-file FILE PATTERN [ITERS] [--aot|--plain]\n",
        stderr);
    return 2;
}

int
main(int argc, char *argv[])
{
    INIT();
    if (argc < 2) return usage();

    if (strcmp(argv[1], "test") == 0)  return astrogre_run_self_tests();
    if (strcmp(argv[1], "bench") == 0) return astrogre_run_microbench();
    if (strcmp(argv[1], "bench-file") == 0) {
        if (argc < 4) return usage();
        const char *file = argv[2];
        const char *pat  = argv[3];
        int iters = (argc >= 5 && argv[4][0] != '-') ? atoi(argv[4]) : 50;
        bool aot = false, plain = false;
        for (int j = 4; j < argc; j++) {
            if      (strcmp(argv[j], "--aot")   == 0) aot   = true;
            else if (strcmp(argv[j], "--plain") == 0) plain = true;
        }
        return astrogre_run_file_bench(file, pat, iters, aot, plain);
    }
    return usage();
}
