/*
 * builtin.c — value-level helpers for built-in functions.
 *
 * Each helper takes a VALUE (the input) and returns a VALUE (the
 * single result).  The NODE_DEF for each builtin in node.def wraps
 * the result into a 1-element nuq_array (or in some cases produces
 * a multi-element stream directly).
 *
 * Builtins that take a sub-expression (`map(f)`, `range(N)`, `select(f)`)
 * live in runtime.c (`nuq_*_eval`) because they need to call EVAL on
 * the body.
 */
#include "context.h"
#include "node.h"
#include <math.h>

/* Shared kernel of `add` over a flat (items[], len) view.  Used by
 * both `nuq_builtin_add` (which dereferences a NUQ_T_ARRAY input)
 * and `node_emit_fold_add` (the fused `[body] | add` node, which
 * passes the EMIT pool slice directly).  Splitting these saves one
 * outer array allocation in the fused path. */
VALUE
nuq_add_fold_items(const VALUE *items, size_t len)
{
    if (len == 0) return NUQ_NULL;

    /* Type-classify all elements once. */
    bool all_arrays = true, all_objects = true, all_strings = true;
    size_t total_arr = 0, total_str = 0;
    for (size_t i = 0; i < len; i++) {
        VALUE v = items[i];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            total_arr += NUQ_PTR(v)->arr.len;
            all_objects = all_strings = false;
        } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            all_arrays = all_strings = false;
        } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            total_str += NUQ_PTR(v)->str.len;
            all_arrays = all_objects = false;
        } else {
            all_arrays = all_objects = all_strings = false;
        }
    }
    if (all_arrays) {
        VALUE r = nuq_make_array(total_arr);
        for (size_t i = 0; i < len; i++) {
            struct nuq_obj *e = NUQ_PTR(items[i]);
            for (size_t j = 0; j < e->arr.len; j++) nuq_array_push(r, e->arr.items[j]);
        }
        return r;
    }
    if (all_strings) {
        char *buf = (char *)GC_malloc_atomic(total_str + 1);
        size_t bp = 0;
        for (size_t i = 0; i < len; i++) {
            struct nuq_obj *e = NUQ_PTR(items[i]);
            memcpy(buf + bp, e->str.bytes, e->str.len);
            bp += e->str.len;
        }
        buf[bp] = '\0';
        return nuq_make_string_take(buf, bp);
    }
    if (all_objects) {
        /* Single fresh object built directly — avoids the pairwise
         * nuq_op_add → nuq_clone cascade that makes the naive fold
         * O(n²) (each clone copies the growing accumulator). */
        size_t total_kv = 0;
        for (size_t i = 0; i < len; i++) total_kv += NUQ_PTR(items[i])->obj.len;
        VALUE r = nuq_make_object(total_kv > 4 ? total_kv : 4);
        for (size_t i = 0; i < len; i++) {
            struct nuq_obj *e = NUQ_PTR(items[i]);
            for (size_t j = 0; j < e->obj.len; j++)
                nuq_object_set(r, e->obj.keys[j], e->obj.vals[j]);
        }
        return r;
    }
    /* Slow path: pairwise add (handles numbers, mixed). */
    VALUE acc = items[0];
    for (size_t i = 1; i < len; i++) acc = nuq_op_add(acc, items[i]);
    return acc;
}

VALUE
nuq_builtin_add(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_NULL) return NUQ_NULL;
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    return nuq_add_fold_items(o->arr.items, o->arr.len);
}

VALUE
nuq_builtin_min(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) return NUQ_NULL;
    struct nuq_obj *o = NUQ_PTR(input);
    if (o->arr.len == 0) return NUQ_NULL;
    VALUE m = o->arr.items[0];
    for (size_t i = 1; i < o->arr.len; i++)
        if (nuq_cmp(o->arr.items[i], m) < 0) m = o->arr.items[i];
    return m;
}

VALUE
nuq_builtin_max(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) return NUQ_NULL;
    struct nuq_obj *o = NUQ_PTR(input);
    if (o->arr.len == 0) return NUQ_NULL;
    VALUE m = o->arr.items[0];
    for (size_t i = 1; i < o->arr.len; i++)
        if (nuq_cmp(o->arr.items[i], m) > 0) m = o->arr.items[i];
    return m;
}

static int cmp_natural_for_qsort(const void *a, const void *b) { return nuq_cmp(*(const VALUE *)a, *(const VALUE *)b); }

VALUE
nuq_builtin_sort(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE r = nuq_make_array(o->arr.len);
    struct nuq_obj *ro = NUQ_PTR(r);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(r, o->arr.items[i]);
    qsort(ro->arr.items, ro->arr.len, sizeof(VALUE), cmp_natural_for_qsort);
    return r;
}

