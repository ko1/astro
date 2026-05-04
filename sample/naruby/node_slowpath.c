// Cold-path bodies for every call-style NODE.
//
// Why this file exists
// --------------------
// The fast path lives inside the SD generated for each call site (in
// `code_store/*.so`).  Anything that's *not* the fast path — i.e. cc
// refresh, function-table lookup, error reporting, slow-mode arg
// evaluation — lives here, in the main `naruby` binary.  The SD's cold
// path is a single `jne <slowpath>` (resolved at dlopen via -rdynamic),
// so each call site adds only a few bytes of cold code to all.so.
//
// Each slowpath has the uniform signature `(CTX *, NODE *, VALUE *fp)`.
// All other call-site information (name, cc, sp_body, arg operands) is
// read from the NODE union via the appropriate `node_<variant>_struct`.
// Args that the fast path would have evaluated inline are
// (re-)evaluated here against the caller's `fp`.
//
// The small helpers `sp_find_func_entry`, `sp_call_check`, and
// `sp_dispatch` are shared across all slowpaths and stay local to this
// TU (they're only used here).
//
// Two-step guard (PG): the fast path checks `cc->serial == c->serial &&
// cc->body == sp_body`.  Slowpath fires when either is false.  We
// refresh cc if needed but never write `sp_body` (it stays at its
// parse-time value so the SD's baked direct call is always tied to a
// stable speculation), then dispatch via `cc->body` indirect — this
// covers both stale-cc and broken-speculation cases correctly.

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
sp_find_func_entry(CTX * restrict c, const char * restrict name)
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
sp_call_check(struct function_entry * restrict fe, const char * restrict name, uint32_t params_cnt)
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

// Refresh cc if stale (serial mismatch).  Does NOT write sp_body, and
// does NOT touch the profile — observations live in cc itself, and
// `profile_capture_call_nodes` (called from main.c at exit) walks all
// known call NODEs and snapshots `cc->body` to the profile in one
// shot.  Each slowpath call would otherwise pay an extra
// hash+lookup+memwrite; cc IS the profile, no separate bookkeeping.
static inline NODE *
sp_refresh_cc(CTX * restrict c, NODE * restrict call_node,
              struct callcache * restrict cc,
              const char * restrict name, uint32_t params_cnt)
{
    (void)call_node;
    if (c->serial != cc->serial) {
        struct function_entry *fe = sp_find_func_entry(c, name);
        sp_call_check(fe, name, params_cnt);
        cc->serial = c->serial;
        cc->body = fe->body;
    }
    return cc->body;
}

// Dispatch to `body` with caller's `fp` slid by `arg_index` (= the
// node_call / node_call2 convention: args are already in caller's
// fp[arg_index..]).  Catches RESULT_RETURN at the function boundary.
static inline RESULT
sp_dispatch_via_fp(CTX * restrict c, NODE * restrict body,
                   VALUE * restrict fp, uint32_t arg_index)
{
    RESULT r = EVAL(c, body, fp + arg_index);
    return RESULT_OK(r.value);
}

// Dispatch to `body` with a freshly-allocated frame F[lc] holding
// `args[0..argc-1]`.  Used by node_call_<N> / node_pg_call_<N>
// slowpaths where the callee frame is disjoint from the caller's fp.
// `lc` is sized off the resolved body (not the call site) because the
// resolved body may have a different locals_cnt than the parse-time
// speculation.
static inline RESULT
sp_dispatch_fresh_frame(CTX * restrict c, NODE * restrict body,
                        const VALUE *args, uint32_t argc)
{
    uint32_t lc = code_repo_find_locals_cnt_by_body(body);
    if (lc < argc) lc = argc;
    if (lc == 0) lc = 1;
    VALUE F[lc];
    for (uint32_t i = 0; i < argc; i++) F[i] = args[i];

    RESULT r = EVAL(c, body, F);
    return RESULT_OK(r.value);
}

// ---------- node_call (argc > 3, non-PG fallback) ----------

