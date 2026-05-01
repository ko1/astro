// pascalast — Pascal subset on ASTro.
//
// One TU bundles lexer, parser, runtime, and driver.  Generated
// dispatchers live in node_*.c (built from node.def by ASTroGen) and are
// included via node.c.
//
// Subset:
//   * types: integer (int64), boolean (0/1), array[lo..hi] of integer
//   * declarations: var, procedure, function (top-level only, recursive
//     calls via forward symbol resolution at end of body parse)
//   * statements: assignment, if/then/else, while/do, repeat/until,
//                 for/to/downto/do, begin..end, proc call, builtins
//   * expressions: full operator precedence, short-circuit and/or, not,
//                  unary +/-, parens, array index, function call
//   * I/O: write / writeln with int and string-literal args (and width
//          spec, "x:w").  read / readln of integer variables.  halt.
//   * builtins (parser-recognized):
//          abs, sqr, succ, pred, ord, chr, odd, inc, dec, halt
//
// Pascal is case-insensitive for keywords and identifiers; we lowercase
// every ID token at lex time so the symbol table is straightforward.

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "context.h"
#include "node.h"

struct pascalast_option OPTION;

NODE **PASCAL_CALL_ARGS;
static uint32_t pascal_call_args_size;
static uint32_t pascal_call_args_capa;

// Post-parse fixup list for baked pcall variants.  mk_pcall emits the
// _baked NODE leaving body=NULL and the per-proc metadata slots at 0;
// resolve_pcall_bodies() walks this list once parse_program has
// returned (every proc body / nslots / etc filled in) and patches in
// the actual values so SPECIALIZE bakes them as literal constants in
// the SD source.  The list is small (one entry per call site) and
// freed after use.  We patch four uint32 metadata slots in addition
// to body so pascal_call_baked has every per-proc invariant as a
// constant the C compiler can fold through.
struct pcall_fixup {
    NODE     **body_slot;
    uint32_t  *nslots_slot;
    uint32_t  *return_slot_slot;
    uint32_t  *lexical_depth_slot;
    uint32_t  *is_function_slot;
    uint32_t   pidx;
};
static struct pcall_fixup *pcall_fixups;
static uint32_t pcall_fixups_size;
static uint32_t pcall_fixups_capa;

static void
register_pcall_fixup(NODE **body_slot,
                     uint32_t *nslots_slot,
                     uint32_t *return_slot_slot,
                     uint32_t *lexical_depth_slot,
                     uint32_t *is_function_slot,
                     uint32_t pidx)
{
    if (pcall_fixups_size >= pcall_fixups_capa) {
        uint32_t cap = pcall_fixups_capa ? pcall_fixups_capa * 2 : 64;
        pcall_fixups = realloc(pcall_fixups, cap * sizeof(*pcall_fixups));
        pcall_fixups_capa = cap;
    }
    pcall_fixups[pcall_fixups_size++] = (struct pcall_fixup){
        body_slot, nslots_slot, return_slot_slot,
        lexical_depth_slot, is_function_slot, pidx
    };
}

// ---------------------------------------------------------------------------
// Runtime helpers — referenced from node.def bodies and main.
// ---------------------------------------------------------------------------