VALUE
nuq_builtin_reverse(VALUE input)
{
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(input);
        VALUE r = nuq_make_array(o->arr.len);
        for (size_t i = o->arr.len; i > 0; i--) nuq_array_push(r, o->arr.items[i-1]);
        return r;
    }
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING) {
        struct nuq_obj *o = NUQ_PTR(input);
        char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
        for (size_t i = 0; i < o->str.len; i++) buf[i] = o->str.bytes[o->str.len - 1 - i];
        buf[o->str.len] = '\0';
        return nuq_make_string_take(buf, o->str.len);
    }
    nuq_helper_error("reverse on %s", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_unique(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE sorted = nuq_make_array(o->arr.len);
    struct nuq_obj *so = NUQ_PTR(sorted);
    for (size_t i = 0; i < o->arr.len; i++) nuq_array_push(sorted, o->arr.items[i]);
    qsort(so->arr.items, so->arr.len, sizeof(VALUE), cmp_natural_for_qsort);
    VALUE r = nuq_make_array(so->arr.len);
    for (size_t i = 0; i < so->arr.len; i++) {
        if (i == 0 || nuq_cmp(so->arr.items[i], so->arr.items[i-1]) != 0)
            nuq_array_push(r, so->arr.items[i]);
    }
    return r;
}

VALUE
nuq_builtin_to_entries(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_OBJECT)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE r = nuq_make_array(o->obj.len);
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        nuq_array_push(r, e);
    }
    return r;
}

VALUE
nuq_builtin_from_entries(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE r = nuq_make_object(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE e = o->arr.items[i];
        if (!(NUQ_IS_PTR(e) && NUQ_PTR(e)->type == NUQ_T_OBJECT)) continue;
        /* jq accepts any of {key|Key|k|name|Name} for the key field
         * and {value|Value|v} for the value, falling back in that
         * priority order. */
        static const char *const k_names[] = { "key", "Key", "k", "name", "Name" };
        static const char *const v_names[] = { "value", "Value", "v" };
        VALUE k = NUQ_NULL;
        for (size_t kn = 0; kn < sizeof(k_names)/sizeof(*k_names); kn++) {
            VALUE candidate = nuq_object_get_cstr(e, k_names[kn]);
            if (!(NUQ_IS_PTR(candidate) && NUQ_PTR(candidate)->type == NUQ_T_NULL)) {
                k = candidate; break;
            }
        }
        VALUE v = NUQ_NULL;
        for (size_t vn = 0; vn < sizeof(v_names)/sizeof(*v_names); vn++) {
            VALUE candidate = nuq_object_get_cstr(e, v_names[vn]);
            if (!(NUQ_IS_PTR(candidate) && NUQ_PTR(candidate)->type == NUQ_T_NULL)) {
                v = candidate; break;
            }
        }
        if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)) k = nuq_to_json_string(k);
        nuq_object_set(r, k, v);
    }
    return r;
}

