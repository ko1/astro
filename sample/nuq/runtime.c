/*
 * runtime.c — semantic helpers for node.def evaluators.
 *
 * Each NODE_DEF returns VALUE = a nuq_array of emits.  `EVAL_ARG(c,
 * child)` returns the child's emit array directly (no shared
 * emit_buf).  The simple cases (binop, comma, leaf builtins) live
 * inline in node.def for SD specialisation; longer / less-hot helpers
 * live here.
 *
 * Helpers in this file call `EVAL` (runtime-resolved dispatcher) on
 * sub-nodes — that's a deliberate trade-off: the SD specialiser can't
 * fold past these helpers, but the body-having builtins they
 * implement are infrequent enough that the loss is small compared to
 * the simpler implementation.  The hot paths (pipe, comma, binop,
 * map, select) are inlined in node.def itself.
 */
#include "context.h"
#include "node.h"

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

/* --- error helpers ------------------------------------------------------- */

static VALUE
err_array(CTX *c, const char *msg)
{
    c->error = nuq_make_string(msg, strlen(msg));
    return nuq_make_array(0);
}

/* --- primitive value helpers (read c->input directly) ------------------- */

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

bool
nuq_to_number(VALUE v, VALUE *out)
{
    if (NUQ_IS_FIX(v)) { *out = v; return true; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) { *out = v; return true; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
        struct nuq_obj *o = NUQ_PTR(v);
        char *e; double d = strtod(o->str.bytes, &e);
        if (e == o->str.bytes) return false;
        *out = nuq_make_double(d);
        return true;
    }
    return false;
}

/* --- index `.[expr]` --------------------------------------------------- */

static int64_t
to_int64(VALUE v)
{
    if (NUQ_IS_FIX(v)) return NUQ_FIX_VAL(v);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) return (int64_t)NUQ_PTR(v)->dbl;
    return 0;
}

VALUE
nuq_index_eval(CTX *c, struct Node *expr, bool optional)
{
    VALUE in = c->input;
    VALUE keys = EVAL(c, expr);
    if (UNLIKELY(c->error != NUQ_NULL)) return keys;
    struct nuq_obj *ko = NUQ_PTR(keys);
    VALUE r = nuq_make_array(ko->arr.len);
    for (size_t i = 0; i < ko->arr.len; i++) {
        VALUE k = ko->arr.items[i];
        if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL) {
            nuq_array_push(r, NUQ_NULL);
            continue;
        }
        if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
            if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)
                nuq_array_push(r, nuq_object_get(in, k));
            else if (!optional)
                return err_array(c, "object index must be string");
            continue;
        }
        if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) {
            if (NUQ_IS_FIX(k) || (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_DOUBLE))
                nuq_array_push(r, nuq_array_get(in, to_int64(k)));
            else if (!optional)
                return err_array(c, "array index must be number");
            continue;
        }
        if (!optional) return err_array(c, "type error: cannot index");
    }
    return r;
}

/* --- slice ------------------------------------------------------------- */

VALUE
nuq_slice_eval(CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional)
{
    VALUE in = c->input;
    int64_t length;
    bool is_str = false;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL) {
        VALUE r = nuq_make_array(1);
        nuq_array_push(r, NUQ_NULL);
        return r;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) length = (int64_t)NUQ_PTR(in)->arr.len;
    else if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_STRING) { length = (int64_t)NUQ_PTR(in)->str.len; is_str = true; }
    else {
        if (optional) return nuq_make_array(0);
        return err_array(c, "type error: cannot slice");
    }
    int64_t start = 0, stop = length;
    if (flags & SLICE_HAS_START) {
        VALUE buf = EVAL(c, startn);
        if (c->error != NUQ_NULL) return buf;
        if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
        VALUE v = NUQ_PTR(buf)->arr.items[0];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) start = 0;
        else start = to_int64(v);
    }
    if (flags & SLICE_HAS_STOP) {
        VALUE buf = EVAL(c, stopn);
        if (c->error != NUQ_NULL) return buf;
        if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
        VALUE v = NUQ_PTR(buf)->arr.items[0];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) stop = length;
        else stop = to_int64(v);
    }
    if (start < 0) start += length;
    if (stop  < 0) stop  += length;
    if (start < 0) start = 0;
    if (stop  > length) stop = length;
    if (stop  < start) stop = start;

    VALUE r = nuq_make_array(1);
    if (is_str) {
        struct nuq_obj *o = NUQ_PTR(in);
        nuq_array_push(r, nuq_make_string(o->str.bytes + start, (size_t)(stop - start)));
    } else {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE arr = nuq_make_array((size_t)(stop - start));
        for (int64_t i = start; i < stop; i++) nuq_array_push(arr, o->arr.items[i]);
        nuq_array_push(r, arr);
    }
    return r;
}

