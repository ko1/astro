#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

struct castro_option OPTION;

// =====================================================================
// VALUE-slot string handling
// =====================================================================
//
// Castro stores 1 byte-of-source per VALUE slot — `"hello"` becomes
// 6 slots {.i = 'h'}, ..., {.i = '\0'}.  This makes `s[i]` fall out of
// the same slot-indexed pointer arithmetic the rest of the language
// uses, at the cost of a 8x space hit and the printf("%s") shim below
// that materialises a contiguous char buffer for the host libc call.
//
// Literals are interned in a small hash table so identical strings
// share storage and can be cheaply compared by address.

struct intern_entry { char *key; size_t klen; VALUE *val; };
static struct {
    struct intern_entry *items;
    size_t cnt, capa;
} intern_pool;

static VALUE *
castro_alloc_slot_string(const char *s, size_t len)
{
    VALUE *buf = (VALUE *)calloc(len + 1, sizeof(VALUE));
    for (size_t i = 0; i < len; i++) buf[i].i = (unsigned char)s[i];
    buf[len].i = 0;
    return buf;
}

VALUE *
castro_intern_string(const char *s)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < intern_pool.cnt; i++) {
        if (intern_pool.items[i].klen == len &&
            memcmp(intern_pool.items[i].key, s, len) == 0) {
            return intern_pool.items[i].val;
        }
    }
    if (intern_pool.cnt == intern_pool.capa) {
        intern_pool.capa = intern_pool.capa ? intern_pool.capa * 2 : 32;
        intern_pool.items = realloc(intern_pool.items,
                                    intern_pool.capa * sizeof(struct intern_entry));
    }
    struct intern_entry *e = &intern_pool.items[intern_pool.cnt++];
    e->key = malloc(len + 1);
    memcpy(e->key, s, len);
    e->key[len] = '\0';
    e->klen = len;
    e->val = castro_alloc_slot_string(s, len);
    return e->val;
}

// Materialise a VALUE-slot string into a contiguous char buffer for
// passing to host libc (printf %s, etc.).  Caller frees.
static char *
castro_slot_to_cstring(const VALUE *s)
{
    if (s == NULL) return NULL;
    size_t len = 0;
    while (s[len].i != 0) len++;
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)s[i].i;
    buf[len] = '\0';
    return buf;
}

int64_t castro_strlen(const VALUE *s) {
    int64_t l = 0;
    if (!s) return 0;
    while (s[l].i != 0) l++;
    return l;
}

int64_t castro_strcmp(const VALUE *a, const VALUE *b) {
    while (a->i && a->i == b->i) { a++; b++; }
    return (int64_t)((unsigned char)a->i) - (int64_t)((unsigned char)b->i);
}

int64_t castro_strncmp(const VALUE *a, const VALUE *b, int64_t n) {
    while (n > 0 && a->i && a->i == b->i) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int64_t)((unsigned char)a->i) - (int64_t)((unsigned char)b->i);
}

VALUE *castro_strcpy(VALUE *dst, const VALUE *src) {
    VALUE *d = dst;
    while ((d->i = src->i) != 0) { d++; src++; }
    return dst;
}

VALUE *castro_strncpy(VALUE *dst, const VALUE *src, int64_t n) {
    VALUE *d = dst;
    while (n > 0 && src->i) { d->i = src->i; d++; src++; n--; }
    while (n > 0) { d->i = 0; d++; n--; }
    return dst;
}

VALUE *castro_strcat(VALUE *dst, const VALUE *src) {
    VALUE *d = dst;
    while (d->i) d++;
    while ((d->i = src->i) != 0) { d++; src++; }
    return dst;
}

void *castro_memset(VALUE *dst, int64_t v, int64_t n) {
    for (int64_t i = 0; i < n; i++) dst[i].i = (unsigned char)v;
    return dst;
}

void *castro_memcpy(VALUE *dst, const VALUE *src, int64_t n) {
    for (int64_t i = 0; i < n; i++) dst[i] = src[i];
    return dst;
}

int64_t castro_atoi(const VALUE *s) {
    if (!s) return 0;
    while (s->i == ' ' || s->i == '\t') s++;
    int sign = 1;
    if (s->i == '-') { sign = -1; s++; }
    else if (s->i == '+') s++;
    int64_t v = 0;
    while (s->i >= '0' && s->i <= '9') { v = v * 10 + (s->i - '0'); s++; }
    return sign * v;
}

void castro_exit(int code) { exit(code); }

int castro_puts(const VALUE *slot_str)
{
    if (!slot_str) return EOF;
    int total = 0;
    while (slot_str->i != 0) {
        putchar((int)slot_str->i);
        slot_str++;
        total++;
    }
    putchar('\n');
    return total + 1;
}

