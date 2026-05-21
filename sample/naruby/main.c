
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "node.h"
#include "astro_code_store.h"
#include "astro_node.h"
#include "astro_build.h"
#include "astro_jit.h"

#ifndef NARUBY_SRC_DIR
#define NARUBY_SRC_DIR "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

// Forward decl from naruby_parse.c — walks the all_pg_call_nodes list
// (every node_pg_call_<N> allocated this run) and updates each call
// site's `sp_body` operand from `cc->body`.  Called immediately before
// build_code_store_pgsd's astro_cs_compile pass so the emitted PGSDs
// embed profile-derived speculation rather than parse-time guesses.
void naruby_update_sp_bodies_from_cc(void);

NODE *PARSE(int argc, char *argv[]);
extern const char *naruby_current_source_file;

struct naruby_option OPTION = {
    // .static_lang = true,
};

// `global_c`, `define_builtin_functions`, `create_context`, the code
// repository, and `find_builtin_func_by_name` all live in
// naruby_runtime.c (shared with exe_main.c).
extern CTX *global_c;
extern CTX *create_context(int frames, int funcs);
extern void define_builtin_functions(CTX *c);
extern uint32_t  naruby_code_repo_size(void);
extern NODE     *naruby_code_repo_body(uint32_t i);
extern const char *naruby_code_repo_name(uint32_t i);
extern bool      naruby_code_repo_skip_specialize(uint32_t i);

size_t node_cnt;

// Common knobs for both bakes (AOT and PGSD).  -Wl,-Bsymbolic resolves
// intra-.so SD→SD references at link time so the body bakes a direct
// `addr32 call` instead of a GOT load.  --param=early-inlining-insns=100
// bumps gcc's early-inliner budget so the medium-sized EVAL_node_*
// chain stays inlined into the SD body (default ~14 insns truncates
// halfway through).
static void
common_build_flags_and_link(void)
{
    setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);
    astro_cs_build("--param=early-inlining-insns=100");
    astro_cs_reload();
}

// AOT bake: emit SD_<HORG>.c for the program AST and every code_repo
// body.  No PGSD output here — that's `build_code_store_pgsd`'s job
// after the run when cc state is available.  Triggered by `-c`.
static void
build_code_store_aot(NODE *ast)
{
    if (ast) astro_cs_compile(ast, NULL);
    uint32_t n = naruby_code_repo_size();
    for (uint32_t i = 0; i < n; i++) {
        if (naruby_code_repo_skip_specialize(i)) continue;
        NODE *body = naruby_code_repo_body(i);
        if (body) astro_cs_compile(body, NULL);
    }
    common_build_flags_and_link();
}

// PGSD bake: walk the all_pg_call_nodes list and update each call
// site's `sp_body` from the just-finished run's `cc->body`, then emit
// PGSD_<HOPT>.c for the AST and every code_repo body.  HOPT now folds
// in the cc-derived speculation, so each PGSD is keyed on (call site
// structure × observed body) and a future cs_load matches it via the
// hopt_index entry written here.
//
// We emit a PGSD for every entry, even when HOPT == HORG (= profile
// didn't refine anything observable through HOPT).  That redundancy
// is required: a parent PGSD's baked direct call uses
// `PGSD_<HOPT(child)>` regardless of whether the child's HOPT differs
// from its HORG, so skipping the HOPT==HORG cases would leave
// undefined-symbol references at dlopen time.
//
// Triggered by `-p`.
static void
build_code_store_pgsd(NODE *ast)
{
    naruby_update_sp_bodies_from_cc();

    if (ast) {
        astro_cs_compile(ast, naruby_current_source_file);
    }
    uint32_t n = naruby_code_repo_size();
    for (uint32_t i = 0; i < n; i++) {
        if (naruby_code_repo_skip_specialize(i)) continue;
        NODE *body = naruby_code_repo_body(i);
        if (!body) continue;
        const char *fname = naruby_code_repo_name(i);
        astro_cs_compile(body, fname);
    }
    common_build_flags_and_link();
}

// Recursive `rm -rf`-style helper for `--ccs`.  Best-effort: failures
// are warned but not fatal (e.g. a partial state from a prior crash
// is acceptable; cs_init will rebuild what it needs).
static void
clear_code_store_dir(void)
{
    // Use system() for simplicity — code_store/ is naruby-managed and
    // this only runs when the user explicitly asks via --ccs.
    int rc = system("rm -rf code_store");
    if (rc != 0) {
        fprintf(stderr, "naruby: --ccs: rm -rf code_store failed (rc=%d)\n", rc);
    }
}

