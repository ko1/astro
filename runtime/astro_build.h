// ASTro build orchestrator: cross-sample machinery for invoking the C
// toolchain to produce standalone executables from a parsed AST.
//
// The build flags are order-free in argv, but must appear BEFORE the
// source file (Unix convention: anything after the first positional is
// passed to the running program).  The flags are organised into two
// orthogonal axes:
//
//   attribute (what kind of compiled code goes into the exe):
//     --plain         use no compiled code (= AST-walk at exe runtime)
//     --aot-compile   bake AOT specializations
//     --pg-compile    bake profile-guided specializations (implies run)
//
//   action:
//     --run           execute the program (implied by --pg-compile;
//                     default in runtime context if no other action)
//     --build OUT     produce an executable at OUT
//
// Examples (all equivalent re: build):
//     naruby --build out --run --aot-compile main.rb
//     naruby --aot-compile --build out --run main.rb
//     naruby --run --aot-compile --build out main.rb
// But not:
//     naruby main.rb --build out      # "main.rb" --build out are ARGV
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
#include <stdio.h>

struct astro_build_config {
    // ---------- mode (from argv parsing) ----------
    bool plain;          // --plain        — don't use compiled code
    bool compiled_only;  // --compiled-only — strict inverse of --plain: run only
                         //   baked SDs; abort on any interpreter dispatch (AOT
                         //   compile-miss detection).  Sample wires the poison.
    bool aot_compile;    // --aot-compile  — bake AOT specializations
    bool pg_compile;     // --pg-compile   — bake PG specializations (implies run)
    bool run;            // --run          — execute (default in runtime, opt-in for build)

    // ---------- universal CLI knobs (from argv parsing) ----------
    bool quiet;             // -q / --quiet         (sample translates to its own quiet state)
    bool verbose;           // -v / --verbose       (runtime verbose; sample translates)
    bool help_requested;    // -h / --help          (sample prints its own help and exits)
    bool version_requested; // --version            (sample prints version and exits)

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
    bool show_cmd;       // ASTRO_BUILD_OPTS=--show-cmd — print the cc command line
    bool keep_intermediates;

    // ---------- sample-supplied (filled by the per-sample build helper) ----------
    bool no_libdl;       // suppress the default trailing -ldl (targets without
                         // libdl, e.g. wasm32-wasip1)
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
// astro_build_extract_flags — pre-scan argv for build-related flags
// ---------------------------------------------------------------------------
//
// Walks argv from argv[1] forward, identifying and removing build flags
// in place.  Stops at the first non-flag token (= source file under Unix
// convention; tokens after it stay untouched, becoming source / ARGV
// for the sample's own parser).
//
// Recognised flags (order-free among themselves):
//   --build PATH       (consumes next argv element as PATH)
//   --run
//   --aot-compile
//   --pg-compile       (also sets --run)
//   --plain
//   -q / --quiet
//   -v / --verbose
//   -h / --help        (signal — sample prints its own help and exits)
//   --version          (signal — sample prints version and exits)
//
// Other flags (starting with `-`) are passed through to the sample's
// parser.  argv is compacted in-place; *argc_io is updated.
//
// Also calls astro_build_load_env_opts() to parse ASTRO_BUILD_OPTS.
//
// Returns 0 on success, non-zero on parse error.
int astro_build_extract_flags(int *argc_io, char **argv,
                              struct astro_build_config *cfg);

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

// ---------------------------------------------------------------------------
// Universal CLI helpers
// ---------------------------------------------------------------------------

// Framework version string.  Embedded in the binary; sample's --version
// handler typically prints something like "<prog> (ASTro <ver>)".
#ifndef ASTRO_VERSION
#define ASTRO_VERSION "0.1"
#endif

// Print the framework's build-flag help section to fp.  Sample's own
// usage() / show_help() includes this so the framework flags get
// documented uniformly across samples.
void astro_print_build_help(FILE *fp);

#endif // ASTRO_BUILD_H
