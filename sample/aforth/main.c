/* aforth — Forth subset on ASTro
 *
 * Tokenizer + recursive-descent parser for a Forth subset.  Toplevel runs
 * the non-definition portion of the source; word definitions populate
 * `aforth_word_table[]` so that node_call can dispatch them by index.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

struct aforth_option OPTION;

NODE **aforth_word_table = NULL;
uint32_t aforth_word_count = 0;
static uint32_t aforth_word_capa = 0;

/* ===== source / token storage ===== */

typedef struct {
    const char *s;
    int len;
    int line;
} Tok;

static char *src_buf;
static Tok *toks;
static int tok_n, tok_p;

/* ===== symbol table ===== */

typedef enum { SYM_VAR, SYM_CONST, SYM_WORD, SYM_CREATE } SymKind;

typedef struct {
    char *name;
    SymKind kind;
    int32_t cval;       /* SYM_CONST */
    uint32_t var_id;    /* SYM_VAR / SYM_CREATE */
    uint32_t word_id;   /* SYM_WORD */
} Sym;

static Sym *symtab;
static int sym_n, sym_capa;

/* ===== per-definition compile context ===== */

static char *cur_def_name = NULL;
static uint32_t cur_def_id = 0;
static bool in_definition = false;

/* ===== utilities ===== */

static char *
str_dup_n(const char *s, int len)
{
    char *r = malloc((size_t)len + 1);
    memcpy(r, s, (size_t)len);
    r[len] = '\0';
    return r;
}

static int
strieq(const char *a, int alen, const char *b)
{
    int blen = (int)strlen(b);
    if (alen != blen) return 0;
    for (int i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}

static int
tok_is(const Tok *t, const char *kw)
{
    return strieq(t->s, t->len, kw);
}

static __attribute__((noreturn)) void
fatal(const char *fmt, ...)
{
    fprintf(stderr, "aforth: ");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ===== load & tokenize ===== */

static char *
load_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) fatal("cannot open %s: %s", path, strerror(errno));
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) fatal("read failed");
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void
push_tok(const char *s, int len, int line)
{
    static int tok_capa = 0;
    if (tok_n >= tok_capa) {
        tok_capa = tok_capa ? tok_capa * 2 : 256;
        toks = realloc(toks, (size_t)tok_capa * sizeof(Tok));
    }
    toks[tok_n].s = s;
    toks[tok_n].len = len;
    toks[tok_n].line = line;
    tok_n++;
}

static void
tokenize(const char *buf)
{
    int line = 1;
    const char *p = buf;
    while (*p) {
        /* whitespace */
        while (isspace((unsigned char)*p)) {
            if (*p == '\n') line++;
            p++;
        }
        if (!*p) break;

        /* comments */
        if (*p == '\\') { while (*p && *p != '\n') p++; continue; }
        if (*p == '(' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\n')) {
            /* paren comment */
            p++;
            while (*p && *p != ')') { if (*p == '\n') line++; p++; }
            if (*p == ')') p++;
            continue;
        }

        /* ." string literal — single-token spanning to next " */
        if (p[0] == '.' && p[1] == '"' && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n')) {
            const char *start = p;            /* token starts at .\" */
            p += 3;                            /* skip past `." ` */
            while (*p && *p != '"') { if (*p == '\n') line++; p++; }
            if (*p == '"') p++;
            push_tok(start, (int)(p - start), line);
            continue;
        }

        /* generic whitespace-delimited word */
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        push_tok(start, (int)(p - start), line);
    }
}

/* ===== symbol table ===== */

static Sym *
sym_find(const char *s, int len)
{
    /* search newest first */
    for (int i = sym_n - 1; i >= 0; i--) {
        Sym *sm = &symtab[i];
        if (strieq(s, len, sm->name)) return sm;
    }
    return NULL;
}

static Sym *
sym_add(const char *s, int len)
{
    if (sym_n >= sym_capa) {
        sym_capa = sym_capa ? sym_capa * 2 : 64;
        symtab = realloc(symtab, (size_t)sym_capa * sizeof(Sym));
    }
    Sym *sm = &symtab[sym_n++];
    memset(sm, 0, sizeof(*sm));
    sm->name = str_dup_n(s, len);
    return sm;
}

