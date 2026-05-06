/*
 * filter.c — recursive-descent parser for the jq filter language.
 *
 * Tree-shape AST with `pipe` as the iteration node.  Built-in names
 * (`length`, `map`, `range`, `select`, ...) are resolved at parse time
 * to dedicated NODEs (`ALLOC_node_b_length`, etc.).  Unknown names
 * fall through to `ALLOC_node_call` for user `def`s.
 *
 * Operator precedence (low → high):
 *   pipe `|`
 *   comma `,`
 *   alt `//`
 *   `or`
 *   `and`
 *   compare `==` `!=` `<` `<=` `>` `>=`
 *   add/sub
 *   mul/div/mod
 *   unary minus
 *   postfix `.foo`, `.[...]`, `.[]`, `?`
 *   primary
 */
#include "context.h"
#include "node.h"
#include <ctype.h>

/* ----- token stream ---------------------------------------------------- */

typedef enum {
    TK_END = 0,
    TK_DOT, TK_DDOT, TK_QUEST,
    TK_LP, TK_RP, TK_LBRK, TK_RBRK, TK_LBRACE, TK_RBRACE,
    TK_COMMA, TK_PIPE, TK_COLON, TK_SEMI,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ASSIGN, TK_UPDEQ, TK_PLUSEQ, TK_MINUSEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ, TK_ALTEQ,
    TK_ALT, TK_AT, TK_DOLLAR,
    TK_INT, TK_NUM, TK_STR, TK_INTERP, TK_IDENT,
    TK_KW_TRUE, TK_KW_FALSE, TK_KW_NULL,
    TK_KW_IF, TK_KW_THEN, TK_KW_ELIF, TK_KW_ELSE, TK_KW_END,
    TK_KW_AND, TK_KW_OR, TK_KW_NOT,
    TK_KW_AS, TK_KW_DEF, TK_KW_IMPORT, TK_KW_INCLUDE, TK_KW_MODULE,
    TK_KW_TRY, TK_KW_CATCH,
    TK_KW_REDUCE, TK_KW_FOREACH,
    TK_KW_LABEL, TK_KW_BREAK,
    TK_DCOLON,            /* `::` namespace separator */
} ttype_t;

typedef struct {
    ttype_t type;
    int64_t i;
    double  d;
    const char *s;
    size_t  slen;        /* length of `s` for TK_STR (may contain NUL) */
} token_t;

typedef struct {
    const char *src;
    const char *p;
    const char *end;
    token_t tok;
    bool peeked;
} lexer_t;

static char *
strdup_n(const char *s, size_t n)
{
    char *r = (char *)GC_malloc_atomic(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static void
lex_skip_ws(lexer_t *L)
{
    while (L->p < L->end) {
        char c = *L->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') L->p++;
        else if (c == '#') { while (L->p < L->end && *L->p != '\n') L->p++; }
        else break;
    }
}

static void parse_error(lexer_t *L, const char *fmt, ...) __attribute__((noreturn, format(printf,2,3)));
static void
parse_error(lexer_t *L, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "nuq parse: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, " at: %.*s\n", (int)(L->end - L->p), L->p);
    exit(1);
}

static void
lex_string(lexer_t *L)
{
    typedef enum { P_STR, P_INTERP } pkind_t;
    typedef struct { pkind_t kind; char *text; size_t tlen; const char *isrc; size_t ilen; } part_t;
    part_t parts[64];
    size_t pcnt = 0;
    char *buf = NULL; size_t bcap = 0, blen = 0;
#define PUT(c) do { if (blen + 1 >= bcap) { bcap = bcap ? bcap*2 : 32; buf = (char *)GC_realloc(buf, bcap); } buf[blen++] = (char)(c); } while (0)
    bool any_interp = false;

    while (L->p < L->end && *L->p != '"') {
        if (*L->p == '\\') {
            L->p++;
            if (L->p >= L->end) parse_error(L, "bad escape");
            char c = *L->p;
            switch (c) {
              case '"': PUT('"'); L->p++; break;
              case '\\': PUT('\\'); L->p++; break;
              case '/': PUT('/'); L->p++; break;
              case 'n': PUT('\n'); L->p++; break;
              case 't': PUT('\t'); L->p++; break;
              case 'r': PUT('\r'); L->p++; break;
              case 'b': PUT('\b'); L->p++; break;
              case 'f': PUT('\f'); L->p++; break;
              case 'u': {
                L->p++;
                if (L->p + 4 > L->end) parse_error(L, "short \\u");
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = L->p[i]; cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else parse_error(L, "bad hex");
                }
                L->p += 4;
                if (cp < 0x80) PUT(cp);
                else if (cp < 0x800) { PUT(0xC0|(cp>>6)); PUT(0x80|(cp&0x3F)); }
                else if (cp < 0x10000) { PUT(0xE0|(cp>>12)); PUT(0x80|((cp>>6)&0x3F)); PUT(0x80|(cp&0x3F)); }
                else { PUT(0xF0|(cp>>18)); PUT(0x80|((cp>>12)&0x3F)); PUT(0x80|((cp>>6)&0x3F)); PUT(0x80|(cp&0x3F)); }
                break;
              }
              case '(': {
                if (blen > 0) {
                    parts[pcnt].kind = P_STR;
                    parts[pcnt].text = strdup_n(buf, blen);
                    parts[pcnt].tlen = blen;
                    pcnt++;
                    blen = 0;
                }
                any_interp = true;
                L->p++;
                int depth = 1;
                const char *istart = L->p;
                while (L->p < L->end && depth > 0) {
                    char ch = *L->p;
                    if (ch == '(') depth++;
                    else if (ch == ')') { depth--; if (depth == 0) break; }
                    else if (ch == '"') {
                        L->p++;
                        while (L->p < L->end && *L->p != '"') {
                            if (*L->p == '\\') L->p++;
                            if (L->p < L->end) L->p++;
                        }
                    }
                    L->p++;
                }
                size_t ilen = (size_t)(L->p - istart);
                if (L->p >= L->end) parse_error(L, "unterminated \\(");
                L->p++;
                parts[pcnt].kind = P_INTERP;
                parts[pcnt].isrc = istart;
                parts[pcnt].ilen = ilen;
                pcnt++;
                break;
              }
              default: parse_error(L, "bad escape \\%c", c);
            }
        } else {
            PUT(*L->p);
            L->p++;
        }
    }
    if (L->p >= L->end || *L->p != '"') parse_error(L, "unterminated string");
    L->p++;
    if (blen > 0) {
        parts[pcnt].kind = P_STR;
        parts[pcnt].text = strdup_n(buf, blen);
        parts[pcnt].tlen = blen;
        pcnt++;
    } else if (pcnt == 0) {
        parts[pcnt].kind = P_STR;
        parts[pcnt].text = "";
        parts[pcnt].tlen = 0;
        pcnt++;
    }

    if (!any_interp) {
        L->tok.type = TK_STR;
        L->tok.s = parts[0].text;
        L->tok.slen = parts[0].tlen;
        return;
    }

    struct Node **pnodes = (struct Node **)GC_malloc(pcnt * sizeof(*pnodes));
    for (size_t i = 0; i < pcnt; i++) {
        if (parts[i].kind == P_STR) {
            pnodes[i] = ALLOC_node_str(parts[i].text, (uint32_t)parts[i].tlen);
        } else {
            pnodes[i] = nuq_compile_subexpr(parts[i].isrc, parts[i].ilen);
        }
    }
    uint32_t pid = nuq_interp_intern(pnodes, pcnt);
    L->tok.type = TK_INTERP;
    L->tok.i = (int64_t)pid;
#undef PUT
}

static void
lex_advance(lexer_t *L)
{
    lex_skip_ws(L);
    L->tok.s = NULL;
    if (L->p >= L->end) { L->tok.type = TK_END; return; }
    char c = *L->p;
    switch (c) {
      case '.':
        L->p++;
        if (L->p < L->end && *L->p == '.') { L->p++; L->tok.type = TK_DDOT; return; }
        /* `.<digit>` — JSON-style decimal-fraction number `.5` etc.
         * We back up so the digit lexer below sees `0.<digits>...`. */
        if (L->p < L->end && isdigit((unsigned char)*L->p)) {
            const char *start = L->p - 1;     /* the `.` */
            while (L->p < L->end && isdigit((unsigned char)*L->p)) L->p++;
            if (L->p < L->end && (*L->p == 'e' || *L->p == 'E')) {
                L->p++;
                if (L->p < L->end && (*L->p == '+' || *L->p == '-')) L->p++;
                while (L->p < L->end && isdigit((unsigned char)*L->p)) L->p++;
            }
            size_t n = (size_t)(L->p - start);
            char buf[64];
            if (n >= sizeof(buf) - 1) parse_error(L, "number too long");
            buf[0] = '0';
            memcpy(buf + 1, start, n);     /* "0" + ".5..." */
            buf[n + 1] = '\0';
            L->tok.type = TK_NUM;
            L->tok.d = strtod(buf, NULL);
            return;
        }
        L->tok.type = TK_DOT; return;
      case '?': L->p++; L->tok.type = TK_QUEST; return;
      case '(': L->p++; L->tok.type = TK_LP; return;
      case ')': L->p++; L->tok.type = TK_RP; return;
      case '[': L->p++; L->tok.type = TK_LBRK; return;
      case ']': L->p++; L->tok.type = TK_RBRK; return;
      case '{': L->p++; L->tok.type = TK_LBRACE; return;
      case '}': L->p++; L->tok.type = TK_RBRACE; return;
      case ',': L->p++; L->tok.type = TK_COMMA; return;
      case ':':
        L->p++;
        if (L->p < L->end && *L->p == ':') { L->p++; L->tok.type = TK_DCOLON; return; }
        L->tok.type = TK_COLON; return;
      case ';': L->p++; L->tok.type = TK_SEMI; return;
      case '|':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_UPDEQ; return; }
        L->tok.type = TK_PIPE; return;
      case '+':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_PLUSEQ; return; }
        L->tok.type = TK_PLUS; return;
      case '-':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_MINUSEQ; return; }
        L->tok.type = TK_MINUS; return;
      case '*':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_MULEQ; return; }
        L->tok.type = TK_STAR; return;
      case '/':
        L->p++;
        if (L->p < L->end && *L->p == '/') { L->p++;
            if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_ALTEQ; return; }
            L->tok.type = TK_ALT; return;
        }
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_DIVEQ; return; }
        L->tok.type = TK_SLASH; return;
      case '%':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_MODEQ; return; }
        L->tok.type = TK_PERCENT; return;
      case '=':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_EQ; return; }
        L->tok.type = TK_ASSIGN; return;
      case '!':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_NEQ; return; }
        parse_error(L, "unexpected '!'");
      case '<':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_LE; return; }
        L->tok.type = TK_LT; return;
      case '>':
        L->p++;
        if (L->p < L->end && *L->p == '=') { L->p++; L->tok.type = TK_GE; return; }
        L->tok.type = TK_GT; return;
      case '$': L->p++; L->tok.type = TK_DOLLAR; return;
      case '@': {
        L->p++;
        const char *s = L->p;
        while (L->p < L->end && (isalnum((unsigned char)*L->p) || *L->p == '_')) L->p++;
        L->tok.type = TK_AT;
        L->tok.s = strdup_n(s, (size_t)(L->p - s));
        return;
      }
      case '"': L->p++; lex_string(L); return;
    }
    if (isdigit((unsigned char)c)) {
        const char *start = L->p;
        bool fp = false;
        while (L->p < L->end && isdigit((unsigned char)*L->p)) L->p++;
        if (L->p < L->end && *L->p == '.' && L->p+1 < L->end && isdigit((unsigned char)L->p[1])) {
            fp = true;
            L->p++;
            while (L->p < L->end && isdigit((unsigned char)*L->p)) L->p++;
        }
        if (L->p < L->end && (*L->p == 'e' || *L->p == 'E')) {
            fp = true;
            L->p++;
            if (L->p < L->end && (*L->p == '+' || *L->p == '-')) L->p++;
            while (L->p < L->end && isdigit((unsigned char)*L->p)) L->p++;
        }
        size_t n = (size_t)(L->p - start);
        char buf[64];
        if (n >= sizeof(buf)) parse_error(L, "number too long");
        memcpy(buf, start, n); buf[n] = '\0';
        if (fp) { L->tok.type = TK_NUM; L->tok.d = strtod(buf, NULL); }
        else    { L->tok.type = TK_INT; L->tok.i = strtoll(buf, NULL, 10); }
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = L->p;
        while (L->p < L->end && (isalnum((unsigned char)*L->p) || *L->p == '_')) L->p++;
        size_t n = (size_t)(L->p - start);
        char *id = strdup_n(start, n);
        L->tok.s = id;          /* keep name even for keyword tokens */
#define KW(name, t) if (n == sizeof(name)-1 && memcmp(id, name, n) == 0) { L->tok.type = t; return; }
        KW("true", TK_KW_TRUE);
        KW("false", TK_KW_FALSE);
        KW("null", TK_KW_NULL);
        KW("if", TK_KW_IF);
        KW("then", TK_KW_THEN);
        KW("elif", TK_KW_ELIF);
        KW("else", TK_KW_ELSE);
        KW("end", TK_KW_END);
        KW("and", TK_KW_AND);
        KW("or", TK_KW_OR);
        KW("not", TK_KW_NOT);
        KW("as", TK_KW_AS);
        KW("def", TK_KW_DEF);
        KW("try", TK_KW_TRY);
        KW("catch", TK_KW_CATCH);
        KW("reduce", TK_KW_REDUCE);
        KW("foreach", TK_KW_FOREACH);
        KW("label", TK_KW_LABEL);
        KW("break", TK_KW_BREAK);
        KW("import", TK_KW_IMPORT);
        KW("include", TK_KW_INCLUDE);
        KW("module", TK_KW_MODULE);
#undef KW
        L->tok.type = TK_IDENT; L->tok.s = id; return;
    }
    parse_error(L, "unexpected '%c'", c);
}

