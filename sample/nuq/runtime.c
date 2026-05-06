/*
 * runtime.c — semantic helpers for node.def evaluators (EMIT form).
 *
 * Each helper takes c->input as the current input, appends emits to
 * c->pool, and returns an EMIT slice descriptor.
 *
 * Hot-path helpers were inlined into node.def NODE_DEF bodies so the
 * SD specializer can fold them.  The helpers here are the longer /
 * less common ones (object construction, interp, format, *_by, etc.).
 * They use EVAL (runtime-resolved) on sub-nodes — the SD specializer
 * can't fold past these calls, but for less-frequent operations the
 * cost is acceptable.
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

/* For nuq_compile_all_def_bodies / nuq_load_all_def_bodies below. */
#include "astro_code_store.h"

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

void
nuq_compile_all_def_bodies(void)
{
    for (size_t i = 0; i < def_tab_len; i++) {
        const struct def_block *const db = &def_tab[i];
        for (size_t j = 0; j < db->cnt; j++) {
            NODE *const body = db->items[j].body;
            if (!body->head.flags.is_specialized) {
                astro_cs_compile(body, NULL);
            }
        }
    }
}

void
nuq_load_all_def_bodies(void)
{
    for (size_t i = 0; i < def_tab_len; i++) {
        const struct def_block *const db = &def_tab[i];
        for (size_t j = 0; j < db->cnt; j++) {
            astro_cs_load(db->items[j].body, NULL);
        }
    }
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

/* --- CLI-supplied bindings (--arg / --argjson / --slurpfile / --rawfile) -- */

struct nuq_user_arg { uint32_t name_id; VALUE value; };
static struct nuq_user_arg *user_args = NULL;
static size_t user_args_len = 0, user_args_capa = 0;

static char *
slurp_file(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
        size_t n = fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
    }
    fclose(fp);
    *len_out = len;
    return buf;
}

static void
user_args_push(uint32_t name_id, VALUE v)
{
    if (user_args_len == user_args_capa) {
        user_args_capa = user_args_capa ? user_args_capa * 2 : 8;
        user_args = (struct nuq_user_arg *)GC_realloc(
            user_args, user_args_capa * sizeof(*user_args));
    }
    user_args[user_args_len].name_id = name_id;
    user_args[user_args_len].value = v;
    user_args_len++;
}

void
nuq_user_arg_add(const char *name, const char *value, bool json)
{
    VALUE v;
    if (json) {
        const char *endp;
        char *err = NULL;
        v = nuq_json_parse(value, strlen(value), &endp, &err);
        if (err) {
            fprintf(stderr, "nuq: --argjson %s: parse error: %s\n", name, err);
            exit(2);
        }
    } else {
        v = nuq_make_string(value, strlen(value));
    }
    user_args_push(nuq_intern(name), v);
}

bool
nuq_user_arg_add_file(const char *name, const char *path, bool raw)
{
    size_t L;
    char *buf = slurp_file(path, &L);
    if (!buf) {
        fprintf(stderr, "nuq: cannot open %s\n", path);
        return false;
    }
    VALUE v;
    if (raw) {
        v = nuq_make_string(buf, L);
    } else {
        /* --slurpfile: parse all JSON values, wrap into a single array. */
        VALUE arr = nuq_make_array(0);
        const char *p = buf, *end = buf + L;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p >= end) break;
            const char *np;
            char *err = NULL;
            VALUE jv = nuq_json_parse(p, end - p, &np, &err);
            if (err) {
                fprintf(stderr, "nuq: --slurpfile %s: parse error: %s\n", path, err);
                return false;
            }
            nuq_array_push(arr, jv);
            p = np;
        }
        v = arr;
    }
    user_args_push(nuq_intern(name), v);
    return true;
}

void
nuq_user_args_bind(CTX *c)
{
    /* `--arg foo bar` exposes both `$foo` (jq style) and `$ENV.foo`...
     * We only do `$foo` for now (no $ENV table). */
    for (size_t i = 0; i < user_args_len; i++) {
        nuq_var_push(c, user_args[i].name_id, user_args[i].value);
    }
}

/* --- input queue (used by `input` / `inputs` builtins) ------------------- */

static VALUE *input_queue = NULL;
static size_t input_pos = 0, input_cnt = 0;

void
nuq_input_queue_set(VALUE *items, size_t cnt)
{
    input_queue = items;
    input_pos = 0;
    input_cnt = cnt;
}

bool
nuq_input_pull(VALUE *out)
{
    if (input_pos >= input_cnt) return false;
    *out = input_queue[input_pos++];
    return true;
}

/* --- helpers ------------------------------------------------------------- */

static EMIT
err_emit(CTX *c, const char *msg)
{
    c->error = nuq_make_string(msg, strlen(msg));
    return EMIT_EMPTY;
}

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

static int64_t
to_int64(VALUE v)
{
    if (NUQ_IS_FIX(v)) return NUQ_FIX_VAL(v);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) return (int64_t)NUQ_PTR(v)->dbl;
    return 0;
}

/* --- slice ------------------------------------------------------------- */

EMIT
nuq_slice_eval(CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional)
{
    VALUE in = c->input;
    int64_t length;
    bool is_str = false;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL)
        return nuq_emit_one(c, NUQ_NULL);
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) length = (int64_t)NUQ_PTR(in)->arr.len;
    else if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_STRING) { length = (int64_t)NUQ_PTR(in)->str.len; is_str = true; }
    else {
        if (optional) return EMIT_EMPTY;
        return err_emit(c, "type error: cannot slice");
    }
    int64_t start = 0, stop = length;
    if (flags & SLICE_HAS_START) {
        size_t top0 = c->pool_top;
        EMIT buf = EVAL(c, startn);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (buf.count == 0) { c->pool_top = top0; return EMIT_EMPTY; }
        VALUE v = buf.items[0];
        c->pool_top = top0;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) start = 0;
        else start = to_int64(v);
    }
    if (flags & SLICE_HAS_STOP) {
        size_t top0 = c->pool_top;
        EMIT buf = EVAL(c, stopn);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (buf.count == 0) { c->pool_top = top0; return EMIT_EMPTY; }
        VALUE v = buf.items[0];
        c->pool_top = top0;
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
        return nuq_emit_one(c, nuq_make_string(o->str.bytes + start, (size_t)(stop - start)));
    }
    struct nuq_obj *o = NUQ_PTR(in);
    VALUE arr = nuq_make_array((size_t)(stop - start));
    for (int64_t i = start; i < stop; i++) nuq_array_push(arr, o->arr.items[i]);
    return nuq_emit_one(c, arr);
}

/* --- object constructor ------------------------------------------------ */

