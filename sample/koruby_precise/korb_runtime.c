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

RESULT
korb_str_new(CTX *c, VALUE *slots, const char *bytes, uint32_t len)
{
    KorbString *s = korb_alloc(c, slots, sizeof(KorbString) + len + 1, KORB_OBJ_STRING);
    s->len = len;
    memcpy(s->bytes, bytes, len);
    s->bytes[len] = '\0';
    return RESULT_OK((VALUE)s);
}

/* a + b — alloc first, then copy through refs (fixup-safe; v2_design §4.3). */
static RESULT
korb_str_plus_ref(CTX *c, VALUE *slots, VALUE_REF a, VALUE_REF b)
{
    uint32_t alen = VAL2STR(VALUE_REF_GET(a))->len;
    uint32_t blen = VAL2STR(VALUE_REF_GET(b))->len;
    KorbString *s = korb_alloc(c, slots, sizeof(KorbString) + (size_t)alen + blen + 1,
                               KORB_OBJ_STRING);
    const KorbString *as = VAL2STR(VALUE_REF_GET(a));   /* re-read: fixed up */
    const KorbString *bs = VAL2STR(VALUE_REF_GET(b));
    memcpy(s->bytes, as->bytes, alen);
    memcpy(s->bytes + alen, bs->bytes, blen);
    s->len = alen + blen;
    s->bytes[s->len] = '\0';
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
    KorbString *s = korb_alloc(c, slots, sizeof(KorbString) + total + 1, KORB_OBJ_STRING);
    const KorbString *ss = VAL2STR(VALUE_REF_GET(src));
    for (intptr_t i = 0; i < cnt; i++) {
        memcpy(s->bytes + (size_t)i * len, ss->bytes, len);
    }
    s->len = (uint32_t)total;
    s->bytes[s->len] = '\0';
    return RESULT_OK((VALUE)s);
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
    }
    return "an instance of Object";
}

/* ---------------------------------------------------------------------------
 * Equality / comparison.
 * ------------------------------------------------------------------------- */

bool
korb_value_eq(VALUE a, VALUE b)
{
    if (a == b) return true;    /* fixnum / symbol / singletons / identity */
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        return x->len == y->len && memcmp(x->bytes, y->bytes, x->len) == 0;
    }
    return false;
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

RESULT
korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line)
{
    if (KORB_STRING_P(l) && KORB_STRING_P(r)) {
        const KorbString *x = VAL2STR(l), *y = VAL2STR(r);
        uint32_t min = x->len < y->len ? x->len : y->len;
        int cmp = memcmp(x->bytes, y->bytes, min);
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

RESULT
korb_mul_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_STRING_P(l) && FIXNUM_P(rhs))
        return korb_str_repeat_ref(c, slots, lhs, FIX2LONG(rhs), line);
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
    const char *msg = (e->msg != KORB_NIL) ? VAL2STR(e->msg)->bytes : cls;

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

/* Shared call path.  `block` (a node_entry NODE) + `def_env` are NULL for an
 * ordinary call; for a call with a literal block they are written into the
 * callee frame's top 2 cells (odd-tagged so the GC root scan skips them) when
 * the callee reserves them (m->uses_block).  A block passed to a method that
 * does not yield is simply ignored. */
static RESULT
korb_call_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
               struct korb_callcache *cc, uint32_t argc,
               NODE *block, VALUE *def_env)
{
    struct korb_vm *const vm = c->vm;
    struct korb_method *m = cc->m;
    if (UNLIKELY(cc->serial != vm->method_serial)) {
        m = korb_method_lookup(vm, mid);
        if (UNLIKELY(m == NULL)) {
            return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                              "undefined method '%s' for main", korb_sym_name(vm, mid));
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

    if (UNLIKELY((uint32_t)m->params_cnt != argc)) {
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %d)",
                          argc, m->params_cnt);
    }

    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    }
    if (locals_cnt > argc) {
        memset(base + argc, 0, (locals_cnt - argc) * sizeof(VALUE));  /* nil-init */
    }
    /* Block cells at the frame top (base[fs-2], base[fs-1]); only for methods
     * that reserve them.  No block → nil from the nil-init above (= no block). */
    if (block != NULL && m->uses_block) {
        base[locals_cnt - 2] = (VALUE)((uintptr_t)block   | 1u);
        base[locals_cnt - 1] = (VALUE)((uintptr_t)def_env | 1u);
    }

    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) {
        r.state = KORB_NORMAL;
    }
    else if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, m->mid));
        e->line = line;
    }
    return r;
}

