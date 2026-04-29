// ascheme — R5RS Scheme on ASTro.
//
// Layout: this single TU bundles the runtime (heap, primitives,
// reader, error handler) and the front-end (compiler from s-expr →
// AST + main driver).  Generated dispatchers live in node_*.c (built
// from node.def by ASTroGen) and are pulled in through node.c.

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct ascheme_option OPTION;

// ---------------------------------------------------------------------------
// Singleton heap objects (immediates).
// ---------------------------------------------------------------------------

struct sobj S_NIL_OBJ    = { .type = OBJ_NIL };
struct sobj S_TRUE_OBJ   = { .type = OBJ_BOOL, .b = true };
struct sobj S_FALSE_OBJ  = { .type = OBJ_BOOL, .b = false };
struct sobj S_UNSPEC_OBJ = { .type = OBJ_UNSPEC };
struct sobj S_EOF_OBJ    = { .type = OBJ_EOF };

// ---------------------------------------------------------------------------
// Heap.  All allocations go through Boehm GC (libgc); we never free.  The
// conservative collector scans data segments and the C stack, so values
// live in C locals and arrays are kept alive without manual root tracking.
// `mp_set_memory_functions` redirects GMP's internal allocations through
// GC_malloc as well, so mpz_t / mpq_t are reclaimed naturally.
// ---------------------------------------------------------------------------

static void *
gmp_alloc(size_t sz)            { return GC_malloc(sz); }
static void *
gmp_realloc(void *p, size_t old, size_t nw) { (void)old; return GC_realloc(p, nw); }
static void
gmp_free(void *p, size_t sz)    { (void)p; (void)sz; /* GC sweeps */ }

static void
scm_gc_init(void)
{
    GC_init();
    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
}

struct sobj *
scm_alloc(int type)
{
    struct sobj *o = (struct sobj *)GC_malloc(sizeof(struct sobj));
    o->type = type;
    return o;
}

VALUE
scm_cons(VALUE a, VALUE d)
{
    // Allocate exactly the bytes a pair needs (type + offset + car +
    // cdr) rather than the full sizeof(struct sobj).  Boehm GC blocks
    // are bucketed in fixed sizes — for a 24-byte request it picks a
    // smaller bucket than for the 48-byte struct.  list-heavy
    // benchmarks (cons in a tight loop) shrink dramatically as a
    // result.  The cast is sound because we never access fields past
    // `pair` for objects of type OBJ_PAIR.
    static const size_t pair_size = offsetof(struct sobj, pair) +
                                    sizeof(((struct sobj *)0)->pair);
    struct sobj *o = (struct sobj *)GC_malloc(pair_size);
    o->type = OBJ_PAIR;
    o->pair.car = a;
    o->pair.cdr = d;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_string(const char *s, size_t len)
{
    struct sobj *o = scm_alloc(OBJ_STRING);
    o->str.chars = (char *)GC_malloc_atomic(len + 1);
    memcpy(o->str.chars, s, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_string_n(size_t len, char fill)
{
    struct sobj *o = scm_alloc(OBJ_STRING);
    o->str.chars = (char *)GC_malloc_atomic(len + 1);
    memset(o->str.chars, fill, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_char(uint32_t cp)
{
    struct sobj *o = scm_alloc(OBJ_CHAR);
    o->ch = cp;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_vector(size_t len, VALUE fill)
{
    struct sobj *o = scm_alloc(OBJ_VECTOR);
    o->vec.items = (VALUE *)GC_malloc(sizeof(VALUE) * (len ? len : 1));
    o->vec.len = len;
    for (size_t i = 0; i < len; i++) o->vec.items[i] = fill;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_double(double d)
{
    // Try Ruby's inline flonum encoding first; falls through to a heap
    // OBJ_DOUBLE only for 0.0 / NaN / ±inf / |d| outside ~[1e-77, 1e+77].
    VALUE v = scm_try_flonum(d);
    if (LIKELY(v != 0)) return v;
    struct sobj *o = scm_alloc(OBJ_DOUBLE);
    o->dbl = d;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_bignum_z(mpz_srcptr z)
{
    struct sobj *o = scm_alloc(OBJ_BIGNUM);
    mpz_init_set(o->mpz, z);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_normalize_int(mpz_srcptr z)
{
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (v >= SCM_FIXNUM_MIN && v <= SCM_FIXNUM_MAX) return SCM_FIX(v);
    }
    return scm_make_bignum_z(z);
}

VALUE
scm_make_rational_q(mpq_srcptr q)
{
    // If denominator is 1, return integer.
    if (mpz_cmp_ui(mpq_denref(q), 1) == 0) {
        return scm_normalize_int(mpq_numref(q));
    }
    struct sobj *o = scm_alloc(OBJ_RATIONAL);
    mpq_init(o->mpq);
    mpq_set(o->mpq, q);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_rational_zz(mpz_srcptr num, mpz_srcptr den)
{
    mpq_t q;
    mpq_init(q);
    mpz_set(mpq_numref(q), num);
    mpz_set(mpq_denref(q), den);
    mpq_canonicalize(q);
    VALUE r = scm_make_rational_q(q);
    mpq_clear(q);
    return r;
}

VALUE
scm_normalize_rat(mpq_t q)
{
    mpq_canonicalize(q);
    return scm_make_rational_q(q);
}

VALUE
scm_make_complex(double re, double im)
{
    struct sobj *o = scm_alloc(OBJ_COMPLEX);
    o->cpx.re = re;
    o->cpx.im = im;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_simplify_complex(double re, double im)
{
    if (im == 0.0) return scm_make_double(re);
    return scm_make_complex(re, im);
}

VALUE
scm_make_mvalues(int count, VALUE *items)
{
    struct sobj *o = scm_alloc(OBJ_MVALUES);
    o->mv.items = (VALUE *)GC_malloc(sizeof(VALUE) * (count ? count : 1));
    o->mv.len = (size_t)count;
    for (int i = 0; i < count; i++) o->mv.items[i] = items[i];
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_int(int64_t v)
{
    if (v >= SCM_FIXNUM_MIN && v <= SCM_FIXNUM_MAX) return SCM_FIX(v);
    mpz_t z; mpz_init(z);
    // mpz_set_si only takes long; for full int64_t, build via string or two halves.
#if LONG_MAX >= INT64_MAX
    mpz_set_si(z, (long)v);
#else
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    mpz_set_str(z, buf, 10);
#endif
    VALUE r = scm_make_bignum_z(z);
    mpz_clear(z);
    return r;
}

VALUE
scm_make_closure(NODE *body, struct sframe *env, int nparams, int has_rest)
{
    struct sobj *o = scm_alloc(OBJ_CLOSURE);
    o->closure.body = body;
    o->closure.env = env;
    o->closure.nparams = nparams;
    o->closure.has_rest = has_rest;
    o->closure.name = NULL;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_prim(const char *name, scm_prim_fn fn, int min_argc, int max_argc)
{
    struct sobj *o = scm_alloc(OBJ_PRIM);
    o->prim.name = name;
    o->prim.fn = fn;
    o->prim.min_argc = min_argc;
    o->prim.max_argc = max_argc;
    return SCM_OBJ_VAL(o);
}

double
scm_get_double(VALUE v)
{
    if (SCM_IS_FIXNUM(v))    return (double)SCM_FIXVAL(v);
    if (SCM_IS_FLONUM(v))    return scm_flonum_to_double(v);
    if (scm_is_heap_double(v)) return SCM_PTR(v)->dbl;
    if (scm_is_bignum(v))    return mpz_get_d(SCM_PTR(v)->mpz);
    if (scm_is_rational(v))  return mpq_get_d(SCM_PTR(v)->mpq);
    if (scm_is_complex(v))   return SCM_PTR(v)->cpx.re;
    return 0.0;
}

bool
scm_is_integer_value(VALUE v)
{
    if (SCM_IS_FIXNUM(v) || scm_is_bignum(v)) return true;
    if (scm_is_double(v)) {
        double d = scm_get_double(v);
        return d == (double)(int64_t)d;
    }
    if (scm_is_rational(v)) {
        return mpz_cmp_ui(mpq_denref(SCM_PTR(v)->mpq), 1) == 0;
    }
    return false;
}

struct sframe *
scm_new_frame(struct sframe *parent, int nslots)
{
    struct sframe *f = (struct sframe *)GC_malloc(sizeof(struct sframe) + sizeof(VALUE) * (nslots ? nslots : 1));
    f->parent = parent;
    f->nslots = nslots;
    for (int i = 0; i < nslots; i++) f->slots[i] = SCM_UNSPEC;
    return f;
}

// ---------------------------------------------------------------------------
// Symbol interning.  Linear table — fine for the source sizes we care
// about (tens of thousands of unique symbols at most).
// ---------------------------------------------------------------------------

static struct sobj **SYMBOL_TABLE = NULL;
static size_t SYMBOL_TABLE_LEN = 0;
static size_t SYMBOL_TABLE_CAP = 0;

VALUE
scm_intern(const char *name)
{
    for (size_t i = 0; i < SYMBOL_TABLE_LEN; i++) {
        if (strcmp(SYMBOL_TABLE[i]->sym.name, name) == 0) {
            return SCM_OBJ_VAL(SYMBOL_TABLE[i]);
        }
    }
    if (SYMBOL_TABLE_LEN == SYMBOL_TABLE_CAP) {
        SYMBOL_TABLE_CAP = SYMBOL_TABLE_CAP ? SYMBOL_TABLE_CAP * 2 : 64;
        SYMBOL_TABLE = (struct sobj **)GC_realloc(SYMBOL_TABLE,
                                                  sizeof(struct sobj *) * SYMBOL_TABLE_CAP);
    }
    struct sobj *o = scm_alloc(OBJ_SYMBOL);
    size_t nlen = strlen(name);
    o->sym.name = (char *)GC_malloc_atomic(nlen + 1);
    memcpy(o->sym.name, name, nlen + 1);
    SYMBOL_TABLE[SYMBOL_TABLE_LEN++] = o;
    return SCM_OBJ_VAL(o);
}

// ---------------------------------------------------------------------------
// Global definitions.  Linear array; lookups are linear but cheap given
// typical R5RS workloads.  Symbols are interned strings (`name` is the
// pointer returned by `scm_intern(...)->sym.name`), so we compare by
// strcmp here for safety with literal C-string lookups.
// ---------------------------------------------------------------------------

void
scm_global_define(CTX *c, const char *name, VALUE v)
{
    c->globals_serial++;
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            c->globals[i].value = v;
            c->globals[i].defined = true;
            return;
        }
    }
    if (c->globals_size == c->globals_capa) {
        c->globals_capa = c->globals_capa ? c->globals_capa * 2 : 256;
        c->globals = (struct gentry *)GC_realloc(c->globals,
                                                 sizeof(struct gentry) * c->globals_capa);
    }
    size_t nlen = strlen(name);
    c->globals[c->globals_size].name = (char *)GC_malloc_atomic(nlen + 1);
    memcpy((char *)c->globals[c->globals_size].name, name, nlen + 1);
    c->globals[c->globals_size].value = v;
    c->globals[c->globals_size].defined = true;
    c->globals_size++;
}

VALUE
scm_global_ref(CTX *c, const char *name)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            if (!c->globals[i].defined) {
                scm_error(c, "unbound variable: %s", name);
            }
            return c->globals[i].value;
        }
    }
    scm_error(c, "unbound variable: %s", name);
}

void
scm_global_set(CTX *c, const char *name, VALUE v)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            c->globals[i].value = v;
            c->globals[i].defined = true;
            c->globals_serial++;
            return;
        }
    }
    scm_error(c, "set! on unbound variable: %s", name);
}

// ---------------------------------------------------------------------------
// Errors — longjmp back to the REPL/script driver if an error handler
// is installed; otherwise print and exit.
// ---------------------------------------------------------------------------

static char SCM_ERR_MSG[1024];

void
scm_error(CTX *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(SCM_ERR_MSG, sizeof(SCM_ERR_MSG), fmt, ap);
    va_end(ap);
    if (c && c->err_jmp_active) {
        longjmp(c->err_jmp, 1);
    }
    fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
    exit(1);
}

// ---------------------------------------------------------------------------
// Display / write.
// ---------------------------------------------------------------------------

static void scm_display1(FILE *fp, VALUE v, bool readable);

static void
write_string(FILE *fp, const char *s, size_t len, bool readable)
{
    if (!readable) {
        fwrite(s, 1, len, fp);
        return;
    }
    fputc('"', fp);
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        switch (ch) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\t': fputs("\\t", fp); break;
        case '\r': fputs("\\r", fp); break;
        default:   fputc(ch, fp);
        }
    }
    fputc('"', fp);
}

static void
write_char(FILE *fp, uint32_t cp, bool readable)
{
    if (!readable) {
        if (cp < 128) fputc((int)cp, fp);
        else fprintf(fp, "?");   // simplified: no UTF-8 encoding
        return;
    }
    switch (cp) {
    case ' ':  fputs("#\\space", fp);   return;
    case '\n': fputs("#\\newline", fp); return;
    case '\t': fputs("#\\tab", fp);     return;
    case 0:    fputs("#\\nul", fp);     return;
    }
    if (cp >= 32 && cp < 127) fprintf(fp, "#\\%c", (int)cp);
    else fprintf(fp, "#\\x%x", cp);
}

static void
write_pair(FILE *fp, VALUE v, bool readable)
{
    fputc('(', fp);
    bool first = true;
    while (scm_is_pair(v)) {
        if (!first) fputc(' ', fp);
        first = false;
        scm_display1(fp, SCM_PTR(v)->pair.car, readable);
        v = SCM_PTR(v)->pair.cdr;
    }
    if (v != SCM_NIL) {
        fputs(" . ", fp);
        scm_display1(fp, v, readable);
    }
    fputc(')', fp);
}

static void
scm_display1(FILE *fp, VALUE v, bool readable)
{
    if (SCM_IS_FIXNUM(v)) { fprintf(fp, "%lld", (long long)SCM_FIXVAL(v)); return; }
    if (SCM_IS_FLONUM(v)) {
        double d = scm_flonum_to_double(v);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", d);
        fputs(buf, fp);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
            fputs(".0", fp);
        }
        return;
    }
    struct sobj *o = SCM_PTR(v);
    if (!o) { fputs("#<NULL>", fp); return; }
    switch (o->type) {
    case OBJ_NIL:    fputs("()", fp);             break;
    case OBJ_BOOL:   fputs(o->b ? "#t" : "#f", fp); break;
    case OBJ_UNSPEC: fputs("", fp);               break;
    case OBJ_EOF:    fputs("#<eof>", fp);         break;
    case OBJ_PAIR:   write_pair(fp, v, readable); break;
    case OBJ_SYMBOL: fputs(o->sym.name, fp);      break;
    case OBJ_STRING: write_string(fp, o->str.chars, o->str.len, readable); break;
    case OBJ_CHAR:   write_char(fp, o->ch, readable); break;
    case OBJ_VECTOR: {
        fputs("#(", fp);
        for (size_t i = 0; i < o->vec.len; i++) {
            if (i) fputc(' ', fp);
            scm_display1(fp, o->vec.items[i], readable);
        }
        fputc(')', fp);
        break;
    }
    case OBJ_DOUBLE: {
        double d = o->dbl;
        if (isnan(d)) { fputs("+nan.0", fp); break; }
        if (isinf(d)) { fputs(d > 0 ? "+inf.0" : "-inf.0", fp); break; }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", d);
        fputs(buf, fp);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
            fputs(".0", fp);
        }
        break;
    }
    case OBJ_BIGNUM: {
        char *s = mpz_get_str(NULL, 10, o->mpz);
        fputs(s, fp);
        // s allocated via GMP's allocator (= GC_malloc); GC reclaims it.
        break;
    }
    case OBJ_RATIONAL: {
        char *s = mpq_get_str(NULL, 10, o->mpq);
        fputs(s, fp);
        break;
    }
    case OBJ_COMPLEX: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", o->cpx.re); fputs(buf, fp);
        if (o->cpx.im >= 0) fputc('+', fp);
        snprintf(buf, sizeof(buf), "%.15g", o->cpx.im); fputs(buf, fp);
        fputc('i', fp);
        break;
    }
    case OBJ_MVALUES: {
        // R5RS leaves the printed form unspecified; we emit each value
        // separated by newlines, matching Gauche's REPL convention.
        for (size_t i = 0; i < o->mv.len; i++) {
            if (i) fputc('\n', fp);
            scm_display1(fp, o->mv.items[i], readable);
        }
        break;
    }
    case OBJ_CLOSURE: fprintf(fp, "#<procedure %s>", o->closure.name ? o->closure.name : "anon"); break;
    case OBJ_PRIM:    fprintf(fp, "#<primitive %s>", o->prim.name); break;
    case OBJ_CONT:    fputs("#<continuation>", fp); break;
    case OBJ_PORT:    fputs("#<port>", fp); break;
    default:          fputs("#<?>", fp); break;
    }
}

void
scm_display(FILE *fp, VALUE v, bool readable)
{
    scm_display1(fp, v, readable);
}

// ---------------------------------------------------------------------------
// S-expression reader.
// ---------------------------------------------------------------------------

struct reader {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    FILE *fp;     // alternative input source (NULL → src/pos)
    int ungot;    // pushed-back char (-2 → none)
};

static int
reader_getc(struct reader *r)
{
    if (r->ungot != -2) { int c = r->ungot; r->ungot = -2; return c; }
    if (r->fp) {
        int c = fgetc(r->fp);
        if (c == '\n') r->line++;
        return c;
    }
    if (r->pos >= r->len) return EOF;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') r->line++;
    return c;
}

static void
reader_ungetc(struct reader *r, int c)
{
    r->ungot = c;
}

static void
reader_skip_ws(struct reader *r)
{
    for (;;) {
        int c = reader_getc(r);
        if (c == EOF) return;
        if (c == ';') {
            while (c != EOF && c != '\n') c = reader_getc(r);
            continue;
        }
        if (isspace(c)) continue;
        reader_ungetc(r, c);
        return;
    }
}

static bool
is_delim(int c)
{
    return c == EOF || isspace(c) || c == '(' || c == ')' ||
           c == '"' || c == ';' || c == '\'' || c == '`' || c == ',';
}

static VALUE read_form(CTX *c, struct reader *r);

