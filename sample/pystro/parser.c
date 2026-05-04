// parser.c — pystro recursive-descent parser.  Operates on the token
// stream produced by lexer.c, emits an AST built from ALLOC_node_*
// allocators.  Scope-aware: when parsing a `def` body the parser
// pre-scans suite tokens to collect local names, then emits lref/lset
// for those and gref/gset for everything else.

// Side-tables for ASTroGen-opaque NODE* references (call args, list /
// tuple / dict literal items, function defaults).
NODE   **PYSTRO_NODE_TABLE = NULL;
size_t   PYSTRO_NODE_TABLE_LEN = 0;
size_t   PYSTRO_NODE_TABLE_CAPA = 0;

// Bag of param-name lists indexed by node_def's `names_idx`.
const char **PYSTRO_NAME_TABLE = NULL;
static size_t pystro_name_len, pystro_name_capa;
static size_t name_table_reserve(const char **names, size_t n)
{
    if (pystro_name_len + n > pystro_name_capa) {
        size_t cap = pystro_name_capa ? pystro_name_capa * 2 : 32;
        while (cap < pystro_name_len + n) cap *= 2;
        PYSTRO_NAME_TABLE = (const char **)GC_realloc(PYSTRO_NAME_TABLE, cap * sizeof(char *));
        pystro_name_capa = cap;
    }
    size_t base = pystro_name_len;
    for (size_t i = 0; i < n; i++) PYSTRO_NAME_TABLE[base + i] = names[i];
    pystro_name_len += n;
    return base;
}

// Bag of (name, NODE *) kwargs indexed by node_call_kw's `kwargs_idx`.
struct pykwarg *PYSTRO_KWARGS = NULL;
static size_t pystro_kwargs_len, pystro_kwargs_capa;
static size_t kwargs_reserve(struct pykwarg *src, size_t n)
{
    if (pystro_kwargs_len + n > pystro_kwargs_capa) {
        size_t cap = pystro_kwargs_capa ? pystro_kwargs_capa * 2 : 16;
        while (cap < pystro_kwargs_len + n) cap *= 2;
        PYSTRO_KWARGS = (struct pykwarg *)GC_realloc(PYSTRO_KWARGS, cap * sizeof(struct pykwarg));
        pystro_kwargs_capa = cap;
    }
    size_t base = pystro_kwargs_len;
    for (size_t i = 0; i < n; i++) PYSTRO_KWARGS[base + i] = src[i];
    pystro_kwargs_len += n;
    return base;
}

// Bag of (slot, NODE *) defaults for node_def / node_lambda.
struct pydefault *PYSTRO_DEFAULTS = NULL;
static size_t pystro_defaults_len, pystro_defaults_capa;
static size_t defaults_reserve(struct pydefault *src, size_t n)
{
    if (pystro_defaults_len + n > pystro_defaults_capa) {
        size_t cap = pystro_defaults_capa ? pystro_defaults_capa * 2 : 8;
        while (cap < pystro_defaults_len + n) cap *= 2;
        PYSTRO_DEFAULTS = (struct pydefault *)GC_realloc(PYSTRO_DEFAULTS, cap * sizeof(struct pydefault));
        pystro_defaults_capa = cap;
    }
    size_t base = pystro_defaults_len;
    for (size_t i = 0; i < n; i++) PYSTRO_DEFAULTS[base + i] = src[i];
    pystro_defaults_len += n;
    return base;
}

static size_t
node_table_reserve(NODE **items, size_t n)
{
    if (PYSTRO_NODE_TABLE_LEN + n > PYSTRO_NODE_TABLE_CAPA) {
        size_t cap = PYSTRO_NODE_TABLE_CAPA ? PYSTRO_NODE_TABLE_CAPA * 2 : 64;
        while (cap < PYSTRO_NODE_TABLE_LEN + n) cap *= 2;
        PYSTRO_NODE_TABLE = (NODE **)GC_realloc(PYSTRO_NODE_TABLE, cap * sizeof(NODE *));
        PYSTRO_NODE_TABLE_CAPA = cap;
    }
    size_t base = PYSTRO_NODE_TABLE_LEN;
    for (size_t i = 0; i < n; i++) PYSTRO_NODE_TABLE[base + i] = items[i];
    PYSTRO_NODE_TABLE_LEN += n;
    return base;
}

// Try-handler array (struct pyhandler in context.h).
struct pyhandler *PYSTRO_HANDLERS = NULL;
static size_t pystro_handlers_len, pystro_handlers_capa;

static size_t
handlers_reserve(struct pyhandler *src, size_t n)
{
    if (pystro_handlers_len + n > pystro_handlers_capa) {
        size_t cap = pystro_handlers_capa ? pystro_handlers_capa * 2 : 8;
        while (cap < pystro_handlers_len + n) cap *= 2;
        PYSTRO_HANDLERS = (struct pyhandler *)GC_realloc(
            PYSTRO_HANDLERS, cap * sizeof(struct pyhandler));
        pystro_handlers_capa = cap;
    }
    size_t base = pystro_handlers_len;
    for (size_t i = 0; i < n; i++) PYSTRO_HANDLERS[base + i] = src[i];
    pystro_handlers_len += n;
    return base;
}

// Unpack-target array (struct pyunpack_target in context.h).
struct pyunpack_target *PYSTRO_UNPACK_TARGETS = NULL;
static size_t pystro_unpack_len, pystro_unpack_capa;

static size_t
unpack_reserve(struct pyunpack_target *src, size_t n)
{
    if (pystro_unpack_len + n > pystro_unpack_capa) {
        size_t cap = pystro_unpack_capa ? pystro_unpack_capa * 2 : 8;
        while (cap < pystro_unpack_len + n) cap *= 2;
        PYSTRO_UNPACK_TARGETS = (struct pyunpack_target *)GC_realloc(
            PYSTRO_UNPACK_TARGETS, cap * sizeof(struct pyunpack_target));
        pystro_unpack_capa = cap;
    }
    size_t base = pystro_unpack_len;
    for (size_t i = 0; i < n; i++) PYSTRO_UNPACK_TARGETS[base + i] = src[i];
    pystro_unpack_len += n;
    return base;
}

// ---------------------------------------------------------------------------
// Token helpers.
// ---------------------------------------------------------------------------

static Tok *peek_tok(int off) { return &tok_arr[tok_pos + off]; }

static bool
match_tok(int kind)
{
    if (peek_tok(0)->kind == kind) { tok_pos++; return true; }
    return false;
}