static const token_t *
peek(lexer_t *L)
{
    if (!L->peeked) { lex_advance(L); L->peeked = true; }
    return &L->tok;
}

static token_t
take(lexer_t *L)
{
    if (!L->peeked) lex_advance(L);
    L->peeked = false;
    return L->tok;
}

static bool
accept(lexer_t *L, ttype_t t)
{
    if (peek(L)->type == t) { take(L); return true; }
    return false;
}

static void
expect(lexer_t *L, ttype_t t, const char *what)
{
    if (peek(L)->type != t) parse_error(L, "expected %s (got token %d)", what, peek(L)->type);
    take(L);
}

/* Identifier-position token check: TK_IDENT or any TK_KW_* (keywords
 * are still valid as `$name`, def-param names, def names, label names).
 * jq permits this — `$then`, `$or`, `def f(then; else): ...` etc. */
static bool
is_name_tok(const token_t *t)
{
    return t->type == TK_IDENT
        || t->type == TK_KW_IF || t->type == TK_KW_THEN || t->type == TK_KW_ELIF
        || t->type == TK_KW_ELSE || t->type == TK_KW_END || t->type == TK_KW_AND
        || t->type == TK_KW_OR || t->type == TK_KW_NOT || t->type == TK_KW_AS
        || t->type == TK_KW_DEF || t->type == TK_KW_TRY || t->type == TK_KW_CATCH
        || t->type == TK_KW_REDUCE || t->type == TK_KW_FOREACH
        || t->type == TK_KW_LABEL || t->type == TK_KW_BREAK
        || t->type == TK_KW_IMPORT || t->type == TK_KW_INCLUDE
        || t->type == TK_KW_MODULE
        || t->type == TK_KW_TRUE || t->type == TK_KW_FALSE || t->type == TK_KW_NULL
        ;
}

/* ----- builtin name resolution ---------------------------------------- */

/* Map a (name, arity) pair to a specific NODE constructor, or fall
 * back to ALLOC_node_call.  Returns NULL when the caller should use
 * the user-def path. */
static struct Node *
build_builtin_call(const char *name, int arity, struct Node **args)
{
#define BUILTIN0(NAME, CTOR) if (arity == 0 && strcmp(name, NAME) == 0) return CTOR()
#define BUILTIN1(NAME, CTOR) if (arity == 1 && strcmp(name, NAME) == 0) return CTOR(args[0])
#define BUILTIN2(NAME, CTOR) if (arity == 2 && strcmp(name, NAME) == 0) return CTOR(args[0], args[1])
#define BUILTIN3(NAME, CTOR) if (arity == 3 && strcmp(name, NAME) == 0) return CTOR(args[0], args[1], args[2])

    /* 0-arg */
    BUILTIN0("length", ALLOC_node_b_length);
    BUILTIN0("type", ALLOC_node_b_type);
    BUILTIN0("keys", ALLOC_node_b_keys);
    BUILTIN0("keys_unsorted", ALLOC_node_b_keys_unsorted);
    BUILTIN0("values", ALLOC_node_b_values);
    BUILTIN0("not", ALLOC_node_not);
    BUILTIN0("tostring", ALLOC_node_b_to_string);
    BUILTIN0("to_string", ALLOC_node_b_to_string);
    BUILTIN0("tonumber", ALLOC_node_b_tonumber);
    BUILTIN0("tojson", ALLOC_node_b_tojson);
    BUILTIN0("fromjson", ALLOC_node_b_fromjson);
    BUILTIN0("add", ALLOC_node_b_add);
    BUILTIN0("min", ALLOC_node_b_min);
    BUILTIN0("max", ALLOC_node_b_max);
    BUILTIN0("sort", ALLOC_node_b_sort);
    BUILTIN0("reverse", ALLOC_node_b_reverse);
    BUILTIN0("unique", ALLOC_node_b_unique);
    BUILTIN0("to_entries", ALLOC_node_b_to_entries);
    BUILTIN0("from_entries", ALLOC_node_b_from_entries);
    BUILTIN0("paths", ALLOC_node_b_paths);
    BUILTIN0("leaf_paths", ALLOC_node_b_leaf_paths);
    BUILTIN0("error", ALLOC_node_error0);
    BUILTIN0("floor", ALLOC_node_b_floor);
    BUILTIN0("ceil", ALLOC_node_b_ceil);
    BUILTIN0("round", ALLOC_node_b_round);
    BUILTIN0("fabs", ALLOC_node_b_fabs);
    BUILTIN0("abs", ALLOC_node_b_fabs);
    BUILTIN0("sqrt", ALLOC_node_b_sqrt);
    BUILTIN0("first", ALLOC_node_b_first0);
    BUILTIN0("last", ALLOC_node_b_last0);
    BUILTIN0("any", ALLOC_node_b_any0);
    BUILTIN0("all", ALLOC_node_b_all0);
    BUILTIN1("any", ALLOC_node_b_any1);
    BUILTIN1("all", ALLOC_node_b_all1);
    BUILTIN1("ascii", ALLOC_node_b_ascii);
    BUILTIN0("isnull", ALLOC_node_b_isnull);
    BUILTIN0("explode", ALLOC_node_b_explode);
    BUILTIN0("implode", ALLOC_node_b_implode);
    BUILTIN0("ascii_upcase", ALLOC_node_b_ascii_upcase);
    BUILTIN0("ascii_downcase", ALLOC_node_b_ascii_downcase);
    BUILTIN0("recurse", ALLOC_node_b_recurse0);
    BUILTIN1("recurse", ALLOC_node_b_recurse1);
    BUILTIN0("input", ALLOC_node_b_input);
    BUILTIN0("inputs", ALLOC_node_b_inputs);
    BUILTIN0("modulemeta", ALLOC_node_b_modulemeta);
    BUILTIN0("nulls", ALLOC_node_b_nulls);
    BUILTIN0("booleans", ALLOC_node_b_booleans);
    BUILTIN0("numbers", ALLOC_node_b_numbers);
    BUILTIN0("strings", ALLOC_node_b_strings);
    BUILTIN0("arrays", ALLOC_node_b_arrays);
    BUILTIN0("objects", ALLOC_node_b_objects);
    BUILTIN0("iterables", ALLOC_node_b_iterables);
    BUILTIN0("scalars", ALLOC_node_b_scalars);
    BUILTIN0("utf8bytelength", ALLOC_node_b_utf8bytelength);
    BUILTIN0("flatten", ALLOC_node_b_flatten);
    BUILTIN1("flatten", ALLOC_node_b_flatten_n);
    BUILTIN0("env", ALLOC_node_b_env);
    BUILTIN1("isvalid", ALLOC_node_b_isvalid);
    BUILTIN1("IN", ALLOC_node_b_IN1);
    BUILTIN2("gsub", ALLOC_node_b_gsub);
    BUILTIN2("sub", ALLOC_node_b_sub);
    BUILTIN0("nan", ALLOC_node_b_nan);
    BUILTIN0("infinite", ALLOC_node_b_infinite);
    BUILTIN0("isnan", ALLOC_node_b_isnan);
    BUILTIN0("isinfinite", ALLOC_node_b_isinfinite);
    BUILTIN0("sin", ALLOC_node_b_sin);
    BUILTIN0("cos", ALLOC_node_b_cos);
    BUILTIN0("tan", ALLOC_node_b_tan);
    BUILTIN0("asin", ALLOC_node_b_asin);
    BUILTIN0("acos", ALLOC_node_b_acos);
    BUILTIN0("atan", ALLOC_node_b_atan);
    BUILTIN0("sinh", ALLOC_node_b_sinh);
    BUILTIN0("cosh", ALLOC_node_b_cosh);
    BUILTIN0("tanh", ALLOC_node_b_tanh);
    BUILTIN0("exp", ALLOC_node_b_exp);
    BUILTIN0("exp2", ALLOC_node_b_exp2);
    BUILTIN0("exp10", ALLOC_node_b_exp10);
    BUILTIN0("log", ALLOC_node_b_log);
    BUILTIN0("log2", ALLOC_node_b_log2);
    BUILTIN0("log10", ALLOC_node_b_log10);
    BUILTIN0("cbrt", ALLOC_node_b_cbrt);
    BUILTIN0("significand", ALLOC_node_b_significand);
    BUILTIN0("logb", ALLOC_node_b_logb);
    BUILTIN0("gamma", ALLOC_node_b_gamma);
    BUILTIN0("tgamma", ALLOC_node_b_tgamma);
    BUILTIN0("j0", ALLOC_node_b_j0);
    BUILTIN0("j1", ALLOC_node_b_j1);
    BUILTIN0("y0", ALLOC_node_b_y0);
    BUILTIN0("y1", ALLOC_node_b_y1);
    BUILTIN0("trim", ALLOC_node_b_trim);
    BUILTIN0("ltrim", ALLOC_node_b_ltrim);
    BUILTIN0("rtrim", ALLOC_node_b_rtrim);
    BUILTIN0("toboolean", ALLOC_node_b_toboolean);
    BUILTIN1("bsearch", ALLOC_node_b_bsearch);
    BUILTIN0("builtins", ALLOC_node_b_builtins);
    BUILTIN0("debug", ALLOC_node_b_debug);
    BUILTIN0("stderr", ALLOC_node_b_stderr);
    BUILTIN0("gmtime", ALLOC_node_b_gmtime);
    BUILTIN0("localtime", ALLOC_node_b_localtime);
    BUILTIN0("mktime", ALLOC_node_b_mktime);
    BUILTIN1("path", ALLOC_node_b_path);
    BUILTIN1("strftime", ALLOC_node_b_strftime);
    BUILTIN1("strflocaltime", ALLOC_node_b_strflocaltime);
    BUILTIN1("strptime", ALLOC_node_b_strptime);
    BUILTIN0("transpose", ALLOC_node_b_transpose);
    BUILTIN1("isempty", ALLOC_node_b_isempty);
    /* `add(f)` left to user-defined `def add(f): ...;` because nuq's
     * test suite defines it that way and registering as a built-in
     * here would shadow the user def at parse time. */
    BUILTIN2("all", ALLOC_node_b_all2);
    BUILTIN2("any", ALLOC_node_b_any2);
    BUILTIN2("pow", ALLOC_node_b_pow);
    BUILTIN0("empty", ALLOC_node_empty);

    /* 1-arg */
    BUILTIN1("select", ALLOC_node_b_select);
    BUILTIN1("map", ALLOC_node_b_map);
    BUILTIN1("map_values", ALLOC_node_b_map_values);
    BUILTIN1("with_entries", ALLOC_node_b_with_entries);
    BUILTIN1("walk", ALLOC_node_b_walk);
    BUILTIN1("range", ALLOC_node_b_range1);
    BUILTIN1("has", ALLOC_node_b_has);
    BUILTIN1("in", ALLOC_node_b_in);
    BUILTIN1("contains", ALLOC_node_b_contains);
    BUILTIN1("split", ALLOC_node_b_split);
    BUILTIN1("join", ALLOC_node_b_join);
    BUILTIN1("startswith", ALLOC_node_b_startswith);
    BUILTIN1("ltrimstr", ALLOC_node_b_ltrimstr);
    BUILTIN1("rtrimstr", ALLOC_node_b_rtrimstr);
    BUILTIN1("splits", ALLOC_node_b_splits);
    BUILTIN1("endswith", ALLOC_node_b_endswith);
    BUILTIN1("first", ALLOC_node_b_first1);
    BUILTIN1("last", ALLOC_node_b_last1);
    BUILTIN1("sort_by", ALLOC_node_b_sort_by);
    BUILTIN1("group_by", ALLOC_node_b_group_by);
    BUILTIN1("unique_by", ALLOC_node_b_unique_by);
    BUILTIN1("min_by", ALLOC_node_b_min_by);
    BUILTIN1("max_by", ALLOC_node_b_max_by);
    BUILTIN1("indices", ALLOC_node_b_indices);
    /* `_strindices/1` — jq private builtin that requires string input
     * and string pat.  We route through indices() but with stricter
     * type checks via the wrapper NODE_DEF below. */
    BUILTIN1("_strindices", ALLOC_node_b_strindices);
    BUILTIN1("index", ALLOC_node_b_index1);
    BUILTIN1("rindex", ALLOC_node_b_rindex);
    BUILTIN1("test", ALLOC_node_b_test);
    BUILTIN1("getpath", ALLOC_node_b_getpath);
    BUILTIN1("delpaths", ALLOC_node_b_delpaths);
    BUILTIN1("del", ALLOC_node_b_del);
    BUILTIN1("error", ALLOC_node_error1);

    /* 2-arg */
    BUILTIN2("range", ALLOC_node_b_range2);
    BUILTIN2("recurse", ALLOC_node_b_recurse2);
    BUILTIN2("skip", ALLOC_node_b_skip);
    BUILTIN2("while", ALLOC_node_b_while);
    BUILTIN2("until", ALLOC_node_b_until);
    BUILTIN2("setpath", ALLOC_node_b_setpath);
    BUILTIN2("limit", ALLOC_node_b_limit);
    BUILTIN2("nth", ALLOC_node_b_nth);

    /* 3-arg */
    BUILTIN3("range", ALLOC_node_b_range3);

#undef BUILTIN0
#undef BUILTIN1
#undef BUILTIN2
#undef BUILTIN3

    /* Fall back to user-def call.  Copy `args` to a heap-allocated
     * array — the side-table holds the pointer for the duration of
     * the program, so we can't use the parser's stack-local buffer. */
    uint32_t name_id = nuq_intern(name);
    if (arity == 0) return ALLOC_node_call(name_id, 0, 0);
    struct Node **args_heap = (struct Node **)GC_malloc(arity * sizeof(struct Node *));
    for (int i = 0; i < arity; i++) args_heap[i] = args[i];
    uint32_t aid = nuq_args_intern(args_heap, (size_t)arity);
    return ALLOC_node_call(name_id, (uint32_t)arity, aid);
}

