#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

struct castro_option OPTION;

// Non-inline helper that wraps setjmp/longjmp for return.  Lives here
// (not in node.def) because EVAL_node_call is always_inlined and gcc
// rejects setjmp in always_inline functions.
__attribute__((noinline))
VALUE
castro_invoke(CTX *c, NODE *body, uint32_t arg_index)
{
    c->fp += arg_index;
    jmp_buf rbuf;
    jmp_buf *prev = c->return_buf;
    c->return_buf = &rbuf;

    VALUE v;
    if (setjmp(rbuf) == 0) {
        v = EVAL(c, body);
    }
    else {
        v = c->return_value;
    }

    c->return_buf = prev;
    c->fp -= arg_index;
    return v;
}

// =====================================================================
// function table
// =====================================================================

void
castro_register_func(CTX *c, const char *name, NODE *body, uint32_t params_cnt, uint32_t locals_cnt)
{
    // search by name (override)
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        if (strcmp(c->func_set[i].name, name) == 0) {
            c->func_set[i].body = body;
            c->func_set[i].params_cnt = params_cnt;
            c->func_set[i].locals_cnt = locals_cnt;
            return;
        }
    }
    if (c->func_set_cnt == c->func_set_capa) {
        c->func_set_capa = c->func_set_capa ? c->func_set_capa * 2 : 16;
        c->func_set = realloc(c->func_set, c->func_set_capa * sizeof(struct function_entry));
    }
    struct function_entry *fe = &c->func_set[c->func_set_cnt++];
    fe->name = strdup(name);
    fe->body = body;
    fe->params_cnt = params_cnt;
    fe->locals_cnt = locals_cnt;
}

struct function_entry *
castro_find_func(CTX *c, const char *name)
{
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        if (strcmp(c->func_set[i].name, name) == 0) {
            return &c->func_set[i];
        }
    }
    return NULL;
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
              case 'n': ch = '\n'; break;
              case 't': ch = '\t'; break;
              case 'r': ch = '\r'; break;
              case '\\': ch = '\\'; break;
              case '"': ch = '"'; break;
              case '0': ch = '\0'; break;
              default:  ch = *l->pos; break;
            }
            l->pos++;
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
    if (IS("nop"))         { sx_expect(l, TK_RPAREN); return ALLOC_node_nop(); }
    if (IS("return_void")) { sx_expect(l, TK_RPAREN); return ALLOC_node_return_void(); }

    // single-arg ops
    static const struct { const char *name; NODE *(*fn)(NODE *); } u1[] = {
        {"drop",    ALLOC_node_drop},
        {"return",  ALLOC_node_return},
        {"neg_i",   ALLOC_node_neg_i},
        {"neg_d",   ALLOC_node_neg_d},
        {"bnot",    ALLOC_node_bnot},
        {"lnot",    ALLOC_node_lnot},
        {"cast_id", ALLOC_node_cast_id},
        {"cast_di", ALLOC_node_cast_di},
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
        {"do_while", ALLOC_node_do_while},
        {"while",   ALLOC_node_while},
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

    if (IS("for")) {
        NODE *a = build_expr(l);
        NODE *b = build_expr(l);
        NODE *c = build_expr(l);
        NODE *d = build_expr(l);
        sx_expect(l, TK_RPAREN);
        return ALLOC_node_for(a, b, c, d);
    }

    if (IS("call")) {
        // (call NAME nargs arg_index)
        char *name = read_string_or_ident(l);
        int64_t nargs = read_int(l);
        int64_t arg_index = read_int(l);
        sx_expect(l, TK_RPAREN);
        struct callcache *cc = calloc(1, sizeof(struct callcache));
        return ALLOC_node_call(name, (uint32_t)nargs, (uint32_t)arg_index, cc);
    }

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

static void
load_program(CTX *c, sx_lexer *l)
{
    sx_expect(l, TK_LPAREN);
    if (!sx_ident_eq(&l->cur, "program")) sx_err(l, "expected `program`");
    sx_next(l);

    while (l->cur.kind == TK_LPAREN) {
        // expect (func NAME PARAMS LOCALS RET BODY)
        sx_next(l);  // consume '('
        if (!sx_ident_eq(&l->cur, "func")) sx_err(l, "expected `func`");
        sx_next(l);

        char *name = read_string_or_ident(l);
        int64_t params = read_int(l);
        int64_t locals = read_int(l);
        // ret type ident: i / d / v
        if (l->cur.kind != TK_IDENT) sx_err(l, "expected ret type ident");
        sx_next(l);

        NODE *body = build_expr(l);
        sx_expect(l, TK_RPAREN);

        body = OPTIMIZE(body);
        castro_register_func(c, name, body, (uint32_t)params, (uint32_t)locals);
        free(name);
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
    c->serial = 1;
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

static int
run_pipeline(const char *cfile, char **out_sx_path)
{
    // Run parse.py to produce the .sx file in the current dir.
    char buf[4096];
    const char *base = strrchr(cfile, '/');
    base = base ? base + 1 : cfile;
    char *sxpath = malloc(strlen(base) + 32);
    snprintf(sxpath, strlen(base) + 32, "./tmp/castro_%d_%s.sx", (int)getpid(), base);

    snprintf(buf, sizeof(buf),
             "python3 %s/parse.py %s > %s",
             "PARSE_DIR_PLACEHOLDER", cfile, sxpath);
    (void)buf;
    *out_sx_path = sxpath;
    return 0;
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

    struct function_entry *fe = castro_find_func(c, "main");
    if (!fe) {
        fprintf(stderr, "no main\n");
        return 1;
    }

    // call main with no args
    jmp_buf rbuf;
    c->return_buf = &rbuf;
    VALUE result;
    if (setjmp(rbuf) == 0) {
        result = EVAL(c, fe->body);
    }
    else {
        result = c->return_value;
    }
    c->return_buf = NULL;

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
