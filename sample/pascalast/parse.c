// pascalast — lexer.
//
// Tokenizes the Pascal subset into the global tk / tk_int / tk_real /
// tk_id / tk_str state.  Comments (`{ ... }`, `(* ... *)`, `// ...`)
// and `{$R+}` / `{$R-}` directives are handled here so the parser
// proper in main.c only ever sees keyword / operator tokens.

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "node.h"  /* g_alloc_line, pascal_error declarations */
#include "parse.h"

extern void pascal_error(const char *fmt, ...);

static const struct { const char *s; int tk; } KEYWORDS[] = {
    {"program", TK_PROGRAM}, {"var", TK_VAR}, {"begin", TK_BEGIN},
    {"end", TK_END}, {"procedure", TK_PROCEDURE}, {"function", TK_FUNCTION},
    {"if", TK_IF}, {"then", TK_THEN}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"do", TK_DO},
    {"for", TK_FOR}, {"to", TK_TO}, {"downto", TK_DOWNTO},
    {"repeat", TK_REPEAT}, {"until", TK_UNTIL},
    {"integer", TK_INTEGER}, {"longint", TK_INTEGER},
    {"int64", TK_INTEGER}, {"word", TK_INTEGER},
    {"boolean", TK_BOOLEAN},
    {"real", TK_REAL}, {"double", TK_REAL}, {"single", TK_REAL},
    {"array", TK_ARRAY}, {"of", TK_OF},
    {"true", TK_TRUE}, {"false", TK_FALSE},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT},
    {"div", TK_DIV}, {"mod", TK_MOD},
    {"const", TK_CONST}, {"nil", TK_NIL},
    {"case", TK_CASE}, {"forward", TK_FORWARD},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"exit", TK_EXIT},
    {"type", TK_TYPE},
    {"record", TK_RECORD}, {"with", TK_WITH},
    {"set", TK_SET}, {"in", TK_IN},
    {"string", TK_STRING},
    {"text", TK_TEXT}, {"file", TK_FILE},
    {"try", TK_TRY}, {"except", TK_EXCEPT}, {"finally", TK_FINALLY}, {"raise", TK_RAISE},
    {"packed", TK_PACKED}, {"goto", TK_GOTO}, {"label", TK_LABEL},
    {"unit", TK_UNIT}, {"uses", TK_USES},
    {"interface", TK_INTERFACE}, {"implementation", TK_IMPLEMENTATION},
    {"class", TK_CLASS}, {"constructor", TK_CONSTRUCTOR},
    {"destructor", TK_DESTRUCTOR}, {"virtual", TK_VIRTUAL},
    {"override", TK_OVERRIDE}, {"inherited", TK_INHERITED}, {"self", TK_SELF},
    {"property", TK_PROPERTY},
    {"private", TK_PRIVATE}, {"public", TK_PUBLIC},
    {"protected", TK_PROTECTED}, {"published", TK_PUBLISHED},
    {"is", TK_IS}, {"as", TK_AS}, {"abstract", TK_ABSTRACT},
    {NULL, 0}
};

const char *src;
int line_no;
int tk;
int64_t tk_int;
double  tk_real;
char tk_id[256];        // already lowercased
char tk_str[1024];

// Compiler-directive state.  `{$R+}` enables subrange range checking
// at assignment, `{$R-}` disables it.  Default on (matches Free
// Pascal's default in $MODE OBJFPC and CodeTyphon defaults).  Other
// `{$X...}` directives are parsed and ignored — many real Pascal
// programs sprinkle `{$H+}` / `{$MODE OBJFPC}` etc. that we don't
// implement and shouldn't error on.
bool range_check_enabled = true;

// Parse the directive contents inside `{$...}` (after the `{$`).
// On entry `*src` points at the first character after `$`; on exit
// it points just past the closing `}`.
static void
lex_parse_directive(void)
{
    // Skip over the directive payload, capturing what we recognise.
    // Format: a letter followed by `+` / `-` is the simple form
    // (e.g. R+, H+).  Anything else is parsed-and-ignored — we just
    // run to the `}`.
    if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z')) {
        char letter = (char)toupper((unsigned char)*src);
        char sign   = src[1];
        if ((sign == '+' || sign == '-')
            && (src[2] == '}' || src[2] == ',' || src[2] == ' ')) {
            switch (letter) {
            case 'R': range_check_enabled = (sign == '+'); break;
            // Other on/off directives accepted silently.
            default: break;
            }
        }
    }
    while (*src && *src != '}') {
        if (*src == '\n') line_no++;
        src++;
    }
    if (*src == '}') src++;
}