/* ===== word_table management ===== */

static uint32_t
word_table_alloc_id(void)
{
    if (aforth_word_count >= aforth_word_capa) {
        aforth_word_capa = aforth_word_capa ? aforth_word_capa * 2 : 32;
        aforth_word_table = realloc(aforth_word_table,
                                    (size_t)aforth_word_capa * sizeof(NODE *));
    }
    aforth_word_table[aforth_word_count] = NULL;  /* filled at ; */
    return aforth_word_count++;
}

/* ===== parse helpers: integer literals ===== */

/* Parse an integer-valued token at compile time.  Accepts either a literal
 * numeric token or a previously-defined CONSTANT.  Used by `<n> CONSTANT`
 * and `<n> ALLOT` peek patterns at top level so user code can write
 * `MAX_N ALLOT` after `... CONSTANT MAX_N`. */
static bool
parse_const_int(const Tok *t, int64_t *out);

static bool
parse_int(const Tok *t, int64_t *out)
{
    const char *s = t->s;
    int len = t->len;
    if (len == 0) return false;
    int64_t sign = 1;
    int i = 0;
    if (s[0] == '-') { sign = -1; i = 1; if (len == 1) return false; }
    else if (s[0] == '+') { i = 1; if (len == 1) return false; }
    int64_t v = 0;
    int base = 10;
    if (i + 1 < len && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
        base = 16; i += 2; if (i >= len) return false;
    }
    for (; i < len; i++) {
        int d;
        char ch = s[i];
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
        else return false;
        if (d >= base) return false;
        v = v * base + d;
    }
    *out = v * sign;
    return true;
}

static bool
parse_const_int(const Tok *t, int64_t *out)
{
    if (parse_int(t, out)) return true;
    Sym *sm = sym_find(t->s, t->len);
    if (sm && sm->kind == SYM_CONST) { *out = sm->cval; return true; }
    return false;
}

/* ===== seq folding ===== */

/* Build a right-associative seq tree from a flat list of nodes.
 * [a, b, c, d]  =>  seq(a, seq(b, seq(c, d)))
 * single item  =>  the item itself
 * empty        =>  node_nop  */
static NODE *
fold_seq(NODE **items, int n)
{
    if (n == 0) return ALLOC_node_nop();
    if (n == 1) return items[0];
    NODE *acc = items[n - 1];
    for (int i = n - 2; i >= 0; i--) {
        acc = ALLOC_node_seq(items[i], acc);
    }
    return acc;
}

/* ===== leaf-word table — maps a token to a 0-arg ALLOC_xxx ===== */

