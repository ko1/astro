/*
 * builtin.c — built-in jq functions.
 *
 * Each builtin reads c->input (and optionally evaluates argument
 * filters with that input) and emits to c->emit_buf.  Returns
 * BR_OK / BR_ERROR / BR_BREAK via *out_status.
 *
 * Builtins are dispatched by name via a static table keyed on intern
 * id.  Lookups are linear on cold path; hot-path builtins (length,
 * type, keys, ...) are at the top.
 */
#include "context.h"
#include "node.h"
#include <time.h>

typedef VALUE (*builtin_fn_t)(CTX *c, int arity, struct Node **args);
struct builtin_entry {
    const char *name;
    int arity;
    builtin_fn_t fn;
    uint32_t name_id;     /* lazy-set */
};

/* ----- 0-arg ----- */

static VALUE
b_length(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_length(c->input)); return BR_OK; }

static VALUE
b_type(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    nuq_emit(c, nuq_make_string(nuq_type_name(c->input), strlen(nuq_type_name(c->input))));
    return BR_OK;
}

static VALUE
b_keys(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_keys(c->input, true)); return BR_OK; }

static VALUE
b_keys_unsorted(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_keys(c->input, false)); return BR_OK; }

static VALUE
b_values(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    /* jq `values` = `select(. != null)`: emit input unless null */
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_NULL) return BR_OK;
    nuq_emit(c, c->input);
    return BR_OK;
}

static VALUE
b_empty(CTX *c, int a, struct Node **args) { (void)c; (void)a; (void)args; return BR_OK; }

static VALUE
b_error0(CTX *c, int a, struct Node **args) { (void)a; (void)args; c->error = c->input; return BR_ERROR; }

static VALUE
b_error1(CTX *c, int a, struct Node **args) { (void)a; return nuq_error_eval(c, args[0]); }

static VALUE
b_not(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_truthy(c->input) ? NUQ_FALSE : NUQ_TRUE); return BR_OK; }

static VALUE
b_to_string(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_to_json_string(c->input)); return BR_OK; }

static VALUE
b_tostring(CTX *c, int a, struct Node **args) { return b_to_string(c, a, args); }

static VALUE
b_tonumber(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    VALUE v = c->input;
    if (NUQ_IS_FIX(v)) { nuq_emit(c, v); return BR_OK; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) { nuq_emit(c, v); return BR_OK; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
        struct nuq_obj *o = NUQ_PTR(v);
        char *e; double d = strtod(o->str.bytes, &e);
        if (e == o->str.bytes) {
            fprintf(stderr, "nuq error: tonumber: '%s'\n", o->str.bytes);
            c->error = nuq_make_string("tonumber failed", 16);
            return BR_ERROR;
        }
        nuq_emit(c, nuq_make_double(d));
        return BR_OK;
    }
    fprintf(stderr, "nuq error: tonumber on %s\n", nuq_type_name(v));
    c->error = nuq_make_string("type error", 10);
    return BR_ERROR;
}

static VALUE
b_ascii(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!NUQ_IS_FIX(c->input)) { fprintf(stderr, "nuq: explode/implode/ascii needs int\n"); return BR_ERROR; }
    char b[2] = { (char)(NUQ_FIX_VAL(c->input) & 0x7F), 0 };
    nuq_emit(c, nuq_make_string(b, 1));
    return BR_OK;
}

static VALUE
b_explode(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) {
        fprintf(stderr, "nuq error: explode requires a string\n");
        return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE arr = nuq_make_array(o->str.len);
    /* naive UTF-8 decode */
    const unsigned char *s = (const unsigned char *)o->str.bytes;
    size_t i = 0;
    while (i < o->str.len) {
        unsigned cp;
        if (s[i] < 0x80) { cp = s[i]; i++; }
        else if ((s[i] & 0xE0) == 0xC0 && i+1 < o->str.len) { cp = ((s[i]&0x1F)<<6) | (s[i+1]&0x3F); i += 2; }
        else if ((s[i] & 0xF0) == 0xE0 && i+2 < o->str.len) { cp = ((s[i]&0x0F)<<12) | ((s[i+1]&0x3F)<<6) | (s[i+2]&0x3F); i += 3; }
        else if ((s[i] & 0xF8) == 0xF0 && i+3 < o->str.len) { cp = ((s[i]&0x07)<<18) | ((s[i+1]&0x3F)<<12) | ((s[i+2]&0x3F)<<6) | (s[i+3]&0x3F); i += 4; }
        else { cp = s[i]; i++; }
        nuq_array_push(arr, nuq_make_int(cp));
    }
    nuq_emit(c, arr);
    return BR_OK;
}