void
pascal_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "pascalast: ");
    if (pascal_runtime_line > 0) fprintf(stderr, "[line %d] ", pascal_runtime_line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

// Variant that stamps `line` in pascal_runtime_line before raising.
// Use from inside NODE_DEF bodies when a specific node is the cause.
void
pascal_error_at(int line, const char *fmt, ...)
{
    pascal_runtime_line = line;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "pascalast: [line %d] ", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

// Catchable variant — when an exception handler is active, raise a
// Pascal-level exception that try/except can intercept; otherwise
// fall back to pascal_error_at.  Used by range checks and similar
// runtime contracts.
CTX *pascal_runtime_ctx;

void
pascal_raise(int line, const char *fmt, ...)
{
    pascal_runtime_line = line;
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *msg = (char *)GC_malloc_atomic(strlen(buf) + 1);
    strcpy(msg, buf);
    if (pascal_runtime_ctx && pascal_runtime_ctx->exc_top) {
        pascal_runtime_ctx->exc_msg = msg;
        longjmp(pascal_runtime_ctx->exc_top->buf, 1);
    }
    pascal_error_at(line, "%s", msg);
}

int64_t
pascal_aref(CTX *c, uint32_t arr_idx, int64_t i)
{
    if (UNLIKELY(i < 0 || i >= c->array_size[arr_idx])) {
        pascal_error("array index out of bounds: index=%ld, size=%d",
                     (long)(i + c->array_lo[arr_idx]), c->array_size[arr_idx]);
    }
    return c->arrays[arr_idx][i];
}

void
pascal_aset(CTX *c, uint32_t arr_idx, int64_t i, int64_t v)
{
    if (UNLIKELY(i < 0 || i >= c->array_size[arr_idx])) {
        pascal_error("array index out of bounds: index=%ld, size=%d",
                     (long)(i + c->array_lo[arr_idx]), c->array_size[arr_idx]);
    }
    c->arrays[arr_idx][i] = v;
}

// Exception handlers — kept outside any EVAL body so setjmp/inline
// don't conflict.  pascal_try_run runs body; if it raises, it
// restores fp/sp/display and returns 1.  pascal_try_run_finally is
// the same except it pops the handler before returning so the caller
// can run the finally clause and re-raise itself.
struct pascal_label_buf pascal_label_bufs[PASCAL_MAX_LABELS];

// Called by `node_label_set` — performs the setjmp dance.  Lives
// outside the EVAL body so the inlined dispatcher chain doesn't
// inherit the setjmp restriction.
int
pascal_label_setjmp(int label_idx)
{
    if (label_idx < 0 || label_idx >= PASCAL_MAX_LABELS) return 0;
    pascal_label_bufs[label_idx].active = 1;
    return setjmp(pascal_label_bufs[label_idx].buf);
}

int
pascal_try_run(CTX *c, NODE *body)
{
    struct exc_handler h;
    h.prev     = c->exc_top;
    h.saved_fp = c->fp;
    h.saved_sp = c->sp;
    memcpy(h.saved_display, c->display, sizeof(c->display));
    c->exc_top = &h;
    if (setjmp(h.buf) == 0) {
        EVAL(c, body);
        c->exc_top = h.prev;
        return 0;
    }
    c->exc_top = h.prev;
    c->fp = h.saved_fp;
    c->sp = h.saved_sp;
    memcpy(c->display, h.saved_display, sizeof(c->display));
    return 1;
}

int
pascal_try_run_finally(CTX *c, NODE *body)
{
    return pascal_try_run(c, body);
}

VALUE
pascal_call(CTX *c, uint32_t pidx, uint32_t argc, VALUE *av)
{
    struct pascal_proc *p = &c->procs[pidx];
    if ((int)argc != p->nparams) {
        pascal_error("%s: arity mismatch — expected %d, got %u",
                     p->name, p->nparams, argc);
    }

    int new_fp = c->sp;
    int new_sp = new_fp + p->nslots;

    if (UNLIKELY(new_sp > PASCAL_STACK_SIZE)) {
        pascal_error("call stack overflow at %s", p->name);
    }

    for (uint32_t i = 0; i < argc; i++) c->stack[new_fp + i] = av[i];
    for (int i = (int)argc; i < p->nslots; i++) c->stack[new_fp + i] = 0;

    int saved_fp = c->fp;
    int saved_sp = c->sp;
    c->fp = new_fp;
    c->sp = new_sp;

    // Display vector: save the previous binding for the callee's
    // lexical depth so siblings/recursion can rebind it freely;
    // restore at return.  display[0] is reserved for main-block locals
    // (the program body), which we set up once at startup.
    int d = p->lexical_depth;
    int saved_display = c->display[d];
    c->display[d] = new_fp;

    EVAL(c, p->body);

    VALUE rv = p->is_function ? c->stack[new_fp + p->return_slot] : 0;
    c->display[d] = saved_display;
    c->fp = saved_fp;
    c->sp = saved_sp;
    // `exit` only escapes the current procedure — clear before
    // returning to the caller.  break/continue must not leak across a
    // call either; treat them as "swallowed" at the boundary.
    c->exit_pending = 0;
    c->loop_action  = 0;
    return rv;
}

// ---------------------------------------------------------------------------
// Lexer.
// ---------------------------------------------------------------------------

enum {
    TK_EOF = 0,
    TK_INT, TK_RNUM, TK_ID, TK_STR,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_COLON, TK_DOT, TK_DOTDOT, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    // keywords
    TK_PROGRAM, TK_VAR, TK_BEGIN, TK_END, TK_PROCEDURE, TK_FUNCTION,
    TK_IF, TK_THEN, TK_ELSE, TK_WHILE, TK_DO, TK_FOR, TK_TO, TK_DOWNTO,
    TK_REPEAT, TK_UNTIL, TK_INTEGER, TK_BOOLEAN, TK_REAL, TK_ARRAY, TK_OF,
    TK_TRUE, TK_FALSE, TK_AND, TK_OR, TK_NOT, TK_DIV, TK_MOD,
    TK_CONST, TK_NIL,
    TK_CASE, TK_FORWARD,
    TK_BREAK, TK_CONTINUE, TK_EXIT, TK_TYPE,
    TK_RECORD, TK_WITH,
    TK_SET, TK_IN,
    TK_STRING,
    TK_HAT,
    TK_AT,
    TK_TEXT, TK_FILE,
    TK_TRY, TK_EXCEPT, TK_FINALLY, TK_RAISE,
    TK_PACKED, TK_GOTO, TK_LABEL,
    TK_UNIT, TK_USES, TK_INTERFACE, TK_IMPLEMENTATION,
    TK_CLASS, TK_CONSTRUCTOR, TK_DESTRUCTOR, TK_VIRTUAL, TK_OVERRIDE, TK_INHERITED, TK_SELF,
    TK_PROPERTY, TK_PRIVATE, TK_PUBLIC, TK_PROTECTED, TK_PUBLISHED,
    TK_IS, TK_AS, TK_ABSTRACT,
};

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
    {"class", TK_CLASS}, {"constructor", TK_CONSTRUCTOR}, {"destructor", TK_DESTRUCTOR},
    {"virtual", TK_VIRTUAL}, {"override", TK_OVERRIDE},
    {"inherited", TK_INHERITED},
    {"property", TK_PROPERTY},
    {"private", TK_PRIVATE}, {"public", TK_PUBLIC},
    {"protected", TK_PROTECTED}, {"published", TK_PUBLISHED},
    {"is", TK_IS}, {"as", TK_AS}, {"abstract", TK_ABSTRACT},
    {NULL, 0}
};

static const char *src;
static int line_no;
static int tk;
static int64_t tk_int;
static double  tk_real;
static char tk_id[256];        // already lowercased
static char tk_str[1024];

// Compiler-directive state.  `{$R+}` enables subrange range checking
// at assignment, `{$R-}` disables it.  Default on (matches Free
// Pascal's default in $MODE OBJFPC and CodeTyphon defaults).  Other
// `{$X...}` directives are parsed and ignored — many real Pascal
// programs sprinkle `{$H+}` / `{$MODE OBJFPC}` etc. that we don't
// implement and shouldn't error on.
static bool range_check_enabled = true;

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

static void
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
static const char *
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

static const char *
pt_name(int t)
{
    switch (t) {
    case PT_INT: return "integer";
    case PT_BOOL: return "boolean";
    case PT_REAL: return "real";
    case PT_ARRAY: return "array";
    default: return "<unknown>";
    }
}

static void
expect(int t, const char *what)
{
    if (tk != t)
        pascal_error("expected %s at line %d, got %s", what, line_no, tk_name(tk));
    next_token();
}

static bool
accept(int t)
{
    if (tk == t) { next_token(); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Symbol table.
// ---------------------------------------------------------------------------

enum sym_kind {
    SYM_GVAR,        // global scalar (idx into globals)
    SYM_GARR,        // global array (1D or 2D)
    SYM_LVAR,        // local scalar (idx into current frame)
    SYM_PROC,        // procedure / function (idx into procs)
    SYM_CONST,       // compile-time integer constant (int64)
    SYM_RCONST,      // compile-time real constant (double bits)
    SYM_TYPE,        // type alias — resolves to a base PT_* in s->type
    SYM_LARR,        // local array — slots [idx, idx+size) of the frame
    SYM_VARR,        // var-array parameter — slot holds the base pointer
    SYM_RECTYPE,     // record type — idx into record_types[]
    SYM_GREC,        // global record-valued variable
    SYM_LREC,        // local  record-valued variable
    SYM_VREC,        // var-record parameter — slot holds the base pointer
    SYM_CLASSTYPE,   // class type — idx into class_types[]
};

struct sym {
    char name[64];
    int kind;
    int idx;          // global slot / local slot / proc index / record-type idx
    int type;         // PT_INT / PT_BOOL / PT_REAL / PT_RECORD — element type for arrays
    bool by_ref;      // SYM_LVAR: true if this is a `var` parameter
    bool is_2d;       // SYM_GARR: true for 2D arrays
    bool has_range;   // SYM_GVAR/SYM_LVAR: subrange bounds (lo/hi reused)
    int32_t lo, hi;
    int32_t lo2, hi2; // 2D
    int64_t cval;     // SYM_CONST (raw bits — interpret as double if SYM_RCONST)
    int rec_idx;      // SYM_GREC / SYM_LREC: record_types[] index
    int depth;        // SYM_LVAR/LARR/LREC/VARR/VREC: lexical depth of declaring scope
    int class_idx;    // class index for class-typed variables and SYM_CLASSTYPE
};

// Add the new PT_RECORD enum value (alongside the ones in context.h).
#define PT_RECORD 4

// Record-type table.  Each declared record type lives here; the
// parser stores an index in the SYM_RECTYPE entry and in every
// record-valued variable.
// Free Pascal-style visibility section.  PUBLIC is the default — bare
// records (non-class) get PUBLIC for every field, and any class field
// that appears before the first visibility marker is also PUBLIC.  We
// don't distinguish PUBLISHED from PUBLIC at the language-semantic
// level (PUBLISHED is for RTTI in real FPC; we treat it as PUBLIC).
typedef enum { VIS_PUBLIC = 0, VIS_PRIVATE = 1, VIS_PROTECTED = 2, VIS_PUBLISHED = 3 } pascal_vis_t;

struct record_field {
    char name[48];
    int  offset;       // in slots
    int  type;         // PT_INT / PT_BOOL / PT_REAL / PT_RECORD / PT_ARRAY / PT_STR / PT_POINTER
    int  rec_idx;      // PT_RECORD: index into record_types[]
    int32_t lo, hi;    // PT_ARRAY: bounds
    int  elem_type;    // PT_ARRAY: element type
    int  arr_elem_size;// PT_ARRAY: slots per element (1 for scalars, N for nested record)
    int  arr_elem_rec; // PT_ARRAY: rec_idx of element if record-valued
    int  vis;          // pascal_vis_t — public for record fields, may be private/protected for class fields
    int  decl_class;   // class_types[] idx that declared the field (-1 for plain records)
};

struct record_type {
    char name[64];
    struct record_field fields[24];
    int  nfields;
    int  total_slots;
};

#define MAX_RECORD_TYPES 64
static struct record_type record_types[MAX_RECORD_TYPES];
static int n_record_types;

// Set by parse_type when it consumes a record-named type.
static int g_last_record_idx;

// Class table.  Each class extends a record_type with a method
// dispatch table.  Inheritance is single — `parent_idx` is the parent
// class's slot, or -1 for none.  The record_type behind `rec_idx`
// already includes the parent's fields (we copy them on declaration).
#define MAX_CLASS_TYPES 64
struct method_entry {
    char name[48];
    int  proc_idx;     // -1 until the method body is parsed
    int  vtable_slot;  // index into class's vtable (for virtual)
    bool is_virtual;
    bool is_constructor;
    bool is_abstract;  // virtual; abstract; — calling raises at runtime
    bool is_class;     // class procedure / class function — no Self
    bool is_function;  // captured from class header so virtual-call return-type
    char return_type;  // is known even when proc_idx is still -1
    int  vis;          // pascal_vis_t
    int  decl_class;   // class_types[] idx that declared the method
};
struct property_entry {
    char name[48];
    int  type;
    // Reader: either a field offset (read_field_offset >= 0) or a
    // method idx (read_method_idx >= 0).  Same for writer.  Read-only
    // properties have write_method_idx = -1 and write_field_offset = -1.
    int  read_field_offset;
    int  read_method_idx;
    int  write_field_offset;
    int  write_method_idx;
    int  vis;          // pascal_vis_t
};

struct class_type {
    char name[64];
    int  parent_idx;   // -1 if no parent
    int  rec_idx;      // record_types[] entry holding the layout
    struct method_entry methods[32];
    int  nmethods;
    struct property_entry properties[16];
    int  nprops;
    int  vtable_size;  // total virtuals (own + inherited)
    int *vtable;       // vtable[i] = proc index for vtable_slot i; built after parse
};
static struct class_type class_types[MAX_CLASS_TYPES];
int n_class_types;     // referenced via extern from node.def helpers

// Per-class vtable address table — populated by finalize_classes
// after parsing.  Read at run time by node_new_object to stamp slot
// 0 of every freshly allocated instance.
int **pascal_vtables;

extern CTX *parser_ctx;

// Helper: get the return type of a method, falling back to the class
// header's declared return type when the body's proc isn't yet
// resolved (abstract methods stay at proc_idx = -1 forever).
static inline int
method_return_type(struct method_entry *me)
{
    if (me->proc_idx >= 0) {
        return parser_ctx->procs[me->proc_idx].is_function
             ? (int)parser_ctx->procs[me->proc_idx].return_type : PT_INT;
    }
    return me->is_function ? (int)me->return_type : PT_INT;
}

// Map an object pointer back to its class_idx by looking up the
// vtable pointer stamped into slot 0.  Linear scan over
// pascal_vtables — O(n_class_types), small in practice.
int
pascal_class_of_obj(int64_t obj)
{
    int64_t *o = (int64_t *)(uintptr_t)obj;
    int *vt = (int *)(uintptr_t)o[0];
    if (!vt) return -1;
    for (int i = 0; i < n_class_types; i++) {
        if (pascal_vtables[i] == vt) return i;
    }
    return -1;
}

int
pascal_class_is_descendant(int sub, int sup)
{
    int walk = sub;
    while (walk >= 0) {
        if (walk == sup) return 1;
        walk = class_types[walk].parent_idx;
    }
    return 0;
}

// Set by parse_type when consuming a class-typed name.  -1 means
// the most-recent type isn't a class.
static int g_last_class_idx = -1;

#define MAX_GSYMS 1024
#define MAX_LSYMS 256

static struct sym gsyms[MAX_GSYMS];
static int n_gsyms;
static int n_globals_alloc;     // next free slot in globals

static struct sym lsyms[MAX_LSYMS];
static int n_lsyms;
static int n_locals_alloc;      // next free slot in current frame

// Currently-being-parsed procedure (for resolving the function-name self
// reference).  NULL while parsing the main body.
static struct pascal_proc *current_proc;
static int                 current_proc_idx;
// Lexical depth of the scope being parsed.  0 = main body; 1 = top-
// level proc; 2+ = nested.  Each new procedure body bumps it on entry
// and restores it on exit.
static int current_depth;
// Stack of nested-procedure metadata, indexed by depth.  Lets a
// nested body resolve its enclosing function for `Result` /
// recursive-call purposes.
static struct pascal_proc *proc_at_depth[PASCAL_MAX_DEPTH];
static int                 proc_idx_at_depth[PASCAL_MAX_DEPTH];
// scope_start_at_depth[d] = the index into lsyms where scope d begins.
// Used by sym_add_local to scope its duplicate check (so an inner
// `var x` may shadow an outer one).
int scope_start_at_depth[PASCAL_MAX_DEPTH + 1];

// Set by parse_program at the start of parsing so deeper helpers can
// reach the proc table without threading CTX* through every signature.
CTX *parser_ctx;
#define PASCAL_PROCS (parser_ctx->procs)

// `with` stack: each entry maps a record-typed symbol's fields into
// scope so bare field names resolve to that record's fields.  Pushed
// while parsing the `with`-statement body, popped on exit.  Inner
// withs shadow outer ones (last-pushed wins).
#define MAX_WITH_DEPTH 16
static struct sym *with_stack[MAX_WITH_DEPTH];
static int with_depth;

static struct record_field *
with_lookup_field(const char *name, struct sym **out_record)
{
    for (int i = with_depth - 1; i >= 0; i--) {
        struct sym *rs = with_stack[i];
        struct record_type *rt = &record_types[rs->rec_idx];
        for (int f = 0; f < rt->nfields; f++) {
            if (strcmp(rt->fields[f].name, name) == 0) {
                if (out_record) *out_record = rs;
                return &rt->fields[f];
            }
        }
    }
    return NULL;
}

static struct sym *
sym_find_local(const char *name)
{
    for (int i = n_lsyms - 1; i >= 0; i--) {
        if (strcmp(lsyms[i].name, name) == 0) return &lsyms[i];
    }
    return NULL;
}

static struct sym *
sym_find_global(const char *name)
{
    for (int i = n_gsyms - 1; i >= 0; i--) {
        if (strcmp(gsyms[i].name, name) == 0) return &gsyms[i];
    }
    return NULL;
}

static struct sym *
sym_find(const char *name)
{
    struct sym *s = sym_find_local(name);
    if (s) return s;
    return sym_find_global(name);
}

static struct sym *
sym_add_global(const char *name)
{
    if (sym_find_global(name)) pascal_error("duplicate global '%s'", name);
    if (n_gsyms >= MAX_GSYMS) pascal_error("too many globals");
    struct sym *s = &gsyms[n_gsyms++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = 0;
    return s;
}

static struct sym *
sym_add_local(const char *name)
{
    // Duplicate check is restricted to the *current* scope: a nested
    // proc may shadow an outer local, so we don't search the entire
    // lsyms stack.
    extern int scope_start_at_depth[];
    int scope_lo = scope_start_at_depth[current_depth];
    for (int i = n_lsyms - 1; i >= scope_lo; i--) {
        if (strcmp(lsyms[i].name, name) == 0)
            pascal_error("duplicate local '%s'", name);
    }
    if (n_lsyms >= MAX_LSYMS) pascal_error("too many locals");
    struct sym *s = &lsyms[n_lsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = 0;
    s->kind  = SYM_LVAR;
    s->idx   = n_locals_alloc++;
    s->depth = current_depth;
    return s;
}

// ---------------------------------------------------------------------------
// AST builders for sequences and statements.
// ---------------------------------------------------------------------------

static NODE *
mk_seq(NODE *a, NODE *b)
{
    if (!a) return b;
    if (!b) return a;
    return ALLOC_node_seq(a, b);
}

static NODE *
mk_int(int64_t v) { return ALLOC_node_int((uint64_t)v); }

// Type-aware expression — every parser helper that builds an
// expression returns one.  Type comes from the static knowledge of the
// symbol or operator that produced the node.
typedef struct {
    NODE *n;
    int   t;          // pascal_type (PT_INT / PT_BOOL / PT_REAL / PT_STR / PT_POINTER ...)
    int   class_idx;  // -1 unless t == PT_POINTER and we statically know the class
} TE;

// Reinterpret a `double` as a `uint64_t` for stuffing into node_real.
static uint64_t
dbl_bits(double d)
{
    union { double d; uint64_t u; } u;
    u.d = d;
    return u.u;
}

static NODE *
mk_real(double d) { return ALLOC_node_real(dbl_bits(d)); }

// Build a TE / NODE for reading or writing a record field.  `s` is
// the record-valued symbol; `f` is the field descriptor.  For SYM_VREC,
// the record is reachable through the indirect base pointer in s->idx;
// reuse the var-array load/store nodes (which already do
// "indirect base pointer + offset").
static TE
te_field_get(struct sym *s, struct record_field *f)
{
    int slot = s->idx + f->offset;
    if (s->kind == SYM_VREC)
        return (TE){ ALLOC_node_var_aref((uint32_t)s->idx, 0, mk_int(f->offset)),
                     f->type };
    if (s->kind == SYM_GREC) return (TE){ ALLOC_node_gref((uint32_t)slot), f->type, -1 };
    return (TE){ ALLOC_node_lref((uint32_t)slot), f->type, -1 };
}

static NODE *
mk_field_set(struct sym *s, struct record_field *f, NODE *val)
{
    int slot = s->idx + f->offset;
    if (s->kind == SYM_VREC)
        return ALLOC_node_var_aset((uint32_t)s->idx, 0, mk_int(f->offset), val);
    if (s->kind == SYM_GREC) return ALLOC_node_gset((uint32_t)slot, val);
    return ALLOC_node_lset((uint32_t)slot, val);
}

// Visibility check for class fields / methods / properties.  `vis` is
// the entry's pascal_vis_t and `decl_class` is the class_types[] index
// that declared it.  Plain record fields pass through (decl_class = -1,
// vis = VIS_PUBLIC), so calling this from a code path that mixes
// records and classes is safe.  current_method_class_idx is the class
// being parsed if we're inside a method body, else -1.
extern int current_method_class_idx;   // defined later in this TU
static void
check_access(int decl_class, int vis, const char *kind, const char *name)
{
    if (decl_class < 0) return;       // plain record — always public
    if (vis == VIS_PUBLIC || vis == VIS_PUBLISHED) return;
    int cur = current_method_class_idx;
    if (cur < 0) {
        pascal_error("%s '%s' is %s and not accessible from outside the class",
                     kind, name, vis == VIS_PRIVATE ? "private" : "protected");
    }
    if (vis == VIS_PRIVATE) {
        if (cur == decl_class) return;
        pascal_error("%s '%s' is private to class '%s' (cannot access from '%s')",
                     kind, name, class_types[decl_class].name, class_types[cur].name);
    }
    // VIS_PROTECTED: cur must be decl_class or a descendant.
    int walk = cur;
    while (walk >= 0) {
        if (walk == decl_class) return;
        walk = class_types[walk].parent_idx;
    }
    pascal_error("%s '%s' is protected (declared in '%s'); '%s' is not a descendant",
                 kind, name, class_types[decl_class].name, class_types[cur].name);
}

// int → real promotion if needed; identity for already-real or bool.
static TE
te_promote_real(TE e)
{
    if (e.t == PT_REAL) return e;
    return (TE){ .n = ALLOC_node_i2r(e.n), .t = PT_REAL, -1 };
}

// For binary numeric ops: promote both operands to real if either is
// real.  Returns the resulting type (PT_INT or PT_REAL).
static int
te_unify_numeric(TE *l, TE *r)
{
    if (l->t == PT_REAL || r->t == PT_REAL) {
        *l = te_promote_real(*l);
        *r = te_promote_real(*r);
        return PT_REAL;
    }
    return PT_INT;
}

// Coerce `e` to `target` if possible.  PT_INT → PT_REAL is automatic
// (Pascal allows assigning an int to a real).  Other directions (real
// → int, bool → numeric, …) require an explicit conversion in Pascal,
// so we error out.
static NODE *
te_coerce(TE e, int target, const char *context)
{
    if (e.t == target) return e.n;
    if (target == PT_REAL && e.t == PT_INT)  return ALLOC_node_i2r(e.n);
    if (target == PT_INT  && e.t == PT_BOOL) return e.n;   // bool fits in int
    if (target == PT_BOOL && e.t == PT_INT)  return e.n;   // tolerated; non-zero = true
    // char (int) → string: malloc a 1-char buffer at runtime.
    if (target == PT_STR  && e.t == PT_INT)  return ALLOC_node_chr_to_str(e.n);
    // Proc value <-> int (proc index) — interchangeable.
    if (target == PT_PROC && e.t == PT_INT)  return e.n;
    if (target == PT_INT  && e.t == PT_PROC) return e.n;
    pascal_error("type mismatch in %s: got %s, want %s",
                 context, pt_name(e.t), pt_name(target));
    return NULL;
}

// ---------------------------------------------------------------------------
// Forward decls.
// ---------------------------------------------------------------------------

static TE    te_expr(void);
static TE    te_simple(void);
static TE    te_term(void);
static TE    te_factor(void);
static NODE *parse_stmt(void);
static NODE *parse_compound(void);
static NODE *parse_stmt_list(int end_tk1, int end_tk2);
static void  parse_decls_block(CTX *c);
static void  parse_unit_file(const char *unit_name, CTX *c);

// Forward decls for class-method context globals; defined alongside
// parse_subprogram further down.
extern int  current_method_class_idx;
extern char current_method_name_buf[64];

struct method_entry;
static inline int method_return_type(struct method_entry *me);
extern CTX *parser_ctx;

// Subrange-bound carry-out from parse_type (defined further down so
// callers like parse_var_decls_global can pick up bounds for `var x:
// 1..10` and stamp them on the symbol for runtime range-checking).
extern int32_t g_last_subrange_lo, g_last_subrange_hi;
extern bool    g_last_subrange_set;
static TE    te_get(struct sym *s, NODE *index, NODE *index2);
static NODE *mk_set_typed(struct sym *s, NODE *index, NODE *index2, NODE *val);
static NODE *mk_addr_of(struct sym *s, NODE *index, NODE *index2);
static NODE *mk_call_with_promotion(int pidx, NODE **args, int *arg_types, uint32_t argc);
static uint32_t parse_typed_call_args(struct pascal_proc *p, NODE **out_nodes,
                                      int *out_types, uint32_t max);
static TE    te_builtin_call_in_expr(const char *name);

// Thin wrapper for the many statement-level callers that only want the
// node and don't care about the static type.
static NODE *parse_expr(void) { return te_expr().n; }

// (Variable get / set helpers `te_get` and `mk_set_typed` are defined
// alongside the expression parser below.)

// ---------------------------------------------------------------------------
// Argument staging for variadic procedure calls (≥ 4 args).  We collect
// argument NODE*s into PASCAL_CALL_ARGS[args_idx..args_idx+argc] so the
// generated node_pcall_n can pick them up at run time.
// ---------------------------------------------------------------------------

static uint32_t
push_call_args(NODE **args, uint32_t argc)
{
    if (pascal_call_args_size + argc > pascal_call_args_capa) {
        uint32_t cap = pascal_call_args_capa ? pascal_call_args_capa * 2 : 64;
        while (cap < pascal_call_args_size + argc) cap *= 2;
        PASCAL_CALL_ARGS = realloc(PASCAL_CALL_ARGS, cap * sizeof(NODE *));
        pascal_call_args_capa = cap;
    }
    uint32_t base = pascal_call_args_size;
    for (uint32_t i = 0; i < argc; i++) PASCAL_CALL_ARGS[base + i] = args[i];
    pascal_call_args_size += argc;
    return base;
}

static NODE *
mk_pcall(uint32_t pidx, NODE **args, uint32_t argc)
{
    NODE *n = NULL;
    NODE **body_slot = NULL;
    uint32_t *nslots_slot = NULL;
    uint32_t *return_slot_slot = NULL;
    uint32_t *lexical_depth_slot = NULL;
    uint32_t *is_function_slot = NULL;
    switch (argc) {
    case 0:
        n = ALLOC_node_pcall_0_baked(NULL, 0, 0, 0, 0);
        body_slot          = &n->u.node_pcall_0_baked.body;
        nslots_slot        = &n->u.node_pcall_0_baked.nslots;
        return_slot_slot   = &n->u.node_pcall_0_baked.return_slot;
        lexical_depth_slot = &n->u.node_pcall_0_baked.lexical_depth;
        is_function_slot   = &n->u.node_pcall_0_baked.is_function;
        break;
    case 1:
        n = ALLOC_node_pcall_1_baked(NULL, 0, 0, 0, 0, args[0]);
        body_slot          = &n->u.node_pcall_1_baked.body;
        nslots_slot        = &n->u.node_pcall_1_baked.nslots;
        return_slot_slot   = &n->u.node_pcall_1_baked.return_slot;
        lexical_depth_slot = &n->u.node_pcall_1_baked.lexical_depth;
        is_function_slot   = &n->u.node_pcall_1_baked.is_function;
        break;
    case 2:
        n = ALLOC_node_pcall_2_baked(NULL, 0, 0, 0, 0, args[0], args[1]);
        body_slot          = &n->u.node_pcall_2_baked.body;
        nslots_slot        = &n->u.node_pcall_2_baked.nslots;
        return_slot_slot   = &n->u.node_pcall_2_baked.return_slot;
        lexical_depth_slot = &n->u.node_pcall_2_baked.lexical_depth;
        is_function_slot   = &n->u.node_pcall_2_baked.is_function;
        break;
    case 3:
        n = ALLOC_node_pcall_3_baked(NULL, 0, 0, 0, 0, args[0], args[1], args[2]);
        body_slot          = &n->u.node_pcall_3_baked.body;
        nslots_slot        = &n->u.node_pcall_3_baked.nslots;
        return_slot_slot   = &n->u.node_pcall_3_baked.return_slot;
        lexical_depth_slot = &n->u.node_pcall_3_baked.lexical_depth;
        is_function_slot   = &n->u.node_pcall_3_baked.is_function;
        break;
    default: {
        uint32_t base = push_call_args(args, argc);
        n = ALLOC_node_pcall_n_baked(NULL, 0, 0, 0, 0, base, argc);
        body_slot          = &n->u.node_pcall_n_baked.body;
        nslots_slot        = &n->u.node_pcall_n_baked.nslots;
        return_slot_slot   = &n->u.node_pcall_n_baked.return_slot;
        lexical_depth_slot = &n->u.node_pcall_n_baked.lexical_depth;
        is_function_slot   = &n->u.node_pcall_n_baked.is_function;
        break;
    }
    }
    register_pcall_fixup(body_slot, nslots_slot, return_slot_slot,
                         lexical_depth_slot, is_function_slot, pidx);
    return n;
}

// Build an address-of expression for `s` (the lvalue passed to a `var`
// parameter).  The runtime value is a (VALUE *) cast to int64.
static NODE *
mk_addr_of(struct sym *s, NODE *index, NODE *index2)
{
    switch (s->kind) {
    case SYM_GVAR: return ALLOC_node_addr_gvar(s->idx);
    case SYM_LVAR:
        // If the source itself is a `var` parameter, its slot already
        // holds the indirect pointer — pass it through.
        if (s->by_ref) return ALLOC_node_addr_passthru(s->idx);
        return ALLOC_node_addr_lvar(s->idx);
    case SYM_GARR:
        // Whole-array pass: bind to a var-array param.
        if (!index) {
            if (s->is_2d) pascal_error("can't pass 2D array '%s' as var", s->name);
            return ALLOC_node_addr_garr_base(s->idx);
        }
        if (s->is_2d) {
            if (!index2) pascal_error("var-arg 2D array '%s' needs both indices", s->name);
            return ALLOC_node_addr_aref2(s->idx, index, index2);
        }
        return ALLOC_node_addr_aref(s->idx, index);
    case SYM_LARR:
        if (!index) return ALLOC_node_addr_larr_base(s->idx);
        return ALLOC_node_addr_aref_local((uint32_t)s->idx, s->lo, index);
    case SYM_GREC:
        // Pass-by-reference of a global record — the record's first
        // cell address.
        return ALLOC_node_addr_gvar(s->idx);
    case SYM_LREC:
        return ALLOC_node_addr_lvar(s->idx);
    case SYM_VREC:
        // The slot itself already holds the indirect base pointer.
        return ALLOC_node_addr_passthru(s->idx);
    case SYM_VARR:
        // Forwarding a whole var-array is the existing pointer.
        if (!index) return ALLOC_node_addr_passthru(s->idx);
        // Element-of-var-array address: deref base + idx.  We don't
        // have a dedicated node for this; build it from the pointer +
        // (size of int64) * (idx - lo) at run time would need a new
        // node.  Reject for now.
        pascal_error("element-of-var-array address-take not supported");
    default:
        pascal_error("cannot pass '%s' as a var argument", s->name);
    }
    return NULL;
}

// Apply Pascal's argument promotion rules: int → real if the parameter
// is real.  Anything else (e.g. real → int) is rejected.
static NODE *
mk_call_with_promotion(int pidx, NODE **args, int *arg_types, uint32_t argc)
{
    struct pascal_proc *p = &PASCAL_PROCS[pidx];
    if ((int)argc != p->nparams)
        pascal_error("%s: expected %d args, got %u", p->name, p->nparams, argc);
    for (uint32_t i = 0; i < argc; i++) {
        if (p->param_by_ref[i]) continue;     // address already passed
        int want = p->param_type[i];
        int got  = arg_types[i];
        if (got == want) continue;
        if (want == PT_REAL && got == PT_INT) {
            args[i] = ALLOC_node_i2r(args[i]);
            continue;
        }
        if (want == PT_INT && got == PT_BOOL) continue; // ok
        if (want == PT_BOOL && got == PT_INT) continue; // ok
        pascal_error("%s: arg %u type mismatch: got %s, want %s",
                     p->name, i + 1, pt_name(got), pt_name(want));
    }
    return mk_pcall((uint32_t)pidx, args, argc);
}

// Parse a comma-separated argument list, choosing per-parameter
// whether to evaluate as expression (by-value) or as lvalue (by-ref).
static uint32_t
parse_typed_call_args(struct pascal_proc *p, NODE **out_nodes,
                      int *out_types, uint32_t max)
{
    uint32_t n = 0;
    expect(TK_LPAREN, "'('");
    if (tk != TK_RPAREN) {
        for (;;) {
            if (n >= max) pascal_error("too many arguments");
            bool by_ref   = (p && (int)n < p->nparams && p->param_by_ref[n]);
            bool is_array = (p && (int)n < p->nparams && p->param_is_array[n]);
            if (by_ref) {
                if (tk != TK_ID) pascal_error("var argument must be a variable");
                char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
                next_token();
                struct sym *s = sym_find(id);
                if (!s) pascal_error("undefined identifier '%s' in var-arg", id);
                NODE *idx1 = NULL, *idx2 = NULL;
                // Records: walk any number of `.field` for nested
                // sub-record passing.  Final result is an address at
                // (base + accumulated_offset).
                if ((s->kind == SYM_GREC || s->kind == SYM_LREC) && tk == TK_DOT) {
                    int extra = 0;
                    int rec_idx = s->rec_idx;
                    while (accept(TK_DOT)) {
                        if (tk != TK_ID) pascal_error("expected field name");
                        char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
                        next_token();
                        struct record_type *rt = &record_types[rec_idx];
                        struct record_field *f = NULL;
                        for (int i = 0; i < rt->nfields; i++)
                            if (strcmp(rt->fields[i].name, fname) == 0) { f = &rt->fields[i]; break; }
                        if (!f) pascal_error("no field '%s'", fname);
                        extra += f->offset;
                        if (f->type == PT_RECORD) rec_idx = f->rec_idx;
                    }
                    int slot = s->idx + extra;
                    out_nodes[n] = (s->kind == SYM_GREC)
                                 ? ALLOC_node_addr_gvar((uint32_t)slot)
                                 : ALLOC_node_addr_lvar((uint32_t)slot);
                    out_types[n] = PT_INT;
                    n++;
                } else {
                    if (!is_array && (s->kind == SYM_GARR || s->kind == SYM_LARR)) {
                        expect(TK_LBRACK, "'['");
                        idx1 = parse_expr();
                        if (s->is_2d) {
                            expect(TK_COMMA, "','");
                            idx2 = parse_expr();
                        }
                        expect(TK_RBRACK, "']'");
                    }
                    out_nodes[n] = mk_addr_of(s, idx1, idx2);
                    out_types[n] = s->type;
                    n++;
                }
            } else {
                TE e = te_expr();
                out_nodes[n] = e.n;
                out_types[n] = e.t;
                n++;
            }
            if (!accept(TK_COMMA)) break;
        }
    }
    expect(TK_RPAREN, "')'");
    return n;
}

// Backward-compatible wrapper: when the parser doesn't yet know the
// callee (e.g. raising into an unknown built-in), fall back to plain
// expression evaluation for every argument.
static uint32_t
parse_call_args(NODE **out, uint32_t max)
{
    int types[16];
    return parse_typed_call_args(NULL, out, types, max);
}

// ---------------------------------------------------------------------------
// Built-in calls recognized by the parser.  Returns NULL if `name` is
// not a recognized built-in.  Each builtin compiles into a node tree;
// they don't go through proc_call.
// ---------------------------------------------------------------------------

// Parser-recognized "magic" functions.  Returns {NULL, …} to signal
// "not a built-in"; otherwise consumes the call expression and returns
// the typed result.
static TE
te_builtin_call_in_expr(const char *name)
{
    if (strcmp(name, "abs") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_REAL) return (TE){ ALLOC_node_rabs(e.n), PT_REAL, -1 };
        return (TE){ ALLOC_node_if(ALLOC_node_lt(e.n, mk_int(0)),
                                   ALLOC_node_neg(e.n), e.n),
                     PT_INT };
    }
    if (strcmp(name, "sqr") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_REAL) return (TE){ ALLOC_node_rmul(e.n, e.n), PT_REAL, -1 };
        return (TE){ ALLOC_node_mul(e.n, e.n), PT_INT, -1 };
    }
    if (strcmp(name, "sqrt") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_promote_real(te_expr());
        expect(TK_RPAREN, "')'");
        return (TE){ ALLOC_node_rsqrt(e.n), PT_REAL, -1 };
    }
    if (strcmp(name, "trunc") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t != PT_REAL) e = te_promote_real(e);
        return (TE){ ALLOC_node_r2i_trunc(e.n), PT_INT, -1 };
    }
    if (strcmp(name, "round") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t != PT_REAL) e = te_promote_real(e);
        return (TE){ ALLOC_node_r2i_round(e.n), PT_INT, -1 };
    }
    if (strcmp(name, "succ") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_REAL) pascal_error("succ requires integer");
        return (TE){ ALLOC_node_add(e.n, mk_int(1)), PT_INT, -1 };
    }
    if (strcmp(name, "pred") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_REAL) pascal_error("pred requires integer");
        return (TE){ ALLOC_node_sub(e.n, mk_int(1)), PT_INT, -1 };
    }
    if (strcmp(name, "ord") == 0 || strcmp(name, "chr") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        return (TE){ e.n, PT_INT, -1 };       // identity for int/bool
    }
    if (strcmp(name, "exceptionmessage") == 0
        || strcmp(name, "exceptobject") == 0) {
        // No arglist required.
        if (accept(TK_LPAREN)) expect(TK_RPAREN, "')'");
        return (TE){ ALLOC_node_exc_msg(), PT_STR, -1 };
    }
    if (strcmp(name, "eof") == 0) {
        expect(TK_LPAREN, "'('");
        if (tk != TK_ID) pascal_error("eof() expects a file variable");
        char fid[64]; strncpy(fid, tk_id, 63); fid[63] = 0;
        next_token();
        expect(TK_RPAREN, "')'");
        struct sym *fs = sym_find(fid);
        if (!fs || fs->type != PT_FILE) pascal_error("'%s' is not a file", fid);
        return (TE){ ALLOC_node_file_eof(mk_addr_of(fs, NULL, NULL)), PT_BOOL, -1 };
    }
    if (strcmp(name, "length") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_STR)    return (TE){ ALLOC_node_str_length(e.n), PT_INT, -1 };
        if (e.t == PT_DYNARR) return (TE){ ALLOC_node_dynarr_length(e.n), PT_INT, -1 };
        pascal_error("length() requires a string or dynamic array");
    }
    if (strcmp(name, "low") == 0) {
        // low(arr) — for static arrays it's the declared lo, but the
        // common use with open-array params (PT_DYNARR) is a literal 0.
        // We accept any of: a static-array name (returns lo), a
        // dynarr expression (returns 0).
        expect(TK_LPAREN, "'('");
        // Peek for identifier first — static-array case wants the
        // sym's lo before evaluating, since te_expr would reject a
        // bare static-array name.
        if (tk == TK_ID) {
            char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
            const char *peek = src;
            int peek_line = line_no;
            next_token();
            if (tk == TK_RPAREN) {
                struct sym *s = sym_find(id);
                if (s && (s->kind == SYM_GARR || s->kind == SYM_LARR || s->kind == SYM_VARR)) {
                    next_token();
                    return (TE){ mk_int(s->lo), PT_INT, -1 };
                }
                // Fall through — id wasn't a static array; rewind and
                // fall into expression path.
                src = peek; line_no = peek_line;
                tk = TK_ID; strncpy(tk_id, id, 63); tk_id[63] = 0;
            } else {
                src = peek; line_no = peek_line;
                tk = TK_ID; strncpy(tk_id, id, 63); tk_id[63] = 0;
            }
        }
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_DYNARR) return (TE){ mk_int(0), PT_INT, -1 };
        pascal_error("low() requires an array");
    }
    if (strcmp(name, "high") == 0) {
        // high(arr) — for static arrays it's the declared hi; for
        // dynarrs (open arrays) it's length(a) - 1.
        expect(TK_LPAREN, "'('");
        if (tk == TK_ID) {
            char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
            const char *peek = src;
            int peek_line = line_no;
            next_token();
            if (tk == TK_RPAREN) {
                struct sym *s = sym_find(id);
                if (s && (s->kind == SYM_GARR || s->kind == SYM_LARR || s->kind == SYM_VARR)) {
                    next_token();
                    return (TE){ mk_int(s->hi), PT_INT, -1 };
                }
                src = peek; line_no = peek_line;
                tk = TK_ID; strncpy(tk_id, id, 63); tk_id[63] = 0;
            } else {
                src = peek; line_no = peek_line;
                tk = TK_ID; strncpy(tk_id, id, 63); tk_id[63] = 0;
            }
        }
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_DYNARR)
            return (TE){ ALLOC_node_sub(ALLOC_node_dynarr_length(e.n), mk_int(1)), PT_INT, -1 };
        pascal_error("high() requires an array");
    }
    if (strcmp(name, "copy") == 0) {
        expect(TK_LPAREN, "'('");
        TE s = te_expr(); expect(TK_COMMA, "','");
        TE st = te_expr(); expect(TK_COMMA, "','");
        TE cn = te_expr(); expect(TK_RPAREN, "')'");
        if (s.t == PT_INT) s.n = ALLOC_node_chr_to_str(s.n);
        return (TE){ ALLOC_node_str_copy(s.n, st.n, cn.n), PT_STR, -1 };
    }
    if (strcmp(name, "pos") == 0) {
        expect(TK_LPAREN, "'('");
        TE sub = te_expr(); expect(TK_COMMA, "','");
        TE s   = te_expr(); expect(TK_RPAREN, "')'");
        // Allow single-char literal as the needle (`pos('x', s)`).
        if (sub.t == PT_INT) sub.n = ALLOC_node_chr_to_str(sub.n);
        if (s.t   == PT_INT) s.n   = ALLOC_node_chr_to_str(s.n);
        return (TE){ ALLOC_node_str_pos(sub.n, s.n), PT_INT, -1 };
    }
    if (strcmp(name, "inttostr") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr(); expect(TK_RPAREN, "')'");
        if (e.t != PT_INT && e.t != PT_BOOL) pascal_error("inttostr requires integer");
        return (TE){ ALLOC_node_int_to_str(e.n), PT_STR, -1 };
    }
    if (strcmp(name, "strtoint") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr(); expect(TK_RPAREN, "')'");
        if (e.t != PT_STR) pascal_error("strtoint requires string");
        return (TE){ ALLOC_node_str_to_int(e.n), PT_INT, -1 };
    }
    if (strcmp(name, "strtofloat") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr(); expect(TK_RPAREN, "')'");
        if (e.t != PT_STR) pascal_error("strtofloat requires string");
        return (TE){ ALLOC_node_str_to_real(e.n), PT_REAL, -1 };
    }
    if (strcmp(name, "floattostr") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr(); expect(TK_RPAREN, "')'");
        if (e.t != PT_REAL) e = te_promote_real(e);
        return (TE){ ALLOC_node_real_to_str(e.n), PT_STR, -1 };
    }
    if (strcmp(name, "odd") == 0) {
        expect(TK_LPAREN, "'('");
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        if (e.t == PT_REAL) pascal_error("odd requires integer");
        return (TE){ ALLOC_node_eq(ALLOC_node_mod(e.n, mk_int(2)), mk_int(1)), PT_BOOL, -1 };
    }
    return (TE){ NULL, PT_INT, -1 };
}