EMIT
nuq_object_eval(CTX *c, uint32_t entries_id)
{
    const struct obj_ctor *const e = &obj_tab[entries_id];
    if (e->cnt > 16) return err_emit(c, "object literal too large");
    const size_t outer_top = c->pool_top;

    /* Fast path — emit exactly one object.  Build it directly into
     * the pool and bail to the cartesian path the first time we hit
     * a multi-emit (or zero-emit) entry.  This avoids all per-entry
     * GC_malloc(sizeof(VALUE)) buffers in the common case. */
    VALUE k_fast[16], v_fast[16];
    size_t fast_done = 0;
    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *const ie = &e->items[i];
        VALUE k, v;
        if (ie->kkind == 0 || ie->kkind == 2) {
            k = ie->kname_value;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            if (ks.count != 1) goto cartesian;
            k = ks.items[0];
            c->pool_top = t0;
        }
        if (ie->vexpr == NULL) {
            if (ie->kkind == 2) v = nuq_var_get(c, ie->var_id);
            else                v = nuq_object_get_cstr(c->input, ie->kname);
        } else {
            const size_t t0 = c->pool_top;
            const EMIT vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            if (vs.count != 1) goto cartesian;
            v = vs.items[0];
            c->pool_top = t0;
        }
        if (UNLIKELY(!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)))
            return err_emit(c, "object key must be string");
        k_fast[i] = k;
        v_fast[i] = v;
        fast_done++;
    }
    {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++)
            nuq_object_set(obj, k_fast[i], v_fast[i]);
        nuq_pool_push(c, obj);
        return nuq_emit_slice(c, outer_top);
    }

cartesian: {
    /* Multi-emit (or zero-emit) somewhere — fall back to the
     * cartesian build.  Restore pool_top so the partial fast-path
     * EVAL slots become free again. */
    c->pool_top = outer_top;
    struct stream { VALUE *items; uint32_t count; };
    struct stream kss[16], vss[16];
    /* Re-evaluate from scratch.  This is the rare path. */
    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *const ie = &e->items[i];
        if (ie->kkind == 0 || ie->kkind == 2) {
            VALUE *const b = (VALUE *)GC_malloc(sizeof(VALUE));
            b[0] = ie->kname_value;
            kss[i].items = b; kss[i].count = 1;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *const b = (VALUE *)GC_malloc(ks.count * sizeof(VALUE) + 1);
            memcpy(b, ks.items, ks.count * sizeof(VALUE));
            kss[i].items = b; kss[i].count = ks.count;
            c->pool_top = t0;
        }
        if (ie->vexpr == NULL) {
            VALUE *const b = (VALUE *)GC_malloc(sizeof(VALUE));
            b[0] = ie->kkind == 2 ? nuq_var_get(c, ie->var_id)
                                  : nuq_object_get_cstr(c->input, ie->kname);
            vss[i].items = b; vss[i].count = 1;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *const b = (VALUE *)GC_malloc(vs.count * sizeof(VALUE) + 1);
            memcpy(b, vs.items, vs.count * sizeof(VALUE));
            vss[i].items = b; vss[i].count = vs.count;
            c->pool_top = t0;
        }
    }
    for (size_t i = 0; i < e->cnt; i++)
        if (kss[i].count == 0 || vss[i].count == 0)
            return EMIT_EMPTY;

    size_t kidx[16] = {0}, vidx[16] = {0};
    for (;;) {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE k = kss[i].items[kidx[i]];
            VALUE v = vss[i].items[vidx[i]];
            if (UNLIKELY(!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)))
                return err_emit(c, "object key must be string");
            nuq_object_set(obj, k, v);
        }
        nuq_pool_push(c, obj);

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            vidx[pos]++;
            if (vidx[pos] < vss[pos].count) break;
            vidx[pos] = 0;
            kidx[pos]++;
            if (kidx[pos] < kss[pos].count) break;
            kidx[pos] = 0;
        }
        if (pos < 0) break;
    }
    (void)fast_done;
    return nuq_emit_slice(c, outer_top);
}
}

/* --- error / user-call / def ---------------------------------------- */

EMIT
nuq_error_eval(CTX *c, struct Node *expr)
{
    size_t top0 = c->pool_top;
    EMIT v = EVAL(c, expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (v.count > 0) c->error = v.items[0];
    else c->error = NUQ_NULL;
    c->pool_top = top0;
    return EMIT_EMPTY;
}

EMIT
nuq_user_call(CTX *c, uint32_t name_id, uint32_t arity, uint32_t args_id)
{
    struct Node **args = NULL;
    if (arity > 0) args = args_tab[args_id].args;

    struct nuq_func_def *fd = nuq_func_lookup(c, name_id, (int)arity);
    if (fd) {
        /* Evaluate value-args FIRST in caller scope (jq semantics). */
        VALUE arg_values[16];
        if (arity > 16) return err_emit(c, "too many args");
        for (uint32_t i = 0; i < arity; i++) {
            if (fd->param_is_value[i]) {
                size_t t0 = c->pool_top;
                EMIT buf = EVAL(c, args[i]);
                if (c->error != NUQ_NULL) return EMIT_EMPTY;
                arg_values[i] = buf.count > 0 ? buf.items[0] : NUQ_NULL;
                c->pool_top = t0;
            }
        }
        size_t var_top = c->var_top;
        size_t func_top = c->func_cnt;
        for (uint32_t i = 0; i < arity; i++) {
            if (fd->param_is_value[i]) {
                nuq_var_push(c, fd->param_ids[i], arg_values[i]);
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
        EMIT r = EVAL(c, fd->body);
        nuq_var_pop(c, var_top);
        c->func_cnt = func_top;
        return r;
    }
    fprintf(stderr, "nuq error: %s/%u is not defined\n", nuq_intern_lookup(name_id), arity);
    return err_emit(c, "undefined");
}

EMIT
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
    EMIT r = EVAL(c, body);
    c->func_cnt = saved;
    return r;
}

/* --- reduce / foreach -------------------------------------------------- */

EMIT
nuq_reduce_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update)
{
    size_t outer_top = c->pool_top;

    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    VALUE acc = init_e.count > 0 ? init_e.items[0] : NUQ_NULL;
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    /* Snapshot src — pool will be reused for update evals. */
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)GC_malloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    c->pool_top = outer_top;

    VALUE saved_input = c->input;
    for (uint32_t i = 0; i < sc; i++) {
        size_t t1 = c->pool_top;
        size_t v_top = c->var_top;
        nuq_var_push(c, var_id, src_local[i]);
        c->input = acc;
        EMIT up = EVAL(c, update);
        nuq_var_pop(c, v_top);
        if (c->error != NUQ_NULL) { c->input = saved_input; return EMIT_EMPTY; }
        if (up.count > 0) acc = up.items[up.count - 1];
        c->pool_top = t1;
    }
    c->input = saved_input;
    return nuq_emit_one(c, acc);
}