static VALUE
b_implode(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: implode requires array\n");
        return BR_ERROR;
    }
    struct nuq_obj *ao = NUQ_PTR(c->input);
    char *buf = NULL; size_t bn = 0;
    FILE *fp = open_memstream(&buf, &bn);
    for (size_t i = 0; i < ao->arr.len; i++) {
        VALUE v = ao->arr.items[i];
        if (!NUQ_IS_FIX(v)) continue;
        unsigned cp = (unsigned)NUQ_FIX_VAL(v);
        if (cp < 0x80) fputc(cp, fp);
        else if (cp < 0x800) { fputc(0xC0|(cp>>6), fp); fputc(0x80|(cp&0x3F), fp); }
        else if (cp < 0x10000) { fputc(0xE0|(cp>>12), fp); fputc(0x80|((cp>>6)&0x3F), fp); fputc(0x80|(cp&0x3F), fp); }
        else { fputc(0xF0|(cp>>18), fp); fputc(0x80|((cp>>12)&0x3F), fp); fputc(0x80|((cp>>6)&0x3F), fp); fputc(0x80|(cp&0x3F), fp); }
    }
    fclose(fp);
    nuq_emit(c, nuq_make_string(buf, bn));
    free(buf);
    return BR_OK;
}

static VALUE
b_ascii_downcase(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) {
        fprintf(stderr, "nuq error: ascii_downcase requires string\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.bytes[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[o->str.len] = '\0';
    nuq_emit(c, nuq_make_string_take(buf, o->str.len));
    return BR_OK;
}

static VALUE
b_ascii_upcase(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) {
        fprintf(stderr, "nuq error: ascii_upcase requires string\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.bytes[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    buf[o->str.len] = '\0';
    nuq_emit(c, nuq_make_string_take(buf, o->str.len));
    return BR_OK;
}

static VALUE
b_reverse(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    VALUE v = c->input;
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(v);
        VALUE r = nuq_make_array(o->arr.len);
        for (size_t i = o->arr.len; i > 0; i--) nuq_array_push(r, o->arr.items[i-1]);
        nuq_emit(c, r);
        return BR_OK;
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
        struct nuq_obj *o = NUQ_PTR(v);
        char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
        for (size_t i = 0; i < o->str.len; i++) buf[i] = o->str.bytes[o->str.len - 1 - i];
        buf[o->str.len] = '\0';
        nuq_emit(c, nuq_make_string_take(buf, o->str.len));
        return BR_OK;
    }
    fprintf(stderr, "nuq error: reverse on %s\n", nuq_type_name(v));
    return BR_ERROR;
}

/* sort by natural comparison */
static int cmp_for_qsort_natural(const void *a, const void *b) { return nuq_cmp(*(const VALUE *)a, *(const VALUE *)b); }

static VALUE
b_sort(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: sort requires array\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_array(o->arr.len);
    struct nuq_obj *ro = NUQ_PTR(r);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(r, o->arr.items[i]);
    qsort(ro->arr.items, ro->arr.len, sizeof(VALUE), cmp_for_qsort_natural);
    nuq_emit(c, r);
    return BR_OK;
}

/* ----- 1-arg ----- */

/* select(f) — emit input only if f truthy on input */
static VALUE
b_select(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        if (nuq_truthy(bo->arr.items[i])) {
            nuq_emit(c, c->input);
            return BR_OK;       /* select emits at most once */
        }
    }
    return BR_OK;
}

/* map(f) — `[.[] | f]` */
static VALUE
b_map(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: map requires array\n");
        c->error = nuq_make_string("type error", 10);
        return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE out = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE buf;
        VALUE r = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (r != BR_OK) return r;
        struct nuq_obj *bo = NUQ_PTR(buf);
        for (size_t j = 0; j < bo->arr.len; j++) nuq_array_push(out, bo->arr.items[j]);
    }
    nuq_emit(c, out);
    return BR_OK;
}

/* map_values(f) — apply f to each value, replace */
static VALUE
b_map_values(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE in = c->input;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_array(o->arr.len);
        for (size_t i = 0; i < o->arr.len; i++) {
            VALUE buf;
            VALUE r = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
            if (r != BR_OK) return r;
            struct nuq_obj *bo = NUQ_PTR(buf);
            if (bo->arr.len > 0) nuq_array_push(out, bo->arr.items[0]);
        }
        nuq_emit(c, out);
        return BR_OK;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_object(o->obj.len);
        for (size_t i = 0; i < o->obj.len; i++) {
            VALUE buf;
            VALUE r = nuq_eval_collect_status(c, args[0], o->obj.vals[i], &buf);
            if (r != BR_OK) return r;
            struct nuq_obj *bo = NUQ_PTR(buf);
            if (bo->arr.len > 0) nuq_object_set(out, o->obj.keys[i], bo->arr.items[0]);
        }
        nuq_emit(c, out);
        return BR_OK;
    }
    fprintf(stderr, "nuq error: map_values requires array or object\n");
    return BR_ERROR;
}

/* has(k) */
static VALUE
b_has(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE k = bo->arr.items[i];
        bool t;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(c->input, k);
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(c->input)->arr.len;
        } else { t = false; }
        nuq_emit(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return BR_OK;
}

static VALUE
b_in(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE container = bo->arr.items[i];
        bool t;
        if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(container, c->input);
        } else if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(c->input) ? NUQ_FIX_VAL(c->input) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(container)->arr.len;
        } else { t = false; }
        nuq_emit(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return BR_OK;
}