/* ----- parser ---------------------------------------------------------- */

static struct Node *parse_pipe(lexer_t *L);
static struct Node *parse_pipe_no_comma(lexer_t *L);
const char *nuq_resolve_call_name(const char *alias, const char *name, int arity);
static struct Node *parse_comma(lexer_t *L);
static struct Node *parse_assign(lexer_t *L);
static struct Node *parse_alt(lexer_t *L);
static struct Node *parse_or(lexer_t *L);
static struct Node *parse_and(lexer_t *L);
static struct Node *parse_compare(lexer_t *L);
static struct Node *parse_addsub(lexer_t *L);
static struct Node *parse_muldiv(lexer_t *L);
static struct Node *parse_unary(lexer_t *L);
static struct Node *parse_postfix(lexer_t *L);
static struct Node *parse_primary(lexer_t *L);
static struct Node *parse_if_tail(lexer_t *L);

/* For `f?` and bare `try f` (no catch), the handler must still be a
 * valid NODE for the generated dispatcher (which derefs operand
 * pointers unconditionally).  Use a sentinel `node_empty` — node_try's
 * body checks the handler via the c->error path, so node_empty's
 * dispatcher is never actually invoked when there's no error. */
static struct Node *g_empty_sentinel = NULL;

static struct Node *
empty_sentinel(void)
{
    if (g_empty_sentinel == NULL) g_empty_sentinel = ALLOC_node_empty();
    return g_empty_sentinel;
}

static struct Node *
wrap_quest(struct Node *body)
{
    return ALLOC_node_try(body, empty_sentinel());
}

/* ----- destructuring patterns for `as` ----- */
static struct nuq_pat *parse_pattern(lexer_t *L);

static struct nuq_pat *
parse_pattern(lexer_t *L)
{
    if (accept(L, TK_DOLLAR)) {
        if (!is_name_tok(peek(L))) parse_error(L, "$name in pattern");
        const char *name = take(L).s;
        struct nuq_pat *p = (struct nuq_pat *)GC_malloc(sizeof(*p));
        p->kind = NUQ_PAT_VAR;
        p->u.var_id = nuq_intern(name);
        return p;
    }
    if (accept(L, TK_LBRK)) {
        struct nuq_pat **items = NULL; size_t cnt = 0, capa = 0;
        if (!accept(L, TK_RBRK)) {
            for (;;) {
                if (cnt == capa) {
                    capa = capa ? capa * 2 : 4;
                    items = (struct nuq_pat **)GC_realloc(items, capa * sizeof(*items));
                }
                items[cnt++] = parse_pattern(L);
                if (accept(L, TK_COMMA)) continue;
                break;
            }
            expect(L, TK_RBRK, "']' in array pattern");
        }
        struct nuq_pat *p = (struct nuq_pat *)GC_malloc(sizeof(*p));
        p->kind = NUQ_PAT_ARRAY;
        p->u.arr.items = items;
        p->u.arr.len = cnt;
        return p;
    }
    if (accept(L, TK_LBRACE)) {
        struct nuq_pat_obj_entry *items = NULL; size_t cnt = 0, capa = 0;
        if (!accept(L, TK_RBRACE)) {
            for (;;) {
                if (cnt == capa) {
                    capa = capa ? capa * 2 : 4;
                    items = (struct nuq_pat_obj_entry *)GC_realloc(items, capa * sizeof(*items));
                }
                struct nuq_pat_obj_entry *e = &items[cnt++];
                /* Three forms:
                 *   $name              -> shorthand: key="name", val=PAT_VAR("name")
                 *   key:   PAT         -> key from ident/string, val nested
                 *   $name: PAT         -> key from var name, val nested (rare)
                 *   (expr): PAT        -> dynamic key (we only support static
                 *                        ident/string here for simplicity) */
                const token_t *k = peek(L);
                if (k->type == TK_DOLLAR) {
                    take(L);
                    if (!is_name_tok(peek(L))) parse_error(L, "$name in pattern");
                    const char *name = take(L).s;
                    if (accept(L, TK_COLON)) {
                        /* `{$name: PAT}` — both bind $name AND descend
                         * with PAT at the same key (jq semantics). We
                         * realize this by emitting two entries that
                         * share the key. */
                        e->key = name;
                        e->key_expr = NULL;
                        struct nuq_pat *v = (struct nuq_pat *)GC_malloc(sizeof(*v));
                        v->kind = NUQ_PAT_VAR;
                        v->u.var_id = nuq_intern(name);
                        e->val = v;
                        if (cnt == capa) {
                            capa = capa ? capa * 2 : 4;
                            items = (struct nuq_pat_obj_entry *)GC_realloc(items, capa * sizeof(*items));
                        }
                        struct nuq_pat_obj_entry *e2 = &items[cnt++];
                        e2->key = name;
                        e2->key_expr = NULL;
                        e2->val = parse_pattern(L);
                    } else {
                        /* shorthand: same name for key and var */
                        e->key = name;
                        e->key_expr = NULL;
                        struct nuq_pat *v = (struct nuq_pat *)GC_malloc(sizeof(*v));
                        v->kind = NUQ_PAT_VAR;
                        v->u.var_id = nuq_intern(name);
                        e->val = v;
                    }
                } else if (k->type == TK_LP) {
                    /* `(expr): PAT` — dynamic key. Evaluate expr at
                     * bind time to get the field name. */
                    take(L);
                    struct Node *kex = parse_pipe(L);
                    expect(L, TK_RP, "')' after pattern key expr");
                    expect(L, TK_COLON, "':' in object pattern");
                    e->key = NULL;
                    e->key_expr = kex;
                    e->val = parse_pattern(L);
                } else if (k->type == TK_IDENT || k->type == TK_STR ||
                           is_name_tok(k)) {
                    /* Accept TK_IDENT, TK_STR, and any keyword as a
                     * literal key — jq permits `{as: PAT}` etc. */
                    const char *name = take(L).s;
                    expect(L, TK_COLON, "':' in object pattern");
                    e->key = name;
                    e->key_expr = NULL;
                    e->val = parse_pattern(L);
                } else parse_error(L, "expected pattern field");
                if (accept(L, TK_COMMA)) continue;
                break;
            }
            expect(L, TK_RBRACE, "'}' in object pattern");
        }
        struct nuq_pat *p = (struct nuq_pat *)GC_malloc(sizeof(*p));
        p->kind = NUQ_PAT_OBJECT;
        p->u.obj.items = items;
        p->u.obj.len = cnt;
        return p;
    }
    parse_error(L, "expected $name, [..] or {..} pattern");
    return NULL;
}

/* Tail of an `if` form, called after `if cond then thn` is consumed.
 * Returns the `else` branch (which itself may be a nested if for
 * elif-chains).  Always returns a non-NULL Node — `if c then t end`
 * desugars to `if(c, t, .)` so the generated dispatcher never sees
 * a NULL operand. */
static struct Node *
parse_if_tail(lexer_t *L)
{
    if (accept(L, TK_KW_END)) return ALLOC_node_identity();
    if (accept(L, TK_KW_ELSE)) {
        struct Node *e = parse_pipe(L);
        expect(L, TK_KW_END, "'end'");
        return e;
    }
    if (accept(L, TK_KW_ELIF)) {
        struct Node *c = parse_pipe(L);
        expect(L, TK_KW_THEN, "'then'");
        struct Node *t = parse_pipe(L);
        struct Node *e = parse_if_tail(L);
        return ALLOC_node_if(c, t, e);
    }
    parse_error(L, "expected elif/else/end");
    return NULL;
}

/* ----- primary --------------------------------------------------------- */

