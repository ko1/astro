#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "context.h"
#include "node.h"
#include "astro_code_store.h"

NODE *PARSE_SOURCE(const char *source);

struct astr_option OPTION;
size_t node_cnt;
CTX *parse_ctx;

// ---------------------------------------------------------------------------
// Code repo (function bodies registered as their own AOT entries).
// ---------------------------------------------------------------------------

static struct code_repo {
    uint32_t size;
    uint32_t capa;
    struct code_entry {
        const char *name;
        NODE *body;
        uint32_t locals_cnt;
    } *entries;
} code_repo;

static struct code_entry *
code_repo_new_entry(void)
{
    if (code_repo.size >= code_repo.capa) {
        uint32_t capa = code_repo.capa ? code_repo.capa * 2 : 8;
        code_repo.entries = realloc(code_repo.entries, sizeof(struct code_entry) * capa);
        code_repo.capa = capa;
    }
    return &code_repo.entries[code_repo.size++];
}

NODE *
code_repo_find(node_hash_t h)
{
    if (h == 0) return NULL;
    for (uint32_t i = 0; i < code_repo.size; i++) {
        NODE *n = code_repo.entries[i].body;
        if (HASH(n) == h) return n;
    }
    return NULL;
}

NODE *
code_repo_find_by_name(const char *name)
{
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (strcmp(code_repo.entries[i].name, name) == 0)
            return code_repo.entries[i].body;
    }
    return NULL;
}

void
code_repo_add(const char *name, NODE *body, bool force_add)
{
    if (body == NULL) return;
    if (!force_add && code_repo_find(HASH(body)) != NULL) return;
    struct code_entry *ce = code_repo_new_entry();
    ce->name = name;
    ce->body = body;
    ce->locals_cnt = 0;
}

NODE *
astr_resolve_body(CTX *c, const char *name)
{
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        if (strcmp(c->func_set[i].name, name) == 0) {
            return c->func_set[i].body;
        }
    }
    fprintf(stderr, "astr: undefined function: %s\n", name);
    exit(1);
}

uint32_t
code_repo_find_locals_cnt_by_name(const char *name)
{
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (strcmp(code_repo.entries[i].name, name) == 0)
            return code_repo.entries[i].locals_cnt;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Built-in R functions.  Numeric helpers wrap the libm op; vector-aware
// ones broadcast over the input.
// ---------------------------------------------------------------------------

static VALUE bf_print(VALUE v)  { astr_print(stdout, v); return v; }
static VALUE bf_cat_n(VALUE *args, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i > 0) fputc(' ', stdout);
        astr_cat(stdout, args[i]);
    }
    return ASTR_NULL;
}
static VALUE bf_length(VALUE v) { return ASTR_FIX((int64_t)astr_length(v)); }

// Element-wise unary numeric → numeric.  Falls through to scalar for
// fixnum/float; vectors map element-wise into a new num_vec.
static VALUE
unary_numeric(VALUE v, double (*op)(double))
{
    if (ASTR_IS_FIX(v)) return astr_make_float(op((double)ASTR_FIX_VAL(v)));
    struct astr_obj *o = ASTR_PTR(v);
    if (o->type == ASTR_T_FLOAT) return astr_make_float(op(o->dbl));
    if (o->type == ASTR_T_NUM_VEC || o->type == ASTR_T_INT_VEC) {
        size_t len = (o->type == ASTR_T_NUM_VEC) ? o->numvec.len : o->intvec.len;
        VALUE result = astr_make_numvec_n(len);
        struct astr_obj *out = ASTR_PTR(result);
        for (size_t i = 0; i < len; i++) {
            double x = (o->type == ASTR_T_NUM_VEC) ? o->numvec.items[i] : (double)o->intvec.items[i];
            out->numvec.items[i] = op(x);
        }
        return result;
    }
    return astr_make_float(op(astr_to_double(v)));
}

