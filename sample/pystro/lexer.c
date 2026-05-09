// lexer.c — pystro tokenizer.  Indentation-based: at the start of a
// logical line, compare leading whitespace to the indent stack and emit
// INDENT / DEDENT(s); inside an open paren / bracket / brace, NEWLINEs
// are suppressed.  Tokens are buffered into `tok_arr` so the parser can
// look ahead and (in the case of `def`) re-scan a suite to collect
// locals.

enum tok_kind {
    T_EOF = 0, T_NEWLINE, T_INDENT, T_DEDENT,
    T_INT, T_FLOAT, T_IMAG, T_STR, T_FSTR, T_BYTES, T_NAME,

    // Keywords.
    T_DEF, T_RETURN, T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN, T_PASS,
    T_BREAK, T_CONTINUE, T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NONE,
    T_CLASS, T_TRY, T_EXCEPT, T_FINALLY, T_RAISE, T_AS, T_LAMBDA,
    T_GLOBAL, T_NONLOCAL, T_IS, T_IMPORT, T_FROM, T_WITH, T_YIELD,
    T_ASSERT, T_DEL, T_MATCH, T_CASE,

    // Punctuation.
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_DOT, T_SEMI, T_AT, T_ARROW,

    // Assignment.
    T_ASSIGN, T_WALRUS,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_SLASH_SLASH_EQ,
    T_PERCENT_EQ, T_AMP_EQ, T_PIPE_EQ, T_CARET_EQ,
    T_LSHIFT_EQ, T_RSHIFT_EQ, T_STAR_STAR_EQ, T_AT_EQ,

    // Operators.
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_SLASH_SLASH, T_PERCENT,
    T_STAR_STAR, T_AMP, T_PIPE, T_CARET, T_TILDE,
    T_LSHIFT, T_RSHIFT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
};

typedef struct {
    int      kind;
    int      line;
    int64_t  ival;          // T_INT (small)
    double   fval;          // T_FLOAT
    const char *sval;       // T_STR / T_FSTR / T_NAME / large-int string
    size_t   slen;
    bool     ival_overflow; // T_INT: true if number didn't fit in int64
} Tok;

static const char *src_buf;
static size_t      src_pos;
static int         src_line;
static const char *src_filename;

static Tok    *tok_arr;
static size_t  tok_len, tok_capa;
static size_t  tok_pos;

static int   indent_stack[64];
static int   indent_top;
static int   paren_depth;
static bool  at_line_start;

// Save/restore the entire lexer/tokenizer state.  Used by bi_import
// when parsing one module triggers a nested import (which calls
// tokenize/parse_program recursively); without this, the outer module's
// tokens get clobbered by the inner module's.
struct lexer_state {
    const char *src_buf;
    size_t      src_pos;
    int         src_line;
    const char *src_filename;
    Tok        *tok_arr;
    size_t      tok_len, tok_capa, tok_pos;
    int         indent_stack[64];
    int         indent_top;
    int         paren_depth;
    bool        at_line_start;
};

void *lexer_save_alloc(void) {
    struct lexer_state *s = (struct lexer_state *)GC_malloc(sizeof(*s));
    s->src_buf = src_buf; s->src_pos = src_pos; s->src_line = src_line;
    s->src_filename = src_filename;
    s->tok_arr = tok_arr; s->tok_len = tok_len; s->tok_capa = tok_capa;
    s->tok_pos = tok_pos;
    for (int i = 0; i < 64; i++) s->indent_stack[i] = indent_stack[i];
    s->indent_top = indent_top; s->paren_depth = paren_depth;
    s->at_line_start = at_line_start;
    return s;
}

void lexer_restore_free(void *p) {
    struct lexer_state *s = (struct lexer_state *)p;
    src_buf = s->src_buf; src_pos = s->src_pos; src_line = s->src_line;
    src_filename = s->src_filename;
    tok_arr = s->tok_arr; tok_len = s->tok_len; tok_capa = s->tok_capa;
    tok_pos = s->tok_pos;
    for (int i = 0; i < 64; i++) indent_stack[i] = s->indent_stack[i];
    indent_top = s->indent_top; paren_depth = s->paren_depth;
    at_line_start = s->at_line_start;
}