static void
lex_skip_ws(void)
{
    for (;;) {
        while (*src == ' ' || *src == '\t' || *src == '\r') src++;
        if (*src == '\n') { line_no++; src++; continue; }
        if (*src == '{') {
            // `{$...}` is a compiler directive — handle separately so
            // we can flip range-check etc.  Other `{ ... }` is a plain
            // comment.
            if (src[1] == '$') {
                src += 2;
                lex_parse_directive();
                continue;
            }
            while (*src && *src != '}') { if (*src == '\n') line_no++; src++; }
            if (*src == '}') src++;
            continue;
        }
        if (src[0] == '(' && src[1] == '*') {
            // `(*$...*)` is the alternate-syntax compiler directive.
            if (src[2] == '$') {
                src += 3;
                // Reuse lex_parse_directive but it expects `}` end —
                // adapt by scanning to `*)` ourselves.
                if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z')) {
                    char letter = (char)toupper((unsigned char)*src);
                    char sign   = src[1];
                    if ((sign == '+' || sign == '-')
                        && (src[2] == '*' || src[2] == ',' || src[2] == ' ')) {
                        if (letter == 'R') range_check_enabled = (sign == '+');
                    }
                }
                while (*src && !(src[0] == '*' && src[1] == ')')) {
                    if (*src == '\n') line_no++;
                    src++;
                }
                if (*src) src += 2;
                continue;
            }
            src += 2;
            while (*src && !(src[0] == '*' && src[1] == ')')) {
                if (*src == '\n') line_no++;
                src++;
            }
            if (*src) src += 2;
            continue;
        }
        if (src[0] == '/' && src[1] == '/') {
            while (*src && *src != '\n') src++;
            continue;
        }
        break;
    }
}

static int
lex_keyword(const char *id)
{
    for (int i = 0; KEYWORDS[i].s; i++) {
        if (strcmp(id, KEYWORDS[i].s) == 0) return KEYWORDS[i].tk;
    }
    return 0;
}

void
next_token(void)
{
    lex_skip_ws();
    g_alloc_line = line_no;
    if (!*src) { tk = TK_EOF; return; }

    char c = *src;
    if (isdigit((unsigned char)c)) {
        // Look ahead: a `.` followed by a digit (so we don't grab the
        // `..` range token) or an explicit `e`/`E` exponent makes this
        // a real literal.  Otherwise it's an integer.
        const char *p = src;
        while (isdigit((unsigned char)*p)) p++;
        bool is_real = false;
        if (*p == '.' && isdigit((unsigned char)p[1])) is_real = true;
        else if (*p == 'e' || *p == 'E') is_real = true;
        char *end;
        if (is_real) {
            errno = 0;
            double v = strtod(src, &end);
            if (errno == ERANGE) pascal_error("real literal out of range at line %d", line_no);
            src = end;
            tk_real = v;
            tk = TK_RNUM;
        } else {
            errno = 0;
            long long v = strtoll(src, &end, 10);
            if (errno == ERANGE) pascal_error("integer literal out of range at line %d", line_no);
            src = end;
            tk_int = v;
            tk = TK_INT;
        }
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        int n = 0;
        while (isalnum((unsigned char)*src) || *src == '_') {
            if (n + 1 < (int)sizeof(tk_id))
                tk_id[n++] = (char)tolower((unsigned char)*src);
            src++;
        }
        tk_id[n] = 0;
        int kw = lex_keyword(tk_id);
        tk = kw ? kw : TK_ID;
        return;
    }
    if (c == '\'') {
        // Single-quoted string literal.  Pascal escape: '' inside string → '.
        src++;
        int n = 0;
        for (;;) {
            if (*src == 0) pascal_error("unterminated string at line %d", line_no);
            if (*src == '\'' && src[1] == '\'') {
                if (n + 1 < (int)sizeof(tk_str)) tk_str[n++] = '\'';
                src += 2;
            } else if (*src == '\'') {
                src++;
                break;
            } else {
                if (*src == '\n') line_no++;
                if (n + 1 < (int)sizeof(tk_str)) tk_str[n++] = *src;
                src++;
            }
        }
        tk_str[n] = 0;
        tk = TK_STR;
        return;
    }

    src++;
    switch (c) {
    case '(': tk = TK_LPAREN; return;
    case ')': tk = TK_RPAREN; return;
    case '[': tk = TK_LBRACK; return;
    case ']': tk = TK_RBRACK; return;
    case ';': tk = TK_SEMI; return;
    case ',': tk = TK_COMMA; return;
    case '+': tk = TK_PLUS; return;
    case '-': tk = TK_MINUS; return;
    case '*': tk = TK_STAR; return;
    case '/': tk = TK_SLASH; return;
    case '=': tk = TK_EQ; return;
    case ':':
        if (*src == '=') { src++; tk = TK_ASSIGN; return; }
        tk = TK_COLON; return;
    case '.':
        if (*src == '.') { src++; tk = TK_DOTDOT; return; }
        tk = TK_DOT; return;
    case '^': tk = TK_HAT; return;
    case '@': tk = TK_AT; return;
    case '<':
        if (*src == '=') { src++; tk = TK_LE; return; }
        if (*src == '>') { src++; tk = TK_NE; return; }
        tk = TK_LT; return;
    case '>':
        if (*src == '=') { src++; tk = TK_GE; return; }
        tk = TK_GT; return;
    }
    pascal_error("unexpected character '%c' at line %d", c, line_no);
}

