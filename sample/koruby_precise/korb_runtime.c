/* koruby_precise v2 — korb_runtime.c
 *
 * Runtime core on the slots ABI (docs/v2_design.md):
 *   - korb_alloc: the ONLY c->slots_top publish point
 *   - strings / exceptions (moving heap, korb_alloc only — no libc objects)
 *   - symbol intern table + global method table (per-CTX VM, no globals)
 *   - korb_call: frame push (params window = staged args), RETURN catch,
 *     unwind backtrace accumulation
 *   - builtins: puts / p / print
 */

#define _GNU_SOURCE 1   /* pthread_getattr_np */
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <ctype.h>

#include "node.h"
#include "precise_gc/gc.h"

/* Frame-push headroom: covers in-frame expression staging without a per-node
 * check.  Deeper-than-slack staging without an intervening call lands on the
 * guard page (the designed last-resort backstop, v2_design §3.5). */
#define KORB_FRAME_SLACK 1024

/* ---------------------------------------------------------------------------
 * Allocation — publish + alloc (v2_design §3.3).
 * ------------------------------------------------------------------------- */

void *
korb_alloc(CTX *c, VALUE *slots, size_t size, unsigned int type)
{
    ASTRO_ASSERT(slots >= c->slots && slots <= c->slots_limit);
    c->slots_top = slots;                 /* publish: live values are below */
    VALUE v = aro_gc_alloc(c, size);      /* may collect; scans [slots, slots_top) */
    AroObjectHeader *h = (AroObjectHeader *)(uintptr_t)v;
    h->flags = (uint16_t)type;
    return h;
}

/* ---------------------------------------------------------------------------
 * Strings.  Bytes are inline (single allocation, copied whole on move).
 * `bytes` source must be C memory (literal operand / stack buffer) — it is
 * read AFTER the allocation.  Heap-sourced constructors take VALUE_REFs.
 * ------------------------------------------------------------------------- */

/* Allocate a KorbString with an uninitialized buffer of capacity `len` (len set,
 * NUL-terminated). Buffer is separate (header never moves on later grow). Caller
 * fills s->buf->data with no intervening GC. */
static KorbString *
korb_str_alloc(CTX *c, VALUE *slots, uint32_t len)
{
    KorbStrBuf *b = korb_alloc(c, slots, sizeof(KorbStrBuf) + len + 1, KORB_OBJ_STR_BUF);
    VALUE_REF bref = SLOTS_PUSH(slots, (VALUE)b);        /* root buf while header allocs */
    KorbString *s = korb_alloc(c, slots, sizeof(KorbString), KORB_OBJ_STRING);
    b = (KorbStrBuf *)(uintptr_t)VALUE_REF_GET(bref);    /* re-read after GC */
    s->len = len; s->capa = len;
    ARO_STORE(c, s, (VALUE *)(uintptr_t)&s->buf, (VALUE)(uintptr_t)b);
    b->data[len] = '\0';
    return s;
}

RESULT
korb_str_new(CTX *c, VALUE *slots, const char *bytes, uint32_t len)
{
    KorbString *s = korb_str_alloc(c, slots, len);
    memcpy(s->buf->data, bytes, len);
    return RESULT_OK((VALUE)s);
}

/* Ensure `s` (rooted via sref) has capacity for at least `need` bytes; grows the
 * buffer (header stays put). Returns the (possibly relocated) KorbString*. */
static KorbString *
korb_str_ensure(CTX *c, VALUE *slots, VALUE_REF sref, uint32_t need)
{
    KorbString *s = VAL2STR(VALUE_REF_GET(sref));
    if (need <= s->capa) return s;
    uint32_t ncapa = s->capa ? s->capa * 2 : 16;
    while (ncapa < need) ncapa *= 2;
    KorbStrBuf *nb = korb_alloc(c, slots, sizeof(KorbStrBuf) + ncapa + 1, KORB_OBJ_STR_BUF);
    s = VAL2STR(VALUE_REF_GET(sref));                    /* re-read after GC */
    memcpy(nb->data, s->buf->data, s->len);
    nb->data[s->len] = '\0';
    ARO_STORE(c, s, (VALUE *)(uintptr_t)&s->buf, (VALUE)(uintptr_t)nb);
    s->capa = ncapa;
    return s;
}

/* Append `n` bytes to string `s` (rooted via sref). src must NOT point into a
 * GC-movable buffer across the grow (copy it to a stable place first, or pass a
 * pointer re-read from a rooted slot — callers ensure this). */
static RESULT
korb_str_cat(CTX *c, VALUE *slots, VALUE_REF sref, const char *src, uint32_t n)
{
    KorbString *s = korb_str_ensure(c, slots, sref, VAL2STR(VALUE_REF_GET(sref))->len + n);
    memcpy(s->buf->data + s->len, src, n);
    s->len += n;
    s->buf->data[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(sref));
}

/* ---------------------------------------------------------------------------
 * Float (heap-boxed double).
 * ------------------------------------------------------------------------- */

RESULT
korb_float_new(CTX *c, VALUE *slots, double d)
{
    KorbFloat *f = korb_alloc(c, slots, sizeof(KorbFloat), KORB_OBJ_FLOAT);
    f->val = d;
    return RESULT_OK((VALUE)f);
}

bool
korb_num_to_d(VALUE v, double *out)
{
    if (FIXNUM_P(v))     { *out = (double)FIX2LONG(v); return true; }
    if (KORB_FLOAT_P(v)) { *out = VAL2FLT(v)->val;     return true; }
    return false;
}

/* Index coercion: Integer as-is, Float truncated via to_int (CRuby Array#[] etc). */
static inline bool
korb_to_index(VALUE v, intptr_t *out)
{
    if (FIXNUM_P(v))     { *out = FIX2LONG(v);          return true; }
    if (KORB_FLOAT_P(v)) { *out = (intptr_t)VAL2FLT(v)->val; return true; }
    return false;
}

/* CRuby-style Float#to_s: shortest round-tripping decimal, always with a '.'
 * or exponent.  buf must be >= 32 bytes; returns the length. */
static uint32_t
korb_float_to_s(double d, char *buf)
{
    if (isnan(d)) { memcpy(buf, "NaN", 4); return 3; }
    if (isinf(d)) {
        if (d < 0) { memcpy(buf, "-Infinity", 10); return 9; }
        memcpy(buf, "Infinity", 9); return 8;
    }
    /* shortest #significant-digits that round-trips (via scientific form) */
    char tmp[48];
    int sig = 1;
    for (; sig < 17; sig++) {
        snprintf(tmp, sizeof tmp, "%.*e", sig - 1, d);
        if (strtod(tmp, NULL) == d) break;
    }
    int exp10 = atoi(strchr(tmp, 'e') + 1);
    if (exp10 >= -4 && exp10 < 15) {                 /* fixed notation (CRuby range: -4..14) */
        int frac = sig - 1 - exp10;
        if (frac < 1) frac = 1;                      /* Ruby always shows ≥1 fractional digit */
        snprintf(buf, 32, "%.*f", frac, d);
    } else {                                         /* scientific: d.dddde±XX */
        snprintf(buf, 32, "%.*e", sig - 1, d);
        char *e = strchr(buf, 'e');
        if (e && !memchr(buf, '.', (size_t)(e - buf))) {   /* "1e+20" → "1.0e+20" */
            char t2[48]; size_t ml = (size_t)(e - buf);
            memcpy(t2, buf, ml); memcpy(t2 + ml, ".0", 2); strcpy(t2 + ml + 2, e);
            strcpy(buf, t2);
        }
    }
    return (uint32_t)strlen(buf);
}

/* numeric arithmetic with at least one Float operand.  op: 0+ 1- 2* 3/ 4% */
RESULT
korb_num_arith(CTX *c, VALUE *slots, VALUE l, VALUE rhs, int op, uint32_t line)
{
    static const char *const opn[] = { "+", "-", "*", "/", "%" };
    double a = 0.0, b = 0.0;
    if (UNLIKELY(!korb_num_to_d(l, &b)))     /* l not numeric → method missing on l */
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '%s' for %s", opn[op], korb_a_type_name(l));
    if (UNLIKELY(!korb_num_to_d(rhs, &b)))   /* rhs not numeric → coercion error */
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Float", korb_type_name(rhs));
    (void)korb_num_to_d(l, &a);
    double r;
    switch (op) {
      case 0: r = a + b; break;
      case 1: r = a - b; break;
      case 2: r = a * b; break;
      case 3: r = a / b; break;                 /* float div: Inf on /0 */
      default: r = fmod(a, b); if (r != 0.0 && ((r < 0) != (b < 0))) r += b; break;
    }
    return korb_float_new(c, slots, r);
}

/* a + b — alloc first, then copy through refs (fixup-safe; v2_design §4.3). */
static RESULT
korb_str_plus_ref(CTX *c, VALUE *slots, VALUE_REF a, VALUE_REF b)
{
    uint32_t alen = VAL2STR(VALUE_REF_GET(a))->len;
    uint32_t blen = VAL2STR(VALUE_REF_GET(b))->len;
    KorbString *s = korb_str_alloc(c, slots, alen + blen);
    const KorbString *as = VAL2STR(VALUE_REF_GET(a));   /* re-read: fixed up */
    const KorbString *bs = VAL2STR(VALUE_REF_GET(b));
    memcpy(s->buf->data, as->buf->data, alen);
    memcpy(s->buf->data + alen, bs->buf->data, blen);
    return RESULT_OK((VALUE)s);
}

static RESULT
korb_str_repeat_ref(CTX *c, VALUE *slots, VALUE_REF src, intptr_t cnt, uint32_t line)
{
    if (cnt < 0)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
    uint32_t len = VAL2STR(VALUE_REF_GET(src))->len;
    size_t total = (size_t)len * (size_t)cnt;
    if (total > (size_t)1 << 31)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "argument too big");
    KorbString *s = korb_str_alloc(c, slots, (uint32_t)total);
    const KorbString *ss = VAL2STR(VALUE_REF_GET(src));
    for (intptr_t i = 0; i < cnt; i++) {
        memcpy(s->buf->data + (size_t)i * len, ss->buf->data, len);
    }
    return RESULT_OK((VALUE)s);
}

/* String interpolation step: acc (a String) + to_s(part).  String parts take
 * the direct concat path; other values render through korb_fprint_to_s (which
 * does not allocate, so `part` stays put) into a transient buffer first. */
RESULT
korb_str_interp(CTX *c, VALUE *slots, VALUE_REF aref, VALUE part)
{
    VALUE_REF pref = SLOTS_PUSH(slots, part);            /* root part across GC */
    VALUE p = VALUE_REF_GET(pref);
    if (KORB_STRING_P(p))
        return korb_str_plus_ref(c, slots, aref, pref);

    char *buf = NULL;
    size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_to_s(c, ms, p);                          /* no GC inside */
    fclose(ms);
    RESULT sr = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    VALUE tmp = UNWRAP(sr);
    VALUE_REF tref = SLOTS_PUSH(slots, tmp);
    return korb_str_plus_ref(c, slots, aref, tref);
}

/* ---------------------------------------------------------------------------
 * Array — header + separately-allocated growable VALUE[] payload.
 * ------------------------------------------------------------------------- */

RESULT
korb_ary_new(CTX *c, VALUE *slots, uint32_t capa)
{
    if (capa < 4) capa = 4;
    /* items first, rooted while the header is allocated (both may GC). */
    KorbArrayItems *it = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)capa * sizeof(VALUE),
                                    KORB_OBJ_VALUE_ARRAY);
    VALUE_REF itref = SLOTS_PUSH(slots, (VALUE)it);
    KorbArray *a = korb_alloc(c, slots, sizeof(KorbArray), KORB_OBJ_ARRAY);
    it = (KorbArrayItems *)(uintptr_t)VALUE_REF_GET(itref);   /* re-read after GC */
    a->len = 0;
    a->capa = capa;
    ARO_STORE(c, a, (VALUE *)(uintptr_t)&a->items, (VALUE)(uintptr_t)it);
    return RESULT_OK((VALUE)a);
}

static RESULT
korb_ary_ensure(CTX *c, VALUE *slots, VALUE_REF aref, uint32_t need)
{
    KorbArray *a = VAL2ARY(VALUE_REF_GET(aref));
    if ((size_t)a->len + need <= a->capa) return RESULT_OK(VALUE_REF_GET(aref));
    uint32_t ncapa = a->capa ? a->capa * 2 : 4;
    while ((size_t)ncapa < (size_t)a->len + need) ncapa *= 2;
    KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * sizeof(VALUE),
                                     KORB_OBJ_VALUE_ARRAY);
    a = VAL2ARY(VALUE_REF_GET(aref));               /* re-read after GC */
    KorbArrayItems *oit = a->items;                 /* a fixed up → its items too */
    memcpy(nit->data, oit->data, (size_t)a->len * sizeof(VALUE));   /* new tail is zero-init = nil */
    ARO_STORE(c, a, (VALUE *)(uintptr_t)&a->items, (VALUE)(uintptr_t)nit);
    a->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(aref));
}

RESULT
korb_ary_push_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE elem)
{
    VALUE_REF eref = SLOTS_PUSH(slots, elem);       /* root elem across the grow GC */
    CHECK(korb_ary_ensure(c, slots, aref, 1));
    KorbArray *a = VAL2ARY(VALUE_REF_GET(aref));
    KorbArrayItems *it = a->items;
    ARO_STORE(c, it, &it->data[a->len], VALUE_REF_GET(eref));
    a->len++;
    return RESULT_OK(VALUE_REF_GET(aref));
}

/* Concatenate two arrays into a fresh one (Array#+ / the `+` binop).  lref/rref
 * are rooted; the result is left on the slots cursor via push. */
static RESULT
korb_ary_plus_ref(CTX *c, VALUE *slots, VALUE_REF lref, VALUE_REF rref)
{
    uint32_t ln = VAL2ARY(VALUE_REF_GET(lref))->len;
    uint32_t rn = VAL2ARY(VALUE_REF_GET(rref))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, ln + rn)));
    for (uint32_t i = 0; i < ln; i++) {
        VALUE elem = VAL2ARY(VALUE_REF_GET(lref))->items->data[i];   /* push roots elem first */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    rn = VAL2ARY(VALUE_REF_GET(rref))->len;                          /* re-read (rooted) */
    for (uint32_t i = 0; i < rn; i++) {
        VALUE elem = VAL2ARY(VALUE_REF_GET(rref))->items->data[i];
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* ---------------------------------------------------------------------------
 * Hash — header + growable [k0,v0,k1,v1,...] payload (insertion-ordered, linear
 * lookup).  Same moving-GC discipline as Array (separate, relocatable items).
 * ------------------------------------------------------------------------- */

RESULT
korb_hash_new(CTX *c, VALUE *slots, uint32_t capa)
{
    if (capa < 4) capa = 4;
    KorbArrayItems *it = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)capa * 2 * sizeof(VALUE),
                                    KORB_OBJ_VALUE_ARRAY);
    VALUE_REF itref = SLOTS_PUSH(slots, (VALUE)it);
    KorbHash *h = korb_alloc(c, slots, sizeof(KorbHash), KORB_OBJ_HASH);
    it = (KorbArrayItems *)(uintptr_t)VALUE_REF_GET(itref);    /* re-read after GC */
    h->len = 0;
    h->capa = capa;                       /* default_val already nil (zero-init) */
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->items, (VALUE)(uintptr_t)it);
    return RESULT_OK((VALUE)h);
}

/* index of key in the pair array, or -1 */
static int32_t
korb_hash_find(const KorbHash *h, VALUE key)
{
    const VALUE *const d = h->items->data;
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(d[2 * i], key)) return (int32_t)i;
    return -1;
}

static RESULT
korb_hash_ensure(CTX *c, VALUE *slots, VALUE_REF href, uint32_t need)
{
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    if ((size_t)h->len + need <= h->capa) return RESULT_OK(VALUE_REF_GET(href));
    uint32_t ncapa = h->capa ? h->capa * 2 : 4;
    while ((size_t)ncapa < (size_t)h->len + need) ncapa *= 2;
    KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * 2 * sizeof(VALUE),
                                     KORB_OBJ_VALUE_ARRAY);
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after GC */
    KorbArrayItems *oit = h->items;
    memcpy(nit->data, oit->data, (size_t)h->len * 2 * sizeof(VALUE));
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->items, (VALUE)(uintptr_t)nit);
    h->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(href));
}

RESULT
korb_hash_set(CTX *c, VALUE *slots, VALUE_REF href, VALUE_REF kref, VALUE val)
{
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val across grow GC */
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    int32_t idx = korb_hash_find(h, VALUE_REF_GET(kref));
    if (idx >= 0) {
        KorbArrayItems *it = h->items;
        ARO_STORE(c, it, &it->data[2 * idx + 1], VALUE_REF_GET(vref));
        return RESULT_OK(VALUE_REF_GET(href));
    }
    CHECK(korb_hash_ensure(c, slots, href, 1));
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after grow */
    KorbArrayItems *it = h->items;
    uint32_t i = h->len;
    ARO_STORE(c, it, &it->data[2 * i],     VALUE_REF_GET(kref));
    ARO_STORE(c, it, &it->data[2 * i + 1], VALUE_REF_GET(vref));
    h->len++;
    return RESULT_OK(VALUE_REF_GET(href));
}

/* ---------------------------------------------------------------------------
 * Object + instance variables.  ivars are a lazily-allocated [sym,val] pair
 * payload (same shape as Hash storage); names are interned symbols compared by
 * identity.
 * ------------------------------------------------------------------------- */

RESULT
korb_obj_new(CTX *c, VALUE *slots, VALUE klass)
{
    VALUE_REF kref = SLOTS_PUSH(slots, klass);        /* root klass across alloc */
    KorbObject *o = korb_alloc(c, slots, sizeof(KorbObject), KORB_OBJ_OBJECT);
    /* zero-init: ivar_len/capa=0, klass=nil, ivars=NULL */
    if (klass != KORB_NIL) ARO_STORE(c, o, (VALUE *)(uintptr_t)&o->klass, VALUE_REF_GET(kref));
    return RESULT_OK((VALUE)o);
}

VALUE
korb_ivar_get(VALUE self, VALUE name_sym)
{
    const KorbObject *o = VAL2OBJ(self);
    if (!o->ivars) return KORB_NIL;
    for (uint32_t i = 0; i < o->ivar_len; i++)
        if (o->ivars->data[2 * i] == name_sym) return o->ivars->data[2 * i + 1];
    return KORB_NIL;
}

RESULT
korb_ivar_set(CTX *c, VALUE *slots, VALUE_REF selfref, VALUE name_sym, VALUE val)
{
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val across grow GC */
    KorbObject *o = VAL2OBJ(VALUE_REF_GET(selfref));
    if (o->ivars) {                                   /* update existing */
        for (uint32_t i = 0; i < o->ivar_len; i++)
            if (o->ivars->data[2 * i] == name_sym) {
                KorbArrayItems *it = o->ivars;
                ARO_STORE(c, it, &it->data[2 * i + 1], VALUE_REF_GET(vref));
                return RESULT_OK(VALUE_REF_GET(vref));
            }
    }
    if (!o->ivars || o->ivar_len == o->ivar_capa) {   /* grow / first alloc */
        uint32_t ncapa = o->ivar_capa ? o->ivar_capa * 2 : 4;
        KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * 2 * sizeof(VALUE),
                                         KORB_OBJ_VALUE_ARRAY);
        o = VAL2OBJ(VALUE_REF_GET(selfref));          /* re-read after GC */
        if (o->ivars) memcpy(nit->data, o->ivars->data, (size_t)o->ivar_len * 2 * sizeof(VALUE));
        ARO_STORE(c, o, (VALUE *)(uintptr_t)&o->ivars, (VALUE)(uintptr_t)nit);
        o->ivar_capa = ncapa;
    }
    KorbArrayItems *it = o->ivars;
    uint32_t i = o->ivar_len;
    ARO_STORE(c, it, &it->data[2 * i],     name_sym);   /* symbol immediate, edge-qualified */
    ARO_STORE(c, it, &it->data[2 * i + 1], VALUE_REF_GET(vref));
    o->ivar_len++;
    return RESULT_OK(VALUE_REF_GET(vref));
}

static void korb_bt_append(struct korb_vm *vm, uint32_t line, const char *name);

/* ---------------------------------------------------------------------------
 * Classes + constants.  A class's instance-method table is a libc side-array
 * (no GC edges); constants live in vm->const_* (root-scanned).
 * ------------------------------------------------------------------------- */

RESULT
korb_class_new(CTX *c, VALUE *slots, uint32_t name_sym, VALUE superclass)
{
    VALUE_REF sref = SLOTS_PUSH(slots, superclass);   /* root super across alloc */
    KorbClass *k = korb_alloc(c, slots, sizeof(KorbClass), KORB_OBJ_CLASS);
    k->name_sym = name_sym;                            /* methods=NULL, cnts=0 (zero-init) */
    k->exc_etype = -1;                                 /* not an exception class by default */
    if (superclass != KORB_NIL) ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->superclass, VALUE_REF_GET(sref));
    return RESULT_OK((VALUE)k);
}

VALUE
korb_const_get(struct korb_vm *vm, uint32_t name_sym)
{
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) return vm->const_vals[i];
    return KORB_NIL;
}

void
korb_const_define(CTX *c, uint32_t name_sym, VALUE val)
{
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) { vm->const_vals[i] = val; return; }
    if (vm->const_cnt == vm->const_capa) {
        uint32_t nc = vm->const_capa ? vm->const_capa * 2 : 16;
        vm->const_names = realloc(vm->const_names, sizeof(uint32_t) * nc);
        vm->const_vals  = realloc(vm->const_vals,  sizeof(VALUE) * nc);
        if (!vm->const_names || !vm->const_vals) { fprintf(stderr, "koruby_precise: oom (consts)\n"); abort(); }
        vm->const_capa = nc;
    }
    vm->const_names[vm->const_cnt] = name_sym;
    vm->const_vals[vm->const_cnt] = val;     /* root cell (scanned); no WB needed */
    vm->const_cnt++;
}

void
korb_class_def_method(CTX *c, VALUE klass, uint32_t mid, NODE *body,
                      uint32_t params_cnt, uint32_t locals_cnt, uint32_t uses_block)
{
    KorbClass *const k = VAL2CLASS(klass);
    struct korb_method *m = NULL;
    for (uint32_t i = 0; i < k->method_cnt; i++)
        if (k->methods[i].mid == mid) { m = &k->methods[i]; break; }
    if (!m) {
        if (k->method_cnt == k->method_capa) {
            uint32_t nc = k->method_capa ? k->method_capa * 2 : 8;
            k->methods = realloc(k->methods, sizeof(struct korb_method) * nc);
            if (!k->methods) { fprintf(stderr, "koruby_precise: oom (methods)\n"); abort(); }
            k->method_capa = nc;
        }
        m = &k->methods[k->method_cnt++];
        m->mid = mid;
    }
    m->kind = KORB_METHOD_ISEQ;
    m->uses_block = (uint8_t)uses_block;
    m->params_cnt = (int32_t)params_cnt;
    m->locals_cnt = locals_cnt;
    m->body = body;
    m->bfn = NULL;
    c->vm->method_serial++;
}

void
korb_class_def_attr(CTX *c, VALUE klass, uint32_t mid, uint32_t ivar_sym, int is_writer)
{
    KorbClass *const k = VAL2CLASS(klass);
    struct korb_method *m = NULL;
    for (uint32_t i = 0; i < k->method_cnt; i++)
        if (k->methods[i].mid == mid) { m = &k->methods[i]; break; }
    if (!m) {
        if (k->method_cnt == k->method_capa) {
            uint32_t nc = k->method_capa ? k->method_capa * 2 : 8;
            k->methods = realloc(k->methods, sizeof(struct korb_method) * nc);
            if (!k->methods) { fprintf(stderr, "koruby_precise: oom (methods)\n"); abort(); }
            k->method_capa = nc;
        }
        m = &k->methods[k->method_cnt++];
        m->mid = mid;
    }
    m->kind = is_writer ? KORB_METHOD_ATTR_W : KORB_METHOD_ATTR_R;
    m->params_cnt = is_writer ? 1 : 0;
    m->attr_ivar = ivar_sym;
    m->body = NULL;
    m->bfn = NULL;
    c->vm->method_serial++;
}

/* lookup mid up the superclass chain; *out_def (if non-NULL) gets the class
 * that defines the found method (for `super`). */
static struct korb_method *
korb_class_find_method(VALUE klass, uint32_t mid, VALUE *out_def)
{
    while (KORB_CLASS_P(klass)) {
        KorbClass *k = VAL2CLASS(klass);
        for (uint32_t i = 0; i < k->method_cnt; i++)
            if (k->methods[i].mid == mid) { if (out_def) *out_def = klass; return &k->methods[i]; }
        klass = k->superclass;
    }
    return NULL;
}

/* Set up an ISEQ frame (args at slots[-argc..]) with `self` and `def_class`
 * (the class that defines this method — for `super`), and dispatch.  Shared by
 * instance dispatch, implicit self-calls, global calls, super, and new's init.
 * Frame reserved cells top-down: self(fs-1), def_class(fs-2), then the block
 * group {block_entry(fs-5), def_env(fs-4), captured_self(fs-3)} if it yields. */