__attribute__((noreturn,format(printf,1,2)))
static void
parse_error(const char *fmt, ...)
{
    Tok *t = peek_tok(0);
    fprintf(stderr, "pystro: %s:%d: parse error: ",
            src_filename ? src_filename : "<input>", t ? t->line : 0);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void
expect(int kind, const char *what)
{
    if (!match_tok(kind)) parse_error("expected %s, got token kind %d", what, peek_tok(0)->kind);
}

// ---------------------------------------------------------------------------
// Scope / locals.
// ---------------------------------------------------------------------------

typedef struct Scope {
    struct Scope *parent;
    const char  **locals;
    int           nlocals;
    int           capa;
    const char  **globals_decls;     // names declared `global` in this scope
    int           nglobals;
    int           globals_capa;
    const char  **nonlocals_decls;   // names declared `nonlocal` in this scope
    int           nnonlocals;
    int           nonlocals_capa;
    bool          has_nested_def;    // body contains an inner def/class
                                     // (false ⇒ leaf, frame can be alloca'd)
    bool          is_generator;      // body contains a `yield` token
    int           gen_result_slot;   // local slot of the hidden __genr__ list
} Scope;

static Scope *cur_scope;

// True while parsing the body of a `class C:` suite.  When set, a
// nested `def` is added to the class via node_def's side effect and
// the parser MUST NOT additionally bind the name (which would shadow
// the method behind a never-read local / global).
static bool in_class_body;

// AST node producing the lexically-enclosing class's base — captured
// from `class C(Base):`'s base expression so `super()` inside a method
// body can resolve the parent without runtime introspection.  NULL
// outside any class body.
static NODE *cur_class_base;

static int
scope_local_index(Scope *s, const char *name)
{
    if (!s) return -1;
    for (int i = 0; i < s->nlocals; i++) if (s->locals[i] == name) return i;
    return -1;
}

static int
scope_add_local(Scope *s, const char *name)
{
    int existing = scope_local_index(s, name);
    if (existing >= 0) return existing;
    if (s->nlocals == s->capa) {
        s->capa = s->capa ? s->capa * 2 : 8;
        s->locals = (const char **)GC_realloc(s->locals, s->capa * sizeof(char *));
    }
    s->locals[s->nlocals] = name;
    return s->nlocals++;
}

static bool
scope_is_global_decl(Scope *s, const char *name)
{
    if (!s) return false;
    for (int i = 0; i < s->nglobals; i++) if (s->globals_decls[i] == name) return true;
    return false;
}

static void
scope_add_global_decl(Scope *s, const char *name)
{
    if (s->nglobals == s->globals_capa) {
        s->globals_capa = s->globals_capa ? s->globals_capa * 2 : 4;
        s->globals_decls = (const char **)GC_realloc(s->globals_decls, s->globals_capa * sizeof(char *));
    }
    s->globals_decls[s->nglobals++] = name;
}

static bool
scope_is_nonlocal_decl(Scope *s, const char *name)
{
    if (!s) return false;
    for (int i = 0; i < s->nnonlocals; i++) if (s->nonlocals_decls[i] == name) return true;
    return false;
}

static void
scope_add_nonlocal_decl(Scope *s, const char *name)
{
    if (s->nnonlocals == s->nonlocals_capa) {
        s->nonlocals_capa = s->nonlocals_capa ? s->nonlocals_capa * 2 : 4;
        s->nonlocals_decls = (const char **)GC_realloc(s->nonlocals_decls, s->nonlocals_capa * sizeof(char *));
    }
    s->nonlocals_decls[s->nnonlocals++] = name;
}

// Pre-scan a token range for `NAME =` (assignment LHS), `for NAME in`,
// `def NAME(...):`, `class NAME(...):` to seed local names.  Skips over
// nested function / class bodies.
static size_t
find_suite_end(size_t suite_start)
{
    size_t i = suite_start;
    if (tok_arr[i].kind != T_NEWLINE) {
        // Inline single-line suite.
        while (tok_arr[i].kind != T_NEWLINE && tok_arr[i].kind != T_EOF) i++;
        return i + 1;
    }
    i++;
    if (tok_arr[i].kind != T_INDENT) return i;
    int depth = 1;
    i++;
    while (depth > 0 && tok_arr[i].kind != T_EOF) {
        if (tok_arr[i].kind == T_INDENT) depth++;
        else if (tok_arr[i].kind == T_DEDENT) depth--;
        i++;
    }
    return i;
}

static void
collect_locals_in_range(Scope *s, size_t start_pos, size_t end_pos)
{
    // Track INDENT/DEDENT depth so we can skip over nested def/class bodies.
    int depth = 0;
    int skip_until_dedent = -1;        // depth at which we entered nested scope
    bool in_global_decl = false;
    for (size_t i = start_pos; i < end_pos; i++) {
        Tok *t = &tok_arr[i];
        if (t->kind == T_INDENT) { depth++; continue; }
        if (t->kind == T_DEDENT) {
            if (skip_until_dedent >= 0 && depth == skip_until_dedent) skip_until_dedent = -1;
            depth--; continue;
        }
        if (skip_until_dedent >= 0) continue;

        // Nested def/class: skip its body.  The body starts at the next NEWLINE+INDENT.
        if ((t->kind == T_DEF || t->kind == T_CLASS) && i + 1 < end_pos
                && tok_arr[i+1].kind == T_NAME) {
            // Register the def/class name as a local.
            scope_add_local(s, tok_arr[i+1].sval);
            s->has_nested_def = true;
            // Find this def's suite (skip until we cross a colon and then NEWLINE+INDENT).
            size_t j = i + 2;
            int paren = 0;
            while (j < end_pos) {
                if (tok_arr[j].kind == T_LPAREN) paren++;
                else if (tok_arr[j].kind == T_RPAREN) paren--;
                else if (tok_arr[j].kind == T_COLON && paren == 0) { j++; break; }
                j++;
            }
            // Now skip its suite.
            if (j < end_pos && tok_arr[j].kind == T_NEWLINE) {
                j++;
                if (j < end_pos && tok_arr[j].kind == T_INDENT) {
                    skip_until_dedent = depth + 1;
                    depth++;
                    j++;
                }
            }
            i = j - 1;     // -1 because the loop increments
            continue;
        }
        if (t->kind == T_YIELD) { s->is_generator = true; continue; }
        if (t->kind == T_GLOBAL) { in_global_decl = true; continue; }
        if (t->kind == T_NONLOCAL) {
            // collect names until newline/comma stops, like global.
            size_t j = i + 1;
            while (j < end_pos && (tok_arr[j].kind == T_NAME || tok_arr[j].kind == T_COMMA)) {
                if (tok_arr[j].kind == T_NAME) scope_add_nonlocal_decl(s, tok_arr[j].sval);
                j++;
            }
            i = j - 1;
            continue;
        }
        if (t->kind == T_NEWLINE) { in_global_decl = false; continue; }
        if (in_global_decl && t->kind == T_NAME) {
            scope_add_global_decl(s, t->sval);
            continue;
        }
        if (t->kind == T_FOR && i + 1 < end_pos && tok_arr[i+1].kind == T_NAME) {
            // for NAME in ...
            if (!scope_is_global_decl(s, tok_arr[i+1].sval) &&
                !scope_is_nonlocal_decl(s, tok_arr[i+1].sval))
                scope_add_local(s, tok_arr[i+1].sval);
            // also handle `for a, b in ...`
            size_t j = i + 2;
            while (j < end_pos && tok_arr[j].kind == T_COMMA) {
                j++;
                if (j < end_pos && tok_arr[j].kind == T_NAME) {
                    if (!scope_is_global_decl(s, tok_arr[j].sval) &&
                        !scope_is_nonlocal_decl(s, tok_arr[j].sval))
                        scope_add_local(s, tok_arr[j].sval);
                    j++;
                }
            }
            continue;
        }
        if (t->kind == T_EXCEPT) {
            // except E as NAME:
            size_t j = i + 1;
            while (j < end_pos && tok_arr[j].kind != T_AS && tok_arr[j].kind != T_COLON
                    && tok_arr[j].kind != T_NEWLINE) j++;
            if (j < end_pos && tok_arr[j].kind == T_AS && j + 1 < end_pos
                    && tok_arr[j+1].kind == T_NAME) {
                if (!scope_is_global_decl(s, tok_arr[j+1].sval))
                    scope_add_local(s, tok_arr[j+1].sval);
            }
            continue;
        }
        // NAME = | NAME += ... → assignment, treat as local.
        if (t->kind == T_NAME && i + 1 < end_pos) {
            int next = tok_arr[i+1].kind;
            if (next == T_ASSIGN || next == T_PLUS_EQ || next == T_MINUS_EQ ||
                next == T_STAR_EQ || next == T_SLASH_EQ || next == T_SLASH_SLASH_EQ ||
                next == T_PERCENT_EQ || next == T_AMP_EQ || next == T_PIPE_EQ ||
                next == T_CARET_EQ || next == T_LSHIFT_EQ || next == T_RSHIFT_EQ ||
                next == T_STAR_STAR_EQ) {
                if (!scope_is_global_decl(s, t->sval) &&
                    !scope_is_nonlocal_decl(s, t->sval))
                    scope_add_local(s, t->sval);
            }
            // Multi-target tuple unpack: `a, b = ...`
            if (next == T_COMMA) {
                size_t j = i;
                bool all_names = true;
                while (j < end_pos && tok_arr[j].kind == T_NAME &&
                       j + 1 < end_pos && tok_arr[j+1].kind == T_COMMA) {
                    j += 2;
                    if (j < end_pos && tok_arr[j].kind != T_NAME) { all_names = false; break; }
                }
                if (all_names && j < end_pos && tok_arr[j].kind == T_NAME &&
                    j + 1 < end_pos && tok_arr[j+1].kind == T_ASSIGN) {
                    size_t k = i;
                    while (k <= j) {
                        if (tok_arr[k].kind == T_NAME &&
                            !scope_is_global_decl(s, tok_arr[k].sval) &&
                            !scope_is_nonlocal_decl(s, tok_arr[k].sval))
                            scope_add_local(s, tok_arr[k].sval);
                        k++;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AST helpers / forward declarations.
// ---------------------------------------------------------------------------

static NODE *parse_expr(void);
static NODE *parse_stmt(void);
static NODE *parse_suite(void);
static NODE *parse_simple_stmt(void);

// Build a right-leaning chain of seq nodes from a list of statements.
static NODE *
seq_of(NODE **stmts, int n)
{
    if (n == 0) return ALLOC_node_nop();
    if (n == 1) return stmts[0];
    NODE *r = stmts[n - 1];
    for (int i = n - 2; i >= 0; i--) r = ALLOC_node_seq(stmts[i], r);
    return r;
}

// Resolve a NAME as a load expression.  Walk the scope chain so a
// nested `def` can read locals from its lexical parents (closure
// capture).  Returns:
//   - lref(idx)              : found in current scope
//   - lref_up(depth, idx)    : found in an ancestor scope
//   - gref(name)             : not found in any function scope, or
//                              `global` declared in current scope
static NODE *
make_load(const char *name)
{
    if (cur_scope && !scope_is_global_decl(cur_scope, name)) {
        int idx = scope_local_index(cur_scope, name);
        if (idx >= 0) return ALLOC_node_lref((uint32_t)idx);
        // Walk parent scopes.
        Scope *s = cur_scope->parent;
        uint32_t depth = 1;
        while (s) {
            if (scope_is_global_decl(s, name)) break;
            int up_idx = scope_local_index(s, name);
            if (up_idx >= 0) return ALLOC_node_lref_up(depth, (uint32_t)up_idx);
            s = s->parent;
            depth++;
        }
    }
    return ALLOC_node_gref(name);
}

// Store NODE for a NAME.  By default, an assignment in a function body
// creates / updates a local in the current scope (Python's standard
// "implicit local" rule).  `nonlocal NAME` would route writes to an
// outer scope's slot — that requires a separate lookup pass; for v0
// only `global NAME` is honoured here, sending writes to globals.
static NODE *
make_store(const char *name, NODE *rhs)
{
    if (cur_scope && !scope_is_global_decl(cur_scope, name)) {
        // `nonlocal` decl: write to an outer scope's existing slot.
        if (scope_is_nonlocal_decl(cur_scope, name)) {
            Scope *s = cur_scope->parent;
            uint32_t depth = 1;
            while (s) {
                int up_idx = scope_local_index(s, name);
                if (up_idx >= 0)
                    return ALLOC_node_lset_up(depth, (uint32_t)up_idx, rhs);
                s = s->parent;
                depth++;
            }
            parse_error("nonlocal '%s' has no binding in any enclosing scope", name);
        }
        int idx = scope_add_local(cur_scope, name);
        return ALLOC_node_lset((uint32_t)idx, rhs);
    }
    return ALLOC_node_gset(name, rhs);
}

// ---------------------------------------------------------------------------
// f-string parsing.  An f-string's payload is the body text between the
// quotes (escapes already processed by the lexer).  We split on `{...}`
// boundaries: literal pieces become `node_const_str`, expression pieces
// are sub-tokenized + parsed and then wrapped in `str(...)`, and the
// whole thing is concatenated with `+`.
// ---------------------------------------------------------------------------

static NODE *parse_fstring_payload(const char *s, size_t len);

static NODE *
parse_fstring(Tok *t)
{
    return parse_fstring_payload(t->sval, t->slen);
}

static NODE *
str_call_of(NODE *expr)
{
    return ALLOC_node_call_1(ALLOC_node_gref("str"), expr);
}

static NODE *
parse_fstring_payload(const char *s, size_t len)
{
    NODE *acc = NULL;
    size_t i = 0;
    char *lit_buf = (char *)GC_malloc_atomic(len + 1);
    size_t lit_len = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == '{') {
            if (i + 1 < len && s[i+1] == '{') { lit_buf[lit_len++] = '{'; i += 2; continue; }
            // Flush literal.
            if (lit_len > 0) {
                lit_buf[lit_len] = '\0';
                NODE *p = ALLOC_node_const_str(intern_name(lit_buf, lit_len));
                acc = acc ? ALLOC_node_add(acc, p) : p;
                lit_len = 0;
            }
            // Find matching '}', allowing nested braces minimally.  A
            // `:` at depth 1 starts the format spec.
            size_t j = i + 1;
            size_t spec_start = 0;
            int depth = 1;
            int paren = 0;
            while (j < len && depth > 0) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') depth--;
                else if (s[j] == '(' || s[j] == '[') paren++;
                else if (s[j] == ')' || s[j] == ']') paren--;
                else if (s[j] == ':' && depth == 1 && paren == 0 && spec_start == 0) {
                    spec_start = j;
                }
                if (depth > 0) j++;
            }
            if (j >= len) parse_error("unterminated f-string expression");

            size_t expr_end = spec_start ? spec_start : j;

            // Sub-parse expression.
            const char *saved_buf = src_buf;
            size_t saved_pos = src_pos;
            int    saved_line = src_line;
            int    saved_indent_top = indent_top;
            int    saved_paren = paren_depth;
            bool   saved_at_line = at_line_start;
            Tok   *saved_tok_arr = tok_arr;
            size_t saved_tok_len = tok_len, saved_tok_capa = tok_capa, saved_tok_pos = tok_pos;

            char *expr_src = (char *)GC_malloc_atomic(expr_end - i);
            memcpy(expr_src, s + i + 1, expr_end - i - 1);
            expr_src[expr_end - i - 1] = '\0';
            tokenize(expr_src, src_filename);
            NODE *expr = parse_expr();

            src_buf = saved_buf; src_pos = saved_pos; src_line = saved_line;
            indent_top = saved_indent_top; paren_depth = saved_paren;
            at_line_start = saved_at_line;
            tok_arr = saved_tok_arr; tok_len = saved_tok_len; tok_capa = saved_tok_capa;
            tok_pos = saved_tok_pos;

            NODE *p;
            if (spec_start) {
                // Build format(expr, "spec") call.
                char *spec_buf = (char *)GC_malloc_atomic(j - spec_start);
                memcpy(spec_buf, s + spec_start + 1, j - spec_start - 1);
                spec_buf[j - spec_start - 1] = '\0';
                NODE *spec_node = ALLOC_node_const_str(intern_name(spec_buf, j - spec_start - 1));
                p = ALLOC_node_call_2(ALLOC_node_gref(intern_name("format", 6)),
                                      expr, spec_node);
            } else {
                p = str_call_of(expr);
            }
            acc = acc ? ALLOC_node_add(acc, p) : p;
            i = j + 1;
        } else if (ch == '}') {
            if (i + 1 < len && s[i+1] == '}') { lit_buf[lit_len++] = '}'; i += 2; continue; }
            parse_error("single '}' in f-string");
        } else {
            lit_buf[lit_len++] = ch;
            i++;
        }
    }
    if (lit_len > 0) {
        lit_buf[lit_len] = '\0';
        NODE *p = ALLOC_node_const_str(intern_name(lit_buf, lit_len));
        acc = acc ? ALLOC_node_add(acc, p) : p;
    }
    if (!acc) acc = ALLOC_node_const_str("");
    return acc;
}

// ---------------------------------------------------------------------------
// Atoms / postfix.
// ---------------------------------------------------------------------------

static NODE *parse_or(void);
static NODE *parse_cond(void);

// `( expr_list )` — single → expr; multi (or trailing comma) → tuple.
static NODE *
parse_paren_or_tuple(void)
{
    expect(T_LPAREN, "'('");
    if (match_tok(T_RPAREN)) {
        // Empty tuple ().
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_tuple((uint32_t)base, 0);
    }
    NODE *first = parse_expr();
    if (match_tok(T_RPAREN)) return first;
    // tuple
    NODE *items[64];
    int n = 0; items[n++] = first;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_RPAREN) break;
        if (n >= 64) parse_error("tuple too long");
        items[n++] = parse_expr();
    }
    expect(T_RPAREN, "')'");
    size_t base = node_table_reserve(items, n);
    return ALLOC_node_make_tuple((uint32_t)base, (uint32_t)n);
}

// ---------------------------------------------------------------------------
// Comprehensions.  Desugared at parse time into a sequence:
//
//   [expr for x in xs if cond]   →
//     seq(__t = [],
//         seq(for x in xs: if cond: __t.append(expr),
//             __t))
//
//   {k:v for x in xs}            →
//     seq(__t = {},
//         seq(for x in xs: __t[k] = v,
//             __t))
//
// Hidden temp `__t` is an auto-allocated local (or global at top level).
// Multiple `for` / `if` clauses are nested.  Comprehensions don't
// introduce a new scope (Python 3 does, pystro v0 leaks the loop var
// to the enclosing scope — minor difference noted in todo.md).
// ---------------------------------------------------------------------------

static int comp_counter = 0;

static const char *
new_temp_name(const char *prefix)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%d__", prefix, comp_counter++);
    return intern_name(buf, strlen(buf));
}

// Build (store_node, load_node) pair for `tmp = initial_value`.
static NODE *
build_temp_init(const char *tmp_name, NODE *initial, NODE **out_load)
{
    NODE *store, *load;
    if (cur_scope && !scope_is_global_decl(cur_scope, tmp_name)) {
        int idx = scope_add_local(cur_scope, tmp_name);
        store = ALLOC_node_lset((uint32_t)idx, initial);
        load = ALLOC_node_lref((uint32_t)idx);
    } else {
        store = ALLOC_node_gset(tmp_name, initial);
        load = ALLOC_node_gref(tmp_name);
    }
    *out_load = load;
    return store;
}

// Wrap `body` in a `for x in iter:` (target may be a NAME or `a, b`
// tuple — only NAME for now).
static NODE *
build_for_loop(const char *target_name, NODE *iter, NODE *body)
{
    if (cur_scope && !scope_is_global_decl(cur_scope, target_name)) {
        int idx = scope_add_local(cur_scope, target_name);
        return ALLOC_node_for_local((uint32_t)idx, iter, body);
    }
    return ALLOC_node_for_global(target_name, iter, body);
}

// Parse one or more `for X (, Y)* in EXPR (if EXPR)*` clauses and
// build the nested loop.  `inner_body` is what the innermost loop body
// produces.  Returns the outermost for_loop node.  Caller positions
// tok at `for`.  Tuple targets desugar to `__t = item; X = __t[0]; Y =
// __t[1]; ...`.
static NODE *
parse_comp_clauses(NODE *inner_body)
{
    expect(T_FOR, "'for'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected target NAME in comprehension");
    const char *names[16];
    int nnames = 0;
    names[nnames++] = peek_tok(0)->sval;
    tok_pos++;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME in tuple target");
        if (nnames >= 16) parse_error("for tuple target too long");
        names[nnames++] = peek_tok(0)->sval;
        tok_pos++;
    }
    expect(T_IN, "'in'");
    NODE *iter = parse_or();

    // Build the body around the (cond, append/setitem) we received.
    while (peek_tok(0)->kind == T_IF) {
        tok_pos++;
        NODE *cond = parse_or();
        inner_body = ALLOC_node_if(cond, inner_body, ALLOC_node_nop());
    }

    // Inner `for` clause?
    if (peek_tok(0)->kind == T_FOR)
        inner_body = parse_comp_clauses(inner_body);

    if (nnames == 1) return build_for_loop(names[0], iter, inner_body);

    // Tuple target: introduce a hidden temp to hold the current item,
    // then prepend `name_i = __t[i]` for each name.
    const char *tmp = new_temp_name("__forT");
    NODE *load_tmp;
    NODE *prefix = NULL;
    if (cur_scope && !scope_is_global_decl(cur_scope, tmp)) {
        int idx = scope_add_local(cur_scope, tmp);
        load_tmp = ALLOC_node_lref((uint32_t)idx);
        // body: assign each name from __t[i], then run inner_body.
        for (int i = nnames - 1; i >= 0; i--) {
            NODE *idx_n = ALLOC_node_const_int(i);
            NODE *el  = ALLOC_node_subscript_get(load_tmp, idx_n);
            int slot = scope_add_local(cur_scope, names[i]);
            NODE *as = ALLOC_node_lset((uint32_t)slot, el);
            prefix = prefix ? ALLOC_node_seq(as, prefix) : as;
        }
        NODE *new_body = ALLOC_node_seq(prefix, inner_body);
        return ALLOC_node_for_local((uint32_t)idx, iter, new_body);
    }
    // top-level
    load_tmp = ALLOC_node_gref(tmp);
    for (int i = nnames - 1; i >= 0; i--) {
        NODE *idx_n = ALLOC_node_const_int(i);
        NODE *el  = ALLOC_node_subscript_get(load_tmp, idx_n);
        NODE *as = ALLOC_node_gset(names[i], el);
        prefix = prefix ? ALLOC_node_seq(as, prefix) : as;
    }
    NODE *new_body = ALLOC_node_seq(prefix, inner_body);
    return ALLOC_node_for_global(tmp, iter, new_body);
}