// =====================================================================
// printf-family runtime
// =====================================================================
//
// Walk the format string; for each %... specifier, pick the matching
// VALUE field based on the conversion (d/i → .i as int, s → .p as
// const char *, f → .d as double, etc.) and stream through host
// printf with a normalized type.  Length modifiers (l, ll, z) get
// preserved so width-correct integer formatting still works.

int
castro_run_printf(const char *fmt_in, VALUE *args, uint32_t arg_count)
{
    // The format string in castro is itself a VALUE-slot array (one
    // byte per slot in .i), so first lower it to a contiguous C string
    // we can walk char by char.
    char *fmt_buf = castro_slot_to_cstring((const VALUE *)fmt_in);
    const char *fmt = fmt_buf ? fmt_buf : "";
    int total = 0;
    uint32_t arg_idx = 0;
    while (*fmt) {
        if (*fmt != '%') {
            putchar(*fmt++);
            total++;
            continue;
        }
        const char *p = fmt + 1;
        if (*p == '%') {
            putchar('%');
            fmt = p + 1;
            total++;
            continue;
        }
        char buf[64];
        char *bp = buf;
        *bp++ = '%';
        // flags
        while (*p && strchr("-+ #0'", *p)) *bp++ = *p++;
        // width (digits or *)
        if (*p == '*') {
            if (arg_idx >= arg_count) goto verbatim;
            int w = (int)args[arg_idx++].i;
            bp += snprintf(bp, buf + sizeof(buf) - bp, "%d", w);
            p++;
        } else {
            while (*p >= '0' && *p <= '9') *bp++ = *p++;
        }
        // precision
        if (*p == '.') {
            *bp++ = *p++;
            if (*p == '*') {
                if (arg_idx >= arg_count) goto verbatim;
                int pr = (int)args[arg_idx++].i;
                bp += snprintf(bp, buf + sizeof(buf) - bp, "%d", pr);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') *bp++ = *p++;
            }
        }
        // length modifier
        const char *lstart = p;
        while (*p && strchr("hljztL", *p)) p++;
        size_t llen = p - lstart;
        for (size_t i = 0; i < llen; i++) *bp++ = lstart[i];
        char conv = *p;
        if (!conv) {
            *bp = '\0';
            total += printf("%s", buf);
            break;
        }
        *bp++ = conv;
        *bp = '\0';

        if (conv != '%' && arg_idx >= arg_count) goto verbatim;
        VALUE v = (conv == '%') ? (VALUE){.i = 0} : args[arg_idx++];
        int n = 0;
        bool ll = (llen == 2 && lstart[0] == 'l' && lstart[1] == 'l');
        bool l1 = (llen == 1 && lstart[0] == 'l');
        bool z  = (llen == 1 && lstart[0] == 'z');
        switch (conv) {
          case 'd': case 'i':
            if (ll)      n = printf(buf, (long long)v.i);
            else if (l1) n = printf(buf, (long)v.i);
            else if (z)  n = printf(buf, (size_t)v.i);
            else         n = printf(buf, (int)v.i);
            break;
          case 'u': case 'o': case 'x': case 'X':
            if (ll)      n = printf(buf, (unsigned long long)v.i);
            else if (l1) n = printf(buf, (unsigned long)v.i);
            else if (z)  n = printf(buf, (size_t)v.i);
            else         n = printf(buf, (unsigned int)v.i);
            break;
          case 'c':
            n = printf(buf, (int)v.i);
            break;
          case 's': {
            // VALUE-slot strings need to be lowered into contiguous char
            // bytes for host printf to read them as a C string.
            char *cs = v.p ? castro_slot_to_cstring((const VALUE *)v.p) : NULL;
            n = printf(buf, cs ? cs : "(null)");
            free(cs);
            break;
          }
          case 'p':
            n = printf(buf, v.p);
            break;
          case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A':
            n = printf(buf, v.d);
            break;
          default:
verbatim:
            n = printf("%s", buf);
            break;
        }
        if (n > 0) total += n;
        fmt = p + 1;
    }
    free(fmt_buf);
    return total;
}

// Cold path: with-setjmp invoke.  Out-of-line because gcc forbids setjmp
// inside always_inline functions, and we want EVAL_node_call inlinable.
__attribute__((noinline))
VALUE
castro_invoke_jmp(CTX *c, NODE *body, node_dispatcher_func_t disp, uint32_t arg_index)
{
    c->fp += arg_index;
    jmp_buf rbuf;
    jmp_buf *prev = c->return_buf;
    c->return_buf = &rbuf;

    VALUE v;
    if (setjmp(rbuf) == 0) {
        v = (*disp)(c, body);
    }
    else {
        v = c->return_value;
    }

    c->return_buf = prev;
    c->fp -= arg_index;
    return v;
}

