// asml — Standard ML subset on ASTro.
//
// Subset: int / real / string / bool / unit / list / tuple / ref / user
// datatypes (zero/one-payload constructors).  Top-level `val` and `fun`
// (mutually recursive via `and`); local `let val/fun ... in expr end`.
// `case` with patterns: literals, wildcard, identifier, constructor,
// tuple, cons, list literal.  `andalso`, `orelse`, `not`, `if/then/else`,
// `raise`, `e handle pat => e | ...`.
//
// Parser: hand-written recursive descent.  No type checker; type errors
// surface at runtime via ml_type_error.

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/stat.h>
#include <math.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct asml_option OPTION;

extern bool ml_node_to_tail(NODE *n);
extern bool ml_node_is_app(NODE *n);

// ---------------------------------------------------------------------------
// Singleton mlobjs.
// ---------------------------------------------------------------------------

struct mlobj ML_UNIT_OBJ  = { .type = MLOBJ_UNIT };
struct mlobj ML_TRUE_OBJ  = { .type = MLOBJ_BOOL, .b = true };
struct mlobj ML_FALSE_OBJ = { .type = MLOBJ_BOOL, .b = false };
struct mlobj ML_NIL_OBJ   = { .type = MLOBJ_NIL };

// ---------------------------------------------------------------------------
// Allocation helpers.
// ---------------------------------------------------------------------------

struct mlobj *
ml_alloc(int type)
{
    struct mlobj *o = (struct mlobj *)calloc(1, sizeof(struct mlobj));
    if (!o) { fprintf(stderr, "asml: oom\n"); exit(1); }
    o->type = type;
    return o;
}

VALUE
ml_cons(VALUE h, VALUE t)
{
    struct mlobj *o = ml_alloc(MLOBJ_CONS);
    o->cons.head = h;
    o->cons.tail = t;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_string(const char *s, size_t len)
{
    struct mlobj *o = ml_alloc(MLOBJ_STRING);
    o->str.chars = (char *)malloc(len + 1);
    memcpy(o->str.chars, s, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_real(double d)
{
    struct mlobj *o = ml_alloc(MLOBJ_REAL);
    o->dbl = d;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_tuple(int n, VALUE *items)
{
    struct mlobj *o = ml_alloc(MLOBJ_TUPLE);
    o->tup.n = n;
    o->tup.items = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) o->tup.items[i] = items[i];
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_ref(VALUE init)
{
    struct mlobj *o = ml_alloc(MLOBJ_REF);
    o->refval = init;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_closure(struct Node *body, struct mlframe *env, int nparams,
                bool is_leaf, const char *name)
{
    struct mlobj *o = ml_alloc(MLOBJ_CLOSURE);
    o->closure.body = body;
    o->closure.env = env;
    o->closure.nparams = nparams;
    o->closure.is_leaf = is_leaf;
    o->closure.name = name;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_prim(const char *name, ml_prim_fn fn, int min_argc, int max_argc)
{
    struct mlobj *o = ml_alloc(MLOBJ_PRIM);
    o->prim.name = name;
    o->prim.fn = fn;
    o->prim.min_argc = min_argc;
    o->prim.max_argc = max_argc;
    return ML_OBJ_VAL(o);
}

VALUE
ml_make_variant(const char *name, int n, VALUE *items)
{
    struct mlobj *o = ml_alloc(MLOBJ_VARIANT);
    o->var.name = name;
    o->var.n = n;
    if (n > 0) {
        o->var.items = (VALUE *)malloc(sizeof(VALUE) * n);
        for (int i = 0; i < n; i++) o->var.items[i] = items[i];
    }
    return ML_OBJ_VAL(o);
}

// fields は呼び出し元で sort 済 (intern も済) を期待。
VALUE
ml_make_record(int n, const char **fields, VALUE *items)
{
    struct mlobj *o = ml_alloc(MLOBJ_RECORD);
    o->rec.n = n;
    o->rec.fields = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    o->rec.items  = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        o->rec.fields[i] = fields[i];
        o->rec.items[i]  = items[i];
    }
    return ML_OBJ_VAL(o);
}

VALUE
ml_string_concat(VALUE a, VALUE b)
{
    struct mlobj *ao = ML_PTR(a), *bo = ML_PTR(b);
    size_t la = ao->str.len, lb = bo->str.len;
    struct mlobj *o = ml_alloc(MLOBJ_STRING);
    o->str.chars = (char *)malloc(la + lb + 1);
    memcpy(o->str.chars, ao->str.chars, la);
    memcpy(o->str.chars + la, bo->str.chars, lb);
    o->str.chars[la + lb] = '\0';
    o->str.len = la + lb;
    return ML_OBJ_VAL(o);
}

double
ml_get_real(VALUE v)
{
    if (ML_IS_REAL(v)) return ML_PTR(v)->dbl;
    if (ML_IS_INT(v))  return (double)ML_INT_VAL(v);
    return 0.0 / 0.0;
}

bool
ml_structural_eq(VALUE a, VALUE b)
{
    if (a == b) return true;
    if (ML_IS_INT(a) || ML_IS_INT(b)) return false;
    if (a == ML_UNIT || b == ML_UNIT) return false;
    if (a == ML_NIL || b == ML_NIL) return false;
    if (a == ML_TRUE || a == ML_FALSE || b == ML_TRUE || b == ML_FALSE) return false;
    struct mlobj *ao = ML_PTR(a), *bo = ML_PTR(b);
    if (ao->type != bo->type) return false;
    switch (ao->type) {
      case MLOBJ_STRING:
        return ao->str.len == bo->str.len &&
               memcmp(ao->str.chars, bo->str.chars, ao->str.len) == 0;
      case MLOBJ_REAL:
        return ao->dbl == bo->dbl;
      case MLOBJ_CONS:
        return ml_structural_eq(ao->cons.head, bo->cons.head) &&
               ml_structural_eq(ao->cons.tail, bo->cons.tail);
      case MLOBJ_TUPLE:
        if (ao->tup.n != bo->tup.n) return false;
        for (int i = 0; i < ao->tup.n; i++)
            if (!ml_structural_eq(ao->tup.items[i], bo->tup.items[i]))
                return false;
        return true;
      case MLOBJ_VARIANT:
        if (strcmp(ao->var.name, bo->var.name) != 0) return false;
        if (ao->var.n != bo->var.n) return false;
        for (int i = 0; i < ao->var.n; i++)
            if (!ml_structural_eq(ao->var.items[i], bo->var.items[i]))
                return false;
        return true;
      case MLOBJ_RECORD:
        if (ao->rec.n != bo->rec.n) return false;
        for (int i = 0; i < ao->rec.n; i++) {
            if (strcmp(ao->rec.fields[i], bo->rec.fields[i]) != 0) return false;
            if (!ml_structural_eq(ao->rec.items[i], bo->rec.items[i])) return false;
        }
        return true;
      default:
        return false;
    }
}

int
ml_compare(VALUE a, VALUE b)
{
    if (ML_IS_INT(a) && ML_IS_INT(b)) {
        int64_t ai = ML_INT_VAL(a), bi = ML_INT_VAL(b);
        return (ai > bi) - (ai < bi);
    }
    if (ML_IS_REAL(a) && ML_IS_REAL(b)) {
        double ad = ML_PTR(a)->dbl, bd = ML_PTR(b)->dbl;
        return (ad > bd) - (ad < bd);
    }
    if (ML_IS_STRING(a) && ML_IS_STRING(b)) {
        struct mlobj *ao = ML_PTR(a), *bo = ML_PTR(b);
        size_t la = ao->str.len, lb = bo->str.len;
        size_t l = la < lb ? la : lb;
        int r = memcmp(ao->str.chars, bo->str.chars, l);
        if (r != 0) return r < 0 ? -1 : 1;
        return (la > lb) - (la < lb);
    }
    if (a == ML_TRUE && b == ML_FALSE) return 1;
    if (a == ML_FALSE && b == ML_TRUE) return -1;
    if (a == b) return 0;
    return 0;
}

struct mlframe *
ml_new_frame(struct mlframe *parent, int nslots)
{
    struct mlframe *f = (struct mlframe *)calloc(1,
        sizeof(struct mlframe) + sizeof(VALUE) * (nslots ? nslots : 1));
    if (!f) { fprintf(stderr, "asml: oom\n"); exit(1); }
    f->parent = parent;
    f->nslots = nslots;
    return f;
}

// ---------------------------------------------------------------------------
// Apply.
// ---------------------------------------------------------------------------

struct partial_state {
    VALUE  fn;
    int    captured;
    int    nparams;
    VALUE *args;
};

VALUE
ml_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    VALUE local_argv[16];
    bool first_iter = true;
loop:
    if (ML_IS_PRIM(fn)) {
        struct mlobj *p = ML_PTR(fn);
        if (p->prim.fn == NULL) {
            struct partial_state *ps = (struct partial_state *)p->prim.name;
            int total = ps->captured + argc;
            VALUE *combined = (VALUE *)alloca(sizeof(VALUE) * (total ? total : 1));
            for (int i = 0; i < ps->captured; i++) combined[i] = ps->args[i];
            for (int i = 0; i < argc;         i++) combined[ps->captured + i] = argv[i];
            fn = ps->fn; argc = total; argv = combined;
            goto loop;
        }
        if (argc < p->prim.min_argc ||
            (p->prim.max_argc >= 0 && argc > p->prim.max_argc)) {
            ml_raise(c, ml_make_variant("Fail", 1,
                (VALUE[]){ ml_make_string("primitive arity mismatch", 24) }));
        }
        return p->prim.fn(c, argc, argv);
    }
    if (!ML_IS_CLOSURE(fn)) {
        ml_raise(c, ml_make_variant("Fail", 1,
            (VALUE[]){ ml_make_string("applying a non-function value", 29) }));
    }
    {
        struct mlobj *cl = ML_PTR(fn);
        int np = cl->closure.nparams;
        if (argc < np) {
            VALUE *capt = (VALUE *)malloc(sizeof(VALUE) * argc);
            for (int i = 0; i < argc; i++) capt[i] = argv[i];
            struct mlobj *p = ml_alloc(MLOBJ_PRIM);
            struct partial_state *ps = (struct partial_state *)malloc(sizeof *ps);
            ps->fn = fn; ps->captured = argc; ps->nparams = np; ps->args = capt;
            p->prim.fn = NULL;
            p->prim.name = (const char *)ps;
            p->prim.min_argc = np - argc;
            p->prim.max_argc = -1;
            return ML_OBJ_VAL(p);
        }
        struct mlframe *f;
        if (LIKELY(cl->closure.is_leaf && first_iter)) {
            size_t bytes = sizeof(struct mlframe) + sizeof(VALUE) * (np ? np : 1);
            f = (struct mlframe *)alloca(bytes);
            f->parent = cl->closure.env;
            f->nslots = np;
        } else {
            f = ml_new_frame(cl->closure.env, np);
        }
        for (int i = 0; i < np; i++) f->slots[i] = argv[i];
        struct mlframe *saved = c->env;
        c->env = f;
        VALUE r = EVAL(c, cl->closure.body);
        c->env = saved;
        if (UNLIKELY(c->tail_call_pending)) {
            c->tail_call_pending = 0;
            fn = c->tc_fn; argc = c->tc_argc;
            if (argc > 16) return ml_apply(c, fn, argc, c->tc_argv);
            for (int i = 0; i < argc; i++) local_argv[i] = c->tc_argv[i];
            argv = local_argv;
            first_iter = false;
            goto loop;
        }
        if (argc == np) return r;
        return ml_apply(c, r, argc - np, argv + np);
    }
}

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------

void
ml_global_define(CTX *c, const char *name, VALUE v)
{
    c->globals_serial++;
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            c->globals[i].value = v;
            return;
        }
    }
    if (c->globals_size == c->globals_capa) {
        size_t cap = c->globals_capa ? c->globals_capa * 2 : 64;
        c->globals = (struct gentry *)realloc(c->globals, cap * sizeof(struct gentry));
        c->globals_capa = cap;
    }
    c->globals[c->globals_size].name = strdup(name);
    c->globals[c->globals_size].value = v;
    c->globals_size++;
}

VALUE
ml_global_ref(CTX *c, const char *name)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0)
            return c->globals[i].value;
    }
    ml_raise(c, ml_make_variant("Fail", 1,
        (VALUE[]){ ml_make_string(name, strlen(name)) }));
}

// ---------------------------------------------------------------------------
// Error handling / raise.
// ---------------------------------------------------------------------------

void
ml_error(CTX *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err_msg, sizeof(c->err_msg), fmt, ap);
    va_end(ap);
    if (c->err_jmp_active) longjmp(c->err_jmp, 1);
    fprintf(stderr, "asml: %s\n", c->err_msg);
    exit(2);
}

void
ml_type_error(CTX *c, const char *op, const char *expected)
{
    (void)op;
    ml_raise(c, ml_make_variant("Fail", 1,
        (VALUE[]){ ml_make_string(expected, strlen(expected)) }));
}

void
ml_raise(CTX *c, VALUE exn)
{
    if (c->handlers_top < 0) {
        const char *name = "?";
        if (ML_IS_VARIANT(exn)) name = ML_PTR(exn)->var.name;
        snprintf(c->err_msg, sizeof(c->err_msg), "uncaught exception %s", name);
        if (c->err_jmp_active) longjmp(c->err_jmp, 1);
        fprintf(stderr, "asml: %s\n", c->err_msg);
        exit(2);
    }
    struct ml_handler *h = &c->handlers[c->handlers_top];
    h->exn = exn;
    longjmp(h->buf, 1);
}

VALUE
ml_run_handle(CTX *c, struct Node *body, struct Node *handler)
{
    if (c->handlers_top + 1 >= ASML_HANDLER_MAX_DEPTH)
        ml_error(c, "handler stack overflow");
    int top = ++c->handlers_top;
    struct ml_handler *h = &c->handlers[top];
    h->saved_env = c->env;
    if (setjmp(h->buf) == 0) {
        VALUE r = EVAL(c, body);
        c->handlers_top--;
        return r;
    }
    VALUE exn = h->exn;
    c->env = h->saved_env;
    c->handlers_top--;
    struct mlframe *f = ml_new_frame(c->env, 1);
    f->slots[0] = exn;
    struct mlframe *saved = c->env;
    c->env = f;
    VALUE r = EVAL(c, handler);
    c->env = saved;
    return r;
}

// ---------------------------------------------------------------------------
// Display.
// ---------------------------------------------------------------------------

static void ml_display_inner(FILE *fp, VALUE v, bool inside);

static void
ml_display_list_tail(FILE *fp, VALUE v)
{
    while (ML_IS_CONS(v)) {
        struct mlobj *o = ML_PTR(v);
        fprintf(fp, ",");
        ml_display_inner(fp, o->cons.head, true);
        v = o->cons.tail;
    }
    if (v != ML_NIL) {
        fprintf(fp, "|");
        ml_display_inner(fp, v, true);
    }
    fprintf(fp, "]");
}

static void
ml_display_inner(FILE *fp, VALUE v, bool inside)
{
    if (ML_IS_INT(v)) {
        int64_t i = ML_INT_VAL(v);
        if (i < 0) fprintf(fp, "~%lld", (long long)(-i));
        else       fprintf(fp, "%lld", (long long)i);
        return;
    }
    if (v == ML_UNIT)  { fprintf(fp, "()"); return; }
    if (v == ML_TRUE)  { fprintf(fp, "true"); return; }
    if (v == ML_FALSE) { fprintf(fp, "false"); return; }
    if (v == ML_NIL)   { fprintf(fp, "[]"); return; }
    struct mlobj *o = ML_PTR(v);
    switch (o->type) {
      case MLOBJ_STRING:
        if (inside) {
            fprintf(fp, "\"");
            for (size_t i = 0; i < o->str.len; i++) {
                char ch = o->str.chars[i];
                if (ch == '\\') fprintf(fp, "\\\\");
                else if (ch == '"') fprintf(fp, "\\\"");
                else if (ch == '\n') fprintf(fp, "\\n");
                else if (ch == '\t') fprintf(fp, "\\t");
                else fputc(ch, fp);
            }
            fprintf(fp, "\"");
        } else {
            fwrite(o->str.chars, 1, o->str.len, fp);
        }
        return;
      case MLOBJ_REAL: {
        double d = o->dbl;
        if (d != d) { fprintf(fp, "nan"); return; }
        if (d < 0) { fprintf(fp, "~"); d = -d; }
        char buf[64];
        snprintf(buf, sizeof buf, "%.12g", d);
        bool has_dot = false;
        for (char *p = buf; *p; p++) if (*p == '.' || *p == 'e' || *p == 'E') { has_dot = true; break; }
        fprintf(fp, "%s%s", buf, has_dot ? "" : ".0");
        return;
      }
      case MLOBJ_CONS:
        fprintf(fp, "[");
        ml_display_inner(fp, o->cons.head, true);
        ml_display_list_tail(fp, o->cons.tail);
        return;
      case MLOBJ_TUPLE:
        fprintf(fp, "(");
        for (int i = 0; i < o->tup.n; i++) {
            if (i) fprintf(fp, ",");
            ml_display_inner(fp, o->tup.items[i], true);
        }
        fprintf(fp, ")");
        return;
      case MLOBJ_REF:
        fprintf(fp, "ref ");
        ml_display_inner(fp, o->refval, true);
        return;
      case MLOBJ_CLOSURE:
        fprintf(fp, "fn");
        return;
      case MLOBJ_PRIM:
        fprintf(fp, "fn");
        return;
      case MLOBJ_VARIANT:
        if (o->var.n == 0) fprintf(fp, "%s", o->var.name);
        else {
            fprintf(fp, "%s ", o->var.name);
            ml_display_inner(fp, o->var.items[0], true);
        }
        return;
      case MLOBJ_RECORD:
        fprintf(fp, "{");
        for (int i = 0; i < o->rec.n; i++) {
            if (i) fprintf(fp, ",");
            fprintf(fp, "%s=", o->rec.fields[i]);
            ml_display_inner(fp, o->rec.items[i], true);
        }
        fprintf(fp, "}");
        return;
      default:
        fprintf(fp, "<?>");
        return;
    }
}

void
ml_display(FILE *fp, VALUE v) { ml_display_inner(fp, v, true); }

// ---------------------------------------------------------------------------
// Side tables.
// ---------------------------------------------------------------------------

NODE **ML_LETREC_VALUES = NULL; size_t ML_LETREC_VALUES_LEN = 0, ML_LETREC_VALUES_CAP = 0;
NODE **ML_EXTRACT_NODES = NULL; size_t ML_EXTRACT_NODES_LEN = 0, ML_EXTRACT_NODES_CAP = 0;
NODE **ML_TUPLE_ITEMS  = NULL;  size_t ML_TUPLE_ITEMS_LEN  = 0, ML_TUPLE_ITEMS_CAP  = 0;
NODE **ML_CALL_ARGS    = NULL;  size_t ML_CALL_ARGS_LEN    = 0, ML_CALL_ARGS_CAP    = 0;

const char **ML_RECORD_FIELDS = NULL; size_t ML_RECORD_FIELDS_LEN = 0, ML_RECORD_FIELDS_CAP = 0;

static uint32_t
push_nodes(NODE ***vec, size_t *len, size_t *cap, NODE **items, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap : 16;
        while (*len + n > nc) nc *= 2;
        *vec = (NODE **)realloc(*vec, nc * sizeof(NODE *));
        *cap = nc;
    }
    uint32_t idx = (uint32_t)*len;
    for (size_t i = 0; i < n; i++) (*vec)[*len + i] = items[i];
    *len += n;
    return idx;
}

static uint32_t
push_strings(const char ***vec, size_t *len, size_t *cap, const char **items, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap : 16;
        while (*len + n > nc) nc *= 2;
        *vec = (const char **)realloc(*vec, nc * sizeof(char *));
        *cap = nc;
    }
    uint32_t idx = (uint32_t)*len;
    for (size_t i = 0; i < n; i++) (*vec)[*len + i] = items[i];
    *len += n;
    return idx;
}

static NODE **AOT_ENTRIES = NULL;
static size_t AOT_ENTRIES_LEN = 0, AOT_ENTRIES_CAP = 0;
static size_t AOT_COMPILED = 0;

static void
aot_add_entry(NODE *n)
{
    if (AOT_ENTRIES_LEN == AOT_ENTRIES_CAP) {
        size_t nc = AOT_ENTRIES_CAP ? AOT_ENTRIES_CAP * 2 : 64;
        AOT_ENTRIES = (NODE **)realloc(AOT_ENTRIES, nc * sizeof(NODE *));
        AOT_ENTRIES_CAP = nc;
    }
    AOT_ENTRIES[AOT_ENTRIES_LEN++] = n;
}

// ---------------------------------------------------------------------------
// String pool.
// ---------------------------------------------------------------------------

static char **STRPOOL = NULL;
static size_t STRPOOL_LEN = 0, STRPOOL_CAP = 0;