static NODE *
parse_list_literal(void)
{
    expect(T_LBRACK, "'['");
    if (match_tok(T_RBRACK)) {
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_list((uint32_t)base, 0);
    }
    NODE *first = parse_expr();
    if (peek_tok(0)->kind == T_FOR) {
        // List comprehension: [expr for x in xs (if cond)*]+
        const char *tmp = new_temp_name("__lc");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_list((uint32_t)empty_idx, 0), &load_tmp);
        NODE *append = ALLOC_node_method_1(load_tmp, intern_name("append", 6), first);
        NODE *loops = parse_comp_clauses(append);
        expect(T_RBRACK, "']'");
        return ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
    }
    NODE *items[256];
    int n = 0; items[n++] = first;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_RBRACK) break;
        if (n >= 256) parse_error("list literal too long");
        items[n++] = parse_expr();
    }
    expect(T_RBRACK, "']'");
    size_t base = node_table_reserve(items, n);
    return ALLOC_node_make_list((uint32_t)base, (uint32_t)n);
}

static NODE *
parse_dict_literal(void)
{
    expect(T_LBRACE, "'{'");
    if (match_tok(T_RBRACE)) {
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_dict((uint32_t)base, 0);
    }
    NODE *first_k = parse_expr();
    expect(T_COLON, "':'");
    NODE *first_v = parse_expr();
    if (peek_tok(0)->kind == T_FOR) {
        // Dict comprehension: {k:v for x in xs (if cond)*}+
        const char *tmp = new_temp_name("__dc");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_dict((uint32_t)empty_idx, 0), &load_tmp);
        NODE *set = ALLOC_node_subscript_set(load_tmp, first_k, first_v);
        NODE *loops = parse_comp_clauses(set);
        expect(T_RBRACE, "'}'");
        return ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
    }
    NODE *items[512];
    int npairs = 0;
    items[0] = first_k; items[1] = first_v; npairs = 1;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_RBRACE) break;
        NODE *k = parse_expr();
        expect(T_COLON, "':'");
        NODE *v = parse_expr();
        if (npairs * 2 + 2 > 512) parse_error("dict literal too long");
        items[npairs * 2] = k;
        items[npairs * 2 + 1] = v;
        npairs++;
    }
    expect(T_RBRACE, "'}'");
    size_t base = node_table_reserve(items, npairs * 2);
    return ALLOC_node_make_dict((uint32_t)base, (uint32_t)npairs);
}