static VALUE
read_list(CTX *c, struct reader *r)
{
    reader_skip_ws(r);
    int ch = reader_getc(r);
    if (ch == ')') return SCM_NIL;
    if (ch == EOF) scm_error(c, "unexpected EOF in list");
    reader_ungetc(r, ch);
    VALUE car = read_form(c, r);
    reader_skip_ws(r);
    ch = reader_getc(r);
    if (ch == '.') {
        int next = reader_getc(r);
        if (is_delim(next)) {
            reader_ungetc(r, next);
            VALUE cdr = read_form(c, r);
            reader_skip_ws(r);
            int close = reader_getc(r);
            if (close != ')') scm_error(c, "expected ')' after dotted tail");
            return scm_cons(car, cdr);
        }
        // not a dotted-tail '.', push back both characters and treat as identifier
        reader_ungetc(r, next);
        reader_ungetc(r, '.');
    } else {
        reader_ungetc(r, ch);
    }
    VALUE cdr = read_list(c, r);
    return scm_cons(car, cdr);
}

static VALUE
read_string(CTX *c, struct reader *r)
{
    char buf[8192];
    size_t n = 0;
    for (;;) {
        int ch = reader_getc(r);
        if (ch == EOF) scm_error(c, "unexpected EOF in string");
        if (ch == '"') break;
        if (ch == '\\') {
            int esc = reader_getc(r);
            switch (esc) {
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case 'r':  ch = '\r'; break;
            case '\\': ch = '\\'; break;
            case '"':  ch = '"';  break;
            case '0':  ch = '\0'; break;
            default:   ch = esc;  break;
            }
        }
        if (n + 1 >= sizeof(buf)) scm_error(c, "string literal too long");
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';
    return scm_make_string(buf, n);
}

static VALUE
read_hash(CTX *c, struct reader *r)
{
    int ch = reader_getc(r);
    if (ch == 't') return SCM_TRUE;
    if (ch == 'f') return SCM_FALSE;
    if (ch == '(') {
        // vector
        VALUE list = read_list(c, r);
        size_t len = 0;
        for (VALUE p = list; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) len++;
        VALUE vec = scm_make_vector(len, SCM_UNSPEC);
        size_t i = 0;
        for (VALUE p = list; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr, i++)
            SCM_PTR(vec)->vec.items[i] = SCM_PTR(p)->pair.car;
        return vec;
    }
    if (ch == '\\') {
        // character
        int first = reader_getc(r);
        if (first == EOF) scm_error(c, "EOF in #\\");
        // Read identifier-like name if first is alpha and next isn't delim.
        char buf[32];
        size_t n = 0;
        buf[n++] = (char)first;
        int next = reader_getc(r);
        if (isalpha(first) && !is_delim(next)) {
            buf[n++] = (char)next;
            for (;;) {
                int e = reader_getc(r);
                if (is_delim(e)) { reader_ungetc(r, e); break; }
                if (n + 1 >= sizeof(buf)) scm_error(c, "char name too long");
                buf[n++] = (char)e;
            }
            buf[n] = '\0';
            if (strcmp(buf, "space")   == 0) return scm_make_char(' ');
            if (strcmp(buf, "newline") == 0) return scm_make_char('\n');
            if (strcmp(buf, "tab")     == 0) return scm_make_char('\t');
            if (strcmp(buf, "return")  == 0) return scm_make_char('\r');
            if (strcmp(buf, "nul")     == 0) return scm_make_char(0);
            if (strcmp(buf, "null")    == 0) return scm_make_char(0);
            if (strcmp(buf, "delete")  == 0) return scm_make_char(127);
            if (strcmp(buf, "escape")  == 0) return scm_make_char(27);
            scm_error(c, "unknown character name #\\%s", buf);
        }
        reader_ungetc(r, next);
        return scm_make_char((uint32_t)(unsigned char)first);
    }
    scm_error(c, "unsupported # syntax: #%c", ch);
}

static VALUE
read_atom(CTX *c, struct reader *r, int first)
{
    char buf[256];
    size_t n = 0;
    buf[n++] = (char)first;
    for (;;) {
        int ch = reader_getc(r);
        if (is_delim(ch)) { reader_ungetc(r, ch); break; }
        if (n + 1 >= sizeof(buf)) scm_error(c, "atom too long");
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';

    // try parse number
    if (n > 0 && (isdigit((unsigned char)buf[0]) ||
                  ((buf[0] == '-' || buf[0] == '+') && n > 1 && (isdigit((unsigned char)buf[1]) || buf[1] == '.')) ||
                  (buf[0] == '.' && n > 1 && isdigit((unsigned char)buf[1])))) {
        // rational: P/Q (integer over integer)
        char *slash = strchr(buf, '/');
        if (slash) {
            *slash = '\0';
            mpz_t num, den;
            if (mpz_init_set_str(num, buf, 10) == 0) {
                if (mpz_init_set_str(den, slash + 1, 10) == 0 && mpz_sgn(den) != 0) {
                    VALUE rv = scm_make_rational_zz(num, den);
                    mpz_clear(num); mpz_clear(den);
                    return rv;
                }
                mpz_clear(num);
            }
            *slash = '/';   // restore
        }
        // integer: try int64 fast path, then bignum
        char *end;
        long long ll = strtoll(buf, &end, 10);
        if (*end == '\0' && errno != ERANGE) {
            return scm_make_int((int64_t)ll);
        }
        // try mpz parse for large integers
        mpz_t z;
        if (mpz_init_set_str(z, buf, 10) == 0) {
            VALUE rv = scm_normalize_int(z); mpz_clear(z); return rv;
        }
        mpz_clear(z);
        // try double
        double d = strtod(buf, &end);
        if (*end == '\0') return scm_make_double(d);
    }
    return scm_intern(buf);
}

static VALUE
read_form(CTX *c, struct reader *r)
{
    reader_skip_ws(r);
    int ch = reader_getc(r);
    if (ch == EOF) return SCM_EOFV;
    switch (ch) {
    case '(': return read_list(c, r);
    case ')': scm_error(c, "unexpected ')'");
    case '"': return read_string(c, r);
    case '#': return read_hash(c, r);
    case '\'': {
        VALUE v = read_form(c, r);
        return scm_cons(scm_intern("quote"), scm_cons(v, SCM_NIL));
    }
    case '`': {
        VALUE v = read_form(c, r);
        return scm_cons(scm_intern("quasiquote"), scm_cons(v, SCM_NIL));
    }
    case ',': {
        int next = reader_getc(r);
        const char *which = "unquote";
        if (next == '@') which = "unquote-splicing";
        else reader_ungetc(r, next);
        VALUE v = read_form(c, r);
        return scm_cons(scm_intern(which), scm_cons(v, SCM_NIL));
    }
    default:
        return read_atom(c, r, ch);
    }
}

VALUE
scm_read(CTX *c, FILE *fp)
{
    struct reader r = { .fp = fp, .ungot = -2 };
    return read_form(c, &r);
}

// Convenience: read all forms from a string into a (begin ...) list.
static VALUE
scm_read_all_string(CTX *c, const char *src, size_t len)
{
    struct reader r = { .src = src, .len = len, .ungot = -2 };
    VALUE forms = SCM_NIL;
    VALUE *tail = &forms;
    for (;;) {
        reader_skip_ws(&r);
        int ch = reader_getc(&r);
        if (ch == EOF) break;
        reader_ungetc(&r, ch);
        VALUE v = read_form(c, &r);
        if (v == SCM_EOFV) break;
        *tail = scm_cons(v, SCM_NIL);
        tail = &SCM_PTR(*tail)->pair.cdr;
    }
    return forms;
}

// ---------------------------------------------------------------------------
// Compiler — s-expression → AST.  Threads a lex_scope chain so symbols
// resolve to (depth, idx) lref nodes when bound, falling back to gref.
// ---------------------------------------------------------------------------

struct lex_scope {
    struct lex_scope *parent;
    int nslots;
    char **names;   // symbol C-strings (interned)
};

static struct lex_scope *
push_scope(struct lex_scope *parent, int nslots, char **names)
{
    struct lex_scope *s = (struct lex_scope *)GC_malloc(sizeof(*s));
    s->parent = parent;
    s->nslots = nslots;
    s->names = names;
    return s;
}

// Look up `name` in the lex chain.  Returns true and writes (depth,idx)
// if found.
static bool
lex_lookup(struct lex_scope *s, const char *name, uint32_t *depth, uint32_t *idx)
{
    uint32_t d = 0;
    for (; s; s = s->parent) {
        for (int i = 0; i < s->nslots; i++) {
            if (s->names[i] && strcmp(s->names[i], name) == 0) {
                *depth = d; *idx = (uint32_t)i;
                return true;
            }
        }
        d++;
    }
    return false;
}

// Variable-arg call args pool — referenced from node_call_n.
NODE  **ASCHEME_CALL_ARGS = NULL;
uint32_t ASCHEME_CALL_ARGS_CNT = 0;
static uint32_t ASCHEME_CALL_ARGS_CAP = 0;

// AOT-mode entry list: every node_lambda body and every top-level form is
// registered here so `--compile` can specialize the lot in one pass.
static NODE **AOT_ENTRIES = NULL;
static size_t AOT_ENTRIES_LEN = 0;
static size_t AOT_ENTRIES_CAP = 0;

static void
aot_add_entry(NODE *n)
{
    // @noinline roots emit an empty SPECIALIZE body, so we'd just create an
    // .c file with no SD function in it.  Skip them — the body's children
    // (which usually are inlinable) are reached when their parent's SD is
    // generated and recurses via SPECIALIZE().
    if (!n || n->head.flags.no_inline) return;
    if (AOT_ENTRIES_LEN == AOT_ENTRIES_CAP) {
        AOT_ENTRIES_CAP = AOT_ENTRIES_CAP ? AOT_ENTRIES_CAP * 2 : 64;
        AOT_ENTRIES = (NODE **)GC_realloc(AOT_ENTRIES, sizeof(NODE *) * AOT_ENTRIES_CAP);
    }
    AOT_ENTRIES[AOT_ENTRIES_LEN++] = n;
}

static uint32_t
register_call_args(NODE **args, uint32_t cnt)
{
    if (ASCHEME_CALL_ARGS_CNT + cnt > ASCHEME_CALL_ARGS_CAP) {
        uint32_t need = ASCHEME_CALL_ARGS_CNT + cnt;
        uint32_t capa = ASCHEME_CALL_ARGS_CAP ? ASCHEME_CALL_ARGS_CAP : 64;
        while (capa < need) capa *= 2;
        ASCHEME_CALL_ARGS = (NODE **)GC_realloc(ASCHEME_CALL_ARGS,
                                                sizeof(NODE *) * capa);
        ASCHEME_CALL_ARGS_CAP = capa;
    }
    uint32_t base = ASCHEME_CALL_ARGS_CNT;
    for (uint32_t i = 0; i < cnt; i++) ASCHEME_CALL_ARGS[base + i] = args[i];
    ASCHEME_CALL_ARGS_CNT += cnt;
    return base;
}

// Helpers for examining s-exprs.
static bool
is_symbol(VALUE v, const char *name)
{
    if (!scm_is_symbol(v)) return false;
    return strcmp(SCM_PTR(v)->sym.name, name) == 0;
}

static int
list_length(VALUE v)
{
    int n = 0;
    while (scm_is_pair(v)) { n++; v = SCM_PTR(v)->pair.cdr; }
    return n;
}

static VALUE car(VALUE v)  { return SCM_PTR(v)->pair.car; }
static VALUE cdr(VALUE v)  { return SCM_PTR(v)->pair.cdr; }
static VALUE cadr(VALUE v) { return car(cdr(v)); }
static VALUE caddr(VALUE v){ return car(cdr(cdr(v))); }
static VALUE cadddr(VALUE v){return car(cdr(cdr(cdr(v)))); }

static NODE *compile(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail);
static NODE *compile_body(CTX *c, VALUE body, struct lex_scope *scope, bool is_tail);

// Expand `(quasiquote form)` into a constructive expression — `cons`,
// `list`, and `append` calls — that builds the same s-expr at runtime
// while interpolating `(unquote …)` and `(unquote-splicing …)`.  R5RS
// §4.2.6.  Nested quasiquotes increase `depth`; only `(unquote x)` at
// depth 1 evaluates `x`, deeper nests just rebuild the syntactic form.
static VALUE
expand_quasiquote(VALUE form, int depth)
{
    if (!scm_is_pair(form)) {
        // Self-evaluating literal vs. a symbol/vector that needs quoting.
        if (SCM_IS_FIXNUM(form) || scm_is_bool(form) || scm_is_null(form) ||
            scm_is_double(form) || scm_is_string(form) || scm_is_char(form) ||
            scm_is_bignum(form) || scm_is_rational(form))
            return form;
        return scm_cons(scm_intern("quote"), scm_cons(form, SCM_NIL));
    }
    VALUE head = SCM_PTR(form)->pair.car;
    VALUE tail = SCM_PTR(form)->pair.cdr;
    if (scm_is_symbol(head)) {
        const char *name = SCM_PTR(head)->sym.name;
        if (strcmp(name, "unquote") == 0) {
            VALUE inner = scm_is_pair(tail) ? SCM_PTR(tail)->pair.car : SCM_NIL;
            if (depth == 1) return inner;
            return scm_cons(scm_intern("list"),
                     scm_cons(scm_cons(scm_intern("quote"), scm_cons(head, SCM_NIL)),
                              scm_cons(expand_quasiquote(inner, depth - 1), SCM_NIL)));
        }
        if (strcmp(name, "quasiquote") == 0) {
            VALUE inner = scm_is_pair(tail) ? SCM_PTR(tail)->pair.car : SCM_NIL;
            return scm_cons(scm_intern("list"),
                     scm_cons(scm_cons(scm_intern("quote"), scm_cons(head, SCM_NIL)),
                              scm_cons(expand_quasiquote(inner, depth + 1), SCM_NIL)));
        }
    }
    // ,@x at the head splices into the surrounding list at depth 1.
    if (depth == 1 && scm_is_pair(head)) {
        VALUE hh = SCM_PTR(head)->pair.car;
        if (scm_is_symbol(hh) && strcmp(SCM_PTR(hh)->sym.name, "unquote-splicing") == 0) {
            VALUE inner = SCM_PTR(SCM_PTR(head)->pair.cdr)->pair.car;
            return scm_cons(scm_intern("append"),
                     scm_cons(inner,
                              scm_cons(expand_quasiquote(tail, depth), SCM_NIL)));
        }
    }
    return scm_cons(scm_intern("cons"),
             scm_cons(expand_quasiquote(head, depth),
                      scm_cons(expand_quasiquote(tail, depth), SCM_NIL)));
}

// Compile-time helpers used by several special-form lowerings.
static VALUE
gensym_at(const char *base)
{
    static int seq = 0;
    char buf[64]; snprintf(buf, sizeof(buf), "|%s-%d|", base, ++seq);
    return scm_intern(buf);
}

static VALUE
list_append1(VALUE list, VALUE elt)
{
    VALUE r = SCM_NIL, *tail = &r;
    for (VALUE p = list; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        *tail = scm_cons(SCM_PTR(p)->pair.car, SCM_NIL);
        tail = &SCM_PTR(*tail)->pair.cdr;
    }
    *tail = scm_cons(elt, SCM_NIL);
    return r;
}

// (begin a b ... last) → seq-chain.  The last form inherits is_tail.
static NODE *
compile_seq(CTX *c, VALUE forms, struct lex_scope *scope, bool is_tail)
{
    if (!scm_is_pair(forms)) return ALLOC_node_const_unspec();
    if (cdr(forms) == SCM_NIL) {
        return compile(c, car(forms), scope, is_tail);
    }
    NODE *head = compile(c, car(forms), scope, false);
    NODE *rest = compile_seq(c, cdr(forms), scope, is_tail);
    return ALLOC_node_seq(head, rest);
}

// Try to fold `(<op> ...)` into a specialized node.  Returns NULL when no
// specialization applies (parser falls back to a generic call_K).  The
// specialization is safe under R5RS rebinding because each emitted node
// carries an arith_cache that tracks the global at its install_prims-time
// snapshot; see node.def for the runtime check.
static NODE *
try_specialize_arith(CTX *c, VALUE fn_form, VALUE args, struct lex_scope *scope)
{
    if (!scm_is_symbol(fn_form)) return NULL;
    int argc = list_length(args);
    const char *name = SCM_PTR(fn_form)->sym.name;
    uint32_t depth, idx;
    if (lex_lookup(scope, name, &depth, &idx)) return NULL;

    // 1-arg specializations: predicates and accessors.
    if (argc == 1) {
        NODE *a = compile(c, car(args), scope, false);
        if (strcmp(name, "null?") == 0) return ALLOC_node_pred_null(a);
        if (strcmp(name, "pair?") == 0) return ALLOC_node_pred_pair(a);
        if (strcmp(name, "car")   == 0) return ALLOC_node_pred_car(a);
        if (strcmp(name, "cdr")   == 0) return ALLOC_node_pred_cdr(a);
        if (strcmp(name, "not")   == 0) return ALLOC_node_pred_not(a);
        return NULL;
    }
    if (argc == 2) {
        NODE *a = compile(c, car(args),  scope, false);
        NODE *b = compile(c, cadr(args), scope, false);
        if (strcmp(name, "+")  == 0) return ALLOC_node_arith_add(a, b);
        if (strcmp(name, "-")  == 0) return ALLOC_node_arith_sub(a, b);
        if (strcmp(name, "*")  == 0) return ALLOC_node_arith_mul(a, b);
        if (strcmp(name, "<")  == 0) return ALLOC_node_arith_lt(a, b);
        if (strcmp(name, "<=") == 0) return ALLOC_node_arith_le(a, b);
        if (strcmp(name, ">")  == 0) return ALLOC_node_arith_gt(a, b);
        if (strcmp(name, ">=") == 0) return ALLOC_node_arith_ge(a, b);
        if (strcmp(name, "=")  == 0) return ALLOC_node_arith_eq(a, b);
        if (strcmp(name, "vector-ref") == 0) return ALLOC_node_vec_ref(a, b);
        if (strcmp(name, "cons") == 0)   return ALLOC_node_cons_op(a, b);
        if (strcmp(name, "eq?") == 0)    return ALLOC_node_eq_op(a, b);
        if (strcmp(name, "eqv?") == 0)   return ALLOC_node_eqv_op(a, b);
        return NULL;
    }
    if (argc == 3) {
        NODE *a = compile(c, car(args),  scope, false);
        NODE *b = compile(c, cadr(args), scope, false);
        NODE *d = compile(c, caddr(args), scope, false);
        if (strcmp(name, "vector-set!") == 0) return ALLOC_node_vec_set(a, b, d);
        return NULL;
    }
    return NULL;
}

// Build call node for given fn + args.
static NODE *
compile_call(CTX *c, VALUE fn_form, VALUE args, struct lex_scope *scope, bool is_tail)
{
    NODE *spec = try_specialize_arith(c, fn_form, args, scope);
    if (spec) return spec;

    NODE *fn = compile(c, fn_form, scope, false);
    int argc = list_length(args);
    NODE *aN[8];
    if (argc <= 4) {
        int i = 0;
        for (VALUE p = args; scm_is_pair(p); p = cdr(p), i++) {
            aN[i] = compile(c, car(p), scope, false);
        }
        switch (argc) {
        case 0: return ALLOC_node_call_0((uint32_t)is_tail, fn);
        case 1: return ALLOC_node_call_1((uint32_t)is_tail, fn, aN[0]);
        case 2: return ALLOC_node_call_2((uint32_t)is_tail, fn, aN[0], aN[1]);
        case 3: return ALLOC_node_call_3((uint32_t)is_tail, fn, aN[0], aN[1], aN[2]);
        case 4: return ALLOC_node_call_4((uint32_t)is_tail, fn, aN[0], aN[1], aN[2], aN[3]);
        }
    }
    NODE **abuf = (NODE **)GC_malloc(sizeof(NODE *) * argc);
    int i = 0;
    for (VALUE p = args; scm_is_pair(p); p = cdr(p), i++) {
        abuf[i] = compile(c, car(p), scope, false);
    }
    uint32_t base = register_call_args(abuf, (uint32_t)argc);
    return ALLOC_node_call_n((uint32_t)is_tail, fn, base, (uint32_t)argc);
}

// Build a quoted constant from a (read-only) scheme value.  Symbols/lists/
// vectors get embedded as a `node_quote(uint64_t)` referencing the value's
// pointer; immediates use the typed const nodes for clarity.
static NODE *
compile_quote(VALUE v)
{
    if (SCM_IS_FIXNUM(v)) {
        int64_t n = SCM_FIXVAL(v);
        if (n >= INT32_MIN && n <= INT32_MAX) return ALLOC_node_const_int((int32_t)n);
        return ALLOC_node_const_int64((uint64_t)v);
    }
    if (v == SCM_NIL)    return ALLOC_node_const_nil();
    if (v == SCM_TRUE)   return ALLOC_node_const_bool(1);
    if (v == SCM_FALSE)  return ALLOC_node_const_bool(0);
    if (v == SCM_UNSPEC) return ALLOC_node_const_unspec();
    if (scm_is_symbol(v)) return ALLOC_node_const_sym(SCM_PTR(v)->sym.name);
    if (scm_is_string(v)) return ALLOC_node_const_str(SCM_PTR(v)->str.chars);
    if (scm_is_char(v))   return ALLOC_node_const_char(SCM_PTR(v)->ch);
    if (scm_is_double(v)) return ALLOC_node_const_double(scm_get_double(v));
    return ALLOC_node_quote((uint64_t)v);
}

// Detect leading internal-define forms in a body and rewrite to letrec
// (modeled with `let` over uninitialized slots + `set!` initializers).
// Returns the transformed body list.
static VALUE
hoist_internal_defines(VALUE body)
{
    if (!scm_is_pair(body)) return body;
    // collect leading defines
    VALUE bindings = SCM_NIL, *bind_tail = &bindings;
    while (scm_is_pair(body) && scm_is_pair(car(body)) && is_symbol(car(car(body)), "define")) {
        VALUE def = car(body);
        VALUE name; VALUE init;
        if (scm_is_pair(cadr(def))) {
            // (define (f params) body...)
            name = car(cadr(def));
            VALUE params = cdr(cadr(def));
            VALUE bodydefs = cdr(cdr(def));
            VALUE lambda = scm_cons(scm_intern("lambda"), scm_cons(params, bodydefs));
            init = lambda;
        } else {
            name = cadr(def);
            init = caddr(def);
        }
        VALUE pair = scm_cons(name, scm_cons(init, SCM_NIL));
        *bind_tail = scm_cons(pair, SCM_NIL);
        bind_tail = &SCM_PTR(*bind_tail)->pair.cdr;
        body = cdr(body);
    }
    if (bindings == SCM_NIL) return body;
    VALUE letrec = scm_cons(scm_intern("letrec"), scm_cons(bindings, body));
    return scm_cons(letrec, SCM_NIL);
}

static NODE *
compile_body(CTX *c, VALUE body, struct lex_scope *scope, bool is_tail)
{
    body = hoist_internal_defines(body);
    return compile_seq(c, body, scope, is_tail);
}

// COMPILE_INNER_LAMBDA_SEEN bubbles outward from any `compile_lambda` —
// each enclosing compile_lambda observes whether its (transitive) body
// contained a nested `lambda`.  The flag feeds the closure's `leaf` bit,
// which scm_apply_tail consults to decide whether a self-tail-call can
// reuse the existing frame in place.  Without this gating, a tail call
// that overwrote a frame captured by an inner closure would silently
// corrupt that closure's lexical view.
static bool COMPILE_INNER_LAMBDA_SEEN = false;

// Build a (lambda (params) body...) node.  Handles fixed-arity, dotted
// rest, and the trivial `(lambda x body)` rest-only form.
static NODE *
compile_lambda(CTX *c, VALUE params, VALUE body, struct lex_scope *scope)
{
    int nparams = 0;
    int has_rest = 0;
    char *names_buf[64];
    int nslots = 0;

    if (scm_is_symbol(params)) {
        // (lambda x body) — single rest param
        names_buf[nslots++] = (char *)SCM_PTR(params)->sym.name;
        nparams = 0;
        has_rest = 1;
    } else {
        VALUE p = params;
        while (scm_is_pair(p)) {
            if (nslots >= (int)(sizeof(names_buf)/sizeof(names_buf[0])))
                scm_error(c, "too many lambda parameters");
            names_buf[nslots++] = (char *)SCM_PTR(car(p))->sym.name;
            nparams++;
            p = cdr(p);
        }
        if (scm_is_symbol(p)) {
            names_buf[nslots++] = (char *)SCM_PTR(p)->sym.name;
            has_rest = 1;
        }
    }
    char **names = (char **)GC_malloc(sizeof(char *) * (nslots ? nslots : 1));
    for (int i = 0; i < nslots; i++) names[i] = names_buf[i];

    struct lex_scope *new_scope = push_scope(scope, nslots, names);

    bool saved = COMPILE_INNER_LAMBDA_SEEN;
    COMPILE_INNER_LAMBDA_SEEN = false;
    NODE *body_node = compile_body(c, body, new_scope, true);   // body is in tail position
    bool body_has_inner_lambda = COMPILE_INNER_LAMBDA_SEEN;
    // Bubble the "we are a lambda" flag to the enclosing scope.
    COMPILE_INNER_LAMBDA_SEEN = saved || true;

    aot_add_entry(body_node);
    return ALLOC_node_lambda((uint32_t)nparams, (uint32_t)has_rest, (uint32_t)nslots,
                             body_has_inner_lambda ? 0 : 1,
                             body_node);
}

// (let ((a v) (b w)) body) → ((lambda (a b) body) v w)
static NODE *
compile_let(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE second = cadr(form);
    if (scm_is_symbol(second)) {
        // named let: (let name ((x v) ...) body) → (letrec ((name (lambda (x ...) body))) (name v ...))
        VALUE name = second;
        VALUE bindings = caddr(form);
        VALUE body = cdr(cdr(cdr(form)));
        VALUE params = SCM_NIL, *p_tail = &params;
        VALUE inits  = SCM_NIL, *i_tail = &inits;
        for (VALUE b = bindings; scm_is_pair(b); b = cdr(b)) {
            VALUE bn = car(car(b)), bv = cadr(car(b));
            *p_tail = scm_cons(bn, SCM_NIL); p_tail = &SCM_PTR(*p_tail)->pair.cdr;
            *i_tail = scm_cons(bv, SCM_NIL); i_tail = &SCM_PTR(*i_tail)->pair.cdr;
        }
        VALUE lambda = scm_cons(scm_intern("lambda"), scm_cons(params, body));
        VALUE letrec_binding = scm_cons(name, scm_cons(lambda, SCM_NIL));
        VALUE letrec_bindings = scm_cons(letrec_binding, SCM_NIL);
        VALUE call = scm_cons(name, inits);
        VALUE call_form = scm_cons(call, SCM_NIL);
        VALUE letrec = scm_cons(scm_intern("letrec"), scm_cons(letrec_bindings, call_form));
        return compile(c, letrec, scope, is_tail);
    }
    VALUE bindings = second;
    VALUE body = cdr(cdr(form));
    VALUE params = SCM_NIL, *p_tail = &params;
    VALUE inits  = SCM_NIL, *i_tail = &inits;
    for (VALUE b = bindings; scm_is_pair(b); b = cdr(b)) {
        VALUE bn = car(car(b)), bv = cadr(car(b));
        *p_tail = scm_cons(bn, SCM_NIL); p_tail = &SCM_PTR(*p_tail)->pair.cdr;
        *i_tail = scm_cons(bv, SCM_NIL); i_tail = &SCM_PTR(*i_tail)->pair.cdr;
    }
    VALUE lambda = scm_cons(scm_intern("lambda"), scm_cons(params, body));
    VALUE call = scm_cons(lambda, inits);
    return compile(c, call, scope, is_tail);
}

// (let* ((a v) (b w)) body) → (let ((a v)) (let ((b w)) body))
static NODE *
compile_letstar(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE bindings = cadr(form);
    VALUE body = cdr(cdr(form));
    if (bindings == SCM_NIL) {
        // R5RS §5.2.2: the body of an empty `let*` is still a body, so
        // internal defines must hoist.  Going through `let` keeps that
        // context (compile_let → compile_lambda → compile_body) instead
        // of degenerating to `begin`, which loses it.
        VALUE letform = scm_cons(scm_intern("let"), scm_cons(SCM_NIL, body));
        return compile(c, letform, scope, is_tail);
    }
    VALUE first = car(bindings);
    VALUE rest = cdr(bindings);
    VALUE inner = scm_cons(scm_intern("let*"), scm_cons(rest, body));
    VALUE outer = scm_cons(scm_intern("let"),
                           scm_cons(scm_cons(first, SCM_NIL),
                                    scm_cons(inner, SCM_NIL)));
    return compile(c, outer, scope, is_tail);
}

// (letrec ((a v) ...) body) →
//   (let ((a <unspec>) ...) (set! a v) ... body)
static NODE *
compile_letrec(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE bindings = cadr(form);
    VALUE body = cdr(cdr(form));
    VALUE pairs = SCM_NIL, *p_tail = &pairs;
    VALUE assigns = SCM_NIL, *a_tail = &assigns;
    for (VALUE b = bindings; scm_is_pair(b); b = cdr(b)) {
        VALUE name = car(car(b));
        VALUE init = cadr(car(b));
        VALUE undef = scm_cons(name, scm_cons(SCM_UNSPEC, SCM_NIL));
        *p_tail = scm_cons(undef, SCM_NIL); p_tail = &SCM_PTR(*p_tail)->pair.cdr;
        VALUE setform = scm_cons(scm_intern("set!"), scm_cons(name, scm_cons(init, SCM_NIL)));
        *a_tail = scm_cons(setform, SCM_NIL); a_tail = &SCM_PTR(*a_tail)->pair.cdr;
    }
    // ((let pairs assigns... body...))
    VALUE inner_body = assigns;
    // append body
    if (assigns == SCM_NIL) {
        inner_body = body;
    } else {
        VALUE tail = assigns;
        while (cdr(tail) != SCM_NIL) tail = cdr(tail);
        SCM_PTR(tail)->pair.cdr = body;
    }
    VALUE letform = scm_cons(scm_intern("let"), scm_cons(pairs, inner_body));
    return compile(c, letform, scope, is_tail);
}

// (cond (test e...) ... (else e...))
static NODE *
compile_cond(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE clauses = cdr(form);
    if (clauses == SCM_NIL) return ALLOC_node_const_unspec();
    VALUE first = car(clauses);
    VALUE rest = cdr(clauses);
    VALUE test = car(first);
    VALUE body = cdr(first);
    bool is_else = is_symbol(test, "else");
    NODE *thn;
    if (body == SCM_NIL) {
        // (cond (test) ...) — value of test is returned when true.
        // Compile as: (let ((t test)) (if t t <rest>))
        // Simpler: just treat as (begin test).
        thn = compile(c, test, scope, is_tail);
    } else if (scm_is_pair(body) && is_symbol(car(body), "=>")) {
        // (cond (test => fn) ...) — apply fn to test result.
        VALUE fn = cadr(body);
        VALUE call = scm_cons(fn, scm_cons(test, SCM_NIL));
        thn = compile(c, call, scope, is_tail);
    } else {
        VALUE begin = scm_cons(scm_intern("begin"), body);
        thn = compile(c, begin, scope, is_tail);
    }
    if (is_else) return thn;
    NODE *cnd = compile(c, test, scope, false);
    NODE *els = (rest == SCM_NIL)
        ? ALLOC_node_const_unspec()
        : compile_cond(c, scm_cons(scm_intern("cond"), rest), scope, is_tail);
    return ALLOC_node_if(cnd, thn, els);
}

// (case key (vals body...) ... (else body...))
static NODE *
compile_case(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE key = cadr(form);
    VALUE clauses = cdr(cdr(form));
    // Bind key once: (let ((k <key>)) (cond ((memv k '(vs...)) body...) ...))
    VALUE k_sym = gensym_at("case-key");
    VALUE bindings = scm_cons(scm_cons(k_sym, scm_cons(key, SCM_NIL)), SCM_NIL);
    VALUE cond_clauses = SCM_NIL, *tail = &cond_clauses;
    for (VALUE cl = clauses; scm_is_pair(cl); cl = cdr(cl)) {
        VALUE clause = car(cl);
        VALUE vals = car(clause);
        VALUE body = cdr(clause);
        VALUE test;
        if (is_symbol(vals, "else")) {
            test = scm_intern("else");
        } else {
            VALUE quoted_vals = scm_cons(scm_intern("quote"), scm_cons(vals, SCM_NIL));
            test = scm_cons(scm_intern("memv"),
                            scm_cons(k_sym, scm_cons(quoted_vals, SCM_NIL)));
        }
        VALUE new_clause = scm_cons(test, body);
        *tail = scm_cons(new_clause, SCM_NIL);
        tail = &SCM_PTR(*tail)->pair.cdr;
    }
    VALUE cnd = scm_cons(scm_intern("cond"), cond_clauses);
    VALUE letform = scm_cons(scm_intern("let"),
                             scm_cons(bindings, scm_cons(cnd, SCM_NIL)));
    return compile(c, letform, scope, is_tail);
}

// (and a b c) → (if a (if b c #f) #f).  (and) → #t.  (and a) → a.
static NODE *
compile_and(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE args = cdr(form);
    if (args == SCM_NIL) return ALLOC_node_const_bool(1);
    if (cdr(args) == SCM_NIL) return compile(c, car(args), scope, is_tail);
    NODE *cnd = compile(c, car(args), scope, false);
    VALUE rest = scm_cons(scm_intern("and"), cdr(args));
    NODE *thn = compile(c, rest, scope, is_tail);
    NODE *els = ALLOC_node_const_bool(0);
    return ALLOC_node_if(cnd, thn, els);
}

// (or a b c) — returns first truthy or #f.  Implemented as
// (let ((t a)) (if t t (or b c))) using temp slot at depth 0 of an
// inserted scope.
static NODE *
compile_or(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE args = cdr(form);
    if (args == SCM_NIL) return ALLOC_node_const_bool(0);
    if (cdr(args) == SCM_NIL) return compile(c, car(args), scope, is_tail);
    VALUE tmp = gensym_at("or-tmp");
    VALUE binding = scm_cons(scm_cons(tmp, scm_cons(car(args), SCM_NIL)), SCM_NIL);
    VALUE rest = scm_cons(scm_intern("or"), cdr(args));
    VALUE iff = scm_cons(scm_intern("if"),
                         scm_cons(tmp, scm_cons(tmp, scm_cons(rest, SCM_NIL))));
    VALUE letform = scm_cons(scm_intern("let"),
                             scm_cons(binding, scm_cons(iff, SCM_NIL)));
    return compile(c, letform, scope, is_tail);
}

// (when test body) → (if test (begin body) <unspec>).  (unless test body)
// inverts.
static NODE *
compile_when(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE test = cadr(form);
    VALUE body = cdr(cdr(form));
    NODE *cnd = compile(c, test, scope, false);
    NODE *thn = compile(c, scm_cons(scm_intern("begin"), body), scope, is_tail);
    NODE *els = ALLOC_node_const_unspec();
    return ALLOC_node_if(cnd, thn, els);
}

static NODE *
compile_unless(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE test = cadr(form);
    VALUE body = cdr(cdr(form));
    NODE *cnd = compile(c, test, scope, false);
    NODE *thn = ALLOC_node_const_unspec();
    NODE *els = compile(c, scm_cons(scm_intern("begin"), body), scope, is_tail);
    return ALLOC_node_if(cnd, thn, els);
}

// (do ((var init step) ...) (test result...) body...) →
//   (letrec ((loop (lambda (var ...)
//                    (if test
//                        (begin result...)
//                        (begin body... (loop step...))))))
//     (loop init ...))
static NODE *
compile_do(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE specs = cadr(form);
    VALUE test_clause = caddr(form);
    VALUE body = cdr(cdr(cdr(form)));
    VALUE test = car(test_clause);
    VALUE result = cdr(test_clause);

    VALUE vars  = SCM_NIL, *vt = &vars;
    VALUE inits = SCM_NIL, *it = &inits;
    VALUE steps = SCM_NIL, *st = &steps;
    for (VALUE p = specs; scm_is_pair(p); p = cdr(p)) {
        VALUE spec = car(p);
        VALUE var = car(spec);
        VALUE init = cadr(spec);
        VALUE step = (cdr(cdr(spec)) != SCM_NIL) ? caddr(spec) : var;
        *vt = scm_cons(var,  SCM_NIL); vt = &SCM_PTR(*vt)->pair.cdr;
        *it = scm_cons(init, SCM_NIL); it = &SCM_PTR(*it)->pair.cdr;
        *st = scm_cons(step, SCM_NIL); st = &SCM_PTR(*st)->pair.cdr;
    }
    VALUE loop_sym = gensym_at("do-loop");
    VALUE recur = scm_cons(loop_sym, steps);
    VALUE result_branch = (result == SCM_NIL)
        ? scm_cons(scm_intern("if"),
                   scm_cons(SCM_TRUE, scm_cons(SCM_UNSPEC, SCM_NIL)))   // unspec
        : scm_cons(scm_intern("begin"), result);
    VALUE body_then_recur = list_append1(body, recur);
    VALUE iff = scm_cons(scm_intern("if"),
                         scm_cons(test,
                                  scm_cons(result_branch,
                                           scm_cons(scm_cons(scm_intern("begin"), body_then_recur), SCM_NIL))));
    VALUE lambda = scm_cons(scm_intern("lambda"),
                            scm_cons(vars, scm_cons(iff, SCM_NIL)));
    VALUE binding = scm_cons(loop_sym, scm_cons(lambda, SCM_NIL));
    VALUE letrec = scm_cons(scm_intern("letrec"),
                            scm_cons(scm_cons(binding, SCM_NIL),
                                     scm_cons(scm_cons(loop_sym, inits), SCM_NIL)));
    return compile(c, letrec, scope, is_tail);
}

static NODE *
compile_define(CTX *c, VALUE form, struct lex_scope *scope)
{
    VALUE second = cadr(form);
    if (scm_is_pair(second)) {
        VALUE name = car(second);
        VALUE params = cdr(second);
        VALUE body = cdr(cdr(form));
        VALUE lambda = scm_cons(scm_intern("lambda"), scm_cons(params, body));
        NODE *val = compile(c, lambda, scope, false);
        return ALLOC_node_gdef(SCM_PTR(name)->sym.name, val);
    }
    VALUE name = second;
    VALUE val_form = caddr(form);
    NODE *val = compile(c, val_form, scope, false);
    return ALLOC_node_gdef(SCM_PTR(name)->sym.name, val);
}

static NODE *
compile(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    if (SCM_IS_FIXNUM(form)) {
        int64_t n = SCM_FIXVAL(form);
        if (n >= INT32_MIN && n <= INT32_MAX) return ALLOC_node_const_int((int32_t)n);
        return ALLOC_node_const_int64((uint64_t)form);
    }
    if (form == SCM_NIL)    return ALLOC_node_const_nil();
    if (form == SCM_TRUE)   return ALLOC_node_const_bool(1);
    if (form == SCM_FALSE)  return ALLOC_node_const_bool(0);
    if (form == SCM_UNSPEC) return ALLOC_node_const_unspec();
    if (scm_is_double(form))return ALLOC_node_const_double(scm_get_double(form));
    if (scm_is_string(form))return ALLOC_node_const_str(SCM_PTR(form)->str.chars);
    if (scm_is_char(form))  return ALLOC_node_const_char(SCM_PTR(form)->ch);
    if (scm_is_vector(form))return ALLOC_node_quote((uint64_t)form);
    if (scm_is_bignum(form) || scm_is_rational(form) || scm_is_complex(form))
        return ALLOC_node_quote((uint64_t)form);
    if (scm_is_symbol(form)) {
        const char *name = SCM_PTR(form)->sym.name;
        uint32_t depth, idx;
        if (lex_lookup(scope, name, &depth, &idx)) {
            return ALLOC_node_lref(depth, idx);
        }
        return ALLOC_node_gref(name);
    }
    if (!scm_is_pair(form)) {
        scm_error(c, "compile: unexpected form");
    }
    VALUE head = car(form);
    if (scm_is_symbol(head)) {
        const char *h = SCM_PTR(head)->sym.name;
        if (strcmp(h, "quote") == 0)    return compile_quote(cadr(form));
        if (strcmp(h, "quasiquote") == 0) {
            VALUE expanded = expand_quasiquote(cadr(form), 1);
            return compile(c, expanded, scope, is_tail);
        }
        if (strcmp(h, "delay") == 0) {
            // (delay E) → (|make-promise| (lambda () E))
            VALUE thunk = scm_cons(scm_intern("lambda"),
                            scm_cons(SCM_NIL,
                                     scm_cons(cadr(form), SCM_NIL)));
            VALUE call  = scm_cons(scm_intern("|make-promise|"),
                            scm_cons(thunk, SCM_NIL));
            return compile(c, call, scope, is_tail);
        }
        if (strcmp(h, "if") == 0) {
            NODE *cnd = compile(c, cadr(form), scope, false);
            NODE *thn = compile(c, caddr(form), scope, is_tail);
            NODE *els = (cdr(cdr(cdr(form))) == SCM_NIL)
                ? ALLOC_node_const_unspec()
                : compile(c, cadddr(form), scope, is_tail);
            return ALLOC_node_if(cnd, thn, els);
        }
        if (strcmp(h, "begin") == 0) {
            return compile_seq(c, cdr(form), scope, is_tail);
        }
        if (strcmp(h, "lambda") == 0) {
            return compile_lambda(c, cadr(form), cdr(cdr(form)), scope);
        }
        if (strcmp(h, "set!") == 0) {
            VALUE name = cadr(form);
            VALUE val_form = caddr(form);
            const char *nm = SCM_PTR(name)->sym.name;
            uint32_t depth, idx;
            if (lex_lookup(scope, nm, &depth, &idx)) {
                NODE *val = compile(c, val_form, scope, false);
                return ALLOC_node_lset(depth, idx, val);
            }
            NODE *val = compile(c, val_form, scope, false);
            return ALLOC_node_gset(nm, val);
        }
        if (strcmp(h, "define") == 0) {
            return compile_define(c, form, scope);
        }
        if (strcmp(h, "let") == 0)    return compile_let(c, form, scope, is_tail);
        if (strcmp(h, "let*") == 0)   return compile_letstar(c, form, scope, is_tail);
        if (strcmp(h, "letrec") == 0) return compile_letrec(c, form, scope, is_tail);
        if (strcmp(h, "cond") == 0)   return compile_cond(c, form, scope, is_tail);
        if (strcmp(h, "case") == 0)   return compile_case(c, form, scope, is_tail);
        if (strcmp(h, "and") == 0)    return compile_and(c, form, scope, is_tail);
        if (strcmp(h, "or") == 0)     return compile_or(c, form, scope, is_tail);
        if (strcmp(h, "when") == 0)   return compile_when(c, form, scope, is_tail);
        if (strcmp(h, "unless") == 0) return compile_unless(c, form, scope, is_tail);
        if (strcmp(h, "do") == 0)     return compile_do(c, form, scope, is_tail);
        if (strcmp(h, "call/cc") == 0 || strcmp(h, "call-with-current-continuation") == 0) {
            NODE *fn = compile(c, cadr(form), scope, false);
            return ALLOC_node_callcc(fn);
        }
    }
    return compile_call(c, head, cdr(form), scope, is_tail);
}

// ---------------------------------------------------------------------------
// Apply / tail-call trampoline.
// ---------------------------------------------------------------------------

// Bind argv into a fresh frame for `cl`.  Handles dotted-rest by collecting
// excess args into a list bound to the last slot.
static struct sframe *
build_frame_for(CTX *c, struct sobj *cl, int argc, VALUE *argv)
{
    int nparams = cl->closure.nparams;
    int has_rest = cl->closure.has_rest;
    int total = nparams + (has_rest ? 1 : 0);
    if (has_rest) {
        if (argc < nparams) scm_error(c, "too few arguments");
    } else {
        if (argc != nparams) scm_error(c, "wrong number of arguments (got %d, expected %d)", argc, nparams);
    }
    struct sframe *f = scm_new_frame(cl->closure.env, total);
    for (int i = 0; i < nparams; i++) f->slots[i] = argv[i];
    if (has_rest) {
        VALUE rest = SCM_NIL;
        for (int i = argc - 1; i >= nparams; i--) {
            rest = scm_cons(argv[i], rest);
        }
        f->slots[nparams] = rest;
    }
    return f;
}

VALUE
scm_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (UNLIKELY(scm_is_prim(fn))) {
        struct sobj *p = SCM_PTR(fn);
        if (argc < p->prim.min_argc) {
            scm_error(c, "%s: too few arguments", p->prim.name);
        }
        if (p->prim.max_argc >= 0 && argc > p->prim.max_argc) {
            scm_error(c, "%s: too many arguments", p->prim.name);
        }
        return p->prim.fn(c, argc, argv);
    }
    if (UNLIKELY(scm_is_cont(fn))) {
        struct sobj *k = SCM_PTR(fn);
        if (!k->cont->active) scm_error(c, "continuation already invoked / expired");
        if (argc != 1) scm_error(c, "continuation expects exactly 1 argument");
        k->cont->result = argv[0];
        longjmp(k->cont->buf, 1);
    }
    if (LIKELY(scm_is_closure(fn))) {
        struct sobj *cl = SCM_PTR(fn);
        struct sframe *new_env;
        // Leaf-closure stack frame.  When the closure's body has no
        // nested `lambda`, no escaped sub-closure can capture this
        // frame, so we can park it on the C stack via `alloca` and
        // avoid the GC_malloc entirely.  Lifetime = the rest of this
        // scm_apply call, which is exactly what the body needs.
        if (LIKELY(cl->closure.leaf)) {
            int total = cl->closure.nparams + (cl->closure.has_rest ? 1 : 0);
            if (cl->closure.has_rest) {
                if (argc < cl->closure.nparams) scm_error(c, "too few arguments");
            } else {
                if (argc != cl->closure.nparams) scm_error(c, "wrong number of arguments (got %d, expected %d)", argc, cl->closure.nparams);
            }
            new_env = (struct sframe *)alloca(sizeof(struct sframe) +
                                              sizeof(VALUE) * (total ? total : 1));
            new_env->parent = cl->closure.env;
            new_env->nslots = total;
            for (int i = 0; i < cl->closure.nparams; i++) new_env->slots[i] = argv[i];
            if (cl->closure.has_rest) {
                VALUE rest = SCM_NIL;
                for (int i = argc - 1; i >= cl->closure.nparams; i--)
                    rest = scm_cons(argv[i], rest);
                new_env->slots[cl->closure.nparams] = rest;
            }
        } else {
            new_env = build_frame_for(c, cl, argc, argv);
        }
        struct sframe *saved = c->env;
        NODE *body = cl->closure.body;
        c->env = new_env;
        // Trampoline: re-enter while tail_call_pending is set.  Also bumps
        // the body's dispatch counter — used by `--profile` mode to decide
        // which entries are worth AOT-compiling on the next run.
        body->head.dispatch_cnt++;
        for (;;) {
            VALUE v = EVAL(c, body);
            if (!c->tail_call_pending) {
                c->env = saved;
                return v;
            }
            c->tail_call_pending = 0;
            body = c->next_body;
            c->env = c->next_env;
            body->head.dispatch_cnt++;
        }
    }
    scm_error(c, "not a procedure");
}

// Slow-path complement to the inline `scm_apply_tail` in node.h.
// Handles non-tail calls, non-closure targets, has_rest closures, and
// the "shape mismatch" cases where the existing frame can't be reused.
VALUE
scm_apply_tail_slow(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail)
{
    if (is_tail && scm_is_closure(fn)) {
        struct sobj *cl = SCM_PTR(fn);
        int total = cl->closure.nparams + (cl->closure.has_rest ? 1 : 0);

        // Self-tail-call frame reuse.  When the new closure shares the
        // current frame's parent + slot count *and* its body has no
        // nested lambda (so no escaped closure can hold a reference),
        // we overwrite the live frame in place and skip a GC_malloc.
        // For tight tail loops (`loop` / `sum` benches) this removes
        // ~30 ns of allocation work per iteration.
        if (LIKELY(cl->closure.leaf &&
                    c->env != NULL &&
                    c->env->parent == cl->closure.env &&
                    c->env->nslots == total)) {
            if (cl->closure.has_rest) {
                if (argc < cl->closure.nparams) scm_error(c, "too few arguments");
            } else {
                if (argc != cl->closure.nparams) scm_error(c, "wrong number of arguments");
            }
            for (int i = 0; i < cl->closure.nparams; i++) c->env->slots[i] = argv[i];
            if (cl->closure.has_rest) {
                VALUE rest = SCM_NIL;
                for (int i = argc - 1; i >= cl->closure.nparams; i--) rest = scm_cons(argv[i], rest);
                c->env->slots[cl->closure.nparams] = rest;
            }
            c->next_body = cl->closure.body;
            c->next_env = c->env;
            c->tail_call_pending = 1;
            return SCM_UNSPEC;
        }

        struct sframe *new_env = build_frame_for(c, cl, argc, argv);
        c->next_body = cl->closure.body;
        c->next_env = new_env;
        c->tail_call_pending = 1;
        return SCM_UNSPEC;
    }
    return scm_apply(c, fn, argc, argv);
}

// Escape continuation via setjmp/longjmp.  Save/restore CTX state so a
// longjmp out of an inner call frame doesn't leave c->env / tail-call
// pending in an inconsistent state.  Calling the captured continuation
// after the original call/cc has returned raises a clean error rather
// than triggering UB.
VALUE
scm_callcc(CTX *c, VALUE fn)
{
    if (!scm_is_proc(fn)) scm_error(c, "call/cc: not a procedure");
    struct sobj *kobj = scm_alloc(OBJ_CONT);
    kobj->cont = (struct scont *)GC_malloc(sizeof(struct scont));
    kobj->cont->active = 1;
    kobj->cont->tag = ++c->cont_tag_seq;
    VALUE k = SCM_OBJ_VAL(kobj);
    struct sframe *saved_env = c->env;
    int saved_tcp = c->tail_call_pending;
    if (setjmp(kobj->cont->buf) != 0) {
        c->env = saved_env;
        c->tail_call_pending = saved_tcp;
        kobj->cont->active = 0;
        return kobj->cont->result;
    }
    VALUE arg[1] = { k };
    VALUE r = scm_apply(c, fn, 1, arg);
    kobj->cont->active = 0;
    return r;
}

// ---------------------------------------------------------------------------
// Primitives (R5RS subset).  Argument count is checked in scm_apply via
// (min_argc, max_argc); each prim assumes its arity has already been
// validated.
// ---------------------------------------------------------------------------

#define PRIM(name) static VALUE prim_##name(CTX *c, int argc, VALUE *argv)
#define ARGV(i) argv[i]
#define ARG_FIX(i) (SCM_IS_FIXNUM(ARGV(i)) ? SCM_FIXVAL(ARGV(i)) : (scm_error(c, "expected integer"), (int64_t)0))

// ---------------------------------------------------------------------------
// Numeric tower: fixnum < bignum < rational < flonum < complex.
//   add2/sub2/mul2/div2 promote both operands to the wider kind, perform the
//   op, then collapse back via scm_normalize_int / scm_make_rational_q /
//   scm_simplify_complex.  Variadic primitives fold these binary ops.
// ---------------------------------------------------------------------------

enum num_kind { NK_FIX = 0, NK_BIG, NK_RAT, NK_FLT, NK_CPX, NK_NONE = -1 };

static enum num_kind
num_kind_of(VALUE v)
{
    if (SCM_IS_FIXNUM(v))   return NK_FIX;
    if (scm_is_bignum(v))   return NK_BIG;
    if (scm_is_rational(v)) return NK_RAT;
    if (scm_is_double(v))   return NK_FLT;
    if (scm_is_complex(v))  return NK_CPX;
    return NK_NONE;
}

static void
require_number(CTX *c, VALUE v, const char *what)
{
    if (num_kind_of(v) == NK_NONE) scm_error(c, "%s: not a number", what);
}

// Linux x86_64: long is 64-bit so mpz_set_si covers full int64_t.  Wrap it
// in case we ever build on a platform where this is no longer true.
static void
mpz_init_si64(mpz_t z, int64_t v)
{
    mpz_init(z);
#if LONG_MAX >= INT64_MAX
    mpz_set_si(z, (long)v);
#else
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    mpz_set_str(z, buf, 10);
#endif
}

static void
to_mpz(VALUE v, mpz_t out)
{
    if (SCM_IS_FIXNUM(v))      mpz_init_si64(out, SCM_FIXVAL(v));
    else if (scm_is_bignum(v)) mpz_init_set(out, SCM_PTR(v)->mpz);
    else                       mpz_init(out);   // shouldn't happen
}

static void
to_mpq(VALUE v, mpq_t out)
{
    mpq_init(out);
    if (SCM_IS_FIXNUM(v)) {
#if LONG_MAX >= INT64_MAX
        mpq_set_si(out, (long)SCM_FIXVAL(v), 1);
#else
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)SCM_FIXVAL(v));
        mpz_set_str(mpq_numref(out), buf, 10);
        mpz_set_ui(mpq_denref(out), 1);
#endif
    }
    else if (scm_is_bignum(v)) {
        mpz_set(mpq_numref(out), SCM_PTR(v)->mpz);
        mpz_set_ui(mpq_denref(out), 1);
    }
    else if (scm_is_rational(v)) mpq_set(out, SCM_PTR(v)->mpq);
    else if (scm_is_double(v))   mpq_set_d(out, scm_get_double(v));
}