RESULT
korb_call(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
          struct korb_callcache *cc, uint32_t argc)
{
    return korb_call_impl(c, slots, mid, line, cc, argc, NULL, NULL);
}

RESULT
korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              struct korb_callcache *cc, uint32_t argc, NODE *block, VALUE *def_env)
{
    return korb_call_impl(c, slots, mid, line, cc, argc, block, def_env);
}

/* ---- node_entry accessors + yield ----------------------------------------- */

uint32_t korb_entry_params_cnt(NODE *entry) { return entry->u.node_entry.params_cnt; }
uint32_t korb_entry_locals_cnt(NODE *entry) { return entry->u.node_entry.locals_cnt; }
NODE    *korb_entry_body(NODE *entry)       { return entry->u.node_entry.body; }

RESULT
korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
           VALUE block_cell, VALUE def_env_cell)
{
    /* Frame-top cells are odd-tagged when a block is present; nil (0) = none. */
    if (UNLIKELY(((uintptr_t)block_cell & 1u) == 0)) {
        return korb_raise(c, slots, KORB_E_LOCALJUMP, line, "no block given (yield)");
    }
    NODE  *entry   = (NODE  *)(uintptr_t)((uintptr_t)block_cell   & ~(uintptr_t)1u);
    VALUE *def_env = (VALUE *)(uintptr_t)((uintptr_t)def_env_cell & ~(uintptr_t)1u);
    const uint32_t blocals = korb_entry_locals_cnt(entry);

    /* block frame: bf[0]=PREV(def_env, tagged), bf[1..1+blocals)=block locals. */
    VALUE *const bf = slots;
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    }
    bf[0] = (VALUE)((uintptr_t)def_env | 1u);
    /* yield args → block params: extra args dropped, missing params nil
     * (CRuby semantics).  np <= blocals (params are a prefix of locals), so
     * this never writes past the block frame. */
    const uint32_t np = korb_entry_params_cnt(entry);
    for (uint32_t i = 0; i < np; i++) {
        bf[1 + i] = (i < argc) ? slots[(intptr_t)i - (intptr_t)argc] : KORB_NIL;
    }
    for (uint32_t i = np; i < blocals; i++) {
        bf[1 + i] = KORB_NIL;
    }

    RESULT r = (*entry->head.dispatcher)(c, entry, bf + 1 + blocals);
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;   /* `next [v]` = yield value */
    return r;
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
    m->mid = mid; m->fn = fn; m->arity = arity;
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

RESULT
korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc)
{
    struct korb_vm *const vm = c->vm;
    VALUE *const recv_slot = &slots[-(intptr_t)argc - 1];
    VALUE self = *recv_slot;
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
    RESULT r = m->fn(c, slots, VALUE_REF_AT(recv_slot),
                     VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc));
    if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    return r;
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

/* ---- String methods ------------------------------------------------------ */

#define SELF_STR  VAL2STR(VALUE_REF_GET(self))
static RESULT korb_m_str_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_STR->len)); }
static RESULT korb_m_str_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_STR->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_str_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_str_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbString *s = SELF_STR;
    return RESULT_OK(ID2SYM(korb_intern(c->vm, s->bytes, s->len)));
}
/* transform-into-new-string helper (op: 0=upcase 1=downcase 2=capitalize 3=reverse) */
static RESULT korb_str_transform(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_alloc(c, slots, sizeof(KorbString) + len + 1, KORB_OBJ_STRING);
    const KorbString *s = SELF_STR;   /* re-read after alloc (GC may have moved it) */
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->bytes[i];
        unsigned char out;
        switch (op) {
          case 0: out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch; break;
          case 1: out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch; break;
          case 2:
            if (i == 0) out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch;
            else        out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch;
            break;
          default: out = (unsigned char)s->bytes[len - 1 - i]; break;
        }
        r->bytes[i] = (char)out;
    }
    r->len = len; r->bytes[len] = '\0';
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_upcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_str_transform(c, slots, self, 0); }
static RESULT korb_m_str_downcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform(c, slots, self, 1); }
static RESULT korb_m_str_capitalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_transform(c, slots, self, 2); }
static RESULT korb_m_str_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)a; return korb_str_transform(c, slots, self, 3); }

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

/* ---- Array methods ------------------------------------------------------- */

