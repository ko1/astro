// astr runtime — heap-object allocators, R-flavoured printing,
// vector / string / list helpers.  Linked once into the main astr
// binary; not part of any generated SD .c (those see only the
// inline arithmetic in context.h plus extern hooks for the heap path).

#include "context.h"

#include <ctype.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Singletons.
// ---------------------------------------------------------------------------

struct astr_obj ASTR_NA_OBJ   = { .type = ASTR_T_NA   };
struct astr_obj ASTR_NULL_OBJ = { .type = ASTR_T_NULL };

// ---------------------------------------------------------------------------
// Heap allocators.  GC_malloc traces the returned region for pointers;
// GC_malloc_atomic skips tracing (use it for raw byte buffers and
// double[] payloads).
// ---------------------------------------------------------------------------

struct astr_obj *
astr_alloc(int type)
{
    struct astr_obj *o = (struct astr_obj *)GC_malloc(sizeof(struct astr_obj));
    o->type = type;
    return o;
}

VALUE
astr_make_float(double d)
{
    struct astr_obj *o = astr_alloc(ASTR_T_FLOAT);
    o->dbl = d;
    return ASTR_OBJ_VAL(o);
}

VALUE
astr_make_int(int64_t v)
{
    if (v >= ASTR_FIX_MIN && v <= ASTR_FIX_MAX) return ASTR_FIX(v);
    return astr_make_float((double)v);
}

VALUE
astr_make_string(const char *s, size_t len)
{
    struct astr_obj *o = astr_alloc(ASTR_T_STRING);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return ASTR_OBJ_VAL(o);
}

VALUE
astr_make_numvec_n(size_t n)
{
    struct astr_obj *o = astr_alloc(ASTR_T_NUM_VEC);
    o->numvec.items = (double *)GC_malloc_atomic(sizeof(double) * (n ? n : 1));
    o->numvec.len = n;
    o->numvec.capa = n ? n : 1;
    if (n) memset(o->numvec.items, 0, sizeof(double) * n);
    return ASTR_OBJ_VAL(o);
}

// Build a numeric vector from a VALUE list — used by `c(...)`.  Each
// item is coerced to double; nested vectors flatten one level (R does
// arbitrary recursion, but one level covers the common case).
VALUE
astr_make_numvec_from(const VALUE *items, size_t n)
{
    // First pass: count flattened length.
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        VALUE v = items[i];
        if (ASTR_IS_PTR(v)) {
            struct astr_obj *o = ASTR_PTR(v);
            if (o->type == ASTR_T_NUM_VEC) { total += o->numvec.len; continue; }
            if (o->type == ASTR_T_INT_VEC) { total += o->intvec.len; continue; }
        }
        total++;
    }

    VALUE result = astr_make_numvec_n(total);
    struct astr_obj *out = ASTR_PTR(result);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        VALUE v = items[i];
        if (ASTR_IS_PTR(v)) {
            struct astr_obj *o = ASTR_PTR(v);
            if (o->type == ASTR_T_NUM_VEC) {
                memcpy(out->numvec.items + k, o->numvec.items, sizeof(double) * o->numvec.len);
                k += o->numvec.len;
                continue;
            }
            if (o->type == ASTR_T_INT_VEC) {
                for (size_t j = 0; j < o->intvec.len; j++)
                    out->numvec.items[k++] = (double)o->intvec.items[j];
                continue;
            }
        }
        out->numvec.items[k++] = astr_to_double(v);
    }
    return result;
}

VALUE
astr_make_intvec_range(int64_t start, int64_t stop)
{
    int64_t step = (start <= stop) ? 1 : -1;
    int64_t n = (stop - start) * step + 1;
    if (n < 0) n = 0;
    struct astr_obj *o = astr_alloc(ASTR_T_INT_VEC);
    o->intvec.items = (int64_t *)GC_malloc_atomic(sizeof(int64_t) * (n ? (size_t)n : 1));
    o->intvec.len = (size_t)n;
    o->intvec.capa = (size_t)(n ? n : 1);
    int64_t v = start;
    for (int64_t i = 0; i < n; i++) {
        o->intvec.items[i] = v;
        v += step;
    }
    return ASTR_OBJ_VAL(o);
}

VALUE
astr_make_list(VALUE *items, size_t n)
{
    struct astr_obj *o = astr_alloc(ASTR_T_LIST);
    o->lst.items = (VALUE *)GC_malloc(sizeof(VALUE) * (n ? n : 1));
    o->lst.len = n;
    o->lst.capa = n ? n : 1;
    if (items && n) memcpy(o->lst.items, items, sizeof(VALUE) * n);
    return ASTR_OBJ_VAL(o);
}