// Human-readable name for each token kind.  Used in error messages.
const char *
tk_name(int t)
{
    switch (t) {
    case TK_EOF: return "end-of-file";
    case TK_INT: return "integer literal";
    case TK_RNUM: return "real literal";
    case TK_ID: return "identifier";
    case TK_STR: return "string literal";
    case TK_LPAREN: return "'('"; case TK_RPAREN: return "')'";
    case TK_LBRACK: return "'['"; case TK_RBRACK: return "']'";
    case TK_SEMI: return "';'"; case TK_COMMA: return "','";
    case TK_COLON: return "':'"; case TK_DOT: return "'.'";
    case TK_DOTDOT: return "'..'"; case TK_ASSIGN: return "':='";
    case TK_PLUS: return "'+'"; case TK_MINUS: return "'-'";
    case TK_STAR: return "'*'"; case TK_SLASH: return "'/'";
    case TK_EQ: return "'='"; case TK_NE: return "'<>'";
    case TK_LT: return "'<'"; case TK_LE: return "'<='";
    case TK_GT: return "'>'"; case TK_GE: return "'>='";
    case TK_SET: return "'set'"; case TK_IN: return "'in'";
    case TK_PROGRAM: return "'program'"; case TK_VAR: return "'var'";
    case TK_BEGIN: return "'begin'"; case TK_END: return "'end'";
    case TK_PROCEDURE: return "'procedure'"; case TK_FUNCTION: return "'function'";
    case TK_IF: return "'if'"; case TK_THEN: return "'then'"; case TK_ELSE: return "'else'";
    case TK_WHILE: return "'while'"; case TK_DO: return "'do'";
    case TK_FOR: return "'for'"; case TK_TO: return "'to'"; case TK_DOWNTO: return "'downto'";
    case TK_REPEAT: return "'repeat'"; case TK_UNTIL: return "'until'";
    case TK_INTEGER: return "'integer'"; case TK_BOOLEAN: return "'boolean'";
    case TK_REAL: return "'real'"; case TK_ARRAY: return "'array'"; case TK_OF: return "'of'";
    case TK_TRUE: return "'true'"; case TK_FALSE: return "'false'";
    case TK_AND: return "'and'"; case TK_OR: return "'or'"; case TK_NOT: return "'not'";
    case TK_DIV: return "'div'"; case TK_MOD: return "'mod'";
    case TK_CONST: return "'const'"; case TK_NIL: return "'nil'";
    case TK_CASE: return "'case'"; case TK_FORWARD: return "'forward'";
    case TK_BREAK: return "'break'"; case TK_CONTINUE: return "'continue'";
    case TK_EXIT: return "'exit'"; case TK_TYPE: return "'type'";
    case TK_RECORD: return "'record'"; case TK_WITH: return "'with'";
    case TK_STRING: return "'string'"; case TK_HAT: return "'^'"; case TK_AT: return "'@'";
    case TK_TEXT: return "'text'"; case TK_FILE: return "'file'";
    case TK_TRY: return "'try'"; case TK_EXCEPT: return "'except'";
    case TK_FINALLY: return "'finally'"; case TK_RAISE: return "'raise'";
    case TK_PACKED: return "'packed'"; case TK_GOTO: return "'goto'"; case TK_LABEL: return "'label'";
    case TK_UNIT: return "'unit'"; case TK_USES: return "'uses'";
    case TK_INTERFACE: return "'interface'"; case TK_IMPLEMENTATION: return "'implementation'";
    default: return "<unknown>";
    }
}

void
expect(int t, const char *what)
{
    if (tk != t)
        pascal_error("expected %s at line %d, got %s", what, line_no, tk_name(tk));
    next_token();
}

bool
accept(int t)
{
    if (tk == t) { next_token(); return true; }
    return false;
}

void
init_lexer(const char *text)
{
    src = text;
    line_no = 1;
    next_token();
}
