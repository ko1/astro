// Runtime helpers shared by the REPL/AOT main (`main.c`) and the
// standalone exe driver (`exe_main.c`).  Owns CTX construction,
// builtin registration, and the code repository.
//
// `OPTION` is defined by the *caller* (main.c or exe_main.c) so each
// driver can pre-seed the option set differently (REPL parses argv;
// exe forces a fixed mode).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "context.h"

CTX *global_c;

// ---------------------------------------------------------------------------
// builtins
// ---------------------------------------------------------------------------

// node.h transitively #includes bf.h (static inline narb_p etc.), so
// no separate include needed here.

extern void code_repo_add2(const char *, NODE *, bool, uint32_t);

static void
define_func(CTX *c, const char *name, const char *func_name,
            builtin_func_ptr func, uint32_t params_cnt)
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

    code_repo_add2(name, fe->body, true, params_cnt);
}

#define DEFINE_FUNC(c, name, func_name, arity) \
    define_func(c, name, #func_name, (builtin_func_ptr)func_name, arity)

void
define_builtin_functions(CTX *c)
{
    DEFINE_FUNC(c, "p",      narb_p,    1);
    DEFINE_FUNC(c, "zero",   narb_zero, 0);
    DEFINE_FUNC(c, "bf_add", narb_add,  2);
}

// Used by EMIT_AST_node_call_builtin: look up a previously-registered
// bf entry by its Ruby name.  At exe runtime, define_builtin_functions
// has already populated `global_c->func_set` so this just scans.
struct builtin_func *
find_builtin_func_by_name(const char *name)
{
    if (!global_c) return NULL;
    for (unsigned int i = 0; i < global_c->func_set_cnt; i++) {
        if (strcmp(global_c->func_set[i].name, name) == 0) {
            NODE *body = global_c->func_set[i].body;
            // body is ALLOC_node_call_builtin(bf, func, params_cnt).
            return body->u.node_call_builtin.bf;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// context
// ---------------------------------------------------------------------------

CTX *
create_context(int frames, int funcs)
{
    CTX *c = (CTX *)malloc(sizeof(CTX));
    c->env = c->fp = (VALUE *)malloc(sizeof(VALUE) * 10 * frames);
    c->func_set = malloc(sizeof(struct function_entry) * funcs);
    c->func_set_cnt = 0;
    c->serial = 1;
#if DEBUG_EVAL
    c->frame_cnt = 0;
    c->rec_cnt = 0;
#endif
    define_builtin_functions(c);
    return c;
}

// ---------------------------------------------------------------------------
// code repository
// ---------------------------------------------------------------------------

static struct code_repo {
    uint32_t size;
    uint32_t capa;

    struct code_entry {
        const char *name;
        NODE *body;
        uint32_t params_cnt;
        uint32_t locals_cnt;
        bool skip_specialize;
    } *entries;
} code_repo;

static struct code_entry *
code_repo_new_entry(void)
{
    if (code_repo.size < code_repo.capa) {
        return &code_repo.entries[code_repo.size++];
    }
    uint32_t capa = code_repo.capa * 2;
    if (capa == 0) capa = 8;
    code_repo.entries = realloc(code_repo.entries,
                                sizeof(struct code_entry) * capa);
    if (!code_repo.entries) {
        fprintf(stderr, "no memory for capa:%u\n", capa);
        exit(1);
    }
    code_repo.capa = capa;
    return code_repo_new_entry();
}

NODE *
code_repo_find(node_hash_t h)
{
    if (h != 0) {
        for (uint32_t i=0; i<code_repo.size; i++) {
            NODE *n = code_repo.entries[i].body;
            if (HASH(n) == h) return n;
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
    if (body == NULL || (!force_add && found)) return;

    struct code_entry *ce = code_repo_new_entry();
    ce->name = name;
    ce->body = body;
    ce->locals_cnt = locals_cnt;
    ce->skip_specialize = found;
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

// ---------------------------------------------------------------------------
// Code repo iteration — exposed for build_code_store_aot in main.c and
// for the exe driver (which doesn't need it but symmetry keeps the
// linkage clean).
// ---------------------------------------------------------------------------

uint32_t  naruby_code_repo_size(void)                       { return code_repo.size; }
NODE     *naruby_code_repo_body(uint32_t i)                 { return code_repo.entries[i].body; }
const char *naruby_code_repo_name(uint32_t i)               { return code_repo.entries[i].name; }
bool      naruby_code_repo_skip_specialize(uint32_t i)      { return code_repo.entries[i].skip_specialize; }
