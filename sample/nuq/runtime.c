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
    struct obj_ctor *e = &obj_tab[entries_id];
    if (e->cnt > 16) return err_emit(c, "object literal too large");
    /* Each entry produces a (key-stream, value-stream) pair.  We
     * collect them all into local arrays first (snapshotted from
     * pool), then cartesian product onto the result pool. */
    struct stream { VALUE *items; uint32_t count; };
    struct stream kss[16], vss[16];
    size_t outer_top = c->pool_top;

    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *ie = &e->items[i];
        if (ie->kkind == 0) {
            VALUE *b = (VALUE *)GC_malloc(sizeof(VALUE));
            
            b[0] = nuq_make_string(ie->kname, strlen(ie->kname));
            kss[i].items = b; kss[i].count = 1;
        } else if (ie->kkind == 2) {
            VALUE *b = (VALUE *)GC_malloc(sizeof(VALUE));
            
            const char *nm = nuq_intern_lookup(ie->var_id);
            b[0] = nuq_make_string(nm, strlen(nm));
            kss[i].items = b; kss[i].count = 1;
        } else {
            size_t t0 = c->pool_top;
            EMIT ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *b = (VALUE *)GC_malloc(ks.count * sizeof(VALUE) + 1);
            
            memcpy(b, ks.items, ks.count * sizeof(VALUE));
            kss[i].items = b; kss[i].count = ks.count;
            c->pool_top = t0;
        }
        if (ie->vexpr == NULL) {
            VALUE *b = (VALUE *)GC_malloc(sizeof(VALUE));
            
            VALUE v;
            if (ie->kkind == 2) v = nuq_var_get(c, ie->var_id);
            else v = nuq_object_get_cstr(c->input, ie->kname);
            b[0] = v;
            vss[i].items = b; vss[i].count = 1;
        } else {
            size_t t0 = c->pool_top;
            EMIT vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *b = (VALUE *)GC_malloc(vs.count * sizeof(VALUE) + 1);
            
            memcpy(b, vs.items, vs.count * sizeof(VALUE));
            vss[i].items = b; vss[i].count = vs.count;
            c->pool_top = t0;
        }
    }

    /* Skip emit if any stream is empty. */
    for (size_t i = 0; i < e->cnt; i++)
        if (kss[i].count == 0 || vss[i].count == 0)
            return EMIT_EMPTY;

    /* Cartesian iteration. */
    size_t kidx[16] = {0}, vidx[16] = {0};
    for (;;) {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE k = kss[i].items[kidx[i]];
            VALUE v = vss[i].items[vidx[i]];
            if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING))
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
    return nuq_emit_slice(c, outer_top);
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
    for (uint32_t i = 0; i < cnt; i++)
        nuq_pool_push(c, nuq_eq(c->input, local[i]) ? NUQ_TRUE : NUQ_FALSE);
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

void
nuq_run(CTX *c, struct Node *filter, VALUE input)
{
    c->input = input;
    c->error = NUQ_NULL;
    c->break_label = 0;
    c->pool_top = 0;
    EMIT emits = EVAL(c, filter);
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
    for (uint32_t i = 0; i < emits.count; i++) {
        VALUE v = emits.items[i];
        if (OPTION.raw_output && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            struct nuq_obj *o = NUQ_PTR(v);
            fwrite(o->str.bytes, 1, o->str.len, stdout);
        } else {
            nuq_json_print(stdout, v, OPTION.compact_output ? 0 : OPTION.indent);
        }
        fputc('\n', stdout);
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