static void
tok_push(int kind, int line)
{
    if (tok_len == tok_capa) {
        tok_capa = tok_capa ? tok_capa * 2 : 256;
        tok_arr = (Tok *)GC_realloc(tok_arr, tok_capa * sizeof(Tok));
    }
    Tok *t = &tok_arr[tok_len++];
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->line = line;
}

static Tok *tok_last(void) { return &tok_arr[tok_len - 1]; }

static const char **name_pool;
static size_t       name_pool_len, name_pool_capa;

// Map a token kind to a human-readable name (for parse error messages).
const char *
tok_kind_name(int k)
{
    switch (k) {
      case T_EOF: return "<EOF>";
      case T_NEWLINE: return "<NEWLINE>";
      case T_INDENT: return "<INDENT>";
      case T_DEDENT: return "<DEDENT>";
      case T_INT: return "<int literal>";
      case T_FLOAT: return "<float literal>";
      case T_IMAG: return "<imaginary literal>";
      case T_STR: return "<str literal>";
      case T_FSTR: return "<f-string>";
      case T_BYTES: return "<bytes>";
      case T_NAME: return "<name>";
      case T_DEF: return "'def'";
      case T_RETURN: return "'return'";
      case T_IF: return "'if'";
      case T_ELIF: return "'elif'";
      case T_ELSE: return "'else'";
      case T_WHILE: return "'while'";
      case T_FOR: return "'for'";
      case T_IN: return "'in'";
      case T_PASS: return "'pass'";
      case T_BREAK: return "'break'";
      case T_CONTINUE: return "'continue'";
      case T_AND: return "'and'";
      case T_OR: return "'or'";
      case T_NOT: return "'not'";
      case T_TRUE: return "'True'";
      case T_FALSE: return "'False'";
      case T_NONE: return "'None'";
      case T_CLASS: return "'class'";
      case T_TRY: return "'try'";
      case T_EXCEPT: return "'except'";
      case T_FINALLY: return "'finally'";
      case T_RAISE: return "'raise'";
      case T_AS: return "'as'";
      case T_LAMBDA: return "'lambda'";
      case T_GLOBAL: return "'global'";
      case T_NONLOCAL: return "'nonlocal'";
      case T_IS: return "'is'";
      case T_IMPORT: return "'import'";
      case T_FROM: return "'from'";
      case T_WITH: return "'with'";
      case T_YIELD: return "'yield'";
      case T_ASSERT: return "'assert'";
      case T_DEL: return "'del'";
      case T_MATCH: return "'match'";
      case T_CASE: return "'case'";
      case T_LPAREN: return "'('";
      case T_RPAREN: return "')'";
      case T_LBRACK: return "'['";
      case T_RBRACK: return "']'";
      case T_LBRACE: return "'{'";
      case T_RBRACE: return "'}'";
      case T_COMMA: return "','";
      case T_COLON: return "':'";
      case T_DOT: return "'.'";
      case T_SEMI: return "';'";
      case T_AT: return "'@'";
      case T_ARROW: return "'->'";
      case T_ASSIGN: return "'='";
      case T_WALRUS: return "':='";
      case T_PLUS: return "'+'";
      case T_MINUS: return "'-'";
      case T_STAR: return "'*'";
      case T_SLASH: return "'/'";
      case T_SLASH_SLASH: return "'//'";
      case T_PERCENT: return "'%'";
      case T_STAR_STAR: return "'**'";
      case T_AMP: return "'&'";
      case T_PIPE: return "'|'";
      case T_CARET: return "'^'";
      case T_TILDE: return "'~'";
      case T_LSHIFT: return "'<<'";
      case T_RSHIFT: return "'>>'";
      case T_EQ: return "'=='";
      case T_NE: return "'!='";
      case T_LT: return "'<'";
      case T_LE: return "'<='";
      case T_GT: return "'>'";
      case T_GE: return "'>='";
    }
    return "<token>";
}