static NODE *
parse_lambda(void)
{
    expect(T_LAMBDA, "'lambda'");

    Scope sc = {0};
    sc.parent = cur_scope;

    int nparams = 0;
    struct pydefault defs[16];
    int ndefaults = 0;
    bool seen_default = false;
    const char *names[16];
    int nnames = 0;

    if (peek_tok(0)->kind != T_COLON) {
        for (;;) {
            if (peek_tok(0)->kind != T_NAME) parse_error("expected parameter");
            const char *pn = peek_tok(0)->sval;
            tok_pos++;
            scope_add_local(&sc, pn);
            names[nnames++] = pn;
            int slot = nparams;
            nparams++;
            if (match_tok(T_ASSIGN)) {
                seen_default = true;
                if (ndefaults >= 16) parse_error("too many defaults");
                Scope *saved = cur_scope; cur_scope = NULL;
                defs[ndefaults].slot = slot;
                defs[ndefaults].expr = parse_expr();
                ndefaults++;
                cur_scope = saved;
            } else if (seen_default) {
                parse_error("non-default after default");
            }
            if (!match_tok(T_COMMA)) break;
        }
    }
    expect(T_COLON, "':'");

    Scope *saved = cur_scope;
    cur_scope = &sc;
    NODE *body_expr = parse_expr();
    cur_scope = saved;

    NODE *body = ALLOC_node_return(body_expr);
    size_t didx = defaults_reserve(defs, ndefaults);
    size_t nidx = name_table_reserve(names, nnames);
    return ALLOC_node_lambda((uint32_t)nparams, (uint32_t)sc.nlocals,
                             (uint32_t)ndefaults, (uint32_t)didx,
                             (uint32_t)1, (uint32_t)nidx, body);
}

static NODE *
parse_atom(void)
{
    Tok *t = peek_tok(0);
    switch (t->kind) {
      case T_INT: {
        tok_pos++;
        if (t->ival_overflow) return ALLOC_node_const_bignum(t->sval);
        if (t->ival >= INT32_MIN && t->ival <= INT32_MAX)
            return ALLOC_node_const_int((int32_t)t->ival);
        return ALLOC_node_const_int64(PY_FIX(t->ival));
      }
      case T_FLOAT: {
        tok_pos++;
        union { uint64_t u; double d; } pun = { .d = t->fval };
        return ALLOC_node_const_float(pun.u);
      }
      case T_STR: {
        tok_pos++;
        return ALLOC_node_const_str(t->sval);
      }
      case T_FSTR: {
        tok_pos++;
        return parse_fstring(t);
      }
      case T_TRUE:  tok_pos++; return ALLOC_node_const_true();
      case T_FALSE: tok_pos++; return ALLOC_node_const_false();
      case T_NONE:  tok_pos++; return ALLOC_node_const_none();
      case T_LPAREN: return parse_paren_or_tuple();
      case T_LBRACK: return parse_list_literal();
      case T_LBRACE: return parse_dict_literal();
      case T_LAMBDA: return parse_lambda();
      case T_NAME: { tok_pos++; return make_load(t->sval); }
      default:
        parse_error("unexpected token in expression (kind=%d)", t->kind);
    }
}

static NODE *
parse_call_args(NODE *fn)
{
    expect(T_LPAREN, "'('");
    NODE *args[64];
    int argc = 0;
    struct pykwarg kws[32];
    int kwc = 0;
    if (peek_tok(0)->kind != T_RPAREN) {
        for (;;) {
            // Lookahead for `NAME =` (kwarg).  Plain `=` only — `==` is
            // a different token.
            if (peek_tok(0)->kind == T_NAME && peek_tok(1)->kind == T_ASSIGN) {
                if (kwc >= 32) parse_error("too many kwargs");
                kws[kwc].name = peek_tok(0)->sval;
                tok_pos += 2;
                kws[kwc].value = parse_expr();
                kwc++;
            } else {
                if (argc >= 64) parse_error("too many args");
                args[argc++] = parse_expr();
            }
            if (!match_tok(T_COMMA)) break;
            if (peek_tok(0)->kind == T_RPAREN) break;
        }
    }
    expect(T_RPAREN, "')'");
    if (kwc > 0) {
        size_t abase = node_table_reserve(args, argc);
        size_t kbase = kwargs_reserve(kws, kwc);
        return ALLOC_node_call_kw(fn, (uint32_t)abase, (uint32_t)argc,
                                  (uint32_t)kbase, (uint32_t)kwc);
    }
    switch (argc) {
      case 0: return ALLOC_node_call_0(fn);
      case 1: return ALLOC_node_call_1(fn, args[0]);
      case 2: return ALLOC_node_call_2(fn, args[0], args[1]);
      case 3: return ALLOC_node_call_3(fn, args[0], args[1], args[2]);
      default: {
        size_t base = node_table_reserve(args, argc);
        return ALLOC_node_call_n(fn, (uint32_t)base, (uint32_t)argc);
      }
    }
}