static void
get_complex(VALUE v, double *re, double *im)
{
    if (scm_is_complex(v)) { *re = SCM_PTR(v)->cpx.re; *im = SCM_PTR(v)->cpx.im; }
    else { *re = scm_get_double(v); *im = 0.0; }
}

static enum num_kind
common_kind(VALUE a, VALUE b)
{
    enum num_kind ka = num_kind_of(a), kb = num_kind_of(b);
    return ka > kb ? ka : kb;
}

VALUE
add2(CTX *c, VALUE a, VALUE b)
{
    // Fast path: both fixnums.  Bypasses require_number + common_kind +
    // switch.  Two fixnums in 63-bit range can overflow int64 only if both
    // operands are extreme; the builtin lets us bail to the bignum path
    // in that rare case.
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_add_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    // Inline-flonum fast path.  Most "scientific" doubles round-trip
    // through the rotated encoding, so this avoids both the kind switch
    // and the heap allocation.
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(scm_flonum_to_double(a) + scm_flonum_to_double(b));
    }
    require_number(c, a, "+"); require_number(c, b, "+");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_add_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(r);
        // overflow → fall through to bignum path
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_add(r, za, zb);
        VALUE rv = scm_normalize_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_add(r, qa, qb);
        VALUE rv = scm_make_rational_q(r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(scm_get_double(a) + scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(ar + br, ai + bi);
    }
    default: scm_error(c, "+: not a number");
    }
}