/* --- and/or with short-circuit ----------------------------------------- */

VALUE
nuq_andor_eval(CTX *c, struct Node *lhs, struct Node *rhs, bool is_and)
{
    VALUE l = EVAL(c, lhs);
    if (c->error != NUQ_NULL) return l;
    struct nuq_obj *lo = NUQ_PTR(l);
    VALUE r = nuq_make_array(0);
    for (size_t i = 0; i < lo->arr.len; i++) {
        bool ta = nuq_truthy(lo->arr.items[i]);
        if ((is_and && !ta) || (!is_and && ta)) {
            nuq_array_push(r, is_and ? NUQ_FALSE : NUQ_TRUE);
            continue;
        }
        VALUE rv = EVAL(c, rhs);
        if (c->error != NUQ_NULL) return rv;
        struct nuq_obj *ro = NUQ_PTR(rv);
        for (size_t j = 0; j < ro->arr.len; j++)
            nuq_array_push(r, nuq_truthy(ro->arr.items[j]) ? NUQ_TRUE : NUQ_FALSE);
    }
    return r;
}

VALUE
nuq_alt_eval(CTX *c, struct Node *lhs, struct Node *rhs)
{
    VALUE saved_err = c->error;
    VALUE l = EVAL(c, lhs);
    if (c->error != NUQ_NULL) {
        c->error = saved_err;
        return EVAL(c, rhs);
    }
    struct nuq_obj *lo = NUQ_PTR(l);
    VALUE r = nuq_make_array(0);
    bool any_truthy = false;
    for (size_t i = 0; i < lo->arr.len; i++) {
        if (nuq_truthy(lo->arr.items[i])) {
            any_truthy = true;
            nuq_array_push(r, lo->arr.items[i]);
        }
    }
    if (!any_truthy) return EVAL(c, rhs);
    return r;
}

/* --- object constructor ------------------------------------------------ */

VALUE
nuq_object_eval(CTX *c, uint32_t entries_id)
{
    struct obj_ctor *e = &obj_tab[entries_id];
    if (e->cnt > 32) return err_array(c, "object literal too large");
    VALUE kstreams[32], vstreams[32];

    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *ie = &e->items[i];
        VALUE ks;
        if (ie->kkind == 0) {
            ks = nuq_make_array(1);
            nuq_array_push(ks, nuq_make_string(ie->kname, strlen(ie->kname)));
        } else if (ie->kkind == 2) {
            ks = nuq_make_array(1);
            const char *nm = nuq_intern_lookup(ie->var_id);
            nuq_array_push(ks, nuq_make_string(nm, strlen(nm)));
        } else {
            ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return ks;
        }
        kstreams[i] = ks;

        VALUE vs;
        if (ie->vexpr == NULL) {
            vs = nuq_make_array(1);
            VALUE v;
            if (ie->kkind == 2) v = nuq_var_get(c, ie->var_id);
            else v = nuq_object_get_cstr(c->input, ie->kname);
            nuq_array_push(vs, v);
        } else {
            vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return vs;
        }
        vstreams[i] = vs;
    }

    /* No emits if any stream is empty */
    for (size_t i = 0; i < e->cnt; i++)
        if (NUQ_PTR(kstreams[i])->arr.len == 0 || NUQ_PTR(vstreams[i])->arr.len == 0)
            return nuq_make_array(0);

    VALUE r = nuq_make_array(1);
    size_t kidx[32] = {0}, vidx[32] = {0};
    for (;;) {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE k = NUQ_PTR(kstreams[i])->arr.items[kidx[i]];
            VALUE v = NUQ_PTR(vstreams[i])->arr.items[vidx[i]];
            if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING))
                return err_array(c, "object key must be string");
            nuq_object_set(obj, k, v);
        }
        nuq_array_push(r, obj);

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
    return r;
}

/* --- if / try / as / error -------------------------------------------- */