EMIT
nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract)
{
    size_t outer_top = c->pool_top;

    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    VALUE acc = init_e.count > 0 ? init_e.items[0] : NUQ_NULL;
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)GC_malloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    c->pool_top = outer_top;

    VALUE saved_input = c->input;
    for (uint32_t i = 0; i < sc; i++) {
        size_t v_top = c->var_top;
        nuq_var_push(c, var_id, src_local[i]);
        c->input = acc;
        size_t t1 = c->pool_top;
        EMIT up = EVAL(c, update);
        if (c->error != NUQ_NULL) { nuq_var_pop(c, v_top); c->input = saved_input; return EMIT_EMPTY; }
        /* update may emit multiple — for each, set acc, then extract or just emit. */
        uint32_t uc = up.count;
        VALUE usmall[16];
        VALUE *up_local = (uc <= 16) ? usmall : (VALUE *)GC_malloc(uc * sizeof(VALUE));
        memcpy(up_local, up.items, uc * sizeof(VALUE));
        c->pool_top = t1;
        for (uint32_t j = 0; j < uc; j++) {
            acc = up_local[j];
            c->input = acc;
            (void)EVAL(c, extract);    /* extract pushes onto pool */
            if (c->error != NUQ_NULL) {
                nuq_var_pop(c, v_top); c->input = saved_input; return EMIT_EMPTY;
            }
        }
        nuq_var_pop(c, v_top);
    }
    c->input = saved_input;
    return nuq_emit_slice(c, outer_top);
}

/* --- string interp ----------------------------------------------------- */

EMIT
nuq_interp_eval(CTX *c, uint32_t parts_id)
{
    struct interp_entry *e = &interp_tab[parts_id];
    if (e->cnt > 16) return err_emit(c, "interp too long");

    struct stream { VALUE *items; uint32_t count; };
    struct stream parts[16];

    for (size_t i = 0; i < e->cnt; i++) {
        size_t t0 = c->pool_top;
        EMIT pe = EVAL(c, e->parts[i]);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        VALUE *b = (VALUE *)GC_malloc(pe.count * sizeof(VALUE) + 1);
        for (uint32_t j = 0; j < pe.count; j++)
            b[j] = nuq_to_json_string(pe.items[j]);
        parts[i].items = b;
        parts[i].count = pe.count;
        c->pool_top = t0;
        if (parts[i].count == 0) return EMIT_EMPTY;
    }

    size_t outer_top = c->pool_top;
    size_t idx[16] = {0};
    for (;;) {
        size_t total = 0;
        size_t lens[16];
        VALUE strs[16];
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE s = parts[i].items[idx[i]];
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
        nuq_pool_push(c, nuq_make_string_take(buf, total));

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            idx[pos]++;
            if (idx[pos] < parts[pos].count) break;
            idx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return nuq_emit_slice(c, outer_top);
}

/* --- format -------------------------------------------------------- */

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

EMIT
nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body)
{
    if (body == NULL) {
        return nuq_emit_one(c, fmt_apply(fmt_id, c->input));
    }
    size_t top0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = bo.count;
    VALUE small[16];
    VALUE *local = (cnt <= 16) ? small : (VALUE *)GC_malloc(cnt * sizeof(VALUE));
    memcpy(local, bo.items, cnt * sizeof(VALUE));
    c->pool_top = top0;
    for (uint32_t i = 0; i < cnt; i++) nuq_pool_push(c, fmt_apply(fmt_id, local[i]));
    return nuq_emit_slice(c, top0);
}

/* --- 1-arg builtins (with sub-expression body) ---------------------- */

EMIT
nuq_map_values_eval(CTX *c, struct Node *body)
{
    VALUE in = c->input;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_array(o->arr.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->arr.len; i++) {
            c->input = o->arr.items[i];
            size_t t0 = c->pool_top;
            EMIT bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
            if (bo.count > 0) nuq_array_push(out, bo.items[0]);
            c->pool_top = t0;
        }
        c->input = saved;
        return nuq_emit_one(c, out);
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_object(o->obj.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->obj.len; i++) {
            c->input = o->obj.vals[i];
            size_t t0 = c->pool_top;
            EMIT bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
            if (bo.count > 0) nuq_object_set(out, o->obj.keys[i], bo.items[0]);
            c->pool_top = t0;
        }
        c->input = saved;
        return nuq_emit_one(c, out);
    }
    return err_emit(c, "map_values: not array/object");
}

EMIT
nuq_with_entries_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT))
        return err_emit(c, "with_entries: not object");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE out = nuq_make_object(o->obj.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        c->input = e;
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        for (uint32_t j = 0; j < bo.count; j++) {
            VALUE ne = bo.items[j];
            if (!(NUQ_IS_PTR(ne) && NUQ_PTR(ne)->type == NUQ_T_OBJECT)) continue;
            VALUE nk = nuq_object_get_cstr(ne, "key");
            VALUE nv = nuq_object_get_cstr(ne, "value");
            if (!(NUQ_IS_PTR(nk) && NUQ_PTR(nk)->type == NUQ_T_STRING)) nk = nuq_to_json_string(nk);
            nuq_object_set(out, nk, nv);
        }
        c->pool_top = t0;
    }
    c->input = saved;
    return nuq_emit_one(c, out);
}

/* Recursively transform input bottom-up: descend into containers,
 * rebuild with transformed children, then apply body to the rebuilt
 * value.  Mirrors jq's `walk(f)`. */
static VALUE
nuq_walk_recurse(CTX *c, struct Node *body, VALUE v)
{
    VALUE rebuilt = v;
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            VALUE arr = nuq_make_array(o->arr.len);
            for (size_t i = 0; i < o->arr.len; i++) {
                VALUE child = nuq_walk_recurse(c, body, o->arr.items[i]);
                if (c->error != NUQ_NULL) return NUQ_NULL;
                nuq_array_push(arr, child);
            }
            rebuilt = arr;
        } else if (o->type == NUQ_T_OBJECT) {
            VALUE obj = nuq_make_object(o->obj.len > 4 ? o->obj.len : 4);
            for (size_t i = 0; i < o->obj.len; i++) {
                VALUE child = nuq_walk_recurse(c, body, o->obj.vals[i]);
                if (c->error != NUQ_NULL) return NUQ_NULL;
                nuq_object_set(obj, o->obj.keys[i], child);
            }
            rebuilt = obj;
        }
    }
    /* Apply body to the rebuilt value.  body is a generic filter that
     * may emit multiple values; jq's walk uses only the first emit
     * (it's effectively a `.|f` chain).  We follow that convention. */
    VALUE saved = c->input;
    c->input = rebuilt;
    size_t t0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    VALUE out = (c->error != NUQ_NULL || bo.count == 0) ? rebuilt : bo.items[0];
    if (bo.count > 0 && c->error == NUQ_NULL) {
        /* steal the first emit before pool rewind. */
        out = bo.items[0];
    }
    c->pool_top = t0;
    c->input = saved;
    return out;
}

EMIT
nuq_walk_eval(CTX *c, struct Node *body)
{
    VALUE r = nuq_walk_recurse(c, body, c->input);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_one(c, r);
}

/* recurse(f) / recurse(f; cond):
 *
 *   def recurse(f):    def r: ., (f | r); r;
 *   def recurse(f;c):  def r: ., (f | select(c) | r); r;
 *
 * We hand-roll: emit current, evaluate body, for each emit (filtered
 * by cond if given), recurse on it.  Iteration is depth-first to
 * match jq's order. */