static double op_floor(double x) { return floor(x); }
static double op_ceil (double x) { return ceil (x); }
static double op_sqrt (double x) { return sqrt (x); }
static double op_abs  (double x) { return fabs (x); }
static double op_log  (double x) { return log  (x); }
static double op_exp  (double x) { return exp  (x); }
static double op_sin  (double x) { return sin  (x); }
static double op_cos  (double x) { return cos  (x); }
static double op_tan  (double x) { return tan  (x); }
static double op_round(double x) { return round(x); }

static VALUE bf_floor(VALUE v) { return unary_numeric(v, op_floor); }
static VALUE bf_ceil (VALUE v) { return unary_numeric(v, op_ceil); }
static VALUE bf_sqrt (VALUE v) { return unary_numeric(v, op_sqrt); }
static VALUE bf_abs  (VALUE v) { return unary_numeric(v, op_abs); }
static VALUE bf_log  (VALUE v) { return unary_numeric(v, op_log); }
static VALUE bf_exp  (VALUE v) { return unary_numeric(v, op_exp); }
static VALUE bf_sin  (VALUE v) { return unary_numeric(v, op_sin); }
static VALUE bf_cos  (VALUE v) { return unary_numeric(v, op_cos); }
static VALUE bf_tan  (VALUE v) { return unary_numeric(v, op_tan); }
static VALUE bf_round(VALUE v) { return unary_numeric(v, op_round); }

static VALUE bf_as_integer(VALUE v) { return astr_make_int(astr_to_int(v)); }
static VALUE bf_as_numeric(VALUE v) { return astr_make_float(astr_to_double(v)); }
static VALUE bf_is_numeric(VALUE v) {
    if (ASTR_IS_FIX(v)) return ASTR_TRUE;
    if (ASTR_IS_PTR(v)) {
        int t = ASTR_PTR(v)->type;
        if (t == ASTR_T_FLOAT || t == ASTR_T_NUM_VEC || t == ASTR_T_INT_VEC) return ASTR_TRUE;
    }
    return ASTR_FALSE;
}
static VALUE bf_is_character(VALUE v) {
    return (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_STRING) ? ASTR_TRUE : ASTR_FALSE;
}

// sum() — sum over a numeric vector or arbitrary numeric args.
static VALUE
bf_sum_n(VALUE *args, size_t n)
{
    int64_t isum = 0;
    double  dsum = 0.0;
    bool    use_double = false;
    for (size_t i = 0; i < n; i++) {
        VALUE v = args[i];
        if (ASTR_IS_FIX(v)) {
            if (!use_double) isum += ASTR_FIX_VAL(v);
            else             dsum += (double)ASTR_FIX_VAL(v);
            continue;
        }
        struct astr_obj *o = ASTR_PTR(v);
        switch (o->type) {
          case ASTR_T_FLOAT:
            if (!use_double) { dsum = (double)isum; use_double = true; }
            dsum += o->dbl;
            break;
          case ASTR_T_NUM_VEC:
            if (!use_double) { dsum = (double)isum; use_double = true; }
            for (size_t j = 0; j < o->numvec.len; j++) dsum += o->numvec.items[j];
            break;
          case ASTR_T_INT_VEC:
            for (size_t j = 0; j < o->intvec.len; j++) {
                if (!use_double) isum += o->intvec.items[j];
                else             dsum += (double)o->intvec.items[j];
            }
            break;
          default: break;
        }
    }
    return use_double ? astr_make_float(dsum) : astr_make_int(isum);
}