static NODE *
alloc_leaf_for(const Tok *t)
{
    /* stack ops */
    if (tok_is(t, "DUP"))    return ALLOC_node_dup();
    if (tok_is(t, "?DUP"))   return ALLOC_node_qdup();
    if (tok_is(t, "DROP"))   return ALLOC_node_drop();
    if (tok_is(t, "SWAP"))   return ALLOC_node_swap();
    if (tok_is(t, "OVER"))   return ALLOC_node_over();
    if (tok_is(t, "ROT"))    return ALLOC_node_rot();
    if (tok_is(t, "NIP"))    return ALLOC_node_nip();
    if (tok_is(t, "TUCK"))   return ALLOC_node_tuck();
    if (tok_is(t, "2DUP"))   return ALLOC_node_2dup();
    if (tok_is(t, "2DROP"))  return ALLOC_node_2drop();
    if (tok_is(t, "DEPTH"))  return ALLOC_node_depth();

    /* arithmetic */
    if (tok_is(t, "+"))      return ALLOC_node_add();
    if (tok_is(t, "-"))      return ALLOC_node_sub();
    if (tok_is(t, "*"))      return ALLOC_node_mul();
    if (tok_is(t, "/"))      return ALLOC_node_div();
    if (tok_is(t, "MOD"))    return ALLOC_node_mod();
    if (tok_is(t, "NEGATE")) return ALLOC_node_neg();
    if (tok_is(t, "ABS"))    return ALLOC_node_abs();
    if (tok_is(t, "1+"))     return ALLOC_node_inc();
    if (tok_is(t, "1-"))     return ALLOC_node_dec();
    if (tok_is(t, "2*"))     return ALLOC_node_2mul();
    if (tok_is(t, "2/"))     return ALLOC_node_2div();

    /* comparisons */
    if (tok_is(t, "="))      return ALLOC_node_eq();
    if (tok_is(t, "<>"))     return ALLOC_node_neq();
    if (tok_is(t, "<"))      return ALLOC_node_lt();
    if (tok_is(t, ">"))      return ALLOC_node_gt();
    if (tok_is(t, "<="))     return ALLOC_node_le();
    if (tok_is(t, ">="))     return ALLOC_node_ge();
    if (tok_is(t, "0="))     return ALLOC_node_zeq();
    if (tok_is(t, "0<"))     return ALLOC_node_zlt();
    if (tok_is(t, "0>"))     return ALLOC_node_zgt();

    /* bitwise */
    if (tok_is(t, "AND"))    return ALLOC_node_and();
    if (tok_is(t, "OR"))     return ALLOC_node_or();
    if (tok_is(t, "XOR"))    return ALLOC_node_xor();
    if (tok_is(t, "INVERT")) return ALLOC_node_invert();
    if (tok_is(t, "LSHIFT")) return ALLOC_node_lshift();
    if (tok_is(t, "RSHIFT")) return ALLOC_node_rshift();

    /* return stack */
    if (tok_is(t, ">R"))     return ALLOC_node_to_r();
    if (tok_is(t, "R>"))     return ALLOC_node_from_r();
    if (tok_is(t, "R@"))     return ALLOC_node_r_fetch();

    /* loop counters */
    if (tok_is(t, "I"))      return ALLOC_node_i();
    if (tok_is(t, "J"))      return ALLOC_node_j();
    if (tok_is(t, "LEAVE"))  return ALLOC_node_leave();

    /* memory */
    if (tok_is(t, "@"))      return ALLOC_node_fetch();
    if (tok_is(t, "!"))      return ALLOC_node_store();
    if (tok_is(t, "+!"))     return ALLOC_node_plus_store();
    if (tok_is(t, "CELLS"))  return ALLOC_node_cells();
    if (tok_is(t, "CELL+"))  return ALLOC_node_cell_plus();

    /* I/O */
    if (tok_is(t, "."))      return ALLOC_node_dot();
    if (tok_is(t, "EMIT"))   return ALLOC_node_emit();
    if (tok_is(t, "CR"))     return ALLOC_node_cr();
    if (tok_is(t, "SPACE"))  return ALLOC_node_space();
    if (tok_is(t, "BL"))     return ALLOC_node_lit(32);    /* push space char */
    if (tok_is(t, "TRUE"))   return ALLOC_node_lit(-1);
    if (tok_is(t, "FALSE"))  return ALLOC_node_lit(0);

    return NULL;
}

/* ===== parser core ===== */

/* end-token ids for parse_seq's `enders` mask */
enum {
    END_SEMI    = 1 << 0,   /* ; (def end) */
    END_THEN    = 1 << 1,
    END_ELSE    = 1 << 2,
    END_REPEAT  = 1 << 3,
    END_AGAIN   = 1 << 4,
    END_UNTIL   = 1 << 5,
    END_WHILE   = 1 << 6,
    END_LOOP    = 1 << 7,
    END_PLOOP   = 1 << 8,
    END_EOF     = 1 << 9,
};

static int classify_ender(const Tok *t)
{
    if (tok_is(t, ";"))      return END_SEMI;
    if (tok_is(t, "THEN"))   return END_THEN;
    if (tok_is(t, "ELSE"))   return END_ELSE;
    if (tok_is(t, "REPEAT")) return END_REPEAT;
    if (tok_is(t, "AGAIN"))  return END_AGAIN;
    if (tok_is(t, "UNTIL"))  return END_UNTIL;
    if (tok_is(t, "WHILE"))  return END_WHILE;
    if (tok_is(t, "LOOP"))   return END_LOOP;
    if (tok_is(t, "+LOOP"))  return END_PLOOP;
    return 0;
}