VALUE
nuq_if_eval(CTX *c, struct Node *cond, struct Node *thn, struct Node *els)
{
    VALUE cs = EVAL(c, cond);
    if (c->error != NUQ_NULL) return cs;
    struct nuq_obj *co = NUQ_PTR(cs);
    VALUE r = nuq_make_array(0);
    for (size_t i = 0; i < co->arr.len; i++) {
        struct Node *branch = nuq_truthy(co->arr.items[i]) ? thn : els;
        if (branch == NULL) {
            nuq_array_push(r, c->input);
        } else {
            VALUE bo = EVAL(c, branch);
            if (c->error != NUQ_NULL) return bo;
            struct nuq_obj *bv = NUQ_PTR(bo);
            for (size_t j = 0; j < bv->arr.len; j++) nuq_array_push(r, bv->arr.items[j]);
        }
    }
    return r;
}

VALUE
nuq_try_eval(CTX *c, struct Node *body, struct Node *handler)
{
    VALUE saved_err = c->error;
    VALUE bo = EVAL(c, body);
    if (c->error != NUQ_NULL) {
        VALUE err = c->error;
        c->error = saved_err;
        if (handler) {
            VALUE saved = c->input;
            c->input = err;
            VALUE r = EVAL(c, handler);
            c->input = saved;
            return r;
        }
        return nuq_make_array(0);
    }
    return bo;
}

VALUE
nuq_as_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *body)
{
    VALUE bs = EVAL(c, src);
    if (c->error != NUQ_NULL) return bs;
    struct nuq_obj *bo = NUQ_PTR(bs);
    VALUE r = nuq_make_array(0);
    for (size_t i = 0; i < bo->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, bo->arr.items[i]);
        VALUE rv = EVAL(c, body);
        nuq_var_pop(c, top);
        if (c->error != NUQ_NULL) return rv;
        struct nuq_obj *ro = NUQ_PTR(rv);
        for (size_t j = 0; j < ro->arr.len; j++) nuq_array_push(r, ro->arr.items[j]);
    }
    return r;
}

VALUE
nuq_error_eval(CTX *c, struct Node *expr)
{
    VALUE v = EVAL(c, expr);
    if (c->error != NUQ_NULL) return v;
    struct nuq_obj *vo = NUQ_PTR(v);
    if (vo->arr.len > 0) c->error = vo->arr.items[0];
    else c->error = NUQ_NULL;
    return nuq_make_array(0);
}

/* --- user-def call / def block ---------------------------------------- */

VALUE
nuq_user_call(CTX *c, uint32_t name_id, uint32_t arity, uint32_t args_id)
{
    struct Node **args = NULL;
    if (arity > 0) args = args_tab[args_id].args;

    struct nuq_func_def *fd = nuq_func_lookup(c, name_id, (int)arity);
    if (fd) {
        size_t var_top = c->var_top;
        size_t func_top = c->func_cnt;
        for (uint32_t i = 0; i < arity; i++) {
            if (fd->param_is_value[i]) {
                VALUE buf = EVAL(c, args[i]);
                if (c->error != NUQ_NULL) return buf;
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
    fprintf(stderr, "nuq error: %s/%u is not defined\n", nuq_intern_lookup(name_id), arity);
    return err_array(c, "undefined");
}

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
    VALUE init_buf = EVAL(c, init);
    if (c->error != NUQ_NULL) return init_buf;
    VALUE acc = NUQ_PTR(init_buf)->arr.len > 0 ? NUQ_PTR(init_buf)->arr.items[0] : NUQ_NULL;

    VALUE src_buf = EVAL(c, src);
    if (c->error != NUQ_NULL) return src_buf;
    struct nuq_obj *so = NUQ_PTR(src_buf);
    VALUE saved_input = c->input;
    for (size_t i = 0; i < so->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, so->arr.items[i]);
        c->input = acc;
        VALUE up_buf = EVAL(c, update);
        nuq_var_pop(c, top);
        if (c->error != NUQ_NULL) { c->input = saved_input; return up_buf; }
        struct nuq_obj *uo = NUQ_PTR(up_buf);
        if (uo->arr.len > 0) acc = uo->arr.items[uo->arr.len - 1];
    }
    c->input = saved_input;
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, acc);
    return r;
}

