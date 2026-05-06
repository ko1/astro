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
    TK_KW_LABEL,
} ttype_t;

typedef struct {
    ttype_t type;
    int64_t i;
    double  d;
    const char *s;
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
        return;
    }

    struct Node **pnodes = (struct Node **)GC_malloc(pcnt * sizeof(*pnodes));
    for (size_t i = 0; i < pcnt; i++) {
        if (parts[i].kind == P_STR) {
            pnodes[i] = ALLOC_node_str(parts[i].text);
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
      case '.': L->p++; if (L->p < L->end && *L->p == '.') { L->p++; L->tok.type = TK_DDOT; return; } L->tok.type = TK_DOT; return;
      case '?': L->p++; L->tok.type = TK_QUEST; return;
      case '(': L->p++; L->tok.type = TK_LP; return;
      case ')': L->p++; L->tok.type = TK_RP; return;
      case '[': L->p++; L->tok.type = TK_LBRK; return;
      case ']': L->p++; L->tok.type = TK_RBRK; return;
      case '{': L->p++; L->tok.type = TK_LBRACE; return;
      case '}': L->p++; L->tok.type = TK_RBRACE; return;
      case ',': L->p++; L->tok.type = TK_COMMA; return;
      case ':': L->p++; L->tok.type = TK_COLON; return;
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
    BUILTIN1("index", ALLOC_node_b_index1);
    BUILTIN1("test", ALLOC_node_b_test);
    BUILTIN1("getpath", ALLOC_node_b_getpath);
    BUILTIN1("delpaths", ALLOC_node_b_delpaths);
    BUILTIN1("del", ALLOC_node_b_del);
    BUILTIN1("error", ALLOC_node_error1);

    /* 2-arg */
    BUILTIN2("range", ALLOC_node_b_range2);
    BUILTIN2("recurse", ALLOC_node_b_recurse2);
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
static struct Node *parse_term_for_keyword(lexer_t *L);
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
        if (tk.i >= INT32_MIN && tk.i <= INT32_MAX) return ALLOC_node_int((int32_t)tk.i);
        return ALLOC_node_lit(nuq_lit_intern(nuq_make_int(tk.i)));
      }
      case TK_NUM: { token_t tk = take(L); return ALLOC_node_lit(nuq_lit_intern(nuq_make_double(tk.d))); }
      case TK_STR: { token_t tk = take(L); return ALLOC_node_str(tk.s); }
      case TK_INTERP: { token_t tk = take(L); return ALLOC_node_interp((uint32_t)tk.i); }
      case TK_AT: {
        const char *name = take(L).s;
        uint32_t fid = nuq_fmt_intern(name);
        const token_t *nx = peek(L);
        if (nx->type == TK_STR) {
            token_t tk = take(L);
            return ALLOC_node_format(fid, ALLOC_node_str(tk.s));
        }
        if (nx->type == TK_INTERP) {
            token_t tk = take(L);
            return ALLOC_node_format(fid, ALLOC_node_interp((uint32_t)tk.i));
        }
        return ALLOC_node_format(fid, NULL);
      }
      case TK_DOLLAR: {
        take(L);
        if (peek(L)->type != TK_IDENT) parse_error(L, "expected $name");
        const char *nm = take(L).s;
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
                    if (peek(L)->type != TK_IDENT) parse_error(L, "$name in obj key");
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
                        ie->kexpr = ALLOC_node_format(nuq_fmt_intern(fnm), ALLOC_node_str(tk.s));
                    } else if (peek(L)->type == TK_INTERP) {
                        token_t tk = take(L);
                        ie->kkind = 1;
                        ie->kexpr = ALLOC_node_format(nuq_fmt_intern(fnm), ALLOC_node_interp((uint32_t)tk.i));
                    } else parse_error(L, "expected string after @%s", fnm);
                } else if (kt->type >= TK_KW_TRUE && kt->type <= TK_KW_LABEL) {
                    static const char *kw_names[] = {
                        [TK_KW_TRUE]="true",[TK_KW_FALSE]="false",[TK_KW_NULL]="null",
                        [TK_KW_IF]="if",[TK_KW_THEN]="then",[TK_KW_ELIF]="elif",
                        [TK_KW_ELSE]="else",[TK_KW_END]="end",[TK_KW_AND]="and",
                        [TK_KW_OR]="or",[TK_KW_NOT]="not",[TK_KW_AS]="as",
                        [TK_KW_DEF]="def",[TK_KW_TRY]="try",[TK_KW_CATCH]="catch",
                        [TK_KW_REDUCE]="reduce",[TK_KW_FOREACH]="foreach",
                        [TK_KW_LABEL]="label"
                    };
                    ie->kkind = 0;
                    ie->kname = kw_names[kt->type] ? kw_names[kt->type] : "?";
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
        struct Node *body = parse_postfix(L);
        struct Node *handler = NULL;
        if (accept(L, TK_KW_CATCH)) handler = parse_postfix(L);
        return ALLOC_node_try(body, handler);
      }
      case TK_KW_REDUCE:
      case TK_KW_FOREACH: {
        bool is_for = (t->type == TK_KW_FOREACH);
        take(L);
        struct Node *src = parse_term_for_keyword(L);
        expect(L, TK_KW_AS, "'as'");
        expect(L, TK_DOLLAR, "'$'");
        if (peek(L)->type != TK_IDENT) parse_error(L, "$name");
        uint32_t var_id = nuq_intern(take(L).s);
        expect(L, TK_LP, "'('");
        struct Node *init = parse_pipe(L);
        expect(L, TK_SEMI, "';'");
        struct Node *update = parse_pipe(L);
        struct Node *extract = NULL;
        if (is_for && accept(L, TK_SEMI)) extract = parse_pipe(L);
        expect(L, TK_RP, "')'");
        if (is_for) {
            /* `foreach SRC as $x (INIT; UPDATE)` (no extract) ≡ extract `.`
             * — always provide a non-NULL extract so the generated
             * dispatcher can deref it. */
            if (extract == NULL) extract = ALLOC_node_identity();
            return ALLOC_node_foreach(src, var_id, init, update, extract);
        }
        return ALLOC_node_reduce(src, var_id, init, update);
      }
      case TK_KW_LABEL: {
        take(L);
        expect(L, TK_DOLLAR, "'$'");
        if (peek(L)->type != TK_IDENT) parse_error(L, "$name");
        uint32_t vid = nuq_intern(take(L).s);
        expect(L, TK_PIPE, "'|' after label");
        struct Node *body = parse_pipe(L);
        return ALLOC_node_label(vid, body);
      }
      case TK_KW_NOT: take(L); return ALLOC_node_not();
      case TK_KW_DEF: {
        struct nuq_def_entry items[64];
        size_t cnt = 0;
        while (peek(L)->type == TK_KW_DEF) {
            take(L);
            if (peek(L)->type != TK_IDENT) parse_error(L, "def name");
            const char *name = take(L).s;
            int arity = 0;
            uint32_t pids[8];
            bool pis_val[8];
            if (accept(L, TK_LP)) {
                for (;;) {
                    if (accept(L, TK_DOLLAR)) {
                        if (peek(L)->type != TK_IDENT) parse_error(L, "$name");
                        pids[arity] = nuq_intern(take(L).s);
                        pis_val[arity] = true;
                    } else {
                        if (peek(L)->type != TK_IDENT) parse_error(L, "param");
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
        struct Node *args[8];
        int arity = 0;
        if (accept(L, TK_LP)) {
            for (;;) {
                args[arity++] = parse_pipe(L);
                if (!accept(L, TK_SEMI)) break;
            }
            expect(L, TK_RP, "')'");
        }
        return build_builtin_call(name, arity, args);
      }
      default: parse_error(L, "unexpected token type %d", t->type);
    }
}

static struct Node *
parse_term_for_keyword(lexer_t *L)
{
    struct Node *acc = parse_primary(L);
    for (;;) {
        const token_t *t = peek(L);
        if (t->type == TK_KW_AS) break;
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
                        acc = ALLOC_node_pipe(acc, ALLOC_node_slice(e1, e2, flags));
                    } else {
                        expect(L, TK_RBRK, "']'");
                        acc = ALLOC_node_pipe(acc, ALLOC_node_index(e1));
                    }
                }
            } else parse_error(L, "expected ident or '['");
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
                    acc = ALLOC_node_pipe(acc, ALLOC_node_slice(e1, e2, flags));
                } else {
                    expect(L, TK_RBRK, "']'");
                    acc = ALLOC_node_pipe(acc, ALLOC_node_index(e1));
                }
            }
        } else if (t->type == TK_QUEST) {
            take(L);
            acc = wrap_quest(acc);
        } else break;
    }
    return acc;
}