// `c(...)` — concatenate.  All-numeric args produce a numeric vector;
// any string arg produces a character vector (R's coercion: non-string
// args get formatted into strings).
static VALUE
bf_c_n(VALUE *args, size_t n)
{
    if (n == 0) return ASTR_NULL;
    bool any_string = false;
    bool any_list   = false;
    for (size_t i = 0; i < n; i++) {
        VALUE v = args[i];
        if (ASTR_IS_PTR(v)) {
            int t = ASTR_PTR(v)->type;
            if (t == ASTR_T_STRING) any_string = true;
            if (t == ASTR_T_LIST)   any_list   = true;
        }
    }
    if (any_list) {
        // Flatten lists into a list (one level).
        size_t total = 0;
        for (size_t i = 0; i < n; i++) {
            VALUE v = args[i];
            if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_LIST) total += ASTR_PTR(v)->lst.len;
            else                                                     total += 1;
        }
        VALUE result = astr_make_list(NULL, total);
        struct astr_obj *out = ASTR_PTR(result);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            VALUE v = args[i];
            if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_LIST) {
                for (size_t j = 0; j < ASTR_PTR(v)->lst.len; j++)
                    out->lst.items[k++] = ASTR_PTR(v)->lst.items[j];
            }
            else {
                out->lst.items[k++] = v;
            }
        }
        return result;
    }
    if (any_string) {
        VALUE result = astr_make_list(NULL, 0);
        struct astr_obj *out = ASTR_PTR(result);
        out->type = ASTR_T_STR_VEC;
        // We emit strings into out->lst.items even for STR_VEC so the
        // GC traces them; the type tag distinguishes printing.  Reuse
        // ASTR_T_LIST allocator for storage and re-tag as STR_VEC.
        out->lst.items = (VALUE *)GC_malloc(sizeof(VALUE) * (n ? n : 1));
        out->lst.capa = n ? n : 1;
        out->lst.len = 0;
        for (size_t i = 0; i < n; i++) {
            VALUE v = args[i];
            if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_STRING) {
                out->lst.items[out->lst.len++] = v;
            }
            else {
                // Coerce to string via paste (single-arg).
                out->lst.items[out->lst.len++] = astr_paste(&v, 1, "");
            }
        }
        return result;
    }
    return astr_make_numvec_from(args, n);
}

// paste(...) — R-style concatenation.  Optional `sep=` keyword arg
// not yet supported; default sep=" " applied.
static VALUE bf_paste_n(VALUE *args, size_t n)  { return astr_paste(args, n, " "); }
static VALUE bf_paste0_n(VALUE *args, size_t n) { return astr_paste(args, n, "");  }

static VALUE
bf_nchar(VALUE v)
{
    if (ASTR_IS_PTR(v) && ASTR_PTR(v)->type == ASTR_T_STRING) {
        return ASTR_FIX((int64_t)ASTR_PTR(v)->str.len);
    }
    return ASTR_FIX(0);
}

static VALUE
bf_substr(VALUE v, VALUE start, VALUE stop)
{
    if (!ASTR_IS_PTR(v) || ASTR_PTR(v)->type != ASTR_T_STRING) {
        return astr_make_string("", 0);
    }
    struct astr_obj *o = ASTR_PTR(v);
    int64_t s = astr_to_int(start);
    int64_t e = astr_to_int(stop);
    if (s < 1) s = 1;
    if (e > (int64_t)o->str.len) e = (int64_t)o->str.len;
    if (e < s) return astr_make_string("", 0);
    return astr_make_string(o->str.chars + (s - 1), (size_t)(e - s + 1));
}

typedef struct {
    const char *name;
    void *func;
    int arity;
} BuiltinDecl;

static const BuiltinDecl builtins[] = {
    { "print",        (void *)bf_print,        1 },
    { "cat",          (void *)bf_cat_n,       -1 },
    { "length",       (void *)bf_length,       1 },
    { "floor",        (void *)bf_floor,        1 },
    { "ceiling",      (void *)bf_ceil,         1 },
    { "sqrt",         (void *)bf_sqrt,         1 },
    { "abs",          (void *)bf_abs,          1 },
    { "log",          (void *)bf_log,          1 },
    { "exp",          (void *)bf_exp,          1 },
    { "sin",          (void *)bf_sin,          1 },
    { "cos",          (void *)bf_cos,          1 },
    { "tan",          (void *)bf_tan,          1 },
    { "round",        (void *)bf_round,        1 },
    { "as.integer",   (void *)bf_as_integer,   1 },
    { "as.numeric",   (void *)bf_as_numeric,   1 },
    { "is.numeric",   (void *)bf_is_numeric,   1 },
    { "is.character", (void *)bf_is_character, 1 },
    { "sum",          (void *)bf_sum_n,       -1 },
    { "c",            (void *)bf_c_n,         -1 },
    { "paste",        (void *)bf_paste_n,     -1 },
    { "paste0",       (void *)bf_paste0_n,    -1 },
    { "nchar",        (void *)bf_nchar,        1 },
    { "substr",       (void *)bf_substr,       3 },
    // alias: cat with single arg uses the unary form for slightly
    // tighter dispatch, but bf_cat_n covers both shapes.
};