VALUE
nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract)
{
    VALUE init_buf = EVAL(c, init);
    if (c->error != NUQ_NULL) return init_buf;
    VALUE acc = NUQ_PTR(init_buf)->arr.len > 0 ? NUQ_PTR(init_buf)->arr.items[0] : NUQ_NULL;

    VALUE src_buf = EVAL(c, src);
    if (c->error != NUQ_NULL) return src_buf;
    struct nuq_obj *so = NUQ_PTR(src_buf);
    VALUE r = nuq_make_array(0);
    VALUE saved_input = c->input;
    for (size_t i = 0; i < so->arr.len; i++) {
        size_t top = c->var_top;
        nuq_var_push(c, var_id, so->arr.items[i]);
        c->input = acc;
        VALUE up_buf = EVAL(c, update);
        if (c->error != NUQ_NULL) { nuq_var_pop(c, top); c->input = saved_input; return up_buf; }
        struct nuq_obj *uo = NUQ_PTR(up_buf);
        for (size_t j = 0; j < uo->arr.len; j++) {
            acc = uo->arr.items[j];
            if (extract) {
                c->input = acc;
                VALUE ex_buf = EVAL(c, extract);
                if (c->error != NUQ_NULL) { nuq_var_pop(c, top); c->input = saved_input; return ex_buf; }
                struct nuq_obj *eo = NUQ_PTR(ex_buf);
                for (size_t k = 0; k < eo->arr.len; k++) nuq_array_push(r, eo->arr.items[k]);
            } else {
                nuq_array_push(r, acc);
            }
        }
        nuq_var_pop(c, top);
    }
    c->input = saved_input;
    return r;
}

/* --- string interp ----------------------------------------------------- */

VALUE
nuq_interp_eval(CTX *c, uint32_t parts_id)
{
    struct interp_entry *e = &interp_tab[parts_id];
    if (e->cnt > 32) return err_array(c, "interp too long");

    VALUE streams[32];
    for (size_t i = 0; i < e->cnt; i++) {
        VALUE buf = EVAL(c, e->parts[i]);
        if (c->error != NUQ_NULL) return buf;
        VALUE strs = nuq_make_array(NUQ_PTR(buf)->arr.len);
        struct nuq_obj *bo = NUQ_PTR(buf);
        for (size_t j = 0; j < bo->arr.len; j++)
            nuq_array_push(strs, nuq_to_json_string(bo->arr.items[j]));
        streams[i] = strs;
        if (NUQ_PTR(strs)->arr.len == 0) return nuq_make_array(0);
    }

    VALUE r = nuq_make_array(0);
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
        nuq_array_push(r, nuq_make_string_take(buf, total));

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            idx[pos]++;
            if (idx[pos] < NUQ_PTR(streams[pos])->arr.len) break;
            idx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return r;
}

/* --- format @csv etc. -------------------------------------------------- */

static VALUE
fmt_apply(uint32_t fmt_id, VALUE v)
{
    const char *fmt = nuq_fmt_lookup(fmt_id);
    if (strcmp(fmt, "text") == 0 || strcmp(fmt, "json") == 0) return nuq_to_json_string(v);
    if (strcmp(fmt, "csv") == 0 || strcmp(fmt, "tsv") == 0) {
        bool tsv = (fmt[0] == 't');
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY))
            return nuq_make_string("", 0);
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
                        char ch = so->str.bytes[k];
                        if (ch == '\t') fputs("\\t", fp);
                        else if (ch == '\r') fputs("\\r", fp);
                        else if (ch == '\n') fputs("\\n", fp);
                        else if (ch == '\\') fputs("\\\\", fp);
                        else fputc(ch, fp);
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
            unsigned char ch = (unsigned char)so->str.bytes[i];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                buf[bl++] = ch;
            } else {
                buf[bl++] = '%';
                buf[bl++] = hex[ch >> 4];
                buf[bl++] = hex[ch & 0xF];
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
            char ch = so->str.bytes[i];
            switch (ch) {
              case '<': fputs("&lt;", fp); break;
              case '>': fputs("&gt;", fp); break;
              case '&': fputs("&amp;", fp); break;
              case '\'': fputs("&#39;", fp); break;
              case '"': fputs("&quot;", fp); break;
              default: fputc(ch, fp); break;
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
    if (strcmp(fmt, "base64") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
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
    if (strcmp(fmt, "base64d") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        const char *src = so->str.bytes;
        size_t sl = so->str.len;
        char *out = (char *)GC_malloc_atomic(sl + 1);
        size_t ol = 0;
        int buf = 0, bits = 0;
        for (size_t i = 0; i < sl; i++) {
            int x; char ch = src[i];
            if (ch >= 'A' && ch <= 'Z') x = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') x = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') x = ch - '0' + 52;
            else if (ch == '+') x = 62;
            else if (ch == '/') x = 63;
            else if (ch == '=') break;
            else continue;
            buf = (buf << 6) | x;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[ol++] = (char)((buf >> bits) & 0xFF);
            }
        }
        out[ol] = '\0';
        return nuq_make_string_take(out, ol);
    }
    return v;
}