VALUE
sub2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_sub_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(scm_flonum_to_double(a) - scm_flonum_to_double(b));
    }
    require_number(c, a, "-"); require_number(c, b, "-");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_sub_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(r);
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_sub(r, za, zb);
        VALUE rv = scm_normalize_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_sub(r, qa, qb);
        VALUE rv = scm_make_rational_q(r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(scm_get_double(a) - scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(ar - br, ai - bi);
    }
    default: scm_error(c, "-: not a number");
    }
}

VALUE
mul2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_mul_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(scm_flonum_to_double(a) * scm_flonum_to_double(b));
    }
    require_number(c, a, "*"); require_number(c, b, "*");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_mul_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(r);
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_mul(r, za, zb);
        VALUE rv = scm_normalize_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_mul(r, qa, qb);
        VALUE rv = scm_make_rational_q(r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(scm_get_double(a) * scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(ar*br - ai*bi, ar*bi + ai*br);
    }
    default: scm_error(c, "*: not a number");
    }
}

static VALUE
div2(CTX *c, VALUE a, VALUE b)
{
    require_number(c, a, "/"); require_number(c, b, "/");
    enum num_kind k = common_kind(a, b);
    if (k <= NK_RAT) {
        // Exact arithmetic: a / b is rational.
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        if (mpq_sgn(qb) == 0) {
            mpq_clear(qa); mpq_clear(qb); mpq_clear(r);
            scm_error(c, "/: division by zero");
        }
        mpq_div(r, qa, qb);
        VALUE rv = scm_make_rational_q(r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    if (k == NK_FLT) {
        double rhs = scm_get_double(b);
        if (rhs == 0) scm_error(c, "/: division by zero");
        return scm_make_double(scm_get_double(a) / rhs);
    }
    // complex division
    double ar, ai, br, bi;
    get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
    double denom = br*br + bi*bi;
    if (denom == 0) scm_error(c, "/: division by zero");
    return scm_simplify_complex((ar*br + ai*bi) / denom, (ai*br - ar*bi) / denom);
}

static VALUE
negate(CTX *c, VALUE a)
{
    require_number(c, a, "-");
    switch (num_kind_of(a)) {
    case NK_FIX: {
        int64_t v = SCM_FIXVAL(a);
        if (v == INT64_MIN) {     // theoretical edge — fixnum range narrower than int64 anyway
            mpz_t z; mpz_init_si64(z, v); mpz_neg(z, z);
            VALUE rv = scm_normalize_int(z); mpz_clear(z); return rv;
        }
        return scm_make_int(-v);
    }
    case NK_BIG: {
        mpz_t z; mpz_init(z); mpz_neg(z, SCM_PTR(a)->mpz);
        VALUE rv = scm_normalize_int(z); mpz_clear(z); return rv;
    }
    case NK_RAT: {
        mpq_t q; mpq_init(q); mpq_neg(q, SCM_PTR(a)->mpq);
        VALUE rv = scm_make_rational_q(q); mpq_clear(q); return rv;
    }
    case NK_FLT: return scm_make_double(-scm_get_double(a));
    case NK_CPX: return scm_simplify_complex(-SCM_PTR(a)->cpx.re, -SCM_PTR(a)->cpx.im);
    default: scm_error(c, "-: not a number");
    }
}

// 3-way compare, real numbers only.  Returns -1, 0, 1, or aborts.
int
cmp2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t x = SCM_FIXVAL(a), y = SCM_FIXVAL(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        double x = scm_flonum_to_double(a), y = scm_flonum_to_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    require_number(c, a, "comparison"); require_number(c, b, "comparison");
    if (scm_is_complex(a) || scm_is_complex(b)) scm_error(c, "comparison: complex not ordered");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t x = SCM_FIXVAL(a), y = SCM_FIXVAL(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    case NK_BIG: {
        mpz_t za, zb; to_mpz(a, za); to_mpz(b, zb);
        int r = mpz_cmp(za, zb);
        mpz_clear(za); mpz_clear(zb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    case NK_RAT: {
        mpq_t qa, qb; to_mpq(a, qa); to_mpq(b, qb);
        int r = mpq_cmp(qa, qb);
        mpq_clear(qa); mpq_clear(qb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    case NK_FLT: {
        double x = scm_get_double(a), y = scm_get_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    default: return 0;
    }
}

PRIM(plus)
{
    if (argc == 0) return SCM_FIX(0);
    VALUE r = argv[0];
    require_number(c, r, "+");
    for (int i = 1; i < argc; i++) r = add2(c, r, argv[i]);
    return r;
}

PRIM(minus)
{
    if (argc == 0) scm_error(c, "-: need at least 1 arg");
    if (argc == 1) return negate(c, argv[0]);
    VALUE r = argv[0];
    for (int i = 1; i < argc; i++) r = sub2(c, r, argv[i]);
    return r;
}

PRIM(mul)
{
    if (argc == 0) return SCM_FIX(1);
    VALUE r = argv[0];
    require_number(c, r, "*");
    for (int i = 1; i < argc; i++) r = mul2(c, r, argv[i]);
    return r;
}

PRIM(div)
{
    if (argc == 0) scm_error(c, "/: need at least 1 arg");
    VALUE r = argv[0];
    if (argc == 1) {
        require_number(c, r, "/");
        return div2(c, SCM_FIX(1), r);
    }
    for (int i = 1; i < argc; i++) r = div2(c, r, argv[i]);
    return r;
}

#define DEF_NUM_CMP(name, op)                                                  \
PRIM(name)                                                                     \
{                                                                              \
    for (int i = 1; i < argc; i++) {                                           \
        if (!(cmp2(c, argv[i-1], argv[i]) op 0)) return SCM_FALSE;             \
    }                                                                          \
    return SCM_TRUE;                                                           \
}
DEF_NUM_CMP(num_eq, ==)
DEF_NUM_CMP(num_lt, <)
DEF_NUM_CMP(num_gt, >)
DEF_NUM_CMP(num_le, <=)
DEF_NUM_CMP(num_ge, >=)

PRIM(modulo)
{
    require_number(c, argv[0], "modulo"); require_number(c, argv[1], "modulo");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "modulo: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        int64_t a = SCM_FIXVAL(argv[0]), b = SCM_FIXVAL(argv[1]);
        int64_t r = a % b;
        if ((r != 0) && ((r < 0) != (b < 0))) r += b;
        return scm_make_int(r);
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_mod(r, za, zb);                  // mpz_mod returns 0..|zb|-1
    if (mpz_sgn(r) != 0 && mpz_sgn(r) != mpz_sgn(zb)) mpz_add(r, r, zb);
    VALUE rv = scm_normalize_int(r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(remainder)
{
    require_number(c, argv[0], "remainder"); require_number(c, argv[1], "remainder");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "remainder: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        return scm_make_int(SCM_FIXVAL(argv[0]) % SCM_FIXVAL(argv[1]));
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_tdiv_r(r, za, zb);
    VALUE rv = scm_normalize_int(r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(quotient)
{
    require_number(c, argv[0], "quotient"); require_number(c, argv[1], "quotient");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "quotient: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        return scm_make_int(SCM_FIXVAL(argv[0]) / SCM_FIXVAL(argv[1]));
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_tdiv_q(r, za, zb);
    VALUE rv = scm_normalize_int(r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(gcd_p)
{
    if (argc == 0) return SCM_FIX(0);
    mpz_t a, b, r; mpz_init(a); to_mpz(argv[0], a);
    if (mpz_sgn(a) < 0) mpz_abs(a, a);
    for (int i = 1; i < argc; i++) {
        to_mpz(argv[i], b); mpz_init(r);
        mpz_gcd(r, a, b);
        mpz_set(a, r); mpz_clear(b); mpz_clear(r);
    }
    VALUE rv = scm_normalize_int(a); mpz_clear(a); return rv;
}

PRIM(lcm_p)
{
    if (argc == 0) return SCM_FIX(1);
    mpz_t a, b, r; mpz_init(a); to_mpz(argv[0], a);
    mpz_abs(a, a);
    for (int i = 1; i < argc; i++) {
        to_mpz(argv[i], b); mpz_init(r);
        mpz_lcm(r, a, b);
        mpz_set(a, r); mpz_clear(b); mpz_clear(r);
    }
    VALUE rv = scm_normalize_int(a); mpz_clear(a); return rv;
}

PRIM(abs)
{
    require_number(c, argv[0], "abs");
    switch (num_kind_of(argv[0])) {
    case NK_FIX: {
        int64_t v = SCM_FIXVAL(argv[0]);
        return scm_make_int(v < 0 ? -v : v);
    }
    case NK_BIG: {
        mpz_t z; mpz_init(z); mpz_abs(z, SCM_PTR(argv[0])->mpz);
        VALUE rv = scm_normalize_int(z); mpz_clear(z); return rv;
    }
    case NK_RAT: {
        mpq_t q; mpq_init(q); mpq_abs(q, SCM_PTR(argv[0])->mpq);
        VALUE rv = scm_make_rational_q(q); mpq_clear(q); return rv;
    }
    case NK_FLT: {
        double d = scm_get_double(argv[0]);
        return scm_make_double(d < 0 ? -d : d);
    }
    default: scm_error(c, "abs: not a real number");
    }
}

PRIM(min) {
    VALUE m = argv[0];
    bool inexact = scm_is_inexact(m);
    for (int i = 1; i < argc; i++) {
        if (scm_is_inexact(argv[i])) inexact = true;
        if (cmp2(c, argv[i], m) < 0) m = argv[i];
    }
    return inexact && !scm_is_inexact(m) ? scm_make_double(scm_get_double(m)) : m;
}
PRIM(max) {
    VALUE m = argv[0];
    bool inexact = scm_is_inexact(m);
    for (int i = 1; i < argc; i++) {
        if (scm_is_inexact(argv[i])) inexact = true;
        if (cmp2(c, argv[i], m) > 0) m = argv[i];
    }
    return inexact && !scm_is_inexact(m) ? scm_make_double(scm_get_double(m)) : m;
}

PRIM(zero_p)     { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) == 0 ? SCM_TRUE : SCM_FALSE; }
PRIM(positive_p) { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) >  0 ? SCM_TRUE : SCM_FALSE; }
PRIM(negative_p) { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) <  0 ? SCM_TRUE : SCM_FALSE; }
PRIM(odd_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0])) return (SCM_FIXVAL(argv[0]) & 1) ? SCM_TRUE : SCM_FALSE;
    if (scm_is_bignum(argv[0])) return mpz_odd_p(SCM_PTR(argv[0])->mpz) ? SCM_TRUE : SCM_FALSE;
    scm_error(c, "odd?: not an integer");
}
PRIM(even_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0])) return (SCM_FIXVAL(argv[0]) & 1) ? SCM_FALSE : SCM_TRUE;
    if (scm_is_bignum(argv[0])) return mpz_even_p(SCM_PTR(argv[0])->mpz) ? SCM_TRUE : SCM_FALSE;
    scm_error(c, "even?: not an integer");
}

PRIM(number_p)   { (void)c; (void)argc; return scm_is_number(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(integer_p)  { (void)c; (void)argc; return scm_is_integer_value(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(real_p)     { (void)c; (void)argc; return scm_is_real(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(rational_p) { (void)c; (void)argc; return scm_is_real(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(complex_p)  { (void)c; (void)argc; return scm_is_number(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(exact_p)    { (void)c; (void)argc; return scm_is_exact(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(inexact_p)  { (void)c; (void)argc; return scm_is_inexact(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(exact_to_inexact) {
    (void)argc;
    if (scm_is_complex(argv[0])) return argv[0];
    return scm_make_double(scm_get_double(argv[0]));
}
PRIM(inexact_to_exact) {
    (void)argc;
    if (scm_is_exact(argv[0])) return argv[0];
    if (scm_is_double(argv[0])) {
        double d = scm_get_double(argv[0]);
        if (d == (double)(int64_t)d) return scm_make_int((int64_t)d);
        // approximate as rational via GMP
        mpq_t q; mpq_init(q); mpq_set_d(q, d);
        VALUE rv = scm_make_rational_q(q); mpq_clear(q);
        return rv;
    }
    scm_error(c, "inexact->exact: not a real number");
}

PRIM(cons)   { (void)c; (void)argc; return scm_cons(argv[0], argv[1]); }
PRIM(car)    {
    if (!scm_is_pair(argv[0])) scm_error(c, "car: not a pair");
    return SCM_PTR(argv[0])->pair.car;
}
PRIM(cdr)    {
    if (!scm_is_pair(argv[0])) scm_error(c, "cdr: not a pair");
    return SCM_PTR(argv[0])->pair.cdr;
}
PRIM(set_car) {
    (void)argc;
    if (!scm_is_pair(argv[0])) scm_error(c, "set-car!: not a pair");
    SCM_PTR(argv[0])->pair.car = argv[1];
    return SCM_UNSPEC;
}
PRIM(set_cdr) {
    (void)argc;
    if (!scm_is_pair(argv[0])) scm_error(c, "set-cdr!: not a pair");
    SCM_PTR(argv[0])->pair.cdr = argv[1];
    return SCM_UNSPEC;
}
PRIM(pair_p) { (void)c; (void)argc; return scm_is_pair(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(null_p) { (void)c; (void)argc; return scm_is_null(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(list)   {
    (void)c;
    VALUE r = SCM_NIL;
    for (int i = argc - 1; i >= 0; i--) r = scm_cons(argv[i], r);
    return r;
}
PRIM(list_p) {
    // R5RS: circular lists return #f.  Floyd's tortoise-and-hare detects
    // cycles in linear time without auxiliary storage.
    (void)c; (void)argc;
    VALUE slow = argv[0], fast = argv[0];
    while (scm_is_pair(fast)) {
        fast = SCM_PTR(fast)->pair.cdr;
        if (!scm_is_pair(fast)) break;
        fast = SCM_PTR(fast)->pair.cdr;
        slow = SCM_PTR(slow)->pair.cdr;
        if (slow == fast) return SCM_FALSE;
    }
    return fast == SCM_NIL ? SCM_TRUE : SCM_FALSE;
}
PRIM(length) {
    int n = 0;
    VALUE v = argv[0];
    while (scm_is_pair(v)) { n++; v = SCM_PTR(v)->pair.cdr; }
    if (v != SCM_NIL) scm_error(c, "length: not a proper list");
    (void)argc;
    return SCM_FIX(n);
}
PRIM(reverse) {
    (void)argc;
    VALUE v = argv[0], r = SCM_NIL;
    while (scm_is_pair(v)) { r = scm_cons(SCM_PTR(v)->pair.car, r); v = SCM_PTR(v)->pair.cdr; }
    if (v != SCM_NIL) scm_error(c, "reverse: not a proper list");
    return r;
}
PRIM(append) {
    (void)c;
    if (argc == 0) return SCM_NIL;
    VALUE result = argv[argc - 1];
    for (int i = argc - 2; i >= 0; i--) {
        VALUE list = argv[i];
        VALUE tmp = SCM_NIL;
        while (scm_is_pair(list)) {
            tmp = scm_cons(SCM_PTR(list)->pair.car, tmp);
            list = SCM_PTR(list)->pair.cdr;
        }
        while (scm_is_pair(tmp)) {
            result = scm_cons(SCM_PTR(tmp)->pair.car, result);
            tmp = SCM_PTR(tmp)->pair.cdr;
        }
    }
    return result;
}
PRIM(list_ref) {
    VALUE v = argv[0];
    int64_t i = ARG_FIX(1);
    while (i-- > 0 && scm_is_pair(v)) v = SCM_PTR(v)->pair.cdr;
    if (!scm_is_pair(v)) scm_error(c, "list-ref: out of range");
    (void)argc;
    return SCM_PTR(v)->pair.car;
}
PRIM(list_tail) {
    VALUE v = argv[0];
    int64_t i = ARG_FIX(1);
    while (i-- > 0 && scm_is_pair(v)) v = SCM_PTR(v)->pair.cdr;
    (void)argc;
    return v;
}
static bool scm_eq(VALUE a, VALUE b) { return a == b; }
static bool scm_eqv_impl(VALUE a, VALUE b) {
    if (a == b) return true;
    if (scm_is_double(a) && scm_is_double(b)) return scm_get_double(a) == scm_get_double(b);
    if (scm_is_char(a) && scm_is_char(b))     return SCM_PTR(a)->ch == SCM_PTR(b)->ch;
    return false;
}
static bool scm_equal_impl(VALUE a, VALUE b) {
    if (scm_eqv_impl(a, b)) return true;
    if (scm_is_pair(a) && scm_is_pair(b))
        return scm_equal_impl(SCM_PTR(a)->pair.car, SCM_PTR(b)->pair.car) &&
               scm_equal_impl(SCM_PTR(a)->pair.cdr, SCM_PTR(b)->pair.cdr);
    if (scm_is_string(a) && scm_is_string(b)) {
        struct sobj *sa = SCM_PTR(a), *sb = SCM_PTR(b);
        return sa->str.len == sb->str.len && memcmp(sa->str.chars, sb->str.chars, sa->str.len) == 0;
    }
    if (scm_is_vector(a) && scm_is_vector(b)) {
        struct sobj *va = SCM_PTR(a), *vb = SCM_PTR(b);
        if (va->vec.len != vb->vec.len) return false;
        for (size_t i = 0; i < va->vec.len; i++)
            if (!scm_equal_impl(va->vec.items[i], vb->vec.items[i])) return false;
        return true;
    }
    return false;
}
PRIM(eq_p)     { (void)c; (void)argc; return scm_eq(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }
PRIM(eqv_p)    { (void)c; (void)argc; return scm_eqv_impl(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }
PRIM(equal_p)  { (void)c; (void)argc; return scm_equal_impl(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }

PRIM(memv) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (scm_eqv_impl(k, SCM_PTR(l)->pair.car)) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(memq) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (k == SCM_PTR(l)->pair.car) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(member) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (scm_equal_impl(k, SCM_PTR(l)->pair.car)) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assq) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && SCM_PTR(p)->pair.car == k) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assv) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && scm_eqv_impl(SCM_PTR(p)->pair.car, k)) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assoc) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && scm_equal_impl(SCM_PTR(p)->pair.car, k)) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}

PRIM(boolean_p)  { (void)c; (void)argc; return scm_is_bool(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(not_p)      { (void)c; (void)argc; return scm_is_false(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(symbol_p)   { (void)c; (void)argc; return scm_is_symbol(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(procedure_p){ (void)c; (void)argc; return scm_is_proc(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(string_p)   { (void)c; (void)argc; return scm_is_string(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_p)     { (void)c; (void)argc; return scm_is_char(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(vector_p)   { (void)c; (void)argc; return scm_is_vector(argv[0]) ? SCM_TRUE : SCM_FALSE; }

PRIM(symbol_to_string) {
    (void)argc;
    if (!scm_is_symbol(argv[0])) scm_error(c, "symbol->string: not a symbol");
    return scm_make_string(SCM_PTR(argv[0])->sym.name, strlen(SCM_PTR(argv[0])->sym.name));
}
PRIM(string_to_symbol) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string->symbol: not a string");
    return scm_intern(SCM_PTR(argv[0])->str.chars);
}

PRIM(number_to_string) {
    (void)argc;
    int base = 10;
    if (argc >= 2) base = (int)ARG_FIX(1);
    char buf[128];
    if (SCM_IS_FIXNUM(argv[0])) {
        if (base == 10) snprintf(buf, sizeof(buf), "%lld", (long long)SCM_FIXVAL(argv[0]));
        else {
            mpz_t z; mpz_init_si64(z, SCM_FIXVAL(argv[0]));
            char *s = mpz_get_str(NULL, base, z);
            VALUE rv = scm_make_string(s, strlen(s));
            mpz_clear(z);
            return rv;
        }
    }
    else if (scm_is_bignum(argv[0])) {
        char *s = mpz_get_str(NULL, base, SCM_PTR(argv[0])->mpz);
        return scm_make_string(s, strlen(s));
    }
    else if (scm_is_rational(argv[0])) {
        char *s = mpq_get_str(NULL, base, SCM_PTR(argv[0])->mpq);
        return scm_make_string(s, strlen(s));
    }
    else if (scm_is_double(argv[0])) {
        snprintf(buf, sizeof(buf), "%.15g", scm_get_double(argv[0]));
    }
    else if (scm_is_complex(argv[0])) {
        snprintf(buf, sizeof(buf), "%.15g%+.15gi",
                 SCM_PTR(argv[0])->cpx.re, SCM_PTR(argv[0])->cpx.im);
    }
    else scm_error(c, "number->string: not a number");
    return scm_make_string(buf, strlen(buf));
}
PRIM(string_to_number) {
    (void)argc; (void)c;
    if (!scm_is_string(argv[0])) scm_error(c, "string->number: not a string");
    int base = 10;
    if (argc >= 2) base = (int)ARG_FIX(1);
    const char *s = SCM_PTR(argv[0])->str.chars;
    // rational?
    char *slash = strchr(s, '/');
    if (slash) {
        char *copy = (char *)GC_malloc_atomic(strlen(s) + 1);
        memcpy(copy, s, strlen(s) + 1);
        char *sl = strchr(copy, '/');
        *sl = '\0';
        mpz_t num, den;
        if (mpz_init_set_str(num, copy, base) == 0) {
            if (mpz_init_set_str(den, sl + 1, base) == 0 && mpz_sgn(den) != 0) {
                VALUE rv = scm_make_rational_zz(num, den);
                mpz_clear(num); mpz_clear(den); return rv;
            }
            mpz_clear(num);
        }
        return SCM_FALSE;
    }
    // integer?
    mpz_t z;
    if (mpz_init_set_str(z, s, base) == 0) {
        VALUE rv = scm_normalize_int(z); mpz_clear(z); return rv;
    }
    mpz_clear(z);
    // double?
    char *end;
    double d = strtod(s, &end);
    if (*end == '\0' && end != s) return scm_make_double(d);
    return SCM_FALSE;
}

PRIM(string_length) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string-length: not a string");
    return SCM_FIX(SCM_PTR(argv[0])->str.len);
}
PRIM(string_ref) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string-ref: not a string");
    size_t i = (size_t)ARG_FIX(1);
    if (i >= SCM_PTR(argv[0])->str.len) scm_error(c, "string-ref: out of range");
    return scm_make_char((unsigned char)SCM_PTR(argv[0])->str.chars[i]);
}
PRIM(string_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) {
        struct sobj *a = SCM_PTR(argv[i-1]), *b = SCM_PTR(argv[i]);
        if (a->str.len != b->str.len || memcmp(a->str.chars, b->str.chars, a->str.len) != 0) return SCM_FALSE;
    }
    return SCM_TRUE;
}

static int
str_cmp(struct sobj *a, struct sobj *b)
{
    size_t n = a->str.len < b->str.len ? a->str.len : b->str.len;
    int c = memcmp(a->str.chars, b->str.chars, n);
    if (c != 0) return c;
    return a->str.len < b->str.len ? -1 : a->str.len > b->str.len ? 1 : 0;
}

#define DEF_STR_CMP(name, op)                                                 \
PRIM(name) {                                                                  \
    (void)c;                                                                  \
    for (int i = 1; i < argc; i++) {                                          \
        if (!(str_cmp(SCM_PTR(argv[i-1]), SCM_PTR(argv[i])) op 0))            \
            return SCM_FALSE;                                                 \
    }                                                                         \
    return SCM_TRUE;                                                          \
}
DEF_STR_CMP(string_lt, <)
DEF_STR_CMP(string_gt, >)
DEF_STR_CMP(string_le, <=)
DEF_STR_CMP(string_ge, >=)
#undef DEF_STR_CMP

PRIM(string_ci_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) {
        struct sobj *a = SCM_PTR(argv[i-1]), *b = SCM_PTR(argv[i]);
        if (a->str.len != b->str.len) return SCM_FALSE;
        for (size_t j = 0; j < a->str.len; j++)
            if (tolower((unsigned char)a->str.chars[j]) !=
                tolower((unsigned char)b->str.chars[j])) return SCM_FALSE;
    }
    return SCM_TRUE;
}

PRIM(char_ci_eq) {
    (void)c;
    for (int i = 1; i < argc; i++)
        if (tolower(SCM_PTR(argv[i-1])->ch) != tolower(SCM_PTR(argv[i])->ch))
            return SCM_FALSE;
    return SCM_TRUE;
}

PRIM(string_copy) {
    (void)argc; (void)c;
    struct sobj *s = SCM_PTR(argv[0]);
    return scm_make_string(s->str.chars, s->str.len);
}

PRIM(string_set) {
    (void)argc;
    struct sobj *s = SCM_PTR(argv[0]);
    size_t i = (size_t)ARG_FIX(1);
    if (i >= s->str.len) scm_error(c, "string-set!: out of range");
    s->str.chars[i] = (char)SCM_PTR(argv[2])->ch;
    return SCM_UNSPEC;
}

PRIM(string_p_proc) {
    (void)argc; (void)c;
    return scm_make_string(SCM_PTR(argv[0])->sym.name,
                           strlen(SCM_PTR(argv[0])->sym.name));
}
PRIM(make_string) {
    char fill = ' ';
    if (argc >= 2 && scm_is_char(argv[1])) fill = (char)SCM_PTR(argv[1])->ch;
    (void)c;
    return scm_make_string_n((size_t)ARG_FIX(0), fill);
}
PRIM(string_append) {
    (void)c;
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += SCM_PTR(argv[i])->str.len;
    VALUE r = scm_make_string_n(total, ' ');
    char *out = SCM_PTR(r)->str.chars;
    for (int i = 0; i < argc; i++) {
        memcpy(out, SCM_PTR(argv[i])->str.chars, SCM_PTR(argv[i])->str.len);
        out += SCM_PTR(argv[i])->str.len;
    }
    return r;
}
PRIM(substring) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "substring: not a string");
    size_t start = (size_t)ARG_FIX(1), end = (size_t)ARG_FIX(2);
    if (start > end || end > SCM_PTR(argv[0])->str.len) scm_error(c, "substring: out of range");
    return scm_make_string(SCM_PTR(argv[0])->str.chars + start, end - start);
}
PRIM(string_to_list) {
    (void)c; (void)argc;
    struct sobj *s = SCM_PTR(argv[0]);
    VALUE r = SCM_NIL;
    for (size_t i = s->str.len; i--; ) r = scm_cons(scm_make_char((unsigned char)s->str.chars[i]), r);
    return r;
}
PRIM(list_to_string) {
    VALUE l = argv[0];
    size_t n = 0;
    for (VALUE p = l; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) n++;
    VALUE r = scm_make_string_n(n, ' ');
    char *out = SCM_PTR(r)->str.chars;
    size_t i = 0;
    for (VALUE p = l; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        if (!scm_is_char(SCM_PTR(p)->pair.car)) scm_error(c, "list->string: not a char");
        out[i++] = (char)SCM_PTR(SCM_PTR(p)->pair.car)->ch;
    }
    (void)argc;
    return r;
}
PRIM(string_form) {
    // (string c1 c2 ...) → string
    (void)c;
    VALUE r = scm_make_string_n((size_t)argc, ' ');
    char *out = SCM_PTR(r)->str.chars;
    for (int i = 0; i < argc; i++) out[i] = (char)SCM_PTR(argv[i])->ch;
    return r;
}

PRIM(char_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) if (SCM_PTR(argv[i-1])->ch != SCM_PTR(argv[i])->ch) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_lt) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch < SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_le) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch <= SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_gt) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch > SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_ge) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch >= SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_to_integer) { (void)c; (void)argc; return SCM_FIX(SCM_PTR(argv[0])->ch); }
PRIM(integer_to_char) { (void)c; (void)argc; return scm_make_char((uint32_t)ARG_FIX(0)); }
PRIM(char_alphabetic_p) { (void)c; (void)argc; return isalpha(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_numeric_p)    { (void)c; (void)argc; return isdigit(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_whitespace_p) { (void)c; (void)argc; return isspace(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_upper_p)      { (void)c; (void)argc; return isupper(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_lower_p)      { (void)c; (void)argc; return islower(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_upcase)       { (void)c; (void)argc; return scm_make_char(toupper(SCM_PTR(argv[0])->ch)); }
PRIM(char_downcase)     { (void)c; (void)argc; return scm_make_char(tolower(SCM_PTR(argv[0])->ch)); }

PRIM(make_vector) {
    size_t n = (size_t)ARG_FIX(0);
    VALUE fill = argc >= 2 ? argv[1] : SCM_UNSPEC;
    (void)c;
    return scm_make_vector(n, fill);
}
PRIM(vector_form) {
    (void)c;
    VALUE r = scm_make_vector((size_t)argc, SCM_UNSPEC);
    for (int i = 0; i < argc; i++) SCM_PTR(r)->vec.items[i] = argv[i];
    return r;
}
PRIM(vector_length) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-length: not a vector");
    return SCM_FIX(SCM_PTR(argv[0])->vec.len);
}
PRIM(vector_ref) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-ref: not a vector");
    size_t i = (size_t)ARG_FIX(1);
    if (i >= SCM_PTR(argv[0])->vec.len) scm_error(c, "vector-ref: out of range");
    return SCM_PTR(argv[0])->vec.items[i];
}
PRIM(vector_set) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-set!: not a vector");
    size_t i = (size_t)ARG_FIX(1);
    if (i >= SCM_PTR(argv[0])->vec.len) scm_error(c, "vector-set!: out of range");
    SCM_PTR(argv[0])->vec.items[i] = argv[2];
    return SCM_UNSPEC;
}
PRIM(vector_fill) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-fill!: not a vector");
    for (size_t i = 0; i < SCM_PTR(argv[0])->vec.len; i++)
        SCM_PTR(argv[0])->vec.items[i] = argv[1];
    return SCM_UNSPEC;
}
PRIM(vector_to_list) {
    (void)c; (void)argc;
    struct sobj *v = SCM_PTR(argv[0]);
    VALUE r = SCM_NIL;
    for (size_t i = v->vec.len; i--; ) r = scm_cons(v->vec.items[i], r);
    return r;
}
PRIM(list_to_vector) {
    (void)c; (void)argc;
    VALUE l = argv[0];
    size_t n = 0;
    for (VALUE p = l; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) n++;
    VALUE r = scm_make_vector(n, SCM_UNSPEC);
    size_t i = 0;
    for (VALUE p = l; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr)
        SCM_PTR(r)->vec.items[i++] = SCM_PTR(p)->pair.car;
    return r;
}

PRIM(display) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], false);
    return SCM_UNSPEC;
}
PRIM(write) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], true);
    return SCM_UNSPEC;
}
PRIM(newline) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    fputc('\n', fp);
    return SCM_UNSPEC;
}
PRIM(write_char) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    fputc((int)SCM_PTR(argv[0])->ch, fp);
    return SCM_UNSPEC;
}

PRIM(read_form) { (void)argc; (void)argv; return scm_read(c, stdin); }
PRIM(eof_p)     { (void)c; (void)argc; return argv[0] == SCM_EOFV ? SCM_TRUE : SCM_FALSE; }

PRIM(error_p)   { (void)argc; scm_error(c, "%s", scm_is_string(argv[0]) ? SCM_PTR(argv[0])->str.chars : "error"); }

PRIM(apply_p) {
    if (argc < 2) scm_error(c, "apply: needs at least 2 args");
    VALUE fn = argv[0];
    int prefix = argc - 2;
    VALUE list = argv[argc - 1];
    int extra = 0;
    for (VALUE p = list; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) extra++;
    int total = prefix + extra;
    VALUE *all = (VALUE *)GC_malloc(sizeof(VALUE) * (total ? total : 1));
    for (int i = 0; i < prefix; i++) all[i] = argv[1 + i];
    int i = prefix;
    for (VALUE p = list; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr, i++) all[i] = SCM_PTR(p)->pair.car;
    return scm_apply(c, fn, total, all);
}

// (map fn list1 list2 ...) — applies fn elementwise to len of shortest.
PRIM(map_p) {
    if (argc < 2) scm_error(c, "map: needs fn + list");
    VALUE fn = argv[0];
    int nlists = argc - 1;
    VALUE *cursors = (VALUE *)GC_malloc(sizeof(VALUE) * nlists);
    for (int i = 0; i < nlists; i++) cursors[i] = argv[i + 1];
    VALUE result = SCM_NIL, *tail = &result;
    for (;;) {
        for (int i = 0; i < nlists; i++) if (!scm_is_pair(cursors[i])) return result;
        VALUE *args = (VALUE *)GC_malloc(sizeof(VALUE) * nlists);
        for (int i = 0; i < nlists; i++) {
            args[i] = SCM_PTR(cursors[i])->pair.car;
            cursors[i] = SCM_PTR(cursors[i])->pair.cdr;
        }
        VALUE v = scm_apply(c, fn, nlists, args);
        *tail = scm_cons(v, SCM_NIL);
        tail = &SCM_PTR(*tail)->pair.cdr;
    }
}

PRIM(for_each_p) {
    if (argc < 2) scm_error(c, "for-each: needs fn + list");
    VALUE fn = argv[0];
    int nlists = argc - 1;
    VALUE *cursors = (VALUE *)GC_malloc(sizeof(VALUE) * nlists);
    for (int i = 0; i < nlists; i++) cursors[i] = argv[i + 1];
    for (;;) {
        for (int i = 0; i < nlists; i++) if (!scm_is_pair(cursors[i])) return SCM_UNSPEC;
        VALUE *args = (VALUE *)GC_malloc(sizeof(VALUE) * nlists);
        for (int i = 0; i < nlists; i++) {
            args[i] = SCM_PTR(cursors[i])->pair.car;
            cursors[i] = SCM_PTR(cursors[i])->pair.cdr;
        }
        scm_apply(c, fn, nlists, args);
    }
}

#include <time.h>
PRIM(time_now) {
    (void)c; (void)argc; (void)argv;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return scm_make_double(ts.tv_sec + ts.tv_nsec * 1e-9);
}

PRIM(exit_p) {
    (void)c;
    int code = 0;
    if (argc > 0 && SCM_IS_FIXNUM(argv[0])) code = (int)SCM_FIXVAL(argv[0]);
    exit(code);
}

PRIM(gensym) {
    (void)argc; (void)argv;
    static int seq = 0;
    char buf[64]; snprintf(buf, sizeof(buf), "|g%d|", ++seq);
    (void)c;
    return scm_intern(buf);
}

// --- Promises (delay/force) -------------------------------------------------
//
// (delay expr) is lowered at compile time to (|make-promise| (lambda () expr));
// `force` runs the thunk on first call and caches the result thereafter.
// R5RS §6.4 allows `force` on a non-promise to act as identity.

PRIM(make_promise) {
    (void)argc;
    if (!scm_is_proc(argv[0])) scm_error(c, "delay: thunk must be a procedure");
    struct sobj *o = scm_alloc(OBJ_PROMISE);
    o->promise.thunk = argv[0];
    o->promise.value = SCM_UNSPEC;
    o->promise.forced = false;
    return SCM_OBJ_VAL(o);
}

PRIM(force_p) {
    (void)argc;
    VALUE p = argv[0];
    if (!scm_is_promise(p)) return p;
    struct sobj *po = SCM_PTR(p);
    if (po->promise.forced) return po->promise.value;
    VALUE result = scm_apply(c, po->promise.thunk, 0, NULL);
    // Re-check after recursive force could have already memoized us.
    if (!po->promise.forced) {
        po->promise.value = result;
        po->promise.forced = true;
    }
    return po->promise.value;
}

PRIM(promise_p) { (void)c; (void)argc; return scm_is_promise(argv[0]) ? SCM_TRUE : SCM_FALSE; }

// --- Ports ------------------------------------------------------------------

static VALUE
port_make(FILE *fp, bool input, bool owned)
{
    struct sobj *o = scm_alloc(OBJ_PORT);
    o->port.fp = fp;
    o->port.input = input;
    o->port.closed = false;
    o->port.owned = owned;
    return SCM_OBJ_VAL(o);
}

static VALUE PORT_STDIN  = 0;
static VALUE PORT_STDOUT = 0;
static VALUE PORT_STDERR = 0;

PRIM(open_input_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "open-input-file: not a string");
    FILE *fp = fopen(SCM_PTR(argv[0])->str.chars, "r");
    if (!fp) scm_error(c, "open-input-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    return port_make(fp, true, true);
}
PRIM(open_output_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "open-output-file: not a string");
    FILE *fp = fopen(SCM_PTR(argv[0])->str.chars, "w");
    if (!fp) scm_error(c, "open-output-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    return port_make(fp, false, true);
}
PRIM(close_input_port) {
    (void)argc;
    if (scm_is_port(argv[0])) {
        struct sobj *p = SCM_PTR(argv[0]);
        if (p->port.owned && !p->port.closed) { fclose(p->port.fp); p->port.closed = true; }
    }
    (void)c; return SCM_UNSPEC;
}
PRIM(close_output_port) { return prim_close_input_port(c, argc, argv); }
PRIM(input_port_p)  { (void)c; (void)argc; return scm_is_port(argv[0]) && SCM_PTR(argv[0])->port.input  ? SCM_TRUE : SCM_FALSE; }
PRIM(output_port_p) { (void)c; (void)argc; return scm_is_port(argv[0]) && !SCM_PTR(argv[0])->port.input ? SCM_TRUE : SCM_FALSE; }
PRIM(port_p)        { (void)c; (void)argc; return scm_is_port(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(current_input_port)  { (void)c; (void)argc; (void)argv; return PORT_STDIN; }
PRIM(current_output_port) { (void)c; (void)argc; (void)argv; return PORT_STDOUT; }
PRIM(read_char) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    int ch = fgetc(fp);
    return ch == EOF ? SCM_EOFV : scm_make_char((uint32_t)ch);
}
PRIM(peek_char) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    int ch = fgetc(fp);
    if (ch == EOF) return SCM_EOFV;
    ungetc(ch, fp);
    return scm_make_char((uint32_t)ch);
}
PRIM(char_ready_p) { (void)c; (void)argc; (void)argv; return SCM_TRUE; }   // simplified
PRIM(write_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], true);
    return SCM_UNSPEC;
}
PRIM(display_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], false);
    return SCM_UNSPEC;
}
PRIM(newline_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    fputc('\n', fp);
    return SCM_UNSPEC;
}
PRIM(write_char_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    fputc((int)SCM_PTR(argv[0])->ch, fp);
    return SCM_UNSPEC;
}
PRIM(read_from_port) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    return scm_read(c, fp);
}
// Re-route stdin / stdout for the duration of `thunk`'s execution.  We
// save the original fd via dup() and restore via dup2() so the redirection
// is reverted on return — `freopen` would have left it permanently.
#include <fcntl.h>
#include <unistd.h>

PRIM(with_input_from_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "with-input-from-file: not a string");
    if (!scm_is_proc(argv[1]))   scm_error(c, "with-input-from-file: not a procedure");
    int new_fd = open(SCM_PTR(argv[0])->str.chars, O_RDONLY);
    if (new_fd < 0) scm_error(c, "with-input-from-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    int saved = dup(fileno(stdin));
    dup2(new_fd, fileno(stdin)); close(new_fd);
    VALUE r = scm_apply(c, argv[1], 0, NULL);
    dup2(saved, fileno(stdin)); close(saved);
    clearerr(stdin);
    return r;
}
PRIM(with_output_to_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "with-output-to-file: not a string");
    if (!scm_is_proc(argv[1]))   scm_error(c, "with-output-to-file: not a procedure");
    fflush(stdout);
    int new_fd = open(SCM_PTR(argv[0])->str.chars, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (new_fd < 0) scm_error(c, "with-output-to-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    int saved = dup(fileno(stdout));
    dup2(new_fd, fileno(stdout)); close(new_fd);
    VALUE r = scm_apply(c, argv[1], 0, NULL);
    fflush(stdout);
    dup2(saved, fileno(stdout)); close(saved);
    clearerr(stdout);
    return r;
}

// --- Multiple values --------------------------------------------------------

PRIM(values_p) {
    (void)c;
    if (argc == 1) return argv[0];
    return scm_make_mvalues(argc, argv);
}

PRIM(call_with_values_p) {
    (void)argc;
    VALUE producer = argv[0];
    VALUE consumer = argv[1];
    if (!scm_is_proc(producer)) scm_error(c, "call-with-values: producer not a procedure");
    if (!scm_is_proc(consumer)) scm_error(c, "call-with-values: consumer not a procedure");
    VALUE r = scm_apply(c, producer, 0, NULL);
    if (scm_is_mvalues(r)) {
        struct sobj *m = SCM_PTR(r);
        return scm_apply(c, consumer, (int)m->mv.len, m->mv.items);
    }
    VALUE single[1] = { r };
    return scm_apply(c, consumer, 1, single);
}

// --- Complex numbers --------------------------------------------------------

PRIM(make_rectangular) {
    (void)argc; (void)c;
    return scm_simplify_complex(scm_get_double(argv[0]), scm_get_double(argv[1]));
}
PRIM(make_polar) {
    (void)argc; (void)c;
    double m = scm_get_double(argv[0]);
    double a = scm_get_double(argv[1]);
    return scm_simplify_complex(m * cos(a), m * sin(a));
}
PRIM(real_part) {
    (void)argc;
    if (scm_is_complex(argv[0])) return scm_make_double(SCM_PTR(argv[0])->cpx.re);
    if (scm_is_real(argv[0])) return argv[0];
    scm_error(c, "real-part: not a number");
}
PRIM(imag_part) {
    (void)argc;
    if (scm_is_complex(argv[0])) return scm_make_double(SCM_PTR(argv[0])->cpx.im);
    if (scm_is_real(argv[0])) return SCM_FIX(0);
    scm_error(c, "imag-part: not a number");
}
PRIM(magnitude) {
    (void)argc;
    if (scm_is_complex(argv[0])) {
        double r = SCM_PTR(argv[0])->cpx.re, i = SCM_PTR(argv[0])->cpx.im;
        return scm_make_double(sqrt(r*r + i*i));
    }
    return prim_abs(c, argc, argv);
}
PRIM(angle) {
    (void)argc;
    if (scm_is_complex(argv[0])) {
        double r = SCM_PTR(argv[0])->cpx.re, i = SCM_PTR(argv[0])->cpx.im;
        return scm_make_double(atan2(i, r));
    }
    if (cmp2(c, argv[0], SCM_FIX(0)) >= 0) return scm_make_double(0.0);
    return scm_make_double(M_PI);
}

// --- Rational accessors -----------------------------------------------------

PRIM(numerator_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0]) || scm_is_bignum(argv[0])) return argv[0];
    if (scm_is_rational(argv[0])) return scm_normalize_int(mpq_numref(SCM_PTR(argv[0])->mpq));
    scm_error(c, "numerator: not a rational");
}
PRIM(denominator_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0]) || scm_is_bignum(argv[0])) return SCM_FIX(1);
    if (scm_is_rational(argv[0])) return scm_normalize_int(mpq_denref(SCM_PTR(argv[0])->mpq));
    scm_error(c, "denominator: not a rational");
}