static struct Node *
parse_primary(lexer_t *L)
{
    const token_t *t = peek(L);
    switch (t->type) {
      case TK_DOT: {
        take(L);
        const token_t *p = peek(L);
        if (p->type == TK_IDENT) { const char *name = take(L).s; return ALLOC_node_field(name); }
        if (p->type == TK_STR) { const char *name = take(L).s; return ALLOC_node_field(name); }
        return ALLOC_node_identity();
      }
      case TK_DDOT: take(L); return ALLOC_node_recurse();
      case TK_KW_NULL:  take(L); return ALLOC_node_null();
      case TK_KW_TRUE:  take(L); return ALLOC_node_true();
      case TK_KW_FALSE: take(L); return ALLOC_node_false();
      case TK_INT: {
        token_t tk = take(L);
        /* jq stores all numbers as IEEE-754 doubles — match by demoting
         * literals > 2^53 to doubles too. */
        const long long jq_int_max = (1LL << 53);
        if (tk.i < -jq_int_max || tk.i > jq_int_max) {
            return ALLOC_node_lit(nuq_lit_intern(nuq_make_double((double)tk.i)));
        }
        if (tk.i >= INT32_MIN && tk.i <= INT32_MAX) return ALLOC_node_int((int32_t)tk.i);
        return ALLOC_node_lit(nuq_lit_intern(nuq_make_int(tk.i)));
      }
      case TK_NUM: { token_t tk = take(L); return ALLOC_node_lit(nuq_lit_intern(nuq_make_double(tk.d))); }
      case TK_STR: { token_t tk = take(L); return ALLOC_node_str(tk.s, (uint32_t)tk.slen); }
      case TK_INTERP: { token_t tk = take(L); return ALLOC_node_interp((uint32_t)tk.i); }
      case TK_AT: {
        const char *name = take(L).s;
        uint32_t fid = nuq_fmt_intern(name);
        const token_t *nx = peek(L);
        if (nx->type == TK_STR) {
            token_t tk = take(L);
            return ALLOC_node_format(fid, ALLOC_node_str(tk.s, (uint32_t)tk.slen));
        }
        if (nx->type == TK_INTERP) {
            token_t tk = take(L);
            return ALLOC_node_format(fid, ALLOC_node_interp((uint32_t)tk.i));
        }
        return ALLOC_node_format(fid, NULL);
      }
      case TK_DOLLAR: {
        take(L);
        if (!is_name_tok(peek(L))) parse_error(L, "expected $name");
        const char *nm = take(L).s;
        /* `$__loc__` is jq's special "current source location" pseudo-var.
         * jq returns `{file:<source>, line:<n>}`; nuq doesn't track
         * source positions, so we return a stable placeholder. */
        if (strcmp(nm, "__loc__") == 0) {
            VALUE obj = nuq_make_object(2);
            nuq_object_set_cstr(obj, "file", nuq_make_string("<top-level>", 11));
            nuq_object_set_cstr(obj, "line", nuq_make_int(1));
            return ALLOC_node_lit(nuq_lit_intern(obj));
        }
        /* `$ns::name` — namespaced variable, used by `import "X" as $name;`
         * to expose the data both as `$name` and `$name::name`.  We
         * splice the parts together so a single var lookup finds it. */
        if (peek(L)->type == TK_DCOLON) {
            take(L);
            if (!is_name_tok(peek(L))) parse_error(L, "expected name after `::`");
            const char *nm2 = take(L).s;
            char buf[160];
            snprintf(buf, sizeof(buf), "%s::%s", nm, nm2);
            return ALLOC_node_var(nuq_intern(buf));
        }
        return ALLOC_node_var(nuq_intern(nm));
      }
      case TK_LP: { take(L); struct Node *e = parse_pipe(L); expect(L, TK_RP, "')'"); return e; }
      case TK_LBRK: {
        take(L);
        if (accept(L, TK_RBRK)) return ALLOC_node_array_empty();
        struct Node *body = parse_pipe(L);
        expect(L, TK_RBRK, "']'");
        return ALLOC_node_array(body);
      }
      case TK_LBRACE: {
        take(L);
        struct nuq_obj_entry *items = NULL;
        size_t cnt = 0, capa = 0;
        if (!accept(L, TK_RBRACE)) {
            for (;;) {
                if (cnt == capa) { capa = capa ? capa * 2 : 4;
                    items = (struct nuq_obj_entry *)GC_realloc(items, capa * sizeof(*items)); }
                struct nuq_obj_entry *ie = &items[cnt++];
                memset(ie, 0, sizeof(*ie));
                const token_t *kt = peek(L);
                if (kt->type == TK_IDENT || kt->type == TK_STR) {
                    ie->kkind = 0;
                    ie->kname = take(L).s;
                } else if (kt->type == TK_INTERP) {
                    token_t tk = take(L);
                    ie->kkind = 1;
                    ie->kexpr = ALLOC_node_interp((uint32_t)tk.i);
                } else if (kt->type == TK_DOLLAR) {
                    take(L);
                    if (!is_name_tok(peek(L))) parse_error(L, "$name in obj key");
                    ie->kkind = 2;
                    ie->kname = take(L).s;
                    ie->var_id = nuq_intern(ie->kname);
                } else if (kt->type == TK_LP) {
                    take(L);
                    ie->kkind = 1;
                    ie->kexpr = parse_pipe(L);
                    expect(L, TK_RP, "')'");
                } else if (kt->type == TK_AT) {
                    const char *fnm = take(L).s;
                    if (peek(L)->type == TK_STR) {
                        token_t tk = take(L);
                        ie->kkind = 1;
                        ie->kexpr = ALLOC_node_format(nuq_fmt_intern(fnm), ALLOC_node_str(tk.s, (uint32_t)tk.slen));
                    } else if (peek(L)->type == TK_INTERP) {
                        token_t tk = take(L);
                        ie->kkind = 1;
                        ie->kexpr = ALLOC_node_format(nuq_fmt_intern(fnm), ALLOC_node_interp((uint32_t)tk.i));
                    } else parse_error(L, "expected string after @%s", fnm);
                } else if (is_name_tok(kt)) {
                    /* Any keyword can be used as an object key — its
                     * spelled name comes from the lexer's saved s. */
                    ie->kkind = 0;
                    ie->kname = kt->s ? kt->s : "?";
                    take(L);
                } else parse_error(L, "bad key in object");
                if (accept(L, TK_COLON)) {
                    ie->vexpr = parse_pipe_no_comma(L);
                } else {
                    ie->vexpr = NULL;
                }
                if (accept(L, TK_COMMA)) continue;
                break;
            }
            expect(L, TK_RBRACE, "'}'");
        }
        struct nuq_obj_entry *heap = (struct nuq_obj_entry *)GC_malloc(cnt * sizeof(*heap));
        memcpy(heap, items, cnt * sizeof(*heap));
        /* Pre-build VALUE for static-string keys so the runtime path
         * doesn't repeat nuq_make_string per ctor call.  Computed
         * keys (kkind == 1) and `nuq_eq`-keyed paths still build at
         * runtime — those are the rare case. */
        for (size_t i = 0; i < cnt; i++) {
            if (heap[i].kkind == 0)
                heap[i].kname_value = nuq_make_string(heap[i].kname, strlen(heap[i].kname));
            else if (heap[i].kkind == 2)
                heap[i].kname_value = nuq_make_string(heap[i].kname, strlen(heap[i].kname));
        }
        return ALLOC_node_object(nuq_obj_ctor_intern(heap, cnt));
      }
      case TK_KW_IF: {
        take(L);
        struct Node *cond = parse_pipe(L);
        expect(L, TK_KW_THEN, "'then'");
        struct Node *thn = parse_pipe(L);
        /* Recursive elif-chain build:
         *   if c then t end                → if(c, t, .)
         *   if c then t else e end         → if(c, t, e)
         *   if c then t elif c2 then t2 ...→ if(c, t, parse_if_tail(...))
         * Each `elif` becomes a nested `if` in the else slot, so chains
         * of arbitrary length are supported. */
        struct Node *els = parse_if_tail(L);
        return ALLOC_node_if(cond, thn, els);
      }
      case TK_KW_TRY: {
        take(L);
        /* try / catch bodies bind unary-minus and postfix; `try -.` and
         * `catch -.` are valid jq.  Anything looser (arithmetic, |) is
         * NOT included since it would swallow the catch / following
         * pipe stages. */
        struct Node *body = parse_unary(L);
        struct Node *handler = empty_sentinel();
        if (accept(L, TK_KW_CATCH)) handler = parse_unary(L);
        return ALLOC_node_try(body, handler);
      }
      case TK_KW_REDUCE:
      case TK_KW_FOREACH: {
        bool is_for = (t->type == TK_KW_FOREACH);
        take(L);
        /* The source expression of `reduce` / `foreach` extends up
         * through `//` (alternative) precedence — `as` is the sentinel
         * that ends it.  Earlier we used a postfix-only parser which
         * rejected `[foreach .[] / .[] as $i ...]` (jq accepts it). */
        struct Node *src = parse_alt(L);
        expect(L, TK_KW_AS, "'as'");
        /* `as PAT` — fast path for `$name`, otherwise pattern. */
        bool is_pat = false;
        uint32_t var_id = 0;
        uint32_t pat_id = 0;
        if (peek(L)->type == TK_DOLLAR) {
            lexer_t snap = *L;
            take(L);
            if (peek(L)->type == TK_IDENT) {
                const char *name = take(L).s;
                if (peek(L)->type == TK_LP) {
                    var_id = nuq_intern(name);
                } else {
                    /* Could be `$name` standalone (var) — both LP and
                     * non-LP terminate fine for foreach/reduce. */
                    var_id = nuq_intern(name);
                }
            } else {
                *L = snap;
                struct nuq_pat *pat = parse_pattern(L);
                pat_id = nuq_pat_intern(pat);
                is_pat = true;
            }
        } else {
            struct nuq_pat *pat = parse_pattern(L);
            pat_id = nuq_pat_intern(pat);
            is_pat = true;
        }
        expect(L, TK_LP, "'('");
        struct Node *init = parse_pipe(L);
        expect(L, TK_SEMI, "';'");
        struct Node *update = parse_pipe(L);
        struct Node *extract = NULL;
        if (is_for && accept(L, TK_SEMI)) extract = parse_pipe(L);
        expect(L, TK_RP, "')'");
        if (is_for) {
            if (extract == NULL) extract = ALLOC_node_identity();
            if (is_pat)
                return ALLOC_node_foreach_pat(src, pat_id, init, update, extract);
            return ALLOC_node_foreach(src, var_id, init, update, extract);
        }
        if (is_pat)
            return ALLOC_node_reduce_pat(src, pat_id, init, update);
        return ALLOC_node_reduce(src, var_id, init, update);
      }
      case TK_KW_LABEL: {
        take(L);
        expect(L, TK_DOLLAR, "'$'");
        if (!is_name_tok(peek(L))) parse_error(L, "$name");
        uint32_t vid = nuq_intern(take(L).s);
        expect(L, TK_PIPE, "'|' after label");
        struct Node *body = parse_pipe(L);
        return ALLOC_node_label(vid, body);
      }
      case TK_KW_BREAK: {
        take(L);
        expect(L, TK_DOLLAR, "'$' after break");
        if (!is_name_tok(peek(L))) parse_error(L, "$name after break");
        uint32_t vid = nuq_intern(take(L).s);
        return ALLOC_node_break(vid);
      }
      case TK_KW_NOT: take(L); return ALLOC_node_not();
      case TK_KW_DEF: {
        struct nuq_def_entry items[64];
        size_t cnt = 0;
        while (peek(L)->type == TK_KW_DEF) {
            take(L);
            if (!is_name_tok(peek(L))) parse_error(L, "def name");
            const char *name = take(L).s;
            int arity = 0;
            uint32_t pids[16];
            bool pis_val[16];
            if (accept(L, TK_LP)) {
                for (;;) {
                    if (arity >= 16) parse_error(L, "too many params (max 16)");
                    if (accept(L, TK_DOLLAR)) {
                        if (!is_name_tok(peek(L))) parse_error(L, "$name");
                        pids[arity] = nuq_intern(take(L).s);
                        pis_val[arity] = true;
                    } else {
                        if (!is_name_tok(peek(L))) parse_error(L, "param");
                        pids[arity] = nuq_intern(take(L).s);
                        pis_val[arity] = false;
                    }
                    arity++;
                    if (!accept(L, TK_SEMI)) break;
                }
                expect(L, TK_RP, "')'");
            }
            expect(L, TK_COLON, "':'");
            items[cnt].name_id = nuq_intern(name);
            items[cnt].arity = arity;
            items[cnt].param_ids = (uint32_t *)GC_malloc(arity * sizeof(uint32_t));
            items[cnt].param_is_value = (bool *)GC_malloc(arity * sizeof(bool));
            for (int i = 0; i < arity; i++) {
                items[cnt].param_ids[i] = pids[i];
                items[cnt].param_is_value[i] = pis_val[i];
            }
            items[cnt].body = parse_pipe(L);
            expect(L, TK_SEMI, "';' after def body");
            cnt++;
        }
        struct nuq_def_entry *heap = (struct nuq_def_entry *)GC_malloc(cnt * sizeof(*heap));
        memcpy(heap, items, cnt * sizeof(*heap));
        struct Node *body = parse_pipe(L);
        return ALLOC_node_defs(nuq_def_block_intern(heap, cnt), body);
      }
      case TK_IDENT: {
        const char *name = take(L).s;
        const char *ns_alias = NULL;
        /* `alias::name` — namespaced call into a module imported via
         * `import "X" as alias;` somewhere in the active scope. */
        if (peek(L)->type == TK_DCOLON) {
            take(L);
            if (!is_name_tok(peek(L)))
                parse_error(L, "expected name after `::`");
            ns_alias = name;
            name = take(L).s;
        }
        struct Node *args[16];
        int arity = 0;
        if (accept(L, TK_LP)) {
            for (;;) {
                if (arity >= 16) parse_error(L, "too many call args (max 16)");
                args[arity++] = parse_pipe(L);
                if (!accept(L, TK_SEMI)) break;
            }
            expect(L, TK_RP, "')'");
        }
        const char *resolved = nuq_resolve_call_name(ns_alias, name, arity);
        return build_builtin_call(resolved, arity, args);
      }
      default: parse_error(L, "unexpected token type %d", t->type);
    }
}

/* ----- postfix --------------------------------------------------------- */

/* `acc[expr]` and `acc[a:b]` — in jq, the index/slice argument
 * expressions evaluate against the OUTER input (the input to the
 * surrounding stage), NOT the value of `acc`.  We model this by
 * lifting `.` to a fresh `$__ix__N__` binding and wrapping each arg
 * as `($__ix__N__ | <orig>)`. */
static uint32_t nuq_outer_ix_counter = 0;

static uint32_t
fresh_outer_var(void)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "__ix__%u__", nuq_outer_ix_counter++);
    (void)n;
    return nuq_intern(buf);
}

static struct Node *
wrap_outer(uint32_t var_id, struct Node *e)
{
    if (!e) return NULL;
    return ALLOC_node_pipe(ALLOC_node_var(var_id), e);
}