static NODE *
parse_inc_dec(const char *name, bool is_inc)
{
    expect(TK_LPAREN, "'('");
    if (tk != TK_ID) pascal_error("expected variable in %s", name);
    char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
    next_token();
    struct sym *s = sym_find(id);
    if (!s) pascal_error("undefined variable '%s'", id);
    NODE *idx1 = NULL, *idx2 = NULL;
    if (s->kind == SYM_GARR || s->kind == SYM_LARR || s->kind == SYM_VARR) {
        expect(TK_LBRACK, "'['");
        idx1 = parse_expr();
        if (s->kind == SYM_GARR && s->is_2d) {
            expect(TK_COMMA, "','");
            idx2 = parse_expr();
        }
        expect(TK_RBRACK, "']'");
    }
    NODE *delta = mk_int(1);
    if (accept(TK_COMMA)) {
        delta = parse_expr();
    }
    expect(TK_RPAREN, "')'");
    if (s->type == PT_REAL) pascal_error("inc/dec requires integer");
    TE cur = te_get(s, idx1, idx2);
    NODE *nv = is_inc ? ALLOC_node_add(cur.n, delta)
                      : ALLOC_node_sub(cur.n, delta);
    return mk_set_typed(s, idx1, idx2, nv);
}

static NODE *
parse_write_call(bool is_writeln)
{
    NODE *seq = NULL;
    NODE *file_addr = NULL;     // non-NULL → emit f-variants

    if (accept(TK_LPAREN)) {
        // Sniff first arg: if it's a file variable, redirect.
        if (tk == TK_ID) {
            struct sym *fs = sym_find(tk_id);
            if (fs && fs->type == PT_FILE) {
                next_token();
                file_addr = mk_addr_of(fs, NULL, NULL);
                if (!accept(TK_COMMA) && tk != TK_RPAREN) {
                    pascal_error("expected ',' or ')' after file");
                }
            }
        }
        if (tk != TK_RPAREN) for (;;) {
            NODE *piece;
            if (tk == TK_STR) {
                char *s = malloc(strlen(tk_str) + 1);
                strcpy(s, tk_str);
                next_token();
                piece = file_addr ? ALLOC_node_fwrite_str(file_addr, s)
                                  : ALLOC_node_write_str(s);
            } else {
                TE e = te_expr();
                int width = -1, prec = -1;
                if (accept(TK_COLON)) {
                    if (tk != TK_INT) pascal_error("expected width after ':' at line %d", line_no);
                    width = (int)tk_int;
                    next_token();
                    if (accept(TK_COLON)) {
                        if (tk != TK_INT) pascal_error("expected precision after second ':' at line %d", line_no);
                        prec = (int)tk_int;
                        next_token();
                    }
                }
                if (file_addr) {
                    // For files we don't support width/prec specifiers
                    // (Free Pascal does, but they would be a chunk of
                    // node-DEF noise).
                    if (e.t == PT_STR)        piece = ALLOC_node_fwrite_strv(file_addr, e.n);
                    else if (e.t == PT_REAL)  piece = ALLOC_node_fwrite_real(file_addr, e.n);
                    else                      piece = ALLOC_node_fwrite_int (file_addr, e.n);
                } else if (e.t == PT_STR) {
                    piece = ALLOC_node_write_strv(e.n);
                } else if (e.t == PT_REAL || prec >= 0) {
                    if (e.t != PT_REAL) e = te_promote_real(e);
                    if (prec >= 0)      piece = ALLOC_node_write_real_wp(e.n, (uint32_t)width, (uint32_t)prec);
                    else if (width >= 0)piece = ALLOC_node_write_real_w (e.n, (uint32_t)width);
                    else                piece = ALLOC_node_write_real   (e.n);
                } else {
                    if (width >= 0) piece = ALLOC_node_write_int_w(e.n, (uint32_t)width);
                    else            piece = ALLOC_node_write_int  (e.n);
                }
            }
            seq = mk_seq(seq, piece);
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_RPAREN, "')'");
    }
    if (is_writeln) {
        seq = mk_seq(seq, file_addr ? ALLOC_node_fwriteln(file_addr)
                                    : ALLOC_node_writeln());
    }
    return seq ? seq : ALLOC_node_int(0);
}

static NODE *node_read_int_dummy(void);  // forward — defined below

// ---------------------------------------------------------------------------
// Expressions.
// ---------------------------------------------------------------------------

// Forward declarations needed by te_factor (built-in calls and
// procedure calls now build TE-aware trees).
static TE       te_builtin_call_in_expr(const char *name);
static NODE *   mk_call_with_promotion(int pidx, NODE **args, int *arg_types, uint32_t argc);
static uint32_t parse_typed_call_args(struct pascal_proc *p, NODE **out_nodes,
                                      int *out_types, uint32_t max);

// Build a TE for reading the value of a symbol `s`.  Optional `index`
// (and `index2`) are required for arrays; pass NULL otherwise.  Type
// of the result follows the symbol's declared element type.
// Is this local-symbol reference reaching into an outer (enclosing)
// scope?  Returns the depth (>0) for non-local access; 0 for innermost.
static inline uint32_t
sym_outer_depth(struct sym *s)
{
    return (s->depth < current_depth) ? (uint32_t)s->depth : 0;
}

static TE
te_get(struct sym *s, NODE *index, NODE *index2)
{
    switch (s->kind) {
    case SYM_GVAR: return (TE){ ALLOC_node_gref(s->idx), s->type, -1 };
    case SYM_LVAR:
        if (s->depth < current_depth) {
            if (s->by_ref)
                return (TE){ ALLOC_node_uvar_ref((uint32_t)s->depth, s->idx), s->type, -1 };
            return (TE){ ALLOC_node_uref((uint32_t)s->depth, s->idx), s->type, -1 };
        }
        if (s->by_ref) return (TE){ ALLOC_node_var_lref(s->idx), s->type, -1 };
        return (TE){ ALLOC_node_lref(s->idx), s->type, -1 };
    case SYM_GARR:
        if (s->is_2d) {
            if (!index || !index2) pascal_error("2D array '%s' needs both indices", s->name);
            return (TE){ ALLOC_node_aref2(s->idx, index, index2), s->type, -1 };
        }
        if (!index) pascal_error("array '%s' needs index", s->name);
        return (TE){ ALLOC_node_aref(s->idx, index), s->type, -1 };
    case SYM_LARR:
        if (!index) pascal_error("local array '%s' needs index", s->name);
        if (s->depth < current_depth)
            return (TE){ ALLOC_node_uaref_local((uint32_t)s->depth, (uint32_t)s->idx, s->lo, index), s->type, -1 };
        return (TE){ ALLOC_node_aref_local((uint32_t)s->idx, s->lo, index), s->type, -1 };
    case SYM_VARR:
        if (!index) pascal_error("var-array '%s' needs index", s->name);
        // var-array params reaching into an enclosing scope aren't
        // currently implemented (would need a uref + indirect deref).
        if (s->depth < current_depth)
            pascal_error("var-array '%s' from enclosing scope not supported", s->name);
        return (TE){ ALLOC_node_var_aref((uint32_t)s->idx, s->lo, index), s->type, -1 };
    case SYM_CONST:
        if (s->type == PT_STR) {
            // String constants: emit a literal node with the baked
            // pointer.  ALLOC_node_str_lit takes a const char * which
            // is exactly what s->cval holds (cast through int64).
            return (TE){ ALLOC_node_str_lit((const char *)(uintptr_t)s->cval), PT_STR, -1 };
        }
        return (TE){ mk_int(s->cval), PT_INT, -1 };
    case SYM_RCONST: {
        union { int64_t i; double d; } u; u.i = s->cval;
        return (TE){ mk_real(u.d), PT_REAL, -1 };
    }
    default:
        pascal_error("cannot read '%s'", s->name);
    }
    return (TE){ NULL, PT_INT, -1 };
}

// Build a node that writes `val` (already a NODE*, of the right type
// for `s`) to the storage that `s` names.
static NODE *
mk_set_typed(struct sym *s, NODE *index, NODE *index2, NODE *val)
{
    switch (s->kind) {
    case SYM_GVAR: return ALLOC_node_gset(s->idx, val);
    case SYM_LVAR:
        if (s->depth < current_depth) {
            if (s->by_ref)
                return ALLOC_node_uvar_set((uint32_t)s->depth, s->idx, val);
            return ALLOC_node_uset((uint32_t)s->depth, s->idx, val);
        }
        if (s->by_ref) return ALLOC_node_var_lset(s->idx, val);
        return ALLOC_node_lset(s->idx, val);
    case SYM_GARR:
        if (s->is_2d) {
            if (!index || !index2) pascal_error("2D array '%s' needs both indices", s->name);
            return ALLOC_node_aset2(s->idx, index, index2, val);
        }
        if (!index) pascal_error("array '%s' needs index", s->name);
        return ALLOC_node_aset(s->idx, index, val);
    case SYM_LARR:
        if (!index) pascal_error("local array '%s' needs index", s->name);
        if (s->depth < current_depth)
            return ALLOC_node_uaset_local((uint32_t)s->depth, (uint32_t)s->idx, s->lo, index, val);
        return ALLOC_node_aset_local((uint32_t)s->idx, s->lo, index, val);
    case SYM_VARR:
        if (!index) pascal_error("var-array '%s' needs index", s->name);
        return ALLOC_node_var_aset((uint32_t)s->idx, s->lo, index, val);
    default:
        pascal_error("cannot assign to '%s'", s->name);
    }
    return NULL;
}

static TE
te_factor(void)
{
    if (tk == TK_INT) {
        int64_t v = tk_int;
        next_token();
        return (TE){ mk_int(v), PT_INT, -1 };
    }
    if (tk == TK_RNUM) {
        double v = tk_real;
        next_token();
        return (TE){ mk_real(v), PT_REAL, -1 };
    }
    if (tk == TK_STR) {
        size_t len = strlen(tk_str);
        if (len == 1) {
            int ch = (unsigned char)tk_str[0];
            next_token();
            return (TE){ mk_int(ch), PT_INT, -1 };
        }
        // Multi-character literal → string value.  Allocate a
        // permanent copy under libgc; the parser tree owns the
        // pointer indirectly through the AST node.
        char *buf = (char *)GC_malloc_atomic(len + 1);
        if (!buf) pascal_error("out of memory");
        memcpy(buf, tk_str, len + 1);
        next_token();
        return (TE){ ALLOC_node_str_lit(buf), PT_STR, -1 };
    }
    if (tk == TK_TRUE)  { next_token(); return (TE){ ALLOC_node_true(),  PT_BOOL, -1 }; }
    if (tk == TK_FALSE) { next_token(); return (TE){ ALLOC_node_false(), PT_BOOL, -1 }; }
    if (tk == TK_NIL)   { next_token(); return (TE){ ALLOC_node_nil(),   PT_POINTER, -1 }; }
    if (accept(TK_INHERITED)) {
        // Same logic as the statement-form, but builds a TE for use
        // in expression contexts (e.g. `Result := inherited f(x);`).
        if (current_method_class_idx < 0)
            pascal_error("'inherited' only valid inside a method");
        int parent = class_types[current_method_class_idx].parent_idx;
        if (parent < 0) pascal_error("'inherited' used in a class with no parent");
        char mname[64];
        if (tk == TK_ID) { strncpy(mname, tk_id, 63); mname[63] = 0; next_token(); }
        else { strncpy(mname, current_method_name_buf, 63); mname[63] = 0; }
        struct method_entry *me = NULL;
        int walk = parent;
        while (walk >= 0) {
            struct class_type *wc = &class_types[walk];
            for (int i = 0; i < wc->nmethods; i++)
                if (strcmp(wc->methods[i].name, mname) == 0
                    && (wc->methods[i].is_virtual || wc->methods[i].proc_idx >= 0)) { me = &wc->methods[i]; break; }
            if (me) break;
            walk = wc->parent_idx;
        }
        if (!me) pascal_error("no inherited method '%s'", mname);
        NODE *args[16]; uint32_t n = 1;
        args[0] = ALLOC_node_lref(0);
        if (accept(TK_LPAREN)) {
            if (tk != TK_RPAREN) for (;;) {
                if (n >= 16) pascal_error("too many args");
                args[n++] = te_expr().n;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RPAREN, "')'");
        }
        NODE *call = mk_pcall((uint32_t)me->proc_idx, args, n);
        int t = method_return_type(me);
        return (TE){ call, t, -1 };
    }
    if (accept(TK_AT)) {
        // @procname — produce a procedure value (the proc's index).
        if (tk != TK_ID) pascal_error("expected procedure name after '@'");
        struct sym *s = sym_find(tk_id);
        if (!s || s->kind != SYM_PROC)
            pascal_error("'%s' is not a procedure or function", tk_id);
        next_token();
        return (TE){ mk_int(s->idx), PT_PROC, -1 };
    }
    if (accept(TK_LPAREN)) {
        TE e = te_expr();
        expect(TK_RPAREN, "')'");
        // Postfix `.field` / `.method` on a parenthesised
        // class-typed expression (e.g. `(p as TDog).speak`).
        while (e.t == PT_POINTER && e.class_idx >= 0 && tk == TK_DOT) {
            next_token();
            if (tk != TK_ID) pascal_error("expected field/method name after '.'");
            char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
            next_token();
            // Save the obj into a temp by using a local slot — but we
            // don't have a slot allocator here, so re-evaluating is
            // OK only if the expression is pure.  For typical cases
            // (`(p as T).field`, `obj.field`) it is.
            // Property first
            struct property_entry *pe = NULL;
            int walk = e.class_idx;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nprops; i++)
                    if (strcmp(wc->properties[i].name, fname) == 0) { pe = &wc->properties[i]; break; }
                if (pe) break;
                walk = wc->parent_idx;
            }
            if (pe) {
                if (pe->read_field_offset >= 0) {
                    e = (TE){ ALLOC_node_ptr_field_ref(e.n, (uint32_t)pe->read_field_offset),
                              pe->type, -1 };
                } else if (pe->read_method_idx >= 0) {
                    struct method_entry *gme = &class_types[e.class_idx].methods[pe->read_method_idx];
                    NODE *args[1] = { e.n };
                    e = (TE){ mk_pcall((uint32_t)gme->proc_idx, args, 1), pe->type, -1 };
                } else pascal_error("property '%s' is write-only", fname);
                continue;
            }
            // Method
            struct method_entry *me = NULL;
            walk = e.class_idx;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nmethods; i++)
                    if (strcmp(wc->methods[i].name, fname) == 0
                        && (wc->methods[i].is_virtual || wc->methods[i].proc_idx >= 0)) { me = &wc->methods[i]; break; }
                if (me) break;
                walk = wc->parent_idx;
            }
            if (me) {
                check_access(me->decl_class, me->vis, "method", me->name);
                NODE *args[16]; uint32_t n = 1;
                args[0] = e.n;
                if (accept(TK_LPAREN)) {
                    if (tk != TK_RPAREN) for (;;) {
                        if (n >= 16) pascal_error("too many args");
                        args[n++] = te_expr().n;
                        if (!accept(TK_COMMA)) break;
                    }
                    expect(TK_RPAREN, "')'");
                }
                NODE *call;
                int rt;
                if (me->is_virtual && me->vtable_slot >= 0) {
                    uint32_t base = push_call_args(&args[1], n - 1);
                    call = ALLOC_node_vcall(args[0], (uint32_t)me->vtable_slot, base, n);
                } else {
                    call = mk_pcall((uint32_t)me->proc_idx, args, n);
                }
                rt = method_return_type(me);
                e = (TE){ call, rt, -1 };
                continue;
            }
            // Field
            struct record_type *rt = &record_types[class_types[e.class_idx].rec_idx];
            struct record_field *f = NULL;
            for (int i = 0; i < rt->nfields; i++)
                if (strcmp(rt->fields[i].name, fname) == 0) { f = &rt->fields[i]; break; }
            if (!f) pascal_error("no field/method '%s' in '%s'", fname, rt->name);
            check_access(f->decl_class, f->vis, "field", f->name);
            e = (TE){ ALLOC_node_ptr_field_ref(e.n, (uint32_t)f->offset), f->type, -1 };
        }
        return e;
    }
    if (accept(TK_LBRACK)) {
        // Set literal `[v, lo..hi, …]`.  Const-only — every element
        // must be an integer literal (with optional sign).
        uint64_t bits = 0;
        if (tk != TK_RBRACK) for (;;) {
            int sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            else if (accept(TK_PLUS)) sign = 1;
            if (tk != TK_INT) pascal_error("set literal element must be an integer literal at line %d", line_no);
            int64_t v1 = sign * tk_int; next_token();
            int64_t v2 = v1;
            if (accept(TK_DOTDOT)) {
                int s2 = 1;
                if (accept(TK_MINUS)) s2 = -1;
                else if (accept(TK_PLUS)) s2 = 1;
                if (tk != TK_INT) pascal_error("set range upper bound must be integer literal");
                v2 = s2 * tk_int; next_token();
            }
            for (int64_t i = v1; i <= v2; i++) {
                if (i < 0 || i > 63) pascal_error("set element %ld out of [0..63]", (long)i);
                bits |= (1ULL << i);
            }
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_RBRACK, "']'");
        return (TE){ ALLOC_node_set_lit(bits), PT_SET, -1 };
    }
    if (accept(TK_NOT)) {
        TE e = te_factor();
        return (TE){ ALLOC_node_not(e.n), PT_BOOL, -1 };
    }
    if (accept(TK_MINUS)) {
        TE e = te_factor();
        if (e.t == PT_REAL) return (TE){ ALLOC_node_rneg(e.n), PT_REAL, -1 };
        return (TE){ ALLOC_node_neg(e.n), PT_INT, -1 };
    }
    if (accept(TK_PLUS)) {
        return te_factor();
    }
    if (tk == TK_ID) {
        char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
        next_token();

        // `with`-stack lookup: bare ID inside `with rec do …` → field
        // of rec, if rec has a field by that name.
        {
            struct sym *rs = NULL;
            struct record_field *f = with_lookup_field(id, &rs);
            if (f) return te_field_get(rs, f);
        }

        // Inside a method body, bare names that match a class field
        // or property auto-resolve to Self.<name>.  But only if the
        // name doesn't shadow a local (param, loop variable, …) or
        // global — otherwise `x` in a method body whose class also has
        // a property `X` would get the property instead of the param.
        if (current_method_class_idx >= 0
            && sym_find_local(id) == NULL
            && sym_find_global(id) == NULL) {
            int walk = current_method_class_idx;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nprops; i++) {
                    if (strcmp(wc->properties[i].name, id) == 0) {
                        struct property_entry *pe = &wc->properties[i];
                        NODE *self_load = ALLOC_node_lref(0);
                        if (pe->read_field_offset >= 0)
                            return (TE){ ALLOC_node_ptr_field_ref(self_load, (uint32_t)pe->read_field_offset),
                                         pe->type, -1 };
                        if (pe->read_method_idx >= 0) {
                            struct method_entry *gme = &class_types[walk].methods[pe->read_method_idx];
                            NODE *args[1] = { self_load };
                            return (TE){ mk_pcall((uint32_t)gme->proc_idx, args, 1), pe->type, -1 };
                        }
                    }
                }
                walk = wc->parent_idx;
            }
            walk = current_method_class_idx;
            while (walk >= 0) {
                struct record_type *rt = &record_types[class_types[walk].rec_idx];
                for (int i = 0; i < rt->nfields; i++) {
                    if (strcmp(rt->fields[i].name, id) == 0) {
                        check_access(rt->fields[i].decl_class, rt->fields[i].vis, "field", rt->fields[i].name);
                        NODE *self_load = ALLOC_node_lref(0);
                        return (TE){ ALLOC_node_ptr_field_ref(self_load, (uint32_t)rt->fields[i].offset),
                                     rt->fields[i].type, -1 };
                    }
                }
                walk = class_types[walk].parent_idx;
            }
        }

        // Built-in pure functions (abs, sqr, sqrt, sin, ...).
        TE bi = te_builtin_call_in_expr(id);
        if (bi.n) return bi;

        // function name self-reference inside its body → recursive call.
        if (current_proc && strcmp(current_proc->name, id) == 0
            && current_proc->is_function) {
            NODE *args[16]; int arg_t[16]; uint32_t n = 0;
            if (tk == TK_LPAREN) n = parse_typed_call_args(current_proc, args, arg_t, 16);
            NODE *call = mk_call_with_promotion(current_proc_idx, args, arg_t, n);
            return (TE){ call, current_proc->return_type, -1 };
        }
        // `Result` (modern Pascal) — alias for the return slot.  Reading
        // it gives the current draft return value.
        if (current_proc && current_proc->is_function && strcmp(id, "result") == 0) {
            return (TE){ ALLOC_node_lref((uint32_t)current_proc->return_slot),
                         current_proc->return_type };
        }

        struct sym *s = sym_find(id);
        if (s && s->kind == SYM_PROC) {
            struct pascal_proc *p = &PASCAL_PROCS[s->idx];
            NODE *args[16]; int arg_t[16]; uint32_t n = 0;
            if (tk == TK_LPAREN) n = parse_typed_call_args(p, args, arg_t, 16);
            NODE *call = mk_call_with_promotion(s->idx, args, arg_t, n);
            return (TE){ call, p->is_function ? p->return_type : PT_INT, -1 };
        }
        // `T.Create(args)` — class-side constructor invocation.
        if (s && s->kind == SYM_CLASSTYPE && tk == TK_DOT) {
            next_token();
            if (tk != TK_ID) pascal_error("expected method name after '.'");
            char mname[64]; strncpy(mname, tk_id, 63); mname[63] = 0;
            next_token();
            int walk = s->idx;
            struct method_entry *me = NULL;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nmethods; i++)
                    if (strcmp(wc->methods[i].name, mname) == 0
                        && (wc->methods[i].is_virtual || wc->methods[i].proc_idx >= 0)) { me = &wc->methods[i]; break; }
                if (me) break;
                walk = wc->parent_idx;
            }
            if (!me) pascal_error("no method '%s' on class", mname);
            NODE *args[16]; uint32_t n = 1;
            if (accept(TK_LPAREN)) {
                if (tk != TK_RPAREN) for (;;) {
                    if (n >= 16) pascal_error("too many args");
                    args[n++] = te_expr().n;
                    if (!accept(TK_COMMA)) break;
                }
                expect(TK_RPAREN, "')'");
            }
            if (me->is_class) {
                NODE *real_args[16]; uint32_t real_n = 0;
                for (uint32_t i = 1; i < n; i++) real_args[real_n++] = args[i];
                NODE *call = mk_pcall((uint32_t)me->proc_idx, real_args, real_n);
                int rt = method_return_type(me);
                return (TE){ call, rt, -1 };
            }
            if (!me->is_constructor) pascal_error("class-side call must be a constructor or class method");
            int slots = record_types[class_types[s->idx].rec_idx].total_slots;
            args[0] = ALLOC_node_new_object((uint32_t)slots, (uint32_t)s->idx);
            NODE *call = mk_pcall((uint32_t)me->proc_idx, args, n);
            return (TE){ call, PT_POINTER, s->idx };
        }

        if (!s) pascal_error("undefined identifier '%s' at line %d", id, line_no);

        // Procedure-typed variable used as a callable in expression
        // position: indirect call.  We allocate the args via the
        // generic call_n machinery (no var-param support).
        if (s->type == PT_PROC && tk == TK_LPAREN) {
            NODE *args[16]; int arg_t[16]; uint32_t n;
            n = parse_typed_call_args(NULL, args, arg_t, 16);
            (void)arg_t;
            uint32_t base = push_call_args(args, n);
            NODE *fn = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                             : ALLOC_node_lref(s->idx);
            return (TE){ ALLOC_node_pcall_ind(fn, base, n), PT_INT, -1 };
        }

        if (s->kind == SYM_GARR || s->kind == SYM_LARR || s->kind == SYM_VARR) {
            expect(TK_LBRACK, "'['");
            NODE *idxn = parse_expr();
            NODE *idx2 = NULL;
            if (s->kind == SYM_GARR && s->is_2d) {
                expect(TK_COMMA, "','");
                idx2 = parse_expr();
            }
            expect(TK_RBRACK, "']'");
            return te_get(s, idxn, idx2);
        }
        // p^.field or obj.field/method (auto-deref for class pointers).
        if (s->type == PT_POINTER && (tk == TK_HAT || (s->class_idx >= 0 && tk == TK_DOT))) {
            if (tk == TK_HAT) next_token();
            expect(TK_DOT, "'.'");
            if (tk != TK_ID) pascal_error("expected field/method name after '.'");
            char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
            next_token();
            // Properties (read) — check first so a field-shadowing
            // property is found before the underlying field.
            if (s->class_idx >= 0) {
                struct class_type *cct = &class_types[s->class_idx];
                struct property_entry *pe = NULL;
                int walk = s->class_idx;
                while (walk >= 0) {
                    struct class_type *wc = &class_types[walk];
                    for (int i = 0; i < wc->nprops; i++)
                        if (strcmp(wc->properties[i].name, fname) == 0) { pe = &wc->properties[i]; break; }
                    if (pe) break;
                    walk = wc->parent_idx;
                }
                if (pe) {
                    NODE *self_load = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                            : ALLOC_node_lref(s->idx);
                    if (pe->read_field_offset >= 0) {
                        return (TE){ ALLOC_node_ptr_field_ref(self_load, (uint32_t)pe->read_field_offset),
                                     pe->type };
                    }
                    if (pe->read_method_idx >= 0) {
                        struct method_entry *gme = &class_types[s->class_idx].methods[pe->read_method_idx];
                        if (gme->is_virtual && gme->vtable_slot >= 0) {
                            uint32_t base = push_call_args(NULL, 0);
                            return (TE){ ALLOC_node_vcall(self_load, (uint32_t)gme->vtable_slot, base, 1),
                                         pe->type };
                        }
                        NODE *args[1] = { self_load };
                        return (TE){ mk_pcall((uint32_t)gme->proc_idx, args, 1), pe->type, -1 };
                    }
                    pascal_error("property '%s' is write-only", fname);
                }
                (void)cct;
            }
            // Class methods take precedence — try those first if it's a class.
            if (s->class_idx >= 0) {
                struct class_type *ct = &class_types[s->class_idx];
                struct method_entry *me = NULL;
                int  walk = s->class_idx;
                while (walk >= 0) {
                    struct class_type *wc = &class_types[walk];
                    for (int i = 0; i < wc->nmethods; i++) {
                        // Match by name; accept methods that are
                        // virtual (proc_idx may be -1 if abstract —
                        // the runtime vtable will dispatch) or that
                        // have a body proc_idx.
                        if (strcmp(wc->methods[i].name, fname) == 0
                            && (wc->methods[i].is_virtual
                                || wc->methods[i].proc_idx >= 0)) {
                            me = &wc->methods[i];
                            break;
                        }
                    }
                    if (me) break;
                    walk = wc->parent_idx;
                }
                if (me) {
                    check_access(me->decl_class, me->vis, "method", me->name);
                    // obj.method(args) — static or virtual dispatch
                    // depending on the method's declaration.  Virtual
                    // methods go through node_vcall (reads vtable from
                    // the object's slot 0); statics use mk_pcall.
                    NODE *self_load = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                            : ALLOC_node_lref(s->idx);
                    NODE *args[16]; uint32_t n = 1;
                    args[0] = self_load;
                    if (accept(TK_LPAREN)) {
                        if (tk != TK_RPAREN) for (;;) {
                            if (n >= 16) pascal_error("too many args");
                            args[n++] = te_expr().n;
                            if (!accept(TK_COMMA)) break;
                        }
                        expect(TK_RPAREN, "')'");
                    }
                    NODE *call;
                    int rt_type;
                    if (me->is_virtual && me->vtable_slot >= 0) {
                        uint32_t base = push_call_args(&args[1], n - 1);
                        call = ALLOC_node_vcall(self_load, (uint32_t)me->vtable_slot, base, n);
                        rt_type = method_return_type(me);
                    } else {
                        call = mk_pcall((uint32_t)me->proc_idx, args, n);
                        rt_type = method_return_type(me);
                    }
                    return (TE){ call, rt_type, -1 };
                    (void)ct;
                }
            }
            // Field access.
            struct record_type *rt = &record_types[s->rec_idx];
            for (int i = 0; i < rt->nfields; i++) {
                if (strcmp(rt->fields[i].name, fname) == 0) {
                    check_access(rt->fields[i].decl_class, rt->fields[i].vis, "field", rt->fields[i].name);
                    NODE *base = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                       : ALLOC_node_lref(s->idx);
                    return (TE){ ALLOC_node_ptr_field_ref(base, (uint32_t)rt->fields[i].offset),
                                 rt->fields[i].type };
                }
            }
            pascal_error("no field/method '%s' in '%s'", fname, rt->name);
        }
        // Dynamic array element a[i].
        if (s->type == PT_DYNARR && tk == TK_LBRACK) {
            next_token();
            NODE *idx = parse_expr();
            expect(TK_RBRACK, "']'");
            NODE *base = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                : ALLOC_node_lref(s->idx);
            return (TE){ ALLOC_node_dynarr_get(base, idx), s->rec_idx, -1 };
        }
        // String element s[i] — read-only.  String variables are
        // ordinary scalar slots whose value is a char *.
        if (s->type == PT_STR && tk == TK_LBRACK) {
            next_token();
            NODE *idxn = parse_expr();
            expect(TK_RBRACK, "']'");
            NODE *base;
            if (s->kind == SYM_GVAR)      base = ALLOC_node_gref(s->idx);
            else if (s->kind == SYM_LVAR) base = ALLOC_node_lref(s->idx);
            else pascal_error("string index on non-variable '%s'", s->name);
            return (TE){ ALLOC_node_str_index(base, idxn), PT_INT, -1 };
        }
        if (s->kind == SYM_GREC || s->kind == SYM_LREC || s->kind == SYM_VREC) {
            // Walk through any number of `.field` accesses.  Nested
            // records (record-valued fields) are flattened: we
            // accumulate a static slot offset relative to the
            // outermost record's base and emit a single read/write at
            // the end.
            int rec_idx     = s->rec_idx;
            int extra_off   = 0;
            int final_type  = PT_RECORD;
            for (;;) {
                expect(TK_DOT, "'.'");
                if (tk != TK_ID) pascal_error("expected field name after '.'");
                char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
                next_token();
                struct record_type *rt = &record_types[rec_idx];
                struct record_field *f = NULL;
                for (int i = 0; i < rt->nfields; i++) {
                    if (strcmp(rt->fields[i].name, fname) == 0) { f = &rt->fields[i]; break; }
                }
                if (!f) pascal_error("no field '%s' in record '%s'", fname, rt->name);
                extra_off += f->offset;
                if (f->type == PT_RECORD && tk == TK_DOT) {
                    rec_idx = f->rec_idx;
                    continue;
                }
                final_type = f->type;
                break;
            }
            // Emit the read at (base + extra_off).
            if (s->kind == SYM_VREC) {
                return (TE){ ALLOC_node_var_aref((uint32_t)s->idx, 0, mk_int(extra_off)),
                             final_type };
            }
            int slot = s->idx + extra_off;
            if (s->kind == SYM_GREC)
                return (TE){ ALLOC_node_gref((uint32_t)slot), final_type, -1 };
            // SYM_LREC: respect lexical depth.
            if (s->depth < current_depth)
                return (TE){ ALLOC_node_uref((uint32_t)s->depth, (uint32_t)slot), final_type, -1 };
            return (TE){ ALLOC_node_lref((uint32_t)slot), final_type, -1 };
        }
        return te_get(s, NULL, NULL);
    }
    pascal_error("unexpected %s in expression at line %d", tk_name(tk), line_no);
    return (TE){ NULL, PT_INT, -1 };
}

