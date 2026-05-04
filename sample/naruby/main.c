
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "node.h"
#include "astro_code_store.h"
#include "astro_jit.h"

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
static CTX *global_c;

size_t node_cnt;

// builtin functions

static void
define_func(CTX *c, const char *name, const char *func_name, builtin_func_ptr func, uint32_t params_cnt)
{
    struct function_entry *fe = &c->func_set[c->func_set_cnt++];
    struct builtin_func *bf = malloc(sizeof(struct builtin_func));
    bf->name = name;
    bf->func = func;
    bf->func_name = func_name;
    bf->have_src = true;

    fe->name = name;
    fe->body = ALLOC_node_call_builtin(bf, func, params_cnt);
    fe->params_cnt = params_cnt;
    fe->locals_cnt = params_cnt;

    // Builtins use params_cnt as locals_cnt — the body just calls the
    // C function with fp[0..params_cnt-1]; no further temporaries.
    code_repo_add2(name, fe->body, true, params_cnt);
}

#define DEFINE_FUNC(c, name, func_name, arity) define_func(c, name, #func_name, (builtin_func_ptr)func_name, arity)

static void
define_builtin_functions(CTX *c)
{
    DEFINE_FUNC(c, "p", narb_p, 1);
    DEFINE_FUNC(c, "zero", narb_zero, 0);
    DEFINE_FUNC(c, "bf_add", narb_add, 2);
}

// code repository

static struct code_repo {
    uint32_t size;
    uint32_t capa;

    struct code_entry {
        const char *name;
        NODE *body;
        uint32_t params_cnt;
        uint32_t locals_cnt;   // = max slot index used by body, ≥ params_cnt
        bool skip_specialize;
    } *entries;
} code_repo;

static struct code_entry *
code_repo_new_entry(void)
{
    if (code_repo.size < code_repo.capa) {
        return &code_repo.entries[code_repo.size++];
    }
    else {
        uint32_t capa = code_repo.capa * 2;
        if (capa == 0) {
            capa = 8;
        }
        code_repo.entries = realloc(code_repo.entries, sizeof(struct code_entry) * capa);

        if (code_repo.entries) {
            code_repo.capa = capa;
            return code_repo_new_entry();
        }
        else {
            fprintf(stderr, "no memory for capa:%u\n", capa);
            exit(1);
        }
    }
}

NODE *
code_repo_find(node_hash_t h)
{
    if (h != 0) {
        for (uint32_t i=0; i<code_repo.size; i++) {
            NODE *n = code_repo.entries[i].body;
            if (HASH(n) == h) {
                return n;
            }
        }
    }

    return NULL;
}

NODE *
code_repo_find_by_name(const char *name)
{
    for (uint32_t i=0; i<code_repo.size; i++) {
        if (strcmp(code_repo.entries[i].name, name) == 0) {
            return code_repo.entries[i].body;
        }
    }

    return NULL;
}

void
code_repo_add(const char *name, NODE *body, bool force_add)
{
    code_repo_add2(name, body, force_add, 0);
}

void
code_repo_add2(const char *name, NODE *body, bool force_add, uint32_t locals_cnt)
{
    bool found = code_repo_find(HASH(body)) != NULL;

    if (body == NULL || (!force_add && found)) {
        // ignore
    }
    else {
        struct code_entry *ce = code_repo_new_entry();
        ce->name = name;
        ce->body = body;
        ce->locals_cnt = locals_cnt;
        ce->skip_specialize = found;
    }
}

uint32_t
code_repo_find_locals_cnt_by_body(NODE *body)
{
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (code_repo.entries[i].body == body) {
            return code_repo.entries[i].locals_cnt;
        }
    }
    return 0;
}

uint32_t
code_repo_find_locals_cnt_by_name(const char *name)
{
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (strcmp(code_repo.entries[i].name, name) == 0) {
            return code_repo.entries[i].locals_cnt;
        }
    }
    return 0;
}

// context management

static CTX *
create_context(int frames, int funcs)
{
    CTX *c = (CTX *)malloc(sizeof(CTX));
    c->env = c->fp = (VALUE *)malloc(sizeof(VALUE) * 10 * frames);
    c->func_set = malloc(sizeof(struct function_entry) * funcs); // supports 100 functions
    c->func_set_cnt = 0;
    c->serial = 1;

#if DEBUG_EVAL
    c->frame_cnt = 0;
    c->rec_cnt = 0;
#endif

    define_builtin_functions(c);
    return c;
}

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
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) continue;
        NODE *body = code_repo.entries[i].body;
        if (body) astro_cs_compile(body, NULL);
    }
    common_build_flags_and_link();
}

// PGSD bake: walk the all_pg_call_nodes list and update each call
// site's `sp_body` from the just-finished run's `cc->body`, then emit
// PGSD_<HOPT>.c for the AST and every code_repo body — HOPT now folds
// in the cc-derived speculation, so each PGSD is keyed on (call site
// structure × observed body) and a future cs_load matches it via the
// hopt_index entry written here.
//
// HOPT == HORG short-circuit: when an entry's HOPT happens to equal
// its HORG (= profile didn't change anything observable through HOPT
// — typically because none of its descendants are pg_call sites with
// a non-zero cc, or because they all speculate to the same body the
// parser already assumed), the PGSD bake would emit an SD identical
// to the AOT one already on disk.  Skip — the existing SD_<Horg>
// covers it, and we save the disk + link time.
//
// Triggered by `-p`.
static void
build_code_store_pgsd(NODE *ast)
{
    naruby_update_sp_bodies_from_cc();

    if (ast && HOPT(ast) != HORG(ast)) {
        astro_cs_compile(ast, naruby_current_source_file);
    }
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) continue;
        NODE *body = code_repo.entries[i].body;
        if (!body) continue;
        if (HOPT(body) == HORG(body)) continue;
        const char *fname = code_repo.entries[i].name;
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

int
main(int argc, char *argv[])
{
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
    NODE *ast = PARSE(argc, argv);

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

    if (OPTION.compile_first && !OPTION.plain) {
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

    if (OPTION.pg_at_exit && !OPTION.plain) {
        build_code_store_pgsd(ast);
    }

    return 0;
}