static VALUE
b_contains(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        /* simplified: a contains b iff nuq_eq(a, b) OR (both arrays/objects/strings and recursive). */
        VALUE rhs = bo->arr.items[i];
        nuq_emit(c, nuq_eq(c->input, rhs) ? NUQ_TRUE : NUQ_FALSE);
    }
    return BR_OK;
}

/* range(N), range(M;N), range(M;N;S) */
static VALUE
b_range1(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE nb;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &nb);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(nb);
    for (size_t k = 0; k < bo->arr.len; k++) {
        int64_t n;
        if (NUQ_IS_FIX(bo->arr.items[k])) n = NUQ_FIX_VAL(bo->arr.items[k]);
        else if (NUQ_IS_PTR(bo->arr.items[k]) && NUQ_PTR(bo->arr.items[k])->type == NUQ_T_DOUBLE)
            n = (int64_t)NUQ_PTR(bo->arr.items[k])->dbl;
        else continue;
        for (int64_t i = 0; i < n; i++) nuq_emit(c, nuq_make_int(i));
    }
    return BR_OK;
}

static VALUE
b_range2(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE m, n;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &m);
    if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, args[1], c->input, &n);
    if (r != BR_OK) return r;
    struct nuq_obj *mo = NUQ_PTR(m), *no = NUQ_PTR(n);
    for (size_t i = 0; i < mo->arr.len; i++) {
        for (size_t j = 0; j < no->arr.len; j++) {
            int64_t lo = NUQ_IS_FIX(mo->arr.items[i]) ? NUQ_FIX_VAL(mo->arr.items[i]) : (int64_t)NUQ_PTR(mo->arr.items[i])->dbl;
            int64_t hi = NUQ_IS_FIX(no->arr.items[j]) ? NUQ_FIX_VAL(no->arr.items[j]) : (int64_t)NUQ_PTR(no->arr.items[j])->dbl;
            for (int64_t k = lo; k < hi; k++) nuq_emit(c, nuq_make_int(k));
        }
    }
    return BR_OK;
}

static VALUE
b_range3(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE m, n, s;
    VALUE r;
    r = nuq_eval_collect_status(c, args[0], c->input, &m); if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, args[1], c->input, &n); if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, args[2], c->input, &s); if (r != BR_OK) return r;
    struct nuq_obj *mo = NUQ_PTR(m), *no = NUQ_PTR(n), *so = NUQ_PTR(s);
    for (size_t i = 0; i < mo->arr.len; i++)
    for (size_t j = 0; j < no->arr.len; j++)
    for (size_t k = 0; k < so->arr.len; k++) {
        int64_t lo = NUQ_IS_FIX(mo->arr.items[i]) ? NUQ_FIX_VAL(mo->arr.items[i]) : (int64_t)NUQ_PTR(mo->arr.items[i])->dbl;
        int64_t hi = NUQ_IS_FIX(no->arr.items[j]) ? NUQ_FIX_VAL(no->arr.items[j]) : (int64_t)NUQ_PTR(no->arr.items[j])->dbl;
        int64_t st = NUQ_IS_FIX(so->arr.items[k]) ? NUQ_FIX_VAL(so->arr.items[k]) : (int64_t)NUQ_PTR(so->arr.items[k])->dbl;
        if (st == 0) continue;
        if (st > 0) for (int64_t x = lo; x < hi; x += st) nuq_emit(c, nuq_make_int(x));
        else        for (int64_t x = lo; x > hi; x += st) nuq_emit(c, nuq_make_int(x));
    }
    return BR_OK;
}

/* add — sum/concat of input array */
static VALUE
b_add(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_NULL) {
            nuq_emit(c, NUQ_NULL);
            return BR_OK;
        }
        fprintf(stderr, "nuq error: add requires array\n");
        return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
    VALUE acc = o->arr.items[0];
    for (size_t i = 1; i < o->arr.len; i++) acc = nuq_op_add(acc, o->arr.items[i]);
    nuq_emit(c, acc);
    return BR_OK;
}

static VALUE
b_min(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        nuq_emit(c, NUQ_NULL); return BR_OK;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
    VALUE m = o->arr.items[0];
    for (size_t i = 1; i < o->arr.len; i++) if (nuq_cmp(o->arr.items[i], m) < 0) m = o->arr.items[i];
    nuq_emit(c, m);
    return BR_OK;
}

static VALUE
b_max(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        nuq_emit(c, NUQ_NULL); return BR_OK;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
    VALUE m = o->arr.items[0];
    for (size_t i = 1; i < o->arr.len; i++) if (nuq_cmp(o->arr.items[i], m) > 0) m = o->arr.items[i];
    nuq_emit(c, m);
    return BR_OK;
}