static NODE *parse_seq(int enders, int *which);

static NODE *
parse_one(void)
{
    if (tok_p >= tok_n) fatal("unexpected EOF");
    Tok *t = &toks[tok_p];
    int line = t->line;

    /* literal int */
    int64_t iv;
    if (parse_int(t, &iv)) {
        tok_p++;
        if (iv >= INT32_MIN && iv <= INT32_MAX) return ALLOC_node_lit((int32_t)iv);
        fatal("integer out of int32 range at line %d", line);
    }

    /* leaf word lookup (DUP, +, etc.) */
    NODE *leaf = alloc_leaf_for(t);
    if (leaf) { tok_p++; return leaf; }

    /* IF / BEGIN / DO / RECURSE */
    if (tok_is(t, "IF")) {
        tok_p++;
        int which;
        NODE *then_b = parse_seq(END_ELSE | END_THEN, &which);
        if (which == END_ELSE) {
            NODE *else_b = parse_seq(END_THEN, NULL);
            return ALLOC_node_if(then_b, else_b);
        }
        return ALLOC_node_if_only(then_b);
    }
    if (tok_is(t, "BEGIN")) {
        tok_p++;
        int which;
        NODE *first = parse_seq(END_UNTIL | END_AGAIN | END_WHILE, &which);
        if (which == END_UNTIL) return ALLOC_node_begin_until(first);
        if (which == END_AGAIN) return ALLOC_node_begin_again(first);
        /* WHILE */
        NODE *body = parse_seq(END_REPEAT, NULL);
        return ALLOC_node_begin_while(first, body);
    }
    if (tok_is(t, "DO")) {
        tok_p++;
        int which;
        NODE *body = parse_seq(END_LOOP | END_PLOOP, &which);
        if (which == END_LOOP) return ALLOC_node_do_loop(body);
        return ALLOC_node_do_plus_loop(body);
    }
    if (tok_is(t, "RECURSE")) {
        tok_p++;
        if (!in_definition) fatal("RECURSE outside : ... ; at line %d", line);
        return ALLOC_node_call(cur_def_name, cur_def_id);
    }
    if (tok_is(t, "EXIT")) {
        fatal("EXIT not supported in this aforth subset (line %d)", line);
    }

    /* string print: ." ..." */
    if (t->len >= 2 && t->s[0] == '.' && t->s[1] == '"') {
        /* token contents: ." HELLO " — strip the leading ." and a single
         * trailing space + "; the body between is what we print. */
        const char *body = t->s + 2;
        int blen = t->len - 2;
        /* skip leading whitespace after ." */
        while (blen > 0 && isspace((unsigned char)*body)) { body++; blen--; }
        /* drop trailing closing " */
        if (blen > 0 && body[blen - 1] == '"') blen--;
        char *cs = str_dup_n(body, blen);
        tok_p++;
        return ALLOC_node_dot_quote(cs);
    }

    /* user-defined word / variable / constant / create */
    Sym *sm = sym_find(t->s, t->len);
    if (sm) {
        const char *iname = sm->name;
        tok_p++;
        switch (sm->kind) {
        case SYM_WORD:
            return ALLOC_node_call(iname, sm->word_id);
        case SYM_VAR:
        case SYM_CREATE:
            return ALLOC_node_var_ref(sm->var_id);
        case SYM_CONST:
            return ALLOC_node_const(sm->cval);
        }
    }

    fatal("unknown word '%.*s' at line %d", t->len, t->s, line);
}

/* parse_seq: collect items until an ender token is hit.
 * `enders` is a bitmask of END_xxx; if any of those tokens is encountered
 * (or EOF if END_EOF is set), the parser returns with `*which` set. */
