/* libastrogre.so provides its own default OPTION (the matcher's tunable knobs).
 * The engine sources declare it `extern` (context.h) and each standalone
 * embedder (are CLI, selftest, koruby's static bridge) defines its own; when
 * astrogre is used as a shared library the library itself supplies it, so a
 * consumer that only links -lastrogre needs no OPTION definition of its own.
 * All fields default to 0/false (compiled-code path enabled, no verbose). */
#include "context.h"

struct astrogre_option OPTION = {0};
