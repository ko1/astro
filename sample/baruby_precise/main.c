
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "node.h"
#include "astro_code_store.h"
#include "astro_jit.h"
#include "precise_gc/gc.h"

// Forward decl from baruby_parse.c — walks the all_pg_call_nodes list
// (every node_pg_call_<N> allocated this run) and updates each call
// site's `sp_body` operand from `cc->body`.  Called immediately before
// build_code_store_pgsd's astro_cs_compile pass so the emitted PGSDs
// embed profile-derived speculation rather than parse-time guesses.
void baruby_update_sp_bodies_from_cc(void);

NODE *PARSE(int argc, char *argv[]);
extern const char *baruby_current_source_file;

struct baruby_option OPTION = {
    // .static_lang = true,
};

// GC timer state moved into CTX (= no global mutable state).
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
    DEFINE_FUNC(c, "p", barb_p, 1);
    DEFINE_FUNC(c, "zero", barb_zero, 0);
    DEFINE_FUNC(c, "bf_add", barb_add, 2);
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

/* iter 76: framework は CTX-opaque 化したため `aro_gc_visit_roots` 関数は
 * 廃止。 root visitor は context.h の ASTRO_GC_VISIT_ROOTS macro に inline
 * 展開する形に変更した (= 各 backend の GC entry で直接 macro 呼び出し)。
 * sp_high_water 状態は sample-side で持つ。 */
VALUE *baruby_gc_sp_high_water;

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

/* iter 61: accessors used by the parse-end walker (baruby_parse.c) to
 * iterate every registered body and walk it with the right locals_cnt. */
uint32_t code_repo_count(void) { return code_repo.size; }
NODE *code_repo_body_at(uint32_t i) { return code_repo.entries[i].body; }
uint32_t code_repo_locals_cnt_at(uint32_t i) { return code_repo.entries[i].locals_cnt; }

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

/* Set by baruby_parse.c after parsing PM_PROGRAM_NODE — the number of
 * locals slots required by toplevel code.  Used by main() (after PARSE)
 * to size c->sp correctly.  Default 0 so create_context's initial
 * c->sp = c->env survives if PARSE somehow doesn't update it; main
 * always re-applies after PARSE returns. */
uint32_t aro_toplevel_locals_cnt = 0;

static CTX *
create_context(int frames, int funcs)
{
    CTX *c = (CTX *)malloc(sizeof(CTX));
    /* VALUE stack: huge virtual reservation, lazy-paged.  Old code used
     * calloc(10 * frames) ~= 800 KB which capped recursion-depth × per-
     * frame-locals.  Now reserve 8 GiB virtual (= 1 billion slots);
     * physical pages commit only as the stack grows.  GC scans
     * c->env..c->sp, untouched pages stay zero = VAL_FALSE.  No program
     * limit on recursion depth beyond available memory. */
    (void)frames;  /* historical: was 10 × frames; now ignored */
    size_t stack_bytes = (size_t)8u << 30;   /* 8 GiB virtual */
    c->env = (VALUE *)mmap(NULL, stack_bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (c->env == MAP_FAILED) { perror("mmap value stack"); abort(); }
    // sp will be moved past toplevel locals after PARSE; see main().
    c->sp = c->env;
    c->func_set = malloc(sizeof(struct function_entry) * funcs);
    c->func_set_cnt = 0;
    c->serial = 1;

#if DEBUG_EVAL
    c->frame_cnt = 0;
    c->rec_cnt = 0;
#endif

    aro_gc_init(c);
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
    /* Iter 36 AOT fix: SD_*.c → node.h → context.h → astro_debug.h chain
     * fails to compile from code_store/ without these include paths.
     * Absolute paths are baked at build time via -DBARUBY_PRECISE_DIR etc.
     * so the AOT bake works regardless of cwd / where the user launched. */
    char extra_cflags[1024];
    snprintf(extra_cflags, sizeof(extra_cflags),
             "--param=early-inlining-insns=100"
             " -fcf-protection=none"           /* drop endbr64 4-byte prologue */
             " -I" BARUBY_PRECISE_DIR
             " -I" ASTRO_RUNTIME_DIR           /* covers precise_gc/gc.h, astro_node.h, etc. */
             " -I" ASTRO_PRISM_INC_DIR
             " -DBARUBY_GC=%d", BARUBY_GC);
    astro_cs_build(extra_cflags);
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
    baruby_update_sp_bodies_from_cc();

    if (ast) {
        astro_cs_compile(ast, baruby_current_source_file);
    }
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) continue;
        NODE *body = code_repo.entries[i].body;
        if (!body) continue;
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
    // Use system() for simplicity — code_store/ is baruby-managed and
    // this only runs when the user explicitly asks via --ccs.
    int rc = system("rm -rf code_store");
    if (rc != 0) {
        fprintf(stderr, "baruby: --ccs: rm -rf code_store failed (rc=%d)\n", rc);
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
    // baruby_precise: precise mark&sweep (gc.c), initialized in create_context.
    CTX *c = create_context(10000, 2000);
    global_c = c;

    // PARSE has to inspect argv to find OPTION flags (--ccs, --plain, …),
    // but it does not touch code_store — we can clear / cs_init around it.
    NODE *ast = PARSE(argc, argv);
    /* Move sp past the toplevel locals area now that the parser has
     * reported the exact size.  No more hardcoded "64" cap.
     * iter 60 child-self-advance: sp is the BASE of the toplevel NODE's
     * slot area; its DISPATCH prologue advances by ast.slot_count
     * internally.  So main.c only needs to position sp at locals top. */
    c->sp = c->env + aro_toplevel_locals_cnt;

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

    if (getenv("BARUBY_DUMP_AST")) {
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
        (void)astro_cs_load(ast, baruby_current_source_file);
    }

    if (!OPTION.compile_only) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        // Top-level EVAL: fp = env, sp leaves room for toplevel locals.
        RESULT r = EVAL(c, ast, c->sp);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("Result: ");
        baruby_print_value(c, stdout, r.value);
        printf(", node_cnt:%lu\n", node_cnt);
        printf("__ELAPSED__ %.6f\n", elapsed);

        if (getenv("BARUBY_GC_STATS")) {
            size_t alloc_bytes = aro_gc_total_bytes(c);
            size_t heap_bytes  = aro_gc_heap_bytes(c);
            size_t gc_count    = aro_gc_count(c);
            size_t minor       = aro_gc_minor_count(c);
            size_t major       = aro_gc_major_count(c);
            double gc_seconds  = aro_gc_total_seconds(c);
            double mark_sec    = aro_gc_mark_seconds(c);
            double reclaim_sec = aro_gc_reclaim_seconds(c);
            double max_pause   = aro_gc_max_pause_seconds(c);
            double gc_pct      = elapsed > 0 ? (gc_seconds / elapsed) * 100 : 0;
            printf("__GC_STATS__ backend=%s alloc_bytes=%zu heap_bytes=%zu "
                   "gc_count=%zu minor=%zu major=%zu "
                   "gc_seconds=%.4f mark_seconds=%.4f reclaim_seconds=%.4f "
                   "gc_pct=%.1f max_pause_ms=%.2f\n",
                    aro_gc_backend_name, alloc_bytes, heap_bytes,
                    gc_count, minor, major, gc_seconds, mark_sec, reclaim_sec,
                    gc_pct, max_pause * 1000);
        }
    }

    if (OPTION.pg_at_exit && !OPTION.plain && !OPTION.skip_bake) {
        build_code_store_pgsd(ast);
    }

    aro_gc_fini(c);
    return 0;
}
