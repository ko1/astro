/*
 * runtime.c — semantic helpers for node.def evaluators.
 *
 * Tree evaluator: every filter reads c->input and pushes its outputs
 * into c->emit_buf via nuq_emit().  Sub-evaluations (pipe stages, the
 * inside of `[ ... ]`, expressions for `.[expr]`, etc.) save the
 * current emit_buf, point it at a fresh array, run the inner, then
 * iterate the captured array.
 *
 * Return code (= VALUE in dispatcher signature) is BR_OK / BR_BREAK /
 * BR_ERROR.  Real values flow through emit_buf, never via return.
 */
#include "context.h"
#include "node.h"

void
nuq_emit(CTX *c, VALUE v)
{
    nuq_array_push(c->emit_buf, v);
}

/* Evaluate `body` with `input`, capturing emits into a fresh array
 * which is returned via *out.  Caller-supplied *out is set unless
 * status is BR_ERROR/BR_BREAK. */
VALUE
nuq_eval_collect_status(CTX *c, struct Node *body, VALUE input, VALUE *out)
{
    VALUE saved_in = c->input;
    VALUE saved_buf = c->emit_buf;
    VALUE buf = nuq_make_array(0);
    c->emit_buf = buf;
    c->input = input;
    VALUE r = EVAL(c, body);
    c->input = saved_in;
    c->emit_buf = saved_buf;
    *out = buf;
    return r;
}

VALUE
nuq_eval_collect(CTX *c, struct Node *body, VALUE input)
{
    VALUE out;
    nuq_eval_collect_status(c, body, input, &out);
    return out;
}

/* --- side tables --------------------------------------------------------- */

static VALUE *lit_tab = NULL; static size_t lit_len = 0, lit_capa = 0;

uint32_t
nuq_lit_intern(VALUE v)
{
    if (lit_len == lit_capa) {
        lit_capa = lit_capa ? lit_capa * 2 : 16;
        lit_tab = (VALUE *)GC_realloc(lit_tab, lit_capa * sizeof(VALUE));
    }
    lit_tab[lit_len] = v;
    return (uint32_t)lit_len++;
}

VALUE nuq_lit_get(uint32_t id) { return lit_tab[id]; }

struct interp_entry { size_t cnt; struct Node **parts; };
static struct interp_entry *interp_tab = NULL;
static size_t interp_len = 0, interp_capa = 0;

uint32_t
nuq_interp_intern(struct Node **parts, size_t cnt)
{
    if (interp_len == interp_capa) {
        interp_capa = interp_capa ? interp_capa * 2 : 16;
        interp_tab = (struct interp_entry *)GC_realloc(interp_tab, interp_capa * sizeof(*interp_tab));
    }
    interp_tab[interp_len].cnt = cnt;
    interp_tab[interp_len].parts = parts;
    return (uint32_t)interp_len++;
}

struct obj_ctor { size_t cnt; struct nuq_obj_entry *items; };
static struct obj_ctor *obj_tab = NULL;
static size_t obj_tab_len = 0, obj_tab_capa = 0;

uint32_t
nuq_obj_ctor_intern(struct nuq_obj_entry *items, size_t cnt)
{
    if (obj_tab_len == obj_tab_capa) {
        obj_tab_capa = obj_tab_capa ? obj_tab_capa * 2 : 16;
        obj_tab = (struct obj_ctor *)GC_realloc(obj_tab, obj_tab_capa * sizeof(*obj_tab));
    }
    obj_tab[obj_tab_len].cnt = cnt;
    obj_tab[obj_tab_len].items = items;
    return (uint32_t)obj_tab_len++;
}

struct args_entry { size_t cnt; struct Node **args; };
static struct args_entry *args_tab = NULL;
static size_t args_tab_len = 0, args_tab_capa = 0;

uint32_t
nuq_args_intern(struct Node **args, size_t cnt)
{
    if (args_tab_len == args_tab_capa) {
        args_tab_capa = args_tab_capa ? args_tab_capa * 2 : 16;
        args_tab = (struct args_entry *)GC_realloc(args_tab, args_tab_capa * sizeof(*args_tab));
    }
    args_tab[args_tab_len].cnt = cnt;
    args_tab[args_tab_len].args = args;
    return (uint32_t)args_tab_len++;
}

struct def_block { size_t cnt; struct nuq_def_entry *items; };
static struct def_block *def_tab = NULL;
static size_t def_tab_len = 0, def_tab_capa = 0;

uint32_t
nuq_def_block_intern(struct nuq_def_entry *items, size_t cnt)
{
    if (def_tab_len == def_tab_capa) {
        def_tab_capa = def_tab_capa ? def_tab_capa * 2 : 16;
        def_tab = (struct def_block *)GC_realloc(def_tab, def_tab_capa * sizeof(*def_tab));
    }
    def_tab[def_tab_len].cnt = cnt;
    def_tab[def_tab_len].items = items;
    return (uint32_t)def_tab_len++;
}