static RESULT
korb_invoke_method(CTX *c, VALUE *slots, struct korb_method *m, uint32_t argc,
                   uint32_t line, uint32_t mid, VALUE self, VALUE def_class,
                   NODE *block, VALUE *def_env, VALUE captured_self)
{
    struct korb_vm *const vm = c->vm;
    if (UNLIKELY((uint32_t)m->params_cnt != argc)) {
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
    }
    VALUE *const base = slots - argc;
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    }
    if (locals_cnt > argc) memset(base + argc, 0, (locals_cnt - argc) * sizeof(VALUE));
    base[locals_cnt - 1] = self;
    base[locals_cnt - 2] = def_class;     /* odd-tagged not needed: class is a heap obj or nil */
    if (block != NULL && m->uses_block) {
        base[locals_cnt - 5] = (VALUE)((uintptr_t)block   | 1u);
        base[locals_cnt - 4] = (VALUE)((uintptr_t)def_env | 1u);
        base[locals_cnt - 3] = captured_self;
    }
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) r.state = KORB_NORMAL;
    else if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    return r;
}

RESULT
korb_class_body(CTX *c, VALUE *slots, uint32_t name_sym, NODE *body_entry, VALUE superclass)
{
    if (superclass != KORB_NIL && !KORB_CLASS_P(superclass))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "superclass must be a Class (%s given)", korb_type_name(superclass));
    VALUE cls = korb_const_get(c->vm, name_sym);
    if (!KORB_CLASS_P(cls)) {
        cls = UNWRAP(korb_class_new(c, slots, name_sym, superclass));
        korb_const_define(c, name_sym, cls);    /* now rooted in the const table */
    }
    slots[0] = cls;                              /* root for the body run + capture */
    return korb_block_yield(c, slots + 1, body_entry, NULL, NULL, 0, slots[0]);
}

/* `super` — invoke `mid` starting from def_class's superclass, keeping self. */
RESULT
korb_super(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
           VALUE def_class, VALUE self, NODE *block, VALUE *def_env, VALUE captured_self)
{
    VALUE sup = KORB_CLASS_P(def_class) ? VAL2CLASS(def_class)->superclass : KORB_NIL;
    VALUE found_def = KORB_NIL;
    struct korb_method *m = korb_class_find_method(sup, mid, &found_def);
    if (UNLIKELY(m == NULL))
        return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                          "super: no superclass method '%s'", korb_sym_name(c->vm, mid));
    if (m->kind == KORB_METHOD_ATTR_R)
        return RESULT_OK(korb_ivar_get(self, ID2SYM(m->attr_ivar)));
    return korb_invoke_method(c, slots, m, argc, line, mid, self, found_def, block, def_env, captured_self);
}

/* a descends from (or equals) b */
static bool
korb_class_le(VALUE a, VALUE b)
{
    while (KORB_CLASS_P(a)) {
        if (a == b) return true;
        a = VAL2CLASS(a)->superclass;
    }
    return false;
}

/* does exception `exc` match rescue class `rescue_class`? */
bool
korb_exc_matches(CTX *c, VALUE exc, VALUE rescue_class)
{
    if (!KORB_CLASS_P(rescue_class) || !KORB_EXC_P(exc)) return false;
    uint32_t et = VAL2EXC(exc)->etype;
    if (et >= 16) return false;
    VALUE exc_class = korb_const_get(c->vm, c->vm->exc_name[et]);
    return korb_class_le(exc_class, rescue_class);
}

/* class object of `self` (for `.class` / is_a?).  User objects → their klass;
 * exceptions → the etype's class; else the builtin class via class_name[]. */
static VALUE
korb_class_obj_of(CTX *c, VALUE self)
{
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) return VAL2OBJ(self)->klass;
    if (KORB_EXC_P(self)) {
        uint32_t et = VAL2EXC(self)->etype;
        if (et < 16) return korb_const_get(c->vm, c->vm->exc_name[et]);
    }
    return korb_const_get(c->vm, c->vm->class_name[korb_class_of(self)]);
}

/* Build the builtin class objects (Object/Integer/String/...) + register
 * constants + fill vm->class_name[].  Run before exception-class init. */
void
korb_init_builtin_classes(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    static const struct { const char *name; int cls; } defs[] = {
        { "Object", KORB_C_OBJECT }, { "Integer", KORB_C_INTEGER }, { "Float", KORB_C_FLOAT },
        { "String", KORB_C_STRING }, { "Symbol", KORB_C_SYMBOL }, { "Array", KORB_C_ARRAY },
        { "Hash", KORB_C_HASH }, { "Range", KORB_C_RANGE }, { "NilClass", KORB_C_NIL },
        { "TrueClass", KORB_C_TRUE }, { "FalseClass", KORB_C_FALSE }, { "Class", KORB_C_CLASS },
    };
    VALUE objc = KORB_NIL;
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        uint32_t name_sym = korb_intern(vm, defs[i].name, strlen(defs[i].name));
        VALUE cls = korb_class_new(c, slots, name_sym, objc).value;   /* Object first (objc=nil) */
        korb_const_define(c, name_sym, cls);
        vm->class_name[defs[i].cls] = name_sym;
        if (defs[i].cls == KORB_C_OBJECT) objc = cls;                 /* rest inherit Object */
    }
    vm->class_name[KORB_C_EXCEPTION] = korb_intern(vm, "Exception", 9);
}

/* Build the builtin Exception class hierarchy + register constants.  `slots` is
 * scratch above any live frame (classes are rooted in the const table). */
void
korb_init_exception_classes(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    static const struct { const char *name; int etype; const char *super; } defs[] = {
        { "Exception",           -1,                "Object" },
        { "StandardError",       -1,                "Exception" },
        { "ScriptError",         -1,                "Exception" },
        { "NameError",           KORB_E_NAME,       "StandardError" },
        { "RuntimeError",        KORB_E_RUNTIME,    "StandardError" },
        { "TypeError",           KORB_E_TYPE,       "StandardError" },
        { "ArgumentError",       KORB_E_ARGUMENT,   "StandardError" },
        { "ZeroDivisionError",   KORB_E_ZERODIV,    "StandardError" },
        { "LocalJumpError",      KORB_E_LOCALJUMP,  "StandardError" },
        { "IndexError",          -1,                "StandardError" },
        { "NoMethodError",       KORB_E_NOMETHOD,   "NameError" },
        { "NotImplementedError", KORB_E_NOTIMPL,    "ScriptError" },
        { "SystemStackError",    KORB_E_SYSSTACK,   "Exception" },
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        uint32_t name_sym = korb_intern(vm, defs[i].name, strlen(defs[i].name));
        VALUE super = defs[i].super
            ? korb_const_get(vm, korb_intern(vm, defs[i].super, strlen(defs[i].super)))
            : KORB_NIL;
        VALUE cls = korb_class_new(c, slots, name_sym, super).value;   /* never raises */
        VAL2CLASS(cls)->exc_etype = defs[i].etype;
        korb_const_define(c, name_sym, cls);
        if (defs[i].etype >= 0) vm->exc_name[defs[i].etype] = name_sym;
    }
}

/* ---------------------------------------------------------------------------
 * Range — {begin, end, exclude_end}.
 * ------------------------------------------------------------------------- */

RESULT
korb_range_new(CTX *c, VALUE *slots, VALUE_REF bref, VALUE end, uint32_t exclude_end)
{
    VALUE_REF eref = SLOTS_PUSH(slots, end);          /* root end across the alloc */
    KorbRange *r = korb_alloc(c, slots, sizeof(KorbRange), KORB_OBJ_RANGE);
    r->exclude_end = exclude_end;
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rbegin, VALUE_REF_GET(bref));
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rend,   VALUE_REF_GET(eref));
    return RESULT_OK((VALUE)r);
}

/* ---------------------------------------------------------------------------
 * Type names for messages.
 * ------------------------------------------------------------------------- */

const char *
korb_type_name(VALUE v)
{
    if (FIXNUM_P(v))  return "Integer";
    if (SYMBOL_P(v))  return "Symbol";
    if (v == KORB_NIL)   return "NilClass";
    if (v == KORB_TRUE)  return "TrueClass";
    if (v == KORB_FALSE) return "FalseClass";
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING:    return "String";
      case KORB_OBJ_EXCEPTION: return "Exception";
      case KORB_OBJ_ARRAY:     return "Array";
      case KORB_OBJ_HASH:      return "Hash";
      case KORB_OBJ_RANGE:     return "Range";
      case KORB_OBJ_FLOAT:     return "Float";
    }
    return "Object";
}

/* "for nil" / "for true" / "for an instance of String" (NoMethodError form) */
const char *
korb_a_type_name(VALUE v)
{
    if (v == KORB_NIL)   return "nil";
    if (v == KORB_TRUE)  return "true";
    if (v == KORB_FALSE) return "false";
    if (FIXNUM_P(v))     return "an instance of Integer";
    if (SYMBOL_P(v))     return "an instance of Symbol";
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: return "an instance of String";
      case KORB_OBJ_ARRAY:  return "an instance of Array";
      case KORB_OBJ_HASH:   return "an instance of Hash";
      case KORB_OBJ_RANGE:  return "an instance of Range";
      case KORB_OBJ_FLOAT:  return "an instance of Float";
    }
    return "an instance of Object";
}

/* ---------------------------------------------------------------------------
 * Equality / comparison.
 * ------------------------------------------------------------------------- */

/* spaceship for sort/min/max: -1/0/1, or 2 if incomparable.  Integers compare
 * numerically, strings byte-lexicographically. */
static int
korb_cmp_values(VALUE a, VALUE b)
{
    if ((FIXNUM_P(a) || KORB_FLOAT_P(a)) && (FIXNUM_P(b) || KORB_FLOAT_P(b))) {
        double x = 0, y = 0; korb_num_to_d(a, &x); korb_num_to_d(b, &y);
        return (x > y) - (x < y);
    }
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        uint32_t m = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->buf->data, y->buf->data, m);
        if (c) return c < 0 ? -1 : 1;
        return (x->len > y->len) - (x->len < y->len);
    }
    return 2;   /* incomparable */
}

/* CTX-aware compare adding Symbol ordering (by name) — for sort/min/max. */
static int
korb_cmp_full(CTX *c, VALUE a, VALUE b)
{
    if (SYMBOL_P(a) && SYMBOL_P(b)) {
        int r = strcmp(korb_sym_name(c->vm, SYM2ID(a)), korb_sym_name(c->vm, SYM2ID(b)));
        return (r > 0) - (r < 0);
    }
    return korb_cmp_values(a, b);
}

bool
korb_value_eq(VALUE a, VALUE b)
{
    if (a == b) return true;    /* fixnum / symbol / singletons / identity */
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        return x->len == y->len && memcmp(x->buf->data, y->buf->data, x->len) == 0;
    }
    double da, db;              /* numeric ==: 1 == 1.0, 1.0 == 1.0 */
    if ((KORB_FLOAT_P(a) || KORB_FLOAT_P(b)) && korb_num_to_d(a, &da) && korb_num_to_d(b, &db))
        return da == db;
    return false;
}

/* case equality `pat === val`: Range membership, Class is-a, else ==. No alloc. */
static bool
korb_case_eq(CTX *c, VALUE pat, VALUE val)
{
    if (KORB_RANGE_P(pat)) {
        const KorbRange *r = VAL2RANGE(pat);
        int lc = korb_cmp_values(r->rbegin, val);
        int uc = korb_cmp_values(val, r->rend);
        if (lc == 2 || uc == 2) return false;
        bool lower = (lc <= 0);
        bool upper = r->exclude_end ? (uc < 0) : (uc <= 0);
        return lower && upper;
    }
    if (KORB_CLASS_P(pat)) {
        if (pat == korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT])) return true;
        VALUE cls = korb_class_obj_of(c, val);
        while (KORB_CLASS_P(cls)) { if (cls == pat) return true; cls = VAL2CLASS(cls)->superclass; }
        return false;
    }
    return korb_value_eq(pat, val);
}

static const char *const korb_cmp_op_name[] = { "<", "<=", ">", ">=" };

/* CRuby rb_cmperr flavor: immediates render via inspect, others by class. */
static void
korb_cmperr_operand(VALUE v, char *buf, size_t cap)
{
    if (FIXNUM_P(v))          snprintf(buf, cap, "%ld", (long)FIX2LONG(v));
    else if (v == KORB_NIL)   snprintf(buf, cap, "nil");
    else if (v == KORB_TRUE)  snprintf(buf, cap, "true");
    else if (v == KORB_FALSE) snprintf(buf, cap, "false");
    else                      snprintf(buf, cap, "%s", korb_type_name(v));
}

static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup);

RESULT
korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line)
{
    if (KORB_HASH_P(l) && KORB_HASH_P(r)) {              /* subset/superset comparison */
        const KorbHash *me = VAL2HASH(l), *other = VAL2HASH(r);
        bool t;
        switch (op) {
          case 0:  t = me->len <  other->len && korb_hash_is_subset(me, other); break;
          case 1:  t = me->len <= other->len && korb_hash_is_subset(me, other); break;
          case 2:  t = me->len >  other->len && korb_hash_is_subset(other, me); break;
          default: t = me->len >= other->len && korb_hash_is_subset(other, me); break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    double ld, rd;
    if ((KORB_FLOAT_P(l) || KORB_FLOAT_P(r)) && korb_num_to_d(l, &ld) && korb_num_to_d(r, &rd)) {
        bool t;
        switch (op) {
          case 0:  t = ld <  rd; break;
          case 1:  t = ld <= rd; break;
          case 2:  t = ld >  rd; break;
          default: t = ld >= rd; break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_STRING_P(l) && KORB_STRING_P(r)) {
        const KorbString *x = VAL2STR(l), *y = VAL2STR(r);
        uint32_t min = x->len < y->len ? x->len : y->len;
        int cmp = memcmp(x->buf->data, y->buf->data, min);
        if (cmp == 0) cmp = (x->len > y->len) - (x->len < y->len);
        bool t;
        switch (op) {
          case 0:  t = cmp <  0; break;
          case 1:  t = cmp <= 0; break;
          case 2:  t = cmp >  0; break;
          default: t = cmp >= 0; break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (FIXNUM_P(l) || KORB_STRING_P(l)) {
        char rdesc[64];
        korb_cmperr_operand(r, rdesc, sizeof(rdesc));
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "comparison of %s with %s failed", korb_type_name(l), rdesc);
    }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '%s' for %s",
                      korb_cmp_op_name[op], korb_a_type_name(l));
}

/* ---------------------------------------------------------------------------
 * Binop slow paths (String variants + type errors).
 * ------------------------------------------------------------------------- */

RESULT
korb_plus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs)) return korb_num_arith(c, slots, l, rhs, 0, line);
    if (KORB_STRING_P(l) && KORB_STRING_P(rhs)) {
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_str_plus_ref(c, slots, lhs, r);
    }
    if (KORB_ARRAY_P(l)) {
        if (!KORB_ARRAY_P(rhs))
            return korb_raise(c, slots, KORB_E_TYPE, line,
                              "no implicit conversion of %s into Array", korb_type_name(rhs));
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_ary_plus_ref(c, slots, lhs, r);
    }
    if (FIXNUM_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "%s can't be coerced into Integer", korb_type_name(rhs));
    if (KORB_STRING_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into String", korb_type_name(rhs));
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '+' for %s", korb_a_type_name(l));
}

static RESULT korb_m_ary_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);

RESULT
korb_mul_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (!KORB_ARRAY_P(l) && !KORB_STRING_P(l) && (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs))) return korb_num_arith(c, slots, l, rhs, 2, line);
    if (KORB_STRING_P(l)) {
        intptr_t cnt;
        if (UNLIKELY(!korb_to_index(rhs, &cnt))) return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Integer", korb_type_name(rhs));
        return korb_str_repeat_ref(c, slots, lhs, cnt, line);
    }
    if (KORB_ARRAY_P(l) && (FIXNUM_P(rhs) || KORB_FLOAT_P(rhs))) {   /* Array * n → repeated array (Float coerced via to_int) */
        intptr_t cnt = FIXNUM_P(rhs) ? FIX2LONG(rhs) : (intptr_t)VAL2FLT(rhs)->val;
        if (cnt < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
        uint32_t len = VAL2ARY(l)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)cnt * len)));
        for (intptr_t r = 0; r < cnt; r++)
            for (uint32_t i = 0; i < len; i++)
                CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(VALUE_REF_GET(lhs))->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_ARRAY_P(l) && KORB_STRING_P(rhs)) {     /* Array * sep → join */
        slots[0] = rhs;
        return korb_m_ary_join(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
    }
    if (KORB_ARRAY_P(l))                             /* Array * non-int/str */
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into Integer", korb_type_name(rhs));
    if (FIXNUM_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "%s can't be coerced into Integer", korb_type_name(rhs));
    if (KORB_STRING_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into Integer", korb_type_name(rhs));
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '*' for %s", korb_a_type_name(l));
}

/* ---------------------------------------------------------------------------
 * Symbols (interned names; immediates — libc table, never GC-scanned).
 * ------------------------------------------------------------------------- */

uint32_t
korb_intern(struct korb_vm *vm, const char *name, size_t len)
{
    for (uint32_t i = 0; i < vm->sym_cnt; i++) {
        if (strlen(vm->sym_names[i]) == len && memcmp(vm->sym_names[i], name, len) == 0)
            return i;
    }
    if (vm->sym_cnt == vm->sym_capa) {
        vm->sym_capa = vm->sym_capa ? vm->sym_capa * 2 : 64;
        vm->sym_names = realloc(vm->sym_names, sizeof(char *) * vm->sym_capa);
        if (!vm->sym_names) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    }
    char *copy = malloc(len + 1);
    if (!copy) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    memcpy(copy, name, len);
    copy[len] = '\0';
    vm->sym_names[vm->sym_cnt] = copy;
    return vm->sym_cnt++;
}

const char *
korb_sym_name(const struct korb_vm *vm, uint32_t id)
{
    return id < vm->sym_cnt ? vm->sym_names[id] : "?";
}

/* ---------------------------------------------------------------------------
 * Method table.
 * ------------------------------------------------------------------------- */

static struct korb_method *
korb_method_slot(CTX *c, uint32_t mid)
{
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->method_cnt; i++) {
        if (vm->methods[i].mid == mid) return &vm->methods[i];
    }
    if (vm->method_cnt == vm->method_capa) {
        vm->method_capa = vm->method_capa ? vm->method_capa * 2 : 32;
        vm->methods = realloc(vm->methods, sizeof(struct korb_method) * vm->method_capa);
        if (!vm->methods) { fprintf(stderr, "koruby_precise: out of memory (methods)\n"); abort(); }
    }
    struct korb_method *m = &vm->methods[vm->method_cnt++];
    memset(m, 0, sizeof(*m));
    m->mid = mid;
    return m;
}

void
korb_method_define(CTX *c, uint32_t mid, NODE *body,
                   uint32_t params_cnt, uint32_t locals_cnt, uint32_t uses_block)
{
    struct korb_method *m = korb_method_slot(c, mid);
    m->kind = KORB_METHOD_ISEQ;
    m->uses_block = (uint8_t)uses_block;
    m->params_cnt = (int32_t)params_cnt;
    m->locals_cnt = locals_cnt;
    m->body = body;
    m->bfn = NULL;
    c->vm->method_serial++;   /* invalidate call caches */
}

void
korb_builtin_define(CTX *c, const char *name, korb_builtin_fn fn, int32_t params_cnt)
{
    uint32_t mid = korb_intern(c->vm, name, strlen(name));
    struct korb_method *m = korb_method_slot(c, mid);
    m->kind = KORB_METHOD_BUILTIN;
    m->params_cnt = params_cnt;   /* -1 = variadic */
    m->locals_cnt = 0;
    m->body = NULL;
    m->bfn = fn;
    c->vm->method_serial++;
}

static struct korb_method *
korb_method_lookup(struct korb_vm *vm, uint32_t mid)
{
    for (uint32_t i = 0; i < vm->method_cnt; i++) {
        if (vm->methods[i].mid == mid) return &vm->methods[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Raise + unwind backtrace.
 *
 * The exception object travels in RESULT.value (registers).  The UNWRAP /
 * CHECK propagation path has no GC points, so this is safe; only the
 * unwind bookkeeping below runs between raise and the toplevel report, and
 * it allocates libc memory only.
 * ------------------------------------------------------------------------- */

static void
korb_bt_append(struct korb_vm *vm, uint32_t line, const char *name)
{
    if (vm->bt_cnt == vm->bt_capa) {
        vm->bt_capa = vm->bt_capa ? vm->bt_capa * 2 : 16;
        vm->bt = realloc(vm->bt, sizeof(struct korb_bt_entry) * vm->bt_capa);
        if (!vm->bt) { fprintf(stderr, "koruby_precise: out of memory (backtrace)\n"); abort(); }
    }
    /* CRuby caps the displayed backtrace; cap accumulation so a runaway
     * recursion unwind doesn't grow without bound. */
    if (vm->bt_cnt >= 4096) return;
    vm->bt[vm->bt_cnt].line = line;
    vm->bt[vm->bt_cnt].name = name;
    vm->bt_cnt++;
}

RESULT
korb_raise(CTX *c, VALUE *slots, unsigned int etype, uint32_t line,
           const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    c->vm->bt_cnt = 0;     /* fresh unwind */

    VALUE_REF msg = SLOTS_PUSH(slots, KORB_NIL);
    VALUE_REF_SET(msg, UNWRAP(korb_str_new(c, slots, buf, (uint32_t)strlen(buf))));
    KorbException *e = korb_alloc(c, slots, sizeof(KorbException), KORB_OBJ_EXCEPTION);
    e->etype = etype;
    e->line = line;
    ARO_STORE(c, e, &e->msg, VALUE_REF_GET(msg));
    return RESULT_RAISE_((VALUE)e);
}

static const char *
korb_etype_name(unsigned int etype)
{
    switch (etype) {
      case KORB_E_TYPE:     return "TypeError";
      case KORB_E_ARGUMENT: return "ArgumentError";
      case KORB_E_ZERODIV:  return "ZeroDivisionError";
      case KORB_E_NOMETHOD: return "NoMethodError";
      case KORB_E_SYSSTACK: return "SystemStackError";
      case KORB_E_NOTIMPL:  return "NotImplementedError";
      case KORB_E_NAME:     return "NameError";
      case KORB_E_LOCALJUMP: return "LocalJumpError";
      default:              return "RuntimeError";
    }
}

void
korb_report_uncaught(CTX *c, VALUE exc)
{
    struct korb_vm *const vm = c->vm;
    const char *file = vm->script_name ? vm->script_name : "?";

    if (!KORB_EXC_P(exc)) {
        fprintf(stderr, "%s: uncaught non-exception raise\n", file);
        return;
    }
    KorbException *e = VAL2EXC(exc);
    const char *cls = korb_etype_name(e->etype);
    const char *msg = (e->msg != KORB_NIL) ? VAL2STR(e->msg)->buf->data : cls;

    if (vm->bt_cnt > 0) {
        fprintf(stderr, "%s:%u:in '%s': %s (%s)\n", file, vm->bt[0].line, vm->bt[0].name, msg, cls);
        /* elide the middle of very deep unwinds (SystemStackError) */
        uint32_t head = vm->bt_cnt, tail = 0;
        if (vm->bt_cnt > 20) { head = 12; tail = 4; }
        for (uint32_t i = 1; i < head; i++) {
            fprintf(stderr, "\tfrom %s:%u:in '%s'\n", file, vm->bt[i].line, vm->bt[i].name);
        }
        if (tail) {
            fprintf(stderr, "\t ... %u levels...\n", vm->bt_cnt - head - tail);
            for (uint32_t i = vm->bt_cnt - tail; i < vm->bt_cnt; i++) {
                fprintf(stderr, "\tfrom %s:%u:in '%s'\n", file, vm->bt[i].line, vm->bt[i].name);
            }
        }
        fprintf(stderr, "\tfrom %s:%u:in '<main>'\n", file, e->line);
    }
    else {
        fprintf(stderr, "%s:%u:in '<main>': %s (%s)\n", file, e->line, msg, cls);
    }
}

/* ---------------------------------------------------------------------------
 * Calls.
 * ------------------------------------------------------------------------- */

/* Shared call path.  Frame reserved cells (top-down): self at base[fs-1]
 * (always, written from `self`); when the callee yields (m->uses_block) the
 * block group {block_entry, def_env, captured_self} sits just below at
 * base[fs-4..fs-2], odd-tagged so the GC root scan skips the two pointers.
 * `block` is NULL for an ordinary call (block ignored by non-yielding callees). */
static RESULT
korb_call_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
               struct korb_callcache *cc, uint32_t argc,
               VALUE self, NODE *block, VALUE *def_env, VALUE captured_self)
{
    struct korb_vm *const vm = c->vm;

    /* implicit self-call on a user instance → dispatch through its class chain
     * (a miss falls through to the global function table). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_class_find_method(VAL2OBJ(self)->klass, mid, &def_class);
        if (um) {
            if (um->kind == KORB_METHOD_ATTR_R)
                return RESULT_OK(korb_ivar_get(self, ID2SYM(um->attr_ivar)));
            if (um->kind == KORB_METHOD_ATTR_W) {
                if (UNLIKELY(argc != 1))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                      "wrong number of arguments (given %u, expected 1)", argc);
                slots[0] = self;                       /* root self for the set */
                VALUE v = slots[-(intptr_t)argc];
                CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(um->attr_ivar), v));
                return RESULT_OK(slots[-(intptr_t)argc]);
            }
            return korb_invoke_method(c, slots, um, argc, line, mid, self, def_class, block, def_env, captured_self);
        }
    }

    struct korb_method *m = cc->m;
    if (UNLIKELY(cc->serial != vm->method_serial)) {
        m = korb_method_lookup(vm, mid);
        if (UNLIKELY(m == NULL)) {
            return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                              "undefined method '%s' for %s", korb_sym_name(vm, mid),
                              (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL)
                                  ? "main" : korb_a_type_name(self));
        }
        cc->m = m;
        cc->serial = vm->method_serial;
    }

    VALUE *const base = slots - argc;     /* staged args = parameter window */

    if (m->kind == KORB_METHOD_BUILTIN) {
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc)) {
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)",
                              argc, m->params_cnt);
        }
        RESULT r = m->bfn(c, slots, VALUE_SLICE_MAKE(base, argc));
        if (UNLIKELY(r.state == KORB_RAISE)) {
            KorbException *e = VAL2EXC(r.value);
            korb_bt_append(vm, e->line, korb_sym_name(vm, m->mid));
            e->line = line;
        }
        return r;
    }

    /* ISEQ global function: no defining class (super in a global fn has none). */
    return korb_invoke_method(c, slots, m, argc, line, mid, self, KORB_NIL,
                              block, def_env, captured_self);
}