// Simple math primitives.
PRIM(sqrt_p) { (void)c; (void)argc; return scm_make_double(sqrt(scm_get_double(argv[0]))); }
PRIM(expt_p) {
    (void)argc;
    // exact integer base + non-negative exact integer exponent → exact via mpz_pow_ui
    if (scm_is_exact(argv[0]) && scm_is_integer_value(argv[0]) &&
        scm_is_exact(argv[1]) && scm_is_integer_value(argv[1]) &&
        cmp2(c, argv[1], SCM_FIX(0)) >= 0)
    {
        mpz_t base, r;
        to_mpz(argv[0], base); mpz_init(r);
        unsigned long e;
        if (SCM_IS_FIXNUM(argv[1])) e = (unsigned long)SCM_FIXVAL(argv[1]);
        else if (mpz_fits_ulong_p(SCM_PTR(argv[1])->mpz)) e = mpz_get_ui(SCM_PTR(argv[1])->mpz);
        else { mpz_clear(base); mpz_clear(r); scm_error(c, "expt: exponent too large"); }
        mpz_pow_ui(r, base, e);
        VALUE rv = scm_normalize_int(r);
        mpz_clear(base); mpz_clear(r);
        return rv;
    }
    return scm_make_double(pow(scm_get_double(argv[0]), scm_get_double(argv[1])));
}
PRIM(floor_p)    { (void)c; (void)argc; return scm_make_double(floor(scm_get_double(argv[0]))); }
PRIM(ceiling_p)  { (void)c; (void)argc; return scm_make_double(ceil(scm_get_double(argv[0]))); }
PRIM(truncate_p) { (void)c; (void)argc; return scm_make_double(trunc(scm_get_double(argv[0]))); }
PRIM(round_p)    { (void)c; (void)argc; return scm_make_double(round(scm_get_double(argv[0]))); }
PRIM(log_p)      { (void)c; (void)argc; return scm_make_double(log(scm_get_double(argv[0]))); }
PRIM(exp_p)      { (void)c; (void)argc; return scm_make_double(exp(scm_get_double(argv[0]))); }
PRIM(sin_p)      { (void)c; (void)argc; return scm_make_double(sin(scm_get_double(argv[0]))); }
PRIM(cos_p)      { (void)c; (void)argc; return scm_make_double(cos(scm_get_double(argv[0]))); }
PRIM(tan_p)      { (void)c; (void)argc; return scm_make_double(tan(scm_get_double(argv[0]))); }
PRIM(atan_p)     { (void)c; (void)argc; return scm_make_double(atan(scm_get_double(argv[0]))); }