static struct Node *
build_indexlike(struct Node *acc, struct Node *e1, struct Node *e2,
                bool has_colon, uint32_t flags)
{
    if (!e1 && !e2) {
        /* `[]` — iter, no args; just chain. */
        if (has_colon) {
            return ALLOC_node_pipe(acc, ALLOC_node_slice(NULL, NULL, flags));
        }
        return ALLOC_node_pipe(acc, ALLOC_node_iter());
    }
    /* Bind `.` (current-stage input) to a fresh var, then evaluate the
     * inner indexer with args qualified by that var. */
    uint32_t v = fresh_outer_var();
    struct Node *ww1 = wrap_outer(v, e1);
    struct Node *ww2 = wrap_outer(v, e2);
    struct Node *inner = has_colon
        ? ALLOC_node_slice(ww1, ww2, flags)
        : ALLOC_node_index(ww1);
    return ALLOC_node_as(ALLOC_node_identity(), v,
                         ALLOC_node_pipe(acc, inner));
}

static struct Node *
parse_postfix(lexer_t *L)
{
    struct Node *acc = parse_primary(L);
    for (;;) {
        const token_t *t = peek(L);
        if (t->type == TK_DOT) {
            take(L);
            const token_t *p = peek(L);
            if (p->type == TK_IDENT || p->type == TK_STR) {
                const char *name = take(L).s;
                acc = ALLOC_node_pipe(acc, ALLOC_node_field(name));
            } else if (p->type == TK_LBRK) {
                take(L);
                if (accept(L, TK_RBRK)) acc = ALLOC_node_pipe(acc, ALLOC_node_iter());
                else {
                    struct Node *e1 = NULL;
                    if (peek(L)->type != TK_COLON) e1 = parse_pipe(L);
                    if (accept(L, TK_COLON)) {
                        struct Node *e2 = NULL;
                        if (peek(L)->type != TK_RBRK) e2 = parse_pipe(L);
                        expect(L, TK_RBRK, "']'");
                        uint32_t flags = 0;
                        if (e1) flags |= SLICE_HAS_START;
                        if (e2) flags |= SLICE_HAS_STOP;
                        acc = build_indexlike(acc, e1, e2, true, flags);
                    } else {
                        expect(L, TK_RBRK, "']'");
                        acc = build_indexlike(acc, e1, NULL, false, 0);
                    }
                }
            } else parse_error(L, "expected ident or '[' after '.'");
        } else if (t->type == TK_LBRK) {
            take(L);
            if (accept(L, TK_RBRK)) acc = ALLOC_node_pipe(acc, ALLOC_node_iter());
            else {
                struct Node *e1 = NULL;
                if (peek(L)->type != TK_COLON) e1 = parse_pipe(L);
                if (accept(L, TK_COLON)) {
                    struct Node *e2 = NULL;
                    if (peek(L)->type != TK_RBRK) e2 = parse_pipe(L);
                    expect(L, TK_RBRK, "']'");
                    uint32_t flags = 0;
                    if (e1) flags |= SLICE_HAS_START;
                    if (e2) flags |= SLICE_HAS_STOP;
                    acc = build_indexlike(acc, e1, e2, true, flags);
                } else {
                    expect(L, TK_RBRK, "']'");
                    acc = build_indexlike(acc, e1, NULL, false, 0);
                }
            }
        } else if (t->type == TK_QUEST) {
            take(L);
            acc = wrap_quest(acc);
        } else break;
    }
    return acc;
}

static struct Node *
parse_unary(lexer_t *L)
{
    if (accept(L, TK_MINUS)) {
        struct Node *e = parse_postfix(L);
        return ALLOC_node_neg(e);
    }
    return parse_postfix(L);
}

static struct Node *
parse_muldiv(lexer_t *L)
{
    struct Node *lhs = parse_unary(L);
    for (;;) {
        const token_t *t = peek(L);
        struct Node *(*ctor)(struct Node *, struct Node *) = NULL;
        if (t->type == TK_STAR) { take(L); ctor = ALLOC_node_mul; }
        else if (t->type == TK_SLASH) { take(L); ctor = ALLOC_node_div; }
        else if (t->type == TK_PERCENT) { take(L); ctor = ALLOC_node_mod; }
        else break;
        lhs = ctor(lhs, parse_unary(L));
    }
    return lhs;
}

static struct Node *
parse_addsub(lexer_t *L)
{
    struct Node *lhs = parse_muldiv(L);
    for (;;) {
        const token_t *t = peek(L);
        struct Node *(*ctor)(struct Node *, struct Node *) = NULL;
        if (t->type == TK_PLUS) { take(L); ctor = ALLOC_node_add; }
        else if (t->type == TK_MINUS) { take(L); ctor = ALLOC_node_sub; }
        else break;
        lhs = ctor(lhs, parse_muldiv(L));
    }
    return lhs;
}

static struct Node *
parse_compare(lexer_t *L)
{
    struct Node *lhs = parse_addsub(L);
    const token_t *t = peek(L);
    struct Node *(*ctor)(struct Node *, struct Node *) = NULL;
    switch (t->type) {
      case TK_EQ:  ctor = ALLOC_node_eq; break;
      case TK_NEQ: ctor = ALLOC_node_ne; break;
      case TK_LT:  ctor = ALLOC_node_lt; break;
      case TK_LE:  ctor = ALLOC_node_le; break;
      case TK_GT:  ctor = ALLOC_node_gt; break;
      case TK_GE:  ctor = ALLOC_node_ge; break;
      default: return lhs;
    }
    take(L);
    return ctor(lhs, parse_addsub(L));
}

static struct Node *
parse_and(lexer_t *L)
{
    struct Node *lhs = parse_compare(L);
    while (accept(L, TK_KW_AND)) lhs = ALLOC_node_and(lhs, parse_compare(L));
    return lhs;
}

static struct Node *
parse_or(lexer_t *L)
{
    struct Node *lhs = parse_and(L);
    while (accept(L, TK_KW_OR)) lhs = ALLOC_node_or(lhs, parse_and(L));
    return lhs;
}

static struct Node *
parse_alt(lexer_t *L)
{
    struct Node *lhs = parse_or(L);
    while (accept(L, TK_ALT)) lhs = ALLOC_node_alt(lhs, parse_or(L));
    return lhs;
}

/* `LHS as PAT | body` — `as` sits between `|` and `,` in precedence,
 * so it binds tighter than `,` and looser than arithmetic / `=` / etc.
 * The body of `as` is parsed at full pipe precedence, so the chain
 * `a as $x | b as $y | c` builds a right-leaning AS tree.
 *
 * `LHS as PAT1 ?// PAT2 ?// ... | body` — alternative destructuring.
 * Each alternative is tried in order; the first whose top-level
 * shape matches the value is used. */
static struct Node *
parse_as_postfix(lexer_t *L, struct Node *lhs)
{
    if (peek(L)->type != TK_KW_AS) return lhs;
    take(L);
    /* Try the fast path `$x` (no destructuring, no alternation) before
     * falling back to the generic pattern parser. */
    if (peek(L)->type == TK_DOLLAR) {
        lexer_t snap = *L;
        take(L);
        if (is_name_tok(peek(L))) {
            const char *name = take(L).s;
            if (peek(L)->type == TK_PIPE) {
                uint32_t vid = nuq_intern(name);
                take(L);                          /* consume | */
                struct Node *body = parse_pipe(L);
                return ALLOC_node_as(lhs, vid, body);
            }
        }
        *L = snap;
    }
    struct nuq_pat *pat = parse_pattern(L);
    /* Look for `?//` (TK_QUEST followed by TK_ALT). */
    uint32_t pids[16];
    size_t pcnt = 0;
    pids[pcnt++] = nuq_pat_intern(pat);
    while (peek(L)->type == TK_QUEST) {
        lexer_t snap = *L;
        take(L);                              /* consume `?` */
        if (peek(L)->type != TK_ALT) { *L = snap; break; }
        take(L);                              /* consume `//` */
        if (pcnt >= 16) parse_error(L, "too many ?// alternatives");
        struct nuq_pat *p2 = parse_pattern(L);
        pids[pcnt++] = nuq_pat_intern(p2);
    }
    expect(L, TK_PIPE, "'|' after as PAT");
    struct Node *body = parse_pipe(L);
    if (pcnt == 1) return ALLOC_node_as_pattern(lhs, pids[0], body);
    uint32_t alt_id = nuq_pat_alt_intern(pids, pcnt);
    return ALLOC_node_as_alt_pattern(lhs, alt_id, body);
}

/* Assignment level — between comma and alt.  Each `op=` is right-
 * associative single-binding (jq semantics).  Tokens already exist
 * in the lexer (TK_ASSIGN, TK_UPDEQ, ...). */
static struct Node *
parse_assign(lexer_t *L)
{
    struct Node *lhs = parse_alt(L);
    int op_kind = 0;
    if      (accept(L, TK_ASSIGN))  op_kind = NUQ_ASSIGN_PLAIN;
    else if (accept(L, TK_UPDEQ))   op_kind = NUQ_ASSIGN_UPDATE;
    else if (accept(L, TK_PLUSEQ))  op_kind = NUQ_ASSIGN_PLUS;
    else if (accept(L, TK_MINUSEQ)) op_kind = NUQ_ASSIGN_MINUS;
    else if (accept(L, TK_MULEQ))   op_kind = NUQ_ASSIGN_MUL;
    else if (accept(L, TK_DIVEQ))   op_kind = NUQ_ASSIGN_DIV;
    else if (accept(L, TK_MODEQ))   op_kind = NUQ_ASSIGN_MOD;
    else if (accept(L, TK_ALTEQ))   op_kind = NUQ_ASSIGN_ALT;
    else return lhs;
    struct Node *rhs = parse_alt(L);
    return ALLOC_node_assign(lhs, rhs, (uint32_t)op_kind);
}

/* Each comma element can be followed by an `as PAT | body` clause.
 * `as` thus sits between assign (`= |= += ...`) and `,` in precedence:
 * `1 + 2 as $x | -$x` parses as `(1 + 2) as $x | -$x = -3` (jq behavior). */
static struct Node *
parse_assign_with_as(lexer_t *L)
{
    struct Node *lhs = parse_assign(L);
    return parse_as_postfix(L, lhs);
}

static struct Node *
parse_comma(lexer_t *L)
{
    struct Node *lhs = parse_assign_with_as(L);
    while (accept(L, TK_COMMA)) lhs = ALLOC_node_comma(lhs, parse_assign_with_as(L));
    return lhs;
}

/* `const struct NodeKind kind_<name>` symbols live in node_alloc.c
 * without an extern prototype in any header — they're consumed via
 * the head.kind pointer set by ALLOC_*.  We compare against them
 * here to identify node kinds for fusion. */
extern const struct NodeKind kind_node_pipe;
extern const struct NodeKind kind_node_b_map;
extern const struct NodeKind kind_node_b_select;
extern const struct NodeKind kind_node_array;
extern const struct NodeKind kind_node_b_length;
extern const struct NodeKind kind_node_b_add;

/* Parse-time pipe fusion — semantic-preserving rewrites that telescope
 * common stage chains so they don't materialise intermediate streams.
 * Applied at parse time, so both interpreter and AOT see the rewritten
 * tree.  Each rule must preserve:
 *   - emit ordering and multiplicity
 *   - error propagation (c->error timing on the first failing stage)
 *   - side effects on c->input via inner stages (none of these rules
 *     reorder inner-stage side effects; they only telescope adjacent
 *     stages whose output goes directly into the next stage's input). */

static struct Node *nuq_make_pipe(struct Node *lhs, struct Node *rhs);

/* Try a single-pair fusion rule.  Returns the rewritten node, or NULL
 * if no rule matches.  Does NOT recurse — the caller (nuq_make_pipe)
 * handles right-edge descent for left-leaning pipe chains. */