static NODE *
parse_seq(int enders, int *which)
{
    /* growable item buffer */
    NODE **items = NULL;
    int n = 0, cap = 0;

    while (tok_p < tok_n) {
        Tok *t = &toks[tok_p];
        int e = classify_ender(t);
        if (e && (enders & e)) {
            tok_p++;
            if (which) *which = e;
            NODE *r = fold_seq(items, n);
            free(items);
            return r;
        }
        if (e && !(enders & e)) {
            fatal("unexpected '%.*s' at line %d", t->len, t->s, t->line);
        }

        NODE *node = parse_one();
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            items = realloc(items, (size_t)cap * sizeof(NODE *));
        }
        items[n++] = node;
    }

    if (enders & END_EOF) {
        if (which) *which = END_EOF;
        NODE *r = fold_seq(items, n);
        free(items);
        return r;
    }
    fatal("unexpected EOF inside structured construct");
}

/* ===== top-level parser ===== */

static NODE *
parse_program(void)
{
    /* growable list of toplevel runtime items */
    NODE **items = NULL;
    int n = 0, cap = 0;

    while (tok_p < tok_n) {
        Tok *t = &toks[tok_p];

        /* `: name body ;` — defines a word, no runtime emission. */
        if (tok_is(t, ":")) {
            tok_p++;
            if (tok_p >= tok_n) fatal("`:` without name");
            Tok *nm = &toks[tok_p++];
            uint32_t wid = word_table_alloc_id();
            Sym *sm = sym_add(nm->s, nm->len);
            sm->kind = SYM_WORD;
            sm->word_id = wid;
            cur_def_name = sm->name;
            cur_def_id = wid;
            in_definition = true;
            NODE *body = parse_seq(END_SEMI, NULL);
            in_definition = false;
            cur_def_name = NULL;
            aforth_word_table[wid] = body;
            code_repo_add(sm->name, body, false);
            continue;
        }

        /* `VARIABLE name` — reserve one storage cell. */
        if (tok_is(t, "VARIABLE")) {
            tok_p++;
            if (tok_p >= tok_n) fatal("VARIABLE without name");
            Tok *nm = &toks[tok_p++];
            Sym *sm = sym_add(nm->s, nm->len);
            sm->kind = SYM_VAR;
            extern uint32_t aforth_vars_used_top;  /* defined below */
            sm->var_id = aforth_vars_used_top++;
            continue;
        }

        /* `CREATE name` — reserve one cell at current vars_used.  Subsequent
         * `<n> ALLOT` advances vars_used by n cells. */
        if (tok_is(t, "CREATE")) {
            tok_p++;
            if (tok_p >= tok_n) fatal("CREATE without name");
            Tok *nm = &toks[tok_p++];
            Sym *sm = sym_add(nm->s, nm->len);
            sm->kind = SYM_CREATE;
            extern uint32_t aforth_vars_used_top;
            sm->var_id = aforth_vars_used_top;
            continue;
        }

        /* `<int|const> CONSTANT name` — peek pattern. */
        int64_t iv;
        if (parse_const_int(t, &iv)
            && tok_p + 2 < tok_n
            && tok_is(&toks[tok_p + 1], "CONSTANT")) {
            Tok *nm = &toks[tok_p + 2];
            Sym *sm = sym_add(nm->s, nm->len);
            sm->kind = SYM_CONST;
            if (iv < INT32_MIN || iv > INT32_MAX)
                fatal("CONSTANT out of int32 range at line %d", t->line);
            sm->cval = (int32_t)iv;
            tok_p += 3;
            continue;
        }

        /* `<int|const> ALLOT` — peek pattern, advances vars_used. */
        if (parse_const_int(t, &iv)
            && tok_p + 1 < tok_n
            && tok_is(&toks[tok_p + 1], "ALLOT")) {
            extern uint32_t aforth_vars_used_top;
            if (iv < 0 || iv > AFORTH_VARS_SIZE)
                fatal("ALLOT count out of range at line %d", t->line);
            aforth_vars_used_top += (uint32_t)iv;
            tok_p += 2;
            continue;
        }

        /* fall-through: regular token, append to toplevel sequence */
        NODE *node = parse_one();
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            items = realloc(items, (size_t)cap * sizeof(NODE *));
        }
        items[n++] = node;
    }

    NODE *r = fold_seq(items, n);
    free(items);
    return r;
}