// ---------------------------------------------------------------------------
// Arithmetic slow paths.  Called by the inline fast paths in context.h
// when at least one operand isn't a fixnum.  Vectors broadcast against
// scalars (R "recycling rule"); two vectors must have compatible lengths
// (longer length must be a multiple of shorter — recycling handled).
// ---------------------------------------------------------------------------

static bool
is_vec(VALUE v, size_t *out_len, double **out_doubles, int64_t **out_ints)
{
    *out_doubles = NULL; *out_ints = NULL; *out_len = 0;
    if (!ASTR_IS_PTR(v)) return false;
    struct astr_obj *o = ASTR_PTR(v);
    if (o->type == ASTR_T_NUM_VEC) { *out_doubles = o->numvec.items; *out_len = o->numvec.len; return true; }
    if (o->type == ASTR_T_INT_VEC) { *out_ints    = o->intvec.items; *out_len = o->intvec.len; return true; }
    return false;
}

static double
elt_as_double(double *dv, int64_t *iv, size_t i)
{
    return dv ? dv[i] : (double)iv[i];
}

typedef double (*scalar_op2)(double, double);
typedef double (*scalar_op1)(double);

static VALUE
do_binop_double(VALUE a, VALUE b, scalar_op2 op)
{
    size_t la, lb;
    double *da, *db;
    int64_t *ia, *ib;
    bool av = is_vec(a, &la, &da, &ia);
    bool bv = is_vec(b, &lb, &db, &ib);

    if (av || bv) {
        size_t na = av ? la : 1;
        size_t nb = bv ? lb : 1;
        size_t n  = na > nb ? na : nb;
        VALUE result = astr_make_numvec_n(n);
        struct astr_obj *out = ASTR_PTR(result);
        for (size_t i = 0; i < n; i++) {
            double x = av ? elt_as_double(da, ia, i % na) : astr_to_double(a);
            double y = bv ? elt_as_double(db, ib, i % nb) : astr_to_double(b);
            out->numvec.items[i] = op(x, y);
        }
        return result;
    }
    return astr_make_float(op(astr_to_double(a), astr_to_double(b)));
}

static double sd_add(double x, double y) { return x + y; }
static double sd_sub(double x, double y) { return x - y; }
static double sd_mul(double x, double y) { return x * y; }
static double sd_div(double x, double y) { return x / y; }
static double sd_pow(double x, double y) { return pow(x, y); }
static double sd_idiv(double x, double y) { return floor(x / y); }
static double sd_mod(double x, double y) { return x - y * floor(x / y); }

VALUE astr_add_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_add); }
VALUE astr_sub_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_sub); }
VALUE astr_mul_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_mul); }
VALUE astr_div_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_div); }
VALUE astr_pow_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_pow); }
VALUE astr_idiv_slow(VALUE a, VALUE b) { return do_binop_double(a, b, sd_idiv); }
VALUE astr_mod_slow(VALUE a, VALUE b)  { return do_binop_double(a, b, sd_mod); }

VALUE
astr_neg_slow(VALUE a)
{
    size_t la;
    double *da;
    int64_t *ia;
    if (is_vec(a, &la, &da, &ia)) {
        VALUE result = astr_make_numvec_n(la);
        struct astr_obj *out = ASTR_PTR(result);
        for (size_t i = 0; i < la; i++) out->numvec.items[i] = -elt_as_double(da, ia, i);
        return result;
    }
    return astr_make_float(-astr_to_double(a));
}

// ---------------------------------------------------------------------------
// Equality / comparison.
// ---------------------------------------------------------------------------

bool
astr_eq(VALUE a, VALUE b)
{
    if (a == b) return true;
    if (ASTR_IS_FIX(a) && ASTR_IS_FIX(b)) return false;       // same bits already checked
    // Strings: content compare.
    if (ASTR_IS_PTR(a) && ASTR_IS_PTR(b)) {
        struct astr_obj *oa = ASTR_PTR(a);
        struct astr_obj *ob = ASTR_PTR(b);
        if (oa->type == ASTR_T_STRING && ob->type == ASTR_T_STRING) {
            return oa->str.len == ob->str.len &&
                   memcmp(oa->str.chars, ob->str.chars, oa->str.len) == 0;
        }
        // String vs numeric ⇒ false.
        if ((oa->type == ASTR_T_STRING) != (ob->type == ASTR_T_STRING)) return false;
    }
    // Otherwise treat both as numeric.
    return astr_to_double(a) == astr_to_double(b);
}