// =====================================================================
// loop helpers (break / continue)
// =====================================================================
//
// Each loop variant pushes a fresh jmp_buf onto c->break_buf (and, when
// the body contains `continue`, a per-iteration buf on c->continue_buf)
// so that `node_break` / `node_continue` can longjmp back here.  Plain
// loops without break/continue do not pay this cost.

__attribute__((noinline))
VALUE
castro_loop_while(CTX *c, NODE *cond, node_dispatcher_func_t cd,
                  NODE *body, node_dispatcher_func_t bd, bool has_continue)
{
    jmp_buf brk;
    jmp_buf *prev_brk = c->break_buf;
    c->break_buf = &brk;
    if (setjmp(brk) == 0) {
        while ((*cd)(c, cond).i) {
            if (has_continue) {
                jmp_buf cnt;
                jmp_buf *prev_cnt = c->continue_buf;
                c->continue_buf = &cnt;
                if (setjmp(cnt) == 0) (*bd)(c, body);
                c->continue_buf = prev_cnt;
            } else {
                (*bd)(c, body);
            }
        }
    }
    c->break_buf = prev_brk;
    return (VALUE){.i = 0};
}

__attribute__((noinline))
VALUE
castro_loop_do_while(CTX *c, NODE *body, node_dispatcher_func_t bd,
                     NODE *cond, node_dispatcher_func_t cd, bool has_continue)
{
    jmp_buf brk;
    jmp_buf *prev_brk = c->break_buf;
    c->break_buf = &brk;
    if (setjmp(brk) == 0) {
        do {
            if (has_continue) {
                jmp_buf cnt;
                jmp_buf *prev_cnt = c->continue_buf;
                c->continue_buf = &cnt;
                if (setjmp(cnt) == 0) (*bd)(c, body);
                c->continue_buf = prev_cnt;
            } else {
                (*bd)(c, body);
            }
        } while ((*cd)(c, cond).i);
    }
    c->break_buf = prev_brk;
    return (VALUE){.i = 0};
}

__attribute__((noinline))
VALUE
castro_loop_for(CTX *c, NODE *init, node_dispatcher_func_t id,
                NODE *cond, node_dispatcher_func_t cd,
                NODE *step, node_dispatcher_func_t sd,
                NODE *body, node_dispatcher_func_t bd, bool has_continue)
{
    jmp_buf brk;
    jmp_buf *prev_brk = c->break_buf;
    c->break_buf = &brk;
    (*id)(c, init);
    if (setjmp(brk) == 0) {
        while ((*cd)(c, cond).i) {
            if (has_continue) {
                jmp_buf cnt;
                jmp_buf *prev_cnt = c->continue_buf;
                c->continue_buf = &cnt;
                if (setjmp(cnt) == 0) (*bd)(c, body);
                c->continue_buf = prev_cnt;
            } else {
                (*bd)(c, body);
            }
            (*sd)(c, step);
        }
    }
    c->break_buf = prev_brk;
    return (VALUE){.i = 0};
}

// =====================================================================
// indirect call (function pointers)
// =====================================================================

__attribute__((noinline))
VALUE
castro_indirect_invoke(CTX *c, struct function_entry *fe, uint32_t arg_index)
{
    if (UNLIKELY(fe == NULL)) {
        fprintf(stderr, "castro: NULL function pointer\n");
        exit(1);
    }
    node_dispatcher_func_t disp = fe->body->head.dispatcher;
    if (fe->needs_setjmp) {
        return castro_invoke_jmp(c, fe->body, disp, arg_index);
    }
    c->fp += arg_index;
    VALUE v = (*disp)(c, fe->body);
    c->fp -= arg_index;
    return v;
}

// =====================================================================
// goto runtime: dispatch loop
// =====================================================================
//
// parse.rb wraps a function with `goto` into a label-dispatch loop
// (a `while(1) { switch(label) { case 0: ... } }`).  This helper
// installs a setjmp barrier so node_goto can longjmp back here with
// a new label, after which the body re-runs to find that case.

__attribute__((noinline))
VALUE
castro_goto_dispatch(CTX *c, NODE *body, node_dispatcher_func_t bd)
{
    jmp_buf gbuf;
    jmp_buf *prev = c->goto_buf;
    c->goto_buf = &gbuf;

    c->goto_target = 0;
    (void)setjmp(gbuf);
    VALUE v = (*bd)(c, body);

    c->goto_buf = prev;
    return v;
}

// =====================================================================
// function table
// =====================================================================
//
// `c->func_set[]` is sized exactly once (to NFUNCS read from the SX
// header) and never realloc'd, so each entry's address is stable.
// parse.rb assigns each function definition an index 0..NFUNCS-1; the
// runtime indexes by that integer for both direct calls (`node_call`)
// and address-of (`node_func_addr`).  No name lookup, no inline cache.