VALUE
nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body)
{
    if (body == NULL) {
        VALUE r = nuq_make_array(1);
        nuq_array_push(r, fmt_apply(fmt_id, c->input));
        return r;
    }
    VALUE bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return bo;
    struct nuq_obj *bv = NUQ_PTR(bo);
    VALUE r = nuq_make_array(bv->arr.len);
    for (size_t i = 0; i < bv->arr.len; i++)
        nuq_array_push(r, fmt_apply(fmt_id, bv->arr.items[i]));
    return r;
}

/* --- 1-arg builtins (with sub-expression body) ---------------------- */

VALUE
nuq_map_eval(CTX *c, struct Node *body)
{
    if (UNLIKELY(!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)))
        return err_array(c, "map: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE result = nuq_make_array(o->arr.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        struct nuq_obj *bv = NUQ_PTR(bo);
        for (size_t j = 0; j < bv->arr.len; j++) nuq_array_push(result, bv->arr.items[j]);
    }
    c->input = saved;
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, result);
    return r;
}

VALUE
nuq_map_values_eval(CTX *c, struct Node *body)
{
    VALUE in = c->input;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_array(o->arr.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->arr.len; i++) {
            c->input = o->arr.items[i];
            VALUE bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
            struct nuq_obj *bv = NUQ_PTR(bo);
            if (bv->arr.len > 0) nuq_array_push(out, bv->arr.items[0]);
        }
        c->input = saved;
        VALUE r = nuq_make_array(1);
        nuq_array_push(r, out);
        return r;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_object(o->obj.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->obj.len; i++) {
            c->input = o->obj.vals[i];
            VALUE bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
            struct nuq_obj *bv = NUQ_PTR(bo);
            if (bv->arr.len > 0) nuq_object_set(out, o->obj.keys[i], bv->arr.items[0]);
        }
        c->input = saved;
        VALUE r = nuq_make_array(1);
        nuq_array_push(r, out);
        return r;
    }
    return err_array(c, "map_values: not array/object");
}

VALUE
nuq_with_entries_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT))
        return err_array(c, "with_entries: not object");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE out = nuq_make_object(o->obj.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        c->input = e;
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        struct nuq_obj *bv = NUQ_PTR(bo);
        for (size_t j = 0; j < bv->arr.len; j++) {
            VALUE ne = bv->arr.items[j];
            if (!(NUQ_IS_PTR(ne) && NUQ_PTR(ne)->type == NUQ_T_OBJECT)) continue;
            VALUE nk = nuq_object_get_cstr(ne, "key");
            VALUE nv = nuq_object_get_cstr(ne, "value");
            if (!(NUQ_IS_PTR(nk) && NUQ_PTR(nk)->type == NUQ_T_STRING)) nk = nuq_to_json_string(nk);
            nuq_object_set(out, nk, nv);
        }
    }
    c->input = saved;
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, out);
    return r;
}

VALUE
nuq_range1_eval(CTX *c, struct Node *to)
{
    VALUE buf = EVAL(c, to);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(0);
    for (size_t k = 0; k < bo->arr.len; k++) {
        int64_t n = to_int64(bo->arr.items[k]);
        for (int64_t i = 0; i < n; i++) nuq_array_push(r, nuq_make_int(i));
    }
    return r;
}

VALUE
nuq_range2_eval(CTX *c, struct Node *from, struct Node *to)
{
    VALUE m = EVAL(c, from); if (c->error != NUQ_NULL) return m;
    VALUE n = EVAL(c, to); if (c->error != NUQ_NULL) return n;
    struct nuq_obj *mo = NUQ_PTR(m), *no = NUQ_PTR(n);
    VALUE r = nuq_make_array(0);
    for (size_t i = 0; i < mo->arr.len; i++)
    for (size_t j = 0; j < no->arr.len; j++) {
        int64_t lo = to_int64(mo->arr.items[i]);
        int64_t hi = to_int64(no->arr.items[j]);
        for (int64_t k = lo; k < hi; k++) nuq_array_push(r, nuq_make_int(k));
    }
    return r;
}