const char *
intern_name(const char *s, size_t len)
{
    for (size_t i = 0; i < name_pool_len; i++) {
        const char *p = name_pool[i];
        if (strlen(p) == len && memcmp(p, s, len) == 0) return p;
    }
    if (name_pool_len == name_pool_capa) {
        name_pool_capa = name_pool_capa ? name_pool_capa * 2 : 64;
        name_pool = (const char **)GC_realloc(name_pool, name_pool_capa * sizeof(char *));
    }
    char *buf = (char *)GC_malloc_atomic(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    name_pool[name_pool_len++] = buf;
    return buf;
}

static int
keyword_kind(const char *s, size_t len)
{
    static const struct { const char *kw; int k; } kws[] = {
        {"def",T_DEF},{"return",T_RETURN},{"if",T_IF},{"elif",T_ELIF},
        {"else",T_ELSE},{"while",T_WHILE},{"for",T_FOR},{"in",T_IN},
        {"pass",T_PASS},{"break",T_BREAK},{"continue",T_CONTINUE},
        {"and",T_AND},{"or",T_OR},{"not",T_NOT},
        {"True",T_TRUE},{"False",T_FALSE},{"None",T_NONE},
        {"class",T_CLASS},{"try",T_TRY},{"except",T_EXCEPT},
        {"finally",T_FINALLY},{"raise",T_RAISE},{"as",T_AS},
        {"lambda",T_LAMBDA},{"global",T_GLOBAL},{"nonlocal",T_NONLOCAL},
        {"is",T_IS},{"import",T_IMPORT},{"from",T_FROM},
        {"with",T_WITH},{"yield",T_YIELD},
        {"assert",T_ASSERT},{"del",T_DEL},
        // match/case are soft keywords in CPython; we keep them as
        // T_NAME at lex time and recognise them at the statement-start
        // position in the parser.
    };
    for (size_t i = 0; i < sizeof(kws)/sizeof(kws[0]); i++) {
        size_t kl = strlen(kws[i].kw);
        if (kl == len && memcmp(kws[i].kw, s, len) == 0) return kws[i].k;
    }
    return T_NAME;
}

__attribute__((noreturn,format(printf,1,2)))
static void
lex_error(const char *fmt, ...)
{
    fprintf(stderr, "pystro: %s:%d: ", src_filename ? src_filename : "<input>", src_line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char
peek(int off) { return src_buf[src_pos + off]; }

static void read_string_lit_raw(int line, char quote, bool is_fstr);

static void
read_string_lit(int line, char quote, bool is_fstr)
{
    src_pos++;
    // Triple-quoted string: matches `quote quote ...` at start.
    bool triple = (peek(0) == quote && peek(1) == quote);
    if (triple) src_pos += 2;
    size_t cap = 32, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    // PEP 701 (3.12+): inside an f-string `{...}` part, the same quote
    // can appear as a string literal (e.g. `f'{' '.join(x)}'`).  Track
    // brace depth so we don't treat the inner quote as the f-string
    // terminator.
    int brace_depth = 0;
    while (peek(0) != '\0') {
        if (triple) {
            if (peek(0) == quote && peek(1) == quote && peek(2) == quote) break;
        } else {
            if (peek(0) == quote && brace_depth == 0) break;
        }
        if (is_fstr) {
            char c = peek(0);
            if (c == '{') {
                if (brace_depth == 0 && peek(1) == '{') {
                    // Escaped `{{` in literal text — keep both bytes;
                    // f-string body parser collapses to one `{`.
                    if (len + 2 + 1 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
                    buf[len++] = '{'; buf[len++] = '{';
                    src_pos += 2;
                    continue;
                }
                brace_depth++;
            } else if (c == '}') {
                if (brace_depth == 0 && peek(1) == '}') {
                    if (len + 2 + 1 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
                    buf[len++] = '}'; buf[len++] = '}';
                    src_pos += 2;
                    continue;
                }
                if (brace_depth > 0) brace_depth--;
            }
        }
        char ch = peek(0);
        if (ch == '\\' && !is_fstr) {
            src_pos++;
            char esc = peek(0);
            if (esc == '\0') lex_error("unterminated string");
            switch (esc) {
              case 'n': ch = '\n'; break;
              case 't': ch = '\t'; break;
              case 'r': ch = '\r'; break;
              case 'b': ch = '\b'; break;
              case 'f': ch = '\f'; break;
              case 'a': ch = '\a'; break;
              case 'v': ch = '\v'; break;
              case '\\': ch = '\\'; break;
              case '\'': ch = '\''; break;
              case '"': ch = '"'; break;
              case '0': ch = '\0'; break;
              case 'x': {
                src_pos++;
                int hi = peek(0), lo = peek(1);
                int hh = (hi >= '0' && hi <= '9') ? hi - '0'
                       : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                       : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
                int ll = (lo >= '0' && lo <= '9') ? lo - '0'
                       : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                       : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
                if (hh < 0 || ll < 0) lex_error("invalid \\x escape");
                ch = (char)((hh << 4) | ll);
                src_pos += 2;        // already past 'x' via outer src_pos++
                if (len + 2 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
                buf[len++] = ch;
                continue;
              }
              case 'u':
              case 'U': {
                int n = (esc == 'u') ? 4 : 8;
                src_pos++;
                uint32_t cp = 0;
                for (int i = 0; i < n; i++) {
                    int dc = peek(0);
                    int dv = (dc >= '0' && dc <= '9') ? dc - '0'
                           : (dc >= 'a' && dc <= 'f') ? dc - 'a' + 10
                           : (dc >= 'A' && dc <= 'F') ? dc - 'A' + 10 : -1;
                    if (dv < 0) lex_error("invalid \\u/\\U escape");
                    cp = (cp << 4) | (uint32_t)dv;
                    src_pos++;
                }
                // Encode cp as UTF-8.
                unsigned char ub[5]; int ulen = 0;
                if (cp < 0x80)        { ub[0] = (unsigned char)cp; ulen = 1; }
                else if (cp < 0x800)  { ub[0] = 0xC0 | (cp >> 6);
                                        ub[1] = 0x80 | (cp & 0x3F); ulen = 2; }
                else if (cp < 0x10000){ ub[0] = 0xE0 | (cp >> 12);
                                        ub[1] = 0x80 | ((cp >> 6) & 0x3F);
                                        ub[2] = 0x80 | (cp & 0x3F); ulen = 3; }
                else                  { ub[0] = 0xF0 | (cp >> 18);
                                        ub[1] = 0x80 | ((cp >> 12) & 0x3F);
                                        ub[2] = 0x80 | ((cp >> 6) & 0x3F);
                                        ub[3] = 0x80 | (cp & 0x3F); ulen = 4; }
                while (len + ulen + 1 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
                for (int i = 0; i < ulen; i++) buf[len++] = (char)ub[i];
                continue;
              }
              default:  ch = esc;
            }
            src_pos++;
        } else if (ch == '\\' && is_fstr) {
            // f-string body.  PEP 701 (3.12+): inside `{...}`, escape
            // sequences are part of the inner Python expression and the
            // re-tokenizer there will handle them.  Outside `{...}`,
            // process escapes the same way a regular string would so
            // the literal text segments mean what they say.
            if (brace_depth > 0) {
                // Pass through verbatim: backslash + next char.
                src_pos++;
                char esc = peek(0);
                if (esc == '\0') lex_error("unterminated f-string");
                if (len + 2 + 1 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
                buf[len++] = '\\';
                buf[len++] = esc;
                src_pos++;
                continue;
            }
            src_pos++;
            char esc = peek(0);
            if (esc == '\0') lex_error("unterminated f-string");
            switch (esc) {
              case 'n': ch = '\n'; break;
              case 't': ch = '\t'; break;
              case 'r': ch = '\r'; break;
              case '\\': ch = '\\'; break;
              case '\'': ch = '\''; break;
              case '"': ch = '"'; break;
              case '{': ch = '{'; src_pos++;
                        if (len + 2 > cap) { cap *= 2; buf = (char*)GC_realloc(buf, cap); }
                        buf[len++] = '{'; buf[len++] = '{';
                        continue;
              case '}': ch = '}'; src_pos++;
                        if (len + 2 > cap) { cap *= 2; buf = (char*)GC_realloc(buf, cap); }
                        buf[len++] = '}'; buf[len++] = '}';
                        continue;
              default:  ch = esc;
            }
            src_pos++;
        } else {
            if (ch == '\n') {
                if (!triple) lex_error("unterminated string");
                src_line++;
            }
            src_pos++;
        }
        if (len + 2 > cap) {
            cap *= 2;
            buf = (char *)GC_realloc(buf, cap);
        }
        buf[len++] = ch;
    }
    if (triple) {
        if (peek(0) != quote || peek(1) != quote || peek(2) != quote)
            lex_error("unterminated triple-quoted string");
        src_pos += 3;
    } else {
        if (peek(0) != quote) lex_error("unterminated string");
        src_pos++;
    }
    buf[len] = '\0';
    tok_push(is_fstr ? T_FSTR : T_STR, line);
    tok_last()->sval = buf;
    tok_last()->slen = len;
}

// Raw string reader: backslashes are kept literal (no escape sequences).
// Like Python: `\` followed by anything is two characters in the string,
// EXCEPT a quote character is still escaped enough to not terminate
// (consistent with CPython: r"\"" is 2 chars '\\' + '"').
static void
read_string_lit_raw(int line, char quote, bool is_fstr)
{
    src_pos++;
    bool triple = (peek(0) == quote && peek(1) == quote);
    if (triple) src_pos += 2;
    size_t cap = 32, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    while (peek(0) != '\0') {
        if (triple) {
            if (peek(0) == quote && peek(1) == quote && peek(2) == quote) break;
        } else {
            if (peek(0) == quote) break;
        }
        char ch = peek(0);
        if (ch == '\\' && peek(1) != '\0') {
            // Keep the backslash and the next char.
            if (len + 2 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
            buf[len++] = '\\';
            buf[len++] = peek(1);
            if (peek(1) == '\n') src_line++;
            src_pos += 2;
            continue;
        }
        if (!triple && ch == '\n') lex_error("unterminated string");
        if (ch == '\n') src_line++;
        if (len + 1 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
        buf[len++] = ch;
        src_pos++;
    }
    if (triple) {
        if (peek(0) != quote || peek(1) != quote || peek(2) != quote)
            lex_error("unterminated triple-quoted raw string");
        src_pos += 3;
    } else {
        if (peek(0) != quote) lex_error("unterminated raw string");
        src_pos++;
    }
    buf[len] = '\0';
    tok_push(is_fstr ? T_FSTR : T_STR, line);
    tok_last()->sval = buf;
    tok_last()->slen = len;
}

static void
read_number(void)
{
    int line = src_line;
    size_t start = src_pos;
    bool is_float = false;
    int  base = 10;
    if (peek(0) == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        base = 16; src_pos += 2;
        while (isxdigit((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
    } else if (peek(0) == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        base = 2; src_pos += 2;
        while (peek(0) == '0' || peek(0) == '1' || peek(0) == '_') src_pos++;
    } else if (peek(0) == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        base = 8; src_pos += 2;
        while ((peek(0) >= '0' && peek(0) <= '7') || peek(0) == '_') src_pos++;
    } else if (peek(0) == '.') {
        // Leading-dot float: '.5'.
        is_float = true;
        src_pos++;
        while (isdigit((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
        if (peek(0) == 'e' || peek(0) == 'E') {
            src_pos++;
            if (peek(0) == '+' || peek(0) == '-') src_pos++;
            while (isdigit((unsigned char)peek(0))) src_pos++;
        }
    } else {
        while (isdigit((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
        // Trailing or middle dot: '5.' or '5.0'.  But don't consume '.'
        // if followed by a non-digit (could be method call: 5.bit_length).
        if (peek(0) == '.' && (isdigit((unsigned char)peek(1)) ||
                               (peek(1) != '_' && peek(1) != '.' &&
                                !isalpha((unsigned char)peek(1))))) {
            is_float = true;
            src_pos++;
            while (isdigit((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
        }
        if (peek(0) == 'e' || peek(0) == 'E') {
            is_float = true;
            src_pos++;
            if (peek(0) == '+' || peek(0) == '-') src_pos++;
            while (isdigit((unsigned char)peek(0))) src_pos++;
        }
    }
    size_t len = src_pos - start;
    // Strip underscores into a fresh buf.
    char *clean = (char *)GC_malloc_atomic(len + 1);
    size_t cl = 0;
    for (size_t i = 0; i < len; i++) if (src_buf[start + i] != '_') clean[cl++] = src_buf[start + i];
    clean[cl] = '\0';

    // Imaginary suffix: `1j` / `1.5j` → T_IMAG, value = the float.
    if (peek(0) == 'j' || peek(0) == 'J') {
        src_pos++;
        tok_push(T_IMAG, line);
        tok_last()->fval = strtod(clean, NULL);
        return;
    }
    if (is_float) {
        tok_push(T_FLOAT, line);
        tok_last()->fval = strtod(clean, NULL);
        return;
    }
    tok_push(T_INT, line);
    // Try int64 first.
    char *end;
    errno = 0;
    long long ll;
    if (base == 10) ll = strtoll(clean, &end, 10);
    else if (base == 16) ll = strtoll(clean + 2, &end, 16);
    else if (base == 2)  ll = strtoll(clean + 2, &end, 2);
    else                 ll = strtoll(clean + 2, &end, 8);
    if (errno == ERANGE || ll > PYS_FIXNUM_MAX || ll < PYS_FIXNUM_MIN) {
        // Need bignum literal.  Stash decimal text on `sval` and set overflow flag.
        char *dec = clean;
        if (base != 10) {
            mpz_t z; mpz_init(z);
            mpz_set_str(z, base == 10 ? clean : clean + 2, base);
            dec = mpz_get_str(NULL, 10, z);
            mpz_clear(z);
        }
        tok_last()->sval = dec;
        tok_last()->slen = strlen(dec);
        tok_last()->ival_overflow = true;
        return;
    }
    tok_last()->ival = (int64_t)ll;
}

// Identifier byte test.  ASCII follows isalnum/_ rules; UTF-8 multibyte
// sequences (any byte >= 0x80) are accepted as identifier bytes — this
// roughly matches Python's permissive identifier rules without needing
// a full Unicode property database.
static inline bool
is_ident_byte(unsigned char b, bool first)
{
    if (b == '_') return true;
    if (b >= 0x80) return true;     // UTF-8 lead/continuation byte
    if (first) return isalpha(b) != 0;
    return isalnum(b) != 0;
}

static void
read_name(void)
{
    int line = src_line;
    size_t start = src_pos;
    while (is_ident_byte((unsigned char)peek(0), false)) src_pos++;
    size_t len = src_pos - start;
    int k = keyword_kind(src_buf + start, len);
    tok_push(k, line);
    if (k == T_NAME) {
        const char *p = intern_name(src_buf + start, len);
        tok_last()->sval = p;
        tok_last()->slen = len;
    }
}

static void
handle_indent_at_line_start(void)
{
    int col = 0;
    while (peek(0) == ' ' || peek(0) == '\t') {
        col += (peek(0) == '\t') ? 8 : 1;
        src_pos++;
    }
    if (peek(0) == '\n' || peek(0) == '#' || peek(0) == '\0') return;

    int top = indent_stack[indent_top];
    if (col > top) {
        if (indent_top + 1 >= (int)(sizeof(indent_stack)/sizeof(int))) lex_error("indent too deep");
        indent_stack[++indent_top] = col;
        tok_push(T_INDENT, src_line);
    } else {
        while (col < indent_stack[indent_top]) {
            indent_top--;
            tok_push(T_DEDENT, src_line);
        }
        if (col != indent_stack[indent_top]) lex_error("inconsistent indentation");
    }
}

void
tokenize(const char *src, const char *filename)
{
    src_buf = src; src_pos = 0; src_line = 1; src_filename = filename;
    indent_top = 0; indent_stack[0] = 0;
    paren_depth = 0; at_line_start = true;
    tok_arr = NULL; tok_len = 0; tok_capa = 0; tok_pos = 0;

    while (peek(0) != '\0') {
        if (at_line_start && paren_depth == 0) {
            handle_indent_at_line_start();
            at_line_start = false;
            if (peek(0) == '\0') break;
        }
        char ch = peek(0);
        if (ch == '#') { while (peek(0) != '\n' && peek(0) != '\0') src_pos++; continue; }
        if (ch == '\n') {
            src_pos++; src_line++;
            if (paren_depth == 0) {
                if (tok_len > 0 && tok_arr[tok_len - 1].kind != T_NEWLINE
                                && tok_arr[tok_len - 1].kind != T_INDENT
                                && tok_arr[tok_len - 1].kind != T_DEDENT)
                    tok_push(T_NEWLINE, src_line - 1);
                at_line_start = true;
            }
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r') { src_pos++; continue; }
        if (ch == '\\' && peek(1) == '\n') { src_pos += 2; src_line++; continue; }
        if (isdigit((unsigned char)ch)) { read_number(); continue; }
        // Leading-dot float literal: ".5" → 0.5  (but not "..." Ellipsis).
        if (ch == '.' && isdigit((unsigned char)peek(1))) { read_number(); continue; }
        // String prefix combinations: r/R/u/U + b/B + f/F.  Order is
        // flexible per Python (rb, br, Rb, etc.).
        {
            bool p_raw = false, p_bytes = false, p_fstr = false;
            int look = 0;
            char c0 = peek(0), c1 = peek(1), c2 = peek(2);
            // Try 1-char prefix
            if ((c0 == 'r' || c0 == 'R') && (c1 == '"' || c1 == '\'')) {
                p_raw = true; look = 1;
            } else if ((c0 == 'u' || c0 == 'U') && (c1 == '"' || c1 == '\'')) {
                look = 1;     // u"..." — same as plain str
            } else if ((c0 == 'b' || c0 == 'B') && (c1 == '"' || c1 == '\'')) {
                p_bytes = true; look = 1;
            } else if ((c0 == 'f' || c0 == 'F') && (c1 == '"' || c1 == '\'')) {
                p_fstr = true; look = 1;
            } else if ((c0 == 't' || c0 == 'T') && (c1 == '"' || c1 == '\'')) {
                // t"..." — PEP 750 template strings (3.14+).  Pystro
                // models them as plain f-strings.
                p_fstr = true; look = 1;
            }
            // Try 2-char prefix combos
            else if ((c0 == 'r' || c0 == 'R') && (c1 == 'b' || c1 == 'B') && (c2 == '"' || c2 == '\'')) {
                p_raw = true; p_bytes = true; look = 2;
            } else if ((c0 == 'b' || c0 == 'B') && (c1 == 'r' || c1 == 'R') && (c2 == '"' || c2 == '\'')) {
                p_raw = true; p_bytes = true; look = 2;
            } else if ((c0 == 'r' || c0 == 'R') && (c1 == 'f' || c1 == 'F') && (c2 == '"' || c2 == '\'')) {
                p_raw = true; p_fstr = true; look = 2;
            } else if ((c0 == 'f' || c0 == 'F') && (c1 == 'r' || c1 == 'R') && (c2 == '"' || c2 == '\'')) {
                p_raw = true; p_fstr = true; look = 2;
            }
            if (look > 0) {
                int line = src_line;
                src_pos += look;
                if (p_raw) read_string_lit_raw(line, peek(0), p_fstr);
                else       read_string_lit(line, peek(0), p_fstr);
                if (p_bytes) tok_last()->kind = T_BYTES;
                continue;
            }
        }
        if (is_ident_byte((unsigned char)ch, true)) { read_name(); continue; }
        if (ch == '\'' || ch == '"') { read_string_lit(src_line, ch, false); continue; }

        int line = src_line;
        // Multi-char operators first.
        char a = ch, b = peek(1), c2 = peek(2);
        // 3-char.
        if (a == '*' && b == '*' && c2 == '=') { src_pos += 3; tok_push(T_STAR_STAR_EQ, line); continue; }
        if (a == '/' && b == '/' && c2 == '=') { src_pos += 3; tok_push(T_SLASH_SLASH_EQ, line); continue; }
        if (a == '<' && b == '<' && c2 == '=') { src_pos += 3; tok_push(T_LSHIFT_EQ, line); continue; }
        if (a == '>' && b == '>' && c2 == '=') { src_pos += 3; tok_push(T_RSHIFT_EQ, line); continue; }
        // 2-char.
        if (a == '*' && b == '*') { src_pos += 2; tok_push(T_STAR_STAR, line); continue; }
        if (a == '/' && b == '/') { src_pos += 2; tok_push(T_SLASH_SLASH, line); continue; }
        if (a == '<' && b == '<') { src_pos += 2; tok_push(T_LSHIFT, line); continue; }
        if (a == '>' && b == '>') { src_pos += 2; tok_push(T_RSHIFT, line); continue; }
        if (a == '-' && b == '>') { src_pos += 2; tok_push(T_ARROW, line); continue; }
        if (a == '=' && b == '=') { src_pos += 2; tok_push(T_EQ, line); continue; }
        if (a == '!' && b == '=') { src_pos += 2; tok_push(T_NE, line); continue; }
        if (a == ':' && b == '=') { src_pos += 2; tok_push(T_WALRUS, line); continue; }
        if (a == '<' && b == '=') { src_pos += 2; tok_push(T_LE, line); continue; }
        if (a == '>' && b == '=') { src_pos += 2; tok_push(T_GE, line); continue; }
        if (a == '+' && b == '=') { src_pos += 2; tok_push(T_PLUS_EQ, line); continue; }
        if (a == '-' && b == '=') { src_pos += 2; tok_push(T_MINUS_EQ, line); continue; }
        if (a == '*' && b == '=') { src_pos += 2; tok_push(T_STAR_EQ, line); continue; }
        if (a == '/' && b == '=') { src_pos += 2; tok_push(T_SLASH_EQ, line); continue; }
        if (a == '%' && b == '=') { src_pos += 2; tok_push(T_PERCENT_EQ, line); continue; }
        if (a == '&' && b == '=') { src_pos += 2; tok_push(T_AMP_EQ, line); continue; }
        if (a == '|' && b == '=') { src_pos += 2; tok_push(T_PIPE_EQ, line); continue; }
        if (a == '^' && b == '=') { src_pos += 2; tok_push(T_CARET_EQ, line); continue; }
        // 1-char.
        switch (a) {
          case '(': src_pos++; tok_push(T_LPAREN, line); paren_depth++; continue;
          case ')': src_pos++; tok_push(T_RPAREN, line); if (paren_depth) paren_depth--; continue;
          case '[': src_pos++; tok_push(T_LBRACK, line); paren_depth++; continue;
          case ']': src_pos++; tok_push(T_RBRACK, line); if (paren_depth) paren_depth--; continue;
          case '{': src_pos++; tok_push(T_LBRACE, line); paren_depth++; continue;
          case '}': src_pos++; tok_push(T_RBRACE, line); if (paren_depth) paren_depth--; continue;
          case ',': src_pos++; tok_push(T_COMMA, line); continue;
          case ':': src_pos++; tok_push(T_COLON, line); continue;
          case '.': src_pos++; tok_push(T_DOT, line); continue;
          case ';': src_pos++; tok_push(T_SEMI, line); continue;
          case '@':
            if (src[src_pos + 1] == '=') { src_pos += 2; tok_push(T_AT_EQ, line); continue; }
            src_pos++; tok_push(T_AT, line); continue;
          case '+': src_pos++; tok_push(T_PLUS, line); continue;
          case '-': src_pos++; tok_push(T_MINUS, line); continue;
          case '*': src_pos++; tok_push(T_STAR, line); continue;
          case '/': src_pos++; tok_push(T_SLASH, line); continue;
          case '%': src_pos++; tok_push(T_PERCENT, line); continue;
          case '&': src_pos++; tok_push(T_AMP, line); continue;
          case '|': src_pos++; tok_push(T_PIPE, line); continue;
          case '^': src_pos++; tok_push(T_CARET, line); continue;
          case '~': src_pos++; tok_push(T_TILDE, line); continue;
          case '<': src_pos++; tok_push(T_LT, line); continue;
          case '>': src_pos++; tok_push(T_GT, line); continue;
          case '=': src_pos++; tok_push(T_ASSIGN, line); continue;
        }
        lex_error("unexpected character '%c' (0x%02x)", a, (unsigned char)a);
    }
    if (tok_len > 0 && tok_arr[tok_len - 1].kind != T_NEWLINE
                    && tok_arr[tok_len - 1].kind != T_DEDENT)
        tok_push(T_NEWLINE, src_line);
    while (indent_top > 0) { indent_top--; tok_push(T_DEDENT, src_line); }
    tok_push(T_EOF, src_line);
}