RESULT
node_call_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call)\n", n->u.node_call.name);
    struct callcache *cc = &n->u.node_call.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call.name, n->u.node_call.params_cnt);
    return sp_dispatch_via_fp(c, body, fp, n->u.node_call.arg_index);
}

// ---------- node_call_<N> (non-PG, arity-N specialized) ----------

RESULT
node_call_0_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_0)\n", n->u.node_call_0.name);
    (void)fp;
    struct callcache *cc = &n->u.node_call_0.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_0.name, 0);
    return sp_dispatch_fresh_frame(c, body, NULL, 0);
}

RESULT
node_call_1_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_1)\n", n->u.node_call_1.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_call_1.a0, fp));
    struct callcache *cc = &n->u.node_call_1.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_1.name, 1);
    VALUE args[1] = { v0 };
    return sp_dispatch_fresh_frame(c, body, args, 1);
}

RESULT
node_call_2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_2)\n", n->u.node_call_2.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_call_2.a0, fp));
    VALUE v1 = UNWRAP(EVAL(c, n->u.node_call_2.a1, fp));
    struct callcache *cc = &n->u.node_call_2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_2.name, 2);
    VALUE args[2] = { v0, v1 };
    return sp_dispatch_fresh_frame(c, body, args, 2);
}

RESULT
node_call_3_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_3)\n", n->u.node_call_3.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_call_3.a0, fp));
    VALUE v1 = UNWRAP(EVAL(c, n->u.node_call_3.a1, fp));
    VALUE v2 = UNWRAP(EVAL(c, n->u.node_call_3.a2, fp));
    struct callcache *cc = &n->u.node_call_3.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_3.name, 3);
    VALUE args[3] = { v0, v1, v2 };
    return sp_dispatch_fresh_frame(c, body, args, 3);
}

// ---------- node_call2 (argc > 3, PG fallback) ----------

RESULT
node_call2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call2)\n", n->u.node_call2.name);
    struct callcache *cc = &n->u.node_call2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call2.name, n->u.node_call2.params_cnt);
    // sp_body is intentionally not used here — slowpath always
    // dispatches via cc->body (the runtime-resolved body) so the
    // baked SD's stale speculation can't propagate.
    return sp_dispatch_via_fp(c, body, fp, n->u.node_call2.arg_index);
}

// ---------- node_pg_call_<N> (PG, arity-N specialized) ----------

RESULT
node_pg_call0_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call0)\n", n->u.node_pg_call0.name);
    (void)fp;
    struct callcache *cc = &n->u.node_pg_call0.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call0.name, 0);
    return sp_dispatch_fresh_frame(c, body, NULL, 0);
}

RESULT
node_pg_call1_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call1)\n", n->u.node_pg_call1.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_pg_call1.a0, fp));
    struct callcache *cc = &n->u.node_pg_call1.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call1.name, 1);
    VALUE args[1] = { v0 };
    return sp_dispatch_fresh_frame(c, body, args, 1);
}

RESULT
node_pg_call2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call2)\n", n->u.node_pg_call2.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_pg_call2.a0, fp));
    VALUE v1 = UNWRAP(EVAL(c, n->u.node_pg_call2.a1, fp));
    struct callcache *cc = &n->u.node_pg_call2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call2.name, 2);
    VALUE args[2] = { v0, v1 };
    return sp_dispatch_fresh_frame(c, body, args, 2);
}

RESULT
node_pg_call3_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call3)\n", n->u.node_pg_call3.name);
    VALUE v0 = UNWRAP(EVAL(c, n->u.node_pg_call3.a0, fp));
    VALUE v1 = UNWRAP(EVAL(c, n->u.node_pg_call3.a1, fp));
    VALUE v2 = UNWRAP(EVAL(c, n->u.node_pg_call3.a2, fp));
    struct callcache *cc = &n->u.node_pg_call3.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call3.name, 3);
    VALUE args[3] = { v0, v1, v2 };
    return sp_dispatch_fresh_frame(c, body, args, 3);
}