RESULT
korb_call(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
          struct korb_callcache *cc, uint32_t argc, VALUE self)
{
    return korb_call_impl(c, slots, mid, line, cc, argc, self, NULL, NULL, KORB_NIL);
}

RESULT
korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              struct korb_callcache *cc, uint32_t argc,
              VALUE self, NODE *block, VALUE *def_env, VALUE captured_self)
{
    return korb_call_impl(c, slots, mid, line, cc, argc, self, block, def_env, captured_self);
}

/* ---- node_entry accessors + yield ----------------------------------------- */

uint32_t korb_entry_params_cnt(NODE *entry) { return entry->u.node_entry.params_cnt; }
uint32_t korb_entry_locals_cnt(NODE *entry) { return entry->u.node_entry.locals_cnt; }
static uint32_t korb_entry_destructure_n(NODE *entry) { return entry->u.node_entry.destructure_n; }
NODE    *korb_entry_body(NODE *entry)       { return entry->u.node_entry.body; }

/* Core block invocation: lay out the block frame at cursor `slots` and
 * dispatch the entry.  Args come from `argv` (argv[i] copied into block
 * params; extra dropped, missing → nil — CRuby semantics).  argv may alias the
 * cursor region (node_yield passes &slots[-argc]); copies happen before any
 * GC, so raw VALUEs in argv are safe.  A stack-overflow check returns RAISE. */
RESULT
korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                 const VALUE *argv, uint32_t argc, VALUE captured_self)
{
    const uint32_t blocals = korb_entry_locals_cnt(block);   /* incl. self cell */
    /* block frame: bf[0]=PREV(def_env, tagged), bf[1..1+blocals)=block locals,
     * with the block's self cell at base[fs-1] = bf[blocals]. */
    VALUE *const bf = slots;
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    }
    bf[0] = (VALUE)((uintptr_t)def_env | 1u);
    uint32_t dn = korb_entry_destructure_n(block);
    if (dn > 0) {                                       /* |(a, b, ...)| — splat the array arg */
        VALUE arr = (argc >= 1) ? argv[0] : KORB_NIL;
        if (KORB_ARRAY_P(arr)) {
            const KorbArray *ar = VAL2ARY(arr);
            for (uint32_t i = 0; i < dn; i++) bf[1 + i] = i < ar->len ? ar->items->data[i] : KORB_NIL;
        } else {
            bf[1] = arr;                               /* non-array → first target, rest nil */
            for (uint32_t i = 1; i < dn; i++) bf[1 + i] = KORB_NIL;
        }
        for (uint32_t i = dn; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else {
        const uint32_t np = korb_entry_params_cnt(block);   /* np <= blocals - 1 */
        for (uint32_t i = 0; i < np; i++)      bf[1 + i] = (i < argc) ? argv[i] : KORB_NIL;
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    }
    bf[blocals] = captured_self;                        /* block's lexical self */

    RESULT r = (*block->head.dispatcher)(c, block, bf + 1 + blocals);
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;   /* `next [v]` = block value */
    return r;
}

RESULT
korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
           VALUE block_cell, VALUE def_env_cell, VALUE captured_self)
{
    /* Frame cells are odd-tagged when a block is present; nil (0) = none. */
    if (UNLIKELY(((uintptr_t)block_cell & 1u) == 0)) {
        return korb_raise(c, slots, KORB_E_LOCALJUMP, line, "no block given (yield)");
    }
    NODE  *entry   = (NODE  *)(uintptr_t)((uintptr_t)block_cell   & ~(uintptr_t)1u);
    VALUE *def_env = (VALUE *)(uintptr_t)((uintptr_t)def_env_cell & ~(uintptr_t)1u);
    return korb_block_yield(c, slots, entry, def_env, slots - argc, argc, captured_self);
}

/* ---------------------------------------------------------------------------
 * Receiver method dispatch (x.foo) — built-in methods on core types.
 * ------------------------------------------------------------------------- */

enum korb_class
korb_class_of(VALUE v)
{
    if (FIXNUM_P(v))     return KORB_C_INTEGER;
    if (v == KORB_NIL)   return KORB_C_NIL;
    if (v == KORB_TRUE)  return KORB_C_TRUE;
    if (v == KORB_FALSE) return KORB_C_FALSE;
    if (SYMBOL_P(v))     return KORB_C_SYMBOL;
    if (AROH_IS_GC_OBJECT(v)) {
        switch (KORB_OBJ_TYPE(v)) {
          case KORB_OBJ_STRING: return KORB_C_STRING;
          case KORB_OBJ_ARRAY:  return KORB_C_ARRAY;
          case KORB_OBJ_HASH:   return KORB_C_HASH;
          case KORB_OBJ_RANGE:  return KORB_C_RANGE;
          case KORB_OBJ_CLASS:  return KORB_C_CLASS;
          case KORB_OBJ_EXCEPTION: return KORB_C_EXCEPTION;
          case KORB_OBJ_FLOAT:  return KORB_C_FLOAT;
        }
    }
    return KORB_C_OBJECT;
}

const char *
korb_class_name(enum korb_class cls)
{
    switch (cls) {
      case KORB_C_INTEGER: return "Integer";
      case KORB_C_STRING:  return "String";
      case KORB_C_SYMBOL:  return "Symbol";
      case KORB_C_ARRAY:   return "Array";
      case KORB_C_HASH:    return "Hash";
      case KORB_C_RANGE:   return "Range";
      case KORB_C_CLASS:   return "Class";
      case KORB_C_FLOAT:   return "Float";
      case KORB_C_NIL:     return "NilClass";
      case KORB_C_TRUE:    return "TrueClass";
      case KORB_C_FALSE:   return "FalseClass";
      default:             return "Object";
    }
}

void
korb_def_cmethod(CTX *c, enum korb_class cls, const char *name,
                 korb_method_fn fn, int32_t arity)
{
    struct korb_vm *const vm = c->vm;
    uint32_t mid = korb_intern(vm, name, strlen(name));
    if (vm->cmethod_cnt[cls] == vm->cmethod_capa[cls]) {
        uint32_t nc = vm->cmethod_capa[cls] ? vm->cmethod_capa[cls] * 2 : 16;
        vm->cmethods[cls] = realloc(vm->cmethods[cls], sizeof(*vm->cmethods[cls]) * nc);
        if (!vm->cmethods[cls]) { fprintf(stderr, "koruby_precise: oom (cmethods)\n"); abort(); }
        vm->cmethod_capa[cls] = nc;
    }
    struct korb_cmethod *m = &vm->cmethods[cls][vm->cmethod_cnt[cls]++];
    m->mid = mid; m->fn = fn; m->bfn = NULL; m->arity = arity; m->takes_block = 0;
}

void
korb_def_cmethod_blk(CTX *c, enum korb_class cls, const char *name,
                     korb_method_blk_fn fn, int32_t arity)
{
    korb_def_cmethod(c, cls, name, NULL, arity);             /* grows + fills common fields */
    struct korb_vm *const vm = c->vm;
    struct korb_cmethod *m = &vm->cmethods[cls][vm->cmethod_cnt[cls] - 1];
    m->bfn = fn; m->takes_block = 1;
}

static const struct korb_cmethod *
korb_find_cmethod(struct korb_vm *vm, enum korb_class cls, uint32_t mid)
{
    for (uint32_t i = 0; i < vm->cmethod_cnt[cls]; i++)
        if (vm->cmethods[cls][i].mid == mid) return &vm->cmethods[cls][i];
    /* fall back to the universal (Object) table */
    if (cls != KORB_C_OBJECT) {
        for (uint32_t i = 0; i < vm->cmethod_cnt[KORB_C_OBJECT]; i++)
            if (vm->cmethods[KORB_C_OBJECT][i].mid == mid) return &vm->cmethods[KORB_C_OBJECT][i];
    }
    return NULL;
}

/* Shared receiver dispatch.  `block`/`def_env` are NULL for a plain send;
 * non-NULL for a `{ ... }` form.  A block handed to a non-yielding method is
 * ignored (CRuby); a yielding method called without a block gets NULL. */
static RESULT
korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
               NODE *block, VALUE *def_env, VALUE captured_self)
{
    struct korb_vm *const vm = c->vm;
    VALUE *const recv_slot = &slots[-(intptr_t)argc - 1];
    VALUE self = *recv_slot;

    /* user instance → dispatch through its class chain (miss falls to Object). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_class_find_method(VAL2OBJ(self)->klass, mid, &def_class);
        if (um) {
            if (um->kind == KORB_METHOD_ATTR_R)
                return RESULT_OK(korb_ivar_get(self, ID2SYM(um->attr_ivar)));
            if (um->kind == KORB_METHOD_ATTR_W) {
                if (UNLIKELY(argc != 1))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                      "wrong number of arguments (given %u, expected 1)", argc);
                VALUE v = slots[-(intptr_t)argc];      /* arg0 (returned by writer) */
                CHECK(korb_ivar_set(c, slots, VALUE_REF_AT(recv_slot), ID2SYM(um->attr_ivar), v));
                return RESULT_OK(slots[-(intptr_t)argc]);
            }
            return korb_invoke_method(c, slots, um, argc, line, mid, self, def_class, block, def_env, captured_self);
        }
    }
    /* class receiver → Klass.new (allocate + initialize). */
    else if (KORB_CLASS_P(self) && strcmp(korb_sym_name(vm, mid), "new") == 0) {
        uint32_t init_mid = korb_intern(vm, "initialize", 10);
        VALUE obj = UNWRAP(korb_obj_new(c, slots, *recv_slot));   /* klass=class (rooted) */
        /* find initialize AFTER the alloc-GC, re-reading the class from the
         * rooted recv slot (the pre-alloc class pointer would be stale). */
        VALUE init_def = KORB_NIL;
        struct korb_method *init = korb_class_find_method(*recv_slot, init_mid, &init_def);
        if (init) {
            VALUE *base = slots - argc;
            RESULT ir = korb_invoke_method(c, slots, init, argc, line, init_mid, obj, init_def, block, def_env, captured_self);
            if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
            return RESULT_OK(base[init->locals_cnt - 1]);        /* the (possibly moved) obj */
        }
        if (UNLIKELY(argc != 0))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected 0)", argc);
        return RESULT_OK(obj);
    }

    enum korb_class cls = korb_class_of(self);
    const struct korb_cmethod *m = korb_find_cmethod(vm, cls, mid);
    if (UNLIKELY(m == NULL)) {
        return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                          "undefined method '%s' for %s",
                          korb_sym_name(vm, mid), korb_a_type_name(self));
    }
    if (UNLIKELY(m->arity >= 0 && (uint32_t)m->arity != argc)) {
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %d)", argc, m->arity);
    }
    const VALUE_REF recv = VALUE_REF_AT(recv_slot);
    const VALUE_SLICE args = VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc);
    RESULT r = m->takes_block ? m->bfn(c, slots, recv, args, block, def_env, captured_self)
                              : m->fn(c, slots, recv, args);
    if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    return r;
}

RESULT
korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc)
{
    return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, KORB_NIL);
}

RESULT
korb_send_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              uint32_t argc, NODE *block, VALUE *def_env, VALUE captured_self)
{
    return korb_send_impl(c, slots, mid, line, argc, block, def_env, captured_self);
}

/* ---- integer formatting (to_s / chr helpers) ----------------------------- */

static uint32_t
korb_fmt_int(intptr_t n, int base, char *buf)
{
    char tmp[80];
    int ti = 0;
    bool neg = n < 0;
    uintptr_t u = neg ? (uintptr_t)(-(n + 1)) + 1u : (uintptr_t)n;
    if (u == 0) tmp[ti++] = '0';
    while (u) { int d = (int)(u % (uintptr_t)base); tmp[ti++] = d < 10 ? (char)('0'+d) : (char)('a'+d-10); u /= (uintptr_t)base; }
    uint32_t len = 0;
    if (neg) buf[len++] = '-';
    while (ti) buf[len++] = tmp[--ti];
    return len;
}

/* ---- Integer methods ----------------------------------------------------- */

#define SELF_INT  FIX2LONG(VALUE_REF_GET(self))
static RESULT korb_m_int_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = SELF_INT;
    if (n >= 0) return RESULT_OK(VALUE_REF_GET(self));
    if (UNLIKELY(!FIXABLE(-n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented in M0)");
    return RESULT_OK(LONG2FIX(-n));
}
static RESULT korb_m_int_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = SELF_INT + 1;
    if (UNLIKELY(!FIXABLE(n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented in M0)");
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_pred(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = SELF_INT - 1;
    if (UNLIKELY(!FIXABLE(n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented in M0)");
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_INT == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_nonzero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_INT != 0 ? VALUE_REF_GET(self) : KORB_NIL); }
static RESULT korb_m_int_even(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((SELF_INT & 1) == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_odd (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((SELF_INT & 1) != 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_pos (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_INT > 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_neg (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_INT < 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_true_lit (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_int_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int base = 10;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE b = VALUE_SLICE_GET(a, 0);
        if (!FIXNUM_P(b)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(b));
        base = (int)FIX2LONG(b);
        if (base < 2 || base > 36) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %d", base);
    }
    char buf[80];
    uint32_t len = korb_fmt_int(SELF_INT, base, buf);
    return korb_str_new(c, slots, buf, len);
}
static RESULT korb_m_int_chr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = SELF_INT;
    if (n < 0 || n > 255) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%ld out of char range", (long)n);
    char ch = (char)n;
    return korb_str_new(c, slots, &ch, 1);
}

/* floored integer division / modulo (Ruby semantics: quotient rounds toward
 * -inf, remainder takes the divisor's sign). */
static intptr_t korb_int_fdiv(intptr_t a, intptr_t b) {
    intptr_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static intptr_t korb_int_fmod(intptr_t a, intptr_t b) {
    intptr_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
static intptr_t korb_int_gcd(intptr_t a, intptr_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { intptr_t t = a % b; a = b; b = t; }
    return a;
}

static RESULT korb_m_int_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ev = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(ev))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(ev));
    intptr_t base = SELF_INT, exp = FIX2LONG(ev);
    if (exp < 0) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "negative exponent (Rational is not implemented)");
    intptr_t r = 1;
    for (intptr_t i = 0; i < exp; i++) {
        if (UNLIKELY(base != 0 && (r * base) / base != r))   /* overflow */
            return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
        r *= base;
        if (UNLIKELY(!FIXABLE(r))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    }
    return RESULT_OK(LONG2FIX(r));
}

static RESULT korb_m_int_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    intptr_t av = SELF_INT;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 2)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(korb_int_fdiv(av, b))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(korb_int_fmod(av, b))));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_int_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(korb_int_fdiv(SELF_INT, b)));
}

static RESULT korb_m_int_modulo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(korb_int_fmod(SELF_INT, b)));
}

static RESULT korb_m_int_gcd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    return RESULT_OK(LONG2FIX(korb_int_gcd(SELF_INT, FIX2LONG(bv))));
}

static RESULT korb_m_int_lcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t av = SELF_INT, b = FIX2LONG(bv);
    if (av == 0 || b == 0) return RESULT_OK(LONG2FIX(0));
    intptr_t g = korb_int_gcd(av, b);
    intptr_t l = (av / g) * b;
    if (l < 0) l = -l;
    if (UNLIKELY(!FIXABLE(l))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    return RESULT_OK(LONG2FIX(l));
}

static RESULT korb_m_int_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, (double)SELF_INT / o);
}
static RESULT korb_m_int_ceildiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(-korb_int_fdiv(-SELF_INT, b)));   /* ceil = -floor(-a/b) */
}
/* coerce(other) → [other, self] both as Integer, or both Float if other is Float */
static RESULT korb_m_int_coerce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    intptr_t s = SELF_INT;
    if (KORB_FLOAT_P(o)) {
        double od = VAL2FLT(o)->val;
        slots[0] = UNWRAP(korb_float_new(c, slots, od));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, (double)s));
    } else if (FIXNUM_P(o)) {
        slots[0] = o; slots[1] = LONG2FIX(s);
    } else {
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Integer", korb_type_name(o));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_int_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    double y;
    if (!korb_num_to_d(o, &y)) return RESULT_OK(KORB_NIL);   /* incomparable → nil */
    double x = (double)SELF_INT;
    return RESULT_OK(LONG2FIX((x > y) - (x < y)));
}
static RESULT korb_m_int_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_float_new(c, slots, (double)SELF_INT);
}

/* ---- Float methods ------------------------------------------------------- */
#define SELF_FLT (VAL2FLT(VALUE_REF_GET(self))->val)
static RESULT korb_m_flt_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_flt_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_float_new(c, slots, fabs(SELF_FLT)); }
static RESULT korb_m_flt_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT == 0.0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_nan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(isnan(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_inf(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; double d = SELF_FLT; return RESULT_OK(isinf(d) ? LONG2FIX(d < 0 ? -1 : 1) : KORB_NIL); }
static RESULT korb_m_flt_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    double o; if (!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o)) return RESULT_OK(KORB_NIL);
    double s = SELF_FLT; return RESULT_OK(LONG2FIX((s > o) - (s < o)));
}
/* round/floor/ceil/truncate → Integer (kind 0=floor 1=ceil 2=round 3=trunc) */
static RESULT korb_flt_toint(CTX *c, VALUE *slots, double d, int kind) {
    double t = kind == 0 ? floor(d) : kind == 1 ? ceil(d) : kind == 2 ? round(d) : trunc(d);
    if (UNLIKELY(!isfinite(t) || t < (double)FIXNUM_MIN || t > (double)FIXNUM_MAX))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Float out of Fixnum range (Bignum not implemented)");
    return RESULT_OK(LONG2FIX((intptr_t)t));
}
static RESULT korb_m_flt_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 3); }
static RESULT korb_m_flt_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 0); }
static RESULT korb_m_flt_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 1); }
static RESULT korb_m_flt_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 2); }
static RESULT korb_m_flt_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; char b[40]; uint32_t n = korb_float_to_s(SELF_FLT, b); return korb_str_new(c, slots, b, n);
}
static RESULT korb_m_flt_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, SELF_FLT / o);
}
static RESULT korb_m_flt_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return korb_flt_toint(c, slots, floor(SELF_FLT / o), 3);   /* floor → Integer */
}
static RESULT korb_m_flt_modulo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    double r = fmod(SELF_FLT, o);
    if (r != 0.0 && ((r < 0) != (o < 0))) r += o;             /* floored division remainder */
    return korb_float_new(c, slots, r);
}
static RESULT korb_m_flt_finite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(isfinite(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)INFINITY)); }
static RESULT korb_m_flt_prev(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)-INFINITY)); }
static RESULT korb_m_flt_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    double s = SELF_FLT, q = floor(s / o), r = s - o * q;
    RESULT qr = korb_flt_toint(c, slots, q, 3);          /* quotient → Integer */
    if (UNLIKELY(qr.state != KORB_NORMAL)) return qr;
    slots[0] = qr.value;
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, r));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_flt_nonzero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT != 0.0 ? VALUE_REF_GET(self) : KORB_NIL); }
static RESULT korb_m_flt_neg_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT < 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_pos_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT > 0 ? KORB_TRUE : KORB_FALSE); }
/* coerce(other) → [Float(other), Float(self)] */
static RESULT korb_m_flt_coerce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    double s = SELF_FLT;
    slots[0] = UNWRAP(korb_float_new(c, slots, o));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, s));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
#undef SELF_FLT

static RESULT korb_m_int_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo = VALUE_SLICE_GET(a, 0), hi = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!FIXNUM_P(lo) || !FIXNUM_P(hi))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
    intptr_t n = SELF_INT;
    return RESULT_OK((n >= FIX2LONG(lo) && n <= FIX2LONG(hi)) ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_int_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo = VALUE_SLICE_GET(a, 0), hi = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!FIXNUM_P(lo) || !FIXNUM_P(hi))) return korb_raise(c, slots, KORB_E_TYPE, 0, "comparison failed");
    intptr_t n = SELF_INT, l = FIX2LONG(lo), h = FIX2LONG(hi);
    return RESULT_OK(LONG2FIX(n < l ? l : (n > h ? h : n)));
}

static RESULT korb_m_int_digits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t n = SELF_INT;
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of domain");
    intptr_t base = 10;
    if (VALUE_SLICE_LEN(a) >= 1) {
        if (!FIXNUM_P(VALUE_SLICE_GET(a, 0))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        base = FIX2LONG(VALUE_SLICE_GET(a, 0));
        if (base < 2) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %ld", (long)base);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    do {
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(n % base)));
        n /= base;
    } while (n > 0);
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* ---- String methods ------------------------------------------------------ */

#define SELF_STR  VAL2STR(VALUE_REF_GET(self))
static RESULT korb_m_str_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_STR->len)); }
static RESULT korb_m_str_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_STR->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_str_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_str_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbString *s = SELF_STR;
    return RESULT_OK(ID2SYM(korb_intern(c->vm, s->buf->data, s->len)));
}
/* transform-into-new-string helper (op: 0=upcase 1=downcase 2=capitalize 3=reverse) */
static RESULT korb_str_transform(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = SELF_STR;   /* re-read after alloc (GC may have moved it) */
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        unsigned char out;
        switch (op) {
          case 0: out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch; break;
          case 1: out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch; break;
          case 2:
            if (i == 0) out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch;
            else        out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch;
            break;
          default: out = (unsigned char)s->buf->data[len - 1 - i]; break;
        }
        r->buf->data[i] = (char)out;
    }
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_upcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_str_transform(c, slots, self, 0); }
static RESULT korb_m_str_downcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform(c, slots, self, 1); }
static RESULT korb_m_str_capitalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_transform(c, slots, self, 2); }
static RESULT korb_m_str_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)a; return korb_str_transform(c, slots, self, 3); }

/* byte-substring search: index of needle in hay[0..hlen), or -1 (empty matches at 0) */
static int32_t
korb_byte_find(const char *hay, uint32_t hlen, const char *needle, uint32_t nlen)
{
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (uint32_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return (int32_t)i;
    return -1;
}

static inline bool korb_is_ws(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/* count UTF-8 codepoints in the first nbytes bytes (lead bytes only) */
static uint32_t
korb_utf8_count(const char *b, uint32_t nbytes)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < nbytes; i++)
        if (((unsigned char)b[i] & 0xC0) != 0x80) n++;
    return n;
}

/* byte offset of codepoint index ci (clamped to [0, len]) */
static uint32_t
korb_utf8_byteoff(const char *b, uint32_t len, uint32_t ci)
{
    uint32_t i = 0, n = 0;
    while (i < len && n < ci) {
        i++;
        while (i < len && ((unsigned char)b[i] & 0xC0) == 0x80) i++;
        n++;
    }
    return i;
}

/* alloc a fresh String = self->buf->data[start, start+len); re-reads self after the
 * alloc-GC (source may have moved) — THE safe substring primitive. */
static RESULT
korb_str_slice_new(CTX *c, VALUE *slots, VALUE_REF sref, uint32_t start, uint32_t len)
{
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = VAL2STR(VALUE_REF_GET(sref));   /* re-read: GC may have moved it */
    memcpy(r->buf->data, s->buf->data + start, len);
    return RESULT_OK((VALUE)r);
}

/* ---- mutable String operations (in place; header never moves) ------------ */