int
astr_cmp(VALUE a, VALUE b)
{
    if (LIKELY(ASTR_IS_FIX(a) && ASTR_IS_FIX(b))) {
        int64_t la = ASTR_FIX_VAL(a), lb = ASTR_FIX_VAL(b);
        return (la > lb) - (la < lb);
    }
    // Strings: lexicographic.
    if (ASTR_IS_PTR(a) && ASTR_IS_PTR(b)) {
        struct astr_obj *oa = ASTR_PTR(a);
        struct astr_obj *ob = ASTR_PTR(b);
        if (oa->type == ASTR_T_STRING && ob->type == ASTR_T_STRING) {
            size_t n = oa->str.len < ob->str.len ? oa->str.len : ob->str.len;
            int c = memcmp(oa->str.chars, ob->str.chars, n);
            if (c != 0) return c < 0 ? -1 : 1;
            return (oa->str.len > ob->str.len) - (oa->str.len < ob->str.len);
        }
    }
    double da = astr_to_double(a), db = astr_to_double(b);
    return (da > db) - (da < db);
}

// ---------------------------------------------------------------------------
// Length / subscript.
// ---------------------------------------------------------------------------

size_t
astr_length(VALUE v)
{
    if (ASTR_IS_FIX(v)) return 1;
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:   return 1;
      case ASTR_T_STRING:  return 1;        // R: length("abc") == 1, nchar == 3
      case ASTR_T_NUM_VEC: return o->numvec.len;
      case ASTR_T_INT_VEC: return o->intvec.len;
      case ASTR_T_LIST:    return o->lst.len;
      case ASTR_T_NULL:    return 0;
      default:             return 1;
    }
}

VALUE
astr_subscript_get(VALUE seq, VALUE idx)
{
    int64_t i = astr_to_int(idx);
    if (i < 1) {
        fprintf(stderr, "astr: 1-based subscript only (got %lld)\n", (long long)i);
        exit(1);
    }
    size_t k = (size_t)(i - 1);
    if (ASTR_IS_FIX(seq)) {
        if (k != 0) return ASTR_NA;
        return seq;
    }
    struct astr_obj *o = ASTR_PTR(seq);
    switch (o->type) {
      case ASTR_T_NUM_VEC:
        return k < o->numvec.len ? astr_make_float(o->numvec.items[k]) : ASTR_NA;
      case ASTR_T_INT_VEC:
        return k < o->intvec.len ? astr_make_int(o->intvec.items[k]) : ASTR_NA;
      case ASTR_T_LIST:
        return k < o->lst.len ? o->lst.items[k] : ASTR_NULL;
      default:
        return k == 0 ? seq : ASTR_NA;
    }
}