// `[ subscript ]` — either index or slice.  Returns either a
// node_subscript_get or node_slice.
static NODE *
parse_subscript(NODE *seq)
{
    expect(T_LBRACK, "'['");
    NODE *start = NULL, *stop = NULL, *step = NULL;
    bool is_slice = false;

    if (peek_tok(0)->kind == T_COLON) is_slice = true;
    else                              start = parse_expr();
    if (match_tok(T_COLON)) {
        is_slice = true;
        if (peek_tok(0)->kind != T_COLON && peek_tok(0)->kind != T_RBRACK)
            stop = parse_expr();
        if (match_tok(T_COLON)) {
            if (peek_tok(0)->kind != T_RBRACK) step = parse_expr();
        }
    }
    expect(T_RBRACK, "']'");
    if (!is_slice) return ALLOC_node_subscript_get(seq, start);
    if (!start) start = ALLOC_node_const_none();
    if (!stop)  stop  = ALLOC_node_const_none();
    if (!step)  step  = ALLOC_node_const_none();
    return ALLOC_node_slice(seq, start, stop, step);
}

// `.attr` / `.method(...)`.
static NODE *
parse_dot_trailer(NODE *obj)
{
    expect(T_DOT, "'.'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected attribute name");
    const char *name = peek_tok(0)->sval;
    tok_pos++;
    if (peek_tok(0)->kind == T_LPAREN) {
        // method call
        tok_pos++;
        NODE *args[64];
        int argc = 0;
        if (peek_tok(0)->kind != T_RPAREN) {
            for (;;) {
                if (argc >= 64) parse_error("too many args");
                args[argc++] = parse_expr();
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == T_RPAREN) break;
            }
        }
        expect(T_RPAREN, "')'");
        switch (argc) {
          case 0: return ALLOC_node_method_0(obj, name);
          case 1: return ALLOC_node_method_1(obj, name, args[0]);
          case 2: return ALLOC_node_method_2(obj, name, args[0], args[1]);
          default: {
            size_t base = node_table_reserve(args, argc);
            return ALLOC_node_method_n(obj, name, (uint32_t)base, (uint32_t)argc);
          }
        }
    }
    return ALLOC_node_attr_get(obj, name);
}

static NODE *
parse_postfix(void)
{
    // `super()` short-circuit: only accepted as `super().METHOD(args)`.
    // Captures the lexically-enclosing class's base via cur_class_base.
    if (peek_tok(0)->kind == T_NAME && peek_tok(0)->sval == intern_name("super", 5)
            && peek_tok(1)->kind == T_LPAREN && peek_tok(2)->kind == T_RPAREN
            && peek_tok(3)->kind == T_DOT && peek_tok(4)->kind == T_NAME
            && peek_tok(5)->kind == T_LPAREN) {
        if (!cur_class_base) parse_error("super() called outside a class body");
        tok_pos += 3;       // past `super` `(` `)`
        expect(T_DOT, "'.'");
        const char *method = peek_tok(0)->sval;
        tok_pos++;
        expect(T_LPAREN, "'('");
        NODE *args[64];
        int argc = 0;
        if (peek_tok(0)->kind != T_RPAREN) {
            for (;;) {
                if (argc >= 64) parse_error("too many super args");
                args[argc++] = parse_expr();
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == T_RPAREN) break;
            }
        }
        expect(T_RPAREN, "')'");
        size_t base = node_table_reserve(args, argc);
        NODE *e = ALLOC_node_super_method(cur_class_base, method, (uint32_t)base, (uint32_t)argc);
        // continue with possible further trailers
        for (;;) {
            int k = peek_tok(0)->kind;
            if (k == T_LPAREN) e = parse_call_args(e);
            else if (k == T_LBRACK) e = parse_subscript(e);
            else if (k == T_DOT)    e = parse_dot_trailer(e);
            else break;
        }
        return e;
    }
    NODE *e = parse_atom();
    for (;;) {
        int k = peek_tok(0)->kind;
        if (k == T_LPAREN) e = parse_call_args(e);
        else if (k == T_LBRACK) e = parse_subscript(e);
        else if (k == T_DOT)    e = parse_dot_trailer(e);
        else break;
    }
    return e;
}

static NODE *parse_factor(void);

static NODE *
parse_power(void)
{
    NODE *l = parse_postfix();
    if (match_tok(T_STAR_STAR)) {
        NODE *r = parse_factor();   // right-associative, allows -x as exponent
        l = ALLOC_node_pow(l, r);
    }
    return l;
}

static NODE *
parse_factor(void)
{
    if (match_tok(T_MINUS)) return ALLOC_node_neg(parse_factor());
    if (match_tok(T_PLUS))  return parse_factor();
    if (match_tok(T_TILDE)) return ALLOC_node_bit_inv(parse_factor());
    return parse_power();
}

static NODE *
parse_term(void)
{
    NODE *l = parse_factor();
    for (;;) {
        if      (match_tok(T_STAR))         l = ALLOC_node_mul(l, parse_factor());
        else if (match_tok(T_SLASH))        l = ALLOC_node_truediv(l, parse_factor());
        else if (match_tok(T_SLASH_SLASH))  l = ALLOC_node_floordiv(l, parse_factor());
        else if (match_tok(T_PERCENT))      l = ALLOC_node_mod(l, parse_factor());
        else break;
    }
    return l;
}

static NODE *
parse_arith(void)
{
    NODE *l = parse_term();
    for (;;) {
        if      (match_tok(T_PLUS))  l = ALLOC_node_add(l, parse_term());
        else if (match_tok(T_MINUS)) l = ALLOC_node_sub(l, parse_term());
        else break;
    }
    return l;
}

static NODE *
parse_shift(void)
{
    NODE *l = parse_arith();
    for (;;) {
        if      (match_tok(T_LSHIFT)) l = ALLOC_node_lshift(l, parse_arith());
        else if (match_tok(T_RSHIFT)) l = ALLOC_node_rshift(l, parse_arith());
        else break;
    }
    return l;
}

static NODE *parse_bitand(void) { NODE *l = parse_shift();  while (match_tok(T_AMP))   l = ALLOC_node_bit_and(l, parse_shift());  return l; }
static NODE *parse_bitxor(void) { NODE *l = parse_bitand(); while (match_tok(T_CARET)) l = ALLOC_node_bit_xor(l, parse_bitand()); return l; }
static NODE *parse_bitor(void)  { NODE *l = parse_bitxor(); while (match_tok(T_PIPE))  l = ALLOC_node_bit_or (l, parse_bitxor()); return l; }

// Comparison with chaining: `a < b < c` → `(a < b) and (b < c)`.
// `b` is evaluated TWICE in this v0 — Python semantics specifies a
// single evaluation, so this is a known caveat for code with
// side-effecting middle operands.
static NODE *
parse_compare(void)
{
    NODE *l = parse_bitor();
    NODE *result = NULL;
    for (;;) {
        int k = peek_tok(0)->kind;
        NODE *(*op)(NODE *, NODE *) = NULL;
        if      (k == T_LT)  { tok_pos++; op = ALLOC_node_lt; }
        else if (k == T_LE)  { tok_pos++; op = ALLOC_node_le; }
        else if (k == T_GT)  { tok_pos++; op = ALLOC_node_gt; }
        else if (k == T_GE)  { tok_pos++; op = ALLOC_node_ge; }
        else if (k == T_EQ)  { tok_pos++; op = ALLOC_node_eq; }
        else if (k == T_NE)  { tok_pos++; op = ALLOC_node_ne; }
        else if (k == T_IS) {
            tok_pos++;
            if (match_tok(T_NOT)) op = ALLOC_node_is_not;
            else                  op = ALLOC_node_is;
        }
        else if (k == T_NOT && peek_tok(1)->kind == T_IN) {
            tok_pos += 2; op = ALLOC_node_not_in;
        }
        else if (k == T_IN) { tok_pos++; op = ALLOC_node_in; }
        else break;

        NODE *r = parse_bitor();
        NODE *cmp = op(l, r);
        result = result ? ALLOC_node_and(result, cmp) : cmp;
        l = r;
    }
    return result ? result : l;
}

static NODE *
parse_not_expr(void)
{
    if (match_tok(T_NOT)) return ALLOC_node_not(parse_not_expr());
    return parse_compare();
}

static NODE *
parse_and(void)
{
    NODE *l = parse_not_expr();
    while (match_tok(T_AND)) l = ALLOC_node_and(l, parse_not_expr());
    return l;
}

static NODE *
parse_or(void)
{
    NODE *l = parse_and();
    while (match_tok(T_OR)) l = ALLOC_node_or(l, parse_and());
    return l;
}

// Conditional expression: `a if c else b`.  Right-assoc on the false side.
static NODE *
parse_cond(void)
{
    NODE *t = parse_or();
    if (match_tok(T_IF)) {
        NODE *c = parse_or();
        expect(T_ELSE, "'else'");
        NODE *e = parse_cond();
        return ALLOC_node_if(c, t, e);
    }
    return t;
}