static uint32_t korb_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0|(cp>>6)); out[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F)); out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>12)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4;
}
/* append other (a rooted String) onto self in place */
static RESULT korb_str_append_str(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF other) {
    uint32_t on = VAL2STR(VALUE_REF_GET(other))->len;
    KorbString *s = korb_str_ensure(c, slots, self, VAL2STR(VALUE_REF_GET(self))->len + on);
    const KorbString *o = VAL2STR(VALUE_REF_GET(other));   /* re-read after grow */
    memcpy(s->buf->data + s->len, o->buf->data, on);
    s->len += on; s->buf->data[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* append one element (String or Integer codepoint) onto self */
static RESULT korb_str_append_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF oref) {
    VALUE o = VALUE_REF_GET(oref);
    if (KORB_STRING_P(o)) return korb_str_append_str(c, slots, self, oref);
    if (FIXNUM_P(o)) {
        intptr_t cp = FIX2LONG(o);
        if (cp < 0 || cp > 0x10FFFF) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%ld out of char range", (long)cp);
        char buf[4]; uint32_t n = korb_utf8_encode((uint32_t)cp, buf);   /* stable C buffer */
        return korb_str_cat(c, slots, self, buf, n);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
}
static RESULT korb_m_str_ltlt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    CHECK(korb_str_append_one(c, slots, self, VALUE_SLICE_REF(a, 0)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_concat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++)
        CHECK(korb_str_append_one(c, slots, self, VALUE_SLICE_REF(a, j)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
    VAL2STR(VALUE_REF_GET(self))->len = 0;             /* clear, then append other */
    return korb_str_append_str(c, slots, self, VALUE_SLICE_REF(a, 0));
}
static RESULT korb_m_str_prepend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t pn = 0;
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE o = VALUE_SLICE_GET(a, j);
        if (UNLIKELY(!KORB_STRING_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
        pn += VAL2STR(o)->len;
    }
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    KorbString *s = korb_str_ensure(c, slots, self, slen + pn);   /* single grow; args rooted */
    s = VAL2STR(VALUE_REF_GET(self));
    memmove(s->buf->data + pn, s->buf->data, slen);
    uint32_t off = 0;
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const KorbString *o = VAL2STR(VALUE_SLICE_GET(a, j));
        memcpy(s->buf->data + off, o->buf->data, o->len); off += o->len;
    }
    s->len = slen + pn; s->buf->data[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    s->len = 0; s->buf->data[0] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* in-place case/reverse (op: 0 upcase 1 downcase 2 capitalize 3 swapcase 4 reverse);
 * returns self if changed, else nil (Ruby bang convention) — reverse! always self. */
static RESULT korb_str_transform_bang(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    (void)c;(void)slots;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len; bool changed = false;
    if (op == 4) {                                     /* reverse! (byte reverse) */
        for (uint32_t i = 0; i < len / 2; i++) { char t = s->buf->data[i]; s->buf->data[i] = s->buf->data[len-1-i]; s->buf->data[len-1-i] = t; }
        return RESULT_OK(VALUE_REF_GET(self));
    }
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i], out = ch;
        switch (op) {
          case 0: if (ch >= 'a' && ch <= 'z') out = (unsigned char)(ch - 32); break;
          case 1: if (ch >= 'A' && ch <= 'Z') out = (unsigned char)(ch + 32); break;
          case 2:
            if (i == 0) { if (ch >= 'a' && ch <= 'z') out = (unsigned char)(ch - 32); }
            else { if (ch >= 'A' && ch <= 'Z') out = (unsigned char)(ch + 32); }
            break;
          default: out = (unsigned char)(isupper(ch) ? tolower(ch) : islower(ch) ? toupper(ch) : ch); break;
        }
        if (out != ch) { s->buf->data[i] = (char)out; changed = true; }
    }
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_upcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_str_transform_bang(c, slots, self, 0); }
static RESULT korb_m_str_downcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform_bang(c, slots, self, 1); }
static RESULT korb_m_str_capitalize_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_transform_bang(c, slots, self, 2); }
static RESULT korb_m_str_swapcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform_bang(c, slots, self, 3); }
static RESULT korb_m_str_reverse_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)a; return korb_str_transform_bang(c, slots, self, 4); }

/* Replace self bytes [bs,be) with replref (a rooted String); do_repl=false deletes. */
static RESULT korb_str_splice(CTX *c, VALUE *slots, VALUE_REF self, uint32_t bs, uint32_t be, VALUE_REF replref, bool do_repl) {
    uint32_t rn = do_repl ? VAL2STR(VALUE_REF_GET(replref))->len : 0;
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    uint32_t newlen = slen - (be - bs) + rn;
    KorbString *s = korb_str_ensure(c, slots, self, newlen);   /* alloc; self+repl rooted */
    s = VAL2STR(VALUE_REF_GET(self));
    memmove(s->buf->data + bs + rn, s->buf->data + be, slen - be);
    if (rn) { const KorbString *r = VAL2STR(VALUE_REF_GET(replref)); memcpy(s->buf->data + bs, r->buf->data, rn); }
    s->len = newlen; s->buf->data[newlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Compute byte span [*bs,*be) for a string index target. idx + optional len arg
 * (len_v = KORB_NIL if absent). *found=false ⇒ no match / out of range. */
static RESULT korb_str_target_span(CTX *c, VALUE *slots, VALUE_REF self, VALUE idx, VALUE len_v, bool *found, uint32_t *bs, uint32_t *be) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    *found = true;
    if (KORB_STRING_P(idx)) {                          /* substring target */
        const KorbString *sub = VAL2STR(idx);
        int32_t at = korb_byte_find(s->buf->data, s->len, sub->buf->data, sub->len);
        if (at < 0) { *found = false; return RESULT_OK(KORB_NIL); }
        *bs = (uint32_t)at; *be = (uint32_t)at + sub->len;
        return RESULT_OK(KORB_NIL);
    }
    intptr_t st, ln;
    if (KORB_RANGE_P(idx)) {
        const KorbRange *r = VAL2RANGE(idx);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += ncp;
        if (e < 0) e += ncp;
        st = b; ln = (r->exclude_end ? e - 1 : e) - b + 1; if (ln < 0) ln = 0;
    } else {
        if (UNLIKELY(!korb_to_index(idx, &st))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(idx));
        if (st < 0) st += ncp;
        if (len_v != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(len_v, &ln))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(len_v));
        } else ln = 1;
    }
    if (st < 0 || st > (intptr_t)ncp || ln < 0) { *found = false; return RESULT_OK(KORB_NIL); }
    if (st + ln > (intptr_t)ncp) ln = (intptr_t)ncp - st;
    *bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)st);
    *be = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(st + ln));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE repl = VALUE_SLICE_GET(a, na - 1);
    if (UNLIKELY(!KORB_STRING_P(repl))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(repl));
    VALUE idx = VALUE_SLICE_GET(a, 0);
    VALUE len_v = (na == 3) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    bool found; uint32_t bs = 0, be = 0;
    RESULT sp = korb_str_target_span(c, slots, self, idx, len_v, &found, &bs, &be);
    if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    if (!found) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index out of string");
    CHECK(korb_str_splice(c, slots, self, bs, be, VALUE_SLICE_REF(a, na - 1), true));
    return RESULT_OK(repl);
}
static RESULT korb_m_str_slice_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE idx = VALUE_SLICE_GET(a, 0);
    VALUE len_v = (na >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    bool found; uint32_t bs = 0, be = 0;
    RESULT sp = korb_str_target_span(c, slots, self, idx, len_v, &found, &bs, &be);
    if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    if (!found) return RESULT_OK(KORB_NIL);
    slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, bs, be - bs));   /* removed part */
    CHECK(korb_str_splice(c, slots + 1, self, bs, be, self, false));      /* delete it */
    return RESULT_OK(slots[0]);
}
/* in-place whitespace strip (mode: 0 both, 1 left, 2 right). self if changed else nil. */
static RESULT korb_str_strip_bang(CTX *c, VALUE *slots, VALUE_REF self, int mode) {
    (void)c;(void)slots;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t lo = 0, hi = s->len;
    if (mode != 2) while (lo < hi && (unsigned char)s->buf->data[lo] <= ' ') lo++;
    if (mode != 1) while (hi > lo && (unsigned char)s->buf->data[hi-1] <= ' ') hi--;
    if (lo == 0 && hi == s->len) return RESULT_OK(KORB_NIL);   /* unchanged */
    uint32_t nlen = hi - lo;
    if (lo) memmove(s->buf->data, s->buf->data + lo, nlen);
    s->len = nlen; s->buf->data[nlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_strip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_str_strip_bang(c, slots, self, 0); }
static RESULT korb_m_str_lstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_strip_bang(c, slots, self, 1); }
static RESULT korb_m_str_rstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_strip_bang(c, slots, self, 2); }
static RESULT korb_m_str_chomp_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    if (n >= 1 && s->buf->data[n-1] == '\n') { n--; if (n >= 1 && s->buf->data[n-1] == '\r') n--; }
    else if (n >= 1 && s->buf->data[n-1] == '\r') n--;
    else return RESULT_OK(KORB_NIL);
    s->len = n; s->buf->data[n] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_chop_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len == 0) return RESULT_OK(KORB_NIL);
    uint32_t n = s->len;
    if (n >= 2 && s->buf->data[n-1] == '\n' && s->buf->data[n-2] == '\r') n -= 2;
    else {
        n--;                                           /* back up over one UTF-8 char */
        while (n > 0 && ((unsigned char)s->buf->data[n] & 0xC0) == 0x80) n--;
    }
    s->len = n; s->buf->data[n] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}

/* char-set membership for count/squeeze/delete: supports leading ^ negation and
 * a-z ranges (ASCII-byte level). */
static bool korb_charset_match(const char *set, uint32_t n, unsigned char ch) {
    bool neg = false; uint32_t i = 0;
    if (n > 0 && set[0] == '^') { neg = true; i = 1; }
    bool in = false;
    for (; i < n; i++) {
        if (i + 2 < n && set[i+1] == '-') {
            if ((unsigned char)set[i] <= ch && ch <= (unsigned char)set[i+2]) in = true;
            i += 2;
        } else if ((unsigned char)set[i] == ch) in = true;
    }
    return neg ? !in : in;
}
/* true if ch is in EVERY set arg (Ruby count/delete intersect multiple sets) */
static bool korb_str_sets_match(VALUE_SLICE a, unsigned char ch) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE sv = VALUE_SLICE_GET(a, j);
        if (!KORB_STRING_P(sv)) continue;
        const KorbString *set = VAL2STR(sv);
        if (!korb_charset_match(set->buf->data, set->len, ch)) return false;
    }
    return true;
}
static RESULT korb_m_str_ascii_only(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < s->len; i++)
        if ((unsigned char)s->buf->data[i] >= 0x80) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
/* rpartition(sep) → [before, sep, after] split at the LAST occurrence of sep. */
static RESULT korb_m_str_rpartition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *sep = VAL2STR(sv);
    int32_t at = -1;
    if (sep->len == 0) at = (int32_t)s->len;
    else for (int32_t i = (int32_t)s->len - (int32_t)sep->len; i >= 0; i--)
        if (memcmp(s->buf->data + i, sep->buf->data, sep->len) == 0) { at = i; break; }
    uint32_t pre_e, post_s;
    if (at < 0) { pre_e = 0; post_s = 0; }            /* not found → ["","",self] */
    else { pre_e = (uint32_t)at; post_s = (uint32_t)at + sep->len; }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 3)));
    if (at < 0) {
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        CHECK(korb_ary_push_val(c, slots + 1, dst, VALUE_REF_GET(self)));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, pre_e));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* the separator */
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, post_s, VAL2STR(VALUE_REF_GET(self))->len - post_s));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t cnt = 0;
    for (uint32_t i = 0; i < s->len; i++)
        if (korb_str_sets_match(a, (unsigned char)s->buf->data[i])) cnt++;
    return RESULT_OK(LONG2FIX(cnt));
}
static RESULT korb_m_str_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t bits = 16;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) bits = FIX2LONG(VALUE_SLICE_GET(a, 0));
    intptr_t sum = 0;
    for (uint32_t i = 0; i < s->len; i++) sum += (unsigned char)s->buf->data[i];
    if (bits > 0 && bits < 64) sum &= ((intptr_t)1 << bits) - 1;
    return RESULT_OK(LONG2FIX(sum));
}
/* squeeze: collapse runs of identical chars (only those in the sets, if given). */
static RESULT korb_str_squeeze_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool in_place) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    bool has_set = VALUE_SLICE_LEN(a) > 0;
    /* build squeezed bytes into the dst string */
    KorbString *r = korb_str_alloc(c, slots, n);          /* capacity n (worst case) */
    s = VAL2STR(VALUE_REF_GET(self));                      /* re-read after alloc */
    uint32_t w = 0; int prev = -1;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        bool squeezable = !has_set || korb_str_sets_match(a, ch);
        if (squeezable && (int)ch == prev) continue;
        r->buf->data[w++] = (char)ch;
        prev = squeezable ? (int)ch : -1;
    }
    r->len = w; r->buf->data[w] = '\0';
    if (!in_place) return RESULT_OK((VALUE)r);
    /* copy back into self */
    slots[0] = (VALUE)r;
    bool changed = (w != n);
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    r = VAL2STR(slots[0]);
    memcpy(s2->buf->data, r->buf->data, w);
    s2->len = w; s2->buf->data[w] = '\0';
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_squeeze(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_squeeze_into(c, slots, self, a, false); }
static RESULT korb_m_str_squeeze_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_squeeze_into(c, slots, self, a, true); }
/* append_as_bytes(*objs): append each Integer as a byte (low 8 bits) / String bytes. */
static RESULT korb_m_str_append_as_bytes(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE o = VALUE_SLICE_GET(a, j);
        if (FIXNUM_P(o)) {
            char b = (char)(FIX2LONG(o) & 0xFF);
            CHECK(korb_str_cat(c, slots, self, &b, 1));
        } else if (KORB_STRING_P(o)) {
            CHECK(korb_str_append_str(c, slots, self, VALUE_SLICE_REF(a, j)));
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Integer or String)", korb_type_name(o));
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_str_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    return RESULT_OK(korb_byte_find(s->buf->data, s->len, n->buf->data, n->len) >= 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_str_start_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(s->buf->data, p->buf->data, p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_str_end_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(s->buf->data + s->len - p->len, p->buf->data, p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    int32_t b = korb_byte_find(s->buf->data, s->len, n->buf->data, n->len);
    if (b < 0) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, (uint32_t)b)));   /* char index */
}

static RESULT korb_m_str_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t i = 0; while (i < s->len && korb_is_ws((unsigned char)s->buf->data[i])) i++;
    intptr_t sign = 1;
    if (i < s->len && (s->buf->data[i] == '+' || s->buf->data[i] == '-')) { if (s->buf->data[i] == '-') sign = -1; i++; }
    intptr_t n = 0;
    while (i < s->len && s->buf->data[i] >= '0' && s->buf->data[i] <= '9') { n = n * 10 + (s->buf->data[i] - '0'); i++; }
    return RESULT_OK(LONG2FIX(sign * n));
}

/* trim: mode 0=both 1=left 2=right */
static RESULT korb_str_strip(CTX *c, VALUE *slots, VALUE_REF self, int mode) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t start = 0, end = s->len;
    if (mode != 2) while (start < end && (korb_is_ws((unsigned char)s->buf->data[start]) || s->buf->data[start] == '\0')) start++;
    if (mode != 1) while (end > start && (korb_is_ws((unsigned char)s->buf->data[end-1]) || s->buf->data[end-1] == '\0')) end--;
    return korb_str_slice_new(c, slots, self, start, end - start);
}
static RESULT korb_m_str_strip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_str_strip(c, slots, self, 0); }
static RESULT korb_m_str_lstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_strip(c, slots, self, 1); }
static RESULT korb_m_str_rstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_strip(c, slots, self, 2); }

static RESULT korb_m_str_chomp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (len >= 2 && s->buf->data[len-2] == '\r' && s->buf->data[len-1] == '\n') len -= 2;
    else if (len >= 1 && (s->buf->data[len-1] == '\n' || s->buf->data[len-1] == '\r')) len -= 1;
    return korb_str_slice_new(c, slots, self, 0, len);
}

static RESULT korb_m_str_chop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (len >= 2 && s->buf->data[len-2] == '\r' && s->buf->data[len-1] == '\n') len -= 2;
    else if (len >= 1) {
        len--;                                  /* drop a whole trailing UTF-8 codepoint */
        while (len > 0 && ((unsigned char)s->buf->data[len] & 0xC0) == 0x80) len--;
    }
    return korb_str_slice_new(c, slots, self, 0, len);
}

/* String#split(sep=nil): nil/" " → whitespace runs; string sep → that literal
 * (trailing empty fields dropped). */
static RESULT korb_m_str_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sepv = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    bool ws = (sepv == KORB_NIL);
    if (!ws) {
        if (UNLIKELY(!KORB_STRING_P(sepv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sepv));
        const KorbString *sp = VAL2STR(sepv);
        if (sp->len == 1 && sp->buf->data[0] == ' ') ws = true;   /* " " behaves as whitespace */
    }
    VALUE_REF sepref = ws ? (VALUE_REF){0} : VALUE_SLICE_REF(a, 0);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read each iter */
        uint32_t slen = s->len;
        if (ws) {
            while (pos < slen && korb_is_ws((unsigned char)s->buf->data[pos])) pos++;
            if (pos >= slen) break;
            uint32_t start = pos;
            while (pos < slen && !korb_is_ws((unsigned char)s->buf->data[pos])) pos++;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, start, pos - start))));
        } else {
            const KorbString *sep = VAL2STR(VALUE_REF_GET(sepref));
            uint32_t seplen = sep->len;
            int32_t found = (pos <= slen) ? korb_byte_find(s->buf->data + pos, slen - pos, sep->buf->data, seplen) : -1;
            if (found < 0) {
                CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, slen - pos))));
                break;
            }
            uint32_t end = pos + (uint32_t)found;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, end - pos))));
            pos = end + (seplen ? seplen : 1);
        }
    }
    if (!ws) {   /* CRuby drops trailing empty fields for an explicit separator */
        KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        while (d->len > 0 && KORB_STRING_P(d->items->data[d->len-1]) && VAL2STR(d->items->data[d->len-1])->len == 0) {
            d->items->data[--d->len] = KORB_NIL;
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_str_charlen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, s->len)));
}

static RESULT korb_m_str_chars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        if (pos >= s->len) break;
        uint32_t cl = 1;                                  /* one UTF-8 codepoint */
        while (pos + cl < s->len && ((unsigned char)s->buf->data[pos+cl] & 0xC0) == 0x80) cl++;
        CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, cl))));
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_str_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(korb_cmp_values(VALUE_REF_GET(self), o)));
}

/* String#[] — int index, (int,len), Range, or substring match.  Indices are
 * codepoints; results are fresh strings (or nil). */
static RESULT korb_m_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = SELF_STR;
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    VALUE i0 = VALUE_SLICE_GET(a, 0);

    if (KORB_STRING_P(i0)) {                       /* s[substr] → copy of substr if present */
        const KorbString *sub = VAL2STR(i0);
        if (korb_byte_find(s->buf->data, s->len, sub->buf->data, sub->len) < 0) return RESULT_OK(KORB_NIL);
        return korb_str_slice_new(c, slots, VALUE_SLICE_REF(a, 0), 0, sub->len);
    }
    if (KORB_RANGE_P(i0)) {
        const KorbRange *r = VAL2RANGE(i0);
        if (UNLIKELY(!FIXNUM_P(r->rbegin) || !FIXNUM_P(r->rend))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t b = FIX2LONG(r->rbegin), e = FIX2LONG(r->rend);
        if (b < 0) b += ncp;
        if (e < 0) e += ncp;
        if (b < 0 || b > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        intptr_t last = r->exclude_end ? e - 1 : e;
        intptr_t cnt = last - b + 1;
        if (cnt < 0) cnt = 0;
        if (b + cnt > (intptr_t)ncp) cnt = (intptr_t)ncp - b;
        uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)b);
        uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(b + cnt));
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    intptr_t i;
    if (UNLIKELY(!korb_to_index(i0, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(i0));
    if (i < 0) i += ncp;

    if (VALUE_SLICE_LEN(a) >= 2) {                  /* s[start, len] */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        intptr_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
        if (len < 0 || i < 0 || i > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        if (i + len > (intptr_t)ncp) len = (intptr_t)ncp - i;
        uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)i);
        uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(i + len));
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    if (i < 0 || i >= (intptr_t)ncp) return RESULT_OK(KORB_NIL);   /* single codepoint */
    uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)i);
    uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(i + 1));
    return korb_str_slice_new(c, slots, self, bs, es - bs);
}

static RESULT korb_m_str_each_char(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "String#each_char without a block (Enumerator) is not supported");
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t cl = 1;
        while (pos + cl < s->len && ((unsigned char)s->buf->data[pos+cl] & 0xC0) == 0x80) cl++;
        slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, pos, cl));   /* root the char */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* ---- Symbol methods ------------------------------------------------------ */

static RESULT korb_m_sym_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
static RESULT korb_m_sym_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_sym_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return RESULT_OK(LONG2FIX((intptr_t)strlen(nm)));
}

/* ---- nil / true / false methods ------------------------------------------ */

static RESULT korb_m_nil_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "", 0); }
static RESULT korb_m_nil_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(0)); }
static RESULT korb_m_true_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "true", 4); }
static RESULT korb_m_false_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "false", 5); }

/* ---- universal (Object) methods ------------------------------------------ */

static RESULT korb_m_obj_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static RESULT korb_m_nil_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_obj_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_neq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_FALSE : KORB_TRUE); }
static RESULT korb_m_obj_equal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(VALUE_REF_GET(self) == VALUE_SLICE_GET(a,0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_itself(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_obj_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)) ? LONG2FIX(0) : KORB_NIL); }

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
static RESULT korb_m_obj_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_to_s(c, ms, VALUE_REF_GET(self));   /* no GC inside */
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_obj_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_inspect(c, ms, VALUE_REF_GET(self));
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_obj_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(korb_class_obj_of(c, VALUE_REF_GET(self)));
}
static RESULT korb_m_obj_is_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE target = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(target))) return korb_raise(c, slots, KORB_E_TYPE, 0, "class or module required");
    if (target == korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT])) return RESULT_OK(KORB_TRUE);
    VALUE cls = korb_class_obj_of(c, VALUE_REF_GET(self));
    while (KORB_CLASS_P(cls)) { if (cls == target) return RESULT_OK(KORB_TRUE); cls = VAL2CLASS(cls)->superclass; }
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_obj_instance_of(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE target = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(target))) return korb_raise(c, slots, KORB_E_TYPE, 0, "class or module required");
    return RESULT_OK(korb_class_obj_of(c, VALUE_REF_GET(self)) == target ? KORB_TRUE : KORB_FALSE);
}

/* Exception#message / to_s — the stored message, or the class name if none. */
static RESULT korb_m_exc_message(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbException *e = VAL2EXC(VALUE_REF_GET(self));
    if (e->msg != KORB_NIL) return RESULT_OK(e->msg);
    const char *nm = korb_etype_name(e->etype);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}

/* ---- Array methods ------------------------------------------------------- */

#define SELF_ARY  VAL2ARY(VALUE_REF_GET(self))
static RESULT korb_m_ary_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_ARY->len)); }
static RESULT korb_m_ary_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_ARY->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_ary_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_ary_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[0] : KORB_NIL); }
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[ary->len - 1] : KORB_NIL); }