static TE
te_term(void)
{
    TE left = te_factor();
    for (;;) {
        if (accept(TK_STAR)) {
            TE r = te_factor();
            if (left.t == PT_SET || r.t == PT_SET) {
                left = (TE){ ALLOC_node_set_intersect(left.n, r.n), PT_SET, -1 };
                continue;
            }
            int t = te_unify_numeric(&left, &r);
            left = (t == PT_REAL)
                 ? (TE){ ALLOC_node_rmul(left.n, r.n), PT_REAL, -1 }
                 : (TE){ ALLOC_node_mul(left.n, r.n), PT_INT, -1 };
        } else if (accept(TK_SLASH)) {
            // Pascal's `/` is always real division — promote both.
            left = te_promote_real(left);
            TE r = te_promote_real(te_factor());
            left = (TE){ ALLOC_node_rdiv(left.n, r.n), PT_REAL, -1 };
        } else if (accept(TK_DIV)) {
            // Integer division.  Both operands must be integer.
            if (left.t != PT_INT) pascal_error("'div' requires integer operands");
            TE r = te_factor();
            if (r.t != PT_INT) pascal_error("'div' requires integer operands");
            left = (TE){ ALLOC_node_div(left.n, r.n), PT_INT, -1 };
        } else if (accept(TK_MOD)) {
            if (left.t != PT_INT) pascal_error("'mod' requires integer operands");
            TE r = te_factor();
            if (r.t != PT_INT) pascal_error("'mod' requires integer operands");
            left = (TE){ ALLOC_node_mod(left.n, r.n), PT_INT, -1 };
        } else if (accept(TK_AND)) {
            TE r = te_factor();
            left = (TE){ ALLOC_node_and(left.n, r.n), PT_BOOL, -1 };
        } else break;
    }
    return left;
}

static TE
te_simple(void)
{
    TE left = te_term();
    for (;;) {
        if (accept(TK_PLUS)) {
            TE r = te_term();
            if (left.t == PT_STR || r.t == PT_STR) {
                NODE *ln = (left.t == PT_STR) ? left.n : ALLOC_node_chr_to_str(left.n);
                NODE *rn = (r.t    == PT_STR) ? r.n    : ALLOC_node_chr_to_str(r.n);
                left = (TE){ ALLOC_node_str_concat(ln, rn), PT_STR, -1 };
                continue;
            }
            if (left.t == PT_SET || r.t == PT_SET) {
                left = (TE){ ALLOC_node_set_union(left.n, r.n), PT_SET, -1 };
                continue;
            }
            int t = te_unify_numeric(&left, &r);
            left = (t == PT_REAL)
                 ? (TE){ ALLOC_node_radd(left.n, r.n), PT_REAL, -1 }
                 : (TE){ ALLOC_node_add(left.n, r.n), PT_INT, -1 };
        } else if (accept(TK_MINUS)) {
            TE r = te_term();
            if (left.t == PT_SET || r.t == PT_SET) {
                left = (TE){ ALLOC_node_set_diff(left.n, r.n), PT_SET, -1 };
                continue;
            }
            int t = te_unify_numeric(&left, &r);
            left = (t == PT_REAL)
                 ? (TE){ ALLOC_node_rsub(left.n, r.n), PT_REAL, -1 }
                 : (TE){ ALLOC_node_sub(left.n, r.n), PT_INT, -1 };
        } else if (accept(TK_OR)) {
            TE r = te_term();
            left = (TE){ ALLOC_node_or(left.n, r.n), PT_BOOL, -1 };
        } else break;
    }
    return left;
}

static TE
te_expr(void)
{
    TE left = te_simple();
    if (accept(TK_IN)) {
        TE rhs = te_simple();
        if (rhs.t != PT_SET) pascal_error("'in' rhs must be a set");
        return (TE){ ALLOC_node_set_in(left.n, rhs.n), PT_BOOL, -1 };
    }
    if (accept(TK_IS)) {
        if (tk != TK_ID) pascal_error("expected class name after 'is'");
        struct sym *cs = sym_find_global(tk_id);
        if (!cs || cs->kind != SYM_CLASSTYPE) pascal_error("'%s' is not a class", tk_id);
        next_token();
        return (TE){ ALLOC_node_is_class(left.n, (uint32_t)cs->idx), PT_BOOL, -1 };
    }
    if (accept(TK_AS)) {
        if (tk != TK_ID) pascal_error("expected class name after 'as'");
        struct sym *cs = sym_find_global(tk_id);
        if (!cs || cs->kind != SYM_CLASSTYPE) pascal_error("'%s' is not a class", tk_id);
        next_token();
        return (TE){ ALLOC_node_as_class(left.n, (uint32_t)cs->idx), PT_POINTER, cs->idx };
    }
    int op = 0;
    switch (tk) {
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
        op = tk; next_token(); break;
    default: return left;
    }
    TE right = te_simple();
    NODE *n;
    if (left.t == PT_STR || right.t == PT_STR) {
        switch (op) {
        case TK_EQ: n = ALLOC_node_str_eq(left.n, right.n); break;
        case TK_NE: n = ALLOC_node_str_ne(left.n, right.n); break;
        case TK_LT: n = ALLOC_node_str_lt(left.n, right.n); break;
        case TK_LE: n = ALLOC_node_str_le(left.n, right.n); break;
        case TK_GT: n = ALLOC_node_str_gt(left.n, right.n); break;
        case TK_GE: n = ALLOC_node_str_ge(left.n, right.n); break;
        default: n = NULL;
        }
        return (TE){ n, PT_BOOL, -1 };
    }
    int t = te_unify_numeric(&left, &right);
    if (t == PT_REAL) {
        switch (op) {
        case TK_EQ: n = ALLOC_node_req(left.n, right.n); break;
        case TK_NE: n = ALLOC_node_rne(left.n, right.n); break;
        case TK_LT: n = ALLOC_node_rlt(left.n, right.n); break;
        case TK_LE: n = ALLOC_node_rle(left.n, right.n); break;
        case TK_GT: n = ALLOC_node_rgt(left.n, right.n); break;
        case TK_GE: n = ALLOC_node_rge(left.n, right.n); break;
        default: n = NULL;
        }
    } else {
        switch (op) {
        case TK_EQ: n = ALLOC_node_eq(left.n, right.n); break;
        case TK_NE: n = ALLOC_node_ne(left.n, right.n); break;
        case TK_LT: n = ALLOC_node_lt(left.n, right.n); break;
        case TK_LE: n = ALLOC_node_le(left.n, right.n); break;
        case TK_GT: n = ALLOC_node_gt(left.n, right.n); break;
        case TK_GE: n = ALLOC_node_ge(left.n, right.n); break;
        default: n = NULL;
        }
    }
    return (TE){ n, PT_BOOL, -1 };
}

// ---------------------------------------------------------------------------
// Statements.
// ---------------------------------------------------------------------------

static NODE *
parse_compound(void)
{
    expect(TK_BEGIN, "'begin'");
    NODE *body = parse_stmt_list(TK_END, 0);
    expect(TK_END, "'end'");
    return body ? body : ALLOC_node_int(0);
}

static NODE *
parse_stmt_list(int end_tk1, int end_tk2)
{
    NODE *seq = NULL;
    while (tk != end_tk1 && tk != end_tk2 && tk != TK_EOF) {
        NODE *s = parse_stmt();
        seq = mk_seq(seq, s);
        if (!accept(TK_SEMI)) break;
    }
    return seq;
}

static NODE *
parse_if(void)
{
    expect(TK_IF, "'if'");
    NODE *cond = parse_expr();
    expect(TK_THEN, "'then'");
    NODE *thn = parse_stmt();
    if (accept(TK_ELSE)) {
        NODE *els = parse_stmt();
        return ALLOC_node_if(cond, thn, els);
    }
    return ALLOC_node_if1(cond, thn);
}

static NODE *
parse_while(void)
{
    expect(TK_WHILE, "'while'");
    NODE *cond = parse_expr();
    expect(TK_DO, "'do'");
    NODE *body = parse_stmt();
    return ALLOC_node_while(cond, body);
}

static NODE *
parse_repeat(void)
{
    expect(TK_REPEAT, "'repeat'");
    NODE *body = parse_stmt_list(TK_UNTIL, 0);
    expect(TK_UNTIL, "'until'");
    NODE *cond = parse_expr();
    return ALLOC_node_repeat(body ? body : ALLOC_node_int(0), cond);
}

// case x of
//   v1 [, v2]: stmt;
//   v3..v4:    stmt;
//   ...
// [else stmt_list]
// end
//
// Lowered to (tmp := x; if cond1 then s1 else if cond2 then s2 ... else else_body).
// `tmp` is allocated as a fresh slot — local if we're inside a proc,
// global at the main-block level — so the case expression is evaluated
// once even if it has side effects.
static NODE *
parse_case(void)
{
    expect(TK_CASE, "'case'");
    TE expr = te_expr();
    if (expr.t == PT_REAL) pascal_error("case expression must be integer");
    expect(TK_OF, "'of'");

    bool in_proc = (current_proc != NULL);
    int  tmp_idx = in_proc ? n_locals_alloc++ : n_globals_alloc++;

    #define TMP_REF()  (in_proc ? ALLOC_node_lref((uint32_t)tmp_idx) \
                                : ALLOC_node_gref((uint32_t)tmp_idx))

    struct case_arm { NODE *cond; NODE *body; };
    struct case_arm arms[64];
    int narms = 0;

    while (tk != TK_END && tk != TK_ELSE && tk != TK_EOF) {
        // label list: const_or_range (',' const_or_range)*
        NODE *cond = NULL;
        for (;;) {
            int sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            else if (accept(TK_PLUS)) sign = 1;
            if (tk != TK_INT) pascal_error("expected integer case label at line %d", line_no);
            int64_t v1 = sign * tk_int;
            next_token();
            NODE *single;
            if (accept(TK_DOTDOT)) {
                int s2 = 1;
                if (accept(TK_MINUS)) s2 = -1;
                else if (accept(TK_PLUS)) s2 = 1;
                if (tk != TK_INT) pascal_error("expected upper bound");
                int64_t v2 = s2 * tk_int;
                next_token();
                single = ALLOC_node_and(
                    ALLOC_node_ge(TMP_REF(), mk_int(v1)),
                    ALLOC_node_le(TMP_REF(), mk_int(v2)));
            } else {
                single = ALLOC_node_eq(TMP_REF(), mk_int(v1));
            }
            cond = cond ? ALLOC_node_or(cond, single) : single;
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_COLON, "':'");
        NODE *body = parse_stmt();
        if (narms >= 64) pascal_error("too many case arms");
        arms[narms].cond = cond;
        arms[narms].body = body;
        narms++;
        if (!accept(TK_SEMI)) break;
    }

    NODE *else_body = ALLOC_node_int(0);
    if (accept(TK_ELSE)) {
        NODE *eb = parse_stmt_list(TK_END, 0);
        if (eb) else_body = eb;
    }
    expect(TK_END, "'end'");

    NODE *chain = else_body;
    for (int i = narms - 1; i >= 0; i--) {
        chain = ALLOC_node_if(arms[i].cond, arms[i].body, chain);
    }

    NODE *init = in_proc ? ALLOC_node_lset((uint32_t)tmp_idx, expr.n)
                         : ALLOC_node_gset((uint32_t)tmp_idx, expr.n);
    #undef TMP_REF
    return ALLOC_node_seq(init, chain);
}