#define SELF_ARY  VAL2ARY(VALUE_REF_GET(self))
static RESULT korb_m_ary_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_ARY->len)); }
static RESULT korb_m_ary_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_ARY->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_ary_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_ary_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[0] : KORB_NIL); }
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[ary->len - 1] : KORB_NIL); }

static RESULT korb_m_ary_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbArray *ary = SELF_ARY;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(iv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    intptr_t i = FIX2LONG(iv);
    if (i < 0) i += ary->len;
    if (i < 0 || (uint32_t)i >= ary->len) return RESULT_OK(KORB_NIL);
    return RESULT_OK(ary->items->data[i]);
}

static RESULT korb_m_ary_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbArray *ary = SELF_ARY;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(iv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    intptr_t i = FIX2LONG(iv);
    if (i < 0) i += ary->len;
    if (UNLIKELY(i < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array; minimum: -%u", (long)FIX2LONG(iv), ary->len);
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
#undef SELF_ARY

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
    korb_def_cmethod(c, KORB_C_INTEGER, "even?", korb_m_int_even, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "odd?", korb_m_int_odd, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "positive?", korb_m_int_pos, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "negative?", korb_m_int_neg, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_i", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_int", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "ord", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "integer?", korb_m_true_lit, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_s", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "inspect", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "chr", korb_m_int_chr, 0);

    /* String */
    korb_def_cmethod(c, KORB_C_STRING, "length", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "size", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "bytesize", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "empty?", korb_m_str_empty, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_s", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_str", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_sym", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "intern", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase", korb_m_str_upcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "downcase", korb_m_str_downcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize", korb_m_str_capitalize, 0);
    korb_def_cmethod(c, KORB_C_STRING, "reverse", korb_m_str_reverse, 0);

    /* Symbol */
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_s", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "id2name", korb_m_sym_to_s, 0);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "first", korb_m_ary_first, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "last", korb_m_ary_last, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]", korb_m_ary_aref, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]=", korb_m_ary_aset, 2);
    korb_def_cmethod(c, KORB_C_ARRAY, "<<", korb_m_ary_ltlt, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "push", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "append", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "pop", korb_m_ary_pop, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "include?", korb_m_ary_include, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse", korb_m_ary_reverse, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "+", korb_m_ary_plus, 1);

    /* Object (universal fallback) */
    korb_def_cmethod(c, KORB_C_OBJECT, "nil?", korb_m_obj_nil_q, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "==", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "!=", korb_m_obj_neq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "equal?", korb_m_obj_equal, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "eql?", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "itself", korb_m_obj_itself, 0);
}

/* ---------------------------------------------------------------------------
 * Printing (CRuby-compatible to_s / inspect, written directly — no GC).
 * ------------------------------------------------------------------------- */

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
        fwrite(s->bytes, 1, s->len, fp);
        return;
      }
      case KORB_OBJ_ARRAY:
        korb_fprint_ary(c, fp, v);
        return;
      case KORB_OBJ_EXCEPTION: {
        const KorbException *e = VAL2EXC(v);
        if (e->msg != KORB_NIL) fwrite(VAL2STR(e->msg)->bytes, 1, VAL2STR(e->msg)->len, fp);
        else fputs(korb_etype_name(e->etype), fp);
        return;
      }
    }
    fputs("#<Object>", fp);
}

static void
korb_fprint_str_inspect(FILE *fp, const KorbString *s)
{
    fputc('"', fp);
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->bytes[i];
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
            /* CRuby escapes # only before { $ @ */
            if (i + 1 < s->len && (s->bytes[i+1] == '{' || s->bytes[i+1] == '$' || s->bytes[i+1] == '@'))
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

void
korb_fprint_inspect(CTX *c, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))     { fprintf(fp, "%ld", (long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)   { fputs("nil", fp); return; }
    if (v == KORB_TRUE)  { fputs("true", fp); return; }
    if (v == KORB_FALSE) { fputs("false", fp); return; }
    if (SYMBOL_P(v))     { fprintf(fp, ":%s", korb_sym_name(c->vm, SYM2ID(v))); return; }
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING:
        korb_fprint_str_inspect(fp, VAL2STR(v));
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
        fwrite(s->bytes, 1, s->len, stdout);
        if (s->len == 0 || s->bytes[s->len - 1] != '\n') fputc('\n', stdout);
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

    korb_register_core_methods(c);

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}