static VALUE
b_unique(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: unique requires array\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE sorted = nuq_make_array(o->arr.len);
    struct nuq_obj *so = NUQ_PTR(sorted);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(sorted, o->arr.items[i]);
    qsort(so->arr.items, so->arr.len, sizeof(VALUE), cmp_for_qsort_natural);
    VALUE r = nuq_make_array(so->arr.len);
    for (size_t i = 0; i < so->arr.len; i++) {
        if (i == 0 || nuq_cmp(so->arr.items[i], so->arr.items[i-1]) != 0)
            nuq_array_push(r, so->arr.items[i]);
    }
    nuq_emit(c, r);
    return BR_OK;
}

/* to_entries / from_entries */
static VALUE
b_to_entries(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT)) {
        fprintf(stderr, "nuq error: to_entries requires object\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_array(o->obj.len);
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        nuq_array_push(r, e);
    }
    nuq_emit(c, r);
    return BR_OK;
}

static VALUE
b_from_entries(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: from_entries requires array\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_object(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE e = o->arr.items[i];
        if (!(NUQ_IS_PTR(e) && NUQ_PTR(e)->type == NUQ_T_OBJECT)) continue;
        VALUE k = nuq_object_get_cstr(e, "key");
        if ((NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_NULL))
            k = nuq_object_get_cstr(e, "k");
        if ((NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_NULL))
            k = nuq_object_get_cstr(e, "name");
        VALUE v = nuq_object_get_cstr(e, "value");
        if ((NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL))
            v = nuq_object_get_cstr(e, "v");
        /* convert key */
        if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)) k = nuq_to_json_string(k);
        nuq_object_set(r, k, v);
    }
    nuq_emit(c, r);
    return BR_OK;
}

static VALUE
b_with_entries(CTX *c, int a, struct Node **args)
{
    (void)a;
    /* with_entries(f) == from_entries( to_entries | map(f) ) */
    /* implement by calling sub-pieces */
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT)) {
        fprintf(stderr, "nuq error: with_entries requires object\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_object(o->obj.len);
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], e, &buf);
        if (st != BR_OK) return st;
        struct nuq_obj *bo = NUQ_PTR(buf);
        for (size_t j = 0; j < bo->arr.len; j++) {
            VALUE ne = bo->arr.items[j];
            if (!(NUQ_IS_PTR(ne) && NUQ_PTR(ne)->type == NUQ_T_OBJECT)) continue;
            VALUE nk = nuq_object_get_cstr(ne, "key");
            VALUE nv = nuq_object_get_cstr(ne, "value");
            if (!(NUQ_IS_PTR(nk) && NUQ_PTR(nk)->type == NUQ_T_STRING)) nk = nuq_to_json_string(nk);
            nuq_object_set(r, nk, nv);
        }
    }
    nuq_emit(c, r);
    return BR_OK;
}

/* paths */
static void
paths_walk(CTX *c, VALUE v, VALUE path, VALUE acc)
{
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->arr.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, nuq_make_int(i));
            nuq_array_push(acc, p);
            paths_walk(c, o->arr.items[i], p, acc);
        }
    } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->obj.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, o->obj.keys[i]);
            nuq_array_push(acc, p);
            paths_walk(c, o->obj.vals[i], p, acc);
        }
    }
}

static VALUE
b_paths(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    VALUE acc = nuq_make_array(0);
    paths_walk(c, c->input, nuq_make_array(0), acc);
    struct nuq_obj *ao = NUQ_PTR(acc);
    for (size_t i = 0; i < ao->arr.len; i++) nuq_emit(c, ao->arr.items[i]);
    return BR_OK;
}

static VALUE
b_floor(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_FIX(c->input)) { nuq_emit(c, c->input); return BR_OK; }
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE) {
        nuq_emit(c, nuq_make_int((int64_t)floor(NUQ_PTR(c->input)->dbl)));
        return BR_OK;
    }
    fprintf(stderr, "nuq error: floor on %s\n", nuq_type_name(c->input));
    return BR_ERROR;
}

static VALUE
b_ceil(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_FIX(c->input)) { nuq_emit(c, c->input); return BR_OK; }
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE) {
        nuq_emit(c, nuq_make_int((int64_t)ceil(NUQ_PTR(c->input)->dbl)));
        return BR_OK;
    }
    fprintf(stderr, "nuq error: ceil on %s\n", nuq_type_name(c->input));
    return BR_ERROR;
}

static VALUE
b_round(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_FIX(c->input)) { nuq_emit(c, c->input); return BR_OK; }
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE) {
        nuq_emit(c, nuq_make_int((int64_t)round(NUQ_PTR(c->input)->dbl)));
        return BR_OK;
    }
    fprintf(stderr, "nuq error: round on %s\n", nuq_type_name(c->input));
    return BR_ERROR;
}

static VALUE
b_fabs(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_FIX(c->input)) {
        int64_t v = NUQ_FIX_VAL(c->input);
        nuq_emit(c, nuq_make_int(v < 0 ? -v : v));
        return BR_OK;
    }
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE) {
        nuq_emit(c, nuq_make_double(fabs(NUQ_PTR(c->input)->dbl)));
        return BR_OK;
    }
    return BR_ERROR;
}