/* fresh array = self[start, len) */
static RESULT korb_ary_subseq(CTX *c, VALUE *slots, VALUE_REF self, uint32_t start, uint32_t len) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++)
        CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(VALUE_REF_GET(self))->items->data[start + i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    uint32_t n = SELF_ARY->len;
    VALUE i0 = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(i0)) {                                 /* a[b..e] → subarray */
        const KorbRange *r = VAL2RANGE(i0);
        if (UNLIKELY(!FIXNUM_P(r->rbegin) || !FIXNUM_P(r->rend))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t b = FIX2LONG(r->rbegin), e = FIX2LONG(r->rend);
        if (b < 0) b += n;
        if (e < 0) e += n;
        if (b < 0 || b > (intptr_t)n) return RESULT_OK(KORB_NIL);
        intptr_t last = r->exclude_end ? e - 1 : e, cnt = last - b + 1;
        if (cnt < 0) cnt = 0;
        if (b + cnt > (intptr_t)n) cnt = (intptr_t)n - b;
        return korb_ary_subseq(c, slots, self, (uint32_t)b, (uint32_t)cnt);
    }
    intptr_t i;
    if (UNLIKELY(!korb_to_index(i0, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(i0));
    if (i < 0) i += n;
    if (VALUE_SLICE_LEN(a) >= 2) {                          /* a[start, len] → subarray */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        intptr_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
        if (len < 0 || i < 0 || i > (intptr_t)n) return RESULT_OK(KORB_NIL);
        if (i + len > (intptr_t)n) len = (intptr_t)n - i;
        return korb_ary_subseq(c, slots, self, (uint32_t)i, (uint32_t)len);
    }
    if (i < 0 || (uint32_t)i >= n) return RESULT_OK(KORB_NIL);
    return RESULT_OK(SELF_ARY->items->data[i]);
}

/* Replace self[start, dellen) with valref (spliced if Array, else single element).
 * `valref` must be a rooted slot. Returns the replacement value. */
static RESULT korb_ary_splice(CTX *c, VALUE *slots, VALUE_REF self, intptr_t start, intptr_t dellen, VALUE_REF valref) {
    intptr_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (start < 0) start += len;
    if (UNLIKELY(start < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array; minimum: -%ld", (long)(start - len + len), (long)len);
    if (dellen < 0) dellen = 0;
    bool splat = KORB_ARRAY_P(VALUE_REF_GET(valref));
    /* build the new sequence in a temp array (rooted), then copy back into self */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF tmp = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = 0; i < start; i++) {
        VALUE e = (i < len) ? VAL2ARY(VALUE_REF_GET(self))->items->data[i] : KORB_NIL;   /* pad gap with nil */
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    if (splat) {
        uint32_t vn = VAL2ARY(VALUE_REF_GET(valref))->len;
        for (uint32_t j = 0; j < vn; j++) {
            VALUE e = VAL2ARY(VALUE_REF_GET(valref))->items->data[j];
            CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
        }
    } else {
        CHECK(korb_ary_push_val(c, slots + 1, tmp, VALUE_REF_GET(valref)));
    }
    for (intptr_t i = start + dellen; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    /* overwrite self with tmp */
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t tn = VAL2ARY(VALUE_REF_GET(tmp))->len;
    for (uint32_t j = 0; j < tn; j++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(tmp))->items->data[j];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(valref));
}
static RESULT korb_m_ary_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2..3)", VALUE_SLICE_LEN(a));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (VALUE_SLICE_LEN(a) >= 3) {                        /* a[start, len] = val */
        intptr_t start, dellen;
        if (UNLIKELY(!korb_to_index(iv, &start)))               return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &dellen))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        return korb_ary_splice(c, slots, self, start, dellen, VALUE_SLICE_REF(a, 2));
    }
    if (KORB_RANGE_P(iv)) {                               /* a[b..e] = val */
        const KorbRange *r = VAL2RANGE(iv);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t len = VAL2ARY(VALUE_REF_GET(self))->len;
        if (b < 0) b += len;
        if (e < 0) e += len;
        intptr_t last = r->exclude_end ? e - 1 : e, dellen = last - b + 1;
        if (dellen < 0) dellen = 0;
        return korb_ary_splice(c, slots, self, b, dellen, VALUE_SLICE_REF(a, 1));
    }
    KorbArray *ary = SELF_ARY;
    intptr_t i;
    if (UNLIKELY(!korb_to_index(iv, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    if (i < 0) i += ary->len;
    if (UNLIKELY(i < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array; minimum: -%u", (long)i, ary->len);
    if ((uint32_t)i >= ary->len) {
        CHECK(korb_ary_ensure(c, slots, self, (uint32_t)i + 1 - ary->len));
        ary = SELF_ARY;                                  /* re-read after grow GC */
        for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ary->items->data[k] = KORB_NIL;
        ary->len = (uint32_t)i + 1;
    }
    VALUE val = VALUE_SLICE_GET(a, 1);                    /* re-read (rooted) after GC */
    KorbArrayItems *it = ary->items;
    ARO_STORE(c, it, &it->data[i], val);
    return RESULT_OK(val);
}

/* slice!: remove and return element (single index) or subarray (range/start,len). */
static RESULT korb_m_ary_slice_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    intptr_t start, dellen; bool subseq_form = false;
    if (KORB_RANGE_P(iv)) {
        const KorbRange *r = VAL2RANGE(iv);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += n;
        if (e < 0) e += n;
        intptr_t last = r->exclude_end ? e - 1 : e;
        start = b; dellen = last - b + 1; if (dellen < 0) dellen = 0;
        subseq_form = true;
    } else if (VALUE_SLICE_LEN(a) >= 2) {
        if (UNLIKELY(!korb_to_index(iv, &start) || !korb_to_index(VALUE_SLICE_GET(a, 1), &dellen))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (start < 0) start += n;
        subseq_form = true;
    } else {
        if (UNLIKELY(!korb_to_index(iv, &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (start < 0) start += n;
        if (start < 0 || start >= n) return RESULT_OK(KORB_NIL);
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[start];   /* removed elem */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));              /* empty replacement */
        CHECK(korb_ary_splice(c, slots + 2, self, start, 1, VALUE_REF_AT(&slots[1])));
        return RESULT_OK(slots[0]);
    }
    if (start < 0 || start > n) return RESULT_OK(KORB_NIL);
    if (dellen < 0) dellen = 0;
    if (start + dellen > n) dellen = n - start;
    slots[0] = UNWRAP(korb_ary_subseq(c, slots, self, (uint32_t)start, (uint32_t)dellen));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
    CHECK(korb_ary_splice(c, slots + 2, self, start, dellen, VALUE_REF_AT(&slots[1])));
    (void)subseq_form;
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_ltlt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, 0)));
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, i)));   /* slice rooted across grow */
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_pop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbArray *ary = SELF_ARY;
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    ary->len--;
    VALUE v = ary->items->data[ary->len];
    ary->items->data[ary->len] = KORB_NIL;               /* drop the reference (nil needs no WB) */
    return RESULT_OK(v);
}

static RESULT korb_m_ary_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbArray *ary = SELF_ARY;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ary->len; i++)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_ary_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE elem = SELF_ARY->items->data[n - 1 - i];   /* push_val roots elem before any GC */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    return korb_ary_plus_ref(c, slots, self, VALUE_SLICE_REF(a, 0));
}
static RESULT korb_m_ary_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mul_slow(c, slots, self, VALUE_SLICE_GET(a, 0), 0);   /* n→repeat, String→join */
}
static RESULT korb_m_str_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mul_slow(c, slots, self, VALUE_SLICE_GET(a, 0), 0);   /* String * n → repeat */
}
#undef SELF_ARY

/* ---- yielding methods (drive a block) ------------------------------------ */

#define REQUIRE_BLOCK(what) \
    do { if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, \
        what " without a block (Enumerator) is not supported"); } while (0)

static RESULT korb_m_ary_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; REQUIRE_BLOCK("Array#each");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));   /* re-read each iter (GC) */
        if (i >= ary->len) break;
        VALUE elem = ary->items->data[i];                      /* copied into bf before GC */
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; REQUIRE_BLOCK("Array#reverse_each");
    uint32_t i = VAL2ARY(VALUE_REF_GET(self))->len;
    while (i > 0) {
        i--;
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) continue;                           /* shrunk during iteration */
        VALUE elem = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; REQUIRE_BLOCK("Array#each_with_index");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        VALUE argv[2] = { ary->items->data[i], LONG2FIX(i) };
        RESULT r = korb_block_yield(c, slots, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; REQUIRE_BLOCK("Array#map");
    uint32_t n0 = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n0)));  /* slots now past dst */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        VALUE elem = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots, dst, r.value));      /* push roots r.value */
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_int_times(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; REQUIRE_BLOCK("Integer#times");
    intptr_t n = FIX2LONG(VALUE_REF_GET(self));
    for (intptr_t i = 0; i < n; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_int_upto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    REQUIRE_BLOCK("Integer#upto");
    VALUE lv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(lv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
    intptr_t to = FIX2LONG(lv);
    for (intptr_t i = FIX2LONG(VALUE_REF_GET(self)); i <= to; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_int_downto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    REQUIRE_BLOCK("Integer#downto");
    VALUE lv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(lv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
    intptr_t to = FIX2LONG(lv);
    for (intptr_t i = FIX2LONG(VALUE_REF_GET(self)); i >= to; i--) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* ---- Hash methods -------------------------------------------------------- */

#define SELF_HASH  VAL2HASH(VALUE_REF_GET(self))
static RESULT korb_m_hash_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_HASH->len)); }
static RESULT korb_m_hash_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_hash_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_hash_default(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->default_val); }

static RESULT korb_m_hash_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    return RESULT_OK(idx < 0 ? h->default_val : h->items->data[2 * idx + 1]);
}

static RESULT korb_m_hash_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    CHECK(korb_hash_set(c, slots, self, VALUE_SLICE_REF(a, 0), VALUE_SLICE_GET(a, 1)));
    return RESULT_OK(VALUE_SLICE_GET(a, 1));        /* []= yields the value (rooted re-read) */
}

static RESULT korb_m_hash_key_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    return RESULT_OK(korb_hash_find(SELF_HASH, VALUE_SLICE_GET(a, 0)) >= 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_hash_value_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbHash *h = SELF_HASH;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(h->items->data[2 * i + 1], needle)) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_hash_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    if (idx >= 0) return RESULT_OK(h->items->data[2 * idx + 1]);
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    return korb_raise(c, slots, KORB_E_RUNTIME, 0, "key not found");
}

/* collect keys (sel 0) or values (sel 1) into a new array */
static RESULT korb_hash_collect(CTX *c, VALUE *slots, VALUE_REF self, int sel) {
    uint32_t n = SELF_HASH->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_HASH->items->data[2 * i + sel];   /* push roots e first */
        CHECK(korb_ary_push_val(c, slots, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_hash_collect(c, slots, self, 0); }
static RESULT korb_m_hash_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_hash_collect(c, slots, self, 1); }

static RESULT korb_m_hash_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    if (idx < 0) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = h->items;
    VALUE removed = it->data[2 * idx + 1];               /* held; no GC before return */
    for (uint32_t i = (uint32_t)idx; i + 1 < h->len; i++) {  /* shift to keep order */
        ARO_STORE(c, it, &it->data[2 * i],     it->data[2 * (i + 1)]);
        ARO_STORE(c, it, &it->data[2 * i + 1], it->data[2 * (i + 1) + 1]);
    }
    h->len--;
    it->data[2 * h->len] = KORB_NIL;                     /* drop tail refs (nil = no WB) */
    it->data[2 * h->len + 1] = KORB_NIL;
    return RESULT_OK(removed);
}

static RESULT korb_m_hash_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#each without a block (Enumerator) is not supported");
    const uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        VALUE k = h->items->data[2 * i];
        VALUE v = h->items->data[2 * i + 1];
        RESULT r;
        if (np >= 2) {                       /* |k, v| — fast path, no pair alloc */
            VALUE argv[2] = { k, v };
            r = korb_block_yield(c, slots, block, def_env, argv, 2, captured_self);
        } else {                             /* |pair| — yield a [k, v] array */
            slots[0] = k; slots[1] = v;                              /* root k,v in scratch */
            VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));      /* slots[0,1] rooted */
            slots[2] = pair;                                         /* root pair */
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            VALUE parg = slots[2];
            r = korb_block_yield(c, slots + 3, block, def_env, &parg, 1, captured_self);
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_hash_merge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(ov));
    uint32_t ln = SELF_HASH->len, rn = VAL2HASH(ov)->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, ln + rn)));  /* slots past dst */
    for (uint32_t i = 0; i < ln; i++) {
        slots[0] = SELF_HASH->items->data[2 * i];                 /* root key in scratch */
        VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
    }
    uint32_t rn2 = VAL2HASH(VALUE_SLICE_GET(a, 0))->len;          /* re-read other (rooted) */
    for (uint32_t i = 0; i < rn2; i++) {
        slots[0] = VAL2HASH(VALUE_SLICE_GET(a, 0))->items->data[2 * i];
        VALUE val = VAL2HASH(VALUE_SLICE_GET(a, 0))->items->data[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_hash_make_pair(CTX *c, VALUE *cursor, VALUE *kslot, VALUE *vslot, VALUE *out);
static RESULT korb_m_hash_key(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(h->items->data[2*i+1], needle)) return RESULT_OK(h->items->data[2*i]);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE needle = VALUE_SLICE_GET(a, 0);
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < h->len; i++) {
        if (korb_value_eq(h->items->data[2*i+1], needle)) {
            slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* true if every pair of `sub` appears in `sup` with an equal value */
static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup) {
    for (uint32_t i = 0; i < sub->len; i++) {
        int32_t idx = korb_hash_find(sup, sub->items->data[2*i]);
        if (idx < 0) return false;
        if (!korb_value_eq(sub->items->data[2*i+1], sup->items->data[2*idx+1])) return false;
    }
    return true;
}
/* op: 0 `<`  1 `<=`  2 `>`  3 `>=` (subset/superset comparison) */
static RESULT korb_hash_cmp_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(ov));
    const KorbHash *me = VAL2HASH(VALUE_REF_GET(self)), *other = VAL2HASH(ov);
    bool res;
    switch (op) {
      case 0: res = me->len <  other->len && korb_hash_is_subset(me, other); break;
      case 1: res = me->len <= other->len && korb_hash_is_subset(me, other); break;
      case 2: res = me->len >  other->len && korb_hash_is_subset(other, me); break;
      default: res = me->len >= other->len && korb_hash_is_subset(other, me); break;
    }
    return RESULT_OK(res ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 0); }
static RESULT korb_m_hash_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 1); }
static RESULT korb_m_hash_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 2); }
static RESULT korb_m_hash_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 3); }
#undef SELF_HASH
#undef REQUIRE_BLOCK

/* ---- Array enumerable / aggregate methods -------------------------------- */

#define SELF_ARY  VAL2ARY(VALUE_REF_GET(self))
#define ARY_REQUIRE_BLOCK(what) \
    do { if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, \
        what " without a block (Enumerator) is not supported"); } while (0)

static RESULT korb_m_ary_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbArray *ary = SELF_ARY;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ary->len; i++)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbArray *ary = SELF_ARY;
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(LONG2FIX(ary->len));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    intptr_t n = 0;
    for (uint32_t i = 0; i < ary->len; i++) if (korb_value_eq(ary->items->data[i], needle)) n++;
    return RESULT_OK(LONG2FIX(n));
}

static RESULT korb_m_ary_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t acc = VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0)) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    const KorbArray *ary = SELF_ARY;
    for (uint32_t i = 0; i < ary->len; i++) {
        VALUE e = ary->items->data[i];
        if (UNLIKELY(!FIXNUM_P(e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(e));
        acc += FIX2LONG(e);
    }
    return RESULT_OK(LONG2FIX(acc));
}

/* min (want=-1) / max (want=1) by <=> */
static RESULT korb_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, int want) {
    const KorbArray *ary = SELF_ARY;
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    VALUE best = ary->items->data[0];
    for (uint32_t i = 1; i < ary->len; i++) {
        VALUE e = ary->items->data[i];
        int cmp = korb_cmp_full(c, e, best);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(e), korb_type_name(best));
        if (cmp == want) best = e;
    }
    return RESULT_OK(best);
}
static RESULT korb_m_ary_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_ary_minmax(c, slots, self, -1); }
static RESULT korb_m_ary_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_ary_minmax(c, slots, self,  1); }

static RESULT korb_m_ary_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_ARY->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    /* in-place insertion sort on the copy — no alloc, so pointers stay put */
    KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
    VALUE *data = d->items->data;
    for (uint32_t i = 1; i < d->len; i++) {
        VALUE key = data[i];
        uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, data[j-1], key);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(data[j-1]), korb_type_name(key));
            if (cmp <= 0) break;
            data[j] = data[j-1]; j--;
        }
        data[j] = key;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* sort_by: build parallel value+key arrays via the block, insertion-sort by key. */
static RESULT korb_m_ary_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#sort_by");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF vals = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF keys = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                              /* root key */
        CHECK(korb_ary_push_val(c, slots + 4, vals, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
    }
    KorbArray *vd = VAL2ARY(VALUE_REF_GET(vals)), *kd = VAL2ARY(VALUE_REF_GET(keys));
    VALUE *vdat = vd->items->data, *kdat = kd->items->data;
    for (uint32_t i = 1; i < vd->len; i++) {             /* lockstep insertion sort, no alloc */
        VALUE vk = vdat[i], kk = kdat[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, kdat[j-1], kk);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(kdat[j-1]), korb_type_name(kk));
            if (cmp <= 0) break;
            vdat[j] = vdat[j-1]; kdat[j] = kdat[j-1]; j--;
        }
        vdat[j] = vk; kdat[j] = kk;
    }
    return RESULT_OK(VALUE_REF_GET(vals));
}
/* sort_by!: sort in place by block key (sort_by then copy back into self). */
static RESULT korb_m_ary_sort_by_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    RESULT sr = korb_m_ary_sort_by(c, slots, self, a, block, def_env, cself);   /* sorted copy at slots[0] */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                  /* root the sorted array */
    VALUE_REF sorted = VALUE_REF_AT(&slots[0]);
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t n = VAL2ARY(VALUE_REF_GET(sorted))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(sorted))->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* min_by(want=-1) / max_by(want=1): element with the extreme block key. */
static RESULT korb_ary_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE cself, int want) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#min_by/max_by without a block is not supported");
    slots[0] = KORB_NIL;   /* best value */
    slots[1] = KORB_NIL;   /* best key */
    bool have = false;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        if (!have) { slots[0] = slots[2]; slots[1] = slots[3]; have = true; continue; }
        int cmp = korb_cmp_full(c, slots[3], slots[1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want < 0 && cmp < 0) || (want > 0 && cmp > 0)) { slots[0] = slots[2]; slots[1] = slots[3]; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_ary_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_ary_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_minmax_by(c, slots, self, block, def_env, cself,  1); }
/* filter_map: collect block results that are truthy. */
static RESULT korb_m_ary_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#filter_map");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* flat_map: map then flatten one level (Array results spliced). */
static RESULT korb_m_ary_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#flat_map");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                              /* root result */
        if (KORB_ARRAY_P(slots[0])) {
            uint32_t m = VAL2ARY(slots[0])->len;
            for (uint32_t j = 0; j < m; j++) {
                VALUE e = VAL2ARY(slots[0])->items->data[j];
                CHECK(korb_ary_push_val(c, slots + 1, dst, e));
            }
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* partition: [truthy_elems, falsy_elems]. */
static RESULT korb_m_ary_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#partition");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF yes = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF no  = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_TRUTHY(r.value) ? yes : no, slots[2]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    VALUE_REF out = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(VALUE_REF_GET(out));
}
/* group_by: Hash{ block_key => [elems...] }. */
static RESULT korb_m_ary_group_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#group_by");
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));       VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = ary->items->data[i];                  /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                              /* key */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[2]);
        if (idx < 0) {                                   /* new bucket array */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
        } else {
            VALUE bucket = VAL2HASH(VALUE_REF_GET(h))->items->data[2 * idx + 1];
            slots[3] = bucket;
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(h));
}

/* grep(pat)(keep=1) / grep_v(pat)(keep=0): select by `pat === elem`, optional block map. */
static RESULT korb_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself, bool keep) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];                  /* root elem across yield */
        if (korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]) == keep) {
            if (block != NULL) {
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
            } else {
                CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
            }
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself)   { return korb_ary_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { return korb_ary_grep(c, slots, self, a, block, def_env, cself, false); }

static RESULT korb_m_ary_sort_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbArray *d = VAL2ARY(VALUE_REF_GET(self));        /* in-place; cmp does not alloc */
    VALUE *data = d->items->data;
    for (uint32_t i = 1; i < d->len; i++) {
        VALUE key = data[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, data[j-1], key);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(data[j-1]), korb_type_name(key));
            if (cmp <= 0) break;
            data[j] = data[j-1]; j--;
        }
        data[j] = key;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));      VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = ary->items->data[i];                 /* elem (root) */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[1]);
        intptr_t cnt = idx < 0 ? 0 : FIX2LONG(VAL2HASH(VALUE_REF_GET(h))->items->data[2*idx+1]);
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(cnt + 1)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}

static RESULT korb_m_ary_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* sep at slots scratch so it survives the per-element to_s allocs */
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    }
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbArray *ary = SELF_ARY;
    const KorbString *sep = (VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0))) ? VAL2STR(VALUE_SLICE_GET(a, 0)) : NULL;
    for (uint32_t i = 0; i < ary->len; i++) {
        if (i && sep) fwrite(sep->buf->data, 1, sep->len, ms);
        korb_fprint_to_s(c, ms, ary->items->data[i]);   /* no GC inside fprint */
    }
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}

static RESULT korb_m_ary_compact(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_ARY->items->data[i];
        if (e != KORB_NIL) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_compact_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (it->data[r] == KORB_NIL) { changed = true; continue; }
        if (w != r) ARO_STORE(c, it, &it->data[w], it->data[r]);
        w++;
    }
    for (uint32_t r = w; r < ary->len; r++) it->data[r] = KORB_NIL;
    ary->len = w;
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_ary_each_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#each_index");
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_ARY->items->data[i];
        const KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        bool seen = false;
        for (uint32_t j = 0; j < d->len; j++) if (korb_value_eq(d->items->data[j], e)) { seen = true; break; }
        if (!seen) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* recursive flatten helper: append all leaves of `src` into dst */
static RESULT korb_ary_flatten_into(CTX *c, VALUE *slots, VALUE_REF dst, VALUE_REF src) {
    uint32_t n = VAL2ARY(VALUE_REF_GET(src))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(src))->items->data[i];
        if (KORB_ARRAY_P(e)) {
            slots[0] = e;                                  /* root nested array */
            CHECK(korb_ary_flatten_into(c, slots + 1, dst, VALUE_REF_AT(&slots[0])));
        } else {
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_flatten(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    CHECK(korb_ary_flatten_into(c, slots + 1, dst, self));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_concat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    uint32_t n = VAL2ARY(ov)->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_SLICE_GET(a, 0))->items->data[i];   /* re-read other (rooted) */
        CHECK(korb_ary_push_val(c, slots, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* select (keep==true) / reject (keep==false) */
static RESULT korb_ary_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#select/reject without a block (Enumerator) is not supported");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE e = ary->items->data[i];
        slots[0] = e;                                       /* root e across the yield */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, false); }

static RESULT korb_m_ary_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#find");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#find_index");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

static bool korb_ary_has(const KorbArray *ar, VALUE v);
static RESULT korb_m_ary_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#take_while");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#drop_while");
    uint32_t start = 0; bool dropping = true;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) { start = i + 1; continue; }
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    (void)start;
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < ary->len; i++) ary->items->data[i] = KORB_NIL;
    ary->len = 0;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_intersect_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    const KorbArray *me = VAL2ARY(VALUE_REF_GET(self)), *other = VAL2ARY(ov);
    for (uint32_t i = 0; i < me->len; i++)
        if (korb_ary_has(other, me->items->data[i])) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}
/* bsearch: find-minimum (boolean block) or find-any (Integer block). Returns the
 * matching element, or nil. Array must be sorted for meaningful results. */
static RESULT korb_m_ary_bsearch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#bsearch");
    uint32_t lo = 0, hi = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE found = KORB_NIL; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        VALUE rv = r.value;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {   /* find-minimum */
            if (KORB_TRUTHY(rv)) { found = slots[0]; have = true; hi = mid; }
            else lo = mid + 1;
        } else if (FIXNUM_P(rv)) {                                     /* find-any */
            intptr_t cmp = FIX2LONG(rv);
            if (cmp == 0) return RESULT_OK(slots[0]);
            else if (cmp < 0) hi = mid;
            else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? found : KORB_NIL);
}

static RESULT korb_m_ary_bsearch_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#bsearch_index");
    uint32_t lo = 0, hi = VAL2ARY(VALUE_REF_GET(self))->len;
    uint32_t found = 0; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        VALUE rv = r.value;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {
            if (KORB_TRUTHY(rv)) { found = mid; have = true; hi = mid; } else lo = mid + 1;
        } else if (FIXNUM_P(rv)) {
            intptr_t cmp = FIX2LONG(rv);
            if (cmp == 0) return RESULT_OK(LONG2FIX(mid));
            else if (cmp < 0) hi = mid; else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? LONG2FIX(found) : KORB_NIL);
}
static RESULT korb_m_ary_map_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#map!");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
        ARO_STORE(c, a2->items, &a2->items->data[i], r.value);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Integer#step / Float#step (generic over numeric self) */
static RESULT korb_m_num_step(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Numeric#step without a block is not supported");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE selfv = VALUE_REF_GET(self);
    VALUE limv = VALUE_SLICE_GET(a, 0);
    VALUE stepv = VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : LONG2FIX(1);
    bool use_float = KORB_FLOAT_P(selfv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    if (use_float) {
        double s, lim, st;
        if (!korb_num_to_d(selfv, &s) || !korb_num_to_d(limv, &lim) || !korb_num_to_d(stepv, &st))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        if (st == 0.0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        for (long i = 0; ; i++) {
            double d = s + (double)i * st;
            if (st > 0 ? d > lim : d < lim) break;
            slots[0] = UNWRAP(korb_float_new(c, slots, d));
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    } else {
        intptr_t s = FIX2LONG(selfv), lim = FIX2LONG(limv), st = FIX2LONG(stepv);
        if (st == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        for (intptr_t i = s; st > 0 ? i <= lim : i >= lim; i += st) {
            slots[0] = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* any? (mode 0) / all? (1) / none? (2), with a block */
static RESULT korb_ary_quant(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, int mode) {
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        bool t;
        if (block != NULL) {                                 /* truthiness of block result */
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(slots[0]);                        /* no block → element truthiness */
        }
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);     /* any? */
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);   /* all? */
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);    /* none? */
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_ary_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_ary_quant(c, slots, self, block, def_env, captured_self, 0); }
static RESULT korb_m_ary_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_ary_quant(c, slots, self, block, def_env, captured_self, 1); }
static RESULT korb_m_ary_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_ary_quant(c, slots, self, block, def_env, captured_self, 2); }

static RESULT korb_m_ary_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#reduce without a block (symbol form) is not supported");
    uint32_t i = 0;
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);                  /* acc = initial */
    } else {
        const KorbArray *ary = SELF_ARY;
        if (ary->len == 0) return RESULT_OK(KORB_NIL);
        slots[0] = ary->items->data[0];
        i = 1;
    }
    for (; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { slots[0], ary->items->data[i] };  /* acc, elem */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                                /* root new acc */
    }
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_each_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    ARY_REQUIRE_BLOCK("Array#each_with_object");
    slots[0] = VALUE_SLICE_GET(a, 0);                      /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { ary->items->data[i], slots[0] };  /* elem, memo */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
