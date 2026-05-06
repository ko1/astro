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

// Spread args at call site (positional / kwarg + their `*expr` / `**expr`).
struct pyspread_arg *PYSTRO_SPREADS = NULL;
static size_t pystro_spreads_len, pystro_spreads_capa;
static size_t spreads_reserve(struct pyspread_arg *src, size_t n)
{
    if (pystro_spreads_len + n > pystro_spreads_capa) {
        size_t cap = pystro_spreads_capa ? pystro_spreads_capa * 2 : 16;
        while (cap < pystro_spreads_len + n) cap *= 2;
        PYSTRO_SPREADS = (struct pyspread_arg *)GC_realloc(PYSTRO_SPREADS, cap * sizeof(struct pyspread_arg));
        pystro_spreads_capa = cap;
    }
    size_t base = pystro_spreads_len;
    for (size_t i = 0; i < n; i++) PYSTRO_SPREADS[base + i] = src[i];
    pystro_spreads_len += n;
    return base;
}

struct pypat  *PYSTRO_PATTERNS = NULL;
static size_t pystro_patterns_len, pystro_patterns_capa;
static int pat_alloc(struct pypat p)
{
    if (pystro_patterns_len == pystro_patterns_capa) {
        size_t cap = pystro_patterns_capa ? pystro_patterns_capa * 2 : 16;
        PYSTRO_PATTERNS = (struct pypat *)GC_realloc(PYSTRO_PATTERNS, cap * sizeof(struct pypat));
        pystro_patterns_capa = cap;
    }
    int idx = (int)pystro_patterns_len++;
    PYSTRO_PATTERNS[idx] = p;
    return idx;
}

struct pycase *PYSTRO_CASES = NULL;
static size_t pystro_cases_len, pystro_cases_capa;
static size_t cases_reserve(struct pycase *src, size_t n)
{
    if (pystro_cases_len + n > pystro_cases_capa) {
        size_t cap = pystro_cases_capa ? pystro_cases_capa * 2 : 8;
        while (cap < pystro_cases_len + n) cap *= 2;
        PYSTRO_CASES = (struct pycase *)GC_realloc(PYSTRO_CASES, cap * sizeof(struct pycase));
        pystro_cases_capa = cap;
    }
    size_t base = pystro_cases_len;
    for (size_t i = 0; i < n; i++) PYSTRO_CASES[base + i] = src[i];
    pystro_cases_len += n;
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
    if (getenv("PYSTRO_DEBUG_PARSE")) {
        fprintf(stderr, " [tok_pos=%zu", tok_pos);
        for (int i = -3; i <= 3; i++) {
            int p = (int)tok_pos + i;
            if (p < 0 || p >= (int)tok_len) continue;
            Tok *tt = &tok_arr[p];
            fprintf(stderr, " %s%d:%d", i==0?"*":"", (int)p, tt->kind);
            if (tt->kind == T_NAME && tt->sval) fprintf(stderr, ":%s", tt->sval);
        }
        fprintf(stderr, "]");
    }
    fputc('\n', stderr);
    exit(1);
}

static void
expect(int kind, const char *what)
{
    if (!match_tok(kind)) {
        extern const char *tok_kind_name(int k);
        parse_error("expected %s, got %s", what, tok_kind_name(peek_tok(0)->kind));
    }
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

// Name remap stack used by list/set/dict/genexp comprehensions to give
// loop targets their own (synthetic) slots, so they don't leak into the
// enclosing scope.  Pushed by prescan_comp_targets, popped after the
// comprehension finishes parsing.  Make_load / make_store consult this
// before resolving names, so references to the loop target inside the
// comprehension body resolve to the synthetic local.
typedef struct CompRemap {
    const char *orig;        // user-visible name
    const char *synth;       // mangled local name
    Scope      *scope;       // scope at which the remap was pushed
} CompRemap;
static CompRemap comp_remap_stack[64];
static int comp_remap_top = 0;
static int comp_remap_uid = 0;

static const char *
comp_remap_lookup(const char *name)
{
    // Only honour remaps whose scope matches the active one.  When a
    // nested scope (lambda / def) is entered, its own locals take
    // precedence and the comp's mangled name shouldn't bleed in.
    for (int i = comp_remap_top - 1; i >= 0; i--) {
        if (comp_remap_stack[i].scope != cur_scope) continue;
        if (comp_remap_stack[i].orig == name) return comp_remap_stack[i].synth;
    }
    return NULL;
}

// Resolve `name` through the active comp remap stack — caller uses the
// returned name when adding/looking up locals.  If no remap, returns
// `name` unchanged.
static const char *
comp_resolve(const char *name)
{
    const char *m = comp_remap_lookup(name);
    return m ? m : name;
}

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

// Walrus pre-scan: `NAME := expr` introduces NAME as a local in the
// enclosing function.  Pick this up so make_load resolves it as lref.
// Also collect `NAME =` etc. — see existing logic below.

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
    int paren_depth = 0;               // ( [ { nesting — kwargs inside a
                                        // call must NOT be registered.
    int skip_until_dedent = -1;        // depth at which we entered nested scope
    bool in_global_decl = false;
    for (size_t i = start_pos; i < end_pos; i++) {
        Tok *t = &tok_arr[i];
        if (t->kind == T_INDENT) { depth++; continue; }
        if (t->kind == T_DEDENT) {
            if (skip_until_dedent >= 0 && depth == skip_until_dedent) skip_until_dedent = -1;
            depth--; continue;
        }
        if (t->kind == T_LPAREN || t->kind == T_LBRACK || t->kind == T_LBRACE) paren_depth++;
        else if (t->kind == T_RPAREN || t->kind == T_RBRACK || t->kind == T_RBRACE) {
            if (paren_depth > 0) paren_depth--;
        }
        if (skip_until_dedent >= 0) continue;

        // A nested `lambda` makes this scope non-leaf — the lambda
        // escapes via its closure parent, so this frame must be heap-
        // allocated rather than alloca'd.  We don't *register* a name
        // here (lambdas are anonymous).
        if (t->kind == T_LAMBDA) {
            s->has_nested_def = true;
        }
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
        if (t->kind == T_FOR && i + 1 < end_pos && tok_arr[i+1].kind == T_NAME
            && paren_depth == 0) {
            // for NAME in ... (top-level statement, NOT a comprehension's
            // `for` clause inside [], (), or {} — those get their own
            // synthetic comp-private slots via comp_remap).
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
        if (t->kind == T_WITH) {
            // with EXPR as NAME [, EXPR as NAME]* :
            // Walk the line, registering every `as NAME` we see.  For the
            // 3.10+ parenthesised form `with (cm1, cm2 as x):`, the
            // outer parens are syntactic — don't count them as nesting.
            size_t j = i + 1;
            int paren = 0;
            // Detect outer parens (followed by `:` after matching `)`).
            bool outer_parens = false;
            if (j < end_pos && tok_arr[j].kind == T_LPAREN) {
                int d = 0;
                size_t k = j;
                while (k < end_pos && tok_arr[k].kind != T_NEWLINE) {
                    int kk = tok_arr[k].kind;
                    if (kk == T_LPAREN) d++;
                    else if (kk == T_RPAREN) {
                        d--;
                        if (d == 0) {
                            if (k + 1 < end_pos && tok_arr[k + 1].kind == T_COLON)
                                outer_parens = true;
                            break;
                        }
                    }
                    k++;
                }
                if (outer_parens) j++;  // skip outer `(`
            }
            while (j < end_pos && tok_arr[j].kind != T_NEWLINE) {
                int kk = tok_arr[j].kind;
                if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) paren++;
                else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) {
                    if (outer_parens && paren == 0 && kk == T_RPAREN) break;
                    paren--;
                }
                else if (paren == 0 && kk == T_COLON) break;
                else if (paren == 0 && kk == T_AS && j + 1 < end_pos
                         && tok_arr[j+1].kind == T_NAME) {
                    if (!scope_is_global_decl(s, tok_arr[j+1].sval))
                        scope_add_local(s, tok_arr[j+1].sval);
                }
                j++;
            }
            continue;
        }
        // Starred-only multi-target unpack at stmt head: `*name, ... = ...`.
        if (t->kind == T_STAR && i + 1 < end_pos && tok_arr[i+1].kind == T_NAME) {
            size_t j = i;
            bool ok2 = true;
            while (j < end_pos) {
                if (tok_arr[j].kind == T_STAR) j++;
                if (j >= end_pos || tok_arr[j].kind != T_NAME) { ok2 = false; break; }
                j++;
                if (j >= end_pos) { ok2 = false; break; }
                if (tok_arr[j].kind == T_ASSIGN) break;
                if (tok_arr[j].kind != T_COMMA) { ok2 = false; break; }
                j++;
                if (j < end_pos && tok_arr[j].kind == T_ASSIGN) break;
            }
            if (ok2 && j < end_pos && tok_arr[j].kind == T_ASSIGN) {
                size_t kw = i;
                while (kw < j) {
                    if (tok_arr[kw].kind == T_STAR) kw++;
                    if (kw < j && tok_arr[kw].kind == T_NAME &&
                        !scope_is_global_decl(s, tok_arr[kw].sval) &&
                        !scope_is_nonlocal_decl(s, tok_arr[kw].sval))
                        scope_add_local(s, tok_arr[kw].sval);
                    kw++;
                    if (kw < j && tok_arr[kw].kind == T_COMMA) kw++;
                }
            }
        }
        // NAME = | NAME += ... | NAME := ... → assignment, treat as local.
        // But `obj.x = ...` (preceded by `.`) is an attribute set, not a binding.
        // And `f(name=val)` (preceded by `,` or `(` inside a call) is a kwarg.
        if (t->kind == T_NAME && i + 1 < end_pos) {
            int next = tok_arr[i+1].kind;
            int prev = i > start_pos ? tok_arr[i-1].kind : T_NEWLINE;
            // We accept stmt-start positions: after NEWLINE/INDENT/DEDENT/SEMI,
            // or chained after `=` (for `a = b = ...`).  Walrus (`:=`) is also
            // an expression-level assignment so we always honour it.
            bool stmt_start = (prev == T_NEWLINE || prev == T_INDENT ||
                               prev == T_DEDENT || prev == T_SEMI || prev == T_ASSIGN);
            bool is_attr_or_subscript = (prev == T_DOT);
            bool is_assign_op = (next == T_ASSIGN ||
                                 next == T_PLUS_EQ || next == T_MINUS_EQ ||
                                 next == T_STAR_EQ || next == T_SLASH_EQ || next == T_SLASH_SLASH_EQ ||
                                 next == T_PERCENT_EQ || next == T_AMP_EQ || next == T_PIPE_EQ ||
                                 next == T_CARET_EQ || next == T_LSHIFT_EQ || next == T_RSHIFT_EQ ||
                                 next == T_STAR_STAR_EQ);
            // Only treat as a local binding if this looks like a stmt-start
            // assignment (avoids registering kwarg names from calls).
            // Walrus is independent (always a real binding even in expressions).
            // Inside parens (a call's argument list, list/dict literal, etc.)
            // a NAME = expr is a kwarg, NOT a binding.
            bool inside_parens = (paren_depth > 0);
            if (!is_attr_or_subscript && !inside_parens &&
                ((stmt_start && is_assign_op) || next == T_WALRUS)) {
                if (!scope_is_global_decl(s, t->sval) &&
                    !scope_is_nonlocal_decl(s, t->sval))
                    scope_add_local(s, t->sval);
            } else if (next == T_WALRUS) {
                // Walrus binds regardless of paren context.
                if (!scope_is_global_decl(s, t->sval) &&
                    !scope_is_nonlocal_decl(s, t->sval))
                    scope_add_local(s, t->sval);
            }
            // Multi-target tuple unpack: `a, b = ...` or `a, *rest, b = ...`.
            // Only register if the chain ends with `=`.  Skip when we're
            // inside a paren/bracket/brace (function call kwargs, etc.).
            if (next == T_COMMA && stmt_start && paren_depth == 0) {
                size_t j = i;
                bool ok = true;
                while (j < end_pos) {
                    if (tok_arr[j].kind == T_STAR) j++;
                    if (j >= end_pos || tok_arr[j].kind != T_NAME) { ok = false; break; }
                    j++;
                    if (j >= end_pos) { ok = false; break; }
                    if (tok_arr[j].kind == T_ASSIGN) break;
                    if (tok_arr[j].kind != T_COMMA) { ok = false; break; }
                    j++;
                    if (j < end_pos && tok_arr[j].kind == T_ASSIGN) break;
                }
                if (ok && j < end_pos && tok_arr[j].kind == T_ASSIGN) {
                    // Register names.
                    size_t kw = i;
                    while (kw < j) {
                        if (tok_arr[kw].kind == T_STAR) kw++;
                        if (kw < j && tok_arr[kw].kind == T_NAME &&
                            !scope_is_global_decl(s, tok_arr[kw].sval) &&
                            !scope_is_nonlocal_decl(s, tok_arr[kw].sval))
                            scope_add_local(s, tok_arr[kw].sval);
                        kw++;
                        if (kw < j && tok_arr[kw].kind == T_COMMA) kw++;
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
    name = comp_resolve(name);
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
    if (in_class_body) return ALLOC_node_class_body_load(name);
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
    // Class body: bindings go to c->current_class.methods (which serves
    // as both the method table and the class attribute namespace).
    if (in_class_body) return ALLOC_node_class_method_set(name, rhs);
    name = comp_resolve(name);
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
            // Find matching '}', allowing nested braces minimally.
            // `!c` (depth 1) sets a conversion; `:` starts format spec.
            // expr_end = first of `!`, `:`, `}` at depth 1.
            size_t j = i + 1;
            size_t spec_start = 0;
            size_t conv_pos = 0;
            size_t eq_pos = 0;       // position of `=` for debug `{x=}` syntax
            int depth = 1;
            int paren = 0;
            while (j < len && depth > 0) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') depth--;
                else if (s[j] == '(' || s[j] == '[') paren++;
                else if (s[j] == ')' || s[j] == ']') paren--;
                else if (s[j] == '=' && depth == 1 && paren == 0 && conv_pos == 0
                         && spec_start == 0 && eq_pos == 0
                         && j + 1 < len
                         && (s[j+1] == '}' || s[j+1] == '!' || s[j+1] == ':')
                         // Avoid mistaking `==` for the debug =.
                         && (j == i + 1 || s[j-1] != '=')
                         && s[j+1] != '=') {
                    eq_pos = j;
                }
                else if (s[j] == '!' && depth == 1 && paren == 0 && conv_pos == 0
                         && spec_start == 0
                         && j + 1 < len && (s[j+1] == 'r' || s[j+1] == 's' || s[j+1] == 'a')
                         && j + 2 < len && (s[j+2] == ':' || s[j+2] == '}')) {
                    conv_pos = j;
                }
                else if (s[j] == ':' && depth == 1 && paren == 0 && spec_start == 0) {
                    spec_start = j;
                }
                if (depth > 0) j++;
            }
            if (j >= len) parse_error("unterminated f-string expression");

            size_t expr_end = eq_pos ? eq_pos
                            : conv_pos ? conv_pos
                            : (spec_start ? spec_start : j);
            char conv = conv_pos ? s[conv_pos + 1] : 0;

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

            // For `{x=}` debug syntax: default conversion is repr if no
            // explicit !r/!s/!a is given.  Also prepend "x=" literal.
            if (eq_pos && conv == 0 && !spec_start) conv = 'r';
            // Apply !r / !s / !a conversion (wraps expr).
            if (conv == 'r') {
                expr = ALLOC_node_call_1(ALLOC_node_gref(intern_name("repr", 4)), expr);
            } else if (conv == 'a') {
                expr = ALLOC_node_call_1(ALLOC_node_gref(intern_name("ascii", 5)), expr);
            } else if (conv == 's') {
                expr = ALLOC_node_call_1(ALLOC_node_gref("str"), expr);
            }
            NODE *p;
            if (spec_start) {
                // Build format(expr, "spec") call.  Spec may contain
                // nested {expr} which we recurse into via parse_fstring_payload.
                size_t spec_len = j - spec_start - 1;
                char *spec_buf = (char *)GC_malloc_atomic(spec_len + 1);
                memcpy(spec_buf, s + spec_start + 1, spec_len);
                spec_buf[spec_len] = '\0';
                bool has_nested = false;
                for (size_t k = 0; k < spec_len; k++)
                    if (spec_buf[k] == '{') { has_nested = true; break; }
                NODE *spec_node;
                if (has_nested) {
                    spec_node = parse_fstring_payload(spec_buf, spec_len);
                } else {
                    spec_node = ALLOC_node_const_str(intern_name(spec_buf, spec_len));
                }
                p = ALLOC_node_call_2(ALLOC_node_gref(intern_name("format", 6)),
                                      expr, spec_node);
            } else if (conv) {
                p = expr;
            } else {
                // No spec, no conv: use format(expr, "") so user
                // classes' __format__ is honoured.
                NODE *empty = ALLOC_node_const_str(intern_name("", 0));
                p = ALLOC_node_call_2(ALLOC_node_gref(intern_name("format", 6)),
                                      expr, empty);
            }
            // Prepend "<expr text>=" for debug syntax.
            if (eq_pos) {
                size_t et_len = eq_pos - i - 1;
                char *et = (char *)GC_malloc_atomic(et_len + 2);
                memcpy(et, s + i + 1, et_len);
                et[et_len] = '=';
                et[et_len + 1] = '\0';
                NODE *prefix = ALLOC_node_const_str(intern_name(et, et_len + 1));
                p = ALLOC_node_add(prefix, p);
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

// Forward decls (these are defined further down).
static const char *new_temp_name(const char *prefix);
static NODE *build_temp_init(const char *tmp, NODE *init_expr, NODE **out_load);
static NODE *parse_comp_clauses(NODE *inner_body);

// Helper: register a comp loop-target name with a synthetic local slot
// AND push a (orig → synth) remap so make_load/make_store inside the
// comp body resolves the user-visible name to the synthetic slot.
// Returns the index pushed (so the caller can pop later).
static int
comp_remap_push(const char *orig)
{
    if (comp_remap_top >= 64) parse_error("too many nested comprehension targets");
    char buf[64];
    int u = comp_remap_uid++;
    snprintf(buf, sizeof(buf), "__cmp$%d$%s", u, orig);
    const char *synth = intern_name(buf, strlen(buf));
    comp_remap_stack[comp_remap_top].orig = orig;
    comp_remap_stack[comp_remap_top].synth = synth;
    comp_remap_stack[comp_remap_top].scope = cur_scope;
    return comp_remap_top++;
}

// Scan tokens for `for NAME[, NAME]*` clauses and walrus targets
// (NAME `:=`) inside the current brace-balanced region, registering each
// NAME as a local in cur_scope.  Used by comprehension/genexp parsers
// so the target name resolves inside the body expression — even when
// the surrounding scope (e.g. a lambda) had no pre-scan.
static void
prescan_comp_targets(int close_kind)
{
    if (!cur_scope) return;
    size_t save = tok_pos;
    int depth = 0;
    while (peek_tok(0)->kind != T_EOF) {
        int k = peek_tok(0)->kind;
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
        else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) {
            if (depth == 0 && k == close_kind) break;
            depth--;
        } else if (depth == 0 && k == T_FOR) {
            tok_pos++;
            // Optional paren-wrapped target: `for (a, b) in ...`.
            if (peek_tok(0)->kind == T_LPAREN) tok_pos++;
            // Collect NAME (NAME ',')* until 'in'.  Each loop-target
            // gets a synthetic comp-private local so the original name
            // doesn't leak into the enclosing scope.
            while (peek_tok(0)->kind == T_NAME) {
                const char *orig = peek_tok(0)->sval;
                if (!scope_is_global_decl(cur_scope, orig) &&
                    !scope_is_nonlocal_decl(cur_scope, orig)) {
                    comp_remap_push(orig);
                    scope_add_local(cur_scope, comp_resolve(orig));
                }
                tok_pos++;
                if (peek_tok(0)->kind == T_LPAREN) {
                    int sub = 1;
                    tok_pos++;
                    while (sub > 0 && peek_tok(0)->kind != T_EOF) {
                        int kk = peek_tok(0)->kind;
                        if (kk == T_LPAREN) sub++;
                        else if (kk == T_RPAREN) sub--;
                        else if (kk == T_NAME && sub == 1) {
                            const char *o2 = peek_tok(0)->sval;
                            if (!scope_is_global_decl(cur_scope, o2)) {
                                comp_remap_push(o2);
                                scope_add_local(cur_scope, comp_resolve(o2));
                            }
                        }
                        tok_pos++;
                    }
                }
                if (!match_tok(T_COMMA)) break;
            }
            continue;
        } else if (k == T_NAME && peek_tok(1)->kind == T_WALRUS) {
            // Walrus expressions bind in the enclosing scope.
            if (!scope_is_global_decl(cur_scope, peek_tok(0)->sval) &&
                !scope_is_nonlocal_decl(cur_scope, peek_tok(0)->sval))
                scope_add_local(cur_scope, peek_tok(0)->sval);
        }
        tok_pos++;
    }
    tok_pos = save;
}

// Forward decls: implemented below parse_paren_or_tuple.
static NODE *parse_genexp_lazy(int saved_remap_at_paren);
static NODE *build_for_loop(const char *target_name, NODE *iter, NODE *body);
static NODE *parse_assignable_target(NODE *rhs);

// `( expr_list )` — single → expr; multi (or trailing comma) → tuple.
// Also handles `( expr for ... )` generator expression form.
static NODE *
parse_paren_or_tuple(void)
{
    expect(T_LPAREN, "'('");
    if (match_tok(T_RPAREN)) {
        // Empty tuple ().
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_tuple((uint32_t)base, 0);
    }
    // Pre-scan for spread.  If found, build a list and convert via tuple().
    {
        size_t save = tok_pos;
        bool has_spread = false;
        int depth = 0;
        while (peek_tok(0)->kind != T_EOF) {
            int k = peek_tok(0)->kind;
            if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
            else if (k == T_RPAREN) {
                if (depth == 0) break;
                depth--;
            } else if (k == T_RBRACK || k == T_RBRACE) depth--;
            else if (k == T_STAR && depth == 0) {
                int p = (tok_pos > 0) ? tok_arr[tok_pos - 1].kind : T_NEWLINE;
                if (p == T_LPAREN || p == T_COMMA) { has_spread = true; break; }
            }
            tok_pos++;
        }
        tok_pos = save;
        if (has_spread) {
            const char *tmp = new_temp_name("__tup");
            NODE *load_tmp;
            size_t empty_idx = node_table_reserve(NULL, 0);
            NODE *init = build_temp_init(tmp, ALLOC_node_make_list((uint32_t)empty_idx, 0), &load_tmp);
            NODE *result = init;
            for (;;) {
                if (peek_tok(0)->kind == T_RPAREN) break;
                if (match_tok(T_STAR)) {
                    NODE *e = parse_expr();
                    NODE *call = ALLOC_node_method_1(load_tmp, intern_name("extend", 6), e);
                    result = ALLOC_node_seq(result, call);
                } else {
                    NODE *e = parse_expr();
                    NODE *call = ALLOC_node_method_1(load_tmp, intern_name("append", 6), e);
                    result = ALLOC_node_seq(result, call);
                }
                if (!match_tok(T_COMMA)) break;
            }
            expect(T_RPAREN, "')'");
            // Convert list to tuple via builtin.
            NODE *call_tuple = ALLOC_node_call_1(
                ALLOC_node_gref(intern_name("tuple", 5)), load_tmp);
            return ALLOC_node_seq(result, call_tuple);
        }
    }
    int saved_remap = comp_remap_top;
    // Detect generator expression: scan forward past first expr looking
    // for top-level `for` before the closing `)`.  If we find one, take
    // the lazy-genexp path that synthesises a generator function with
    // `(expr) for x in OUTER` desugared as
    //   def __genexp$N(.0):
    //       for x in .0:
    //           yield expr
    //   __genexp$N(iter(OUTER))
    // so the source is consumed lazily (matches CPython).  Inner clauses
    // (nested for / if) and the expr live in the synthesised function's
    // scope; the outermost iterable is captured eagerly in the parent.
    {
        size_t look = tok_pos;
        int depth = 0;
        bool is_genexp = false;
        while (tok_arr[look].kind != T_EOF) {
            int kk = tok_arr[look].kind;
            if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) depth++;
            else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) {
                if (depth == 0) break;
                depth--;
            }
            else if (depth == 0 && kk == T_FOR) { is_genexp = true; break; }
            else if (depth == 0 && kk == T_NAME
                    && tok_arr[look].sval == intern_name("async", 5)
                    && tok_arr[look+1].kind == T_FOR) { is_genexp = true; break; }
            look++;
        }
        if (is_genexp) {
            NODE *r = parse_genexp_lazy(saved_remap);
            expect(T_RPAREN, "')'");
            return r;
        }
    }
    prescan_comp_targets(T_RPAREN);
    NODE *first = parse_expr();
    comp_remap_top = saved_remap;
    if (match_tok(T_RPAREN)) return first;
    // tuple
    NODE *items[1024];
    int n = 0; items[n++] = first;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_RPAREN) break;
        if (n >= 1024) parse_error("tuple too long");
        items[n++] = parse_expr();
    }
    expect(T_RPAREN, "')'");
    size_t base = node_table_reserve(items, n);
    return ALLOC_node_make_tuple((uint32_t)base, (uint32_t)n);
}