// car/cdr compositions: caar, cadr, ..., caddr.  Generated via macros.
#define CADR_OP(x) cdr(x)
#define CAAR_OP(x) car(x)
PRIM(caar) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.car)->pair.car; }
PRIM(cadr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.car; }
PRIM(cdar) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.car)->pair.cdr; }
PRIM(cddr) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr; }
PRIM(caddr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.car; }
PRIM(cdddr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.cdr; }
PRIM(cadddr_p){ (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.cdr)->pair.car; }

// ---------------------------------------------------------------------------
// Primitive table.
// ---------------------------------------------------------------------------

static struct prim_entry {
    const char *name;
    scm_prim_fn fn;
    int min_argc, max_argc;
} PRIM_TABLE[] = {
    { "+", prim_plus, 0, -1 },
    { "-", prim_minus, 1, -1 },
    { "*", prim_mul, 0, -1 },
    { "/", prim_div, 1, -1 },
    { "=", prim_num_eq, 2, -1 },
    { "<", prim_num_lt, 2, -1 },
    { ">", prim_num_gt, 2, -1 },
    { "<=", prim_num_le, 2, -1 },
    { ">=", prim_num_ge, 2, -1 },
    { "modulo", prim_modulo, 2, 2 },
    { "remainder", prim_remainder, 2, 2 },
    { "quotient", prim_quotient, 2, 2 },
    { "abs", prim_abs, 1, 1 },
    { "min", prim_min, 1, -1 },
    { "max", prim_max, 1, -1 },
    { "zero?", prim_zero_p, 1, 1 },
    { "positive?", prim_positive_p, 1, 1 },
    { "negative?", prim_negative_p, 1, 1 },
    { "odd?", prim_odd_p, 1, 1 },
    { "even?", prim_even_p, 1, 1 },
    { "number?", prim_number_p, 1, 1 },
    { "integer?", prim_integer_p, 1, 1 },
    { "real?", prim_real_p, 1, 1 },
    { "rational?", prim_rational_p, 1, 1 },
    { "complex?", prim_complex_p, 1, 1 },
    { "exact?", prim_exact_p, 1, 1 },
    { "inexact?", prim_inexact_p, 1, 1 },
    { "exact->inexact", prim_exact_to_inexact, 1, 1 },
    { "inexact->exact", prim_inexact_to_exact, 1, 1 },
    { "expt", prim_expt_p, 2, 2 },
    { "gcd", prim_gcd_p, 0, -1 },
    { "lcm", prim_lcm_p, 0, -1 },
    { "numerator", prim_numerator_p, 1, 1 },
    { "denominator", prim_denominator_p, 1, 1 },
    { "make-rectangular", prim_make_rectangular, 2, 2 },
    { "make-polar", prim_make_polar, 2, 2 },
    { "real-part", prim_real_part, 1, 1 },
    { "imag-part", prim_imag_part, 1, 1 },
    { "magnitude", prim_magnitude, 1, 1 },
    { "angle", prim_angle, 1, 1 },
    { "values", prim_values_p, 0, -1 },
    { "call-with-values", prim_call_with_values_p, 2, 2 },
    { "|make-promise|", prim_make_promise, 1, 1 },
    { "force", prim_force_p, 1, 1 },
    { "promise?", prim_promise_p, 1, 1 },
    { "open-input-file", prim_open_input_file, 1, 1 },
    { "open-output-file", prim_open_output_file, 1, 1 },
    { "close-input-port", prim_close_input_port, 1, 1 },
    { "close-output-port", prim_close_output_port, 1, 1 },
    { "close-port", prim_close_input_port, 1, 1 },
    { "input-port?", prim_input_port_p, 1, 1 },
    { "output-port?", prim_output_port_p, 1, 1 },
    { "port?", prim_port_p, 1, 1 },
    { "current-input-port", prim_current_input_port, 0, 0 },
    { "current-output-port", prim_current_output_port, 0, 0 },
    { "read-char", prim_read_char, 0, 1 },
    { "peek-char", prim_peek_char, 0, 1 },
    { "char-ready?", prim_char_ready_p, 0, 1 },
    { "with-input-from-file", prim_with_input_from_file, 2, 2 },
    { "with-output-to-file", prim_with_output_to_file, 2, 2 },
    { "sqrt", prim_sqrt_p, 1, 1 },
    { "floor", prim_floor_p, 1, 1 },
    { "ceiling", prim_ceiling_p, 1, 1 },
    { "truncate", prim_truncate_p, 1, 1 },
    { "round", prim_round_p, 1, 1 },
    { "log", prim_log_p, 1, 1 },
    { "exp", prim_exp_p, 1, 1 },
    { "sin", prim_sin_p, 1, 1 },
    { "cos", prim_cos_p, 1, 1 },
    { "tan", prim_tan_p, 1, 1 },
    { "atan", prim_atan_p, 1, 1 },
    { "cons", prim_cons, 2, 2 },
    { "car", prim_car, 1, 1 },
    { "cdr", prim_cdr, 1, 1 },
    { "set-car!", prim_set_car, 2, 2 },
    { "set-cdr!", prim_set_cdr, 2, 2 },
    { "pair?", prim_pair_p, 1, 1 },
    { "null?", prim_null_p, 1, 1 },
    { "list", prim_list, 0, -1 },
    { "list?", prim_list_p, 1, 1 },
    { "length", prim_length, 1, 1 },
    { "reverse", prim_reverse, 1, 1 },
    { "append", prim_append, 0, -1 },
    { "list-ref", prim_list_ref, 2, 2 },
    { "list-tail", prim_list_tail, 2, 2 },
    { "caar", prim_caar, 1, 1 },
    { "cadr", prim_cadr_p, 1, 1 },
    { "cdar", prim_cdar, 1, 1 },
    { "cddr", prim_cddr, 1, 1 },
    { "caddr", prim_caddr_p, 1, 1 },
    { "cdddr", prim_cdddr_p, 1, 1 },
    { "cadddr", prim_cadddr_p, 1, 1 },
    { "eq?", prim_eq_p, 2, 2 },
    { "eqv?", prim_eqv_p, 2, 2 },
    { "equal?", prim_equal_p, 2, 2 },
    { "memq", prim_memq, 2, 2 },
    { "memv", prim_memv, 2, 2 },
    { "member", prim_member, 2, 2 },
    { "assq", prim_assq, 2, 2 },
    { "assv", prim_assv, 2, 2 },
    { "assoc", prim_assoc, 2, 2 },
    { "boolean?", prim_boolean_p, 1, 1 },
    { "not", prim_not_p, 1, 1 },
    { "symbol?", prim_symbol_p, 1, 1 },
    { "procedure?", prim_procedure_p, 1, 1 },
    { "string?", prim_string_p, 1, 1 },
    { "char?", prim_char_p, 1, 1 },
    { "vector?", prim_vector_p, 1, 1 },
    { "symbol->string", prim_symbol_to_string, 1, 1 },
    { "string->symbol", prim_string_to_symbol, 1, 1 },
    { "number->string", prim_number_to_string, 1, 2 },
    { "string->number", prim_string_to_number, 1, 2 },
    { "string-length", prim_string_length, 1, 1 },
    { "string-ref", prim_string_ref, 2, 2 },
    { "string=?", prim_string_eq, 2, -1 },
    { "string<?", prim_string_lt, 2, -1 },
    { "string>?", prim_string_gt, 2, -1 },
    { "string<=?", prim_string_le, 2, -1 },
    { "string>=?", prim_string_ge, 2, -1 },
    { "string-ci=?", prim_string_ci_eq, 2, -1 },
    { "char-ci=?", prim_char_ci_eq, 2, -1 },
    { "string-copy", prim_string_copy, 1, 1 },
    { "string-set!", prim_string_set, 3, 3 },
    { "make-string", prim_make_string, 1, 2 },
    { "string-append", prim_string_append, 0, -1 },
    { "substring", prim_substring, 3, 3 },
    { "string->list", prim_string_to_list, 1, 1 },
    { "list->string", prim_list_to_string, 1, 1 },
    { "string", prim_string_form, 0, -1 },
    { "char=?", prim_char_eq, 2, -1 },
    { "char<?", prim_char_lt, 2, -1 },
    { "char<=?", prim_char_le, 2, -1 },
    { "char>?", prim_char_gt, 2, -1 },
    { "char>=?", prim_char_ge, 2, -1 },
    { "char->integer", prim_char_to_integer, 1, 1 },
    { "integer->char", prim_integer_to_char, 1, 1 },
    { "char-alphabetic?", prim_char_alphabetic_p, 1, 1 },
    { "char-numeric?", prim_char_numeric_p, 1, 1 },
    { "char-whitespace?", prim_char_whitespace_p, 1, 1 },
    { "char-upper-case?", prim_char_upper_p, 1, 1 },
    { "char-lower-case?", prim_char_lower_p, 1, 1 },
    { "char-upcase", prim_char_upcase, 1, 1 },
    { "char-downcase", prim_char_downcase, 1, 1 },
    { "make-vector", prim_make_vector, 1, 2 },
    { "vector", prim_vector_form, 0, -1 },
    { "vector-length", prim_vector_length, 1, 1 },
    { "vector-ref", prim_vector_ref, 2, 2 },
    { "vector-set!", prim_vector_set, 3, 3 },
    { "vector-fill!", prim_vector_fill, 2, 2 },
    { "vector->list", prim_vector_to_list, 1, 1 },
    { "list->vector", prim_list_to_vector, 1, 1 },
    { "display", prim_display, 1, 2 },
    { "write", prim_write, 1, 2 },
    { "newline", prim_newline, 0, 1 },
    { "write-char", prim_write_char, 1, 2 },
    { "read", prim_read_from_port, 0, 1 },
    { "eof-object?", prim_eof_p, 1, 1 },
    { "error", prim_error_p, 1, -1 },
    { "apply", prim_apply_p, 2, -1 },
    { "map", prim_map_p, 2, -1 },
    { "for-each", prim_for_each_p, 2, -1 },
    { "current-time", prim_time_now, 0, 0 },
    { "exit", prim_exit_p, 0, 1 },
    { "gensym", prim_gensym, 0, 0 },
    { NULL, NULL, 0, 0 }
};