/* fmt id intern — for @csv, @tsv, etc. */
static const char **fmt_tab = NULL;
static size_t fmt_tab_len = 0, fmt_tab_capa = 0;

uint32_t
nuq_fmt_intern(const char *name)
{
    for (size_t i = 0; i < fmt_tab_len; i++) {
        if (strcmp(fmt_tab[i], name) == 0) return (uint32_t)i;
    }
    if (fmt_tab_len == fmt_tab_capa) {
        fmt_tab_capa = fmt_tab_capa ? fmt_tab_capa * 2 : 8;
        fmt_tab = (const char **)GC_realloc(fmt_tab, fmt_tab_capa * sizeof(*fmt_tab));
    }
    char *dup = (char *)GC_malloc_atomic(strlen(name) + 1);
    strcpy(dup, name);
    fmt_tab[fmt_tab_len] = dup;
    return (uint32_t)fmt_tab_len++;
}

const char *nuq_fmt_lookup(uint32_t id) { return fmt_tab[id]; }

/* --- field access ------------------------------------------------------- */

bool
nuq_field_lookup(VALUE in, const char *name, bool optional, VALUE *out)
{
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        *out = nuq_object_get_cstr(in, name);
        return true;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL) {
        *out = NUQ_NULL;
        return true;
    }
    if (optional) return false;
    fprintf(stderr, "nuq error: cannot index %s with \"%s\"\n", nuq_type_name(in), name);
    return false;
}

/* --- pipe --------------------------------------------------------------- */

VALUE
nuq_pipe_eval(CTX *c, struct Node *lhs, struct Node *rhs)
{
    VALUE out;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &out);
    if (r != BR_OK) return r;
    struct nuq_obj *ao = NUQ_PTR(out);
    VALUE saved = c->input;
    for (size_t i = 0; i < ao->arr.len; i++) {
        c->input = ao->arr.items[i];
        r = EVAL(c, rhs);
        if (r != BR_OK) { c->input = saved; return r; }
    }
    c->input = saved;
    return BR_OK;
}

/* --- iter / recurse ----------------------------------------------------- */

VALUE
nuq_iter_emit(CTX *c, VALUE in, bool optional)
{
    if (NUQ_IS_PTR(in)) {
        struct nuq_obj *o = NUQ_PTR(in);
        if (o->type == NUQ_T_ARRAY) {
            for (size_t i = 0; i < o->arr.len; i++) nuq_emit(c, o->arr.items[i]);
            return BR_OK;
        }
        if (o->type == NUQ_T_OBJECT) {
            for (size_t i = 0; i < o->obj.len; i++) nuq_emit(c, o->obj.vals[i]);
            return BR_OK;
        }
    }
    if (optional) return BR_OK;
    fprintf(stderr, "nuq error: Cannot iterate over %s\n", nuq_type_name(in));
    c->error = nuq_make_string("Cannot iterate", 14);
    return BR_ERROR;
}

VALUE
nuq_recurse_emit(CTX *c, VALUE v)
{
    nuq_emit(c, v);
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            for (size_t i = 0; i < o->arr.len; i++) {
                VALUE r = nuq_recurse_emit(c, o->arr.items[i]);
                if (r != BR_OK) return r;
            }
        } else if (o->type == NUQ_T_OBJECT) {
            for (size_t i = 0; i < o->obj.len; i++) {
                VALUE r = nuq_recurse_emit(c, o->obj.vals[i]);
                if (r != BR_OK) return r;
            }
        }
    }
    return BR_OK;
}

/* --- index `.[expr]` ---------------------------------------------------- */

static int64_t
to_int64(VALUE v)
{
    if (NUQ_IS_FIX(v)) return NUQ_FIX_VAL(v);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) return (int64_t)NUQ_PTR(v)->dbl;
    return 0;
}

static VALUE
do_index(CTX *c, VALUE container, VALUE key, bool optional, bool *errored)
{
    *errored = false;
    if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_NULL) return NUQ_NULL;
    if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_OBJECT) {
        if (NUQ_IS_PTR(key) && NUQ_PTR(key)->type == NUQ_T_STRING)
            return nuq_object_get(container, key);
        if (optional) { *errored = true; return NUQ_NULL; }
        fprintf(stderr, "nuq error: object can only be indexed by string\n");
        c->error = nuq_make_string("type error", 10);
        *errored = true;
        return NUQ_NULL;
    }
    if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_ARRAY) {
        if (NUQ_IS_FIX(key) || (NUQ_IS_PTR(key) && NUQ_PTR(key)->type == NUQ_T_DOUBLE))
            return nuq_array_get(container, to_int64(key));
        if (optional) { *errored = true; return NUQ_NULL; }
        fprintf(stderr, "nuq error: array can only be indexed by number\n");
        c->error = nuq_make_string("type error", 10);
        *errored = true;
        return NUQ_NULL;
    }
    if (optional) { *errored = true; return NUQ_NULL; }
    fprintf(stderr, "nuq error: cannot index %s\n", nuq_type_name(container));
    c->error = nuq_make_string("type error", 10);
    *errored = true;
    return NUQ_NULL;
}