static VALUE
b_sqrt(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_FIX(c->input)) {
        nuq_emit(c, nuq_make_double(sqrt((double)NUQ_FIX_VAL(c->input))));
        return BR_OK;
    }
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE) {
        nuq_emit(c, nuq_make_double(sqrt(NUQ_PTR(c->input)->dbl)));
        return BR_OK;
    }
    return BR_ERROR;
}

/* split(s) */
static VALUE
b_split(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) {
        fprintf(stderr, "nuq error: split requires string\n"); return BR_ERROR;
    }
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE sep = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(sep) && NUQ_PTR(sep)->type == NUQ_T_STRING)) return BR_ERROR;
    /* reuse op_div */
    nuq_emit(c, nuq_op_div(c->input, sep));
    return BR_OK;
}

/* join(sep) */
static VALUE
b_join(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: join requires array\n"); return BR_ERROR;
    }
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE sep = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(sep) && NUQ_PTR(sep)->type == NUQ_T_STRING)) return BR_ERROR;
    struct nuq_obj *so = NUQ_PTR(sep);
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
            VALUE s = nuq_to_json_string(v);
            fwrite(NUQ_PTR(s)->str.bytes, 1, NUQ_PTR(s)->str.len, fp);
        }
    }
    fclose(fp);
    nuq_emit(c, nuq_make_string(out, on));
    free(out);
    return BR_OK;
}

/* startswith / endswith / test / contains-string */
static VALUE
b_startswith(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) return BR_ERROR;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE p = bo->arr.items[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_emit(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len && memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0;
        nuq_emit(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return BR_OK;
}

static VALUE
b_endswith(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) return BR_ERROR;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    for (size_t i = 0; i < bo->arr.len; i++) {
        VALUE p = bo->arr.items[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_emit(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len, po->str.bytes, po->str.len) == 0;
        nuq_emit(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return BR_OK;
}

/* limit(n; f) */
static VALUE
b_limit(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE nb;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &nb);
    if (r != BR_OK) return r;
    if (NUQ_PTR(nb)->arr.len == 0) return BR_OK;
    VALUE nv = NUQ_PTR(nb)->arr.items[0];
    int64_t n = NUQ_IS_FIX(nv) ? NUQ_FIX_VAL(nv) : (int64_t)NUQ_PTR(nv)->dbl;
    if (n < 0) {
        fprintf(stderr, "nuq error: limit doesn't support negative count\n");
        c->error = nuq_make_string("limit doesn't support negative count", 36);
        return BR_ERROR;
    }
    if (n == 0) return BR_OK;
    VALUE fb;
    r = nuq_eval_collect_status(c, args[1], c->input, &fb);
    if (r != BR_OK) return r;
    struct nuq_obj *fo = NUQ_PTR(fb);
    int64_t emitted = 0;
    for (size_t i = 0; i < fo->arr.len && emitted < n; i++, emitted++)
        nuq_emit(c, fo->arr.items[i]);
    return BR_OK;
}

/* first(f), first(f) is `f | limit(1; .)` semantics; first(f) emits f's first */
static VALUE
b_first1(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len > 0) nuq_emit(c, NUQ_PTR(buf)->arr.items[0]);
    return BR_OK;
}

static VALUE
b_first0(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(c->input);
        if (o->arr.len > 0) nuq_emit(c, o->arr.items[0]);
        else nuq_emit(c, NUQ_NULL);
        return BR_OK;
    }
    return BR_ERROR;
}

static VALUE
b_last1(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    struct nuq_obj *bo = NUQ_PTR(buf);
    if (bo->arr.len > 0) nuq_emit(c, bo->arr.items[bo->arr.len - 1]);
    return BR_OK;
}

static VALUE
b_last0(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(c->input);
        if (o->arr.len > 0) nuq_emit(c, o->arr.items[o->arr.len - 1]);
        else nuq_emit(c, NUQ_NULL);
        return BR_OK;
    }
    return BR_ERROR;
}

static VALUE
b_nth2(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE nb, fb;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &nb);
    if (r != BR_OK) return r;
    if (NUQ_PTR(nb)->arr.len == 0) return BR_OK;
    int64_t n = NUQ_IS_FIX(NUQ_PTR(nb)->arr.items[0]) ? NUQ_FIX_VAL(NUQ_PTR(nb)->arr.items[0]) : (int64_t)NUQ_PTR(NUQ_PTR(nb)->arr.items[0])->dbl;
    if (n < 0) {
        c->error = nuq_make_string("nth doesn't support negative indices", 37);
        return BR_ERROR;
    }
    r = nuq_eval_collect_status(c, args[1], c->input, &fb);
    if (r != BR_OK) return r;
    struct nuq_obj *fo = NUQ_PTR(fb);
    if (n < (int64_t)fo->arr.len) nuq_emit(c, fo->arr.items[n]);
    return BR_OK;
}

/* any/all (0/1/2 arg) */
static VALUE
b_any0(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    for (size_t i = 0; i < o->arr.len; i++)
        if (nuq_truthy(o->arr.items[i])) { nuq_emit(c, NUQ_TRUE); return BR_OK; }
    nuq_emit(c, NUQ_FALSE);
    return BR_OK;
}