static NODE *
parse_for(void)
{
    expect(TK_FOR, "'for'");
    if (tk != TK_ID) pascal_error("expected loop variable at line %d", line_no);
    char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
    next_token();
    struct sym *s = sym_find(id);
    if (!s || (s->kind != SYM_LVAR && s->kind != SYM_GVAR))
        pascal_error("for-loop variable '%s' must be a scalar var", id);

    // for-in form: `for x in expr do BODY`.  Desugars depend on expr's
    // type:
    //   * static array  → for IDX := lo to hi do begin x := arr[IDX]; BODY end
    //   * dynamic array → for IDX := 0 to length(d)-1 do begin x := d[IDX]; BODY end
    //   * set (bitset)  → for IDX := 0 to 63 do if IDX in S then begin x := IDX; BODY end
    // where IDX is a fresh hidden slot (local in procs, global at
    // the top level — same scheme as `case` temps).
    if (accept(TK_IN)) {
        if (tk != TK_ID) pascal_error("expected array/set variable after 'in'");
        char arrid[64]; strncpy(arrid, tk_id, 63); arrid[63] = 0;
        next_token();
        struct sym *as = sym_find(arrid);
        if (!as) pascal_error("undefined identifier '%s'", arrid);

        bool in_proc = (current_proc != NULL);
        int  hidden  = in_proc ? n_locals_alloc++ : n_globals_alloc++;
        NODE *idx_ref = in_proc ? ALLOC_node_lref((uint32_t)hidden)
                                : ALLOC_node_gref((uint32_t)hidden);

        // Static array path (existing behaviour).
        if (as->kind == SYM_GARR || as->kind == SYM_LARR || as->kind == SYM_VARR) {
            if (as->kind == SYM_GARR && as->is_2d)
                pascal_error("for-in over 2D arrays not supported");
            expect(TK_DO, "'do'");
            int32_t lo = as->lo, hi = as->hi;
            NODE *elem;
            if (as->kind == SYM_GARR)
                elem = ALLOC_node_aref((uint32_t)as->idx, idx_ref);
            else if (as->kind == SYM_LARR)
                elem = ALLOC_node_aref_local((uint32_t)as->idx, as->lo, idx_ref);
            else
                elem = ALLOC_node_var_aref((uint32_t)as->idx, as->lo, idx_ref);
            NODE *assign = mk_set_typed(s, NULL, NULL, elem);
            NODE *user_body = parse_stmt();
            NODE *body = mk_seq(assign, user_body);
            if (in_proc)
                return ALLOC_node_lfor_to((uint32_t)hidden, mk_int(lo), mk_int(hi), body);
            return ALLOC_node_gfor_to((uint32_t)hidden, mk_int(lo), mk_int(hi), body);
        }

        // Dynamic-array path.
        if (as->type == PT_DYNARR && (as->kind == SYM_GVAR || as->kind == SYM_LVAR)) {
            expect(TK_DO, "'do'");
            NODE *base    = (as->kind == SYM_GVAR)
                          ? ALLOC_node_gref((uint32_t)as->idx)
                          : ALLOC_node_lref((uint32_t)as->idx);
            NODE *base2   = (as->kind == SYM_GVAR)
                          ? ALLOC_node_gref((uint32_t)as->idx)
                          : ALLOC_node_lref((uint32_t)as->idx);
            NODE *elem    = ALLOC_node_dynarr_get(base, idx_ref);
            NODE *assign  = mk_set_typed(s, NULL, NULL, elem);
            NODE *user_body = parse_stmt();
            NODE *body    = mk_seq(assign, user_body);
            NODE *hi_expr = ALLOC_node_sub(ALLOC_node_dynarr_length(base2), mk_int(1));
            if (in_proc)
                return ALLOC_node_lfor_to((uint32_t)hidden, mk_int(0), hi_expr, body);
            return ALLOC_node_gfor_to((uint32_t)hidden, mk_int(0), hi_expr, body);
        }

        // Set (bitset) path.
        if (as->type == PT_SET && (as->kind == SYM_GVAR || as->kind == SYM_LVAR)) {
            expect(TK_DO, "'do'");
            NODE *set_ref = (as->kind == SYM_GVAR)
                          ? ALLOC_node_gref((uint32_t)as->idx)
                          : ALLOC_node_lref((uint32_t)as->idx);
            // Body of inner if: x := IDX; BODY.
            NODE *assign = mk_set_typed(s, NULL, NULL, idx_ref);
            NODE *user_body = parse_stmt();
            NODE *inner = mk_seq(assign, user_body);
            // if (IDX in set) inner.  Re-fetch idx_ref (it's already
            // consumed for assign) using a fresh allocation.
            NODE *idx_ref2 = in_proc ? ALLOC_node_lref((uint32_t)hidden)
                                     : ALLOC_node_gref((uint32_t)hidden);
            NODE *guard  = ALLOC_node_set_in(idx_ref2, set_ref);
            NODE *gated  = ALLOC_node_if1(guard, inner);
            // Iterate IDX = 0..63 (the universe of the 64-bit set).
            if (in_proc)
                return ALLOC_node_lfor_to((uint32_t)hidden, mk_int(0), mk_int(63), gated);
            return ALLOC_node_gfor_to((uint32_t)hidden, mk_int(0), mk_int(63), gated);
        }

        pascal_error("for-in: '%s' is not an array/dynarr/set", arrid);
    }

    expect(TK_ASSIGN, "':='");
    NODE *from = parse_expr();
    bool downto;
    if (accept(TK_TO))           downto = false;
    else if (accept(TK_DOWNTO))  downto = true;
    else pascal_error("expected 'to' or 'downto' at line %d", line_no);
    NODE *to_e = parse_expr();
    expect(TK_DO, "'do'");
    NODE *body = parse_stmt();

    if (s->kind == SYM_LVAR) {
        return downto ? ALLOC_node_lfor_downto(s->idx, from, to_e, body)
                      : ALLOC_node_lfor_to(s->idx, from, to_e, body);
    } else {
        return downto ? ALLOC_node_gfor_downto(s->idx, from, to_e, body)
                      : ALLOC_node_gfor_to(s->idx, from, to_e, body);
    }
}

static NODE *
parse_id_stmt(void)
{
    char id[64]; strncpy(id, tk_id, 63); id[63] = 0;
    next_token();

    // halt with no parens.
    if (strcmp(id, "halt") == 0) {
        if (accept(TK_LPAREN)) {
            if (tk == TK_RPAREN) { next_token(); return ALLOC_node_halt(); }
            NODE *e = parse_expr();
            expect(TK_RPAREN, "')'");
            return ALLOC_node_halt_with(e);
        }
        return ALLOC_node_halt();
    }

    // write / writeln
    if (strcmp(id, "write") == 0)   return parse_write_call(false);
    if (strcmp(id, "writeln") == 0) return parse_write_call(true);

    // inc / dec
    if (strcmp(id, "inc") == 0) return parse_inc_dec("inc", true);
    if (strcmp(id, "dec") == 0) return parse_inc_dec("dec", false);

    // String mutators: insert / delete / setlength.  All take a
    // string variable as one of their args; the new value is built
    // by the runtime helper and assigned back.
    if (strcmp(id, "insert") == 0) {
        expect(TK_LPAREN, "'('");
        TE sube = te_expr();
        NODE *sub = (sube.t == PT_INT) ? ALLOC_node_chr_to_str(sube.n) : sube.n;
        expect(TK_COMMA, "','");
        if (tk != TK_ID) pascal_error("insert: 2nd arg must be a string variable");
        char sname[64]; strncpy(sname, tk_id, 63); sname[63] = 0;
        next_token();
        struct sym *ss = sym_find(sname);
        if (!ss || ss->type != PT_STR) pascal_error("insert: '%s' is not a string", sname);
        expect(TK_COMMA, "','");
        NODE *pos = parse_expr(); expect(TK_RPAREN, "')'");
        NODE *load = (ss->kind == SYM_GVAR) ? ALLOC_node_gref(ss->idx)
                                            : ALLOC_node_lref(ss->idx);
        NODE *fresh = ALLOC_node_str_insert(sub, load, pos);
        return mk_set_typed(ss, NULL, NULL, fresh);
    }
    if (strcmp(id, "delete") == 0) {
        expect(TK_LPAREN, "'('");
        if (tk != TK_ID) pascal_error("delete: 1st arg must be a string variable");
        char sname[64]; strncpy(sname, tk_id, 63); sname[63] = 0;
        next_token();
        struct sym *ss = sym_find(sname);
        if (!ss || ss->type != PT_STR) pascal_error("delete: '%s' is not a string", sname);
        expect(TK_COMMA, "','");
        NODE *st = parse_expr(); expect(TK_COMMA, "','");
        NODE *cn = parse_expr(); expect(TK_RPAREN, "')'");
        NODE *load = (ss->kind == SYM_GVAR) ? ALLOC_node_gref(ss->idx)
                                            : ALLOC_node_lref(ss->idx);
        NODE *fresh = ALLOC_node_str_delete(load, st, cn);
        return mk_set_typed(ss, NULL, NULL, fresh);
    }
    if (strcmp(id, "setlength") == 0) {
        expect(TK_LPAREN, "'('");
        if (tk != TK_ID) pascal_error("setlength: 1st arg must be a variable");
        char sname[64]; strncpy(sname, tk_id, 63); sname[63] = 0;
        next_token();
        struct sym *ss = sym_find(sname);
        if (!ss || (ss->type != PT_STR && ss->type != PT_DYNARR))
            pascal_error("setlength: '%s' is not a string or dynamic array", sname);
        expect(TK_COMMA, "','");
        NODE *ln = parse_expr(); expect(TK_RPAREN, "')'");
        NODE *addr = (ss->kind == SYM_GVAR) ? ALLOC_node_addr_gvar(ss->idx)
                                             : ALLOC_node_addr_lvar(ss->idx);
        if (ss->type == PT_DYNARR) {
            return ALLOC_node_dynarr_setlen(addr, ln);
        }
        // string variant takes a value, not an address — keep old code.
        NODE *load = (ss->kind == SYM_GVAR) ? ALLOC_node_gref(ss->idx)
                                            : ALLOC_node_lref(ss->idx);
        NODE *fresh = ALLOC_node_str_setlength(load, ln);
        return mk_set_typed(ss, NULL, NULL, fresh);
    }

    // File I/O built-ins.  Each takes a file variable's slot address
    // so the runtime can read/write the heap-allocated `pascal_file *`
    // it points at.
    if (strcmp(id, "assign") == 0 || strcmp(id, "reset") == 0
        || strcmp(id, "rewrite") == 0 || strcmp(id, "close") == 0) {
        expect(TK_LPAREN, "'('");
        if (tk != TK_ID) pascal_error("%s expects a file variable", id);
        char fid[64]; strncpy(fid, tk_id, 63); fid[63] = 0;
        next_token();
        struct sym *fs = sym_find(fid);
        if (!fs || fs->type != PT_FILE) pascal_error("'%s' is not a file", fid);
        NODE *addr = mk_addr_of(fs, NULL, NULL);
        if (strcmp(id, "assign") == 0) {
            expect(TK_COMMA, "','");
            NODE *name = parse_expr();
            expect(TK_RPAREN, "')'");
            return ALLOC_node_file_assign(addr, name);
        }
        expect(TK_RPAREN, "')'");
        if (strcmp(id, "reset")   == 0) return ALLOC_node_file_reset(addr);
        if (strcmp(id, "rewrite") == 0) return ALLOC_node_file_rewrite(addr);
        return ALLOC_node_file_close(addr);
    }

    // new(p), dispose(p) — pointer lifecycle.
    if (strcmp(id, "new") == 0 || strcmp(id, "dispose") == 0) {
        bool is_new = (id[0] == 'n');
        expect(TK_LPAREN, "'('");
        if (tk != TK_ID) pascal_error("%s expects a pointer variable", id);
        char rid[64]; strncpy(rid, tk_id, 63); rid[63] = 0;
        next_token();
        expect(TK_RPAREN, "')'");
        struct sym *rs = sym_find(rid);
        if (!rs || rs->type != PT_POINTER)
            pascal_error("'%s' is not a pointer", rid);
        if (is_new) {
            int slots = record_types[rs->rec_idx].total_slots;
            return mk_set_typed(rs, NULL, NULL, ALLOC_node_new_record((uint32_t)slots));
        }
        NODE *load = (rs->kind == SYM_GVAR) ? ALLOC_node_gref(rs->idx)
                                            : ALLOC_node_lref(rs->idx);
        return ALLOC_node_dispose(load);
    }

    // read / readln — first arg may be a file; subsequent vars
    // receive scanf-parsed integers (or, for string vars, full lines).
    if (strcmp(id, "read") == 0 || strcmp(id, "readln") == 0) {
        bool is_readln = id[4] == 'l';
        NODE *seq = NULL;
        NODE *file_addr = NULL;
        if (accept(TK_LPAREN)) {
            if (tk == TK_ID) {
                struct sym *fs = sym_find(tk_id);
                if (fs && fs->type == PT_FILE) {
                    next_token();
                    file_addr = mk_addr_of(fs, NULL, NULL);
                    if (!accept(TK_COMMA) && tk != TK_RPAREN)
                        pascal_error("expected ',' or ')' after file");
                }
            }
            if (tk != TK_RPAREN) for (;;) {
                if (tk != TK_ID) pascal_error("read expects a variable");
                char rid[64]; strncpy(rid, tk_id, 63); rid[63] = 0;
                next_token();
                struct sym *rs = sym_find(rid);
                if (!rs) pascal_error("undefined variable '%s'", rid);
                NODE *idx1 = NULL, *idx2 = NULL;
                if (rs->kind == SYM_GARR || rs->kind == SYM_LARR || rs->kind == SYM_VARR) {
                    expect(TK_LBRACK, "'['");
                    idx1 = parse_expr();
                    if (rs->kind == SYM_GARR && rs->is_2d) {
                        expect(TK_COMMA, "','");
                        idx2 = parse_expr();
                    }
                    expect(TK_RBRACK, "']'");
                }
                NODE *src;
                if (file_addr) {
                    src = (rs->type == PT_STR) ? ALLOC_node_fread_line(file_addr)
                                               : ALLOC_node_fread_int (file_addr);
                } else {
                    src = node_read_int_dummy();
                }
                seq = mk_seq(seq, mk_set_typed(rs, idx1, idx2, src));
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RPAREN, "')'");
        }
        if (is_readln) {
            seq = mk_seq(seq, file_addr ? ALLOC_node_freadln_eat(file_addr)
                                        : ALLOC_node_readln_eat());
        }
        return seq ? seq : ALLOC_node_int(0);
    }

    // `with`-stack lookup: bare ID is a field of an active with-record.
    {
        struct sym *rs = NULL;
        struct record_field *f = with_lookup_field(id, &rs);
        if (f) {
            expect(TK_ASSIGN, "':='");
            TE v = te_expr();
            NODE *rhs = te_coerce(v, f->type, "field assignment (with)");
            return mk_field_set(rs, f, rhs);
        }
    }

    // Inside a method body, bare LHS that names a class field or
    // property auto-resolves to Self.<name>.  Skip if the name
    // shadows a local or global to avoid clobbering params and
    // loop vars (which are valid lvalue targets too).
    if (current_method_class_idx >= 0
        && sym_find_local(id) == NULL
        && sym_find_global(id) == NULL) {
        int walk = current_method_class_idx;
        while (walk >= 0) {
            struct class_type *wc = &class_types[walk];
            for (int i = 0; i < wc->nprops; i++) {
                if (strcmp(wc->properties[i].name, id) == 0) {
                    struct property_entry *pe = &wc->properties[i];
                    expect(TK_ASSIGN, "':='");
                    TE v = te_expr();
                    NODE *rhs = te_coerce(v, pe->type, "property assignment");
                    NODE *self_load = ALLOC_node_lref(0);
                    if (pe->write_field_offset >= 0)
                        return ALLOC_node_ptr_field_set(self_load, (uint32_t)pe->write_field_offset, rhs);
                    if (pe->write_method_idx >= 0) {
                        struct method_entry *sme = &class_types[walk].methods[pe->write_method_idx];
                        NODE *args[2] = { self_load, rhs };
                        return mk_pcall((uint32_t)sme->proc_idx, args, 2);
                    }
                    pascal_error("property '%s' is read-only", id);
                }
            }
            walk = wc->parent_idx;
        }
        walk = current_method_class_idx;
        while (walk >= 0) {
            struct record_type *rt = &record_types[class_types[walk].rec_idx];
            for (int i = 0; i < rt->nfields; i++) {
                if (strcmp(rt->fields[i].name, id) == 0) {
                    check_access(rt->fields[i].decl_class, rt->fields[i].vis, "field", rt->fields[i].name);
                    expect(TK_ASSIGN, "':='");
                    TE v = te_expr();
                    NODE *rhs = te_coerce(v, rt->fields[i].type, "field assignment");
                    NODE *self_load = ALLOC_node_lref(0);
                    return ALLOC_node_ptr_field_set(self_load, (uint32_t)rt->fields[i].offset, rhs);
                }
            }
            walk = class_types[walk].parent_idx;
        }
    }

    // function name self-reference inside body → either call or
    // assignment to return slot.  `Result` is the modern alias.
    // For methods (proc->name like "TAnimal.describe") we also accept
    // the bare method-name part on the LHS.
    bool name_matches = false;
    if (current_proc && current_proc->is_function) {
        if (strcmp(current_proc->name, id) == 0) name_matches = true;
        else if (strcmp(id, "result") == 0)      name_matches = true;
        else {
            const char *dot = strchr(current_proc->name, '.');
            if (dot && strcmp(dot + 1, id) == 0) name_matches = true;
        }
    }
    if (current_proc && current_proc->is_function && name_matches) {
        if (accept(TK_ASSIGN)) {
            TE val = te_expr();
            NODE *rhs = te_coerce(val, current_proc->return_type, "function return");
            return ALLOC_node_lset((uint32_t)current_proc->return_slot, rhs);
        }
        // bare `Result;` is meaningless — fall through to call handling
        // only when the name matches the function name.
        if (strcmp(id, "result") == 0) pascal_error("'Result' must appear in an assignment");
        // expression-statement form: discard return value
        NODE *args[16]; int arg_t[16]; uint32_t n = 0;
        if (tk == TK_LPAREN) n = parse_typed_call_args(current_proc, args, arg_t, 16);
        return mk_call_with_promotion(current_proc_idx, args, arg_t, n);
    }

    struct sym *s = sym_find(id);
    if (!s) pascal_error("undefined identifier '%s' at line %d", id, line_no);

    if (s->kind == SYM_PROC) {
        struct pascal_proc *p = &PASCAL_PROCS[s->idx];
        NODE *args[16]; int arg_t[16]; uint32_t n = 0;
        if (tk == TK_LPAREN) n = parse_typed_call_args(p, args, arg_t, 16);
        return mk_call_with_promotion(s->idx, args, arg_t, n);
    }

    // Procedure-typed variable in statement position: indirect call.
    if (s->type == PT_PROC && tk == TK_LPAREN) {
        NODE *args[16]; int arg_t[16]; uint32_t n = 0;
        n = parse_typed_call_args(NULL, args, arg_t, 16);
        uint32_t base = push_call_args(args, n);
        NODE *fn = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                         : ALLOC_node_lref(s->idx);
        return ALLOC_node_pcall_ind(fn, base, n);
    }

    // p^.field := val (or obj.field := val for class types).
    if (s->type == PT_POINTER && (tk == TK_HAT || (s->class_idx >= 0 && tk == TK_DOT))) {
        if (tk == TK_HAT) next_token();
        expect(TK_DOT, "'.'");
        if (tk != TK_ID) pascal_error("expected field/method name after '.'");
        char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
        next_token();

        // Property write — check before method/field paths.
        if (s->class_idx >= 0) {
            struct property_entry *pe = NULL;
            int walk = s->class_idx;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nprops; i++)
                    if (strcmp(wc->properties[i].name, fname) == 0) { pe = &wc->properties[i]; break; }
                if (pe) break;
                walk = wc->parent_idx;
            }
            if (pe && tk == TK_ASSIGN) {
                next_token();
                TE val = te_expr();
                NODE *rhs = te_coerce(val, pe->type, "property assignment");
                NODE *self_load = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                        : ALLOC_node_lref(s->idx);
                if (pe->write_field_offset >= 0)
                    return ALLOC_node_ptr_field_set(self_load, (uint32_t)pe->write_field_offset, rhs);
                if (pe->write_method_idx >= 0) {
                    struct method_entry *sme = &class_types[s->class_idx].methods[pe->write_method_idx];
                    NODE *args[2] = { self_load, rhs };
                    if (sme->is_virtual && sme->vtable_slot >= 0) {
                        uint32_t base = push_call_args(&args[1], 1);
                        return ALLOC_node_vcall(self_load, (uint32_t)sme->vtable_slot, base, 2);
                    }
                    return mk_pcall((uint32_t)sme->proc_idx, args, 2);
                }
                pascal_error("property '%s' is read-only", fname);
            }
        }
        // obj.method(args)  — statement form (discard return value).
        if (s->class_idx >= 0) {
            struct method_entry *me = NULL;
            int walk = s->class_idx;
            while (walk >= 0) {
                struct class_type *wc = &class_types[walk];
                for (int i = 0; i < wc->nmethods; i++)
                    if (strcmp(wc->methods[i].name, fname) == 0
                        && (wc->methods[i].is_virtual || wc->methods[i].proc_idx >= 0)) { me = &wc->methods[i]; break; }
                if (me) break;
                walk = wc->parent_idx;
            }
            if (me) {
                NODE *self_load = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                                        : ALLOC_node_lref(s->idx);
                NODE *args[16]; uint32_t n = 1;
                args[0] = self_load;
                if (accept(TK_LPAREN)) {
                    if (tk != TK_RPAREN) for (;;) {
                        if (n >= 16) pascal_error("too many args");
                        args[n++] = te_expr().n;
                        if (!accept(TK_COMMA)) break;
                    }
                    expect(TK_RPAREN, "')'");
                }
                if (me->is_virtual && me->vtable_slot >= 0) {
                    uint32_t base = push_call_args(&args[1], n - 1);
                    return ALLOC_node_vcall(self_load, (uint32_t)me->vtable_slot, base, n);
                }
                return mk_pcall((uint32_t)me->proc_idx, args, n);
            }
        }

        struct record_type *rt = &record_types[s->rec_idx];
        struct record_field *fld = NULL;
        for (int i = 0; i < rt->nfields; i++) {
            if (strcmp(rt->fields[i].name, fname) == 0) { fld = &rt->fields[i]; break; }
        }
        if (!fld) pascal_error("no field '%s' in '%s'", fname, rt->name);
        check_access(fld->decl_class, fld->vis, "field", fld->name);
        expect(TK_ASSIGN, "':='");
        TE val = te_expr();
        NODE *rhs = te_coerce(val, fld->type, "field assignment");
        NODE *base = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                           : ALLOC_node_lref(s->idx);
        return ALLOC_node_ptr_field_set(base, (uint32_t)fld->offset, rhs);
    }
    // a[i] := v — dynamic array element write.
    if (s->type == PT_DYNARR && (s->kind == SYM_GVAR || s->kind == SYM_LVAR)
        && tk == TK_LBRACK) {
        next_token();
        NODE *idx = parse_expr();
        expect(TK_RBRACK, "']'");
        expect(TK_ASSIGN, "':='");
        NODE *base = (s->kind == SYM_GVAR) ? ALLOC_node_gref(s->idx)
                                            : ALLOC_node_lref(s->idx);
        TE val = te_expr();
        NODE *rhs = te_coerce(val, s->rec_idx, "dynamic array element assignment");
        return ALLOC_node_dynarr_set(base, idx, rhs);
    }
    // s[i] := ch — string element write (copy-on-write).
    if (s->type == PT_STR && (s->kind == SYM_GVAR || s->kind == SYM_LVAR)
        && tk == TK_LBRACK) {
        next_token();
        NODE *idx_e = parse_expr();
        expect(TK_RBRACK, "']'");
        expect(TK_ASSIGN, "':='");
        TE val = te_expr();
        if (val.t != PT_INT) pascal_error("string element must be a char (integer) value");
        if (s->kind == SYM_GVAR)
            return ALLOC_node_str_setidx_g((uint32_t)s->idx, idx_e, val.n);
        return ALLOC_node_str_setidx_l((uint32_t)s->idx, idx_e, val.n);
    }
    NODE *idx1 = NULL, *idx2 = NULL;
    struct record_field *field = NULL;
    if (s->kind == SYM_GARR || s->kind == SYM_LARR || s->kind == SYM_VARR) {
        expect(TK_LBRACK, "'['");
        idx1 = parse_expr();
        if (s->kind == SYM_GARR && s->is_2d) {
            expect(TK_COMMA, "','");
            idx2 = parse_expr();
        }
        expect(TK_RBRACK, "']'");
    } else if (s->kind == SYM_GREC || s->kind == SYM_LREC || s->kind == SYM_VREC) {
        int rec_idx    = s->rec_idx;
        int extra_off  = 0;
        int final_type = PT_RECORD;
        for (;;) {
            expect(TK_DOT, "'.'");
            if (tk != TK_ID) pascal_error("expected field name after '.'");
            char fname[64]; strncpy(fname, tk_id, 63); fname[63] = 0;
            next_token();
            struct record_type *rt = &record_types[rec_idx];
            struct record_field *f = NULL;
            for (int i = 0; i < rt->nfields; i++) {
                if (strcmp(rt->fields[i].name, fname) == 0) { f = &rt->fields[i]; break; }
            }
            if (!f) pascal_error("no field '%s' in record '%s'", fname, rt->name);
            extra_off += f->offset;
            if (f->type == PT_RECORD && tk == TK_DOT) { rec_idx = f->rec_idx; continue; }
            final_type = f->type;
            break;
        }
        expect(TK_ASSIGN, "':='");
        TE val = te_expr();
        NODE *rhs = te_coerce(val, final_type, "field assignment");
        if (s->kind == SYM_VREC)
            return ALLOC_node_var_aset((uint32_t)s->idx, 0, mk_int(extra_off), rhs);
        int slot = s->idx + extra_off;
        if (s->kind == SYM_GREC) return ALLOC_node_gset((uint32_t)slot, rhs);
        if (s->depth < current_depth)
            return ALLOC_node_uset((uint32_t)s->depth, (uint32_t)slot, rhs);
        return ALLOC_node_lset((uint32_t)slot, rhs);
    }
    expect(TK_ASSIGN, "':='");
    TE val = te_expr();
    if (field) {
        NODE *rhs = te_coerce(val, field->type, "field assignment");
        return mk_field_set(s, field, rhs);
    }
    NODE *rhs = te_coerce(val, s->type, "assignment");
    if (s->has_range && range_check_enabled)
        rhs = ALLOC_node_range_check(rhs, s->lo, s->hi);
    return mk_set_typed(s, idx1, idx2, rhs);
}

