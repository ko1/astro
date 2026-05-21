// ASTro build orchestrator: cross-sample machinery for invoking the C
// toolchain to produce standalone executables from a parsed AST.
//
// The orchestrator owns all build-flavor knobs (CC, target, optimization,
// linkage, sanitizers, etc.).  Each language sample provides only the
// source-list / driver C file and any language-specific cflags through
// the config; argv flag parsing is shared.
//
// Typical usage from a sample's main():
//
//     struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
//     astro_build_parse_args(&argc, argv, &bcfg);
//     if (bcfg.out_exe) {
//         bcfg.src_dir     = MY_SRC_DIR;        // -DMY_SRC_DIR=...
//         bcfg.runtime_dir = ASTRO_RUNTIME_DIR; // -D... from Makefile
//         bcfg.sources     = (const char *[]){"parse.c", "node.c",
//                                             "exe_main.c", NULL};
//         astro_build_executable(&bcfg);
//         return 0;
//     }
//
// Then proceed with the normal interpreter loop.

#ifndef ASTRO_BUILD_H
#define ASTRO_BUILD_H

#include <stdbool.h>
#include <stddef.h>

struct astro_build_config {
    // Toolchain.
    const char *cc;              // NULL → $ASTRO_CC → $CC → "cc"
    const char *target;          // e.g. "aarch64-linux-gnu"; NULL → host
    const char *sysroot;         // --sysroot=...; NULL → none

    // Optimization / debug.
    int  opt_level;              // -1 = unspecified (then $ASTRO_OPT_LEVEL or 2)
    bool debug;                  // pass -g3 / -ggdb3
    bool strip;                  // run `strip` post-link
    bool lto;                    // pass -flto to compile & link
    bool static_link;            // pass -static
    bool gc_sections;            // -ffunction-sections + -Wl,--gc-sections
                                 // (drop unused dispatchers / runtime helpers)

    // Sanitizers.  Empty string or NULL → none.  Comma-separated, e.g.
    // "address,undefined".  Translates to `-fsanitize=...` on both
    // compile and link.
    const char *sanitize;

    // Pass-through flags (NULL-terminated arrays).
    const char *const *extra_cflags;
    const char *const *extra_ldflags;

    // Sample-supplied.
    const char *out_exe;         // -o path; required for executable build
    const char *src_dir;         // -I & source-root for sources[]
    const char *runtime_dir;     // -I (ASTro runtime)
    const char *const *sources;  // NULL-terminated; relative to src_dir
    const char *const *extra_sources_abs;  // NULL-terminated; absolute paths
    const char *const *extra_objects;      // NULL-terminated; .o / .a to link

    // Behavior.
    bool verbose;                // print the command line
    bool keep_intermediates;     // don't unlink generated _embed/_table files
};

// opt_level = -1 → "unspecified": falls back to $ASTRO_OPT_LEVEL → 2.
// Without this sentinel, the zero-init would mean "always -O0".
#define ASTRO_BUILD_CONFIG_INIT { .opt_level = -1 }

// Parse build-related flags out of argv in-place: matched flags are
// consumed and the remaining argv is compacted.  Returns 0 on success,
// non-zero on parse error (and writes to stderr).
//
// Recognised flags (long-form only — short opts left to host main()):
//   --generate-executable PATH
//   --cc CC
//   --target TRIPLE
//   --sysroot PATH
//   -O0, -O1, -O2, -O3, -Os, -Og
//   --debug, --no-debug
//   --strip, --no-strip
//   --lto, --no-lto
//   --static
//   --gc-sections              (link-time dead-code elimination)
//   --sanitize=LIST            ("address,undefined")
//   --cflag=ARG                (repeatable; collected into extra_cflags)
//   --ldflag=ARG               (repeatable; collected into extra_ldflags)
//   --verbose-build
//   --keep-intermediates
//
// Internally accumulates --cflag / --ldflag in heap-allocated arrays
// referenced by cfg->extra_cflags / cfg->extra_ldflags.  Memory
// ownership is on the cfg; freed by astro_build_config_dispose.
int astro_build_parse_args(int *argc_io, char **argv,
                           struct astro_build_config *cfg);

// Free heap memory owned by cfg (the --cflag / --ldflag arrays).
void astro_build_config_dispose(struct astro_build_config *cfg);

// Invoke the toolchain.  Returns the exit status of the compile (0 = ok).
// `cfg->out_exe`, `cfg->src_dir`, `cfg->runtime_dir`, and `cfg->sources`
// must be set; other fields default sensibly.
int astro_build_executable(const struct astro_build_config *cfg);

#endif // ASTRO_BUILD_H