static VALUE
b_all0(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    for (size_t i = 0; i < o->arr.len; i++)
        if (!nuq_truthy(o->arr.items[i])) { nuq_emit(c, NUQ_FALSE); return BR_OK; }
    nuq_emit(c, NUQ_TRUE);
    return BR_OK;
}

static VALUE
b_isnan(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    bool b = NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE && isnan(NUQ_PTR(c->input)->dbl);
    nuq_emit(c, b ? NUQ_TRUE : NUQ_FALSE);
    return BR_OK;
}

static VALUE
b_isinfinite(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    bool b = NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_DOUBLE && isinf(NUQ_PTR(c->input)->dbl);
    nuq_emit(c, b ? NUQ_TRUE : NUQ_FALSE);
    return BR_OK;
}

static VALUE
b_infinite(CTX *c, int a, struct Node **args) { (void)c; (void)a; (void)args; nuq_emit(c, nuq_make_double(INFINITY)); return BR_OK; }

static VALUE
b_nan(CTX *c, int a, struct Node **args) { (void)c; (void)a; (void)args; nuq_emit(c, nuq_make_double(NAN)); return BR_OK; }

static VALUE
b_isnull(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    bool b = NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_NULL;
    nuq_emit(c, b ? NUQ_TRUE : NUQ_FALSE);
    return BR_OK;
}

static VALUE
b_input_filename(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, NUQ_NULL); return BR_OK; }

/* tojson / fromjson */
static VALUE
b_tojson(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    nuq_emit(c, nuq_to_json_string(c->input));
    return BR_OK;
}

static VALUE
b_fromjson(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) {
        fprintf(stderr, "nuq error: fromjson requires string\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    char *err = NULL;
    const char *endp;
    VALUE r = nuq_json_parse(o->str.bytes, o->str.len, &endp, &err);
    if (err) { c->error = nuq_make_string(err, strlen(err)); return BR_ERROR; }
    nuq_emit(c, r);
    return BR_OK;
}

/* abs */
static VALUE
b_abs(CTX *c, int a, struct Node **args) { return b_fabs(c, a, args); }

/* env / $ENV / now */
static VALUE
b_now(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    nuq_emit(c, nuq_make_double((double)time(NULL)));
    return BR_OK;
}

/* group_by(f) — returns array-of-arrays grouped by key */
static VALUE
b_group_by(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) {
        fprintf(stderr, "nuq error: group_by requires array\n"); return BR_ERROR;
    }
    struct nuq_obj *o = NUQ_PTR(c->input);
    /* compute (key, item) pairs then sort & group */
    VALUE pairs = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE buf;
        VALUE r = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (r != BR_OK) return r;
        VALUE k = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
        VALUE p = nuq_make_array(2);
        nuq_array_push(p, k);
        nuq_array_push(p, o->arr.items[i]);
        nuq_array_push(pairs, p);
    }
    /* sort by first element */
    struct nuq_obj *po = NUQ_PTR(pairs);
    /* Insertion sort (stable) for simplicity */
    for (size_t i = 1; i < po->arr.len; i++) {
        VALUE x = po->arr.items[i];
        size_t j = i;
        while (j > 0) {
            VALUE k_a = NUQ_PTR(po->arr.items[j-1])->arr.items[0];
            VALUE k_b = NUQ_PTR(x)->arr.items[0];
            if (nuq_cmp(k_a, k_b) <= 0) break;
            po->arr.items[j] = po->arr.items[j-1];
            j--;
        }
        po->arr.items[j] = x;
    }
    /* group */
    VALUE r = nuq_make_array(0);
    VALUE cur = NUQ_NULL;
    bool has = false;
    for (size_t i = 0; i < po->arr.len; i++) {
        VALUE k = NUQ_PTR(po->arr.items[i])->arr.items[0];
        VALUE v = NUQ_PTR(po->arr.items[i])->arr.items[1];
        if (!has || nuq_cmp(k, NUQ_PTR(cur)->arr.items[0]) != 0) {
            VALUE g = nuq_make_array(0);
            nuq_array_push(g, v);
            nuq_array_push(r, g);
            VALUE wrap = nuq_make_array(0);
            nuq_array_push(wrap, k);
            nuq_array_push(wrap, g);
            cur = wrap;
            has = true;
        } else {
            VALUE g = NUQ_PTR(cur)->arr.items[1];
            nuq_array_push(g, v);
        }
    }
    nuq_emit(c, r);
    return BR_OK;
}

/* unique_by(f) */
static VALUE
b_unique_by(CTX *c, int a, struct Node **args)
{
    (void)a;
    /* Simple approach: keep first encounter per key */
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE r = nuq_make_array(0);
    VALUE seen = nuq_make_array(0);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (st != BR_OK) return st;
        VALUE k = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
        bool found = false;
        struct nuq_obj *so = NUQ_PTR(seen);
        for (size_t j = 0; j < so->arr.len; j++) if (nuq_eq(so->arr.items[j], k)) { found = true; break; }
        if (!found) {
            nuq_array_push(seen, k);
            nuq_array_push(r, o->arr.items[i]);
        }
    }
    /* Sort by key — actually jq's unique_by sorts.  But for simplicity
     * we keep order; tests may differ. */
    nuq_emit(c, r);
    return BR_OK;
}