static void
nuq_recurse_dfs(CTX *c, struct Node *body, struct Node *cond, VALUE v)
{
    nuq_pool_push(c, v);
    VALUE saved = c->input;
    c->input = v;
    size_t t0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) { c->input = saved; return; }
    /* Snapshot — recursion below grows the pool so we can't iterate
     * a live slice. */
    uint32_t cnt = bo.count;
    VALUE small[16];
    VALUE *local = (cnt <= 16) ? small
                               : (VALUE *)GC_malloc(cnt * sizeof(VALUE));
    memcpy(local, bo.items, cnt * sizeof(VALUE));
    c->pool_top = t0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (cond != NULL) {
            c->input = local[i];
            size_t tt = c->pool_top;
            EMIT co = EVAL(c, cond);
            if (c->error != NUQ_NULL) { c->input = saved; return; }
            bool any = false;
            for (uint32_t j = 0; j < co.count; j++)
                if (nuq_truthy(co.items[j])) { any = true; break; }
            c->pool_top = tt;
            if (!any) continue;
        }
        nuq_recurse_dfs(c, body, cond, local[i]);
        if (c->error != NUQ_NULL) { c->input = saved; return; }
    }
    c->input = saved;
}

EMIT
nuq_recurse_eval(CTX *c, struct Node *body, struct Node *cond)
{
    size_t outer = c->pool_top;
    nuq_recurse_dfs(c, body, cond, c->input);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_slice(c, outer);
}

/* `env` — build an object of environment variables (lazy, once per
 * call; for hot use the caller can bind it to a variable). */
extern char **environ;
VALUE
nuq_env_object(void)
{
    VALUE r = nuq_make_object(16);
    for (char **p = environ; *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq) continue;
        VALUE k = nuq_make_string(*p, eq - *p);
        VALUE v = nuq_make_string(eq + 1, strlen(eq + 1));
        nuq_object_set(r, k, v);
    }
    return r;
}

/* gsub(s; r) — replace ALL non-overlapping occurrences of substring s
 * with r.  Matches jq's gsub when both args are literal (non-regex). */