VALUE PRIM_PLUS_VAL, PRIM_MINUS_VAL, PRIM_MUL_VAL;
VALUE PRIM_NUM_LT_VAL, PRIM_NUM_LE_VAL, PRIM_NUM_GT_VAL, PRIM_NUM_GE_VAL, PRIM_NUM_EQ_VAL;
VALUE PRIM_NULL_P_VAL, PRIM_PAIR_P_VAL, PRIM_CAR_VAL, PRIM_CDR_VAL, PRIM_NOT_VAL;
VALUE PRIM_VECTOR_REF_VAL, PRIM_VECTOR_SET_VAL;
VALUE PRIM_CONS_VAL, PRIM_EQ_P_VAL, PRIM_EQV_P_VAL;

static void
install_prims(CTX *c)
{
    for (struct prim_entry *p = PRIM_TABLE; p->name; p++) {
        VALUE v = scm_make_prim(p->name, p->fn, p->min_argc, p->max_argc);
        scm_global_define(c, p->name, v);
    }
    // Snapshot the original prim sobj for every specialized operator.
    // node_arith_<op>'s fast path compares the live global value at its
    // cached index against the snapshot; a mismatch means the user has
    // rebound the operator and we must fall through to general dispatch.
    PRIM_PLUS_VAL   = scm_global_ref(c, "+");
    PRIM_MINUS_VAL  = scm_global_ref(c, "-");
    PRIM_MUL_VAL    = scm_global_ref(c, "*");
    PRIM_NUM_LT_VAL = scm_global_ref(c, "<");
    PRIM_NUM_LE_VAL = scm_global_ref(c, "<=");
    PRIM_NUM_GT_VAL = scm_global_ref(c, ">");
    PRIM_NUM_GE_VAL = scm_global_ref(c, ">=");
    PRIM_NUM_EQ_VAL = scm_global_ref(c, "=");

    // Standard ports.  These are wrappers around the libc FILE* and not
    // owned (close-input-port / close-output-port leaves stdin/out/err
    // alone).
    PORT_STDIN  = port_make(stdin,  true,  false);
    PORT_STDOUT = port_make(stdout, false, false);
    PORT_STDERR = port_make(stderr, false, false);

    PRIM_NULL_P_VAL     = scm_global_ref(c, "null?");
    PRIM_PAIR_P_VAL     = scm_global_ref(c, "pair?");
    PRIM_CAR_VAL        = scm_global_ref(c, "car");
    PRIM_CDR_VAL        = scm_global_ref(c, "cdr");
    PRIM_NOT_VAL        = scm_global_ref(c, "not");
    PRIM_VECTOR_REF_VAL = scm_global_ref(c, "vector-ref");
    PRIM_VECTOR_SET_VAL = scm_global_ref(c, "vector-set!");
    PRIM_CONS_VAL       = scm_global_ref(c, "cons");
    PRIM_EQ_P_VAL       = scm_global_ref(c, "eq?");
    PRIM_EQV_P_VAL      = scm_global_ref(c, "eqv?");
}

