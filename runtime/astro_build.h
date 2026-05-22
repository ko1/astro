// ASTro build orchestrator: cross-sample machinery for invoking the C
// toolchain to produce standalone executables from a parsed AST.
//
// The user CLI is a "--build" subcommand.  Inside it, the flags are
// organised into two orthogonal axes:
//
//   attribute (what kind of compiled code goes into the exe):
//     --plain         use no compiled code (= AST-walk at exe runtime)
//     --aot-compile   bake AOT specializations
//     --pg-compile    bake profile-guided specializations (implies run)
//
//   action (whether to run the program during build):
//     --run           execute the program during build (= use require chain
//                     to auto-discover the file set).  Implied by --pg-compile.
//
// Default (no flag) = "embed entry's AST only, no run, no AOT".
//
// Positional handling depends on whether the program runs during build:
//   - runs: first positional = entry source, rest = ARGV for the run
//   - doesn't run: all positionals = source files to embed/compile
//
// C-toolchain knobs (CC, -O*, --strip, --lto, --static, --gc-sections,
// --sanitize, --cflag, --ldflag, --keep, --verbose) are NOT in argv.
// They live in the ASTRO_BUILD_OPTS environment variable.  This keeps
// argv strictly "what to do" and env "how to compile".
//
//     ASTRO_BUILD_OPTS="--cc=clang -O3 --strip" naruby --build out main.rb

#ifndef ASTRO_BUILD_H
#define ASTRO_BUILD_H

#include <stdbool.h>
#include <stddef.h>

struct astro_build_config {
    // ---------- mode (from argv parsing) ----------
    bool plain;          // --plain        — don't use compiled code
    bool aot_compile;    // --aot-compile  — bake AOT specializations
    bool pg_compile;     // --pg-compile   — bake PG specializations (implies run)
    bool run;            // --run          — execute during build (or implied by --pg-compile)

    // ---------- output ----------
    const char *out_exe; // first positional after --build

    // ---------- C toolchain (from ASTRO_BUILD_OPTS env) ----------
    const char *cc;
    int  opt_level;      // -1 = unspecified → 2
    bool debug;
    bool strip;
    bool lto;
    bool static_link;
    bool gc_sections;
    const char *sanitize;
    const char *const *extra_cflags;   // heap-allocated, from ASTRO_BUILD_OPTS env (freed by dispose)
    const char *const *extra_ldflags;  // heap-allocated, from env
    bool verbose;
    bool keep_intermediates;

    // ---------- sample-supplied (filled by the per-sample build helper) ----------
    const char *src_dir;
    const char *runtime_dir;
    const char *const *sources;
    const char *const *extra_sources_abs;
    const char *const *extra_objects;
    const char *const *sample_cflags;   // sample-supplied, NOT freed by dispose
    const char *const *sample_ldflags;  // sample-supplied, NOT freed by dispose
};

#define ASTRO_BUILD_CONFIG_INIT { .opt_level = -1 }

// ---------------------------------------------------------------------------
// --build subcommand parser
// ---------------------------------------------------------------------------
//
// Recognised syntax:
//     <prog> --build OUTPUT [mode flags...] [positionals...]
//
// `argc` / `argv` should start at the `--build` token (i.e. the sample
// passes argv+1 from the position where argv[0] == "--build").
//
// On success:
//   - cfg->out_exe is set to the first positional after `--build`.
//   - Mode flags (--plain / --aot-compile / --pg-compile / --run) are
//     folded into cfg.
//   - ASTRO_BUILD_OPTS environment variable is parsed for C-toolchain
//     knobs (sets cfg->cc, opt_level, strip, lto, etc.).
//   - Remaining positional tokens are written to *rest_argc / *rest_argv.
//     The sample decides how to interpret them based on cfg->run:
//       - cfg->run == true:  positionals[0] = entry source, [1..] = ARGV
//       - cfg->run == false: positionals[0..n) = source file list
//
// Returns 0 on success, non-zero on parse error (with diagnostic on stderr).
int astro_build_subcommand_parse(int argc, char **argv,
                                 struct astro_build_config *cfg,
                                 int *rest_argc, char ***rest_argv);

// Parse the ASTRO_BUILD_OPTS environment variable into cfg.  Called
// automatically by astro_build_subcommand_parse, but exposed so a
// non-subcommand caller (e.g. an alternate driver) can use it too.
//
// The env value is split on whitespace and each token is parsed as a
// C-toolchain knob.  Recognised tokens:
//   --cc=PATH
//   -O0..-O3, -Os, -Og, --opt=N
//   --debug / --no-debug
//   --strip / --no-strip
//   --lto / --no-lto
//   --static
//   --gc-sections
//   --sanitize=LIST
//   --cflag=ARG (repeatable; appended to cfg->extra_cflags)
//   --ldflag=ARG (repeatable; appended to cfg->extra_ldflags)
//   --verbose
//   --keep
int astro_build_load_env_opts(struct astro_build_config *cfg);

// Free heap memory owned by cfg (the --cflag / --ldflag arrays).
void astro_build_config_dispose(struct astro_build_config *cfg);

// Invoke the toolchain.  Returns the exit status of the compile (0 = ok).
// `cfg->out_exe`, `cfg->src_dir`, `cfg->runtime_dir`, and `cfg->sources`
// must be set.
int astro_build_executable(const struct astro_build_config *cfg);

// One-shot AOT executable builder.  Wraps the common end of every
// sample's `--build` path:
//   1. Emit `_embed.c` via astro_emit_ast_c_program (DAG-aware AST builder
//      + per-node dispatcher patches + ASTRO_SD_PROTO forward decls).
//   2. Walk the framework's compile log and translate each entry into
//      a `<store_dir>/c/<name>.c` path.
//   3. Append `_embed.c` + the SD paths to cfg->extra_sources_abs.
//   4. Invoke astro_build_executable.
//   5. Unlink intermediates unless cfg->keep_intermediates.
//
// Caller is responsible for the preceding bake pass: turning on the
// compile log via astro_build_begin_aot_session, then calling
// astro_cs_compile on each entry.
//
// `code_store_dir` defaults to "code_store" if NULL.
int astro_build_aot_executable(struct Node *root,
                               struct astro_build_config *cfg,
                               const char *code_store_dir);

// Bracket the bake-and-link region.  Begin enables compile-log
// recording; end disables it and clears.
struct Node;
void astro_build_begin_aot_session(void);
void astro_build_end_aot_session(void);

#endif // ASTRO_BUILD_H