VALUE
nuq_index_eval(CTX *c, struct Node *expr, bool optional)
{
    VALUE container = c->input;
    VALUE keys;
    VALUE r = nuq_eval_collect_status(c, expr, container, &keys);
    if (r != BR_OK) return r;
    struct nuq_obj *ao = NUQ_PTR(keys);
    for (size_t i = 0; i < ao->arr.len; i++) {
        bool err = false;
        VALUE got = do_index(c, container, ao->arr.items[i], optional, &err);
        if (err && !optional) return BR_ERROR;
        if (!err) nuq_emit(c, got);
    }
    return BR_OK;
}

/* --- slice -------------------------------------------------------------- */

VALUE
nuq_slice_eval(CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional)
{
    VALUE in = c->input;
    int64_t length;
    bool is_str = false;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL) {
        nuq_emit(c, NUQ_NULL);
        return BR_OK;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) length = (int64_t)NUQ_PTR(in)->arr.len;
    else if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_STRING) { length = (int64_t)NUQ_PTR(in)->str.len; is_str = true; }
    else {
        if (optional) return BR_OK;
        fprintf(stderr, "nuq error: cannot slice %s\n", nuq_type_name(in));
        c->error = nuq_make_string("type error", 10);
        return BR_ERROR;
    }
    int64_t start = 0, stop = length;
    if (flags & SLICE_HAS_START) {
        VALUE buf;
        VALUE r = nuq_eval_collect_status(c, startn, in, &buf);
        if (r != BR_OK) return r;
        if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
        VALUE v = NUQ_PTR(buf)->arr.items[0];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) start = 0;
        else start = to_int64(v);
    }
    if (flags & SLICE_HAS_STOP) {
        VALUE buf;
        VALUE r = nuq_eval_collect_status(c, stopn, in, &buf);
        if (r != BR_OK) return r;
        if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
        VALUE v = NUQ_PTR(buf)->arr.items[0];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) stop = length;
        else stop = to_int64(v);
    }
    if (start < 0) start += length;
    if (stop  < 0) stop  += length;
    if (start < 0) start = 0;
    if (stop  > length) stop = length;
    if (stop  < start) stop = start;

    if (is_str) {
        struct nuq_obj *o = NUQ_PTR(in);
        nuq_emit(c, nuq_make_string(o->str.bytes + start, (size_t)(stop - start)));
    } else {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE r = nuq_make_array((size_t)(stop - start));
        for (int64_t i = start; i < stop; i++) nuq_array_push(r, o->arr.items[i]);
        nuq_emit(c, r);
    }
    return BR_OK;
}

/* --- arithmetic / comparison ------------------------------------------- */

static VALUE
apply_binop(int op, VALUE a, VALUE b)
{
    switch (op) {
      case NUQ_OP_ADD_K: return nuq_op_add(a, b);
      case NUQ_OP_SUB_K: return nuq_op_sub(a, b);
      case NUQ_OP_MUL_K: return nuq_op_mul(a, b);
      case NUQ_OP_DIV_K: return nuq_op_div(a, b);
      case NUQ_OP_MOD_K: return nuq_op_mod(a, b);
    }
    return NUQ_NULL;
}

VALUE
nuq_binop_eval(CTX *c, struct Node *lhs, struct Node *rhs, int op)
{
    VALUE las, ras;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &las);
    if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, rhs, c->input, &ras);
    if (r != BR_OK) return r;
    struct nuq_obj *la = NUQ_PTR(las);
    struct nuq_obj *rb = NUQ_PTR(ras);
    for (size_t i = 0; i < la->arr.len; i++) {
        for (size_t j = 0; j < rb->arr.len; j++) {
            nuq_emit(c, apply_binop(op, la->arr.items[i], rb->arr.items[j]));
        }
    }
    return BR_OK;
}

VALUE
nuq_neg_eval(CTX *c, struct Node *expr)
{
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, expr, c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *ao = NUQ_PTR(buf);
    for (size_t i = 0; i < ao->arr.len; i++) nuq_emit(c, nuq_op_neg(ao->arr.items[i]));
    return BR_OK;
}