static NODE *
parse_expr(void) { return parse_cond(); }

// expr_list: expr (',' expr)+ → tuple; single → expr.
static NODE *
parse_expr_list(void)
{
    NODE *first = parse_expr();
    if (peek_tok(0)->kind != T_COMMA) return first;
    NODE *items[64];
    int n = 0; items[n++] = first;
    while (match_tok(T_COMMA)) {
        int k = peek_tok(0)->kind;
        if (k == T_NEWLINE || k == T_RPAREN || k == T_RBRACK || k == T_RBRACE
                || k == T_COLON || k == T_ASSIGN || k == T_SEMI) break;
        if (n >= 64) parse_error("expr list too long");
        items[n++] = parse_expr();
    }
    if (n == 1) return items[0];
    size_t base = node_table_reserve(items, n);
    return ALLOC_node_make_tuple((uint32_t)base, (uint32_t)n);
}

// ---------------------------------------------------------------------------
// Statement parsing.
// ---------------------------------------------------------------------------

// Build a store of `rhs` into target `lhs` (an expr that is a valid lvalue).
// Supported: NAME, obj.attr, seq[idx].  Plain expressions for parser-time
// re-routing of `lhs op= rhs` work the same since we duplicate the LHS expr
// (which is just NODE *  reuse — fine since AST nodes are read-only after
// allocation).
static NODE *
build_store(NODE *lhs_expr, NODE *rhs, Tok *first_tok)
{
    (void)first_tok;
    // We can't easily inspect what `lhs_expr` is structurally without a
    // tag.  Instead the simple-stmt parser routes here only after
    // detecting LHS shape from tokens.  This function is unused in v1.
    (void)lhs_expr; (void)rhs;
    parse_error("internal: build_store fallthrough");
}

static NODE *
parse_suite(void)
{
    expect(T_COLON, "':'");
    if (match_tok(T_NEWLINE)) {
        expect(T_INDENT, "indent");
        NODE *stmts[1024];
        int n = 0;
        while (peek_tok(0)->kind != T_DEDENT && peek_tok(0)->kind != T_EOF) {
            if (n >= 1024) parse_error("suite too long");
            stmts[n++] = parse_stmt();
        }
        expect(T_DEDENT, "dedent");
        return seq_of(stmts, n);
    }
    NODE *s = parse_simple_stmt();
    expect(T_NEWLINE, "newline");
    return s;
}

static NODE *
parse_if_tail(void)
{
    if (match_tok(T_ELIF)) {
        NODE *c2 = parse_expr();
        NODE *t2 = parse_suite();
        NODE *e2 = parse_if_tail();
        return ALLOC_node_if(c2, t2, e2);
    }
    if (match_tok(T_ELSE)) return parse_suite();
    return ALLOC_node_nop();
}

static NODE *
parse_if(void)
{
    expect(T_IF, "'if'");
    NODE *c = parse_expr();
    NODE *t = parse_suite();
    NODE *e = parse_if_tail();
    return ALLOC_node_if(c, t, e);
}

static NODE *
parse_while(void)
{
    expect(T_WHILE, "'while'");
    NODE *c = parse_expr();
    NODE *b = parse_suite();
    return ALLOC_node_while(c, b);
}

static NODE *
parse_for(void)
{
    expect(T_FOR, "'for'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected target name in 'for'");
    // For now: support only single-name target.  For tuple targets,
    // we'd need to desugar via a hidden temp.
    const char *target = peek_tok(0)->sval;
    tok_pos++;
    if (peek_tok(0)->kind == T_COMMA) {
        // Tuple target: collect names, then desugar.
        const char *names[16];
        int nnames = 0; names[nnames++] = target;
        while (match_tok(T_COMMA)) {
            if (peek_tok(0)->kind != T_NAME) parse_error("expected name in tuple target");
            if (nnames >= 16) parse_error("for tuple target too long");
            names[nnames++] = peek_tok(0)->sval;
            tok_pos++;
        }
        expect(T_IN, "'in'");
        NODE *iter = parse_expr();
        // Desugar: introduce a hidden local `__t` to hold the current item.
        const char *tmp_name = intern_name("__forT__", 8);
        int tmp_idx = -1;
        if (cur_scope) tmp_idx = scope_add_local(cur_scope, tmp_name);

        NODE *body = parse_suite();
        // Build prefix: assign tuple elements to each name.
        NODE *prefix = NULL;
        for (int i = nnames - 1; i >= 0; i--) {
            NODE *idx = ALLOC_node_const_int(i);
            NODE *el  = ALLOC_node_subscript_get(make_load(tmp_name), idx);
            NODE *as  = make_store(names[i], el);
            prefix = prefix ? ALLOC_node_seq(as, prefix) : as;
        }
        NODE *new_body = ALLOC_node_seq(prefix, body);
        if (cur_scope && tmp_idx >= 0)
            return ALLOC_node_for_local((uint32_t)tmp_idx, iter, new_body);
        return ALLOC_node_for_global(tmp_name, iter, new_body);
    }
    expect(T_IN, "'in'");
    NODE *iter = parse_expr();
    NODE *body = parse_suite();
    if (cur_scope && !scope_is_global_decl(cur_scope, target)) {
        int idx = scope_add_local(cur_scope, target);
        return ALLOC_node_for_local((uint32_t)idx, iter, body);
    }
    return ALLOC_node_for_global(target, iter, body);
}