VALUE
nuq_builtin_floor(VALUE input)
{
    if (NUQ_IS_FIX(input)) return input;
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_int((int64_t)floor(NUQ_PTR(input)->dbl));
    nuq_helper_error("floor on %s", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_ceil(VALUE input)
{
    if (NUQ_IS_FIX(input)) return input;
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_int((int64_t)ceil(NUQ_PTR(input)->dbl));
    nuq_helper_error("ceil on %s", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_round(VALUE input)
{
    if (NUQ_IS_FIX(input)) return input;
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_int((int64_t)round(NUQ_PTR(input)->dbl));
    nuq_helper_error("round on %s", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_fabs(VALUE input)
{
    if (NUQ_IS_FIX(input)) {
        int64_t v = NUQ_FIX_VAL(input);
        return nuq_make_int(v < 0 ? -v : v);
    }
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_double(fabs(NUQ_PTR(input)->dbl));
    /* jq: `abs` is identity for non-numeric values. */
    return input;
}

VALUE
nuq_builtin_sqrt(VALUE input)
{
    if (NUQ_IS_FIX(input)) return nuq_make_double(sqrt((double)NUQ_FIX_VAL(input)));
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_double(sqrt(NUQ_PTR(input)->dbl));
    return NUQ_NULL;
}

VALUE
nuq_builtin_explode(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE arr = nuq_make_array(o->str.len);
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
    return arr;
}

VALUE
nuq_builtin_implode(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        nuq_helper_error("implode input must be an array");
        return NUQ_NULL;
    }
    struct nuq_obj *ao = NUQ_PTR(input);
    char *buf = NULL; size_t bn = 0;
    FILE *fp = open_memstream(&buf, &bn);
    for (size_t i = 0; i < ao->arr.len; i++) {
        VALUE v = ao->arr.items[i];
        int64_t cpv;
        if (NUQ_IS_FIX(v)) cpv = NUQ_FIX_VAL(v);
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) {
            double d = NUQ_PTR(v)->dbl;
            if (isnan(d) || isinf(d)) {
                fclose(fp); free(buf);
                char dd[80];
                nuq_value_descr(v, dd, sizeof(dd));
                nuq_helper_error("%s can't be imploded, unicode codepoint needs to be numeric", dd);
                return NUQ_NULL;
            }
            cpv = (int64_t)d;       /* truncate toward zero */
        } else {
            fclose(fp); free(buf);
            char dd[80];
            nuq_value_descr(v, dd, sizeof(dd));
            nuq_helper_error("%s can't be imploded, unicode codepoint needs to be numeric", dd);
            return NUQ_NULL;
        }
        /* Out-of-range or UTF-16 surrogate halves become U+FFFD. */
        unsigned cp;
        if (cpv < 0 || cpv > 0x10FFFF || (cpv >= 0xD800 && cpv <= 0xDFFF))
            cp = 0xFFFD;
        else cp = (unsigned)cpv;
        if (cp < 0x80) fputc(cp, fp);
        else if (cp < 0x800) { fputc(0xC0|(cp>>6), fp); fputc(0x80|(cp&0x3F), fp); }
        else if (cp < 0x10000) { fputc(0xE0|(cp>>12), fp); fputc(0x80|((cp>>6)&0x3F), fp); fputc(0x80|(cp&0x3F), fp); }
        else { fputc(0xF0|(cp>>18), fp); fputc(0x80|((cp>>12)&0x3F), fp); fputc(0x80|((cp>>6)&0x3F), fp); fputc(0x80|(cp&0x3F), fp); }
    }
    fclose(fp);
    VALUE r = nuq_make_string(buf, bn);
    free(buf);
    return r;
}

VALUE
nuq_builtin_ascii_upcase(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.bytes[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    buf[o->str.len] = '\0';
    return nuq_make_string_take(buf, o->str.len);
}

VALUE
nuq_builtin_ascii_downcase(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) {
        nuq_helper_error("");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.bytes[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[o->str.len] = '\0';
    return nuq_make_string_take(buf, o->str.len);
}

bool
nuq_builtin_fromjson(VALUE input, VALUE *out)
{
    return nuq_builtin_fromjson_err(input, out, NULL);
}

/* Variant that surfaces the parse error string so callers can include
 * it in the error message (jq emits a detailed `Invalid ...` message). */
bool
nuq_builtin_fromjson_err(VALUE input, VALUE *out, char **err_out)
{
    if (err_out) *err_out = NULL;
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) return false;
    struct nuq_obj *o = NUQ_PTR(input);
    char *err = NULL;
    const char *endp;
    *out = nuq_json_parse(o->str.bytes, o->str.len, &endp, &err);
    if (err != NULL) {
        if (err_out) *err_out = err;
        return false;
    }
    /* Strict: anything past the parsed value (besides whitespace) is
     * an error.  Otherwise `"NaN1"` parses NaN then leaves trailing 1. */
    while (endp < o->str.bytes + o->str.len) {
        char c = *endp;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            if (err_out) {
                /* jq emits "Invalid numeric literal at EOF at line L,
                 * column C (while parsing '<input>')". The column
                 * points at the trailing-garbage offset. */
                /* jq reports column = length of source — position of
                 * the last char (1-indexed). */
                size_t col = o->str.len;
                if (col == 0) col = 1;
                char buf[256];
                /* trim source for the suffix */
                char head[80];
                size_t copy = o->str.len < sizeof(head) - 4
                              ? o->str.len : sizeof(head) - 4;
                memcpy(head, o->str.bytes, copy);
                if (copy < o->str.len) {
                    head[copy++] = '.'; head[copy++] = '.'; head[copy++] = '.';
                }
                head[copy] = '\0';
                snprintf(buf, sizeof(buf),
                         "Invalid numeric literal at EOF at line 1, column %zu"
                         " (while parsing '%s')",
                         col, head);
                size_t bn = strlen(buf);
                char *r = (char *)GC_malloc_atomic(bn + 1);
                memcpy(r, buf, bn + 1);
                *err_out = r;
            }
            return false;
        }
        endp++;
    }
    return true;
}

void
nuq_recurse_collect(VALUE r, VALUE v)
{
    nuq_array_push(r, v);
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            for (size_t i = 0; i < o->arr.len; i++) nuq_recurse_collect(r, o->arr.items[i]);
        } else if (o->type == NUQ_T_OBJECT) {
            for (size_t i = 0; i < o->obj.len; i++) nuq_recurse_collect(r, o->obj.vals[i]);
        }
    }
}

static void
paths_walk(VALUE r, VALUE v, VALUE path)
{
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->arr.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, nuq_make_int(i));
            nuq_array_push(r, p);
            paths_walk(r, o->arr.items[i], p);
        }
    } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(v);
        for (size_t i = 0; i < o->obj.len; i++) {
            VALUE p = nuq_clone(path);
            nuq_array_push(p, o->obj.keys[i]);
            nuq_array_push(r, p);
            paths_walk(r, o->obj.vals[i], p);
        }
    }
}

void
nuq_paths_collect(VALUE r, VALUE v)
{
    paths_walk(r, v, nuq_make_array(0));
}

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