EMIT
nuq_gsub_eval(CTX *c, struct Node *pat_n, struct Node *repl_n)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "gsub: not string");
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, pat_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0 || !(NUQ_IS_PTR(pe.items[0]) && NUQ_PTR(pe.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "gsub: pattern not string");
    VALUE pv = pe.items[0];
    c->pool_top = t0;
    EMIT re = EVAL(c, repl_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (re.count == 0 || !(NUQ_IS_PTR(re.items[0]) && NUQ_PTR(re.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "gsub: replacement not string");
    VALUE rv = re.items[0];
    c->pool_top = t0;
    struct nuq_obj *si = NUQ_PTR(c->input);
    struct nuq_obj *sp = NUQ_PTR(pv);
    struct nuq_obj *sr = NUQ_PTR(rv);
    if (sp->str.len == 0) return nuq_emit_one(c, c->input);   /* empty pat: identity */
    char *out = NULL; size_t on = 0;
    FILE *fp = open_memstream(&out, &on);
    size_t i = 0;
    while (i + sp->str.len <= si->str.len) {
        if (memcmp(si->str.bytes + i, sp->str.bytes, sp->str.len) == 0) {
            fwrite(sr->str.bytes, 1, sr->str.len, fp);
            i += sp->str.len;
        } else {
            fputc(si->str.bytes[i], fp);
            i++;
        }
    }
    while (i < si->str.len) { fputc(si->str.bytes[i], fp); i++; }
    fclose(fp);
    VALUE result = nuq_make_string(out, on);
    free(out);
    return nuq_emit_one(c, result);
}

/* sub(s; r) — replace FIRST occurrence only. */
EMIT
nuq_sub_eval(CTX *c, struct Node *pat_n, struct Node *repl_n)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "sub: not string");
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, pat_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0 || !(NUQ_IS_PTR(pe.items[0]) && NUQ_PTR(pe.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "sub: pattern not string");
    VALUE pv = pe.items[0];
    c->pool_top = t0;
    EMIT re = EVAL(c, repl_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (re.count == 0 || !(NUQ_IS_PTR(re.items[0]) && NUQ_PTR(re.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "sub: replacement not string");
    VALUE rv = re.items[0];
    c->pool_top = t0;
    struct nuq_obj *si = NUQ_PTR(c->input);
    struct nuq_obj *sp = NUQ_PTR(pv);
    struct nuq_obj *sr = NUQ_PTR(rv);
    if (sp->str.len == 0) return nuq_emit_one(c, c->input);
    /* find first */
    size_t hit = (size_t)-1;
    for (size_t i = 0; i + sp->str.len <= si->str.len; i++) {
        if (memcmp(si->str.bytes + i, sp->str.bytes, sp->str.len) == 0) {
            hit = i; break;
        }
    }
    if (hit == (size_t)-1) return nuq_emit_one(c, c->input);
    size_t out_len = si->str.len - sp->str.len + sr->str.len;
    char *buf = (char *)GC_malloc_atomic(out_len + 1);
    memcpy(buf, si->str.bytes, hit);
    memcpy(buf + hit, sr->str.bytes, sr->str.len);
    memcpy(buf + hit + sr->str.len,
           si->str.bytes + hit + sp->str.len,
           si->str.len - hit - sp->str.len);
    buf[out_len] = '\0';
    return nuq_emit_one(c, nuq_make_string_take(buf, out_len));
}

/* Recursively flatten `arr` to `depth` levels into `out`.
 * `depth == 0` ⇒ append elements as-is, no recursion. */
static void
flatten_into(VALUE arr, int depth, VALUE out)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE v = o->arr.items[i];
        if (depth > 0 && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            flatten_into(v, depth - 1, out);
        } else {
            nuq_array_push(out, v);
        }
    }
}

VALUE
nuq_flatten_eval(VALUE arr, int depth)
{
    VALUE out = nuq_make_array(NUQ_PTR(arr)->arr.len);
    flatten_into(arr, depth, out);
    return out;
}

EMIT
nuq_flatten1_eval(CTX *c, struct Node *depth_expr)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "flatten: not array");
    size_t t0 = c->pool_top;
    EMIT bo = EVAL(c, depth_expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (bo.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE dv = bo.items[0];
    c->pool_top = t0;
    if (!NUQ_IS_FIX(dv)) return err_emit(c, "flatten: depth not int");
    int64_t d = NUQ_FIX_VAL(dv);
    if (d < 0) return err_emit(c, "flatten: negative depth");
    return nuq_emit_one(c, nuq_flatten_eval(c->input, (int)d));
}

/* `while(cond; update)`: emit ., then if cond(.) truthy apply update,
 * loop.  Stops at first non-truthy cond (without emitting the
 * post-stop value). */
EMIT
nuq_while_eval(CTX *c, struct Node *cond, struct Node *update)
{
    size_t outer = c->pool_top;
    VALUE cur = c->input;
    VALUE saved = c->input;
    for (;;) {
        c->input = cur;
        size_t t0 = c->pool_top;
        EMIT co = EVAL(c, cond);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bool truthy = false;
        for (uint32_t i = 0; i < co.count; i++)
            if (nuq_truthy(co.items[i])) { truthy = true; break; }
        c->pool_top = t0;
        if (!truthy) break;
        nuq_pool_push(c, cur);
        c->input = cur;
        size_t t1 = c->pool_top;
        EMIT uo = EVAL(c, update);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        if (uo.count == 0) { c->pool_top = t1; break; }
        cur = uo.items[0];
        c->pool_top = t1;
    }
    c->input = saved;
    return nuq_emit_slice(c, outer);
}

/* `until(cond; update)`: apply update until cond(.) is truthy, emit
 * the final ..  Note: jq's until emits exactly one value. */
EMIT
nuq_until_eval(CTX *c, struct Node *cond, struct Node *update)
{
    VALUE cur = c->input;
    VALUE saved = c->input;
    for (;;) {
        c->input = cur;
        size_t t0 = c->pool_top;
        EMIT co = EVAL(c, cond);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bool truthy = false;
        for (uint32_t i = 0; i < co.count; i++)
            if (nuq_truthy(co.items[i])) { truthy = true; break; }
        c->pool_top = t0;
        if (truthy) break;
        c->input = cur;
        size_t t1 = c->pool_top;
        EMIT uo = EVAL(c, update);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        if (uo.count == 0) { c->pool_top = t1; break; }
        cur = uo.items[0];
        c->pool_top = t1;
    }
    c->input = saved;
    return nuq_emit_one(c, cur);
}

EMIT
nuq_range2_eval(CTX *c, struct Node *from, struct Node *to)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT m = EVAL(c, from); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t mc = m.count; VALUE msm[16]; VALUE *ml = (mc<=16)?msm:(VALUE*)GC_malloc(mc*sizeof(VALUE));
    memcpy(ml, m.items, mc*sizeof(VALUE)); c->pool_top = t0;
    EMIT nv = EVAL(c, to); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t nc = nv.count; VALUE nsm[16]; VALUE *nl = (nc<=16)?nsm:(VALUE*)GC_malloc(nc*sizeof(VALUE));
    memcpy(nl, nv.items, nc*sizeof(VALUE)); c->pool_top = outer;
    for (uint32_t i = 0; i < mc; i++)
    for (uint32_t j = 0; j < nc; j++) {
        int64_t lo = to_int64(ml[i]), hi = to_int64(nl[j]);
        for (int64_t k = lo; k < hi; k++) nuq_pool_push(c, nuq_make_int(k));
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_range3_eval(CTX *c, struct Node *from, struct Node *to, struct Node *step)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT m = EVAL(c, from); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t mc = m.count; VALUE msm[16]; VALUE *ml = (mc<=16)?msm:(VALUE*)GC_malloc(mc*sizeof(VALUE));
    memcpy(ml, m.items, mc*sizeof(VALUE)); c->pool_top = t0;
    EMIT nv = EVAL(c, to); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t nc = nv.count; VALUE nsm[16]; VALUE *nl = (nc<=16)?nsm:(VALUE*)GC_malloc(nc*sizeof(VALUE));
    memcpy(nl, nv.items, nc*sizeof(VALUE)); c->pool_top = t0;
    EMIT s = EVAL(c, step); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t sc = s.count; VALUE ssm[16]; VALUE *sl = (sc<=16)?ssm:(VALUE*)GC_malloc(sc*sizeof(VALUE));
    memcpy(sl, s.items, sc*sizeof(VALUE)); c->pool_top = outer;
    for (uint32_t i = 0; i < mc; i++)
    for (uint32_t j = 0; j < nc; j++)
    for (uint32_t k = 0; k < sc; k++) {
        int64_t lo = to_int64(ml[i]), hi = to_int64(nl[j]), st = to_int64(sl[k]);
        if (st == 0) continue;
        if (st > 0) for (int64_t x = lo; x < hi; x += st) nuq_pool_push(c, nuq_make_int(x));
        else        for (int64_t x = lo; x > hi; x += st) nuq_pool_push(c, nuq_make_int(x));
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_has_eval(CTX *c, struct Node *key)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, key);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE k = local[i];
        bool t;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(c->input, k);
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(c->input)->arr.len;
        } else { t = false; }
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    (void)t0;
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_in_eval(CTX *c, struct Node *container)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, container);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE cn = local[i];
        bool t;
        if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(cn, c->input);
        } else if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(c->input) ? NUQ_FIX_VAL(c->input) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(cn)->arr.len;
        } else { t = false; }
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_contains_eval(CTX *c, struct Node *rhs)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, rhs);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        /* jq raises an error on type mismatch; we mirror that
         * to stay compatible.  Booleans / null / numbers are
         * homogeneous-only, anything mixed is a type error. */
        VALUE a = c->input, b = local[i];
        bool a_fix = NUQ_IS_FIX(a), b_fix = NUQ_IS_FIX(b);
        enum nuq_type ta = a_fix ? NUQ_T_DOUBLE : NUQ_PTR(a)->type;
        enum nuq_type tb = b_fix ? NUQ_T_DOUBLE : NUQ_PTR(b)->type;
        if ((a_fix || ta == NUQ_T_DOUBLE) && (b_fix || tb == NUQ_T_DOUBLE)) {
            /* both numeric: equality */
            nuq_pool_push(c, nuq_eq(a, b) ? NUQ_TRUE : NUQ_FALSE);
            continue;
        }
        if (ta != tb) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "%s and %s cannot have their containment checked",
                     nuq_type_name(a), nuq_type_name(b));
            return err_emit(c, msg);
        }
        nuq_pool_push(c, nuq_contains(a, b) ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_split_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "split: not string");
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE s = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING)) return err_emit(c, "split: sep not string");
    return nuq_emit_one(c, nuq_op_div(c->input, s));
}

/* `splits(s)` — like split(s) but emits each piece as a separate
 * value rather than as one array.  jq compatibility. */
EMIT
nuq_splits_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "splits: not string");
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE s = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING))
        return err_emit(c, "splits: sep not string");
    /* Reuse op_div which returns an array of pieces, then unpack. */
    VALUE arr = nuq_op_div(c->input, s);
    if (!(NUQ_IS_PTR(arr) && NUQ_PTR(arr)->type == NUQ_T_ARRAY))
        return err_emit(c, "splits: internal error");
    struct nuq_obj *ao = NUQ_PTR(arr);
    size_t outer = c->pool_top;
    for (size_t i = 0; i < ao->arr.len; i++) nuq_pool_push(c, ao->arr.items[i]);
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_join_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "join: not array");
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE s = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING)) return err_emit(c, "join: sep not string");
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
    EMIT r = nuq_emit_one(c, nuq_make_string(out, on));
    free(out);
    return r;
}

EMIT
nuq_startswith_eval(CTX *c, struct Node *prefix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "startswith: not string");
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, prefix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_pool_push(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len && memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0;
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

/* `ltrimstr(s)`: if input starts with s, strip prefix; else input. */
EMIT
nuq_ltrimstr_eval(CTX *c, struct Node *prefix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return nuq_emit_one(c, c->input);          /* jq: passthrough */
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, prefix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) {
            nuq_pool_push(c, c->input);             /* non-string prefix → passthrough */
            continue;
        }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        if (io->str.len >= po->str.len &&
            memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0) {
            nuq_pool_push(c, nuq_make_string(io->str.bytes + po->str.len,
                                             io->str.len - po->str.len));
        } else {
            nuq_pool_push(c, c->input);
        }
    }
    return nuq_emit_slice(c, outer);
}