static struct Node *
nuq_try_fuse_pair(struct Node *lhs, struct Node *rhs)
{
    /* Rule 1:  map(F) | map(G)   →   map(F | G)
     * `map(F)` builds [.[] | F] and pipes it to `map(G)` which builds
     * [.[] | G] of THAT — equivalent to flatmap(G, flatmap(F, .[]))
     * which is exactly `map(F | G)` since pipe is flatmap.
     * Multi-emit and error semantics carry through (any error in F
     * aborts before G runs, same as the non-fused chain). */
    if (lhs->head.kind == &kind_node_b_map &&
        rhs->head.kind == &kind_node_b_map) {
        struct Node *inner = nuq_make_pipe(lhs->u.node_b_map.body,
                                           rhs->u.node_b_map.body);
        return ALLOC_node_b_map(inner);
    }

    /* Rule 2:  select(F) | select(G)   →   select(F and G)
     * `select(E)` emits . iff any-truthy(E).  Chaining gives
     * any-truthy(F) ∧ any-truthy(G).  `F and G` evaluates F's stream;
     * for each truthy value of F, evaluates G's stream pairwise; emits
     * truthy iff some f-value AND some (corresponding) g-value are
     * truthy.  Since G doesn't depend on a specific f-value, this
     * coincides with any-truthy(F) ∧ any-truthy(G).  `and` short-
     * circuits when F is uniformly falsy, matching the `select |
     * select` short-circuit (G never runs in that case). */
    if (lhs->head.kind == &kind_node_b_select &&
        rhs->head.kind == &kind_node_b_select) {
        struct Node *body = ALLOC_node_and(lhs->u.node_b_select.body,
                                           rhs->u.node_b_select.body);
        return ALLOC_node_b_select(body);
    }

    /* Rule 3:  [body] | length   →   emit_count(body)
     * `[E]` always emits an array; `length` of an array is its size,
     * which equals the count of E's emits.  The intermediate array is
     * unobservable, so we replace the build-then-measure with a count.
     * Errors in body still abort before length would have run. */
    if (lhs->head.kind == &kind_node_array &&
        rhs->head.kind == &kind_node_b_length) {
        return ALLOC_node_emit_count(lhs->u.node_array.body);
    }

    /* Rule 4:  [body] | add   →   emit_fold_add(body)
     * `add` on an array dispatches by element type (all arrays / all
     * strings / all objects / pairwise).  Folding directly over body's
     * pool emits saves the outer array allocation; the dispatch logic
     * is shared with `nuq_builtin_add` (called via a thin wrapper).
     * Empty input still returns null (matches `add` on []). */
    if (lhs->head.kind == &kind_node_array &&
        rhs->head.kind == &kind_node_b_add) {
        return ALLOC_node_emit_fold_add(lhs->u.node_array.body);
    }

    return NULL;
}

static struct Node *
nuq_make_pipe(struct Node *lhs, struct Node *rhs)
{
    /* Direct fusion at this pair. */
    struct Node *fused = nuq_try_fuse_pair(lhs, rhs);
    if (fused) return fused;

    /* Right-edge fusion: parsing is left-associative, so
     * `f | g | h` becomes `pipe(pipe(f, g), h)`.  For rules that look
     * at adjacent stages (e.g. `select | select`), the second `g | h`
     * pair is hidden inside the lhs pipe.  Try fusing rhs against the
     * RHS of lhs's outermost pipe; if it succeeds, splice the result
     * back in as the new RHS, leaving lhs's LHS intact.  This makes
     * chains of arbitrary length collapse left-to-right one stage at
     * a time, e.g.:
     *   f | sel(a) | sel(b) | sel(c)
     *     → pipe(pipe(pipe(f, sel(a)), sel(b)), sel(c))
     *     → pipe(pipe(f, sel(a)), sel(b and c))             [fuse step 4]
     *     → pipe(f, sel(a and (b and c)))                    [fuse step 5]
     * (Each step is invoked once via the parser's while loop; we
     * recurse here only into the immediate lhs.rhs, not deeper, since
     * earlier fusion steps already collapsed deeper chains.) */
    if (lhs->head.kind == &kind_node_pipe) {
        struct Node *inner_rhs = lhs->u.node_pipe.rhs;
        struct Node *inner_fused = nuq_try_fuse_pair(inner_rhs, rhs);
        if (inner_fused) {
            return ALLOC_node_pipe(lhs->u.node_pipe.lhs, inner_fused);
        }
    }

    return ALLOC_node_pipe(lhs, rhs);
}

static struct Node *
parse_pipe(lexer_t *L)
{
    struct Node *lhs = parse_comma(L);
    while (accept(L, TK_PIPE)) lhs = nuq_make_pipe(lhs, parse_comma(L));
    return lhs;
}

static struct Node *
parse_pipe_no_comma(lexer_t *L)
{
    struct Node *lhs = parse_assign(L);
    while (accept(L, TK_PIPE)) lhs = nuq_make_pipe(lhs, parse_assign(L));
    return lhs;
}

/* ----- module loader -------------------------------------------------- */

/* Each loaded module gets a unique numeric ns id; defs from that
 * module are renamed to `<ns>::<def_name>` at parse time so the flat
 * def table can hold every loaded module's defs without name clashes.
 *
 * `import "X" as foo;`  records foo → ns(X) in the parser context,
 * and the parser rewrites `foo::a` to `<ns(X)>::a` before emitting
 * the call node.
 *
 * `include "X";` makes X's defs visible by their bare name in the
 * caller's scope — we register each X def under TWO names (qualified
 * and bare) when emitting the unified def block.
 *
 * `import "X" as $var;` reads X.json (or X/X.json) as data and binds
 * it to $var via the same mechanism as --argjson.
 *
 * The `modulemeta` builtin reads its input as a module relpath, loads
 * the module's metadata + dep list (without injecting its defs) and
 * returns the resulting JSON object. */

#include <sys/stat.h>

struct mod_alias { uint32_t alias_id; int ns_id; };
struct mod_local_def { uint32_t name_id; int arity; };
struct mod_dep_record {
    char *relpath;            /* as written */
    char *as_name;            /* alias / var name (no $) */
    char *search;             /* search override from `{search: "..."}` */
    bool  is_data;            /* `as $var` form */
    int   ns_id;              /* of the loaded dep, -1 for data */
};

struct nuq_module {
    char *abs_path;           /* canonical resolved path */
    char *relpath;            /* as the user requested it */
    int   ns_id;
    VALUE meta;               /* `module {meta};` value, or NUQ_NULL */
    struct mod_dep_record *deps;
    size_t dep_cnt;
    /* Original def names + arities for modulemeta `defs` listing. */
    struct mod_local_def *defs;
    size_t def_cnt;
    /* Data import: parsed JSON value, no defs. */
    bool   is_data;
    VALUE  data_value;
};

#define MAX_MODULES 256
static struct nuq_module *module_cache[MAX_MODULES];
static size_t module_cache_cnt = 0;
static int next_ns_id = 0;

/* Defs aggregated across all loaded modules, prepended to the user's
 * def chain at the top of nuq_parse_filter. */
static struct nuq_def_entry *loaded_defs = NULL;
static size_t loaded_def_cnt = 0;
static size_t loaded_def_capa = 0;

/* Per-module / per-top-level parse context.  Always non-NULL while
 * parsing — `current_pctx` points at the active one. */
struct parse_ctx {
    int my_ns_id;             /* -1 for top-level (user filter) */
    struct mod_alias aliases[32];
    int alias_cnt;
    /* ns ids of modules whose defs are visible without prefix. */
    int includes[16];
    int include_cnt;
    /* Local def names declared in this module / top-level (pre-scanned). */
    struct mod_local_def locals[256];
    int local_cnt;
};

static struct parse_ctx *current_pctx = NULL;

static void
loaded_defs_push(struct nuq_def_entry e)
{
    if (loaded_def_cnt == loaded_def_capa) {
        loaded_def_capa = loaded_def_capa ? loaded_def_capa * 2 : 16;
        loaded_defs = (struct nuq_def_entry *)
            GC_realloc(loaded_defs, loaded_def_capa * sizeof(*loaded_defs));
    }
    loaded_defs[loaded_def_cnt++] = e;
}

/* Build a qualified def name "<ns>::<base>" using the GC heap. */
static const char *
qualify_name(int ns_id, const char *base)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%d::%s", ns_id, base);
    size_t n = strlen(buf);
    char *r = (char *)GC_malloc_atomic(n + 1);
    memcpy(r, buf, n + 1);
    return r;
}

/* Read a whole file into a freshly-allocated NUL-terminated buffer. */
static char *
read_file_all(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)GC_malloc(sz + 1);
    size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* Resolve a module relpath against the search list.  Tries:
 *   <dir>/<rel>.jq          (single-file form)
 *   <dir>/<rel>/<basename>.jq (directory form, jq's preferred layout)
 * for non-data, and `.json` for data imports.  `search_override` (from
 * `{search: "..."}` meta) is tried first if non-NULL.  Returns a
 * GC-allocated absolute path, or NULL if not found. */
static char *
resolve_module_path(const char *relpath, const char *search_override,
                    bool is_data, const char *anchor_dir)
{
    const char *ext = is_data ? ".json" : ".jq";
    /* basename of relpath, used for directory-form lookup. */
    const char *bn = relpath;
    for (const char *p = relpath; *p; p++) if (*p == '/') bn = p + 1;

    char buf[1024];
    /* Build a candidate list: search_override → anchor_dir-relative
     * (for searches in a module's `{search: "./../lib/jq"}`-style
     * directives) → -L paths. */
    const char *dirs[16];
    size_t dn = 0;
    if (search_override) {
        /* search may be relative to anchor_dir. */
        if (anchor_dir && search_override[0] != '/') {
            snprintf(buf, sizeof(buf), "%s/%s", anchor_dir, search_override);
            dirs[dn++] = strdup(buf);
        } else {
            dirs[dn++] = search_override;
        }
    }
    if (OPTION.module_search) {
        for (size_t i = 0; i < OPTION.module_search_cnt && dn < 16; i++)
            dirs[dn++] = OPTION.module_search[i];
    }
    /* Always allow CWD as a final fallback (jq does too). */
    if (dn < 16) dirs[dn++] = ".";

    for (size_t i = 0; i < dn; i++) {
        const char *d = dirs[i];
        struct stat st;
        /* Single-file form */
        snprintf(buf, sizeof(buf), "%s/%s%s", d, relpath, ext);
        if (stat(buf, &st) == 0) {
            char *r = (char *)GC_malloc_atomic(strlen(buf) + 1);
            strcpy(r, buf);
            return r;
        }
        /* Directory form */
        snprintf(buf, sizeof(buf), "%s/%s/%s%s", d, relpath, bn, ext);
        if (stat(buf, &st) == 0) {
            char *r = (char *)GC_malloc_atomic(strlen(buf) + 1);
            strcpy(r, buf);
            return r;
        }
    }
    return NULL;
}

static char *
dirname_dup(const char *path)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    if (!slash) {
        char *r = (char *)GC_malloc_atomic(2);
        r[0] = '.'; r[1] = '\0';
        return r;
    }
    size_t n = (size_t)(slash - path);
    char *r = (char *)GC_malloc_atomic(n + 1);
    memcpy(r, path, n);
    r[n] = '\0';
    return r;
}

/* Forward decls for module-load path. */
static struct nuq_module *parse_module_src(const char *src, size_t len,
                                           const char *abs_path,
                                           const char *relpath);

static struct nuq_module *
nuq_module_lookup_cache(const char *abs_path)
{
    for (size_t i = 0; i < module_cache_cnt; i++)
        if (strcmp(module_cache[i]->abs_path, abs_path) == 0)
            return module_cache[i];
    return NULL;
}

/* Load a module by relpath, recursing through its imports.  Returns
 * the cached module on a hit, otherwise parses the file fresh.  For
 * data imports the returned module has `is_data = true` and
 * `data_value` populated. */
static struct nuq_module *
nuq_module_load(const char *relpath, const char *search_override,
                bool is_data, const char *anchor_dir)
{
    char *abs = resolve_module_path(relpath, search_override, is_data,
                                    anchor_dir);
    if (!abs) return NULL;
    struct nuq_module *cached = nuq_module_lookup_cache(abs);
    if (cached) return cached;

    size_t len = 0;
    char *src = read_file_all(abs, &len);
    if (!src) return NULL;

    if (is_data) {
        /* JSON data import. */
        char *err = NULL;
        const char *endp;
        VALUE v = nuq_json_parse(src, len, &endp, &err);
        struct nuq_module *m = (struct nuq_module *)GC_malloc(sizeof(*m));
        memset(m, 0, sizeof(*m));
        m->abs_path = abs;
        m->relpath = strdup(relpath);
        m->ns_id = -1;
        m->meta = NUQ_NULL;
        m->is_data = true;
        m->data_value = err ? NUQ_NULL : v;
        if (module_cache_cnt < MAX_MODULES) module_cache[module_cache_cnt++] = m;
        return m;
    }

    struct nuq_module *m = parse_module_src(src, len, abs, relpath);
    if (m && module_cache_cnt < MAX_MODULES) module_cache[module_cache_cnt++] = m;
    return m;
}

/* Pre-scan a `def`-chain to collect (name, arity) pairs without
 * building AST.  Skips bodies by token-level brace/paren tracking.
 * Restores lexer position before returning so the real parse can
 * proceed identically. */