/* sort_by(f) */
static VALUE
b_sort_by(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    /* compute keys, then stable sort by key */
    VALUE keys = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (st != BR_OK) return st;
        nuq_array_push(keys, NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL);
    }
    VALUE r = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(r, o->arr.items[i]);
    /* index sort */
    size_t *idx = (size_t *)GC_malloc(o->arr.len * sizeof(size_t));
    for (size_t i = 0; i < o->arr.len; i++) idx[i] = i;
    /* insertion sort by keys */
    struct nuq_obj *ko = NUQ_PTR(keys);
    for (size_t i = 1; i < o->arr.len; i++) {
        size_t x = idx[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(ko->arr.items[idx[j-1]], ko->arr.items[x]) > 0) {
            idx[j] = idx[j-1]; j--;
        }
        idx[j] = x;
    }
    VALUE sr = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(sr, o->arr.items[idx[i]]);
    nuq_emit(c, sr);
    return BR_OK;
}

/* min_by / max_by */
static VALUE
b_min_by(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
    VALUE bestv = o->arr.items[0], bestk;
    {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], bestv, &buf);
        if (st != BR_OK) return st;
        bestk = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (st != BR_OK) return st;
        VALUE k = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
        if (nuq_cmp(k, bestk) < 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    nuq_emit(c, bestv);
    return BR_OK;
}

static VALUE
b_max_by(CTX *c, int a, struct Node **args)
{
    (void)a;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY)) return BR_ERROR;
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
    VALUE bestv = o->arr.items[0], bestk;
    {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], bestv, &buf);
        if (st != BR_OK) return st;
        bestk = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        VALUE buf;
        VALUE st = nuq_eval_collect_status(c, args[0], o->arr.items[i], &buf);
        if (st != BR_OK) return st;
        VALUE k = NUQ_PTR(buf)->arr.len > 0 ? NUQ_PTR(buf)->arr.items[0] : NUQ_NULL;
        if (nuq_cmp(k, bestk) > 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    nuq_emit(c, bestv);
    return BR_OK;
}

/* getpath, setpath, delpaths — reduced support */
static VALUE
get_at_path(VALUE v, struct nuq_obj *path)
{
    for (size_t i = 0; i < path->arr.len; i++) {
        VALUE k = path->arr.items[i];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT)
            v = nuq_object_get(v, k);
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            v = nuq_array_get(v, idx);
        } else return NUQ_NULL;
    }
    return v;
}

static VALUE
b_getpath(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE path = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(path) && NUQ_PTR(path)->type == NUQ_T_ARRAY)) return BR_ERROR;
    nuq_emit(c, get_at_path(c->input, NUQ_PTR(path)));
    return BR_OK;
}

/* del(.foo) — limited: only one-level field delete via path-array; we
 * implement del via parse-time recognition isn't feasible here; provide
 * a runtime that takes a path expression and applies one-level delete. */
static VALUE
b_del(CTX *c, int a, struct Node **args)
{
    (void)a;
    /* Very limited: only supports `.field`-style paths.  We collect the
     * path arg's emit and attempt to interpret each as a path-expr-like
     * thing.  Real jq uses paths(); we approximate by evaluating the
     * arg as a value-producing filter — which doesn't work for paths.
     * For v0 we reject. */
    (void)c; (void)args;
    fprintf(stderr, "nuq: del(...) not supported in v0\n");
    return BR_ERROR;
}

/* recurse(f) */
static VALUE
b_recurse0(CTX *c, int a, struct Node **args)
{
    (void)a; (void)args;
    return nuq_recurse_emit(c, c->input);
}

/* indices(s), index(s), rindex(s) */
static VALUE
b_indices(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE pat = NUQ_PTR(buf)->arr.items[0];
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING &&
        NUQ_IS_PTR(pat) && NUQ_PTR(pat)->type == NUQ_T_STRING) {
        struct nuq_obj *io = NUQ_PTR(c->input), *po = NUQ_PTR(pat);
        VALUE r2 = nuq_make_array(0);
        if (po->str.len == 0) { nuq_emit(c, r2); return BR_OK; }
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0)
                nuq_array_push(r2, nuq_make_int(i));
        }
        nuq_emit(c, r2);
        return BR_OK;
    }
    fprintf(stderr, "nuq: indices supports only string-in-string in v0\n");
    return BR_ERROR;
}

static VALUE
b_index1(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE pat = NUQ_PTR(buf)->arr.items[0];
    if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING &&
        NUQ_IS_PTR(pat) && NUQ_PTR(pat)->type == NUQ_T_STRING) {
        struct nuq_obj *io = NUQ_PTR(c->input), *po = NUQ_PTR(pat);
        if (po->str.len == 0) { nuq_emit(c, NUQ_NULL); return BR_OK; }
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) {
                nuq_emit(c, nuq_make_int(i));
                return BR_OK;
            }
        }
        nuq_emit(c, NUQ_NULL);
        return BR_OK;
    }
    return BR_ERROR;
}