/* `rtrimstr(s)`: if input ends with s, strip suffix; else input. */
EMIT
nuq_rtrimstr_eval(CTX *c, struct Node *suffix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return nuq_emit_one(c, c->input);
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, suffix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) {
            nuq_pool_push(c, c->input);
            continue;
        }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        if (io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len,
                   po->str.bytes, po->str.len) == 0) {
            nuq_pool_push(c, nuq_make_string(io->str.bytes,
                                             io->str.len - po->str.len));
        } else {
            nuq_pool_push(c, c->input);
        }
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_endswith_eval(CTX *c, struct Node *suffix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "endswith: not string");
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, suffix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_pool_push(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len, po->str.bytes, po->str.len) == 0;
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

/* --- *_by builtins -------------------------------------------------- */

static int cmp_pair_by_first(const void *a, const void *b) {
    VALUE ka = NUQ_PTR(*(const VALUE *)a)->arr.items[0];
    VALUE kb = NUQ_PTR(*(const VALUE *)b)->arr.items[0];
    return nuq_cmp(ka, kb);
}

EMIT
nuq_sort_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "sort_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE pairs = nuq_make_array(o->arr.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        VALUE p = nuq_make_array(2);
        nuq_array_push(p, k);
        nuq_array_push(p, o->arr.items[i]);
        nuq_array_push(pairs, p);
    }
    c->input = saved;
    struct nuq_obj *po = NUQ_PTR(pairs);
    qsort(po->arr.items, po->arr.len, sizeof(VALUE), cmp_pair_by_first);
    VALUE result = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < po->arr.len; i++)
        nuq_array_push(result, NUQ_PTR(po->arr.items[i])->arr.items[1]);
    return nuq_emit_one(c, result);
}

EMIT
nuq_group_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "group_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE pairs = nuq_make_array(o->arr.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
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
    return nuq_emit_one(c, result);
}

EMIT
nuq_unique_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "unique_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE seen = nuq_make_array(0);
    VALUE result = nuq_make_array(0);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        bool found = false;
        struct nuq_obj *so = NUQ_PTR(seen);
        for (size_t j = 0; j < so->arr.len; j++) if (nuq_eq(so->arr.items[j], k)) { found = true; break; }
        if (!found) {
            nuq_array_push(seen, k);
            nuq_array_push(result, o->arr.items[i]);
        }
    }
    c->input = saved;
    return nuq_emit_one(c, result);
}

EMIT
nuq_min_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "min_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) return nuq_emit_one(c, NUQ_NULL);
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    {
        c->input = o->arr.items[0];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bestk = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        if (nuq_cmp(k, bestk) < 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    return nuq_emit_one(c, bestv);
}

EMIT
nuq_max_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "max_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) return nuq_emit_one(c, NUQ_NULL);
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    {
        c->input = o->arr.items[0];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bestk = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        if (nuq_cmp(k, bestk) > 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    return nuq_emit_one(c, bestv);
}

/* --- string search builtins ---------------------------------------- */

EMIT
nuq_indices_eval(CTX *c, struct Node *pat)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE p = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_emit(c, "indices: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    VALUE arr = nuq_make_array(0);
    if (po->str.len > 0) {
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0)
                nuq_array_push(arr, nuq_make_int(i));
        }
    }
    return nuq_emit_one(c, arr);
}

EMIT
nuq_index1_eval(CTX *c, struct Node *pat)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE p = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_emit(c, "index: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    if (po->str.len == 0) return nuq_emit_one(c, NUQ_NULL);
    for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
        if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0)
            return nuq_emit_one(c, nuq_make_int(i));
    }
    return nuq_emit_one(c, NUQ_NULL);
}

EMIT
nuq_test_eval(CTX *c, struct Node *pat)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE p = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_emit(c, "test: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    bool t = po->str.len == 0;
    if (!t) {
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) { t = true; break; }
        }
    }
    return nuq_emit_one(c, t ? NUQ_TRUE : NUQ_FALSE);
}

/* Recursive setpath helper: returns a new value with `keys[start..]`
 * set to `new_val` inside `v`.  jq semantics: missing intermediate
 * keys auto-create objects (strings) or arrays (ints). */
static VALUE
setpath_recurse(VALUE v, VALUE *keys, size_t cnt, VALUE new_val)
{
    if (cnt == 0) return new_val;
    VALUE k = keys[0];
    bool key_is_str = NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING;
    bool key_is_int = NUQ_IS_FIX(k);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) {
        /* auto-vivify */
        v = key_is_str ? nuq_make_object(4) : nuq_make_array(0);
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        if (!key_is_str) return v;        /* type error → leave v */
        VALUE child = nuq_object_get(v, k);
        VALUE updated = setpath_recurse(child, keys + 1, cnt - 1, new_val);
        VALUE clone = nuq_clone(v);
        nuq_object_set(clone, k, updated);
        return clone;
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        if (!key_is_int) return v;
        int64_t idx = NUQ_FIX_VAL(k);
        struct nuq_obj *o = NUQ_PTR(v);
        if (idx < 0) idx += (int64_t)o->arr.len;
        if (idx < 0) return v;            /* out of range below 0 */
        VALUE clone = nuq_clone(v);
        struct nuq_obj *co = NUQ_PTR(clone);
        while ((int64_t)co->arr.len <= idx) nuq_array_push(clone, NUQ_NULL);
        VALUE child = co->arr.items[idx];
        VALUE updated = setpath_recurse(child, keys + 1, cnt - 1, new_val);
        co->arr.items[idx] = updated;
        return clone;
    }
    return v;   /* unknown type — leave alone */
}

/* `setpath(p; v)` — return input with value at path p replaced by v. */
EMIT
nuq_setpath_eval(CTX *c, struct Node *path, struct Node *value)
{
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, path);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = pe.items[0];
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_emit(c, "setpath: path not array");
    /* Snapshot path keys before evaluating value (which grows pool). */
    struct nuq_obj *po = NUQ_PTR(pv);
    size_t kcnt = po->arr.len;
    VALUE small[16];
    VALUE *keys = (kcnt <= 16) ? small : (VALUE *)GC_malloc(kcnt * sizeof(VALUE));
    memcpy(keys, po->arr.items, kcnt * sizeof(VALUE));
    c->pool_top = t0;
    EMIT ve = EVAL(c, value);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (ve.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE nv = ve.items[0];
    c->pool_top = t0;
    return nuq_emit_one(c, setpath_recurse(c->input, keys, kcnt, nv));
}

