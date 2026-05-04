// lexer.c — pystro tokenizer.  Indentation-based: at the start of a
// logical line, compare leading whitespace to the indent stack and emit
// INDENT / DEDENT(s); inside an open paren / bracket / brace, NEWLINEs
// are suppressed.  Tokens are buffered into `tok_arr` so the parser can
// look ahead and (in the case of `def`) re-scan a suite to collect
// locals.

enum tok_kind {
    T_EOF = 0, T_NEWLINE, T_INDENT, T_DEDENT,
    T_INT, T_FLOAT, T_STR, T_FSTR, T_NAME,

    // Keywords.
    T_DEF, T_RETURN, T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN, T_PASS,
    T_BREAK, T_CONTINUE, T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NONE,
    T_CLASS, T_TRY, T_EXCEPT, T_FINALLY, T_RAISE, T_AS, T_LAMBDA,
    T_GLOBAL, T_NONLOCAL, T_IS, T_IMPORT, T_FROM, T_WITH, T_YIELD,

    // Punctuation.
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_DOT, T_SEMI, T_AT, T_ARROW,

    // Assignment.
    T_ASSIGN,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_SLASH_SLASH_EQ,
    T_PERCENT_EQ, T_AMP_EQ, T_PIPE_EQ, T_CARET_EQ,
    T_LSHIFT_EQ, T_RSHIFT_EQ, T_STAR_STAR_EQ,

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

static const char *
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

static void
read_string_lit(int line, char quote, bool is_fstr)
{
    src_pos++;
    size_t cap = 32, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    while (peek(0) != '\0' && peek(0) != quote) {
        char ch = peek(0);
        if (ch == '\\' && !is_fstr) {
            src_pos++;
            char esc = peek(0);
            if (esc == '\0') lex_error("unterminated string");
            switch (esc) {
              case 'n': ch = '\n'; break;
              case 't': ch = '\t'; break;
              case 'r': ch = '\r'; break;
              case '\\': ch = '\\'; break;
              case '\'': ch = '\''; break;
              case '"': ch = '"'; break;
              case '0': ch = '\0'; break;
              default:  ch = esc;
            }
            src_pos++;
        } else if (ch == '\\' && is_fstr) {
            // f-string: keep backslash for parser to handle, but process the same escapes.
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
            if (ch == '\n') lex_error("unterminated string");
            src_pos++;
        }
        if (len + 2 > cap) {
            cap *= 2;
            buf = (char *)GC_realloc(buf, cap);
        }
        buf[len++] = ch;
    }
    if (peek(0) != quote) lex_error("unterminated string");
    src_pos++;
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
    } else {
        while (isdigit((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
        if (peek(0) == '.' && isdigit((unsigned char)peek(1))) {
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
    if (errno == ERANGE || ll > PY_FIXNUM_MAX || ll < PY_FIXNUM_MIN) {
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

static void
read_name(void)
{
    int line = src_line;
    size_t start = src_pos;
    while (isalnum((unsigned char)peek(0)) || peek(0) == '_') src_pos++;
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

static void
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
        // f-string prefix: f"..." or f'...'
        if ((ch == 'f' || ch == 'F') && (peek(1) == '"' || peek(1) == '\'')) {
            int line = src_line;
            src_pos++;
            read_string_lit(line, peek(0), true);
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') { read_name(); continue; }
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
          case '@': src_pos++; tok_push(T_AT, line); continue;
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