VALUE
astr_subscript_set(VALUE seq, VALUE idx, VALUE val)
{
    int64_t i = astr_to_int(idx);
    if (i < 1) {
        fprintf(stderr, "astr: 1-based subscript only (got %lld)\n", (long long)i);
        exit(1);
    }
    size_t k = (size_t)(i - 1);
    if (!ASTR_IS_PTR(seq)) {
        fprintf(stderr, "astr: cannot index-assign into a scalar\n");
        exit(1);
    }
    struct astr_obj *o = ASTR_PTR(seq);
    switch (o->type) {
      case ASTR_T_NUM_VEC: {
        if (k >= o->numvec.capa) {
            size_t new_capa = (k + 1) * 2;
            double *items = (double *)GC_malloc_atomic(sizeof(double) * new_capa);
            memcpy(items, o->numvec.items, sizeof(double) * o->numvec.len);
            for (size_t j = o->numvec.len; j < new_capa; j++) items[j] = 0.0;
            o->numvec.items = items;
            o->numvec.capa = new_capa;
        }
        if (k >= o->numvec.len) {
            for (size_t j = o->numvec.len; j < k; j++) o->numvec.items[j] = 0.0;
            o->numvec.len = k + 1;
        }
        o->numvec.items[k] = astr_to_double(val);
        return val;
      }
      case ASTR_T_INT_VEC: {
        if (k >= o->intvec.capa) {
            size_t new_capa = (k + 1) * 2;
            int64_t *items = (int64_t *)GC_malloc_atomic(sizeof(int64_t) * new_capa);
            memcpy(items, o->intvec.items, sizeof(int64_t) * o->intvec.len);
            for (size_t j = o->intvec.len; j < new_capa; j++) items[j] = 0;
            o->intvec.items = items;
            o->intvec.capa = new_capa;
        }
        if (k >= o->intvec.len) {
            for (size_t j = o->intvec.len; j < k; j++) o->intvec.items[j] = 0;
            o->intvec.len = k + 1;
        }
        o->intvec.items[k] = astr_to_int(val);
        return val;
      }
      case ASTR_T_LIST: {
        if (k >= o->lst.capa) {
            size_t new_capa = (k + 1) * 2;
            VALUE *items = (VALUE *)GC_malloc(sizeof(VALUE) * new_capa);
            for (size_t j = 0; j < o->lst.len; j++) items[j] = o->lst.items[j];
            for (size_t j = o->lst.len; j < new_capa; j++) items[j] = ASTR_NULL;
            o->lst.items = items;
            o->lst.capa = new_capa;
        }
        if (k >= o->lst.len) {
            for (size_t j = o->lst.len; j < k; j++) o->lst.items[j] = ASTR_NULL;
            o->lst.len = k + 1;
        }
        o->lst.items[k] = val;
        return val;
      }
      default:
        fprintf(stderr, "astr: cannot index-assign into this type\n");
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// Printing — tries to match R's `print(x)` output reasonably closely so
// scripts can be diff'd against R.  Numbers: integer values render
// without a decimal; non-integer doubles use `%.15g` (R uses `getOption("digits")`).
// Strings: quoted.  Vectors: `[1] 1 2 3 4 5` with line wrap.
// ---------------------------------------------------------------------------

#define ASTR_PRINT_WIDTH 80

static void
print_double(FILE *fp, double d)
{
    if (isnan(d)) { fputs("NaN", fp); return; }
    if (!isfinite(d)) { fputs(d < 0 ? "-Inf" : "Inf", fp); return; }
    // Integer-valued and in the safe-int range ⇒ no decimal, like R.
    if (d == (double)(long long)d && fabs(d) < 1e16) {
        fprintf(fp, "%lld", (long long)d);
    }
    else {
        // R defaults to 7 significant digits; keep that for parity.
        fprintf(fp, "%.7g", d);
    }
}

static void
print_scalar(FILE *fp, VALUE v, bool quote_string)
{
    if (ASTR_IS_FIX(v)) {
        fprintf(fp, "%lld", (long long)ASTR_FIX_VAL(v));
        return;
    }
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:
        print_double(fp, o->dbl);
        return;
      case ASTR_T_STRING:
        if (quote_string) {
            fputc('"', fp);
            fwrite(o->str.chars, 1, o->str.len, fp);
            fputc('"', fp);
        }
        else {
            fwrite(o->str.chars, 1, o->str.len, fp);
        }
        return;
      case ASTR_T_NA:    fputs("NA",   fp); return;
      case ASTR_T_NULL:  fputs("NULL", fp); return;
      default: break;
    }
}

// Render a scalar to a malloc'd string; returns the byte length via
// *out_len.  Used by vector printing to compute column widths.
static size_t
fmt_scalar(VALUE v, bool quote_string, char *buf, size_t bufsz)
{
    FILE *fp = fmemopen(buf, bufsz, "w");
    if (!fp) { buf[0] = '\0'; return 0; }
    print_scalar(fp, v, quote_string);
    long pos = ftell(fp);
    fclose(fp);
    return pos < 0 ? 0 : (size_t)pos;
}

static void
print_vector_with_index(FILE *fp,
                        VALUE *items_or_null,
                        const double *dv, const int64_t *iv,
                        size_t len, bool quote)
{
    // Compute the max width of any element so columns align (matching
    // R's right-aligned print behaviour for atomic vectors).
    char buf[64];
    size_t max_w = 0;
    for (size_t i = 0; i < len; i++) {
        VALUE v;
        if (items_or_null)   v = items_or_null[i];
        else if (dv)         v = astr_make_float(dv[i]);
        else                 v = astr_make_int(iv[i]);
        size_t w = fmt_scalar(v, quote, buf, sizeof(buf));
        if (w > max_w) max_w = w;
    }

    // R prefixes each line with `[<index>]`.  Compute per-line element
    // count from the available width.
    char idx_buf[32];
    int idx_w = snprintf(idx_buf, sizeof(idx_buf), "[%zu]", len);
    int avail = ASTR_PRINT_WIDTH - idx_w - 1;
    int per_line = avail / (int)(max_w + 1);
    if (per_line < 1) per_line = 1;

    for (size_t i = 0; i < len; i += (size_t)per_line) {
        fprintf(fp, "[%zu]", i + 1);
        size_t end = i + (size_t)per_line;
        if (end > len) end = len;
        for (size_t j = i; j < end; j++) {
            fputc(' ', fp);
            VALUE v;
            if (items_or_null)   v = items_or_null[j];
            else if (dv)         v = astr_make_float(dv[j]);
            else                 v = astr_make_int(iv[j]);
            size_t w = fmt_scalar(v, quote, buf, sizeof(buf));
            // Right-align within max_w.
            for (size_t pad = w; pad < max_w; pad++) fputc(' ', fp);
            fwrite(buf, 1, w, fp);
        }
        fputc('\n', fp);
    }
}

void
astr_print(FILE *fp, VALUE v)
{
    if (ASTR_IS_FIX(v)) {
        fprintf(fp, "[1] %lld\n", (long long)ASTR_FIX_VAL(v));
        return;
    }
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:
        fputs("[1] ", fp);
        print_double(fp, o->dbl);
        fputc('\n', fp);
        return;
      case ASTR_T_STRING:
        fputs("[1] \"", fp);
        fwrite(o->str.chars, 1, o->str.len, fp);
        fputs("\"\n", fp);
        return;
      case ASTR_T_NUM_VEC:
        if (o->numvec.len == 0) { fputs("numeric(0)\n", fp); return; }
        print_vector_with_index(fp, NULL, o->numvec.items, NULL, o->numvec.len, false);
        return;
      case ASTR_T_INT_VEC:
        if (o->intvec.len == 0) { fputs("integer(0)\n", fp); return; }
        print_vector_with_index(fp, NULL, NULL, o->intvec.items, o->intvec.len, false);
        return;
      case ASTR_T_LIST:
        // Minimal list rendering — not bit-for-bit R-compatible.
        for (size_t i = 0; i < o->lst.len; i++) {
            fprintf(fp, "[[%zu]]\n", i + 1);
            astr_print(fp, o->lst.items[i]);
            if (i + 1 < o->lst.len) fputc('\n', fp);
        }
        return;
      case ASTR_T_NA:    fputs("[1] NA\n",   fp); return;
      case ASTR_T_NULL:  fputs("NULL\n",     fp); return;
    }
}