// Slow-path helpers for the specialized arith / pred / vec nodes — used
// when the inline cache is cold or the user has rebound the operator.
// Refresh the (serial, value) cache pair against the current globals.

static VALUE
arith_refresh(CTX *c, struct arith_cache *cache, const char *opname)
{
    VALUE v = scm_global_ref(c, opname);
    cache->value  = v;
    cache->serial = c->globals_serial;
    return v;
}

VALUE
arith_dispatch1(CTX *c, struct arith_cache *cache, const char *opname, VALUE av)
{
    VALUE fn = (cache->serial == c->globals_serial)
                 ? cache->value
                 : arith_refresh(c, cache, opname);
    return scm_apply(c, fn, 1, &av);
}

VALUE
arith_dispatch(CTX *c, struct arith_cache *cache, const char *opname, VALUE av, VALUE bv)
{
    VALUE fn = (cache->serial == c->globals_serial)
                 ? cache->value
                 : arith_refresh(c, cache, opname);
    VALUE args[2] = { av, bv };
    return scm_apply(c, fn, 2, args);
}

VALUE
arith_dispatch3(CTX *c, struct arith_cache *cache, const char *opname, VALUE a, VALUE b, VALUE d)
{
    VALUE fn = (cache->serial == c->globals_serial)
                 ? cache->value
                 : arith_refresh(c, cache, opname);
    VALUE args[3] = { a, b, d };
    return scm_apply(c, fn, 3, args);
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

static CTX *
create_context(void)
{
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    memset(c, 0, sizeof(CTX));
    c->env = NULL;
    c->globals_serial = 1;   // any cache with serial==0 is uninitialised
    install_prims(c);
    return c;
}

static VALUE
eval_top(CTX *c, NODE *body)
{
    c->tail_call_pending = 0;
    return EVAL(c, body);
}

// ---------------------------------------------------------------------------
// Profile-guided entry selection.
//
// Modeled on abruby's PGO machinery (sample/abruby/abruby_gen.rb registers
// HOPT + PROFILE tasks; we implement a stripped-down version).  The flow:
//
//   1. `--profile run.scm` runs interpretively; scm_apply increments
//      `body->head.dispatch_cnt` on each closure entry.  At exit we walk
//      AOT_ENTRIES and dump (Horg, count) tuples to code_store/profile.txt.
//
//   2. `--use-profile -c run.scm` loads the profile and, during
//      `aot_compile_and_load`, skips entries whose recorded count is below
//      AOT_PROFILE_THRESHOLD.  Cold entries keep their default dispatcher,
//      so make/gcc only burns time on the hot ones — typically 10× fewer
//      entries means 10× faster cold AOT and a smaller all.so.
//
// abruby goes further by emitting a separate Hopt-keyed PGSD_<Hopt>
// variant that bakes profile-derived constants (method prologues, etc.)
// into the generated C; ascheme's specialized nodes already inline their
// hot-path constants via PRIM_*_VAL, so we get most of the same benefit
// without a parallel hash.

#define AOT_PROFILE_THRESHOLD 10

struct profile_entry {
    node_hash_t horg;
    uint32_t    count;
};

static struct profile_entry *PROFILE_DATA = NULL;
static size_t PROFILE_LEN = 0;
static size_t PROFILE_CAPA = 0;
static bool   PROFILE_LOADED = false;

static void
profile_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "code_store/profile.txt");
}

static void
profile_dump(void)
{
    char path[256]; profile_path(path, sizeof(path));
    (void)!system("mkdir -p code_store");
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "# ascheme profile: <Horg-hex> <count>\n");
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        NODE *n = AOT_ENTRIES[i];
        if (n->head.dispatch_cnt == 0) continue;
        fprintf(fp, "%lx %u\n",
                (unsigned long)HASH(n), n->head.dispatch_cnt);
    }
    fclose(fp);
}

static void
profile_load(void)
{
    char path[256]; profile_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    PROFILE_LOADED = true;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long horg; unsigned int count;
        if (sscanf(line, "%lx %u", &horg, &count) != 2) continue;
        if (PROFILE_LEN == PROFILE_CAPA) {
            PROFILE_CAPA = PROFILE_CAPA ? PROFILE_CAPA * 2 : 64;
            PROFILE_DATA = (struct profile_entry *)GC_realloc(PROFILE_DATA,
                                sizeof(struct profile_entry) * PROFILE_CAPA);
        }
        PROFILE_DATA[PROFILE_LEN].horg = (node_hash_t)horg;
        PROFILE_DATA[PROFILE_LEN].count = count;
        PROFILE_LEN++;
    }
    fclose(fp);
}

static uint32_t
profile_lookup(node_hash_t h)
{
    for (size_t i = 0; i < PROFILE_LEN; i++)
        if (PROFILE_DATA[i].horg == h) return PROFILE_DATA[i].count;
    return 0;
}

// AOT compile each registered entry, build all.so, reload, then patch every
// entry's dispatcher from the freshly-loaded shared object.  Returns the
// number of entries that successfully loaded a specialized SD_<hash>.
static size_t
aot_compile_and_load(bool verbose)
{
    size_t skipped = 0;
    if (verbose) {
        if (PROFILE_LOADED)
            fprintf(stderr, "ascheme: AOT compiling (profile-guided, threshold=%d, %zu entries)...\n",
                    AOT_PROFILE_THRESHOLD, AOT_ENTRIES_LEN);
        else
            fprintf(stderr, "ascheme: AOT compiling %zu entries...\n", AOT_ENTRIES_LEN);
    }
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        if (PROFILE_LOADED) {
            uint32_t count = profile_lookup(HASH(AOT_ENTRIES[i]));
            if (count < AOT_PROFILE_THRESHOLD) { skipped++; continue; }
        }
        astro_cs_compile(AOT_ENTRIES[i], NULL);
    }
    if (verbose && skipped > 0)
        fprintf(stderr, "ascheme: skipped %zu cold entries\n", skipped);
    if (verbose) fprintf(stderr, "ascheme: building all.so (-O3 -lgc -lgmp)...\n");
    // SD_*.c needs gc.h-free build; we link gc/gmp via the host (-rdynamic).
    // Disable ccache: it tries to write to its cache dir which may be on a
    // read-only / sandboxed FS.  Setting CCACHE_DISABLE turns ccache into a
    // pass-through to the underlying gcc.
    setenv("CCACHE_DISABLE", "1", 1);
    astro_cs_build(NULL);
    astro_cs_reload();
    OPTION.no_compiled_code = false;        // OPTIMIZE will load via cs_load
    size_t loaded = 0, dedup = 0;
    // Dedup entries that hash-collide so we don't double-count.  Two entries
    // with the same hash share the same SD_<hash> function in all.so; one
    // load patches both dispatchers if we cared, but we just track unique
    // hashes for the verbose report.
    node_hash_t *seen = (node_hash_t *)GC_malloc(sizeof(node_hash_t) * AOT_ENTRIES_LEN);
    size_t seen_n = 0;
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        node_hash_t h = HASH(AOT_ENTRIES[i]);
        bool already = false;
        for (size_t j = 0; j < seen_n; j++) if (seen[j] == h) { already = true; break; }
        if (already) { dedup++; continue; }
        seen[seen_n++] = h;
        if (astro_cs_load(AOT_ENTRIES[i], NULL)) loaded++;
    }
    if (verbose) fprintf(stderr, "ascheme: loaded %zu / %zu specialized dispatchers (%zu duplicate hashes)\n",
                         loaded, seen_n, dedup);
    return loaded;
}

static int
run_string(CTX *c, const char *src, size_t len, bool print_results)
{
    // Per-form error recovery: a script that mis-types one expression
    // shouldn't abort the rest.  Each iteration installs a fresh setjmp
    // landing pad; on error we print and move on, accumulating an error
    // count for the exit status.
    VALUE forms;
    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        return 1;
    }
    c->err_jmp_active = 1;
    forms = scm_read_all_string(c, src, len);
    int errors = 0;
    for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        VALUE form = SCM_PTR(p)->pair.car;
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            continue;
        }
        c->err_jmp_active = 1;
        NODE *ast = compile(c, form, NULL, false);
        VALUE r = eval_top(c, ast);
        if (print_results && r != SCM_UNSPEC) {
            scm_display(stdout, r, true);
            putchar('\n');
        }
    }
    c->err_jmp_active = 0;
    return errors > 0 ? 1 : 0;
}

// One-shot profile-guided compilation, modeled on abruby's --pg-compile.
// We run the program interpretively (no AOT applied during the run) so
// `body->head.dispatch_cnt` accumulates true execution counts; on exit we
// AOT-compile entries above the threshold and persist the resulting
// code_store/ for the *next* invocation to consume via `-c`.
static int
run_file_pg_compile(CTX *c, const char *path, bool verbose)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)GC_malloc_atomic(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);

    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        return 1;
    }
    c->err_jmp_active = 1;

    // Parse + compile to collect entries (no AOT yet).
    VALUE forms = scm_read_all_string(c, buf, (size_t)sz);
    NODE **asts = (NODE **)GC_malloc(sizeof(NODE *) * (list_length(forms) + 1));
    int nasts = 0;
    for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        asts[nasts] = compile(c, SCM_PTR(p)->pair.car, NULL, false);
        aot_add_entry(asts[nasts]);
        nasts++;
    }

    // Run interpretively — this populates dispatch_cnt on every body.
    int errors = 0;
    for (int i = 0; i < nasts; i++) {
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            continue;
        }
        c->err_jmp_active = 1;
        eval_top(c, asts[i]);
    }
    c->err_jmp_active = 0;

    // Synthesize a profile from the live counters.  We reuse the
    // file-loading path so aot_compile_and_load filters cold entries.
    PROFILE_LOADED = true;
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        NODE *n = AOT_ENTRIES[i];
        if (n->head.dispatch_cnt == 0) continue;
        if (PROFILE_LEN == PROFILE_CAPA) {
            PROFILE_CAPA = PROFILE_CAPA ? PROFILE_CAPA * 2 : 64;
            PROFILE_DATA = (struct profile_entry *)GC_realloc(PROFILE_DATA,
                                sizeof(struct profile_entry) * PROFILE_CAPA);
        }
        PROFILE_DATA[PROFILE_LEN].horg = HASH(n);
        PROFILE_DATA[PROFILE_LEN].count = n->head.dispatch_cnt;
        PROFILE_LEN++;
    }
    if (verbose) fprintf(stderr, "ascheme: --pg-compile: synthesized profile (%zu entries with count > 0)\n", PROFILE_LEN);

    // Compile hot entries; cache them on disk via astro_cs_build.  The
    // freshly-loaded SDs go unused here (we already finished the run),
    // but the next `ascheme -c` invocation picks them up from
    // code_store/all.so.
    aot_compile_and_load(verbose);

    // Persist a textual copy too — useful for inspection / sharing
    // profiles across machines without a binary code store.
    profile_dump();

    return errors > 0 ? 1 : 0;
}

// Two-pass execution for AOT mode: first parse + compile every form,
// register entries, AOT-compile, build, reload, load — then evaluate.
static int
run_file_aot(CTX *c, const char *path, bool verbose)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)GC_malloc_atomic(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);

    // If a profile from a prior `--pg-compile` exists in the code store,
    // pick it up automatically.  This makes `-c` after `--pg-compile`
    // behave as a pure cache-load for hot entries — cold entries are
    // skipped (left running on the default dispatcher) instead of being
    // compiled on the spot.
    profile_load();

    // Pass 1 (parse+compile) runs under one error envelope — mid-parse
    // errors abort the whole pass.  Pass 3 re-arms setjmp per form so a
    // runtime error in form N doesn't squash the rest of the script.
    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        return 1;
    }
    c->err_jmp_active = 1;

    VALUE forms = scm_read_all_string(c, buf, (size_t)sz);
    NODE **asts = (NODE **)GC_malloc(sizeof(NODE *) * (list_length(forms) + 1));
    int nasts = 0;
    for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        asts[nasts] = compile(c, SCM_PTR(p)->pair.car, NULL, false);
        aot_add_entry(asts[nasts]);
        nasts++;
    }

    aot_compile_and_load(verbose);

    int errors = 0;
    for (int i = 0; i < nasts; i++) {
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            continue;
        }
        c->err_jmp_active = 1;
        eval_top(c, asts[i]);
    }
    c->err_jmp_active = 0;
    return errors > 0 ? 1 : 0;
}

static int
run_file(CTX *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)GC_malloc_atomic(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);
    int r = run_string(c, buf, (size_t)sz, false);
    return r;
}

static char *
read_line(const char *prompt)
{
#ifdef USE_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;
#else
    fputs(prompt, stdout);
    fflush(stdout);
    static char buf[8192];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
#endif
}

static int
repl(CTX *c)
{
    if (!OPTION.quiet) {
        printf("ascheme — R5RS Scheme on ASTro.  Type (exit) to quit.\n");
    }
    char *line;
    while ((line = read_line("ascheme> ")) != NULL) {
        if (!*line) continue;
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            c->err_jmp_active = 0;
            continue;
        }
        c->err_jmp_active = 1;
        VALUE forms = scm_read_all_string(c, line, strlen(line));
        for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
            VALUE form = SCM_PTR(p)->pair.car;
            NODE *ast = compile(c, form, NULL, false);
            VALUE r = eval_top(c, ast);
            if (r != SCM_UNSPEC) {
                scm_display(stdout, r, true);
                putchar('\n');
            }
        }
        c->err_jmp_active = 0;
#ifdef USE_READLINE
        free(line);
#endif
    }
    putchar('\n');
    return 0;
}

int
main(int argc, char *argv[])
{
    scm_gc_init();
    OPTION.no_compiled_code = true;        // plain interpreter is the default

    int ai = 1;
    bool aot = false;
    bool verbose = false;
    bool clear_cs = false;
    bool pg_compile = false;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        if (!strcmp(argv[ai], "-q") || !strcmp(argv[ai], "--quiet")) OPTION.quiet = true;
        else if (!strcmp(argv[ai], "-c") || !strcmp(argv[ai], "--compile")) aot = true;
        else if (!strcmp(argv[ai], "-v") || !strcmp(argv[ai], "--verbose")) verbose = true;
        else if (!strcmp(argv[ai], "--clear-cs")) clear_cs = true;
        else if (!strcmp(argv[ai], "--pg-compile") || !strcmp(argv[ai], "--pg")) pg_compile = true;
        else if (!strcmp(argv[ai], "-e")) break;        // delayed
        else if (!strcmp(argv[ai], "--")) { ai++; break; }
        else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) {
            fprintf(stderr,
                "usage: ascheme [options] [file.scm | -e <expr> | -]\n"
                "options:\n"
                "  -q, --quiet      suppress non-error chatter\n"
                "  -c, --compile    AOT-compile every entry before running (uses code_store/)\n"
                "  -v, --verbose    print AOT compilation progress\n"
                "      --clear-cs   delete code_store/ before starting\n"
                "      --pg-compile interpret first, then AOT-compile hot entries (modeled\n"
                "                   on abruby's --pg-compile).  Cold entries stay as default\n"
                "                   dispatchers; the produced code_store/ accelerates the next run.\n"
                "  -e <expr>        evaluate expression and print result\n"
                "  -                read program from stdin\n");
            return 0;
        }
        else { fprintf(stderr, "ascheme: unknown option %s\n", argv[ai]); return 2; }
        ai++;
    }
    if (clear_cs) (void)!system("rm -rf code_store");
    INIT();
    CTX *c = create_context();
    if (ai >= argc) return repl(c);
    if (!strcmp(argv[ai], "-e")) {
        if (ai + 1 >= argc) { fprintf(stderr, "ascheme: -e requires an argument\n"); return 2; }
        return run_string(c, argv[ai + 1], strlen(argv[ai + 1]), true);
    }
    if (!strcmp(argv[ai], "-")) {
        char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
        buf[n] = '\0';
        return run_string(c, buf, n, false);
    }
    if (pg_compile) return run_file_pg_compile(c, argv[ai], verbose);
    if (aot) return run_file_aot(c, argv[ai], verbose);
    return run_file(c, argv[ai]);
}