/* delpath_recurse: return v with `keys[start..]` deleted. */
static VALUE
delpath_recurse(VALUE v, VALUE *keys, size_t cnt)
{
    if (cnt == 0) return NUQ_NULL;     /* shouldn't reach: caller handles */
    VALUE k = keys[0];
    if (cnt == 1) {
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            VALUE clone = nuq_clone(v);
            struct nuq_obj *co = NUQ_PTR(clone);
            for (size_t i = 0; i < co->obj.len; i++) {
                if (nuq_eq(co->obj.keys[i], k)) {
                    /* shift down */
                    for (size_t j = i; j + 1 < co->obj.len; j++) {
                        co->obj.keys[j] = co->obj.keys[j+1];
                        co->obj.vals[j] = co->obj.vals[j+1];
                    }
                    co->obj.len--;
                    co->obj.idx = NULL;     /* invalidate hash idx */
                    co->obj.idx_mask = 0;
                    break;
                }
            }
            return clone;
        }
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY && NUQ_IS_FIX(k)) {
            int64_t idx = NUQ_FIX_VAL(k);
            struct nuq_obj *o = NUQ_PTR(v);
            if (idx < 0) idx += (int64_t)o->arr.len;
            if (idx < 0 || (size_t)idx >= o->arr.len) return v;
            VALUE clone = nuq_clone(v);
            struct nuq_obj *co = NUQ_PTR(clone);
            for (size_t j = (size_t)idx; j + 1 < co->arr.len; j++)
                co->arr.items[j] = co->arr.items[j+1];
            co->arr.len--;
            return clone;
        }
        return v;
    }
    /* descend, splice updated child back. */
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        VALUE child = nuq_object_get(v, k);
        VALUE updated = delpath_recurse(child, keys + 1, cnt - 1);
        VALUE clone = nuq_clone(v);
        nuq_object_set(clone, k, updated);
        return clone;
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY && NUQ_IS_FIX(k)) {
        int64_t idx = NUQ_FIX_VAL(k);
        struct nuq_obj *o = NUQ_PTR(v);
        if (idx < 0) idx += (int64_t)o->arr.len;
        if (idx < 0 || (size_t)idx >= o->arr.len) return v;
        VALUE child = o->arr.items[idx];
        VALUE updated = delpath_recurse(child, keys + 1, cnt - 1);
        VALUE clone = nuq_clone(v);
        NUQ_PTR(clone)->arr.items[idx] = updated;
        return clone;
    }
    return v;
}

/* `delpaths(paths)` — delete each path in turn (jq sorts paths
 * descending to avoid index drift; we do too for arrays). */
EMIT
nuq_delpaths_eval(CTX *c, struct Node *paths_expr)
{
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, paths_expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = pe.items[0];
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_emit(c, "delpaths: not array of paths");
    struct nuq_obj *po = NUQ_PTR(pv);
    /* Snapshot since we'll re-use pool. */
    size_t cnt = po->arr.len;
    VALUE small[64];
    VALUE *paths = (cnt <= 64) ? small : (VALUE *)GC_malloc(cnt * sizeof(VALUE));
    memcpy(paths, po->arr.items, cnt * sizeof(VALUE));
    c->pool_top = t0;
    /* Sort paths descending lex order so deeper / later paths are
     * removed first (avoids index shift in arrays). */
    for (size_t i = 1; i < cnt; i++) {
        VALUE x = paths[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(paths[j-1], x) < 0) {
            paths[j] = paths[j-1];
            j--;
        }
        paths[j] = x;
    }
    VALUE cur = c->input;
    for (size_t i = 0; i < cnt; i++) {
        VALUE p = paths[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
        struct nuq_obj *pp = NUQ_PTR(p);
        if (pp->arr.len == 0) { cur = NUQ_NULL; continue; }
        cur = delpath_recurse(cur, pp->arr.items, pp->arr.len);
    }
    return nuq_emit_one(c, cur);
}

/* Extract a path (= array of string/int keys) from a sub-AST that
 * looks like a chain of static accessors.  Returns true on success
 * and pushes path components into `path` (a nuq_array).  Returns
 * false if the AST contains a shape we don't know how to lift to
 * a path (e.g. a `.[expr]` whose expression isn't a literal int,
 * `.[]` iteration, or any non-accessor node). */
extern const struct NodeKind kind_node_identity;
extern const struct NodeKind kind_node_pipe;
extern const struct NodeKind kind_node_field;
extern const struct NodeKind kind_node_field_opt;
extern const struct NodeKind kind_node_index;
extern const struct NodeKind kind_node_int;
extern const struct NodeKind kind_node_str;

static bool
extract_path(struct Node *n, VALUE path)
{
    if (n->head.kind == &kind_node_identity) return true;
    if (n->head.kind == &kind_node_field) {
        const char *name = n->u.node_field.name;
        nuq_array_push(path, nuq_make_string(name, strlen(name)));
        return true;
    }
    if (n->head.kind == &kind_node_field_opt) {
        const char *name = n->u.node_field_opt.name;
        nuq_array_push(path, nuq_make_string(name, strlen(name)));
        return true;
    }
    if (n->head.kind == &kind_node_pipe) {
        return extract_path(n->u.node_pipe.lhs, path) &&
               extract_path(n->u.node_pipe.rhs, path);
    }
    if (n->head.kind == &kind_node_index) {
        /* node_index only carries the key expression; the receiver
         * is propagated via c->input from the surrounding pipe.  The
         * outer pipe walk pushes parent keys; we just push our own. */
        struct Node *idx = n->u.node_index.expr;
        if (idx->head.kind == &kind_node_int) {
            nuq_array_push(path, nuq_make_int((int64_t)idx->u.node_int.v));
            return true;
        }
        if (idx->head.kind == &kind_node_str) {
            const char *s = idx->u.node_str.s;
            nuq_array_push(path, nuq_make_string(s, strlen(s)));
            return true;
        }
        return false;
    }
    return false;
}

/* `del(path-expr)` — lift path-expr statically and delete that path. */
EMIT
nuq_del_eval(CTX *c, struct Node *path_expr)
{
    VALUE path = nuq_make_array(4);
    if (!extract_path(path_expr, path))
        return err_emit(c, "del: path expression not supported "
                           "(only static .foo / .[N] chains)");
    struct nuq_obj *po = NUQ_PTR(path);
    if (po->arr.len == 0) return nuq_emit_one(c, NUQ_NULL);   /* del(.) → null */
    return nuq_emit_one(c, delpath_recurse(c->input, po->arr.items, po->arr.len));
}

EMIT
nuq_assign_eval(CTX *c, struct Node *lhs, struct Node *rhs, uint32_t op_kind)
{
    /* Step 1: lift the lhs AST into a literal path array. */
    VALUE path = nuq_make_array(4);
    if (!extract_path(lhs, path))
        return err_emit(c, "assignment: path expression not supported "
                           "(only static .foo / .[N] chains)");
    struct nuq_obj *po = NUQ_PTR(path);

    /* Step 2: compute the new value depending on op_kind. */
    VALUE new_val;
    if (op_kind == NUQ_ASSIGN_PLAIN) {
        VALUE saved = c->input;
        size_t t0 = c->pool_top;
        EMIT re = EVAL(c, rhs);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        if (re.count == 0) { c->pool_top = t0; c->input = saved; return EMIT_EMPTY; }
        new_val = re.items[0];
        c->pool_top = t0;
        c->input = saved;
    } else {
        /* Read existing value at path */
        VALUE cur = c->input;
        for (size_t i = 0; i < po->arr.len; i++) {
            VALUE k = po->arr.items[i];
            if (NUQ_IS_PTR(cur) && NUQ_PTR(cur)->type == NUQ_T_OBJECT)
                cur = nuq_object_get(cur, k);
            else if (NUQ_IS_PTR(cur) && NUQ_PTR(cur)->type == NUQ_T_ARRAY && NUQ_IS_FIX(k))
                cur = nuq_array_get(cur, NUQ_FIX_VAL(k));
            else { cur = NUQ_NULL; break; }
        }
        /* Compute new value from cur and rhs */
        VALUE saved = c->input;
        c->input = cur;
        size_t t0 = c->pool_top;
        EMIT re = EVAL(c, rhs);
        c->input = saved;
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        VALUE rv = re.count > 0 ? re.items[0] : NUQ_NULL;
        c->pool_top = t0;
        switch (op_kind) {
          case NUQ_ASSIGN_UPDATE: new_val = rv; break;
          case NUQ_ASSIGN_PLUS:   new_val = nuq_op_add(cur, rv); break;
          case NUQ_ASSIGN_MINUS:  new_val = nuq_op_sub(cur, rv); break;
          case NUQ_ASSIGN_MUL:    new_val = nuq_op_mul(cur, rv); break;
          case NUQ_ASSIGN_DIV:    new_val = nuq_op_div(cur, rv); break;
          case NUQ_ASSIGN_MOD:    new_val = nuq_op_mod(cur, rv); break;
          case NUQ_ASSIGN_ALT:
            new_val = nuq_truthy(cur) ? cur : rv; break;
          default: return err_emit(c, "assignment: unknown op");
        }
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
    }

    return nuq_emit_one(c, setpath_recurse(c->input, po->arr.items, po->arr.len, new_val));
}

EMIT
nuq_getpath_eval(CTX *c, struct Node *path)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, path);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_emit(c, "getpath: not array");
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
    return nuq_emit_one(c, v);
}