/* ----- postfix --------------------------------------------------------- */

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
                        acc = ALLOC_node_pipe(acc, ALLOC_node_slice(e1, e2, flags));
                    } else {
                        expect(L, TK_RBRK, "']'");
                        acc = ALLOC_node_pipe(acc, ALLOC_node_index(e1));
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
                    acc = ALLOC_node_pipe(acc, ALLOC_node_slice(e1, e2, flags));
                } else {
                    expect(L, TK_RBRK, "']'");
                    acc = ALLOC_node_pipe(acc, ALLOC_node_index(e1));
                }
            }
        } else if (t->type == TK_QUEST) {
            take(L);
            acc = wrap_quest(acc);
        } else if (t->type == TK_KW_AS) {
            take(L);
            expect(L, TK_DOLLAR, "'$'");
            if (peek(L)->type != TK_IDENT) parse_error(L, "$name");
            uint32_t vid = nuq_intern(take(L).s);
            expect(L, TK_PIPE, "'|' after as $x");
            struct Node *body = parse_pipe(L);
            return ALLOC_node_as(acc, vid, body);
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

static struct Node *
parse_comma(lexer_t *L)
{
    struct Node *lhs = parse_assign(L);
    while (accept(L, TK_COMMA)) lhs = ALLOC_node_comma(lhs, parse_assign(L));
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

/* ----- entry points ---------------------------------------------------- */

struct Node *
nuq_parse_filter(const char *src)
{
    lexer_t L;
    L.src = src;
    L.p = src;
    L.end = src + strlen(src);
    L.peeked = false;
    struct Node *r = parse_pipe(&L);
    if (peek(&L)->type != TK_END) parse_error(&L, "trailing tokens");
    return r;
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