static void
prescan_local_defs(lexer_t *L, struct mod_local_def *out, int *out_cnt,
                   int max_cnt)
{
    /* Save lexer state. */
    const char *save_p = L->p;
    bool save_peeked = L->peeked;
    token_t save_tok = L->tok;
    int cnt = 0;
    while (peek(L)->type == TK_KW_DEF) {
        take(L);
        const token_t *tn = peek(L);
        if (!is_name_tok(tn)) break;
        const char *name = take(L).s;
        int arity = 0;
        if (accept(L, TK_LP)) {
            for (;;) {
                if (accept(L, TK_DOLLAR)) { (void)take(L); }
                else if (is_name_tok(peek(L))) (void)take(L);
                else break;
                arity++;
                if (!accept(L, TK_SEMI)) break;
            }
            (void)accept(L, TK_RP);
        }
        if (cnt < max_cnt) {
            out[cnt].name_id = nuq_intern(name);
            out[cnt].arity = arity;
            cnt++;
        }
        (void)accept(L, TK_COLON);
        /* Skip body until the matching SEMI at depth 0. */
        int depth = 0;
        while (peek(L)->type != TK_END) {
            const token_t *t = peek(L);
            if (t->type == TK_LP || t->type == TK_LBRK || t->type == TK_LBRACE)
                depth++;
            else if (t->type == TK_RP || t->type == TK_RBRK || t->type == TK_RBRACE)
                depth--;
            else if (depth == 0 && t->type == TK_SEMI) { take(L); break; }
            take(L);
        }
    }
    *out_cnt = cnt;
    /* Restore. */
    L->p = save_p;
    L->peeked = save_peeked;
    L->tok = save_tok;
}

/* Parse a constant-only meta object: `{key: value, ...}`.  We only
 * support string-valued keys and plain literal values (number / string
 * / bool / null) — enough for jq's `module {whatever};` and the
 * `{search:"./", as: "d"}` import options. */
static VALUE
parse_meta_const(lexer_t *L)
{
    if (!accept(L, TK_LBRACE)) parse_error(L, "expected meta object");
    VALUE obj = nuq_make_object(4);
    if (accept(L, TK_RBRACE)) return obj;
    for (;;) {
        const token_t *kt = peek(L);
        const char *key = NULL;
        if (kt->type == TK_STR || kt->type == TK_IDENT || is_name_tok(kt)) {
            key = take(L).s;
        } else parse_error(L, "expected meta key");
        VALUE val = NUQ_NULL;
        if (accept(L, TK_COLON)) {
            const token_t *vt = peek(L);
            if (vt->type == TK_STR) {
                token_t v = take(L);
                val = nuq_make_string(v.s, v.slen);
            } else if (vt->type == TK_INT) {
                val = nuq_make_int(take(L).i);
            } else if (vt->type == TK_NUM) {
                val = nuq_make_double(take(L).d);
            } else if (vt->type == TK_KW_TRUE) {
                take(L); val = NUQ_TRUE;
            } else if (vt->type == TK_KW_FALSE) {
                take(L); val = NUQ_FALSE;
            } else if (vt->type == TK_KW_NULL) {
                take(L); val = NUQ_NULL;
            } else {
                /* Unsupported value form — bail with a generic placeholder. */
                val = NUQ_NULL;
                /* swallow one token to avoid infinite loop */
                if (vt->type != TK_RBRACE && vt->type != TK_COMMA) take(L);
            }
        }
        nuq_object_set_cstr(obj, key, val);
        if (!accept(L, TK_COMMA)) break;
    }
    expect(L, TK_RBRACE, "'}' in meta");
    return obj;
}

/* Parse top-level directives (`module`, `import`, `include`) into the
 * supplied module record (or, for top-level user filters, into a
 * synthetic record so we can still record deps for modulemeta).
 * Side-effect: appends loaded modules' renamed defs to `loaded_defs`,
 * updates `pctx`'s alias / include / locals tables. */
static void
parse_directives(lexer_t *L, struct parse_ctx *pctx, const char *anchor_dir,
                 struct nuq_module *self_mod)
{
    /* `module {meta};` — only valid at the start of a module file. */
    if (peek(L)->type == TK_KW_MODULE) {
        take(L);
        VALUE meta = parse_meta_const(L);
        expect(L, TK_SEMI, "';' after module meta");
        if (self_mod) self_mod->meta = meta;
    }
    while (peek(L)->type == TK_KW_IMPORT || peek(L)->type == TK_KW_INCLUDE) {
        bool is_include = (peek(L)->type == TK_KW_INCLUDE);
        take(L);
        if (peek(L)->type != TK_STR) parse_error(L, "expected string after import/include");
        token_t pt = take(L);
        const char *relpath = pt.s;
        bool is_data = false;
        const char *as_name = NULL;
        if (!is_include && accept(L, TK_KW_AS)) {
            if (accept(L, TK_DOLLAR)) {
                is_data = true;
                if (!is_name_tok(peek(L))) parse_error(L, "expected $name");
                as_name = take(L).s;
            } else {
                if (!is_name_tok(peek(L))) parse_error(L, "expected import alias");
                as_name = take(L).s;
            }
        }
        /* Optional metadata block. */
        VALUE imp_meta = NUQ_NULL;
        const char *search_override = NULL;
        if (peek(L)->type == TK_LBRACE) {
            imp_meta = parse_meta_const(L);
            VALUE sv = nuq_object_get_cstr(imp_meta, "search");
            if (NUQ_IS_PTR(sv) && NUQ_PTR(sv)->type == NUQ_T_STRING) {
                struct nuq_obj *so = NUQ_PTR(sv);
                char *s = (char *)GC_malloc_atomic(so->str.len + 1);
                memcpy(s, so->str.bytes, so->str.len);
                s[so->str.len] = 0;
                search_override = s;
            }
        }
        (void)imp_meta;
        expect(L, TK_SEMI, "';' after import/include");

        /* Record dependency for modulemeta. */
        if (self_mod) {
            self_mod->deps = (struct mod_dep_record *)GC_realloc(
                self_mod->deps,
                (self_mod->dep_cnt + 1) * sizeof(self_mod->deps[0]));
            struct mod_dep_record *d = &self_mod->deps[self_mod->dep_cnt++];
            d->relpath = strdup(relpath);
            d->as_name = as_name ? strdup(as_name) : NULL;
            d->search = search_override ? strdup(search_override) : NULL;
            d->is_data = is_data;
            d->ns_id = -1;
        }

        struct nuq_module *dep = nuq_module_load(relpath, search_override,
                                                 is_data, anchor_dir);
        if (!dep) {
            parse_error(L, "module not found: %s", relpath);
        }
        if (self_mod && self_mod->dep_cnt > 0)
            self_mod->deps[self_mod->dep_cnt - 1].ns_id = dep->ns_id;

        if (is_data) {
            /* Bind data value to the named variable at runtime via
             * --argjson-style mechanism.  Do this only at the top
             * level (when the user's filter runs) — modules don't
             * have separate variable scopes.  Also bind `name::name`
             * (jq exposes data imports as both `$name` and `$name::name`
             * so qualified accesses like `$d::d[].this` work). */
            extern void nuq_user_arg_add_value(const char *name, VALUE v);
            nuq_user_arg_add_value(as_name, dep->data_value);
            char qbuf[128];
            snprintf(qbuf, sizeof(qbuf), "%s::%s", as_name, as_name);
            nuq_user_arg_add_value(qbuf, dep->data_value);
        } else if (is_include) {
            /* Include's defs are visible by their bare name in this
             * scope.  Track its ns_id so unqualified-name resolution
             * can find them. */
            if (pctx->include_cnt < 16)
                pctx->includes[pctx->include_cnt++] = dep->ns_id;
        } else {
            /* import "X" as foo;  → foo → dep ns. */
            if (as_name && pctx->alias_cnt < 32) {
                pctx->aliases[pctx->alias_cnt].alias_id = nuq_intern(as_name);
                pctx->aliases[pctx->alias_cnt].ns_id = dep->ns_id;
                pctx->alias_cnt++;
            }
        }
    }
}

/* Parse a module file: `module ...?; import/include ...; <defs>`.
 * No filter body — modules contribute defs only.  The defs are
 * registered into the global `loaded_defs` table with namespace-
 * prefixed names. */
static struct nuq_module *
parse_module_src(const char *src, size_t len, const char *abs_path,
                 const char *relpath)
{
    struct nuq_module *mod = (struct nuq_module *)GC_malloc(sizeof(*mod));
    memset(mod, 0, sizeof(*mod));
    mod->abs_path = abs_path ? strdup(abs_path) : NULL;
    mod->relpath = relpath ? strdup(relpath) : NULL;
    mod->ns_id = next_ns_id++;
    mod->meta = NUQ_NULL;
    /* Pre-register so circular imports terminate. */
    if (module_cache_cnt < MAX_MODULES) module_cache[module_cache_cnt++] = mod;

    char *anchor_dir = abs_path ? dirname_dup(abs_path) : strdup(".");

    lexer_t L = {0};
    L.src = src;
    L.p = src;
    L.end = src + len;
    L.peeked = false;

    struct parse_ctx pctx = {0};
    pctx.my_ns_id = mod->ns_id;

    /* Save and swap parser context. */
    struct parse_ctx *outer = current_pctx;
    current_pctx = &pctx;

    parse_directives(&L, &pctx, anchor_dir, mod);

    /* Pre-scan local defs so body-pass call resolution can rewrite
     * bare names that refer to this module's own defs. */
    prescan_local_defs(&L, pctx.locals, &pctx.local_cnt, 256);
    /* Snapshot before parser consumes them. */
    mod->def_cnt = pctx.local_cnt;
    mod->defs = (struct mod_local_def *)GC_malloc(
        mod->def_cnt * sizeof(*mod->defs));
    memcpy(mod->defs, pctx.locals, mod->def_cnt * sizeof(*mod->defs));

    /* Parse the def chain (no filter body in a module). */
    while (peek(&L)->type == TK_KW_DEF) {
        take(&L);
        if (!is_name_tok(peek(&L))) parse_error(&L, "def name");
        const char *name = take(&L).s;
        int arity = 0;
        uint32_t pids[16];
        bool pis_val[16];
        if (accept(&L, TK_LP)) {
            for (;;) {
                if (arity >= 16) parse_error(&L, "too many params (max 16)");
                if (accept(&L, TK_DOLLAR)) {
                    if (!is_name_tok(peek(&L))) parse_error(&L, "$name");
                    pids[arity] = nuq_intern(take(&L).s);
                    pis_val[arity] = true;
                } else {
                    if (!is_name_tok(peek(&L))) parse_error(&L, "param");
                    pids[arity] = nuq_intern(take(&L).s);
                    pis_val[arity] = false;
                }
                arity++;
                if (!accept(&L, TK_SEMI)) break;
            }
            expect(&L, TK_RP, "')'");
        }
        expect(&L, TK_COLON, "':'");
        struct Node *body = parse_pipe(&L);
        expect(&L, TK_SEMI, "';' after def body");

        struct nuq_def_entry e = {0};
        e.name_id = nuq_intern(qualify_name(mod->ns_id, name));
        e.arity = arity;
        e.param_ids = (uint32_t *)GC_malloc(arity * sizeof(uint32_t));
        e.param_is_value = (bool *)GC_malloc(arity * sizeof(bool));
        for (int i = 0; i < arity; i++) {
            e.param_ids[i] = pids[i];
            e.param_is_value[i] = pis_val[i];
        }
        e.body = body;
        loaded_defs_push(e);
    }
    if (peek(&L)->type != TK_END)
        parse_error(&L, "unexpected token in module after defs");

    current_pctx = outer;
    return mod;
}

/* Resolve a name + optional namespace alias into a (possibly renamed)
 * call name.  Returns the original name string when nothing matches
 * (no module def, no namespace alias). */
static const char *
resolve_call_name(struct parse_ctx *pctx, const char *alias, const char *name,
                  int arity)
{
    if (alias) {
        if (!pctx) return name;
        uint32_t aid = nuq_intern(alias);
        /* Reverse scan so a later `import "X" as foo;` shadows an
         * earlier one (jq semantics — last binding wins). */
        for (int i = pctx->alias_cnt - 1; i >= 0; i--) {
            if (pctx->aliases[i].alias_id == aid)
                return qualify_name(pctx->aliases[i].ns_id, name);
        }
        /* unresolved alias — keep raw, runtime will report error */
        char buf[160];
        snprintf(buf, sizeof(buf), "%s::%s", alias, name);
        size_t n = strlen(buf);
        char *r = (char *)GC_malloc_atomic(n + 1);
        memcpy(r, buf, n + 1);
        return r;
    }
    if (!pctx) return name;
    /* Bare name: prefer this module's own def if it matches. */
    uint32_t nid = nuq_intern(name);
    for (int i = 0; i < pctx->local_cnt; i++) {
        if (pctx->locals[i].name_id == nid &&
            pctx->locals[i].arity == arity) {
            if (pctx->my_ns_id < 0) return name;   /* top-level user def */
            return qualify_name(pctx->my_ns_id, name);
        }
    }
    /* Then included modules: scan in REVERSE order so a later include
     * shadows an earlier one (`include "shadow1"; include "shadow2";`
     * → shadow2's `e` wins, matching jq's semantics). */
    for (int i = pctx->include_cnt - 1; i >= 0; i--) {
        int ns = pctx->includes[i];
        char qbuf[160];
        snprintf(qbuf, sizeof(qbuf), "%d::%s", ns, name);
        uint32_t qid = nuq_intern(qbuf);
        for (size_t j = 0; j < loaded_def_cnt; j++) {
            if (loaded_defs[j].name_id == qid &&
                loaded_defs[j].arity == arity) {
                return qualify_name(ns, name);
            }
        }
    }
    return name;
}