const BuiltinDecl *astr_builtins      = builtins;
unsigned int       astr_builtin_count = sizeof(builtins) / sizeof(builtins[0]);

// ---------------------------------------------------------------------------
// CTX setup.
// ---------------------------------------------------------------------------

static CTX *
create_context(void)
{
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    c->env = c->fp = (VALUE *)GC_malloc(sizeof(VALUE) * 4096);
    c->func_set = (struct function_entry *)GC_malloc(sizeof(struct function_entry) * 256);
    c->func_set_cnt = 0;
    return c;
}

static char *
read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "astr: cannot open `%s`\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fprintf(stderr, "astr: read error\n");
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void
build_code_store_aot(NODE *ast)
{
    // ccache misbehaves under sandboxed runs (writes outside its
    // allowed paths); the user has confirmed it can stay disabled
    // for our bake.  Plain `cc` is what astro_cs_build invokes, so
    // CCACHE_DISABLE=1 makes ccache forward unconditionally.
    setenv("CCACHE_DISABLE", "1", 0);

    if (ast) astro_cs_compile(ast, NULL);
    for (uint32_t i = 0; i < code_repo.size; i++) {
        NODE *body = code_repo.entries[i].body;
        if (body) astro_cs_compile(body, NULL);
    }
    astro_cs_build(NULL);
    astro_cs_reload();
}

static void
clear_code_store_dir(void)
{
    int rc = system("rm -rf code_store");
    if (rc != 0) {
        fprintf(stderr, "astr: --ccs: rm -rf code_store failed (rc=%d)\n", rc);
    }
}

static const char *
parse_argv(int argc, char *argv[])
{
    const char *src_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-q") || !strcmp(a, "--quiet"))   OPTION.quiet = true;
        else if (!strcmp(a, "-i") || !strcmp(a, "--plain"))   OPTION.plain = true;
        else if (!strcmp(a, "-c") || !strcmp(a, "--aot"))     OPTION.compile_first = true;
        else if (!strcmp(a, "-b"))                            OPTION.skip_bake = true;
        else if (!strcmp(a, "--aot-compile"))                 OPTION.compile_only = true;
        else if (!strcmp(a, "--ccs"))                         OPTION.clear_store = true;
        else if (!strcmp(a, "--dump-ast"))                    OPTION.dump_ast = true;
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "astr: unknown option: %s\n", a);
            exit(1);
        }
        else {
            if (src_path) {
                fprintf(stderr, "astr: multiple input files not supported\n");
                exit(1);
            }
            src_path = a;
        }
    }
    return src_path;
}

int
main(int argc, char *argv[])
{
    GC_init();

    const char *src_path = parse_argv(argc, argv);
    if (!src_path) {
        fprintf(stderr, "usage: astr [options] script.r\n");
        return 1;
    }

    parse_ctx = create_context();
    char *source = read_file(src_path);
    NODE *ast = PARSE_SOURCE(source);

    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    if (OPTION.clear_store) clear_code_store_dir();
    if (!OPTION.plain) INIT();

    if (OPTION.compile_first && !OPTION.plain && !OPTION.skip_bake) {
        build_code_store_aot(ast);
    }

    if (!OPTION.plain) OPTIMIZE(ast);

    if (OPTION.compile_only) return 0;

    RESULT r = EVAL(parse_ctx, ast, parse_ctx->env);
    if (!OPTION.quiet) {
        printf("Result: ");
        astr_cat(stdout, r.value);
        putchar('\n');
    }
    return 0;
}
