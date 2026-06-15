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
 * Printing (CRuby-compatible to_s / inspect, written directly — no GC).
 * ------------------------------------------------------------------------- */

void
korb_fprint_to_s(CTX *c, FILE *fp, VALUE v)
{
    (void)c;
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

static RESULT
korb_bi_puts(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) {
        fputc('\n', stdout);
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = 0; i < n; i++) {
        VALUE v = VALUE_SLICE_GET(args, i);
        if (KORB_STRING_P(v)) {
            const KorbString *s = VAL2STR(v);
            fwrite(s->bytes, 1, s->len, stdout);
            if (s->len == 0 || s->bytes[s->len - 1] != '\n') fputc('\n', stdout);
        }
        else {
            korb_fprint_to_s(c, stdout, v);
            fputc('\n', stdout);
        }
    }
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

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}