#undef ARY_REQUIRE_BLOCK
#undef SELF_ARY

/* ---- Range methods ------------------------------------------------------- */

#define SELF_RANGE  VAL2RANGE(VALUE_REF_GET(self))

/* integer iteration bounds [lo, hi) ; false if endpoints aren't both Integer */
static bool korb_range_int_bounds(const KorbRange *r, intptr_t *lo, intptr_t *hi) {
    if (!FIXNUM_P(r->rbegin) || !FIXNUM_P(r->rend)) return false;
    intptr_t e = FIX2LONG(r->rend);
    *lo = FIX2LONG(r->rbegin);
    *hi = r->exclude_end ? e : e + 1;
    return true;
}

static RESULT korb_m_range_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rbegin); }
static RESULT korb_m_range_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rend); }
static RESULT korb_m_range_exclude(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a){ (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->exclude_end ? KORB_TRUE : KORB_FALSE); }

static RESULT korb_m_range_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
    return RESULT_OK(LONG2FIX(hi > lo ? hi - lo : 0));
}

static RESULT korb_m_range_cover(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbRange *r = SELF_RANGE;
    VALUE x = VALUE_SLICE_GET(a, 0);
    int lc = korb_cmp_values(r->rbegin, x);     /* begin <=> x */
    int uc = korb_cmp_values(x, r->rend);       /* x <=> end */
    if (lc == 2 || uc == 2) return RESULT_OK(KORB_FALSE);
    bool lower = (lc <= 0);
    bool upper = r->exclude_end ? (uc < 0) : (uc <= 0);
    return RESULT_OK((lower && upper) ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_range_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbRange *r = SELF_RANGE;
    intptr_t lo, hi;
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(lo) : KORB_NIL);
    return RESULT_OK(r->rbegin);
}
static RESULT korb_m_range_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbRange *r = SELF_RANGE;
    intptr_t lo, hi;
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(hi - 1) : KORB_NIL);
    if (r->exclude_end) return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot exclude non Integer end value");
    return RESULT_OK(r->rend);
}
static RESULT korb_m_range_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rbegin); }
static RESULT korb_m_range_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rend); }

static RESULT korb_m_range_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t init = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    intptr_t acc = init;
    for (intptr_t i = lo; i < hi; i++) acc += i;   /* small ranges; Bignum unneeded here */
    return RESULT_OK(LONG2FIX(acc));
}

static RESULT korb_m_range_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#each without a block (Enumerator) is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
    for (intptr_t i = lo; i < hi; i++) {           /* bounds are plain ints — GC-safe */
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (intptr_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_range_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#map without a block (Enumerator) is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_range_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo + n; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#drop_while without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool dropping = true;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) continue;
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#take_while without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#filter_map without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#each_with_index without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = lo; i < hi; i++) {
        VALUE argv[2] = { LONG2FIX(i), LONG2FIX(i - lo) };
        RESULT r = korb_block_yield(c, slots, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_range_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (intptr_t i = lo; i < hi; i++) {
        uint32_t idx = (uint32_t)(i - lo);
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, k + 1));
        VALUE_REF row = VALUE_REF_AT(&slots[0]);
        CHECK(korb_ary_push_val(c, slots + 1, row, LONG2FIX(i)));
        for (uint32_t j = 0; j < k; j++) {
            VALUE ov = VALUE_SLICE_GET(a, j);
            VALUE e = (KORB_ARRAY_P(ov) && idx < VAL2ARY(ov)->len) ? VAL2ARY(ov)->items->data[idx] : KORB_NIL;
            CHECK(korb_ary_push_val(c, slots + 1, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    uint32_t cnt = 0;
    for (intptr_t i = lo; i < hi; i++) {
        bool t;
        if (block != NULL) {
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else t = true;                              /* every Integer is truthy */
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_range_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    bool has_arg = VALUE_SLICE_LEN(a) >= 1;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool hit;
        if (has_arg) hit = korb_value_eq(iv, VALUE_SLICE_GET(a, 0));
        else {
            if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#find_index without arg or block is not supported");
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            hit = KORB_TRUTHY(r.value);
        }
        if (hit) return RESULT_OK(LONG2FIX(i - lo));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_range_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#reduce without a block (symbol form) is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    intptr_t i = lo;
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);
    } else {
        if (lo >= hi) return RESULT_OK(KORB_NIL);
        slots[0] = LONG2FIX(lo); i = lo + 1;
    }
    for (; i < hi; i++) {
        VALUE argv[2] = { slots[0], LONG2FIX(i) };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}

/* select (keep==true) / reject (keep==false) over an integer range */
static RESULT korb_range_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#select/reject without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_range_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, false); }

static RESULT korb_m_range_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#find without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

/* any? (0) / all? (1) / none? (2) over an integer range, with block */
static RESULT korb_range_quant(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, int mode) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#any?/all?/none? without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        bool t = KORB_TRUTHY(r.value);
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_range_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_range_quant(c, slots, self, block, def_env, captured_self, 0); }
static RESULT korb_m_range_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_range_quant(c, slots, self, block, def_env, captured_self, 1); }
static RESULT korb_m_range_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_range_quant(c, slots, self, block, def_env, captured_self, 2); }

static RESULT korb_m_range_step(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#step without a block (Enumerator) is not supported");
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(sv));
    intptr_t st = FIX2LONG(sv);
    if (UNLIKELY(st <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0 or negative");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = lo; i < hi; i += st) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
#undef SELF_RANGE

/* ---- more Array (query/mutate) + Integer (bit) methods ------------------- */

static RESULT korb_m_ary_unshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    if (k == 0) return RESULT_OK(VALUE_REF_GET(self));
    CHECK(korb_ary_ensure(c, slots, self, k));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)        /* shift right by k */
        ARO_STORE(c, it, &it->data[i + k], it->data[i]);
    for (uint32_t i = 0; i < k; i++)
        ARO_STORE(c, it, &it->data[i], VALUE_SLICE_GET(a, i));
    ary->len += k;
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_shift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE first = it->data[0];
    for (uint32_t i = 1; i < ary->len; i++) ARO_STORE(c, it, &it->data[i - 1], it->data[i]);
    ary->len--;
    it->data[ary->len] = KORB_NIL;
    return RESULT_OK(first);
}

/* assoc (idx 0) / rassoc (idx 1): find the sub-array whose [idx] == key */
static RESULT korb_ary_assoc(CTX *c, VALUE_REF self, VALUE key, uint32_t idx) {
    (void)c;
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < ary->len; i++) {
        VALUE e = ary->items->data[i];
        if (KORB_ARRAY_P(e) && VAL2ARY(e)->len > idx && korb_value_eq(VAL2ARY(e)->items->data[idx], key))
            return RESULT_OK(e);
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)slots; return korb_ary_assoc(c, self, VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_ary_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)slots; return korb_ary_assoc(c, self, VALUE_SLICE_GET(a, 0), 1); }

static RESULT korb_m_ary_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (UNLIKELY(!korb_to_index(iv, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    intptr_t orig = i;
    if (i < 0) i += ary->len;
    if (i >= 0 && (uint32_t)i < ary->len) return RESULT_OK(ary->items->data[i]);
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld outside of array bounds: -%u...%u",
                      (long)orig, ary->len, ary->len);
}

/* dig: recursive index into nested Array/Hash */
static RESULT korb_m_ary_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE cur = VALUE_REF_GET(self);
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE key = VALUE_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        if (KORB_ARRAY_P(cur)) {
            intptr_t i;
            if (UNLIKELY(!korb_to_index(key, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(key));
            KorbArray *ar = VAL2ARY(cur);
            if (i < 0) i += ar->len;
            cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : ar->items->data[i];
        } else if (KORB_HASH_P(cur)) {
            int32_t idx = korb_hash_find(VAL2HASH(cur), key);
            cur = idx < 0 ? KORB_NIL : VAL2HASH(cur)->items->data[2 * idx + 1];
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(cur));
        }
    }
    return RESULT_OK(cur);
}

static RESULT korb_m_int_lshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    intptr_t n = FIX2LONG(VALUE_REF_GET(self)), sh = FIX2LONG(o);
    intptr_t r = sh >= 0 ? (sh < 62 ? (n << sh) : 0) : (n >> (-sh < 63 ? -sh : 62));
    if (sh >= 0 && (sh >= 62 || (r >> sh) != n || !FIXABLE(r)))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_rshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    intptr_t n = FIX2LONG(VALUE_REF_GET(self)), sh = FIX2LONG(o);
    intptr_t r = sh >= 0 ? (sh < 63 ? (n >> sh) : (n < 0 ? -1 : 0)) : (sh > -62 ? (n << -sh) : 0);
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_bitref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!FIXNUM_P(o)) return RESULT_OK(LONG2FIX(0));
    intptr_t n = FIX2LONG(VALUE_REF_GET(self)), i = FIX2LONG(o);
    if (i < 0 || i >= 63) return RESULT_OK(LONG2FIX(n < 0 && i >= 63 ? 1 : 0));
    return RESULT_OK(LONG2FIX((n >> i) & 1));
}
/* bitwise & | ^ (kind 0/1/2) */
static RESULT korb_int_bitop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int kind) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(o));
    intptr_t x = FIX2LONG(VALUE_REF_GET(self)), y = FIX2LONG(o);
    return RESULT_OK(LONG2FIX(kind == 0 ? (x & y) : kind == 1 ? (x | y) : (x ^ y)));
}
static RESULT korb_m_int_and(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 0); }
static RESULT korb_m_int_or (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 1); }
static RESULT korb_m_int_xor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 2); }
static RESULT korb_m_int_inv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(~FIX2LONG(VALUE_REF_GET(self)))); }
static RESULT korb_m_int_remainder(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(o));
    intptr_t b = FIX2LONG(o);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(FIX2LONG(VALUE_REF_GET(self)) % b));   /* truncated (sign of dividend) */
}
static RESULT korb_m_true_lit2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_flt_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; double d = VAL2FLT(VALUE_REF_GET(self))->val; return korb_float_new(c, slots, d * d); }

static RESULT korb_m_ary_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        VALUE iv = VALUE_SLICE_GET(a, j);
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        VALUE e = KORB_NIL;
        intptr_t i; if (korb_to_index(iv, &i)) { if (i < 0) i += ary->len; if (i >= 0 && (uint32_t)i < ary->len) e = ary->items->data[i]; }
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_fill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    intptr_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    uint32_t base = block ? 0 : 1;                        /* position args start here */
    if (UNLIKELY(!block && VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    VALUE v = block ? KORB_NIL : VALUE_SLICE_GET(a, 0);
    /* compute fill region [beg, beg+len) following MRI rb_ary_fill order */
    intptr_t beg = 0, len = n; bool have_len = false;
    VALUE pos0 = VALUE_SLICE_LEN(a) > base ? VALUE_SLICE_GET(a, base) : KORB_NIL;
    if (KORB_RANGE_P(pos0)) {
        const KorbRange *r = VAL2RANGE(pos0);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += n;
        if (e < 0) e += n;
        if (b < 0) b = 0;
        beg = b; len = (r->exclude_end ? e - 1 : e) - b + 1; have_len = true;
    } else {
        if (pos0 != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(pos0, &beg))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(pos0));
        }
        VALUE pos1 = VALUE_SLICE_LEN(a) > base + 1 ? VALUE_SLICE_GET(a, base + 1) : KORB_NIL;
        if (pos1 != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(pos1, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(pos1));
            have_len = true;
        }
        if (beg < 0) { beg += n; if (beg < 0) beg = 0; }
        if (!have_len) len = n - beg;                    /* default: to end of array */
    }
    if (len < 0) len = 0;
    slots[0] = v;                                        /* root value across any grow */
    for (intptr_t i = beg; i < beg + len; i++) {
        if (i < 0) continue;
        if (block != NULL) {
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;                          /* root before grow GC */
        }
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if ((uint32_t)i >= ary->len) {
            CHECK(korb_ary_ensure(c, slots + 1, self, (uint32_t)i + 1 - ary->len));
            ary = VAL2ARY(VALUE_REF_GET(self));
            for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ary->items->data[k] = KORB_NIL;
            ary->len = (uint32_t)i + 1;
        }
        ary = VAL2ARY(VALUE_REF_GET(self));
        ARO_STORE(c, ary->items, &ary->items->data[i], slots[0]);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash dig / values_at / slice / except */
static RESULT korb_m_hash_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE cur = VALUE_REF_GET(self);
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE key = VALUE_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        if (KORB_HASH_P(cur)) { int32_t idx = korb_hash_find(VAL2HASH(cur), key); cur = idx < 0 ? KORB_NIL : VAL2HASH(cur)->items->data[2 * idx + 1]; }
        else if (KORB_ARRAY_P(cur)) { if (!FIXNUM_P(key)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); KorbArray *ar = VAL2ARY(cur); intptr_t i = FIX2LONG(key); if (i < 0) i += ar->len; cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : ar->items->data[i]; }
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(cur));
    }
    return RESULT_OK(cur);
}
static RESULT korb_m_hash_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, j));
        CHECK(korb_ary_push_val(c, slots + 1, dst, idx < 0 ? h->default_val : h->items->data[2 * idx + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* slice (keep==true) / except (keep==false) the listed keys */
static RESULT korb_hash_pick(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool keep) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        VALUE key = h->items->data[2 * i];
        bool listed = false;
        for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) if (korb_value_eq(VALUE_SLICE_GET(a, j), key)) { listed = true; break; }
        if (listed == keep) {
            slots[0] = key;                               /* root key (scratch above dst) */
            VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[0] = h->items->data[2 * i];      /* k */
        slots[1] = h->items->data[2 * i + 1];  /* v */
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_pick(c, slots, self, a, true); }
static RESULT korb_m_hash_except(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_hash_pick(c, slots, self, a, false); }

/* yield (k, v) to a Hash block: np>=2 → two args; else a single [k, v] pair. */
static RESULT korb_hash_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE cself, uint32_t np, VALUE k, VALUE v) {
    if (np >= 2) { VALUE argv[2] = { k, v }; return korb_block_yield(c, slots, block, def_env, argv, 2, cself); }
    slots[0] = k; slots[1] = v;
    VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
    slots[2] = pair;
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    VALUE parg = slots[2];
    return korb_block_yield(c, slots + 3, block, def_env, &parg, 1, cself);
}
#define HASH_REQ_BLOCK(what) do { if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, what " without a block is not supported"); } while (0)

static RESULT korb_m_hash_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a; HASH_REQ_BLOCK("Hash#map");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* select(keep=1)/reject(keep=0) → new Hash */
static RESULT korb_hash_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#select/reject without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        VALUE k = h->items->data[2*i], v = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, k, v);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) {
            slots[0] = k;
            VALUE vv = VAL2HASH(VALUE_REF_GET(self))->items->data[2*i+1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), vv));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_hash_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, false); }
/* any?(0)/all?(1)/none?(2) */
static RESULT korb_hash_quant(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE captured_self, int mode) {
    if (block == NULL) {                                  /* pairs are always truthy */
        uint32_t len = VAL2HASH(VALUE_REF_GET(self))->len;
        if (mode == 0) return RESULT_OK(len > 0 ? KORB_TRUE : KORB_FALSE);   /* any? */
        if (mode == 2) return RESULT_OK(len == 0 ? KORB_TRUE : KORB_FALSE);  /* none? */
        return RESULT_OK(KORB_TRUE);                                          /* all? */
    }
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, captured_self, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        bool t = KORB_TRUTHY(r.value);
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_hash_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_hash_quant(c, slots, self, block, def_env, captured_self, 0); }
static RESULT korb_m_hash_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self)  { (void)a; return korb_hash_quant(c, slots, self, block, def_env, captured_self, 1); }
static RESULT korb_m_hash_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) { (void)a; return korb_hash_quant(c, slots, self, block, def_env, captured_self, 2); }
/* in-place select!/filter!(keep_truthy=1) and reject!/delete_if(keep_truthy=0) */
static RESULT korb_hash_filter_bang(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE cself, bool keep_truthy, bool ret_nil_if_unchanged) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "in-place Hash filter without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; ; r++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (r >= h->len) break;
        slots[0] = h->items->data[2*r];                    /* root k,v across the yield */
        slots[1] = h->items->data[2*r+1];
        RESULT res = korb_hash_yield(c, slots + 2, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        if (KORB_TRUTHY(res.value) == keep_truthy) {
            KorbHash *h2 = VAL2HASH(VALUE_REF_GET(self));
            if (w != r) {
                ARO_STORE(c, h2->items, &h2->items->data[2*w],   slots[0]);
                ARO_STORE(c, h2->items, &h2->items->data[2*w+1], slots[1]);
            }
            w++;
        } else changed = true;
    }
    KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    for (uint32_t r = 2*w; r < 2*h->len; r++) h->items->data[r] = KORB_NIL;
    h->len = w;
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, true); }
static RESULT korb_m_hash_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, false); }
static RESULT korb_m_hash_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, true); }
static RESULT korb_m_hash_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, false); }
static RESULT korb_m_hash_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#one? without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
/* Build a fresh [k,v] pair array at *out (a rooted slot). cursor = scratch above it. */
static RESULT korb_hash_make_pair(CTX *c, VALUE *cursor, VALUE *kslot, VALUE *vslot, VALUE *out) {
    *out = UNWRAP(korb_ary_new(c, cursor, 2));
    CHECK(korb_ary_push_val(c, cursor + 1, VALUE_REF_AT(out), *kslot));
    CHECK(korb_ary_push_val(c, cursor + 1, VALUE_REF_AT(out), *vslot));
    return RESULT_OK(*out);
}
/* sort_by → array of [k,v] pairs sorted by block key */
static RESULT korb_m_hash_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#sort_by without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF vals = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF keys = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));  /* pair at slots[4] */
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[5] = r.value;
        CHECK(korb_ary_push_val(c, slots + 6, vals, slots[4]));
        CHECK(korb_ary_push_val(c, slots + 6, keys, slots[5]));
    }
    KorbArray *vd = VAL2ARY(VALUE_REF_GET(vals)), *kd = VAL2ARY(VALUE_REF_GET(keys));
    VALUE *vdat = vd->items->data, *kdat = kd->items->data;
    for (uint32_t i = 1; i < vd->len; i++) {
        VALUE vk = vdat[i], kk = kdat[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, kdat[j-1], kk);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
            if (cmp <= 0) break;
            vdat[j] = vdat[j-1]; kdat[j] = kdat[j-1]; j--;
        }
        vdat[j] = vk; kdat[j] = kk;
    }
    return RESULT_OK(VALUE_REF_GET(vals));
}
/* min_by/max_by → the [k,v] pair with the extreme block key */
static RESULT korb_hash_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE cself, int want) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#min_by/max_by without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = KORB_NIL; slots[1] = KORB_NIL; bool have = false;   /* best pair / best key */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[5] = r.value;
        if (!have) { slots[0] = slots[4]; slots[1] = slots[5]; have = true; continue; }
        int cmp = korb_cmp_full(c, slots[5], slots[1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want < 0 && cmp < 0) || (want > 0 && cmp > 0)) { slots[0] = slots[4]; slots[1] = slots[5]; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_hash_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_hash_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself,  1); }
/* filter_map → collect truthy block results */
static RESULT korb_m_hash_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#filter_map without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* partition → [yes_pairs, no_pairs] */
static RESULT korb_m_hash_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#partition without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF yes = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF no  = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 5, KORB_TRUTHY(r.value) ? yes : no, slots[4]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    VALUE_REF out = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(VALUE_REF_GET(out));
}
/* find/detect → the first [k,v] pair whose block is truthy, else nil */
static RESULT korb_m_hash_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* find_all/select(Enumerable) → array of [k,v] pairs where block truthy */
static RESULT korb_m_hash_find_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find_all without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* zip → array of rows: [ [k,v], other0[i], other1[i], ... ] per pair i */
static RESULT korb_m_hash_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));   /* dst below cursor */
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 4, &slots[0], &slots[1], &slots[2]));  /* pair at slots[2] */
        slots[3] = UNWRAP(korb_ary_new(c, slots + 4, k + 1));                /* row at slots[3] */
        VALUE_REF row = VALUE_REF_AT(&slots[3]);
        CHECK(korb_ary_push_val(c, slots + 4, row, slots[2]));
        for (uint32_t j = 0; j < k; j++) {
            VALUE ov = VALUE_SLICE_GET(a, j);
            VALUE e = (KORB_ARRAY_P(ov) && i < VAL2ARY(ov)->len) ? VAL2ARY(ov)->items->data[i] : KORB_NIL;
            CHECK(korb_ary_push_val(c, slots + 4, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* find_index → integer index of first pair where block truthy, else nil */
static RESULT korb_m_hash_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find_index without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    HASH_REQ_BLOCK("Hash#reduce");
    uint32_t np = korb_entry_params_cnt(block);   /* acc + pair: block takes |acc, (k,v)| */
    if (VALUE_SLICE_LEN(a) < 1) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#reduce needs an initial value");
    slots[0] = VALUE_SLICE_GET(a, 0);             /* acc */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        /* build [k,v] pair and yield (acc, pair) */
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
        VALUE argv[2] = { slots[0], slots[3] };
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, np >= 2 ? 2 : 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_hash_each_wo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    HASH_REQ_BLOCK("Hash#each_with_object");
    if (VALUE_SLICE_LEN(a) < 1) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    slots[0] = VALUE_SLICE_GET(a, 0);             /* memo */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
        VALUE argv[2] = { slots[3], slots[0] };   /* (pair, memo) */
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
#undef HASH_REQ_BLOCK

/* ---- more Array methods -------------------------------------------------- */

static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, 0, (uint32_t)n);
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, (uint32_t)n, len - (uint32_t)n);
}
static RESULT korb_m_ary_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE v = VALUE_SLICE_GET(a, 0);
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; bool found = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (korb_value_eq(it->data[r], v)) found = true;
        else { if (w != r) ARO_STORE(c, it, &it->data[w], it->data[r]); w++; }
    }
    for (uint32_t r = w; r < ary->len; r++) it->data[r] = KORB_NIL;
    ary->len = w;
    return RESULT_OK(found ? v : KORB_NIL);
}
static RESULT korb_m_ary_delete_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (!korb_to_index(iv, &i)) return RESULT_OK(KORB_NIL);
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    if (i < 0) i += ary->len;
    if (i < 0 || (uint32_t)i >= ary->len) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE removed = it->data[i];
    for (uint32_t r = (uint32_t)i; r + 1 < ary->len; r++) ARO_STORE(c, it, &it->data[r], it->data[r + 1]);
    ary->len--; it->data[ary->len] = KORB_NIL;
    return RESULT_OK(removed);
}
static RESULT korb_m_ary_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_rotate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    intptr_t sh = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 1;
    if (len == 0) return korb_ary_subseq(c, slots, self, 0, 0);
    intptr_t s = ((sh % (intptr_t)len) + (intptr_t)len) % (intptr_t)len;   /* normalized left rotation */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[(s + i) % len];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));   /* dst at slots[-1] */
    for (uint32_t i = 0; i < n; i++) {
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, k + 1));               /* row, rooted at slots[0] */
        VALUE_REF row = VALUE_REF_AT(&slots[0]);
        CHECK(korb_ary_push_val(c, slots + 1, row, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        for (uint32_t j = 0; j < k; j++) {
            VALUE ov = VALUE_SLICE_GET(a, j);
            VALUE e = (KORB_ARRAY_P(ov) && i < VAL2ARY(ov)->len) ? VAL2ARY(ov)->items->data[i] : KORB_NIL;
            CHECK(korb_ary_push_val(c, slots + 1, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static bool korb_ary_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eq(ar->items->data[i], v)) return true;
    return false;
}
/* `|` union (in self then other, deduped) / `&` intersection (in both, self order, deduped) */
static RESULT korb_m_ary_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) { VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i]; if (!korb_ary_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    uint32_t on = VAL2ARY(VALUE_SLICE_GET(a, 0))->len;
    for (uint32_t i = 0; i < on; i++) { VALUE e = VAL2ARY(VALUE_SLICE_GET(a, 0))->items->data[i]; if (!korb_ary_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        if (korb_ary_has(VAL2ARY(VALUE_SLICE_GET(a, 0)), e) && !korb_ary_has(VAL2ARY(VALUE_REF_GET(dst)), e))
            CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* ---- more Integer / Float methods ---------------------------------------- */
static RESULT korb_m_int_self2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_int_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = FIX2LONG(VALUE_REF_GET(self)); intptr_t r;
    if (__builtin_mul_overflow(n, n, &r) || !FIXABLE(r)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_bits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    intptr_t n = FIX2LONG(VALUE_REF_GET(self)) & FIX2LONG(o), m = FIX2LONG(o);
    bool r = mode == 0 ? (n == 0) : mode == 1 ? (n != 0) : (n == m);   /* nobits/anybits/allbits */
    return RESULT_OK(r ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_nobits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_m_int_bits(c, slots, self, a, 0); }
static RESULT korb_m_int_anybits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 1); }
static RESULT korb_m_int_allbits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 2); }
static RESULT korb_m_int_gcdlcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    intptr_t x = FIX2LONG(VALUE_REF_GET(self)), y = FIX2LONG(o);
    intptr_t g = korb_int_gcd(x, y), l = (x == 0 || y == 0) ? 0 : (x / g) * y;
    if (l < 0) l = -l;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 2)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(g)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(l)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_flt_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double e; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, pow(VAL2FLT(VALUE_REF_GET(self))->val, e));
}
static RESULT korb_m_flt_angle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d = VAL2FLT(VALUE_REF_GET(self))->val;
    return d < 0 ? korb_float_new(c, slots, M_PI) : RESULT_OK(LONG2FIX(0));   /* arg: 0 (Integer) or PI */
}
static RESULT korb_m_flt_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double lo, hi, s = VAL2FLT(VALUE_REF_GET(self))->val;
    if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &lo) || !korb_num_to_d(VALUE_SLICE_GET(a, 1), &hi)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
    return RESULT_OK((s >= lo && s <= hi) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_ary_insert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t idx;
    if (UNLIKELY(!korb_to_index(iv, &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    uint32_t k = VALUE_SLICE_LEN(a) - 1;
    if (k == 0) return RESULT_OK(VALUE_REF_GET(self));
    intptr_t orig = idx;
    uint32_t oldlen = VAL2ARY(VALUE_REF_GET(self))->len;
    if (idx < 0) idx += (intptr_t)oldlen + 1;
    if (UNLIKELY(idx < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array", (long)orig);
    uint32_t at = (uint32_t)idx;
    uint32_t pad = at > oldlen ? at - oldlen : 0;
    CHECK(korb_ary_ensure(c, slots, self, pad + k));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    if (at <= oldlen) {
        for (int32_t r = (int32_t)oldlen - 1; r >= (int32_t)at; r--) ARO_STORE(c, it, &it->data[r + k], it->data[r]);
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &it->data[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = oldlen + k;
    } else {
        for (uint32_t r = oldlen; r < at; r++) it->data[r] = KORB_NIL;
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &it->data[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = at + k;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#sort → array of [k,v] pairs sorted by key; fetch_values(*keys) */
static RESULT korb_m_hash_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    RESULT ar = korb_m_hash_to_a(c, slots, self, a);     /* pairs array (rooted via return) */
    if (ar.state != KORB_NORMAL) return ar;
    VALUE_REF dst = SLOTS_PUSH(slots, ar.value);
    KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));           /* in-place insertion sort by pair[0] */
    VALUE *data = d->items->data;
    for (uint32_t i = 1; i < d->len; i++) {
        VALUE key = data[i]; uint32_t j = i;
        while (j > 0) {
            VALUE pa = VAL2ARY(data[j-1])->items->data[0], pb = VAL2ARY(key)->items->data[0];
            int cmp = korb_cmp_full(c, pa, pb);
            if (cmp != 1) break;
            data[j] = data[j-1]; j--;
        }
        data[j] = key;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, j));
        if (idx < 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "key not found");
        CHECK(korb_ary_push_val(c, slots + 1, dst, h->items->data[2 * idx + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* delete_if/reject!(keep_when_false) and keep_if/select!(keep_when_true), in-place */
static RESULT korb_ary_filter_bang(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE cself, bool keep_truthy, bool ret_nil_if_unchanged) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "in-place filter without a block is not supported");
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; ; r++) {
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (r >= ary->len) break;
        slots[0] = ary->items->data[r];                    /* root elem across the yield */
        RESULT res = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        bool kept = (KORB_TRUTHY(res.value) == keep_truthy);
        if (kept) {
            KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
            if (w != r) ARO_STORE(c, a2->items, &a2->items->data[w], slots[0]);
            w++;
        } else changed = true;
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t r = w; r < ary->len; r++) ary->items->data[r] = KORB_NIL;
    ary->len = w;
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, false); }
static RESULT korb_m_ary_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, true); }
static RESULT korb_m_ary_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, false); }
static RESULT korb_m_ary_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, true); }