VALUE
nuq_cmpop_eval(CTX *c, struct Node *lhs, struct Node *rhs, int op)
{
    VALUE las, ras;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &las);
    if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, rhs, c->input, &ras);
    if (r != BR_OK) return r;
    struct nuq_obj *la = NUQ_PTR(las);
    struct nuq_obj *rb = NUQ_PTR(ras);
    for (size_t i = 0; i < la->arr.len; i++) {
        for (size_t j = 0; j < rb->arr.len; j++) {
            VALUE a = la->arr.items[i], b = rb->arr.items[j];
            bool t;
            switch (op) {
              case NUQ_CMP_EQ_K: t = nuq_eq(a, b); break;
              case NUQ_CMP_NE_K: t = !nuq_eq(a, b); break;
              case NUQ_CMP_LT_K: t = nuq_cmp(a, b) <  0; break;
              case NUQ_CMP_LE_K: t = nuq_cmp(a, b) <= 0; break;
              case NUQ_CMP_GT_K: t = nuq_cmp(a, b) >  0; break;
              case NUQ_CMP_GE_K: t = nuq_cmp(a, b) >= 0; break;
              default: t = false;
            }
            nuq_emit(c, t ? NUQ_TRUE : NUQ_FALSE);
        }
    }
    return BR_OK;
}

VALUE
nuq_andor_eval(CTX *c, struct Node *lhs, struct Node *rhs, bool is_and)
{
    /* short-circuit per LHS value: emit FALSE/TRUE for short-circuit
     * cases without evaluating RHS */
    VALUE las;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &las);
    if (r != BR_OK) return r;
    struct nuq_obj *la = NUQ_PTR(las);
    for (size_t i = 0; i < la->arr.len; i++) {
        bool ta = nuq_truthy(la->arr.items[i]);
        if ((is_and && !ta) || (!is_and && ta)) {
            nuq_emit(c, is_and ? NUQ_FALSE : NUQ_TRUE);
            continue;
        }
        VALUE ras;
        r = nuq_eval_collect_status(c, rhs, c->input, &ras);
        if (r != BR_OK) return r;
        struct nuq_obj *rb = NUQ_PTR(ras);
        for (size_t j = 0; j < rb->arr.len; j++) {
            nuq_emit(c, nuq_truthy(rb->arr.items[j]) ? NUQ_TRUE : NUQ_FALSE);
        }
    }
    return BR_OK;
}

VALUE
nuq_alt_eval(CTX *c, struct Node *lhs, struct Node *rhs)
{
    /* `a // b`: emit truthy outputs of a; if a has no truthy, emit b's */
    VALUE las;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &las);
    if (r == BR_ERROR) {
        c->error = NUQ_NULL;
        return EVAL(c, rhs);     /* rhs in current emit_buf */
    }
    if (r != BR_OK) return r;
    struct nuq_obj *la = NUQ_PTR(las);
    bool any = false;
    for (size_t i = 0; i < la->arr.len; i++) {
        if (nuq_truthy(la->arr.items[i])) {
            any = true;
            nuq_emit(c, la->arr.items[i]);
        }
    }
    if (!any) return EVAL(c, rhs);
    return BR_OK;
}

/* --- array/object construction ----------------------------------------- */

VALUE
nuq_array_eval(CTX *c, struct Node *body)
{
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, body, c->input, &buf);
    if (r != BR_OK) return r;
    nuq_emit(c, buf);
    return BR_OK;
}

VALUE
nuq_object_eval(CTX *c, uint32_t entries_id)
{
    struct obj_ctor *e = &obj_tab[entries_id];

    /* For each entry, compute key-stream and value-stream. */
    if (e->cnt > 32) { fprintf(stderr, "nuq error: object literal too large\n"); return BR_ERROR; }
    VALUE kstreams[32], vstreams[32];

    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *ie = &e->items[i];
        VALUE ks = nuq_make_array(0);
        if (ie->kkind == 0) {
            nuq_array_push(ks, nuq_make_string(ie->kname, strlen(ie->kname)));
        } else if (ie->kkind == 2) {
            const char *nm = nuq_intern_lookup(ie->var_id);
            nuq_array_push(ks, nuq_make_string(nm, strlen(nm)));
        } else {
            VALUE r = nuq_eval_collect_status(c, ie->kexpr, c->input, &ks);
            if (r != BR_OK) return r;
        }
        kstreams[i] = ks;

        VALUE vs;
        if (ie->vexpr == NULL) {
            vs = nuq_make_array(0);
            VALUE val;
            if (ie->kkind == 2) val = nuq_var_get(c, ie->var_id);
            else val = nuq_object_get_cstr(c->input, ie->kname);
            nuq_array_push(vs, val);
        } else {
            VALUE r = nuq_eval_collect_status(c, ie->vexpr, c->input, &vs);
            if (r != BR_OK) return r;
        }
        vstreams[i] = vs;
    }

    /* If any stream is empty, no emit. */
    for (size_t i = 0; i < e->cnt; i++)
        if (NUQ_PTR(kstreams[i])->arr.len == 0 || NUQ_PTR(vstreams[i])->arr.len == 0)
            return BR_OK;

    size_t kidx[32] = {0}, vidx[32] = {0};
    for (;;) {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE k = NUQ_PTR(kstreams[i])->arr.items[kidx[i]];
            VALUE v = NUQ_PTR(vstreams[i])->arr.items[vidx[i]];
            if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)) {
                fprintf(stderr, "nuq error: object key must be string, got %s\n", nuq_type_name(k));
                c->error = nuq_make_string("type error", 10);
                return BR_ERROR;
            }
            nuq_object_set(obj, k, v);
        }
        nuq_emit(c, obj);

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            vidx[pos]++;
            if (vidx[pos] < NUQ_PTR(vstreams[pos])->arr.len) break;
            vidx[pos] = 0;
            kidx[pos]++;
            if (kidx[pos] < NUQ_PTR(kstreams[pos])->arr.len) break;
            kidx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return BR_OK;
}