// --generate-executable support — invoked AFTER the program AST has
// been built and (optionally) AOT-baked into code_store/c/SD_*.c.
//
// Writes _embed.c (DAG-aware AST builder) and _static_table.c, then
// dispatches to astro_build_executable() with the right source list.
static int
generate_naruby_executable(NODE *ast,
                           const struct astro_build_config *bcfg_in)
{
    // Emit the embedded AST builder (DAG mode — naruby has node
    // sharing via function bodies referenced from multiple sites).
    FILE *fp = fopen("_embed.c", "w");
    if (!fp) { perror("_embed.c"); return 1; }
    astro_emit_ast_c_program(fp, ast, "astro_build_embedded_ast", "node.h");
    fclose(fp);

    // Emit the static SD lookup table.
    fp = fopen("_static_table.c", "w");
    if (!fp) { perror("_static_table.c"); return 1; }
    astro_cs_emit_static_table(fp, "ASTRO_SD_PROTO");
    fclose(fp);

    // Enumerate SD_*.c / PGSD_*.c in code_store/c/.
    const char **sd_files = NULL;
    size_t sd_n = 0, sd_capa = 0;
    DIR *d = opendir("code_store/c");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *nm = ent->d_name;
            size_t l = strlen(nm);
            if (l < 5) continue;
            if (strcmp(nm + l - 2, ".c") != 0) continue;
            if (strncmp(nm, "SD_", 3) != 0 &&
                strncmp(nm, "PGSD_", 5) != 0) continue;
            if (sd_n + 1 >= sd_capa) {
                sd_capa = sd_capa == 0 ? 16 : sd_capa * 2;
                sd_files = realloc(sd_files, sizeof(*sd_files) * sd_capa);
            }
            char *full = malloc(l + 32);
            snprintf(full, l + 32, "code_store/c/%s", nm);
            sd_files[sd_n++] = full;
        }
        closedir(d);
    }
    if (sd_n + 1 >= sd_capa) {
        sd_capa = sd_n + 1;
        sd_files = realloc(sd_files, sizeof(*sd_files) * sd_capa);
    }
    sd_files[sd_n] = NULL;

    size_t extra_n = 2 + sd_n + 1;
    const char **extras = malloc(sizeof(*extras) * extra_n);
    extras[0] = "_embed.c";
    extras[1] = "_static_table.c";
    for (size_t i = 0; i < sd_n; i++) extras[2 + i] = sd_files[i];
    extras[2 + sd_n] = NULL;

    struct astro_build_config bcfg = *bcfg_in;
    bcfg.src_dir = NARUBY_SRC_DIR;
    bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
    static const char *sources[] = {
        "node.c", "node_slowpath.c", "naruby_runtime.c", "exe_main.c", NULL,
    };
    bcfg.sources = sources;
    bcfg.extra_sources_abs = extras;
    // Naruby's SD code uses gcc's intra-.so symbol resolution to bake
    // direct calls between SDs.  In an exe build there's no .so, so we
    // skip -Wl,-Bsymbolic; the exe's flat namespace already gives us
    // direct call resolution.
    const char *extra_cflags[] = { "--param=early-inlining-insns=100", NULL };
    if (!bcfg.extra_cflags) bcfg.extra_cflags = extra_cflags;

    int rc = astro_build_executable(&bcfg);

    if (!bcfg.keep_intermediates) {
        unlink("_embed.c");
        unlink("_static_table.c");
    }
    free(extras);
    for (size_t i = 0; i < sd_n; i++) free((void *)sd_files[i]);
    free(sd_files);
    return rc;
}

int
main(int argc, char *argv[])
{
    // Pull astro_build flags (--generate-executable, --cc, -O*, etc.)
    // out of argv FIRST so the naruby parser doesn't see them.
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_parse_args(&argc, argv, &bcfg) != 0) return 1;
    // Order:
    //   1. Parse CLI (so we know about --ccs / --plain / -c / -p).
    //   2. Optionally wipe code_store (--ccs).
    //   3. INIT (cs_init dlopens any existing all.so) — skipped under --plain.
    //   4. Parse source.
    //   5. -c: AOT-bake SDs before EVAL so the run uses them.
    //   6. cs_load each entry (binds dispatcher → SD/PGSD when found).
    //   7. EVAL.
    //   8. -p: PGSD-bake using cc state from this run.
    //
    // -c and -p are orthogonal: either, neither, or both.  The default
    // (no flags) is "use whatever's in code_store" — `make_code_store`
    // skipped, cs_load still runs.
    CTX *c = create_context(10000, 2000);
    global_c = c;

    // PARSE has to inspect argv to find OPTION flags (--ccs, --plain, …),
    // but it does not touch code_store — we can clear / cs_init around it.
    NODE *ast = PARSE(argc, argv);

    // --ccs takes effect AFTER parse so that any logging through cs_init
    // sees the post-clear state.  Then INIT() dlopens whatever's left in
    // code_store/all.so so subsequent cs_load calls (during OPTIMIZE and
    // node_def's EVAL hook) can bind dispatchers.
    if (OPTION.clear_store) {
        clear_code_store_dir();
    }
    if (!OPTION.plain) {
        INIT();
    }

    if (!OPTION.quiet && false) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    if (OPTION.compile_first && !OPTION.plain && !OPTION.skip_bake) {
        build_code_store_aot(ast);
    }

    if (!OPTION.plain) {
        OPTIMIZE(ast);
        // Override OPTIMIZE's plain-AOT cs_load with one that consults
        // hopt_index for a PGSD_<HOPT(top)>.  That makes top's baked
        // direct calls go through PGSDs (when present) so the whole
        // chain inherits profile-derived speculation.
        (void)astro_cs_load(ast, naruby_current_source_file);
    }

    if (!OPTION.compile_only) {
        RESULT r = EVAL(c, ast, c->env);
        printf("Result: %ld, node_cnt:%lu\n", r.value, node_cnt);
    }

    if (OPTION.pg_at_exit && !OPTION.plain && !OPTION.skip_bake) {
        build_code_store_pgsd(ast);
    }

    if (bcfg.out_exe) {
        // AOT-bake into code_store unless --plain.  The static table
        // emitter reads the same compile log.
        if (!OPTION.plain) {
            astro_cs_compile(ast, NULL);
            uint32_t n = naruby_code_repo_size();
            for (uint32_t i = 0; i < n; i++) {
                if (naruby_code_repo_skip_specialize(i)) continue;
                NODE *body = naruby_code_repo_body(i);
                if (body) astro_cs_compile(body, NULL);
            }
        }
        int rc = generate_naruby_executable(ast, &bcfg);
        astro_build_config_dispose(&bcfg);
        return rc;
    }

    return 0;
}