static RESULT korb_m_hash_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#sum without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    intptr_t acc = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (UNLIKELY(!FIXNUM_P(r.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "Hash#sum block must return Integer here");
        acc += FIX2LONG(r.value);
    }
    return RESULT_OK(LONG2FIX(acc));
}

static RESULT korb_m_obj_false(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }

static RESULT korb_m_ary_difference(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        if (!korb_ary_has(VAL2ARY(VALUE_SLICE_GET(a, 0)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
RESULT korb_sub_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line) {
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_ARRAY_P(l)) {                            /* Array - Array → set difference */
        slots[0] = rhs;
        return korb_m_ary_difference(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
    }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '-' for %s", korb_a_type_name(l));
}
static RESULT korb_m_ary_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    VAL2ARY(VALUE_REF_GET(self))->len = 0;               /* clear, then copy other */
    uint32_t on = VAL2ARY(VALUE_SLICE_GET(a, 0))->len;
    for (uint32_t i = 0; i < on; i++)
        CHECK(korb_ary_push_val(c, slots, self, VAL2ARY(VALUE_SLICE_GET(a, 0))->items->data[i]));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(nv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    intptr_t n = FIX2LONG(nv); if (n < 0) n = 0;
    uint32_t len = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = (uint32_t)n; i < len; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* dup: shallow copy. Immutables (fixnum/symbol/nil/true/false/float) return self;
 * String/Array/Hash get a fresh shallow copy. */
static RESULT korb_m_obj_dup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE v = VALUE_REF_GET(self);
    if (KORB_STRING_P(v)) {
        uint32_t len = VAL2STR(v)->len;
        KorbString *r = korb_str_alloc(c, slots, len);
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read after alloc */
        memcpy(r->buf->data, s->buf->data, len);
        return RESULT_OK((VALUE)r);
    }
    if (KORB_ARRAY_P(v)) {
        uint32_t n = VAL2ARY(v)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
        for (uint32_t i = 0; i < n; i++) {
            VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_HASH_P(v)) {
        uint32_t n = VAL2HASH(v)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, n)));
        for (uint32_t i = 0; i < n; i++) {
            slots[0] = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i];
            VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
        }
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    return RESULT_OK(v);   /* immutable / no special copy */
}

/* in-place reverse of items[lo, hi) — no alloc, so pointers are stable. */
static void korb_ary_rev_range(CTX *c, KorbArrayItems *it, uint32_t lo, uint32_t hi) {
    while (lo + 1 < hi + 1 && lo < hi) {       /* lo < hi (guard wrap) */
        hi--;
        VALUE t = it->data[lo];
        ARO_STORE(c, it, &it->data[lo], it->data[hi]);
        ARO_STORE(c, it, &it->data[hi], t);
        lo++;
    }
}
static RESULT korb_m_ary_reverse_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    korb_ary_rev_range(c, ary->items, 0, ary->len);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_rotate_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t cnt = 1;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE cv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(cv, &cnt))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(cv));
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    uint32_t n = ary->len;
    if (n > 1) {
        intptr_t k = ((cnt % (intptr_t)n) + (intptr_t)n) % (intptr_t)n;   /* normalize */
        KorbArrayItems *it = ary->items;
        korb_ary_rev_range(c, it, 0, (uint32_t)k);
        korb_ary_rev_range(c, it, (uint32_t)k, n);
        korb_ary_rev_range(c, it, 0, n);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* product(other, ...) → cartesian product as an array of rows. */
static RESULT korb_m_ary_product(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t na = VALUE_SLICE_LEN(a);
    if (na > 15) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#product with >16 arrays is not supported");
    for (uint32_t j = 0; j < na; j++)
        if (UNLIKELY(!KORB_ARRAY_P(VALUE_SLICE_GET(a, j))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, j)));
    uint32_t k = na + 1;                                  /* self + args */
    #define ARR_J(j) ((j) == 0 ? VAL2ARY(VALUE_REF_GET(self)) : VAL2ARY(VALUE_SLICE_GET(a, (j) - 1)))
    uint32_t lens[16];
    uint64_t total = 1;
    for (uint32_t j = 0; j < k; j++) { lens[j] = ARR_J(j)->len; total *= lens[j]; }
    uint32_t capa = total > 0 && total < 0x40000000ull ? (uint32_t)total : 4;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, capa)));
    if (total == 0) return RESULT_OK(VALUE_REF_GET(dst));
    uint32_t idx[16] = {0};
    for (uint64_t t = 0; t < total; t++) {
        slots[0] = UNWRAP(korb_ary_new(c, slots, k));    /* row, rooted at slots[0] */
        VALUE_REF row = VALUE_REF_AT(&slots[0]);
        for (uint32_t j = 0; j < k; j++) {
            VALUE e = ARR_J(j)->items->data[idx[j]];
            CHECK(korb_ary_push_val(c, slots + 1, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        for (int j = (int)k - 1; j >= 0; j--) { if (++idx[j] < lens[j]) break; idx[j] = 0; }
    }
    #undef ARR_J
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE iv = VALUE_SLICE_GET(a, j);
        intptr_t idx;
        if (UNLIKELY(!korb_to_index(iv, &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        intptr_t n = ary->len, orig = idx;
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld outside of array bounds: -%ld...%ld", (long)orig, (long)n, (long)n);
        CHECK(korb_ary_push_val(c, slots + 1, dst, ary->items->data[idx]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* one?: exactly one truthy element (or exactly one block-truthy element). */
static RESULT korb_m_ary_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    (void)a;
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        VALUE e = ary->items->data[i];
        bool t;
        if (block != NULL) {
            slots[0] = e;
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(e);
        }
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}

/* ---- more String methods ------------------------------------------------- */

/* String#% : printf-style formatting. Single arg or an Array of args.
 * Supports d/i/u, f/e/E/g/G, x/X/o, b, s, c, p, %% with C flags/width/precision
 * (binary `b` honors width/0-flag manually; `*` dynamic width is unsupported). */
static RESULT korb_m_str_format(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *fs = VAL2STR(VALUE_REF_GET(self));
    const char *fmt = fs->buf->data; uint32_t flen = fs->len;
    VALUE single = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    const VALUE *args; uint32_t argn;
    if (KORB_ARRAY_P(single)) { args = VAL2ARY(single)->items->data; argn = VAL2ARY(single)->len; }
    else { args = &single; argn = VALUE_SLICE_LEN(a); }
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    uint32_t ai = 0; bool err = false; const char *errmsg = NULL;
    for (uint32_t i = 0; i < flen; i++) {
        if (fmt[i] != '%') { fputc(fmt[i], ms); continue; }
        char spec[64]; int si = 0; spec[si++] = '%';
        i++;
        if (i < flen && fmt[i] == '%') { fputc('%', ms); continue; }
        while (i < flen && strchr("-+ 0#", fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; }
        while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; }
        if (i < flen && fmt[i] == '.') { if (si < 58) spec[si++] = '.'; i++; while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; } }
        if (i >= flen) { err = true; errmsg = "malformed format sequence"; break; }
        char conv = fmt[i];
        VALUE arg = (ai < argn) ? args[ai] : KORB_NIL;
        switch (conv) {
          case 'd': case 'i': case 'u': {
            intptr_t v;
            if (FIXNUM_P(arg)) v = FIX2LONG(arg);
            else if (KORB_FLOAT_P(arg)) v = (intptr_t)VAL2FLT(arg)->val;
            else { err = true; errmsg = "expected a number"; break; }
            spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = '\0';
            fprintf(ms, spec, (long)v); ai++;
            break;
          }
          case 'f': case 'e': case 'E': case 'g': case 'G': {
            double v; if (!korb_num_to_d(arg, &v)) { err = true; errmsg = "expected a number"; break; }
            spec[si++] = conv; spec[si] = '\0';
            fprintf(ms, spec, v); ai++;
            break;
          }
          case 'x': case 'X': case 'o': {
            intptr_t v; if (FIXNUM_P(arg)) v = FIX2LONG(arg); else { err = true; errmsg = "expected Integer"; break; }
            spec[si++] = 'l'; spec[si++] = conv; spec[si] = '\0';
            fprintf(ms, spec, (long)v); ai++;
            break;
          }
          case 'b': case 'B': {
            intptr_t v; if (FIXNUM_P(arg)) v = FIX2LONG(arg); else { err = true; errmsg = "expected Integer"; break; }
            bool left = false, zero = false; int width = 0;          /* parse flags/width from spec */
            for (int k = 1; k < si; k++) {
                char sc = spec[k];
                if (sc == '-') left = true;
                else if (sc == '0') zero = true;
                else if (sc == '+' || sc == ' ' || sc == '#') { /* ignored for binary */ }
                else if (isdigit((unsigned char)sc)) width = width * 10 + (sc - '0');
                else break;
            }
            char tmp[80]; uint32_t n = korb_fmt_int(v, 2, tmp);
            int pad = width > (int)n ? width - (int)n : 0;
            if (!left) { char padc = zero ? '0' : ' '; for (int p = 0; p < pad; p++) fputc(padc, ms); }
            fwrite(tmp, 1, n, ms);
            if (left) for (int p = 0; p < pad; p++) fputc(' ', ms);
            ai++;
            break;
          }
          case 's': {
            spec[si++] = 's'; spec[si] = '\0';
            if (KORB_STRING_P(arg)) { fprintf(ms, spec, VAL2STR(arg)->buf->data); }
            else {
                char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
                if (tms) { korb_fprint_to_s(c, tms, arg); fclose(tms); }
                fprintf(ms, spec, tb ? tb : ""); free(tb);
            }
            ai++;
            break;
          }
          case 'p': {
            char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
            if (tms) { korb_fprint_inspect(c, tms, arg); fclose(tms); }
            spec[si++] = 's'; spec[si] = '\0';
            fprintf(ms, spec, tb ? tb : ""); free(tb); ai++;
            break;
          }
          case 'c': {
            if (FIXNUM_P(arg)) fputc((int)FIX2LONG(arg), ms);
            else if (KORB_STRING_P(arg) && VAL2STR(arg)->len > 0) fwrite(VAL2STR(arg)->buf->data, 1, 1, ms);
            ai++;
            break;
          }
          default: err = true; errmsg = "malformed format sequence"; break;
        }
        if (err) break;
    }
    fclose(ms);
    if (err) { free(buf); return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "%s", errmsg ? errmsg : "format error"); }
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}

RESULT korb_str_mod(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs) {
    slots[0] = rhs;
    return korb_m_str_format(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
}

static int korb_ci_cmp(const char *a, uint32_t al, const char *b, uint32_t bl) {
    uint32_t m = al < bl ? al : bl;
    for (uint32_t i = 0; i < m; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return (al > bl) - (al < bl);
}
static RESULT korb_m_str_casecmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(LONG2FIX(korb_ci_cmp(s->buf->data, s->len, t->buf->data, t->len)));
}
static RESULT korb_m_str_casecmp_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(korb_ci_cmp(s->buf->data, s->len, t->buf->data, t->len) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_str_byteslice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t bn = s->len;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (UNLIKELY(!korb_to_index(iv, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    if (i < 0) i += bn;
    if (i < 0 || i > (intptr_t)bn) return RESULT_OK(KORB_NIL);
    intptr_t lentmp;
    intptr_t len = (VALUE_SLICE_LEN(a) >= 2 && korb_to_index(VALUE_SLICE_GET(a, 1), &lentmp)) ? lentmp : 1;
    if (len < 0) return RESULT_OK(KORB_NIL);
    if (i + len > (intptr_t)bn) len = (intptr_t)bn - i;
    return korb_str_slice_new(c, slots, self, (uint32_t)i, (uint32_t)len);
}
static RESULT korb_m_str_getbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (!FIXNUM_P(iv)) return RESULT_OK(KORB_NIL);
    intptr_t i = FIX2LONG(iv); if (i < 0) i += s->len;
    if (i < 0 || (uint32_t)i >= s->len) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX((unsigned char)s->buf->data[i]));
}
static RESULT korb_m_str_setbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE iv = VALUE_SLICE_GET(a, 0), bv = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!FIXNUM_P(iv) || !FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t i = FIX2LONG(iv); if (i < 0) i += s->len;
    if (UNLIKELY(i < 0 || (uint32_t)i >= s->len)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld out of string", (long)FIX2LONG(iv));
    s->buf->data[i] = (char)(FIX2LONG(bv) & 0xFF);
    return RESULT_OK(bv);
}
static RESULT korb_m_sym_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    return korb_m_str_aref(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_str_byteindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    int32_t b = korb_byte_find(s->buf->data, s->len, n->buf->data, n->len);
    return RESULT_OK(b < 0 ? KORB_NIL : LONG2FIX(b));
}
static RESULT korb_m_str_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    if (n->len > s->len) return RESULT_OK(KORB_NIL);
    for (int32_t i = (int32_t)(s->len - n->len); i >= 0; i--)
        if (memcmp(s->buf->data + i, n->buf->data, n->len) == 0)
            return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, (uint32_t)i)));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t len = VAL2STR(VALUE_REF_GET(self))->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));     /* re-read after GC */
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        r->buf->data[i] = (char)(isupper(ch) ? tolower(ch) : islower(ch) ? toupper(ch) : ch);
    }
    return RESULT_OK((VALUE)r);
}
/* ljust(0)/rjust(1)/center(2) — char-width padding via a transient buffer */
static RESULT korb_str_pad(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE wv = VALUE_SLICE_GET(a, 0);
    intptr_t width;
    if (UNLIKELY(!korb_to_index(wv, &width))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(wv));
    const KorbString *padstr = (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) ? VAL2STR(VALUE_SLICE_GET(a, 1)) : NULL;
    if (padstr && padstr->len == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "zero width padding");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    if (width <= (intptr_t)ncp) return korb_str_slice_new(c, slots, self, 0, s->len);
    uint32_t total_pad = (uint32_t)width - ncp;
    uint32_t left = mode == 1 ? total_pad : mode == 2 ? total_pad / 2 : 0;
    uint32_t right = total_pad - left;
    const char *pb = padstr ? padstr->buf->data : " ";
    uint32_t pl = padstr ? padstr->len : 1;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    for (uint32_t i = 0; i < left; i++)  fputc(pb[i % pl], ms);   /* byte-cycle pad (ASCII pad exact) */
    fwrite(s->buf->data, 1, s->len, ms);
    for (uint32_t i = 0; i < right; i++) fputc(pb[i % pl], ms);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_str_ljust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 0); }
static RESULT korb_m_str_rjust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 1); }
static RESULT korb_m_str_center(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_pad(c, slots, self, a, 2); }