static struct function_entry *
castro_register_stub(CTX *c, const char *name, uint32_t params_cnt, uint32_t locals_cnt, bool needs_setjmp)
{
    if (c->func_set_cnt >= c->func_set_capa) {
        fprintf(stderr, "castro: func_set capacity exceeded (capa=%u, cnt=%u)\n",
                c->func_set_capa, c->func_set_cnt);
        exit(1);
    }
    struct function_entry *fe = &c->func_set[c->func_set_cnt++];
    fe->name = strdup(name);
    fe->body = NULL;
    fe->params_cnt = params_cnt;
    fe->locals_cnt = locals_cnt;
    fe->needs_setjmp = needs_setjmp;
    return fe;
}

// =====================================================================
// S-expression tokenizer
// =====================================================================

typedef enum {
    TK_LPAREN, TK_RPAREN, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_EOF
} tk_kind;

typedef struct {
    tk_kind kind;
    const char *start;
    size_t len;
    int64_t  ival;
    double   fval;
    char    *sval;  // owned for strings
} tok_t;

typedef struct {
    const char *src;
    const char *pos;
    tok_t cur;
} sx_lexer;

static void
sx_skip_ws(sx_lexer *l)
{
    while (*l->pos) {
        if (isspace((unsigned char)*l->pos)) {
            l->pos++;
        }
        else if (*l->pos == ';') {
            while (*l->pos && *l->pos != '\n') l->pos++;
        }
        else {
            break;
        }
    }
}