/* --- if / try / as / error -------------------------------------------- */

VALUE
nuq_if_eval(CTX *c, struct Node *cond, struct Node *thn, struct Node *els)
{
    VALUE cs;
    VALUE r = nuq_eval_collect_status(c, cond, c->input, &cs);
    if (r != BR_OK) return r;
    struct nuq_obj *co = NUQ_PTR(cs);
    for (size_t i = 0; i < co->arr.len; i++) {
        struct Node *branch = nuq_truthy(co->arr.items[i]) ? thn : els;
        if (branch == NULL) {
            /* no else — pass `.` through */
            nuq_emit(c, c->input);
        } else {
            r = EVAL(c, branch);
            if (r != BR_OK) return r;
        }
    }
    return BR_OK;
}

VALUE
nuq_try_eval(CTX *c, struct Node *body, struct Node *handler)
{
    /*
     * Stream body's emits directly into our caller's buffer.  When
     * body returns BR_ERROR, run the handler with the error as input
     * (its emits also go to the caller's buffer).  This way
     * pre-error successful emits aren't lost.
     */
    VALUE saved_error = c->error;
    VALUE r = EVAL(c, body);
    if (r == BR_ERROR) {
        VALUE err = c->error;
        c->error = saved_error;
        if (handler) {
            VALUE saved = c->input;
            c->input = err;
            r = EVAL(c, handler);
            c->input = saved;
            return r;
        }
        return BR_OK;       /* swallow without handler */
    }
    return r;
}

VALUE
nuq_as_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *body)
{
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, src, c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, bo->arr.items[i]);
        r = EVAL(c, body);
        nuq_var_pop(c, top);
        if (r != BR_OK) return r;
    }
    return BR_OK;
}

VALUE
nuq_error_eval(CTX *c, struct Node *expr)
{
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, expr, c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    if (bo->arr.len == 0) c->error = NUQ_NULL;
    else c->error = bo->arr.items[0];
    return BR_ERROR;
}

/* --- function calls --------------------------------------------------- */

VALUE
nuq_call_eval(CTX *c, uint32_t name_id, int arity, struct Node **args)
{
    /* user `def`s shadow builtins */
    struct nuq_func_def *fd = nuq_func_lookup(c, name_id, arity);
    if (fd) {
        size_t var_top = c->var_top;
        size_t func_top = c->func_cnt;
        for (int i = 0; i < arity; i++) {
            if (fd->param_is_value[i]) {
                VALUE buf;
                VALUE r = nuq_eval_collect_status(c, args[i], c->input, &buf);
                if (r != BR_OK) return r;
                VALUE v = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
                nuq_var_push(c, fd->param_ids[i], v);
            } else {
                struct nuq_func_def *pfd = (struct nuq_func_def *)GC_malloc(sizeof(*pfd));
                pfd->name_id = fd->param_ids[i];
                pfd->arity = 0;
                pfd->param_ids = NULL;
                pfd->param_is_value = NULL;
                pfd->body = args[i];
                nuq_func_define(c, pfd);
            }
        }
        VALUE r = EVAL(c, fd->body);
        nuq_var_pop(c, var_top);
        c->func_cnt = func_top;
        return r;
    }

    VALUE st = BR_OK;
    if (nuq_builtin_call(c, name_id, arity, args, &st)) return st;

    fprintf(stderr, "nuq error: %s/%d is not defined\n", nuq_intern_lookup(name_id), arity);
    c->error = nuq_make_string("undefined", 9);
    return BR_ERROR;
}