EMIT
nuq_limit_eval(CTX *c, struct Node *cnt, struct Node *body)
{
    size_t t0 = c->pool_top;
    EMIT nb = EVAL(c, cnt);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (nb.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    int64_t n = to_int64(nb.items[0]);
    c->pool_top = t0;
    if (n < 0) return err_emit(c, "limit doesn't support negative count");
    if (n == 0) return EMIT_EMPTY;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    /* Truncate by adjusting pool_top — since bo's slice starts at t0
     * and goes to pool_top, we just shrink. */
    size_t take = n < (int64_t)bo.count ? (size_t)n : (size_t)bo.count;
    c->pool_top = t0 + take;
    return nuq_emit_slice(c, t0);
}

EMIT
nuq_nth_eval(CTX *c, struct Node *idx, struct Node *body)
{
    size_t t0 = c->pool_top;
    EMIT nb = EVAL(c, idx);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (nb.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    int64_t n = to_int64(nb.items[0]);
    c->pool_top = t0;
    if (n < 0) return err_emit(c, "nth doesn't support negative indices");
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (n < (int64_t)bo.count) {
        VALUE v = bo.items[n];
        c->pool_top = t0;
        return nuq_emit_one(c, v);
    }
    c->pool_top = t0;
    return EMIT_EMPTY;
}

/* --- top-level run ---------------------------------------------------- */

/* Track whether any truthy output has been emitted across the run —
 * used by `-e` / `--exit-status` to set exit code 5 when none. */
bool nuq_had_truthy_output = false;
/* Counter — non-zero suppresses stderr prints in value helpers. */
int  nuq_suppress_error_print = 0;
/* Track whether any error has been raised — used to set exit code 1. */
bool nuq_had_error = false;

void
nuq_run(CTX *const c, struct Node *const filter, VALUE input)
{
    /* Re-bind --arg style variables on each top-level run so they stay
     * visible after var_top has been used (and trimmed) on prior runs. */
    c->var_top = 0;
    nuq_user_args_bind(c);
    c->input = input;
    c->error = NUQ_NULL;
    c->break_label = 0;
    c->pool_top = 0;
    EMIT emits = EVAL(c, filter);
    /* Even when the filter ultimately errored, jq still prints any
     * emits that came before the error.  Output them first, then
     * surface the error. */
    for (uint32_t i = 0; i < emits.count; i++) {
        VALUE v = emits.items[i];
        if (OPTION.seq_output) fputc('\x1e', stdout);  /* RFC 7464 RS */
        if (OPTION.raw_output && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            struct nuq_obj *o = NUQ_PTR(v);
            fwrite(o->str.bytes, 1, o->str.len, stdout);
        } else {
            nuq_json_print(stdout, v, OPTION.compact_output ? 0 : OPTION.indent);
        }
        fputc('\n', stdout);
        if (nuq_truthy(v)) nuq_had_truthy_output = true;
    }
    if (c->error != NUQ_NULL) {
        fflush(stdout);     /* keep stderr after stdout */
        if (NUQ_IS_PTR(c->error) && NUQ_PTR(c->error)->type == NUQ_T_STRING)
            fprintf(stderr, "nuq: error: %s\n", nuq_string_cstr(c->error));
        else {
            fprintf(stderr, "nuq: error: ");
            nuq_json_print(stderr, c->error, 0);
            fputc('\n', stderr);
        }
        c->error = NUQ_NULL;
        nuq_had_error = true;
    }
}

/* --- recurse / paths pool versions -------------------------------- */

void
nuq_recurse_collect_pool(CTX *c, VALUE v)
{
    nuq_pool_push(c, v);
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            for (size_t i = 0; i < o->arr.len; i++) nuq_recurse_collect_pool(c, o->arr.items[i]);
        } else if (o->type == NUQ_T_OBJECT) {
            for (size_t i = 0; i < o->obj.len; i++) nuq_recurse_collect_pool(c, o->obj.vals[i]);
        }
    }
}

static void
paths_walk_pool(CTX *c, VALUE v, VALUE path)
{
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->arr.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, nuq_make_int(i));
            nuq_pool_push(c, p);
            paths_walk_pool(c, o->arr.items[i], p);
        }
    } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->obj.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, o->obj.keys[i]);
            nuq_pool_push(c, p);
            paths_walk_pool(c, o->obj.vals[i], p);
        }
    }
}

void
nuq_paths_collect_pool(CTX *c, VALUE v)
{
    paths_walk_pool(c, v, nuq_make_array(0));
}

/* leaf_paths: only emit paths that point to scalar leaves
 * (i.e. not arrays / objects). */
static void
leaf_paths_walk(CTX *c, VALUE v, VALUE path)
{
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->arr.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, nuq_make_int((int64_t)i));
            leaf_paths_walk(c, o->arr.items[i], p);
        }
    } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->obj.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, o->obj.keys[i]);
            leaf_paths_walk(c, o->obj.vals[i], p);
        }
    } else {
        nuq_pool_push(c, path);
    }
}

void
nuq_leaf_paths_collect_pool(CTX *c, VALUE v)
{
    leaf_paths_walk(c, v, nuq_make_array(0));
}