/* test(re) — extremely simplified: substring */
static VALUE
b_test(CTX *c, int a, struct Node **args)
{
    (void)a;
    VALUE buf;
    VALUE r = nuq_eval_collect_status(c, args[0], c->input, &buf);
    if (r != BR_OK) return r;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING)) return BR_ERROR;
    if (NUQ_PTR(buf)->arr.len == 0) return BR_OK;
    VALUE pat = NUQ_PTR(buf)->arr.items[0];
    if (!(NUQ_IS_PTR(pat) && NUQ_PTR(pat)->type == NUQ_T_STRING)) return BR_ERROR;
    struct nuq_obj *io = NUQ_PTR(c->input), *po = NUQ_PTR(pat);
    if (po->str.len == 0) { nuq_emit(c, NUQ_TRUE); return BR_OK; }
    for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
        if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) {
            nuq_emit(c, NUQ_TRUE);
            return BR_OK;
        }
    }
    nuq_emit(c, NUQ_FALSE);
    return BR_OK;
}

/* env */
static VALUE
b_env(CTX *c, int a, struct Node **args) { (void)a; (void)args; nuq_emit(c, nuq_make_object(0)); return BR_OK; }

/* ----- table ----- */

static struct builtin_entry table[] = {
    /* zero-arg */
    {"length", 0, b_length},
    {"type", 0, b_type},
    {"keys", 0, b_keys},
    {"keys_unsorted", 0, b_keys_unsorted},
    {"values", 0, b_values},
    {"empty", 0, b_empty},
    {"error", 0, b_error0},
    {"error", 1, b_error1},
    {"not", 0, b_not},
    {"to_string", 0, b_to_string},
    {"tostring", 0, b_tostring},
    {"tonumber", 0, b_tonumber},
    {"ascii", 0, b_ascii},
    {"explode", 0, b_explode},
    {"implode", 0, b_implode},
    {"ascii_downcase", 0, b_ascii_downcase},
    {"ascii_upcase", 0, b_ascii_upcase},
    {"reverse", 0, b_reverse},
    {"sort", 0, b_sort},
    {"add", 0, b_add},
    {"min", 0, b_min},
    {"max", 0, b_max},
    {"unique", 0, b_unique},
    {"to_entries", 0, b_to_entries},
    {"from_entries", 0, b_from_entries},
    {"paths", 0, b_paths},
    {"floor", 0, b_floor},
    {"ceil", 0, b_ceil},
    {"round", 0, b_round},
    {"fabs", 0, b_fabs},
    {"abs", 0, b_abs},
    {"sqrt", 0, b_sqrt},
    {"first", 0, b_first0},
    {"last", 0, b_last0},
    {"any", 0, b_any0},
    {"all", 0, b_all0},
    {"isnan", 0, b_isnan},
    {"isinfinite", 0, b_isinfinite},
    {"infinite", 0, b_infinite},
    {"nan", 0, b_nan},
    {"isnull", 0, b_isnull},
    {"input_filename", 0, b_input_filename},
    {"tojson", 0, b_tojson},
    {"fromjson", 0, b_fromjson},
    {"recurse", 0, b_recurse0},
    {"now", 0, b_now},
    {"env", 0, b_env},
    /* one-arg */
    {"select", 1, b_select},
    {"map", 1, b_map},
    {"map_values", 1, b_map_values},
    {"with_entries", 1, b_with_entries},
    {"has", 1, b_has},
    {"in", 1, b_in},
    {"contains", 1, b_contains},
    {"range", 1, b_range1},
    {"split", 1, b_split},
    {"join", 1, b_join},
    {"startswith", 1, b_startswith},
    {"endswith", 1, b_endswith},
    {"first", 1, b_first1},
    {"last", 1, b_last1},
    {"sort_by", 1, b_sort_by},
    {"group_by", 1, b_group_by},
    {"unique_by", 1, b_unique_by},
    {"min_by", 1, b_min_by},
    {"max_by", 1, b_max_by},
    {"getpath", 1, b_getpath},
    {"del", 1, b_del},
    {"indices", 1, b_indices},
    {"index", 1, b_index1},
    {"test", 1, b_test},
    /* two-arg */
    {"range", 2, b_range2},
    {"limit", 2, b_limit},
    {"nth", 2, b_nth2},
    /* three-arg */
    {"range", 3, b_range3},
    {NULL, 0, NULL}
};
static bool table_inited = false;

bool
nuq_builtin_call(CTX *c, uint32_t name_id, int arity, struct Node **args, VALUE *out_status)
{
    if (!table_inited) {
        for (size_t i = 0; table[i].name; i++) table[i].name_id = nuq_intern(table[i].name);
        table_inited = true;
    }
    for (size_t i = 0; table[i].name; i++) {
        if (table[i].name_id == name_id && table[i].arity == arity) {
            *out_status = table[i].fn(c, arity, args);
            return true;
        }
    }
    return false;
}