VALUE nuq_call_eval1(CTX *c, uint32_t n, struct Node *a) { struct Node *aa[1] = {a}; return nuq_call_eval(c, n, 1, aa); }
VALUE nuq_call_eval2(CTX *c, uint32_t n, struct Node *a, struct Node *b) { struct Node *aa[2] = {a,b}; return nuq_call_eval(c, n, 2, aa); }
VALUE nuq_call_eval3(CTX *c, uint32_t n, struct Node *a, struct Node *b, struct Node *d) { struct Node *aa[3] = {a,b,d}; return nuq_call_eval(c, n, 3, aa); }

VALUE
nuq_defs_eval(CTX *c, uint32_t defs_id, struct Node *body)
{
    struct def_block *db = &def_tab[defs_id];
    size_t saved = c->func_cnt;
    for (size_t i = 0; i < db->cnt; i++) {
        struct nuq_def_entry *de = &db->items[i];
        struct nuq_func_def *fd = (struct nuq_func_def *)GC_malloc(sizeof(*fd));
        fd->name_id = de->name_id;
        fd->arity = de->arity;
        fd->param_ids = de->param_ids;
        fd->param_is_value = de->param_is_value;
        fd->body = de->body;
        nuq_func_define(c, fd);
    }
    VALUE r = EVAL(c, body);
    c->func_cnt = saved;
    return r;
}

/* --- reduce / foreach -------------------------------------------------- */

VALUE
nuq_reduce_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update)
{
    VALUE acc_buf;
    VALUE r = nuq_eval_collect_status(c, init, c->input, &acc_buf);
    if (r != BR_OK) return r;
    VALUE acc = NUQ_PTR(acc_buf)->arr.len > 0 ? NUQ_PTR(acc_buf)->arr.items[0] : NUQ_NULL;

    VALUE src_buf;
    r = nuq_eval_collect_status(c, src, c->input, &src_buf);
    if (r != BR_OK) return r;
    struct nuq_obj *so = NUQ_PTR(src_buf);
    for (size_t i = 0; i < so->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, so->arr.items[i]);
        VALUE up_buf;
        r = nuq_eval_collect_status(c, update, acc, &up_buf);
        nuq_var_pop(c, top);
        if (r != BR_OK) return r;
        if (NUQ_PTR(up_buf)->arr.len > 0) acc = NUQ_PTR(up_buf)->arr.items[NUQ_PTR(up_buf)->arr.len - 1];
    }
    nuq_emit(c, acc);
    return BR_OK;
}

VALUE
nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract)
{
    VALUE acc_buf;
    VALUE r = nuq_eval_collect_status(c, init, c->input, &acc_buf);
    if (r != BR_OK) return r;
    VALUE acc = NUQ_PTR(acc_buf)->arr.len > 0 ? NUQ_PTR(acc_buf)->arr.items[0] : NUQ_NULL;

    VALUE src_buf;
    r = nuq_eval_collect_status(c, src, c->input, &src_buf);
    if (r != BR_OK) return r;
    struct nuq_obj *so = NUQ_PTR(src_buf);
    for (size_t i = 0; i < so->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, so->arr.items[i]);
        VALUE up_buf;
        r = nuq_eval_collect_status(c, update, acc, &up_buf);
        if (r != BR_OK) { nuq_var_pop(c, top); return r; }
        struct nuq_obj *uo = NUQ_PTR(up_buf);
        for (size_t j = 0; j < uo->arr.len; j++) {
            acc = uo->arr.items[j];
            if (extract) {
                VALUE ex_buf;
                r = nuq_eval_collect_status(c, extract, acc, &ex_buf);
                if (r != BR_OK) { nuq_var_pop(c, top); return r; }
                struct nuq_obj *eo = NUQ_PTR(ex_buf);
                for (size_t k = 0; k < eo->arr.len; k++) nuq_emit(c, eo->arr.items[k]);
            } else {
                nuq_emit(c, acc);
            }
        }
        nuq_var_pop(c, top);
    }
    return BR_OK;
}

/* --- string interp ----------------------------------------------------- */

/*
 * Convert v to a string for use in string interpolation / @text.
 * Strings stay as-is (no JSON quoting).  Everything else is encoded
 * as JSON.
 */
VALUE
nuq_to_json_string(VALUE v)
{
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) return v;
    char *buf = NULL;
    size_t bn = 0;
    FILE *fp = open_memstream(&buf, &bn);
    nuq_json_print(fp, v, 0);
    fclose(fp);
    VALUE r = nuq_make_string(buf, bn);
    free(buf);
    return r;
}

/* Like nuq_to_json_string, but for `tostring` (jq) which has the same
 * semantics: strings raw, others JSON.  We keep one helper. */

