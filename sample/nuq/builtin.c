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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: add requires array\n");
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: sort requires array\n");
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
    if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: reverse on %s\n", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_unique(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_ARRAY)) {
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: unique requires array\n");
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: to_entries requires object\n");
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: from_entries requires array\n");
        return NUQ_NULL;
    }
    struct nuq_obj *o = NUQ_PTR(input);
    VALUE r = nuq_make_object(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE e = o->arr.items[i];
        if (!(NUQ_IS_PTR(e) && NUQ_PTR(e)->type == NUQ_T_OBJECT)) continue;
        VALUE k = nuq_object_get_cstr(e, "key");
        if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_NULL) k = nuq_object_get_cstr(e, "k");
        if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_NULL) k = nuq_object_get_cstr(e, "name");
        VALUE v = nuq_object_get_cstr(e, "value");
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) v = nuq_object_get_cstr(e, "v");
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
    if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: floor on %s\n", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_ceil(VALUE input)
{
    if (NUQ_IS_FIX(input)) return input;
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_int((int64_t)ceil(NUQ_PTR(input)->dbl));
    if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: ceil on %s\n", nuq_type_name(input));
    return NUQ_NULL;
}

VALUE
nuq_builtin_round(VALUE input)
{
    if (NUQ_IS_FIX(input)) return input;
    if (NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_DOUBLE)
        return nuq_make_int((int64_t)round(NUQ_PTR(input)->dbl));
    if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: round on %s\n", nuq_type_name(input));
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
    return NUQ_NULL;
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: explode requires string\n");
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: implode requires array\n");
        return NUQ_NULL;
    }
    struct nuq_obj *ao = NUQ_PTR(input);
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
    VALUE r = nuq_make_string(buf, bn);
    free(buf);
    return r;
}

VALUE
nuq_builtin_ascii_upcase(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) {
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: ascii_upcase requires string\n");
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
        if (!nuq_suppress_error_print) fprintf(stderr, "nuq error: ascii_downcase requires string\n");
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
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) return false;
    struct nuq_obj *o = NUQ_PTR(input);
    char *err = NULL;
    const char *endp;
    *out = nuq_json_parse(o->str.bytes, o->str.len, &endp, &err);
    return err == NULL;
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