VALUE
nuq_range3_eval(CTX *c, struct Node *from, struct Node *to, struct Node *step)
{
    VALUE m = EVAL(c, from); if (c->error != NUQ_NULL) return m;
    VALUE n = EVAL(c, to); if (c->error != NUQ_NULL) return n;
    VALUE s = EVAL(c, step); if (c->error != NUQ_NULL) return s;
    struct nuq_obj *mo = NUQ_PTR(m), *no = NUQ_PTR(n), *so = NUQ_PTR(s);
    VALUE r = nuq_make_array(0);
    for (size_t i = 0; i < mo->arr.len; i++)
    for (size_t j = 0; j < no->arr.len; j++)
    for (size_t k = 0; k < so->arr.len; k++) {
        int64_t lo = to_int64(mo->arr.items[i]);
        int64_t hi = to_int64(no->arr.items[j]);
        int64_t st = to_int64(so->arr.items[k]);
        if (st == 0) continue;
        if (st > 0) for (int64_t x = lo; x < hi; x += st) nuq_array_push(r, nuq_make_int(x));
        else        for (int64_t x = lo; x > hi; x += st) nuq_array_push(r, nuq_make_int(x));
    }
    return r;
}

VALUE
nuq_has_eval(CTX *c, struct Node *key)
{
    VALUE buf = EVAL(c, key);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(bo->arr.len);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE k = bo->arr.items[i];
        bool t;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(c->input, k);
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(c->input)->arr.len;
        } else { t = false; }
        nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return r;
}

VALUE
nuq_in_eval(CTX *c, struct Node *container)
{
    VALUE buf = EVAL(c, container);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(bo->arr.len);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE cn = bo->arr.items[i];
        bool t;
        if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(cn, c->input);
        } else if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(c->input) ? NUQ_FIX_VAL(c->input) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(cn)->arr.len;
        } else { t = false; }
        nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return r;
}

VALUE
nuq_contains_eval(CTX *c, struct Node *rhs)
{
    VALUE buf = EVAL(c, rhs);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(bo->arr.len);
    for (size_t i = 0; i < bo->arr.len; i++)
        nuq_array_push(r, nuq_eq(c->input, bo->arr.items[i]) ? NUQ_TRUE : NUQ_FALSE);
    return r;
}

VALUE
nuq_split_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_array(c, "split: not string");
    VALUE buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE s = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING)) return err_array(c, "split: sep not string");
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, nuq_op_div(c->input, s));
    return r;
}

VALUE
nuq_join_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "join: not array");
    VALUE buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE s = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING)) return err_array(c, "join: sep not string");
    struct nuq_obj *so = NUQ_PTR(s);
    struct nuq_obj *ao = NUQ_PTR(c->input);
    char *out = NULL; size_t on = 0;
    FILE *fp = open_memstream(&out, &on);
    for (size_t i = 0; i < ao->arr.len; i++) {
        if (i) fwrite(so->str.bytes, 1, so->str.len, fp);
        VALUE v = ao->arr.items[i];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) continue;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            fwrite(NUQ_PTR(v)->str.bytes, 1, NUQ_PTR(v)->str.len, fp);
        } else {
            VALUE js = nuq_to_json_string(v);
            fwrite(NUQ_PTR(js)->str.bytes, 1, NUQ_PTR(js)->str.len, fp);
        }
    }
    fclose(fp);
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, nuq_make_string(out, on));
    free(out);
    return r;
}

