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

// node_call2 slowpath: triggered when `node_call2`'s two-step guard
// fails — either cc is stale (serial mismatch) or current body
// disagrees with sp_body (parse-time speculation).  We refresh cc
// (only when needed) and dispatch via the resolved body.  We do
// **not** write back sp_body: it's a parse-time invariant that ties
// the call site to its baked SD constant.  When sp_body and cc->body
// disagree persistently (= speculation broken), this slowpath fires
// every call, but the fast-path `&& cc->body == sp_body` short-circuit
// catches it cheaply.
RESULT
node_pg_call_slowpath(CTX * restrict c, NODE * restrict n,
                      VALUE * restrict fp, const char *name,
                      uint32_t params_cnt, uint32_t arg_index,
                      struct callcache * restrict cc)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss1\n", name);
    (void)n;

    if (c->serial != cc->serial) {
        struct function_entry *fe = sp_find_func_entry(c, name);
        sp_call_check(fe, name, params_cnt);
        cc->serial = c->serial;
        cc->body = fe->body;
    }

    RESULT r = EVAL(c, cc->body, fp + arg_index);
    return RESULT_OK(r.value);  // catch RESULT_RETURN at function boundary
}

// node_pg_call_<N> slowpath.  Same logic as `node_pg_call_slowpath`
// but allocates a fresh F sized from the resolved body's locals_cnt
// (the call site's `locals_cnt` operand was sized for sp_body, which
// may be different from cc->body).  Args are passed separately
// because the caller's VLA-sized F could be too small for the new
// body.  Like node_call2's slowpath, sp_body is **not** updated.
RESULT
node_pg_call_n_slowpath(CTX * restrict c, NODE * restrict n,
                        const VALUE *args, uint32_t argc,
                        const char *name,
                        struct callcache * restrict cc)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss1 (pg_call_N)\n", name);
    (void)n;

    if (c->serial != cc->serial) {
        struct function_entry *fe = sp_find_func_entry(c, name);
        sp_call_check(fe, name, argc);
        cc->serial = c->serial;
        cc->body = fe->body;
    }

    uint32_t lc = code_repo_find_locals_cnt_by_body(cc->body);
    if (lc < argc) lc = argc;
    if (lc == 0) lc = 1;
    VALUE F[lc];
    for (uint32_t i = 0; i < argc; i++) F[i] = args[i];

    RESULT r = EVAL(c, cc->body, F);
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
