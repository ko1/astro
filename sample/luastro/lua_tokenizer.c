// luastro tokenizer — Lua 5.4 lexer.
//
// Hand-written, matches Lua's reference lexer behavior:
//   - identifiers and keywords
//   - integer / float / hex literals (including hex floats with 'p' exponent)
//   - short strings "..." and '...' with escape sequences
//   - long brackets [[...]] / [==[...]==]
//   - comments: -- short, --[[...]] or --[==[...]==] long
//   - all the punctuation Lua uses: + - * / // % ^ # & | ~ << >> == ~= <= >=
//                                    < > = ( ) { } [ ] :: ; : , . .. ...
//
// Public surface:
//   void lua_tok_init(const char *src, const char *filename);
//   void lua_tok_next(void);
//   const Token *lua_tok_cur(void);
//   void lua_tok_error(const char *msg) __attribute__((noreturn));
//
// All static state is module-scoped — luastro is single-threaded.

#include <ctype.h>
#include "context.h"
#include "lua_token.h"

// --- Module state ----------------------------------------------------

static const char *L_src;
static const char *L_pos;
static const char *L_end;
static const char *L_filename = "?";
static int         L_line = 1;
static int         L_col  = 1;

static Token L_cur;
static jmp_buf L_err_jmp;
static int     L_err_active = 0;
static char    L_err_msg[256];

// --- Helpers ---------------------------------------------------------

__attribute__((noreturn))
void
lua_tok_error_at(const char *msg, int line, int col)
{
    snprintf(L_err_msg, sizeof(L_err_msg),
             "%s:%d:%d: lex error: %s", L_filename, line, col, msg);
    if (L_err_active) longjmp(L_err_jmp, 1);
    fprintf(stderr, "%s\n", L_err_msg);
    exit(1);
}

__attribute__((noreturn))
void
lua_tok_error(const char *msg)
{
    lua_tok_error_at(msg, L_line, L_col);
}

static inline int
peek0(void)         { return L_pos < L_end ? (unsigned char)L_pos[0] : -1; }
static inline int
peek1(void)         { return L_pos + 1 < L_end ? (unsigned char)L_pos[1] : -1; }
static inline int
peek2(void)         { return L_pos + 2 < L_end ? (unsigned char)L_pos[2] : -1; }

static inline int
advance(void)
{
    int ch = peek0();
    if (ch < 0) return -1;
    L_pos++;
    if (ch == '\n') { L_line++; L_col = 1; }
    else            { L_col++; }
    return ch;
}

static int
is_ident_start(int ch) { return isalpha(ch) || ch == '_'; }

static int
is_ident_cont(int ch)  { return isalnum(ch) || ch == '_'; }

// --- Long bracket helpers --------------------------------------------
//
// Lua's long brackets: opening is [, optional `=`s, [, and the matching
// close is ], same number of `=`s, ].
//
// Returns -1 if the current position is not a long-bracket open;
// otherwise returns the equals count and advances past the open.

static int
try_long_open(void)
{
    if (peek0() != '[') return -1;
    const char *save = L_pos;
    int save_line = L_line, save_col = L_col;
    advance();
    int level = 0;
    while (peek0() == '=') { advance(); level++; }
    if (peek0() != '[') {
        L_pos = save; L_line = save_line; L_col = save_col;
        return -1;
    }
    advance();
    // First newline immediately after opening is ignored.
    if (peek0() == '\r') advance();
    if (peek0() == '\n') advance();
    return level;
}