VALUE
nuq_startswith_eval(CTX *c, struct Node *prefix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_array(c, "startswith: not string");
    VALUE buf = EVAL(c, prefix);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(bo->arr.len);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE p = bo->arr.items[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_array_push(r, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len && memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0;
        nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return r;
}

VALUE
nuq_endswith_eval(CTX *c, struct Node *suffix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_array(c, "endswith: not string");
    VALUE buf = EVAL(c, suffix);
    if (c->error != NUQ_NULL) return buf;
    struct nuq_obj *bo = NUQ_PTR(buf);
    VALUE r = nuq_make_array(bo->arr.len);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE p = bo->arr.items[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_array_push(r, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len, po->str.bytes, po->str.len) == 0;
        nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return r;
}

/* --- *_by builtins ---------------------------------------------------- */

static int cmp_natural(const void *a, const void *b) { return nuq_cmp(*(const VALUE *)a, *(const VALUE *)b); }

VALUE
nuq_sort_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "sort_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    /* compute (key, value) pairs */
    VALUE pairs = nuq_make_array(o->arr.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        VALUE k = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
        VALUE p = nuq_make_array(2);
        nuq_array_push(p, k);
        nuq_array_push(p, o->arr.items[i]);
        nuq_array_push(pairs, p);
    }
    c->input = saved;

    struct nuq_obj *po = NUQ_PTR(pairs);
    /* simple insertion sort by key (small n typical) */
    for (size_t i = 1; i < po->arr.len; i++) {
        VALUE x = po->arr.items[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(NUQ_PTR(po->arr.items[j-1])->arr.items[0], NUQ_PTR(x)->arr.items[0]) > 0) {
            po->arr.items[j] = po->arr.items[j-1]; j--;
        }
        po->arr.items[j] = x;
    }
    VALUE result = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < po->arr.len; i++)
        nuq_array_push(result, NUQ_PTR(po->arr.items[i])->arr.items[1]);
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, result);
    return r;
}

static int cmp_pair_by_first(const void *a, const void *b) {
    VALUE ka = NUQ_PTR(*(const VALUE *)a)->arr.items[0];
    VALUE kb = NUQ_PTR(*(const VALUE *)b)->arr.items[0];
    return nuq_cmp(ka, kb);
}

VALUE
nuq_group_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "group_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE pairs = nuq_make_array(o->arr.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        VALUE k = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
        VALUE p = nuq_make_array(2);
        nuq_array_push(p, k);
        nuq_array_push(p, o->arr.items[i]);
        nuq_array_push(pairs, p);
    }
    c->input = saved;

    struct nuq_obj *po = NUQ_PTR(pairs);
    qsort(po->arr.items, po->arr.len, sizeof(VALUE), cmp_pair_by_first);

    VALUE result = nuq_make_array(0);
    VALUE cur_group = NUQ_NULL;
    VALUE cur_key = NUQ_NULL;
    bool has = false;
    for (size_t i = 0; i < po->arr.len; i++) {
        VALUE k = NUQ_PTR(po->arr.items[i])->arr.items[0];
        VALUE v = NUQ_PTR(po->arr.items[i])->arr.items[1];
        if (!has || nuq_cmp(k, cur_key) != 0) {
            cur_group = nuq_make_array(0);
            cur_key = k;
            has = true;
            nuq_array_push(cur_group, v);
            nuq_array_push(result, cur_group);
        } else {
            nuq_array_push(cur_group, v);
        }
    }
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, result);
    return r;
}

VALUE
nuq_unique_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "unique_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE seen = nuq_make_array(0);
    VALUE result = nuq_make_array(0);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        VALUE k = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
        bool found = false;
        struct nuq_obj *so = NUQ_PTR(seen);
        for (size_t j = 0; j < so->arr.len; j++) if (nuq_eq(so->arr.items[j], k)) { found = true; break; }
        if (!found) {
            nuq_array_push(seen, k);
            nuq_array_push(result, o->arr.items[i]);
        }
    }
    c->input = saved;
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, result);
    return r;
}

VALUE
nuq_min_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "min_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_array(1);
    if (o->arr.len == 0) { nuq_array_push(r, NUQ_NULL); return r; }
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    c->input = o->arr.items[0];
    {
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        bestk = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        VALUE k = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
        if (nuq_cmp(k, bestk) < 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    nuq_array_push(r, bestv);
    return r;
}

VALUE
nuq_max_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_array(c, "max_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_array(1);
    if (o->arr.len == 0) { nuq_array_push(r, NUQ_NULL); return r; }
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    c->input = o->arr.items[0];
    {
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        bestk = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return nuq_make_array(0); }
        VALUE k = NUQ_PTR(bo)->arr.len > 0 ? NUQ_PTR(bo)->arr.items[0] : NUQ_NULL;
        if (nuq_cmp(k, bestk) > 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    nuq_array_push(r, bestv);
    return r;
}

/* --- string search builtins ----------------------------------------- */

VALUE
nuq_indices_eval(CTX *c, struct Node *pat)
{
    VALUE buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE p = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_array(c, "indices: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    VALUE arr = nuq_make_array(0);
    if (po->str.len > 0) {
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0)
                nuq_array_push(arr, nuq_make_int(i));
        }
    }
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, arr);
    return r;
}