VALUE
nuq_interp_eval(CTX *c, uint32_t parts_id)
{
    struct interp_entry *e = &interp_tab[parts_id];
    if (e->cnt > 32) { fprintf(stderr, "nuq error: interp too long\n"); return BR_ERROR; }

    /* For each part, collect its emit-stream of stringified values. */
    VALUE streams[32];
    for (size_t i = 0; i < e->cnt; i++) {
        VALUE buf;
        VALUE r = nuq_eval_collect_status(c, e->parts[i], c->input, &buf);
        if (r != BR_OK) return r;
        VALUE strs = nuq_make_array(NUQ_PTR(buf)->arr.len);
        struct nuq_obj *bo = NUQ_PTR(buf);
        for (size_t j = 0; j < bo->arr.len; j++) {
            nuq_array_push(strs, nuq_to_json_string(bo->arr.items[j]));
        }
        streams[i] = strs;
        if (NUQ_PTR(strs)->arr.len == 0) return BR_OK;
    }

    size_t idx[32] = {0};
    for (;;) {
        size_t total = 0;
        VALUE strs[32];
        size_t lens[32];
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE s = NUQ_PTR(streams[i])->arr.items[idx[i]];
            strs[i] = s;
            lens[i] = NUQ_PTR(s)->str.len;
            total += lens[i];
        }
        char *buf = (char *)GC_malloc_atomic(total + 1);
        char *p = buf;
        for (size_t i = 0; i < e->cnt; i++) {
            memcpy(p, NUQ_PTR(strs[i])->str.bytes, lens[i]);
            p += lens[i];
        }
        buf[total] = '\0';
        nuq_emit(c, nuq_make_string_take(buf, total));

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            idx[pos]++;
            if (idx[pos] < NUQ_PTR(streams[pos])->arr.len) break;
            idx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return BR_OK;
}

/* @csv etc: encode a single VALUE as a string per the format.  Body
 * is the value-producing filter; we apply the format to each emit. */
static VALUE
fmt_apply(uint32_t fmt_id, VALUE v)
{
    const char *fmt = nuq_fmt_lookup(fmt_id);
    if (strcmp(fmt, "text") == 0) {
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) return v;
        return nuq_to_json_string(v);
    }
    if (strcmp(fmt, "json") == 0) {
        return nuq_to_json_string(v);
    }
    if (strcmp(fmt, "csv") == 0 || strcmp(fmt, "tsv") == 0) {
        bool tsv = (fmt[0] == 't');
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY)) {
            fprintf(stderr, "nuq error: @%s requires an array\n", fmt);
            return nuq_make_string("", 0);
        }
        struct nuq_obj *ao = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        for (size_t i = 0; i < ao->arr.len; i++) {
            if (i) fputc(tsv ? '\t' : ',', fp);
            VALUE x = ao->arr.items[i];
            if (NUQ_IS_PTR(x) && NUQ_PTR(x)->type == NUQ_T_STRING) {
                struct nuq_obj *so = NUQ_PTR(x);
                if (tsv) {
                    for (size_t k = 0; k < so->str.len; k++) {
                        char c = so->str.bytes[k];
                        if (c == '\t') fputs("\\t", fp);
                        else if (c == '\r') fputs("\\r", fp);
                        else if (c == '\n') fputs("\\n", fp);
                        else if (c == '\\') fputs("\\\\", fp);
                        else fputc(c, fp);
                    }
                } else {
                    fputc('"', fp);
                    for (size_t k = 0; k < so->str.len; k++) {
                        if (so->str.bytes[k] == '"') fputc('"', fp);
                        fputc(so->str.bytes[k], fp);
                    }
                    fputc('"', fp);
                }
            } else if (NUQ_IS_PTR(x) && NUQ_PTR(x)->type == NUQ_T_NULL) {
                /* empty */
            } else {
                nuq_json_print(fp, x, 0);
            }
        }
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "uri") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = (char *)GC_malloc_atomic(so->str.len * 3 + 1);
        size_t bl = 0;
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < so->str.len; i++) {
            unsigned char c = (unsigned char)so->str.bytes[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                buf[bl++] = c;
            } else {
                buf[bl++] = '%';
                buf[bl++] = hex[c >> 4];
                buf[bl++] = hex[c & 0xF];
            }
        }
        buf[bl] = '\0';
        return nuq_make_string_take(buf, bl);
    }
    if (strcmp(fmt, "html") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        for (size_t i = 0; i < so->str.len; i++) {
            char c = so->str.bytes[i];
            switch (c) {
              case '<': fputs("&lt;", fp); break;
              case '>': fputs("&gt;", fp); break;
              case '&': fputs("&amp;", fp); break;
              case '\'': fputs("&#39;", fp); break;
              case '"': fputs("&quot;", fp); break;
              default: fputc(c, fp); break;
            }
        }
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "sh") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        fputc('\'', fp);
        for (size_t i = 0; i < so->str.len; i++) {
            if (so->str.bytes[i] == '\'') fputs("'\\''", fp);
            else fputc(so->str.bytes[i], fp);
        }
        fputc('\'', fp);
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "base64") == 0 || strcmp(fmt, "base64d") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        if (fmt[strlen(fmt)-1] == 'd') {
            /* decode */
            const char *src = so->str.bytes;
            size_t sl = so->str.len;
            char *out = (char *)GC_malloc_atomic(sl + 1);
            size_t ol = 0;
            int buf = 0, bits = 0;
            for (size_t i = 0; i < sl; i++) {
                int v;
                char c = src[i];
                if (c >= 'A' && c <= 'Z') v = c - 'A';
                else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
                else if (c >= '0' && c <= '9') v = c - '0' + 52;
                else if (c == '+') v = 62;
                else if (c == '/') v = 63;
                else if (c == '=') break;
                else continue;
                buf = (buf << 6) | v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out[ol++] = (char)((buf >> bits) & 0xFF);
                }
            }
            out[ol] = '\0';
            return nuq_make_string_take(out, ol);
        } else {
            static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const unsigned char *src = (const unsigned char *)so->str.bytes;
            size_t sl = so->str.len;
            size_t ol = 4 * ((sl + 2) / 3);
            char *out = (char *)GC_malloc_atomic(ol + 1);
            size_t op = 0;
            for (size_t i = 0; i < sl; i += 3) {
                int b0 = src[i];
                int b1 = i+1 < sl ? src[i+1] : 0;
                int b2 = i+2 < sl ? src[i+2] : 0;
                out[op++] = tab[b0 >> 2];
                out[op++] = tab[((b0 & 3) << 4) | (b1 >> 4)];
                out[op++] = i+1 < sl ? tab[((b1 & 0xF) << 2) | (b2 >> 6)] : '=';
                out[op++] = i+2 < sl ? tab[b2 & 0x3F] : '=';
            }
            out[op] = '\0';
            return nuq_make_string_take(out, op);
        }
    }
    fprintf(stderr, "nuq error: unknown @%s\n", fmt);
    return v;
}