// Read until the matching close `]==..]`.  Writes the content (excluding
// the brackets) into a malloc'd buffer if `out`/`out_len` are non-NULL.
static void
read_long(int level, char **out, size_t *out_len)
{
    size_t cap = 64, n = 0;
    char *buf = (char *)malloc(cap);
    while (peek0() >= 0) {
        if (peek0() == ']') {
            const char *save = L_pos;
            int save_line = L_line, save_col = L_col;
            advance();
            int got = 0;
            while (peek0() == '=' && got < level) { advance(); got++; }
            if (got == level && peek0() == ']') {
                advance();
                if (out) { *out = buf; *out_len = n; }
                else free(buf);
                return;
            }
            // Not a match, restore.
            L_pos = save; L_line = save_line; L_col = save_col;
        }
        int ch = advance();
        if (n + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        buf[n++] = (char)ch;
    }
    free(buf);
    lua_tok_error("unterminated long bracket");
}

// --- Whitespace and comments -----------------------------------------

static void
skip_ws_and_comments(void)
{
    for (;;) {
        int ch = peek0();
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { advance(); continue; }
        if (ch == '-' && peek1() == '-') {
            // comment
            advance(); advance();
            int level = (peek0() == '[') ? try_long_open() : -1;
            if (level >= 0) {
                read_long(level, NULL, NULL);
            } else {
                while (peek0() >= 0 && peek0() != '\n') advance();
            }
            continue;
        }
        break;
    }
}

// --- Keyword lookup --------------------------------------------------

static struct { const char *kw; ltok_t kind; } KEYWORDS[] = {
    {"and",      LT_AND},      {"break",    LT_BREAK},
    {"do",       LT_DO},       {"else",     LT_ELSE},
    {"elseif",   LT_ELSEIF},   {"end",      LT_END},
    {"false",    LT_FALSE},    {"for",      LT_FOR},
    {"function", LT_FUNCTION}, {"goto",     LT_GOTO},
    {"if",       LT_IF},       {"in",       LT_IN},
    {"local",    LT_LOCAL},    {"nil",      LT_NIL},
    {"not",      LT_NOT},      {"or",       LT_OR},
    {"repeat",   LT_REPEAT},   {"return",   LT_RETURN},
    {"then",     LT_THEN},     {"true",     LT_TRUE},
    {"until",    LT_UNTIL},    {"while",    LT_WHILE},
    {NULL, 0},
};

static ltok_t
keyword_lookup(const char *s, size_t len)
{
    for (int i = 0; KEYWORDS[i].kw; i++) {
        if (strlen(KEYWORDS[i].kw) == len && memcmp(KEYWORDS[i].kw, s, len) == 0)
            return KEYWORDS[i].kind;
    }
    return LT_IDENT;
}

// --- Number lexing ---------------------------------------------------

static int
hex_digit(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void
lex_number(Token *t)
{
    const char *start = L_pos;
    int line = L_line, col = L_col;
    bool is_float = false;
    bool is_hex   = false;

    if (peek0() == '0' && (peek1() == 'x' || peek1() == 'X')) {
        advance(); advance();
        is_hex = true;
        while (hex_digit(peek0()) >= 0) advance();
        if (peek0() == '.') {
            is_float = true;
            advance();
            while (hex_digit(peek0()) >= 0) advance();
        }
        if (peek0() == 'p' || peek0() == 'P') {
            is_float = true;
            advance();
            if (peek0() == '+' || peek0() == '-') advance();
            while (isdigit(peek0())) advance();
        }
    } else {
        while (isdigit(peek0())) advance();
        if (peek0() == '.') {
            is_float = true;
            advance();
            while (isdigit(peek0())) advance();
        }
        if (peek0() == 'e' || peek0() == 'E') {
            is_float = true;
            advance();
            if (peek0() == '+' || peek0() == '-') advance();
            while (isdigit(peek0())) advance();
        }
    }
    size_t n = L_pos - start;
    char buf[64];
    if (n >= sizeof(buf)) lua_tok_error_at("number literal too long", line, col);
    memcpy(buf, start, n); buf[n] = '\0';
    t->start = start;
    t->len   = n;
    t->line  = line;
    t->col   = col;
    if (is_float) {
        t->kind = LT_FLOAT;
        t->float_value = strtod(buf, NULL);
    } else {
        t->kind = LT_INT;
        if (is_hex) {
            t->int_value = (int64_t)strtoull(buf, NULL, 16);
        } else {
            t->int_value = (int64_t)strtoll(buf, NULL, 10);
        }
    }
}

// --- String lexing ---------------------------------------------------

static int
read_decimal_escape(int first)
{
    int v = first - '0';
    for (int k = 0; k < 2; k++) {
        if (!isdigit(peek0())) break;
        v = v * 10 + (advance() - '0');
    }
    if (v > 255) lua_tok_error("decimal escape > 255");
    return v;
}

static int
read_hex_escape(void)
{
    int a = hex_digit(peek0());
    if (a < 0) lua_tok_error("hex escape needs 2 hex digits");
    advance();
    int b = hex_digit(peek0());
    if (b < 0) lua_tok_error("hex escape needs 2 hex digits");
    advance();
    return (a << 4) | b;
}

static void
lex_short_string(Token *t, int quote)
{
    int line = L_line, col = L_col;
    advance();   // open quote

    size_t cap = 32, n = 0;
    char *buf = (char *)malloc(cap);
    while (peek0() >= 0 && peek0() != quote) {
        int ch = peek0();
        if (ch == '\n') lua_tok_error("unterminated short string");
        if (ch == '\\') {
            advance();
            int e = peek0();
            if (e < 0) lua_tok_error("invalid escape");
            switch (e) {
            case 'a': advance(); ch = '\a'; break;
            case 'b': advance(); ch = '\b'; break;
            case 'f': advance(); ch = '\f'; break;
            case 'n': advance(); ch = '\n'; break;
            case 'r': advance(); ch = '\r'; break;
            case 't': advance(); ch = '\t'; break;
            case 'v': advance(); ch = '\v'; break;
            case '\\': advance(); ch = '\\'; break;
            case '"': advance(); ch = '"'; break;
            case '\'': advance(); ch = '\''; break;
            case '\n': advance(); ch = '\n'; break;
            case 'x': advance(); ch = read_hex_escape(); break;
            case 'z':
                advance();
                while (peek0() == ' ' || peek0() == '\t' || peek0() == '\n' ||
                       peek0() == '\r' || peek0() == '\f' || peek0() == '\v') advance();
                continue;
            default:
                if (isdigit(e)) { advance(); ch = read_decimal_escape(e); break; }
                lua_tok_error("invalid escape sequence");
            }
        } else {
            advance();
        }
        if (n + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        buf[n++] = (char)ch;
    }
    if (peek0() != quote) lua_tok_error("unterminated string");
    advance();
    t->kind      = LT_STRING;
    t->str_value = buf;
    t->str_len   = n;
    t->line      = line;
    t->col       = col;
}

static void
lex_long_string(Token *t)
{
    int line = L_line, col = L_col;
    int level = try_long_open();
    char *buf = NULL;
    size_t n = 0;
    read_long(level, &buf, &n);
    t->kind      = LT_STRING;
    t->str_value = buf;
    t->str_len   = n;
    t->line      = line;
    t->col       = col;
}

// --- Public API ------------------------------------------------------

void
lua_tok_init(const char *src, const char *filename)
{
    L_src      = src;
    L_pos      = src;
    L_end      = src + strlen(src);
    L_filename = filename ? filename : "?";
    L_line     = 1;
    L_col      = 1;
    memset(&L_cur, 0, sizeof(L_cur));
    L_cur.kind = LT_EOF;
}

void
lua_tok_next(void)
{
    if (L_cur.kind == LT_STRING && L_cur.str_value) {
        free(L_cur.str_value);
        L_cur.str_value = NULL;
    }
    skip_ws_and_comments();
    if (peek0() < 0) {
        L_cur.kind = LT_EOF;
        L_cur.start = L_pos; L_cur.len = 0;
        L_cur.line = L_line; L_cur.col = L_col;
        return;
    }
    int line = L_line, col = L_col;
    const char *start = L_pos;
    int ch = peek0();

    // Identifier / keyword
    if (is_ident_start(ch)) {
        advance();
        while (is_ident_cont(peek0())) advance();
        size_t n = L_pos - start;
        L_cur.start = start; L_cur.len = n; L_cur.line = line; L_cur.col = col;
        L_cur.kind  = keyword_lookup(start, n);
        return;
    }

    // Number
    if (isdigit(ch) || (ch == '.' && isdigit(peek1()))) {
        lex_number(&L_cur);
        return;
    }

    // Strings
    if (ch == '"' || ch == '\'') {
        lex_short_string(&L_cur, ch);
        return;
    }
    if (ch == '[' && (peek1() == '[' || peek1() == '=')) {
        const char *save = L_pos;
        int save_line = L_line, save_col = L_col;
        int level = try_long_open();
        if (level >= 0) {
            // Re-position to before the open and re-lex as long string.
            L_pos = save; L_line = save_line; L_col = save_col;
            lex_long_string(&L_cur);
            return;
        }
    }

    // Punctuation
    L_cur.start = start; L_cur.line = line; L_cur.col = col;
#define ONE(K) do { advance(); L_cur.kind = K; L_cur.len = 1; return; } while (0)
#define TWO(K) do { advance(); advance(); L_cur.kind = K; L_cur.len = 2; return; } while (0)
#define THREE(K) do { advance(); advance(); advance(); L_cur.kind = K; L_cur.len = 3; return; } while (0)
    switch (ch) {
    case '(': ONE(LT_LPAREN);
    case ')': ONE(LT_RPAREN);
    case '{': ONE(LT_LBRACE);
    case '}': ONE(LT_RBRACE);
    case '[': ONE(LT_LBRACK);
    case ']': ONE(LT_RBRACK);
    case ',': ONE(LT_COMMA);
    case ';': ONE(LT_SEMI);
    case '+': ONE(LT_PLUS);
    case '-': ONE(LT_MINUS);
    case '*': ONE(LT_STAR);
    case '%': ONE(LT_PERCENT);
    case '^': ONE(LT_CARET);
    case '#': ONE(LT_HASH);
    case '&': ONE(LT_AMP);
    case '|': ONE(LT_PIPE);
    case '~':
        if (peek1() == '=') TWO(LT_NEQ);
        ONE(LT_TILDE);
    case '/':
        if (peek1() == '/') TWO(LT_DSLASH);
        ONE(LT_SLASH);
    case ':':
        if (peek1() == ':') TWO(LT_DBLCOLON);
        ONE(LT_COLON);
    case '<':
        if (peek1() == '=') TWO(LT_LE);
        if (peek1() == '<') TWO(LT_LSHIFT);
        ONE(LT_LT);
    case '>':
        if (peek1() == '=') TWO(LT_GE);
        if (peek1() == '>') TWO(LT_RSHIFT);
        ONE(LT_GT);
    case '=':
        if (peek1() == '=') TWO(LT_EQ);
        ONE(LT_ASSIGN);
    case '.':
        if (peek1() == '.') {
            if (peek2() == '.') THREE(LT_ELLIPSIS);
            TWO(LT_DOTDOT);
        }
        ONE(LT_DOT);
    }
#undef ONE
#undef TWO
#undef THREE
    lua_tok_error("unexpected character");
}

const Token *
lua_tok_cur(void) { return &L_cur; }

// --- Error-mode harness (used by tests) ------------------------------

void lua_tok_error_jmp_set(jmp_buf *jb, int active) {
    if (active) memcpy(&L_err_jmp, jb, sizeof(jmp_buf));
    L_err_active = active;
}
const char *lua_tok_last_error(void) { return L_err_msg; }

// Pretty name for diagnostics.
const char *
lua_tok_kind_name(ltok_t k)
{
    switch (k) {
    case LT_EOF: return "<eof>";
    case LT_INT: return "<integer>";
    case LT_FLOAT: return "<float>";
    case LT_STRING: return "<string>";
    case LT_IDENT: return "<ident>";
    default: return "<token>";
    }
}