static const char *
intern(const char *s)
{
    for (size_t i = 0; i < STRPOOL_LEN; i++)
        if (strcmp(STRPOOL[i], s) == 0) return STRPOOL[i];
    if (STRPOOL_LEN == STRPOOL_CAP) {
        size_t nc = STRPOOL_CAP ? STRPOOL_CAP * 2 : 128;
        STRPOOL = (char **)realloc(STRPOOL, nc * sizeof(char *));
        STRPOOL_CAP = nc;
    }
    STRPOOL[STRPOOL_LEN] = strdup(s);
    return STRPOOL[STRPOOL_LEN++];
}

// ---------------------------------------------------------------------------
// Lexer.
// ---------------------------------------------------------------------------

enum tk {
    TK_EOF = 0,
    TK_INT, TK_REAL, TK_STR, TK_ID, TK_CTOR,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_COMMA, TK_SEMI, TK_BAR,
    TK_EQ, TK_DARROW, TK_ARROW, TK_COLON, TK_DCOLON, TK_AT, TK_CARET,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_TILDE,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_NEQ, TK_DOT, TK_ASSIGN,
    TK_VAL, TK_FUN, TK_AND, TK_REC,
    TK_IF, TK_THEN, TK_ELSE,
    TK_LET, TK_IN, TK_END,
    TK_FN, TK_CASE, TK_OF, TK_DATATYPE,
    TK_DIV, TK_MOD,
    TK_ANDALSO, TK_ORELSE, TK_NOT,
    TK_TRUE, TK_FALSE, TK_NIL, TK_UNIT_LIT,
    TK_RAISE, TK_HANDLE,
    TK_REF, TK_OP, TK_UNDERSCORE,
    TK_LBRACE, TK_RBRACE,           // { }
    TK_HASH,                         // #  (record field selector)
};

struct token {
    enum tk     kind;
    const char *str;
    int64_t     ival;
    double      rval;
    int         line;
};

static const char  *src;
static size_t       src_pos;
static size_t       src_len;
static int          src_line;
static struct token tk;

static void
lex_skip_ws(void)
{
    for (;;) {
        while (src_pos < src_len && isspace((unsigned char)src[src_pos])) {
            if (src[src_pos] == '\n') src_line++;
            src_pos++;
        }
        if (src_pos + 1 < src_len && src[src_pos] == '(' && src[src_pos+1] == '*') {
            int depth = 1; src_pos += 2;
            while (src_pos + 1 < src_len && depth > 0) {
                if (src[src_pos] == '(' && src[src_pos+1] == '*') { depth++; src_pos += 2; }
                else if (src[src_pos] == '*' && src[src_pos+1] == ')') { depth--; src_pos += 2; }
                else { if (src[src_pos] == '\n') src_line++; src_pos++; }
            }
            continue;
        }
        break;
    }
}

static bool is_id_start(int c) { return isalpha(c) || c == '_' || c == '\''; }
static bool is_id_cont(int c)  { return isalnum(c) || c == '_' || c == '\''; }