void
astr_cat(FILE *fp, VALUE v)
{
    if (ASTR_IS_FIX(v)) {
        fprintf(fp, "%lld", (long long)ASTR_FIX_VAL(v));
        return;
    }
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:   print_double(fp, o->dbl); return;
      case ASTR_T_STRING:  fwrite(o->str.chars, 1, o->str.len, fp); return;
      case ASTR_T_NUM_VEC:
        for (size_t i = 0; i < o->numvec.len; i++) {
            if (i > 0) fputc(' ', fp);
            print_double(fp, o->numvec.items[i]);
        }
        return;
      case ASTR_T_INT_VEC:
        for (size_t i = 0; i < o->intvec.len; i++) {
            if (i > 0) fputc(' ', fp);
            fprintf(fp, "%lld", (long long)o->intvec.items[i]);
        }
        return;
      case ASTR_T_NA:    fputs("NA",   fp); return;
      case ASTR_T_NULL:                    return;
      default: break;
    }
}

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------

VALUE
astr_paste(VALUE *items, size_t n, const char *sep)
{
    size_t sep_len = sep ? strlen(sep) : 0;

    // Two-pass: compute total length, then allocate + fill.
    char tmp[64];
    size_t total = 0;
    size_t *lens = (size_t *)alloca(sizeof(size_t) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) {
        VALUE v = items[i];
        if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_STRING) {
            lens[i] = ASTR_PTR(v)->str.len;
        }
        else {
            lens[i] = fmt_scalar(v, false, tmp, sizeof(tmp));
        }
        total += lens[i];
        if (i + 1 < n) total += sep_len;
    }

    char *buf = (char *)GC_malloc_atomic(total + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        VALUE v = items[i];
        if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_STRING) {
            memcpy(buf + k, ASTR_PTR(v)->str.chars, lens[i]);
            k += lens[i];
        }
        else {
            fmt_scalar(v, false, buf + k, total + 1 - k);
            k += lens[i];
        }
        if (i + 1 < n && sep_len) {
            memcpy(buf + k, sep, sep_len);
            k += sep_len;
        }
    }
    buf[total] = '\0';

    struct astr_obj *o = astr_alloc(ASTR_T_STRING);
    o->str.chars = buf;
    o->str.len = total;
    return ASTR_OBJ_VAL(o);
}