static char *
sx_read_string(sx_lexer *l)
{
    // pos points at opening "
    l->pos++;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    while (*l->pos && *l->pos != '"') {
        char ch;
        if (*l->pos == '\\') {
            l->pos++;
            switch (*l->pos) {
              case 'n': ch = '\n'; l->pos++; break;
              case 't': ch = '\t'; l->pos++; break;
              case 'r': ch = '\r'; l->pos++; break;
              case '\\': ch = '\\'; l->pos++; break;
              case '"': ch = '"'; l->pos++; break;
              case 'a': ch = '\a'; l->pos++; break;
              case 'b': ch = '\b'; l->pos++; break;
              case 'f': ch = '\f'; l->pos++; break;
              case 'v': ch = '\v'; l->pos++; break;
              case '0': case '1': case '2': case '3':
              case '4': case '5': case '6': case '7': {
                int v = 0, k = 0;
                while (k < 3 && *l->pos >= '0' && *l->pos <= '7') {
                    v = v * 8 + (*l->pos - '0');
                    l->pos++;
                    k++;
                }
                ch = (char)v;
                break;
              }
              case 'x': {
                l->pos++;
                int v = 0;
                while (isxdigit((unsigned char)*l->pos)) {
                    int d = *l->pos;
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                    else d = d - 'A' + 10;
                    v = v * 16 + d;
                    l->pos++;
                }
                ch = (char)v;
                break;
              }
              default:  ch = *l->pos++; break;
            }
        }
        else {
            ch = *l->pos++;
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = ch;
    }
    if (*l->pos == '"') l->pos++;
    buf[len] = '\0';
    return buf;
}

static void
sx_next(sx_lexer *l)
{
    sx_skip_ws(l);
    l->cur.start = l->pos;
    if (*l->pos == '\0') { l->cur.kind = TK_EOF; return; }
    if (*l->pos == '(')  { l->cur.kind = TK_LPAREN; l->pos++; return; }
    if (*l->pos == ')')  { l->cur.kind = TK_RPAREN; l->pos++; return; }
    if (*l->pos == '"')  {
        l->cur.kind = TK_STRING;
        l->cur.sval = sx_read_string(l);
        return;
    }

    // number or identifier — try number if it looks numeric
    const char *p = l->pos;
    bool is_num = false, is_float = false;
    if (*p == '-' || *p == '+') p++;
    if (isdigit((unsigned char)*p)) {
        is_num = true;
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.' || *q == 'e' || *q == 'E') is_float = true;
    }
    if (is_num) {
        char *endp;
        if (is_float) {
            l->cur.kind = TK_FLOAT;
            l->cur.fval = strtod(l->pos, &endp);
        }
        else {
            l->cur.kind = TK_INT;
            errno = 0;
            l->cur.ival = strtoll(l->pos, &endp, 10);
            // also support 0x prefix
        }
        l->pos = endp;
        return;
    }

    // identifier: until whitespace or paren
    const char *q = l->pos;
    while (*q && !isspace((unsigned char)*q) && *q != '(' && *q != ')') q++;
    l->cur.kind = TK_IDENT;
    l->cur.start = l->pos;
    l->cur.len = q - l->pos;
    l->pos = q;
}

static void
sx_init(sx_lexer *l, const char *src)
{
    l->src = src;
    l->pos = src;
    sx_next(l);
}

static bool
sx_ident_eq(tok_t *t, const char *s)
{
    return t->kind == TK_IDENT && strlen(s) == t->len && memcmp(t->start, s, t->len) == 0;
}

static char *
sx_ident_dup(tok_t *t)
{
    char *s = malloc(t->len + 1);
    memcpy(s, t->start, t->len);
    s[t->len] = '\0';
    return s;
}

static void
sx_err(sx_lexer *l, const char *msg)
{
    fprintf(stderr, "sx parse error: %s near `%.*s`\n", msg,
            (int)(l->cur.len ? l->cur.len : 4), l->cur.start);
    exit(1);
}

static void
sx_expect(sx_lexer *l, tk_kind k)
{
    if (l->cur.kind != k) sx_err(l, "unexpected token");
    sx_next(l);
}

// =====================================================================
// S-expression -> NODE *
// =====================================================================

static NODE *build_expr(sx_lexer *l);

static int64_t
read_int(sx_lexer *l)
{
    if (l->cur.kind != TK_INT) sx_err(l, "expected int");
    int64_t v = l->cur.ival;
    sx_next(l);
    return v;
}

static double
read_num(sx_lexer *l)
{
    if (l->cur.kind == TK_FLOAT) { double v = l->cur.fval; sx_next(l); return v; }
    if (l->cur.kind == TK_INT)   { double v = (double)l->cur.ival; sx_next(l); return v; }
    sx_err(l, "expected number");
    return 0;
}

static char *
read_string_or_ident(sx_lexer *l)
{
    if (l->cur.kind == TK_STRING) {
        char *s = l->cur.sval;
        sx_next(l);
        return s;
    }
    if (l->cur.kind == TK_IDENT) {
        char *s = sx_ident_dup(&l->cur);
        sx_next(l);
        return s;
    }
    sx_err(l, "expected string or ident");
    return NULL;
}

// op dispatch
static NODE *
build_op(sx_lexer *l, tok_t op)
{
    // op is the IDENT token; following positional args, then ')'.
    // Each branch reads its expected operand pattern.
#define IS(s) (strlen(s) == op.len && memcmp(op.start, s, op.len) == 0)

    if (IS("lit_i")) {
        int64_t v = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lit_i((int32_t)v);
    }
    if (IS("lit_i64")) {
        int64_t v = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lit_i64((uint64_t)v);
    }
    if (IS("lit_d")) {
        double v = read_num(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lit_d(v);
    }
    if (IS("lit_str")) {
        if (l->cur.kind != TK_STRING) sx_err(l, "expected string");
        char *s = l->cur.sval;
        sx_next(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lit_str(s);
    }
    if (IS("lget")) {
        int64_t idx = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lget((uint32_t)idx);
    }
    if (IS("lset")) {
        int64_t idx = read_int(l);
        NODE *rhs = build_expr(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lset((uint32_t)idx, rhs);
    }
    if (IS("gget")) {
        int64_t idx = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_gget((uint32_t)idx);
    }
    if (IS("gset")) {
        int64_t idx = read_int(l);
        NODE *rhs = build_expr(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_gset((uint32_t)idx, rhs);
    }
    if (IS("addr_local")) {
        int64_t idx = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_addr_local((uint32_t)idx);
    }
    if (IS("addr_global")) {
        int64_t idx = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_addr_global((uint32_t)idx);
    }
    if (IS("goto")) {
        int64_t lbl = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_goto((int32_t)lbl);
    }
    if (IS("goto_dispatch")) {
        NODE *b = build_expr(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_goto_dispatch(b);
    }
    if (IS("nop"))         { sx_expect(l, TK_RPAREN); return ALLOC_node_nop(); }
    if (IS("return_void")) { sx_expect(l, TK_RPAREN); return ALLOC_node_return_void(); }
    if (IS("break"))       { sx_expect(l, TK_RPAREN); return ALLOC_node_break(); }
    if (IS("continue"))    { sx_expect(l, TK_RPAREN); return ALLOC_node_continue(); }
    if (IS("goto_target")) { sx_expect(l, TK_RPAREN); return ALLOC_node_goto_target(); }

    // single-arg ops
    static const struct { const char *name; NODE *(*fn)(NODE *); } u1[] = {
        {"drop",        ALLOC_node_drop},
        {"return",      ALLOC_node_return},
        {"neg_i",       ALLOC_node_neg_i},
        {"neg_d",       ALLOC_node_neg_d},
        {"bnot",        ALLOC_node_bnot},
        {"lnot",        ALLOC_node_lnot},
        {"cast_id",     ALLOC_node_cast_id},
        {"cast_di",     ALLOC_node_cast_di},
        {"load_i",      ALLOC_node_load_i},
        {"load_d",      ALLOC_node_load_d},
        {"load_p",      ALLOC_node_load_p},
        {"call_putchar",ALLOC_node_putchar},
        {"call_puts",   ALLOC_node_puts},
        {"call_malloc", ALLOC_node_call_malloc},
        {"call_free",   ALLOC_node_call_free},
        {"call_strlen", ALLOC_node_call_strlen},
        {"call_atoi",   ALLOC_node_call_atoi},
        {"call_exit",   ALLOC_node_call_exit},
        {"call_abs",    ALLOC_node_call_abs},
        {NULL, NULL}
    };
    for (int i = 0; u1[i].name; i++) {
        if (IS(u1[i].name)) {
            NODE *a = build_expr(l);
            sx_expect(l, TK_RPAREN);
            return u1[i].fn(a);
        }
    }

    // two-arg ops
    static const struct { const char *name; NODE *(*fn)(NODE *, NODE *); } u2[] = {
        {"seq",     ALLOC_node_seq},
        {"add_i",   ALLOC_node_add_i},
        {"sub_i",   ALLOC_node_sub_i},
        {"mul_i",   ALLOC_node_mul_i},
        {"div_i",   ALLOC_node_div_i},
        {"mod_i",   ALLOC_node_mod_i},
        {"add_d",   ALLOC_node_add_d},
        {"sub_d",   ALLOC_node_sub_d},
        {"mul_d",   ALLOC_node_mul_d},
        {"div_d",   ALLOC_node_div_d},
        {"band",    ALLOC_node_band},
        {"bor",     ALLOC_node_bor},
        {"bxor",    ALLOC_node_bxor},
        {"shl",     ALLOC_node_shl},
        {"shr",     ALLOC_node_shr},
        {"lt_i",    ALLOC_node_lt_i},
        {"le_i",    ALLOC_node_le_i},
        {"gt_i",    ALLOC_node_gt_i},
        {"ge_i",    ALLOC_node_ge_i},
        {"eq_i",    ALLOC_node_eq_i},
        {"neq_i",   ALLOC_node_neq_i},
        {"lt_d",    ALLOC_node_lt_d},
        {"le_d",    ALLOC_node_le_d},
        {"gt_d",    ALLOC_node_gt_d},
        {"ge_d",    ALLOC_node_ge_d},
        {"eq_d",    ALLOC_node_eq_d},
        {"neq_d",   ALLOC_node_neq_d},
        {"land",    ALLOC_node_land},
        {"lor",     ALLOC_node_lor},
        {"do_while",        ALLOC_node_do_while},
        {"do_while_brk",    ALLOC_node_do_while_brk},
        {"do_while_brk_only", ALLOC_node_do_while_brk_only},
        {"while",           ALLOC_node_while},
        {"while_brk",       ALLOC_node_while_brk},
        {"while_brk_only",  ALLOC_node_while_brk_only},
        {"store_i",         ALLOC_node_store_i},
        {"store_d",         ALLOC_node_store_d},
        {"store_p",         ALLOC_node_store_p},
        {"ptr_add",         ALLOC_node_ptr_add},
        {"ptr_sub_i",       ALLOC_node_ptr_sub_i},
        {"ptr_diff",        ALLOC_node_ptr_diff},
        {"call_strcmp",     ALLOC_node_call_strcmp},
        {"call_strcpy",     ALLOC_node_call_strcpy},
        {"call_strcat",     ALLOC_node_call_strcat},
        {"call_calloc",     ALLOC_node_call_calloc},
        {NULL, NULL}
    };
    for (int i = 0; u2[i].name; i++) {
        if (IS(u2[i].name)) {
            NODE *a = build_expr(l);
            NODE *b = build_expr(l);
            sx_expect(l, TK_RPAREN);
            return u2[i].fn(a, b);
        }
    }

    if (IS("if") || IS("ternary")) {
        NODE *a = build_expr(l);
        NODE *b = build_expr(l);
        NODE *c = build_expr(l);
        sx_expect(l, TK_RPAREN);
        return IS("if") ? ALLOC_node_if(a, b, c) : ALLOC_node_ternary(a, b, c);
    }

    // 3-arg ops
    static const struct { const char *name; NODE *(*fn)(NODE *, NODE *, NODE *); } u3[] = {
        {"call_strncmp",  ALLOC_node_call_strncmp},
        {"call_strncpy",  ALLOC_node_call_strncpy},
        {"call_memset",   ALLOC_node_call_memset},
        {"call_memcpy",   ALLOC_node_call_memcpy},
        {NULL, NULL}
    };
    for (int i = 0; u3[i].name; i++) {
        if (IS(u3[i].name)) {
            NODE *a = build_expr(l);
            NODE *b = build_expr(l);
            NODE *c = build_expr(l);
            sx_expect(l, TK_RPAREN);
            return u3[i].fn(a, b, c);
        }
    }

    if (IS("func_addr")) {
        // (func_addr FUNC_IDX) — index into c->func_set[].
        int64_t func_idx = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_func_addr((uint32_t)func_idx);
    }
    if (IS("call_indirect")) {
        // (call_indirect FN_EXPR nargs arg_index)
        NODE *fn = build_expr(l);
        int64_t nargs = read_int(l);
        int64_t arg_index = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_call_indirect(fn, (uint32_t)nargs, (uint32_t)arg_index);
    }
    if (IS("lit_str_array")) {
        if (l->cur.kind != TK_STRING) sx_err(l, "expected string");
        char *s = l->cur.sval;
        sx_next(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_lit_str_array(s);
    }

    if (IS("for") || IS("for_brk") || IS("for_brk_only")) {
        bool is_brk = IS("for_brk");
        bool is_brk_only = IS("for_brk_only");
        NODE *a = build_expr(l);
        NODE *b = build_expr(l);
        NODE *c = build_expr(l);
        NODE *d = build_expr(l);
        sx_expect(l, TK_RPAREN);
        if (is_brk)      return ALLOC_node_for_brk(a, b, c, d);
        if (is_brk_only) return ALLOC_node_for_brk_only(a, b, c, d);
        return ALLOC_node_for(a, b, c, d);
    }

    if (IS("call")) {
        // (call FUNC_IDX nargs arg_index) — index into c->func_set[].
        int64_t func_idx = read_int(l);
        int64_t nargs = read_int(l);
        int64_t arg_index = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_call((uint32_t)func_idx, (uint32_t)nargs, (uint32_t)arg_index);
    }
    if (IS("call_printf")) {
        // (call_printf nargs arg_index) — args[0] is format string.
        int64_t nargs = read_int(l);
        int64_t arg_index = read_int(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_printf((uint32_t)nargs, (uint32_t)arg_index);
    }
    // call_putchar / call_puts handled via the u1 table

    fprintf(stderr, "unknown op: %.*s\n", (int)op.len, op.start);
    exit(1);
#undef IS
}

static NODE *
build_expr(sx_lexer *l)
{
    sx_expect(l, TK_LPAREN);
    if (l->cur.kind != TK_IDENT) sx_err(l, "expected op ident");
    tok_t op = l->cur;
    sx_next(l);
    return build_op(l, op);
}

// =====================================================================
// program loading
// =====================================================================

// Forward decl: the init expression is built before load_program
// finishes, but we want to run it once, after both globals are
// allocated and all functions are registered (so global initializers
// can call other functions).
static NODE *G_INIT_EXPR = NULL;

static void
load_program(CTX *c, sx_lexer *l)
{
    sx_expect(l, TK_LPAREN);
    if (!sx_ident_eq(&l->cur, "program")) sx_err(l, "expected `program`");
    sx_next(l);

    // (program GLOBALS_SIZE NFUNCS INIT_EXPR
    //   (sig NAME P L T HR) ... NFUNCS times
    //   BODY_EXPR ...        NFUNCS times in matching order)
    //
    // We pre-allocate `c->func_set` to NFUNCS up front and register
    // every function as a stub before parsing any body — this lets a
    // body refer to other functions (including itself) by index even
    // when the called function is defined later in the SX stream.
    int64_t globals_size = read_int(l);
    int64_t nfuncs = read_int(l);
    if (globals_size > 0) {
        c->globals = calloc((size_t)globals_size, sizeof(VALUE));
        c->globals_size = (size_t)globals_size;
    }
    if (nfuncs > 0) {
        c->func_set_capa = (unsigned)nfuncs;
        c->func_set = calloc(c->func_set_capa, sizeof(struct function_entry));
    }
    c->func_set_cnt = 0;

    G_INIT_EXPR = build_expr(l);
    G_INIT_EXPR = OPTIMIZE(G_INIT_EXPR);

    // Phase 1: signatures.  These come right after INIT_EXPR.
    for (int64_t i = 0; i < nfuncs; i++) {
        sx_expect(l, TK_LPAREN);
        if (!sx_ident_eq(&l->cur, "sig")) sx_err(l, "expected `sig`");
        sx_next(l);
        char *name = read_string_or_ident(l);
        int64_t params = read_int(l);
        int64_t locals = read_int(l);
        if (l->cur.kind != TK_IDENT) sx_err(l, "expected ret type ident");
        sx_next(l);
        int64_t has_returns = read_int(l);
        sx_expect(l, TK_RPAREN);
        castro_register_stub(c, name, (uint32_t)params, (uint32_t)locals, has_returns != 0);
        free(name);
    }

    // Phase 2: bodies, one per registered function, in matching order.
    for (int64_t i = 0; i < nfuncs; i++) {
        NODE *body = build_expr(l);
        body = OPTIMIZE(body);
        c->func_set[i].body = body;
    }

    sx_expect(l, TK_RPAREN);
}

// =====================================================================
// CTX management
// =====================================================================

#define CASTRO_ENV_SLOTS (1 << 20)  // 1M slots = 8MB stack

static CTX *
create_context(void)
{
    CTX *c = calloc(1, sizeof(CTX));
    c->env = malloc(sizeof(VALUE) * CASTRO_ENV_SLOTS);
    c->env_end = c->env + CASTRO_ENV_SLOTS;
    c->fp = c->env;
    c->func_set = NULL;
    c->func_set_cnt = 0;
    c->func_set_capa = 0;
    c->return_buf = NULL;
    return c;
}

// =====================================================================
// driver
// =====================================================================

static char *
slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("fread"); exit(1); }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void
compile_all_funcs(CTX *c)
{
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        astro_cs_compile(c->func_set[i].body, NULL);
    }
    astro_cs_build(NULL);
    astro_cs_reload();
}

static void
load_all_funcs(CTX *c)
{
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        bool ok = astro_cs_load(c->func_set[i].body, NULL);
        if (!OPTION.quiet) {
            fprintf(stderr, "cs_load: %s -> %s\n",
                    c->func_set[i].name, ok ? "specialized" : "default");
        }
    }
}

int
main(int argc, char *argv[])
{
    const char *file = NULL;
    bool sx_input = false;
    bool compile_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            OPTION.quiet = true;
        }
        else if (strcmp(argv[i], "--no-compile") == 0) {
            OPTION.no_compiled_code = true;
        }
        else if (strcmp(argv[i], "--no-spec") == 0) {
            OPTION.no_generate_specialized_code = true;
        }
        else if (strcmp(argv[i], "--compile-all") == 0 || strcmp(argv[i], "-c") == 0) {
            compile_all = true;
        }
        else if (strcmp(argv[i], "--dump") == 0) {
            OPTION.dump = true;
        }
        else if (strcmp(argv[i], "--sx") == 0) {
            sx_input = true;
        }
        else if (file == NULL) {
            file = argv[i];
        }
    }
    if (file == NULL) {
        fprintf(stderr, "usage: %s [-q] [--no-compile] [--sx] FILE.c\n", argv[0]);
        return 1;
    }

    INIT();
    CTX *c = create_context();

    char *sx_path = NULL;
    if (sx_input) {
        sx_path = strdup(file);
    }
    else {
        // Run parse.py, capture output to /tmp/claude/...
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        sx_path = malloc(256);
        snprintf(sx_path, 256, "./tmp/castro_%d_%s.sx", (int)getpid(), base);
        char cmd[2048];
        // self-locate parse.py: assume executable is in the same dir
        char self[1024];
        ssize_t r = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (r < 0) { perror("readlink"); return 1; }
        self[r] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) *slash = '\0';
        snprintf(cmd, sizeof(cmd), "ruby %s/parse.rb %s > %s", self, file, sx_path);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "parse.py failed\n");
            return 1;
        }
    }

    char *src = slurp(sx_path);
    sx_lexer l;
    sx_init(&l, src);
    load_program(c, &l);

    if (OPTION.dump) {
        for (unsigned int i = 0; i < c->func_set_cnt; i++) {
            fprintf(stderr, "func %s:\n  ", c->func_set[i].name);
            DUMP(stderr, c->func_set[i].body, true);
            fprintf(stderr, "\n");
        }
    }

    if (compile_all) {
        compile_all_funcs(c);
        load_all_funcs(c);
    }
    else if (!OPTION.no_compiled_code) {
        load_all_funcs(c);
    }

    // Run global initializers once, before main.
    if (G_INIT_EXPR) {
        // Treat init body the same as a function body so that any
        // sub-expressions that take addresses (or call functions that
        // do) work consistently.
        EVAL(c, G_INIT_EXPR);
    }

    // Find main by name (only used here for the program entry point).
    // All other call sites resolve their target by index at parse time.
    struct function_entry *fe = NULL;
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        if (strcmp(c->func_set[i].name, "main") == 0) {
            fe = &c->func_set[i];
            break;
        }
    }
    if (!fe) {
        fprintf(stderr, "no main\n");
        return 1;
    }

    // call main with no args
    VALUE result;
    if (fe->needs_setjmp) {
        jmp_buf rbuf;
        c->return_buf = &rbuf;
        if (setjmp(rbuf) == 0) {
            result = EVAL(c, fe->body);
        }
        else {
            result = c->return_value;
        }
        c->return_buf = NULL;
    }
    else {
        result = EVAL(c, fe->body);
    }

    if (!OPTION.quiet) {
        printf("=> %lld\n", (long long)result.i);
    }
    return (int)result.i;
}

// =====================================================================
// hooks for code-store / replace / dump that the framework expects
// =====================================================================

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}