VALUE
nuq_index1_eval(CTX *c, struct Node *pat)
{
    VALUE buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE p = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_array(c, "index: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    VALUE r = nuq_make_array(1);
    if (po->str.len == 0) { nuq_array_push(r, NUQ_NULL); return r; }
    for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
        if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) {
            nuq_array_push(r, nuq_make_int(i));
            return r;
        }
    }
    nuq_array_push(r, NUQ_NULL);
    return r;
}

VALUE
nuq_test_eval(CTX *c, struct Node *pat)
{
    /* substring-only fallback */
    VALUE buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE p = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_array(c, "test: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    bool t = po->str.len == 0;
    if (!t) {
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) { t = true; break; }
        }
    }
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
    return r;
}

VALUE
nuq_getpath_eval(CTX *c, struct Node *path)
{
    VALUE buf = EVAL(c, path);
    if (c->error != NUQ_NULL) return buf;
    if (NUQ_PTR(buf)->arr.len == 0) return nuq_make_array(0);
    VALUE pv = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_array(c, "getpath: not array");
    struct nuq_obj *po = NUQ_PTR(pv);
    VALUE v = c->input;
    for (size_t i = 0; i < po->arr.len; i++) {
        VALUE k = po->arr.items[i];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) v = nuq_object_get(v, k);
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            v = nuq_array_get(v, idx);
        } else { v = NUQ_NULL; break; }
    }
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, v);
    return r;
}

VALUE
nuq_limit_eval(CTX *c, struct Node *cnt, struct Node *body)
{
    VALUE nb = EVAL(c, cnt);
    if (c->error != NUQ_NULL) return nb;
    if (NUQ_PTR(nb)->arr.len == 0) return nuq_make_array(0);
    int64_t n = to_int64(NUQ_PTR(nb)->arr.items[0]);
    if (n < 0) return err_array(c, "limit doesn't support negative count");
    if (n == 0) return nuq_make_array(0);
    VALUE bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return bo;
    struct nuq_obj *bv = NUQ_PTR(bo);
    int64_t take = n < (int64_t)bv->arr.len ? n : (int64_t)bv->arr.len;
    VALUE r = nuq_make_array((size_t)take);
    for (int64_t i = 0; i < take; i++) nuq_array_push(r, bv->arr.items[i]);
    return r;
}

VALUE
nuq_nth_eval(CTX *c, struct Node *idx, struct Node *body)
{
    VALUE nb = EVAL(c, idx);
    if (c->error != NUQ_NULL) return nb;
    if (NUQ_PTR(nb)->arr.len == 0) return nuq_make_array(0);
    int64_t n = to_int64(NUQ_PTR(nb)->arr.items[0]);
    if (n < 0) return err_array(c, "nth doesn't support negative indices");
    VALUE bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return bo;
    struct nuq_obj *bv = NUQ_PTR(bo);
    VALUE r = nuq_make_array(1);
    if (n < (int64_t)bv->arr.len) nuq_array_push(r, bv->arr.items[n]);
    return r;
}

/* --- top-level run ---------------------------------------------------- */

void
nuq_run(CTX *c, struct Node *filter, VALUE input)
{
    c->input = input;
    c->error = NUQ_NULL;
    c->break_label = 0;
    VALUE emits = EVAL(c, filter);
    if (c->error != NUQ_NULL) {
        if (NUQ_IS_PTR(c->error) && NUQ_PTR(c->error)->type == NUQ_T_STRING)
            fprintf(stderr, "nuq: error: %s\n", nuq_string_cstr(c->error));
        else {
            fprintf(stderr, "nuq: error: ");
            nuq_json_print(stderr, c->error, 0);
            fputc('\n', stderr);
        }
        c->error = NUQ_NULL;
        return;
    }
    struct nuq_obj *eo = NUQ_PTR(emits);
    for (size_t i = 0; i < eo->arr.len; i++) {
        VALUE v = eo->arr.items[i];
        if (OPTION.raw_output && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            struct nuq_obj *o = NUQ_PTR(v);
            fwrite(o->str.bytes, 1, o->str.len, stdout);
        } else {
            nuq_json_print(stdout, v, OPTION.compact_output ? 0 : OPTION.indent);
        }
        fputc('\n', stdout);
    }
}