// Lazy generator expression: `(expr for x in xs (if cond)* (for y in ys ...)*)`.
// Synthesises a generator function bound to a fresh scope.  The OUTERMOST
// iterable is parsed in the parent scope (eagerly evaluated when the
// genexp value is constructed); everything else lives in the synth scope.
static int genexp_fn_uid = 0;
static NODE *
parse_genexp_lazy(int saved_remap_at_paren)
{
    Scope *parent_scope = cur_scope;
    Scope sc = {0};
    sc.parent = parent_scope;
    sc.is_generator = true;
    cur_scope = &sc;
    // The parent scope hosts a synthesised generator function (the
    // genexp).  That generator escapes the parent's call (callers can
    // hold the genexp value), so the parent's frame must live on the
    // heap, not alloca.  Pre-scan token-walk doesn't see synthesised
    // defs, so flip has_nested_def explicitly here.
    if (parent_scope) parent_scope->has_nested_def = true;

    // Slot 0: synthesised parameter ".0" — receives iter(OUTER_ITER).
    const char *p0 = intern_name(".0", 2);
    int p0_slot = scope_add_local(&sc, p0);
    (void)p0_slot;

    // prescan auto-allocates `__cmp$N$x` style synth names as locals in
    // the (currently active) synth scope.
    prescan_comp_targets(T_RPAREN);

    NODE *first = parse_expr();
    if (peek_tok(0)->kind != T_FOR) {
        // Should not happen — caller verified genexp lookahead.
        cur_scope = parent_scope;
        comp_remap_top = saved_remap_at_paren;
        return first;
    }

    expect(T_FOR, "'for'");
    bool paren_target = match_tok(T_LPAREN);
    if (peek_tok(0)->kind != T_NAME)
        parse_error("expected target NAME in genexp");
    const char *first_var = comp_resolve(peek_tok(0)->sval);
    tok_pos++;
    // Tuple targets: collect names.  `for a, b in xs` → unpack.
    const char *first_extra[16]; int n_first_extra = 0;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind != T_NAME) break;
        if (n_first_extra >= 16) parse_error("tuple target too long");
        first_extra[n_first_extra++] = comp_resolve(peek_tok(0)->sval);
        tok_pos++;
    }
    if (paren_target) expect(T_RPAREN, "')'");
    expect(T_IN, "'in'");

    // OUTER iterable: parse in parent scope.
    Scope *saved = cur_scope;
    cur_scope = parent_scope;
    NODE *outer_iter = parse_or();
    cur_scope = saved;

    // Build inner_body = yield first.
    NODE *body = ALLOC_node_yield(first);

    // Trailing if-conditions on the outermost for-clause.
    while (peek_tok(0)->kind == T_IF) {
        tok_pos++;
        NODE *cond = parse_or();
        body = ALLOC_node_if(cond, body, ALLOC_node_nop());
    }

    // Inner `for` clauses — parse_comp_clauses handles them in synth
    // scope (their iterables are evaluated lazily inside the generator).
    if (peek_tok(0)->kind == T_FOR) {
        body = parse_comp_clauses(body);
    }

    // Caller (parse_paren_or_tuple or parse_call_args) consumes the ')'.
    comp_remap_top = saved_remap_at_paren;

    // Build the outer for-loop: `for first_var in .0: body`.
    NODE *load_p0 = ALLOC_node_lref(0);
    NODE *outer_loop;
    if (n_first_extra == 0) {
        outer_loop = build_for_loop(first_var, load_p0, body);
    } else {
        // Tuple target — desugar to `tmp = item; first_var = tmp[0]; ...`.
        const char *tmp = new_temp_name("__forT");
        int tmp_slot = scope_add_local(&sc, tmp);
        NODE *prefix = NULL;
        const char *all_names[17];
        all_names[0] = first_var;
        for (int i = 0; i < n_first_extra; i++) all_names[i+1] = first_extra[i];
        for (int i = n_first_extra; i >= 0; i--) {
            NODE *idx_n = ALLOC_node_const_int(i);
            NODE *load_tmp = ALLOC_node_lref((uint32_t)tmp_slot);
            NODE *el = ALLOC_node_subscript_get(load_tmp, idx_n);
            int s = scope_add_local(&sc, all_names[i]);
            NODE *st = ALLOC_node_lset((uint32_t)s, el);
            prefix = prefix ? ALLOC_node_seq(st, prefix) : st;
        }
        NODE *combined = ALLOC_node_seq(prefix, body);
        outer_loop = ALLOC_node_for_local((uint32_t)tmp_slot, load_p0, combined,
                                           ALLOC_node_nop());
    }

    // Reserve the param-name table.
    const char *param_names[1] = { p0 };
    uint32_t nidx = (uint32_t)name_table_reserve(param_names, 1);
    uint32_t didx = (uint32_t)defaults_reserve(NULL, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "__genexp$%d__", genexp_fn_uid++);
    const char *fname = intern_name(buf, strlen(buf));

    // is_generator=1, leaf=0 (yield is non-leaf).
    NODE *def_node = ALLOC_node_def(fname, 1, 1,
                                    (uint32_t)sc.nlocals,
                                    0, didx,
                                    0,                       // leaf
                                    nidx, 0,                 // flags
                                    1,                       // is_gen
                                    outer_loop);

    // Restore parent scope for the call site.
    cur_scope = parent_scope;

    NODE *iter_call = ALLOC_node_call_1(
        ALLOC_node_gref(intern_name("iter", 4)), outer_iter);
    return ALLOC_node_call_1(def_node, iter_call);
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
// tuple — only NAME for now).  Comprehensions have no `else` clause.
static NODE *
build_for_loop(const char *target_name, NODE *iter, NODE *body)
{
    NODE *no_else = ALLOC_node_nop();
    if (cur_scope && !scope_is_global_decl(cur_scope, target_name)) {
        int idx = scope_add_local(cur_scope, target_name);
        return ALLOC_node_for_local((uint32_t)idx, iter, body, no_else);
    }
    return ALLOC_node_for_global(target_name, iter, body, no_else);
}

// Parse one or more `for X (, Y)* in EXPR (if EXPR)*` clauses and
// build the nested loop.  `inner_body` is what the innermost loop body
// produces.  Returns the outermost for_loop node.  Caller positions
// tok at `for`.  Tuple targets desugar to `__t = item; X = __t[0]; Y =
// __t[1]; ...`.
static NODE *
parse_comp_clauses(NODE *inner_body)
{
    // `[expr async for x in xs]` — pystro treats async-for as plain
    // for in comprehensions.
    if (peek_tok(0)->kind == T_NAME
            && peek_tok(0)->sval == intern_name("async", 5)
            && peek_tok(1)->kind == T_FOR) {
        tok_pos++;
    }
    expect(T_FOR, "'for'");
    // Accept `for X in ...`, `for X, Y in ...`, or `for (X, Y) in ...`.
    bool paren_target = match_tok(T_LPAREN);
    if (peek_tok(0)->kind != T_NAME) parse_error("expected target NAME in comprehension");
    const char *names[16];
    int nnames = 0;
    names[nnames++] = comp_resolve(peek_tok(0)->sval);
    tok_pos++;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME in tuple target");
        if (nnames >= 16) parse_error("for tuple target too long");
        names[nnames++] = comp_resolve(peek_tok(0)->sval);
        tok_pos++;
    }
    if (paren_target) expect(T_RPAREN, "')'");
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
        NODE *no_else = ALLOC_node_nop();
        return ALLOC_node_for_local((uint32_t)idx, iter, new_body, no_else);
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
    NODE *no_else2 = ALLOC_node_nop();
    return ALLOC_node_for_global(tmp, iter, new_body, no_else2);
}