/* Used by parse_primary to consult the active parse_ctx. */
const char *
nuq_resolve_call_name(const char *alias, const char *name, int arity)
{
    return resolve_call_name(current_pctx, alias, name, arity);
}

/* ----- modulemeta: metadata-only inspector --------------------------- */

/* Build a JSON object describing the module at `relpath`:
 *   {<meta>..., "deps":[{relpath, as, is_data, search?}, ...],
 *               "defs":["name/arity", ...]}
 * Used by the `modulemeta` builtin.  Reads + lex-scans the module file
 * for directive structure and def names, but does NOT recursively
 * load deps (modulemeta reports them, doesn't follow them). */
VALUE
nuq_modulemeta(VALUE input)
{
    if (!(NUQ_IS_PTR(input) && NUQ_PTR(input)->type == NUQ_T_STRING)) {
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
            nuq_active_ctx->error = nuq_make_string(
                "modulemeta input must be a string", 33);
        return NUQ_NULL;
    }
    struct nuq_obj *so = NUQ_PTR(input);
    char relpath[256];
    size_t rl = so->str.len < sizeof(relpath) - 1 ? so->str.len : sizeof(relpath) - 1;
    memcpy(relpath, so->str.bytes, rl);
    relpath[rl] = 0;

    char *abs = resolve_module_path(relpath, NULL, false, NULL);
    if (!abs) {
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL) {
            char buf[300];
            snprintf(buf, sizeof(buf), "module not found: %s", relpath);
            nuq_active_ctx->error = nuq_make_string(buf, strlen(buf));
        }
        return NUQ_NULL;
    }
    size_t flen = 0;
    char *src = read_file_all(abs, &flen);
    if (!src) {
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
            nuq_active_ctx->error = nuq_make_string("read failed", 11);
        return NUQ_NULL;
    }

    lexer_t L = {0};
    L.src = src; L.p = src; L.end = src + flen; L.peeked = false;

    /* Collect deps + meta + defs. */
    VALUE meta = NUQ_NULL;
    VALUE deps = nuq_make_array(0);
    VALUE defs = nuq_make_array(0);

    /* `module {meta};` */
    if (peek(&L)->type == TK_KW_MODULE) {
        take(&L);
        meta = parse_meta_const(&L);
        (void)accept(&L, TK_SEMI);
    }
    /* import / include directives */
    while (peek(&L)->type == TK_KW_IMPORT || peek(&L)->type == TK_KW_INCLUDE) {
        bool is_include = (peek(&L)->type == TK_KW_INCLUDE);
        take(&L);
        if (peek(&L)->type != TK_STR) break;
        token_t pt = take(&L);
        const char *rel = pt.s;
        bool is_data = false;
        const char *as_name = NULL;
        if (!is_include && accept(&L, TK_KW_AS)) {
            if (accept(&L, TK_DOLLAR)) {
                is_data = true;
                if (is_name_tok(peek(&L))) as_name = take(&L).s;
            } else if (is_name_tok(peek(&L))) {
                as_name = take(&L).s;
            }
        }
        const char *search = NULL;
        if (peek(&L)->type == TK_LBRACE) {
            VALUE m = parse_meta_const(&L);
            VALUE sv = nuq_object_get_cstr(m, "search");
            if (NUQ_IS_PTR(sv) && NUQ_PTR(sv)->type == NUQ_T_STRING) {
                struct nuq_obj *sso = NUQ_PTR(sv);
                char *s = (char *)GC_malloc_atomic(sso->str.len + 1);
                memcpy(s, sso->str.bytes, sso->str.len);
                s[sso->str.len] = 0;
                search = s;
            }
        }
        (void)accept(&L, TK_SEMI);

        VALUE d = nuq_make_object(4);
        if (search) nuq_object_set_cstr(d, "search",
            nuq_make_string(search, strlen(search)));
        if (as_name) nuq_object_set_cstr(d, "as",
            nuq_make_string(as_name, strlen(as_name)));
        nuq_object_set_cstr(d, "is_data", is_data ? NUQ_TRUE : NUQ_FALSE);
        nuq_object_set_cstr(d, "relpath",
            nuq_make_string(rel, strlen(rel)));
        nuq_array_push(deps, d);
    }
    /* defs scan */
    struct mod_local_def locals[256];
    int local_cnt = 0;
    prescan_local_defs(&L, locals, &local_cnt, 256);
    for (int i = 0; i < local_cnt; i++) {
        const char *n = nuq_intern_lookup(locals[i].name_id);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s/%d", n, locals[i].arity);
        nuq_array_push(defs, nuq_make_string(buf, strlen(buf)));
    }

    /* Build the result: meta keys + deps + defs. */
    VALUE r = nuq_make_object(8);
    if (NUQ_IS_PTR(meta) && NUQ_PTR(meta)->type == NUQ_T_OBJECT) {
        struct nuq_obj *mo = NUQ_PTR(meta);
        for (size_t i = 0; i < mo->obj.len; i++)
            nuq_object_set(r, mo->obj.keys[i], mo->obj.vals[i]);
    }
    nuq_object_set_cstr(r, "deps", deps);
    nuq_object_set_cstr(r, "defs", defs);
    return r;
}

/* ----- entry points ---------------------------------------------------- */

/* Stdlib prelude — jq-compatible defs that wrap the user's filter.
 * User-level `def name: ...;` shadows these (lookup walks innermost-first).
 * Keep this list small and avoid recursion that would loop without
 * tail-call optimization.  Items here unblock direct jq compatibility
 * for builtins that jq itself implements as defs in its stdlib. */
static const char *nuq_prelude =
    "def add(f): reduce f as $x (null; . + $x); "
    "def pick(f): . as $v | reduce path(f) as $p (null; setpath($p; $v|getpath($p))); "
    "def IN(s): any(s == .; .); "
    "def IN(src; s): any(src == s; .); "
    "def INDEX(stream; idx): reduce stream as $row ({}; .[$row|idx|tostring] = $row); "
    "def INDEX(idx): INDEX(.[]; idx); "
    "def JOIN($idx; idx_expr): [.[] | [., $idx[idx_expr]?]]; "
    "def JOIN($idx; stream; idx_expr; join_expr): "
    "    [stream | [., $idx[idx_expr]?] | join_expr]; "
    "def trimstr(s): "
    "    if type == \"string\" and (s|type) == \"string\" "
    "    then ltrimstr(s) | rtrimstr(s) "
    "    else . end; "
    /* jq exposes `have_decnum` to let scripts branch on whether the
     * runtime supports arbitrary-precision decimals.  nuq doesn't, so
     * advertise false and let the scripts take the fallback. */
    "def have_decnum: false; "
    "def have_decimal: false; "
    "def have_literal_numbers: false; "
    ;

struct Node *
nuq_parse_filter(const char *src)
{
    /* Reset module-loader state so successive parses don't bleed defs
     * from an earlier filter (test runners reuse the binary). */
    loaded_def_cnt = 0;
    module_cache_cnt = 0;
    next_ns_id = 0;

    /* User filters can begin with `module {meta}; import "X" as foo;
     * include "X"; ...` directives.  Detect this with a cheap text
     * scan first so we don't burn lexer tokens we'd then have to
     * un-consume — only enter directive parsing when one of the
     * directive keywords appears at the front of the source. */
    struct parse_ctx pctx = {0};
    pctx.my_ns_id = -1;
    struct parse_ctx *outer = current_pctx;
    current_pctx = &pctx;

    const char *user_remaining = src;
    {
        /* Skip leading whitespace + line comments. */
        const char *p = src;
        const char *e = src + strlen(src);
        while (p < e) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            else if (*p == '#') { while (p < e && *p != '\n') p++; }
            else break;
        }
        bool starts_directive =
            (e - p >= 7 && memcmp(p, "module ",  7) == 0) ||
            (e - p >= 7 && memcmp(p, "module\t", 7) == 0) ||
            (e - p >= 7 && memcmp(p, "module{",  7) == 0) ||
            (e - p >= 7 && memcmp(p, "import ",  7) == 0) ||
            (e - p >= 7 && memcmp(p, "import\t", 7) == 0) ||
            (e - p >= 7 && memcmp(p, "import\"", 7) == 0) ||
            (e - p >= 8 && memcmp(p, "include ",  8) == 0) ||
            (e - p >= 8 && memcmp(p, "include\t", 8) == 0) ||
            (e - p >= 8 && memcmp(p, "include\"", 8) == 0);
        if (starts_directive) {
            lexer_t L0 = {0};
            L0.src = src;
            L0.p = src;
            L0.end = e;
            L0.peeked = false;
            parse_directives(&L0, &pctx, ".", NULL);
            /* parse_directives ends with a successfully-consumed `;`.
             * After that, the lexer is positioned past the last `;`
             * and whitespace; the next token (if any) is buffered
             * via peek.  Take peeking into account so the rebuilt
             * source for the second pass starts at the un-consumed
             * filter body. */
            user_remaining = L0.peeked
                ? (L0.tok.type == TK_END ? L0.end : NULL)
                : L0.p;
            if (!user_remaining) {
                /* Peeked token isn't END — find its true start by
                 * walking back to the last whitespace before L0.p. */
                const char *q = L0.p;
                while (q > src) {
                    char c = q[-1];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                        c == ';' || c == '}') break;
                    q--;
                }
                user_remaining = q;
            }
        }
    }
    size_t pl = strlen(nuq_prelude);
    size_t rl = (size_t)((src + strlen(src)) - user_remaining);
    char *buf = (char *)GC_malloc(pl + rl + 1);
    memcpy(buf, nuq_prelude, pl);
    memcpy(buf + pl, user_remaining, rl);
    buf[pl + rl] = 0;
    lexer_t L;
    L.src = buf;
    L.p = buf;
    L.end = buf + pl + rl;
    L.peeked = false;

    /* Pre-scan top-level def names so user calls can resolve to them
     * over `include`d module names. */
    prescan_local_defs(&L, pctx.locals, &pctx.local_cnt, 256);

    struct Node *body = parse_pipe(&L);
    if (peek(&L)->type != TK_END) parse_error(&L, "trailing tokens");

    current_pctx = outer;

    /* Wrap the body in a defs block holding every loaded module's
     * defs (with namespace-qualified names) — and also bare-name
     * aliases for any `include`-style imports. */
    if (loaded_def_cnt == 0 && pctx.include_cnt == 0) return body;

    /* Build the final def block: loaded defs + bare-name include
     * aliases.  Each entry is the full nuq_def_entry. */
    size_t total = loaded_def_cnt;
    size_t bare_extra = 0;
    /* Count bare-aliases for include's defs. */
    for (int i = 0; i < pctx.include_cnt; i++) {
        int ns = pctx.includes[i];
        for (size_t j = 0; j < loaded_def_cnt; j++) {
            const char *qn = nuq_intern_lookup(loaded_defs[j].name_id);
            char prefix[32];
            int pn = snprintf(prefix, sizeof(prefix), "%d::", ns);
            if ((int)strlen(qn) > pn && memcmp(qn, prefix, pn) == 0) {
                bare_extra++;
            }
        }
    }
    total += bare_extra;
    struct nuq_def_entry *all = (struct nuq_def_entry *)GC_malloc(
        total * sizeof(*all));
    size_t k = 0;
    for (size_t j = 0; j < loaded_def_cnt; j++) all[k++] = loaded_defs[j];
    for (int i = 0; i < pctx.include_cnt; i++) {
        int ns = pctx.includes[i];
        char prefix[32];
        int pn = snprintf(prefix, sizeof(prefix), "%d::", ns);
        for (size_t j = 0; j < loaded_def_cnt; j++) {
            const char *qn = nuq_intern_lookup(loaded_defs[j].name_id);
            if ((int)strlen(qn) > pn && memcmp(qn, prefix, pn) == 0) {
                struct nuq_def_entry copy = loaded_defs[j];
                copy.name_id = nuq_intern(qn + pn);
                all[k++] = copy;
            }
        }
    }
    return ALLOC_node_defs(nuq_def_block_intern(all, total), body);
}

struct Node *
nuq_compile_subexpr(const char *src, size_t len)
{
    lexer_t L;
    L.src = src;
    L.p = src;
    L.end = src + len;
    L.peeked = false;
    struct Node *r = parse_pipe(&L);
    if (peek(&L)->type != TK_END) parse_error(&L, "trailing tokens in subexpr");
    return r;
}