static void
lex(void)
{
    lex_skip_ws();
    tk.line = src_line;
    if (src_pos >= src_len) { tk.kind = TK_EOF; return; }
    int ch = (unsigned char)src[src_pos];

    if (is_id_start(ch)) {
        size_t start = src_pos++;
        while (src_pos < src_len && is_id_cont((unsigned char)src[src_pos])) src_pos++;
        // Allow `Module.name` qualified ids: re-extend across `.` followed
        // by another identifier-start.  Stops the parser from seeing
        // `Int` (CTOR) `.` `toString` (ID) as three tokens.
        while (src_pos + 1 < src_len && src[src_pos] == '.' &&
               is_id_start((unsigned char)src[src_pos + 1])) {
            src_pos++;       // skip '.'
            while (src_pos < src_len && is_id_cont((unsigned char)src[src_pos])) src_pos++;
        }
        size_t len = src_pos - start;
        char buf[256];
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, src + start, len); buf[len] = '\0';
        struct kw { const char *s; int kind; };
        static const struct kw kws[] = {
            {"val",TK_VAL},{"fun",TK_FUN},{"and",TK_AND},{"rec",TK_REC},
            {"if",TK_IF},{"then",TK_THEN},{"else",TK_ELSE},
            {"let",TK_LET},{"in",TK_IN},{"end",TK_END},
            {"fn",TK_FN},{"case",TK_CASE},{"of",TK_OF},
            {"datatype",TK_DATATYPE},
            {"div",TK_DIV},{"mod",TK_MOD},
            {"andalso",TK_ANDALSO},{"orelse",TK_ORELSE},{"not",TK_NOT},
            {"true",TK_TRUE},{"false",TK_FALSE},{"nil",TK_NIL},
            {"raise",TK_RAISE},{"handle",TK_HANDLE},
            {"op",TK_OP},
            {NULL,0}
        };
        for (int i = 0; kws[i].s; i++)
            if (strcmp(buf, kws[i].s) == 0) { tk.kind = kws[i].kind; return; }
        if (strcmp(buf, "_") == 0) { tk.kind = TK_UNDERSCORE; return; }
        tk.str = intern(buf);
        // Qualified `Module.name` is always a value-id (TK_ID), even
        // though it starts with a capital letter.
        bool dotted = strchr(buf, '.') != NULL;
        tk.kind = (!dotted && isupper((unsigned char)buf[0])) ? TK_CTOR : TK_ID;
        return;
    }

    if (isdigit(ch)) {
        size_t start = src_pos;
        while (src_pos < src_len && isdigit((unsigned char)src[src_pos])) src_pos++;
        bool is_real = false;
        if (src_pos < src_len && src[src_pos] == '.' &&
            src_pos + 1 < src_len && isdigit((unsigned char)src[src_pos+1])) {
            is_real = true;
            src_pos++;
            while (src_pos < src_len && isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        if (src_pos < src_len && (src[src_pos] == 'e' || src[src_pos] == 'E')) {
            is_real = true;
            src_pos++;
            if (src_pos < src_len && (src[src_pos] == '+' || src[src_pos] == '-' || src[src_pos] == '~')) src_pos++;
            while (src_pos < src_len && isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        size_t len = src_pos - start;
        char buf[64];
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, src + start, len); buf[len] = '\0';
        if (is_real) {
            for (char *p = buf; *p; p++) if (*p == '~') *p = '-';
            tk.kind = TK_REAL; tk.rval = strtod(buf, NULL);
        } else {
            tk.kind = TK_INT;  tk.ival = strtoll(buf, NULL, 10);
        }
        return;
    }

    if (ch == '"') {
        src_pos++;
        char buf[4096]; size_t bl = 0;
        while (src_pos < src_len && src[src_pos] != '"') {
            char c = src[src_pos++];
            if (c == '\\' && src_pos < src_len) {
                char esc = src[src_pos++];
                switch (esc) {
                  case 'n': c = '\n'; break;
                  case 't': c = '\t'; break;
                  case 'r': c = '\r'; break;
                  case '\\': c = '\\'; break;
                  case '"': c = '"'; break;
                  default: c = esc; break;
                }
            } else if (c == '\n') src_line++;
            if (bl < sizeof(buf) - 1) buf[bl++] = c;
        }
        if (src_pos < src_len) src_pos++;
        buf[bl] = '\0';
        tk.kind = TK_STR;
        char *s = (char *)malloc(bl + 1);
        memcpy(s, buf, bl + 1);
        tk.str = s;
        tk.ival = (int64_t)bl;
        return;
    }

    if (ch == '!') {
        src_pos++;
        // `!` is the deref operator — surface as a regular identifier so
        // it dispatches via the prelude prim.  `!=` would be an SML typo
        // but we don't have it as a token, so a bare `!` is fine.
        tk.kind = TK_ID;
        tk.str = intern("!");
        return;
    }
    src_pos++;
    switch (ch) {
      case '(':
        if (src_pos < src_len && src[src_pos] == ')') { src_pos++; tk.kind = TK_UNIT_LIT; return; }
        tk.kind = TK_LPAREN; return;
      case ')': tk.kind = TK_RPAREN; return;
      case '[':
        if (src_pos < src_len && src[src_pos] == ']') { src_pos++; tk.kind = TK_NIL; return; }
        tk.kind = TK_LBRACK; return;
      case ']': tk.kind = TK_RBRACK; return;
      case ',': tk.kind = TK_COMMA; return;
      case ';': tk.kind = TK_SEMI;  return;
      case '|': tk.kind = TK_BAR;   return;
      case '=':
        if (src_pos < src_len && src[src_pos] == '>') { src_pos++; tk.kind = TK_DARROW; return; }
        tk.kind = TK_EQ; return;
      case '+': tk.kind = TK_PLUS;  return;
      case '-':
        if (src_pos < src_len && src[src_pos] == '>') { src_pos++; tk.kind = TK_ARROW; return; }
        tk.kind = TK_MINUS; return;
      case '*': tk.kind = TK_STAR; return;
      case '/': tk.kind = TK_SLASH; return;
      case '~': tk.kind = TK_TILDE; return;
      case '<':
        if (src_pos < src_len && src[src_pos] == '=') { src_pos++; tk.kind = TK_LE; return; }
        if (src_pos < src_len && src[src_pos] == '>') { src_pos++; tk.kind = TK_NEQ; return; }
        tk.kind = TK_LT; return;
      case '>':
        if (src_pos < src_len && src[src_pos] == '=') { src_pos++; tk.kind = TK_GE; return; }
        tk.kind = TK_GT; return;
      case ':':
        if (src_pos < src_len && src[src_pos] == ':') { src_pos++; tk.kind = TK_DCOLON; return; }
        if (src_pos < src_len && src[src_pos] == '=') { src_pos++; tk.kind = TK_ASSIGN; return; }
        tk.kind = TK_COLON; return;
      case '@': tk.kind = TK_AT;     return;
      case '^': tk.kind = TK_CARET;  return;
      case '.': tk.kind = TK_DOT;    return;
      case '{': tk.kind = TK_LBRACE; return;
      case '}': tk.kind = TK_RBRACE; return;
      case '#': tk.kind = TK_HASH;   return;
      default:
        fprintf(stderr, "asml: lex error at line %d: unexpected '%c'\n", src_line, ch);
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// Type system — Hindley-Milner with level-based generalisation
// (Remy's algorithm).  Type variables are mutable: when unified with a
// type, their `link` is set; subsequent dereferences chase the chain.
// `level` records the let-binding depth at the var's birth, so
// generalise(level, t) quantifies only the vars created strictly inside
// the RHS we just inferred.
// ---------------------------------------------------------------------------

enum ty_kind {
    TYK_INT, TYK_REAL, TYK_STRING, TYK_BOOL, TYK_UNIT, TYK_EXN,
    TYK_VAR,
    TYK_ARR,
    TYK_TUP,
    TYK_LIST,
    TYK_REF,
    TYK_CON,
    TYK_RECORD,
};

typedef struct ty TY;
struct ty {
    enum ty_kind kind;
    union {
        // TYK_VAR
        struct {
            TY  *link;
            int  level;        // INT_MAX when generic
            int  id;           // for printing
            int  q_idx;        // generalised position (for instantiation)
        } var;
        struct { TY *param, *result; } arr;
        struct { int n; TY **items; } tup;
        TY  *list_elt;
        TY  *ref_elt;
        struct { const char *name; int n; TY **args; } con;
        struct { int n; const char **fields; TY **types; } rec;   // sorted fields
    };
};

#define TY_LEVEL_GENERIC 0x7fffffff

struct ty_scheme {
    int  n_quants;          // number of generic vars
    TY  *body;
};

static TY TY_INT_OBJ    = { .kind = TYK_INT };
static TY TY_REAL_OBJ   = { .kind = TYK_REAL };
static TY TY_STRING_OBJ = { .kind = TYK_STRING };
static TY TY_BOOL_OBJ   = { .kind = TYK_BOOL };
static TY TY_UNIT_OBJ   = { .kind = TYK_UNIT };
static TY TY_EXN_OBJ    = { .kind = TYK_EXN };

static int TY_VAR_ID_SEQ = 0;

static TY *ty_int(void)    { return &TY_INT_OBJ; }
static TY *ty_real(void)   { return &TY_REAL_OBJ; }
static TY *ty_string(void) { return &TY_STRING_OBJ; }
static TY *ty_bool(void)   { return &TY_BOOL_OBJ; }
static TY *ty_unit(void)   { return &TY_UNIT_OBJ; }
static TY *ty_exn(void)    { return &TY_EXN_OBJ; }

static TY *
ty_alloc(enum ty_kind k)
{
    TY *t = (TY *)calloc(1, sizeof *t);
    t->kind = k;
    return t;
}

static TY *
ty_var(int level)
{
    TY *t = ty_alloc(TYK_VAR);
    t->var.link  = NULL;
    t->var.level = level;
    t->var.id    = TY_VAR_ID_SEQ++;
    return t;
}

static TY *ty_arr(TY *p, TY *r)  { TY *t = ty_alloc(TYK_ARR); t->arr.param = p; t->arr.result = r; return t; }
static TY *ty_list(TY *e)        { TY *t = ty_alloc(TYK_LIST); t->list_elt = e; return t; }
static TY *ty_ref(TY *e)         { TY *t = ty_alloc(TYK_REF); t->ref_elt = e; return t; }

static TY *
ty_tup(int n, TY **items)
{
    TY *t = ty_alloc(TYK_TUP);
    t->tup.n = n;
    t->tup.items = (TY **)malloc(sizeof(TY *) * (n ? n : 1));
    for (int i = 0; i < n; i++) t->tup.items[i] = items[i];
    return t;
}

static TY *
ty_con(const char *name, int n, TY **args)
{
    TY *t = ty_alloc(TYK_CON);
    t->con.name = name;
    t->con.n = n;
    t->con.args = NULL;
    if (n > 0) {
        t->con.args = (TY **)malloc(sizeof(TY *) * n);
        for (int i = 0; i < n; i++) t->con.args[i] = args[i];
    }
    return t;
}

// fields は sort 済を期待。
static TY *
ty_record(int n, const char **fields, TY **types)
{
    TY *t = ty_alloc(TYK_RECORD);
    t->rec.n = n;
    t->rec.fields = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    t->rec.types  = (TY **)malloc(sizeof(TY *) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        t->rec.fields[i] = fields[i];
        t->rec.types[i]  = types[i];
    }
    return t;
}

// Walk var.link chain.
static TY *
ty_deref(TY *t)
{
    while (t->kind == TYK_VAR && t->var.link) {
        // Path compression: rebind to fully dereffed.
        TY *next = t->var.link;
        while (next->kind == TYK_VAR && next->var.link) next = next->var.link;
        t->var.link = next;
        t = next;
    }
    return t;
}

// Pretty-print a type.  Uses a small per-call ID-to-letter table so vars
// come out as 'a, 'b, etc.
struct ty_pp {
    int   ids[64];
    int   n;
    char *buf;
    int   bcap;
    int   blen;
};

static void
ty_pp_grow(struct ty_pp *p, int extra)
{
    while (p->blen + extra >= p->bcap) {
        p->bcap = p->bcap ? p->bcap * 2 : 64;
        p->buf = (char *)realloc(p->buf, p->bcap);
    }
}
static void
ty_pp_str(struct ty_pp *p, const char *s)
{
    int l = strlen(s); ty_pp_grow(p, l + 1);
    memcpy(p->buf + p->blen, s, l); p->blen += l; p->buf[p->blen] = '\0';
}
static void
ty_pp_ch(struct ty_pp *p, char c)
{ ty_pp_grow(p, 2); p->buf[p->blen++] = c; p->buf[p->blen] = '\0'; }

static void
ty_pp_var(struct ty_pp *p, int id)
{
    int letter = -1;
    for (int i = 0; i < p->n; i++) if (p->ids[i] == id) { letter = i; break; }
    if (letter < 0) {
        if (p->n < (int)(sizeof(p->ids)/sizeof(p->ids[0]))) {
            letter = p->n;
            p->ids[p->n++] = id;
        } else letter = id & 0x1f;
    }
    char buf[8];
    if (letter < 26) snprintf(buf, sizeof buf, "'%c", 'a' + letter);
    else snprintf(buf, sizeof buf, "'%c%d", 'a' + (letter % 26), letter / 26);
    ty_pp_str(p, buf);
}

static void ty_pp_walk(struct ty_pp *p, TY *t, int prec);

static void
ty_pp_walk(struct ty_pp *p, TY *t, int prec)
{
    t = ty_deref(t);
    switch (t->kind) {
      case TYK_INT:    ty_pp_str(p, "int"); return;
      case TYK_REAL:   ty_pp_str(p, "real"); return;
      case TYK_STRING: ty_pp_str(p, "string"); return;
      case TYK_BOOL:   ty_pp_str(p, "bool"); return;
      case TYK_UNIT:   ty_pp_str(p, "unit"); return;
      case TYK_EXN:    ty_pp_str(p, "exn"); return;
      case TYK_VAR:    ty_pp_var(p, t->var.id); return;
      case TYK_ARR: {
        if (prec > 0) ty_pp_ch(p, '(');
        ty_pp_walk(p, t->arr.param, 1);
        ty_pp_str(p, " -> ");
        ty_pp_walk(p, t->arr.result, 0);
        if (prec > 0) ty_pp_ch(p, ')');
        return;
      }
      case TYK_TUP: {
        if (prec > 1) ty_pp_ch(p, '(');
        for (int i = 0; i < t->tup.n; i++) {
            if (i) ty_pp_str(p, " * ");
            ty_pp_walk(p, t->tup.items[i], 2);
        }
        if (prec > 1) ty_pp_ch(p, ')');
        return;
      }
      case TYK_LIST:
        ty_pp_walk(p, t->list_elt, 3);
        ty_pp_str(p, " list");
        return;
      case TYK_REF:
        ty_pp_walk(p, t->ref_elt, 3);
        ty_pp_str(p, " ref");
        return;
      case TYK_CON: {
        if (t->con.n == 0) { ty_pp_str(p, t->con.name); return; }
        if (t->con.n == 1) {
            ty_pp_walk(p, t->con.args[0], 3);
            ty_pp_ch(p, ' ');
            ty_pp_str(p, t->con.name);
        } else {
            ty_pp_ch(p, '(');
            for (int i = 0; i < t->con.n; i++) {
                if (i) ty_pp_str(p, ", ");
                ty_pp_walk(p, t->con.args[i], 0);
            }
            ty_pp_ch(p, ')');
            ty_pp_ch(p, ' ');
            ty_pp_str(p, t->con.name);
        }
        return;
      }
      case TYK_RECORD: {
        ty_pp_ch(p, '{');
        for (int i = 0; i < t->rec.n; i++) {
            if (i) ty_pp_str(p, ", ");
            ty_pp_str(p, t->rec.fields[i]);
            ty_pp_str(p, ": ");
            ty_pp_walk(p, t->rec.types[i], 0);
        }
        ty_pp_ch(p, '}');
        return;
      }
    }
}

// One-shot formatter: caller must free the returned buffer.
static char *
ty_format(TY *t)
{
    struct ty_pp p = {0};
    ty_pp_walk(&p, t, 0);
    return p.buf ? p.buf : strdup("?");
}

// ---------------------------------------------------------------------------
// Type errors.
// ---------------------------------------------------------------------------

__attribute__((noreturn,format(printf,2,3)))
static void
type_error(int line, const char *fmt, ...)
{
    fprintf(stderr, "asml: type error at line %d: ", line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

// ---------------------------------------------------------------------------
// Occurs check + level adjustment.  When we're about to bind ?a := t,
// we walk t to (a) verify ?a doesn't appear in t (would create a cycle)
// and (b) lower the level of any var inside t to min(its_level, ?a's
// level) so generalisation later doesn't grab vars that are still in
// use at outer scope.
// ---------------------------------------------------------------------------

static void
ty_adjust_levels(TY *t, int target_level, int v_id, int v_line)
{
    t = ty_deref(t);
    switch (t->kind) {
      case TYK_VAR:
        if (t->var.id == v_id) type_error(v_line, "occurs check failed");
        if (t->var.level > target_level) t->var.level = target_level;
        return;
      case TYK_ARR:
        ty_adjust_levels(t->arr.param,  target_level, v_id, v_line);
        ty_adjust_levels(t->arr.result, target_level, v_id, v_line);
        return;
      case TYK_TUP:
        for (int i = 0; i < t->tup.n; i++) ty_adjust_levels(t->tup.items[i], target_level, v_id, v_line);
        return;
      case TYK_LIST: ty_adjust_levels(t->list_elt, target_level, v_id, v_line); return;
      case TYK_REF:  ty_adjust_levels(t->ref_elt,  target_level, v_id, v_line); return;
      case TYK_CON:
        for (int i = 0; i < t->con.n; i++) ty_adjust_levels(t->con.args[i], target_level, v_id, v_line);
        return;
      case TYK_RECORD:
        for (int i = 0; i < t->rec.n; i++) ty_adjust_levels(t->rec.types[i], target_level, v_id, v_line);
        return;
      default: return;
    }
}

// ---------------------------------------------------------------------------
// Unify.  The caller's `line` is used for error messages.
// ---------------------------------------------------------------------------

static void
ty_unify_at(int line, TY *a, TY *b)
{
    a = ty_deref(a);
    b = ty_deref(b);
    if (a == b) return;
    if (a->kind == TYK_VAR) {
        ty_adjust_levels(b, a->var.level, a->var.id, line);
        a->var.link = b;
        return;
    }
    if (b->kind == TYK_VAR) {
        ty_adjust_levels(a, b->var.level, b->var.id, line);
        b->var.link = a;
        return;
    }
    if (a->kind != b->kind) goto mismatch;
    switch (a->kind) {
      case TYK_INT: case TYK_REAL: case TYK_STRING:
      case TYK_BOOL: case TYK_UNIT: case TYK_EXN:
        return;
      case TYK_ARR:
        ty_unify_at(line, a->arr.param,  b->arr.param);
        ty_unify_at(line, a->arr.result, b->arr.result);
        return;
      case TYK_TUP:
        if (a->tup.n != b->tup.n) goto mismatch;
        for (int i = 0; i < a->tup.n; i++) ty_unify_at(line, a->tup.items[i], b->tup.items[i]);
        return;
      case TYK_LIST: ty_unify_at(line, a->list_elt, b->list_elt); return;
      case TYK_REF:  ty_unify_at(line, a->ref_elt,  b->ref_elt);  return;
      case TYK_CON:
        if (strcmp(a->con.name, b->con.name) != 0) goto mismatch;
        if (a->con.n != b->con.n) goto mismatch;
        for (int i = 0; i < a->con.n; i++) ty_unify_at(line, a->con.args[i], b->con.args[i]);
        return;
      case TYK_RECORD:
        if (a->rec.n != b->rec.n) goto mismatch;
        for (int i = 0; i < a->rec.n; i++) {
            if (strcmp(a->rec.fields[i], b->rec.fields[i]) != 0) goto mismatch;
            ty_unify_at(line, a->rec.types[i], b->rec.types[i]);
        }
        return;
      case TYK_VAR: return;     // handled above
    }
mismatch: {
    char *as = ty_format(a), *bs = ty_format(b);
    char buf[512];
    snprintf(buf, sizeof buf, "cannot unify %s with %s", as, bs);
    free(as); free(bs);
    type_error(line, "%s", buf);
}}

// ---------------------------------------------------------------------------
// Generalise / instantiate.  After inferring a let-RHS at level `level`,
// generalise quantifies any vars whose level is strictly greater than
// `level` (they were born inside the RHS and don't escape).  The
// resulting scheme stores a `q_idx` on each generalised var so
// instantiation can build a fresh array of replacements indexed by it.
// ---------------------------------------------------------------------------

static int GEN_COUNTER;

static void
ty_collect_gen_vars(TY *t, int level)
{
    t = ty_deref(t);
    switch (t->kind) {
      case TYK_VAR:
        if (t->var.level > level && t->var.q_idx == 0 && t->var.level != TY_LEVEL_GENERIC) {
            t->var.q_idx = ++GEN_COUNTER;
        }
        return;
      case TYK_ARR:
        ty_collect_gen_vars(t->arr.param, level);
        ty_collect_gen_vars(t->arr.result, level);
        return;
      case TYK_TUP:
        for (int i = 0; i < t->tup.n; i++) ty_collect_gen_vars(t->tup.items[i], level);
        return;
      case TYK_LIST: ty_collect_gen_vars(t->list_elt, level); return;
      case TYK_REF:  ty_collect_gen_vars(t->ref_elt,  level); return;
      case TYK_CON:
        for (int i = 0; i < t->con.n; i++) ty_collect_gen_vars(t->con.args[i], level);
        return;
      case TYK_RECORD:
        for (int i = 0; i < t->rec.n; i++) ty_collect_gen_vars(t->rec.types[i], level);
        return;
      default: return;
    }
}

static struct ty_scheme *
ty_generalize(int level, TY *t)
{
    GEN_COUNTER = 0;
    ty_collect_gen_vars(t, level);
    struct ty_scheme *s = (struct ty_scheme *)calloc(1, sizeof *s);
    s->n_quants = GEN_COUNTER;
    s->body = t;
    return s;
}

static struct ty_scheme *
ty_mono_scheme(TY *t)
{
    struct ty_scheme *s = (struct ty_scheme *)calloc(1, sizeof *s);
    s->n_quants = 0;
    s->body = t;
    return s;
}

static TY *
ty_instantiate_walk(TY *t, TY **fresh, int n_fresh, int new_level)
{
    t = ty_deref(t);
    switch (t->kind) {
      case TYK_VAR:
        if (t->var.q_idx > 0 && t->var.q_idx <= n_fresh)
            return fresh[t->var.q_idx - 1];
        return t;
      case TYK_ARR: {
        TY *p = ty_instantiate_walk(t->arr.param, fresh, n_fresh, new_level);
        TY *r = ty_instantiate_walk(t->arr.result, fresh, n_fresh, new_level);
        if (p == t->arr.param && r == t->arr.result) return t;
        return ty_arr(p, r);
      }
      case TYK_TUP: {
        TY **its = (TY **)alloca(sizeof(TY *) * (t->tup.n ? t->tup.n : 1));
        bool changed = false;
        for (int i = 0; i < t->tup.n; i++) {
            its[i] = ty_instantiate_walk(t->tup.items[i], fresh, n_fresh, new_level);
            if (its[i] != t->tup.items[i]) changed = true;
        }
        if (!changed) return t;
        return ty_tup(t->tup.n, its);
      }
      case TYK_LIST: {
        TY *e = ty_instantiate_walk(t->list_elt, fresh, n_fresh, new_level);
        return e == t->list_elt ? t : ty_list(e);
      }
      case TYK_REF: {
        TY *e = ty_instantiate_walk(t->ref_elt, fresh, n_fresh, new_level);
        return e == t->ref_elt ? t : ty_ref(e);
      }
      case TYK_CON: {
        if (t->con.n == 0) return t;
        TY **its = (TY **)alloca(sizeof(TY *) * t->con.n);
        bool changed = false;
        for (int i = 0; i < t->con.n; i++) {
            its[i] = ty_instantiate_walk(t->con.args[i], fresh, n_fresh, new_level);
            if (its[i] != t->con.args[i]) changed = true;
        }
        if (!changed) return t;
        return ty_con(t->con.name, t->con.n, its);
      }
      case TYK_RECORD: {
        if (t->rec.n == 0) return t;
        TY **its = (TY **)alloca(sizeof(TY *) * t->rec.n);
        bool changed = false;
        for (int i = 0; i < t->rec.n; i++) {
            its[i] = ty_instantiate_walk(t->rec.types[i], fresh, n_fresh, new_level);
            if (its[i] != t->rec.types[i]) changed = true;
        }
        if (!changed) return t;
        return ty_record(t->rec.n, t->rec.fields, its);
      }
      default: return t;
    }
}

static TY *
ty_instantiate(struct ty_scheme *s, int new_level)
{
    if (s->n_quants == 0) return s->body;
    TY **fresh = (TY **)alloca(sizeof(TY *) * s->n_quants);
    for (int i = 0; i < s->n_quants; i++) fresh[i] = ty_var(new_level);
    return ty_instantiate_walk(s->body, fresh, s->n_quants, new_level);
}

// ---------------------------------------------------------------------------
// Type environment + globals + datatype/constructor registries.
// ---------------------------------------------------------------------------
//
// Lexical type env: parallel to the runtime frame chain.  Each frame is a
// flat array of (name, scheme) pairs, indexed identically to the runtime
// frame's slots so EX_LREF (depth, idx) maps directly.

struct ty_env_slot { const char *name; struct ty_scheme *scheme; };

struct ty_frame {
    int                 n;
    int                 cap;
    struct ty_env_slot *slots;
};

struct ty_env {
    int               n_frames;
    int               cap;
    struct ty_frame **frames;
};

static struct ty_env *TENV = NULL;

static void
tenv_init(void)
{
    TENV = (struct ty_env *)calloc(1, sizeof *TENV);
}

static void
tenv_push(void)
{
    if (TENV->n_frames == TENV->cap) {
        int nc = TENV->cap ? TENV->cap * 2 : 8;
        TENV->frames = (struct ty_frame **)realloc(TENV->frames, nc * sizeof(struct ty_frame *));
        TENV->cap = nc;
    }
    TENV->frames[TENV->n_frames++] = (struct ty_frame *)calloc(1, sizeof(struct ty_frame));
}

static void
tenv_pop(void)
{
    struct ty_frame *f = TENV->frames[--TENV->n_frames];
    free(f->slots);
    free(f);
}

static void
tenv_add(const char *name, struct ty_scheme *scheme)
{
    struct ty_frame *f = TENV->frames[TENV->n_frames - 1];
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 4;
        f->slots = (struct ty_env_slot *)realloc(f->slots, nc * sizeof(struct ty_env_slot));
        f->cap = nc;
    }
    f->slots[f->n].name = name;
    f->slots[f->n].scheme = scheme;
    f->n++;
}

static struct ty_scheme *
tenv_lookup_local(uint32_t depth, uint32_t idx)
{
    if ((int)depth >= TENV->n_frames) return NULL;
    struct ty_frame *f = TENV->frames[TENV->n_frames - 1 - depth];
    if ((int)idx >= f->n) return NULL;
    return f->slots[idx].scheme;
}

// Globals — type schemes for top-level vals / funs / prelude.
struct gentry_ty {
    const char       *name;
    struct ty_scheme *scheme;
};
static struct gentry_ty *GLOBAL_TYPES = NULL;
static int GLOBAL_TYPES_LEN = 0, GLOBAL_TYPES_CAP = 0;

static void
ty_global_define(const char *name, struct ty_scheme *s)
{
    for (int i = 0; i < GLOBAL_TYPES_LEN; i++)
        if (strcmp(GLOBAL_TYPES[i].name, name) == 0) {
            GLOBAL_TYPES[i].scheme = s;
            return;
        }
    if (GLOBAL_TYPES_LEN == GLOBAL_TYPES_CAP) {
        int nc = GLOBAL_TYPES_CAP ? GLOBAL_TYPES_CAP * 2 : 64;
        GLOBAL_TYPES = (struct gentry_ty *)realloc(GLOBAL_TYPES, nc * sizeof(struct gentry_ty));
        GLOBAL_TYPES_CAP = nc;
    }
    GLOBAL_TYPES[GLOBAL_TYPES_LEN].name = name;
    GLOBAL_TYPES[GLOBAL_TYPES_LEN].scheme = s;
    GLOBAL_TYPES_LEN++;
}

static struct ty_scheme *
ty_global_lookup(const char *name)
{
    for (int i = 0; i < GLOBAL_TYPES_LEN; i++)
        if (strcmp(GLOBAL_TYPES[i].name, name) == 0) return GLOBAL_TYPES[i].scheme;
    return NULL;
}

// Constructor table — name → arity (already exists) + scheme + is-exception flag.
struct ctor_ty_entry {
    const char       *name;
    int               arity;       // 0 or 1
    bool              is_exn;      // true → result type is exn
    struct ty_scheme *scheme;      // for nullary: result ty;  for 1-arg: arg -> result
    // For nullary scheme: scheme.body has TYK_CON / TYK_EXN.
    // For 1-arg: scheme.body is TYK_ARR (param -> result).
};
static struct ctor_ty_entry *CTOR_TY_TABLE = NULL;
static int CTOR_TY_LEN = 0, CTOR_TY_CAP = 0;

static struct ctor_ty_entry *
ctor_ty_find(const char *name)
{
    for (int i = 0; i < CTOR_TY_LEN; i++)
        if (strcmp(CTOR_TY_TABLE[i].name, name) == 0) return &CTOR_TY_TABLE[i];
    return NULL;
}

static void
ctor_ty_register(const char *name, int arity, bool is_exn, struct ty_scheme *s)
{
    struct ctor_ty_entry *e = ctor_ty_find(name);
    if (e) { e->arity = arity; e->is_exn = is_exn; e->scheme = s; return; }
    if (CTOR_TY_LEN == CTOR_TY_CAP) {
        int nc = CTOR_TY_CAP ? CTOR_TY_CAP * 2 : 32;
        CTOR_TY_TABLE = (struct ctor_ty_entry *)realloc(CTOR_TY_TABLE, nc * sizeof(*CTOR_TY_TABLE));
        CTOR_TY_CAP = nc;
    }
    CTOR_TY_TABLE[CTOR_TY_LEN].name = name;
    CTOR_TY_TABLE[CTOR_TY_LEN].arity = arity;
    CTOR_TY_TABLE[CTOR_TY_LEN].is_exn = is_exn;
    CTOR_TY_TABLE[CTOR_TY_LEN].scheme = s;
    CTOR_TY_LEN++;
}

// Type-constructor registry — datatype names + their arities.  Used by
// the type parser to know how many arguments a `tycon` takes.
struct tycon_entry { const char *name; int arity; };
static struct tycon_entry *TYCON_TABLE = NULL;
static int TYCON_LEN = 0, TYCON_CAP = 0;

static int
tycon_arity(const char *name)
{
    for (int i = 0; i < TYCON_LEN; i++)
        if (strcmp(TYCON_TABLE[i].name, name) == 0) return TYCON_TABLE[i].arity;
    return -1;
}

static void
tycon_register(const char *name, int arity)
{
    for (int i = 0; i < TYCON_LEN; i++)
        if (strcmp(TYCON_TABLE[i].name, name) == 0) { TYCON_TABLE[i].arity = arity; return; }
    if (TYCON_LEN == TYCON_CAP) {
        int nc = TYCON_CAP ? TYCON_CAP * 2 : 16;
        TYCON_TABLE = (struct tycon_entry *)realloc(TYCON_TABLE, nc * sizeof(*TYCON_TABLE));
        TYCON_CAP = nc;
    }
    TYCON_TABLE[TYCON_LEN].name = name;
    TYCON_TABLE[TYCON_LEN].arity = arity;
    TYCON_LEN++;
}

// ---------------------------------------------------------------------------
// Parser scaffolding.
// ---------------------------------------------------------------------------

static void
parse_error(const char *msg)
{
    fprintf(stderr, "asml: parse error at line %d (token kind=%d): %s\n",
            tk.line, tk.kind, msg);
    exit(1);
}

static bool accept(int kind) { if (tk.kind == kind) { lex(); return true; } return false; }
static void expect(int kind, const char *what) { if (!accept(kind)) parse_error(what); }

// Lexical scopes — chain of frames; each frame is a flat list of names.

enum bind_kind { BK_VAR };

struct binding { const char *name; enum bind_kind kind; };

struct scope {
    struct scope    *parent;
    struct binding  *binds;
    size_t           size, cap;
};

static struct scope *cur_scope = NULL;

static void scope_push(void)
{
    struct scope *s = (struct scope *)calloc(1, sizeof *s);
    s->parent = cur_scope;
    cur_scope = s;
}

static void scope_pop(void)
{
    struct scope *s = cur_scope;
    cur_scope = s->parent;
    free(s->binds);
    free(s);
}

static void
scope_add(const char *name, enum bind_kind k)
{
    struct scope *s = cur_scope;
    if (s->size == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8;
        s->binds = (struct binding *)realloc(s->binds, nc * sizeof(struct binding));
        s->cap = nc;
    }
    s->binds[s->size].name = name;
    s->binds[s->size].kind = k;
    s->size++;
}

static bool
scope_lookup(const char *name, uint32_t *depth, uint32_t *idx)
{
    uint32_t d = 0;
    for (struct scope *s = cur_scope; s; s = s->parent) {
        for (size_t i = 0; i < s->size; i++) {
            if (strcmp(s->binds[i].name, name) == 0) {
                *depth = d; *idx = (uint32_t)i; return true;
            }
        }
        d++;
    }
    return false;
}

// Constructor table — global tags.
struct ctor_def { const char *name; int arity; };
static struct ctor_def *CTOR_TABLE = NULL;
static size_t CTOR_LEN = 0, CTOR_CAP = 0;

static void
register_ctor(const char *name, int arity)
{
    for (size_t i = 0; i < CTOR_LEN; i++)
        if (strcmp(CTOR_TABLE[i].name, name) == 0) { CTOR_TABLE[i].arity = arity; return; }
    if (CTOR_LEN == CTOR_CAP) {
        size_t nc = CTOR_CAP ? CTOR_CAP * 2 : 32;
        CTOR_TABLE = (struct ctor_def *)realloc(CTOR_TABLE, nc * sizeof(*CTOR_TABLE));
        CTOR_CAP = nc;
    }
    CTOR_TABLE[CTOR_LEN].name = name;
    CTOR_TABLE[CTOR_LEN].arity = arity;
    CTOR_LEN++;
}

static int
ctor_arity(const char *name)
{
    for (size_t i = 0; i < CTOR_LEN; i++)
        if (strcmp(CTOR_TABLE[i].name, name) == 0) return CTOR_TABLE[i].arity;
    return -1;
}

// ---------------------------------------------------------------------------
// Patterns (parser-internal).
// ---------------------------------------------------------------------------

enum pat_kind {
    P_WILDCARD, P_VAR, P_INT, P_BOOL, P_STR, P_UNIT, P_NIL,
    P_CONS, P_TUPLE, P_LIST, P_CTOR0, P_CTOR1, P_RECORD,
};

typedef struct pat {
    enum pat_kind kind;
    union {
        const char *var_name;
        int64_t     ival;
        bool        bval;
        const char *sval;
        struct { struct pat *h, *t; } cons;
        struct { struct pat **items; int n; } tup;
        struct { struct pat **items; int n; } lst;
        struct { const char *name; struct pat *arg; } ctor1;
        struct { int n; const char **fields; struct pat **items; } rec;  // sorted
    };
} PAT;

static PAT *pp_alloc(enum pat_kind k) { PAT *p = (PAT *)calloc(1, sizeof *p); p->kind = k; return p; }

static PAT *parse_pattern(void);
static PAT *parse_pattern_atom(void);

static PAT *
parse_pattern_atom(void)
{
    if (accept(TK_UNDERSCORE)) return pp_alloc(P_WILDCARD);
    if (accept(TK_TRUE))       { PAT *p = pp_alloc(P_BOOL); p->bval = true;  return p; }
    if (accept(TK_FALSE))      { PAT *p = pp_alloc(P_BOOL); p->bval = false; return p; }
    if (accept(TK_NIL))        return pp_alloc(P_NIL);
    if (accept(TK_UNIT_LIT))   return pp_alloc(P_UNIT);
    if (tk.kind == TK_INT) {
        PAT *p = pp_alloc(P_INT); p->ival = tk.ival; lex(); return p;
    }
    if (tk.kind == TK_TILDE) {
        lex();
        if (tk.kind != TK_INT) parse_error("expected int after ~");
        PAT *p = pp_alloc(P_INT); p->ival = -tk.ival; lex(); return p;
    }
    if (tk.kind == TK_STR) {
        PAT *p = pp_alloc(P_STR); p->sval = tk.str; lex(); return p;
    }
    if (tk.kind == TK_CTOR) {
        const char *name = tk.str; lex();
        PAT *p = pp_alloc(P_CTOR0); p->var_name = name;
        return p;
    }
    if (tk.kind == TK_ID) {
        const char *name = tk.str; lex();
        PAT *p = pp_alloc(P_VAR); p->var_name = name;
        return p;
    }
    if (accept(TK_LPAREN)) {
        if (accept(TK_RPAREN)) return pp_alloc(P_UNIT);
        PAT *first = parse_pattern();
        if (accept(TK_RPAREN)) return first;
        PAT **items = NULL; int n = 1, cap = 4;
        items = (PAT **)malloc(sizeof(PAT *) * cap);
        items[0] = first;
        while (accept(TK_COMMA)) {
            if (n == cap) { cap *= 2; items = (PAT **)realloc(items, sizeof(PAT *) * cap); }
            items[n++] = parse_pattern();
        }
        expect(TK_RPAREN, "expected ')'");
        PAT *p = pp_alloc(P_TUPLE); p->tup.items = items; p->tup.n = n;
        return p;
    }
    if (accept(TK_LBRACK)) {
        if (accept(TK_RBRACK)) return pp_alloc(P_NIL);
        PAT **items = NULL; int n = 0, cap = 4;
        items = (PAT **)malloc(sizeof(PAT *) * cap);
        items[n++] = parse_pattern();
        while (accept(TK_COMMA)) {
            if (n == cap) { cap *= 2; items = (PAT **)realloc(items, sizeof(PAT *) * cap); }
            items[n++] = parse_pattern();
        }
        expect(TK_RBRACK, "expected ']'");
        PAT *p = pp_alloc(P_LIST); p->lst.items = items; p->lst.n = n;
        return p;
    }
    if (accept(TK_LBRACE)) {
        // record パターン: `{f1 = p1, f2 = p2, ...}` または短縮 `{f1, f2, ...}` (= `{f1 = f1, ...}`)
        const char **fields = NULL; PAT **subs = NULL;
        int n = 0, cap = 4;
        fields = (const char **)malloc(sizeof(char *) * cap);
        subs   = (PAT **)malloc(sizeof(PAT *) * cap);
        if (!accept(TK_RBRACE)) {
            for (;;) {
                if (tk.kind != TK_ID) parse_error("expected field name in record pattern");
                if (n == cap) { cap *= 2;
                    fields = (const char **)realloc(fields, sizeof(char *) * cap);
                    subs   = (PAT **)realloc(subs, sizeof(PAT *) * cap);
                }
                const char *fn = tk.str; lex();
                fields[n] = fn;
                if (accept(TK_EQ)) subs[n] = parse_pattern();
                else { PAT *vp = pp_alloc(P_VAR); vp->var_name = fn; subs[n] = vp; }
                n++;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RBRACE, "expected '}'");
        }
        // sort fields (insertion sort, expecting small N)
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 && strcmp(fields[j - 1], fields[j]) > 0; j--) {
                const char *tf = fields[j]; fields[j] = fields[j - 1]; fields[j - 1] = tf;
                PAT *tp = subs[j]; subs[j] = subs[j - 1]; subs[j - 1] = tp;
            }
        }
        PAT *p = pp_alloc(P_RECORD);
        p->rec.n = n; p->rec.fields = fields; p->rec.items = subs;
        return p;
    }
    parse_error("expected pattern");
    return NULL;
}

static PAT *
parse_pattern(void)
{
    if (tk.kind == TK_CTOR) {
        const char *name = tk.str; lex();
        bool can_arg =
            tk.kind == TK_ID || tk.kind == TK_CTOR ||
            tk.kind == TK_INT || tk.kind == TK_STR ||
            tk.kind == TK_LPAREN || tk.kind == TK_LBRACK ||
            tk.kind == TK_LBRACE ||
            tk.kind == TK_NIL || tk.kind == TK_UNIT_LIT ||
            tk.kind == TK_TRUE || tk.kind == TK_FALSE ||
            tk.kind == TK_UNDERSCORE || tk.kind == TK_TILDE;
        if (can_arg && ctor_arity(name) == 1) {
            PAT *arg = parse_pattern_atom();
            PAT *p = pp_alloc(P_CTOR1); p->ctor1.name = name; p->ctor1.arg = arg;
            if (accept(TK_DCOLON)) {
                PAT *t = parse_pattern();
                PAT *cp = pp_alloc(P_CONS); cp->cons.h = p; cp->cons.t = t;
                return cp;
            }
            return p;
        }
        PAT *p = pp_alloc(P_CTOR0); p->var_name = name;
        if (accept(TK_DCOLON)) {
            PAT *t = parse_pattern();
            PAT *cp = pp_alloc(P_CONS); cp->cons.h = p; cp->cons.t = t;
            return cp;
        }
        return p;
    }
    PAT *h = parse_pattern_atom();
    if (accept(TK_DCOLON)) {
        PAT *t = parse_pattern();
        PAT *cp = pp_alloc(P_CONS); cp->cons.h = h; cp->cons.t = t;
        return cp;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Pattern lowering: walk the pattern, given a NODE that yields the
// scrutinee, append AND-of-tests + binders.
// ---------------------------------------------------------------------------

struct pat_binder { const char *name; NODE *extractor; };

struct pat_compile {
    NODE              *test;
    struct pat_binder *binders;
    int                n_binders, cap;
};

static void
pc_add_binder(struct pat_compile *pc, const char *name, NODE *e)
{
    if (pc->n_binders == pc->cap) {
        int nc = pc->cap ? pc->cap * 2 : 4;
        pc->binders = (struct pat_binder *)realloc(pc->binders, nc * sizeof(struct pat_binder));
        pc->cap = nc;
    }
    pc->binders[pc->n_binders].name = name;
    pc->binders[pc->n_binders].extractor = e;
    pc->n_binders++;
}

static NODE *
pat_and(NODE *a, NODE *b) { if (!a) return b; if (!b) return a; return ALLOC_node_pat_and(a, b); }

static void
compile_pat(PAT *p, NODE *scrut, struct pat_compile *pc)
{
    switch (p->kind) {
      case P_WILDCARD: return;
      case P_VAR:      pc_add_binder(pc, p->var_name, scrut); return;
      case P_INT:      pc->test = pat_and(pc->test, ALLOC_node_pat_test_int(scrut, (uint64_t)p->ival)); return;
      case P_BOOL:     pc->test = pat_and(pc->test, ALLOC_node_pat_test_bool(scrut, p->bval ? 1 : 0)); return;
      case P_STR:      pc->test = pat_and(pc->test, ALLOC_node_pat_test_str(scrut, p->sval)); return;
      case P_UNIT:     pc->test = pat_and(pc->test, ALLOC_node_pat_test_unit(scrut)); return;
      case P_NIL:      pc->test = pat_and(pc->test, ALLOC_node_pat_test_nil(scrut)); return;
      case P_CONS: {
        pc->test = pat_and(pc->test, ALLOC_node_pat_test_cons(scrut));
        compile_pat(p->cons.h, ALLOC_node_proj_head(scrut), pc);
        compile_pat(p->cons.t, ALLOC_node_proj_tail(scrut), pc);
        return;
      }
      case P_TUPLE: {
        pc->test = pat_and(pc->test, ALLOC_node_pat_test_tuple(scrut, (uint32_t)p->tup.n));
        for (int i = 0; i < p->tup.n; i++)
            compile_pat(p->tup.items[i], ALLOC_node_proj_tuple(scrut, (uint32_t)i), pc);
        return;
      }
      case P_LIST: {
        NODE *cur = scrut;
        for (int i = 0; i < p->lst.n; i++) {
            pc->test = pat_and(pc->test, ALLOC_node_pat_test_cons(cur));
            compile_pat(p->lst.items[i], ALLOC_node_proj_head(cur), pc);
            cur = ALLOC_node_proj_tail(cur);
        }
        pc->test = pat_and(pc->test, ALLOC_node_pat_test_nil(cur));
        return;
      }
      case P_CTOR0: {
        if (ctor_arity(p->var_name) == 0)
            pc->test = pat_and(pc->test, ALLOC_node_pat_test_ctor(scrut, p->var_name));
        else if (ctor_arity(p->var_name) == 1)
            pc->test = pat_and(pc->test, ALLOC_node_pat_test_ctor(scrut, p->var_name));
        else
            pc_add_binder(pc, p->var_name, scrut);
        return;
      }
      case P_CTOR1: {
        pc->test = pat_and(pc->test, ALLOC_node_pat_test_ctor(scrut, p->ctor1.name));
        compile_pat(p->ctor1.arg, ALLOC_node_proj_ctor(scrut), pc);
        return;
      }
      case P_RECORD: {
        // HM が record の field set を保証しているので test は不要。
        // 各 field の sub-pattern を proj_record で再帰。
        for (int i = 0; i < p->rec.n; i++)
            compile_pat(p->rec.items[i],
                        ALLOC_node_proj_record(scrut, p->rec.fields[i]), pc);
        return;
      }
    }
}

// ---------------------------------------------------------------------------
// Expression IR — `struct expr` (EX) — parser output, then type-inferred,
// then lowered to NODE.  Decoupling parse from NODE construction lets us
// run inference on a clean tree and pick specialised dispatchers at lower
// time based on inferred types (drop dynamic IS_INT / IS_BOOL / IS_REF
// checks for code that the type-checker accepts).
// ---------------------------------------------------------------------------

typedef struct expr EX;

enum binop_kind {
    BO_ADD, BO_SUB, BO_MUL, BO_DIV, BO_MOD, BO_RDIV,
    BO_LT, BO_LE, BO_GT, BO_GE, BO_EQ, BO_NE,
    BO_CONCAT,
};

enum unop_kind {
    UO_NEG, UO_NOT, UO_DEREF,
};

enum ex_kind {
    EX_INT, EX_REAL, EX_STR, EX_BOOL, EX_UNIT, EX_NIL,
    EX_LREF, EX_GREF, EX_CTOR0, EX_CTOR1,
    EX_IF, EX_SEQ, EX_LET, EX_LETREC, EX_FN, EX_APP,
    EX_TUPLE, EX_CONS,
    EX_RECORD, EX_FIELD,
    EX_CASE, EX_HANDLE,
    EX_REF_NEW, EX_DEREF, EX_ASSIGN,
    EX_RAISE,
    EX_BINOP, EX_UNOP,
    EX_ANDALSO, EX_ORELSE,
    EX_TOPBIND, EX_TOPLET_FUNS, EX_NOOP,
};

struct case_arm_ir {
    PAT *pat;
    EX  *body;
};

struct expr {
    enum ex_kind kind;
    int line;
    TY *ty;
    union {
        int64_t  ival;
        double   rval;
        const char *sval;
        bool     bval;
        struct { uint32_t depth, idx; const char *name; } lref;
        struct { const char *name; } gref;
        struct { const char *name; } ctor0;
        struct { const char *name; EX *arg; } ctor1;
        struct { EX *cond, *thn, *els; } iff;
        struct { EX *first, *rest; } seq;
        struct { const char *name; EX *value, *body; } let;
        struct { int n; const char **names; EX **values; EX *body; } letrec;
        struct {
            int           nparams;
            const char  **param_names;       // for diagnostics; one per param
            PAT         **param_pats;        // raw patterns (for non-var)
            EX           *body;
            const char   *name;
        } fn;
        struct { EX *fn; EX *arg; } app;
        struct { int n; EX **items; } tuple;
        struct { int n; const char **fields; EX **items; } rec;     // sorted fields
        struct { EX *e; const char *field; } fld;
        struct { EX *head, *tail; } cons;
        struct { EX *scrut; int n_arms; struct case_arm_ir *arms; } cse;
        struct { EX *body; int n_arms; struct case_arm_ir *arms; } handle;
        struct { EX *e; } un;        // ref/deref/raise/not
        struct { int op; EX *l, *r; } bin;
        struct { int op; EX *e; } unop;
        struct { EX *l, *r; } andalso;
        struct { EX *l, *r; } orelse;
        struct { EX *l, *r; } assign;
        struct { const char *name; EX *value; } topbind;
        struct { int n; const char **names; EX **values; } toplet;
    };
};

static EX *
ex_alloc(enum ex_kind k)
{
    EX *e = (EX *)calloc(1, sizeof *e);
    e->kind = k;
    e->line = tk.line;
    return e;
}

static EX *ex_int(int64_t v)         { EX *e = ex_alloc(EX_INT); e->ival = v; return e; }
static EX *ex_real(double v)         { EX *e = ex_alloc(EX_REAL); e->rval = v; return e; }
static EX *ex_str(const char *s)     { EX *e = ex_alloc(EX_STR); e->sval = s; return e; }
static EX *ex_bool(bool b)           { EX *e = ex_alloc(EX_BOOL); e->bval = b; return e; }
static EX *ex_unit(void)             { return ex_alloc(EX_UNIT); }
static EX *ex_nil(void)              { return ex_alloc(EX_NIL); }
static EX *ex_lref(uint32_t d, uint32_t i, const char *n) { EX *e = ex_alloc(EX_LREF); e->lref.depth = d; e->lref.idx = i; e->lref.name = n; return e; }
static EX *ex_gref(const char *n)    { EX *e = ex_alloc(EX_GREF); e->gref.name = n; return e; }
static EX *ex_ctor0(const char *n)   { EX *e = ex_alloc(EX_CTOR0); e->ctor0.name = n; return e; }
static EX *ex_ctor1(const char *n, EX *a) { EX *e = ex_alloc(EX_CTOR1); e->ctor1.name = n; e->ctor1.arg = a; return e; }
static EX *ex_if(EX *c, EX *t, EX *l) { EX *e = ex_alloc(EX_IF); e->iff.cond = c; e->iff.thn = t; e->iff.els = l; return e; }
static EX *ex_seq(EX *a, EX *b)      { EX *e = ex_alloc(EX_SEQ); e->seq.first = a; e->seq.rest = b; return e; }
static EX *ex_let(const char *n, EX *v, EX *b) { EX *e = ex_alloc(EX_LET); e->let.name = n; e->let.value = v; e->let.body = b; return e; }
static EX *ex_app(EX *f, EX *a)      { EX *e = ex_alloc(EX_APP); e->app.fn = f; e->app.arg = a; return e; }
static EX *ex_cons(EX *h, EX *t)     { EX *e = ex_alloc(EX_CONS); e->cons.head = h; e->cons.tail = t; return e; }
static EX *ex_ref_new(EX *x)         { EX *e = ex_alloc(EX_REF_NEW); e->un.e = x; return e; }
static EX *ex_deref(EX *x)           { EX *e = ex_alloc(EX_DEREF); e->un.e = x; return e; }
static EX *ex_assign(EX *l, EX *r)   { EX *e = ex_alloc(EX_ASSIGN); e->assign.l = l; e->assign.r = r; return e; }
static EX *ex_raise(EX *x)           { EX *e = ex_alloc(EX_RAISE); e->un.e = x; return e; }
static EX *ex_binop(int op, EX *l, EX *r) { EX *e = ex_alloc(EX_BINOP); e->bin.op = op; e->bin.l = l; e->bin.r = r; return e; }
static EX *ex_unop(int op, EX *x)    { EX *e = ex_alloc(EX_UNOP); e->unop.op = op; e->unop.e = x; return e; }
static EX *ex_andalso(EX *l, EX *r)  { EX *e = ex_alloc(EX_ANDALSO); e->andalso.l = l; e->andalso.r = r; return e; }
static EX *ex_orelse(EX *l, EX *r)   { EX *e = ex_alloc(EX_ORELSE); e->orelse.l = l; e->orelse.r = r; return e; }
static EX *ex_topbind(const char *n, EX *v) { EX *e = ex_alloc(EX_TOPBIND); e->topbind.name = n; e->topbind.value = v; return e; }
static EX *ex_noop(void)             { return ex_alloc(EX_NOOP); }

// ---------------------------------------------------------------------------
// Forward decls.
// ---------------------------------------------------------------------------

static EX *parse_expr(void);
static EX *parse_atom(void);
static void process_datatype(void);
static EX *parse_app(void);
static EX *parse_let(void);
static EX *parse_case(void);
static EX *parse_fn(void);
static EX *parse_match_chain(int *out_n_arms, struct case_arm_ir **out_arms);

// ---------------------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------------------

static bool
can_start_atom(void)
{
    switch (tk.kind) {
      case TK_INT: case TK_REAL: case TK_STR: case TK_ID: case TK_CTOR:
      case TK_TRUE: case TK_FALSE: case TK_NIL: case TK_UNIT_LIT:
      case TK_LPAREN: case TK_LBRACK: case TK_LBRACE: case TK_HASH:
      case TK_LET: case TK_IF: case TK_FN: case TK_CASE:
      case TK_TILDE: case TK_NOT: case TK_RAISE: case TK_OP:
        return true;
      default: return false;
    }
}

static EX *
parse_atom(void)
{
    if (tk.kind == TK_INT)  { int64_t v = tk.ival; lex(); return ex_int(v); }
    if (tk.kind == TK_REAL) { double v = tk.rval;  lex(); return ex_real(v); }
    if (tk.kind == TK_STR)  { const char *s = tk.str; lex(); return ex_str(s); }
    if (accept(TK_TRUE))     return ex_bool(true);
    if (accept(TK_FALSE))    return ex_bool(false);
    if (accept(TK_UNIT_LIT)) return ex_unit();
    if (accept(TK_NIL))      return ex_nil();
    if (tk.kind == TK_ID) {
        const char *name = tk.str; lex();
        uint32_t depth, idx;
        if (scope_lookup(name, &depth, &idx)) return ex_lref(depth, idx, name);
        return ex_gref(name);
    }
    if (tk.kind == TK_CTOR) {
        const char *name = tk.str; lex();
        int ar = ctor_arity(name);
        if (ar == 0) return ex_ctor0(name);
        // 1-arg ctor used as a value: emit `fn $x => Ctor $x`.  This
        // is allocated lazily; well-typed code is unaffected.
        return ex_gref(name);   // the lowered NODE will look up a global prim.
    }
    if (accept(TK_LPAREN)) {
        if (accept(TK_RPAREN)) return ex_unit();
        EX *first = parse_expr();
        if (accept(TK_RPAREN)) return first;
        if (accept(TK_COMMA)) {
            EX **items = NULL; int n = 1, cap = 4;
            items = (EX **)malloc(sizeof(EX *) * cap);
            items[0] = first;
            items[n++] = parse_expr();
            while (accept(TK_COMMA)) {
                if (n == cap) { cap *= 2; items = (EX **)realloc(items, sizeof(EX *) * cap); }
                items[n++] = parse_expr();
            }
            expect(TK_RPAREN, "expected ')'");
            EX *e = ex_alloc(EX_TUPLE);
            e->tuple.n = n;
            e->tuple.items = items;
            return e;
        }
        if (accept(TK_SEMI)) {
            EX *seq = first;
            seq = ex_seq(seq, parse_expr());
            while (accept(TK_SEMI)) seq = ex_seq(seq, parse_expr());
            expect(TK_RPAREN, "expected ')'");
            return seq;
        }
        parse_error("expected ',' ';' or ')' in parenthesised expression");
    }
    if (accept(TK_LBRACK)) {
        if (accept(TK_RBRACK)) return ex_nil();
        EX *first = parse_expr();
        EX **items = (EX **)malloc(sizeof(EX *) * 8);
        int n = 0, cap = 8;
        items[n++] = first;
        while (accept(TK_COMMA)) {
            if (n == cap) { cap *= 2; items = (EX **)realloc(items, sizeof(EX *) * cap); }
            items[n++] = parse_expr();
        }
        expect(TK_RBRACK, "expected ']'");
        EX *result = ex_nil();
        for (int i = n - 1; i >= 0; i--) result = ex_cons(items[i], result);
        free(items);
        return result;
    }
    if (accept(TK_LBRACE)) {
        // record literal: `{f1 = e1, f2 = e2, ...}`.  fields ソート済で保持。
        const char **fields = (const char **)malloc(sizeof(char *) * 8);
        EX **items = (EX **)malloc(sizeof(EX *) * 8);
        int n = 0, cap = 8;
        if (!accept(TK_RBRACE)) {
            for (;;) {
                if (tk.kind != TK_ID) parse_error("expected field name in record literal");
                if (n == cap) { cap *= 2;
                    fields = (const char **)realloc(fields, sizeof(char *) * cap);
                    items  = (EX **)realloc(items, sizeof(EX *) * cap);
                }
                fields[n] = tk.str; lex();
                expect(TK_EQ, "expected '=' after field name");
                items[n] = parse_expr();
                n++;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RBRACE, "expected '}'");
        }
        // sort by field name
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 && strcmp(fields[j - 1], fields[j]) > 0; j--) {
                const char *tf = fields[j]; fields[j] = fields[j - 1]; fields[j - 1] = tf;
                EX *te = items[j]; items[j] = items[j - 1]; items[j - 1] = te;
            }
        }
        EX *e = ex_alloc(EX_RECORD);
        e->rec.n = n; e->rec.fields = fields; e->rec.items = items;
        return e;
    }
    if (accept(TK_HASH)) {
        // `#field` — field selector. SML 流: `#f r` でフィールドアクセス。
        // 我々は単独の `#f` も受け付けるが、その時 `fn $r => #f $r` 相当の
        // closure を emit (型推論で record 型が確定すれば lower で
        // node_field を選ぶ)。簡単のため、ここでは必ず引数を取る形を要求:
        // `#f e` のみ。`#f` 単独で値として渡す場合は `(fn r => #f r)` を
        // 書いてもらう (文書化)。
        if (tk.kind != TK_ID) parse_error("expected field name after '#'");
        const char *fname = tk.str; lex();
        if (!can_start_atom()) {
            // closure を emit: `fn $r => #fname $r`
            scope_push();
            scope_add(intern("$r"), BK_VAR);
            EX *body = ex_alloc(EX_FIELD);
            body->fld.e = ex_lref(0, 0, intern("$r"));
            body->fld.field = fname;
            scope_pop();
            EX *fn = ex_alloc(EX_FN);
            fn->fn.nparams = 1;
            fn->fn.param_names = (const char **)malloc(sizeof(char *));
            fn->fn.param_names[0] = intern("$r");
            fn->fn.param_pats = (PAT **)malloc(sizeof(PAT *));
            PAT *vp = pp_alloc(P_VAR); vp->var_name = intern("$r");
            fn->fn.param_pats[0] = vp;
            fn->fn.body = body;
            fn->fn.name = intern("<#field>");
            return fn;
        }
        EX *r = parse_atom();
        EX *e = ex_alloc(EX_FIELD);
        e->fld.e = r;
        e->fld.field = fname;
        return e;
    }
    if (tk.kind == TK_LET) return parse_let();
    if (accept(TK_IF)) {
        EX *c = parse_expr(); expect(TK_THEN, "expected 'then'");
        EX *t = parse_expr(); expect(TK_ELSE, "expected 'else'");
        EX *l = parse_expr();
        return ex_if(c, t, l);
    }
    if (tk.kind == TK_FN)   return parse_fn();
    if (tk.kind == TK_CASE) return parse_case();
    if (accept(TK_RAISE))   { EX *e = parse_expr(); return ex_raise(e); }
    if (accept(TK_TILDE))   { EX *a = parse_atom(); return ex_unop(UO_NEG, a); }
    if (accept(TK_NOT))     { EX *a = parse_atom(); return ex_unop(UO_NOT, a); }
    if (accept(TK_OP)) {
        const char *opname = NULL;
        switch (tk.kind) {
          case TK_PLUS:  opname = "+";  break;
          case TK_MINUS: opname = "-";  break;
          case TK_STAR:  opname = "*";  break;
          case TK_LT:    opname = "<";  break;
          case TK_LE:    opname = "<="; break;
          case TK_GT:    opname = ">";  break;
          case TK_GE:    opname = ">="; break;
          case TK_EQ:    opname = "=";  break;
          case TK_NEQ:   opname = "<>"; break;
          case TK_CARET: opname = "^";  break;
          case TK_AT:    opname = "@";  break;
          case TK_ID:    opname = tk.str; break;
          default: parse_error("expected operator after 'op'");
        }
        lex();
        return ex_gref(intern(opname));
    }
    parse_error("expected expression");
    return NULL;
}

static EX *
parse_app(void)
{
    if (tk.kind == TK_CTOR && ctor_arity(tk.str) == 1) {
        const char *name = tk.str; lex();
        if (can_start_atom()) {
            EX *a = parse_atom();
            return ex_ctor1(name, a);
        }
        return ex_gref(name);
    }
    EX *f = parse_atom();
    while (can_start_atom()) {
        EX *a = parse_atom();
        f = ex_app(f, a);
    }
    return f;
}

static EX *
parse_mul(void)
{
    EX *left = parse_app();
    for (;;) {
        if      (accept(TK_STAR))  { EX *r = parse_app(); left = ex_binop(BO_MUL,  left, r); }
        else if (accept(TK_SLASH)) { EX *r = parse_app(); left = ex_binop(BO_RDIV, left, r); }
        else if (accept(TK_DIV))   { EX *r = parse_app(); left = ex_binop(BO_DIV,  left, r); }
        else if (accept(TK_MOD))   { EX *r = parse_app(); left = ex_binop(BO_MOD,  left, r); }
        else break;
    }
    return left;
}

static EX *
parse_add(void)
{
    EX *left = parse_mul();
    for (;;) {
        if      (accept(TK_PLUS))  { EX *r = parse_mul(); left = ex_binop(BO_ADD, left, r); }
        else if (accept(TK_MINUS)) { EX *r = parse_mul(); left = ex_binop(BO_SUB, left, r); }
        else break;
    }
    return left;
}

static EX *
parse_concat(void)
{
    EX *left = parse_add();
    if (accept(TK_CARET)) {
        EX *r = parse_concat();
        return ex_binop(BO_CONCAT, left, r);
    }
    return left;
}

static EX *
parse_cons(void)
{
    EX *left = parse_concat();
    if (accept(TK_DCOLON)) {
        EX *r = parse_cons();
        return ex_cons(left, r);
    }
    return left;
}

static EX *
parse_listcat(void)
{
    EX *left = parse_cons();
    if (accept(TK_AT)) {
        EX *r = parse_listcat();
        // emit as `(@ left) r`
        return ex_app(ex_app(ex_gref(intern("@")), left), r);
    }
    return left;
}

static EX *
parse_cmp(void)
{
    EX *left = parse_listcat();
    for (;;) {
        if      (accept(TK_LT))  { EX *r = parse_listcat(); left = ex_binop(BO_LT, left, r); }
        else if (accept(TK_LE))  { EX *r = parse_listcat(); left = ex_binop(BO_LE, left, r); }
        else if (accept(TK_GT))  { EX *r = parse_listcat(); left = ex_binop(BO_GT, left, r); }
        else if (accept(TK_GE))  { EX *r = parse_listcat(); left = ex_binop(BO_GE, left, r); }
        else if (accept(TK_EQ))  { EX *r = parse_listcat(); left = ex_binop(BO_EQ, left, r); }
        else if (accept(TK_NEQ)) { EX *r = parse_listcat(); left = ex_binop(BO_NE, left, r); }
        else break;
    }
    return left;
}

static EX *
parse_andalso_op(void)
{
    EX *left = parse_cmp();
    while (accept(TK_ANDALSO)) {
        EX *r = parse_cmp();
        left = ex_andalso(left, r);
    }
    return left;
}

static EX *
parse_orelse_op(void)
{
    EX *left = parse_andalso_op();
    while (accept(TK_ORELSE)) {
        EX *r = parse_andalso_op();
        left = ex_orelse(left, r);
    }
    return left;
}

static EX *
parse_assign_op(void)
{
    EX *left = parse_orelse_op();
    if (accept(TK_ASSIGN)) {
        EX *r = parse_assign_op();
        return ex_assign(left, r);
    }
    return left;
}

static EX *
parse_expr(void)
{
    EX *e = parse_assign_op();
    if (accept(TK_HANDLE)) {
        // Handler arms see the raised value bound at slot 0 of a fresh frame.
        scope_push();
        scope_add(intern("$exn"), BK_VAR);
        int n_arms; struct case_arm_ir *arms;
        parse_match_chain(&n_arms, &arms);
        scope_pop();
        EX *h = ex_alloc(EX_HANDLE);
        h->handle.body = e;
        h->handle.n_arms = n_arms;
        h->handle.arms = arms;
        return h;
    }
    return e;
}

// Parse `pat => body | pat => body | ...` chain into an array of arms.
static EX *
parse_match_chain(int *out_n_arms, struct case_arm_ir **out_arms)
{
    struct case_arm_ir *arms = NULL;
    int n = 0, cap = 4;
    arms = (struct case_arm_ir *)malloc(sizeof(struct case_arm_ir) * cap);
    for (;;) {
        if (n == cap) { cap *= 2; arms = (struct case_arm_ir *)realloc(arms, sizeof(struct case_arm_ir) * cap); }
        PAT *pat = parse_pattern();
        expect(TK_DARROW, "expected '=>'");
        // Push a fresh parser scope for the arm's binders (see compile_pat
        // at lower-time for which names get bound at runtime).  The arm
        // pushes a runtime frame iff it has at least one binder; we mirror
        // that by only pushing the parser scope when there are binders.
        // First we count binders by walking the pattern (without emitting
        // NODEs).
        int n_binders = 0;
        // Quick walk: same logic as compile_pat but counting bindings only.
        // Use a closure-free recursive helper.
        struct count { int (*f)(struct count *, PAT *); };
        // Inline the walk:
        {
            // Recursive lambda alternative — explicit stack walk.
            // To avoid rewriting, use a static helper: pat_count_binders.
        }
        extern int pat_count_binders(PAT *p);
        n_binders = pat_count_binders(pat);
        bool pushed = n_binders > 0;
        if (pushed) {
            scope_push();
            // We don't have the binder names here without re-walking the
            // pattern; reuse compile_pat-style traversal to register names
            // in scope in the same order it'll allocate slots.
            extern void pat_register_binders(PAT *p);
            pat_register_binders(pat);
        }
        EX *body = parse_expr();
        if (pushed) scope_pop();
        arms[n].pat = pat;
        arms[n].body = body;
        n++;
        if (!accept(TK_BAR)) break;
    }
    *out_n_arms = n;
    *out_arms = arms;
    return NULL;
}

// Pattern walk helpers used by parse_match_chain (binder count + register).
// They mirror compile_pat's binder ordering (left-to-right depth-first).
int
pat_count_binders(PAT *p)
{
    switch (p->kind) {
      case P_WILDCARD: case P_INT: case P_BOOL: case P_STR:
      case P_UNIT: case P_NIL:
        return 0;
      case P_VAR: return 1;
      case P_CONS: return pat_count_binders(p->cons.h) + pat_count_binders(p->cons.t);
      case P_TUPLE: {
        int s = 0; for (int i = 0; i < p->tup.n; i++) s += pat_count_binders(p->tup.items[i]); return s;
      }
      case P_LIST: {
        int s = 0; for (int i = 0; i < p->lst.n; i++) s += pat_count_binders(p->lst.items[i]); return s;
      }
      case P_CTOR0: {
        // unknown name acts as a variable; known ctor binds nothing.
        if (ctor_arity(p->var_name) >= 0) return 0;
        return 1;
      }
      case P_CTOR1: return pat_count_binders(p->ctor1.arg);
      case P_RECORD: {
        int s = 0;
        for (int i = 0; i < p->rec.n; i++) s += pat_count_binders(p->rec.items[i]);
        return s;
      }
    }
    return 0;
}

void
pat_register_binders(PAT *p)
{
    switch (p->kind) {
      case P_WILDCARD: case P_INT: case P_BOOL: case P_STR:
      case P_UNIT: case P_NIL: return;
      case P_VAR: scope_add(p->var_name, BK_VAR); return;
      case P_CONS: pat_register_binders(p->cons.h); pat_register_binders(p->cons.t); return;
      case P_TUPLE: for (int i = 0; i < p->tup.n; i++) pat_register_binders(p->tup.items[i]); return;
      case P_LIST:  for (int i = 0; i < p->lst.n; i++) pat_register_binders(p->lst.items[i]); return;
      case P_CTOR0:
        if (ctor_arity(p->var_name) < 0) scope_add(p->var_name, BK_VAR);
        return;
      case P_CTOR1: pat_register_binders(p->ctor1.arg); return;
      case P_RECORD:
        for (int i = 0; i < p->rec.n; i++) pat_register_binders(p->rec.items[i]);
        return;
    }
}

static EX *
parse_case(void)
{
    expect(TK_CASE, "expected 'case'");
    EX *value = parse_expr();
    expect(TK_OF, "expected 'of'");
    // Wrap in a let-bind so the scrutinee is at lref(0,0) for arm extractors.
    scope_push();
    scope_add(intern("$scrut"), BK_VAR);
    int n_arms; struct case_arm_ir *arms;
    parse_match_chain(&n_arms, &arms);
    scope_pop();
    EX *cse = ex_alloc(EX_CASE);
    cse->cse.scrut = value;
    cse->cse.n_arms = n_arms;
    cse->cse.arms = arms;
    return cse;
}

static EX *
parse_fn(void)
{
    expect(TK_FN, "expected 'fn'");
    PAT *pat = parse_pattern();
    expect(TK_DARROW, "expected '=>'");
    // Closure frame holds 1 slot.
    scope_push();
    const char *param_name;
    if (pat->kind == P_VAR)               param_name = pat->var_name;
    else if (pat->kind == P_WILDCARD)     param_name = intern("_");
    else                                  param_name = intern("$arg");
    scope_add(param_name, BK_VAR);
    bool extra_pushed = false;
    int n_binders = pat_count_binders(pat);
    if (pat->kind != P_VAR && pat->kind != P_WILDCARD && n_binders > 0) {
        scope_push();
        pat_register_binders(pat);
        extra_pushed = true;
    }
    EX *body = parse_expr();
    if (extra_pushed) scope_pop();
    scope_pop();
    EX *fn = ex_alloc(EX_FN);
    fn->fn.nparams = 1;
    fn->fn.param_names = (const char **)malloc(sizeof(char *));
    fn->fn.param_names[0] = param_name;
    fn->fn.param_pats  = (PAT **)malloc(sizeof(PAT *));
    fn->fn.param_pats[0]  = pat;
    fn->fn.body = body;
    fn->fn.name = intern("<fn>");
    return fn;
}

// ---------------------------------------------------------------------------
// Function-decl parameters.
// ---------------------------------------------------------------------------

static PAT **
parse_fun_params(int *out_n)
{
    PAT **params = NULL;
    int n = 0, cap = 4;
    params = (PAT **)malloc(sizeof(PAT *) * cap);
    while (tk.kind == TK_ID || tk.kind == TK_LPAREN || tk.kind == TK_UNDERSCORE
           || tk.kind == TK_UNIT_LIT || tk.kind == TK_LBRACE) {
        if (n == cap) { cap *= 2; params = (PAT **)realloc(params, sizeof(PAT *) * cap); }
        params[n++] = parse_pattern_atom();
    }
    *out_n = n;
    return params;
}

// Build closure body for `fun f p1 p2 ... pN = body`.  Each non-var param
// pushes its own arm scope (matching what the runtime match_arm does).
// Returns (body_ex, param_names, param_pats); caller wraps in EX_FN.
static EX *
build_fun_body(PAT **params, int np, const char ***out_param_names)
{
    scope_push();   // closure frame
    const char **pnames = (const char **)malloc(sizeof(char *) * (np ? np : 1));
    int n_extra_scopes = 0;
    for (int i = 0; i < np; i++) {
        if (params[i]->kind == P_VAR) {
            pnames[i] = params[i]->var_name;
            scope_add(pnames[i], BK_VAR);
        } else if (params[i]->kind == P_WILDCARD || params[i]->kind == P_UNIT) {
            char tmp[24]; snprintf(tmp, sizeof tmp, "_w%d", i);
            pnames[i] = intern(tmp);
            scope_add(pnames[i], BK_VAR);
        } else {
            char tmp[24]; snprintf(tmp, sizeof tmp, "$arg%d", i);
            pnames[i] = intern(tmp);
            scope_add(pnames[i], BK_VAR);
        }
    }
    for (int i = 0; i < np; i++) {
        if (params[i]->kind == P_VAR || params[i]->kind == P_WILDCARD ||
            params[i]->kind == P_UNIT) continue;
        if (pat_count_binders(params[i]) == 0) continue;
        scope_push();
        pat_register_binders(params[i]);
        n_extra_scopes++;
    }
    EX *body = parse_expr();
    for (int i = 0; i < n_extra_scopes; i++) scope_pop();
    scope_pop();
    *out_param_names = pnames;
    return body;
}

// ---------------------------------------------------------------------------
// `let ... in ... end`.
// ---------------------------------------------------------------------------

enum decl_kind { DK_VAL, DK_FUNGROUP };
struct decl_item {
    enum decl_kind kind;
    // DK_VAL
    const char *name;
    EX         *value;
    // DK_FUNGROUP
    int          n;
    const char **names;
    EX         **values;
};

static struct decl_item *
parse_one_decl(int *pushed_scopes_out)
{
    *pushed_scopes_out = 0;
    if (accept(TK_VAL)) {
        accept(TK_REC);
        PAT *pat = parse_pattern();
        expect(TK_EQ, "expected '='");
        EX *value = parse_expr();
        if (pat->kind != P_VAR) {
            fprintf(stderr, "asml: only simple var patterns supported in val-bindings (in let)\n");
            exit(1);
        }
        struct decl_item *d = (struct decl_item *)calloc(1, sizeof *d);
        d->kind = DK_VAL;
        d->name = pat->var_name;
        d->value = value;
        scope_push();
        scope_add(d->name, BK_VAR);
        *pushed_scopes_out = 1;
        return d;
    }
    if (accept(TK_FUN)) {
        // Two-pass for mutual recursion.
        size_t pass_pos = src_pos; int pass_line = src_line;
        struct token saved_tk = tk;
        struct fun_hdr {
            const char *name; int np; PAT **pats;
        } *hdrs = NULL;
        int n = 0, cap = 4;
        hdrs = (struct fun_hdr *)malloc(sizeof *hdrs * cap);
        for (;;) {
            if (tk.kind != TK_ID) parse_error("expected function name");
            const char *fname = tk.str; lex();
            int np;
            PAT **pats = parse_fun_params(&np);
            expect(TK_EQ, "expected '='");
            int depth = 0;
            while (tk.kind != TK_EOF) {
                if (depth == 0 && (tk.kind == TK_AND || tk.kind == TK_SEMI ||
                                   tk.kind == TK_IN || tk.kind == TK_END ||
                                   tk.kind == TK_VAL || tk.kind == TK_FUN ||
                                   tk.kind == TK_DATATYPE)) break;
                if (tk.kind == TK_LPAREN || tk.kind == TK_LBRACK ||
                    tk.kind == TK_LBRACE || tk.kind == TK_LET) depth++;
                if (tk.kind == TK_RPAREN || tk.kind == TK_RBRACK ||
                    tk.kind == TK_RBRACE || tk.kind == TK_END) depth--;
                lex();
            }
            if (n == cap) { cap *= 2; hdrs = (struct fun_hdr *)realloc(hdrs, sizeof *hdrs * cap); }
            hdrs[n].name = fname; hdrs[n].np = np; hdrs[n].pats = pats;
            n++;
            if (!accept(TK_AND)) break;
        }
        src_pos = pass_pos; src_line = pass_line; tk = saved_tk;
        scope_push();
        for (int i = 0; i < n; i++) scope_add(hdrs[i].name, BK_VAR);
        EX **bodies = (EX **)malloc(sizeof(EX *) * n);
        const char **names = (const char **)malloc(sizeof(char *) * n);
        for (int i = 0; i < n; i++) {
            if (tk.kind != TK_ID) parse_error("internal: lost track of fun header");
            lex();
            int np;
            PAT **dummy = parse_fun_params(&np);
            (void)dummy; (void)np;
            expect(TK_EQ, "expected '='");
            const char **pnames;
            EX *body = build_fun_body(hdrs[i].pats, hdrs[i].np, &pnames);
            EX *fn = ex_alloc(EX_FN);
            fn->fn.nparams = hdrs[i].np;
            fn->fn.param_names = pnames;
            fn->fn.param_pats = hdrs[i].pats;
            fn->fn.body = body;
            fn->fn.name = hdrs[i].name;
            bodies[i] = fn;
            names[i] = hdrs[i].name;
            if (i + 1 < n) accept(TK_AND);
        }
        struct decl_item *d = (struct decl_item *)calloc(1, sizeof *d);
        d->kind = DK_FUNGROUP;
        d->n = n;
        d->names = names;
        d->values = bodies;
        free(hdrs);
        *pushed_scopes_out = 1;     // the recursive scope stays pushed
        return d;
    }
    if (accept(TK_DATATYPE)) {
        process_datatype();
        return NULL;        // datatype is purely type-level; no runtime form
    }
    parse_error("expected decl");
    return NULL;
}

static EX *
parse_let(void)
{
    expect(TK_LET, "expected 'let'");
    struct decl_item **decls = NULL;
    int n_decls = 0, cap = 8;
    decls = (struct decl_item **)malloc(sizeof(struct decl_item *) * cap);
    int n_pushed = 0;
    while (tk.kind != TK_IN && tk.kind != TK_EOF) {
        if (accept(TK_SEMI)) continue;
        int pushed = 0;
        struct decl_item *d = parse_one_decl(&pushed);
        n_pushed += pushed;
        if (!d) continue;
        if (n_decls == cap) { cap *= 2; decls = (struct decl_item **)realloc(decls, sizeof(struct decl_item *) * cap); }
        decls[n_decls++] = d;
    }
    expect(TK_IN, "expected 'in'");
    EX *body = parse_expr();
    while (accept(TK_SEMI)) {
        if (tk.kind == TK_END) break;
        body = ex_seq(body, parse_expr());
    }
    expect(TK_END, "expected 'end'");
    for (int i = 0; i < n_pushed; i++) scope_pop();
    // Wrap inside-out.
    for (int i = n_decls - 1; i >= 0; i--) {
        struct decl_item *d = decls[i];
        if (d->kind == DK_VAL) {
            body = ex_let(d->name, d->value, body);
        } else {
            EX *e = ex_alloc(EX_LETREC);
            e->letrec.n = d->n;
            e->letrec.names = d->names;
            e->letrec.values = d->values;
            e->letrec.body = body;
            body = e;
        }
    }
    free(decls);
    return body;
}

// ---------------------------------------------------------------------------
// Type parser (for datatype `of <type>` clauses).  Subset:
//   tyatom ::= 'a | int | bool | string | unit | real | exn
//            | tyatom list | tyatom ref
//            | tycon                  (nullary)
//            | tyatom tycon           (1-arg parametric)
//            | (type, type, ...) tycon (multi-arg parametric)
//            | (type)
//   typrod ::= tyatom (* tyatom)*    (tuple)
//   type   ::= typrod                (no arrow support — uncommon in datatype RHS)
//
// `tyvars` is a name → TY map: when we encounter `'a` and it's in this
// map, we use the mapped TY (so all uses of `'a` in the same datatype
// share the same var).
// ---------------------------------------------------------------------------

struct tyvar_binding { const char *name; TY *ty; };

struct tyvar_env {
    int n, cap;
    struct tyvar_binding *binds;
};

static TY *
tyvar_lookup_or_add(struct tyvar_env *tv, const char *name)
{
    for (int i = 0; i < tv->n; i++)
        if (strcmp(tv->binds[i].name, name) == 0) return tv->binds[i].ty;
    if (tv->n == tv->cap) {
        int nc = tv->cap ? tv->cap * 2 : 4;
        tv->binds = (struct tyvar_binding *)realloc(tv->binds, nc * sizeof(*tv->binds));
        tv->cap = nc;
    }
    TY *t = ty_var(0);              // level 0 — will be quantified later
    tv->binds[tv->n].name = name;
    tv->binds[tv->n].ty = t;
    tv->n++;
    return t;
}

static TY *parse_type_full(struct tyvar_env *tv);

static TY *
parse_type_atom(struct tyvar_env *tv)
{
    if (tk.kind == TK_ID && tk.str[0] == '\'') {
        const char *name = tk.str; lex();
        TY *t = tyvar_lookup_or_add(tv, name);
        // A `'a tycon` postfix may follow.
        while (tk.kind == TK_ID || tk.kind == TK_CTOR) {
            const char *cname = tk.str;
            if (strcmp(cname, "list") == 0)      { lex(); t = ty_list(t); }
            else if (strcmp(cname, "ref") == 0)  { lex(); t = ty_ref(t); }
            else if (tycon_arity(cname) == 1)    { lex(); t = ty_con(intern(cname), 1, &t); }
            else break;
        }
        return t;
    }
    if (tk.kind == TK_LPAREN) {
        lex();
        TY *first = parse_type_full(tv);
        if (accept(TK_RPAREN)) {
            // (T) — possibly followed by a tycon: (T) list
            while (tk.kind == TK_ID || tk.kind == TK_CTOR) {
                const char *cname = tk.str;
                if (strcmp(cname, "list") == 0)      { lex(); first = ty_list(first); }
                else if (strcmp(cname, "ref") == 0)  { lex(); first = ty_ref(first); }
                else if (tycon_arity(cname) == 1)    { lex(); first = ty_con(intern(cname), 1, &first); }
                else break;
            }
            return first;
        }
        // (T1, T2, ...) tycon
        TY **args = (TY **)malloc(sizeof(TY *) * 8);
        int n = 1, cap = 8; args[0] = first;
        while (accept(TK_COMMA)) {
            if (n == cap) { cap *= 2; args = (TY **)realloc(args, sizeof(TY *) * cap); }
            args[n++] = parse_type_full(tv);
        }
        expect(TK_RPAREN, "expected ')'");
        if (tk.kind != TK_ID && tk.kind != TK_CTOR) parse_error("expected tycon");
        const char *cname = tk.str; lex();
        TY *r = ty_con(intern(cname), n, args);
        free(args);
        return r;
    }
    if (tk.kind == TK_ID || tk.kind == TK_CTOR) {
        const char *name = tk.str;
        if      (strcmp(name, "int") == 0)    { lex(); return ty_int(); }
        else if (strcmp(name, "real") == 0)   { lex(); return ty_real(); }
        else if (strcmp(name, "string") == 0) { lex(); return ty_string(); }
        else if (strcmp(name, "bool") == 0)   { lex(); return ty_bool(); }
        else if (strcmp(name, "unit") == 0)   { lex(); return ty_unit(); }
        else if (strcmp(name, "exn") == 0)    { lex(); return ty_exn(); }
        else {
            lex();
            int ar = tycon_arity(name);
            if (ar < 0) ar = 0;             // unknown — assume nullary
            return ty_con(intern(name), 0, NULL);
        }
    }
    if (accept(TK_LBRACE)) {
        // record 型: `{f1 : T1, f2 : T2, ...}`
        const char **fields = (const char **)malloc(sizeof(char *) * 8);
        TY **types = (TY **)malloc(sizeof(TY *) * 8);
        int n = 0, cap = 8;
        if (!accept(TK_RBRACE)) {
            for (;;) {
                if (tk.kind != TK_ID) parse_error("expected field name in record type");
                if (n == cap) { cap *= 2;
                    fields = (const char **)realloc(fields, sizeof(char *) * cap);
                    types  = (TY **)realloc(types, sizeof(TY *) * cap);
                }
                fields[n] = tk.str; lex();
                expect(TK_COLON, "expected ':' after field name");
                types[n] = parse_type_full(tv);
                n++;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RBRACE, "expected '}'");
        }
        // sort by name
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 && strcmp(fields[j - 1], fields[j]) > 0; j--) {
                const char *tf = fields[j]; fields[j] = fields[j - 1]; fields[j - 1] = tf;
                TY *tt = types[j]; types[j] = types[j - 1]; types[j - 1] = tt;
            }
        }
        return ty_record(n, fields, types);
    }
    parse_error("expected type atom");
    return NULL;
}

static TY *
parse_type_full(struct tyvar_env *tv)
{
    TY *t = parse_type_atom(tv);
    if (tk.kind == TK_STAR) {
        TY **its = (TY **)malloc(sizeof(TY *) * 8);
        int n = 1, cap = 8; its[0] = t;
        while (accept(TK_STAR)) {
            if (n == cap) { cap *= 2; its = (TY **)realloc(its, sizeof(TY *) * cap); }
            its[n++] = parse_type_atom(tv);
        }
        TY *r = ty_tup(n, its);
        free(its);
        return r;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Datatype declaration handler.  Parses type params, name, ctors with
// proper type annotations.  Registers tycon arity, ctor arity, and ctor
// type schemes.
// ---------------------------------------------------------------------------

static void
process_datatype(void)
{
    // type params: 'a, or ('a, 'b, ...), or none
    int n_params = 0;
    const char *param_names[16];
    if (tk.kind == TK_ID && tk.str[0] == '\'') {
        param_names[n_params++] = tk.str; lex();
    } else if (accept(TK_LPAREN)) {
        for (;;) {
            if (tk.kind != TK_ID || tk.str[0] != '\'') parse_error("expected 'a in datatype params");
            if (n_params == 16) parse_error("too many type params");
            param_names[n_params++] = tk.str; lex();
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_RPAREN, "expected ')'");
    }
    if (tk.kind != TK_ID && tk.kind != TK_CTOR) parse_error("expected datatype name");
    const char *tyname = intern(tk.str); lex();
    expect(TK_EQ, "expected '='");
    tycon_register(tyname, n_params);
    for (;;) {
        if (tk.kind != TK_CTOR && tk.kind != TK_ID) parse_error("expected constructor");
        const char *cname = intern(tk.str); lex();
        // Parse this ctor with its own tyvar env, pre-populated with the
        // datatype params so they share TYs across arg/result types.
        struct tyvar_env tv = {0};
        TY **param_tys = NULL;
        if (n_params > 0) {
            param_tys = (TY **)alloca(sizeof(TY *) * n_params);
            for (int i = 0; i < n_params; i++)
                param_tys[i] = tyvar_lookup_or_add(&tv, param_names[i]);
        }
        TY *result_ty = ty_con(tyname, n_params, param_tys);
        TY *arg_ty = NULL;
        int arity = 0;
        if (accept(TK_OF)) {
            arg_ty = parse_type_full(&tv);
            arity = 1;
        }
        register_ctor(cname, arity);
        TY *body = arg_ty ? ty_arr(arg_ty, result_ty) : result_ty;
        struct ty_scheme *s = ty_generalize(-1, body);
        ctor_ty_register(cname, arity, false, s);
        free(tv.binds);
        if (!accept(TK_BAR)) break;
    }
}

// ---------------------------------------------------------------------------
// Top-level forms.
// ---------------------------------------------------------------------------

static EX *
parse_top_form(bool *had_form)
{
    if (tk.kind == TK_EOF) { *had_form = false; return NULL; }
    *had_form = true;

    if (accept(TK_DATATYPE)) {
        process_datatype();
        accept(TK_SEMI);
        return ex_noop();
    }

    if (accept(TK_VAL)) {
        accept(TK_REC);
        PAT *pat = parse_pattern();
        expect(TK_EQ, "expected '='");
        EX *value = parse_expr();
        accept(TK_SEMI);
        if (pat->kind == P_VAR) return ex_topbind(pat->var_name, value);
        // Wildcard / unit / literal: discard.  We still bind to a
        // throwaway name so the value's side-effects fire and the
        // expression goes through the pipeline.
        if (pat->kind == P_WILDCARD || pat->kind == P_UNIT) {
            return ex_topbind(intern("_"), value);
        }
        // For tuple / cons / ctor patterns: bind the value to a hidden
        // global, then emit one topbind per extracted name.  We need to
        // construct extractors *as expressions* (not NODEs yet) since the
        // value is now an EX.  Easiest: stash value at `$top`, then for
        // each binder emit `topbind(name, extract($top))`.
        EX *seq = ex_topbind(intern("$top"), value);
        // Walk the pattern and emit topbinds for each variable, building
        // an extractor expression as we go.
        // This is duplicated from compile_pat but operates on EX.
        struct pat_to_ex_ctx {
            void (*walk)(struct pat_to_ex_ctx *, PAT *, EX *);
            EX **out_seq;
        };
        // Use a recursive helper with state via closure (workaround C):
        // We'll define the recursion inline.
        // Implementation: a static function with state via globals isn't
        // re-entrant.  Use a manual stack-based traversal.
        // For brevity, only support tuple destructuring at top-level: that
        // covers `val (a, b) = ...`.  Other patterns at top-level are
        // unusual.
        if (pat->kind == P_TUPLE) {
            for (int i = 0; i < pat->tup.n; i++) {
                if (pat->tup.items[i]->kind != P_VAR) {
                    fprintf(stderr,
                        "asml: nested patterns in top-level val not supported (use let or case)\n");
                    exit(1);
                }
                EX *getter = ex_alloc(EX_BINOP);    // placeholder
                (void)getter;
                // Build: $top.tup_i  via case dispatch.  Simpler: emit a
                // case expression that destructures.
            }
            // Build case ($top) of (a, b, ...) => () that side-effects
            // global definitions.  Simplest: rely on lower-time to handle
            // by emitting a `case`-style sequence.  Easier still: just
            // emit a sequence of topbinds where each value is `case $top
            // of (..., x_i, ...) => x_i` (one arm).
            for (int i = 0; i < pat->tup.n; i++) {
                const char *vname = pat->tup.items[i]->var_name;
                // Build `case $top of (... x_i ...) => x_i`
                PAT *cse_pat = pp_alloc(P_TUPLE);
                cse_pat->tup.n = pat->tup.n;
                cse_pat->tup.items = (PAT **)malloc(sizeof(PAT *) * pat->tup.n);
                for (int j = 0; j < pat->tup.n; j++) {
                    if (j == i) {
                        PAT *vp = pp_alloc(P_VAR); vp->var_name = vname;
                        cse_pat->tup.items[j] = vp;
                    } else cse_pat->tup.items[j] = pp_alloc(P_WILDCARD);
                }
                EX *body_ex = ex_lref(0, 0, vname);     // refers to bound x_i; arm pushes new frame
                struct case_arm_ir *arms = (struct case_arm_ir *)malloc(sizeof *arms);
                arms[0].pat = cse_pat;
                arms[0].body = body_ex;
                EX *cse = ex_alloc(EX_CASE);
                cse->cse.scrut = ex_gref(intern("$top"));
                cse->cse.n_arms = 1;
                cse->cse.arms = arms;
                seq = ex_seq(seq, ex_topbind(vname, cse));
            }
            return seq;
        }
        fprintf(stderr, "asml: complex patterns in top-level val not supported\n");
        exit(1);
    }

    if (accept(TK_FUN)) {
        size_t pass_pos = src_pos; int pass_line = src_line;
        struct token saved_tk = tk;
        struct fun_hdr {
            const char *name; int np; PAT **pats;
        } *hdrs = NULL;
        int n = 0, cap = 4;
        hdrs = (struct fun_hdr *)malloc(sizeof *hdrs * cap);
        for (;;) {
            if (tk.kind != TK_ID) parse_error("expected function name");
            const char *fname = tk.str; lex();
            int np;
            PAT **pats = parse_fun_params(&np);
            expect(TK_EQ, "expected '='");
            int depth = 0;
            while (tk.kind != TK_EOF) {
                if (depth == 0 && (tk.kind == TK_AND || tk.kind == TK_SEMI ||
                                   tk.kind == TK_VAL || tk.kind == TK_FUN ||
                                   tk.kind == TK_DATATYPE)) break;
                if (tk.kind == TK_LPAREN || tk.kind == TK_LBRACK ||
                    tk.kind == TK_LBRACE || tk.kind == TK_LET) depth++;
                if (tk.kind == TK_RPAREN || tk.kind == TK_RBRACK ||
                    tk.kind == TK_RBRACE || tk.kind == TK_END) depth--;
                lex();
            }
            if (n == cap) { cap *= 2; hdrs = (struct fun_hdr *)realloc(hdrs, sizeof *hdrs * cap); }
            hdrs[n].name = fname; hdrs[n].np = np; hdrs[n].pats = pats;
            n++;
            if (!accept(TK_AND)) break;
        }
        src_pos = pass_pos; src_line = pass_line; tk = saved_tk;
        scope_push();
        for (int i = 0; i < n; i++) scope_add(hdrs[i].name, BK_VAR);
        EX **bodies = (EX **)malloc(sizeof(EX *) * n);
        const char **names = (const char **)malloc(sizeof(char *) * n);
        for (int i = 0; i < n; i++) {
            if (tk.kind != TK_ID) parse_error("internal: lost track of fun header");
            lex();
            int np;
            PAT **dummy = parse_fun_params(&np);
            (void)dummy; (void)np;
            expect(TK_EQ, "expected '='");
            const char **pnames;
            EX *body = build_fun_body(hdrs[i].pats, hdrs[i].np, &pnames);
            EX *fn = ex_alloc(EX_FN);
            fn->fn.nparams = hdrs[i].np;
            fn->fn.param_names = pnames;
            fn->fn.param_pats = hdrs[i].pats;
            fn->fn.body = body;
            fn->fn.name = hdrs[i].name;
            bodies[i] = fn;
            names[i] = hdrs[i].name;
            if (i + 1 < n) accept(TK_AND);
        }
        accept(TK_SEMI);
        scope_pop();
        EX *e = ex_alloc(EX_TOPLET_FUNS);
        e->toplet.n = n;
        e->toplet.names = names;
        e->toplet.values = bodies;
        free(hdrs);
        return e;
    }

    EX *e = parse_expr();
    accept(TK_SEMI);
    return ex_topbind(intern("it"), e);
}

// ---------------------------------------------------------------------------
// Lowering: EX → NODE.  Mostly mechanical.  Specialisation choices for
// typed code happen in lower_binop / lower_unop based on inferred types.
// ---------------------------------------------------------------------------

static NODE *lower_expr(EX *e);

// Build NODE tree for a `case`'s arm chain.  Mirrors the old
// parse_match_chain but starts from EX arms.
static NODE *
lower_arm_chain(struct case_arm_ir *arms, int n, NODE *scrut, NODE *failure_default)
{
    NODE *next = failure_default;
    // Build innermost-first (last arm wraps in failure_default; earlier
    // arms wrap that as their failure).
    for (int i = n - 1; i >= 0; i--) {
        struct case_arm_ir *a = &arms[i];
        struct pat_compile pc = {0};
        compile_pat(a->pat, scrut, &pc);
        NODE **extracts = NULL;
        if (pc.n_binders > 0) {
            if (pc.n_binders <= 0) abort();
            extracts = (NODE **)malloc(sizeof(NODE *) * (size_t)pc.n_binders);
            for (int j = 0; j < pc.n_binders; j++) extracts[j] = pc.binders[j].extractor;
        }
        uint32_t eidx = pc.n_binders > 0
            ? push_nodes(&ML_EXTRACT_NODES, &ML_EXTRACT_NODES_LEN, &ML_EXTRACT_NODES_CAP,
                         extracts, pc.n_binders)
            : 0;
        free(extracts);
        NODE *test = pc.test ? pc.test : ALLOC_node_const_bool(1);
        free(pc.binders);
        NODE *body = lower_expr(a->body);
        next = ALLOC_node_match_arm(test, (uint32_t)pc.n_binders, eidx,
                                    body, next, /*is_leaf=*/1);
    }
    return next;
}

static NODE *
lower_case(EX *e)
{
    NODE *value = lower_expr(e->cse.scrut);
    NODE *scrut_ref = ALLOC_node_lref(0, 0);
    NODE *failure = ALLOC_node_match_fail();
    NODE *body = lower_arm_chain(e->cse.arms, e->cse.n_arms, scrut_ref, failure);
    return ALLOC_node_let(value, body);
}

static NODE *
lower_handle(EX *e)
{
    NODE *body = lower_expr(e->handle.body);
    NODE *scrut_ref = ALLOC_node_lref(0, 0);
    NODE *re_raise = ALLOC_node_raise(ALLOC_node_lref(0, 0));
    NODE *handler = lower_arm_chain(e->handle.arms, e->handle.n_arms, scrut_ref, re_raise);
    return ALLOC_node_handle(body, handler);
}

// Build closure body, wrapping with match_arm chain for each non-var fn param.
static NODE *
lower_fn_body(EX *fnex)
{
    int np = fnex->fn.nparams;
    PAT **params = fnex->fn.param_pats;
    NODE *body = lower_expr(fnex->fn.body);

    // Build wraps right-to-left so leftmost arm becomes outermost.
    int n_extra = 0;       // tracks how many wraps already pushed (for depth)
    struct {
        NODE *test;
        int   arity;
        uint32_t eidx;
    } *wraps = NULL;
    int n_wraps = 0;
    if (np > 0) wraps = (typeof(wraps))calloc(np, sizeof(*wraps));
    for (int i = 0; i < np; i++) {
        if (params[i]->kind == P_VAR ||
            params[i]->kind == P_WILDCARD ||
            params[i]->kind == P_UNIT) continue;
        NODE *scrut = ALLOC_node_lref((uint32_t)n_extra, (uint32_t)i);
        struct pat_compile pc = {0};
        compile_pat(params[i], scrut, &pc);
        if (pc.n_binders == 0) {
            wraps[n_wraps].test = pc.test ? pc.test : ALLOC_node_const_bool(1);
            wraps[n_wraps].arity = 0;
            wraps[n_wraps].eidx = 0;
            n_wraps++;
            free(pc.binders);
            continue;
        }
        if (pc.n_binders <= 0) abort();
        NODE **extracts = (NODE **)malloc(sizeof(NODE *) * (size_t)pc.n_binders);
        for (int j = 0; j < pc.n_binders; j++) extracts[j] = pc.binders[j].extractor;
        uint32_t eidx = push_nodes(&ML_EXTRACT_NODES, &ML_EXTRACT_NODES_LEN, &ML_EXTRACT_NODES_CAP,
                                   extracts, pc.n_binders);
        free(extracts);
        wraps[n_wraps].test = pc.test ? pc.test : ALLOC_node_const_bool(1);
        wraps[n_wraps].arity = pc.n_binders;
        wraps[n_wraps].eidx = eidx;
        n_wraps++;
        n_extra++;
        free(pc.binders);
    }
    for (int i = n_wraps - 1; i >= 0; i--) {
        NODE *failure = ALLOC_node_match_fail();
        body = ALLOC_node_match_arm(wraps[i].test, (uint32_t)wraps[i].arity,
                                    wraps[i].eidx, body, failure, /*is_leaf=*/0);
    }
    free(wraps);
    return body;
}

// 推論済み型 (deref-aware) の判別用ヘルパ。
static enum ty_kind
ex_ty_kind(EX *e)
{
    if (!e->ty) return TYK_VAR;     // 推論未走 (起こらないはず)
    return ty_deref(e->ty)->kind;
}

// 比較 op の operand 型 (両辺は HM で同一に unify 済みなので l 側を見る)
// に応じて、最適な NODE を選ぶ。HM が op の operand を多相のまま残した
// 場合 (e.g. `'a list` の比較) は `_poly` で structural compare に丸投げ。
static NODE *
lower_cmp(int op, NODE *l, NODE *r, EX *operand)
{
    enum ty_kind k = ex_ty_kind(operand);
    switch (op) {
      case BO_LT:
        if (k == TYK_INT)    return ALLOC_node_lt_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_lt_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_lt_string(l, r);
        return ALLOC_node_lt_poly(l, r);
      case BO_LE:
        if (k == TYK_INT)    return ALLOC_node_le_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_le_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_le_string(l, r);
        return ALLOC_node_le_poly(l, r);
      case BO_GT:
        if (k == TYK_INT)    return ALLOC_node_gt_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_gt_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_gt_string(l, r);
        return ALLOC_node_gt_poly(l, r);
      case BO_GE:
        if (k == TYK_INT)    return ALLOC_node_ge_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_ge_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_ge_string(l, r);
        return ALLOC_node_ge_poly(l, r);
      case BO_EQ:
        if (k == TYK_INT)    return ALLOC_node_eq_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_eq_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_eq_string(l, r);
        return ALLOC_node_eq_poly(l, r);
      case BO_NE:
        if (k == TYK_INT)    return ALLOC_node_ne_int(l, r);
        if (k == TYK_REAL)   return ALLOC_node_ne_real(l, r);
        if (k == TYK_STRING) return ALLOC_node_ne_string(l, r);
        return ALLOC_node_ne_poly(l, r);
    }
    fprintf(stderr, "asml: lower_cmp: unknown op %d\n", op);
    exit(1);
}

static NODE *
lower_binop(EX *e)
{
    NODE *l = lower_expr(e->bin.l);
    NODE *r = lower_expr(e->bin.r);
    switch (e->bin.op) {
      // 算術 — HM で int / real が確定済み。
      case BO_ADD:  return ALLOC_node_add_int(l, r);
      case BO_SUB:  return ALLOC_node_sub_int(l, r);
      case BO_MUL:  return ALLOC_node_mul_int(l, r);
      case BO_DIV:  return ALLOC_node_div_int(l, r);
      case BO_MOD:  return ALLOC_node_mod_int(l, r);
      case BO_RDIV: return ALLOC_node_rdiv(l, r);
      // 比較 — 型ごとに dispatch。
      case BO_LT: case BO_LE: case BO_GT:
      case BO_GE: case BO_EQ: case BO_NE:
        return lower_cmp(e->bin.op, l, r, e->bin.l);
      // 文字列連結 — string が確定。
      case BO_CONCAT: return ALLOC_node_concat_str(l, r);
    }
    fprintf(stderr, "asml: lower_binop: unknown op %d\n", e->bin.op);
    exit(1);
}

static NODE *
lower_unop(EX *e)
{
    NODE *x = lower_expr(e->unop.e);
    switch (e->unop.op) {
      case UO_NEG: {
        enum ty_kind k = ex_ty_kind(e->unop.e);
        if (k == TYK_INT)  return ALLOC_node_neg_int(x);
        if (k == TYK_REAL) return ALLOC_node_neg_real(x);
        // HM は `~` を int|real に絞る — ここに来たら型エラー
        // (本来 infer 段階で停止しているはず)。
        fprintf(stderr, "asml: internal: ~ on non-numeric type\n");
        exit(1);
      }
      case UO_NOT:   return ALLOC_node_not_bool(x);
      case UO_DEREF: return ALLOC_node_deref_unchecked(x);
    }
    fprintf(stderr, "asml: lower_unop: unknown op %d\n", e->unop.op);
    exit(1);
}

static NODE *
lower_top_funs(EX *e)
{
    int n = e->toplet.n;
    NODE **vals = (NODE **)malloc(sizeof(NODE *) * n);
    for (int i = 0; i < n; i++) {
        vals[i] = lower_expr(e->toplet.values[i]);
        // Each value is a closure NODE; aot register its body.
        if (e->toplet.values[i]->kind == EX_FN) {
            // The actual fn body is buried inside the ALLOC_node_fn we just
            // created; AOT registry happens during EX_FN lowering (below).
        }
    }
    uint32_t vidx = push_nodes(&ML_LETREC_VALUES, &ML_LETREC_VALUES_LEN, &ML_LETREC_VALUES_CAP, vals, n);
    free(vals);
    NODE *body = NULL;
    for (int i = 0; i < n; i++) {
        NODE *r = ALLOC_node_lref(0, (uint32_t)i);
        NODE *b = ALLOC_node_topbind(e->toplet.names[i], r);
        body = body ? ALLOC_node_seq(body, b) : b;
    }
    return ALLOC_node_letrec_n((uint32_t)n, vidx, body);
}

static NODE *
lower_expr(EX *e)
{
    switch (e->kind) {
      case EX_INT:  return ALLOC_node_const_int((uint64_t)e->ival);
      case EX_REAL: return ALLOC_node_const_real(e->rval);
      case EX_STR:  return ALLOC_node_const_str(e->sval);
      case EX_BOOL: return ALLOC_node_const_bool(e->bval ? 1 : 0);
      case EX_UNIT: return ALLOC_node_const_unit();
      case EX_NIL:  return ALLOC_node_const_nil();
      case EX_LREF: return ALLOC_node_lref(e->lref.depth, e->lref.idx);
      case EX_GREF: return ALLOC_node_gref(e->gref.name);
      case EX_CTOR0: return ALLOC_node_ctor0(e->ctor0.name);
      case EX_CTOR1: return ALLOC_node_ctor1(e->ctor1.name, lower_expr(e->ctor1.arg));
      case EX_IF:   return ALLOC_node_if_bool(lower_expr(e->iff.cond),
                                               lower_expr(e->iff.thn),
                                               lower_expr(e->iff.els));
      case EX_SEQ:  return ALLOC_node_seq(lower_expr(e->seq.first), lower_expr(e->seq.rest));
      case EX_LET:  return ALLOC_node_let(lower_expr(e->let.value), lower_expr(e->let.body));
      case EX_LETREC: {
        int n = e->letrec.n;
        NODE **vs = (NODE **)malloc(sizeof(NODE *) * n);
        for (int i = 0; i < n; i++) vs[i] = lower_expr(e->letrec.values[i]);
        uint32_t idx = push_nodes(&ML_LETREC_VALUES, &ML_LETREC_VALUES_LEN, &ML_LETREC_VALUES_CAP, vs, n);
        free(vs);
        return ALLOC_node_letrec_n((uint32_t)n, idx, lower_expr(e->letrec.body));
      }
      case EX_FN: {
        NODE *body = lower_fn_body(e);
        NODE *fn = ALLOC_node_fn((uint32_t)e->fn.nparams, body, /*leaf=*/0,
                                 e->fn.name ? e->fn.name : intern("<fn>"));
        aot_add_entry(body);
        return fn;
      }
      case EX_APP: return ALLOC_node_app1(lower_expr(e->app.fn), lower_expr(e->app.arg));
      case EX_TUPLE: {
        int n = e->tuple.n;
        NODE **its = (NODE **)malloc(sizeof(NODE *) * n);
        for (int i = 0; i < n; i++) its[i] = lower_expr(e->tuple.items[i]);
        uint32_t idx = push_nodes(&ML_TUPLE_ITEMS, &ML_TUPLE_ITEMS_LEN, &ML_TUPLE_ITEMS_CAP, its, n);
        free(its);
        return ALLOC_node_tuple((uint32_t)n, idx);
      }
      case EX_RECORD: {
        int n = e->rec.n;
        NODE **its = (NODE **)malloc(sizeof(NODE *) * (n ? n : 1));
        for (int i = 0; i < n; i++) its[i] = lower_expr(e->rec.items[i]);
        uint32_t items_idx  = push_nodes(&ML_TUPLE_ITEMS, &ML_TUPLE_ITEMS_LEN, &ML_TUPLE_ITEMS_CAP, its, n);
        uint32_t fields_idx = push_strings(&ML_RECORD_FIELDS, &ML_RECORD_FIELDS_LEN, &ML_RECORD_FIELDS_CAP,
                                           e->rec.fields, n);
        free(its);
        return ALLOC_node_record((uint32_t)n, fields_idx, items_idx);
      }
      case EX_FIELD: return ALLOC_node_field(lower_expr(e->fld.e), e->fld.field);
      case EX_CONS: return ALLOC_node_cons(lower_expr(e->cons.head), lower_expr(e->cons.tail));
      case EX_CASE: return lower_case(e);
      case EX_HANDLE: return lower_handle(e);
      case EX_REF_NEW: return ALLOC_node_ref(lower_expr(e->un.e));
      case EX_DEREF:   return ALLOC_node_deref_unchecked(lower_expr(e->un.e));
      case EX_ASSIGN:  return ALLOC_node_assign_unchecked(lower_expr(e->assign.l), lower_expr(e->assign.r));
      case EX_RAISE:   return ALLOC_node_raise(lower_expr(e->un.e));
      case EX_BINOP:   return lower_binop(e);
      case EX_UNOP:    return lower_unop(e);
      case EX_ANDALSO: return ALLOC_node_andalso_bool(lower_expr(e->andalso.l), lower_expr(e->andalso.r));
      case EX_ORELSE:  return ALLOC_node_orelse_bool(lower_expr(e->orelse.l), lower_expr(e->orelse.r));
      case EX_TOPBIND: return ALLOC_node_topbind(e->topbind.name, lower_expr(e->topbind.value));
      case EX_TOPLET_FUNS: return lower_top_funs(e);
      case EX_NOOP:    return ALLOC_node_const_unit();
    }
    fprintf(stderr, "asml: lower_expr: unknown EX kind %d\n", e->kind);
    exit(1);
}


// ---------------------------------------------------------------------------
// Built-in primitives.
// ---------------------------------------------------------------------------

static VALUE
prim_print(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    if (ML_IS_STRING(v)) {
        struct mlobj *o = ML_PTR(v);
        fwrite(o->str.chars, 1, o->str.len, stdout);
    } else ml_display(stdout, v);
    fflush(stdout);
    return ML_UNIT;
}

static VALUE
prim_println(CTX *c, int argc, VALUE *argv)
{ prim_print(c, argc, argv); fputc('\n', stdout); return ML_UNIT; }

static VALUE
prim_int_to_string(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (!ML_IS_INT(argv[0])) ml_type_error(c, "Int.toString", "int");
    char buf[32];
    int64_t i = ML_INT_VAL(argv[0]);
    if (i < 0) snprintf(buf, sizeof buf, "~%lld", (long long)(-i));
    else       snprintf(buf, sizeof buf, "%lld", (long long)i);
    return ml_make_string(buf, strlen(buf));
}

static VALUE
prim_real_to_string(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    double d = ml_get_real(argv[0]);
    char buf[64];
    if (d < 0) { buf[0] = '~'; snprintf(buf + 1, sizeof(buf) - 1, "%.12g", -d); }
    else       { snprintf(buf, sizeof buf, "%.12g", d); }
    return ml_make_string(buf, strlen(buf));
}

static VALUE
prim_string_size(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (!ML_IS_STRING(argv[0])) ml_type_error(c, "String.size", "string");
    return ML_INT((int64_t)ML_PTR(argv[0])->str.len);
}

static VALUE
prim_string_concat(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (!ML_IS_STRING(argv[0]) || !ML_IS_STRING(argv[1]))
        ml_type_error(c, "^", "string");
    return ml_string_concat(argv[0], argv[1]);
}

static VALUE
prim_list_append(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    if (a == ML_NIL) return b;
    if (!ML_IS_CONS(a)) ml_type_error(c, "@", "list");
    return ml_cons(ML_PTR(a)->cons.head,
                   prim_list_append(c, 2, (VALUE[]){ ML_PTR(a)->cons.tail, b }));
}

static VALUE
prim_list_length(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    int64_t n = 0; VALUE v = argv[0];
    while (ML_IS_CONS(v)) { n++; v = ML_PTR(v)->cons.tail; }
    if (v != ML_NIL) ml_type_error(c, "List.length", "list");
    return ML_INT(n);
}

static VALUE
prim_list_hd(CTX *c, int argc, VALUE *argv)
{ (void)argc; if (!ML_IS_CONS(argv[0])) ml_raise(c, ml_make_variant("Empty", 0, NULL)); return ML_PTR(argv[0])->cons.head; }

static VALUE
prim_list_tl(CTX *c, int argc, VALUE *argv)
{ (void)argc; if (!ML_IS_CONS(argv[0])) ml_raise(c, ml_make_variant("Empty", 0, NULL)); return ML_PTR(argv[0])->cons.tail; }

static VALUE
prim_list_null(CTX *c, int argc, VALUE *argv)
{ (void)c; (void)argc; return argv[0] == ML_NIL ? ML_TRUE : ML_FALSE; }

static VALUE
prim_list_rev(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE r = ML_NIL, v = argv[0];
    while (ML_IS_CONS(v)) { r = ml_cons(ML_PTR(v)->cons.head, r); v = ML_PTR(v)->cons.tail; }
    return r;
}

static VALUE
prim_arith(CTX *c, int argc, VALUE *argv, char op)
{
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    if (ML_IS_INT(a) && ML_IS_INT(b)) {
        int64_t ai = ML_INT_VAL(a), bi = ML_INT_VAL(b);
        if (op == '+') return ML_INT(ai + bi);
        if (op == '-') return ML_INT(ai - bi);
        return ML_INT(ai * bi);
    }
    if (ML_IS_REAL(a) || ML_IS_REAL(b)) {
        double ad = ml_get_real(a), bd = ml_get_real(b);
        if (op == '+') return ml_make_real(ad + bd);
        if (op == '-') return ml_make_real(ad - bd);
        return ml_make_real(ad * bd);
    }
    ml_type_error(c, "arith", "int or real");
}

static VALUE prim_add(CTX *c, int argc, VALUE *argv) { return prim_arith(c, argc, argv, '+'); }
static VALUE prim_sub(CTX *c, int argc, VALUE *argv) { return prim_arith(c, argc, argv, '-'); }
static VALUE prim_mul(CTX *c, int argc, VALUE *argv) { return prim_arith(c, argc, argv, '*'); }

static VALUE prim_lt(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_compare(argv[0], argv[1]) <  0 ? ML_TRUE : ML_FALSE; }
static VALUE prim_le(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_compare(argv[0], argv[1]) <= 0 ? ML_TRUE : ML_FALSE; }
static VALUE prim_gt(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_compare(argv[0], argv[1]) >  0 ? ML_TRUE : ML_FALSE; }
static VALUE prim_ge(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_compare(argv[0], argv[1]) >= 0 ? ML_TRUE : ML_FALSE; }
static VALUE prim_eq(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_structural_eq(argv[0], argv[1]) ? ML_TRUE : ML_FALSE; }
static VALUE prim_ne(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_structural_eq(argv[0], argv[1]) ? ML_FALSE : ML_TRUE; }

static VALUE prim_ref(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return ml_make_ref(argv[0]); }
static VALUE prim_deref(CTX *c, int argc, VALUE *argv) { (void)argc; if (!ML_IS_REF(argv[0])) ml_type_error(c, "!", "ref"); return ML_PTR(argv[0])->refval; }
static VALUE prim_real_of_int(CTX *c, int argc, VALUE *argv) { (void)argc; if (!ML_IS_INT(argv[0])) ml_type_error(c, "real", "int"); return ml_make_real((double)ML_INT_VAL(argv[0])); }
static VALUE prim_floor(CTX *c, int argc, VALUE *argv) { (void)argc; if (!ML_IS_REAL(argv[0])) ml_type_error(c, "floor", "real"); return ML_INT((int64_t)floor(ML_PTR(argv[0])->dbl)); }

static VALUE prim_some(CTX *c, int argc, VALUE *argv)
{ (void)c; (void)argc; return ml_make_variant(intern("SOME"), 1, argv); }

// ---------------------------------------------------------------------------
// INIT.
// ---------------------------------------------------------------------------

static void
register_prim(CTX *c, const char *name, ml_prim_fn fn, int argc)
{
    ml_global_define(c, name, ml_make_prim(name, fn, argc, argc));
}

static void
install_prelude(CTX *c)
{
    register_prim(c, "print",         prim_print,           1);
    register_prim(c, "println",       prim_println,         1);
    register_prim(c, "Int.toString",  prim_int_to_string,   1);
    register_prim(c, "Real.toString", prim_real_to_string,  1);
    register_prim(c, "String.size",   prim_string_size,     1);
    register_prim(c, "size",          prim_string_size,     1);
    register_prim(c, "List.length",   prim_list_length,     1);
    register_prim(c, "List.null",     prim_list_null,       1);
    register_prim(c, "List.hd",       prim_list_hd,         1);
    register_prim(c, "List.tl",       prim_list_tl,         1);
    register_prim(c, "List.rev",      prim_list_rev,        1);
    register_prim(c, "hd",            prim_list_hd,         1);
    register_prim(c, "tl",            prim_list_tl,         1);
    register_prim(c, "null",          prim_list_null,       1);
    register_prim(c, "rev",           prim_list_rev,        1);
    register_prim(c, "real",          prim_real_of_int,     1);
    register_prim(c, "floor",         prim_floor,           1);
    register_prim(c, "ref",           prim_ref,             1);
    register_prim(c, "!",             prim_deref,           1);
    register_prim(c, "+",             prim_add,             2);
    register_prim(c, "-",             prim_sub,             2);
    register_prim(c, "*",             prim_mul,             2);
    register_prim(c, "<",             prim_lt,              2);
    register_prim(c, "<=",            prim_le,              2);
    register_prim(c, ">",             prim_gt,              2);
    register_prim(c, ">=",            prim_ge,              2);
    register_prim(c, "=",             prim_eq,              2);
    register_prim(c, "<>",            prim_ne,              2);
    register_prim(c, "^",             prim_string_concat,   2);
    register_prim(c, "@",             prim_list_append,     2);

    register_ctor(intern("NONE"),  0);
    register_ctor(intern("SOME"),  1);
    register_ctor(intern("Match"), 0);
    register_ctor(intern("Div"),   0);
    register_ctor(intern("Empty"), 0);
    register_ctor(intern("Fail"),  1);

    // SOME exposed as a 1-arg primitive so it can be used first-class.
    register_prim(c, "SOME", prim_some, 1);

    // ---- Type schemes for prelude prims and built-in ctors ----

    tycon_register(intern("option"), 1);

    // SOME : forall 'a. 'a -> 'a option
    {
        TY *a = ty_var(0);
        TY *body = ty_arr(a, ty_con(intern("option"), 1, (TY *[]){a}));
        ctor_ty_register(intern("SOME"), 1, false, ty_generalize(-1, body));
        // also as a global value (since we exposed prim_some)
        ty_global_define(intern("SOME"), ty_generalize(-1, ty_arr(a, ty_con(intern("option"), 1, (TY *[]){a}))));
    }
    // NONE : forall 'a. 'a option
    {
        TY *a = ty_var(0);
        TY *body = ty_con(intern("option"), 1, (TY *[]){a});
        ctor_ty_register(intern("NONE"), 0, false, ty_generalize(-1, body));
    }
    // Exception ctors
    ctor_ty_register(intern("Match"), 0, true, ty_mono_scheme(ty_exn()));
    ctor_ty_register(intern("Div"),   0, true, ty_mono_scheme(ty_exn()));
    ctor_ty_register(intern("Empty"), 0, true, ty_mono_scheme(ty_exn()));
    ctor_ty_register(intern("Fail"),  1, true, ty_mono_scheme(ty_arr(ty_string(), ty_exn())));

    // Prelude global type schemes.
    ty_global_define(intern("print"),         ty_generalize(-1, ty_arr(ty_var(0), ty_unit())));
    ty_global_define(intern("println"),       ty_generalize(-1, ty_arr(ty_var(0), ty_unit())));
    ty_global_define(intern("Int.toString"),  ty_mono_scheme(ty_arr(ty_int(), ty_string())));
    ty_global_define(intern("Real.toString"), ty_mono_scheme(ty_arr(ty_real(), ty_string())));
    ty_global_define(intern("String.size"),   ty_mono_scheme(ty_arr(ty_string(), ty_int())));
    ty_global_define(intern("size"),          ty_mono_scheme(ty_arr(ty_string(), ty_int())));
    {
        TY *a = ty_var(0);
        ty_global_define(intern("List.length"), ty_generalize(-1, ty_arr(ty_list(a), ty_int())));
    }
    { TY *a = ty_var(0); ty_global_define(intern("List.null"), ty_generalize(-1, ty_arr(ty_list(a), ty_bool()))); }
    { TY *a = ty_var(0); ty_global_define(intern("null"),      ty_generalize(-1, ty_arr(ty_list(a), ty_bool()))); }
    { TY *a = ty_var(0); ty_global_define(intern("List.hd"),   ty_generalize(-1, ty_arr(ty_list(a), a))); }
    { TY *a = ty_var(0); ty_global_define(intern("hd"),        ty_generalize(-1, ty_arr(ty_list(a), a))); }
    { TY *a = ty_var(0); ty_global_define(intern("List.tl"),   ty_generalize(-1, ty_arr(ty_list(a), ty_list(a)))); }
    { TY *a = ty_var(0); ty_global_define(intern("tl"),        ty_generalize(-1, ty_arr(ty_list(a), ty_list(a)))); }
    { TY *a = ty_var(0); ty_global_define(intern("List.rev"),  ty_generalize(-1, ty_arr(ty_list(a), ty_list(a)))); }
    { TY *a = ty_var(0); ty_global_define(intern("rev"),       ty_generalize(-1, ty_arr(ty_list(a), ty_list(a)))); }
    ty_global_define(intern("real"),  ty_mono_scheme(ty_arr(ty_int(),  ty_real())));
    ty_global_define(intern("floor"), ty_mono_scheme(ty_arr(ty_real(), ty_int())));
    { TY *a = ty_var(0); ty_global_define(intern("ref"),  ty_generalize(-1, ty_arr(a, ty_ref(a)))); }
    { TY *a = ty_var(0); ty_global_define(intern("!"),    ty_generalize(-1, ty_arr(ty_ref(a), a))); }
    // Operator prims (overloads handled via specific binop nodes — but
    // also exposed as values for `op +` etc.)
    ty_global_define(intern("+"),  ty_mono_scheme(ty_arr(ty_int(), ty_arr(ty_int(), ty_int()))));
    ty_global_define(intern("-"),  ty_mono_scheme(ty_arr(ty_int(), ty_arr(ty_int(), ty_int()))));
    ty_global_define(intern("*"),  ty_mono_scheme(ty_arr(ty_int(), ty_arr(ty_int(), ty_int()))));
    { TY *a = ty_var(0); ty_global_define(intern("<"),  ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    { TY *a = ty_var(0); ty_global_define(intern("<="), ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    { TY *a = ty_var(0); ty_global_define(intern(">"),  ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    { TY *a = ty_var(0); ty_global_define(intern(">="), ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    { TY *a = ty_var(0); ty_global_define(intern("="),  ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    { TY *a = ty_var(0); ty_global_define(intern("<>"), ty_generalize(-1, ty_arr(a, ty_arr(a, ty_bool())))); }
    ty_global_define(intern("^"),  ty_mono_scheme(ty_arr(ty_string(), ty_arr(ty_string(), ty_string()))));
    { TY *a = ty_var(0); ty_global_define(intern("@"),  ty_generalize(-1, ty_arr(ty_list(a), ty_arr(ty_list(a), ty_list(a))))); }
}

// ---------------------------------------------------------------------------
// Type inference (Algorithm W).  Walks the expr tree, fills `ex->ty` on
// each node, and on ill-typed input emits a diagnostic and exits with
// status 2.  Uses level-tracking for let-polymorphism and the standard
// value restriction (only generalise vals whose RHS is a syntactic value).
// ---------------------------------------------------------------------------

static bool
ex_is_value(EX *e)
{
    switch (e->kind) {
      case EX_INT: case EX_REAL: case EX_STR: case EX_BOOL:
      case EX_UNIT: case EX_NIL:
      case EX_FN:
      case EX_LREF: case EX_GREF:
      case EX_CTOR0:
        return true;
      case EX_CTOR1: return ex_is_value(e->ctor1.arg);
      case EX_TUPLE: {
        for (int i = 0; i < e->tuple.n; i++) if (!ex_is_value(e->tuple.items[i])) return false;
        return true;
      }
      case EX_RECORD: {
        for (int i = 0; i < e->rec.n; i++) if (!ex_is_value(e->rec.items[i])) return false;
        return true;
      }
      case EX_CONS: return ex_is_value(e->cons.head) && ex_is_value(e->cons.tail);
      default: return false;
    }
}

struct binders_acc {
    struct ty_env_slot *bs;
    int n, cap;
};

static void
binders_add(struct binders_acc *b, const char *name, struct ty_scheme *s)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->bs = (struct ty_env_slot *)realloc(b->bs, sizeof(*b->bs) * b->cap);
    }
    b->bs[b->n].name = name;
    b->bs[b->n].scheme = s;
    b->n++;
}

static void
infer_pat_walk(int level, PAT *p, TY *expected, struct binders_acc *acc, int line)
{
    switch (p->kind) {
      case P_WILDCARD: return;
      case P_VAR:
        binders_add(acc, p->var_name, ty_mono_scheme(expected));
        return;
      case P_INT:  ty_unify_at(line, expected, ty_int()); return;
      case P_BOOL: ty_unify_at(line, expected, ty_bool()); return;
      case P_STR:  ty_unify_at(line, expected, ty_string()); return;
      case P_UNIT: ty_unify_at(line, expected, ty_unit()); return;
      case P_NIL: {
        TY *a = ty_var(level);
        ty_unify_at(line, expected, ty_list(a));
        return;
      }
      case P_CONS: {
        TY *a = ty_var(level);
        ty_unify_at(line, expected, ty_list(a));
        infer_pat_walk(level, p->cons.h, a, acc, line);
        infer_pat_walk(level, p->cons.t, ty_list(a), acc, line);
        return;
      }
      case P_TUPLE: {
        TY **its = (TY **)alloca(sizeof(TY *) * p->tup.n);
        for (int i = 0; i < p->tup.n; i++) its[i] = ty_var(level);
        ty_unify_at(line, expected, ty_tup(p->tup.n, its));
        for (int i = 0; i < p->tup.n; i++) infer_pat_walk(level, p->tup.items[i], its[i], acc, line);
        return;
      }
      case P_LIST: {
        TY *a = ty_var(level);
        ty_unify_at(line, expected, ty_list(a));
        for (int i = 0; i < p->lst.n; i++) infer_pat_walk(level, p->lst.items[i], a, acc, line);
        return;
      }
      case P_CTOR0: {
        struct ctor_ty_entry *ce = ctor_ty_find(p->var_name);
        if (!ce) {
            // Unknown name acts as a variable binding (per parser convention).
            binders_add(acc, p->var_name, ty_mono_scheme(expected));
            return;
        }
        if (ce->arity != 0)
            type_error(line, "constructor %s used without argument in pattern", p->var_name);
        TY *t = ty_instantiate(ce->scheme, level);
        ty_unify_at(line, expected, t);
        return;
      }
      case P_CTOR1: {
        struct ctor_ty_entry *ce = ctor_ty_find(p->ctor1.name);
        if (!ce) type_error(line, "unknown constructor %s in pattern", p->ctor1.name);
        if (ce->arity != 1) type_error(line, "constructor %s takes no argument", p->ctor1.name);
        TY *cs = ty_instantiate(ce->scheme, level);
        TY *cs_d = ty_deref(cs);
        ty_unify_at(line, expected, cs_d->arr.result);
        infer_pat_walk(level, p->ctor1.arg, cs_d->arr.param, acc, line);
        return;
      }
      case P_RECORD: {
        // pattern's "shape" is a record type with the listed fields.
        // Each field type is fresh; recurse into each field's sub-pattern.
        TY **its = (TY **)alloca(sizeof(TY *) * (p->rec.n ? p->rec.n : 1));
        for (int i = 0; i < p->rec.n; i++) its[i] = ty_var(level);
        ty_unify_at(line, expected, ty_record(p->rec.n, p->rec.fields, its));
        for (int i = 0; i < p->rec.n; i++)
            infer_pat_walk(level, p->rec.items[i], its[i], acc, line);
        return;
      }
    }
}

// Infer expression — returns its type, sets e->ty.
static TY *infer(int level, EX *e);

static TY *
infer(int level, EX *e)
{
    TY *t = NULL;
    int line = e->line;
    switch (e->kind) {
      case EX_INT:  t = ty_int(); break;
      case EX_REAL: t = ty_real(); break;
      case EX_STR:  t = ty_string(); break;
      case EX_BOOL: t = ty_bool(); break;
      case EX_UNIT: t = ty_unit(); break;
      case EX_NIL:  t = ty_list(ty_var(level)); break;
      case EX_LREF: {
        struct ty_scheme *s = tenv_lookup_local(e->lref.depth, e->lref.idx);
        if (!s) type_error(line, "internal: no type for lref %s (depth=%u idx=%u)",
                           e->lref.name ? e->lref.name : "?", e->lref.depth, e->lref.idx);
        t = ty_instantiate(s, level);
        break;
      }
      case EX_GREF: {
        struct ty_scheme *s = ty_global_lookup(e->gref.name);
        if (!s) {
            struct ctor_ty_entry *ce = ctor_ty_find(e->gref.name);
            if (ce) s = ce->scheme;
        }
        if (!s) type_error(line, "unbound identifier %s", e->gref.name);
        t = ty_instantiate(s, level);
        break;
      }
      case EX_CTOR0: {
        struct ctor_ty_entry *ce = ctor_ty_find(e->ctor0.name);
        if (!ce) type_error(line, "unknown constructor %s", e->ctor0.name);
        TY *cs = ty_instantiate(ce->scheme, level);
        TY *cs_d = ty_deref(cs);
        if (cs_d->kind == TYK_ARR)
            type_error(line, "constructor %s requires an argument", e->ctor0.name);
        t = cs;
        break;
      }
      case EX_CTOR1: {
        struct ctor_ty_entry *ce = ctor_ty_find(e->ctor1.name);
        if (!ce) type_error(line, "unknown constructor %s", e->ctor1.name);
        TY *cs = ty_instantiate(ce->scheme, level);
        TY *cs_d = ty_deref(cs);
        if (cs_d->kind != TYK_ARR)
            type_error(line, "constructor %s takes no argument", e->ctor1.name);
        TY *at = infer(level, e->ctor1.arg);
        ty_unify_at(line, cs_d->arr.param, at);
        t = cs_d->arr.result;
        break;
      }
      case EX_IF: {
        ty_unify_at(line, infer(level, e->iff.cond), ty_bool());
        TY *tt = infer(level, e->iff.thn);
        TY *et = infer(level, e->iff.els);
        ty_unify_at(line, tt, et);
        t = tt;
        break;
      }
      case EX_SEQ: {
        infer(level, e->seq.first);     // discard
        t = infer(level, e->seq.rest);
        break;
      }
      case EX_LET: {
        TY *vt = infer(level + 1, e->let.value);
        struct ty_scheme *s = ex_is_value(e->let.value)
            ? ty_generalize(level, vt)
            : ty_mono_scheme(vt);
        tenv_push();
        tenv_add(e->let.name, s);
        t = infer(level, e->let.body);
        tenv_pop();
        break;
      }
      case EX_LETREC: {
        int n = e->letrec.n;
        tenv_push();
        TY **v_tys = (TY **)alloca(sizeof(TY *) * n);
        for (int i = 0; i < n; i++) {
            v_tys[i] = ty_var(level + 1);
            tenv_add(e->letrec.names[i], ty_mono_scheme(v_tys[i]));
        }
        for (int i = 0; i < n; i++) {
            TY *vt = infer(level + 1, e->letrec.values[i]);
            ty_unify_at(line, v_tys[i], vt);
        }
        struct ty_frame *f = TENV->frames[TENV->n_frames - 1];
        for (int i = 0; i < n; i++)
            f->slots[i].scheme = ty_generalize(level, v_tys[i]);
        t = infer(level, e->letrec.body);
        tenv_pop();
        break;
      }
      case EX_FN: {
        int np = e->fn.nparams;
        TY **param_tys = (TY **)alloca(sizeof(TY *) * np);
        tenv_push();
        for (int i = 0; i < np; i++) {
            param_tys[i] = ty_var(level);
            tenv_add(e->fn.param_names[i], ty_mono_scheme(param_tys[i]));
        }
        // For non-var patterns: each pushes a separate type-env frame
        // matching the runtime match_arm push.  Patterns with zero
        // binders (wildcards/literals) do NOT push (mirrors runtime).
        int extra_pushed = 0;
        for (int i = 0; i < np; i++) {
            PAT *p = e->fn.param_pats[i];
            if (p->kind == P_VAR || p->kind == P_WILDCARD || p->kind == P_UNIT) continue;
            int n_b = pat_count_binders(p);
            struct binders_acc acc = {0};
            infer_pat_walk(level, p, param_tys[i], &acc, line);
            if (n_b == 0) {
                free(acc.bs);
                continue;
            }
            tenv_push();
            for (int j = 0; j < acc.n; j++) tenv_add(acc.bs[j].name, acc.bs[j].scheme);
            free(acc.bs);
            extra_pushed++;
        }
        TY *body_ty = infer(level, e->fn.body);
        for (int i = 0; i < extra_pushed; i++) tenv_pop();
        tenv_pop();
        t = body_ty;
        for (int i = np - 1; i >= 0; i--) t = ty_arr(param_tys[i], t);
        break;
      }
      case EX_APP: {
        TY *ft = infer(level, e->app.fn);
        TY *at = infer(level, e->app.arg);
        TY *rt = ty_var(level);
        ty_unify_at(line, ft, ty_arr(at, rt));
        t = rt;
        break;
      }
      case EX_TUPLE: {
        TY **its = (TY **)alloca(sizeof(TY *) * e->tuple.n);
        for (int i = 0; i < e->tuple.n; i++) its[i] = infer(level, e->tuple.items[i]);
        t = ty_tup(e->tuple.n, its);
        break;
      }
      case EX_RECORD: {
        // 各 field の式を infer。fields 配列はパース時にソート済。
        TY **its = (TY **)alloca(sizeof(TY *) * (e->rec.n ? e->rec.n : 1));
        for (int i = 0; i < e->rec.n; i++) its[i] = infer(level, e->rec.items[i]);
        t = ty_record(e->rec.n, e->rec.fields, its);
        break;
      }
      case EX_FIELD: {
        // `#f r` — r の型を infer し、record と要求。フィールドが含まれて
        // いなければ型エラー。SML でいう「曖昧な field selector」を回避する
        // ため、receiver の型は確定 (TYK_RECORD まで deref できる) を要求
        // — 行き当たりばったりに row poly を入れない。
        TY *rt = infer(level, e->fld.e);
        TY *rd = ty_deref(rt);
        if (rd->kind != TYK_RECORD)
            type_error(line, "field selector #%s requires a known record type, got %s",
                       e->fld.field, ty_format(rt));
        TY *found = NULL;
        for (int i = 0; i < rd->rec.n; i++) {
            if (strcmp(rd->rec.fields[i], e->fld.field) == 0) {
                found = rd->rec.types[i];
                break;
            }
        }
        if (!found) type_error(line, "no field '%s' in record %s",
                                e->fld.field, ty_format(rd));
        t = found;
        break;
      }
      case EX_CONS: {
        TY *ht = infer(level, e->cons.head);
        TY *tt = infer(level, e->cons.tail);
        ty_unify_at(line, tt, ty_list(ht));
        t = tt;
        break;
      }
      case EX_CASE: {
        TY *st = infer(level, e->cse.scrut);
        TY *result = ty_var(level);
        // Mirror runtime: case → let scrut = ... in arms.  The let frame
        // holds 1 slot (scrut) so depth math works for arms' lrefs.
        tenv_push();
        tenv_add(intern("$scrut"), ty_mono_scheme(st));
        for (int i = 0; i < e->cse.n_arms; i++) {
            PAT *p = e->cse.arms[i].pat;
            EX *body = e->cse.arms[i].body;
            int n_b = pat_count_binders(p);
            int pushed = 0;
            struct binders_acc acc = {0};
            infer_pat_walk(level, p, st, &acc, line);
            if (n_b > 0) {
                tenv_push(); pushed = 1;
                for (int j = 0; j < acc.n; j++) tenv_add(acc.bs[j].name, acc.bs[j].scheme);
            }
            free(acc.bs);
            TY *bt = infer(level, body);
            ty_unify_at(line, bt, result);
            if (pushed) tenv_pop();
        }
        tenv_pop();
        t = result;
        break;
      }
      case EX_HANDLE: {
        TY *bt = infer(level, e->handle.body);
        // Handler runs with $exn bound at slot 0 of a fresh frame.
        tenv_push();
        tenv_add(intern("$exn"), ty_mono_scheme(ty_exn()));
        for (int i = 0; i < e->handle.n_arms; i++) {
            PAT *p = e->handle.arms[i].pat;
            EX *body = e->handle.arms[i].body;
            int n_b = pat_count_binders(p);
            int pushed = 0;
            struct binders_acc acc = {0};
            infer_pat_walk(level, p, ty_exn(), &acc, line);
            if (n_b > 0) {
                tenv_push(); pushed = 1;
                for (int j = 0; j < acc.n; j++) tenv_add(acc.bs[j].name, acc.bs[j].scheme);
            }
            free(acc.bs);
            TY *at = infer(level, body);
            ty_unify_at(line, at, bt);
            if (pushed) tenv_pop();
        }
        tenv_pop();
        t = bt;
        break;
      }
      case EX_REF_NEW: t = ty_ref(infer(level, e->un.e)); break;
      case EX_DEREF: {
        TY *xt = infer(level, e->un.e);
        TY *a = ty_var(level);
        ty_unify_at(line, xt, ty_ref(a));
        t = a;
        break;
      }
      case EX_ASSIGN: {
        TY *lt = infer(level, e->assign.l);
        TY *rt = infer(level, e->assign.r);
        ty_unify_at(line, lt, ty_ref(rt));
        t = ty_unit();
        break;
      }
      case EX_RAISE: {
        ty_unify_at(line, infer(level, e->un.e), ty_exn());
        t = ty_var(level);
        break;
      }
      case EX_BINOP: {
        TY *lt = infer(level, e->bin.l);
        TY *rt = infer(level, e->bin.r);
        switch (e->bin.op) {
          case BO_ADD: case BO_SUB: case BO_MUL: case BO_DIV: case BO_MOD:
            ty_unify_at(line, lt, ty_int());
            ty_unify_at(line, rt, ty_int());
            t = ty_int();
            break;
          case BO_RDIV:
            ty_unify_at(line, lt, ty_real());
            ty_unify_at(line, rt, ty_real());
            t = ty_real();
            break;
          case BO_LT: case BO_LE: case BO_GT: case BO_GE: case BO_EQ: case BO_NE:
            ty_unify_at(line, lt, rt);
            t = ty_bool();
            break;
          case BO_CONCAT:
            ty_unify_at(line, lt, ty_string());
            ty_unify_at(line, rt, ty_string());
            t = ty_string();
            break;
        }
        break;
      }
      case EX_UNOP: {
        TY *xt = infer(level, e->unop.e);
        switch (e->unop.op) {
          case UO_NEG: {
            TY *xd = ty_deref(xt);
            if (xd->kind == TYK_REAL) t = ty_real();
            else { ty_unify_at(line, xt, ty_int()); t = ty_int(); }
            break;
          }
          case UO_NOT:
            ty_unify_at(line, xt, ty_bool());
            t = ty_bool();
            break;
          case UO_DEREF: {
            TY *a = ty_var(level);
            ty_unify_at(line, xt, ty_ref(a));
            t = a;
            break;
          }
        }
        break;
      }
      case EX_ANDALSO: {
        ty_unify_at(line, infer(level, e->andalso.l), ty_bool());
        ty_unify_at(line, infer(level, e->andalso.r), ty_bool());
        t = ty_bool();
        break;
      }
      case EX_ORELSE: {
        ty_unify_at(line, infer(level, e->orelse.l), ty_bool());
        ty_unify_at(line, infer(level, e->orelse.r), ty_bool());
        t = ty_bool();
        break;
      }
      case EX_TOPBIND: {
        TY *vt = infer(level + 1, e->topbind.value);
        struct ty_scheme *s = ex_is_value(e->topbind.value)
            ? ty_generalize(level, vt)
            : ty_mono_scheme(vt);
        ty_global_define(e->topbind.name, s);
        t = ty_unit();
        break;
      }
      case EX_TOPLET_FUNS: {
        int n = e->toplet.n;
        tenv_push();
        TY **v_tys = (TY **)alloca(sizeof(TY *) * n);
        for (int i = 0; i < n; i++) {
            v_tys[i] = ty_var(level + 1);
            tenv_add(e->toplet.names[i], ty_mono_scheme(v_tys[i]));
        }
        for (int i = 0; i < n; i++) {
            TY *vt = infer(level + 1, e->toplet.values[i]);
            ty_unify_at(line, v_tys[i], vt);
        }
        for (int i = 0; i < n; i++) {
            struct ty_scheme *s = ty_generalize(level, v_tys[i]);
            ty_global_define(e->toplet.names[i], s);
        }
        tenv_pop();
        t = ty_unit();
        break;
      }
      case EX_NOOP: t = ty_unit(); break;
    }
    e->ty = t;
    return t;
}

static void
infer_top(EX *e)
{
    infer(0, e);
}

// ---------------------------------------------------------------------------
// AOT compile orchestration.
// ---------------------------------------------------------------------------

static void
maybe_aot_compile(NODE *form)
{
    if (!OPTION.compile || !form) return;
    if (form->head.flags.is_specialized) return;
    setenv("CCACHE_DISABLE", "1", 1);
    for (; AOT_COMPILED < AOT_ENTRIES_LEN; AOT_COMPILED++) {
        NODE *e = AOT_ENTRIES[AOT_COMPILED];
        if (e && !e->head.flags.is_specialized) astro_cs_compile(e, NULL);
    }
    astro_cs_compile(form, NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++)
        if (AOT_ENTRIES[i]) astro_cs_load(AOT_ENTRIES[i], NULL);
    astro_cs_load(form, NULL);
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

static char *
slurp(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "asml: cannot open %s\n", path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(n + 1);
    size_t got = fread(buf, 1, n, fp);
    if (got != (size_t)n) { fprintf(stderr, "asml: short read on %s\n", path); exit(1); }
    buf[n] = '\0';
    fclose(fp);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

static void
usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options] [file.sml]\n"
        "\n"
        "  -e EXPR         evaluate EXPR once and exit\n"
        "  -c, --compile   AOT-compile each top-level form\n"
        "  -q, --quiet     suppress hit/miss progress messages\n"
        "      --no-compile  disable specialised code-store loading\n"
        "  -h, --help      show this help\n",
        progname);
}

static void
run_source(CTX *c, const char *text, size_t len)
{
    src = text;
    src_pos = 0;
    src_len = len;
    src_line = 1;
    lex();
    while (tk.kind != TK_EOF) {
        bool had;
        EX *form_ex = parse_top_form(&had);
        if (!had) break;
        infer_top(form_ex);
        NODE *form = lower_expr(form_ex);
        maybe_aot_compile(form);
        EVAL(c, form);
    }
}

int
main(int argc, char *argv[])
{
    const char *eval_expr = NULL;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            OPTION.quiet = true;
        } else if (strcmp(argv[i], "--no-compile") == 0) {
            OPTION.no_compiled_code = true;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--compile") == 0) {
            OPTION.compile = true;
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) { fprintf(stderr, "asml: -e requires an argument\n"); return 1; }
            eval_expr = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "asml: unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        } else {
            file = argv[i];
        }
    }

    INIT();
    CTX *c = (CTX *)calloc(1, sizeof(CTX));
    c->handlers_top = -1;
    tenv_init();
    install_prelude(c);
    scope_push();   // empty top scope; globals win lookups

    if (eval_expr) {
        run_source(c, eval_expr, strlen(eval_expr));
        return 0;
    }
    if (file) {
        size_t n;
        char *s = slurp(file, &n);
        run_source(c, s, n);
        free(s);
        return 0;
    }

    char *line;
    char prompt[8] = "- ";
    for (;;) {
#ifdef USE_READLINE
        line = readline(prompt);
        if (!line) break;
        if (*line) add_history(line);
#else
        printf("%s", prompt);
        fflush(stdout);
        static char buf[4096];
        if (!fgets(buf, sizeof buf, stdin)) break;
        line = buf;
#endif
        if (line[0]) run_source(c, line, strlen(line));
#ifdef USE_READLINE
        free(line);
#endif
    }
    printf("\n");
    return 0;
}
