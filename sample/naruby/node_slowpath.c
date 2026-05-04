// Cold-path bodies for `node_call2` / `node_pg_call_<N>`.
//
// Why this file exists
// --------------------
// Both `node_eval.c` (generated from node.def) and the AOT/PG
// code_store/*.c files include the slowpath bodies via
// `#include "node_eval.c"`.  When the slowpaths were `static
// __attribute__((noinline))` in node_eval.c, every dlopen-able .so
// in code_store/ ended up with its own duplicate copy of the
// slowpath text — significant binary bloat for a path that only
// fires on cache miss / redefinition.
//
// Solution: pull the slowpath bodies out of node_eval.c into this
// standalone .c.  It compiles into the main `naruby` binary only.
// The generated SDs in code_store/*.so reference the slowpaths as
// unresolved externs; -rdynamic + dlopen resolves them against the
// main binary at load time, so there is exactly one copy.
//
// node_eval.c keeps the `extern` declarations (via node.h) so both
// the main-binary EVAL_ functions and the AOT/PG SDs link cleanly.
//
// The small helpers (find_func_entry, call_check, call_body) are
// duplicated locally here — they're tiny, and node_call's NODE_DEF
// fast path inlines them in node_eval.c, so we can't simply remove
// them from there.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "node.h"
#include "context.h"

#define CALL_DEBUG 0

#ifndef LIKELY
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

static struct function_entry *
sp_find_func_entry(CTX * restrict c, const char *name)
{
    for (int i = 0; i < c->func_set_cnt; i++) {
        struct function_entry *fe = &c->func_set[i];
        if (strcmp(name, fe->name) == 0) {
            return fe;
        }
    }
    return NULL;
}

static void
sp_call_check(struct function_entry *fe, const char *name, uint32_t params_cnt)
{
    if (UNLIKELY(fe == NULL)) {
        fprintf(stderr, "unknown function: %s\n", name);
        exit(1);
    }
    else if (UNLIKELY(fe->params_cnt != params_cnt)) {
        fprintf(stderr, "wrong parameter count for %s (%u for %u)\n",
                name, params_cnt, fe->params_cnt);
    }
}

// node_call2 slowpath: caller's args are already in fp[arg_index..],
// callee's frame aliases that span.  Triggered on cc miss.
RESULT
node_pg_call_slowpath(CTX * restrict c, NODE * restrict n,
                      VALUE * restrict fp, const char *name,
                      uint32_t params_cnt, uint32_t arg_index,
                      struct callcache * restrict cc,
                      NODE **sp_body_p)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss1\n", name);
    bool was_warm = (cc->serial != 0);

    struct function_entry *fe = sp_find_func_entry(c, name);
    sp_call_check(fe, name, params_cnt);
    cc->serial = c->serial;
    *sp_body_p = cc->body = fe->body;

    // On redefinition (was_warm + currently specialized), demote the
    // dispatcher: the baked SD points at the OLD body's specialized
    // code, but the cache now holds the NEW body — calling through
    // the SD would dispatch the new body via the old body's code.
    if (was_warm && n->head.flags.is_specialized) {
        n->head.dispatcher = n->head.kind->default_dispatcher;
        n->head.flags.is_specialized = false;
    }

    RESULT r = EVAL(c, fe->body, fp + arg_index);
    return RESULT_OK(r.value);  // catch RESULT_RETURN at function boundary
}

// node_pg_call_<N> slowpath.  Args are passed separately (not via a
// pre-allocated F): on a cc miss the body may have just been
// redefined with a different `locals_cnt`, so the caller's
// VLA-sized F could be too small for the new body.  We size a fresh
// F here from the resolved body's locals_cnt.
RESULT
node_pg_call_n_slowpath(CTX * restrict c, NODE * restrict n,
                        const VALUE *args, uint32_t argc,
                        const char *name,
                        struct callcache * restrict cc,
                        NODE **sp_body_p)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss1 (pg_call_N)\n", name);
    bool was_warm = (cc->serial != 0);

    struct function_entry *fe = sp_find_func_entry(c, name);
    sp_call_check(fe, name, argc);
    cc->serial = c->serial;
    *sp_body_p = cc->body = fe->body;

    if (was_warm && n->head.flags.is_specialized) {
        n->head.dispatcher = n->head.kind->default_dispatcher;
        n->head.flags.is_specialized = false;
    }

    uint32_t lc = code_repo_find_locals_cnt_by_body(fe->body);
    if (lc < argc) lc = argc;
    if (lc == 0) lc = 1;
    VALUE F[lc];
    for (uint32_t i = 0; i < argc; i++) F[i] = args[i];

    RESULT r = EVAL(c, fe->body, F);
    return RESULT_OK(r.value);
}

// node_call_<N> slowpath.  Same shape as `node_pg_call_n_slowpath` but
// without `sp_body` — these AOT/interp call nodes resolve the body via
// `cc` only.  Triggered on cc miss.
RESULT
node_call_n_slowpath(CTX * restrict c, NODE * restrict n,
                     const VALUE *args, uint32_t argc,
                     const char *name,
                     struct callcache * restrict cc)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss1 (call_N)\n", name);
    bool was_warm = (cc->serial != 0);

    struct function_entry *fe = sp_find_func_entry(c, name);
    sp_call_check(fe, name, argc);
    cc->serial = c->serial;
    cc->body = fe->body;

    if (was_warm && n->head.flags.is_specialized) {
        n->head.dispatcher = n->head.kind->default_dispatcher;
        n->head.flags.is_specialized = false;
    }

    uint32_t lc = code_repo_find_locals_cnt_by_body(fe->body);
    if (lc < argc) lc = argc;
    if (lc == 0) lc = 1;
    VALUE F[lc];
    for (uint32_t i = 0; i < argc; i++) F[i] = args[i];

    RESULT r = EVAL(c, fe->body, F);
    return RESULT_OK(r.value);
}