// Parse params for `def NAME(params):` — handles default values,
// `*args`, `**kwargs`, and keyword-only params (after `*args`).
// Returns slot counts + table indices via out-params.
//   nparams         total slots
//   n_pos_named     # of pos-or-keyword params (before *args)
//   ndefaults       # of (slot, expr) entries pushed to PYSTRO_DEFAULTS
//   didx            base in PYSTRO_DEFAULTS
//   nidx            base in PYSTRO_NAME_TABLE
//   flags           bit0=has_va, bit1=has_kw
static void
parse_params(Scope *sc, int *out_nparams, int *out_n_pos_named, int *out_ndefaults,
             uint32_t *out_didx, uint32_t *out_nidx, uint32_t *out_flags)
{
    int nparams = 0;
    int n_pos_named = 0;
    bool saw_star = false;
    bool has_va = false, has_kw = false;
    struct pydefault defs[32];
    int ndefaults = 0;
    bool seen_default_in_pos = false;
    const char *names[32];
    int nnames = 0;

    if (peek_tok(0)->kind != T_RPAREN) {
        for (;;) {
            // **kwargs always comes last.
            if (match_tok(T_STAR_STAR)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '**'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                scope_add_local(sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_kw = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            // *args (and bare `*` could mark "kwonly start" but we
            // require a name for now).
            if (match_tok(T_STAR)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '*'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                scope_add_local(sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_va = true;
                saw_star = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            if (peek_tok(0)->kind != T_NAME) parse_error("expected parameter name");
            const char *pn = peek_tok(0)->sval;
            tok_pos++;
            scope_add_local(sc, pn);
            names[nnames++] = pn;
            int slot = nparams;
            nparams++;
            if (!saw_star) n_pos_named++;
            if (match_tok(T_ASSIGN)) {
                if (ndefaults >= 32) parse_error("too many defaults");
                Scope *saved = cur_scope; cur_scope = sc->parent;
                defs[ndefaults].slot = slot;
                defs[ndefaults].expr = parse_expr();
                ndefaults++;
                if (!saw_star) seen_default_in_pos = true;
                cur_scope = saved;
            } else if (seen_default_in_pos && !saw_star) {
                parse_error("non-default after default in '%s'", pn);
            }
            if (!match_tok(T_COMMA)) break;
        }
    }
    if (!has_va) {
        // n_pos_named already counts everything since saw_star never flipped.
    }
    *out_nparams = nparams;
    *out_n_pos_named = n_pos_named;
    *out_ndefaults = ndefaults;
    *out_didx = (uint32_t)defaults_reserve(defs, ndefaults);
    *out_nidx = (uint32_t)name_table_reserve(names, nnames);
    *out_flags = (has_va ? 1u : 0u) | (has_kw ? 2u : 0u);
}

static NODE *
parse_def(void)
{
    expect(T_DEF, "'def'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected function name");
    const char *fname = peek_tok(0)->sval;
    tok_pos++;
    expect(T_LPAREN, "'('");

    Scope sc = {0}; sc.parent = cur_scope;
    int nparams, n_pos_named, ndefaults;
    uint32_t didx, nidx, flags;
    parse_params(&sc, &nparams, &n_pos_named, &ndefaults, &didx, &nidx, &flags);
    expect(T_RPAREN, "')'");
    expect(T_COLON, "':'");

    size_t suite_start = tok_pos;
    size_t suite_end   = find_suite_end(suite_start);
    collect_locals_in_range(&sc, suite_start, suite_end);

    // Generator: reserve a hidden local for the result list.  Each
    // `yield E` inside the body becomes `__genr__.append(E)`, and the
    // implicit final `return __genr__` is appended below.
    if (sc.is_generator) {
        sc.gen_result_slot = scope_add_local(&sc, intern_name("__genr__", 8));
    }

    Scope *saved = cur_scope; cur_scope = &sc;
    NODE *body;
    if (peek_tok(0)->kind == T_NEWLINE) {
        tok_pos++;
        expect(T_INDENT, "indent");
        NODE *stmts[1024];
        int n = 0;
        while (peek_tok(0)->kind != T_DEDENT && peek_tok(0)->kind != T_EOF) {
            if (n >= 1024) parse_error("suite too long");
            stmts[n++] = parse_stmt();
        }
        expect(T_DEDENT, "dedent");
        body = seq_of(stmts, n);
    } else {
        body = parse_simple_stmt();
        expect(T_NEWLINE, "newline");
    }
    cur_scope = saved;

    // Generator wrap: prepend `__genr__ = []`, append `return __genr__`.
    if (sc.is_generator) {
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = ALLOC_node_lset((uint32_t)sc.gen_result_slot,
                                     ALLOC_node_make_list((uint32_t)empty_idx, 0));
        NODE *ret  = ALLOC_node_return(ALLOC_node_lref((uint32_t)sc.gen_result_slot));
        body = ALLOC_node_seq(init, ALLOC_node_seq(body, ret));
    }

    NODE *def_node = ALLOC_node_def(fname, (uint32_t)nparams, (uint32_t)n_pos_named,
                                    (uint32_t)sc.nlocals,
                                    (uint32_t)ndefaults, didx,
                                    (uint32_t)(sc.has_nested_def ? 0 : 1),
                                    nidx, flags, body);
    // Class body: node_def's side effect already added the method;
    // discard the returned value (the method dict is the binding).
    if (in_class_body) return def_node;
    // Otherwise bind the function value at fname — local in a function
    // scope, global at top level.
    return make_store(fname, def_node);
}

static NODE *
parse_class(void)
{
    expect(T_CLASS, "'class'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected class name");
    const char *cname = peek_tok(0)->sval;
    tok_pos++;
    NODE *base = ALLOC_node_const_none();
    if (match_tok(T_LPAREN)) {
        if (peek_tok(0)->kind != T_RPAREN) base = parse_expr();
        expect(T_RPAREN, "')'");
    }
    bool saved_icb = in_class_body;
    NODE *saved_base = cur_class_base;
    in_class_body = true;
    cur_class_base = base;
    NODE *body = parse_suite();
    in_class_body = saved_icb;
    cur_class_base = saved_base;
    NODE *cls = ALLOC_node_class(cname, base, body);
    // Bind the class object at `cname`.  Same nesting rule as def.
    if (in_class_body) return cls;     // class inside class body — rare, but binds via outer
    return make_store(cname, cls);
}

// `with EXPR as NAME:` desugars to:
//   __cm = EXPR
//   NAME = __cm.__enter__()
//   try:
//     body
//   finally:
//     __cm.__exit__(None, None, None)
static NODE *
parse_with(void)
{
    expect(T_WITH, "'with'");
    NODE *cm_expr = parse_expr();
    const char *as_name = NULL;
    if (match_tok(T_AS)) {
        if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after 'as'");
        as_name = peek_tok(0)->sval;
        tok_pos++;
    }
    NODE *body = parse_suite();
    // Hidden temp.
    const char *tmp = new_temp_name("__cm");
    NODE *load_cm;
    NODE *init = build_temp_init(tmp, cm_expr, &load_cm);
    NODE *enter_call = ALLOC_node_method_0(load_cm, intern_name("__enter__", 9));
    NODE *bind = as_name ? make_store(as_name, enter_call)
                         : enter_call;       // discard returned value
    NODE *none1 = ALLOC_node_const_none();
    NODE *none2 = ALLOC_node_const_none();
    NODE *none3 = ALLOC_node_const_none();
    // method_n with 3 args
    NODE *args3[3] = { none1, none2, none3 };
    size_t base = node_table_reserve(args3, 3);
    NODE *exit_call = ALLOC_node_method_n(load_cm, intern_name("__exit__", 8), (uint32_t)base, 3);
    // try body, no except handlers, finally = exit_call
    NODE *try_node = ALLOC_node_try(body, 0, 0, exit_call);
    return ALLOC_node_seq(init, ALLOC_node_seq(bind, try_node));
}

static NODE *
parse_try(void)
{
    expect(T_TRY, "'try'");
    NODE *body = parse_suite();

    struct pyhandler hs[16];
    int nh = 0;
    while (peek_tok(0)->kind == T_EXCEPT) {
        tok_pos++;
        struct pyhandler h = {0};
        if (peek_tok(0)->kind != T_COLON) {
            h.exc_class = parse_expr();
            if (match_tok(T_AS)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after 'as'");
                const char *nm = peek_tok(0)->sval;
                tok_pos++;
                if (cur_scope && !scope_is_global_decl(cur_scope, nm)) {
                    h.name = nm;
                    h.name_is_global = false;
                    h.name_slot = scope_add_local(cur_scope, nm);
                } else {
                    h.name = nm;
                    h.name_is_global = true;
                }
            }
        }
        h.body = parse_suite();
        if (nh >= 16) parse_error("too many except handlers");
        hs[nh++] = h;
    }
    NODE *finally_body = NULL;
    if (match_tok(T_FINALLY)) finally_body = parse_suite();
    if (nh == 0 && !finally_body) parse_error("try without except/finally");

    size_t hidx = handlers_reserve(hs, nh);
    return ALLOC_node_try(body, (uint32_t)hidx, (uint32_t)nh, finally_body ? finally_body : ALLOC_node_nop());
}

static NODE *
parse_return(void)
{
    expect(T_RETURN, "'return'");
    NODE *v;
    if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI)
        v = ALLOC_node_const_none();
    else
        v = parse_expr_list();
    return ALLOC_node_return(v);
}

// `yield E` (and `yield from E`) — eager v0 implementation: translates
// to `__genr__.append(E)` (or extend for `yield from`).  The wrapping
// is set up in parse_def: a hidden local `__genr__` plus a trailing
// `return __genr__`.
static NODE *
parse_yield(void)
{
    expect(T_YIELD, "'yield'");
    if (!cur_scope || !cur_scope->is_generator) {
        // The pre-scan should have caught this, but be defensive.
        parse_error("'yield' outside generator function");
    }
    NODE *load_r = ALLOC_node_lref((uint32_t)cur_scope->gen_result_slot);
    // `yield from E` → for each x in E: __genr__.append(x).  v0 hack:
    // call extend() if it's iterable.  Plain method_1(__genr__, "extend", E).
    if (peek_tok(0)->kind == T_FROM) {
        tok_pos++;
        NODE *e = parse_expr();
        return ALLOC_node_method_1(load_r, intern_name("extend", 6), e);
    }
    NODE *e;
    if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI
            || peek_tok(0)->kind == T_RPAREN || peek_tok(0)->kind == T_COMMA)
        e = ALLOC_node_const_none();
    else
        e = parse_expr();
    return ALLOC_node_method_1(load_r, intern_name("append", 6), e);
}

static NODE *
parse_raise(void)
{
    expect(T_RAISE, "'raise'");
    if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI)
        return ALLOC_node_raise_bare();
    NODE *e = parse_expr();
    return ALLOC_node_raise(e);
}

static NODE *
parse_global_decl(void)
{
    expect(T_GLOBAL, "'global'");
    while (peek_tok(0)->kind == T_NAME) {
        if (cur_scope) scope_add_global_decl(cur_scope, peek_tok(0)->sval);
        tok_pos++;
        if (!match_tok(T_COMMA)) break;
    }
    return ALLOC_node_nop();
}

static NODE *
parse_nonlocal_decl(void)
{
    expect(T_NONLOCAL, "'nonlocal'");
    while (peek_tok(0)->kind == T_NAME) {
        if (cur_scope) scope_add_nonlocal_decl(cur_scope, peek_tok(0)->sval);
        tok_pos++;
        if (!match_tok(T_COMMA)) break;
    }
    return ALLOC_node_nop();
}

// Augmented-assignment desugar helper.  Produces RHS = LHS op RHS.
static NODE *
augop_apply(int kind, NODE *lhs, NODE *rhs)
{
    switch (kind) {
      case T_PLUS_EQ:        return ALLOC_node_add(lhs, rhs);
      case T_MINUS_EQ:       return ALLOC_node_sub(lhs, rhs);
      case T_STAR_EQ:        return ALLOC_node_mul(lhs, rhs);
      case T_SLASH_EQ:       return ALLOC_node_truediv(lhs, rhs);
      case T_SLASH_SLASH_EQ: return ALLOC_node_floordiv(lhs, rhs);
      case T_PERCENT_EQ:     return ALLOC_node_mod(lhs, rhs);
      case T_AMP_EQ:         return ALLOC_node_bit_and(lhs, rhs);
      case T_PIPE_EQ:        return ALLOC_node_bit_or(lhs, rhs);
      case T_CARET_EQ:       return ALLOC_node_bit_xor(lhs, rhs);
      case T_LSHIFT_EQ:      return ALLOC_node_lshift(lhs, rhs);
      case T_RSHIFT_EQ:      return ALLOC_node_rshift(lhs, rhs);
      case T_STAR_STAR_EQ:   return ALLOC_node_pow(lhs, rhs);
    }
    return rhs;
}

static bool
is_aug_assign(int k)
{
    return k == T_PLUS_EQ || k == T_MINUS_EQ || k == T_STAR_EQ
        || k == T_SLASH_EQ || k == T_SLASH_SLASH_EQ || k == T_PERCENT_EQ
        || k == T_AMP_EQ || k == T_PIPE_EQ || k == T_CARET_EQ
        || k == T_LSHIFT_EQ || k == T_RSHIFT_EQ || k == T_STAR_STAR_EQ;
}

// Forward decl.
static NODE *parse_assignable_target(NODE *rhs);

// Statement-form assignment.  Handles:
//   NAME = expr
//   NAME op= expr (augmented)
//   NAME[i] = expr  /  NAME.attr = expr  / op= versions
//   NAME (',' NAME)+ '=' expr   (flat tuple unpack)
//
// Multi-assign (`a = b = expr`) is not supported in v0.
static NODE *
parse_simple_stmt(void)
{
    int k = peek_tok(0)->kind;
    if (k == T_PASS)     { tok_pos++; return ALLOC_node_nop(); }
    if (k == T_BREAK)    { tok_pos++; return ALLOC_node_break(); }
    if (k == T_CONTINUE) { tok_pos++; return ALLOC_node_continue(); }
    if (k == T_RETURN)   return parse_return();
    if (k == T_RAISE)    return parse_raise();
    if (k == T_YIELD)    return parse_yield();
    if (k == T_GLOBAL)   return parse_global_decl();
    if (k == T_NONLOCAL) return parse_nonlocal_decl();
    if (k == T_IMPORT || k == T_FROM) {
        while (peek_tok(0)->kind != T_NEWLINE && peek_tok(0)->kind != T_EOF) tok_pos++;
        return ALLOC_node_nop();
    }

    // Multi-target unpack assignment: NAME (',' NAME)+ '=' expr.
    if (k == T_NAME && peek_tok(1)->kind == T_COMMA) {
        const char *names[16];
        int nn = 0;
        size_t p = tok_pos;
        names[nn++] = tok_arr[p].sval;
        p++;
        bool ok = true;
        while (tok_arr[p].kind == T_COMMA) {
            p++;
            if (tok_arr[p].kind != T_NAME || nn >= 16) { ok = false; break; }
            names[nn++] = tok_arr[p].sval;
            p++;
        }
        if (ok && tok_arr[p].kind == T_ASSIGN) {
            tok_pos = p + 1;
            NODE *rhs = parse_expr_list();
            struct pyunpack_target ts[16];
            for (int i = 0; i < nn; i++) {
                if (cur_scope && !scope_is_global_decl(cur_scope, names[i])) {
                    ts[i].is_local = true;
                    ts[i].slot = scope_add_local(cur_scope, names[i]);
                    ts[i].global_name = NULL;
                } else {
                    ts[i].is_local = false;
                    ts[i].slot = -1;
                    ts[i].global_name = names[i];
                }
            }
            size_t idx = unpack_reserve(ts, nn);
            return ALLOC_node_unpack_assign((uint32_t)idx, (uint32_t)nn, rhs);
        }
    }

    // Save start, parse LHS as expression, then peek operator.
    size_t lhs_start = tok_pos;
    NODE *lhs_expr = parse_expr();
    int k2 = peek_tok(0)->kind;

    if (k2 == T_ASSIGN) {
        tok_pos++;
        NODE *rhs = parse_expr_list();
        // Re-parse LHS as a target so we emit the right store node.
        size_t saved = tok_pos; tok_pos = lhs_start;
        NODE *store = parse_assignable_target(rhs);
        tok_pos = saved;
        return store;
    }

    if (is_aug_assign(k2)) {
        int op = k2;
        tok_pos++;
        NODE *rhs = parse_expr_list();
        // For the LHS load: reuse `lhs_expr` (it's an expression node
        // shaped as a get-trailer chain, which is fine to evaluate).
        NODE *new_rhs = augop_apply(op, lhs_expr, rhs);
        size_t saved = tok_pos; tok_pos = lhs_start;
        NODE *store = parse_assignable_target(new_rhs);
        tok_pos = saved;
        return store;
    }

    // Expression statement.
    return lhs_expr;
}

// Parse an assignment target expression at the current token position
// and emit a store of `rhs` into it.  Supports NAME, NAME (DOT NAME)*,
// NAME ('[' subscript ']')*, mixing the two, with the last trailer
// being the actual assignment site.
static NODE *
parse_assignable_target(NODE *rhs)
{
    Tok *t = peek_tok(0);
    if (t->kind != T_NAME) parse_error("invalid assignment target");
    const char *base_name = t->sval;
    tok_pos++;
    // Walk trailers.  We need to know the last trailer to choose between
    // attr_set / subscript_set; everything before is built as a get chain.
    // Strategy: collect trailers into an array, build a get for all but
    // the last, then emit attr_set / subscript_set for the last.
    enum trailer_kind { TR_DOT, TR_SUB };
    struct {
        int kind;
        const char *name;       // TR_DOT
        NODE *idx;              // TR_SUB (single-index only)
        NODE *slice_a, *slice_b, *slice_c;  // (unused for store)
        bool is_slice;
    } trs[16];
    int ntr = 0;
    while (peek_tok(0)->kind == T_DOT || peek_tok(0)->kind == T_LBRACK) {
        if (ntr >= 16) parse_error("too many trailers in target");
        if (match_tok(T_DOT)) {
            if (peek_tok(0)->kind != T_NAME) parse_error("expected attr name");
            trs[ntr].kind = TR_DOT;
            trs[ntr].name = peek_tok(0)->sval;
            tok_pos++;
            ntr++;
        } else {
            expect(T_LBRACK, "'['");
            // single-index only for assign target
            trs[ntr].kind = TR_SUB;
            trs[ntr].idx = parse_expr();
            trs[ntr].is_slice = false;
            expect(T_RBRACK, "']'");
            ntr++;
        }
    }
    if (ntr == 0) {
        // bare NAME
        return make_store(base_name, rhs);
    }
    // Build get chain up to the last trailer.
    NODE *cur = make_load(base_name);
    for (int i = 0; i < ntr - 1; i++) {
        if (trs[i].kind == TR_DOT) cur = ALLOC_node_attr_get(cur, trs[i].name);
        else                       cur = ALLOC_node_subscript_get(cur, trs[i].idx);
    }
    // Emit final store.
    int last = ntr - 1;
    if (trs[last].kind == TR_DOT) return ALLOC_node_attr_set(cur, trs[last].name, rhs);
    return ALLOC_node_subscript_set(cur, trs[last].idx, rhs);
}

// `@dec` desugars to: parse the def normally, then emit `name = dec(name)`.
// Multiple decorators stack innermost-first: `@a @b def f` →
// `def f; f = b(f); f = a(f)`.
static NODE *
parse_decorated(void)
{
    NODE *decs[16];
    int ndecs = 0;
    while (match_tok(T_AT)) {
        if (ndecs >= 16) parse_error("too many decorators");
        decs[ndecs++] = parse_or();
        expect(T_NEWLINE, "newline after decorator");
        while (match_tok(T_NEWLINE)) {}
    }
    NODE *body;
    const char *target = NULL;
    if (peek_tok(0)->kind == T_DEF) {
        // Capture the function name so we can emit `name = dec(name)`.
        target = tok_arr[tok_pos + 1].sval;
        body = parse_def();
    } else if (peek_tok(0)->kind == T_CLASS) {
        target = tok_arr[tok_pos + 1].sval;
        body = parse_class();
    } else {
        parse_error("expected 'def' or 'class' after decorator");
    }
    // Build the wrap chain: load → dec_inner(load) → dec_outer(...)
    NODE *result = body;
    for (int i = ndecs - 1; i >= 0; i--) {
        NODE *load = make_load(target);
        NODE *call = ALLOC_node_call_1(decs[i], load);
        result = ALLOC_node_seq(result, make_store(target, call));
    }
    return result;
}

static NODE *
parse_stmt(void)
{
    int k = peek_tok(0)->kind;
    if (k == T_AT)     return parse_decorated();
    if (k == T_DEF)    return parse_def();
    if (k == T_CLASS)  return parse_class();
    if (k == T_IF)     return parse_if();
    if (k == T_WHILE)  return parse_while();
    if (k == T_FOR)    return parse_for();
    if (k == T_TRY)    return parse_try();
    if (k == T_WITH)   return parse_with();
    NODE *s = parse_simple_stmt();
    // Allow optional ; or NEWLINE
    if (match_tok(T_SEMI)) {
        // chain another simple stmt
        NODE *more = parse_stmt();
        // if `more` is a stmt that already consumed its newline, just seq.
        return ALLOC_node_seq(s, more);
    }
    expect(T_NEWLINE, "newline");
    return s;
}

static NODE *
parse_program(void)
{
    NODE *stmts[4096];
    int n = 0;
    while (match_tok(T_NEWLINE)) {}
    while (peek_tok(0)->kind != T_EOF) {
        if (n >= 4096) parse_error("program too long");
        stmts[n++] = parse_stmt();
        while (match_tok(T_NEWLINE)) {}
    }
    return seq_of(stmts, n);
}