VALUE
nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body)
{
    if (body == NULL) {
        /* @fmt alone — apply to current input */
        nuq_emit(c, fmt_apply(fmt_id, c->input));
        return BR_OK;
    }
    /* @fmt "..." — body is interp; per emit, apply fmt to result */
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, body, c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) nuq_emit(c, fmt_apply(fmt_id, bo->arr.items[i]));
    return BR_OK;
}

/* --- assignment / update — minimal v0: only supports `f |= g` for top-level paths.
 * Real jq path support requires path-tracking; we'll skip for v0. */
VALUE nuq_assign_eval(CTX *c, struct Node *path, struct Node *value)
{
    (void)c; (void)path; (void)value;
    fprintf(stderr, "nuq: `=` assignment not supported in v0\n");
    return BR_ERROR;
}
VALUE nuq_update_assign_eval(CTX *c, struct Node *path, struct Node *value, uint32_t op)
{
    (void)c; (void)path; (void)value; (void)op;
    fprintf(stderr, "nuq: `|=` update assignment not supported in v0\n");
    return BR_ERROR;
}

/* --- top-level run ---------------------------------------------------- */

void
nuq_run(CTX *c, struct Node *filter, VALUE input)
{
    c->input = input;
    c->break_label = 0;
    c->error = NUQ_NULL;
    c->emit_buf = nuq_make_array(0);
    VALUE r = EVAL(c, filter);
    if (r == BR_ERROR && c->error != NUQ_NULL) {
        if (NUQ_IS_PTR(c->error) && NUQ_PTR(c->error)->type == NUQ_T_STRING)
            fprintf(stderr, "nuq: error: %s\n", nuq_string_cstr(c->error));
        else {
            fprintf(stderr, "nuq: error: ");
            nuq_json_print(stderr, c->error, 0);
            fputc('\n', stderr);
        }
        c->error = NUQ_NULL;
    }
    /* Print all collected outputs */
    if (r == BR_OK) {
        struct nuq_obj *bo = NUQ_PTR(c->emit_buf);
        for (size_t i = 0; i < bo->arr.len; i++) {
            VALUE v = bo->arr.items[i];
            if (OPTION.raw_output && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
                struct nuq_obj *o = NUQ_PTR(v);
                fwrite(o->str.bytes, 1, o->str.len, stdout);
            } else {
                nuq_json_print(stdout, v, OPTION.compact_output ? 0 : OPTION.indent);
            }
            fputc('\n', stdout);
        }
    }
}