static void
korb_register_core_methods(CTX *c)
{
    /* Integer */
    korb_def_cmethod(c, KORB_C_INTEGER, "abs", korb_m_int_abs, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "magnitude", korb_m_int_abs, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "succ", korb_m_int_succ, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "next", korb_m_int_succ, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "pred", korb_m_int_pred, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "zero?", korb_m_int_zero, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "nonzero?", korb_m_int_nonzero, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "even?", korb_m_int_even, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "odd?", korb_m_int_odd, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "positive?", korb_m_int_pos, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "negative?", korb_m_int_neg, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_f", korb_m_int_to_f, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_i", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_int", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "ord", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "integer?", korb_m_true_lit, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_s", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "inspect", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "chr", korb_m_int_chr, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "**", korb_m_int_pow, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "pow", korb_m_int_pow, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "divmod", korb_m_int_divmod, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "div", korb_m_int_div, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "modulo", korb_m_int_modulo, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "gcd", korb_m_int_gcd, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "lcm", korb_m_int_lcm, 1);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "step", korb_m_num_step, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "finite?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "fdiv", korb_m_int_fdiv, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "ceildiv", korb_m_int_ceildiv, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "coerce", korb_m_int_coerce, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "<=>", korb_m_int_cmp, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "between?", korb_m_int_between, 2);
    korb_def_cmethod(c, KORB_C_INTEGER, "clamp", korb_m_int_clamp, 2);
    korb_def_cmethod(c, KORB_C_INTEGER, "digits", korb_m_int_digits, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "<<", korb_m_int_lshift, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, ">>", korb_m_int_rshift, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "[]", korb_m_int_bitref, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "&", korb_m_int_and, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "|", korb_m_int_or, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "^", korb_m_int_xor, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "~", korb_m_int_inv, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "remainder", korb_m_int_remainder, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "truncate", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "floor", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "ceil", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "round", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "dup", korb_m_int_self2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "clone", korb_m_int_self2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "abs2", korb_m_int_abs2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "nobits?", korb_m_int_nobits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "anybits?", korb_m_int_anybits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "allbits?", korb_m_int_allbits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "gcdlcm", korb_m_int_gcdlcm, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "+@", korb_m_int_self2, 0);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "times", korb_m_int_times, 0);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "upto", korb_m_int_upto, 1);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "downto", korb_m_int_downto, 1);

    /* String */
    korb_def_cmethod(c, KORB_C_STRING, "length", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "size", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "bytesize", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "empty?", korb_m_str_empty, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_s", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_str", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "+@", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "-@", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_i", korb_m_str_to_i, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_sym", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "intern", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase", korb_m_str_upcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "downcase", korb_m_str_downcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize", korb_m_str_capitalize, 0);
    korb_def_cmethod(c, KORB_C_STRING, "reverse", korb_m_str_reverse, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<<", korb_m_str_ltlt, 1);
    korb_def_cmethod(c, KORB_C_STRING, "concat", korb_m_str_concat, -1);
    korb_def_cmethod(c, KORB_C_STRING, "replace", korb_m_str_replace, 1);
    korb_def_cmethod(c, KORB_C_STRING, "prepend", korb_m_str_prepend, -1);
    korb_def_cmethod(c, KORB_C_STRING, "clear", korb_m_str_clear, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase!", korb_m_str_upcase_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "downcase!", korb_m_str_downcase_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize!", korb_m_str_capitalize_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase!", korb_m_str_swapcase_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "reverse!", korb_m_str_reverse_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "[]=", korb_m_str_aset, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice!", korb_m_str_slice_bang, -1);
    korb_def_cmethod(c, KORB_C_STRING, "strip!", korb_m_str_strip_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "lstrip!", korb_m_str_lstrip_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "rstrip!", korb_m_str_rstrip_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "chomp!", korb_m_str_chomp_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "chop!", korb_m_str_chop_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "count", korb_m_str_count, -1);
    korb_def_cmethod(c, KORB_C_STRING, "sum", korb_m_str_sum, -1);
    korb_def_cmethod(c, KORB_C_STRING, "squeeze", korb_m_str_squeeze, -1);
    korb_def_cmethod(c, KORB_C_STRING, "squeeze!", korb_m_str_squeeze_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "append_as_bytes", korb_m_str_append_as_bytes, -1);
    korb_def_cmethod(c, KORB_C_STRING, "ascii_only?", korb_m_str_ascii_only, 0);
    korb_def_cmethod(c, KORB_C_STRING, "rpartition", korb_m_str_rpartition, 1);
    korb_def_cmethod(c, KORB_C_STRING, "scrub", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "scrub!", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "include?", korb_m_str_include, 1);
    korb_def_cmethod(c, KORB_C_STRING, "start_with?", korb_m_str_start_with, -1);
    korb_def_cmethod(c, KORB_C_STRING, "end_with?", korb_m_str_end_with, -1);
    korb_def_cmethod(c, KORB_C_STRING, "index", korb_m_str_index, 1);
    korb_def_cmethod(c, KORB_C_STRING, "strip", korb_m_str_strip, 0);
    korb_def_cmethod(c, KORB_C_STRING, "lstrip", korb_m_str_lstrip, 0);
    korb_def_cmethod(c, KORB_C_STRING, "rstrip", korb_m_str_rstrip, 0);
    korb_def_cmethod(c, KORB_C_STRING, "chomp", korb_m_str_chomp, 0);
    korb_def_cmethod(c, KORB_C_STRING, "chop", korb_m_str_chop, 0);
    korb_def_cmethod(c, KORB_C_STRING, "split", korb_m_str_split, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chars", korb_m_str_chars, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<=>", korb_m_str_cmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "%", korb_m_str_format, 1);
    korb_def_cmethod(c, KORB_C_STRING, "*", korb_m_str_mul, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp", korb_m_str_casecmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp?", korb_m_str_casecmp_p, 1);
    korb_def_cmethod(c, KORB_C_STRING, "byteslice", korb_m_str_byteslice, -1);
    korb_def_cmethod(c, KORB_C_STRING, "getbyte", korb_m_str_getbyte, 1);
    korb_def_cmethod(c, KORB_C_STRING, "setbyte", korb_m_str_setbyte, 2);
    korb_def_cmethod(c, KORB_C_STRING, "b", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "dedup", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "byteindex", korb_m_str_byteindex, 1);
    korb_def_cmethod(c, KORB_C_STRING, "rindex", korb_m_str_rindex, 1);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase", korb_m_str_swapcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "ljust", korb_m_str_ljust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rjust", korb_m_str_rjust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "center", korb_m_str_center, -1);
    korb_def_cmethod(c, KORB_C_STRING, "[]", korb_m_str_aref, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice", korb_m_str_aref, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_char", korb_m_str_each_char, 0);

    /* Symbol */
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_s", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "id2name", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "slice", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "[]", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "name", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_sym", korb_m_sym_to_sym, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "length", korb_m_sym_len, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "size", korb_m_sym_len, 0);

    /* nil */
    korb_def_cmethod(c, KORB_C_NIL, "to_s", korb_m_nil_to_s, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_i", korb_m_nil_to_i, 0);
    korb_def_cmethod(c, KORB_C_NIL, "nil?", korb_m_nil_nil_q, 0);

    /* true / false */
    korb_def_cmethod(c, KORB_C_TRUE,  "to_s", korb_m_true_to_s, 0);
    korb_def_cmethod(c, KORB_C_TRUE,  "inspect", korb_m_true_to_s, 0);
    korb_def_cmethod(c, KORB_C_FALSE, "to_s", korb_m_false_to_s, 0);
    korb_def_cmethod(c, KORB_C_FALSE, "inspect", korb_m_false_to_s, 0);

    /* Array */
    korb_def_cmethod(c, KORB_C_ARRAY, "length", korb_m_ary_len, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "size", korb_m_ary_len, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "empty?", korb_m_ary_empty, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "to_a", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "to_ary", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "entries", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "sort!", korb_m_ary_sort_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "tally", korb_m_ary_tally, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "first", korb_m_ary_first, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "last", korb_m_ary_last, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]", korb_m_ary_aref, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "slice", korb_m_ary_aref, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "at", korb_m_ary_aref, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "values_at", korb_m_ary_values_at, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "fill", korb_m_ary_fill, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]=", korb_m_ary_aset, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "<<", korb_m_ary_ltlt, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "push", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "append", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "pop", korb_m_ary_pop, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "include?", korb_m_ary_include, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse", korb_m_ary_reverse, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse!", korb_m_ary_reverse_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "rotate!", korb_m_ary_rotate_bang, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "product", korb_m_ary_product, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "fetch_values", korb_m_ary_fetch_values, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "one?", korb_m_ary_one, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reverse_each", korb_m_ary_reverse_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "take_while", korb_m_ary_take_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "drop_while", korb_m_ary_drop_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "bsearch", korb_m_ary_bsearch, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "bsearch_index", korb_m_ary_bsearch_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "map!", korb_m_ary_map_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect!", korb_m_ary_map_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_entry", korb_m_ary_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort_by", korb_m_ary_sort_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort_by!", korb_m_ary_sort_by_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "min_by", korb_m_ary_min_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "max_by", korb_m_ary_max_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter_map", korb_m_ary_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "flat_map", korb_m_ary_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect_concat", korb_m_ary_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "partition", korb_m_ary_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "group_by", korb_m_ary_group_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "grep", korb_m_ary_grep, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "grep_v", korb_m_ary_grep_v, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "clear", korb_m_ary_clear, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "intersect?", korb_m_ary_intersect_q, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "+", korb_m_ary_plus, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "*", korb_m_ary_mul, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "index", korb_m_ary_index, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "count", korb_m_ary_count, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "sum", korb_m_ary_sum, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "min", korb_m_ary_min, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "max", korb_m_ary_max, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "sort", korb_m_ary_sort, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "join", korb_m_ary_join, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "compact", korb_m_ary_compact, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "compact!", korb_m_ary_compact_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "slice!", korb_m_ary_slice_bang, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_index", korb_m_ary_each_index, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "uniq", korb_m_ary_uniq, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "flatten", korb_m_ary_flatten, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "concat", korb_m_ary_concat, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "unshift", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "prepend", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shift", korb_m_ary_shift, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "assoc", korb_m_ary_assoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rassoc", korb_m_ary_rassoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "fetch", korb_m_ary_fetch, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "dig", korb_m_ary_dig, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "take", korb_m_ary_take, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "drop", korb_m_ary_drop, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "difference", korb_m_ary_difference, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "-", korb_m_ary_difference, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "replace", korb_m_ary_replace, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "delete", korb_m_ary_delete, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "delete_at", korb_m_ary_delete_at, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rindex", korb_m_ary_rindex, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "member?", korb_m_ary_include, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rotate", korb_m_ary_rotate, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "zip", korb_m_ary_zip, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "deconstruct", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "insert", korb_m_ary_insert, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "|", korb_m_ary_union, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "union", korb_m_ary_union, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "&", korb_m_ary_intersect, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "intersection", korb_m_ary_intersect, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each", korb_m_ary_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_with_index", korb_m_ary_each_wi, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "map", korb_m_ary_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect", korb_m_ary_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "select", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find_all", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reject", korb_m_ary_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find", korb_m_ary_find, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "detect", korb_m_ary_find, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find_index", korb_m_ary_find_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "any?", korb_m_ary_any, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "all?", korb_m_ary_all, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "none?", korb_m_ary_none, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reduce", korb_m_ary_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "inject", korb_m_ary_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_with_object", korb_m_ary_each_with_object, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "delete_if", korb_m_ary_delete_if, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reject!", korb_m_ary_reject_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "keep_if", korb_m_ary_keep_if, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "select!", korb_m_ary_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter!", korb_m_ary_select_bang, 0);

    /* Hash */
    korb_def_cmethod(c, KORB_C_HASH, "[]", korb_m_hash_aref, 1);
    korb_def_cmethod(c, KORB_C_HASH, "[]=", korb_m_hash_aset, 2);
    korb_def_cmethod(c, KORB_C_HASH, "store", korb_m_hash_aset, 2);
    korb_def_cmethod(c, KORB_C_HASH, "size", korb_m_hash_size, 0);
    korb_def_cmethod(c, KORB_C_HASH, "length", korb_m_hash_size, 0);
    korb_def_cmethod(c, KORB_C_HASH, "count", korb_m_hash_size, 0);
    korb_def_cmethod(c, KORB_C_HASH, "empty?", korb_m_hash_empty, 0);
    korb_def_cmethod(c, KORB_C_HASH, "key?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "has_key?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "include?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "member?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "value?", korb_m_hash_value_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "has_value?", korb_m_hash_value_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "fetch", korb_m_hash_fetch, -1);
    korb_def_cmethod(c, KORB_C_HASH, "keys", korb_m_hash_keys, 0);
    korb_def_cmethod(c, KORB_C_HASH, "values", korb_m_hash_values, 0);
    korb_def_cmethod(c, KORB_C_HASH, "delete", korb_m_hash_delete, 1);
    korb_def_cmethod(c, KORB_C_HASH, "merge", korb_m_hash_merge, 1);
    korb_def_cmethod(c, KORB_C_HASH, "key", korb_m_hash_key, 1);
    korb_def_cmethod(c, KORB_C_HASH, "rassoc", korb_m_hash_rassoc, 1);
    korb_def_cmethod(c, KORB_C_HASH, "<", korb_m_hash_lt, 1);
    korb_def_cmethod(c, KORB_C_HASH, "<=", korb_m_hash_le, 1);
    korb_def_cmethod(c, KORB_C_HASH, ">", korb_m_hash_gt, 1);
    korb_def_cmethod(c, KORB_C_HASH, ">=", korb_m_hash_ge, 1);
    korb_def_cmethod(c, KORB_C_HASH, "to_h", korb_m_hash_self, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_a", korb_m_hash_to_a, 0);
    korb_def_cmethod(c, KORB_C_HASH, "sort", korb_m_hash_sort, 0);
    korb_def_cmethod(c, KORB_C_HASH, "fetch_values", korb_m_hash_fetch_values, -1);
    korb_def_cmethod(c, KORB_C_HASH, "dig", korb_m_hash_dig, -1);
    korb_def_cmethod(c, KORB_C_HASH, "values_at", korb_m_hash_values_at, -1);
    korb_def_cmethod(c, KORB_C_HASH, "slice", korb_m_hash_slice, -1);
    korb_def_cmethod(c, KORB_C_HASH, "except", korb_m_hash_except, -1);
    korb_def_cmethod(c, KORB_C_HASH, "default", korb_m_hash_default, 0);
    korb_def_cmethod(c, KORB_C_HASH, "drop", korb_m_hash_drop, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_pair", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "map", korb_m_hash_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "collect", korb_m_hash_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "flat_map", korb_m_hash_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "select", korb_m_hash_select, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter", korb_m_hash_select, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reject", korb_m_hash_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "any?", korb_m_hash_any, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "all?", korb_m_hash_all, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "none?", korb_m_hash_none, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reduce", korb_m_hash_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "inject", korb_m_hash_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_with_object", korb_m_hash_each_wo, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "sum", korb_m_hash_sum, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "select!", korb_m_hash_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter!", korb_m_hash_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "keep_if", korb_m_hash_keep_if, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reject!", korb_m_hash_reject_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "delete_if", korb_m_hash_delete_if, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "one?", korb_m_hash_one, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "sort_by", korb_m_hash_sort_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "min_by", korb_m_hash_min_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "max_by", korb_m_hash_max_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter_map", korb_m_hash_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "partition", korb_m_hash_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find", korb_m_hash_find, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "detect", korb_m_hash_find, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find_all", korb_m_hash_find_all, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find_index", korb_m_hash_find_index, 0);
    korb_def_cmethod(c, KORB_C_HASH, "zip", korb_m_hash_zip, -1);

    /* Range */
    korb_def_cmethod(c, KORB_C_RANGE, "begin", korb_m_range_begin, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "first", korb_m_range_first, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "end", korb_m_range_end, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "last", korb_m_range_last, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "exclude_end?", korb_m_range_exclude, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "size", korb_m_range_size, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "count", korb_m_range_size, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "include?", korb_m_range_cover, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "member?", korb_m_range_cover, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "cover?", korb_m_range_cover, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "min", korb_m_range_min, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "max", korb_m_range_max, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "sum", korb_m_range_sum, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "to_a", korb_m_range_to_a, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "to_ary", korb_m_range_to_a, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "entries", korb_m_range_to_a, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each", korb_m_range_each, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_entry", korb_m_range_each, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_with_index", korb_m_range_each_wi, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "filter_map", korb_m_range_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "map", korb_m_range_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "collect", korb_m_range_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "step", korb_m_range_step, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "reduce", korb_m_range_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "inject", korb_m_range_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "select", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "filter", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find_all", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "reject", korb_m_range_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find", korb_m_range_find, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "detect", korb_m_range_find, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "drop", korb_m_range_drop, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "zip", korb_m_range_zip, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "one?", korb_m_range_one, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find_index", korb_m_range_find_index, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "drop_while", korb_m_range_drop_while, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "take_while", korb_m_range_take_while, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "any?", korb_m_range_any, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "all?", korb_m_range_all, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "none?", korb_m_range_none, 0);

    /* Object (universal fallback) */
    korb_def_cmethod(c, KORB_C_OBJECT, "nil?", korb_m_obj_nil_q, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "==", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "!=", korb_m_obj_neq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "equal?", korb_m_obj_equal, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "eql?", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "itself", korb_m_obj_itself, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "<=>", korb_m_obj_cmp, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "class", korb_m_obj_class, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "is_a?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "kind_of?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_of?", korb_m_obj_instance_of, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_false, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "dup", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "clone", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_NIL,    "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_TRUE,   "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_FALSE,  "frozen?", korb_m_true_lit2, 0);

    /* Exception */
    korb_def_cmethod(c, KORB_C_EXCEPTION, "message", korb_m_exc_message, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "to_s", korb_m_exc_message, 0);

    /* Float */
    korb_def_cmethod(c, KORB_C_FLOAT, "to_f", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_i", korb_m_flt_to_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_int", korb_m_flt_to_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "truncate", korb_m_flt_to_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "floor", korb_m_flt_floor, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "ceil", korb_m_flt_ceil, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "round", korb_m_flt_round, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "abs", korb_m_flt_abs, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "magnitude", korb_m_flt_abs, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "abs2", korb_m_flt_abs2, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "**", korb_m_flt_pow, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "pow", korb_m_flt_pow, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "angle", korb_m_flt_angle, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "arg", korb_m_flt_angle, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "dup", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "+@", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "between?", korb_m_flt_between, 2);
    korb_def_cmethod(c, KORB_C_FLOAT, "zero?", korb_m_flt_zero, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "nonzero?", korb_m_flt_nonzero, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "nan?", korb_m_flt_nan, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "infinite?", korb_m_flt_inf, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "<=>", korb_m_flt_cmp, 1);
    korb_def_cmethod_blk(c, KORB_C_FLOAT, "step", korb_m_num_step, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "fdiv", korb_m_flt_fdiv, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "quo", korb_m_flt_fdiv, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "div", korb_m_flt_div, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "modulo", korb_m_flt_modulo, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "coerce", korb_m_flt_coerce, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "finite?", korb_m_flt_finite, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "next_float", korb_m_flt_next, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "prev_float", korb_m_flt_prev, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "divmod", korb_m_flt_divmod, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "negative?", korb_m_flt_neg_q, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "positive?", korb_m_flt_pos_q, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_s", korb_m_flt_to_s, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "inspect", korb_m_flt_to_s, 0);
}

/* ---------------------------------------------------------------------------
 * Printing (CRuby-compatible to_s / inspect, written directly — no GC).
 * ------------------------------------------------------------------------- */

/* Inspect-quote a byte run: "..." with CRuby's escape set. */
static void
korb_fprint_quoted(FILE *fp, const char *bytes, uint32_t len)
{
    fputc('"', fp);
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)bytes[i];
        switch (ch) {
          case '"':  fputs("\\\"", fp); break;
          case '\\': fputs("\\\\", fp); break;
          case '\n': fputs("\\n", fp); break;
          case '\t': fputs("\\t", fp); break;
          case '\r': fputs("\\r", fp); break;
          case '\f': fputs("\\f", fp); break;
          case '\v': fputs("\\v", fp); break;
          case '\b': fputs("\\b", fp); break;
          case '\a': fputs("\\a", fp); break;
          case 27:   fputs("\\e", fp); break;
          case '#':
            if (i + 1 < len && (bytes[i+1] == '{' || bytes[i+1] == '$' || bytes[i+1] == '@'))
                fputs("\\#", fp);
            else
                fputc('#', fp);
            break;
          default:
            if (ch < 0x20 || ch == 0x7f) fprintf(fp, "\\u%04X", ch);
            else fputc(ch, fp);   /* non-ASCII UTF-8 passes through */
        }
    }
    fputc('"', fp);
}

/* Can a symbol name appear bare as a hash label `name:`?  CRuby 3.4+: an
 * identifier optionally ending in ? or ! (not =, not empty, not operator). */
static bool
korb_sym_label_bare(const char *nm)
{
    if (!(*nm == '_' || (*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z')))
        return false;
    const char *p = nm + 1;
    while (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9')) p++;
    if (*p == '?' || *p == '!') p++;
    return *p == '\0';
}

/* Range: "1..5" / "1...5"; endpoints to_s for to_s, inspect for inspect. */
static void
korb_fprint_range(CTX *c, FILE *fp, VALUE v, bool insp)
{
    const KorbRange *r = VAL2RANGE(v);
    if (insp) korb_fprint_inspect(c, fp, r->rbegin); else korb_fprint_to_s(c, fp, r->rbegin);
    fputs(r->exclude_end ? "..." : "..", fp);
    if (insp) korb_fprint_inspect(c, fp, r->rend); else korb_fprint_to_s(c, fp, r->rend);
}

/* Array renders identically for to_s and inspect: "[1, 2, \"x\"]" — elements
 * always use inspect form. */
static void
korb_fprint_ary(CTX *c, FILE *fp, VALUE v)
{
    const KorbArray *a = VAL2ARY(v);
    fputc('[', fp);
    for (uint32_t i = 0; i < a->len; i++) {
        if (i) fputs(", ", fp);
        korb_fprint_inspect(c, fp, a->items->data[i]);
    }
    fputc(']', fp);
}

/* Hash inspect (== to_s), CRuby 4.0 form: symbol keys as `name: v` (quoted if
 * not a bare label), other keys as `k => v`. */
static void
korb_fprint_hash(CTX *c, FILE *fp, VALUE v)
{
    const KorbHash *h = VAL2HASH(v);
    fputc('{', fp);
    for (uint32_t i = 0; i < h->len; i++) {
        if (i) fputs(", ", fp);
        VALUE k = h->items->data[2 * i];
        if (SYMBOL_P(k)) {
            const char *nm = korb_sym_name(c->vm, SYM2ID(k));
            if (korb_sym_label_bare(nm)) fputs(nm, fp);
            else korb_fprint_quoted(fp, nm, (uint32_t)strlen(nm));
            fputs(": ", fp);
        } else {
            korb_fprint_inspect(c, fp, k);
            fputs(" => ", fp);
        }
        korb_fprint_inspect(c, fp, h->items->data[2 * i + 1]);
    }
    fputc('}', fp);
}

void
korb_fprint_to_s(CTX *c, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))           { fprintf(fp, "%ld", (long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)         { return; }                     /* "" */
    if (v == KORB_TRUE)        { fputs("true", fp); return; }
    if (v == KORB_FALSE)       { fputs("false", fp); return; }
    if (SYMBOL_P(v))           { fputs(korb_sym_name(c->vm, SYM2ID(v)), fp); return; }
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: {
        const KorbString *s = VAL2STR(v);
        fwrite(s->buf->data, 1, s->len, fp);
        return;
      }
      case KORB_OBJ_ARRAY:
        korb_fprint_ary(c, fp, v);
        return;
      case KORB_OBJ_HASH:
        korb_fprint_hash(c, fp, v);
        return;
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, false);
        return;
      case KORB_OBJ_OBJECT: {
        const KorbObject *o = VAL2OBJ(v);
        if (o->klass == KORB_NIL) { fputs("main", fp); return; }       /* top-level self */
        fprintf(fp, "#<%s>", korb_sym_name(c->vm, VAL2CLASS(o->klass)->name_sym));
        return;
      }
      case KORB_OBJ_CLASS:
        fputs(korb_sym_name(c->vm, VAL2CLASS(v)->name_sym), fp);       /* class name */
        return;
      case KORB_OBJ_FLOAT: {
        char fb[40];
        korb_float_to_s(VAL2FLT(v)->val, fb);
        fputs(fb, fp);
        return;
      }
      case KORB_OBJ_EXCEPTION: {
        const KorbException *e = VAL2EXC(v);
        if (e->msg != KORB_NIL) fwrite(VAL2STR(e->msg)->buf->data, 1, VAL2STR(e->msg)->len, fp);
        else fputs(korb_etype_name(e->etype), fp);
        return;
      }
    }
    fputs("#<Object>", fp);
}

void
korb_fprint_inspect(CTX *c, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))     { fprintf(fp, "%ld", (long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)   { fputs("nil", fp); return; }
    if (v == KORB_TRUE)  { fputs("true", fp); return; }
    if (v == KORB_FALSE) { fputs("false", fp); return; }
    if (SYMBOL_P(v))     { fprintf(fp, ":%s", korb_sym_name(c->vm, SYM2ID(v))); return; }
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: {
        const KorbString *s = VAL2STR(v);
        korb_fprint_quoted(fp, s->buf->data, s->len);
        return;
      }
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, true);   /* inspect endpoints */
        return;
    }
    korb_fprint_to_s(c, fp, v);
}

/* ---------------------------------------------------------------------------
 * Builtins.
 * ------------------------------------------------------------------------- */

/* puts one value, newline-terminated; arrays flatten recursively (each element
 * on its own line), matching CRuby.  An empty array prints nothing. */
static void
korb_puts_one(CTX *c, VALUE v)
{
    if (KORB_ARRAY_P(v)) {
        const KorbArray *a = VAL2ARY(v);
        for (uint32_t i = 0; i < a->len; i++) korb_puts_one(c, a->items->data[i]);
        return;
    }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        fwrite(s->buf->data, 1, s->len, stdout);
        if (s->len == 0 || s->buf->data[s->len - 1] != '\n') fputc('\n', stdout);
        return;
    }
    korb_fprint_to_s(c, stdout, v);
    fputc('\n', stdout);
}

static RESULT
korb_bi_puts(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) {
        fputc('\n', stdout);
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = 0; i < n; i++) korb_puts_one(c, VALUE_SLICE_GET(args, i));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_bi_p(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    uint32_t n = VALUE_SLICE_LEN(args);
    for (uint32_t i = 0; i < n; i++) {
        korb_fprint_inspect(c, stdout, VALUE_SLICE_GET(args, i));
        fputc('\n', stdout);
    }
    /* M0: p(a) → a; p() → nil; p(a, b, ...) returns an Array in CRuby —
     * arrays land in M1, return the first arg until then. */
    return RESULT_OK(n > 0 ? VALUE_SLICE_GET(args, 0) : KORB_NIL);
}

static RESULT
korb_bi_print(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(args); i++) {
        korb_fprint_to_s(c, stdout, VALUE_SLICE_GET(args, i));
    }
    return RESULT_OK(KORB_NIL);
}

/* raise — `raise "msg"` / `raise` → RuntimeError.  (Class-form raise needs the
 * Exception hierarchy, not yet present.) */
static RESULT
korb_bi_raise(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n >= 1) {
        VALUE a0 = VALUE_SLICE_GET(args, 0);
        if (KORB_STRING_P(a0)) {
            const KorbString *s = VAL2STR(a0);
            return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%.*s", (int)s->len, s->buf->data);
        }
        if (KORB_EXC_P(a0)) return RESULT_RAISE_(a0);   /* re-raise an exception object */
        if (KORB_CLASS_P(a0)) {                          /* raise SomeError[, msg] */
            const KorbClass *k = VAL2CLASS(a0);
            if (k->exc_etype < 0)
                return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
            if (n >= 2 && KORB_STRING_P(VALUE_SLICE_GET(args, 1))) {
                const KorbString *s = VAL2STR(VALUE_SLICE_GET(args, 1));
                return korb_raise(c, slots, (unsigned)k->exc_etype, 0, "%.*s", (int)s->len, s->buf->data);
            }
            return korb_raise(c, slots, (unsigned)k->exc_etype, 0, "%s", korb_sym_name(c->vm, k->name_sym));
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
    }
    return korb_raise(c, slots, KORB_E_RUNTIME, 0, "unhandled exception");
}

/* ---------------------------------------------------------------------------
 * CTX creation (main.c entry helper).
 * ------------------------------------------------------------------------- */

CTX *
korb_ctx_new(void)
{
    CTX *c = calloc(1, sizeof(CTX));
    if (!c) { fprintf(stderr, "koruby_precise: out of memory (CTX)\n"); abort(); }

    /* slots: virtual reservation, lazy commit, guard page at the end
     * (v2_design §3.5).  The buffer address is fixed for the CTX lifetime. */
    size_t bytes = (size_t)8 << 20;                 /* 8 MiB default */
    const char *env = getenv("KORUBY_SLOTS_BYTES");
    if (env && *env) {
        long long req = atoll(env);
        if (req > 0) bytes = (size_t)req;
    }
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    bytes = (bytes + page - 1) & ~(page - 1);

    char *base = mmap(NULL, bytes + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("koruby_precise: mmap slots"); abort(); }
    if (mprotect(base + bytes, page, PROT_NONE) != 0) {
        perror("koruby_precise: mprotect guard");
        abort();
    }

    c->slots = (VALUE *)base;
    c->slots_top = c->slots;
    c->slots_limit = (VALUE *)(base + bytes);
    c->slots_high_water = c->slots;

    /* Native C-stack floor: the AST walker recurses on the C stack, which
     * overflows long before the slots reservation.  Margin must cover one
     * deepest expression chain + the raise/unwind path. */
    {
        pthread_attr_t attr;
        void *stack_addr = NULL;
        size_t stack_size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            pthread_attr_getstack(&attr, &stack_addr, &stack_size);
            pthread_attr_destroy(&attr);
        }
        if (stack_addr && stack_size > 0) {
            size_t margin = stack_size / 8;
            if (margin < (size_t)512 << 10) margin = (size_t)512 << 10;
            c->cstack_limit = (const char *)stack_addr + margin;
        }
        else {
            char here;
            c->cstack_limit = &here - ((size_t)6 << 20);   /* fallback: ~6 MiB below */
        }
    }

    c->vm = calloc(1, sizeof(struct korb_vm));
    if (!c->vm) { fprintf(stderr, "koruby_precise: out of memory (VM)\n"); abort(); }

    aro_gc_init(c);

    korb_builtin_define(c, "puts",  korb_bi_puts,  -1);
    korb_builtin_define(c, "p",     korb_bi_p,     -1);
    korb_builtin_define(c, "print", korb_bi_print, -1);
    korb_builtin_define(c, "raise", korb_bi_raise, -1);

    korb_register_core_methods(c);

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}
