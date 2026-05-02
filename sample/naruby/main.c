
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "node.h"
#include "astro_code_store.h"
#include "astro_jit.h"

NODE *PARSE(int argc, char *argv[]);

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

// AOT compile: emit one SD_<hash>.c per entry into code_store/c/, then
// `make` the bundle into code_store/all.so.  Subsequent runs auto-load
// SD_<hash> via OPTIMIZE() → astro_cs_load (in node.c).
//
// Entries: the program AST plus every named body in code_repo.  Bodies
// flagged `skip_specialize` are duplicates already emitted under another
// entry's hash — astro_cs_compile would still file-dedup them, but we
// keep the explicit skip to avoid touching the same .c twice.

static void
build_code_store(NODE *ast)
{
    if (ast) astro_cs_compile(ast, NULL);

    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) continue;
        NODE *body = code_repo.entries[i].body;
        if (body) astro_cs_compile(body, NULL);
    }

    // -Wl,-Bsymbolic: SDs in all.so call each other (the recursive
    // call site SD calls the body's SD baked as a constant); without
    // this flag the linker routes those references through the GOT
    // even though the symbol is defined in the same .so, costing one
    // extra load per call and disabling tail-call optimisation across
    // SD boundaries.
    setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);

    // --param=early-inlining-insns=100: gcc's default early-inliner
    // budget is ~14 insns; that's enough for leaf nodes but bails out
    // on the medium-sized EVAL_node_call2 / call_body chain that the
    // SD wants to inline (cc check + push frame + EVAL + state catch).
    // Bumping the budget unblocks SROA on the inlined chain.
    //
    // (We tried -flto=auto for cross-TU IPA on the recursive call
    // chain — it removed the post-call state check but didn't fix
    // the `fp[0]` reload, and slightly regressed call-heavy benches.
    // Bsymbolic-only direct call already gives us most of the win.)
    astro_cs_build("--param=early-inlining-insns=100");

    astro_cs_reload();
}

int
main(int argc, char *argv[])
{
    INIT();

    CTX *c = create_context(10000, 2000);
    global_c = c;
    NODE *ast = PARSE(argc, argv);

    if (0 && !OPTION.quiet) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    if (!OPTION.compile_only) {
        OPTIMIZE(ast);
        // printf("main dispatcher:%s (h:%lx)\n", ast->head.dispatcher_name, HASH(ast));
        RESULT r = EVAL(c, ast, c->env);
        printf("Result: %ld, node_cnt:%lu\n", r.value, node_cnt);
    }

    if (!OPTION.no_generate_specialized_code) {
        build_code_store(ast);
    }

    return 0;
}