/* track vars_used at parse time (for VARIABLE / CREATE / ALLOT layout) */
uint32_t aforth_vars_used_top = 0;

/* ===== public parser entry ===== */

NODE *
aforth_parse_file(const char *path)
{
    src_buf = load_file(path);
    tokenize(src_buf);
    return parse_program();
}

/* ===== context ===== */

CTX *
aforth_ctx_new(void)
{
    CTX *c = calloc(1, sizeof(CTX));
    c->dstack_base = calloc(AFORTH_DSTACK_SIZE, sizeof(VALUE));
    c->dstack_end  = c->dstack_base + AFORTH_DSTACK_SIZE;
    c->dsp = c->dstack_base;
    c->rstack_base = calloc(AFORTH_RSTACK_SIZE, sizeof(VALUE));
    c->rstack_end  = c->rstack_base + AFORTH_RSTACK_SIZE;
    c->rsp = c->rstack_base;
    c->dostack_base = calloc(AFORTH_DOSTACK_SIZE, sizeof(struct aforth_do_frame));
    c->dostack_end  = c->dostack_base + AFORTH_DOSTACK_SIZE;
    c->dop = c->dostack_base;
    c->vars = calloc(AFORTH_VARS_SIZE, sizeof(VALUE));
    c->vars_used = aforth_vars_used_top;
    c->leave_flag = 0;
    return c;
}

void
aforth_ctx_free(CTX *c)
{
    free(c->dstack_base);
    free(c->rstack_base);
    free(c->dostack_base);
    free(c->vars);
    free(c);
}

void
aforth_run(CTX *c, NODE *toplevel)
{
    EVAL(c, toplevel);
}

void
aforth_aot_compile_all(NODE *toplevel)
{
    /* register every word body + the toplevel as compile entries */
    if (!OPTION.quiet)
        fprintf(stderr, "aforth: AOT compiling %u words + toplevel\n",
                aforth_word_count);
    astro_cs_compile(toplevel, NULL);
    for (uint32_t i = 0; i < aforth_word_count; i++) {
        if (aforth_word_table[i]) astro_cs_compile(aforth_word_table[i], NULL);
    }
    astro_cs_build(NULL);
    astro_cs_reload();
    /* re-resolve dispatchers so this run uses the freshly-baked SDs */
    astro_cs_load(toplevel, NULL);
    for (uint32_t i = 0; i < aforth_word_count; i++) {
        if (aforth_word_table[i]) astro_cs_load(aforth_word_table[i], NULL);
    }
}

/* ===== entry ===== */

static void
usage(void)
{
    fprintf(stderr,
        "usage: aforth [options] FILE.fs\n"
        "  -q              quiet (suppress framework chatter)\n"
        "  --no-compile    don't try to load specialized SDs\n"
        "  --no-codegen    don't generate specialized SDs\n"
        "  --aot-compile   compile every entry to code_store/all.so, then run\n"
        "  --dump-ast      dump the parsed AST and exit\n"
    );
    exit(1);
}

int
main(int argc, char *argv[])
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-q") == 0)              OPTION.quiet = true;
        else if (strcmp(a, "--no-compile") == 0)  OPTION.no_compiled_code = true;
        else if (strcmp(a, "--no-codegen") == 0)  OPTION.no_generate_specialized_code = true;
        else if (strcmp(a, "--aot-compile") == 0) OPTION.aot_compile = true;
        else if (strcmp(a, "--dump-ast") == 0)    OPTION.dump_ast = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage();
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); }
        else if (!path) path = a;
        else { fprintf(stderr, "extra arg %s\n", a); usage(); }
    }
    if (!path) usage();

    INIT();
    NODE *toplevel = aforth_parse_file(path);

    if (OPTION.dump_ast) {
        DUMP(stdout, toplevel, true);
        printf("\n");
        return 0;
    }

    if (OPTION.aot_compile) aforth_aot_compile_all(toplevel);

    CTX *c = aforth_ctx_new();
    aforth_run(c, toplevel);
    aforth_ctx_free(c);
    return 0;
}
