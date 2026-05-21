// ASTro build orchestrator: cross-sample machinery for invoking the C
// toolchain to produce standalone executables from a parsed AST.
//
// The orchestrator owns all build-flavor knobs (CC, target, optimization,
// linkage, sanitizers, etc.).  Each language sample provides only the
// source-list / driver C file and any language-specific cflags through
// the config; argv flag parsing is shared.
//
// The build interface is a subcommand: when argv[1] == "--build" the
// sample dispatches to the framework; otherwise the existing interp
// path runs untouched.  That keeps the build option namespace and the
// language option namespace strictly separate — they live in disjoint
// argv positions and can never collide.

#ifndef ASTRO_BUILD_H
#define ASTRO_BUILD_H

#include <stdbool.h>
#include <stddef.h>

struct astro_build_config {
    // Toolchain.
    const char *cc;              // NULL → $ASTRO_CC → $CC → "cc"

    // Optimization / debug.
    int  opt_level;              // -1 = unspecified (then $ASTRO_OPT_LEVEL or 2)
    bool debug;                  // pass -ggdb3
    bool strip;                  // run `strip` post-link
    bool lto;                    // pass -flto to compile & link
    bool static_link;            // pass -static
    bool gc_sections;            // -ffunction-sections + -Wl,--gc-sections
                                 // (drop unused dispatchers / runtime helpers)
    bool no_aot;                 // skip the AOT SD bake — exe runs as a
                                 // pure interpreter (smaller exe, slower
                                 // run).  Default false (= AOT on).

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
    bool keep_intermediates;     // don't unlink generated _embed file
};

// opt_level = -1 → "unspecified": falls back to $ASTRO_OPT_LEVEL → 2.
#define ASTRO_BUILD_CONFIG_INIT { .opt_level = -1 }

// ---------------------------------------------------------------------------
// Subcommand parser — used by samples to dispatch the `--build` syntax.
// ---------------------------------------------------------------------------
//
// Recognised syntax:
//     <prog> --build <output> [build opts...] [source spec...]
//
// `argc` / `argv` should start at the `--build` token (i.e. the sample
// passes `argc - 1, argv + 1` after detecting argv[1] == "--build").
//
// On success:
//   - cfg->out_exe is set to the first positional argument after --build.
//   - Build opts (recognised by name; see the table below) are folded
//     into cfg.
//   - Remaining tokens (= source spec, e.g. `-e EXPR` or a file path)
//     are written to *rest_argv / *rest_argc for the sample's source
//     parser to consume.
//
// Recognised build opts:
//   --aot-compile / --aot       — explicit AOT bake (the default)
//   --no-aot-compile / --no-aot — skip AOT bake (smaller, slower exe)
//   --strip / --no-strip
//   --lto / --no-lto
//   --static
//   --gc-sections
//   --debug / --no-debug
//   --cc=PATH
//   -O0/-O1/-O2/-O3/-Os/-Og
//   --opt=N                     — same as -O<N>; useful in scripts
//   --sanitize=LIST
//   --cflag=ARG (repeatable)
//   --ldflag=ARG (repeatable)
//   --verbose                   — print the cc command line
//   --keep                      — don't unlink _embed.c
//
// `--cflag` / `--ldflag` values are strdup'd into heap-allocated arrays
// referenced by cfg->extra_cflags / cfg->extra_ldflags; free them via
// astro_build_config_dispose after the build.
//
// Returns 0 on success, non-zero on parse error (with diagnostic on
// stderr).  Unknown tokens are passed through to *rest_argv unchanged.
int astro_build_subcommand_parse(int argc, char **argv,
                                 struct astro_build_config *cfg,
                                 int *rest_argc, char ***rest_argv);

// Free heap memory owned by cfg (the --cflag / --ldflag arrays).
void astro_build_config_dispose(struct astro_build_config *cfg);

// Invoke the toolchain.  Returns the exit status of the compile (0 = ok).
// `cfg->out_exe`, `cfg->src_dir`, `cfg->runtime_dir`, and `cfg->sources`
// must be set; other fields default sensibly.
int astro_build_executable(const struct astro_build_config *cfg);

// One-shot AOT executable builder.  Wraps the common end of every
// sample's `--build` path:
//   1. Emit `_embed.c` via astro_emit_ast_c_program (DAG-aware AST
//      builder + per-node dispatcher patches + ASTRO_SD_PROTO forward
//      decls for every linked-in SD).
//   2. Walk the framework's per-process compile log (populated by
//      preceding astro_cs_compile calls) and translate each entry into
//      a `<store_dir>/c/<name>.c` path.
//   3. Append `_embed.c` + the SD paths to cfg->extra_sources_abs.
//   4. Invoke astro_build_executable.
//   5. Unlink intermediates unless cfg->keep_intermediates.
//
// The caller is responsible for everything BEFORE this helper: turning
// on the compile log via astro_build_begin_aot_session, then calling
// astro_cs_compile on the program AST and every method / function body.
//
// `code_store_dir` defaults to "code_store" if NULL.
int astro_build_aot_executable(struct Node *root,
                               struct astro_build_config *cfg,
                               const char *code_store_dir);

// Bracket the bake-and-link region.  Begin enables compile-log
// recording (sets `astro_cs_log_compiles = true` and clears the log);
// end disables it and clears.
struct Node;
void astro_build_begin_aot_session(void);
void astro_build_end_aot_session(void);

#endif // ASTRO_BUILD_H