static NODE *
parse_list_literal(void)
{
    expect(T_LBRACK, "'['");
    if (match_tok(T_RBRACK)) {
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_list((uint32_t)base, 0);
    }
    // Pre-scan for any `*expr` spread.  If found, desugar to
    // append/extend on a temp list.
    bool has_spread = false;
    {
        size_t save = tok_pos;
        int depth = 0;
        while (peek_tok(0)->kind != T_EOF) {
            int k = peek_tok(0)->kind;
            if (k == T_LBRACK || k == T_LPAREN || k == T_LBRACE) depth++;
            else if (k == T_RBRACK) {
                if (depth == 0) break;
                depth--;
            } else if (k == T_RPAREN || k == T_RBRACE) depth--;
            else if (k == T_STAR && depth == 0) {
                int p = (tok_pos > 0) ? tok_arr[tok_pos - 1].kind : T_NEWLINE;
                if (p == T_LBRACK || p == T_COMMA) { has_spread = true; break; }
            }
            tok_pos++;
        }
        tok_pos = save;
    }
    if (has_spread) {
        const char *tmp = new_temp_name("__lc");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_list((uint32_t)empty_idx, 0), &load_tmp);
        NODE *result = init;
        for (;;) {
            if (peek_tok(0)->kind == T_RBRACK) break;
            if (match_tok(T_STAR)) {
                NODE *e = parse_expr();
                NODE *call = ALLOC_node_method_1(load_tmp, intern_name("extend", 6), e);
                result = ALLOC_node_seq(result, call);
            } else {
                NODE *e = parse_expr();
                NODE *call = ALLOC_node_method_1(load_tmp, intern_name("append", 6), e);
                result = ALLOC_node_seq(result, call);
            }
            if (!match_tok(T_COMMA)) break;
        }
        expect(T_RBRACK, "']'");
        return ALLOC_node_seq(result, load_tmp);
    }
    int saved_remap_lc = comp_remap_top;
    prescan_comp_targets(T_RBRACK);
    NODE *first = parse_expr();
    bool comp_async_prefix = (peek_tok(0)->kind == T_NAME
            && peek_tok(0)->sval == intern_name("async", 5)
            && peek_tok(1)->kind == T_FOR);
    if (peek_tok(0)->kind == T_FOR || comp_async_prefix) {
        if (comp_async_prefix) tok_pos++;
        // List comprehension: [expr (async)? for x in xs (if cond)*]+
        const char *tmp = new_temp_name("__lc");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_list((uint32_t)empty_idx, 0), &load_tmp);
        NODE *append = ALLOC_node_method_1(load_tmp, intern_name("append", 6), first);
        NODE *loops = parse_comp_clauses(append);
        expect(T_RBRACK, "']'");
        comp_remap_top = saved_remap_lc;
        return ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
    }
    comp_remap_top = saved_remap_lc;
    NODE *items[2048];
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
parse_dict_or_set_literal(void)
{
    expect(T_LBRACE, "'{'");
    if (match_tok(T_RBRACE)) {
        // {} is an empty dict in Python — there is no empty set literal.
        size_t base = node_table_reserve(NULL, 0);
        return ALLOC_node_make_dict((uint32_t)base, 0);
    }
    // Pre-scan for spread (`**d` or `*s`) anywhere within the literal.
    // If found, decide dict vs set by whether any unspread item has `:`.
    bool has_dspread = false, has_sspread = false, has_pair = false;
    {
        size_t save = tok_pos;
        int depth = 0;
        while (peek_tok(0)->kind != T_EOF) {
            int k = peek_tok(0)->kind;
            if (k == T_LBRACE || k == T_LBRACK || k == T_LPAREN) depth++;
            else if (k == T_RBRACE) {
                if (depth == 0) break;
                depth--;
            } else if (k == T_RBRACK || k == T_RPAREN) depth--;
            else if (depth == 0) {
                int p = (tok_pos > 0) ? tok_arr[tok_pos - 1].kind : T_NEWLINE;
                if (k == T_STAR_STAR && (p == T_LBRACE || p == T_COMMA)) has_dspread = true;
                else if (k == T_STAR && (p == T_LBRACE || p == T_COMMA)) has_sspread = true;
                else if (k == T_COLON) has_pair = true;
            }
            tok_pos++;
        }
        tok_pos = save;
    }
    if (has_dspread || (has_sspread && has_pair)) {
        // Dict literal with spread.
        const char *tmp = new_temp_name("__ds");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_dict((uint32_t)empty_idx, 0), &load_tmp);
        NODE *result = init;
        for (;;) {
            if (peek_tok(0)->kind == T_RBRACE) break;
            if (match_tok(T_STAR_STAR)) {
                NODE *e = parse_expr();
                NODE *call = ALLOC_node_method_1(load_tmp, intern_name("update", 6), e);
                result = ALLOC_node_seq(result, call);
            } else {
                NODE *kk = parse_expr();
                expect(T_COLON, "':'");
                NODE *vv = parse_expr();
                NODE *set_n = ALLOC_node_subscript_set(load_tmp, kk, vv);
                result = ALLOC_node_seq(result, set_n);
            }
            if (!match_tok(T_COMMA)) break;
        }
        expect(T_RBRACE, "'}'");
        return ALLOC_node_seq(result, load_tmp);
    }
    if (has_sspread) {
        // Set literal with spread.
        const char *tmp = new_temp_name("__ss");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_set((uint32_t)empty_idx, 0), &load_tmp);
        NODE *result = init;
        for (;;) {
            if (peek_tok(0)->kind == T_RBRACE) break;
            if (match_tok(T_STAR)) {
                NODE *e = parse_expr();
                NODE *call = ALLOC_node_method_1(load_tmp, intern_name("update", 6), e);
                result = ALLOC_node_seq(result, call);
            } else {
                NODE *e = parse_expr();
                NODE *call = ALLOC_node_method_1(load_tmp, intern_name("add", 3), e);
                result = ALLOC_node_seq(result, call);
            }
            if (!match_tok(T_COMMA)) break;
        }
        expect(T_RBRACE, "'}'");
        return ALLOC_node_seq(result, load_tmp);
    }
    int saved_remap_dc = comp_remap_top;
    prescan_comp_targets(T_RBRACE);
    NODE *first = parse_expr();
    // dict literal / dict comprehension if next is `:`
    if (peek_tok(0)->kind == T_COLON) {
        tok_pos++;
        NODE *first_v = parse_expr();
        if (peek_tok(0)->kind == T_FOR) {
            const char *tmp = new_temp_name("__dc");
            NODE *load_tmp;
            size_t empty_idx = node_table_reserve(NULL, 0);
            NODE *init = build_temp_init(tmp, ALLOC_node_make_dict((uint32_t)empty_idx, 0), &load_tmp);
            NODE *set_n = ALLOC_node_subscript_set(load_tmp, first, first_v);
            NODE *loops = parse_comp_clauses(set_n);
            expect(T_RBRACE, "'}'");
            comp_remap_top = saved_remap_dc;
            return ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
        }
        comp_remap_top = saved_remap_dc;
        NODE *items[8192];
        int npairs = 0;
        items[0] = first; items[1] = first_v; npairs = 1;
        while (match_tok(T_COMMA)) {
            if (peek_tok(0)->kind == T_RBRACE) break;
            NODE *k = parse_expr();
            expect(T_COLON, "':'");
            NODE *v = parse_expr();
            if (npairs * 2 + 2 > 8192) parse_error("dict literal too long");
            items[npairs * 2] = k;
            items[npairs * 2 + 1] = v;
            npairs++;
        }
        expect(T_RBRACE, "'}'");
        size_t base = node_table_reserve(items, npairs * 2);
        return ALLOC_node_make_dict((uint32_t)base, (uint32_t)npairs);
    }
    // Set literal / set comprehension.
    if (peek_tok(0)->kind == T_FOR) {
        const char *tmp = new_temp_name("__sc");
        NODE *load_tmp;
        size_t empty_idx = node_table_reserve(NULL, 0);
        NODE *init = build_temp_init(tmp, ALLOC_node_make_set((uint32_t)empty_idx, 0), &load_tmp);
        NODE *add = ALLOC_node_method_1(load_tmp, intern_name("add", 3), first);
        NODE *loops = parse_comp_clauses(add);
        expect(T_RBRACE, "'}'");
        comp_remap_top = saved_remap_dc;
        return ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
    }
    comp_remap_top = saved_remap_dc;
    NODE *items[2048];
    int n = 0; items[n++] = first;
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_RBRACE) break;
        if (n >= 256) parse_error("set literal too long");
        items[n++] = parse_expr();
    }
    expect(T_RBRACE, "'}'");
    size_t base = node_table_reserve(items, n);
    return ALLOC_node_make_set((uint32_t)base, (uint32_t)n);
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

    bool has_va = false, has_kw = false;
    int n_pos_named = 0;
    bool saw_star = false;
    if (peek_tok(0)->kind != T_COLON) {
        for (;;) {
            // *args
            if (match_tok(T_STAR)) {
                if (peek_tok(0)->kind == T_COMMA || peek_tok(0)->kind == T_COLON) {
                    saw_star = true;
                    if (!match_tok(T_COMMA)) break;
                    continue;
                }
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '*'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                scope_add_local(&sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_va = true;
                saw_star = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            // **kwargs
            if (match_tok(T_STAR_STAR)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '**'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                scope_add_local(&sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_kw = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            if (peek_tok(0)->kind != T_NAME) parse_error("expected parameter");
            const char *pn = peek_tok(0)->sval;
            tok_pos++;
            scope_add_local(&sc, pn);
            names[nnames++] = pn;
            int slot = nparams;
            nparams++;
            if (!saw_star) n_pos_named++;
            if (match_tok(T_ASSIGN)) {
                seen_default = true;
                if (ndefaults >= 16) parse_error("too many defaults");
                // Default values are evaluated in the *enclosing* scope,
                // not the lambda's.
                defs[ndefaults].slot = slot;
                defs[ndefaults].expr = parse_expr();
                ndefaults++;
            } else if (seen_default) {
                parse_error("non-default after default");
            }
            if (!match_tok(T_COMMA)) break;
        }
    }
    expect(T_COLON, "':'");

    Scope *saved = cur_scope;
    cur_scope = &sc;
    // Pre-scan: if body contains a nested lambda or def, this lambda
    // can't use an alloca'd frame (the inner would capture our dead
    // stack memory after we return).  Also detect a `yield` token,
    // which makes this lambda a generator (`lambda: (yield x)`).
    {
        size_t p = tok_pos;
        int depth = 0;
        while (tok_arr[p].kind != T_EOF) {
            int k = tok_arr[p].kind;
            if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
            else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) {
                if (depth == 0) break;
                depth--;
            } else if (k == T_NEWLINE || k == T_SEMI || (depth == 0 && k == T_COMMA))
                break;
            else if (k == T_LAMBDA || k == T_DEF) {
                sc.has_nested_def = true;
            }
            else if (k == T_YIELD) sc.is_generator = true;
            p++;
        }
    }
    NODE *body_expr = parse_expr();
    cur_scope = saved;

    NODE *body = ALLOC_node_return(body_expr);
    size_t didx = defaults_reserve(defs, ndefaults);
    size_t nidx = name_table_reserve(names, nnames);
    uint32_t leaf_bit = (sc.has_nested_def || sc.is_generator) ? 0u : 1u;
    uint32_t leaf_flags = leaf_bit
        | (sc.is_generator ? 2u : 0u)
        | (has_va ? 4u : 0u)
        | (has_kw ? 8u : 0u)
        | ((uint32_t)(n_pos_named & 0xFF) << 16);
    return ALLOC_node_lambda((uint32_t)nparams, (uint32_t)sc.nlocals,
                             (uint32_t)ndefaults, (uint32_t)didx,
                             leaf_flags, (uint32_t)nidx, body);
}

static NODE *
parse_atom(void)
{
    Tok *t = peek_tok(0);
    // `await EXPR` — pystro has no real coroutine model, so we treat
    // `await x` as just `x` (the value is whatever the call returned).
    // This allows `async def f(): await something()` to run as plain
    // synchronous code.
    if (t->kind == T_NAME && t->sval == intern_name("await", 5)
            && peek_tok(1)->kind != T_LPAREN
            && peek_tok(1)->kind != T_DOT
            && peek_tok(1)->kind != T_COMMA
            && peek_tok(1)->kind != T_RPAREN
            && peek_tok(1)->kind != T_ASSIGN
            && peek_tok(1)->kind != T_NEWLINE) {
        tok_pos++;
        return parse_atom();
    }
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
      case T_IMAG: {
        tok_pos++;
        union { uint64_t u; double d; } pun = { .d = t->fval };
        return ALLOC_node_const_imag(pun.u);
      }
      case T_STR: {
        tok_pos++;
        // Implicit concatenation of adjacent string literals — possibly
        // mixing plain and f-string forms, e.g.  "hi " f"{name}" "!".
        // Plain runs are concatenated into one literal; f-strings join
        // via runtime + at the AST level.
        size_t total = strlen(t->sval);
        char buf[65536];
        size_t bl = 0;
        if (total + 1 > sizeof(buf)) parse_error("implicit concat string too long");
        memcpy(buf, t->sval, total); bl = total;
        while (peek_tok(0)->kind == T_STR) {
            const char *s = peek_tok(0)->sval;
            size_t l = strlen(s);
            if (bl + l + 1 > sizeof(buf)) parse_error("implicit concat string too long");
            memcpy(buf + bl, s, l); bl += l;
            tok_pos++;
        }
        buf[bl] = '\0';
        NODE *result = ALLOC_node_const_str(intern_name(buf, bl));
        while (peek_tok(0)->kind == T_FSTR || peek_tok(0)->kind == T_STR) {
            if (peek_tok(0)->kind == T_FSTR) {
                Tok *ft = peek_tok(0);
                tok_pos++;
                NODE *fnode = parse_fstring(ft);
                result = ALLOC_node_add(result, fnode);
            } else {
                size_t bl2 = 0;
                while (peek_tok(0)->kind == T_STR) {
                    const char *s = peek_tok(0)->sval;
                    size_t l = strlen(s);
                    if (bl2 + l + 1 > sizeof(buf)) parse_error("implicit concat string too long");
                    memcpy(buf + bl2, s, l); bl2 += l;
                    tok_pos++;
                }
                buf[bl2] = '\0';
                result = ALLOC_node_add(result, ALLOC_node_const_str(intern_name(buf, bl2)));
            }
        }
        return result;
      }
      case T_BYTES: {
        tok_pos++;
        // Implicit concat of bytes literals.
        if (peek_tok(0)->kind != T_BYTES)
            return ALLOC_node_const_bytes(t->sval, (uint32_t)t->slen);
        char buf[65536];
        size_t bl = 0;
        if ((size_t)t->slen > sizeof(buf)) parse_error("bytes concat too long");
        memcpy(buf, t->sval, t->slen); bl = t->slen;
        while (peek_tok(0)->kind == T_BYTES) {
            const Tok *tt = peek_tok(0);
            if (bl + tt->slen > sizeof(buf)) parse_error("bytes concat too long");
            memcpy(buf + bl, tt->sval, tt->slen); bl += tt->slen;
            tok_pos++;
        }
        return ALLOC_node_const_bytes(intern_name(buf, bl), (uint32_t)bl);
      }
      case T_FSTR: {
        tok_pos++;
        NODE *result = parse_fstring(t);
        // Implicit concatenation: f"..." f"..." or f"..." "..."
        while (peek_tok(0)->kind == T_FSTR || peek_tok(0)->kind == T_STR) {
            if (peek_tok(0)->kind == T_FSTR) {
                Tok *ft = peek_tok(0);
                tok_pos++;
                result = ALLOC_node_add(result, parse_fstring(ft));
            } else {
                char buf[65536];
                size_t bl = 0;
                while (peek_tok(0)->kind == T_STR) {
                    const char *s = peek_tok(0)->sval;
                    size_t l = strlen(s);
                    if (bl + l + 1 > sizeof(buf)) parse_error("implicit concat string too long");
                    memcpy(buf + bl, s, l); bl += l;
                    tok_pos++;
                }
                buf[bl] = '\0';
                result = ALLOC_node_add(result, ALLOC_node_const_str(intern_name(buf, bl)));
            }
        }
        return result;
      }
      case T_TRUE:  tok_pos++; return ALLOC_node_const_true();
      case T_FALSE: tok_pos++; return ALLOC_node_const_false();
      case T_NONE:  tok_pos++; return ALLOC_node_const_none();
      case T_DOT:
        // `...` ellipsis literal: three dots.
        if (peek_tok(1)->kind == T_DOT && peek_tok(2)->kind == T_DOT) {
            tok_pos += 3;
            return ALLOC_node_gref(intern_name("Ellipsis", 8));
        }
        parse_error("unexpected '.'");
      case T_LPAREN: return parse_paren_or_tuple();
      case T_LBRACK: return parse_list_literal();
      case T_LBRACE: return parse_dict_or_set_literal();
      case T_LAMBDA: return parse_lambda();
      case T_NAME: { tok_pos++; return make_load(t->sval); }
      default:
        {
            extern const char *tok_kind_name(int k);
            parse_error("unexpected token in expression: %s", tok_kind_name(t->kind));
        }
    }
}

static NODE *
parse_call_args(NODE *fn)
{
    expect(T_LPAREN, "'('");
    NODE *args[64];
    int argc = 0;
    struct pykwarg kws[256];
    int kwc = 0;
    struct pyspread_arg spreads[256];
    int nspreads = 0;
    bool has_spread = false;
    if (peek_tok(0)->kind != T_RPAREN) {
        for (;;) {
            int k = peek_tok(0)->kind;
            if (k == T_STAR_STAR) {
                tok_pos++;
                NODE *e = parse_expr();
                spreads[nspreads].kind = 3; spreads[nspreads].name = NULL; spreads[nspreads].node = e;
                nspreads++;
                has_spread = true;
            } else if (k == T_STAR) {
                tok_pos++;
                NODE *e = parse_expr();
                spreads[nspreads].kind = 1; spreads[nspreads].name = NULL; spreads[nspreads].node = e;
                nspreads++;
                has_spread = true;
            } else if (k == T_NAME && peek_tok(1)->kind == T_ASSIGN) {
                const char *nm = peek_tok(0)->sval;
                tok_pos += 2;
                NODE *e = parse_expr();
                if (kwc >= 256) parse_error("too many kwargs");
                kws[kwc].name = nm; kws[kwc].value = e; kwc++;
                spreads[nspreads].kind = 2; spreads[nspreads].name = nm; spreads[nspreads].node = e;
                nspreads++;
            } else {
                // `f(expr for x in xs)` — implicit lazy gen-expression
                // as the sole argument.  Lookahead for top-level `for`
                // before `)` to detect; if found, take the same lazy
                // synth-def path used by `(expr for x in xs)`.
                int saved_remap_ge = comp_remap_top;
                NODE *e;
                bool inline_genexp = false;
                if (argc == 0 && nspreads == 0) {
                    size_t look = tok_pos;
                    int depth = 0;
                    while (tok_arr[look].kind != T_EOF) {
                        int kk = tok_arr[look].kind;
                        if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) depth++;
                        else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) {
                            if (depth == 0) break;
                            depth--;
                        }
                        else if (depth == 0 && kk == T_COMMA) break;  // not a sole arg
                        else if (depth == 0 && kk == T_FOR) { inline_genexp = true; break; }
                        look++;
                    }
                }
                if (inline_genexp) {
                    e = parse_genexp_lazy(saved_remap_ge);
                } else {
                    e = parse_expr();
                }
                comp_remap_top = saved_remap_ge;
                if (argc >= 64) parse_error("too many args");
                args[argc++] = e;
                spreads[nspreads].kind = 0; spreads[nspreads].name = NULL; spreads[nspreads].node = e;
                nspreads++;
            }
            if (!match_tok(T_COMMA)) break;
            if (peek_tok(0)->kind == T_RPAREN) break;
        }
    }
    expect(T_RPAREN, "')'");
    if (has_spread) {
        size_t base = spreads_reserve(spreads, nspreads);
        return ALLOC_node_call_spread(fn, (uint32_t)base, (uint32_t)nspreads);
    }
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
    // `obj[a, b, c]` — tuple subscript (used by typing generic aliases).
    if (!is_slice && peek_tok(0)->kind == T_COMMA) {
        NODE *items[64];
        int n = 0;
        items[n++] = start;
        while (match_tok(T_COMMA)) {
            if (peek_tok(0)->kind == T_RBRACK) break;
            if (n >= 16) parse_error("too many tuple items in subscript");
            items[n++] = parse_expr();
        }
        size_t base = node_table_reserve(items, n);
        start = ALLOC_node_make_tuple((uint32_t)base, (uint32_t)n);
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
        // method call.  Look ahead for `NAME = expr` (kwarg) or `*` /
        // `**` spreads to decide between fast-path method_n and full
        // call_kw via attr_get.
        size_t save = tok_pos;
        tok_pos++;        // consume '('
        bool has_kwarg = false;
        bool has_spread = false;
        {
            int depth = 1;
            for (size_t p = tok_pos; depth > 0; p++) {
                int k = tok_arr[p].kind;
                if (k == T_EOF || k == T_NEWLINE) break;
                if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
                else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) {
                    depth--;
                    if (depth == 0) break;
                }
                else if (depth == 1 && k == T_NAME &&
                         tok_arr[p+1].kind == T_ASSIGN) {
                    has_kwarg = true; break;
                }
                else if (depth == 1 && (k == T_STAR || k == T_STAR_STAR)) {
                    // Leading * / ** at top level of arg list.
                    has_spread = true;
                }
            }
        }
        if (has_kwarg || has_spread) {
            // Desugar `obj.m(args, kw=val)` to `(obj.m)(args, kw=val)`
            // by reusing parse_call_args on the attribute-get node.
            tok_pos = save;       // back up to '(' so parse_call_args consumes it
            NODE *attr = ALLOC_node_attr_get(obj, name);
            return parse_call_args(attr);
        }
        NODE *args[64];
        int argc = 0;
        if (peek_tok(0)->kind != T_RPAREN) {
            for (;;) {
                if (argc >= 64) parse_error("too many args");
                int saved_remap_mge = comp_remap_top;
                if (argc == 0) prescan_comp_targets(T_RPAREN);
                NODE *e = parse_expr();
                // Implicit generator-expression as sole argument.
                if (peek_tok(0)->kind == T_FOR && argc == 0) {
                    const char *tmp = new_temp_name("__ge");
                    NODE *load_tmp;
                    size_t empty_idx = node_table_reserve(NULL, 0);
                    NODE *init = build_temp_init(tmp, ALLOC_node_make_list((uint32_t)empty_idx, 0), &load_tmp);
                    NODE *append = ALLOC_node_method_1(load_tmp, intern_name("append", 6), e);
                    NODE *loops = parse_comp_clauses(append);
                    e = ALLOC_node_seq(init, ALLOC_node_seq(loops, load_tmp));
                }
                comp_remap_top = saved_remap_mge;
                args[argc++] = e;
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
    // `super()` and `super(C, self)` short-circuit.
    if (peek_tok(0)->kind == T_NAME && peek_tok(0)->sval == intern_name("super", 5)
            && peek_tok(1)->kind == T_LPAREN) {
        // Distinguish bare super() vs super(C, self).
        bool bare = (peek_tok(2)->kind == T_RPAREN);
        NODE *cls_expr = NULL, *self_expr = NULL;
        if (bare) {
            tok_pos += 3;        // past `super` `(` `)`
        } else {
            tok_pos += 2;        // past `super` `(`
            cls_expr = parse_expr();
            expect(T_COMMA, "','");
            self_expr = parse_expr();
            expect(T_RPAREN, "')'");
        }
        // If NOT followed by `.METHOD(args)`, return a super proxy
        // value that the user can bind to a variable.
        if (peek_tok(0)->kind != T_DOT) {
            if (bare) return ALLOC_node_super_obj();
            return ALLOC_node_super_obj_explicit(cls_expr, self_expr);
        }
        tok_pos++;
        if (peek_tok(0)->kind != T_NAME) parse_error("expected method name after super().");
        const char *method = peek_tok(0)->sval;
        tok_pos++;
        // `super().attr` (no call) — return as attribute-get on the
        // super proxy, then continue with any postfix trailers.
        if (peek_tok(0)->kind != T_LPAREN) {
            NODE *super_obj;
            if (bare) {
                if (!cur_class_base) parse_error("super() called outside a class body");
                super_obj = ALLOC_node_super_obj();
            } else {
                super_obj = ALLOC_node_super_obj_explicit(cls_expr, self_expr);
            }
            NODE *e = ALLOC_node_attr_get(super_obj, method);
            for (;;) {
                int k = peek_tok(0)->kind;
                if (k == T_LPAREN) e = parse_call_args(e);
                else if (k == T_LBRACK) e = parse_subscript(e);
                else if (k == T_DOT)    e = parse_dot_trailer(e);
                else break;
            }
            return e;
        }
        // Lookahead inside the call args for *spread or **kwarg; if either,
        // fall back to attribute-get + parse_call_args (full kwarg support).
        size_t paren_save = tok_pos;
        bool has_spread_or_kw = false;
        if (peek_tok(0)->kind == T_LPAREN) {
            int depth = 0;
            size_t p = tok_pos;
            while (tok_arr[p].kind != T_EOF && tok_arr[p].kind != T_NEWLINE) {
                int kk = tok_arr[p].kind;
                if (kk == T_LPAREN) depth++;
                else if (kk == T_RPAREN) {
                    depth--;
                    if (depth == 0) break;
                } else if (depth == 1 && (kk == T_STAR || kk == T_STAR_STAR)) {
                    has_spread_or_kw = true; break;
                } else if (depth == 1 && kk == T_NAME && tok_arr[p+1].kind == T_ASSIGN) {
                    has_spread_or_kw = true; break;
                }
                p++;
            }
        }
        if (has_spread_or_kw) {
            // Build super().attr expression, then parse_call_args.
            NODE *super_obj;
            if (bare) {
                if (!cur_class_base) parse_error("super() called outside a class body");
                super_obj = ALLOC_node_super_obj();
            } else {
                super_obj = ALLOC_node_super_obj_explicit(cls_expr, self_expr);
            }
            NODE *attr = ALLOC_node_attr_get(super_obj, method);
            tok_pos = paren_save;  // back to '('
            NODE *e = parse_call_args(attr);
            for (;;) {
                int k = peek_tok(0)->kind;
                if (k == T_LPAREN) e = parse_call_args(e);
                else if (k == T_LBRACK) e = parse_subscript(e);
                else if (k == T_DOT)    e = parse_dot_trailer(e);
                else break;
            }
            return e;
        }
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
        NODE *e;
        if (bare) {
            if (!cur_class_base) parse_error("super() called outside a class body");
            e = ALLOC_node_super_method(cur_class_base, method, (uint32_t)base, (uint32_t)argc);
        } else {
            e = ALLOC_node_super_method_explicit(cls_expr, self_expr, method,
                                                 (uint32_t)base, (uint32_t)argc);
        }
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
    if (match_tok(T_PLUS))  {
        NODE *e = parse_factor();
        NODE *args[1] = { e };
        size_t bidx = node_table_reserve(args, 1);
        return ALLOC_node_call_n(
            ALLOC_node_gref(intern_name("__pystro_pos__", 14)),
            (uint32_t)bidx, 1);
    }
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
        else if (match_tok(T_AT))           l = ALLOC_node_matmul(l, parse_factor());
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

// Comparison with chaining: `a < b < c` → `(a < b) and (b < c)` with
// `b` bound to a hidden temp so it evaluates exactly once (matching
// Python semantics).  The first and last operands always appear once,
// only the middle ones get temped.
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

        // If another comparison op follows, this `r` is the middle of
        // a chain — bind it to a temp so both uses share one eval.
        int nk = peek_tok(0)->kind;
        bool another = (nk == T_LT || nk == T_LE || nk == T_GT ||
                        nk == T_GE || nk == T_EQ || nk == T_NE ||
                        nk == T_IS || nk == T_IN ||
                        (nk == T_NOT && peek_tok(1)->kind == T_IN));
        if (another) {
            const char *tmp = new_temp_name("__cc");
            NODE *load_tmp;
            NODE *init = build_temp_init(tmp, r, &load_tmp);
            // r-value = seq(init, load_tmp) — eval-and-bind, returns load
            NODE *r_once = ALLOC_node_seq(init, load_tmp);
            NODE *cmp = op(l, r_once);
            result = result ? ALLOC_node_and(result, cmp) : cmp;
            l = load_tmp;       // re-read of the bound temp for next op
        } else {
            NODE *cmp = op(l, r);
            result = result ? ALLOC_node_and(result, cmp) : cmp;
            l = r;
        }
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

// Walrus: NAME := expr.  Recognised as a primary alternative to a
// regular expression when the lookahead is `NAME :=`.  The result
// expression is the assigned value; the binding goes to the local /
// global the name resolves to.
static NODE *parse_yield(void);

static NODE *
parse_walrus(void)
{
    // `yield` / `yield expr` as an expression — most common in
    // `x = yield ...`.  parse_yield returns a node whose runtime value
    // is the .send() argument (PY_NONE for plain next()).
    if (peek_tok(0)->kind == T_YIELD) return parse_yield();
    if (peek_tok(0)->kind == T_NAME && peek_tok(1)->kind == T_WALRUS) {
        const char *nm = peek_tok(0)->sval;
        tok_pos += 2;
        // Walrus binds in the enclosing function/lambda scope.  Register
        // the name as a local of cur_scope so make_store emits an lset.
        if (cur_scope && !scope_is_global_decl(cur_scope, nm) &&
            !scope_is_nonlocal_decl(cur_scope, nm))
            scope_add_local(cur_scope, nm);
        NODE *val = parse_walrus();
        const char *tmp = new_temp_name("__wal");
        NODE *load_tmp;
        NODE *init = build_temp_init(tmp, val, &load_tmp);
        NODE *store_nm = make_store(nm, load_tmp);
        return ALLOC_node_seq(init, ALLOC_node_seq(store_nm, load_tmp));
    }
    return parse_cond();
}

static NODE *
parse_expr(void) { return parse_walrus(); }

// expr_list: expr (',' expr)+ → tuple; single → expr.
// Trailing comma creates a 1-tuple (`x,` -> (x,)).
static NODE *
parse_expr_list(void)
{
    NODE *first = parse_expr();
    if (peek_tok(0)->kind != T_COMMA) return first;
    NODE *items[1024];
    int n = 0; items[n++] = first;
    bool saw_trailing_comma = false;
    while (match_tok(T_COMMA)) {
        int k = peek_tok(0)->kind;
        if (k == T_NEWLINE || k == T_RPAREN || k == T_RBRACK || k == T_RBRACE
                || k == T_COLON || k == T_ASSIGN || k == T_SEMI) {
            saw_trailing_comma = true;
            break;
        }
        if (n >= 1024) parse_error("expr list too long");
        items[n++] = parse_expr();
    }
    if (n == 1 && !saw_trailing_comma) return items[0];
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
    // Inline suite: simple_stmt (';' simple_stmt)* NEWLINE
    NODE *s = parse_simple_stmt();
    while (match_tok(T_SEMI)) {
        if (peek_tok(0)->kind == T_NEWLINE) break;
        NODE *more = parse_simple_stmt();
        s = ALLOC_node_seq(s, more);
    }
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
    NODE *e = match_tok(T_ELSE) ? parse_suite() : ALLOC_node_nop();
    return ALLOC_node_while(c, b, e);
}

// Helper: recursively build assignments for `target = src`.
// target is parsed at the current tok_pos; on return tok_pos has advanced.
// Names are registered as locals in cur_scope.  Supports nested
// (a, (b, c), d) forms.
static NODE *
build_for_target_assigns(NODE *src)
{
    int k = peek_tok(0)->kind;
    if (k == T_NAME) {
        const char *nm = peek_tok(0)->sval;
        tok_pos++;
        if (cur_scope && !scope_is_global_decl(cur_scope, nm))
            scope_add_local(cur_scope, nm);
        return make_store(nm, src);
    }
    if (k == T_LPAREN || k == T_LBRACK) {
        int close = (k == T_LPAREN) ? T_RPAREN : T_RBRACK;
        tok_pos++;
        // src must be subscripted by index for each child target.
        // Use a temp so src isn't evaluated multiple times.
        const char *tmp = new_temp_name("__fnt");
        NODE *load_tmp;
        NODE *init = build_temp_init(tmp, src, &load_tmp);
        NODE *result = init;
        int i = 0;
        if (peek_tok(0)->kind != close) {
            for (;;) {
                NODE *idx = ALLOC_node_const_int(i);
                NODE *el  = ALLOC_node_subscript_get(load_tmp, idx);
                NODE *as  = build_for_target_assigns(el);
                result = ALLOC_node_seq(result, as);
                i++;
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == close) break;
            }
        }
        if (close == T_RPAREN) expect(T_RPAREN, "')'");
        else                   expect(T_RBRACK, "']'");
        return result;
    }
    parse_error("expected target name");
}

static NODE *
parse_for(void)
{
    expect(T_FOR, "'for'");
    int k0 = peek_tok(0)->kind;
    if (k0 != T_NAME && k0 != T_LPAREN && k0 != T_LBRACK)
        parse_error("expected target name in 'for'");
    // Single-name fast path.
    if (k0 == T_NAME && peek_tok(1)->kind != T_COMMA) {
        const char *target = peek_tok(0)->sval;
        tok_pos++;
        expect(T_IN, "'in'");
        NODE *iter = parse_expr_list();
        NODE *body = parse_suite();
        NODE *else_body = match_tok(T_ELSE) ? parse_suite() : ALLOC_node_nop();
        if (cur_scope && !scope_is_global_decl(cur_scope, target)) {
            int idx = scope_add_local(cur_scope, target);
            return ALLOC_node_for_local((uint32_t)idx, iter, body, else_body);
        }
        return ALLOC_node_for_global(target, iter, body, else_body);
    }
    // Tuple target (with optional nested ()).  Use a top-level temp so
    // build_for_target_assigns can index into it.
    const char *tmp_name = intern_name("__forT__", 8);
    int tmp_idx = -1;
    if (cur_scope) tmp_idx = scope_add_local(cur_scope, tmp_name);
    NODE *load_tmp = (cur_scope && tmp_idx >= 0)
        ? ALLOC_node_lref((uint32_t)tmp_idx) : ALLOC_node_gref(tmp_name);
    NODE *prefix = NULL;
    // Special case: the whole LHS is a parenthesised tuple, e.g.
    //   for (a, b) in pairs:
    // — treat it as a single pattern unpacking load_tmp directly (not
    // load_tmp[0]), so we don't get a spurious extra subscription level.
    bool single_paren = false;
    if (k0 == T_LPAREN || k0 == T_LBRACK) {
        int close = (k0 == T_LPAREN) ? T_RPAREN : T_RBRACK;
        int depth = 1;
        int p = 1;
        while (peek_tok(p)->kind != T_EOF) {
            int tk = peek_tok(p)->kind;
            if (tk == T_LPAREN || tk == T_LBRACK) depth++;
            else if (tk == T_RPAREN || tk == T_RBRACK) {
                depth--;
                if (depth == 0) {
                    if (peek_tok(p)->kind == close && peek_tok(p + 1)->kind == T_IN)
                        single_paren = true;
                    break;
                }
            }
            p++;
        }
    }
    // Detect plain-name targets with optional `*name`: emit
    // unpack_assign so starred captures work (`for x, *rest in pairs`).
    if (k0 == T_NAME || k0 == T_STAR) {
        size_t pp = tok_pos;
        const char *names[16];
        bool starred[16];
        int nn = 0;
        bool ok = true;
        for (;;) {
            bool s = false;
            if (tok_arr[pp].kind == T_STAR) { s = true; pp++; }
            if (tok_arr[pp].kind != T_NAME || nn >= 16) { ok = false; break; }
            names[nn] = tok_arr[pp].sval;
            starred[nn] = s;
            nn++; pp++;
            if (tok_arr[pp].kind == T_IN) break;
            if (tok_arr[pp].kind != T_COMMA) { ok = false; break; }
            pp++;
            if (tok_arr[pp].kind == T_IN) break;
        }
        if (ok && nn >= 2 && tok_arr[pp].kind == T_IN) {
            tok_pos = pp;
            struct pyunpack_target ts[16];
            for (int i = 0; i < nn; i++) {
                ts[i].is_starred = starred[i];
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
            prefix = ALLOC_node_unpack_assign((uint32_t)idx, (uint32_t)nn, load_tmp);
            goto for_after_target;
        }
    }
    if (single_paren) {
        prefix = build_for_target_assigns(load_tmp);
    } else {
        // Parse comma-separated targets — each may be NAME or nested tuple.
        int i = 0;
        for (;;) {
            NODE *idx_n = ALLOC_node_const_int(i);
            NODE *el = ALLOC_node_subscript_get(load_tmp, idx_n);
            NODE *as = build_for_target_assigns(el);
            prefix = prefix ? ALLOC_node_seq(prefix, as) : as;
            i++;
            if (!match_tok(T_COMMA)) break;
            if (peek_tok(0)->kind == T_IN) break;
        }
    }
for_after_target: ;
    expect(T_IN, "'in'");
    NODE *iter = parse_expr_list();
    NODE *body = parse_suite();
    NODE *else_body = match_tok(T_ELSE) ? parse_suite() : ALLOC_node_nop();
    NODE *new_body = ALLOC_node_seq(prefix, body);
    if (cur_scope && tmp_idx >= 0)
        return ALLOC_node_for_local((uint32_t)tmp_idx, iter, new_body, else_body);
    return ALLOC_node_for_global(tmp_name, iter, new_body, else_body);
}

// Parse params for `def NAME(params):` — handles default values,
// `*args`, `**kwargs`, and keyword-only params (after `*args`).
// Returns slot counts + table indices via out-params.
//   nparams         total slots
//   n_pos_named     # of pos-or-keyword params (before *args)
//   ndefaults       # of (slot, expr) entries pushed to PYSTRO_DEFAULTS
//   didx            base in PYSTRO_DEFAULTS
//   nidx            base in PYSTRO_NAME_TABLE
// Parse-time scratch buffer for parameter annotations.  parse_params
// fills these arrays so the caller (parse_def / parse_lambda) can
// attach them to the function via `f.__annotations__ = {...}` after
// the def node is built.  Cleared at parse_params entry.
#define MAX_ANNS 64
static const char *g_ann_names[MAX_ANNS];
static NODE       *g_ann_nodes[MAX_ANNS];
static int         g_ann_count = 0;

//   flags           bit0=has_va, bit1=has_kw
static void
parse_params(Scope *sc, int *out_nparams, int *out_n_pos_named, int *out_ndefaults,
             uint32_t *out_didx, uint32_t *out_nidx, uint32_t *out_flags)
{
    g_ann_count = 0;
    int nparams = 0;
    int n_pos_named = 0;
    int n_pos_only = 0;
    bool saw_star = false;
    bool saw_slash = false;
    bool has_va = false, has_kw = false;
    struct pydefault defs[32];
    int ndefaults = 0;
    bool seen_default_in_pos = false;
    const char *names[32];
    int nnames = 0;

    if (peek_tok(0)->kind != T_RPAREN) {
        for (;;) {
            // Trailing comma allowed.
            if (peek_tok(0)->kind == T_RPAREN) break;
            // **kwargs always comes last.
            if (match_tok(T_STAR_STAR)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '**'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                // Optional `: annotation` on **kwargs — discard for now
                // (we collect annotations for `name: T` form only).
                if (match_tok(T_COLON)) {
                    Scope *saved = cur_scope; cur_scope = sc->parent;
                    (void)parse_expr();
                    cur_scope = saved;
                }
                scope_add_local(sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_kw = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            // *args, or bare `*` for kw-only marker.
            if (match_tok(T_STAR)) {
                if (peek_tok(0)->kind == T_COMMA || peek_tok(0)->kind == T_RPAREN) {
                    // Bare `*` — kw-only marker.  Pystro doesn't enforce
                    // kw-only-ness; just record that we passed the
                    // separator so subsequent params don't require defaults.
                    saw_star = true;
                    if (!match_tok(T_COMMA)) break;
                    continue;
                }
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '*'");
                const char *pn = peek_tok(0)->sval; tok_pos++;
                // Optional `: annotation` on *args — discard.
                if (match_tok(T_COLON)) {
                    Scope *saved = cur_scope; cur_scope = sc->parent;
                    (void)parse_expr();
                    cur_scope = saved;
                }
                scope_add_local(sc, pn);
                names[nnames++] = pn;
                nparams++;
                has_va = true;
                saw_star = true;
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            // `/` — positional-only marker: everything BEFORE `/` is pos-only.
            if (match_tok(T_SLASH)) {
                saw_slash = true;
                n_pos_only = nparams;  // current count IS the # of pos-only
                if (!match_tok(T_COMMA)) break;
                continue;
            }
            if (peek_tok(0)->kind != T_NAME) parse_error("expected parameter name");
            const char *pn = peek_tok(0)->sval;
            tok_pos++;
            // Optional `: annotation` — capture so the caller can build
            // an __annotations__ dict.  The annotation expression is
            // parsed in the parent scope (annotations don't see the
            // function's own params).
            if (match_tok(T_COLON)) {
                Scope *saved = cur_scope; cur_scope = sc->parent;
                NODE *ann = parse_expr();
                cur_scope = saved;
                if (g_ann_count < MAX_ANNS) {
                    g_ann_names[g_ann_count] = pn;
                    g_ann_nodes[g_ann_count] = ann;
                    g_ann_count++;
                }
            }
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
    (void)saw_slash;
    if (n_pos_only > 255) n_pos_only = 255;
    *out_flags = (has_va ? 1u : 0u) | (has_kw ? 2u : 0u)
               | ((uint32_t)n_pos_only << 8);
}

static NODE *
parse_def(void)
{
    expect(T_DEF, "'def'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected function name");
    const char *fname = peek_tok(0)->sval;
    tok_pos++;
    // PEP 695: `def f[T](x): ...` — discard type-param list.
    if (match_tok(T_LBRACK)) {
        int depth = 1;
        while (depth > 0 && peek_tok(0)->kind != T_EOF) {
            int kk = peek_tok(0)->kind;
            if (kk == T_LBRACK) depth++;
            else if (kk == T_RBRACK) depth--;
            tok_pos++;
        }
    }
    expect(T_LPAREN, "'('");

    Scope sc = {0}; sc.parent = cur_scope;
    int nparams, n_pos_named, ndefaults;
    uint32_t didx, nidx, flags;
    parse_params(&sc, &nparams, &n_pos_named, &ndefaults, &didx, &nidx, &flags);
    // Snapshot annotations now (parse_params filled g_ann_*).
    int n_anns = g_ann_count;
    const char *ann_names[MAX_ANNS];
    NODE *ann_nodes[MAX_ANNS];
    for (int i = 0; i < n_anns; i++) {
        ann_names[i] = g_ann_names[i];
        ann_nodes[i] = g_ann_nodes[i];
    }
    expect(T_RPAREN, "')'");
    // Optional `-> annotation` — capture for __annotations__["return"].
    NODE *ret_ann = NULL;
    if (match_tok(T_ARROW)) {
        Scope *saved = cur_scope; cur_scope = sc.parent;
        ret_ann = parse_expr();
        cur_scope = saved;
    }
    expect(T_COLON, "':'");

    size_t suite_start = tok_pos;
    size_t suite_end   = find_suite_end(suite_start);
    collect_locals_in_range(&sc, suite_start, suite_end);

    Scope *saved = cur_scope; cur_scope = &sc;
    // Method bodies are NOT class-body context; assignments inside
    // them are local, not class attributes.
    bool saved_icb = in_class_body;
    in_class_body = false;
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
        while (match_tok(T_SEMI)) {
            if (peek_tok(0)->kind == T_NEWLINE) break;
            body = ALLOC_node_seq(body, parse_simple_stmt());
        }
        expect(T_NEWLINE, "newline");
    }
    in_class_body = saved_icb;
    cur_scope = saved;

    NODE *def_node = ALLOC_node_def(fname, (uint32_t)nparams, (uint32_t)n_pos_named,
                                    (uint32_t)sc.nlocals,
                                    (uint32_t)ndefaults, didx,
                                    (uint32_t)(sc.has_nested_def ? 0 : 1),
                                    nidx, flags,
                                    (uint32_t)(sc.is_generator ? 1 : 0),
                                    body);

    // If the function had any annotations, build a dict at function-
    // creation time and attach it via `f.__annotations__ = {...}`.
    NODE *ann_set = NULL;
    if (n_anns > 0 || ret_ann) {
        // Build pairs: each as (key_name_str, value_node).  Reserve in
        // node table.
        NODE *items[MAX_ANNS * 2 + 2];
        int ni = 0;
        for (int i = 0; i < n_anns; i++) {
            items[ni++] = ALLOC_node_const_str(ann_names[i]);
            items[ni++] = ann_nodes[i];
        }
        if (ret_ann) {
            items[ni++] = ALLOC_node_const_str(intern_name("return", 6));
            items[ni++] = ret_ann;
        }
        size_t base = node_table_reserve(items, ni);
        ann_set = ALLOC_node_make_dict((uint32_t)base, (uint32_t)(ni / 2));
    }

    if (in_class_body) {
        // Class body: node_def's side effect added the method;
        // attach annotations onto that method via class_method_set.
        if (ann_set) {
            // We can't easily reach the func object here — pystro stores
            // it in the class's method dict.  Skip for class methods
            // (less common need than module-level functions).
        }
        return def_node;
    }

    // Module/function-level def: bind, then attach __annotations__ via
    // attribute set on the bound name.
    NODE *bind = make_store(fname, def_node);
    if (ann_set) {
        NODE *load = make_load(fname);
        NODE *setann = ALLOC_node_attr_set(load, intern_name("__annotations__", 15), ann_set);
        return ALLOC_node_seq(bind, setann);
    }
    return bind;
}

static NODE *
parse_class(void)
{
    expect(T_CLASS, "'class'");
    if (peek_tok(0)->kind != T_NAME) parse_error("expected class name");
    const char *cname = peek_tok(0)->sval;
    tok_pos++;
    // PEP 695 generic class syntax: `class C[T, U]: ...` — pystro
    // doesn't track type parameters, so consume and discard the
    // bracketed type-param list.
    if (match_tok(T_LBRACK)) {
        int depth = 1;
        while (depth > 0 && peek_tok(0)->kind != T_EOF) {
            int kk = peek_tok(0)->kind;
            if (kk == T_LBRACK) depth++;
            else if (kk == T_RBRACK) depth--;
            tok_pos++;
        }
    }
    NODE *base = ALLOC_node_const_none();
    NODE *extra_bases[8];
    int nextra = 0;
    NODE *metaclass = NULL;
    // Collect class-level kwargs (excl. metaclass) for __init_subclass__.
    const char *kw_names[8];
    NODE *kw_vals[8];
    int nkw = 0;
    if (match_tok(T_LPAREN)) {
        bool first = true;
        if (peek_tok(0)->kind != T_RPAREN) {
            for (;;) {
                if (peek_tok(0)->kind == T_NAME && peek_tok(1)->kind == T_ASSIGN) {
                    const char *kw_name = peek_tok(0)->sval;
                    tok_pos += 2;
                    NODE *kw_val = parse_expr();
                    if (strcmp(kw_name, "metaclass") == 0) {
                        metaclass = kw_val;
                    } else if (nkw < 8) {
                        kw_names[nkw] = kw_name;
                        kw_vals[nkw] = kw_val;
                        nkw++;
                    }
                } else if (first) {
                    base = parse_expr();
                } else {
                    if (nextra >= 8) parse_error("too many base classes");
                    extra_bases[nextra++] = parse_expr();
                }
                first = false;
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == T_RPAREN) break;
            }
        }
        expect(T_RPAREN, "')'");
    }
    bool saved_icb = in_class_body;
    NODE *saved_base = cur_class_base;
    in_class_body = true;
    cur_class_base = base;
    NODE *body = parse_suite();
    in_class_body = saved_icb;
    cur_class_base = saved_base;
    // Prepend `__class_kwargs__ = {n1: v1, ...}` to the body so the
    // metaclass / __init_subclass__ path can recover them.
    if (nkw > 0) {
        NODE *items[64];
        for (int i = 0; i < nkw; i++) {
            items[2*i]   = ALLOC_node_const_str(kw_names[i]);
            items[2*i+1] = kw_vals[i];
        }
        size_t bidx = node_table_reserve(items, 2 * nkw);
        NODE *kwd = ALLOC_node_make_dict((uint32_t)bidx, (uint32_t)nkw);
        NODE *set_kw = ALLOC_node_class_method_set(
            intern_name("__class_kwargs__", 16), kwd);
        body = ALLOC_node_seq(set_kw, body);
    }
    // Class-body docstring: if the first statement is a bare string
    // literal, route it to `__doc__`.  parse_stmt already consumed it
    // as an expr stmt; we walk the seq's leftmost spine to its leaf.
    {
        extern const struct NodeKind kind_node_seq;
        extern const struct NodeKind kind_node_const_str;
        NODE *first = body;
        NODE *parent = NULL;
        while (first && first->head.kind == &kind_node_seq) {
            parent = first;
            first = first->u.node_seq.first;
        }
        if (first && first->head.kind == &kind_node_const_str) {
            NODE *doc_set = ALLOC_node_class_method_set(
                intern_name("__doc__", 7), first);
            if (parent) parent->u.node_seq.first = doc_set;
            else        body = doc_set;
        }
    }
    NODE *cls;
    if (nextra == 0) {
        cls = ALLOC_node_class(cname, base, body);
    } else {
        NODE *all_bases[16];
        all_bases[0] = base;
        for (int i = 0; i < nextra; i++) all_bases[i + 1] = extra_bases[i];
        size_t bidx = node_table_reserve(all_bases, nextra + 1);
        cls = ALLOC_node_class_multi(cname, (uint32_t)bidx, (uint32_t)(nextra + 1), body);
    }
    if (metaclass) {
        cls = ALLOC_node_class_with_meta(cls, metaclass, cname);
    }
    if (in_class_body) {
        // Nested class — install on the enclosing class so
        // OuterClass.InnerClass works.
        return ALLOC_node_class_method_set(intern_name(cname, strlen(cname)), cls);
    }
    return make_store(cname, cls);
}

// match / case pattern parsing.  Patterns:
//   _                       wildcard
//   literal (int / str /
//     None / True / False)  literal compare
//   NAME                    capture (binds the subject)
//   ClassName               class isinstance check
//   pat | pat               OR
//   [a, b, ...]             sequence
//   (a, b, ...)             sequence (tuple form)
static int parse_pattern_or(void);

static int
parse_pattern_atom(void)
{
    int k = peek_tok(0)->kind;
    struct pypat p = {0};
    if (k == T_INT || k == T_FLOAT || k == T_IMAG || k == T_STR
            || k == T_NONE || k == T_TRUE || k == T_FALSE) {
        p.kind = PYPAT_LITERAL;
        p.literal = parse_atom();
        return pat_alloc(p);
    }
    if (k == T_MINUS) {
        // negative literal
        p.kind = PYPAT_LITERAL;
        p.literal = parse_factor();
        return pat_alloc(p);
    }
    if (k == T_NAME) {
        // _ wildcard, lowercase NAME = capture, ClassName(possibly with .) = value/class
        const char *nm = peek_tok(0)->sval;
        if (strcmp(nm, "_") == 0) {
            tok_pos++;
            p.kind = PYPAT_WILDCARD;
            return pat_alloc(p);
        }
        // Lookahead: NAME (`.` NAME)* `(` ⇒ class pattern; NAME (`.` NAME)+ ⇒ value pattern
        bool is_class_pattern = false;
        if (peek_tok(1)->kind == T_LPAREN) {
            is_class_pattern = true;
        } else if (peek_tok(1)->kind == T_DOT) {
            int p = 1;
            while (peek_tok(p)->kind == T_DOT && peek_tok(p+1)->kind == T_NAME) p += 2;
            if (peek_tok(p)->kind == T_LPAREN) is_class_pattern = true;
        }
        if (is_class_pattern) {
            // class pattern Cls() / pkg.Cls() / a.b.Cls() — consume the
            // dotted name then `(`.
            NODE *cls_node = parse_atom();
            while (peek_tok(0)->kind == T_DOT && peek_tok(1)->kind == T_NAME) {
                tok_pos++;
                cls_node = ALLOC_node_attr_get(cls_node, peek_tok(0)->sval);
                tok_pos++;
            }
            expect(T_LPAREN, "'('");
            if (match_tok(T_RPAREN)) {
                p.kind = PYPAT_CLASS;
                p.literal = cls_node;
                return pat_alloc(p);
            }
            // class with attribute patterns OR positional patterns.
            // Positional patterns are looked up via the class's __match_args__
            // at match time; we encode them with attrs[i] = NULL.
            const char *attrs[16];
            int child_pats[16];
            int nargs = 0;
            bool seen_kw = false;
            for (;;) {
                if (peek_tok(0)->kind == T_NAME && peek_tok(1)->kind == T_ASSIGN) {
                    seen_kw = true;
                    attrs[nargs] = peek_tok(0)->sval;
                    tok_pos += 2;
                    child_pats[nargs] = parse_pattern_or();
                } else {
                    if (seen_kw)
                        parse_error("positional after keyword in class pattern");
                    attrs[nargs] = NULL;     // positional — resolved at match time
                    child_pats[nargs] = parse_pattern_or();
                }
                nargs++;
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == T_RPAREN) break;
            }
            expect(T_RPAREN, "')'");
            int base = (int)pystro_patterns_len;
            for (int i = 0; i < nargs; i++) {
                struct pypat copy = PYSTRO_PATTERNS[child_pats[i]];
                pat_alloc(copy);
            }
            p.kind = PYPAT_CLASS_ARGS;
            p.literal = cls_node;
            p.first_child = base;
            p.nchildren = nargs;
            const char **anames = (const char **)GC_malloc(sizeof(char *) * nargs);
            for (int i = 0; i < nargs; i++) anames[i] = attrs[i];
            p.attrs = anames;
            return pat_alloc(p);
        }
        if (peek_tok(1)->kind == T_DOT) {
            // value pattern: dotted reference
            NODE *value_expr = parse_atom();
            while (peek_tok(0)->kind == T_DOT) {
                tok_pos++;
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after '.'");
                value_expr = ALLOC_node_attr_get(value_expr, peek_tok(0)->sval);
                tok_pos++;
            }
            p.kind = PYPAT_VALUE;
            p.literal = value_expr;
            return pat_alloc(p);
        }
        // capture
        tok_pos++;
        p.kind = PYPAT_CAPTURE;
        if (cur_scope && !scope_is_global_decl(cur_scope, nm)) {
            p.slot = scope_add_local(cur_scope, nm);
            p.name = NULL;
        } else {
            p.slot = -1;
            p.name = nm;
        }
        return pat_alloc(p);
    }
    if (k == T_LBRACK || k == T_LPAREN) {
        int close = (k == T_LBRACK) ? T_RBRACK : T_RPAREN;
        tok_pos++;
        int children[64]; int nc = 0;
        if (peek_tok(0)->kind != close) {
            // *NAME inside seq pattern — capture rest.
            if (peek_tok(0)->kind == T_STAR) {
                tok_pos++;
                struct pypat sp = {0};
                sp.kind = PYPAT_STAR;
                if (peek_tok(0)->kind == T_NAME && strcmp(peek_tok(0)->sval, "_") != 0) {
                    sp.name = peek_tok(0)->sval;
                    if (cur_scope && !scope_is_global_decl(cur_scope, sp.name)) {
                        sp.slot = scope_add_local(cur_scope, sp.name);
                        sp.name = NULL;
                    } else sp.slot = -1;
                    tok_pos++;
                } else if (peek_tok(0)->kind == T_NAME) {
                    sp.slot = -1; sp.name = NULL; tok_pos++;
                } else { sp.slot = -1; sp.name = NULL; }
                children[nc++] = pat_alloc(sp);
            } else {
                children[nc++] = parse_pattern_or();
            }
            while (match_tok(T_COMMA)) {
                if (peek_tok(0)->kind == close) break;
                if (nc >= 64) parse_error("seq pattern too long");
                if (peek_tok(0)->kind == T_STAR) {
                    tok_pos++;
                    struct pypat sp = {0};
                    sp.kind = PYPAT_STAR;
                    if (peek_tok(0)->kind == T_NAME && strcmp(peek_tok(0)->sval, "_") != 0) {
                        sp.name = peek_tok(0)->sval;
                        if (cur_scope && !scope_is_global_decl(cur_scope, sp.name)) {
                            sp.slot = scope_add_local(cur_scope, sp.name);
                            sp.name = NULL;
                        } else sp.slot = -1;
                        tok_pos++;
                    } else { sp.slot = -1; sp.name = NULL;
                        if (peek_tok(0)->kind == T_NAME) tok_pos++;
                    }
                    children[nc++] = pat_alloc(sp);
                } else {
                    children[nc++] = parse_pattern_or();
                }
            }
        }
        if (close == T_RBRACK) expect(T_RBRACK, "']'");
        else                   expect(T_RPAREN, "')'");
        int base = (int)pystro_patterns_len;
        for (int i = 0; i < nc; i++) {
            struct pypat copy = PYSTRO_PATTERNS[children[i]];
            pat_alloc(copy);
        }
        p.kind = PYPAT_SEQUENCE;
        p.first_child = base;
        p.nchildren = nc;
        return pat_alloc(p);
    }
    if (k == T_LBRACE) {
        // mapping pattern: {key_expr: pat, ...}
        tok_pos++;
        NODE *keys[64];
        int child_pats[64];
        int nc = 0;
        if (peek_tok(0)->kind != T_RBRACE) {
            for (;;) {
                NODE *kexpr = parse_expr();
                expect(T_COLON, "':'");
                int sub = parse_pattern_or();
                if (nc >= 64) parse_error("mapping pattern too long");
                keys[nc] = kexpr;
                child_pats[nc] = sub;
                nc++;
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == T_RBRACE) break;
            }
        }
        expect(T_RBRACE, "'}'");
        int base = (int)pystro_patterns_len;
        for (int i = 0; i < nc; i++) {
            struct pypat copy = PYSTRO_PATTERNS[child_pats[i]];
            pat_alloc(copy);
        }
        p.kind = PYPAT_MAPPING;
        p.first_child = base;
        p.nchildren = nc;
        NODE **kn = (NODE **)GC_malloc(sizeof(NODE *) * nc);
        for (int i = 0; i < nc; i++) kn[i] = keys[i];
        p.keys = kn;
        return pat_alloc(p);
    }
    {
        extern const char *tok_kind_name(int k_);
        parse_error("unexpected token in pattern: %s", tok_kind_name(k));
    }
}

static int
parse_pattern_or(void)
{
    int first = parse_pattern_atom();
    int result;
    if (peek_tok(0)->kind != T_PIPE) {
        result = first;
    } else {
        int alts[16]; int n = 0;
        alts[n++] = first;
        while (match_tok(T_PIPE)) {
            if (n >= 16) parse_error("too many | alternatives");
            alts[n++] = parse_pattern_atom();
        }
        int base = (int)pystro_patterns_len;
        for (int i = 0; i < n; i++) {
            struct pypat copy = PYSTRO_PATTERNS[alts[i]];
            pat_alloc(copy);
        }
        struct pypat p = {0};
        p.kind = PYPAT_OR;
        p.first_child = base;
        p.nchildren = n;
        result = pat_alloc(p);
    }
    // `pat as NAME` — bind NAME to the matched subject if pat matches.
    if (peek_tok(0)->kind == T_AS) {
        tok_pos++;
        if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after 'as'");
        const char *bind_name = peek_tok(0)->sval;
        tok_pos++;
        int slot = -1;
        if (cur_scope && !scope_is_global_decl(cur_scope, bind_name) &&
            !scope_is_nonlocal_decl(cur_scope, bind_name))
            slot = scope_add_local(cur_scope, bind_name);
        int base = (int)pystro_patterns_len;
        struct pypat copy = PYSTRO_PATTERNS[result];
        pat_alloc(copy);
        struct pypat p = {0};
        p.kind = PYPAT_AS;
        p.first_child = base;
        p.nchildren = 1;
        p.slot = slot;
        p.name = bind_name;
        const char **anames = (const char **)GC_malloc(sizeof(char *) * 1);
        anames[0] = bind_name;
        p.attrs = anames;
        result = pat_alloc(p);
    }
    return result;
}

static NODE *
parse_match(void)
{
    // Caller already verified peek(0) is the NAME "match".
    tok_pos++;
    NODE *subject = parse_expr();
    expect(T_COLON, "':'");
    expect(T_NEWLINE, "newline");
    expect(T_INDENT, "indent");
    struct pycase cases[64];
    int nc = 0;
    while (peek_tok(0)->kind == T_NAME &&
           peek_tok(0)->sval == intern_name("case", 4)) {
        tok_pos++;
        int pat = parse_pattern_or();
        NODE *guard = NULL;
        if (match_tok(T_IF)) guard = parse_expr();
        NODE *body = parse_suite();
        if (nc >= 64) parse_error("too many cases");
        cases[nc].pat_idx = pat;
        cases[nc].guard = guard;
        cases[nc].body = body;
        nc++;
    }
    expect(T_DEDENT, "dedent");
    size_t base = cases_reserve(cases, nc);
    return ALLOC_node_match(subject, (uint32_t)base, (uint32_t)nc);
}

// `with EXPR as NAME: body` desugars to:
//   __cm = EXPR
//   NAME = __cm.__enter__()       (if `as NAME` was given)
//   <node_with __cm: body>         (handles __exit__ protocol)
static NODE *
build_with_one(NODE *cm_expr, const char *as_name, NODE *inner_body)
{
    const char *tmp = new_temp_name("__cm");
    NODE *load_cm;
    NODE *init = build_temp_init(tmp, cm_expr, &load_cm);
    NODE *enter_call = ALLOC_node_method_0(load_cm, intern_name("__enter__", 9));
    NODE *bind = as_name ? make_store(as_name, enter_call) : enter_call;
    NODE *with_node = ALLOC_node_with(load_cm, inner_body, 0, 0);
    return ALLOC_node_seq(init, ALLOC_node_seq(bind, with_node));
}

static NODE *
parse_with(void)
{
    expect(T_WITH, "'with'");
    // Collect (cm_expr, as_name) pairs separated by ','.
    // Optional parens (3.10+): `with (cm1, cm2 as x): ...`.
    bool parens = false;
    // Heuristic: opening `(` followed by an item that could be a
    // context manager + `as` or `,` or `)` before `:`.
    if (peek_tok(0)->kind == T_LPAREN) {
        // Scan to matching `)` and see what follows; if a `:`, it's the
        // parenthesised with form.
        int depth = 0;
        size_t p = tok_pos;
        for (;;) {
            int kk = tok_arr[p].kind;
            if (kk == T_EOF || kk == T_NEWLINE) break;
            if (kk == T_LPAREN) depth++;
            else if (kk == T_RPAREN) {
                depth--;
                if (depth == 0) {
                    if (tok_arr[p + 1].kind == T_COLON) parens = true;
                    break;
                }
            }
            p++;
        }
        if (parens) tok_pos++;  // consume `(`
    }
    // Each item: (cm_expr, as_name) where as_name may be NULL OR
    // a synthesised tmp name when the source has a tuple target — in
    // that case `unpack_prefix` is the tuple-unpack stmt that goes at
    // the head of the with-body.
    struct { NODE *expr; const char *as_name; NODE *unpack_prefix; } items[8];
    int n = 0;
    for (;;) {
        if (n >= 8) parse_error("too many with items");
        items[n].expr = parse_expr();
        items[n].as_name = NULL;
        items[n].unpack_prefix = NULL;
        if (match_tok(T_AS)) {
            // Plain NAME (no trailers): the cm value binds directly to it.
            if (peek_tok(0)->kind == T_NAME
                    && peek_tok(1)->kind != T_DOT
                    && peek_tok(1)->kind != T_LBRACK) {
                items[n].as_name = peek_tok(0)->sval;
                tok_pos++;
            } else if (peek_tok(0)->kind == T_NAME) {
                // `with cm as obj.attr:` / `with cm as a[i]:` —
                // bind to a synthetic tmp, then assign the
                // trailer-target from the tmp at body-head.
                size_t lhs_start = tok_pos;
                (void)parse_expr();   // consume the LHS expression
                const char *tmp = new_temp_name("__withT");
                if (cur_scope && !scope_is_global_decl(cur_scope, tmp))
                    scope_add_local(cur_scope, tmp);
                items[n].as_name = tmp;
                NODE *load_tmp = make_load(tmp);
                size_t saved = tok_pos;
                tok_pos = lhs_start;
                NODE *store = parse_assignable_target(load_tmp);
                tok_pos = saved;
                items[n].unpack_prefix = store;
            } else if (peek_tok(0)->kind == T_LPAREN || peek_tok(0)->kind == T_LBRACK) {
                // `with cm as (a, b):` — desugar to `with cm as __t: a, b = __t; body`.
                int close = peek_tok(0)->kind == T_LPAREN ? T_RPAREN : T_RBRACK;
                tok_pos++;
                const char *names[16]; int nn = 0;
                while (peek_tok(0)->kind == T_NAME) {
                    if (nn >= 16) parse_error("with: too many tuple targets");
                    names[nn++] = peek_tok(0)->sval;
                    tok_pos++;
                    if (!match_tok(T_COMMA)) break;
                }
                expect(close, close == T_RPAREN ? "')'" : "']'");
                const char *tmp = new_temp_name("__withT");
                if (cur_scope && !scope_is_global_decl(cur_scope, tmp))
                    scope_add_local(cur_scope, tmp);
                items[n].as_name = tmp;
                NODE *load_tmp = make_load(tmp);
                NODE *prefix = NULL;
                for (int i = 0; i < nn; i++) {
                    NODE *idx_n = ALLOC_node_const_int(i);
                    NODE *el = ALLOC_node_subscript_get(load_tmp, idx_n);
                    NODE *st = make_store(names[i], el);
                    prefix = prefix ? ALLOC_node_seq(prefix, st) : st;
                }
                items[n].unpack_prefix = prefix;
            } else {
                parse_error("expected NAME or tuple target after 'as'");
            }
        }
        n++;
        if (!match_tok(T_COMMA)) break;
        if (parens && peek_tok(0)->kind == T_RPAREN) break;
    }
    if (parens) expect(T_RPAREN, "')'");
    NODE *body = parse_suite();
    // Wrap from innermost (last item) to outermost (first item).
    NODE *result = body;
    for (int i = n - 1; i >= 0; i--) {
        if (items[i].unpack_prefix) result = ALLOC_node_seq(items[i].unpack_prefix, result);
        result = build_with_one(items[i].expr, items[i].as_name, result);
    }
    return result;
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
        // PEP 654: `except*` — exception group split-and-handle.
        if (match_tok(T_STAR)) {
            h.is_star = true;
        }
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
    NODE *else_body = match_tok(T_ELSE) ? parse_suite() : ALLOC_node_nop();
    NODE *finally_body = NULL;
    if (match_tok(T_FINALLY)) finally_body = parse_suite();
    if (nh == 0 && !finally_body) parse_error("try without except/finally");

    size_t hidx = handlers_reserve(hs, nh);
    return ALLOC_node_try(body, (uint32_t)hidx, (uint32_t)nh,
                          else_body,
                          finally_body ? finally_body : ALLOC_node_nop());
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

// `assert cond [, msg]` — desugars to `if not cond: raise AssertionError(msg)`.
// AssertionError is bound from c->EXC_RuntimeError as a fallback if the
// builtin isn't installed; for v0 we just use Exception.
static NODE *
parse_assert(void)
{
    expect(T_ASSERT, "'assert'");
    NODE *cond = parse_expr();
    NODE *msg = NULL;
    if (match_tok(T_COMMA)) msg = parse_expr();
    NODE *exc_cls = ALLOC_node_gref(intern_name("AssertionError", 14));
    NODE *raise_call;
    if (msg) raise_call = ALLOC_node_call_1(exc_cls, msg);
    else     raise_call = ALLOC_node_call_0(exc_cls);
    NODE *raise_n = ALLOC_node_raise(raise_call);
    return ALLOC_node_if(ALLOC_node_not(cond), raise_n, ALLOC_node_nop());
}

// `del` — supports `del NAME`, `del obj.attr`, `del a[i]`.  For
// `del NAME` we emit a builtin call that unbinds the global / writes
// PY_NONE to the local (Python truly unbinds; pystro's local frame
// has no per-slot validity bit so the local case is approximate but
// commonly safe).
static NODE *parse_del_one(void);

static NODE *
parse_del(void)
{
    expect(T_DEL, "'del'");
    NODE *result = parse_del_one();
    while (match_tok(T_COMMA)) {
        if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI) break;
        result = ALLOC_node_seq(result, parse_del_one());
    }
    return result;
}

static NODE *
parse_del_one(void)
{
    if (peek_tok(0)->kind != T_NAME) parse_error("del expects a target");
    const char *base = peek_tok(0)->sval;
    tok_pos++;
    NODE *cur = make_load(base);
    while (peek_tok(0)->kind == T_DOT || peek_tok(0)->kind == T_LBRACK
            || peek_tok(0)->kind == T_LPAREN) {
        // `del expr.attr` / `del expr[i]` / `del expr(...)[i]` — accept
        // intermediate calls; the actual del happens on the last
        // attr/subscript trailer.
        if (peek_tok(0)->kind == T_LPAREN) {
            cur = parse_call_args(cur);
            continue;
        }
        if (match_tok(T_DOT)) {
            if (peek_tok(0)->kind != T_NAME) parse_error("attr name expected");
            const char *nm = peek_tok(0)->sval;
            tok_pos++;
            if (peek_tok(0)->kind == T_DOT || peek_tok(0)->kind == T_LBRACK
                    || peek_tok(0)->kind == T_LPAREN) {
                cur = ALLOC_node_attr_get(cur, nm);
                continue;
            }
            // last: `del obj.attr` — call __pystro_delattr__(obj, "attr")
            NODE *args[2] = { cur, ALLOC_node_const_str(nm) };
            size_t bidx = node_table_reserve(args, 2);
            return ALLOC_node_call_n(
                ALLOC_node_gref(intern_name("__pystro_delattr__", 18)),
                (uint32_t)bidx, 2);
        }
        expect(T_LBRACK, "'['");
        // index or slice
        NODE *istart = NULL, *istop = NULL, *istep = NULL;
        bool is_slice = false;
        if (peek_tok(0)->kind == T_COLON) is_slice = true;
        else                              istart = parse_expr();
        if (match_tok(T_COLON)) {
            is_slice = true;
            if (peek_tok(0)->kind != T_COLON && peek_tok(0)->kind != T_RBRACK)
                istop = parse_expr();
            if (match_tok(T_COLON)) {
                if (peek_tok(0)->kind != T_RBRACK) istep = parse_expr();
            }
        }
        expect(T_RBRACK, "']'");
        if (peek_tok(0)->kind == T_DOT || peek_tok(0)->kind == T_LBRACK) {
            if (is_slice) {
                if (!istart) istart = ALLOC_node_const_none();
                if (!istop)  istop  = ALLOC_node_const_none();
                if (!istep)  istep  = ALLOC_node_const_none();
                cur = ALLOC_node_slice(cur, istart, istop, istep);
            } else cur = ALLOC_node_subscript_get(cur, istart);
            continue;
        }
        if (is_slice) {
            // del x[a:b] → x[a:b] = []
            if (!istart) istart = ALLOC_node_const_none();
            if (!istop)  istop  = ALLOC_node_const_none();
            if (!istep)  istep  = ALLOC_node_const_none();
            size_t empty_idx = node_table_reserve(NULL, 0);
            NODE *empty = ALLOC_node_make_list((uint32_t)empty_idx, 0);
            return ALLOC_node_slice_set(cur, istart, istop, istep, empty);
        }
        NODE *args[2] = { cur, istart };
        size_t bidx = node_table_reserve(args, 2);
        return ALLOC_node_call_n(
            ALLOC_node_gref(intern_name("__pystro_del__", 14)),
            (uint32_t)bidx, 2);
    }
    // bare `del NAME`: undefine global, or set sentinel 0 for local.
    if (cur_scope && !scope_is_global_decl(cur_scope, base)) {
        int idx = scope_local_index(cur_scope, base);
        if (idx >= 0)
            return ALLOC_node_lunbind((uint32_t)idx);
    }
    // class body: `del _make_binop` should remove the class attribute,
    // not touch module globals.  Implemented as a class method delete.
    if (in_class_body) {
        // Build: class_method_set(name, NULL) — actually we need an
        // explicit class-method delete.  Approximate by calling
        // `__pystro_delclassattr__(name)` which we add below.  Until
        // that exists, set to `None` and accept that the slot lingers.
        return ALLOC_node_class_method_set(base, ALLOC_node_const_none());
    }
    // global: call __pystro_delglobal__(name_str).
    NODE *args[1] = { ALLOC_node_const_str(base) };
    size_t bidx = node_table_reserve(args, 1);
    return ALLOC_node_call_n(
        ALLOC_node_gref(intern_name("__pystro_delglobal__", 20)),
        (uint32_t)bidx, 1);
}

// `yield E` (and `yield from E`) — true lazy generator implementation.
// Each `yield` swaps back to the next() caller; the body resumes when
// next() is called again.  `yield from E` desugars to `for x in E:
// yield x`.
static NODE *
parse_yield(void)
{
    expect(T_YIELD, "'yield'");
    if (!cur_scope || !cur_scope->is_generator) {
        parse_error("'yield' outside generator function");
    }
    if (peek_tok(0)->kind == T_FROM) {
        tok_pos++;
        NODE *iter = parse_expr();
        // `yield from iter` — call __pystro_yield_from__(iter) which
        // does the inner-iter loop and returns the StopIteration value
        // (or None) — making `result = yield from gen()` work.
        NODE *args[1] = { iter };
        size_t bidx = node_table_reserve(args, 1);
        return ALLOC_node_call_n(
            ALLOC_node_gref(intern_name("__pystro_yield_from__", 21)),
            (uint32_t)bidx, 1);
    }
    NODE *e;
    if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI
            || peek_tok(0)->kind == T_RPAREN || peek_tok(0)->kind == T_COMMA)
        e = ALLOC_node_const_none();
    else
        e = parse_expr_list();    // 'yield a, b' yields a tuple (a, b)
    return ALLOC_node_yield(e);
}

static NODE *
parse_raise(void)
{
    expect(T_RAISE, "'raise'");
    if (peek_tok(0)->kind == T_NEWLINE || peek_tok(0)->kind == T_SEMI)
        return ALLOC_node_raise_bare();
    NODE *e = parse_expr();
    // `raise X from Y` — desugar to:
    //   __cause = Y
    //   __exc   = X (if a class, instantiate)
    //   __exc.__cause__ = __cause
    //   raise __exc
    if (peek_tok(0)->kind == T_FROM) {
        tok_pos++;
        NODE *cause = parse_expr();
        // Desugar: temp_e = X; temp_e.__cause__ = Y; raise temp_e.
        const char *te = new_temp_name("__rx");
        NODE *load_e;
        NODE *init_e = build_temp_init(te, e, &load_e);
        NODE *set_cause = ALLOC_node_attr_set(load_e, intern_name("__cause__", 9), cause);
        NODE *set_suppress = ALLOC_node_attr_set(load_e,
            intern_name("__suppress_context__", 20), ALLOC_node_const_true());
        return ALLOC_node_seq(init_e,
               ALLOC_node_seq(set_cause,
               ALLOC_node_seq(set_suppress,
               ALLOC_node_raise(load_e))));
    }
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
    if (k == T_ASSERT)   return parse_assert();
    if (k == T_DEL)      return parse_del();
    if (k == T_GLOBAL)   return parse_global_decl();
    if (k == T_NONLOCAL) return parse_nonlocal_decl();
    if (k == T_IMPORT) {
        // `import a.b.c [as x] [, d.e [as y], ...]` — multi-import.
        tok_pos++;
        NODE *result = NULL;
        for (;;) {
            if (peek_tok(0)->kind != T_NAME) parse_error("import: name expected");
            char dotted[512];
            size_t dn = 0;
            const char *first = peek_tok(0)->sval;
            size_t fn = strlen(first);
            memcpy(dotted, first, fn); dn = fn;
            const char *last = first;
            tok_pos++;
            while (peek_tok(0)->kind == T_DOT) {
                tok_pos++;
                if (peek_tok(0)->kind != T_NAME) parse_error("import: name expected after '.'");
                const char *seg = peek_tok(0)->sval;
                size_t sn = strlen(seg);
                if (dn + sn + 1 >= sizeof(dotted)) parse_error("import: name too long");
                dotted[dn++] = '.';
                memcpy(dotted + dn, seg, sn); dn += sn;
                last = seg;
                tok_pos++;
            }
            dotted[dn] = '\0';
            // CPython binding rule for `import a.b.c`:
            //   - no `as` → bind the *top-level* name (`a`) and let the
            //     user reach `c` via `a.b.c`.
            //   - `as X` → bind `X` to the leaf module.
            const char *alias = first;
            bool has_as = false;
            if (match_tok(T_AS)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after as");
                alias = peek_tok(0)->sval;
                tok_pos++;
                has_as = true;
            }
            NODE *imp_arg = ALLOC_node_const_str(intern_name(dotted, dn));
            NODE *call = ALLOC_node_call_1(
                ALLOC_node_gref(intern_name("__pystro_import__", 17)),
                has_as ? imp_arg : ALLOC_node_const_str(intern_name(first, fn)));
            // For non-aliased dotted: still trigger the leaf import so
            // the parent's attribute is populated.
            NODE *one;
            if (has_as) {
                one = make_store(alias, call);
            } else if (dn != fn) {
                NODE *leaf_import = ALLOC_node_call_1(
                    ALLOC_node_gref(intern_name("__pystro_import__", 17)),
                    imp_arg);
                NODE *bind = make_store(alias, call);
                one = ALLOC_node_seq(bind, leaf_import);
            } else {
                one = make_store(alias, call);
            }
            (void)last;
            result = result ? ALLOC_node_seq(result, one) : one;
            if (!match_tok(T_COMMA)) break;
        }
        return result;
    }
    if (k == T_FROM) {
        // `from a.b.c import x [as y], z`  or  `from a.b.c import *`
        // `from .x import y` / `from ..x import y` — relative imports:
        // each leading dot ascends one package level.  Pystro converts
        // these to absolute imports at parse time using the current
        // module's __package__ as the base.
        tok_pos++;
        char dotted[512];
        size_t dn = 0;
        int n_dots = 0;
        while (peek_tok(0)->kind == T_DOT) { tok_pos++; n_dots++; }
        if (n_dots > 0) {
            // Resolve relative: emit `__pystro_relimport__("dots+name", level)`
            // — actually simpler: we expect `__name__` of the current
            // module to be set.  At parse time, prepend ".." * (n_dots - 1)
            // dots handled at runtime.  For now, prepend the current
            // module's package via a runtime helper.
            // Encode the relative spec as `<n_dots>:rest` so the runtime
            // import helper can resolve it.
            char buf[8];
            snprintf(buf, sizeof(buf), "\1%d\1", n_dots);
            size_t bl = strlen(buf);
            memcpy(dotted, buf, bl); dn = bl;
        }
        if (peek_tok(0)->kind == T_NAME) {
            const char *first = peek_tok(0)->sval;
            size_t fn = strlen(first);
            memcpy(dotted + dn, first, fn); dn += fn;
            tok_pos++;
            while (peek_tok(0)->kind == T_DOT) {
                tok_pos++;
                if (peek_tok(0)->kind != T_NAME) parse_error("from: name expected after '.'");
                const char *seg = peek_tok(0)->sval;
                size_t sn = strlen(seg);
                if (dn + sn + 1 >= sizeof(dotted)) parse_error("from: name too long");
                dotted[dn++] = '.';
                memcpy(dotted + dn, seg, sn); dn += sn;
                tok_pos++;
            }
        } else if (n_dots == 0) {
            parse_error("from: name expected");
        }
        dotted[dn] = '\0';
        expect(T_IMPORT, "'import'");
        const char *tmp = new_temp_name("__mod");
        NODE *load_tmp;
        NODE *init = build_temp_init(tmp,
            ALLOC_node_call_1(
                ALLOC_node_gref(intern_name("__pystro_import__", 17)),
                ALLOC_node_const_str(intern_name(dotted, dn))),
            &load_tmp);
        NODE *result = init;
        if (peek_tok(0)->kind == T_STAR) {
            // `from m import *` — desugar to a builtin call that walks
            // the module's globals and binds each non-underscore name
            // into the current globals.
            tok_pos++;
            NODE *star = ALLOC_node_call_1(
                ALLOC_node_gref(intern_name("__pystro_import_star__", 22)),
                load_tmp);
            return ALLOC_node_seq(result, star);
        }
        // Parenthesised import list `from m import (a, b, c)` — strip
        // the surrounding parens and let trailing-comma + newline-inside
        // be ignored.  Tokens inside () are already produced normally.
        bool paren = match_tok(T_LPAREN);
        for (;;) {
            // Permit a trailing comma before ')'.
            if (paren && peek_tok(0)->kind == T_RPAREN) break;
            if (peek_tok(0)->kind != T_NAME) parse_error("from: name expected");
            const char *src = peek_tok(0)->sval;
            tok_pos++;
            const char *target = src;
            if (match_tok(T_AS)) {
                if (peek_tok(0)->kind != T_NAME) parse_error("expected NAME after as");
                target = peek_tok(0)->sval;
                tok_pos++;
            }
            // Auto-import submodule: `from a.b import c` may need to
            // load `a.b.c` (a submodule) before `mod.c` resolves.  Ignore
            // any error from this side-import — the subsequent attr_get
            // either succeeds (regular attr) or raises AttributeError.
            // Build "dotted.src" string.
            char sub_path[512];
            if (dn + strlen(src) + 2 < sizeof(sub_path)) {
                memcpy(sub_path, dotted, dn);
                sub_path[dn] = '.';
                strcpy(sub_path + dn + 1, src);
                NODE *side = ALLOC_node_call_1(
                    ALLOC_node_gref(intern_name("__pystro_try_import__", 21)),
                    ALLOC_node_const_str(intern_name(sub_path, strlen(sub_path))));
                result = ALLOC_node_seq(result, side);
            }
            NODE *get = ALLOC_node_attr_get(load_tmp, src);
            result = ALLOC_node_seq(result, make_store(target, get));
            if (!match_tok(T_COMMA)) break;
        }
        if (paren) expect(T_RPAREN, "')'");
        return result;
    }

    // Annotated attribute / subscript assignment: `obj.attr : ann = val`
    // (CPython supports this; pystro discards the annotation).
    if (k == T_NAME) {
        // Lookahead: NAME ('.' NAME)+ ':' ... '='?
        size_t pp = tok_pos + 1;
        bool saw_dot = false;
        while (tok_arr[pp].kind == T_DOT && tok_arr[pp+1].kind == T_NAME) {
            saw_dot = true;
            pp += 2;
        }
        if (saw_dot && tok_arr[pp].kind == T_COLON) {
            // Save lhs; parse and discard ann; require `=` then rhs.
            size_t lhs_start_pos = tok_pos;
            (void)parse_expr();   // parse `obj.attr...`
            expect(T_COLON, "':'");
            (void)parse_expr();   // discard annotation
            if (match_tok(T_ASSIGN)) {
                NODE *rhs = parse_expr_list();
                size_t saved = tok_pos;
                tok_pos = lhs_start_pos;
                NODE *store = parse_assignable_target(rhs);
                tok_pos = saved;
                return store;
            }
            // Bare annotation on attr — no-op.
            return ALLOC_node_nop();
        }
    }
    // Annotated assignment / declaration: `NAME : ann (= expr)?`
    if (k == T_NAME && peek_tok(1)->kind == T_COLON) {
        size_t save = tok_pos;
        const char *nm = peek_tok(0)->sval;
        tok_pos += 2;
        NODE *ann_expr = parse_expr();
        // Inside a class body: track in `__annotations__` so introspection
        // (e.g. dataclasses, typing) can see field declarations.  Capture
        // the annotation expression's value (best-effort — some type
        // forms like `int | str` evaluate at runtime to a tuple-of-classes).
        NODE *track_ann = NULL;
        if (in_class_body) {
            NODE *get_ann = ALLOC_node_class_method_get(intern_name("__annotations__", 15));
            NODE *empty = ALLOC_node_make_dict(node_table_reserve(NULL, 0), 0);
            NODE *or_node = ALLOC_node_or(get_ann, empty);
            NODE *set_ann = ALLOC_node_class_method_set(intern_name("__annotations__", 15), or_node);
            NODE *load_ann = ALLOC_node_class_method_get(intern_name("__annotations__", 15));
            NODE *key = ALLOC_node_const_str(nm);
            NODE *sset = ALLOC_node_subscript_set(load_ann, key, ann_expr);
            // Wrap in try/except NameError, AttributeError, TypeError so
            // class-body annotations referencing undefined names (common
            // before `from __future__ import annotations`) don't kill
            // the class definition.
            NODE *fallback_key = ALLOC_node_const_str(nm);
            NODE *fallback = ALLOC_node_subscript_set(
                ALLOC_node_class_method_get(intern_name("__annotations__", 15)),
                fallback_key, ALLOC_node_const_str(nm));
            struct pyhandler hs[1] = {0};
            // Catch tuple of common issues.
            NODE *exc_tuple_items[3] = {
                ALLOC_node_gref(intern_name("NameError", 9)),
                ALLOC_node_gref(intern_name("AttributeError", 14)),
                ALLOC_node_gref(intern_name("TypeError", 9)),
            };
            size_t etb = node_table_reserve(exc_tuple_items, 3);
            hs[0].exc_class = ALLOC_node_make_tuple((uint32_t)etb, 3);
            hs[0].body = fallback;
            size_t hidx = handlers_reserve(hs, 1);
            NODE *try_node = ALLOC_node_try(sset, (uint32_t)hidx, 1,
                                             ALLOC_node_nop(), ALLOC_node_nop());
            track_ann = ALLOC_node_seq(set_ann, try_node);
        }
        if (match_tok(T_ASSIGN)) {
            NODE *rhs = parse_expr_list();
            NODE *store = make_store(nm, rhs);
            return track_ann ? ALLOC_node_seq(track_ann, store) : store;
        }
        // Bare annotation `x: int` — track but no value bound.
        return track_ann ? track_ann : ALLOC_node_nop();
        (void)save;
    }

    // Parens/brackets-wrapped tuple unpack: '(' / '[' NAMES ')' / ']' '=' rhs.
    // Detection: scan to matching close + `=`; only NAME/`*NAME` inside.
    if (k == T_LPAREN || k == T_LBRACK) {
        int open = k;
        int close = (k == T_LPAREN) ? T_RPAREN : T_RBRACK;
        size_t p = tok_pos + 1;
        const char *names[16];
        bool starred[16];
        int nn = 0;
        bool ok = true;
        for (;;) {
            bool is_star = false;
            if (tok_arr[p].kind == T_STAR) { is_star = true; p++; }
            if (tok_arr[p].kind != T_NAME || nn >= 16) { ok = false; break; }
            names[nn] = tok_arr[p].sval;
            starred[nn] = is_star;
            nn++;
            p++;
            if (tok_arr[p].kind == close) { p++; break; }
            if (tok_arr[p].kind != T_COMMA) { ok = false; break; }
            p++;
            if (tok_arr[p].kind == close) { p++; break; }
        }
        if (ok && tok_arr[p].kind == T_ASSIGN) {
            tok_pos = p + 1;
            NODE *rhs = parse_expr_list();
            struct pyunpack_target ts[16];
            int n_starred = 0;
            for (int i = 0; i < nn; i++) {
                ts[i].is_starred = starred[i];
                if (starred[i]) n_starred++;
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
            if (n_starred > 1) parse_error("at most one starred target");
            size_t idx = unpack_reserve(ts, nn);
            (void)open;
            return ALLOC_node_unpack_assign((uint32_t)idx, (uint32_t)nn, rhs);
        }
    }

    // Detect chained assignment with tuple targets: `t1, t2 = ... = expr`.
    // The plain-name path below only handles a single target group, so
    // bail out early and let the attr/subscript path (which has chain
    // support) handle this.
    if ((k == T_NAME || k == T_STAR)) {
        size_t pp = tok_pos;
        int depth_pp = 0;
        bool saw_eq = false, saw_chain = false, saw_comma = false;
        while (tok_arr[pp].kind != T_EOF && tok_arr[pp].kind != T_NEWLINE
               && tok_arr[pp].kind != T_SEMI) {
            int kk = tok_arr[pp].kind;
            if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) depth_pp++;
            else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) depth_pp--;
            else if (depth_pp == 0 && kk == T_COMMA) saw_comma = true;
            else if (depth_pp == 0 && kk == T_ASSIGN) {
                if (saw_eq) saw_chain = true;
                saw_eq = true;
            }
            pp++;
        }
        // If chain present AND tuple target, fall through to the
        // attr/subscript path which handles chained groups.
        if (saw_chain && saw_comma) goto skip_plain_unpack;
    }
    // Multi-target unpack assignment: TARGET (',' TARGET)+ '=' expr,
    // where TARGET is NAME or `*NAME` (starred — at most one).
    if (k == T_NAME || k == T_STAR) {
        // Lookahead: scan a sequence of (`*`?NAME) separated by `,`,
        // ending at `=`.  All-or-nothing — restore tok_pos otherwise.
        const char *names[16];
        bool starred[16];
        int nn = 0;
        size_t p = tok_pos;
        bool ok = true;
        for (;;) {
            bool is_star = false;
            if (tok_arr[p].kind == T_STAR) { is_star = true; p++; }
            if (tok_arr[p].kind != T_NAME || nn >= 16) { ok = false; break; }
            names[nn] = tok_arr[p].sval;
            starred[nn] = is_star;
            nn++;
            p++;
            if (tok_arr[p].kind == T_ASSIGN) break;
            if (tok_arr[p].kind != T_COMMA) { ok = false; break; }
            p++;
            // Trailing comma: `*name, = ...` is a valid 1-target form.
            if (tok_arr[p].kind == T_ASSIGN) break;
        }
        // Need >= 2 targets (or 1 starred) for unpack form, AND `=` next.
        bool starred_present = false;
        for (int i = 0; i < nn; i++) if (starred[i]) { starred_present = true; break; }
        if (ok && tok_arr[p].kind == T_ASSIGN && (nn >= 2 || starred_present)) {
            tok_pos = p + 1;
            NODE *rhs = parse_expr_list();
            struct pyunpack_target ts[16];
            int n_starred = 0;
            for (int i = 0; i < nn; i++) {
                ts[i].is_starred = starred[i];
                if (starred[i]) n_starred++;
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
            if (n_starred > 1) parse_error("at most one starred target");
            size_t idx = unpack_reserve(ts, nn);
            return ALLOC_node_unpack_assign((uint32_t)idx, (uint32_t)nn, rhs);
        }
    }

skip_plain_unpack: ;
    // Save start, parse LHS as expression, then peek operator.
    size_t lhs_start = tok_pos;
    NODE *lhs_expr = parse_expr();
    int k2 = peek_tok(0)->kind;

    // Tuple-unpack with arbitrary targets: `t1, t2, ... = rhs`.
    // (Plain-name unpack handled above; this catches attr/subscript targets.)
    if (k2 == T_COMMA) {
        // Lookahead to see if a top-level `=` follows.
        size_t p = tok_pos;
        int depth = 0;
        bool found_assign = false;
        while (tok_arr[p].kind != T_EOF && tok_arr[p].kind != T_NEWLINE
               && tok_arr[p].kind != T_SEMI) {
            int kk = tok_arr[p].kind;
            if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) depth++;
            else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) depth--;
            else if (depth == 0 && kk == T_ASSIGN) { found_assign = true; break; }
            else if (depth == 0 && kk == T_COLON) break;  // dict/slice
            p++;
        }
        if (found_assign) {
            // First target group: starts at lhs_start.  Could have
            // multiple comma-separated targets.  Multiple `=` chain
            // additional target lists.
            //
            //   t11, t12 = t21, t22 = ... = expr
            //
            // For each target group, recover the starts and replay parse
            // against a temp holding the RHS.
            size_t group_starts[8][16];   // [group][target]
            int    group_counts[8];
            int    n_groups = 0;
            // First group from already-consumed targets — re-collect.
            size_t target_starts[16];
            int    n_targets = 1;
            target_starts[0] = lhs_start;
            while (match_tok(T_COMMA)) {
                if (peek_tok(0)->kind == T_ASSIGN) break;
                if (n_targets >= 16) parse_error("too many unpack targets");
                target_starts[n_targets++] = tok_pos;
                (void)parse_expr();
            }
            for (int i = 0; i < n_targets; i++) group_starts[0][i] = target_starts[i];
            group_counts[0] = n_targets;
            n_groups = 1;
            expect(T_ASSIGN, "'='");
            // Look for further chained target groups: starts(`=`?stmtish?expr+,`=`)
            // Heuristic — scan for next top-level `=` before NEWLINE.
            for (;;) {
                size_t after_eq = tok_pos;
                int depth2 = 0;
                size_t pp = after_eq;
                bool more_chain = false;
                while (tok_arr[pp].kind != T_EOF && tok_arr[pp].kind != T_NEWLINE
                       && tok_arr[pp].kind != T_SEMI) {
                    int kk2 = tok_arr[pp].kind;
                    if (kk2 == T_LPAREN || kk2 == T_LBRACK || kk2 == T_LBRACE) depth2++;
                    else if (kk2 == T_RPAREN || kk2 == T_RBRACK || kk2 == T_RBRACE) depth2--;
                    else if (depth2 == 0 && kk2 == T_ASSIGN) { more_chain = true; break; }
                    else if (depth2 == 0 && (kk2 == T_PLUS_EQ || kk2 == T_MINUS_EQ)) break;
                    pp++;
                }
                if (!more_chain) break;
                if (n_groups >= 8) parse_error("too many chained assign groups");
                int nt = 0;
                group_starts[n_groups][nt++] = tok_pos;
                (void)parse_expr();
                while (match_tok(T_COMMA)) {
                    if (peek_tok(0)->kind == T_ASSIGN) break;
                    if (nt >= 16) parse_error("too many unpack targets");
                    group_starts[n_groups][nt++] = tok_pos;
                    (void)parse_expr();
                }
                group_counts[n_groups] = nt;
                n_groups++;
                expect(T_ASSIGN, "'='");
            }
            NODE *rhs = parse_expr_list();
            // Unpack via temp tuple subscript: __t = rhs; t_i = __t[i].
            const char *tmp = new_temp_name("__upk");
            NODE *load_tmp;
            NODE *init = build_temp_init(tmp, rhs, &load_tmp);
            NODE *result = init;
            for (int g = 0; g < n_groups; g++) {
                int ng = group_counts[g];
                if (ng == 1) {
                    // single target — assign load_tmp directly
                    size_t saved = tok_pos;
                    tok_pos = group_starts[g][0];
                    NODE *store = parse_assignable_target(load_tmp);
                    tok_pos = saved;
                    result = ALLOC_node_seq(result, store);
                } else {
                    for (int i = 0; i < ng; i++) {
                        size_t saved = tok_pos;
                        tok_pos = group_starts[g][i];
                        NODE *elem = ALLOC_node_subscript_get(load_tmp,
                            ALLOC_node_const_int(i));
                        NODE *store = parse_assignable_target(elem);
                        tok_pos = saved;
                        result = ALLOC_node_seq(result, store);
                    }
                }
            }
            return result;
        }
    }

    if (k2 == T_ASSIGN) {
        // Possible chain `a = b = ... = expr` — accumulate target spans
        // and bind via a hidden temp so the RHS evaluates exactly once.
        size_t starts[8] = { lhs_start };
        int    nt = 1;
        tok_pos++;        // past first '='
        // We've already parsed `lhs_expr` once and seen `=`; the next
        // expr is either another LHS (followed by `=`) or the final RHS.
        for (;;) {
            size_t s = tok_pos;
            (void)parse_expr_list();
            if (peek_tok(0)->kind == T_ASSIGN) {
                if (nt >= 8) parse_error("too many = chains");
                starts[nt++] = s;
                tok_pos++;
                continue;
            }
            // The last parsed expr was the RHS; we need to retain its
            // NODE *.  Easiest: rewind and re-parse it.
            size_t rhs_end = tok_pos;
            tok_pos = s;
            NODE *rhs = parse_expr_list();
            (void)rhs_end;
            const char *tmp = new_temp_name("__ma");
            NODE *load_tmp;
            NODE *init = build_temp_init(tmp, rhs, &load_tmp);
            NODE *result = init;
            for (int i = nt - 1; i >= 0; i--) {
                size_t saved2 = tok_pos;
                tok_pos = starts[i];
                NODE *store = parse_assignable_target(load_tmp);
                tok_pos = saved2;
                result = ALLOC_node_seq(result, store);
            }
            return result;
        }
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

    // Expression statement — accept a trailing comma (which makes the
    // statement a tuple-of-one expression).  Some real-world code does
    // `assertEqual(a, b),` — odd, but valid Python.
    if (k2 == T_COMMA) {
        // Lookahead: if next non-trivial token suggests this is just a
        // trailing comma (NEWLINE / ; / EOF after one or more exprs),
        // wrap as tuple statement.
        NODE *items[64];
        int nn = 1;
        items[0] = lhs_expr;
        while (match_tok(T_COMMA)) {
            int kk = peek_tok(0)->kind;
            if (kk == T_NEWLINE || kk == T_SEMI || kk == T_EOF) break;
            if (nn >= 16) parse_error("expression statement too long");
            items[nn++] = parse_expr();
        }
        if (nn == 1) return lhs_expr;
        size_t base = node_table_reserve(items, nn);
        return ALLOC_node_make_tuple((uint32_t)base, (uint32_t)nn);
    }
    return lhs_expr;
}

// Parse an assignment target expression and emit a store of `rhs`.
// Supports NAME, NAME ('.' NAME)*, NAME ('[' subscript ']')*, and a
// final `[i:j(:k)?]` slice trailer for `a[i:j] = list`.
static NODE *
parse_assignable_target(NODE *rhs)
{
    Tok *t = peek_tok(0);
    // Nested-tuple/list pattern: `(a, b, ...) = rhs` or `[a, b] = rhs`.
    // Each child becomes `child_i = __t[i]` recursively.
    if (t->kind == T_LPAREN || t->kind == T_LBRACK) {
        int close = (t->kind == T_LPAREN) ? T_RPAREN : T_RBRACK;
        tok_pos++;
        const char *tmp = new_temp_name("__nupk");
        NODE *load_tmp;
        NODE *result = build_temp_init(tmp, rhs, &load_tmp);
        int i = 0;
        if (peek_tok(0)->kind != close) {
            for (;;) {
                NODE *idx = ALLOC_node_const_int(i);
                NODE *el  = ALLOC_node_subscript_get(load_tmp, idx);
                NODE *as  = parse_assignable_target(el);
                result = ALLOC_node_seq(result, as);
                i++;
                if (!match_tok(T_COMMA)) break;
                if (peek_tok(0)->kind == close) break;
            }
        }
        if (close == T_RPAREN) expect(T_RPAREN, "')'");
        else                   expect(T_RBRACK, "']'");
        return result;
    }
    if (t->kind != T_NAME) parse_error("invalid assignment target");
    const char *base_name = t->sval;
    tok_pos++;
    enum trailer_kind { TR_DOT, TR_SUB, TR_SLICE };
    struct {
        int kind;
        const char *name;                  // TR_DOT
        NODE *idx;                         // TR_SUB
        NODE *sa, *sb, *sc;                // TR_SLICE
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
            // index or slice
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
            // `a[i, j]` — tuple subscript; pack into a tuple.
            if (!is_slice && peek_tok(0)->kind == T_COMMA) {
                NODE *items[16];
                int nn = 1;
                items[0] = start;
                while (match_tok(T_COMMA)) {
                    if (peek_tok(0)->kind == T_RBRACK) break;
                    if (nn >= 16) parse_error("too many tuple subscript items");
                    items[nn++] = parse_expr();
                }
                size_t base = node_table_reserve(items, nn);
                start = ALLOC_node_make_tuple((uint32_t)base, (uint32_t)nn);
            }
            expect(T_RBRACK, "']'");
            if (is_slice) {
                trs[ntr].kind = TR_SLICE;
                trs[ntr].sa = start ? start : ALLOC_node_const_none();
                trs[ntr].sb = stop  ? stop  : ALLOC_node_const_none();
                trs[ntr].sc = step  ? step  : ALLOC_node_const_none();
            } else {
                trs[ntr].kind = TR_SUB;
                trs[ntr].idx = start;
            }
            ntr++;
        }
    }
    if (ntr == 0) return make_store(base_name, rhs);
    NODE *cur = make_load(base_name);
    for (int i = 0; i < ntr - 1; i++) {
        if (trs[i].kind == TR_DOT)        cur = ALLOC_node_attr_get(cur, trs[i].name);
        else if (trs[i].kind == TR_SUB)   cur = ALLOC_node_subscript_get(cur, trs[i].idx);
        else                              cur = ALLOC_node_slice(cur, trs[i].sa, trs[i].sb, trs[i].sc);
    }
    int last = ntr - 1;
    if (trs[last].kind == TR_DOT)
        return ALLOC_node_attr_set(cur, trs[last].name, rhs);
    if (trs[last].kind == TR_SUB)
        return ALLOC_node_subscript_set(cur, trs[last].idx, rhs);
    return ALLOC_node_slice_set(cur, trs[last].sa, trs[last].sb, trs[last].sc, rhs);
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
    // `async def` after decorator — consume `async`, fall through to def.
    if (peek_tok(0)->kind == T_NAME
            && peek_tok(0)->sval == intern_name("async", 5)
            && peek_tok(1)->kind == T_DEF) {
        tok_pos++;     // consume async
    }
    bool is_class_method_dec = in_class_body && peek_tok(0)->kind == T_DEF;
    if (peek_tok(0)->kind == T_DEF) {
        target = tok_arr[tok_pos + 1].sval;
        body = parse_def();
    } else if (peek_tok(0)->kind == T_CLASS) {
        target = tok_arr[tok_pos + 1].sval;
        body = parse_class();
    } else {
        parse_error("expected 'def' or 'class' after decorator");
    }
    if (is_class_method_dec) {
        // Decorators on a class method must be evaluated BEFORE the def
        // runs (because the def overwrites the class slot of the same
        // name — e.g. `@x.setter\ndef x` references the prior `x` from
        // the @property def).  Evaluate each decorator into a synthetic
        // class slot first, then run the body, then apply.
        static int dec_uid = 0;
        const char *names[16];
        for (int i = 0; i < ndecs; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "__pystro_dec$%d$%s__", dec_uid++, target);
            names[i] = intern_name(buf, strlen(buf));
        }
        NODE *result = NULL;
        // Eval decorators first.
        for (int i = 0; i < ndecs; i++) {
            NODE *save = ALLOC_node_class_method_set(names[i], decs[i]);
            result = result ? ALLOC_node_seq(result, save) : save;
        }
        // Then the def body (overwrites target).
        result = ALLOC_node_seq(result, body);
        // Then apply each saved decorator innermost-first.
        for (int i = ndecs - 1; i >= 0; i--) {
            NODE *load = ALLOC_node_class_method_get(target);
            NODE *dec  = ALLOC_node_class_method_get(names[i]);
            NODE *call = ALLOC_node_call_1(dec, load);
            result = ALLOC_node_seq(result, ALLOC_node_class_method_set(target, call));
        }
        return result;
    }
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
    // `async def` / `async for` / `async with` — pystro has no real
    // coroutine model, but we accept the syntax and treat them as
    // their plain (sync) counterparts.  This makes `async def f():`
    // declarations parse and run as ordinary functions.
    if (k == T_NAME && peek_tok(0)->sval == intern_name("async", 5)) {
        int next = peek_tok(1)->kind;
        if (next == T_DEF || next == T_FOR || next == T_WITH) {
            tok_pos++;     // consume "async"
            k = peek_tok(0)->kind;
        }
    }
    if (k == T_AT)     return parse_decorated();
    if (k == T_DEF)    return parse_def();
    if (k == T_CLASS)  return parse_class();
    // PEP 690 lazy imports (experimental, 3.13+): `lazy import x` /
    // `lazy from x import y`.  Treat as eager import.
    if (k == T_NAME && peek_tok(0)->sval == intern_name("lazy", 4)
            && (peek_tok(1)->kind == T_IMPORT || peek_tok(1)->kind == T_FROM)) {
        tok_pos++;        // consume `lazy`
        k = peek_tok(0)->kind;
    }
    // PEP 695 type alias: `type NAME = expr`.  pystro doesn't track
    // type-time-only aliases, so desugar to plain `NAME = expr`.
    if (k == T_NAME && peek_tok(0)->sval == intern_name("type", 4)
            && peek_tok(1)->kind == T_NAME
            && (peek_tok(2)->kind == T_ASSIGN || peek_tok(2)->kind == T_LBRACK)) {
        tok_pos++;        // consume `type`
        const char *nm = peek_tok(0)->sval;
        tok_pos++;        // NAME
        // PEP 695 generic type alias `type X[T] = ...`: parse T's as
        // names and emit assignments `T = "T"` so the RHS can reference
        // them.  pystro doesn't track real TypeVar.
        NODE *param_inits = NULL;
        if (match_tok(T_LBRACK)) {
            int depth = 1;
            while (depth > 0 && peek_tok(0)->kind != T_EOF) {
                int kk = peek_tok(0)->kind;
                if (kk == T_LBRACK) depth++;
                else if (kk == T_RBRACK) { depth--; tok_pos++; continue; }
                if (kk == T_NAME && depth == 1) {
                    const char *tp = peek_tok(0)->sval;
                    NODE *binding = make_store(tp, ALLOC_node_const_str(tp));
                    param_inits = param_inits ? ALLOC_node_seq(param_inits, binding) : binding;
                }
                tok_pos++;
            }
        }
        expect(T_ASSIGN, "'='");
        NODE *rhs = parse_expr_list();
        expect(T_NEWLINE, "newline");
        NODE *store = make_store(nm, rhs);
        return param_inits ? ALLOC_node_seq(param_inits, store) : store;
    }
    if (k == T_IF)     return parse_if();
    if (k == T_WHILE)  return parse_while();
    if (k == T_FOR)    return parse_for();
    if (k == T_TRY)    return parse_try();
    if (k == T_WITH)   return parse_with();
    // `match EXPR:` — soft keyword.  Recognise when the NAME is
    // "match" and the line clearly starts a match statement (NAME or
    // expression followed by `:` at the end of the logical line).
    if (k == T_NAME && peek_tok(0)->sval == intern_name("match", 5)) {
        // Heuristic: lookahead until newline; if a top-level `:` is
        // followed by NEWLINE+INDENT+`case`, treat as match statement.
        size_t save = tok_pos;
        tok_pos++;
        int depth = 0;
        bool saw_colon = false;
        while (peek_tok(0)->kind != T_EOF && peek_tok(0)->kind != T_NEWLINE) {
            int kk = peek_tok(0)->kind;
            if (kk == T_LPAREN || kk == T_LBRACK || kk == T_LBRACE) depth++;
            else if (kk == T_RPAREN || kk == T_RBRACK || kk == T_RBRACE) depth--;
            else if (depth == 0 && kk == T_COLON) { saw_colon = true; break; }
            tok_pos++;
        }
        bool is_match = false;
        if (saw_colon) {
            tok_pos++;     // past colon
            if (peek_tok(0)->kind == T_NEWLINE && peek_tok(1)->kind == T_INDENT
                && peek_tok(2)->kind == T_NAME
                && peek_tok(2)->sval == intern_name("case", 4)) {
                is_match = true;
            }
        }
        tok_pos = save;
        if (is_match) return parse_match();
    }
    NODE *s = parse_simple_stmt();
    // Allow optional ; or NEWLINE
    if (match_tok(T_SEMI)) {
        // Trailing semicolon: `expr;` followed by NEWLINE is also valid.
        if (peek_tok(0)->kind == T_NEWLINE) {
            tok_pos++;
            return s;
        }
        // chain another simple stmt
        NODE *more = parse_stmt();
        // if `more` is a stmt that already consumed its newline, just seq.
        return ALLOC_node_seq(s, more);
    }
    expect(T_NEWLINE, "newline");
    return s;
}