static NODE *
parse_stmt(void)
{
    switch (tk) {
    case TK_BEGIN:  return parse_compound();
    case TK_IF:     return parse_if();
    case TK_WHILE:  return parse_while();
    case TK_REPEAT: return parse_repeat();
    case TK_FOR:    return parse_for();
    case TK_CASE:   return parse_case();
    case TK_WITH: {
        next_token();
        int pushed = 0;
        for (;;) {
            if (tk != TK_ID) pascal_error("expected record name after 'with'");
            struct sym *rs = sym_find(tk_id);
            if (!rs || (rs->kind != SYM_GREC && rs->kind != SYM_LREC && rs->kind != SYM_VREC))
                pascal_error("'%s' is not a record variable", tk_id);
            if (with_depth >= MAX_WITH_DEPTH) pascal_error("nested 'with' too deep");
            with_stack[with_depth++] = rs;
            pushed++;
            next_token();
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_DO, "'do'");
        NODE *body = parse_stmt();
        with_depth -= pushed;
        return body;
    }
    case TK_TRY: {
        next_token();
        NODE *body = parse_stmt_list(TK_EXCEPT, TK_FINALLY);
        if (!body) body = ALLOC_node_int(0);
        if (accept(TK_EXCEPT)) {
            NODE *handler = parse_stmt_list(TK_END, 0);
            if (!handler) handler = ALLOC_node_int(0);
            expect(TK_END, "'end'");
            return ALLOC_node_try_except(body, handler);
        }
        if (accept(TK_FINALLY)) {
            NODE *fin = parse_stmt_list(TK_END, 0);
            if (!fin) fin = ALLOC_node_int(0);
            expect(TK_END, "'end'");
            return ALLOC_node_try_finally(body, fin);
        }
        pascal_error("expected 'except' or 'finally' at line %d", line_no);
    }
    case TK_INHERITED: {
        next_token();
        if (current_method_class_idx < 0)
            pascal_error("'inherited' only valid inside a method");
        int parent = class_types[current_method_class_idx].parent_idx;
        if (parent < 0) pascal_error("'inherited' used in a class with no parent");
        // Optional explicit method name; defaults to the current
        // method's name.
        char mname[64];
        if (tk == TK_ID) {
            strncpy(mname, tk_id, 63); mname[63] = 0;
            next_token();
        } else {
            strncpy(mname, current_method_name_buf, 63); mname[63] = 0;
        }
        // Find the method in the parent (or its ancestors).
        struct method_entry *me = NULL;
        int walk = parent;
        while (walk >= 0) {
            struct class_type *wc = &class_types[walk];
            for (int i = 0; i < wc->nmethods; i++)
                if (strcmp(wc->methods[i].name, mname) == 0
                    && (wc->methods[i].is_virtual || wc->methods[i].proc_idx >= 0)) { me = &wc->methods[i]; break; }
            if (me) break;
            walk = wc->parent_idx;
        }
        if (!me) pascal_error("no inherited method '%s'", mname);
        NODE *args[16]; uint32_t n = 1;
        args[0] = ALLOC_node_lref(0);    // Self
        if (accept(TK_LPAREN)) {
            if (tk != TK_RPAREN) for (;;) {
                if (n >= 16) pascal_error("too many args");
                args[n++] = te_expr().n;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RPAREN, "')'");
        }
        // `inherited` is always static — even if the method is
        // virtual, we want to call the parent's specific impl.
        return mk_pcall((uint32_t)me->proc_idx, args, n);
    }
    case TK_RAISE: {
        next_token();
        // `raise <expr>` with a string-valued expr.
        TE e = te_expr();
        NODE *msg = (e.t == PT_STR) ? e.n : ALLOC_node_chr_to_str(e.n);
        return ALLOC_node_raise(msg);
    }
    case TK_GOTO: {
        next_token();
        if (tk != TK_INT) pascal_error("expected label number after 'goto'");
        int64_t v = tk_int;
        next_token();
        if (v < 0 || v >= PASCAL_MAX_LABELS)
            pascal_error("label %ld out of range", (long)v);
        return ALLOC_node_goto((uint32_t)v);
    }
    case TK_INT: {
        // Maybe a label statement:  N: stmt
        int64_t v = tk_int;
        const char *save_src = src; int save_line = line_no;
        next_token();
        if (accept(TK_COLON)) {
            if (v < 0 || v >= PASCAL_MAX_LABELS)
                pascal_error("label %ld out of range", (long)v);
            NODE *here = ALLOC_node_label_set((uint32_t)v);
            NODE *next = parse_stmt();
            return mk_seq(here, next);
        }
        // Wasn't a label — restore and bail (an integer can't start a statement).
        src = save_src; line_no = save_line;
        tk = TK_INT; tk_int = v;
        pascal_error("unexpected integer at statement start (line %d)", line_no);
    }
    case TK_BREAK:    next_token(); return ALLOC_node_break();
    case TK_CONTINUE: next_token(); return ALLOC_node_continue();
    case TK_EXIT:
        next_token();
        if (accept(TK_LPAREN)) {
            // exit(value) inside a function — assigns to the return slot
            // and raises exit_pending.  Outside a function, treat as exit;
            // discard the value.
            TE v = te_expr();
            expect(TK_RPAREN, "')'");
            if (current_proc && current_proc->is_function) {
                NODE *rhs = te_coerce(v, current_proc->return_type, "exit value");
                return ALLOC_node_exit_with((uint32_t)current_proc->return_slot, rhs);
            }
            return ALLOC_node_exit();
        }
        return ALLOC_node_exit();
    case TK_ID:     return parse_id_stmt();
    case TK_SEMI: case TK_END: case TK_ELSE: case TK_UNTIL:
        return ALLOC_node_int(0);   // empty statement
    default:
        pascal_error("unexpected %s in statement at line %d", tk_name(tk), line_no);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Declarations.
// ---------------------------------------------------------------------------

// Forward decls so parse_type can recurse via parse_type_with_rec for
// record-field types.
static int parse_type(int32_t *lo_out, int32_t *hi_out,
                      int32_t *lo2_out, int32_t *hi2_out, bool *has2);
static int parse_type_with_rec(int32_t *lo, int32_t *hi, int32_t *lo2, int32_t *hi2,
                               bool *has2, int *elem_out, int *rec_out);

// `parse_type` reports the kind of declared type encountered.  PT_ARRAY
// signals the caller that *_lo / *_hi (and *_lo2 / *_hi2 for 2D) were
// also filled in.
static int
parse_type(int32_t *lo_out, int32_t *hi_out,
           int32_t *lo2_out, int32_t *hi2_out, bool *has2)
{
    if (has2) *has2 = false;
    g_last_class_idx = -1;
    g_last_subrange_set = false;
    // `packed` is a layout hint; we ignore it (no packed-storage
    // support yet, but consuming the keyword keeps source compatible).
    accept(TK_PACKED);
    if (accept(TK_INTEGER)) return PT_INT;
    if (accept(TK_BOOLEAN)) return PT_BOOL;
    if (accept(TK_REAL))    return PT_REAL;
    // Procedural type: `procedure[(params)]` or `function[(params)]: T`.
    // We accept and discard the signature — calls are checked
    // dynamically (or, mostly, not at all).
    if (tk == TK_PROCEDURE || tk == TK_FUNCTION) {
        bool is_func = (tk == TK_FUNCTION);
        next_token();
        if (accept(TK_LPAREN)) {
            int paren = 1;
            while (paren > 0 && tk != TK_EOF) {
                if (tk == TK_LPAREN) paren++;
                else if (tk == TK_RPAREN) paren--;
                if (paren > 0) next_token();
            }
            expect(TK_RPAREN, "')'");
        }
        if (is_func) {
            expect(TK_COLON, "':'");
            int32_t a, b, c2, d2;
            bool h2 = false;
            (void)parse_type(&a, &b, &c2, &d2, &h2);
        }
        return PT_PROC;
    }
    // `class [(parent)] field-and-method-headers end` — minimal OOP:
    // single inheritance, statically-dispatched methods, fields
    // overlap parent's first.  Method bodies live outside the class
    // declaration; their proc index is patched in when the body is
    // parsed (`procedure T.m;`).
    if (accept(TK_CLASS)) {
        if (n_class_types >= MAX_CLASS_TYPES) pascal_error("too many class types");
        if (n_record_types >= MAX_RECORD_TYPES) pascal_error("too many record types");
        int cidx = n_class_types++;
        int ridx = n_record_types++;
        struct class_type  *ct = &class_types[cidx];
        struct record_type *rt = &record_types[ridx];
        memset(ct, 0, sizeof(*ct));
        memset(rt, 0, sizeof(*rt));
        ct->parent_idx = -1;
        ct->rec_idx    = ridx;
        snprintf(ct->name, sizeof(ct->name), "<class_%d>", cidx);
        snprintf(rt->name, sizeof(rt->name), "<class_%d>", cidx);
        // Slot 0 reserved for a per-class metadata pointer (placeholder
        // for a real vtable; currently unused but kept so layout is
        // OOP-shaped and a future virtual-dispatch impl can drop in).
        int offset = 1;

        if (accept(TK_LPAREN)) {
            if (tk != TK_ID) pascal_error("expected parent class name");
            struct sym *ps = sym_find_global(tk_id);
            if (!ps || ps->kind != SYM_CLASSTYPE) pascal_error("'%s' is not a class type", tk_id);
            next_token();
            expect(TK_RPAREN, "')'");
            ct->parent_idx = ps->idx;
            struct class_type  *pc = &class_types[ps->idx];
            struct record_type *pr = &record_types[pc->rec_idx];
            for (int i = 0; i < pr->nfields; i++) {
                rt->fields[rt->nfields++] = pr->fields[i];
            }
            offset = pr->total_slots;
            for (int i = 0; i < pc->nmethods; i++) {
                ct->methods[ct->nmethods++] = pc->methods[i];
            }
            for (int i = 0; i < pc->nprops; i++) {
                ct->properties[ct->nprops++] = pc->properties[i];
            }
            ct->vtable_size = pc->vtable_size;
        }

        // Fields and method headers, in any order, terminated by `end`.
        // `current_vis` tracks the active visibility section.  Free
        // Pascal (and Delphi) default new fields to `published`, but
        // before *any* visibility marker the convention is `public` —
        // we follow that here.  A marker switches the section until
        // the next marker.
        int current_vis = VIS_PUBLIC;
        while (tk != TK_END && tk != TK_EOF) {
            if (accept(TK_PRIVATE))   { current_vis = VIS_PRIVATE;   continue; }
            if (accept(TK_PROTECTED)) { current_vis = VIS_PROTECTED; continue; }
            if (accept(TK_PUBLIC))    { current_vis = VIS_PUBLIC;    continue; }
            if (accept(TK_PUBLISHED)) { current_vis = VIS_PUBLISHED; continue; }
            // property NAME : TYPE read SOURCE [write TARGET];
            if (accept(TK_PROPERTY)) {
                if (tk != TK_ID) pascal_error("expected property name");
                char pname[48]; strncpy(pname, tk_id, 47); pname[47] = 0;
                next_token();
                expect(TK_COLON, "':'");
                int32_t a, b, c2, d2; bool h2 = false;
                int t = parse_type(&a, &b, &c2, &d2, &h2);
                int read_off = -1, read_m = -1, write_off = -1, write_m = -1;
                if (tk == TK_ID && strcmp(tk_id, "read") == 0) {
                    next_token();
                    if (tk != TK_ID) pascal_error("expected reader name");
                    char rname[48]; strncpy(rname, tk_id, 47); rname[47] = 0;
                    next_token();
                    for (int i = 0; i < rt->nfields; i++)
                        if (strcmp(rt->fields[i].name, rname) == 0) { read_off = rt->fields[i].offset; break; }
                    if (read_off < 0) {
                        for (int i = 0; i < ct->nmethods; i++)
                            if (strcmp(ct->methods[i].name, rname) == 0) { read_m = i; break; }
                    }
                    if (read_off < 0 && read_m < 0)
                        pascal_error("property '%s': no reader '%s'", pname, rname);
                }
                if (tk == TK_ID && strcmp(tk_id, "write") == 0) {
                    next_token();
                    if (tk != TK_ID) pascal_error("expected writer name");
                    char wname[48]; strncpy(wname, tk_id, 47); wname[47] = 0;
                    next_token();
                    for (int i = 0; i < rt->nfields; i++)
                        if (strcmp(rt->fields[i].name, wname) == 0) { write_off = rt->fields[i].offset; break; }
                    if (write_off < 0) {
                        for (int i = 0; i < ct->nmethods; i++)
                            if (strcmp(ct->methods[i].name, wname) == 0) { write_m = i; break; }
                    }
                    if (write_off < 0 && write_m < 0)
                        pascal_error("property '%s': no writer '%s'", pname, wname);
                }
                expect(TK_SEMI, "';'");
                if (ct->nprops >= 16) pascal_error("too many properties");
                struct property_entry *pe = &ct->properties[ct->nprops++];
                memset(pe, 0, sizeof(*pe));
                strncpy(pe->name, pname, sizeof(pe->name) - 1);
                pe->type = t;
                pe->read_field_offset  = read_off;
                pe->read_method_idx    = read_m;
                pe->write_field_offset = write_off;
                pe->write_method_idx   = write_m;
                pe->vis = current_vis;
                continue;
            }
            // `class` modifier before procedure/function = class
            // method (no implicit Self).  The class-method body
            // declared outside the class also uses this form.
            bool is_class_method = false;
            if (accept(TK_CLASS)) is_class_method = true;
            if (tk == TK_PROCEDURE || tk == TK_FUNCTION
                || tk == TK_CONSTRUCTOR || tk == TK_DESTRUCTOR) {
                bool is_func = (tk == TK_FUNCTION);
                bool is_ctor = (tk == TK_CONSTRUCTOR);
                next_token();
                if (tk != TK_ID) pascal_error("expected method name");
                char mname[48]; strncpy(mname, tk_id, 47); mname[47] = 0;
                next_token();
                // Skip the param list textually — we'll re-parse the
                // signature when the body is declared.  This is a
                // simplification (we don't typecheck class headers
                // against bodies) but keeps the parser small.
                if (accept(TK_LPAREN)) {
                    int paren = 1;
                    while (paren > 0 && tk != TK_EOF) {
                        if (tk == TK_LPAREN) paren++;
                        else if (tk == TK_RPAREN) paren--;
                        if (paren > 0) next_token();
                    }
                    expect(TK_RPAREN, "')'");
                }
                int header_return_type = PT_INT;
                if (is_func) {
                    expect(TK_COLON, "':'");
                    int32_t a, b, c2, d2; bool h2 = false;
                    header_return_type = parse_type(&a, &b, &c2, &d2, &h2);
                }
                expect(TK_SEMI, "';'");
                bool got_virtual  = false;
                bool got_override = false;
                bool got_abstract = false;
                while (tk == TK_VIRTUAL || tk == TK_OVERRIDE || tk == TK_ABSTRACT) {
                    if (accept(TK_VIRTUAL))  got_virtual  = true;
                    if (accept(TK_OVERRIDE)) got_override = true;
                    if (accept(TK_ABSTRACT)) got_abstract = true;
                    accept(TK_SEMI);
                }
                // Find an inherited entry by name first (override
                // updates that one); otherwise insert fresh.
                struct method_entry *me = NULL;
                bool inherited_entry = false;
                for (int i = 0; i < ct->nmethods; i++) {
                    if (strcmp(ct->methods[i].name, mname) == 0) {
                        me = &ct->methods[i];
                        inherited_entry = true;
                        break;
                    }
                }
                if (!me) {
                    if (ct->nmethods >= 32) pascal_error("too many methods");
                    me = &ct->methods[ct->nmethods++];
                    memset(me, 0, sizeof(*me));
                    strncpy(me->name, mname, sizeof(me->name) - 1);
                    me->vtable_slot = -1;
                }
                me->proc_idx = -1;          // patched when the body is parsed
                me->is_constructor = is_ctor;
                me->is_class       = is_class_method;
                me->is_abstract    = got_abstract;
                me->is_function    = is_func;
                me->return_type    = (char)header_return_type;
                me->vis            = current_vis;
                me->decl_class     = cidx;
                if (got_virtual && !inherited_entry) {
                    me->is_virtual  = true;
                    me->vtable_slot = ct->vtable_size++;
                } else if (got_override) {
                    if (!inherited_entry || me->vtable_slot < 0)
                        pascal_error("override without matching parent virtual");
                    me->is_virtual = true;
                } else if (got_virtual && inherited_entry) {
                    me->is_virtual = true;
                }
                continue;
            }
            // Field declaration — same shape as a record field.
            char fnames[16][48]; int fcount = 0;
            for (;;) {
                if (tk != TK_ID) pascal_error("expected field name in class");
                strncpy(fnames[fcount], tk_id, 47); fnames[fcount][47] = 0;
                fcount++; next_token();
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_COLON, "':'");
            int ft;
            if (accept(TK_INTEGER))      ft = PT_INT;
            else if (accept(TK_BOOLEAN)) ft = PT_BOOL;
            else if (accept(TK_REAL))    ft = PT_REAL;
            else if (accept(TK_STRING))  ft = PT_STR;
            else pascal_error("class field must be integer/boolean/real/string");
            for (int i = 0; i < fcount; i++) {
                if (rt->nfields >= 24) pascal_error("too many fields");
                struct record_field *f = &rt->fields[rt->nfields++];
                memset(f, 0, sizeof(*f));
                strncpy(f->name, fnames[i], sizeof(f->name) - 1);
                f->offset = offset++;
                f->type   = ft;
                f->vis    = current_vis;
                f->decl_class = cidx;
            }
            accept(TK_SEMI);
        }
        expect(TK_END, "'end'");
        rt->total_slots = offset;
        g_last_class_idx  = cidx;
        g_last_record_idx = ridx;
        return PT_POINTER;     // class-typed vars hold a pointer
    }
    if (accept(TK_HAT)) {
        // `^TypeName` — pointer to a previously declared record type.
        if (tk != TK_ID) pascal_error("expected type name after '^'");
        struct sym *ts = sym_find_global(tk_id);
        if (!ts || ts->kind != SYM_RECTYPE)
            pascal_error("'^' requires a record type name (got '%s')", tk_id);
        next_token();
        g_last_record_idx = ts->idx;
        return PT_POINTER;
    }
    if (accept(TK_TEXT)) return PT_FILE;
    if (accept(TK_FILE)) {
        // Optional `of T` — accepted and ignored; we only support text.
        if (accept(TK_OF)) {
            int32_t a, b, c2, d2;
            bool h2 = false;
            (void)parse_type(&a, &b, &c2, &d2, &h2);
        }
        return PT_FILE;
    }
    if (accept(TK_STRING)) {
        // Optional `[N]` length spec — accepted and discarded.
        if (accept(TK_LBRACK)) {
            if (tk == TK_INT) next_token();
            expect(TK_RBRACK, "']'");
        }
        return PT_STR;
    }
    if (accept(TK_SET)) {
        // `set of <element-type>` — we only really care that it's a
        // set; the element type is parsed and discarded.
        expect(TK_OF, "'of'");
        // Element type can be a subrange, integer, or a previously
        // declared enum type.  We just skip through it.
        if (accept(TK_INTEGER)) {}
        else if (tk == TK_INT || tk == TK_MINUS || tk == TK_PLUS) {
            // subrange: parse_type's subrange branch will consume.
            int32_t a, b, c2, d2;
            bool h2 = false;
            (void)parse_type(&a, &b, &c2, &d2, &h2);
        } else if (tk == TK_ID) {
            next_token();   // assume enum type alias
        } else {
            pascal_error("expected set element type");
        }
        return PT_SET;
    }
    // Subrange syntax `lo..hi` (with optional sign on each end) — used
    // standalone as a type or inline inside `array[…]`.  We treat it
    // as plain integer at runtime; the bounds are recorded only for
    // future range checks (not yet enforced).
    if (tk == TK_INT || tk == TK_MINUS || tk == TK_PLUS) {
        // Probe: only commit to subrange if the next non-sign token is
        // `..`.
        const char *save = src;
        int save_line = line_no;
        int save_tk = tk;
        int64_t save_int = tk_int;
        char save_id[256]; strncpy(save_id, tk_id, 255); save_id[255] = 0;
        char save_str[1024]; strncpy(save_str, tk_str, 1023); save_str[1023] = 0;
        int sign = 1;
        if (accept(TK_MINUS)) sign = -1;
        else if (accept(TK_PLUS)) sign = 1;
        if (tk == TK_INT) {
            int64_t lo_v = sign * tk_int;
            int64_t save_int2 = tk_int;
            const char *src2 = src;
            int line2 = line_no;
            next_token();
            if (tk == TK_DOTDOT) {
                next_token();
                int s2 = 1;
                if (accept(TK_MINUS)) s2 = -1;
                else if (accept(TK_PLUS)) s2 = 1;
                if (tk != TK_INT) pascal_error("expected upper bound of subrange");
                int64_t hi_v = s2 * tk_int;
                next_token();
                extern int32_t g_last_subrange_lo, g_last_subrange_hi;
                extern bool g_last_subrange_set;
                g_last_subrange_lo  = (int32_t)lo_v;
                g_last_subrange_hi  = (int32_t)hi_v;
                g_last_subrange_set = true;
                return PT_INT;
            }
            // Not a subrange — rewind to the integer token.
            src = src2; line_no = line2; tk = TK_INT; tk_int = save_int2;
            // But we already consumed sign; we need to rewind that too.
            (void)save_int;
            // Easier: restore everything from the saved point.
            src = save; line_no = save_line; tk = save_tk; tk_int = save_int;
            strncpy(tk_id, save_id, sizeof(tk_id) - 1);
            strncpy(tk_str, save_str, sizeof(tk_str) - 1);
        } else {
            src = save; line_no = save_line; tk = save_tk; tk_int = save_int;
            strncpy(tk_id, save_id, sizeof(tk_id) - 1);
            strncpy(tk_str, save_str, sizeof(tk_str) - 1);
        }
    }
    if (tk == TK_ID) {
        struct sym *s = sym_find_global(tk_id);
        if (s && s->kind == SYM_TYPE) {
            next_token();
            return s->type;
        }
        if (s && s->kind == SYM_RECTYPE) {
            next_token();
            g_last_record_idx = s->idx;
            return PT_RECORD;
        }
        if (s && s->kind == SYM_CLASSTYPE) {
            next_token();
            g_last_class_idx  = s->idx;
            g_last_record_idx = class_types[s->idx].rec_idx;
            return PT_POINTER;
        }
    }
    if (accept(TK_RECORD)) {
        // Inline `record field-list [variant-part] end`.
        if (n_record_types >= MAX_RECORD_TYPES) pascal_error("too many record types");
        int rec_idx = n_record_types++;
        struct record_type *rt = &record_types[rec_idx];
        memset(rt, 0, sizeof(*rt));
        snprintf(rt->name, sizeof(rt->name), "<anon_%d>", rec_idx);
        int offset = 0;

        // Inner helper to consume one field group `id (, id)* : type`
        // and append it to rt->fields starting at `*p_offset`.  Used
        // for both fixed fields and the inside of each variant.
        // (Implemented inline — no nested function in C; we just
        // duplicate the loop.)

        while (tk != TK_END && tk != TK_CASE && tk != TK_EOF) {
            char fnames[16][48]; int fcount = 0;
            for (;;) {
                if (tk != TK_ID) pascal_error("expected field name in record");
                if (fcount >= 16) pascal_error("too many fields per group");
                strncpy(fnames[fcount], tk_id, 47); fnames[fcount][47] = 0;
                fcount++; next_token();
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_COLON, "':'");
            // Field type: full set — int/bool/real/record/array/string/pointer.
            int32_t flo, fhi, flo2, fhi2;
            bool fhas2 = false;
            int felem = PT_INT;
            int frec = -1;
            int ft = parse_type_with_rec(&flo, &fhi, &flo2, &fhi2, &fhas2, &felem, &frec);
            int elem_slots = 1;     // slots per array element
            int elem_rec   = -1;
            int arr_total  = 0;
            if (ft == PT_ARRAY) {
                if (fhas2) pascal_error("2D array fields not supported");
                if (felem == PT_RECORD) {
                    elem_slots = record_types[g_last_record_idx].total_slots;
                    elem_rec   = g_last_record_idx;
                }
                arr_total = (fhi - flo + 1) * elem_slots;
                if (arr_total <= 0) pascal_error("bad array field bounds");
            }
            for (int i = 0; i < fcount; i++) {
                if (rt->nfields >= 24) pascal_error("too many fields in record");
                struct record_field *f = &rt->fields[rt->nfields++];
                memset(f, 0, sizeof(*f));
                strncpy(f->name, fnames[i], sizeof(f->name) - 1);
                f->name[sizeof(f->name) - 1] = 0;
                f->offset  = offset;
                f->type    = ft;
                f->rec_idx = (ft == PT_RECORD) ? frec : -1;
                if (ft == PT_RECORD) {
                    offset += record_types[frec].total_slots;
                } else if (ft == PT_ARRAY) {
                    f->lo = flo; f->hi = fhi;
                    f->elem_type = felem;
                    f->arr_elem_size = elem_slots;
                    f->arr_elem_rec  = elem_rec;
                    offset += arr_total;
                } else {
                    offset += 1;
                }
            }
            if (!accept(TK_SEMI)) break;
        }

        // Variant part: `case [tag-name :] type of  v1: (fields); v2: (fields); …`
        if (accept(TK_CASE)) {
            // Optionally a tag name `id :`.
            if (tk == TK_ID) {
                const char *save_src = src; int save_line = line_no;
                int save_tk = tk; int64_t save_ti = tk_int;
                char save_id[256]; strncpy(save_id, tk_id, 255); save_id[255] = 0;
                next_token();
                if (accept(TK_COLON)) {
                    // It's a tag — register a fixed integer field.
                    if (rt->nfields >= 24) pascal_error("too many fields in record");
                    struct record_field *f = &rt->fields[rt->nfields++];
                    strncpy(f->name, save_id, sizeof(f->name) - 1);
                    f->name[sizeof(f->name) - 1] = 0;
                    f->offset = offset++;
                    f->type   = PT_INT;
                } else {
                    // Rewind — the ID was a type alias name.
                    src = save_src; line_no = save_line;
                    tk = save_tk; tk_int = save_ti;
                    strncpy(tk_id, save_id, sizeof(tk_id) - 1);
                }
            }
            // Discriminant type — parsed and discarded.
            int32_t a, b, c2, d2; bool h2 = false;
            (void)parse_type(&a, &b, &c2, &d2, &h2);
            expect(TK_OF, "'of'");

            int variant_base = offset;
            int max_variant_end = offset;

            while (tk != TK_END && tk != TK_EOF) {
                // Label list: const_int (',' const_int)*  — consumed.
                for (;;) {
                    int sign = 1;
                    if (accept(TK_MINUS)) sign = -1;
                    else if (accept(TK_PLUS)) sign = 1;
                    if (tk != TK_INT) pascal_error("expected integer label in variant");
                    (void)sign; next_token();
                    if (!accept(TK_COMMA)) break;
                }
                expect(TK_COLON, "':'");
                expect(TK_LPAREN, "'('");
                int variant_offset = variant_base;
                while (tk != TK_RPAREN) {
                    char fnames[16][48]; int fcount = 0;
                    for (;;) {
                        if (tk != TK_ID) pascal_error("expected field name in variant");
                        strncpy(fnames[fcount], tk_id, 47); fnames[fcount][47] = 0;
                        fcount++; next_token();
                        if (!accept(TK_COMMA)) break;
                    }
                    expect(TK_COLON, "':'");
                    int ft;
                    if (accept(TK_INTEGER))      ft = PT_INT;
                    else if (accept(TK_BOOLEAN)) ft = PT_BOOL;
                    else if (accept(TK_REAL))    ft = PT_REAL;
                    else pascal_error("variant field must be integer/boolean/real");
                    for (int i = 0; i < fcount; i++) {
                        if (rt->nfields >= 24) pascal_error("too many fields in record");
                        struct record_field *f = &rt->fields[rt->nfields++];
                        strncpy(f->name, fnames[i], sizeof(f->name) - 1);
                        f->name[sizeof(f->name) - 1] = 0;
                        f->offset = variant_offset++;
                        f->type   = ft;
                    }
                    if (!accept(TK_SEMI)) break;
                }
                expect(TK_RPAREN, "')'");
                if (variant_offset > max_variant_end) max_variant_end = variant_offset;
                if (!accept(TK_SEMI)) break;
            }
            offset = max_variant_end;
        }

        expect(TK_END, "'end'");
        rt->total_slots = offset;
        g_last_record_idx = rec_idx;
        return PT_RECORD;
    }
    if (accept(TK_ARRAY)) {
        // `array of T` (no [lo..hi]) — dynamic array.
        if (accept(TK_OF)) {
            int elem = PT_INT;
            if      (accept(TK_INTEGER)) elem = PT_INT;
            else if (accept(TK_BOOLEAN)) elem = PT_BOOL;
            else if (accept(TK_REAL))    elem = PT_REAL;
            else if (accept(TK_STRING))  elem = PT_STR;
            else pascal_error("dynamic array element must be integer/boolean/real/string");
            extern int g_last_array_elem;
            g_last_array_elem = elem;
            return PT_DYNARR;
        }
        expect(TK_LBRACK, "'['");
        int sign = 1;
        if (accept(TK_MINUS)) sign = -1;
        if (tk != TK_INT) pascal_error("expected array lower bound");
        int32_t lo = sign * (int32_t)tk_int;
        next_token();
        expect(TK_DOTDOT, "'..'");
        sign = 1;
        if (accept(TK_MINUS)) sign = -1;
        if (tk != TK_INT) pascal_error("expected array upper bound");
        int32_t hi = sign * (int32_t)tk_int;
        next_token();
        // Optional 2nd dimension: `array[lo..hi, lo2..hi2]`
        bool got2 = false;
        int32_t lo2 = 0, hi2 = 0;
        if (accept(TK_COMMA)) {
            sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            if (tk != TK_INT) pascal_error("expected 2nd-dim lower bound");
            lo2 = sign * (int32_t)tk_int;
            next_token();
            expect(TK_DOTDOT, "'..'");
            sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            if (tk != TK_INT) pascal_error("expected 2nd-dim upper bound");
            hi2 = sign * (int32_t)tk_int;
            next_token();
            got2 = true;
        }
        expect(TK_RBRACK, "']'");
        expect(TK_OF, "'of'");
        // Either the element type, or another `array[lo2..hi2] of ...`
        // (the alternate Pascal syntax for a 2D array).
        if (!got2 && accept(TK_ARRAY)) {
            expect(TK_LBRACK, "'['");
            sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            if (tk != TK_INT) pascal_error("expected 2nd-dim lower bound");
            lo2 = sign * (int32_t)tk_int;
            next_token();
            expect(TK_DOTDOT, "'..'");
            sign = 1;
            if (accept(TK_MINUS)) sign = -1;
            if (tk != TK_INT) pascal_error("expected 2nd-dim upper bound");
            hi2 = sign * (int32_t)tk_int;
            next_token();
            expect(TK_RBRACK, "']'");
            expect(TK_OF, "'of'");
            got2 = true;
        }
        // Element type — also stash it so callers that care can read
        // it back via parse_type_with_elem (avoids passing yet another
        // out-pointer through every caller).
        extern int g_last_array_elem;
        if      (accept(TK_INTEGER)) g_last_array_elem = PT_INT;
        else if (accept(TK_BOOLEAN)) g_last_array_elem = PT_BOOL;
        else if (accept(TK_REAL))    g_last_array_elem = PT_REAL;
        else pascal_error("array element must be integer/boolean/real");
        if (lo_out) *lo_out = lo;
        if (hi_out) *hi_out = hi;
        if (got2) {
            if (lo2_out) *lo2_out = lo2;
            if (hi2_out) *hi2_out = hi2;
            if (has2)    *has2    = true;
        }
        return PT_ARRAY;
    }
    pascal_error("expected type at line %d", line_no);
    return -1;
}

// Track the element type of the most recent array decl so the table
// is initialized accurately.  parse_type only reports PT_ARRAY here;
// for the element type we re-peek the keyword inside parse_type — but
// to keep the change small, we record array element type via a
// follow-up mini scan.  Simpler: parse_type now always sets *_lo/_hi
// for arrays, and we store the element type as PT_INT (we don't
// distinguish int vs bool vs real on the array yet — extend later).
int g_last_array_elem;     // set by parse_type when it scans an array element kw
int32_t g_last_subrange_lo, g_last_subrange_hi;
bool    g_last_subrange_set;

static int
parse_type_with_elem(int32_t *lo, int32_t *hi, int32_t *lo2, int32_t *hi2,
                     bool *has2, int *elem_out)
{
    int t = parse_type(lo, hi, lo2, hi2, has2);
    *elem_out = (t == PT_ARRAY) ? g_last_array_elem : t;
    return t;
}

static int
parse_type_with_rec(int32_t *lo, int32_t *hi, int32_t *lo2, int32_t *hi2,
                    bool *has2, int *elem_out, int *rec_out)
{
    int t = parse_type_with_elem(lo, hi, lo2, hi2, has2, elem_out);
    *rec_out = (t == PT_RECORD || t == PT_POINTER) ? g_last_record_idx : -1;
    return t;
}

static void
parse_var_decls_global(void)
{
    while (tk == TK_ID) {
        char names[16][64];
        int  nnames = 0;
        for (;;) {
            if (tk != TK_ID) pascal_error("expected identifier in var decl");
            if (nnames >= 16) pascal_error("too many names per var decl");
            strncpy(names[nnames], tk_id, 63);
            names[nnames][63] = 0;
            nnames++;
            next_token();
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_COLON, "':'");
        int32_t lo = 0, hi = 0, lo2 = 0, hi2 = 0;
        bool has2 = false;
        int elem = PT_INT;
        int rec = -1;
        int t = parse_type_with_rec(&lo, &hi, &lo2, &hi2, &has2, &elem, &rec);
        expect(TK_SEMI, "';'");
        for (int i = 0; i < nnames; i++) {
            struct sym *s = sym_add_global(names[i]);
            if (t == PT_ARRAY) {
                s->kind = SYM_GARR;
                s->idx  = n_globals_alloc++;
                s->lo   = lo;
                s->hi   = hi;
                s->is_2d = has2;
                s->lo2  = lo2;
                s->hi2  = hi2;
                s->type = elem;
            } else if (t == PT_RECORD) {
                int slots = record_types[rec].total_slots;
                s->kind = SYM_GREC;
                s->idx  = n_globals_alloc;
                s->rec_idx = rec;
                n_globals_alloc += slots;
            } else if (t == PT_POINTER) {
                s->kind      = SYM_GVAR;
                s->idx       = n_globals_alloc++;
                s->type      = PT_POINTER;
                s->rec_idx   = rec;
                s->class_idx = g_last_class_idx;
            } else if (t == PT_DYNARR) {
                s->kind = SYM_GVAR;
                s->idx  = n_globals_alloc++;
                s->type = PT_DYNARR;
                s->rec_idx = g_last_array_elem;   // element type
            } else {
                s->kind = SYM_GVAR;
                s->idx  = n_globals_alloc++;
                s->type = t;
                if (g_last_subrange_set) {
                    s->has_range = true;
                    s->lo = g_last_subrange_lo;
                    s->hi = g_last_subrange_hi;
                }
            }
        }
    }
}

static void
parse_var_decls_local(void)
{
    while (tk == TK_ID) {
        char names[16][64];
        int  nnames = 0;
        for (;;) {
            if (tk != TK_ID) pascal_error("expected identifier in var decl");
            if (nnames >= 16) pascal_error("too many names per var decl");
            strncpy(names[nnames], tk_id, 63);
            names[nnames][63] = 0;
            nnames++;
            next_token();
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_COLON, "':'");
        int32_t lo, hi, lo2, hi2;
        bool has2 = false;
        int elem = PT_INT;
        int rec = -1;
        int t = parse_type_with_rec(&lo, &hi, &lo2, &hi2, &has2, &elem, &rec);
        if (t == PT_ARRAY && has2) pascal_error("2D local arrays not supported");
        expect(TK_SEMI, "';'");
        for (int i = 0; i < nnames; i++) {
            if (t == PT_ARRAY) {
                int32_t size = hi - lo + 1;
                if (size <= 0) pascal_error("bad local array bounds");
                struct sym *s = sym_add_local(names[i]);
                s->kind = SYM_LARR;
                s->type = elem;
                s->lo   = lo;
                s->hi   = hi;
                n_locals_alloc += size - 1;
            } else if (t == PT_RECORD) {
                int slots = record_types[rec].total_slots;
                struct sym *s = sym_add_local(names[i]);
                s->kind = SYM_LREC;
                s->rec_idx = rec;
                n_locals_alloc += slots - 1;
            } else if (t == PT_POINTER) {
                struct sym *s = sym_add_local(names[i]);
                s->type      = PT_POINTER;
                s->rec_idx   = rec;
                s->class_idx = g_last_class_idx;
            } else if (t == PT_DYNARR) {
                struct sym *s = sym_add_local(names[i]);
                s->type = PT_DYNARR;
                s->rec_idx = g_last_array_elem;
            } else {
                struct sym *s = sym_add_local(names[i]);
                s->type = t;
                if (g_last_subrange_set) {
                    s->has_range = true;
                    s->lo = g_last_subrange_lo;
                    s->hi = g_last_subrange_hi;
                }
            }
        }
    }
}

// `type` block — defines named aliases for a base type.  Three kinds:
//   - enum:  `name = (id1, id2, ...)`  — adds id1..idN as SYM_CONST 0..N-1
//                                        (base type PT_INT)
//   - subrange: `name = lo..hi`        — base PT_INT
//   - alias: `name = integer | boolean | real | <other_type>`
// All three end up as a SYM_TYPE entry whose `type` field is the base
// PT_* code.  We don't enforce range/enum membership at run time.
static void
parse_type_decls_global(void)
{
    while (tk == TK_ID) {
        char name[64]; strncpy(name, tk_id, 63); name[63] = 0;
        next_token();
        expect(TK_EQ, "'='");

        if (accept(TK_LPAREN)) {
            // Enum.  Add each name as SYM_CONST with sequential value.
            int64_t v = 0;
            if (tk != TK_RPAREN) for (;;) {
                if (tk != TK_ID) pascal_error("expected enum element name");
                char ename[64]; strncpy(ename, tk_id, 63); ename[63] = 0;
                next_token();
                struct sym *es = sym_add_global(ename);
                es->kind = SYM_CONST;
                es->cval = v++;
                es->type = PT_INT;
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_RPAREN, "')'");
            struct sym *ts = sym_add_global(name);
            ts->kind = SYM_TYPE;
            ts->type = PT_INT;
        } else {
            // Subrange, plain type, or record.  parse_type recognises
            // all three; record returns PT_RECORD with rec_idx in
            // g_last_record_idx.
            int32_t lo, hi, lo2, hi2;
            bool has2 = false;
            int elem = PT_INT;
            int rec = -1;
            int t = parse_type_with_rec(&lo, &hi, &lo2, &hi2, &has2, &elem, &rec);
            if (t == PT_ARRAY)
                pascal_error("array type aliases not yet supported");
            struct sym *ts = sym_add_global(name);
            if (t == PT_RECORD) {
                ts->kind = SYM_RECTYPE;
                ts->idx  = rec;
                strncpy(record_types[rec].name, name, sizeof(record_types[rec].name) - 1);
                record_types[rec].name[sizeof(record_types[rec].name) - 1] = 0;
            } else if (t == PT_POINTER && g_last_class_idx >= 0
                       && class_types[g_last_class_idx].rec_idx == rec) {
                // Class type — register as SYM_CLASSTYPE and stamp
                // the class's name on its record_type.
                int cidx = g_last_class_idx;
                ts->kind = SYM_CLASSTYPE;
                ts->idx  = cidx;
                ts->rec_idx = class_types[cidx].rec_idx;
                strncpy(class_types[cidx].name, name, sizeof(class_types[cidx].name) - 1);
                strncpy(record_types[class_types[cidx].rec_idx].name, name,
                        sizeof(record_types[0].name) - 1);
                g_last_class_idx = -1;     // consumed
            } else {
                ts->kind = SYM_TYPE;
                ts->type = t;
            }
        }
        expect(TK_SEMI, "';'");
    }
}

static void
parse_const_decls_global(void)
{
    while (tk == TK_ID) {
        char name[64]; strncpy(name, tk_id, 63); name[63] = 0;
        next_token();
        expect(TK_EQ, "'='");
        int sign = 1;
        if (accept(TK_MINUS)) sign = -1;
        else if (accept(TK_PLUS)) sign = 1;

        struct sym *s = sym_add_global(name);
        if (tk == TK_INT) {
            int64_t v = sign * tk_int;
            next_token();
            s->kind = SYM_CONST;
            s->cval = v;
            s->type = PT_INT;
        } else if (tk == TK_RNUM) {
            double d = sign * tk_real;
            next_token();
            union { double d; int64_t i; } u; u.d = d;
            s->kind = SYM_RCONST;
            s->cval = u.i;
            s->type = PT_REAL;
        } else if (tk == TK_STR) {
            // String const: allocate a permanent buffer under libgc.
            size_t len = strlen(tk_str);
            char *buf = (char *)GC_malloc_atomic(len + 1);
            memcpy(buf, tk_str, len + 1);
            next_token();
            s->kind = SYM_CONST;
            s->cval = (int64_t)(uintptr_t)buf;
            s->type = PT_STR;
        } else {
            pascal_error("only integer/real/string const is supported");
        }
        expect(TK_SEMI, "';'");
    }
}

// procedure / function header + body.  Two paths share the same code:
//  - first encounter: reserve proc slot, add to gsyms, parse signature.
//    If body is `forward;`, leave body=NULL and return; else parse body.
//  - re-encounter (resolving a forward decl): reuse the existing slot,
//    re-parse signature (must match), then parse body.
// Track which class a method body belongs to (the param-list parser
// uses this to inject the implicit `Self` parameter and also to put
// fields in scope via the with-stack).  current_method_name lets
// `inherited` find the same-named method in the parent.
int  current_method_class_idx = -1;
char current_method_name_buf[64];

static void
parse_subprogram(bool is_function, CTX *c)
{
    bool is_constructor = false;
    bool is_class_method = false;
    // `class procedure` / `class function` — outside-the-class body of
    // a class method.  No implicit Self.
    if (accept(TK_CLASS)) {
        is_class_method = true;
        if (tk == TK_FUNCTION) is_function = true;
        next_token();   // consume the following procedure/function
    } else if (accept(TK_CONSTRUCTOR)) {
        is_constructor = true;
        is_function    = true;     // constructor returns the new object pointer
    } else if (accept(TK_DESTRUCTOR)) {
        // Treat destructor like a regular procedure — the keyword is
        // already consumed by accept, the method name follows.
    } else {
        next_token();              // consume 'procedure' / 'function'
    }
    if (tk != TK_ID) pascal_error("expected name after procedure/function");
    char name[64]; strncpy(name, tk_id, 63); name[63] = 0;
    next_token();

    // `T.method` — method body declaration.
    int method_class = -1;
    char method_name[64] = "";
    if (accept(TK_DOT)) {
        struct sym *cs = sym_find_global(name);
        if (!cs || cs->kind != SYM_CLASSTYPE) pascal_error("'%s' is not a class", name);
        method_class = cs->idx;
        if (tk != TK_ID) pascal_error("expected method name after '.'");
        strncpy(method_name, tk_id, 63); method_name[63] = 0;
        next_token();
    }

    // For nested procedures we don't add the proc symbol globally —
    // we keep it in the lsyms stack so it's only visible inside its
    // enclosing scope.  Forward declarations (`forward;`) only work
    // at the top level for now.
    int pidx;
    bool resolving_forward = false;
    bool is_nested = (current_depth > 0);
    if (method_class >= 0) {
        // Method body — fresh proc, no global symbol.
        if (c->nprocs >= PASCAL_MAX_PROCS) pascal_error("too many procs");
        pidx = c->nprocs++;
        struct pascal_proc *p0 = &c->procs[pidx];
        memset(p0, 0, sizeof(*p0));
        char mangled[128];
        snprintf(mangled, sizeof(mangled), "%s.%s", name, method_name);
        p0->name = strdup(mangled);
        p0->is_function = is_function;
        p0->lexical_depth = 1;
        // Patch / register the method in the class's method table.
        struct class_type *ct = &class_types[method_class];
        struct method_entry *me = NULL;
        for (int i = 0; i < ct->nmethods; i++) {
            if (strcmp(ct->methods[i].name, method_name) == 0) { me = &ct->methods[i]; break; }
        }
        if (!me) {
            if (ct->nmethods >= 32) pascal_error("too many methods");
            me = &ct->methods[ct->nmethods++];
            memset(me, 0, sizeof(*me));
            strncpy(me->name, method_name, sizeof(me->name) - 1);
        }
        me->proc_idx = pidx;
        if (is_constructor) me->is_constructor = true;
    } else if (!is_nested) {
        struct sym *existing = sym_find_global(name);
        if (existing && existing->kind == SYM_PROC
            && c->procs[existing->idx].body == NULL) {
            pidx = existing->idx;
            resolving_forward = true;
        } else if (existing) {
            pascal_error("duplicate symbol '%s'", name);
        } else {
            if (c->nprocs >= PASCAL_MAX_PROCS) pascal_error("too many procs");
            pidx = c->nprocs++;
            struct pascal_proc *p0 = &c->procs[pidx];
            memset(p0, 0, sizeof(*p0));
            p0->name = strdup(name);
            p0->is_function = is_function;
            p0->lexical_depth = 1;
            struct sym *psym = sym_add_global(name);
            psym->kind = SYM_PROC;
            psym->idx  = pidx;
        }
    } else {
        // Nested.  Add to lsyms (visible only in the enclosing scope).
        if (c->nprocs >= PASCAL_MAX_PROCS) pascal_error("too many procs");
        pidx = c->nprocs++;
        struct pascal_proc *p0 = &c->procs[pidx];
        memset(p0, 0, sizeof(*p0));
        p0->name = strdup(name);
        p0->is_function = is_function;
        p0->lexical_depth = current_depth + 1;
        struct sym *psym = sym_add_local(name);
        // We hijacked sym_add_local which set kind=SYM_LVAR and
        // bumped n_locals_alloc; undo that — a proc isn't a slot.
        psym->kind = SYM_PROC;
        psym->idx  = pidx;
        n_locals_alloc--;
    }
    struct pascal_proc *p = &c->procs[pidx];

    // Push a new scope.  Inner-scope syms accumulate on top of the
    // existing lsyms; on exit we truncate back to the saved n_lsyms.
    int saved_n_lsyms        = n_lsyms;
    int saved_n_locals_alloc = n_locals_alloc;
    int saved_method_class   = current_method_class_idx;
    current_depth++;
    if (current_depth >= PASCAL_MAX_DEPTH) pascal_error("nesting too deep");
    scope_start_at_depth[current_depth] = n_lsyms;
    proc_at_depth[current_depth]        = p;
    proc_idx_at_depth[current_depth]    = pidx;
    n_locals_alloc = 0;
    current_method_class_idx = method_class;
    if (method_class >= 0) {
        strncpy(current_method_name_buf, method_name, sizeof(current_method_name_buf) - 1);
        current_method_name_buf[sizeof(current_method_name_buf) - 1] = 0;
    } else {
        current_method_name_buf[0] = 0;
    }

    // Inject the implicit `Self` parameter for instance methods.
    // Class methods (declared `class procedure T.foo`) skip this.
    int nparams = 0;
    if (method_class >= 0 && !is_class_method) {
        struct sym *self_s = sym_add_local("self");
        self_s->type      = PT_POINTER;
        self_s->rec_idx   = class_types[method_class].rec_idx;
        self_s->class_idx = method_class;
        p->param_by_ref[0]   = false;
        p->param_is_array[0] = false;
        p->param_type[0]     = (char)PT_POINTER;
        nparams = 1;
    }

    if (accept(TK_LPAREN)) {
        if (tk != TK_RPAREN) for (;;) {
            bool by_ref = accept(TK_VAR);
            // Pascal grouped param: id (, id)* : type
            char pnames[PASCAL_MAX_PARAMS][64];
            int  pcount = 0;
            for (;;) {
                if (tk != TK_ID) pascal_error("expected param name");
                if (pcount >= PASCAL_MAX_PARAMS) pascal_error("too many param names per group");
                strncpy(pnames[pcount], tk_id, 63);
                pnames[pcount][63] = 0;
                pcount++;
                next_token();
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_COLON, "':'");
            int32_t lo, hi, lo2, hi2;
            bool has2 = false;
            int elem;
            int rec = -1;
            int t = parse_type_with_rec(&lo, &hi, &lo2, &hi2, &has2, &elem, &rec);
            if (t == PT_ARRAY && !by_ref)
                pascal_error("array params must be `var`");
            if (t == PT_ARRAY && has2)
                pascal_error("2D var-array params not yet supported");
            if (t == PT_RECORD && !by_ref)
                pascal_error("record params must be `var`");
            // `array of T` (open array, no bounds) becomes a PT_DYNARR
            // value-passed param.  Element type sits in the global
            // g_last_array_elem set by parse_type.  The callee uses
            // length(a) / a[i] / a[i] := v exactly as for a local
            // dynamic array.  Caller side accepts a dynarr expression
            // directly, or a static-array name (auto-wrapped at the
            // call site — see parse_typed_call_args).
            extern int g_last_array_elem;
            int oarr_elem = (t == PT_DYNARR) ? g_last_array_elem : 0;
            for (int i = 0; i < pcount; i++) {
                if (nparams >= PASCAL_MAX_PARAMS) pascal_error("too many params");
                struct sym *ls = sym_add_local(pnames[i]);
                if (t == PT_ARRAY) {
                    ls->kind = SYM_VARR;
                    ls->type = elem;
                    ls->lo   = lo;
                    ls->hi   = hi;
                    p->param_by_ref[nparams]   = true;
                    p->param_is_array[nparams] = true;
                    p->param_arr_lo[nparams]   = lo;
                    p->param_type[nparams]     = (char)elem;
                } else if (t == PT_DYNARR) {
                    if (by_ref) pascal_error("open-array param must not be `var`");
                    ls->type    = PT_DYNARR;
                    ls->rec_idx = oarr_elem;        // dynarr elem type
                    p->param_by_ref[nparams]   = false;
                    p->param_is_array[nparams] = false;
                    p->param_type[nparams]     = (char)PT_DYNARR;
                    p->param_arr_lo[nparams]   = oarr_elem; // stash elem
                } else if (t == PT_RECORD) {
                    ls->kind = SYM_VREC;
                    ls->rec_idx = rec;
                    p->param_by_ref[nparams]   = true;
                    p->param_is_array[nparams] = true;   // pass base address, not lvalue
                    p->param_arr_lo[nparams]   = 0;
                    p->param_type[nparams]     = (char)PT_INT; // unused for records
                } else {
                    ls->type   = t;
                    ls->by_ref = by_ref;
                    p->param_by_ref[nparams]   = by_ref;
                    p->param_is_array[nparams] = false;
                    p->param_type[nparams]     = (char)t;
                }
                nparams++;
            }
            if (!accept(TK_SEMI)) break;
        }
        expect(TK_RPAREN, "')'");
    }
    if (resolving_forward && p->nparams != nparams)
        pascal_error("forward decl arity mismatch for '%s'", name);
    p->nparams = nparams;

    // Function return type.  Constructors have an implicit return
    // type of the class pointer; the body needn't `Result :=`
    // explicitly because the call-site path will treat the allocated
    // object as the return value.
    if (is_function) {
        if (is_constructor) {
            p->return_type = PT_POINTER;
        } else {
            expect(TK_COLON, "':'");
            int32_t lo, hi, lo2, hi2;
            bool has2 = false;
            int elem;
            int rt = parse_type_with_elem(&lo, &hi, &lo2, &hi2, &has2, &elem);
            if (rt == PT_ARRAY) pascal_error("function may not return array");
            p->return_type = (char)rt;
        }
        p->return_slot = n_locals_alloc++;
    }
    expect(TK_SEMI, "';'");

    // `forward;` — header-only declaration.  Body comes later.
    // Inside a unit's interface section, every signature is body-less
    // (the bodies live in the implementation section); the
    // `parsing_interface` flag handles those.
    extern bool parsing_interface;
    if (accept(TK_FORWARD) || parsing_interface) {
        if (!parsing_interface) expect(TK_SEMI, "';'");
        p->nslots = n_locals_alloc;
        p->body   = NULL;
        current_depth--;
        n_lsyms        = saved_n_lsyms;
        n_locals_alloc = saved_n_locals_alloc;
        current_method_class_idx = saved_method_class;
        return;
    }

    // Body section: any combination of var, const, type, nested
    // procedure / function declarations, then begin..end.
    for (;;) {
        if (accept(TK_VAR))   parse_var_decls_local();
        else if (accept(TK_CONST)) parse_const_decls_global(); // local const ≈ global const
        else if (accept(TK_TYPE))  parse_type_decls_global();
        else if (tk == TK_PROCEDURE) parse_subprogram(false, c);
        else if (tk == TK_FUNCTION)  parse_subprogram(true, c);
        else break;
    }

    current_proc     = p;
    current_proc_idx = pidx;

    NODE *body = parse_compound();
    expect(TK_SEMI, "';'");

    // Constructors return the freshly-allocated Self pointer.
    if (is_constructor && !is_class_method) {
        NODE *seed = ALLOC_node_lset((uint32_t)p->return_slot,
                                     ALLOC_node_lref(0));
        body = mk_seq(seed, body);
    }

    p->body   = body;
    p->nslots = n_locals_alloc;

    // Pop the scope: truncate lsyms back, restore the caller's
    // n_locals_alloc, restore current_depth.
    current_depth--;
    n_lsyms        = saved_n_lsyms;
    n_locals_alloc = saved_n_locals_alloc;
    current_method_class_idx = saved_method_class;
    if (current_depth > 0) {
        current_proc     = proc_at_depth[current_depth];
        current_proc_idx = proc_idx_at_depth[current_depth];
    } else {
        current_proc     = NULL;
        current_proc_idx = 0;
    }
}

// ---------------------------------------------------------------------------
// Top-level program.
// ---------------------------------------------------------------------------

static NODE *
parse_program(CTX *c)
{
    parser_ctx = c;
    if (accept(TK_PROGRAM)) {
        if (tk == TK_ID) next_token();
        // optional `(input, output)` list
        if (accept(TK_LPAREN)) {
            while (tk != TK_RPAREN && tk != TK_EOF) next_token();
            expect(TK_RPAREN, "')'");
        }
        expect(TK_SEMI, "';'");
    }

    if (accept(TK_USES)) {
        for (;;) {
            if (tk != TK_ID) pascal_error("expected unit name in `uses`");
            char unit[64]; strncpy(unit, tk_id, 63); unit[63] = 0;
            next_token();
            parse_unit_file(unit, c);
            if (!accept(TK_COMMA)) break;
        }
        expect(TK_SEMI, "';'");
    }

    parse_decls_block(c);

    NODE *body = parse_compound();
    expect(TK_DOT, "'.'");

    for (int i = 0; i < c->nprocs; i++) {
        if (c->procs[i].body == NULL)
            pascal_error("forward declaration '%s' has no body", c->procs[i].name);
    }
    return body;
}

// Parse a sequence of var / const / type / procedure / function /
// uses declarations.  Stops at any other token.  Reused by the main
// program parser and by the unit-file parser.
static void
parse_decls_block(CTX *c)
{
    for (;;) {
        if (accept(TK_USES)) {
            for (;;) {
                if (tk != TK_ID) pascal_error("expected unit name in `uses`");
                char unit[64]; strncpy(unit, tk_id, 63); unit[63] = 0;
                next_token();
                parse_unit_file(unit, c);
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_SEMI, "';'");
        }
        else if (accept(TK_LABEL)) {
            // `label N1, N2, ...;` — labels are tracked by their
            // number directly (mapped into pascal_label_bufs); the
            // declaration just consumes the list.
            while (tk == TK_INT) {
                if (tk_int < 0 || tk_int >= PASCAL_MAX_LABELS)
                    pascal_error("label %ld out of range [0..%d]", (long)tk_int, PASCAL_MAX_LABELS - 1);
                next_token();
                if (!accept(TK_COMMA)) break;
            }
            expect(TK_SEMI, "';'");
        }
        else if (accept(TK_VAR))   parse_var_decls_global();
        else if (accept(TK_CONST)) parse_const_decls_global();
        else if (accept(TK_TYPE))  parse_type_decls_global();
        else if (tk == TK_PROCEDURE)   parse_subprogram(false, c);
        else if (tk == TK_FUNCTION)    parse_subprogram(true,  c);
        else if (tk == TK_CONSTRUCTOR) parse_subprogram(true,  c);
        else if (tk == TK_DESTRUCTOR)  parse_subprogram(false, c);
        else if (tk == TK_CLASS)       parse_subprogram(false, c);
        else break;
    }
}

// ---------------------------------------------------------------------------
// `read` support (stubbed via a tiny dispatcher; we don't have a NODE
// for it in node.def — we synthesize one with a custom dispatcher
// pointer at parse time).  Returning a bare allocator into the AST kept
// the node.def simple at the cost of one hand-written NODE.
// ---------------------------------------------------------------------------

extern const struct NodeKind kind_node_int;

static VALUE
DISPATCH_node_read_int(CTX *c, NODE *n)
{
    (void)c; (void)n;
    int64_t v = 0;
    if (scanf("%ld", &v) != 1) v = 0;
    return v;
}

static NODE *
node_read_int_dummy(void)
{
    // Borrow node_int's kind so hashing/dump still work; install our
    // own dispatcher so EVAL goes to scanf.  Allocate a full NODE so
    // accesses through the union are well-defined regardless of which
    // node_*_struct is the largest variant.
    NODE *n = (NODE *)calloc(1, sizeof(NODE));
    n->head.kind = &kind_node_int;
    n->head.dispatcher = DISPATCH_node_read_int;
    n->head.dispatcher_name = "DISPATCH_node_read_int";
    n->u.node_int.v = 0;
    n->head.flags.has_hash_value = false;
    return n;
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

static char parser_dir[512];   // directory of the main source, for `uses` lookup
bool parsing_interface = false;

static char *
slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "pascalast: cannot open %s\n", path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (fread(buf, 1, len, fp) != (size_t)len) {
        fprintf(stderr, "pascalast: read error %s\n", path); exit(1);
    }
    buf[len] = 0;
    fclose(fp);
    return buf;
}

// Locate <name>.pas (case-insensitively for the file name) in
// parser_dir.  We try the lower-cased name first, then the literal.
static char *
slurp_unit(const char *name)
{
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.pas", parser_dir[0] ? parser_dir : ".", name);
    if (access(path, R_OK) == 0) return slurp(path);
    // Try lower-case
    char lower[64];
    size_t i = 0;
    for (; i < sizeof(lower) - 1 && name[i]; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    snprintf(path, sizeof(path), "%s/%s.pas", parser_dir[0] ? parser_dir : ".", lower);
    if (access(path, R_OK) == 0) return slurp(path);
    pascal_error("unit '%s' not found (looked for %s.pas)", name, name);
    return NULL;
}


// Parse a unit file as a sequence of declarations and (optional)
// implementation / initialization sections.  Globals declared in the
// unit end up in the same gsyms / record_types / procs as the main
// program — there's no namespace separation.
static void
parse_unit_file(const char *unit_name, CTX *c)
{
    char *source = slurp_unit(unit_name);

    // Save lexer state.
    const char *save_src = src;
    int     save_line = line_no;
    int     save_tk   = tk;
    int64_t save_ti   = tk_int;
    double  save_tr   = tk_real;
    char    save_id[256];  strncpy(save_id,  tk_id,  255); save_id[255] = 0;
    char    save_str[1024];strncpy(save_str, tk_str, 1023); save_str[1023] = 0;

    src = source; line_no = 1;
    next_token();

    if (accept(TK_UNIT)) {
        if (tk == TK_ID) next_token();
        expect(TK_SEMI, "';'");
    }
    if (accept(TK_INTERFACE)) {
        parsing_interface = true;
        parse_decls_block(c);
        parsing_interface = false;
    }
    accept(TK_IMPLEMENTATION);
    parse_decls_block(c);
    if (accept(TK_BEGIN)) {
        // unit initialization — runs at startup if we tracked it; we
        // don't (yet), so just parse and discard.
        (void)parse_stmt_list(TK_END, 0);
        expect(TK_END, "'end'");
    }
    accept(TK_DOT);

    // Restore.
    src = save_src; line_no = save_line;
    tk = save_tk;   tk_int = save_ti;  tk_real = save_tr;
    strncpy(tk_id,  save_id,  sizeof(tk_id) - 1);
    strncpy(tk_str, save_str, sizeof(tk_str) - 1);
}

// Public API exported by node.c for the AOT writer.
char *SPECIALIZED_SRC(NODE *n);
void  sc_repo_clear(void);
uint32_t sc_repo_count(void);
void  sc_repo_get(uint32_t i, node_hash_t *h_out, const char **name_out);

// Walk every AST root (each procedure body and the program body) and
// emit the specialized C translation unit to node_specialized.c, with
// a sc_entries[] table the next build picks up via #include.
static void
write_specialized(CTX *c, NODE *main_body, const char *out_path)
{
    FILE *fp = fopen(out_path, "w");
    if (!fp) pascal_error("cannot write %s", out_path);

    fprintf(fp, "// auto-generated by `pascalast -c FILE.pas` — do not edit\n");
    fprintf(fp, "#define NODE_SPECIALIZED_INCLUDED 1\n\n");

    sc_repo_clear();

    char *buf;
    for (int i = 0; i < c->nprocs; i++) {
        if (c->procs[i].body == NULL) continue;
        buf = SPECIALIZED_SRC(c->procs[i].body);
        if (buf) { fputs(buf, fp); free(buf); }
    }
    if (main_body) {
        buf = SPECIALIZED_SRC(main_body);
        if (buf) { fputs(buf, fp); free(buf); }
    }

    uint32_t n = sc_repo_count();
    fprintf(fp, "\nstatic struct specialized_code sc_entries[] = {\n");
    for (uint32_t i = 0; i < n; i++) {
        node_hash_t h; const char *name;
        sc_repo_get(i, &h, &name);
        fprintf(fp, "    { 0x%lxULL, \"%s\", %s },\n",
                (unsigned long)h, name, name);
    }
    fprintf(fp, "};\n");
    fprintf(fp, "#define SC_ENTRIES_COUNT %u\n", n);
    fclose(fp);
    if (!OPTION.quiet) fprintf(stderr, "wrote %s (%u entries)\n", out_path, n);
}

int
main(int argc, char *argv[])
{
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            OPTION.quiet = true;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            OPTION.dump_ast = true;
        } else if (strcmp(argv[i], "--no-run") == 0) {
            OPTION.no_run = true;
        } else if (strcmp(argv[i], "--no-compile") == 0) {
            OPTION.no_compiled_code = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            OPTION.compile_only = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "pascalast: unknown option %s\n", argv[i]);
            return 1;
        } else {
            file = argv[i];
        }
    }
    if (!file) {
        fprintf(stderr, "usage: pascalast [-q] [-c] [--no-compile] [--dump-ast] FILE.pas\n");
        return 1;
    }

    INIT();
    CTX *c = calloc(1, sizeof(CTX));
    pascal_runtime_ctx = c;
    c->stack = malloc(sizeof(VALUE) * PASCAL_STACK_SIZE);
    c->fp = 0;
    c->sp = 0;

    // Record the source file's directory for `uses` resolution.
    {
        const char *slash = strrchr(file, '/');
        if (slash) {
            size_t n = slash - file;
            if (n >= sizeof(parser_dir)) n = sizeof(parser_dir) - 1;
            memcpy(parser_dir, file, n);
            parser_dir[n] = 0;
        } else {
            strcpy(parser_dir, ".");
        }
    }

    char *source = slurp(file);
    src = source;
    line_no = 1;
    next_token();
    NODE *prog = parse_program(c);

    // Resolve every pcall_K_baked NODE's body + per-proc metadata
    // slots now that all proc bodies have been parsed.  body must be
    // non-NULL by the time SPECIALIZE walks the AST (its
    // DISPATCHER_NAME would otherwise dereference NULL).  An
    // unresolved entry means a `forward;` proc had no matching
    // definition — already rejected earlier in parse_program with
    // "forward declaration … has no body".
    for (uint32_t i = 0; i < pcall_fixups_size; i++) {
        struct pcall_fixup *f = &pcall_fixups[i];
        if (f->pidx >= (uint32_t)c->nprocs) continue;
        struct pascal_proc *p = &c->procs[f->pidx];
        if (!p->body) continue;
        *f->body_slot          = p->body;
        *f->nslots_slot        = (uint32_t)p->nslots;
        *f->return_slot_slot   = (uint32_t)p->return_slot;
        *f->lexical_depth_slot = (uint32_t)p->lexical_depth;
        *f->is_function_slot   = p->is_function ? 1u : 0u;
    }

    // Build per-class vtables now that every method body has been
    // parsed.  We always allocate at least 1 entry so the vtable
    // address is unique-per-class — `is` / `as` rely on identifying
    // a class by its vtable pointer.
    pascal_vtables = (int **)calloc(n_class_types, sizeof(int *));
    for (int i = 0; i < n_class_types; i++) {
        struct class_type *ct = &class_types[i];
        int sz = ct->vtable_size > 0 ? ct->vtable_size : 1;
        int *vt = (int *)calloc(sz, sizeof(int));
        if (ct->parent_idx >= 0 && pascal_vtables[ct->parent_idx]) {
            int parent_size = class_types[ct->parent_idx].vtable_size;
            int copy = parent_size < ct->vtable_size ? parent_size : ct->vtable_size;
            memcpy(vt, pascal_vtables[ct->parent_idx], copy * sizeof(int));
        }
        for (int j = 0; j < ct->nmethods; j++) {
            struct method_entry *me = &ct->methods[j];
            if (me->is_virtual && me->vtable_slot >= 0 && me->proc_idx >= 0) {
                vt[me->vtable_slot] = me->proc_idx;
            }
        }
        ct->vtable = vt;
        pascal_vtables[i] = vt;
    }

    // Allocate global arrays.  2D arrays are stored row-major in a
    // single flat buffer of `size1 * size2` cells.
    for (int i = 0; i < n_gsyms; i++) {
        if (gsyms[i].kind == SYM_GARR) {
            int32_t size1 = gsyms[i].hi - gsyms[i].lo + 1;
            if (size1 <= 0) pascal_error("bad array bounds for '%s'", gsyms[i].name);
            int slot = gsyms[i].idx;
            c->array_lo[slot]   = gsyms[i].lo;
            c->array_size[slot] = size1;
            int32_t total = size1;
            if (gsyms[i].is_2d) {
                int32_t size2 = gsyms[i].hi2 - gsyms[i].lo2 + 1;
                if (size2 <= 0) pascal_error("bad 2nd-dim bounds for '%s'", gsyms[i].name);
                c->array_lo2[slot]   = gsyms[i].lo2;
                c->array_size2[slot] = size2;
                total = size1 * size2;
            }
            c->arrays[slot] = calloc((size_t)total, sizeof(int64_t));
        }
    }

    if (OPTION.dump_ast) {
        DUMP(stderr, prog, false);
        fputc('\n', stderr);
    }
    if (OPTION.compile_only) {
        write_specialized(c, prog, "node_specialized.c");
        return 0;
    }
    if (!OPTION.no_run) EVAL(c, prog);

    return 0;
}
