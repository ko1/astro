// baruby_precise slowpaths: iter 61 — fp eliminated, dispatchers take
// (c, n, sp).  Callee frames still live on the shared VALUE stack so
// precise GC can scan them.  For node_call / node_call2 (= variadic
// fallback path), the callee_fp baked by the walker as
// `callee_fp_offset` operand resolves to sp + offset at runtime.

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

// Dispatch via a walker-baked callee_fp_offset (node_call / node_call2 path).
// Callee body's frame sits at sp[callee_fp_offset..]; its sp top is
// callee_fp + locals_cnt.
static inline RESULT
sp_dispatch_via_callee_fp_offset(CTX * restrict c, NODE * restrict body,
                                 VALUE * restrict sp,
                                 int32_t callee_fp_offset)
{
    uint32_t lc = code_repo_find_locals_cnt_by_body(body);
    if (lc == 0) lc = 1;
    VALUE *callee_fp = sp + callee_fp_offset;
    RESULT r = EVAL(c, body, callee_fp + lc);
    return RESULT_OK(r.value);
}

// Dispatch with a fresh callee frame at sp[0..lc-1] (node_call_<N> /
// node_pg_call_<N> path).  Args are placed at sp[0..argc-1].
static inline RESULT
sp_dispatch_fresh_frame(CTX * restrict c, NODE * restrict body,
                        const VALUE *args, uint32_t argc,
                        VALUE * restrict sp)
{
    uint32_t lc = code_repo_find_locals_cnt_by_body(body);
    if (lc < argc) lc = argc;
    if (lc == 0) lc = 1;
    for (uint32_t i = 0; i < lc; i++) sp[i] = 0;
    for (uint32_t i = 0; i < argc; i++) sp[i] = args[i];
    RESULT r = EVAL(c, body, sp + lc);
    return RESULT_OK(r.value);
}

// ---------- node_call (argc > 3, non-PG fallback) ----------

RESULT
node_call_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call)\n", n->u.node_call.name);
    struct callcache *cc = &n->u.node_call.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call.name, n->u.node_call.params_cnt);
    return sp_dispatch_via_callee_fp_offset(c, body, sp, n->u.node_call.callee_fp_offset);
}

// ---------- node_call_<N> (non-PG, arity-N specialized) ----------

RESULT
node_call_0_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_0)\n", n->u.node_call_0.name);
    struct callcache *cc = &n->u.node_call_0.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_0.name, 0);
    /* iter 71: 0 args、 sp 不変 (slot_count=0)。 fresh_frame は sp 基底で動作。 */
    return sp_dispatch_fresh_frame(c, body, NULL, 0, sp);
}

RESULT
node_call_1_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_1)\n", n->u.node_call_1.name);
    /* iter 71: @child により framework が a0 を sp[-1] に spill 済。
     * fresh_frame には sp - 1 (= caller の sp = callee_fp) を渡す。 */
    VALUE v0 = sp[-1];
    struct callcache *cc = &n->u.node_call_1.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_1.name, 1);
    VALUE args[1] = { v0 };
    return sp_dispatch_fresh_frame(c, body, args, 1, sp - 1);
}

RESULT
node_call_2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_2)\n", n->u.node_call_2.name);
    VALUE v0 = sp[-2];
    VALUE v1 = sp[-1];
    struct callcache *cc = &n->u.node_call_2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_2.name, 2);
    VALUE args[2] = { v0, v1 };
    return sp_dispatch_fresh_frame(c, body, args, 2, sp - 2);
}

RESULT
node_call_3_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call_3)\n", n->u.node_call_3.name);
    VALUE v0 = sp[-3];
    VALUE v1 = sp[-2];
    VALUE v2 = sp[-1];
    struct callcache *cc = &n->u.node_call_3.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call_3.name, 3);
    VALUE args[3] = { v0, v1, v2 };
    return sp_dispatch_fresh_frame(c, body, args, 3, sp - 3);
}

// ---------- node_call2 (argc > 3, PG fallback) ----------

RESULT
node_call2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (call2)\n", n->u.node_call2.name);
    struct callcache *cc = &n->u.node_call2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_call2.name, n->u.node_call2.params_cnt);
    return sp_dispatch_via_callee_fp_offset(c, body, sp, n->u.node_call2.callee_fp_offset);
}

// ---------- node_pg_call_<N> (PG, arity-N specialized) ----------

RESULT
node_pg_call0_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call0)\n", n->u.node_pg_call0.name);
    struct callcache *cc = &n->u.node_pg_call0.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call0.name, 0);
    return sp_dispatch_fresh_frame(c, body, NULL, 0, sp);
}

RESULT
node_pg_call1_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call1)\n", n->u.node_pg_call1.name);
    VALUE v0 = sp[-1];
    struct callcache *cc = &n->u.node_pg_call1.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call1.name, 1);
    VALUE args[1] = { v0 };
    return sp_dispatch_fresh_frame(c, body, args, 1, sp - 1);
}

RESULT
node_pg_call2_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call2)\n", n->u.node_pg_call2.name);
    VALUE v0 = sp[-2];
    VALUE v1 = sp[-1];
    struct callcache *cc = &n->u.node_pg_call2.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call2.name, 2);
    VALUE args[2] = { v0, v1 };
    return sp_dispatch_fresh_frame(c, body, args, 2, sp - 2);
}

RESULT
node_pg_call3_slowpath(CTX * restrict c, NODE * restrict n, VALUE * restrict sp)
{
    if (CALL_DEBUG) fprintf(stderr, "name:%s miss (pg_call3)\n", n->u.node_pg_call3.name);
    VALUE v0 = sp[-3];
    VALUE v1 = sp[-2];
    VALUE v2 = sp[-1];
    struct callcache *cc = &n->u.node_pg_call3.cc;
    NODE *body = sp_refresh_cc(c, n, cc, n->u.node_pg_call3.name, 3);
    VALUE args[3] = { v0, v1, v2 };
    return sp_dispatch_fresh_frame(c, body, args, 3, sp - 3);
}