// Save parser state for nested module imports.  bi_import wraps a
// tokenize+parse_program in calls to lexer_save_state / parser_save_state
// before re-entering the parser.
struct parser_state {
    Scope *cur_scope;
    int    comp_remap_top;
    bool   in_class_body;
    int    g_ann_count;
};

void *parser_save_alloc(void) {
    struct parser_state *s = (struct parser_state *)GC_malloc(sizeof(*s));
    s->cur_scope = cur_scope;
    s->comp_remap_top = comp_remap_top;
    s->in_class_body = in_class_body;
    s->g_ann_count = g_ann_count;
    return s;
}

void parser_restore_free(void *p) {
    struct parser_state *s = (struct parser_state *)p;
    cur_scope = s->cur_scope;
    comp_remap_top = s->comp_remap_top;
    in_class_body = s->in_class_body;
    g_ann_count = s->g_ann_count;
}

NODE *
parse_program(void)
{
    // Reset parser state.  bi_import re-enters parse_program for each
    // imported module; without this, comp_remap leftover from one
    // tokenize+parse cycle can leak into the next.
    comp_remap_top = 0;
    cur_scope = NULL;
    in_class_body = false;
    g_ann_count = 0;
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

// Used by `eval()` builtin: parse a single expression.
NODE *
parse_eval_expr(void)
{
    while (match_tok(T_NEWLINE)) {}
    return parse_expr();
}
