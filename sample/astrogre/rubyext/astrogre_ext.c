/*
 * astrogre Ruby C extension.
 *
 *   require "astrogre_ext"
 *
 *   p = ASTrogre.compile(/\d+/)
 *   p.match?("abc 123")     #=> true
 *   p.match("abc 123")      #=> [[4, 7]]            (whole-match span as [s,e])
 *   p.match_all("a1 b2 c3") #=> [[1,2], [4,5], [7,8]]
 *
 *   ASTrogre.compile(/(\w+)\s+(\w+)/).match("hello world")
 *     #=> [[0, 11], [0, 5], [6, 11]]
 *     # group 0 (whole match), group 1, group 2 — each as [start, end].
 *
 * The Cext mirrors astrogre_search / astrogre_search_from from the
 * engine; AOT compile and per-pattern code-store hooks aren't exposed
 * here yet — test infrastructure is the primary consumer.
 */

#define _GNU_SOURCE 1

#include <ruby.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Don't include context.h / node.h / parse.h here — they typedef their
 * own `VALUE` (int64_t for astrogre's match-result protocol) which
 * collides with Ruby's `VALUE` (unsigned long).  Forward-declare just
 * the C entry points the Cext needs.  The struct layout for
 * astrogre_match_t mirrors parse.h verbatim; if the engine ever grows
 * a field, this needs updating in lockstep. */

#define ASTROGRE_MAX_GROUPS 32

typedef struct astrogre_pattern astrogre_pattern;

typedef struct astrogre_match {
    bool   matched;
    size_t starts[ASTROGRE_MAX_GROUPS];
    size_t ends[ASTROGRE_MAX_GROUPS];
    bool   valid[ASTROGRE_MAX_GROUPS];
    int    n_groups;
} astrogre_match_t;

extern void INIT(void);
extern bool astrogre_search(astrogre_pattern *p, const char *str, size_t len, astrogre_match_t *out);
extern bool astrogre_search_from(astrogre_pattern *p, const char *str, size_t len,
                                  size_t start, astrogre_match_t *out);
extern astrogre_pattern *astrogre_parse(const char *pat, size_t pat_len, uint32_t prism_flags);
extern void astrogre_pattern_free(astrogre_pattern *p);
extern void astrogre_pattern_aot_compile(astrogre_pattern *p, bool verbose);
extern int  astrogre_pattern_n_named(const astrogre_pattern *p);
extern const char *astrogre_pattern_named_at(const astrogre_pattern *p, int i, int *out_idx);
extern const char *astrogre_pattern_source(const astrogre_pattern *p, size_t *out_len);
extern bool astrogre_pattern_case_insensitive(const astrogre_pattern *p);
extern bool astrogre_pattern_multiline(const astrogre_pattern *p);

/* OPTION fields the engine peeks at — declared so we can flip cs_verbose
 * from Ruby when debugging.  The struct itself lives in
 * astrogre_dump_helper.c (the bridge TU). */
struct astrogre_option_min { uint8_t flags[64]; };  /* opaque from this TU */

/* For Pattern#dump: cross over to node.h-land to call DUMP, but only
 * inside helper that doesn't expose astrogre's VALUE to this TU. */
extern void astrogre_ext_dump_root(astrogre_pattern *p, FILE *fp);

/* Ruby Regexp option bits — match the values in Ruby's regexp.c. */
#define RB_RE_OPT_IGNORECASE 1
#define RB_RE_OPT_EXTENDED   2
#define RB_RE_OPT_MULTILINE  4

/* astrogre prism-flag bits (mirrored from parse.c). */
#define PR_FLAGS_IGNORE_CASE 4
#define PR_FLAGS_EXTENDED    8
#define PR_FLAGS_MULTI_LINE  16

extern void INIT(void);

static VALUE rb_mASTrogre;
static VALUE rb_cPattern;

/* ------------------------------------------------------------------ */
/* Pattern wrapper                                                     */
/* ------------------------------------------------------------------ */

static void
pattern_free(void *p)
{
    astrogre_pattern_free((astrogre_pattern *)p);
}

static size_t
pattern_memsize(const void *p)
{
    (void)p;
    /* astrogre_pattern is opaque to this TU; the framework's side
     * array owns the AST node memory anyway, so no point trying to
     * account for it here. */
    return 0;
}

static const rb_data_type_t pattern_type = {
    .wrap_struct_name = "astrogre/pattern",
    .function = {
        .dmark    = NULL,
        .dfree    = pattern_free,
        .dsize    = pattern_memsize,
    },
    .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

static astrogre_pattern *
unwrap(VALUE self)
{
    astrogre_pattern *p;
    TypedData_Get_Struct(self, astrogre_pattern, &pattern_type, p);
    return p;
}

/* ------------------------------------------------------------------ */
/* ASTrogre.compile(regex_or_string, flags = 0) → Pattern              */
/* ------------------------------------------------------------------ */

static VALUE
m_compile(int argc, VALUE *argv, VALUE self)
{
    VALUE input, opts;
    rb_scan_args(argc, argv, "11", &input, &opts);

    const char *pat;
    long pat_len;
    uint32_t flags = 0;

    if (rb_obj_is_kind_of(input, rb_cRegexp)) {
        VALUE source = rb_funcall(input, rb_intern("source"), 0);
        Check_Type(source, T_STRING);
        pat = RSTRING_PTR(source);
        pat_len = RSTRING_LEN(source);
        long ropts = NUM2LONG(rb_funcall(input, rb_intern("options"), 0));
        if (ropts & RB_RE_OPT_IGNORECASE) flags |= PR_FLAGS_IGNORE_CASE;
        if (ropts & RB_RE_OPT_EXTENDED)   flags |= PR_FLAGS_EXTENDED;
        if (ropts & RB_RE_OPT_MULTILINE)  flags |= PR_FLAGS_MULTI_LINE;
    } else {
        Check_Type(input, T_STRING);
        pat = RSTRING_PTR(input);
        pat_len = RSTRING_LEN(input);
        if (!NIL_P(opts)) {
            long ropts = NUM2LONG(opts);
            if (ropts & RB_RE_OPT_IGNORECASE) flags |= PR_FLAGS_IGNORE_CASE;
            if (ropts & RB_RE_OPT_EXTENDED)   flags |= PR_FLAGS_EXTENDED;
            if (ropts & RB_RE_OPT_MULTILINE)  flags |= PR_FLAGS_MULTI_LINE;
        }
    }

    astrogre_pattern *p = astrogre_parse(pat, (size_t)pat_len, flags);
    if (!p) {
        rb_raise(rb_eArgError, "astrogre: failed to parse pattern: %.*s",
                 (int)pat_len, pat);
    }
    return TypedData_Wrap_Struct(rb_cPattern, &pattern_type, p);
}

/* ------------------------------------------------------------------ */
/* ASTrogre.native_compile(regex_or_string, flags = 0) → Pattern       */
/*                                                                     */
/* Like .compile but eagerly drives the AOT specializer so the result   */
/* dispatches through baked native code.  Equivalent to                 */
/*     pat = ASTrogre.compile(re); pat.aot_compile!; pat                */
/* in one call — convenient for the common "make this regex fast"      */
/* shape, and for tests that want to exercise the AOT path uniformly.   */
/* ------------------------------------------------------------------ */

static VALUE
m_native_compile(int argc, VALUE *argv, VALUE self)
{
    VALUE pat = m_compile(argc, argv, self);
    astrogre_pattern_aot_compile(unwrap(pat), false);
    return pat;
}

/* Helper: build a Ruby [start, end] pair, or nil when capture invalid. */
static VALUE
match_pair(astrogre_match_t *m, int idx)
{
    if (!m->valid[idx]) return Qnil;
    return rb_ary_new3(2, LONG2NUM((long)m->starts[idx]), LONG2NUM((long)m->ends[idx]));
}

/* ------------------------------------------------------------------ */
/* Pattern#match?(str) → bool                                          */
/* ------------------------------------------------------------------ */

static VALUE
pattern_match_p(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_match_t m;
    bool r = astrogre_search(unwrap(self), RSTRING_PTR(str), RSTRING_LEN(str), &m);
    return r ? Qtrue : Qfalse;
}

/* ------------------------------------------------------------------ */
/* Pattern#match(str) → [[start,end], [g1s,g1e], …] or nil             */
/* ------------------------------------------------------------------ */

static VALUE
pattern_match(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_match_t m;
    if (!astrogre_search(unwrap(self), RSTRING_PTR(str), RSTRING_LEN(str), &m))
        return Qnil;
    VALUE arr = rb_ary_new();
    for (int i = 0; i <= m.n_groups; i++) {
        rb_ary_push(arr, match_pair(&m, i));
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* Pattern#_match_at(str, start) → [[s,e], [g1s,g1e], …] | nil         */
/*                                                                     */
/* Resume the matcher at byte offset `start` without slicing the input,*/
/* so anchors like ^ and \A still see the surrounding context.  Used   */
/* by the Ruby `match_all` to enumerate matches with full captures.    */
/* ------------------------------------------------------------------ */

static VALUE
pattern_match_at(VALUE self, VALUE str, VALUE start)
{
    Check_Type(str, T_STRING);
    astrogre_match_t m;
    if (!astrogre_search_from(unwrap(self), RSTRING_PTR(str), RSTRING_LEN(str),
                              (size_t)NUM2LONG(start), &m))
        return Qnil;
    VALUE arr = rb_ary_new();
    for (int i = 0; i <= m.n_groups; i++) {
        rb_ary_push(arr, match_pair(&m, i));
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* Pattern#match_all(str) → array of group-0 [start,end] pairs         */
/* (non-overlapping enumeration, like String#scan but as positions).   */
/* ------------------------------------------------------------------ */

static VALUE
pattern_match_all(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_pattern *p = unwrap(self);
    const char *s = RSTRING_PTR(str);
    long len = RSTRING_LEN(str);
    VALUE arr = rb_ary_new();
    size_t pos = 0;
    astrogre_match_t m;
    while (astrogre_search_from(p, s, (size_t)len, pos, &m)) {
        rb_ary_push(arr, match_pair(&m, 0));
        pos = (m.ends[0] == m.starts[0]) ? m.ends[0] + 1 : m.ends[0];
        if (pos > (size_t)len) break;
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* Pattern#count(str) → integer                                        */
/* (number of non-overlapping matches in str).                          */
/* ------------------------------------------------------------------ */

static VALUE
pattern_count(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_pattern *p = unwrap(self);
    const char *s = RSTRING_PTR(str);
    long len = RSTRING_LEN(str);
    long n = 0;
    size_t pos = 0;
    astrogre_match_t m;
    while (astrogre_search_from(p, s, (size_t)len, pos, &m)) {
        n++;
        pos = (m.ends[0] == m.starts[0]) ? m.ends[0] + 1 : m.ends[0];
        if (pos > (size_t)len) break;
    }
    return LONG2NUM(n);
}

/* ------------------------------------------------------------------ */
/* Pattern#captures(str) → [[g1s,g1e], [g2s,g2e], …] or nil            */
/*                                                                     */
/* Mirrors Ruby's MatchData#captures: positional captures only, the    */
/* whole-match span (group 0) is excluded.  An optional group that     */
/* didn't participate in the match is reported as nil.  Returns nil if */
/* the pattern doesn't match the input at all.                         */
/* ------------------------------------------------------------------ */

static VALUE
pattern_captures(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_match_t m;
    if (!astrogre_search(unwrap(self), RSTRING_PTR(str), RSTRING_LEN(str), &m))
        return Qnil;
    VALUE arr = rb_ary_new_capa(m.n_groups);
    for (int i = 1; i <= m.n_groups; i++) {
        rb_ary_push(arr, match_pair(&m, i));
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* Pattern#named_captures(str) → { "name" => [s,e] | nil, … } or nil   */
/*                                                                     */
/* Mirrors Ruby's MatchData#named_captures.  Uses the (name, idx)      */
/* table populated by the parser; for patterns with no `(?<…>)` groups */
/* this is always an empty hash.  When two groups share a name, the    */
/* later one wins — matching Ruby's behaviour.                         */
/* ------------------------------------------------------------------ */

static VALUE
pattern_named_captures(VALUE self, VALUE str)
{
    Check_Type(str, T_STRING);
    astrogre_pattern *p = unwrap(self);
    astrogre_match_t m;
    if (!astrogre_search(p, RSTRING_PTR(str), RSTRING_LEN(str), &m))
        return Qnil;
    VALUE h = rb_hash_new();
    const int n = astrogre_pattern_n_named(p);
    for (int i = 0; i < n; i++) {
        int idx = 0;
        const char *name = astrogre_pattern_named_at(p, i, &idx);
        if (!name) continue;
        rb_hash_aset(h, rb_str_new_cstr(name), match_pair(&m, idx));
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Pattern#named_groups → { "name" => idx, … }                         */
/*                                                                     */
/* Static introspection of the pattern's named-group table; doesn't    */
/* require an input string.  Useful for tooling that wants to check    */
/* whether a name exists before running a match. */
/* ------------------------------------------------------------------ */

static VALUE
pattern_named_groups(VALUE self)
{
    astrogre_pattern *p = unwrap(self);
    VALUE h = rb_hash_new();
    const int n = astrogre_pattern_n_named(p);
    for (int i = 0; i < n; i++) {
        int idx = 0;
        const char *name = astrogre_pattern_named_at(p, i, &idx);
        if (!name) continue;
        rb_hash_aset(h, rb_str_new_cstr(name), INT2NUM(idx));
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Pattern#source → String                                             */
/* Original pattern bytes (between the slashes), like Regexp#source.   */
/* ------------------------------------------------------------------ */

static VALUE
pattern_source(VALUE self)
{
    size_t len = 0;
    const char *const s = astrogre_pattern_source(unwrap(self), &len);
    if (!s) return rb_str_new_cstr("");
    return rb_str_new(s, (long)len);
}

/* ------------------------------------------------------------------ */
/* Pattern#options → Integer                                           */
/* Bitmask matching Ruby's Regexp::IGNORECASE / MULTILINE / EXTENDED.  */
/* /x (extended) isn't tracked on the compiled pattern (it only        */
/* affects parsing) so it never appears in the result.                 */
/* ------------------------------------------------------------------ */

static VALUE
pattern_options(VALUE self)
{
    astrogre_pattern *const p = unwrap(self);
    int opts = 0;
    if (astrogre_pattern_case_insensitive(p)) opts |= RB_RE_OPT_IGNORECASE;
    if (astrogre_pattern_multiline(p))        opts |= RB_RE_OPT_MULTILINE;
    return INT2NUM(opts);
}

/* ------------------------------------------------------------------ */
/* Pattern#aot_compile!(verbose: false) → self                         */
/*                                                                     */
/* Drive the ASTro AOT specializer for this pattern: emit C source for  */
/* every reachable node, run `make` to build the shared object, dlopen   */
/* the result, and patch every node's dispatcher to its baked SD.       */
/* Persistent: future ASTrogre.compile in any process picks up the      */
/* cached SD automatically (see Init_astrogre_ext below).               */
/* ------------------------------------------------------------------ */

static VALUE
pattern_aot_compile_bang(int argc, VALUE *argv, VALUE self)
{
    VALUE verbose;
    rb_scan_args(argc, argv, "01", &verbose);
    bool v = !NIL_P(verbose) && RTEST(verbose);
    astrogre_pattern_aot_compile(unwrap(self), v);
    return self;
}

/* ------------------------------------------------------------------ */
/* Pattern#dump → S-expression of the AST (for diagnostics)            */
/* ------------------------------------------------------------------ */

static VALUE
pattern_dump(VALUE self)
{
    astrogre_pattern *p = unwrap(self);
    char *buf = NULL;
    size_t buflen = 0;
    FILE *fp = open_memstream(&buf, &buflen);
    if (!fp) rb_raise(rb_eRuntimeError, "open_memstream failed");
    astrogre_ext_dump_root(p, fp);
    fclose(fp);
    VALUE s = rb_str_new(buf, (long)buflen);
    free(buf);
    return s;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void
Init_astrogre_ext(void)
{
    /* Pin the source dir to where astrogre actually lives — INIT()
     * otherwise reads /proc/self/exe (= the ruby binary's path) and
     * astro_cs_compile would emit SD files #include'ing
     * "<ruby-bin-dir>/node.h" which doesn't exist.  ASTROGRE_SRC_DIR
     * is baked at build time by extconf.rb. */
#ifdef ASTROGRE_SRC_DIR
    setenv("ASTRO_CS_SRC_DIR", ASTROGRE_SRC_DIR, 0);
#endif
    INIT();   /* framework init: code-store probe, dispatcher table, … */

    rb_mASTrogre = rb_define_module("ASTrogre");
    rb_cPattern  = rb_define_class_under(rb_mASTrogre, "Pattern", rb_cObject);
    rb_undef_alloc_func(rb_cPattern);

    rb_define_module_function(rb_mASTrogre, "compile",        m_compile,        -1);
    rb_define_module_function(rb_mASTrogre, "native_compile", m_native_compile, -1);

    rb_define_method(rb_cPattern, "match?",                  pattern_match_p,          1);
    rb_define_method(rb_cPattern, "count",                   pattern_count,            1);
    rb_define_method(rb_cPattern, "named_groups",            pattern_named_groups,     0);
    rb_define_method(rb_cPattern, "source",                  pattern_source,           0);
    rb_define_method(rb_cPattern, "options",                 pattern_options,          0);
    rb_define_method(rb_cPattern, "aot_compile!",            pattern_aot_compile_bang, -1);
    rb_define_method(rb_cPattern, "dump",                    pattern_dump,             0);
    /* Low-level position-returning methods.  Public API in astrogre.rb
     * (match / captures / named_captures / match_all) wraps these to
     * return Ruby Regexp-compatible types (MatchData / strings / hash). */
    rb_define_method(rb_cPattern, "_match_offsets",          pattern_match,            1);
    rb_define_method(rb_cPattern, "_match_at_offsets",       pattern_match_at,         2);
    rb_define_method(rb_cPattern, "_match_all_offsets",      pattern_match_all,        1);
    rb_define_method(rb_cPattern, "_captures_offsets",       pattern_captures,         1);
    rb_define_method(rb_cPattern, "_named_captures_offsets", pattern_named_captures,   1);
}
