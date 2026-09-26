/* koruby_precise v2 — parse.c: prism AST → koruby NODE transduction (M0).
 *
 * Local variables use the one-stack model (v2_design §7.8): each lvar
 * access bakes a negative cursor offset
 *
 *     off = index - locals_cnt - chain
 *
 * where `chain` is the staging depth at that program point (the sum of
 * slot_counts of the enclosing dispatchers within the current frame).
 * `chain` is known during transduction; `locals_cnt` only at scope end, so
 * nodes bake (index - chain) and the frame-pop fixup subtracts locals_cnt.
 *
 * Cached structural hashes stay correct because nothing hashes a NODE
 * before its frame is popped: main.c calls INIT() (which dlopens the code
 * store) only after PARSE, so the OPTIMIZE call inside every ALLOC is a
 * no-op during parsing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "prism.h"
#include "node.h"

uint32_t koruby_toplevel_locals_cnt = 0;
const uint32_t *koruby_toplevel_local_syms = NULL;
uint32_t koruby_toplevel_local_cnt = 0;

struct kp_frame {
    const pm_constant_id_list_t *locals;
    uint32_t bake_base;
    int32_t saved_chain;
    uint32_t synth_cnt;       /* synthetic temporaries appended after prism locals (e.g. case subject) */
    bool uses_block;          /* yield / block_given? seen → reserve 2 frame-top cells */
    bool uses_backref;        /* the body can set `$~` (regexp literal / =~ / a matching method
                               * name) → the invoke path saves and restores the caller's */
    bool it_param;            /* `{ it * 2 }` — slot 0 is the implicit `it` (prism lists no name for it) */
    bool module_function_mode; /* no-arg `module_function` seen in this class/module body */
    uint32_t method_mid;      /* enclosing def's name (0 = not a method body) — for super */
    bool dm_body;             /* this block frame is a define_method body: it IS a method at run time,
                               * so a `super` inside it names the entry being executed, not any def
                               * it happens to sit inside lexically. */
    uint32_t class_name_sym;  /* enclosing class/module body's const name (0 = not a class body) — for Module.nesting */
    uint32_t body_label;      /* class/module/singleton body: its backtrace label (0 = not such a body) — see kp_block_label */
    bool block_body;          /* a block/lambda body: `self` inside it can have been rebound (Class.new,
                               * class_eval), so a `class X` here nests by LEXICAL scope, not by self. */
    bool anon_class_body;     /* `class << obj` — the cref is a class with no name, so constants must be
                               * owned via the runtime self instead of a baked name */
    bool anon_cref_method;    /* a method defined in such a body: its cref is that unnamed class, which only
                               * the frame's method-entry cell can name at runtime */
    uint32_t method_params;   /* enclosing def's positional param count — for forwarding super */
    bool depth_shift;         /* END { } body: prism resolved its locals against the ENCLOSING scope
                               * (depth 0), but the body runs as a closure, so every reference is one
                               * level out. */
    bool lambda_body;         /* `-> { }` / `lambda { }` body: a `return` boundary like a method, so the
                               * walk for a non-local return stops here. */
    int32_t  fwd_slot;        /* `def m(...)` → synth rest local holding all positional args (-1 = none) */
    int32_t  fwd_blk_slot;    /* `def m(...)` → synth local holding the caller's block as a Proc (-1 = none) */
    int32_t  anon_rest_slot;  /* `def m(*)` → synth local holding the anonymous rest; bare `*` forwards it (-1 = none) */
    int32_t  anon_blk_slot;   /* `def m(&)` → synth local holding the block as a Proc; bare `&` forwards it (-1 = none) */
    int32_t  anon_kwrest_slot;/* `def m(**)` → synth local holding the collected keywords; bare `**` forwards it (-1 = none) */
    int32_t  method_rest_slot;/* `def m(*rest)` → the rest param's local slot, for forwarding super (-1 = none) */
    int32_t  method_post_base;/* `def m(*rest, a, b)` → first post-param slot, for forwarding super (-1 = none) */
    uint32_t method_post_cnt; /* number of post params (params after *rest), for forwarding super */
    struct korb_kw_info *method_kw_info; /* `def m(k:, ...)` → keyword params, for forwarding super (NULL = none) */
    uint32_t max_ref_depth;   /* B3: deepest outer-scope depth this block's body reads (0=none) */
    /* B3: a closure normally captures only this frame's DECLARED locals (synth
     * temps are compiler scratch nobody can name).  `...` forwarding breaks that
     * — its args Array and block Proc live in synth slots that a block inside
     * the body genuinely reads — so a read like that raises the floor here. */
    uint32_t cap_min;
    uint32_t yield_depth;     /* B3: env-link depth of the METHOD scope this body yields to (0=none).
                               * Its block lives in the frame-top trio, outside the captured locals,
                               * so an escaping closure must reify it before that frame returns. */
    int32_t **add_cells;      /* yield-in-block trio-index cells: fixed up by += frame_size at pop */
    uint32_t add_cnt, add_capa;
    uint32_t flink_base;      /* first kp_ctx.flink_list entry belonging to this frame */
    struct kp_frame *prev;
};

/* `{ it }` binds the implicit parameter at slot 0 and prism lists no local for
 * it, so every prism local of that frame sits one slot higher. */
static inline uint32_t kp_it_shift(const struct kp_frame *const f) { return (f && f->it_param) ? 1u : 0u; }

/* A call node's baked frame link: the distance it carries is cursor → the
 * CALLER's base, i.e. chain + locals_cnt (node.h).  Only `chain` is known while
 * transducing, so the cell is registered here and pop_frame folds in the frame
 * size — the same deferral the lvar offsets use (bake_list). */
struct kp_flink_fix {
    uint64_t *cell;           /* the node's flink operand */
    uint32_t  line;
    uint32_t  staged;         /* the part of the distance known at parse time (chain [+ the node's own advance]) */
};

/* One multi-assign target whose receiver (and index arguments) were evaluated
 * up front, into synth locals — CRuby's left-to-right order. */
struct kp_hoist {
    const pm_node_t *t;
    int32_t recv;
    int32_t *args;
    uint32_t nargs;
};

struct kp_ctx {
    pm_parser_t *parser;
    CTX *c;
    const char *fname;
    struct kp_frame *frame;
    struct kp_hoist *hoists;  /* multi-assign receiver hoists, by target node */
    uint32_t hoist_cnt, hoist_capa;
    bool hoisting;            /* inside the outermost massign_general's pre-pass */
    bool next_block_is_dm;    /* the literal block about to be transduced is a define_method body */
    int32_t chain;            /* staging depth at the current program point */
    /* BEGIN { } bodies, hoisted to the very front of the program (CRuby runs
     * them once, before everything else — including a -n/-p gets loop). */
    NODE   **pre_list;
    uint32_t pre_cnt, pre_capa;
    int32_t **bake_list;      /* lvar-offset cells awaiting locals_cnt fixup */
    uint32_t bake_cnt, bake_capa;
    struct kp_flink_fix *flink_list;   /* call-node frame links awaiting the same fixup (kp_flink) */
    uint32_t flink_cnt, flink_capa;
    bool syntax_error;        /* transduce-time SyntaxError (e.g. binding in alternative pattern) */
    bool pending_depth_shift; /* the next pushed frame is an END { } body (see kp_frame.depth_shift) */
    bool pending_lambda_body; /* the next pushed frame is a lambda body (see kp_frame.lambda_body) */
    uint8_t src_enc;          /* KORB_ENC_* for this file's string literals (from the magic comment) */
    bool fsl_false;           /* an explicit `# frozen_string_literal: false` — literals are plain, not chilled */
    bool fsl_true;            /* an explicit `# frozen_string_literal: true` (or --enable-frozen-string-literal).
                               * prism reports it per StringNode, but NOT on an InterpolatedStringNode whose parts
                               * are all literal (a `<<~` heredoc), so that case reads the file's setting here. */
    const char *syntax_err;   /* a compile-time error found while lowering (bad Regexp literal): parse fails */
    uint32_t eval_aliases[8]; /* `alias x eval`: names that get eval's hidden-binding lowering too */
    uint32_t eval_alias_cnt;
    uint32_t eval_method_mid; /* eval(str, binding): the method the BINDING was taken in, so `__method__`
                               * inside the string answers it (CRuby); 0 = none / not an eval. */
    /* class_eval/module_eval(String): the receiver's lexical nesting, seeded into
     * the program frame so a `def` inside the string resolves constants the way a
     * `module Foo` body would (CRuby pushes the receiver onto the caller's cref).
     * `seed_class_name` is the receiver itself; `seed_chain` is what encloses it,
     * outermost first. */
    /* The seeded cref names the CALLER's scope, not the eval'd one: an ANONYMOUS
     * class_eval receiver falls back to it so constants in the source still
     * resolve, but a `binding` the source takes must NOT report it as its cref
     * (pry's Object#__binding__ is `class_eval("binding")` on a Class.new). */
    bool seed_cref_is_caller;
    uint32_t seed_class_name;
    const uint32_t *seed_chain;
    uint32_t seed_chain_len;
    bool in_eval;             /* eval(String): the program's namespace is the cref the eval
                               * installed, never the running self (it can be rebound). */
    /* eval under a binding: the binding's lexical levels, nearest first.  They sit
     * outside the program frame for LOCAL RESOLUTION only (kp_frame_at); the
     * program frame's prev stays NULL, so method / block / class walks still stop
     * at the eval's own top.  Caller locals are then depth >= 1 references into
     * the binding's env chain, which the eval frame's EP points at. */
    struct kp_frame *eval_outer;
    uint32_t eval_outer_cnt;
    /* while building a pattern descriptor: how far above the match node's slots
     * the matcher evaluates this level's value nodes (korb_pat_match: cur =
     * base + 1, each nested element level + 2) */
    int32_t pat_cur;
};

/* `alias x eval` seen earlier in this file: x is lowered like eval itself. */
static bool
kp_is_eval_alias(const struct kp_ctx *tc, pm_constant_id_t name)
{
    for (uint32_t i = 0; i < tc->eval_alias_cnt; i++)
        if (tc->eval_aliases[i] == (uint32_t)name) return true;
    return false;
}

/* Is there an `# encoding:` magic comment where CRuby looks for one? */
static bool
kp_enc_comment_first_line(const char *const src, const size_t len)
{
    size_t i = 0;
    if (len >= 2 && src[0] == '#' && src[1] == '!') {          /* a shebang lets it slip to line 2 */
        while (i < len && src[i] != '\n') i++;
        if (i < len) i++;
    }
    if (i >= len || src[i] != '#') return false;
    size_t eol = i;
    while (eol < len && src[eol] != '\n') eol++;
    /* `coding` — with any prefix (encoding:, fileencoding=, -*- coding: -*-) —
     * followed by ':' or '=' is what CRuby's parser_magic_comment accepts. */
    for (size_t k = i; k + 6 <= eol; k++) {
        if (strncasecmp(src + k, "coding", 6) != 0) continue;   /* `# CODING:` is accepted too */
        size_t t = k + 6;
        while (t < eol && (src[t] == ' ' || src[t] == '\t')) t++;
        if (t < eol && (src[t] == ':' || src[t] == '=')) return true;
    }
    return false;
}

/* The source's `# encoding:` magic comment, as prism resolved it, mapped onto the
 * string encoding tag (a name koruby has no tag for is registered on the spot;
 * the literal's bytes are unaffected either way). */
static uint8_t
kp_src_enc(CTX *c, const pm_parser_t *parser, const char *const src, const size_t len, const char *const seed)
{
    /* prism honours an `# encoding:` magic comment anywhere in the leading
     * comment block; CRuby only on the first line (second after a shebang), so
     * the position is checked here.  `seed` is the encoding the source already
     * had (eval's String); NULL = the parser default. */
    const char *const name = (parser->encoding && kp_enc_comment_first_line(src, len)) ? parser->encoding->name : NULL;
    /* -K<letter> sets the script encoding too, unless a magic comment already
     * picked one. */
    if (OPTION.kcode && (name == NULL || strcmp(name, "UTF-8") == 0))
        return (uint8_t)korb_enc_index_pub(c->vm, OPTION.kcode);
    if (name == NULL) return seed ? (uint8_t)korb_enc_index_pub(c->vm, seed) : KORB_ENC_UTF8;
    return (uint8_t)korb_enc_index_pub(c->vm, name);
}

/* Evaluate BODY (allocations / transduction of the children of a node
 * whose dispatcher claims `n_slots` staging slots).  The dispatcher
 * advances the cursor by slot_count before evaluating ANY operand, so all
 * child subtrees see chain + n_slots. */
#define WITH_CHAIN(tc, n_slots, BODY) ({ \
    int32_t _saved = (tc)->chain;        \
    (tc)->chain = _saved + (int32_t)(n_slots); \
    __typeof__(BODY) _r = (BODY);        \
    (tc)->chain = _saved;                \
    _r; \
})

static NODE *transduce(struct kp_ctx *tc, const pm_node_t *node);
static bool kp_kernel_const_p(struct kp_ctx *tc, const pm_node_t *recv);
static NODE *build_const_set(struct kp_ctx *tc, uint32_t name_cid, NODE *val);
static NODE *index_opassign_splat(struct kp_ctx *tc, const pm_index_operator_write_node_t *iw, const pm_node_t *node,
                                  const pm_arguments_node_t *args, const pm_node_t *value);

/* node_send takes a parse-time NODE* array [recv, args...]; these build the
 * common small-arity synthetic sends used throughout the parser (op-assign,
 * []/[]= desugar, case/when ===, ...).  The staging depth a caller must reserve
 * is the element count (recv + args). */
/* Frame link baked into a call node (node.h): the call site line plus the
 * distance from the cell the link ends up in down to THIS frame's BASE.  Only
 * the staging part is known here; pop_frame adds the frame size through the
 * KP_LINK registration below.  Read tc->chain after the children are transduced
 * (cursor restored).
 * `staged` is where that cell sits above the node's own cursor: for a node that
 * hands the link to the dispatch (korb_flink_stage) it is the dispatcher's own
 * advance, for one that stores it into the callee's base[-3] it is where that
 * cell lands. */
static uint64_t kp_flink_at(const struct kp_ctx *tc, uint32_t line, uint32_t staged) {
    const uint32_t d = (uint32_t)(tc->chain + (int32_t)staged);
    if (d >= KORB_FLINK_DIST_MAX) return 0;   /* no link at all, rather than one aimed short */
    return korb_flink_make(line, d);
}
/* A @framehdr node: its dispatcher reserves the header, so base[-3] lands
 * KORB_FRAME_HDR - 2 cells above the cursor. */
static uint64_t kp_flink(const struct kp_ctx *tc, uint32_t line) { return kp_flink_at(tc, line, KORB_FRAME_HDR - 2); }

/* The backtrace label of a class / module / singleton-class BODY frame
 * (node_entry.body_label).  CRuby's is lexical — `class A::B` is "<class:B>" —
 * so it is fully known here; `kind` is 'c', 'm' or 's'. */
static uint32_t kp_body_label(const struct kp_ctx *tc, char kind, uint32_t name_sym)
{
    if (kind == 's') return korb_intern(tc->c->vm, "singleton class", 15);
    char buf[288];
    const int len = snprintf(buf, sizeof buf, "<%s:%s>", kind == 'c' ? "class" : "module",
                             korb_sym_name(tc->c->vm, name_sym));
    return korb_intern(tc->c->vm, buf, (size_t)(len >= (int)sizeof buf ? (int)sizeof buf - 1 : len));
}

/* The label of a block whose lexical chain reaches a class / module / singleton
 * body through blocks only (node_entry.blk_label).  0 = not such a block: the
 * owner is a method, whose label only the run-time frame chain knows.  Called
 * after the block's own frame is popped, so tc->frame is the scope it was
 * written in — level 1.
 *
 * `*top_level` is set when the chain reaches the FILE's top level instead: the
 * depth is lexical, but the owner's name is not (node.def blk_top_level). */
static uint32_t kp_block_label(const struct kp_ctx *tc, uint32_t *const top_level)
{
    uint32_t level = 1;
    *top_level = 0;
    for (const struct kp_frame *f = tc->frame; f != NULL; f = f->prev, level++) {
        if (f->body_label != 0) {
            char buf[352];
            const char *const owner = korb_sym_name(tc->c->vm, f->body_label);
            const int len = (level == 1) ? snprintf(buf, sizeof buf, "block in %s", owner)
                                         : snprintf(buf, sizeof buf, "block (%u levels) in %s", level, owner);
            return korb_intern(tc->c->vm, buf, (size_t)(len >= (int)sizeof buf ? (int)sizeof buf - 1 : len));
        }
        if (!f->block_body) {
            if (f->prev != NULL) return 0;   /* a method body: the owner is a run-time question */
            *top_level = level;              /* the program scope (PM_PROGRAM_NODE is the root frame) */
            return 0;
        }
    }
    return 0;                                /* unreachable: the program frame ends every chain */
}

/* A constant read bakes fb_off = THIS frame's base: a miss dispatches
 * const_missing, and the hook's `caller` must start at the reference site. */
#define KP_CONST(tc, ...) __extension__({ NODE *const _cn = ALLOC_node_const(__VA_ARGS__, -(tc)->chain); \
                                          bake_add((tc), &_cn->u.node_const.fb_off); _cn; })
#define KP_CONST_PATH(tc, NAME, LINE, PARENT) __extension__({ \
    NODE *const _cp = ALLOC_node_const_path((NAME), (LINE), -(tc)->chain, (PARENT)); \
    bake_add((tc), &_cp->u.node_const_path.fb_off); _cp; })

/* Register a just-built call node's link for the frame-size fixup.  KP_LINK
 * wraps the ALLOC so the two can never drift apart. */
static void kp_flink_add(struct kp_ctx *tc, uint64_t *cell);
#define KP_LINK(tc, KIND, ...) ({ NODE *const _ln = ALLOC_##KIND(__VA_ARGS__); \
                                  kp_flink_add((tc), &_ln->u.KIND.flink); _ln; })

/* synthetic sends target public methods ([]/===/+/...), so self_off = INT32_MIN
 * disables the private/protected visibility guard (no caller-self needed). */
static NODE *kp_send0(struct kp_ctx *tc, uint32_t mid, uint32_t line, NODE *recv) {
    NODE **argv = malloc(sizeof(NODE *)); if (!argv) abort();
    argv[0] = recv;
    return KP_LINK(tc, node_send, mid, kp_flink(tc, line), INT32_MIN, argv, 1);
}
static NODE *kp_send1(struct kp_ctx *tc, uint32_t mid, uint32_t line, NODE *recv, NODE *a0) {
    NODE **argv = malloc(sizeof(NODE *) * 2); if (!argv) abort();
    argv[0] = recv; argv[1] = a0;
    return KP_LINK(tc, node_send, mid, kp_flink(tc, line), INT32_MIN, argv, 2);
}
static NODE *kp_send2(struct kp_ctx *tc, uint32_t mid, uint32_t line, NODE *recv, NODE *a0, NODE *a1) {
    NODE **argv = malloc(sizeof(NODE *) * 3); if (!argv) abort();
    argv[0] = recv; argv[1] = a0; argv[2] = a1;
    return KP_LINK(tc, node_send, mid, kp_flink(tc, line), INT32_MIN, argv, 3);
}
/* recv.mid(args[0..n)) — n args of any count (recv + n → n+1 children). */
static NODE *kp_send_n(struct kp_ctx *tc, uint32_t mid, uint32_t line, NODE *recv, NODE *const *args, uint32_t n) {
    NODE **argv = malloc(sizeof(NODE *) * (1u + n)); if (!argv) abort();
    argv[0] = recv;
    for (uint32_t i = 0; i < n; i++) argv[1u + i] = args[i];
    return KP_LINK(tc, node_send, mid, kp_flink(tc, line), INT32_MIN, argv, 1u + n);
}
/* Staging depth a caller must reserve to build a kp_sendN node.  node_send is
 * @framehdr, so its dispatcher advances the cursor by (children + KORB_FRAME_HDR)
 * — the reserved meta cells (identity/link) sit below the staged recv/args.  Any
 * WITH_CHAIN that builds the recv/args of a kp_sendN MUST use these so the
 * parse-time chain bump matches the runtime cursor advance (else the children's
 * baked lget offsets are off by KORB_FRAME_HDR and read the wrong slot). */
#define KP_SEND0_SC (1u + KORB_FRAME_HDR)   /* recv only            */
#define KP_SEND1_SC (2u + KORB_FRAME_HDR)   /* recv + 1 arg         */
#define KP_SEND2_SC (3u + KORB_FRAME_HDR)   /* recv + 2 args        */
#define KP_SENDN_SC(n) ((1u + (n)) + KORB_FRAME_HDR)   /* recv + n args */

/* True when every argument has a fixed shape — no `*splat` and no `...`.  Only
 * then may a call be lowered to a fixed-arity node (binop / aref / aset). */
static bool kp_no_splat_p(struct pm_node *const *nodes, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (PM_NODE_TYPE_P(nodes[i], PM_SPLAT_NODE) || PM_NODE_TYPE_P(nodes[i], PM_FORWARDING_ARGUMENTS_NODE))
            return false;
    return true;
}

/* ---------------------------------------------------------------------- */

static uint32_t
kp_line(struct kp_ctx *tc, const pm_node_t *node)
{
    const int32_t line = pm_newline_list_line(&tc->parser->newline_list,
                                             node->location.start,
                                             tc->parser->start_line);
    return (uint32_t)line;                  /* eval can set a negative first line: kept as-is, printed signed */
}

/* Ruby 4.1 #source_range: register the full extent of a def/block/lambda node.
 * Columns are 0-based BYTE offsets into their line (what the spec asks for, and
 * what prism counts), and the end is exclusive — location.start + length is the
 * first byte past the construct, so its column is already the exclusive end. */
static void
kp_reg_srcrange(struct kp_ctx *tc, NODE *n, const pm_node_t *node)
{
    const pm_newline_list_t *const nl = &tc->parser->newline_list;
    const int32_t sl = tc->parser->start_line;
    const pm_line_column_t beg = pm_newline_list_line_column(nl, node->location.start, sl);
    const pm_line_column_t end = pm_newline_list_line_column(nl, node->location.end, sl);
    korb_reg_srcrange(tc->c->vm, n, beg.column, (uint32_t)end.line, end.column);
}

static __attribute__((noreturn)) void
kp_failf(struct kp_ctx *tc, const pm_node_t *node, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u: ", tc->fname, node ? kp_line(tc, node) : 0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Subset boundary: emit a node that raises NotImplementedError when (if)
 * this program point is reached.  Everything else in the file keeps
 * working — rubyharness scores per line, so a parse-time exit here would
 * zero whole corpus files over one exotic construct. */
static NODE *
kp_unsupported(struct kp_ctx *tc, const pm_node_t *node, const char *what)
{
    (void)tc;
    return ALLOC_node_unsupported(what, node ? kp_line(tc, node) : 0);
}

/* constant_id → interned (cstr, len) via the parser's constant pool */
static uint32_t
kp_intern_cid(struct kp_ctx *tc, pm_constant_id_t cid)
{
    pm_constant_t *ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);
    return korb_intern(tc->c->vm, (const char *)ct->start, ct->length);
}

/* Full dotted name of a constant-path parent ("Net::HTTP" for `class Net::HTTP::Get`),
 * interned as one symbol.  The rightmost component alone is ambiguous — URI::HTTP
 * and Net::HTTP both end in "HTTP" — so the whole path is baked and walked at run
 * time by korb_const_get_path.  Returns 0 when the parent is not a static path. */
static uint32_t
kp_intern_cpath(struct kp_ctx *tc, const pm_node_t *node)
{
    const pm_node_t *chain[32];
    uint32_t n = 0;
    for (const pm_node_t *p = node; p != NULL && n < 32; ) {
        if (PM_NODE_TYPE_P(p, PM_CONSTANT_READ_NODE)) { chain[n++] = p; break; }
        if (!PM_NODE_TYPE_P(p, PM_CONSTANT_PATH_NODE)) return 0;      /* dynamic parent */
        chain[n++] = p;
        p = ((const pm_constant_path_node_t *)p)->parent;
    }
    if (n == 0 || n == 32) return 0;
    char buf[512];
    uint32_t len = 0;
    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        const pm_constant_id_t cid = PM_NODE_TYPE_P(chain[i], PM_CONSTANT_READ_NODE)
            ? ((const pm_constant_read_node_t *)chain[i])->name
            : ((const pm_constant_path_node_t *)chain[i])->name;
        pm_constant_t *const ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);
        if (len + ct->length + 2 >= sizeof buf) return 0;
        if (len) { buf[len++] = ':'; buf[len++] = ':'; }
        memcpy(buf + len, ct->start, ct->length);
        len += (uint32_t)ct->length;
    }
    return korb_intern(tc->c->vm, buf, len);
}

/* ---- `alias $NEW $OLD` — parse 時の名前 alias (process-wide、English.rb 用) ----
 * gvar は const table 流用なので、NEW の read/write を OLD の read/write として
 * transduce すれば常に live な alias になる。$! / $& 等の特例 read も解決後の
 * 名前で既存分岐に落ちるため自動で正しく合成される。 */
static struct { uint32_t nw, old; } *kp_gvar_aliases = NULL;
static uint32_t kp_gvar_alias_cnt = 0, kp_gvar_alias_cap = 0;

static void kp_gvar_alias_seed(struct kp_ctx *tc);   /* fwd */
static uint32_t
kp_gvar_resolve(uint32_t id)
{
    for (uint32_t hops = 0; hops < 8; hops++) {        /* 転送の連鎖 (循環は打ち切り) */
        uint32_t next = id;
        for (uint32_t i = 0; i < kp_gvar_alias_cnt; i++)
            if (kp_gvar_aliases[i].nw == id) { next = kp_gvar_aliases[i].old; break; }
        if (next == id) return id;
        id = next;
    }
    return id;
}



/* English.rb の標準 alias 集合を先付け seed する。alias は parse 時解決なので、
 * 同一ファイル内で `require "English"` → 即 $ERROR_INFO 使用というパターンに
 * 対応するには、require の実行を待たず最初から解決できる必要がある。
 * (English.rb 本体の alias 文は同じ内容の再登録になるだけ) */
static void kp_gvar_alias_add(uint32_t nw, uint32_t old);
static void
kp_gvar_alias_seed(struct kp_ctx *tc)
{
    static bool seeded = false;
    if (seeded) return;
    seeded = true;
    static const char *const pairs[][2] = {
        { "$ERROR_INFO", "$!" }, { "$ERROR_POSITION", "$@" },
        { "$FS", "$;" }, { "$FIELD_SEPARATOR", "$;" },
        { "$OFS", "$," }, { "$OUTPUT_FIELD_SEPARATOR", "$," },
        { "$RS", "$/" }, { "$INPUT_RECORD_SEPARATOR", "$/" },
        { "$ORS", "$\\" }, { "$OUTPUT_RECORD_SEPARATOR", "$\\" },
        { "$INPUT_LINE_NUMBER", "$." }, { "$NR", "$." },
        { "$LAST_READ_LINE", "$_" }, { "$DEFAULT_OUTPUT", "$>" },
        { "$DEFAULT_INPUT", "$<" }, { "$PID", "$$" }, { "$PROCESS_ID", "$$" },
        { "$CHILD_STATUS", "$?" }, { "$LAST_MATCH_INFO", "$~" },
        { "$IGNORECASE", "$=" }, { "$ARGV", "$*" }, { "$MATCH", "$&" },
        { "$PREMATCH", "$`" }, { "$POSTMATCH", "$'" },
        { "$LAST_PAREN_MATCH", "$+" }, { "$LOADED_FEATURES", "$\"" },
        { "$PROGRAM_NAME", "$0" },
        /* $> IS $stdout in CRuby (one slot), so `$stdout = io` must retarget it —
         * pp.rb's `def PP.pp(obj, out=$>)` picks up a replaced $stdout that way. */
        { "$>", "$stdout" },
        /* command-line switch aliases (CRuby exposes these as the same slot) */
        { "$-d", "$DEBUG" }, { "$-v", "$VERBOSE" }, { "$-w", "$VERBOSE" },
        { "$-I", "$LOAD_PATH" }, { "$:", "$LOAD_PATH" },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        kp_gvar_alias_add(korb_intern(tc->c->vm, pairs[i][0], (uint32_t)strlen(pairs[i][0])),
                          korb_intern(tc->c->vm, pairs[i][1], (uint32_t)strlen(pairs[i][1])));
}

static void
kp_gvar_alias_add(uint32_t nw, uint32_t old)
{
    for (uint32_t i = 0; i < kp_gvar_alias_cnt; i++)
        if (kp_gvar_aliases[i].nw == nw) { kp_gvar_aliases[i].old = old; return; }
    if (kp_gvar_alias_cnt == kp_gvar_alias_cap) {
        kp_gvar_alias_cap = kp_gvar_alias_cap ? kp_gvar_alias_cap * 2 : 32;
        kp_gvar_aliases = realloc(kp_gvar_aliases, sizeof(*kp_gvar_aliases) * kp_gvar_alias_cap);
        if (!kp_gvar_aliases) abort();
    }
    kp_gvar_aliases[kp_gvar_alias_cnt].nw = nw;
    kp_gvar_aliases[kp_gvar_alias_cnt].old = old;
    kp_gvar_alias_cnt++;
}

static const char *
kp_cid_cstr(struct kp_ctx *tc, pm_constant_id_t cid)
{
    return korb_sym_name(tc->c->vm, kp_intern_cid(tc, cid));
}

/* `$& = 1` and friends (incl. via an alias): CRuby raises NameError naming the
 * variable AS WRITTEN, so the check happens here where that name is known.
 * Returns the raising node, or NULL when the target is writable. */
static NODE *
kp_gvar_readonly_write(struct kp_ctx *tc, const pm_node_t *at, pm_constant_id_t cid)
{
    const uint32_t resolved = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, cid));
    const char *const r = korb_sym_name(tc->c->vm, resolved);
    bool ro = (r[0] == '$' && r[1] != '\0' && r[2] == '\0' &&
               (r[1] == '&' || r[1] == '`' || r[1] == '\'' || r[1] == '+' || r[1] == '!' ||
                r[1] == ':' || r[1] == '"' || r[1] == '<' || r[1] == '?' ||
                (r[1] >= '1' && r[1] <= '9')));
    if (!ro) {                                           /* the multi-char read-only names */
        /* the prelude reaches these through __set_gvar, which bypasses the check */
        static const char *const ro_names[] = { "$-a", "$-l", "$-p", "$LOADED_FEATURES",
                                                "$LOAD_PATH", "$FILENAME", "$:", "$-I", NULL };
        for (uint32_t i = 0; ro_names[i]; i++) if (strcmp(r, ro_names[i]) == 0) { ro = true; break; }
    }
    if (!ro) return NULL;
    const char *const written = kp_cid_cstr(tc, cid);
    char *msgname = malloc(strlen(written) + 1);         /* immortal (baked into the node) */
    if (!msgname) abort();
    strcpy(msgname, written);
    return ALLOC_node_readonly_gvar(msgname, kp_line(tc, at));
}



/* ---- frames + lvar offset bake ---------------------------------------- */

static void
push_frame(struct kp_ctx *tc, const pm_constant_id_list_t *locals)
{
    struct kp_frame *f = malloc(sizeof(*f));
    if (!f) abort();
    f->locals = locals;
    f->bake_base = tc->bake_cnt;
    f->flink_base = tc->flink_cnt;
    f->saved_chain = tc->chain;
    f->synth_cnt = 0;
    f->uses_block = false;
    f->uses_backref = false;
    f->it_param = false;
    f->module_function_mode = false;
    f->method_mid = 0;
    f->dm_body = false;
    f->method_params = 0;
    f->depth_shift = tc->pending_depth_shift;
    f->lambda_body = tc->pending_lambda_body;
    tc->pending_lambda_body = false;   /* single-use: a block nested in the lambda body is not one */
    f->fwd_slot = -1;
    f->fwd_blk_slot = -1;
    f->cap_min = 0;
    f->anon_rest_slot = -1;
    f->anon_blk_slot = -1;
    f->anon_kwrest_slot = -1;
    f->method_rest_slot = -1;
    f->method_post_base = -1;
    f->method_post_cnt = 0;
    f->method_kw_info = NULL;
    f->class_name_sym = 0;
    f->body_label = 0;
    f->anon_class_body = false;
    f->block_body = false;
    f->anon_cref_method = false;
    f->max_ref_depth = 0;
    f->yield_depth = 0;
    f->add_cells = NULL;
    f->add_cnt = f->add_capa = 0;
    f->prev = tc->frame;
    tc->frame = f;
    tc->chain = 0;
}

/* A body that consults the frame's definee (nested `def`, class/module body,
 * `class << x`, `@@var`) must not take the streamlined is_simple invoke: that
 * path skips the save/restore, so an enclosing instance_eval's definee leaks
 * into the callee.  uses_block is the existing "not is_simple" channel and
 * reserves the frame cells to match.  Only the enclosing method frame matters:
 * class/sclass bodies and blocks are entered through paths that already
 * save/restore it. */
/* Mark the enclosing METHOD frame as "can set $~".  Ruby scopes the backref to
 * the method frame, so only such a method has to save/restore the caller's —
 * everything else cannot clobber it.  Blocks belong to their enclosing method
 * (CRuby shares the frame), which is why the walk goes out to it. */
static void kp_note_backref(struct kp_ctx *tc) {
    struct kp_frame *mf = tc->frame;
    while (mf && mf->method_mid == 0 && !mf->dm_body && mf->prev) mf = mf->prev;
    if (mf) mf->uses_backref = true;
}
/* Method names whose builtin sets `$~` (CRuby sets the backref in the CALLER's
 * frame from a C method, so a Ruby method that calls one must own the save).
 * A regexp literal in the same body already marks it; this catches the shapes
 * that pass a Regexp around in a variable. */
static void kp_note_backref_call(struct kp_ctx *tc, const char *name) {
    static const char *const setters[] = {
        "=~", "!~", "match", "scan", "gsub", "gsub!", "sub", "sub!", "split",
        "grep", "grep_v", "index", "rindex", "partition", "rpartition",
        "last_match", "start_with?", "end_with?", "slice", "slice!", NULL,
    };
    for (uint32_t i = 0; setters[i]; i++)
        if (strcmp(name, setters[i]) == 0) { kp_note_backref(tc); return; }
}
static void kp_needs_definee(struct kp_ctx *tc) {
    struct kp_frame *mf = tc->frame;
    while (mf->method_mid == 0 && mf->prev) mf = mf->prev;
    if (mf->method_mid != 0) mf->uses_block = true;
}

/* the innermost lexically-enclosing class/module const name (0 = top-level) —
 * the cref for owner-aware bare constant reads (walk the frame chain). */
static uint32_t kp_cref_owner(struct kp_ctx *tc) {
    for (struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0) return f->class_name_sym;
    return 0;
}

/* The lexically-enclosing class/module as a full constant path ("Outer::A").
 * kp_cref_owner answers only the innermost name, which any same-named module in
 * the flat constant table also answers to — so a `binding` taken in Outer::A#m
 * used to put `eval "class C; end"` into Other::A.  0 = no enclosing class. */
static uint32_t
kp_cref_owner_path(struct kp_ctx *tc)
{
    uint32_t syms[32], depth = 0;
    for (const struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0) depth++;
    if (depth == 0) return 0;
    if (depth > 32) return kp_cref_owner(tc);
    uint32_t i = depth;                               /* fill innermost→outermost, store outermost at [0] */
    for (const struct kp_frame *f = tc->frame; f && i > 0; f = f->prev)
        if (f->class_name_sym != 0) syms[--i] = f->class_name_sym;
    char buf[512];
    size_t len = 0;
    for (uint32_t k = 0; k < depth; k++) {
        const char *const nm = korb_sym_name(tc->c->vm, syms[k]);
        const size_t nl = strlen(nm);
        if (len + nl + 3 > sizeof buf) return kp_cref_owner(tc);
        if (len) { buf[len++] = ':'; buf[len++] = ':'; }
        memcpy(buf + len, nm, nl);
        len += nl;
    }
    return korb_intern(tc->c->vm, buf, (uint32_t)len);
}

/* Register `cell` to be fixed up by `+= frame_size` when frame `f` pops — for a
 * yield-in-block trio index baked relative to the (ancestor) method frame's
 * size (the block trio sits at method node[frame_size-5..-3]). */
static void
add_bake_to(struct kp_frame *f, int32_t *cell)
{
    if (f->add_cnt == f->add_capa) {
        f->add_capa = f->add_capa ? f->add_capa * 2 : 4;
        f->add_cells = realloc(f->add_cells, sizeof(int32_t *) * f->add_capa);
        if (!f->add_cells) abort();
    }
    f->add_cells[f->add_cnt++] = cell;
}

/* Returns the frame size.  The frame is exactly its locals — self / EP / link /
 * identity all live BELOW the base (node.h) — plus, for a yielding frame, the
 * 3-cell block group at the top:
 *   [locals... | block_entry(fs-3) | def_env(fs-2) | captured_self(fs-1)] */
static uint32_t
pop_frame(struct kp_ctx *tc)
{
    struct kp_frame *f = tc->frame;
    /* `{ it }`: prism lists no local for the implicit param, but the body binds
     * it at slot 0 — count it. */
    const uint32_t it_slot = kp_it_shift(f);
    uint32_t frame_size = (uint32_t)f->locals->size + it_slot + f->synth_cnt + (f->uses_block ? 3u : 0u);
    for (uint32_t i = f->bake_base; i < tc->bake_cnt; i++) {
        *tc->bake_list[i] -= (int32_t)frame_size;
    }
    tc->bake_cnt = f->bake_base;
    /* Frame links: the baked staging depth plus this frame's size is the whole
     * distance from a callee's link cell down to this frame's base.  Past the
     * field it becomes a 0 distance, which ends the chain at this frame rather
     * than naming the wrong one. */
    for (uint32_t i = f->flink_base; i < tc->flink_cnt; i++) {
        const struct kp_flink_fix *const fx = &tc->flink_list[i];
        *fx->cell = korb_flink_make(fx->line, fx->staged + frame_size);   /* clamps an overflowing distance to 0 */
    }
    tc->flink_cnt = f->flink_base;
    for (uint32_t i = 0; i < f->add_cnt; i++) *f->add_cells[i] += (int32_t)frame_size;
    free(f->add_cells);
    tc->chain = f->saved_chain;
    /* B3: a nested block reaching depth d reaches depth d-1 from its parent, so
     * the parent (if reified as a Proc) must materialize that far too.  prism's
     * depths already stop at method boundaries, so methods stay at 0. */
    if (f->prev && f->max_ref_depth >= 1) {
        uint32_t up = f->max_ref_depth - 1;
        if (up > f->prev->max_ref_depth) f->prev->max_ref_depth = up;
    }
    /* Likewise for the yield target: reaching it from the parent is one level
     * nearer, and at 0 the parent IS the method (its trio is its own). */
    if (f->prev && f->yield_depth >= 2) {
        uint32_t up = f->yield_depth - 1;
        if (up > f->prev->yield_depth) f->prev->yield_depth = up;
    }
    tc->frame = f->prev;
    free(f);
    return frame_size;
}

/* See KP_LINK: remember where a call node's link lives and what part of its
 * distance is already baked, so this frame's pop can finish it. */
static void
kp_flink_add(struct kp_ctx *tc, uint64_t *cell)
{
    if ((*cell & 1u) == 0) return;                     /* the node carries no link */
    if (tc->flink_cnt == tc->flink_capa) {
        tc->flink_capa = tc->flink_capa ? tc->flink_capa * 2 : 256;
        tc->flink_list = realloc(tc->flink_list, sizeof(struct kp_flink_fix) * tc->flink_capa);
        if (!tc->flink_list) abort();
    }
    tc->flink_list[tc->flink_cnt].cell   = cell;
    tc->flink_list[tc->flink_cnt].line   = korb_flink_line(*cell);
    tc->flink_list[tc->flink_cnt].staged = korb_flink_dist(*cell);
    tc->flink_cnt++;
}

static void
bake_add(struct kp_ctx *tc, int32_t *cell)
{
    if (tc->bake_cnt == tc->bake_capa) {
        tc->bake_capa = tc->bake_capa ? tc->bake_capa * 2 : 1024;
        tc->bake_list = realloc(tc->bake_list, sizeof(int32_t *) * tc->bake_capa);
        if (!tc->bake_list) abort();
    }
    tc->bake_list[tc->bake_cnt++] = cell;
}

static uint32_t
lvar_index(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid)
{
    const pm_constant_id_list_t *list = tc->frame->locals;
    for (size_t i = 0; i < list->size; i++) {
        if (list->ids[i] == cid) return (uint32_t)i + kp_it_shift(tc->frame);
    }
    kp_failf(tc, node, "koruby_precise: local '%s' not in scope table", kp_cid_cstr(tc, cid));
}

/* The frame `depth` scopes out, continuing past an eval program's top into the
 * binding's levels (tc->eval_outer). */
static struct kp_frame *
kp_frame_at(struct kp_ctx *tc, uint32_t depth)
{
    struct kp_frame *f = tc->frame;
    for (uint32_t d = 0; d < depth && f; d++) {
        if (f->prev == NULL && tc->eval_outer_cnt != 0) {
            const uint32_t v = depth - d - 1;
            return v < tc->eval_outer_cnt ? &tc->eval_outer[v] : NULL;
        }
        f = f->prev;
    }
    return f;
}

/* Index of an outer variable `depth` enclosing scopes out (prism depth). */
static uint32_t
lvar_index_at(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    struct kp_frame *f = kp_frame_at(tc, depth);
    if (!f) kp_failf(tc, node, "koruby_precise: outer scope depth %u not found", depth);
    for (size_t i = 0; i < f->locals->size; i++) {
        if (f->locals->ids[i] == cid) return (uint32_t)i + kp_it_shift(f);
    }
    kp_failf(tc, node, "koruby_precise: outer local '%s' not at depth %u", kp_cid_cstr(tc, cid), depth);
}

static NODE *
bake_lget(struct kp_ctx *tc, uint32_t index)
{
    NODE *n = ALLOC_node_lget((int32_t)index - tc->chain);
    bake_add(tc, &n->u.node_lget.off);
    return n;
}

/* Reserve a synthetic temporary in the current frame, appended after prism
 * locals.  Returns its local index (usable with bake_lget/bake_lset). */
static uint32_t
alloc_synth_local(struct kp_ctx *tc)
{
    return (uint32_t)tc->frame->locals->size + kp_it_shift(tc->frame) + tc->frame->synth_cnt++;
}

static NODE *
bake_lset(struct kp_ctx *tc, uint32_t index, NODE *rval)
{
    NODE *n = ALLOC_node_lset((int32_t)index - tc->chain, rval);
    bake_add(tc, &n->u.node_lset.off);
    return n;
}

/* Outer-variable get/set (depth >= 1).  prev_off addresses the current frame's
 * PREV cell (bf[0] = base[-1]): baked -1 - chain, pop subtracts frame_size →
 * -(frame_size+1) - chain.  depth/index are constants (no fixup). */
static NODE *
bake_eget(struct kp_ctx *tc, uint32_t depth, uint32_t index)
{
    NODE *n = ALLOC_node_eget(-2 - tc->chain, depth, index);
    bake_add(tc, &n->u.node_eget.prev_off);
    return n;
}

static NODE *
bake_eset(struct kp_ctx *tc, uint32_t depth, uint32_t index, NODE *rval)
{
    NODE *n = ALLOC_node_eset(-2 - tc->chain, depth, index, rval);
    bake_add(tc, &n->u.node_eset.prev_off);
    return n;
}

static void kp_note_depth(struct kp_ctx *tc, uint32_t depth);                       /* fwd */
static NODE *build_array_with_fwd(struct kp_ctx *tc, struct pm_node **elems, size_t n,
                                  uint32_t depth, int32_t slot);                     /* fwd */
/* `...` may be written inside a block nested in the `def m(...)` body (rack's
 * Rack::Builder#use does `@use << proc { middleware.new(app, ...) }`).  Find the
 * enclosing method frame that owns the forwarding slots and how many env links
 * out it lies; a `def`/class body in between is a scope barrier. */
static struct kp_frame *kp_fwd_owner;   /* set by kp_fwd_find, read by the emitters below */
static bool
kp_fwd_find(struct kp_ctx *tc, uint32_t *depth_out, int32_t *slot_out, int32_t *blk_out)
{
    uint32_t d = tc->frame->depth_shift ? 1u : 0u;
    for (struct kp_frame *f = tc->frame; f; f = f->prev) {
        if (f->fwd_slot >= 0) {
            *depth_out = d; *slot_out = f->fwd_slot; *blk_out = f->fwd_blk_slot;
            kp_fwd_owner = f;
            return true;
        }
        if (f->method_mid || f->class_name_sym || f->anon_class_body) return false;
        d++;
    }
    return false;
}
/* The forwarded positional Array, read from wherever kp_fwd_find placed it. */
static NODE *
kp_fwd_args(struct kp_ctx *tc, uint32_t depth, int32_t slot)
{
    if (depth == 0) return bake_lget(tc, (uint32_t)slot);
    kp_note_depth(tc, depth);
    return bake_eget(tc, depth, (uint32_t)slot);
}
/* The call nodes address the forwarded block by a plain offset in THIS frame,
 * so an outer one is copied into a synth local first.  Returns the local and,
 * through *pre, the copy that must run before the call (NULL at depth 0). */
static int32_t
kp_fwd_blk_local(struct kp_ctx *tc, uint32_t depth, int32_t blk, NODE **pre)
{
    *pre = NULL;
    if (depth == 0) return blk;
    const int32_t local = (int32_t)alloc_synth_local(tc);
    kp_note_depth(tc, depth);
    *pre = bake_lset(tc, (uint32_t)local, bake_eget(tc, depth, (uint32_t)blk));
    return local;
}


/* Bare `&` / `*` / `**` may be written inside a block nested in the `def m(&)`
 * body (simplecov: `def synchronize(&) = LOCK.synchronize { with_flock(&) }`).
 * Find the enclosing method frame owning the synth slot and how many env links
 * out it lies; a def/class body in between is a scope barrier.  A read from
 * outside raises that frame's capture floor, like `...` does. */
enum kp_anon_kind { KP_ANON_BLK, KP_ANON_REST, KP_ANON_KWREST };
static bool
kp_anon_find(struct kp_ctx *const tc, const enum kp_anon_kind kind, uint32_t *const depth_out, int32_t *const slot_out)
{
    uint32_t d = tc->frame->depth_shift ? 1u : 0u;
    for (struct kp_frame *f = tc->frame; f; f = f->prev) {
        const int32_t slot = kind == KP_ANON_BLK  ? f->anon_blk_slot
                           : kind == KP_ANON_REST ? f->anon_rest_slot
                                                  : f->anon_kwrest_slot;
        if (slot >= 0) {
            *depth_out = d; *slot_out = slot;
            if (d > 0 && f->cap_min < (uint32_t)slot + 1u) f->cap_min = (uint32_t)slot + 1u;
            return true;
        }
        if (f->method_mid || f->class_name_sym || f->anon_class_body) return false;
        d++;
    }
    return false;
}

/* node_self reading the frame's self cell at base[-1] (bottom header).  The
 * initial offset -1-chain is fixed up by pop_frame (-= frame_size) to
 * -(frame_size+1)-chain = base[-1].  Other self_off readers (ivar/def/attr/
 * cvar/super/...) follow the same rule: keep their initial -1-chain (cursor →
 * base[fs-1]) value and bake the field so it becomes base[-1]. */
static NODE *
bake_self(struct kp_ctx *tc)
{
    NODE *n = ALLOC_node_self(-1 - tc->chain);
    bake_add(tc, &n->u.node_self.self_off);
    return n;
}
/* @x read/write: self at base[-1] (bottom header) — bake the self_off field. */
static NODE *
bake_ivar_get(struct kp_ctx *tc, uint32_t name)
{
    NODE *n = ALLOC_node_ivar_get(-1 - tc->chain, name);
    bake_add(tc, &n->u.node_ivar_get.self_off);
    return n;
}
static NODE *
bake_ivar_set(struct kp_ctx *tc, uint32_t name, NODE *val)
{
    NODE *n = ALLOC_node_ivar_set(-1 - tc->chain, name, val);
    bake_add(tc, &n->u.node_ivar_set.self_off);
    return n;
}
/* @@x read/write: self at base[-1], the method entry / def_class at base[-4]
 * (the frame's identity cell) — both baked -N-chain and finished by pop_frame. */
static NODE *
bake_cvar_get(struct kp_ctx *tc, uint32_t name, uint32_t soft)
{
    kp_needs_definee(tc);
    NODE *n = ALLOC_node_cvar_get(-1 - tc->chain, -4 - tc->chain, name, soft);
    bake_add(tc, &n->u.node_cvar_get.self_off);
    bake_add(tc, &n->u.node_cvar_get.dc_off);
    return n;
}
static NODE *
bake_cvar_set(struct kp_ctx *tc, uint32_t name, NODE *val)
{
    kp_needs_definee(tc);
    NODE *n = ALLOC_node_cvar_set(-1 - tc->chain, -4 - tc->chain, name, val);
    bake_add(tc, &n->u.node_cvar_set.self_off);
    bake_add(tc, &n->u.node_cvar_set.dc_off);
    return n;
}

/* depth==0 → local, depth>=1 → outer.  Centralizes the read/write dispatch. */
/* record the deepest outer-scope read/write so a reified Proc knows how many
 * enclosing scopes to materialize on escape (B3). */
static void kp_note_depth(struct kp_ctx *tc, uint32_t depth) {
    if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
}
static NODE *
lvar_read(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    if (tc->frame->depth_shift) depth++;
    if (depth == 0) return bake_lget(tc, lvar_index(tc, node, cid));
    kp_note_depth(tc, depth);
    return bake_eget(tc, depth, lvar_index_at(tc, node, cid, depth));
}

static NODE *
lvar_write(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth, NODE *rval)
{
    if (tc->frame->depth_shift) depth++;
    if (depth == 0) return bake_lset(tc, lvar_index(tc, node, cid), rval);
    kp_note_depth(tc, depth);
    return bake_eset(tc, depth, lvar_index_at(tc, node, cid, depth), rval);
}

/* ---- helpers ----------------------------------------------------------- */

static NODE *
lit_nil(void)
{
    return ALLOC_node_lit(KORB_NIL);
}

/* `defined?(expr)` in a statement position: nothing is evaluated, and the
 * warning fires once, on the first execution of the enclosing code (CRuby warns
 * while compiling — we go through Ruby's $stderr so a harness can capture it). */
static NODE *
kp_defined_void(struct kp_ctx *tc, const pm_node_t *st)
{
    struct korb_dupkey_warn *const w = malloc(sizeof(*w));   /* immortal: the node reads it */
    if (!w) abort();
    w->file = strdup(tc->fname); w->key = NULL;
    w->line = kp_line(tc, st); w->line2 = 0; w->done = 0;
    return ALLOC_node_void_warn(w, lit_nil());
}

static NODE *
transduce_statements(struct kp_ctx *tc, const pm_statements_node_t *stmts)
{
    if (stmts == NULL || stmts->body.size == 0) return lit_nil();
    NODE *acc = NULL;
    for (size_t i = 0; i < stmts->body.size; i++) {
        const pm_node_t *const st = stmts->body.nodes[i];
        NODE *one;
        /* A `defined?(...)` whose value is discarded is not evaluated at all —
         * CRuby warns at compile time and drops it. */
        if (i + 1 < stmts->body.size && PM_NODE_TYPE_P(st, PM_DEFINED_NODE)) {
            one = kp_defined_void(tc, st);
        } else {
            one = transduce(tc, st);
        }
        acc = acc ? ALLOC_node_seq(acc, one) : one;
    }
    return acc;
}

static NODE *
transduce_opt(struct kp_ctx *tc, const pm_node_t *node)
{
    return node ? transduce(tc, node) : lit_nil();
}

static NODE *massign_general(struct kp_ctx *tc, const pm_node_list_t *lefts, const pm_node_t *rest, const pm_node_list_t *rights, NODE *rhs);
static NODE *assign_target_from_synth(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local);
static NODE *assign_target_from_synth_r(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local, int32_t recv_local);
static uint32_t alloc_synth_local(struct kp_ctx *tc);
static NODE *bake_lset(struct kp_ctx *tc, uint32_t idx, NODE *val);

/* Build a node_rescue chain from a prism rescue-clause list (one clause per
 * `rescue ...`, threaded via ->subsequent).  Each clause tests its class(es)
 * against the exception node_begin parks at slots[0]; the terminal `next` is
 * node_reraise (re-propagate when nothing matched).  `rescue A, B => e` expands
 * to one node_rescue per class, all sharing the body + `=> e` binding, with the
 * first listed class tried first. */
static NODE *
build_rescue_chain(struct kp_ctx *tc, const pm_rescue_node_t *rc)
{
    if (!rc) return ALLOC_node_reraise();
    NODE *next = build_rescue_chain(tc, rc->subsequent);   /* later clauses */

    NODE *body = rc->statements ? transduce_statements(tc, rc->statements) : lit_nil();
    int32_t resc_var = 0;
    uint32_t flags = 0;
    if (rc->reference && !PM_NODE_TYPE_P(rc->reference, PM_LOCAL_VARIABLE_TARGET_NODE)) {
        /* `rescue => @ivar / CONST / $g / obj.x= / obj[k]=`: node_rescue has
         * already pushed $! when the body runs, so stash it in a synth local and
         * plumb it out with the same target writer multi-assign uses. */
        const uint32_t tmp = alloc_synth_local(tc);
        NODE *const store = bake_lset(tc, tmp, ALLOC_node_errinfo());
        NODE *const assign = assign_target_from_synth(tc, rc->reference, tmp);
        if (!assign) return kp_unsupported(tc, (const pm_node_t *)rc, "rescue => unsupported target");
        body = ALLOC_node_seq(ALLOC_node_seq(store, assign), body);
    } else if (rc->reference) {
        const pm_local_variable_target_node_t *ref = (const pm_local_variable_target_node_t *)rc->reference;
        if (ref->depth == 0) {                   /* `=> e` binds a current-frame slot */
            uint32_t idx = lvar_index(tc, rc->reference, ref->name);
            resc_var = (int32_t)idx - tc->chain; /* frame-size fixup via bake_add below */
            flags |= 1u;
        } else {
            /* `=> e` where e is an ENCLOSING local (e.g. a block reusing an outer
             * name): node_rescue pushes $! before running the body, so bind via
             * `e = $!` prepended to the body using the depth-aware lvar_write.
             * flags bit 0 stays 0 (no frame-slot binding). */
            NODE *assign = lvar_write(tc, rc->reference, ref->name, ref->depth, ALLOC_node_errinfo());
            body = ALLOC_node_seq(assign, body);
        }
    }

    /* bare `rescue` catches StandardError; otherwise one node_rescue per listed
     * class.  Build classes back-to-front so the first listed is tried first. */
    if (rc->exceptions.size == 0) {
        NODE *cls = KP_CONST(tc, korb_intern(tc->c->vm, "StandardError", 13), 0, INT32_MIN, INT32_MIN, 0);
        NODE *nd = ALLOC_node_rescue(cls, body, next, resc_var, flags, -tc->chain);   /* base_off: this frame's own base (backtrace) */
        bake_add(tc, &nd->u.node_rescue.base_off);
        if (flags & 1u) bake_add(tc, &nd->u.node_rescue.resc_var);
        return nd;
    }
    for (size_t k = rc->exceptions.size; k-- > 0; ) {
        /* node_rescue evaluates the matcher one slot up (slots+1), so bake its
         * offsets at chain+1 — otherwise a local / captured-var matcher (e.g.
         * `rescue klass`) reads the wrong slot and crashes. */
        const pm_node_t *ex = rc->exceptions.nodes[k];
        const bool splat = PM_NODE_TYPE_P(ex, PM_SPLAT_NODE);          /* `rescue *list` */
        if (splat) ex = ((const pm_splat_node_t *)ex)->expression;
        NODE *cls = WITH_CHAIN(tc, 1, transduce_opt(tc, ex));
        NODE *nd = splat ? ALLOC_node_rescue_splat(cls, body, next, resc_var, flags, -tc->chain)
                         : ALLOC_node_rescue(cls, body, next, resc_var, flags, -tc->chain);
        bake_add(tc, splat ? &nd->u.node_rescue_splat.base_off : &nd->u.node_rescue.base_off);
        if (flags & 1u) bake_add(tc, splat ? &nd->u.node_rescue_splat.resc_var : &nd->u.node_rescue.resc_var);
        next = nd;
    }
    return next;
}

/* true if the constant-pool name is a bindable variable — i.e. NOT underscore
 * prefixed (CRuby permits `_`-prefixed binds inside alternative patterns). */
static bool
kp_name_is_bindable(struct kp_ctx *tc, pm_constant_id_t cid)
{
    const pm_constant_t *ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);
    return !(ct->length > 0 && ct->start[0] == '_');
}

/* CRuby forbids variable binding inside an alternative pattern (`a | b`), save
 * for underscore-prefixed names.  Recursively true if `pat` binds such a var. */
static bool
pattern_binds_var(struct kp_ctx *tc, const pm_node_t *pat)
{
    if (!pat) return false;
    if (PM_NODE_TYPE_P(pat, PM_IMPLICIT_NODE))
        return pattern_binds_var(tc, (const pm_node_t *)((const pm_implicit_node_t *)pat)->value);
    switch (PM_NODE_TYPE(pat)) {
      case PM_LOCAL_VARIABLE_TARGET_NODE:
        return kp_name_is_bindable(tc, ((const pm_local_variable_target_node_t *)pat)->name);
      case PM_CAPTURE_PATTERN_NODE: {
        const pm_capture_pattern_node_t *cp = (const pm_capture_pattern_node_t *)pat;
        return kp_name_is_bindable(tc, cp->target->name) || pattern_binds_var(tc, cp->value);
      }
      case PM_ALTERNATION_PATTERN_NODE: {
        const pm_alternation_pattern_node_t *ap = (const pm_alternation_pattern_node_t *)pat;
        return pattern_binds_var(tc, ap->left) || pattern_binds_var(tc, ap->right);
      }
      case PM_ARRAY_PATTERN_NODE: {
        const pm_array_pattern_node_t *ap = (const pm_array_pattern_node_t *)pat;
        for (size_t i = 0; i < ap->requireds.size; i++) if (pattern_binds_var(tc, ap->requireds.nodes[i])) return true;
        if (pattern_binds_var(tc, ap->rest)) return true;
        for (size_t i = 0; i < ap->posts.size; i++) if (pattern_binds_var(tc, ap->posts.nodes[i])) return true;
        return false;
      }
      case PM_FIND_PATTERN_NODE: {
        const pm_find_pattern_node_t *fp = (const pm_find_pattern_node_t *)pat;
        if (pattern_binds_var(tc, (const pm_node_t *)fp->left) || pattern_binds_var(tc, (const pm_node_t *)fp->right)) return true;
        for (size_t i = 0; i < fp->requireds.size; i++) if (pattern_binds_var(tc, fp->requireds.nodes[i])) return true;
        return false;
      }
      case PM_HASH_PATTERN_NODE: {
        const pm_hash_pattern_node_t *hp = (const pm_hash_pattern_node_t *)pat;
        for (size_t i = 0; i < hp->elements.size; i++) {
            if (!PM_NODE_TYPE_P(hp->elements.nodes[i], PM_ASSOC_NODE)) continue;
            const pm_assoc_node_t *as = (const pm_assoc_node_t *)hp->elements.nodes[i];
            if (as->value) { if (pattern_binds_var(tc, as->value)) return true; }   /* `key: pat` */
            else if (PM_NODE_TYPE_P(as->key, PM_SYMBOL_NODE)) {                     /* `key:` binds `key` */
                const pm_symbol_node_t *sn = (const pm_symbol_node_t *)as->key;
                const char *kn = (const char *)pm_string_source(&sn->unescaped);
                if (pm_string_length(&sn->unescaped) > 0 && kn[0] != '_') return true;
            }
        }
        if (hp->rest && PM_NODE_TYPE_P(hp->rest, PM_ASSOC_SPLAT_NODE))
            return pattern_binds_var(tc, ((const pm_assoc_splat_node_t *)hp->rest)->value);
        return false;
      }
      case PM_SPLAT_NODE:
        return pattern_binds_var(tc, ((const pm_splat_node_t *)pat)->expression);
      case PM_ASSOC_SPLAT_NODE:
        return pattern_binds_var(tc, ((const pm_assoc_splat_node_t *)pat)->value);
      default:
        return false;
    }
}

/* Compile a prism pattern into a korb_pat descriptor (immortal; walked by the
 * runtime matcher).  Supports binding / array / hash / value (=== ) patterns;
 * rest/post/constant/find patterns fall back to an unsupported value node. */
static struct korb_pat *build_pattern_desc_r(struct kp_ctx *tc, const pm_node_t *pat);
/* A value node the matcher EVALs at its cursor, not at the match node's slots:
 * transduce it at that chain so its baked offsets (frame base, scratch) agree. */
static NODE *
kp_pat_value(struct kp_ctx *tc, const pm_node_t *node)
{
    tc->chain += tc->pat_cur;
    NODE *const v = transduce(tc, node);
    tc->chain -= tc->pat_cur;
    return v;
}
static struct korb_pat *
build_pattern_desc(struct kp_ctx *tc, const pm_node_t *pat)
{
    const int32_t saved = tc->pat_cur;
    tc->pat_cur = 1;
    struct korb_pat *const d = build_pattern_desc_r(tc, pat);
    tc->pat_cur = saved;
    return d;
}
/* an element of an array / hash / find pattern: matched two cells further up */
static struct korb_pat *
build_pattern_elem(struct kp_ctx *tc, const pm_node_t *pat)
{
    tc->pat_cur += 2;
    struct korb_pat *const d = build_pattern_desc_r(tc, pat);
    tc->pat_cur -= 2;
    return d;
}
static struct korb_pat *
build_pattern_desc_r(struct kp_ctx *tc, const pm_node_t *pat)
{
    struct korb_pat *p = calloc(1, sizeof(*p));
    if (!p) abort();
    if (PM_NODE_TYPE_P(pat, PM_IMPLICIT_NODE))                      /* `{key:}` shorthand wraps the target */
        pat = (const pm_node_t *)((const pm_implicit_node_t *)pat)->value;
    if (PM_NODE_TYPE_P(pat, PM_LOCAL_VARIABLE_TARGET_NODE)) {       /* binding */
        const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)pat;
        if (lt->depth == 0) {
            p->kind = 0;
            p->bind_off = (int32_t)lvar_index(tc, pat, lt->name) - tc->chain;
            bake_add(tc, &p->bind_off);
            return p;
        }
    } else if (PM_NODE_TYPE_P(pat, PM_ARRAY_PATTERN_NODE)) {
        const pm_array_pattern_node_t *ap = (const pm_array_pattern_node_t *)pat;
        if (!ap->rest && ap->posts.size == 0) {
            p->kind = 2; p->n = (uint32_t)ap->requireds.size;
            if (ap->constant) p->value_node = kp_pat_value(tc, ap->constant);   /* `Const[...]` → Const === subject first */
            p->elems = calloc(p->n ? p->n : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_elem(tc, ap->requireds.nodes[i]);
            return p;
        }
        if (ap->rest) {                                       /* `[pre..., *rest, post...]` (optionally `Const[...]`) */
            p->kind = 6; p->n = (uint32_t)ap->requireds.size; p->npost = (uint32_t)ap->posts.size;
            if (ap->constant) p->value_node = kp_pat_value(tc, ap->constant);
            const uint32_t total = p->n + p->npost;
            p->elems = calloc(total ? total : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_elem(tc, ap->requireds.nodes[i]);
            for (uint32_t i = 0; i < p->npost; i++) p->elems[p->n + i] = build_pattern_elem(tc, ap->posts.nodes[i]);
            p->bind_off = INT32_MIN;                          /* anonymous `*` (no bind); valid offsets are negative */
            if (PM_NODE_TYPE_P(ap->rest, PM_SPLAT_NODE)) {
                const pm_splat_node_t *sp = (const pm_splat_node_t *)ap->rest;
                if (sp->expression && PM_NODE_TYPE_P(sp->expression, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)sp->expression;
                    if (lt->depth == 0) { p->bind_off = (int32_t)lvar_index(tc, sp->expression, lt->name) - tc->chain; bake_add(tc, &p->bind_off); }
                }
            }
            return p;
        }
    } else if (PM_NODE_TYPE_P(pat, PM_FIND_PATTERN_NODE)) {        /* `[*pre, mid..., *post]` find pattern */
        const pm_find_pattern_node_t *fp = (const pm_find_pattern_node_t *)pat;
        {
            p->kind = 8; p->n = (uint32_t)fp->requireds.size;
            if (fp->constant) p->value_node = kp_pat_value(tc, fp->constant);   /* `Const[*, …, *]` */
            p->elems = calloc(p->n ? p->n : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_elem(tc, fp->requireds.nodes[i]);
            p->bind_off = INT32_MIN;                              /* leading `*pre` slot (or anonymous) */
            if (fp->left && fp->left->expression && PM_NODE_TYPE_P(fp->left->expression, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)fp->left->expression;
                if (lt->depth == 0) { p->bind_off = (int32_t)lvar_index(tc, fp->left->expression, lt->name) - tc->chain; bake_add(tc, &p->bind_off); }
            }
            int32_t right_off = INT32_MIN;                        /* trailing `*post` slot (npost holds it) */
            if (fp->right && PM_NODE_TYPE_P(fp->right, PM_SPLAT_NODE)) {
                const pm_splat_node_t *rsp = (const pm_splat_node_t *)fp->right;
                if (rsp->expression && PM_NODE_TYPE_P(rsp->expression, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)rsp->expression;
                    if (lt->depth == 0) right_off = (int32_t)lvar_index(tc, rsp->expression, lt->name) - tc->chain;
                }
            }
            p->npost = (uint32_t)right_off;
            if (right_off != INT32_MIN) bake_add(tc, (int32_t *)&p->npost);
            return p;
        }
    } else if (PM_NODE_TYPE_P(pat, PM_HASH_PATTERN_NODE)) {
        const pm_hash_pattern_node_t *hp = (const pm_hash_pattern_node_t *)pat;
        {
            bool ok = true;
            p->kind = 3; p->n = (uint32_t)hp->elements.size;
            p->npost = 0; p->bind_off = INT32_MIN;        /* rest mode: 0=none, 1=**rest, 2=**nil */
            if (hp->constant) p->value_node = kp_pat_value(tc, hp->constant);   /* `Const(k: …)` → Const === subject */
            p->keys  = calloc(p->n ? p->n : 1, sizeof(VALUE));
            p->elems = calloc(p->n ? p->n : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) {
                if (!PM_NODE_TYPE_P(hp->elements.nodes[i], PM_ASSOC_NODE)) { ok = false; break; }
                const pm_assoc_node_t *as = (const pm_assoc_node_t *)hp->elements.nodes[i];
                if (!PM_NODE_TYPE_P(as->key, PM_SYMBOL_NODE)) { ok = false; break; }
                const pm_symbol_node_t *sn = (const pm_symbol_node_t *)as->key;
                const char *kname = (const char *)pm_string_source(&sn->unescaped);
                size_t klen = pm_string_length(&sn->unescaped);
                p->keys[i] = ID2SYM(korb_intern(tc->c->vm, kname, klen));
                if (as->value) {                              /* `key: pattern` */
                    p->elems[i] = build_pattern_elem(tc, as->value);
                } else {                                      /* `key:` shorthand → bind local named `key` */
                    int32_t li = -1;
                    for (size_t j = 0; j < tc->frame->locals->size; j++) {
                        const pm_constant_id_t lid = tc->frame->locals->ids[j];
                        if (lid == 0) continue;            /* anonymous `*` placeholder slot */
                        pm_constant_t *ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, lid);
                        if (ct->length == klen && memcmp(ct->start, kname, klen) == 0) { li = (int32_t)j; break; }
                    }
                    if (li >= 0) li += (int32_t)kp_it_shift(tc->frame);
                    if (li < 0) { ok = false; break; }
                    struct korb_pat *bp = calloc(1, sizeof(*bp));
                    if (!bp) abort();
                    bp->kind = 0;
                    bp->bind_off = li - tc->chain;
                    bake_add(tc, &bp->bind_off);
                    p->elems[i] = bp;
                }
            }
            if (ok && hp->rest) {                         /* `**rest` / `**nil` / `**` */
                if (PM_NODE_TYPE_P(hp->rest, PM_ASSOC_SPLAT_NODE)) {
                    p->npost = 1;                         /* **rest (or anonymous **): bind extra entries */
                    const pm_assoc_splat_node_t *sp = (const pm_assoc_splat_node_t *)hp->rest;
                    if (sp->value && PM_NODE_TYPE_P(sp->value, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                        const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)sp->value;
                        if (lt->depth == 0) { p->bind_off = (int32_t)lvar_index(tc, sp->value, lt->name) - tc->chain; bake_add(tc, &p->bind_off); }
                    }
                } else if (PM_NODE_TYPE_P(hp->rest, PM_NO_KEYWORDS_PARAMETER_NODE)) {
                    p->npost = 2;                         /* **nil: forbid extra entries */
                } else {
                    ok = false;
                }
            }
            if (ok) return p;
        }
    } else if (PM_NODE_TYPE_P(pat, PM_CAPTURE_PATTERN_NODE)) {     /* `pat => name` */
        const pm_capture_pattern_node_t *cp = (const pm_capture_pattern_node_t *)pat;
        if (cp->target->depth == 0) {
            p->kind = 4; p->n = 1;
            p->elems = calloc(1, sizeof(struct korb_pat *));
            p->elems[0] = build_pattern_desc_r(tc, cp->value);
            p->bind_off = (int32_t)lvar_index(tc, (const pm_node_t *)cp->target, cp->target->name) - tc->chain;
            bake_add(tc, &p->bind_off);
            return p;
        }
    } else if (PM_NODE_TYPE_P(pat, PM_ALTERNATION_PATTERN_NODE)) {  /* `a | b` (no bindings) */
        const pm_alternation_pattern_node_t *ap = (const pm_alternation_pattern_node_t *)pat;
        if (pattern_binds_var(tc, ap->left) || pattern_binds_var(tc, ap->right))
            tc->syntax_error = true;   /* CRuby: "illegal variable in alternative pattern" → SyntaxError */
        p->kind = 5; p->n = 2;
        p->elems = calloc(2, sizeof(struct korb_pat *));
        p->elems[0] = build_pattern_desc_r(tc, ap->left);
        p->elems[1] = build_pattern_desc_r(tc, ap->right);
        return p;
    } else if (PM_NODE_TYPE_P(pat, PM_PINNED_VARIABLE_NODE)) {      /* `^var` → var === subject */
        p->kind = 7;   /* pin: value_node is a frame-local read → must EVAL in the match frame (base), not cur */
        p->value_node = transduce(tc, (const pm_node_t *)((const pm_pinned_variable_node_t *)pat)->variable);
        return p;
    } else if (PM_NODE_TYPE_P(pat, PM_PINNED_EXPRESSION_NODE)) {    /* `^(expr)` */
        p->kind = 1;
        p->value_node = kp_pat_value(tc, (const pm_node_t *)((const pm_pinned_expression_node_t *)pat)->expression);
        return p;
    }
    /* value pattern: `pattern === subject` (constant / literal / range / etc.) */
    p->kind = 1;
    p->value_node = kp_pat_value(tc, pat);
    return p;
}

static bool
kp_integer_value(const pm_integer_t *integer, korb_sword_t *out)
{
    uint64_t mag;
    if (integer->values == NULL) {
        mag = integer->value;
    }
    else if (integer->length <= 2) {
        mag = (uint64_t)integer->values[0];
        if (integer->length == 2) mag |= (uint64_t)integer->values[1] << 32;
    }
    else {
        return false;
    }
    if (integer->negative) {
        if (mag > (uint64_t)FIXNUM_MAX + 1) return false;
        *out = -(korb_sword_t)mag;
        return true;
    }
    if (mag > (uint64_t)FIXNUM_MAX) return false;
    *out = (korb_sword_t)mag;
    return true;
}

/* pm_integer → malloc'd decimal string (immortal; for Bignum literals).  Limbs
 * are uint32, least-significant first (prism convention). */
static char *
kp_integer_to_decimal(const pm_integer_t *iv)
{
    korb_mp_t z; korb_mp_init(z);
    if (iv->values == NULL) korb_mp_set_ui(z, iv->value);
    else                    korb_mp_import(z, iv->length, -1, sizeof(uint32_t), 0, 0, iv->values);
    if (iv->negative) korb_mp_neg(z, z);
    char *gs = korb_mp_get_str(NULL, 10, z);                 /* GMP-malloc'd */
    korb_mp_clear(z);
    size_t len = strlen(gs);
    char *buf = malloc(len + 1);
    if (!buf) abort();
    memcpy(buf, gs, len + 1);
    korb_mp_strfree(gs, len + 1);          /* backend が確保した文字列 */
    return buf;
}

/* malloc-backed copy of a pm_string (NODE operands are immortal) */
static const char *
kp_strdup_pm(const pm_string_t *s, uint32_t *len_out)
{
    size_t len = pm_string_length(s);
    char *buf = malloc(len + 1);
    if (!buf) abort();
    memcpy(buf, pm_string_source(s), len);
    buf[len] = '\0';
    *len_out = (uint32_t)len;
    return buf;
}

static NODE *kp_warn_dup_hash_keys(struct kp_ctx *tc, struct pm_node **assocs, size_t n, NODE *h);

/* ---- operators --------------------------------------------------------- */

extern const struct NodeKind kind_node_plus;         /* all binops share slot_count */
extern const struct NodeKind kind_node_ivar_arith;   /* fused `@x +=` / `@x -=` */
extern const struct NodeKind kind_node_caseeq;       /* case/when `v === subj` */
extern const struct NodeKind kind_node_entry;        /* block/lambda entry — guards proc/block reify */
extern const struct NodeKind kind_node_aref;         /* recv[idx] */
extern const struct NodeKind kind_node_aset;         /* recv[idx] = val */
extern const struct NodeKind kind_node_ary_push;     /* array-literal push chain */
extern const struct NodeKind kind_node_ary_concat;   /* array-literal splat (*) chain */

/* Where a `class X` / `module X` written HERE nests.  Normally that is the
 * runtime `self` (a class body runs with self = the class), but inside a block
 * self can have been rebound by Class.new / class_eval, while Ruby's constant
 * nesting stays lexical.  Returns false when self is the right answer; returns
 * true and sets *owner to the enclosing class body's name (0 = top level) when
 * the definition sits inside a block. */
/* Reads and literals only: evaluating one cannot be observed by a sibling
 * expression, so their relative order is free. */
static bool
kp_order_transparent(const pm_node_t *n)
{
    if (n == NULL) return true;
    switch (PM_NODE_TYPE(n)) {
      case PM_LOCAL_VARIABLE_READ_NODE: case PM_INSTANCE_VARIABLE_READ_NODE:
      case PM_CLASS_VARIABLE_READ_NODE: case PM_GLOBAL_VARIABLE_READ_NODE:
      case PM_CONSTANT_READ_NODE:       case PM_SELF_NODE:
      case PM_INTEGER_NODE:             case PM_FLOAT_NODE:
      case PM_SYMBOL_NODE:              case PM_STRING_NODE:
      case PM_NIL_NODE:                 case PM_TRUE_NODE: case PM_FALSE_NODE:
        return true;
      default:
        return false;
    }
}

static bool
kp_lexical_class_owner(struct kp_ctx *tc, uint32_t *owner)
{
    bool in_block = false;
    const struct kp_frame *inner = NULL;
    for (const struct kp_frame *f = tc->frame; f; f = f->prev) {
        if (f->anon_class_body) return false;      /* `class << obj`: only self names that cref */
        if (f->class_name_sym) { inner = f; break; }
        if (f->block_body) in_block = true;
    }
    /* eval(str, binding): the program frame carries the binding's cref name, and
     * THAT names the namespace even with no block in between -- self can have
     * been rebound (module_exec) and CRuby nests by the cref, not by self. */
    const bool seeded = tc->seed_class_name != 0;
    if (inner == NULL) { *owner = 0; return in_block || tc->in_eval; }   /* no enclosing class body → top level */
    /* The WHOLE nesting path, not just the innermost name: the constant table is
     * flat, so a bare `A` would also answer for an unrelated `M2::A`
     * (`Temple::ERB` answered the `ERB` of `class ERB::Compiler`). */
    uint32_t syms[32], depth = tc->seed_chain_len;   /* eval(String): what encloses the seeded cref */
    for (const struct kp_frame *f = inner; f; f = f->prev)
        if (f->class_name_sym != 0) depth++;
    if (depth == 0 || depth > 32) { *owner = inner->class_name_sym; return in_block || seeded; }
    uint32_t i = depth;
    for (const struct kp_frame *f = inner; f && i > tc->seed_chain_len; f = f->prev)
        if (f->class_name_sym != 0) syms[--i] = f->class_name_sym;
    for (uint32_t k = tc->seed_chain_len; k-- > 0; ) syms[--i] = tc->seed_chain[k];
    char buf[512];
    size_t len = 0;
    for (uint32_t k = 0; k < depth; k++) {
        const char *const nm = korb_sym_name(tc->c->vm, syms[k]);
        const size_t nl = strlen(nm);
        if (len + nl + 3 > sizeof buf) { *owner = inner->class_name_sym; return in_block || seeded; }
        if (len) { buf[len++] = ':'; buf[len++] = ':'; }
        memcpy(buf + len, nm, nl);
        len += nl;
    }
    *owner = korb_intern(tc->c->vm, buf, (uint32_t)len);
    return in_block || seeded;
}


/* `(expr)::N ||= v` / `&&=` — the module part is evaluated exactly once into a
 * synth local, then the read and the write go through const_get / const_set.
 * `or_form` picks ||= (probe with const_defined? so an unset constant does not
 * raise) over &&= (a plain read, which does raise when unset, like CRuby). */
static NODE *
kp_dyn_const_opassign(struct kp_ctx *tc, const pm_node_t *node, const pm_node_t *modpart,
                      uint32_t name, const pm_node_t *value, bool or_form)
{
    struct korb_vm *const vm = tc->c->vm;
    const uint32_t line = kp_line(tc, node);
    const uint32_t mtmp = alloc_synth_local(tc);
    NODE *const store = bake_lset(tc, mtmp, transduce(tc, modpart));
    NODE *gr, *gk;
    WITH_CHAIN(tc, KP_SEND1_SC, (gr = bake_lget(tc, mtmp), gk = ALLOC_node_lit(ID2SYM(name))));
    NODE *read = kp_send1(tc, korb_intern(vm, "const_get", 9), line, gr, gk);
    NODE *sr, *sk, *sv;
    WITH_CHAIN(tc, KP_SEND2_SC, (sr = bake_lget(tc, mtmp), sk = ALLOC_node_lit(ID2SYM(name)),
                                 sv = transduce(tc, value)));
    NODE *const set = kp_send2(tc, korb_intern(vm, "const_set", 9), line, sr, sk, sv);
    if (!or_form) return ALLOC_node_seq(store, ALLOC_node_and(read, set));
    NODE *dr, *dk;
    WITH_CHAIN(tc, KP_SEND1_SC, (dr = bake_lget(tc, mtmp), dk = ALLOC_node_lit(ID2SYM(name))));
    NODE *const defq = kp_send1(tc, korb_intern(vm, "const_defined?", 14), line, dr, dk);
    return ALLOC_node_seq(store, ALLOC_node_or(ALLOC_node_and(defq, read), set));
}

extern const struct NodeKind kind_node_const_set;    /* FOO = expr */
static NODE *build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa);
static NODE *build_array_with_fwd(struct kp_ctx *tc, struct pm_node **elems, size_t n,
                                  uint32_t depth, int32_t slot);
static NODE *build_call_args(struct kp_ctx *tc, struct pm_node **elems, size_t n);
static NODE *kp_make_binding_node(struct kp_ctx *tc, uint32_t line);
static NODE *kp_make_binding_node_x(struct kp_ctx *tc, uint32_t line, uint32_t user_dispatch);
extern const struct NodeKind kind_node_hash_merge;   /* hash literal ** splat chain */
extern const struct NodeKind kind_node_dstr_concat;  /* string-interp concat chain */
extern const struct NodeKind kind_node_hash_set;     /* hash-literal set chain */
extern const struct NodeKind kind_node_range_new;    /* range literal */

enum kp_binop {
    KP_BINOP_NONE = 0,
    KP_PLUS, KP_MINUS, KP_MUL, KP_DIV, KP_MOD,
    KP_LT, KP_LE, KP_GT, KP_GE, KP_EQ, KP_NEQ,
    KP_BAND, KP_BOR, KP_BXOR, KP_SHL, KP_SHR,
};

static enum kp_binop
kp_binop_kind(const char *name)
{
    if (strcmp(name, "+") == 0)  return KP_PLUS;
    if (strcmp(name, "-") == 0)  return KP_MINUS;
    if (strcmp(name, "*") == 0)  return KP_MUL;
    if (strcmp(name, "/") == 0)  return KP_DIV;
    if (strcmp(name, "%") == 0)  return KP_MOD;
    if (strcmp(name, "<") == 0)  return KP_LT;
    if (strcmp(name, "<=") == 0) return KP_LE;
    if (strcmp(name, ">") == 0)  return KP_GT;
    if (strcmp(name, ">=") == 0) return KP_GE;
    if (strcmp(name, "==") == 0) return KP_EQ;
    if (strcmp(name, "!=") == 0) return KP_NEQ;
    if (strcmp(name, "&") == 0)  return KP_BAND;
    if (strcmp(name, "|") == 0)  return KP_BOR;
    if (strcmp(name, "^") == 0)  return KP_BXOR;
    if (strcmp(name, "<<") == 0) return KP_SHL;
    if (strcmp(name, ">>") == 0) return KP_SHR;
    return KP_BINOP_NONE;
}

/* Every binop node stages the same way (lhs by reference, rhs by value = one
 * slot) and reserves no header cells, so they all take the same baked link; the
 * node hands it to whatever dispatch its deopt reaches (node.def KP_DEOPT). */
static NODE *
alloc_binop(struct kp_ctx *tc, enum kp_binop op, NODE *lhs, NODE *rhs, uint32_t line)
{
    const uint64_t fl = kp_flink_at(tc, line, kind_node_plus.slot_count);
    switch (op) {
      case KP_PLUS:  return KP_LINK(tc, node_plus, lhs, rhs, fl);
      case KP_MINUS: return KP_LINK(tc, node_minus, lhs, rhs, fl);
      case KP_MUL:   return KP_LINK(tc, node_mul, lhs, rhs, fl);
      case KP_DIV:   return KP_LINK(tc, node_div, lhs, rhs, fl);
      case KP_MOD:   return KP_LINK(tc, node_mod, lhs, rhs, fl);
      case KP_LT:    return KP_LINK(tc, node_lt, lhs, rhs, fl);
      case KP_LE:    return KP_LINK(tc, node_le, lhs, rhs, fl);
      case KP_GT:    return KP_LINK(tc, node_gt, lhs, rhs, fl);
      case KP_GE:    return KP_LINK(tc, node_ge, lhs, rhs, fl);
      case KP_EQ:    return KP_LINK(tc, node_eq, lhs, rhs, fl);
      case KP_NEQ:   return KP_LINK(tc, node_neq, lhs, rhs, fl);
      case KP_BAND:  return KP_LINK(tc, node_band, lhs, rhs, fl);
      case KP_BOR:   return KP_LINK(tc, node_bor, lhs, rhs, fl);
      case KP_BXOR:  return KP_LINK(tc, node_bxor, lhs, rhs, fl);
      case KP_SHL:   return KP_LINK(tc, node_shl, lhs, rhs, fl);
      case KP_SHR:   return KP_LINK(tc, node_shr, lhs, rhs, fl);
      default:       abort();
    }
}

/* ---- calls -------------------------------------------------------------- */

/* Local NAMES of the frame being closed, in slot order (struct korb_locals_info,
 * node.h).  Hung off the scope's ISEQ entry so that C can name the locals of any
 * live frame from its identity cell.  Must run BEFORE pop_frame: the frame owns
 * the prism name list.  Synthetic temporaries sit past the prism locals and are
 * left out (never user-visible). */
static void *
build_locals_info(struct kp_ctx *tc)
{
    const struct kp_frame *const f = tc->frame;
    const uint32_t it_slot = kp_it_shift(f);   /* `{ it }` binds slot 0 with no prism local */
    const uint32_t n = (uint32_t)f->locals->size + it_slot;
    if (n == 0) return NULL;
    struct korb_locals_info *const li = malloc(sizeof(*li) + n * sizeof(uint32_t));   /* immortal */
    if (!li) abort();
    li->n = n;
    /* the implicit `it` is not a local variable (CRuby): leave its slot
     * unnamed, like the anonymous `*` slot below */
    if (it_slot) li->names[0] = 0;
    for (uint32_t i = it_slot; i < n; i++) {
        const pm_constant_id_t cid = f->locals->ids[i - it_slot];
        /* cid 0 = the slot kp_param_first_locals reserves for an anonymous `*`;
         * it has no name and CRuby keeps it out of local_variables. */
        li->names[i] = cid ? kp_intern_cid(tc, cid) : 0;
    }
    return li;
}

/* Build Proc#parameters metadata once at parse time (cold; never touched on the
 * call/yield hot path).  Stored on node_entry.param_info.  Raw kinds (req kept
 * as req); #parameters converts positional req→opt for non-lambda procs. */
/* `&nil` (Ruby 4.1): a nameless BlockParameterNode flagged by the vendored
 * prism (PM_PARAMETER_FLAGS_NO_BLOCK) — declares that no block is taken. */
static inline bool
kp_no_block_param(const pm_block_parameter_node_t *const b)
{
    return b != NULL && PM_NODE_FLAG_P(b, PM_PARAMETER_FLAGS_NO_BLOCK);
}

static void *
build_param_info(struct kp_ctx *tc, const pm_node_t *blk_params)
{
    if (!blk_params) return NULL;
    if (PM_NODE_TYPE_P(blk_params, PM_NUMBERED_PARAMETERS_NODE)) {
        uint32_t n = ((const pm_numbered_parameters_node_t *)blk_params)->maximum;
        struct korb_param_info *pi = malloc(sizeof(*pi) + n * sizeof(struct korb_param_entry));
        if (!pi) abort();
        pi->n = n; pi->r2k = 0;
        for (uint32_t i = 0; i < n; i++) { char nm[8]; snprintf(nm, sizeof(nm), "_%u", i + 1);
            pi->e[i].kind = 0; pi->e[i].name = korb_intern(tc->c->vm, nm, (uint32_t)strlen(nm)); }
        return pi;
    }
    if (PM_NODE_TYPE_P(blk_params, PM_IT_PARAMETERS_NODE)) {
        struct korb_param_info *pi = malloc(sizeof(*pi) + sizeof(struct korb_param_entry));
        if (!pi) abort();
        pi->n = 1; pi->r2k = 0; pi->e[0].kind = 0; pi->e[0].name = 0;   /* `it` is anonymous */
        return pi;
    }
    const pm_parameters_node_t *ps = NULL;
    if (PM_NODE_TYPE_P(blk_params, PM_BLOCK_PARAMETERS_NODE))
        ps = ((const pm_block_parameters_node_t *)blk_params)->parameters;
    else if (PM_NODE_TYPE_P(blk_params, PM_PARAMETERS_NODE))
        ps = (const pm_parameters_node_t *)blk_params;
    if (!ps) return NULL;
    /* `|x,|` (implicit rest) is not a parameter at all in CRuby's #parameters */
    const bool has_rest_param = ps->rest && !PM_NODE_TYPE_P(ps->rest, PM_IMPLICIT_REST_NODE);
    /* `...` reports as the three anonymous parameters it stands for (CRuby):
     *  [[:rest, :*], [:keyrest, :**], [:block, :&]] */
    const bool fwd = ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_FORWARDING_PARAMETER_NODE);
    uint32_t n = (uint32_t)(ps->requireds.size + ps->optionals.size + (has_rest_param ? 1 : 0) +
                            ps->posts.size + ps->keywords.size + (ps->keyword_rest ? (fwd ? 3 : 1) : 0) +
                            (ps->block ? 1 : 0));
    if (n == 0) return NULL;
    struct korb_param_info *pi = malloc(sizeof(*pi) + n * sizeof(struct korb_param_entry));
    if (!pi) abort();
    uint32_t k = 0;
    #define PI_ADD(knd, cid) do { pi->e[k].kind = (knd); pi->e[k].name = (cid) ? kp_intern_cid(tc, (cid)) : 0; k++; } while (0)
    for (uint32_t i = 0; i < ps->requireds.size; i++) {
        const pm_node_t *p = ps->requireds.nodes[i];
        PI_ADD(0, PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE) ? ((const pm_required_parameter_node_t *)p)->name : 0);
    }
    for (uint32_t i = 0; i < ps->optionals.size; i++)
        PI_ADD(1, ((const pm_optional_parameter_node_t *)ps->optionals.nodes[i])->name);
    if (has_rest_param) {   /* anonymous `*` reports the name :* (Ruby 3.x), a named rest its name */
        const pm_constant_id_t rn = PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE) ? ((const pm_rest_parameter_node_t *)ps->rest)->name : 0;
        pi->e[k].kind = 2; pi->e[k].name = rn ? kp_intern_cid(tc, rn) : korb_intern(tc->c->vm, "*", 1); k++;
    }
    for (uint32_t i = 0; i < ps->posts.size; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        PI_ADD(0, PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE) ? ((const pm_required_parameter_node_t *)p)->name : 0);
    }
    for (uint32_t i = 0; i < ps->keywords.size; i++) {
        const pm_node_t *p = ps->keywords.nodes[i];
        if (PM_NODE_TYPE_P(p, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) PI_ADD(3, ((const pm_required_keyword_parameter_node_t *)p)->name);
        else PI_ADD(4, ((const pm_optional_keyword_parameter_node_t *)p)->name);
    }
    if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {   /* anonymous `**` → :** */
        const pm_constant_id_t kn = ((const pm_keyword_rest_parameter_node_t *)ps->keyword_rest)->name;
        pi->e[k].kind = 5; pi->e[k].name = kn ? kp_intern_cid(tc, kn) : korb_intern(tc->c->vm, "**", 2); k++;
    } else if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_NO_KEYWORDS_PARAMETER_NODE)) {   /* `**nil` → :nokey */
        pi->e[k].kind = 7; pi->e[k].name = 0; k++;
    } else if (fwd) {                                  /* `...` → *, **, & */
        pi->e[k].kind = 2; pi->e[k].name = korb_intern(tc->c->vm, "*", 1);  k++;
        pi->e[k].kind = 5; pi->e[k].name = korb_intern(tc->c->vm, "**", 2); k++;
        pi->e[k].kind = 6; pi->e[k].name = korb_intern(tc->c->vm, "&", 1);  k++;
    }
    if (kp_no_block_param(ps->block)) {   /* `&nil` → :noblock */
        pi->e[k].kind = 8; pi->e[k].name = 0; k++;
    } else if (ps->block) {   /* anonymous `&` → :& */
        const pm_constant_id_t bn = ((const pm_block_parameter_node_t *)ps->block)->name;
        pi->e[k].kind = 6; pi->e[k].name = bn ? kp_intern_cid(tc, bn) : korb_intern(tc->c->vm, "&", 1); k++;
    }
    #undef PI_ADD
    pi->n = k; pi->r2k = 0;
    return pi;
}

/* Parse a block literal into a node_entry (its own scope; registered as an
 * AOT entry like a method body).  docs/v2_blocks_design.md. */

/* Byte-stream encoder for destructuring block parameters (see the call site for
 * the format).  `loc` counts leaves so a caller can tell how many locals the
 * spec consumes; `bad` marks a shape the binder cannot describe. */
struct kp_destr { uint8_t *buf; uint32_t len, cap, loc; bool bad; };

static void
kp_destr_put(struct kp_destr *e, uint32_t b)
{
    if (e->len == e->cap) { e->cap = e->cap ? e->cap * 2 : 16; e->buf = realloc(e->buf, e->cap); if (!e->buf) abort(); }
    e->buf[e->len++] = (uint8_t)b;
}

static bool
kp_destr_leaf_cid(const pm_node_t *t, pm_constant_id_t *out)
{
    if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE))      { *out = ((const pm_required_parameter_node_t *)t)->name; return true; }
    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE))   { *out = ((const pm_local_variable_target_node_t *)t)->name; return true; }
    return false;
}

static void
kp_encode_destr(struct kp_ctx *tc, const pm_node_t *p, struct kp_destr *e)
{
    if (e->bad) return;
    pm_constant_id_t cid;
    if (kp_destr_leaf_cid(p, &cid)) {
        const uint32_t li = lvar_index(tc, p, cid);
        if (li > 254) { e->bad = true; return; }
        /* A repeated name shares ONE prism slot.  Unlike flat block params
         * (where CRuby keeps the FIRST), a destructuring target is a masgn and
         * assigns left to right, so the LAST write wins — emit the slot twice. */
        kp_destr_put(e, 0x00); kp_destr_put(e, li);
        e->loc++;
        return;
    }
    if (!PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) { e->bad = true; return; }
    const pm_multi_target_node_t *const mt = (const pm_multi_target_node_t *)p;
    if (mt->lefts.size > 254 || mt->rights.size > 254) { e->bad = true; return; }
    if (!mt->rest) {
        if (mt->rights.size || mt->lefts.size == 0) { e->bad = true; return; }
        kp_destr_put(e, 0xFF); kp_destr_put(e, mt->lefts.size);
        for (uint32_t i = 0; i < mt->lefts.size; i++) kp_encode_destr(tc, mt->lefts.nodes[i], e);
        return;
    }
    /* `(a, *b, c)` — 0xFE <nleft> <rest_named> <nright>, then the left entries,
     * the rest leaf's bare local byte (when named), then the right entries. */
    const pm_node_t *rn = mt->rest;
    bool rest_named = false;
    pm_constant_id_t rcid = 0;
    if (PM_NODE_TYPE_P(rn, PM_SPLAT_NODE)) {
        const pm_node_t *const ex = (const pm_node_t *)((const pm_splat_node_t *)rn)->expression;
        if (ex && kp_destr_leaf_cid(ex, &rcid)) rest_named = true;
        else if (ex) { e->bad = true; return; }
    } else if (PM_NODE_TYPE_P(rn, PM_REST_PARAMETER_NODE)) {
        rcid = ((const pm_rest_parameter_node_t *)rn)->name;
        rest_named = (rcid != 0);
    } else if (!PM_NODE_TYPE_P(rn, PM_IMPLICIT_REST_NODE)) { e->bad = true; return; }
    kp_destr_put(e, 0xFE); kp_destr_put(e, mt->lefts.size);
    kp_destr_put(e, rest_named ? 1 : 0); kp_destr_put(e, mt->rights.size);
    for (uint32_t i = 0; i < mt->lefts.size; i++) kp_encode_destr(tc, mt->lefts.nodes[i], e);
    if (e->bad) return;
    if (rest_named) {
        const uint32_t li = lvar_index(tc, rn, rcid);
        if (li > 254) { e->bad = true; return; }
        kp_destr_put(e, li);
        e->loc++;
    }
    for (uint32_t i = 0; i < mt->rights.size; i++) kp_encode_destr(tc, mt->rights.nodes[i], e);
}

/* `{ r = 1; _1 }` — prism lists a block's locals in SOURCE order, so a numbered
 * parameter can land past a local written before it, while the ABI binds the
 * arguments to slots 0..max.  Same remedy as kp_param_first_locals: a private
 * copy with `_1`.._N first.  NULL = prism's order already works. */
static const pm_constant_id_list_t *
kp_numbered_first_locals(struct kp_ctx *tc, const pm_constant_id_list_t *const locals, const uint32_t maximum)
{
    if (maximum == 0 || maximum > 9 || locals->size < maximum) return NULL;
    pm_constant_id_t want[9];
    bool already = true;
    for (uint32_t i = 0; i < maximum; i++) {
        const char nm[3] = { '_', (char)('1' + i), '\0' };
        want[i] = 0;
        for (size_t k = 0; k < locals->size && want[i] == 0; k++) {
            if (locals->ids[k] == 0) continue;
            const pm_constant_t *const ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, locals->ids[k]);
            if (ct->length == 2 && memcmp(ct->start, nm, 2) == 0) { want[i] = locals->ids[k]; already &= (k == i); }
        }
        if (want[i] == 0) return NULL;                   /* `_i` not declared: leave the layout alone */
    }
    if (already) return NULL;
    pm_constant_id_t *const ids = malloc(sizeof(*ids) * locals->size);   /* immortal: the frame reads it */
    if (!ids) abort();
    memcpy(ids, want, sizeof(*ids) * maximum);
    uint32_t w = maximum;
    for (size_t k = 0; k < locals->size; k++) {
        bool is_param = false;
        for (uint32_t i = 0; !is_param && i < maximum; i++) is_param = (want[i] == locals->ids[k]);
        if (!is_param) ids[w++] = locals->ids[k];
    }
    if (w != locals->size) { free(ids); return NULL; }   /* duplicate ids: leave it alone */
    pm_constant_id_list_t *const out = malloc(sizeof(*out));
    if (!out) abort();
    out->ids = ids; out->size = locals->size; out->capacity = locals->size;
    return out;
}

static NODE *
transduce_block_parts(struct kp_ctx *tc, const pm_constant_id_list_t *blk_locals,
                      const pm_node_t *blk_params, const pm_node_t *blk_body)
{
    if (blk_params && PM_NODE_TYPE_P(blk_params, PM_NUMBERED_PARAMETERS_NODE)) {
        const pm_constant_id_list_t *const ro = kp_numbered_first_locals(
            tc, blk_locals, ((const pm_numbered_parameters_node_t *)blk_params)->maximum);
        if (ro) blk_locals = ro;
    }
    push_frame(tc, blk_locals);
    tc->frame->block_body = true;
    tc->frame->dm_body = tc->next_block_is_dm;
    tc->next_block_is_dm = false;

    uint32_t bparams = 0;
    uint32_t destructure_n = 0;     /* >0 for a single |(a,b,...)| destructuring param */
    uint8_t *destructure_spec = NULL;  /* per-param arity for mixed |a,(k,v)| (0=scalar) */
    int32_t rest_slot = -1;         /* local index of a `*rest` param, or -1 */
    uint32_t post_cnt = 0;          /* trailing required params (bound from the end) */
    uint32_t destr_len = 0;         /* bytes in destructure_spec (variable-length: nested groups) */
    bool needs_spec = false;        /* a destructured param appeared in the opt/rest/post form */
    struct Node **opt_defaults = NULL;  /* default exprs for optional params */
    uint32_t req_cnt = 0;           /* leading required positional count */
    struct korb_kw_info *kw_info = NULL;  /* keyword params (a:, b: 10, **kw) */
    int32_t blk_param_slot = -1;    /* local index of a `&blk` param, or -1 — see korb_block_yield_full */
    if (blk_params && PM_NODE_TYPE_P(blk_params, PM_NUMBERED_PARAMETERS_NODE)) {
        /* `{ _1 * _2 }` — prism puts `_1`.._N in locals[0..N-1]; the body's
         * `_N` reads resolve as ordinary locals.  N = maximum referenced. */
        bparams = ((const pm_numbered_parameters_node_t *)blk_params)->maximum;
    } else if (blk_params && PM_NODE_TYPE_P(blk_params, PM_IT_PARAMETERS_NODE)) {
        /* `{ it * 2 }` — one implicit param `it`; the body reads it via a
         * PM_IT_LOCAL_VARIABLE_READ_NODE, mapped to local slot 0 in transduce. */
        bparams = 1;
        tc->frame->it_param = true;   /* name slot 0 "it" for Binding (prism lists no local) */
    } else if (blk_params) {
        const pm_parameters_node_t *ps;
        if (PM_NODE_TYPE_P(blk_params, PM_BLOCK_PARAMETERS_NODE)) {
            const pm_block_parameters_node_t *bp = (const pm_block_parameters_node_t *)blk_params;
            /* Block-local variables (`|x; y|`): prism already lists them in the
             * block scope's local table (so they're in this frame's locals), and
             * they are NOT bound from args — just fresh locals.  korb_block_yield
             * nils every slot past params_cnt, so no extra work is needed; only
             * the positional params below are bound. */
            ps = bp->parameters;
        } else if (PM_NODE_TYPE_P(blk_params, PM_PARAMETERS_NODE)) {   /* `->(x) {}` lambda params */
            ps = (const pm_parameters_node_t *)blk_params;
        } else {
            pop_frame(tc);
            return kp_unsupported(tc, blk_params, "unsupported block parameters");
        }
        if (ps) {
            if (ps->block && !kp_no_block_param(ps->block)) {   /* `|&blk|`: bind the forwarded block into its local (`&nil` binds nothing) */
                const pm_node_t *const bpn = (const pm_node_t *)ps->block;
                if (!((const pm_block_parameter_node_t *)bpn)->name) {
                    pop_frame(tc);
                    return kp_unsupported(tc, bpn, "anonymous/forwarding block parameter (&)");
                }
                blk_param_slot = (int32_t)lvar_index(tc, bpn,
                    ((const pm_block_parameter_node_t *)bpn)->name);
            }
            if (ps->keywords.size || ps->keyword_rest) {   /* keyword params (built after positional) */
                kw_info = malloc(sizeof(*kw_info));
                if (!kw_info) abort();
                kw_info->count = (uint32_t)ps->keywords.size;
                kw_info->kwrest_slot = -1;
                kw_info->entries = ps->keywords.size ? malloc(sizeof(struct korb_kw_entry) * ps->keywords.size) : NULL;
                for (uint32_t j = 0; j < kw_info->count; j++) {
                    const pm_node_t *kp = ps->keywords.nodes[j];
                    pm_constant_id_t name; NODE *deflt = NULL;
                    if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                        name = ((const pm_required_keyword_parameter_node_t *)kp)->name;
                    } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                        const pm_optional_keyword_parameter_node_t *ok = (const pm_optional_keyword_parameter_node_t *)kp;
                        name = ok->name; deflt = transduce(tc, ok->value);
                    } else { pop_frame(tc); return kp_unsupported(tc, kp, "block keyword parameter form"); }
                    kw_info->entries[j].mid  = kp_intern_cid(tc, name);
                    kw_info->entries[j].slot = lvar_index(tc, kp, name);
                    kw_info->entries[j].deflt = deflt;
                }
                if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {
                    pm_constant_id_t kr = ((const pm_keyword_rest_parameter_node_t *)ps->keyword_rest)->name;
                    if (kr) kw_info->kwrest_slot = (int32_t)lvar_index(tc, ps->keyword_rest, kr);
                    else    kw_info->kwrest_slot = -2;   /* anonymous `**` : accept & discard all keywords */
                } else if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_NO_KEYWORDS_PARAMETER_NODE))
                    kw_info->kwrest_slot = -3;           /* `**nil` : no keywords accepted */
            }
            if (ps->rest || ps->optionals.size || ps->posts.size) {
                /* general positional block params: req..., opt..., *rest, post...
                 * (plain, non-destructured — block params are runtime metadata;
                 * validate local slots in declaration order). */
                uint32_t loc = 0;
                for (uint32_t i = 0; i < ps->requireds.size; i++) {
                    const pm_node_t *p = ps->requireds.nodes[i];
                    if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) { needs_spec = true; continue; }   /* |(a,b), *rest| → spec path */
                    if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, p, "destructuring block param with opt/rest"); }
                    if (lvar_index(tc, p, ((const pm_required_parameter_node_t *)p)->name) != loc)
                        { pop_frame(tc); return kp_unsupported(tc, p, "block param slot ordering"); }   /* skip this block, don't abort the file */
                    loc++;
                }
                req_cnt = loc;
                if (ps->optionals.size) {
                    opt_defaults = malloc(sizeof(struct Node *) * ps->optionals.size);
                    if (!opt_defaults) abort();
                    for (uint32_t j = 0; j < ps->optionals.size; j++) {
                        const pm_optional_parameter_node_t *op = (const pm_optional_parameter_node_t *)ps->optionals.nodes[j];
                        if (lvar_index(tc, (const pm_node_t *)op, op->name) != loc)
                            { free(opt_defaults); pop_frame(tc);   /* skip this block, don't abort the file */
                              return kp_unsupported(tc, (const pm_node_t *)op, "block optional parameter slot ordering"); }
                        opt_defaults[j] = transduce(tc, op->value);   /* default runs in block scope */
                        loc++;
                    }
                }
                if (ps->rest) {
                    /* `|s,|` (implicit rest) and `|s,*|` (anonymous rest) discard
                     * the extra args but still make the block multi-parameter for
                     * auto-splat purposes.  No local is consumed: rest_slot = -2
                     * tells the binder "splat like a rest, store nothing".
                     * (With post params the discarded slot would shift the posts'
                     * local indexes — keep that combination unsupported.)
                     * -3 is the implicit `|s,|` form: it binds identically but is
                     * NOT a rest param — CRuby keeps it out of #arity /
                     * #parameters and a lambda still enforces its exact arity. */
                    const bool implicit_rest = PM_NODE_TYPE_P(ps->rest, PM_IMPLICIT_REST_NODE);
                    const bool anon_rest = implicit_rest ||
                        (PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE) &&
                         !((const pm_rest_parameter_node_t *)ps->rest)->name);
                    if (anon_rest) {
                        if (ps->posts.size) { pop_frame(tc); return kp_unsupported(tc, ps->rest, "anonymous block rest with post params"); }
                        rest_slot = implicit_rest ? -3 : -2;
                    } else {
                    if (!PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, ps->rest, "block splat parameter"); }
                    const pm_rest_parameter_node_t *rp = (const pm_rest_parameter_node_t *)ps->rest;
                    rest_slot = (int32_t)lvar_index(tc, ps->rest, rp->name);   /* destructured params shift the count */
                    if (!needs_spec && rest_slot != (int32_t)loc)
                        kp_failf(tc, ps->rest, "koruby_precise: block rest not locals[%u]", loc);
                    loc++;
                    }
                }
                post_cnt = (uint32_t)ps->posts.size;      /* |a, b=1, c| / |a, *r, c| — posts bind from the END */
                for (uint32_t i = 0; i < ps->posts.size; i++) {
                    const pm_node_t *p = ps->posts.nodes[i];
                    if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) { needs_spec = true; continue; }
                    if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, p, "destructuring block post"); }
                    if (lvar_index(tc, p, ((const pm_required_parameter_node_t *)p)->name) != loc)
                        kp_failf(tc, p, "koruby_precise: block post not locals[%u]", loc);
                    loc++;
                }
                bparams = loc;                              /* req + opt + rest(1) + post */
                if (needs_spec) {
                    /* one entry per logical param (the rest param is a scalar
                     * entry); leaves carry their target local index. */
                    struct kp_destr e2 = { NULL, 0, 0, 0, false };
                    /* header: number of LOGICAL params before *rest (locals and
                     * logical params diverge once a param destructures) */
                    kp_destr_put(&e2, (uint32_t)(ps->requireds.size + ps->optionals.size));
                    for (uint32_t pass = 0; pass < 3 && !e2.bad; pass++) {
                        const pm_node_list_t *lst = pass == 0 ? &ps->requireds
                                                  : pass == 1 ? &ps->optionals : &ps->posts;
                        for (uint32_t i = 0; i < lst->size && !e2.bad; i++) {
                            const pm_node_t *p = lst->nodes[i];
                            if (PM_NODE_TYPE_P(p, PM_OPTIONAL_PARAMETER_NODE)) {   /* an optional is a plain leaf here */
                                const uint32_t li = lvar_index(tc, p, ((const pm_optional_parameter_node_t *)p)->name);
                                if (li > 254) { e2.bad = true; break; }
                                kp_destr_put(&e2, 0x00); kp_destr_put(&e2, li); e2.loc++;
                            } else kp_encode_destr(tc, p, &e2);
                        }
                        if (pass == 1 && rest_slot >= 0)          /* the rest param's own leaf */
                            { kp_destr_put(&e2, 0x00); kp_destr_put(&e2, (uint32_t)rest_slot); e2.loc++; }
                    }
                    if (e2.bad) { free(e2.buf); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "destructuring block param with opt/rest"); }
                    destructure_spec = e2.buf;
                    destr_len = e2.len;
                }
            }
            /* single |(a, b, ...)| → destructure the one array arg into N locals
             * (a NESTED group goes through the general spec encoder below) */
            else if (ps->requireds.size == 1 && PM_NODE_TYPE_P(ps->requireds.nodes[0], PM_MULTI_TARGET_NODE) &&
                     ({ const pm_multi_target_node_t *m0 = (const pm_multi_target_node_t *)ps->requireds.nodes[0];
                        bool flat = !m0->rest && !m0->rights.size;
                        for (uint32_t z = 0; flat && z < m0->lefts.size; z++)
                            if (PM_NODE_TYPE_P(m0->lefts.nodes[z], PM_MULTI_TARGET_NODE)) flat = false;
                        flat; })) {
                const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)ps->requireds.nodes[0];
                bool dup_names = false;
                if (mt->rest || mt->rights.size) {
                    pop_frame(tc);
                    return kp_unsupported(tc, ps->requireds.nodes[0], "block param with splat/post destructure");
                }
                for (uint32_t i = 0; i < mt->lefts.size; i++) {
                    const pm_node_t *t = mt->lefts.nodes[i];
                    pm_constant_id_t cid;
                    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE))
                        cid = ((const pm_local_variable_target_node_t *)t)->name;
                    else if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE))
                        cid = ((const pm_required_parameter_node_t *)t)->name;
                    else {
                        pop_frame(tc);
                        return kp_unsupported(tc, t, "nested destructuring block parameter");
                    }
                    /* the destructure writes position j into locals[j]; a repeated
                     * name (`|(_, a, _)|`) makes prism reuse a slot, so the
                     * identity mapping no longer holds.  Bind through an explicit
                     * spec instead (the encoder sends duplicates to scratch). */
                    if (lvar_index(tc, t, cid) != i) dup_names = true;
                }
                bparams = 1;
                if (dup_names) {
                    struct kp_destr enc = { NULL, 0, 0, 0, false };
                    kp_encode_destr(tc, ps->requireds.nodes[0], &enc);
                    if (enc.bad) { free(enc.buf); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "repeated name in a destructuring block parameter"); }
                    destructure_spec = enc.buf;
                    destr_len = enc.len;
                } else destructure_n = (uint32_t)mt->lefts.size;
            } else {
                bparams = (uint32_t)ps->requireds.size;
                bool any_destr = false, flat_dup = false;
                for (uint32_t i = 0; i < bparams; i++)
                    if (PM_NODE_TYPE_P(ps->requireds.nodes[i], PM_MULTI_TARGET_NODE)) { any_destr = true; break; }
                if (!any_destr) {
                    for (uint32_t i = 0; i < bparams; i++) {
                        const pm_node_t *p = ps->requireds.nodes[i];
                        if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) {
                            pop_frame(tc);
                            return kp_unsupported(tc, p, "destructuring block parameter");
                        }
                        pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
                        /* A repeated name (only `_` is legal) makes prism reuse ONE
                         * slot, so every parameter after it has slot != position and
                         * the positional ABI would bind them one short. */
                        if (lvar_index(tc, p, cid) != i) { flat_dup = true; break; }
                    }
                }
                if (flat_dup) {
                    /* Bind through the destructure spec, which names the target slot
                     * per parameter (0x00 <slot>).  A REPEATED name keeps the FIRST
                     * occurrence (`proc { |_, _| _ }` reads argument 1, CRuby), so
                     * later duplicates are sent to a scratch local instead. */
                    struct kp_destr enc = { NULL, 0, 0, 0, false };
                    uint8_t used[256] = { 0 };
                    for (uint32_t i = 0; i < bparams && !enc.bad; i++) {
                        const pm_node_t *p = ps->requireds.nodes[i];
                        const pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
                        uint32_t li = lvar_index(tc, p, cid);
                        if (li > 254) { enc.bad = true; break; }
                        if (used[li]) { li = alloc_synth_local(tc); if (li > 254) { enc.bad = true; break; } }
                        used[li] = 1;
                        kp_destr_put(&enc, 0x00); kp_destr_put(&enc, li);
                    }
                    if (enc.bad) { free(enc.buf); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "repeated block parameter name"); }
                    destructure_spec = enc.buf;
                    destr_len = enc.len;
                } else if (any_destr) {
                    /* mixed scalar + |(...)| destructuring params, e.g. |a, (k, v)|
                     * or nested |(a, (b, c))|.  The spec is a byte stream, one
                     * entry per top-level param: 0x00 <local> = scalar, 0xFF <k>
                     * + k entries = a plain group, 0xFE <nl> <rest?> <nr> +
                     * entries = a group with a splat.  Groups nest freely; the
                     * binder's korb_bind_destr_entry decodes the same shape. */
                    struct kp_destr enc = { NULL, 0, 0, 0, false };
                    for (uint32_t i = 0; i < bparams; i++)
                        kp_encode_destr(tc, ps->requireds.nodes[i], &enc);
                    if (enc.bad) { free(enc.buf); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "destructuring block parameter"); }
                    destructure_spec = enc.buf;
                    destr_len = enc.len;
                }
            }
        }
    }

    NODE *body;
    if (blk_body == NULL) {
        body = lit_nil();
    }
    else if (PM_NODE_TYPE_P(blk_body, PM_STATEMENTS_NODE)) {
        body = transduce_statements(tc, (const pm_statements_node_t *)blk_body);
    }
    else {
        /* block-body rescue/ensure — an (implicit) begin node, handled by the
         * PM_BEGIN_NODE transducer (node_begin), same as a def body. */
        body = transduce(tc, blk_body);
    }

    /* B3 capture metadata: how many enclosing scopes this block reaches, and
     * each one's local count (final prism locals->size — synth temps aren't
     * captured by closures).  Computed before pop_frame (needs the prev chain). */
    uint32_t cap_depth = tc->frame->max_ref_depth;
    const uint32_t cap_yield = tc->frame->yield_depth;
    uint16_t *cap_ns = NULL;
    if (cap_depth > 0) {
        cap_ns = malloc(sizeof(uint16_t) * cap_depth);
        if (!cap_ns) abort();
        for (uint32_t k = 0; k < cap_depth; k++) {
            const struct kp_frame *const encl = kp_frame_at(tc, k + 1);
            uint32_t n_enc = encl ? (uint32_t)(encl->locals->size + kp_it_shift(encl)) : 0;
            if (encl && encl->cap_min > n_enc) n_enc = encl->cap_min;   /* `...` synth slots read from inside */
            cap_ns[k] = (uint16_t)n_enc;
        }
    }
    void *const linfo = build_locals_info(tc);      /* before pop_frame: it frees the frame */
    uint32_t frame_size = pop_frame(tc);    /* block locals (+2 if the block yields) */
    uint32_t blk_top = 0;
    const uint32_t blk_label = kp_block_label(tc, &blk_top);
    NODE *entry = ALLOC_node_entry(body, bparams, frame_size, destructure_n, destructure_spec, destr_len, cap_depth, cap_ns, cap_yield, rest_slot, opt_defaults, req_cnt, kw_info, build_param_info(tc, blk_params), linfo, blk_param_slot, post_cnt, -1, 0, 0, blk_label, blk_top, 0);
    /* backtrace: KORB_ID_OFF marks "this block has an enclosing frame", which is
     * all the link a C-driven yield rebuilds needs — the PREV handle names that
     * frame's base directly. */
    if (tc->frame) entry->u.node_entry.def_top_off = KORB_ID_OFF;
    /* Proc#source_location is registered by the caller (which has the block/lambda
     * node, so an empty `{ }` still gets a line). */
    /* node_entry is the dispatch root (yield → entry->head.dispatcher); its own
     * AOT entry, body inlined into its SD. */
    code_repo_add("block", entry, true);
    return entry;
}

/* END { } body: a block whose own scope is empty and whose every local
 * reference is one level out (see kp_frame.depth_shift). */
static NODE *
transduce_block_parts_shifted(struct kp_ctx *tc, const pm_constant_id_list_t *locals, const pm_node_t *body)
{
    struct kp_frame *const outer = tc->frame;
    (void)outer;
    tc->pending_depth_shift = true;
    NODE *e = transduce_block_parts(tc, locals, NULL, body);
    tc->pending_depth_shift = false;
    return e;
}

static NODE *
transduce_block(struct kp_ctx *tc, const pm_block_node_t *blk)
{
    NODE *e = transduce_block_parts(tc, &blk->locals, blk->parameters, blk->body);
    korb_reg_srcloc(tc->c->vm, e, korb_intern(tc->c->vm, tc->fname, strlen(tc->fname)), kp_line(tc, (const pm_node_t *)blk));
    kp_reg_srcrange(tc, e, (const pm_node_t *)blk);   /* Proc#source_range: the `{ }` / `do end` extent */
    if (e->head.kind == &kind_node_entry) e->u.node_entry.def_line = kp_line(tc, (const pm_node_t *)blk);
    return e;
}

/* Call with a literal block.  Bakes def_env_off (caller frame base) and
 * hands the node_entry + def_env to the callee.  B2: 0 or 1 positional arg. */
/* Synthesize the block `{ |x| x.sym }` for `&:sym` (symbol-to-proc), reusing the
 * normal block machinery — a real node_entry, so dispatcher prefetch is safe. */
static NODE *
kp_symbol_block(struct kp_ctx *tc, uint32_t sym_id)
{
    static pm_constant_id_t one_id[1] = { 0 };       /* one synthetic local `x` */
    pm_constant_id_list_t fake; fake.ids = one_id; fake.size = 1; fake.capacity = 1;
    push_frame(tc, &fake);
    NODE *recv;
    WITH_CHAIN(tc, KP_SEND0_SC, (recv = bake_lget(tc, 0)));     /* x (local 0), staged as send recv */
    NODE *body = kp_send0(tc, sym_id, 0, recv);
    uint32_t frame_size = pop_frame(tc);
    NODE *entry = ALLOC_node_entry(body, 1, frame_size, 0, NULL, 0, 0, NULL, 0, -1, NULL, 0, NULL, NULL, NULL, -1, 0, -1, 0, 0, 0, 0, sym_id);
    code_repo_add("symblock", entry, true);
    return entry;
}

/* Resolve a call's block: a literal `{ }` → real node_entry; `&:sym` → a
 * synthesized `{ |x| x.sym }` block; else NULL = unsupported. */
/* `define_method(:x) { ... }` / `define_singleton_method` — the literal block
 * becomes a method body, so a `super` inside it is a method's super. */
static bool
kp_defines_method_p(struct kp_ctx *tc, uint32_t mid)
{
    const char *const nm = korb_sym_name(tc->c->vm, mid);
    return !strcmp(nm, "define_method") || !strcmp(nm, "define_singleton_method");
}

static NODE *
kp_block_entry(struct kp_ctx *tc, const pm_node_t *blk)
{
    if (PM_NODE_TYPE_P(blk, PM_BLOCK_NODE))
        return transduce_block(tc, (const pm_block_node_t *)blk);
    if (PM_NODE_TYPE_P(blk, PM_BLOCK_ARGUMENT_NODE)) {
        const pm_block_argument_node_t *ba = (const pm_block_argument_node_t *)blk;
        if (ba->expression && PM_NODE_TYPE_P(ba->expression, PM_SYMBOL_NODE)) {
            const pm_symbol_node_t *sn = (const pm_symbol_node_t *)ba->expression;
            size_t len = pm_string_length(&sn->unescaped);
            uint32_t id = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), len);
            return kp_symbol_block(tc, id);
        }
    }
    return NULL;
}

static NODE *
transduce_call_with_block(struct kp_ctx *tc, const pm_call_node_t *cn, uint32_t mid,
                          uint32_t line, const pm_arguments_node_t *args, size_t argc,
                          NODE *entry)
{
    /* def_env_off: cursor → caller frame base = -(chain + staging); staging =
     * argc.  bake_add fixes up by the caller's frame_size.  node_call_blk stages
     * the args via argv@children (any fixed arity). */
    uint32_t cnt = 1u + (uint32_t)argc;                 /* self receiver + args */
    NODE **argv = malloc(sizeof(NODE *) * cnt);
    if (!argv) abort();
    int32_t saved = tc->chain;
    tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;  /* @framehdr cursor +HDR */
    argv[0] = bake_self(tc);          /* self → base[-1] (also the block's captured self) */
    for (size_t i = 0; i < argc; i++)
        argv[1 + i] = transduce(tc, args->arguments.nodes[i]);
    tc->chain = saved;
    NODE *call = KP_LINK(tc, node_call_blk, mid, kp_flink(tc, line), entry, -(tc->chain + (int32_t)cnt + KORB_FRAME_HDR), argv, cnt);
    bake_add(tc, &call->u.node_call_blk.def_env_off);
    return call;
}

static NODE *
transduce_func_call_1(struct kp_ctx *tc, const pm_call_node_t *cn);

/* A VARIABLE call (`foo` — no receiver, no parens, no args, no block) that
 * misses is a NameError, not a NoMethodError.  Only the parser can tell, so it
 * marks the node; korb_call_impl reads the mark on the miss path.  A wrapper
 * node would cost a second dispatcher on every successful call. */
static NODE *
transduce_func_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    NODE *const call = transduce_func_call_1(tc, cn);
    if (call != NULL && (cn->base.flags & PM_CALL_NODE_FLAGS_VARIABLE_CALL) != 0)
        call->head.flags.is_vcall = true;
    return call;
}

static NODE *
transduce_func_call_1(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    const pm_arguments_node_t *args = cn->arguments;
    size_t argc = args ? args->arguments.size : 0;

    /* block_given? — reaches the enclosing METHOD's block, not a nested block's.
     * Walk up to the method frame (like yield); flat read at method level, else
     * resolve through `depth` env links to the method frame's biseq.
     * `Kernel.block_given?` / `::Kernel::block_given?` mean the same thing and
     * take the same lowering (builder writes it that way). */
    if (argc == 0 && cn->block == NULL &&
        (cn->receiver == NULL || kp_kernel_const_p(tc, cn->receiver)) &&
        strcmp(kp_cid_cstr(tc, cn->name), "block_given?") == 0) {
        struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
        if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }   /* outside a method: legacy flat path */
        mf->uses_block = true;                                     /* the method reserves the block trio */
        if (depth > 0) {   /* the block is in an outer frame: the escape must keep it */
            if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
            if (depth > tc->frame->yield_depth)   tc->frame->yield_depth = depth;
        }
        if (depth == 0)
            return ALLOC_node_block_given(-3 - tc->chain);        /* method top-level: this frame's biseq cell */
        NODE *bg = ALLOC_node_block_given_outer(-2 - tc->chain, depth, -3);   /* prev_off = this block's EP; trio_base += method frame_size */
        bake_add(tc, &bg->u.node_block_given_outer.prev_off);
        add_bake_to(mf, &bg->u.node_block_given_outer.trio_base);
        return bg;
    }

    /* __method__ / __callee__ — the enclosing method's name (nil at top level),
     * baked at parse time (#__method__ returns the definition name). */
    if (argc == 0 && cn->block == NULL && cn->receiver == NULL) {
        const char *const nm = kp_cid_cstr(tc, cn->name);
        if (strcmp(nm, "__method__") == 0 || strcmp(nm, "__callee__") == 0) {
            uint32_t mid = 0;                            /* walk out through block frames to the enclosing method */
            for (const struct kp_frame *f = tc->frame; f; f = f->prev) {
                if (f->dm_body) return ALLOC_node_dm_name();   /* define_method: named at run time */
                if (f->method_mid) { mid = f->method_mid; break; }
            }
            if (mid == 0) mid = tc->eval_method_mid;   /* eval(str, binding): the binding's method */
            /* `__callee__` is the name the frame was CALLED under, which an alias
             * or `define_method(name, unbound)` copy renames — read it off the
             * frame's method entry.  (`__method__` stays the definition name.)
             * Only when the body IS the method's: inside a block the entry cell
             * belongs to the block frame, so keep the lexical bake there. */
            if (nm[2] == 'c' && mid != 0) {
                uint32_t cdepth = 0;
                for (const struct kp_frame *f = tc->frame; f; f = f->prev) {
                    if (f->dm_body) { cdepth = UINT32_MAX; break; }
                    if (f->method_mid) break;
                    cdepth++;
                }
                if (cdepth == 0) {
                    NODE *cl = ALLOC_node_callee(-4 - tc->chain, mid);
                    bake_add(tc, &cl->u.node_callee.dc_off);   /* method entry at base[-4] */
                    return cl;
                }
                if (cdepth != UINT32_MAX) {
                    if (cdepth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = cdepth;
                    NODE *cl = ALLOC_node_callee_outer(-2 - tc->chain, cdepth, mid);
                    bake_add(tc, &cl->u.node_callee_outer.prev_off);   /* this block's EP cell */
                    return cl;
                }
            }
            return ALLOC_node_lit(mid ? ID2SYM(mid) : KORB_NIL);
        }
    }

    /* `binding` — capture the LEXICAL local scope (this frame + every enclosing
     * block frame up to and including the method / class-body / toplevel frame)
     * into a Binding.  Baked as a packed u32 scope table (see
     * kp_binding_scope_tbl); the runtime materializes the whole env chain so
     * enclosing locals stay reachable (and writable) even after frames close. */
    if (cn->receiver == NULL && argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "binding") == 0) {
        return kp_make_binding_node_x(tc, kp_line(tc, (const pm_node_t *)cn), 1u);   /* a user's #binding wins */
    }

    /* `alias my_eval eval` in this file: the alias gets the same hidden-binding
     * lowering (CRuby's eval is special at the call site, not in the method). */
    /* `eval(str)` — CRuby evaluates in the CALLER's binding.  koruby has no
     * runtime frame metadata, so bake a binding for the call site and pass it
     * as the hidden 2nd argument.  `eval(str, nil, file, line)` means the same
     * thing (an explicit nil binding IS the caller's), so it takes the same
     * lowering with the file/line arguments carried through — without it the
     * builtin runs the string against a fresh `main` and a `def` inside is lost.
     * A real Binding or a splat form goes through the normal path.  A
     * user-defined `eval` would see the extra Binding — same caveat as CRuby's
     * own "eval is special" corners, accepted for compatibility. */
    if (cn->receiver == NULL && argc >= 1 && argc <= 4 && cn->block == NULL &&
        (argc == 1 || PM_NODE_TYPE_P(cn->arguments->arguments.nodes[1], PM_NIL_NODE)) &&
        (strcmp(kp_cid_cstr(tc, cn->name), "eval") == 0 || kp_is_eval_alias(tc, cn->name))) {
        bool plain = true;                               /* no splat/forwarding anywhere */
        for (uint32_t i = 0; i < (uint32_t)argc && plain; i++)
            plain = !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE) &&
                    !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_FORWARDING_ARGUMENTS_NODE);
        if (plain) {
            const uint32_t line = kp_line(tc, (const pm_node_t *)cn);
            /* [self, str, binding] plus whatever followed the nil (file, line) */
            const uint32_t cnt = (argc == 1) ? 3 : (uint32_t)argc + 1;
            NODE **argv = malloc(sizeof(NODE *) * cnt);
            if (!argv) abort();
            int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;
            argv[0] = bake_self(tc);
            argv[1] = transduce(tc, cn->arguments->arguments.nodes[0]);
            argv[2] = kp_make_binding_node(tc, line);    /* replaces the explicit nil */
            for (uint32_t i = 2; i < (uint32_t)argc; i++)
                argv[i + 1] = transduce(tc, cn->arguments->arguments.nodes[i]);
            tc->chain = saved;
            return KP_LINK(tc, node_call, korb_intern(tc->c->vm, "eval", 4), kp_flink(tc, line), argv, cnt);
        }
    }

    /* bare `instance_eval(str...)` / `class_eval` / `module_eval` — same hidden
     * binding contract as the explicit-receiver lowering in transduce_call
     * (caller locals; faraday generates methods with `class_eval <<-RUBY`). */
    if (cn->block == NULL && argc >= 1 && argc <= 3 &&
        (strcmp(kp_cid_cstr(tc, cn->name), "instance_eval") == 0 ||
         strcmp(kp_cid_cstr(tc, cn->name), "class_eval") == 0 ||
         strcmp(kp_cid_cstr(tc, cn->name), "module_eval") == 0)) {
        bool plain = true;
        for (size_t i = 0; i < argc; i++) {
            const pm_node_t *an = cn->arguments->arguments.nodes[i];
            if (PM_NODE_TYPE_P(an, PM_SPLAT_NODE) || PM_NODE_TYPE_P(an, PM_FORWARDING_ARGUMENTS_NODE) ||
                PM_NODE_TYPE_P(an, PM_KEYWORD_HASH_NODE) || PM_NODE_TYPE_P(an, PM_BLOCK_ARGUMENT_NODE)) { plain = false; break; }
        }
        if (plain) {
            const uint32_t line2 = kp_line(tc, (const pm_node_t *)cn);
            uint32_t cnt = 1u + (uint32_t)argc + 1u;     /* [self, args..., binding] */
            NODE **argv = malloc(sizeof(NODE *) * cnt);
            if (!argv) abort();
            int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;
            argv[0] = bake_self(tc);
            for (size_t i = 0; i < argc; i++)
                argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
            argv[cnt - 1] = kp_make_binding_node(tc, line2);
            tc->chain = saved;
            return KP_LINK(tc, node_call, kp_intern_cid(tc, cn->name), kp_flink(tc, line2), argv, cnt);
        }
    }

    /* `local_variables` — desugar to `binding.local_variables`.  The Binding node is
     * built staged as the send's receiver (KP_SEND0_SC) so its baked offsets match. */
    if (cn->receiver == NULL && argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "local_variables") == 0) {
        NODE *nb;
        WITH_CHAIN(tc, KP_SEND0_SC, (nb = kp_make_binding_node(tc, line)));
        return kp_send0(tc, korb_intern(tc->c->vm, "local_variables", 15), line, nb);
    }

    /* bare `module_function` is a normal runtime call (korb_m_module_function
     * sets cur_visibility mode 3: subsequent defs become private instance methods
     * with a public module-singleton copy).  No parse-time interception. */

    /* attr_reader/writer/accessor :sym... → node_attr (defines getters/setters
     * on self = the enclosing class). */
    if (cn->block == NULL && argc > 0) {
        const char *nm = kp_cid_cstr(tc, cn->name);
        int mode = !strcmp(nm, "attr_reader") ? 0 : !strcmp(nm, "attr_writer") ? 1
                 : !strcmp(nm, "attr_accessor") ? 2 : -1;
        if (mode >= 0) {
            bool all_syms = true;
            for (size_t i = 0; i < argc; i++)
                if (!PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SYMBOL_NODE)) { all_syms = false; break; }
            if (all_syms) {
                uint32_t count = (uint32_t)argc * (mode == 2 ? 2u : 1u);
                struct korb_attr_desc *descs = malloc(sizeof(*descs) * count);
                if (!descs) abort();
                uint32_t di = 0;
                for (size_t i = 0; i < argc; i++) {
                    const pm_symbol_node_t *sn = (const pm_symbol_node_t *)args->arguments.nodes[i];
                    const char *bn = (const char *)pm_string_source(&sn->unescaped);
                    size_t blen = pm_string_length(&sn->unescaped);
                    char buf[256];
                    if (blen + 2 >= sizeof(buf)) { free(descs); return kp_unsupported(tc, (const pm_node_t *)cn, "attr name too long"); }
                    buf[0] = '@'; memcpy(buf + 1, bn, blen);                 /* "@name" */
                    uint32_t ivar = korb_intern(tc->c->vm, buf, blen + 1);
                    uint32_t rmid = korb_intern(tc->c->vm, bn, blen);        /* "name" */
                    memcpy(buf, bn, blen); buf[blen] = '=';                  /* "name=" */
                    uint32_t wmid = korb_intern(tc->c->vm, buf, blen + 1);
                    if (mode != 1) { descs[di].mid = rmid; descs[di].ivar = ivar; descs[di].is_writer = 0; di++; }
                    if (mode != 0) { descs[di].mid = wmid; descs[di].ivar = ivar; descs[di].is_writer = 1; di++; }
                }
                /* the ORIGINAL argument names, so a class that overrides
                 * attr_reader/writer/accessor can still be dispatched */
                uint32_t *names = malloc(sizeof(uint32_t) * argc);
                if (!names) abort();
                for (size_t i = 0; i < argc; i++) {
                    const pm_symbol_node_t *sn2 = (const pm_symbol_node_t *)args->arguments.nodes[i];
                    names[i] = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn2->unescaped),
                                           (uint32_t)pm_string_length(&sn2->unescaped));
                }
                { NODE *_na = ALLOC_node_attr(-1 - tc->chain, descs, count, kp_line(tc, (const pm_node_t *)cn),
                                              kp_intern_cid(tc, cn->name), names, (uint32_t)argc);
                  bake_add(tc, &_na->u.node_attr.self_off); return _na; }
            }
        }
    }

    if (cn->block) {
        /* proc { } / lambda { } — reify the literal block into a Proc object. */
        if (argc == 0 && PM_NODE_TYPE_P(cn->block, PM_BLOCK_NODE)) {
            const char *cnm = kp_cid_cstr(tc, cn->name);
            int is_lam = !strcmp(cnm, "lambda");
            if (is_lam || !strcmp(cnm, "proc")) {
                tc->pending_lambda_body = is_lam != 0;
                NODE *entry = transduce_block(tc, (const pm_block_node_t *)cn->block);
                tc->pending_lambda_body = false;
                if (entry->head.kind != &kind_node_entry) return entry;   /* unsupported params → propagate (don't reify a non-entry) */
                int32_t self_off = -1 - tc->chain;
                NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, (uint32_t)is_lam, 1u);
                bake_add(tc, &mk->u.node_make_proc.def_env_off);
        bake_add(tc, &mk->u.node_make_proc.self_off);   /* self at base[-1] */
                return mk;
            }
        }
        /* `m(args, &proc)` (implicit self) — forward a runtime Proc.  A bare `&`
         * forwards the enclosing `def m(&)`'s block straight from its synth slot. */
        if (PM_NODE_TYPE_P(cn->block, PM_BLOCK_ARGUMENT_NODE)) {
            const pm_block_argument_node_t *ba = (const pm_block_argument_node_t *)cn->block;
            if (!ba->expression || !PM_NODE_TYPE_P(ba->expression, PM_SYMBOL_NODE)) {
                const bool anon_blk = (ba->expression == NULL);
                uint32_t anon_d = 0; int32_t anon_s = -1; NODE *anon_pre = NULL;
                if (anon_blk && !kp_anon_find(tc, KP_ANON_BLK, &anon_d, &anon_s))
                    return kp_unsupported(tc, (const pm_node_t *)cn, "bare & outside a (&) method body");
                uint32_t pslot = anon_blk ? (uint32_t)kp_fwd_blk_local(tc, anon_d, anon_s, &anon_pre)
                                          : alloc_synth_local(tc);
                bool has_splat = false;
                for (size_t i = 0; i < argc; i++)
                    if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
                /* CRuby evaluates `&expr` LAST (after the arguments).  Only
                 * materialize that when something here can observe the order —
                 * otherwise keep the cheap shape. */
                bool blk_last = argc > 0 && !anon_blk && !kp_order_transparent(ba->expression);
                for (size_t i = 0; !blk_last && !anon_blk && i < argc; i++)
                    if (!kp_order_transparent(args->arguments.nodes[i])) blk_last = true;
                NODE *pset = anon_blk ? anon_pre
                           : (blk_last ? NULL : bake_lset(tc, pslot, transduce(tc, ba->expression)));
                if (has_splat) {
                    /* `f(*arr, &proc)` (implicit self) — build the args Array; one
                     * staged child = the array, self via self_off, proc via proc_off. */
                    int32_t self_off = -1 - tc->chain - 1;
                    int32_t proc_off = (int32_t)pslot - tc->chain - 1;
                    NODE *arr, *pre = pset;
                    if (blk_last) {   /* stash the built Array, then evaluate `&expr` */
                        const uint32_t ta = alloc_synth_local(tc);
                        NODE *const sa = bake_lset(tc, ta, build_array(tc, args->arguments.nodes, argc, (uint32_t)argc));
                        pre = ALLOC_node_seq(sa, bake_lset(tc, pslot, transduce(tc, ba->expression)));
                        WITH_CHAIN(tc, 1, (arr = bake_lget(tc, ta)));
                    } else {
                        WITH_CHAIN(tc, 1, (arr = build_array(tc, args->arguments.nodes, argc, (uint32_t)argc)));
                    }
                    NODE *_cs = KP_LINK(tc, node_call_splat_blkproc, mid, kp_flink_at(tc, line, 2), self_off, proc_off, arr);
                    bake_add(tc, &_cs->u.node_call_splat_blkproc.self_off);
                    bake_add(tc, &_cs->u.node_call_splat_blkproc.proc_off);
                    return pre ? ALLOC_node_seq(pre, _cs) : _cs;
                }
                uint32_t cnt = 1u + (uint32_t)argc;     /* self receiver + args */
                NODE **argv = malloc(sizeof(NODE *) * cnt);
                if (!argv) abort();
                int32_t saved = tc->chain;
                tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
                argv[0] = bake_self(tc);   /* self → base[-1] */
                for (size_t i = 0; i < argc; i++)
                    argv[1 + i] = transduce(tc, args->arguments.nodes[i]);
                if (blk_last) {
                    /* thread `&expr` through the LAST staged child: stash that
                     * child's value, evaluate the block expression, hand it back */
                    const uint32_t tmp = alloc_synth_local(tc);
                    NODE *const last = ALLOC_node_seq(bake_lset(tc, pslot, transduce(tc, ba->expression)),
                                                      bake_lget(tc, tmp));
                    argv[cnt - 1] = ALLOC_node_seq(bake_lset(tc, tmp, argv[cnt - 1]), last);
                }
                tc->chain = saved;
                NODE *call = KP_LINK(tc, node_call_blkproc, mid, kp_flink(tc, line), (int32_t)pslot - tc->chain - (int32_t)cnt - KORB_FRAME_HDR, argv, cnt);
                bake_add(tc, &call->u.node_call_blkproc.proc_off);
                return pset ? ALLOC_node_seq(pset, call) : call;
            }
        }
        tc->next_block_is_dm = kp_defines_method_p(tc, mid);
        NODE *entry = kp_block_entry(tc, cn->block);
        tc->next_block_is_dm = false;
        if (!entry) return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument (only literal block or &:sym)");
        /* `f(*arr) { ... }` — build the args Array and dispatch dynamically with
         * the block threaded (one staged child = the args array). */
        {
            bool has_splat = false;
            for (size_t i = 0; i < argc; i++)
                if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
            if (has_splat) {
                int32_t self_off = -1 - tc->chain - 1;   /* one staged child: the args array */
                int32_t def_env_off = -tc->chain - 1;    /* caller frame base (tagged |1 at eval) */
                NODE *arr;
                WITH_CHAIN(tc, 1, (arr = build_call_args(tc, args->arguments.nodes, argc)));
                NODE *_cs = KP_LINK(tc, node_call_splat_blk, mid, kp_flink_at(tc, line, 2), self_off, entry, def_env_off, arr);
                bake_add(tc, &_cs->u.node_call_splat_blk.self_off);
                bake_add(tc, &_cs->u.node_call_splat_blk.def_env_off);
                return _cs;
            }
        }
        return transduce_call_with_block(tc, cn, mid, line, args, argc, entry);
    }

    /* `inner(...)` — forward the enclosing `def m(...)`'s collected args: splat the
     * synth rest local (keywords ride in its trailing hash) and thread the block
     * from the synth forwarding slot.  node_call_splat_blkproc treats a nil proc
     * as "no block", so the blockless call needs no separate node. */
    if (argc >= 1 && PM_NODE_TYPE_P(args->arguments.nodes[argc - 1], PM_FORWARDING_ARGUMENTS_NODE)) {
        uint32_t fdep; int32_t fslot, fblk;
        if (!kp_fwd_find(tc, &fdep, &fslot, &fblk))
            return kp_unsupported(tc, (const pm_node_t *)cn, "... forwarding outside a (...) method body");
        NODE *blk_pre; const int32_t blk_local = kp_fwd_blk_local(tc, fdep, fblk, &blk_pre);
        int32_t self_off = -1 - tc->chain - 1;
        int32_t proc_off = blk_local - tc->chain - 1;
        NODE *arr;
        WITH_CHAIN(tc, 1, (arr = (argc == 1)
            ? kp_fwd_args(tc, fdep, fslot)
            : build_array_with_fwd(tc, args->arguments.nodes, argc - 1, fdep, fslot)));   /* f(a, ..., ...) */
        NODE *_cs = KP_LINK(tc, node_call_splat_blkproc, mid, kp_flink_at(tc, line, 2), self_off, proc_off, arr);
        bake_add(tc, &_cs->u.node_call_splat_blkproc.self_off);
        bake_add(tc, &_cs->u.node_call_splat_blkproc.proc_off);
        return blk_pre ? ALLOC_node_seq(blk_pre, _cs) : _cs;
    }

    /* actual `*splat` call f(*arr): build the args Array, dispatch dynamically */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            int32_t self_off = -1 - tc->chain - 1;       /* one staged child: the args array */
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_call_args(tc, args->arguments.nodes, argc)));
            { NODE *_cs = KP_LINK(tc, node_call_splat, mid, kp_flink_at(tc, line, 2), self_off, arr); bake_add(tc, &_cs->u.node_call_splat.self_off); return _cs; }
        }
    }

    /* `f(pos..., k: v...)` — trailing keyword hash with literal symbol keys (no
     * **splat, no `k:` shorthand) → hash-free keyword call (node_call_kw). */
    if (argc >= 1 && PM_NODE_TYPE_P(args->arguments.nodes[argc - 1], PM_KEYWORD_HASH_NODE)) {
        const pm_keyword_hash_node_t *kh = (const pm_keyword_hash_node_t *)args->arguments.nodes[argc - 1];
        bool ok = kh->elements.size > 0;
        for (size_t i = 0; ok && i < kh->elements.size; i++) {
            const pm_node_t *e = kh->elements.nodes[i];
            if (!PM_NODE_TYPE_P(e, PM_ASSOC_NODE)) { ok = false; break; }
            const pm_assoc_node_t *as = (const pm_assoc_node_t *)e;
            if (!PM_NODE_TYPE_P(as->key, PM_SYMBOL_NODE) || !as->value || PM_NODE_TYPE_P(as->value, PM_IMPLICIT_NODE)) ok = false;
        }
        if (ok) {
            const uint32_t pos_argc = (uint32_t)argc - 1, kw_cnt = (uint32_t)kh->elements.size, total = pos_argc + kw_cnt;
            uint32_t *kw_syms = malloc(sizeof(uint32_t) * kw_cnt);
            if (!kw_syms) abort();
            for (uint32_t j = 0; j < kw_cnt; j++) {
                const pm_symbol_node_t *sn = (const pm_symbol_node_t *)((const pm_assoc_node_t *)kh->elements.nodes[j])->key;
                kw_syms[j] = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), pm_string_length(&sn->unescaped));
            }
            uint32_t cnt = 1u + total;                   /* self receiver + pos + kw */
            NODE **argv = malloc(sizeof(NODE *) * cnt);
            if (!argv) abort();
            int32_t saved = tc->chain; tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
            argv[0] = bake_self(tc);   /* self → base[-1] */
            for (uint32_t i = 0; i < pos_argc; i++) argv[1 + i] = transduce(tc, args->arguments.nodes[i]);
            for (uint32_t j = 0; j < kw_cnt; j++) argv[1 + pos_argc + j] = transduce(tc, ((const pm_assoc_node_t *)kh->elements.nodes[j])->value);
            tc->chain = saved;
            /* `f(a: 1, a: 2)` warns like a Hash literal would */
            return kp_warn_dup_hash_keys(tc, kh->elements.nodes, kh->elements.size,
                                         KP_LINK(tc, node_call_kw, mid, kp_flink(tc, line), pos_argc, (const char *)(const void *)kw_syms, argv, cnt));
        }
    }

    /* Stage self as the receiver (argv[0]) — uniform with node_send's [recv,args]
     * so the callee's base[-1] is always the receiver cell (consumable as the EP
     * cell on return).  node_call keeps the global-fn (cc) path for top-level defs. */
    {
        uint32_t cnt = 1u + (uint32_t)argc;
        NODE **argv = malloc(sizeof(NODE *) * cnt);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr: dispatcher reserves HDR cells → cursor +HDR */
        argv[0] = bake_self(tc);      /* self → base[-1] */
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, args->arguments.nodes[i]);
        tc->chain = saved;
        /* trailing `**h` / `k: v, **h` bundle → drop an empty kwargs Hash at call time */
        if (argc >= 1 && PM_NODE_TYPE_P(args->arguments.nodes[argc - 1], PM_KEYWORD_HASH_NODE))
            return KP_LINK(tc, node_call_kws, mid, kp_flink(tc, line), argv, cnt);
        return KP_LINK(tc, node_call, mid, kp_flink(tc, line), argv, cnt);
    }
}

static uint8_t kp_forced_enc(pm_node_flags_t flags, uint8_t src_enc);   /* fwd (literal `.freeze`) */

static NODE *
transduce_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    if (cn->receiver == NULL) {
        return transduce_func_call(tc, cn);
    }
    /* `Kernel.block_given?` / `::Kernel::block_given?` mean the bare form
     * (builder writes it that way); the receiver is only a namespace here. */
    if (cn->arguments == NULL && cn->block == NULL && kp_kernel_const_p(tc, cn->receiver) &&
        strcmp(kp_cid_cstr(tc, cn->name), "block_given?") == 0)
        return transduce_func_call(tc, cn);

    const char *name = kp_cid_cstr(tc, cn->name);
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    kp_note_backref_call(tc, name);   /* a name that can set `$~` makes this method save/restore it */
    size_t argc = cn->arguments ? cn->arguments->arguments.size : 0;

    /* `"lit".freeze` IS the fstring-pool entry (CRuby's opt_str_freeze), so
     * `-"lit".freeze` gives back the same object.  Without this a literal
     * `.freeze` yields a private frozen copy and `-it` finds an unrelated pool
     * entry instead (rubyspec core/string/uminus). */
    if (argc == 0 && cn->block == NULL && strcmp(name, "freeze") == 0 &&
        !(cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) &&
        PM_NODE_TYPE_P(cn->receiver, PM_STRING_NODE)) {
        const pm_string_node_t *const sn = (const pm_string_node_t *)cn->receiver;
        uint32_t slen;
        const char *const sbytes = kp_strdup_pm(&sn->unescaped, &slen);
        const uint8_t senc = kp_forced_enc(sn->base.flags, tc->src_enc);
        return (senc != KORB_ENC_UTF8) ? ALLOC_node_str_enc(sbytes, slen, senc, 2u)
                                       : ALLOC_node_str_frozen(sbytes, slen);
    }

    /* `recv.attr = v` is an assignment: it yields v, not whatever the setter
     * returns.  Stage v in a temp, call the setter, hand the temp back.  With
     * safe navigation a nil receiver skips the argument too. */
    if ((cn->base.flags & PM_CALL_NODE_FLAGS_ATTRIBUTE_WRITE) && argc == 1 && cn->block == NULL &&
        !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE)) {
        const bool safe = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;
        const uint32_t tr = alloc_synth_local(tc), tv = alloc_synth_local(tc);
        /* receiver first, then the right-hand side (CRuby's evaluation order) */
        NODE *const store_recv = bake_lset(tc, tr, transduce(tc, cn->receiver));
        NODE *const store_val = bake_lset(tc, tv, transduce(tc, cn->arguments->arguments.nodes[0]));
        /* the setter call is an ordinary node_send: it must carry self_off so a
         * private `attr=` stays callable only through `self.` */
        NODE **argv = malloc(sizeof(NODE *) * 2);
        if (!argv) abort();
        const int32_t self_off = -1 - tc->chain - 2 - KORB_FRAME_HDR;
        const int32_t saved = tc->chain;
        tc->chain = saved + 2 + KORB_FRAME_HDR;
        argv[0] = bake_lget(tc, tr);
        argv[1] = bake_lget(tc, tv);
        tc->chain = saved;
        NODE *const set = KP_LINK(tc, node_send, mid, kp_flink(tc, line), self_off, argv, 2);
        bake_add(tc, &set->u.node_send.self_off);
        NODE *body = ALLOC_node_seq(store_val, ALLOC_node_seq(set, bake_lget(tc, tv)));
        if (safe) body = ALLOC_node_nil_guard(bake_lget(tc, tr), body);   /* nil receiver: no rhs, no call */
        return ALLOC_node_seq(store_recv, body);
    }

    /* `Module.nesting` — the lexically-enclosing class/module bodies, innermost
     * first.  Resolved at parse time from the frame chain (the names) and at
     * runtime to the live class objects (koruby's flat const table). */
    if (argc == 0 && cn->block == NULL && strcmp(name, "nesting") == 0 &&
        PM_NODE_TYPE_P(cn->receiver, PM_CONSTANT_READ_NODE) &&
        strcmp(kp_cid_cstr(tc, ((const pm_constant_read_node_t *)cn->receiver)->name), "Module") == 0) {
        uint32_t syms[64]; uint32_t n = 0;
        for (struct kp_frame *f = tc->frame; f && n < 64; f = f->prev)
            if (f->class_name_sym != 0) syms[n++] = f->class_name_sym;
        for (uint32_t k = tc->seed_chain_len; k-- > 0 && n < 64; ) syms[n++] = tc->seed_chain[k];   /* eval(String) cref */
        /* calloc, not malloc: the operand is typed `const char *`, so the node
         * hash runs hash_cstr over these bytes.  At top level n == 0 and the
         * one-element padding would be uninitialized — a per-process hash, which
         * makes --compiled-only miss the SD the bake produced. */
        uint32_t *baked = calloc(n ? n : 1, sizeof(uint32_t));   /* immortal (baked into the node) */
        if (!baked) abort();
        for (uint32_t i = 0; i < n; i++) baked[i] = syms[i];
        return ALLOC_node_nesting((const char *)(const void *)baked, n);
    }

    /* `recv&.m(...)`.  The operator / aref fast paths below build nodes that
     * dispatch unconditionally, so a safe call must skip them and take the
     * generic node_send_safe path; the splat and `...` forms have no such node
     * and get an explicit temp + nil guard instead. */
    const bool sn = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;

    /* operator calls → dedicated binop / unary nodes (a splat arg has no fixed
     * arity, so it falls through to the generic splat send below) */
    enum kp_binop op = kp_binop_kind(name);
    if (!sn && op != KP_BINOP_NONE && argc == 1 &&
        kp_no_splat_p(cn->arguments->arguments.nodes, 1)) {
        uint32_t n_slots = kind_node_plus.slot_count;   /* lhs staging (all binops alike) */
        NODE *lhs, *rhs;
        WITH_CHAIN(tc, n_slots, (lhs = transduce(tc, cn->receiver),
                                 rhs = transduce(tc, cn->arguments->arguments.nodes[0])));
        return alloc_binop(tc, op, lhs, rhs, line);
    }
    if (!sn && strcmp(name, "-@") == 0 && argc == 0) {
        return ALLOC_node_neg(transduce(tc, cn->receiver), line);
    }
    if (!sn && strcmp(name, "!") == 0 && argc == 0) {
        return ALLOC_node_not(transduce(tc, cn->receiver));
    }

    /* recv[idx] / recv[idx] = val → type-fast-path nodes (Array+fixnum inline,
     * else deopt to a real send).  No block, no splat args. */
    if (!cn->block && !sn) {
        if (mid == tc->c->vm->mid_aref && argc == 1 &&
            kp_no_splat_p(cn->arguments->arguments.nodes, 1)) {
            NODE *recv, *idx;
            WITH_CHAIN(tc, kind_node_aref.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0])));
            return KP_LINK(tc, node_aref, kp_flink_at(tc, line, kind_node_aref.slot_count), recv, idx);
        }
        if (mid == tc->c->vm->mid_aset && argc == 2 &&
            kp_no_splat_p(cn->arguments->arguments.nodes, 2)) {
            NODE *recv, *idx, *val;
            WITH_CHAIN(tc, kind_node_aset.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0]),
                        val  = transduce(tc, cn->arguments->arguments.nodes[1])));
            return KP_LINK(tc, node_aset, kp_flink_at(tc, line, kind_node_aset.slot_count), recv, idx, val);
        }
        /* `recv[i, j, ...] = v` (more than one index): an element assignment
         * yields the assigned value, not []='s return.  Stage v in a temp while
         * the send runs, then hand the temp back. */
        if (mid == tc->c->vm->mid_aset && argc > 2 && kp_no_splat_p(cn->arguments->arguments.nodes, argc)) {
            const uint32_t tv = alloc_synth_local(tc);
            NODE **const av = malloc(sizeof(NODE *) * argc);
            if (!av) abort();
            const int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)KP_SENDN_SC((uint32_t)argc);
            NODE *const r = transduce(tc, cn->receiver);
            for (size_t i = 0; i + 1 < argc; i++) av[i] = transduce(tc, cn->arguments->arguments.nodes[i]);
            av[argc - 1] = bake_lset(tc, tv, transduce(tc, cn->arguments->arguments.nodes[argc - 1]));
            tc->chain = saved;
            NODE *const set = kp_send_n(tc, mid, line, r, av, (uint32_t)argc);
            free(av);
            return ALLOC_node_seq(set, bake_lget(tc, tv));
        }
    }

    /* receiver method dispatch with a block: recv.mid(args) { ... } or &:sym */
    if (cn->block) {
        /* `recv&.m(...) { ... }` — safe navigation WITH a block: evaluate the
         * receiver once into a temp, guard on nil, and let the normal block-call
         * code read the temp as its receiver (recv_node below).  node_send_safe
         * only covers the block-less form. */
        const bool blk_safe = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;
        int32_t safe_tmp = -1;
        NODE *safe_set = NULL;
        if (blk_safe) {
            safe_tmp = (int32_t)alloc_synth_local(tc);
            safe_set = bake_lset(tc, (uint32_t)safe_tmp, transduce(tc, cn->receiver));
        }
        #define RECV_NODE() (safe_tmp >= 0 ? bake_lget(tc, (uint32_t)safe_tmp) : transduce(tc, cn->receiver))
        #define SAFE_WRAP(call) (safe_tmp >= 0 ? ALLOC_node_seq(safe_set, ALLOC_node_nil_guard(bake_lget(tc, (uint32_t)safe_tmp), (call))) : (call))
        /* `recv.m(args, &proc)` — forward a runtime Proc.  Evaluate the proc into
         * a rooted synth local, then node_send_blkproc reads it. */
        if (PM_NODE_TYPE_P(cn->block, PM_BLOCK_ARGUMENT_NODE)) {
            const pm_block_argument_node_t *ba = (const pm_block_argument_node_t *)cn->block;
            if (!ba->expression || !PM_NODE_TYPE_P(ba->expression, PM_SYMBOL_NODE)) {
                const bool anon_blk = (ba->expression == NULL);
                uint32_t anon_d = 0; int32_t anon_s = -1; NODE *anon_pre = NULL;
                if (anon_blk && !kp_anon_find(tc, KP_ANON_BLK, &anon_d, &anon_s))
                    return kp_unsupported(tc, (const pm_node_t *)cn, "bare & outside a (&) method body");
                uint32_t pslot = anon_blk ? (uint32_t)kp_fwd_blk_local(tc, anon_d, anon_s, &anon_pre)
                                          : alloc_synth_local(tc);
                bool has_splat = false;
                for (size_t i = 0; i < argc; i++)
                    if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
                /* CRuby evaluates `&expr` LAST (after the receiver and the
                 * arguments).  Only materialize that when something here can
                 * observe the order — otherwise keep the cheap shape. */
                bool blk_last = !anon_blk && !kp_order_transparent(ba->expression);
                if (!blk_last && !anon_blk && !kp_order_transparent(cn->receiver)) blk_last = true;
                /* a bare `&` reads a slot — there is no expression to order, and
                 * NULLing through the blk_last path would crash the transducer */
                for (size_t i = 0; !blk_last && !anon_blk && i < argc; i++)
                    if (!kp_order_transparent(cn->arguments->arguments.nodes[i])) blk_last = true;
                NODE *pset = anon_blk ? anon_pre
                           : (blk_last ? NULL : bake_lset(tc, pslot, transduce(tc, ba->expression)));
                if (has_splat) {
                    /* `recv.m(*arr, &proc)` — build the args Array; node_send_splat_blkproc
                     * coerces the proc and forwards it (2 staged children: recv + array). */
                    int32_t proc_off = (int32_t)pslot - tc->chain - 2;
                    const int32_t bp_self_off = -tc->chain - 3;   /* caller self (base[-1]) */
                    NODE *recv, *arr, *pre = pset;
                    if (blk_last) {   /* stash receiver + built Array, then evaluate `&expr` */
                        const uint32_t tr = alloc_synth_local(tc), ta = alloc_synth_local(tc);
                        NODE *const sr = bake_lset(tc, tr, RECV_NODE());
                        NODE *const sa = bake_lset(tc, ta, build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc));
                        pre = ALLOC_node_seq(sr, ALLOC_node_seq(sa, bake_lset(tc, pslot, transduce(tc, ba->expression))));
                        WITH_CHAIN(tc, 2, (recv = bake_lget(tc, tr), arr = bake_lget(tc, ta)));
                    } else {
                        WITH_CHAIN(tc, 2, (recv = RECV_NODE(),
                                           arr  = build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc)));
                    }
                    NODE *_cs = KP_LINK(tc, node_send_splat_blkproc, mid, kp_flink_at(tc, line, 2), bp_self_off, proc_off, recv, arr);
                    bake_add(tc, &_cs->u.node_send_splat_blkproc.self_off);
                    bake_add(tc, &_cs->u.node_send_splat_blkproc.proc_off);
                    return SAFE_WRAP(pre ? ALLOC_node_seq(pre, _cs) : _cs);
                }
                uint32_t sc = 1u + (uint32_t)argc;
                NODE **argv = malloc(sizeof(NODE *) * sc);
                if (!argv) abort();
                int32_t saved = tc->chain;
                tc->chain = saved + (int32_t)sc + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
                argv[0] = RECV_NODE();
                for (size_t i = 0; i < argc; i++)
                    argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
                if (blk_last) {
                    /* thread `&expr` through the LAST staged child: stash that
                     * child's value, evaluate the block expression, hand it back */
                    const uint32_t tmp = alloc_synth_local(tc);
                    NODE *const last = ALLOC_node_seq(bake_lset(tc, pslot, transduce(tc, ba->expression)),
                                                      bake_lget(tc, tmp));
                    argv[sc - 1] = ALLOC_node_seq(bake_lset(tc, tmp, argv[sc - 1]), last);
                }
                tc->chain = saved;
                NODE *call = KP_LINK(tc, node_send_blkproc, mid, kp_flink(tc, line),
                                     -1 - tc->chain - (int32_t)sc - KORB_FRAME_HDR,
                                     (int32_t)pslot - tc->chain - (int32_t)sc - KORB_FRAME_HDR, argv, sc);
                bake_add(tc, &call->u.node_send_blkproc.self_off);
                bake_add(tc, &call->u.node_send_blkproc.proc_off);
                return SAFE_WRAP(pset ? ALLOC_node_seq(pset, call) : call);
            }
        }
        tc->next_block_is_dm = kp_defines_method_p(tc, mid);
        NODE *entry = kp_block_entry(tc, cn->block);
        tc->next_block_is_dm = false;
        if (!entry) return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument (only literal block or &:sym)");
        /* `recv.m(*arr) { ... }` — build the args Array and dispatch dynamically
         * with the block threaded (two staged children: recv + the args array). */
        {
            bool has_splat = false;
            for (size_t i = 0; i < argc; i++)
                if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
            if (has_splat) {
                int32_t self_off = -tc->chain - 3;       /* caller self (base[-1]), 2 staged children */
                int32_t def_env_off = -tc->chain - 2;    /* caller frame base (tagged |1 at eval) */
                NODE *recv, *arr;
                WITH_CHAIN(tc, 2, (recv = RECV_NODE(),
                                   arr  = build_call_args(tc, cn->arguments->arguments.nodes, argc)));
                NODE *_cs = KP_LINK(tc, node_send_splat_blk, mid, kp_flink_at(tc, line, 2), self_off, entry, def_env_off, recv, arr);
                bake_add(tc, &_cs->u.node_send_splat_blk.self_off);
                bake_add(tc, &_cs->u.node_send_splat_blk.def_env_off);
                return SAFE_WRAP(_cs);
            }
        }
        /* def_env_off: cursor → caller frame base = -(chain + staging); staging
         * = recv(1) + argc.  bake_add fixes up by the caller's frame_size.
         * node_send_blk stages [recv, args...] via argv@children (any arity). */
        uint32_t sc = 1u + (uint32_t)argc;
        int32_t self_off = -1 - tc->chain - (int32_t)sc - KORB_FRAME_HDR;   /* block's captured self, from the +HDR-shifted cursor */
        NODE **argv = malloc(sizeof(NODE *) * sc);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)sc + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
        argv[0] = RECV_NODE();
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        NODE *call = KP_LINK(tc, node_send_blk, mid, kp_flink(tc, line), self_off, entry, -(tc->chain + (int32_t)sc + KORB_FRAME_HDR), argv, sc);
        bake_add(tc, &call->u.node_send_blk.def_env_off);
        bake_add(tc, &call->u.node_send_blk.self_off);    /* captured self at base[-1] (bottom header) */
        return SAFE_WRAP(call);
    }
    #undef RECV_NODE
    #undef SAFE_WRAP
    /* `recv.m(...)` — same forwarding as the implicit-self case, with the receiver
     * as the extra staged child. */
    if (argc >= 1 && PM_NODE_TYPE_P(cn->arguments->arguments.nodes[argc - 1], PM_FORWARDING_ARGUMENTS_NODE)) {
        uint32_t fdep; int32_t fslot, fblk;
        if (!kp_fwd_find(tc, &fdep, &fslot, &fblk))
            return kp_unsupported(tc, (const pm_node_t *)cn, "... forwarding outside a (...) method body");
        NODE *blk_pre; const int32_t blk_local = kp_fwd_blk_local(tc, fdep, fblk, &blk_pre);
        int32_t proc_off = blk_local - tc->chain - 2;
        const int32_t fwd_self_off = -tc->chain - 3;   /* caller self (base[-1]) */
        int32_t sn_tmp = sn ? (int32_t)alloc_synth_local(tc) : -1;
        NODE *sn_set = NULL, *recv, *arr;
        if (sn) sn_set = bake_lset(tc, (uint32_t)sn_tmp, transduce(tc, cn->receiver));
        WITH_CHAIN(tc, 2, (recv = sn ? bake_lget(tc, (uint32_t)sn_tmp) : transduce(tc, cn->receiver),
                           arr  = (argc == 1)
                               ? kp_fwd_args(tc, fdep, fslot)
                               : build_array_with_fwd(tc, cn->arguments->arguments.nodes, argc - 1, fdep, fslot)));
        NODE *_cs = KP_LINK(tc, node_send_splat_blkproc, mid, kp_flink_at(tc, line, 2), fwd_self_off, proc_off, recv, arr);
        bake_add(tc, &_cs->u.node_send_splat_blkproc.self_off);
        bake_add(tc, &_cs->u.node_send_splat_blkproc.proc_off);
        NODE *_out = blk_pre ? ALLOC_node_seq(blk_pre, _cs) : _cs;
        if (sn) return ALLOC_node_seq(sn_set, ALLOC_node_nil_guard(bake_lget(tc, (uint32_t)sn_tmp), _out));
        return _out;
    }
    /* actual `*splat` receiver call recv.m(*arr): build args Array, dynamic dispatch */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            const int32_t sn_tmp = sn ? (int32_t)alloc_synth_local(tc) : -1;
            const int32_t splat_self_off = -tc->chain - 3;   /* caller self (base[-1]), 2 staged children */
            NODE *sn_set = NULL, *recv, *arr;
            if (sn) sn_set = bake_lset(tc, (uint32_t)sn_tmp, transduce(tc, cn->receiver));
            WITH_CHAIN(tc, 2, (recv = sn ? bake_lget(tc, (uint32_t)sn_tmp) : transduce(tc, cn->receiver),
                               arr  = build_call_args(tc, cn->arguments->arguments.nodes, argc)));
            /* an element assignment evaluates to the assigned value, not to []='s
             * return value, even when the index list is splatted */
            NODE *_sp;
            if (mid == tc->c->vm->mid_aset) {
                _sp = KP_LINK(tc, node_send_splat_aset, mid, kp_flink_at(tc, line, 2), recv, arr);
            } else {
                _sp = KP_LINK(tc, node_send_splat, mid, kp_flink_at(tc, line, 2), splat_self_off, recv, arr);
                bake_add(tc, &_sp->u.node_send_splat.self_off);
            }
            if (sn) return ALLOC_node_seq(sn_set, ALLOC_node_nil_guard(bake_lget(tc, (uint32_t)sn_tmp), _sp));
            return _sp;
        }
    }
    /* instance_eval / class_eval / module_eval with positional args (the String
     * form): append the caller's binding as a hidden trailing arg so the eval'd
     * string sees (and writes back) caller locals — same contract as the
     * eval(str) lowering above.  Block forms don't need it. */
    if (cn->block == NULL && argc >= 1 && argc <= 3 &&
        !(cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) &&
        (strcmp(name, "instance_eval") == 0 || strcmp(name, "class_eval") == 0 ||
         strcmp(name, "module_eval") == 0)) {
        bool plain = true;
        for (size_t i = 0; i < argc; i++) {
            const pm_node_t *an = cn->arguments->arguments.nodes[i];
            if (PM_NODE_TYPE_P(an, PM_SPLAT_NODE) || PM_NODE_TYPE_P(an, PM_FORWARDING_ARGUMENTS_NODE) ||
                PM_NODE_TYPE_P(an, PM_KEYWORD_HASH_NODE) || PM_NODE_TYPE_P(an, PM_BLOCK_ARGUMENT_NODE)) { plain = false; break; }
        }
        if (plain) {
            uint32_t cnt = 1u + (uint32_t)argc + 1u;     /* [recv, args..., binding] */
            int32_t self_off = -1 - tc->chain - (int32_t)cnt - KORB_FRAME_HDR;
            NODE **argv = malloc(sizeof(NODE *) * cnt);
            if (!argv) abort();
            int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;
            argv[0] = transduce(tc, cn->receiver);
            for (size_t i = 0; i < argc; i++)
                argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
            argv[cnt - 1] = kp_make_binding_node(tc, line);
            tc->chain = saved;
            NODE *call = KP_LINK(tc, node_send, mid, kp_flink(tc, line), self_off, argv, cnt);
            bake_add(tc, &call->u.node_send.self_off);
            return call;
        }
    }

    /* unified send (any fixed arity): stage [recv, arg0..] into a parse-time
     * NODE* array; node_send / node_send_safe stage them into consecutive slots
     * at eval.  Safe navigation (recv&.m) nil-checks recv before dispatch. */
    {
        bool safe = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;
        /* `recv&.m(args)`: node_send_safe stages its children before it can look
         * at the receiver, so with ARGUMENTS the nil check has to come first —
         * CRuby never evaluates them for a nil receiver (bundler's
         * `@sorted&.insert(@sorted.bsearch_index { … })` is exactly this shape).
         * Evaluate the receiver into a temp and guard the whole call on it, the
         * way the block form (SAFE_WRAP) already does. */
        int32_t safe_tmp = -1;
        NODE *safe_set = NULL;
        if (safe && argc > 0) {
            safe_tmp = (int32_t)alloc_synth_local(tc);
            safe_set = bake_lset(tc, (uint32_t)safe_tmp, transduce(tc, cn->receiver));
            safe = false;                                  /* the guard replaces node_send_safe */
        }
        uint32_t cnt = 1u + (uint32_t)argc;
        int32_t self_off = -1 - tc->chain - (int32_t)cnt - KORB_FRAME_HDR;   /* caller self at base[-1], from the +HDR-shifted cursor */
        NODE **argv = malloc(sizeof(NODE *) * cnt);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr: cursor +HDR (node_send/safe reserve a frame header) */
        argv[0] = safe_tmp >= 0 ? bake_lget(tc, (uint32_t)safe_tmp) : transduce(tc, cn->receiver);
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        #define KP_SAFE_GUARD(call) (safe_tmp >= 0 \
            ? ALLOC_node_seq(safe_set, ALLOC_node_nil_guard(bake_lget(tc, (uint32_t)safe_tmp), (call))) : (call))
        if (safe) { NODE *sf = KP_LINK(tc, node_send_safe, mid, kp_flink(tc, line), self_off, argv, cnt);
                    bake_add(tc, &sf->u.node_send_safe.self_off);   /* recv&.m still honours private/protected */
                    return sf; }
        /* trailing `**h` / `k: v, **h` bundle → drop an empty kwargs Hash at call time */
        if (argc >= 1 && PM_NODE_TYPE_P(cn->arguments->arguments.nodes[argc - 1], PM_KEYWORD_HASH_NODE)) {
            NODE *ckws = KP_LINK(tc, node_send_kws, mid, kp_flink(tc, line), self_off, argv, cnt);
            bake_add(tc, &ckws->u.node_send_kws.self_off);
            return KP_SAFE_GUARD(ckws);
        }
        NODE *call = KP_LINK(tc, node_send, mid, kp_flink(tc, line), self_off, argv, cnt);
        bake_add(tc, &call->u.node_send.self_off);    /* fixed up by the caller frame_size (base[-1] = self) */
        return KP_SAFE_GUARD(call);
        #undef KP_SAFE_GUARD
    }
}

/* ---- def ----------------------------------------------------------------- */


/* prism lists a def's locals in source order, so a local first created inside a
 * default expression (`def m(a = b = 1, c = 2)`) lands *between* the parameters
 * and breaks the ABI's "parameters occupy locals[0..params_cnt)" layout.  Build
 * a reordered private copy in that case: every named parameter first, in ABI
 * order, then the rest of the locals as prism had them.  Returns NULL when the
 * natural order already works or when the shape is one this cannot describe
 * (destructured or repeated parameters).
 *
 * `def m(*, a)` has no local for the anonymous rest, yet the ABI derives the
 * post slots from rest_slot+1, so reserve an unnamed slot (id 0, which no name
 * can match) between the optionals and the posts. */
static const pm_constant_id_list_t *
kp_param_first_locals(struct kp_ctx *tc, const pm_constant_id_list_t *locals,
                      const pm_parameters_node_t *ps)
{
    if (!ps || locals->size == 0) return NULL;
    pm_constant_id_t want[64];
    uint32_t n = 0, ph = 0;                 /* ph = reserved unnamed slots in want[] */
    #define WANT(cid) do { if ((cid) == 0 || n >= 64) return NULL; want[n++] = (cid); } while (0)
    for (size_t i = 0; i < ps->requireds.size; i++) {
        if (!PM_NODE_TYPE_P(ps->requireds.nodes[i], PM_REQUIRED_PARAMETER_NODE)) return NULL;
        WANT(((const pm_required_parameter_node_t *)ps->requireds.nodes[i])->name);
    }
    for (size_t i = 0; i < ps->optionals.size; i++) {
        if (!PM_NODE_TYPE_P(ps->optionals.nodes[i], PM_OPTIONAL_PARAMETER_NODE)) return NULL;
        WANT(((const pm_optional_parameter_node_t *)ps->optionals.nodes[i])->name);
    }
    if (ps->rest) {
        if (!PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE)) return NULL;
        const pm_constant_id_t rn = ((const pm_rest_parameter_node_t *)ps->rest)->name;
        if (rn == 0) {                                  /* anonymous rest: reserve a slot only when posts need one */
            if (ps->posts.size) { if (n >= 64) return NULL; want[n++] = 0; ph++; }
        }
        else WANT(rn);
    }
    for (size_t i = 0; i < ps->posts.size; i++) {
        if (!PM_NODE_TYPE_P(ps->posts.nodes[i], PM_REQUIRED_PARAMETER_NODE)) return NULL;
        WANT(((const pm_required_parameter_node_t *)ps->posts.nodes[i])->name);
    }
    #undef WANT
    if (n == 0) return NULL;
    const uint32_t out_size = (uint32_t)locals->size + ph;
    if (ph == 0) {
        bool same = (n <= locals->size);
        for (uint32_t i = 0; same && i < n; i++) if (locals->ids[i] != want[i]) same = false;
        if (same) return NULL;                          /* prism's order already matches */
    }
    for (uint32_t i = 0; i < n; i++) {                  /* a repeated name has no unique slot */
        if (want[i] == 0) continue;
        for (uint32_t j = i + 1; j < n; j++) if (want[i] == want[j]) return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {                  /* every parameter must BE one of the locals */
        if (want[i] == 0) continue;
        bool found = false;
        for (size_t k = 0; !found && k < locals->size; k++) found = (locals->ids[k] == want[i]);
        if (!found) return NULL;
    }
    pm_constant_id_t *const ids = malloc(sizeof(*ids) * out_size);   /* immortal: the frame reads it */
    if (!ids) abort();
    memcpy(ids, want, sizeof(*ids) * n);
    uint32_t w = n;
    for (size_t k = 0; k < locals->size; k++) {
        bool is_param = false;
        for (uint32_t i = 0; !is_param && i < n; i++) is_param = (want[i] == locals->ids[k]);
        if (!is_param) ids[w++] = locals->ids[k];
    }
    if (w != out_size) { free(ids); return NULL; }       /* duplicate local ids: leave it alone */
    pm_constant_id_list_t *const out = malloc(sizeof(*out));
    if (!out) abort();
    out->ids = ids; out->size = out_size; out->capacity = out_size;
    (void)tc;
    return out;
}

static NODE *
transduce_def_recv(struct kp_ctx *tc, const pm_def_node_t *dn, const pm_node_t *recv_override)
{
    /* `def recv.name` — singleton method.  Evaluate the receiver in the enclosing
     * scope (staged as the node's child, like node_class's super); attach to its
     * singleton class.  recv_override (from `class << recv`) wins over dn->receiver. */
    NODE *recv_node = NULL;
    const pm_node_t *recv = recv_override ? recv_override : dn->receiver;
    /* `module_function` mode: an instance def also becomes a singleton method on
     * self (the module).  recv_node = self for the singleton copy. */
    bool mod_func = !recv && tc->frame->module_function_mode;
    if (recv)           WITH_CHAIN(tc, 1, (recv_node = transduce(tc, recv)));
    else if (mod_func)  WITH_CHAIN(tc, 1, (recv_node = bake_self(tc)));

    uint32_t params_cnt = 0, req_cnt = 0, opt_cnt = 0;
    const pm_parameters_node_t *ps = dn->parameters;
    pm_constant_id_t blk_param_name = 0;   /* &blk param name (0 = none) */
    bool anon_blk_param = false;           /* `def m(&)` — slot allocated after push_frame */
    if (ps) {
        if (kp_no_block_param(ps->block)) { /* `&nil`: no local, no forwarding — korb_method.no_block carries it */ }
        else if (ps->block) {
            if (!ps->block->name) anon_blk_param = true;
            else                  blk_param_name = ps->block->name;
        }
        /* posts without a rest = required params after optionals, e.g. `def m(a=1, b)`:
         * supported — they bind from the tail, optionals fill the middle. */
        if (ps->rest && !PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE))
            return kp_unsupported(tc, (const pm_node_t *)dn, "forwarding rest parameter");
        req_cnt = (uint32_t)ps->requireds.size;
        opt_cnt = (uint32_t)ps->optionals.size;
        params_cnt = req_cnt + opt_cnt;   /* positional fixed slots; rest/keywords follow */
    }

    const bool anon_cref = tc->frame->anon_class_body || tc->frame->anon_cref_method;
    /* This def sits in the BODY of a `class << X` method: self there is the
     * receiver, so the definee must come from the method's cref.  A def in the
     * `class << X` body itself already has that class as self (and must keep the
     * body's private/public scope), so it is not this shape. */
    const uint32_t sclass_cref = tc->frame->anon_cref_method ? 1u : 0u;
    const pm_constant_id_list_t *const reordered = kp_param_first_locals(tc, &dn->locals, ps);
    /* `def m(*, a)`: the posts sit at rest_slot+1, so the anonymous rest needs the
     * slot kp_param_first_locals reserves — without that list there is nowhere to put it. */
    const bool anon_rest_posts = ps && ps->rest && PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE)
                                 && ((const pm_rest_parameter_node_t *)ps->rest)->name == 0 && ps->posts.size;
    if (anon_rest_posts && !reordered)
        return kp_unsupported(tc, (const pm_node_t *)dn, "anonymous rest parameter with post parameters");
    push_frame(tc, reordered ? reordered : &dn->locals);
    tc->frame->anon_cref_method = anon_cref;               /* constants resolve via the entry cell */
    tc->frame->method_mid = kp_intern_cid(tc, dn->name);   /* for `super` inside the body */
    tc->frame->method_params = params_cnt;

    /* prism orders def locals with the parameters first (required, then optional);
     * the staged-args window doubles as the parameter slots.  Verify the layout. */
    struct Node **opt_defaults = NULL;
    bool has_destructured = false;         /* a `(a, b)` parameter shifts the later params' locals */
    if (dn->parameters) {
        for (uint32_t i = 0; i < req_cnt; i++) {
            const pm_node_t *p = dn->parameters->requireds.nodes[i];
            if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) { has_destructured = true; continue; }   /* `def m(a, (b, c))` — destructured below */
            if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) {
                pop_frame(tc);
                return kp_unsupported(tc, p, "non-plain required parameter");
            }
            pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
            if (lvar_index(tc, p, cid) != i && !has_destructured) {   /* name resolves to another slot: a repeated param */
                /* Ruby permits repeated `_`-prefixed params (`def m(_, _)`); each occupies
                 * its own positional slot (frame has them), name reads hit the first. */
                if (kp_cid_cstr(tc, cid)[0] != '_') { pop_frame(tc); return kp_unsupported(tc, p, "repeated parameter name"); }
            }
        }
        if (opt_cnt) {
            opt_defaults = malloc(sizeof(struct Node *) * opt_cnt);
            if (!opt_defaults) abort();
            for (uint32_t j = 0; j < opt_cnt; j++) {
                const pm_optional_parameter_node_t *op =
                    (const pm_optional_parameter_node_t *)dn->parameters->optionals.nodes[j];
                if (lvar_index(tc, (const pm_node_t *)op, op->name) != req_cnt + j
                    && !has_destructured                        /* a `(a, b)` param shifts every later local */
                    && kp_cid_cstr(tc, op->name)[0] != '_') {   /* non-`_` repeat: unsupported (see required-param note) */
                    pop_frame(tc); free(opt_defaults);
                    return kp_unsupported(tc, (const pm_node_t *)op, "repeated parameter name");
                }
                /* default expr runs in method scope at the body cursor (chain 0) */
                opt_defaults[j] = transduce(tc, op->value);
            }
        }
    }

    /* *rest param: collects surplus positionals; its local slot follows req+opt. */
    int32_t rest_slot = -1;
    if (ps && ps->rest) {
        pm_constant_id_t rn = ((const pm_rest_parameter_node_t *)ps->rest)->name;
        rest_slot = rn                ? (int32_t)lvar_index(tc, ps->rest, rn)   /* named rest → its local slot */
                  : anon_rest_posts   ? (int32_t)params_cnt                     /* the reserved unnamed slot, so posts land at +1 */
                                      : (int32_t)alloc_synth_local(tc);         /* anonymous `*` → synth slot (bare `*` forwards it) */
        if (!rn) tc->frame->anon_rest_slot = rest_slot;
        tc->frame->method_rest_slot = rest_slot;                 /* for forwarding super (`super` re-splats the rest) */
    }
    /* `def m(...)` — prism gives no locals, so collect ALL positional args into a
     * synth rest local; `inner(...)` in the body splats it.  Keywords ride along
     * as the trailing hash (re-bound by name at the callee).  A second synth
     * local receives the caller's block, materialized by node_blkparam below. */
    if (ps && ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_FORWARDING_PARAMETER_NODE)) {
        rest_slot = (int32_t)alloc_synth_local(tc);
        tc->frame->fwd_slot = rest_slot;
        tc->frame->fwd_blk_slot = (int32_t)alloc_synth_local(tc);
        /* Raise the capture floor HERE, not where a `...` is first read: the
         * blocks in this body share ONE materialized env, and the first one to
         * close it fixes its size.  A block written before the one that
         * forwards would otherwise seal a shorter env and the forwarding read
         * would run past its end. */
        tc->frame->cap_min = (uint32_t)tc->frame->fwd_blk_slot + 1u;
    }
    /* anonymous `&` → a synth local the body can't name; a bare `&` at a call
     * site inside the body forwards it. */
    if (anon_blk_param) tc->frame->anon_blk_slot = (int32_t)alloc_synth_local(tc);

    /* post params (after *rest): plain requireds occupying the slots right after
     * the rest slot, bound from the tail of the positional args. */
    uint32_t post_cnt = ps ? (uint32_t)ps->posts.size : 0;
    /* posts follow the rest slot when there is one, else they follow the
     * optionals (locals[params_cnt..]) — required-after-optional. */
    const uint32_t post_base = (rest_slot >= 0) ? (uint32_t)rest_slot + 1 : params_cnt;
    if (post_cnt) { tc->frame->method_post_base = (int32_t)post_base; tc->frame->method_post_cnt = post_cnt; }  /* for forwarding super */
    bool post_destructured = false;    /* a `(b, c)` post shifts every later post's local */
    for (uint32_t i = 0; i < post_cnt; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) { post_destructured = true; continue; }   /* destructured below */
        if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, p, "non-plain post parameter"); }
        pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
        if (lvar_index(tc, p, cid) != post_base + i && !post_destructured
            && kp_cid_cstr(tc, cid)[0] != '_') {   /* non-`_` repeat: unsupported (see required-param note) */
            pop_frame(tc); free(opt_defaults);
            return kp_unsupported(tc, p, "repeated parameter name");
        }
    }

    /* keyword params (required `k:` / optional `k: default`) + keyword-rest `**kw`,
     * occupying locals after the positional params; bound by name in korb_invoke_method. */
    struct korb_kw_info *kw_info = NULL;
    if (ps && (ps->keywords.size ||
               (ps->keyword_rest && !PM_NODE_TYPE_P(ps->keyword_rest, PM_FORWARDING_PARAMETER_NODE)))) {
        kw_info = malloc(sizeof(*kw_info));
        if (!kw_info) abort();
        kw_info->count = (uint32_t)ps->keywords.size;
        kw_info->kwrest_slot = -1;
        kw_info->entries = ps->keywords.size ? malloc(sizeof(struct korb_kw_entry) * ps->keywords.size) : NULL;
        for (uint32_t j = 0; j < kw_info->count; j++) {
            const pm_node_t *kp = ps->keywords.nodes[j];
            pm_constant_id_t name; NODE *deflt = NULL;
            if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                name = ((const pm_required_keyword_parameter_node_t *)kp)->name;
            } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                const pm_optional_keyword_parameter_node_t *ok = (const pm_optional_keyword_parameter_node_t *)kp;
                name = ok->name;
                deflt = transduce(tc, ok->value);    /* default runs at body cursor */
            } else { pop_frame(tc); return kp_unsupported(tc, kp, "keyword parameter form"); }
            kw_info->entries[j].mid  = kp_intern_cid(tc, name);
            kw_info->entries[j].slot = lvar_index(tc, kp, name);
            kw_info->entries[j].deflt = deflt;
        }
        if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {
            pm_constant_id_t kr = ((const pm_keyword_rest_parameter_node_t *)ps->keyword_rest)->name;
            if (kr) kw_info->kwrest_slot = (int32_t)lvar_index(tc, ps->keyword_rest, kr);
            else {                               /* anonymous `**` : collect into a synth local so bare `**` can forward it */
                kw_info->kwrest_slot = (int32_t)alloc_synth_local(tc);
                tc->frame->anon_kwrest_slot = kw_info->kwrest_slot;
            }
        } else if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_NO_KEYWORDS_PARAMETER_NODE))
            kw_info->kwrest_slot = -3;           /* `**nil` : no keywords accepted (positional Hash stays positional) */
    }
    tc->frame->method_kw_info = kw_info;   /* for forwarding super (`super` re-passes the keyword args) */

    NODE *body;
    if (dn->body == NULL) {
        body = lit_nil();
    }
    else if (PM_NODE_TYPE_P(dn->body, PM_STATEMENTS_NODE)) {
        body = transduce_statements(tc, (const pm_statements_node_t *)dn->body);
    }
    else {
        /* def-body rescue/ensure — the body is an (implicit) begin node, handled
         * by the PM_BEGIN_NODE transducer (node_begin). */
        body = transduce(tc, dn->body);
    }

    /* `def m(a, (b, c), d)` — a destructuring parameter takes one positional slot
     * but its names occupy several locals, so every later parameter's local sits
     * one or more slots past where the caller staged its argument.  At body entry,
     * move each plain parameter down to its own local (rightmost first, so a
     * destructure never clobbers an argument still to be moved), then unpack the
     * destructuring ones through the multi-assign machinery (#to_ary, nesting and
     * nil padding included).  Statements are prepended, so building them
     * left-to-right yields right-to-left execution. */
    if (has_destructured) {
        for (uint32_t i = 0; i < req_cnt; i++) {
            const pm_node_t *p = dn->parameters->requireds.nodes[i];
            if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) {
                const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)p;
                NODE *const un = massign_general(tc, &mt->lefts, mt->rest, &mt->rights, bake_lget(tc, i));
                if (!un) { pop_frame(tc); return kp_unsupported(tc, p, "destructuring parameter"); }
                body = ALLOC_node_seq(un, body);
            } else {
                const uint32_t L = lvar_index(tc, p, ((const pm_required_parameter_node_t *)p)->name);
                if (L != i) body = ALLOC_node_seq(bake_lset(tc, L, bake_lget(tc, i)), body);
            }
        }
        /* An optional parameter is filled at its POSITIONAL slot (req_cnt + j) by
         * the invoke path, so it needs the same move.  Emitted after the required
         * ones: prepending reverses, and these sit further right. */
        for (uint32_t j = 0; j < opt_cnt; j++) {
            const pm_optional_parameter_node_t *const op =
                (const pm_optional_parameter_node_t *)dn->parameters->optionals.nodes[j];
            const uint32_t L = lvar_index(tc, (const pm_node_t *)op, op->name);
            if (L != req_cnt + j) body = ALLOC_node_seq(bake_lset(tc, L, bake_lget(tc, req_cnt + j)), body);
        }
    }
    /* Same shift on the post side: `def m(a=1, (b, c), d)` stages d one slot below
     * its local, because (b, c) spends two.  Prepending left-to-right runs them
     * rightmost-first, so a destructure never clobbers an argument still to move. */
    for (uint32_t i = 0; i < post_cnt; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) {
            const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)p;
            NODE *const un = massign_general(tc, &mt->lefts, mt->rest, &mt->rights, bake_lget(tc, post_base + i));
            if (!un) { pop_frame(tc); return kp_unsupported(tc, p, "destructuring parameter"); }
            body = ALLOC_node_seq(un, body);
        } else if (post_destructured) {
            const uint32_t L = lvar_index(tc, p, ((const pm_required_parameter_node_t *)p)->name);
            if (L != post_base + i) body = ALLOC_node_seq(bake_lset(tc, L, bake_lget(tc, post_base + i)), body);
        }
    }

    if (blk_param_name) {                  /* `&blk` → materialize the block into the local at body entry */
        tc->frame->uses_block = true;      /* reserve the frame's block cells */
        int32_t dst = (int32_t)lvar_index(tc, (const pm_node_t *)ps->block, blk_param_name) - tc->chain;
        NODE *bp = ALLOC_node_blkparam(dst, -3 - tc->chain, -2 - tc->chain, -1 - tc->chain);
        bake_add(tc, &bp->u.node_blkparam.dst_off);
        body = ALLOC_node_seq(bp, body);
    } else if (tc->frame->fwd_blk_slot >= 0 || tc->frame->anon_blk_slot >= 0) {
        /* `def m(...)` / `def m(&)` → same, into the synth forwarding slot */
        tc->frame->uses_block = true;
        int32_t slot = tc->frame->fwd_blk_slot >= 0 ? tc->frame->fwd_blk_slot : tc->frame->anon_blk_slot;
        NODE *bp = ALLOC_node_blkparam(slot - tc->chain, -3 - tc->chain, -2 - tc->chain, -1 - tc->chain);
        bake_add(tc, &bp->u.node_blkparam.dst_off);
        body = ALLOC_node_seq(bp, body);
    }
    uint32_t uses_block = (tc->frame->uses_block ? KORB_DEFF_USES_BLOCK : 0u) |
                          (tc->frame->uses_backref ? KORB_DEFF_USES_BACKREF : 0u) |
                          (tc->frame->fwd_slot >= 0 ? KORB_DEFF_FWD_ALL : 0u);
    void *const linfo = build_locals_info(tc);   /* before pop_frame: it frees the frame */
    uint32_t frame_size = pop_frame(tc);   /* = locals + 2 if the method yields */

    uint32_t mid = kp_intern_cid(tc, dn->name);
    /* Method#source_location: remember the (file, line) of this def's body. */
    korb_reg_srcloc(tc->c->vm, body, korb_intern(tc->c->vm, tc->fname, strlen(tc->fname)), kp_line(tc, (const pm_node_t *)dn));
    kp_reg_srcrange(tc, body, (const pm_node_t *)dn);   /* Method#source_range: `def` .. `end` */
    /* full param list (names + kinds) for Method#parameters — cold-read only, like the block node_entry's. */
    void *pinfo = build_param_info(tc, dn->parameters ? (const pm_node_t *)dn->parameters : NULL);
    /* Simple methods (fixed positional arity, nothing exotic) get a
     * node_simple_entry: the frame setup moves onto the callee so params_cnt /
     * locals_cnt fold into that entry's SD.  Non-simple shapes keep the old
     * caller-side binding (korb_invoke_method), so they get no entry. */
    /* uses_block でも作る: ブロックを渡さないサイトからは、yield を持つだけの
     * メソッドも固定 arity なら同じ entry で呼べる (locals_cnt はブロック用の
     * 3 セルを含んでおり、ゼロ埋めで nil になる = yield は LocalJumpError)。
     * ブロック付きの呼び出しは別ノード (node_call_blk) を通るので無関係。 */
    NODE *entry = NULL;
    if (kw_info == NULL && rest_slot < 0 && post_cnt == 0 && req_cnt == params_cnt)
        entry = ALLOC_node_simple_entry(body, params_cnt, frame_size,
                                        (uses_block & KORB_DEFF_USES_BACKREF) ? 1u : 0u);

    /* self at the def site (enclosing frame) = the default definee.  A `def`
     * inside a block has no method entry of its own to name the cref, so bake
     * the lexically enclosing class/module path for that case (0 = none). */
    const uint32_t def_cref_name = (tc->frame->anon_class_body || tc->frame->anon_cref_method)
                                 ? 0u : kp_cref_owner_path(tc);
    NODE *def;
    if (mod_func) {
        /* module_function: define as instance method AND as a singleton on self. */
        NODE *idef = ALLOC_node_def(mid, body, entry, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, linfo, -1 - tc->chain, -4 - tc->chain, sclass_cref, def_cref_name);
        bake_add(tc, &idef->u.node_def.self_off);          /* definee = self at base[-1] */
        bake_add(tc, &idef->u.node_def.dc_off);            /* enclosing method entry = base[-4] */
        NODE *sdef = ALLOC_node_singleton_def(mid, body, entry, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, linfo, recv_node);
        def = ALLOC_node_seq(idef, sdef);
    } else if (recv_node) {
        def = ALLOC_node_singleton_def(mid, body, entry, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, linfo, recv_node);
    } else {
        def = ALLOC_node_def(mid, body, entry, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, linfo, -1 - tc->chain, -4 - tc->chain, sclass_cref, def_cref_name);
        bake_add(tc, &def->u.node_def.self_off);           /* definee = self at base[-1] */
        bake_add(tc, &def->u.node_def.dc_off);             /* enclosing method entry = base[-4] */
    }

    /* Every method body is its own AOT entry: call sites reach it through
     * body->head.dispatcher at runtime (specializer can't fold that). */
    code_repo_add(korb_sym_name(tc->c->vm, mid), body, true);
    /* the simple entry is its own AOT root (call sites dispatch it via
     * ic->dispatch).  The body stays registered too: refinements can demote
     * is_simple at run time, and korb_invoke_method then dispatches the body
     * directly — it must still have an SD. */
    if (entry) code_repo_add(korb_sym_name(tc->c->vm, mid), entry, true);
    return def;
}

/* `class A::B` / `module A::B` at a scope with no enclosing class body: the
 * nesting entry is the WHOLE path (CRuby's Module.nesting is [A::B]), and the
 * cref chain has to carry it — baking only `B` lets any same-named `B` in the
 * flat table answer, so `Zeitwerk::Loader::Config`'s constants landed in
 * `Dry::Container::Config`.  Inside an enclosing class body the path's head is
 * itself resolved lexically, which the chain walk cannot express: keep the bare
 * name there. */
static uint32_t
kp_join_path(struct kp_ctx *tc, uint32_t head, uint32_t tail)
{
    if (head == 0) return tail;
    char buf[512];
    const int n = snprintf(buf, sizeof buf, "%s::%s", korb_sym_name(tc->c->vm, head),
                           korb_sym_name(tc->c->vm, tail));
    if (n <= 0 || n >= (int)sizeof buf) return tail;
    return korb_intern(tc->c->vm, buf, (uint32_t)n);
}
static uint32_t
kp_nesting_sym(struct kp_ctx *tc, uint32_t path_owner, uint32_t name_sym)
{
    if (path_owner == 0) return name_sym;
    for (const struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0 || f->anon_class_body) return name_sym;
    return kp_join_path(tc, path_owner, name_sym);
}
/* `module Ruby::Patterns` inside `module CodeRay::Scanners` resolves its `Ruby`
 * LEXICALLY, so bake the enclosing nesting in front of it; korb_const_get_path
 * drops leading segments until the path resolves, which is that same walk. */
static uint32_t
kp_qualify_owner(struct kp_ctx *tc, uint32_t path_owner)
{
    if (path_owner == 0) return 0;
    uint32_t encl = 0;
    (void)kp_lexical_class_owner(tc, &encl);
    return encl ? kp_join_path(tc, encl, path_owner) : path_owner;
}

/* `class Name ... end` → node_class carrying the class name + a node_entry for
 * the body (its own scope, run with self = the class). */
static NODE *
transduce_class(struct kp_ctx *tc, const pm_class_node_t *cn)
{
    /* `class A::B` — koruby's const table is flat, so the value is still stored
     * under the rightmost name (cn->name); but the path's parent (A) is baked as
     * the enclosing namespace so A::B.name / A.constants work. */
    if (!PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_READ_NODE) &&
        !PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_PATH_NODE))
        return kp_unsupported(tc, (const pm_node_t *)cn, "dynamic class name");
    uint32_t name_sym = kp_intern_cid(tc, cn->name);
    uint32_t path_owner = 0;                         /* `class M::C` → M (full dotted path) */
    const pm_node_t *dyn_base = NULL;                /* `class expr::C` → evaluate expr */
    bool abs_top = false;                            /* `class ::C` — top level, whatever encloses it */
    if (PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_PATH_NODE)) {
        const pm_node_t *const parent = ((const pm_constant_path_node_t *)cn->constant_path)->parent;
        abs_top = (parent == NULL);
        path_owner = kp_intern_cpath(tc, parent);
        if (path_owner == 0 && !abs_top) dyn_base = parent;
    }
    const uint32_t path_kind = dyn_base ? 1u : (path_owner ? 2u : 0u);   /* 0 none / 1 dynamic base / 2 explicit A::B */
    const uint32_t nest_sym = kp_nesting_sym(tc, path_owner, name_sym);   /* before the lexical-owner fallback below */
    if (!abs_top) path_owner = kp_qualify_owner(tc, path_owner);

    /* a bare `class X` inside a block nests lexically, not by self */
    bool lex_top = abs_top;
    if (!abs_top && path_owner == 0 && dyn_base == NULL) {
        uint32_t lex = 0;
        if (kp_lexical_class_owner(tc, &lex)) { path_owner = lex; lex_top = (lex == 0); }
    }
    /* path base + superclass expression (both evaluated in the ENCLOSING scope)
     * → node_class's staged children; nil when absent. */
    NODE *base_node, *super_node;
    WITH_CHAIN(tc, 2, (base_node  = dyn_base ? transduce(tc, dyn_base) : ALLOC_node_lit(KORB_NIL),
                       super_node = cn->superclass ? transduce(tc, cn->superclass)
                                                   : ALLOC_node_lit(KORB_NIL)));

    push_frame(tc, &cn->locals);
    tc->frame->class_name_sym = nest_sym;       /* for Module.nesting (the whole path for `class A::B`) */
    const uint32_t blabel = tc->frame->body_label = kp_body_label(tc, 'c', name_sym);   /* backtrace label; blocks inside read it */
    NODE *body;
    if (cn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(cn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)cn->body);
    else
        body = transduce(tc, cn->body);   /* a begin/rescue/ensure body is just another node */
    void *const linfo = build_locals_info(tc);   /* before pop_frame: it frees the frame */
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, 0, -1, NULL, 0, NULL, NULL, linfo, -1, 0, -1, 0, blabel, 0, 0, 0);
    code_repo_add("class", entry, true);          /* its own AOT entry */
    /* the BODY's own srcloc: a backtrace has to recognise this node as one of
     * ours to label the class-body frame `<class:Foo>` (korb_bt_known_ptr) */
    korb_reg_srcloc(tc->c->vm, entry, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)cn));
    NODE *_ncls = KP_LINK(tc, node_class, name_sym, kp_flink_at(tc, kp_line(tc, (const pm_node_t *)cn), 2), entry, lex_top ? INT32_MIN : -1 - tc->chain - 2, path_owner, path_kind, base_node, super_node);   /* self_off = enclosing self (base[-1]); -2 for the staged base+super children */
    korb_reg_srcloc(tc->c->vm, _ncls, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)cn));   /* Module#const_source_location */
    if (!lex_top) bake_add(tc, &_ncls->u.node_class.self_off);
    return _ncls;
}

extern const struct NodeKind kind_node_const_set;
static NODE *assign_target_from_synth(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local);
static NODE *assign_target_from_synth_r(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local, int32_t recv_local);

/* General multi-assign desugar: `lefts..., *rest, rights... = rhs`.  Runs the
 * same node_massign_splat the all-local fast path uses, but into synth locals,
 * then plumbs each synth out to its real target — so every target kind (local at
 * any depth / @ivar / CONST / $g / recv.x= / recv[k]= / nested group) inherits
 * the node's #to_ary coercion and CRuby's nil padding.  `rest` may be NULL or an
 * implicit rest (no splat: the extras are collected into a throwaway synth and
 * dropped), or a splat whose expression is NULL (anonymous `*`).  The result
 * value is the original rhs, as a multi-assign expression yields.  Returns NULL
 * if some target is unsupported.  `rhs` must be built at the current chain. */
static void kp_hoist_reset(struct kp_ctx *tc) {
    for (uint32_t i = 0; i < tc->hoist_cnt; i++) free(tc->hoists[i].args);
    tc->hoist_cnt = 0;
    tc->hoisting = false;
}
static struct kp_hoist *kp_hoist_find(struct kp_ctx *tc, const pm_node_t *t) {
    for (uint32_t i = 0; i < tc->hoist_cnt; i++) if (tc->hoists[i].t == t) return &tc->hoists[i];
    return NULL;
}
static struct kp_hoist *kp_hoist_add(struct kp_ctx *tc, const pm_node_t *t) {
    if (tc->hoist_cnt == tc->hoist_capa) {
        tc->hoist_capa = tc->hoist_capa ? tc->hoist_capa * 2 : 8;
        tc->hoists = realloc(tc->hoists, sizeof(*tc->hoists) * tc->hoist_capa);
        if (!tc->hoists) abort();
    }
    struct kp_hoist *h = &tc->hoists[tc->hoist_cnt++];
    h->t = t; h->recv = -1; h->args = NULL; h->nargs = 0;
    return h;
}
/* Evaluate `e` into a fresh synth local now, appending to *prologue. */
static int32_t kp_hoist_expr(struct kp_ctx *tc, const pm_node_t *e, NODE **prologue) {
    const uint32_t sl = alloc_synth_local(tc);
    NODE *const st = bake_lset(tc, sl, transduce(tc, e));
    *prologue = *prologue ? ALLOC_node_seq(*prologue, st) : st;
    return (int32_t)sl;
}
static void kp_hoist_targets(struct kp_ctx *tc, const pm_node_list_t *lefts, const pm_node_t *rest,
                             const pm_node_list_t *rights, NODE **prologue);
/* Hoist one target's receiver / index arguments (recursing into nested groups). */
static void kp_hoist_one(struct kp_ctx *tc, const pm_node_t *t, NODE **prologue) {
    if (!t) return;
    /* Evaluate first, add second: kp_hoist_expr can transduce a nested
     * multi-assign, whose kp_hoist_add reallocs the table out from under us. */
    if (PM_NODE_TYPE_P(t, PM_CALL_TARGET_NODE)) {
        const int32_t r = kp_hoist_expr(tc, ((const pm_call_target_node_t *)t)->receiver, prologue);
        kp_hoist_add(tc, t)->recv = r;
        return;
    }
    if (PM_NODE_TYPE_P(t, PM_CONSTANT_PATH_TARGET_NODE)) {
        const pm_constant_path_target_node_t *cpt = (const pm_constant_path_target_node_t *)t;
        if (!cpt->parent) return;
        const int32_t r = kp_hoist_expr(tc, cpt->parent, prologue);
        kp_hoist_add(tc, t)->recv = r;
        return;
    }
    if (PM_NODE_TYPE_P(t, PM_INDEX_TARGET_NODE)) {
        const pm_index_target_node_t *it = (const pm_index_target_node_t *)t;
        if (it->block) return;
        struct pm_node **const ia = it->arguments ? it->arguments->arguments.nodes : NULL;
        const uint32_t na = it->arguments ? (uint32_t)it->arguments->arguments.size : 0u;
        for (uint32_t i = 0; i < na; i++) if (PM_NODE_TYPE_P(ia[i], PM_SPLAT_NODE)) return;   /* splat: built as one Array later */
        const int32_t r = kp_hoist_expr(tc, it->receiver, prologue);
        int32_t *const av = na ? malloc(sizeof(int32_t) * na) : NULL;
        if (na && !av) abort();
        for (uint32_t i = 0; i < na; i++) av[i] = kp_hoist_expr(tc, ia[i], prologue);
        struct kp_hoist *const h = kp_hoist_add(tc, t);   /* add AFTER the allocs: realloc invalidates pointers */
        h->recv = r; h->args = av; h->nargs = na;
        return;
    }
    if (PM_NODE_TYPE_P(t, PM_MULTI_TARGET_NODE)) {
        const pm_multi_target_node_t *const mt = (const pm_multi_target_node_t *)t;
        kp_hoist_targets(tc, &mt->lefts, mt->rest, &mt->rights, prologue);
    }
}
static void kp_hoist_targets(struct kp_ctx *tc, const pm_node_list_t *lefts, const pm_node_t *rest,
                             const pm_node_list_t *rights, NODE **prologue) {
    for (size_t i = 0; i < lefts->size; i++) kp_hoist_one(tc, lefts->nodes[i], prologue);
    if (rest && PM_NODE_TYPE_P(rest, PM_SPLAT_NODE))
        kp_hoist_one(tc, ((const pm_splat_node_t *)rest)->expression, prologue);
    if (rights) for (size_t i = 0; i < rights->size; i++) kp_hoist_one(tc, rights->nodes[i], prologue);
}

static NODE *
massign_general(struct kp_ctx *tc, const pm_node_list_t *lefts, const pm_node_t *rest,
                const pm_node_list_t *rights, NODE *rhs)
{
    const bool has_splat = rest && PM_NODE_TYPE_P(rest, PM_SPLAT_NODE);
    if (rest && !has_splat && !PM_NODE_TYPE_P(rest, PM_IMPLICIT_REST_NODE)) return NULL;
    /* posts only ever accompany a rest; without one the extras are just dropped */
    const uint32_t npre  = (uint32_t)lefts->size;
    const uint32_t npost = has_splat && rights ? (uint32_t)rights->size : 0u;
    const uint32_t no    = npre + 1u + npost;
    /* the splat slot is always allocated: with no splat target it is the
     * throwaway that node_massign_splat collects the surplus elements into */
    const pm_node_t *const splat_t = has_splat ? ((const pm_splat_node_t *)rest)->expression : NULL;

    uint32_t *const synth = malloc(sizeof(uint32_t) * no);
    int32_t  *const offs  = malloc(sizeof(int32_t) * no);
    int32_t  *const rcv   = malloc(sizeof(int32_t) * no);
    if (!synth || !offs || !rcv) abort();
    for (uint32_t k = 0; k < no; k++) { synth[k] = alloc_synth_local(tc); offs[k] = (int32_t)synth[k] - tc->chain; }
    /* CRuby evaluates every left-hand-side RECEIVER (and index argument), left
     * to right and through nested groups, BEFORE the right-hand side; only then
     * does it assign.  The outermost call hoists the whole target tree into
     * synth locals up front; the recursive calls reuse that table. */
    NODE *prologue = NULL;
    const bool outermost = !tc->hoisting;
    if (outermost) {
        tc->hoisting = true;
        kp_hoist_targets(tc, lefts, rest, rights, &prologue);
    }
    for (uint32_t k = 0; k < no; k++) rcv[k] = -1;
    NODE *massign = ALLOC_node_massign_splat(offs, npre, npost, rhs);
    for (uint32_t k = 0; k < no; k++) bake_add(tc, &offs[k]);
    const uint32_t rettmp = alloc_synth_local(tc);          /* hold the massign value across the plumbing */
    NODE *acc = bake_lset(tc, rettmp, massign);
    if (prologue) acc = ALLOC_node_seq(prologue, acc);
    for (uint32_t i = 0; i < no; i++) {
        const pm_node_t *const t = i < npre  ? lefts->nodes[i]
                                 : i == npre ? splat_t      /* NULL for `*` / no splat → nothing to plumb */
                                             : rights->nodes[i - npre - 1u];
        if (!t) continue;
        NODE *const a = assign_target_from_synth_r(tc, t, synth[i], rcv[i]);
        if (!a) { free(synth); free(rcv); if (outermost) kp_hoist_reset(tc); return NULL; }
        acc = ALLOC_node_seq(acc, a);
    }
    free(synth); free(rcv);                                /* offs stays: the node owns it */
    if (outermost) kp_hoist_reset(tc);
    return ALLOC_node_seq(acc, bake_lget(tc, rettmp));
}

/* `[elems..., lget(src_local)]` — the argument Array for a splatted `[]=`
 * target (`a[*idx] = v`), i.e. the index args with the assigned value appended. */
static NODE *
build_args_plus_value(struct kp_ctx *tc, struct pm_node **elems, uint32_t n, uint32_t src_local)
{
    NODE *acc, *elem;
    WITH_CHAIN(tc, kind_node_ary_push.slot_count, (acc  = build_array(tc, elems, n, n + 1u),
                                                   elem = bake_lget(tc, src_local)));
    return ALLOC_node_ary_push(acc, elem);
}

/* Assign the value held in synth local `src_local` to multi-assign target `t`
 * (local / @ivar / CONST / $global / recv.setter= / recv[k]= / nested group),
 * reading the source at each target's own staging chain — this is how
 * massign_general plumbs its synth temps out to the real targets.  Returns NULL
 * if unsupported. */
static NODE *
assign_target_from_synth_r(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local, int32_t recv_local)
{
    uint32_t line = kp_line(tc, t);
    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
        const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)t;
        return lvar_write(tc, t, lt->name, lt->depth, bake_lget(tc, src_local));
    }
    if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE))     /* a name inside a destructuring parameter */
        return lvar_write(tc, t, ((const pm_required_parameter_node_t *)t)->name, 0, bake_lget(tc, src_local));
    if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE))
        return bake_ivar_set(tc, kp_intern_cid(tc, ((const pm_instance_variable_target_node_t *)t)->name),
                                  bake_lget(tc, src_local));
    if (PM_NODE_TYPE_P(t, PM_CONSTANT_TARGET_NODE)) {
        uint32_t name = kp_intern_cid(tc, ((const pm_constant_target_node_t *)t)->name);
        NODE *v; WITH_CHAIN(tc, kind_node_const_set.slot_count, (v = bake_lget(tc, src_local)));
        return build_const_set(tc, name, v);
    }
    if (PM_NODE_TYPE_P(t, PM_CONSTANT_PATH_TARGET_NODE)) {   /* `a, M::X = ...` → M.const_set(:X, v) */
        const pm_constant_path_target_node_t *cpt = (const pm_constant_path_target_node_t *)t;
        if (cpt->parent == NULL) return NULL;                /* `::X` — no runtime namespace to send to */
        const struct kp_hoist *const hp = kp_hoist_find(tc, t);
        NODE *r, *k, *v;
        WITH_CHAIN(tc, KP_SEND2_SC, (r = (hp && hp->recv >= 0) ? bake_lget(tc, (uint32_t)hp->recv)
                                                               : transduce(tc, cpt->parent),
                                     k = ALLOC_node_lit(ID2SYM(kp_intern_cid(tc, cpt->name))),
                                     v = bake_lget(tc, src_local)));
        return kp_send2(tc, korb_intern(tc->c->vm, "const_set", 9), line, r, k, v);
    }
    if (PM_NODE_TYPE_P(t, PM_CLASS_VARIABLE_TARGET_NODE)) {
        return bake_cvar_set(tc, kp_intern_cid(tc, ((const pm_class_variable_target_node_t *)t)->name),
                             bake_lget(tc, src_local));
    }
    if (PM_NODE_TYPE_P(t, PM_GLOBAL_VARIABLE_TARGET_NODE)) {
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, ((const pm_global_variable_target_node_t *)t)->name));
        NODE *v; WITH_CHAIN(tc, kind_node_const_set.slot_count, (v = bake_lget(tc, src_local)));
        return build_const_set(tc, name, v);
    }
    if (PM_NODE_TYPE_P(t, PM_CALL_TARGET_NODE)) {
        const pm_call_target_node_t *ct = (const pm_call_target_node_t *)t;
        const struct kp_hoist *const hc = kp_hoist_find(tc, t);
        if (hc && hc->recv >= 0) recv_local = hc->recv;
        NODE *r, *v;
        WITH_CHAIN(tc, KP_SEND1_SC, (r = (recv_local >= 0) ? bake_lget(tc, (uint32_t)recv_local)   /* pre-evaluated (multi-assign order) */
                                                           : transduce(tc, ct->receiver),
                                     v = bake_lget(tc, src_local)));
        return kp_send1(tc, kp_intern_cid(tc, ct->name), line, r, v);
    }
    if (PM_NODE_TYPE_P(t, PM_INDEX_TARGET_NODE)) {     /* recv[k...] = v — any index arity, `*splat` included */
        const pm_index_target_node_t *it = (const pm_index_target_node_t *)t;
        if (it->block) return NULL;
        const uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
        struct pm_node **const ia = it->arguments ? it->arguments->arguments.nodes : NULL;
        const uint32_t na = it->arguments ? (uint32_t)it->arguments->arguments.size : 0u;
        bool has_splat = false;
        for (uint32_t i = 0; i < na; i++) if (PM_NODE_TYPE_P(ia[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {                               /* build [indices..., value] and dispatch dynamically */
            NODE *r, *arr;
            WITH_CHAIN(tc, 2, (r   = transduce(tc, it->receiver),
                               arr = build_args_plus_value(tc, ia, na, src_local)));
            return KP_LINK(tc, node_send_splat_aset, aset, kp_flink_at(tc, line, 2), r, arr);
        }
        const struct kp_hoist *const hi = kp_hoist_find(tc, t);
        NODE **const av = malloc(sizeof(NODE *) * (na + 1u));
        if (!av) abort();
        const int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)KP_SENDN_SC(na + 1u);
        NODE *const r = (hi && hi->recv >= 0) ? bake_lget(tc, (uint32_t)hi->recv) : transduce(tc, it->receiver);
        for (uint32_t i = 0; i < na; i++)
            av[i] = (hi && hi->nargs == na) ? bake_lget(tc, (uint32_t)hi->args[i]) : transduce(tc, ia[i]);
        av[na] = bake_lget(tc, src_local);
        tc->chain = saved;
        NODE *const s = kp_send_n(tc, aset, line, r, av, na + 1u);
        free(av);
        return s;
    }
    if (PM_NODE_TYPE_P(t, PM_MULTI_TARGET_NODE))       /* nested `(a, b)` → the synth already holds its source */
    {
        const pm_multi_target_node_t *const mt = (const pm_multi_target_node_t *)t;
        return massign_general(tc, &mt->lefts, mt->rest, &mt->rights, bake_lget(tc, src_local));
    }
    return NULL;
}
static NODE *
assign_target_from_synth(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local)
{
    return assign_target_from_synth_r(tc, t, src_local, -1);
}

/* `recv[*args] op= v` — the index arguments include a splat, so they are built
 * ONCE into an Array and both sends go through the dynamic (splat) form.  The
 * value is appended to that same Array for the `[]=` call, which keeps the
 * single-evaluation guarantee (`#to_a` must run only once). */
static NODE *
index_opassign_splat(struct kp_ctx *tc, const pm_index_operator_write_node_t *iw, const pm_node_t *node,
                     const pm_arguments_node_t *args, const pm_node_t *value)
{
    /* `iw` is only read for the op= fields; ||=/&&= pass their own args/value
     * because those prism structs have a different layout. */
    const uint32_t argc = (uint32_t)args->arguments.size;
    const uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
    const uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
    /* `||=` / `&&=` reuse this shape but combine with or/and instead of an
     * operator (binary_operator is 0 for those prism nodes). */
    const int logic = PM_NODE_TYPE_P(node, PM_INDEX_OR_WRITE_NODE) ? 1
                    : PM_NODE_TYPE_P(node, PM_INDEX_AND_WRITE_NODE) ? 2 : 0;
    const enum kp_binop op = logic ? KP_BINOP_NONE : kp_binop_kind(kp_cid_cstr(tc, iw->binary_operator));
    const uint32_t opmid = logic ? 0 : kp_intern_cid(tc, iw->binary_operator);
    const uint32_t line = kp_line(tc, node);
    const uint32_t t0 = alloc_synth_local(tc);           /* receiver */
    const uint32_t ta = alloc_synth_local(tc);           /* the index Array */
    const uint32_t tn = alloc_synth_local(tc);           /* the computed value */
    NODE *stores = bake_lset(tc, t0, transduce(tc, iw->receiver));   /* receiver is at the same offset in all three */
    NODE *const arr = build_array(tc, args->arguments.nodes, argc, argc);
    stores = ALLOC_node_seq(stores, bake_lset(tc, ta, arr));
    NODE *newval;
    const uint32_t bsc = logic ? 0u                                 /* node_or/and build their children in place */
                       : (op != KP_BINOP_NONE) ? kind_node_plus.slot_count : KP_SEND1_SC;
    WITH_CHAIN(tc, bsc, ({
        NODE *g_recv, *g_arr, *get, *val;
        { const int32_t _s = tc->chain; tc->chain = _s + 2;
          g_recv = bake_lget(tc, t0); g_arr = bake_lget(tc, ta);
          tc->chain = _s; }
        get = KP_LINK(tc, node_send_splat, aref, kp_flink_at(tc, line, 2), INT32_MIN, g_recv, g_arr);
        val = transduce(tc, value);
        newval = logic ? (logic == 1 ? ALLOC_node_or(get, val) : ALLOC_node_and(get, val))
               : (op != KP_BINOP_NONE) ? alloc_binop(tc, op, get, val, line)
               : kp_send1(tc, opmid, line, get, val);
        newval;
    }));
    NODE *seq = ALLOC_node_seq(stores, bake_lset(tc, tn, newval));
    NODE *s_recv, *s_arr;
    { const int32_t _s = tc->chain; tc->chain = _s + 2;
      s_recv = bake_lget(tc, t0);
      NODE *acc, *elem;
      WITH_CHAIN(tc, kind_node_ary_push.slot_count, (acc = bake_lget(tc, ta), elem = bake_lget(tc, tn)));
      s_arr = ALLOC_node_ary_push(acc, elem);            /* [indices..., value] */
      tc->chain = _s; }
    NODE *const store = KP_LINK(tc, node_send_splat, aset, kp_flink_at(tc, line, 2), INT32_MIN, s_recv, s_arr);
    if (logic) {
        /* ||= assigns only when the read was falsy, &&= only when it was truthy —
         * re-test the computed value against the original read so a no-op stays
         * a no-op (`h[*k] &&= v` must not create the key). */
        NODE *guard;
        { const int32_t _s = tc->chain; tc->chain = _s + 2;
          NODE *g2_recv = bake_lget(tc, t0), *g2_arr = bake_lget(tc, ta);
          tc->chain = _s;
          guard = KP_LINK(tc, node_send_splat, aref, kp_flink_at(tc, line, 2), INT32_MIN, g2_recv, g2_arr); }
        seq = ALLOC_node_seq(seq, (logic == 1) ? ALLOC_node_or(guard, store)
                                               : ALLOC_node_and(guard, store));
        return ALLOC_node_seq(seq, bake_lget(tc, tn));
    }
    seq = ALLOC_node_seq(seq, store);
    return ALLOC_node_seq(seq, bake_lget(tc, tn));       /* the expression yields the assigned value */
}

/* Emit a `super`/`super(args)` that forwards the current method's incoming block.
 * `arr` (built at chain+1, staged as the node's array child) supplies the args;
 * the block trio is read from the method frame (self_off is bottom-header →
 * baked; the entry cell + trio are top cells → chain-relative, not baked). */
/* Build {k0: lget(slot0), ...}(.merge(**kwrest)) as a Hash NODE — the keyword
 * arguments a bare `super` forwards (each kw param's current value, incl.
 * defaults, then the kwrest merged).  Inside-out like build_hash. */
/* Read the enclosing METHOD's local `idx` from `depth` scopes out (0 = we are in
 * the method frame itself).  Bare `super` inside a block forwards the method's
 * parameters, which live that many env links away. */
static NODE *bake_lget_at(struct kp_ctx *tc, uint32_t depth, int32_t idx) {
    return depth == 0 ? bake_lget(tc, (uint32_t)idx) : bake_eget(tc, depth, (uint32_t)idx);
}

static NODE *
build_fwd_kwargs(struct kp_ctx *tc, struct korb_kw_info *kw, uint32_t n, uint32_t depth)
{
    if (n == 0) {                                        /* base: {} (+ **kwrest merged on top) */
        NODE *h = ALLOC_node_hash_new(kw->count);
        if (kw->kwrest_slot >= 0) {
            NODE *src; const uint32_t sc = kind_node_hash_merge.slot_count;
            WITH_CHAIN(tc, sc, (src = bake_lget_at(tc, depth, kw->kwrest_slot)));
            h = ALLOC_node_hash_merge(h, src);
        }
        return h;
    }
    const uint32_t sc = kind_node_hash_set.slot_count;
    NODE *acc, *key, *val;
    WITH_CHAIN(tc, sc, (acc = build_fwd_kwargs(tc, kw, n - 1, depth),
                        key = ALLOC_node_lit(ID2SYM(kw->entries[n - 1].mid)),
                        val = bake_lget_at(tc, depth, kw->entries[n - 1].slot)));
    return ALLOC_node_hash_set(acc, key, val);
}

/* Build [pos0..pos_{np-1}, *rest, post0..post_{pc-1}(, {kwargs})] as an Array NODE — the
 * argument list a bare `super` forwards (positional params, the current rest
 * array splatted, then post params).  Inside-out like build_array; `total` =
 * np + (rest?1:0) + pc, indexing the segments in that order. */
static NODE *
build_fwd_args(struct kp_ctx *tc, uint32_t np, int32_t rest_slot, uint32_t post_base, uint32_t pc,
               struct korb_kw_info *kw, uint32_t total, uint32_t depth)
{
    if (total == 0) return ALLOC_node_array_new(0);
    const uint32_t bi = total - 1;                        /* build index of the last element */
    const uint32_t has_rest = (rest_slot >= 0) ? 1u : 0u;
    const bool has_kw = (kw && (kw->count || kw->kwrest_slot >= 0));
    const uint32_t sc = kind_node_ary_push.slot_count;
    if (has_kw && bi == np + has_rest + pc) {             /* trailing kwargs hash element */
        NODE *acc, *kwh;
        WITH_CHAIN(tc, sc,                             /* _kw shares node_ary_push's layout */
                   (acc = build_fwd_args(tc, np, rest_slot, post_base, pc, kw, total - 1, depth),
                    kwh = build_fwd_kwargs(tc, kw, kw->count, depth)));
        /* _kw: an EMPTY forwarded kwargs Hash is dropped, as at any other call
         * site — `def m(*, **); super; end` must reach a 0-arity parent. */
        return ALLOC_node_ary_push_kw(acc, ALLOC_node_kwargs_mark(kwh));
    }
    bool is_rest = false; int32_t slot;
    if (bi < np)                          slot = (int32_t)bi;                          /* positional */
    else if (has_rest && bi == np)      { slot = rest_slot; is_rest = true; }          /* *rest (splat) */
    else                                  slot = (int32_t)(post_base + (bi - np - has_rest));  /* post */
    NODE *acc, *elem;
    WITH_CHAIN(tc, sc, (acc  = build_fwd_args(tc, np, rest_slot, post_base, pc, kw, total - 1, depth),
                        elem = bake_lget_at(tc, depth, slot)));
    return is_rest ? ALLOC_node_ary_concat(acc, elem) : ALLOC_node_ary_push(acc, elem);
}

static NODE *
emit_super_fwd(struct kp_ctx *tc, uint32_t m_mid, uint32_t line, NODE *arr)
{
    /* The block `super` forwards is the enclosing METHOD's, which is `depth`
     * scopes out when the super is written inside a block
     * (`lock.synchronize { super }` — concurrent-ruby's map backends). */
    struct kp_frame *mf = tc->frame;
    uint32_t depth = 0;
    while (mf->method_mid == 0 && !mf->dm_body && mf->prev) { mf = mf->prev; depth++; }
    if (mf->method_mid == 0 && !mf->dm_body) { mf = tc->frame; depth = 0; }
    mf->uses_block = true;                                /* reserve the block trio */
    int32_t soff = -1 - tc->chain - 1, dco = -4 - tc->chain - 1;
    if (depth > 0) {                                      /* read the method frame's trio through the env chain */
        if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
        if (depth > tc->frame->yield_depth)   tc->frame->yield_depth = depth;
        NODE *_so = ALLOC_node_super_fwd_outer(m_mid, line, soff, dco, -2 - tc->chain - 1, depth, -3, arr);
        bake_add(tc, &_so->u.node_super_fwd_outer.self_off);
        bake_add(tc, &_so->u.node_super_fwd_outer.dc_off);
        bake_add(tc, &_so->u.node_super_fwd_outer.prev_off);
        add_bake_to(mf, &_so->u.node_super_fwd_outer.trio_base);   /* += method frame_size */
        return _so;
    }
    int32_t bo = -3 - tc->chain - 1, deo = -2 - tc->chain - 1, cso = -1 - tc->chain - 1;
    NODE *_s = ALLOC_node_super_fwd(m_mid, line, soff, dco, bo, deo, cso, arr);
    bake_add(tc, &_s->u.node_super_fwd.self_off);
    bake_add(tc, &_s->u.node_super_fwd.dc_off);   /* the method entry is base[-4] */
    return _s;
}

/* `module Name ... end` → node_module (own scope, run with self = the module). */
static NODE *
transduce_module(struct kp_ctx *tc, const pm_module_node_t *mn)
{
    if (!PM_NODE_TYPE_P(mn->constant_path, PM_CONSTANT_READ_NODE) &&
        !PM_NODE_TYPE_P(mn->constant_path, PM_CONSTANT_PATH_NODE))
        return kp_unsupported(tc, (const pm_node_t *)mn, "dynamic module name");
    uint32_t name_sym = kp_intern_cid(tc, mn->name);
    uint32_t path_owner = 0;                         /* `module M::Inner` → M (full dotted path) */
    const pm_node_t *dyn_base = NULL;                /* `module expr::Inner` → evaluate expr */
    bool abs_top = false;                            /* `module ::M` — top level, whatever encloses it */
    if (PM_NODE_TYPE_P(mn->constant_path, PM_CONSTANT_PATH_NODE)) {
        const pm_node_t *const parent = ((const pm_constant_path_node_t *)mn->constant_path)->parent;
        abs_top = (parent == NULL);
        path_owner = kp_intern_cpath(tc, parent);
        if (path_owner == 0 && !abs_top) dyn_base = parent;
    }
    const uint32_t path_kind = dyn_base ? 1u : (path_owner ? 2u : 0u);   /* 0 none / 1 dynamic base / 2 explicit A::B */
    const uint32_t nest_sym = kp_nesting_sym(tc, path_owner, name_sym);   /* before the lexical-owner fallback below */
    if (!abs_top) path_owner = kp_qualify_owner(tc, path_owner);
    bool lex_top = abs_top;                          /* see node_class: lexical nesting inside a block */
    if (!abs_top && path_owner == 0 && dyn_base == NULL) {
        uint32_t lex = 0;
        if (kp_lexical_class_owner(tc, &lex)) { path_owner = lex; lex_top = (lex == 0); }
    }
    /* the base expression is evaluated in the ENCLOSING scope → staged child */
    NODE *base_node;
    WITH_CHAIN(tc, 1, (base_node = dyn_base ? transduce(tc, dyn_base) : ALLOC_node_lit(KORB_NIL)));
    push_frame(tc, &mn->locals);
    tc->frame->class_name_sym = nest_sym;       /* for Module.nesting (the whole path for `class A::B`) */
    const uint32_t blabel = tc->frame->body_label = kp_body_label(tc, 'm', name_sym);
    NODE *body;
    if (mn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(mn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)mn->body);
    else
        body = transduce(tc, mn->body);   /* a begin/rescue/ensure body is just another node */
    void *const linfo = build_locals_info(tc);   /* before pop_frame: it frees the frame */
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, 0, -1, NULL, 0, NULL, NULL, linfo, -1, 0, -1, 0, blabel, 0, 0, 0);
    code_repo_add("module", entry, true);
    korb_reg_srcloc(tc->c->vm, entry, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)mn));   /* see transduce_class */
    NODE *_nmod = KP_LINK(tc, node_module, name_sym, kp_flink_at(tc, kp_line(tc, (const pm_node_t *)mn), 1), entry, lex_top ? INT32_MIN : -1 - tc->chain - 1, path_owner, path_kind, base_node);   /* self_off = enclosing self (base[-1]); -1 for the staged base child */
    korb_reg_srcloc(tc->c->vm, _nmod, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)mn));   /* Module#const_source_location */
    if (!lex_top) bake_add(tc, &_nmod->u.node_module.self_off);
    return _nmod;
}

/* Array literal `[e0, e1, ...]` → inside-out push chain (variadic @child is
 * unsupported).  Element i nests i pushes deep, so it transduces at the chain
 * depth matching its runtime cursor offset. */
static NODE *
build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_array_new(capa);
    const pm_node_t *last = elems[n - 1];
    bool splat = PM_NODE_TYPE_P(last, PM_SPLAT_NODE);
    const pm_node_t *expr = splat ? (const pm_node_t *)((const pm_splat_node_t *)last)->expression : last;
    if (splat && expr == NULL) {                        /* bare `*` — anonymous rest forward */
        uint32_t ard = 0; int32_t ars = -1;
        if (!kp_anon_find(tc, KP_ANON_REST, &ard, &ars)) return build_array(tc, elems, n - 1, capa);   /* no anon rest in scope → nothing */
        NODE *acc0, *elem0;
        uint32_t sc0 = kind_node_ary_push.slot_count;
        WITH_CHAIN(tc, sc0, (acc0  = build_array(tc, elems, n - 1, capa),
                             elem0 = kp_fwd_args(tc, ard, ars)));   /* splat the collected rest array */
        return ALLOC_node_ary_concat(acc0, elem0);
    }
    NODE *acc, *elem;
    uint32_t sc = kind_node_ary_push.slot_count;        /* concat shares the push layout */
    WITH_CHAIN(tc, sc, (acc  = build_array(tc, elems, n - 1, capa),
                        elem = transduce(tc, expr)));
    if (splat) return ALLOC_node_ary_concat(acc, elem);
    /* call-site trailing `**h` bundle (only ever the last arg): an empty kwargs
     * Hash is elided at run time (CRuby 3.0 empty-kwsplat rule) */
    if (PM_NODE_TYPE_P(last, PM_KEYWORD_HASH_NODE)) return ALLOC_node_ary_push_kw(acc, elem);
    return ALLOC_node_ary_push(acc, elem);
}

/* Leading positionals + forwarded rest: `f(a, b, ...)` → [a, b, *fwd_rest].
 * Same staging shape as build_array's splat member. */
static NODE *
build_array_with_fwd(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t depth, int32_t slot)
{
    NODE *acc, *fwd;
    uint32_t sc = kind_node_ary_push.slot_count;
    WITH_CHAIN(tc, sc, (acc = build_array(tc, elems, n, (uint32_t)n),
                        fwd = kp_fwd_args(tc, depth, slot)));
    return ALLOC_node_ary_concat(acc, fwd);
}

/* Call-site argument Array.  A lone `*x` becomes node_splat_view (no copy when
 * x already is an Array — the spreader reads it immediately); anything else is
 * build_array's push/concat chain.  Not for the &proc variants: they run
 * #to_proc (user code) before reading the array. */
static NODE *
build_call_args(struct kp_ctx *tc, struct pm_node **elems, size_t n)
{
    if (n == 1 && PM_NODE_TYPE_P(elems[0], PM_SPLAT_NODE)) {
        const pm_node_t *const expr = (const pm_node_t *)((const pm_splat_node_t *)elems[0])->expression;
        if (expr != NULL) return ALLOC_node_splat_view(transduce(tc, expr));
    }
    return build_array(tc, elems, n, (uint32_t)n);
}

/* Packed scope table for `binding` / caller-binding eval:
 *   [0]            L        — number of lexical levels captured (innermost first)
 *   [1 .. L]       ns[d]    — locals count of level d (frame slots 0..ns)
 *   then name_cnt triples   — (sym, depth, slot), innermost-first, shadowing
 *                             deduped (an inner name hides an outer one).
 * The lexical chain crosses block frames only: it stops at (and includes) the
 * enclosing method / class body / toplevel frame. */
static uint32_t *
kp_binding_scope_tbl(struct kp_ctx *tc, uint32_t *out_name_cnt)
{
    struct kp_frame *levels[64];
    uint32_t L = 0;
    for (struct kp_frame *f = tc->frame; f && L < 64; f = f->prev) {
        levels[L++] = f;
        if (f->prev == NULL && tc->eval_outer_cnt != 0) {   /* eval top: the binding's own levels follow */
            for (uint32_t v = 0; v < tc->eval_outer_cnt && L < 64; v++) levels[L++] = &tc->eval_outer[v];
            break;
        }
        if (f->method_mid || f->class_name_sym || f->anon_class_body) break;   /* scope barrier */
    }
    uint32_t total = 0;
    for (uint32_t d = 0; d < L; d++) {
        const pm_constant_id_list_t *ls = levels[d]->locals;
        total += (uint32_t)ls->size + kp_it_shift(levels[d]);
    }
    uint32_t *tbl = malloc(sizeof(uint32_t) * (1 + L + 3 * (total ? total : 1)));   /* immortal */
    if (!tbl) abort();
    tbl[0] = L;
    uint32_t cnt = 0;
    for (uint32_t d = 0; d < L; d++) {
        const pm_constant_id_list_t *ls = levels[d]->locals;
        const bool it_slot = kp_it_shift(levels[d]) != 0;
        tbl[1 + d] = (uint32_t)ls->size + (it_slot ? 1u : 0u);
        for (uint32_t i = 0; i < tbl[1 + d]; i++) {
            const pm_constant_id_t cid = (it_slot && i == 0) ? 0 : ls->ids[i - (it_slot ? 1u : 0u)];
            /* cid 0 with no `it` = the slot reserved for an anonymous `*`: it has
             * no name, so it gets a slot in the level but no table entry. */
            if (cid == 0 && !(it_slot && i == 0)) continue;
            const uint32_t sym = (it_slot && i == 0)
                ? korb_intern(tc->c->vm, "it", 2)
                : kp_intern_cid(tc, cid);
            bool shadowed = false;                       /* inner name wins */
            for (uint32_t k = 0; k < cnt; k++)
                if (tbl[1 + L + 3 * k] == sym) { shadowed = true; break; }
            if (shadowed) continue;
            tbl[1 + L + 3 * cnt]     = sym;
            /* `it` is reachable through #implicit_parameter_* but is not a local
             * variable, so the entry carries a flag the accessors filter on. */
            tbl[1 + L + 3 * cnt + 1] = d | ((it_slot && i == 0) ? KORB_BIND_IMPLICIT : 0u);
            tbl[1 + L + 3 * cnt + 2] = i;
            cnt++;
        }
    }
    *out_name_cnt = cnt;
    /* the binding walks L-1 env links at runtime: force full-depth capture so
     * enclosing procs materialize the whole chain (pop propagates upward) */
    if (L > 1 && tc->frame->max_ref_depth < L - 1) tc->frame->max_ref_depth = L - 1;
    return tbl;
}

static NODE *
kp_make_binding_node_x(struct kp_ctx *tc, uint32_t line, uint32_t user_dispatch)
{
    uint32_t cnt = 0;
    uint32_t *tbl = kp_binding_scope_tbl(tc, &cnt);
    const int32_t self_off = -1 - tc->chain;
    /* The lexically-enclosing class/module NAME, resolved at run time (the frame's
     * method-entry cell is not reachable from here: this node is often built
     * inside a call's argument-staging window, where a cursor-relative offset
     * does not land on the frame cursor).  0 = no enclosing class body. */
    /* An UNNAMED cref (`class << obj`, or a method defined in one) cannot be
     * baked by name; leave it 0 and let the runtime fall back to self, which IS
     * that class inside the body. */
    const uint32_t cref_name = (tc->frame->anon_class_body || tc->frame->anon_cref_method ||
                                tc->seed_cref_is_caller)
                             ? 0u : kp_cref_owner_path(tc);
    NODE *nb = ALLOC_node_binding(-tc->chain, self_off, (const char *)(const void *)tbl, cnt, cref_name, user_dispatch);
    bake_add(tc, &nb->u.node_binding.def_env_off);    /* frame base shifts with frame_size */
    bake_add(tc, &nb->u.node_binding.self_off);       /* self at base[-1] (bottom header) */
    korb_reg_srcloc(tc->c->vm, nb, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), line);  /* for Binding#source_location */
    return nb;
}
/* the hidden binding eval(str) / class_eval(String) is lowered with: never a
 * user's #binding */
static NODE *kp_make_binding_node(struct kp_ctx *tc, uint32_t line) { return kp_make_binding_node_x(tc, line, 0u); }

/* Value of `return` / `next` / `break` arguments: none → nil, one plain arg →
 * that value, splat or multiple args → an Array (CRuby wraps even a 1-element
 * splat: `return *[1]` → [1], `return *x, y` → [*x, y]). */
static NODE *
kp_jump_args_value(struct kp_ctx *tc, const pm_arguments_node_t *args)
{
    const size_t n = args ? args->arguments.size : 0;
    if (n == 0) return lit_nil();
    if (n == 1 && !PM_NODE_TYPE_P(args->arguments.nodes[0], PM_SPLAT_NODE))
        return transduce(tc, args->arguments.nodes[0]);
    return build_array(tc, args->arguments.nodes, n, (uint32_t)n);
}

/* A literal Hash key, for the duplicate-key warning.  Only keys whose value is
 * known at parse time take part; `kind` separates :a from "a" from 1. */
struct kp_hkey { const char *txt; size_t len; uint8_t kind; uint32_t line; };

static bool
kp_hkey_of(struct kp_ctx *tc, const pm_node_t *key, struct kp_hkey *out)
{
    out->line = kp_line(tc, key);
    if (PM_NODE_TYPE_P(key, PM_SYMBOL_NODE)) {
        const pm_string_t *const u = &((const pm_symbol_node_t *)key)->unescaped;
        out->txt = (const char *)pm_string_source(u); out->len = pm_string_length(u); out->kind = 0;
        return true;
    }
    if (PM_NODE_TYPE_P(key, PM_STRING_NODE)) {
        const pm_string_t *const u = &((const pm_string_node_t *)key)->unescaped;
        out->txt = (const char *)pm_string_source(u); out->len = pm_string_length(u); out->kind = 1;
        return true;
    }
    if (PM_NODE_TYPE_P(key, PM_INTEGER_NODE) || PM_NODE_TYPE_P(key, PM_FLOAT_NODE)) {
        out->txt = (const char *)key->location.start;
        out->len = (size_t)(key->location.end - key->location.start);
        out->kind = 2;
        return true;
    }
    return false;                                       /* dynamic / interpolated: no parse-time answer */
}

/* Collect the literal keys of a hash literal, descending into a `**{...}` whose
 * operand is itself a literal (CRuby warns across that boundary too). */
static void
kp_collect_hkeys(struct kp_ctx *tc, struct pm_node **assocs, size_t n,
                 struct kp_hkey *keys, uint32_t *cnt, uint32_t max)
{
    for (size_t i = 0; i < n && *cnt < max; i++) {
        const pm_node_t *const el = assocs[i];
        if (PM_NODE_TYPE_P(el, PM_ASSOC_SPLAT_NODE)) {
            const pm_node_t *const v = (const pm_node_t *)((const pm_assoc_splat_node_t *)el)->value;
            if (v && PM_NODE_TYPE_P(v, PM_HASH_NODE)) {
                const pm_hash_node_t *const inner = (const pm_hash_node_t *)v;
                kp_collect_hkeys(tc, inner->elements.nodes, inner->elements.size, keys, cnt, max);
            }
            continue;
        }
        if (!PM_NODE_TYPE_P(el, PM_ASSOC_NODE)) continue;
        if (kp_hkey_of(tc, (const pm_node_t *)((const pm_assoc_node_t *)el)->key, &keys[*cnt])) (*cnt)++;
    }
}

/* Wrap `h` so the first evaluation reports each duplicate literal key, or return
 * it unchanged when there is none. */
static NODE *
kp_warn_dup_hash_keys(struct kp_ctx *tc, struct pm_node **assocs, size_t n, NODE *h)
{
    if (n < 2 || n > 256) return h;
    struct kp_hkey keys[256];
    uint32_t cnt = 0;
    kp_collect_hkeys(tc, assocs, n, keys, &cnt, 256);
    struct korb_dupkey_warn *warns = NULL;
    uint32_t wcnt = 0;
    static const char *const open[] = { ":", "\"", "" }, *const close[] = { "", "\"", "" };
    for (uint32_t j = 1; j < cnt; j++) {                 /* one report per overwritten occurrence */
        for (uint32_t i = j; i-- > 0; ) {
            if (keys[i].kind != keys[j].kind || keys[i].len != keys[j].len ||
                memcmp(keys[i].txt, keys[j].txt, keys[i].len) != 0) continue;
            char *const key = malloc(keys[i].len + 3);
            if (!key) abort();
            snprintf(key, keys[i].len + 3, "%s%.*s%s", open[keys[i].kind],
                     (int)keys[i].len, keys[i].txt, close[keys[i].kind]);
            warns = realloc(warns, sizeof(*warns) * (wcnt + 1));
            if (!warns) abort();
            warns[wcnt++] = (struct korb_dupkey_warn){ strdup(tc->fname), key, keys[i].line, keys[j].line, 0 };
            break;
        }
    }
    if (wcnt == 0) return h;
    return ALLOC_node_dupkey_warn(warns, wcnt, h);
}

/* Hash literal `{k => v, ...}` → inside-out set chain (same shape as
 * build_array): hash_set(hash_set(hash_new(n), k0, v0), k1, v1)... */
static NODE *
build_hash(struct kp_ctx *tc, struct pm_node **assocs, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_hash_new(capa);
    const pm_node_t *last = assocs[n - 1];
    if (PM_NODE_TYPE_P(last, PM_ASSOC_SPLAT_NODE)) {       /* `**h` → merge */
        const pm_node_t *expr = (const pm_node_t *)((const pm_assoc_splat_node_t *)last)->value;
        if (expr == NULL) {                                 /* bare `**` — anonymous kwrest forward */
            uint32_t akd = 0; int32_t aks = -1;
            if (!kp_anon_find(tc, KP_ANON_KWREST, &akd, &aks)) return build_hash(tc, assocs, n - 1, capa);   /* no anon kwrest in scope → nothing */
            NODE *acc0, *src0;
            uint32_t sc0 = kind_node_hash_merge.slot_count;
            WITH_CHAIN(tc, sc0, (acc0 = build_hash(tc, assocs, n - 1, capa),
                                 src0 = kp_fwd_args(tc, akd, aks)));
            return ALLOC_node_hash_merge(acc0, src0);
        }
        NODE *acc, *src;
        uint32_t sc = kind_node_hash_merge.slot_count;
        WITH_CHAIN(tc, sc, (acc = build_hash(tc, assocs, n - 1, capa),
                            src = transduce(tc, expr)));
        return ALLOC_node_hash_merge(acc, src);
    }
    const pm_assoc_node_t *as = (const pm_assoc_node_t *)last;
    NODE *acc, *key, *val;
    uint32_t sc = kind_node_hash_set.slot_count;
    WITH_CHAIN(tc, sc, (acc = build_hash(tc, assocs, n - 1, capa),
                        key = transduce(tc, as->key),
                        val = transduce(tc, as->value)));
    return ALLOC_node_hash_set(acc, key, val);
}

/* The encoding a literal ends up in: prism marks a literal whose escapes force
 * it off the file's encoding (`\u` above U+7F → UTF-8; `\xNN` above 0x7F in a
 * US-ASCII source → binary).  The two flag bits are the same on every node kind. */
static uint8_t
kp_forced_enc(pm_node_flags_t flags, uint8_t src_enc)
{
    if (flags & PM_ENCODING_FLAGS_FORCED_UTF8_ENCODING)   return KORB_ENC_UTF8;
    if (flags & PM_ENCODING_FLAGS_FORCED_BINARY_ENCODING) return KORB_ENC_BINARY;
    return src_enc;
}

/* Same for a regexp literal, where the encoding rides on the modifier bits:
 * a forced binary source reads as /n, a forced UTF-8 one as /u. */
static uint32_t
kp_re_forced_flags(pm_node_flags_t flags)
{
    if (flags & PM_REGULAR_EXPRESSION_FLAGS_FORCED_BINARY_ENCODING) return PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT;
    if (flags & PM_REGULAR_EXPRESSION_FLAGS_FORCED_UTF8_ENCODING)   return PM_REGULAR_EXPRESSION_FLAGS_UTF_8;
    return 0;
}

/* `Kernel` / `::Kernel` written as a call receiver (Kernel.block_given? etc). */
static bool
kp_kernel_const_p(struct kp_ctx *tc, const pm_node_t *recv)
{
    pm_constant_id_t cid;
    if (PM_NODE_TYPE_P(recv, PM_CONSTANT_READ_NODE)) {
        cid = ((const pm_constant_read_node_t *)recv)->name;
    } else if (PM_NODE_TYPE_P(recv, PM_CONSTANT_PATH_NODE)) {
        const pm_constant_path_node_t *const cp = (const pm_constant_path_node_t *)recv;
        if (cp->parent != NULL) return false;                    /* only a top-level ::Kernel */
        cid = cp->name;
    } else {
        return false;
    }
    return strcmp(kp_cid_cstr(tc, cid), "Kernel") == 0;
}

/* Interpolated string `"a#{x}b"` → inside-out concat chain (same shape as
 * build_array).  The accumulator is always a String; each part is appended
 * via its to_s inside node_dstr_concat. */
static NODE *
build_dstr_enc(struct kp_ctx *tc, struct pm_node **parts, size_t n, uint8_t seed_enc)
{
    /* The empty seed carries the FILE's encoding, not the default: an
     * interpolated literal in a us-ascii source is us-ascii (its parts negotiate
     * upward from there), and seeding UTF-8 would drag every one to UTF-8.
     * A regexp source is the exception — there an /u /e /s /n modifier decides,
     * so its caller passes UTF-8 and lets the modifier apply. */
    if (n == 0) return seed_enc != KORB_ENC_UTF8 ? ALLOC_node_str_enc("", 0, seed_enc, 0)
                                                 : ALLOC_node_str("", 0);
    NODE *acc, *part;
    uint32_t sc = kind_node_dstr_concat.slot_count;
    WITH_CHAIN(tc, sc, (acc  = build_dstr_enc(tc, parts, n - 1, seed_enc),
                        part = transduce(tc, parts[n - 1])));
    return ALLOC_node_dstr_concat(acc, part);
}

static NODE *
build_dstr(struct kp_ctx *tc, struct pm_node **parts, size_t n)
{
    return build_dstr_enc(tc, parts, n, tc->src_enc);
}

/* Bake the lexically-enclosing class/module names (outermost→innermost) into a
 * constant node's cache, so the node resolves its cref by walking the chain
 * owner-scoped instead of picking the first same-named module in the table. */
static void
bake_cref_chain(struct kp_ctx *tc, struct korb_constcache *cache)
{
    uint32_t depth = tc->seed_chain_len;                        /* eval(String): what encloses the receiver */
    for (struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0) depth++;
    if (depth == 0) return;
    uint32_t *const chain = malloc(sizeof(uint32_t) * depth);   /* immortal (compile-time) */
    if (chain == NULL) return;
    uint32_t i = depth;                                         /* fill innermost→outermost; store outermost at [0] */
    for (struct kp_frame *f = tc->frame; f && i > tc->seed_chain_len; f = f->prev)
        if (f->class_name_sym != 0) chain[--i] = f->class_name_sym;
    for (uint32_t k = tc->seed_chain_len; k-- > 0; ) chain[--i] = tc->seed_chain[k];
    cache->owner_chain = chain;
    cache->chain_len = depth;
}

/* Build a bare constant read (`NAME`) node, resolving via the cref owner + the
 * baked lexical enclosing chain — same as the PM_CONSTANT_READ_NODE case.  Used
 * to synthesize the read side of `NAME op= v` op-assignments. */
static NODE *
build_const_read(struct kp_ctx *tc, uint32_t name_cid, uint32_t line)
{
    /* `class << obj` bodies have an unnamed cref: pass self so the read can look
     * in the singleton class itself. */
    /* An UNNAMED cref (a `class << obj` body, or a method defined in one) can only
     * be found at runtime: self IS the class in the body, and in a method the
     * frame's method-entry cell names it.  The entry cell is only meaningful in a
     * method frame, so it is baked only there. */
    const bool anon = tc->frame->anon_class_body || tc->frame->anon_cref_method;
    NODE *cn = KP_CONST(tc, name_cid, kp_cref_owner(tc), anon ? -1 - tc->chain : INT32_MIN,
                        tc->frame->anon_cref_method ? -4 - tc->chain : INT32_MIN, line);
    if (anon) bake_add(tc, &cn->u.node_const.self_off);              /* self at base[-1] */
    if (tc->frame->anon_cref_method) bake_add(tc, &cn->u.node_const.dc_off);   /* method entry at base[-4] */
    bake_cref_chain(tc, &cn->u.node_const.cache);
    return cn;
}

/* Build a bare constant write (`NAME = v`) node with the same baked chain: the
 * owner must be the lexical module, not whichever module of that name happens
 * to come first in the table. */
static NODE *
build_const_set(struct kp_ctx *tc, uint32_t name_cid, NODE *val)
{
    /* Inside a block the current frame names no class, so the constant used to
     * land on Object; the owner is the enclosing lexical class body.  Only the
     * block case is redirected — kp_lexical_class_owner returns false for a
     * `class << obj` body (self names that cref) and when no block intervenes. */
    const bool anon = tc->frame->anon_class_body;
    uint32_t owner = tc->frame->class_name_sym;
    if (owner == 0 && !anon) {
        uint32_t lex = 0;
        if (kp_lexical_class_owner(tc, &lex)) owner = lex;
    }
    NODE *cn = ALLOC_node_const_set(name_cid, owner,
                                    anon ? -1 - tc->chain : INT32_MIN, val);
    if (anon) bake_add(tc, &cn->u.node_const_set.self_off);   /* self at base[-1] */
    bake_cref_chain(tc, &cn->u.node_const_set.cache);
    return cn;
}
/* same, remembering where the assignment is written (Module#const_source_location) */
static NODE *
build_const_set_at(struct kp_ctx *tc, uint32_t name_cid, NODE *val, const pm_node_t *at)
{
    NODE *cn = build_const_set(tc, name_cid, val);
    korb_reg_srcloc(tc->c->vm, cn, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, at));
    return cn;
}

/* A constant path names its module statically only when EVERY step is a constant.
 * `A::B` does; `a::B` (a local holding a module) does not — the rightmost name
 * alone would resolve through the cref chain and find some other `B`. */
static bool
kp_const_path_static_p(const pm_node_t *n)
{
    while (PM_NODE_TYPE_P(n, PM_CONSTANT_PATH_NODE)) {
        const pm_node_t *const parent = ((const pm_constant_path_node_t *)n)->parent;
        if (parent == NULL) return true;                         /* `::A` */
        n = parent;
    }
    return PM_NODE_TYPE_P(n, PM_CONSTANT_READ_NODE);
}

/* Resolve the owner module name for a `PARENT::NAME` constant-path target with a
 * STATIC parent (a constant read or nested constant path).  Returns true and sets
 * *owner_name; returns false for a dynamic parent (`expr::NAME`, unsupported for
 * op-assign) — matching the owner resolution in PM_CONSTANT_PATH_WRITE_NODE. */
static bool
const_path_static_owner(struct kp_ctx *tc, const pm_node_t *parent, uint32_t frame_default, uint32_t *owner_name)
{
    if (parent == NULL) { *owner_name = frame_default; return true; }   /* `::X` — top-level frame default */
    if (PM_NODE_TYPE_P(parent, PM_CONSTANT_READ_NODE)) { *owner_name = kp_intern_cid(tc, ((const pm_constant_read_node_t *)parent)->name); return true; }
    if (PM_NODE_TYPE_P(parent, PM_CONSTANT_PATH_NODE) && kp_const_path_static_p(parent)) {
        const uint32_t full = kp_intern_cpath(tc, parent);   /* the whole dotted path (see PM_CONSTANT_PATH_WRITE_NODE) */
        *owner_name = full ? full : kp_intern_cid(tc, ((const pm_constant_path_node_t *)parent)->name);
        return true;
    }
    return false;   /* dynamic owner */
}

/* ---- main dispatch -------------------------------------------------------- */

static NODE *
transduce(struct kp_ctx *tc, const pm_node_t *node)
{
    switch (PM_NODE_TYPE(node)) {
      case PM_PROGRAM_NODE: {
        const pm_program_node_t *pn = (const pm_program_node_t *)node;
        /* expose the toplevel local-name table (for TOPLEVEL_BINDING). */
        koruby_toplevel_local_cnt = (uint32_t)pn->locals.size;
        uint32_t *tl_syms = koruby_toplevel_local_cnt ? malloc(sizeof(uint32_t) * koruby_toplevel_local_cnt) : NULL;
        for (uint32_t i = 0; i < koruby_toplevel_local_cnt; i++)
            tl_syms[i] = kp_intern_cid(tc, pn->locals.ids[i]);
        koruby_toplevel_local_syms = tl_syms;
        push_frame(tc, &pn->locals);
        tc->frame->class_name_sym = tc->seed_class_name;   /* eval(String) under a named cref */
        NODE *body = transduce_statements(tc, pn->statements);
        koruby_toplevel_locals_cnt = pop_frame(tc);   /* frame_size for main's cursor */
        return body;
      }

      case PM_STATEMENTS_NODE:
        return transduce_statements(tc, (const pm_statements_node_t *)node);

      case PM_PRE_EXECUTION_NODE: {     /* BEGIN { ... } */
        const pm_pre_execution_node_t *pe = (const pm_pre_execution_node_t *)node;
        const int32_t saved = tc->chain;
        tc->chain = 0;                  /* the body is emitted at program top level */
        NODE *body = pe->statements ? transduce_statements(tc, pe->statements) : lit_nil();
        tc->chain = saved;
        if (tc->pre_cnt == tc->pre_capa) {
            tc->pre_capa = tc->pre_capa ? tc->pre_capa * 2 : 4;
            tc->pre_list = realloc(tc->pre_list, sizeof(NODE *) * tc->pre_capa);
            if (!tc->pre_list) abort();
        }
        tc->pre_list[tc->pre_cnt++] = body;
        return lit_nil();
      }

      case PM_PARENTHESES_NODE: {
        const pm_parentheses_node_t *pn = (const pm_parentheses_node_t *)node;
        return transduce_opt(tc, pn->body);
      }

      case PM_BEGIN_NODE: {
        const pm_begin_node_t *bn = (const pm_begin_node_t *)node;
        uint32_t flags = 0;
        NODE *body = bn->statements ? transduce_statements(tc, bn->statements) : lit_nil();
        NODE *else_b = lit_nil();
        if (bn->else_clause)       /* else runs after a successful body; its value is the result */
            else_b = bn->else_clause->statements ? transduce_statements(tc, bn->else_clause->statements) : lit_nil();
        if (!bn->rescue_clause && !bn->ensure_clause)   /* plain begin[/else]/end */
            return bn->else_clause ? ALLOC_node_seq(body, else_b) : body;
        if (bn->else_clause) flags |= 4u;
        NODE *rescues = lit_nil();
        NODE *ensure_b = lit_nil();

        if (bn->rescue_clause) {
            flags |= 1u;
            rescues = build_rescue_chain(tc, bn->rescue_clause);
        }
        if (bn->ensure_clause) {
            const pm_ensure_node_t *en = bn->ensure_clause;
            /* node_begin runs the ensure body at slots+1 (one slot reserved to
             * root the in-flight value across the ensure's GC), so bake its
             * offsets at chain+1 to match. */
            ensure_b = WITH_CHAIN(tc, 1, en->statements ? transduce_statements(tc, en->statements) : lit_nil());
            flags |= 2u;
        }
        return ALLOC_node_begin(body, rescues, ensure_b, else_b, flags);
      }

      /* ---- literals ---- */
      case PM_INTEGER_NODE: {
        const pm_integer_node_t *in = (const pm_integer_node_t *)node;
        korb_sword_t v;
        if (!kp_integer_value(&in->value, &v)) {       /* beyond Fixnum → bake source digits, rebuild Bignum at eval */
            size_t slen = (size_t)(node->location.end - node->location.start);
            char *buf = malloc(slen + 1);
            if (!buf) abort();
            memcpy(buf, node->location.start, slen);
            buf[slen] = '\0';
            return ALLOC_node_bignum(buf, (uint32_t)slen);
        }
        return ALLOC_node_lit(LONG2FIX(v));
      }
      case PM_FLOAT_NODE:
        return ALLOC_node_float(((const pm_float_node_t *)node)->value);

      case PM_RATIONAL_NODE: {     /* `2r` / `1.5r` → Rational */
        const pm_rational_node_t *rn = (const pm_rational_node_t *)node;
        korb_sword_t num, den;
        if (!kp_integer_value(&rn->numerator, &num) || !kp_integer_value(&rn->denominator, &den)) {
            char *ns = kp_integer_to_decimal(&rn->numerator);     /* beyond Fixnum → bake digit strings */
            char *ds = kp_integer_to_decimal(&rn->denominator);
            return ALLOC_node_rational_big(ns, (uint32_t)strlen(ns), ds, (uint32_t)strlen(ds));
        }
        return ALLOC_node_rational((uint64_t)num, (uint64_t)den);
      }

      case PM_IMAGINARY_NODE:      /* `3i` / `1.5i` / `2ri` → Complex(0, numeric) */
        return ALLOC_node_imaginary(transduce(tc, ((const pm_imaginary_node_t *)node)->numeric));

      case PM_IMPLICIT_NODE:       /* hash shorthand `{x:, y:}` — value = the named local/call */
        return transduce(tc, ((const pm_implicit_node_t *)node)->value);

      case PM_STRING_NODE: {
        const pm_string_node_t *sn = (const pm_string_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&sn->unescaped, &len);
        /* An escape can force the literal away from the file's encoding: `\uNNNN`
         * above U+7F makes it UTF-8, and `\xNN` above 0x7F in a US-ASCII source
         * makes it binary (prism already decided which). */
        const uint8_t lenc = kp_forced_enc(sn->base.flags, tc->src_enc);
        /* # frozen_string_literal: true → frozen; no comment either way →
         * chilled (Ruby 3.4+); an explicit `false` opts out. */
        const uint32_t mode = (sn->base.flags & PM_STRING_FLAGS_FROZEN) ? 2u : (tc->fsl_false ? 0u : 1u);
        if (lenc != KORB_ENC_UTF8) return ALLOC_node_str_enc(bytes, len, lenc, mode);
        if (mode == 2) return ALLOC_node_str_frozen(bytes, len);
        if (mode == 1) return ALLOC_node_str_chilled(bytes, len);
        return ALLOC_node_str(bytes, len);
      }
      case PM_REGULAR_EXPRESSION_NODE: {   /* /pat/ → Regexp (matching via astrogre) */
        kp_note_backref(tc);
        const pm_regular_expression_node_t *rn = (const pm_regular_expression_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&rn->unescaped, &len);
        /* Pass prism regex flags straight through (astrogre reads the same bits:
         * IGNORE_CASE=4 / EXTENDED=8 / MULTI_LINE=16). */
        uint32_t flags = rn->base.flags & (PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE |
                                           PM_REGULAR_EXPRESSION_FLAGS_EXTENDED |
                                           PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE |
                                           PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT |
                                           PM_REGULAR_EXPRESSION_FLAGS_EUC_JP |
                                           PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J |
                                           PM_REGULAR_EXPRESSION_FLAGS_UTF_8);
        flags |= kp_re_forced_flags(rn->base.flags);
        /* CRuby rejects a bad pattern while COMPILING, so a literal the engine
         * refuses is a SyntaxError, not a first-evaluation RegexpError. */
        if (tc->syntax_err == NULL) {
            const char *const why = korb_re_literal_error(tc->c, bytes, len, flags);
            if (UNLIKELY(why != NULL)) {
                static char buf[512];
                const pm_node_t *const at = (const pm_node_t *)rn;
                snprintf(buf, sizeof buf, "%s:%u: %s: /%.*s/", tc->fname, kp_line(tc, at), why, (int)len, bytes);
                tc->syntax_err = buf;
                tc->syntax_error = true;
            }
        }
        return ALLOC_node_regexp(bytes, len, flags);
      }
      case PM_SYMBOL_NODE: {
        const pm_symbol_node_t *sn = (const pm_symbol_node_t *)node;
        size_t len = pm_string_length(&sn->unescaped);
        /* a symbol literal carries the file's encoding (CRuby); an ASCII-only
         * name still normalises to US-ASCII inside korb_intern_enc. */
        uint32_t id = korb_intern_enc(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), len,
                                      (sn->base.flags & PM_SYMBOL_FLAGS_FORCED_US_ASCII_ENCODING)
                                          ? KORB_ENC_USASCII : kp_forced_enc(sn->base.flags, tc->src_enc));
        return ALLOC_node_lit(ID2SYM(id));
      }
      case PM_ALIAS_GLOBAL_VARIABLE_NODE: {   /* alias $NEW $OLD — parse 時 alias (English.rb) */
        const pm_alias_global_variable_node_t *ag = (const pm_alias_global_variable_node_t *)node;
        uint32_t nw = UINT32_MAX, old = UINT32_MAX;
        if (PM_NODE_TYPE_P(ag->new_name, PM_GLOBAL_VARIABLE_READ_NODE))
            nw = kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)ag->new_name)->name);
        if (PM_NODE_TYPE_P(ag->old_name, PM_GLOBAL_VARIABLE_READ_NODE))
            old = kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)ag->old_name)->name);
        else if (PM_NODE_TYPE_P(ag->old_name, PM_BACK_REFERENCE_READ_NODE))
            old = kp_intern_cid(tc, ((const pm_back_reference_read_node_t *)ag->old_name)->name);
        if (nw == UINT32_MAX || old == UINT32_MAX)
            return kp_unsupported(tc, node, "alias of a numbered reference");
        kp_gvar_alias_add(nw, kp_gvar_resolve(old));
        return lit_nil();
      }
      case PM_ALIAS_METHOD_NODE: {   /* alias new old — copy a method on the enclosing class */
        const pm_alias_method_node_t *al = (const pm_alias_method_node_t *)node;
        uint32_t nm = UINT32_MAX, om = UINT32_MAX;
        if (PM_NODE_TYPE_P(al->new_name, PM_SYMBOL_NODE)) { const pm_symbol_node_t *s = (const pm_symbol_node_t *)al->new_name; nm = korb_intern(tc->c->vm, (const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped)); }
        if (PM_NODE_TYPE_P(al->old_name, PM_SYMBOL_NODE)) { const pm_symbol_node_t *s = (const pm_symbol_node_t *)al->old_name; om = korb_intern(tc->c->vm, (const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped)); }
        if (nm == UINT32_MAX || om == UINT32_MAX) {
            /* An interpolated name is only known at run time: `alias :"#{x}" y`
             * becomes self.alias_method(new, old) — inside a class body / a
             * class_eval block self IS the module, which is what `alias` targets. */
            const uint32_t line = kp_line(tc, node);
            NODE *selfn, *nn, *on;
            WITH_CHAIN(tc, KP_SEND2_SC, (selfn = bake_self(tc),
                                         nn = (nm != UINT32_MAX) ? ALLOC_node_lit(ID2SYM(nm)) : transduce(tc, al->new_name),
                                         on = (om != UINT32_MAX) ? ALLOC_node_lit(ID2SYM(om)) : transduce(tc, al->old_name)));
            return kp_send2(tc, korb_intern(tc->c->vm, "alias_method", 12), line, selfn, nn, on);
        }
        /* `alias my_eval eval`: remember the new name so its call sites get the
         * hidden caller-binding argument too (the lowering is by name). */
        if (om == korb_intern(tc->c->vm, "eval", 4) && tc->eval_alias_cnt < 8 &&
            PM_NODE_TYPE_P(al->new_name, PM_SYMBOL_NODE)) {
            const pm_symbol_node_t *sn = (const pm_symbol_node_t *)al->new_name;
            const pm_constant_id_t cid =
                pm_constant_pool_find(&tc->parser->constant_pool,
                                      pm_string_source(&sn->unescaped),
                                      pm_string_length(&sn->unescaped));
            if (cid != 0) tc->eval_aliases[tc->eval_alias_cnt++] = (uint32_t)cid;
        }
        NODE *na = ALLOC_node_alias(nm, om, -1 - tc->chain, kp_line(tc, node));   /* self (the class) at base[-1] */
        bake_add(tc, &na->u.node_alias.self_off);
        return na;
      }
      case PM_UNDEF_NODE: {          /* undef foo, bar — retire methods on the enclosing class */
        const pm_undef_node_t *un = (const pm_undef_node_t *)node;
        const uint32_t cnt = (uint32_t)un->names.size;
        uint32_t *mids = calloc(cnt ? cnt : 1, sizeof(uint32_t));   /* immortal (baked); calloc so the hash_cstr over these bytes is deterministic */
        if (!mids) abort();
        for (uint32_t i = 0; i < cnt; i++) {
            const pm_node_t *nm = un->names.nodes[i];
            if (!PM_NODE_TYPE_P(nm, PM_SYMBOL_NODE)) { free(mids); return kp_unsupported(tc, node, "undef with a dynamic-symbol name"); }
            const pm_symbol_node_t *s = (const pm_symbol_node_t *)nm;
            mids[i] = korb_intern(tc->c->vm, (const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped));
        }
        NODE *nu = ALLOC_node_undef((const char *)(const void *)mids, cnt, -1 - tc->chain);   /* self (the class) at base[-1] */
        bake_add(tc, &nu->u.node_undef.self_off);
        return nu;
      }
      case PM_NIL_NODE:   return ALLOC_node_lit(KORB_NIL);
      case PM_TRUE_NODE:  return ALLOC_node_lit(KORB_TRUE);
      case PM_FALSE_NODE: return ALLOC_node_lit(KORB_FALSE);
      case PM_SOURCE_FILE_NODE: return ALLOC_node_str(tc->fname, (uint32_t)strlen(tc->fname));   /* __FILE__ */
      case PM_POST_EXECUTION_NODE: {     /* END { } — an at_exit handler over the current scope */
        const pm_post_execution_node_t *const pe = (const pm_post_execution_node_t *)node;
        if (tc->frame->method_mid != 0)
            fprintf(stderr, "%s:%u: warning: END in method; use at_exit\n", tc->fname, kp_line(tc, node));
        /* END shares the enclosing scope (prism resolves its locals at depth 0),
         * so the block is built over that same locals table. */
        static const pm_constant_id_list_t no_locals = { 0 };
        NODE *entry = transduce_block_parts_shifted(tc, &no_locals, (const pm_node_t *)pe->statements);
        if (entry->head.kind != &kind_node_entry) return entry;
        NODE *en = ALLOC_node_end_block(entry, -tc->chain, -1 - tc->chain);   /* the once cell lives in the node */
        bake_add(tc, &en->u.node_end_block.def_env_off);
        bake_add(tc, &en->u.node_end_block.self_off);
        return en;
      }
      case PM_FLIP_FLOP_NODE: {     /* `if a..b` — sed/awk range condition (state on the node) */
        const pm_flip_flop_node_t *ff = (const pm_flip_flop_node_t *)node;
        NODE *l = transduce(tc, ff->left);       /* evaluated in place (no staging slots) */
        NODE *r = transduce(tc, ff->right);
        return ALLOC_node_flipflop(l, r, (ff->base.flags & PM_RANGE_FLAGS_EXCLUDE_END) ? 1u : 0u);
      }
      case PM_SOURCE_LINE_NODE: return ALLOC_node_lit(LONG2FIX((korb_sword_t)(int32_t)kp_line(tc, node)));    /* __LINE__ (signed: eval's first line may be negative) */
      case PM_SOURCE_ENCODING_NODE: {   /* __ENCODING__ → Encoding.find(<file encoding>) */
        const char *const nm = korb_enc_name_of(tc->c->vm, tc->src_enc);
        NODE *recv, *arg;
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = KP_CONST(tc, korb_intern(tc->c->vm, "Encoding", 8), 0, INT32_MIN, INT32_MIN, 0),
                                     arg = ALLOC_node_str(nm, (uint32_t)strlen(nm))));
        return kp_send1(tc, korb_intern(tc->c->vm, "find", 4), kp_line(tc, node), recv, arg);
      }

      /* ---- self / instance variables (self cell at base[fs-1], -1-chain) ---- */
      case PM_SELF_NODE:
        return bake_self(tc);
      case PM_INSTANCE_VARIABLE_READ_NODE: {
        const pm_instance_variable_read_node_t *iv = (const pm_instance_variable_read_node_t *)node;
        return bake_ivar_get(tc, kp_intern_cid(tc, iv->name));
      }
      case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        const pm_instance_variable_write_node_t *iw = (const pm_instance_variable_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, iw->name);
        NODE *val = transduce(tc, iw->value);    /* register child, current chain */
        return bake_ivar_set(tc, name, val);
      }
      case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {   /* @x op= v */
        const pm_instance_variable_operator_write_node_t *ow =
            (const pm_instance_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t name = kp_intern_cid(tc, ow->name), line = kp_line(tc, node);
        NODE *lhs, *rhs, *comb;
        if (op == KP_PLUS || op == KP_MINUS) {   /* fused: one self guard / shape check / ivars deref */
            NODE *v;
            WITH_CHAIN(tc, kind_node_ivar_arith.slot_count, (v = transduce(tc, ow->value)));
            NODE *a = ALLOC_node_ivar_arith(-1 - tc->chain, name, op == KP_MINUS, v, line);
            bake_add(tc, &a->u.node_ivar_arith.self_off);
            return a;
        }
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = bake_ivar_get(tc, name),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(tc, op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = bake_ivar_get(tc, name),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(tc, opmid, line, lhs, rhs);
        }
        return bake_ivar_set(tc, name, comb);
      }
      case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {        /* @x &&= v */
        const pm_instance_variable_and_write_node_t *aw =
            (const pm_instance_variable_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->name);
        return ALLOC_node_and(bake_ivar_get(tc, name),
                              bake_ivar_set(tc, name, transduce(tc, aw->value)));
      }
      case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {         /* @x ||= v */
        const pm_instance_variable_or_write_node_t *ow =
            (const pm_instance_variable_or_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->name);
        return ALLOC_node_or(bake_ivar_get(tc, name),
                             bake_ivar_set(tc, name, transduce(tc, ow->value)));
      }

      /* ---- class variables `@@x` (self at base[fs-1], def_class at fs-2) ---- */
      case PM_CLASS_VARIABLE_READ_NODE: {
        const pm_class_variable_read_node_t *cv = (const pm_class_variable_read_node_t *)node;
        return bake_cvar_get(tc, kp_intern_cid(tc, cv->name), 0);
      }
      case PM_CLASS_VARIABLE_WRITE_NODE: {
        const pm_class_variable_write_node_t *cw = (const pm_class_variable_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, cw->name);
        NODE *val = transduce(tc, cw->value);
        return bake_cvar_set(tc, name, val);
      }
      case PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE: {      /* @@x op= v */
        const pm_class_variable_operator_write_node_t *ow =
            (const pm_class_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t name = kp_intern_cid(tc, ow->name), line = kp_line(tc, node);
        NODE *lhs, *rhs, *comb;
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = bake_cvar_get(tc, name, 0),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(tc, op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = bake_cvar_get(tc, name, 0),
                                         rhs = transduce(tc, ow->value)));
            comb = kp_send1(tc, opmid, line, lhs, rhs);
        }
        return bake_cvar_set(tc, name, comb);
      }
      case PM_CLASS_VARIABLE_AND_WRITE_NODE: {           /* @@x &&= v */
        const pm_class_variable_and_write_node_t *aw =
            (const pm_class_variable_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->name);
        return ALLOC_node_and(bake_cvar_get(tc, name, 1),
                              bake_cvar_set(tc, name, transduce(tc, aw->value)));
      }
      case PM_CLASS_VARIABLE_OR_WRITE_NODE: {            /* @@x ||= v */
        const pm_class_variable_or_write_node_t *ow =
            (const pm_class_variable_or_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->name);
        return ALLOC_node_or(bake_cvar_get(tc, name, 1),
                             bake_cvar_set(tc, name, transduce(tc, ow->value)));
      }

      case PM_ARRAY_NODE: {
        const pm_array_node_t *an = (const pm_array_node_t *)node;
        size_t cnt = an->elements.size;
        return build_array(tc, an->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_RANGE_NODE: {
        const pm_range_node_t *rn = (const pm_range_node_t *)node;
        uint32_t excl = (rn->base.flags & PM_RANGE_FLAGS_EXCLUDE_END) ? 1u : 0u;
        NODE *b, *e;                                     /* a missing bound → nil (beginless/endless range) */
        uint32_t sc = kind_node_range_new.slot_count;
        WITH_CHAIN(tc, sc, (b = rn->left  ? transduce(tc, rn->left)  : lit_nil(),
                            e = rn->right ? transduce(tc, rn->right) : lit_nil()));
        return ALLOC_node_range_new(excl, b, e);
      }

      case PM_HASH_NODE: {
        const pm_hash_node_t *hn = (const pm_hash_node_t *)node;
        size_t cnt = hn->elements.size;
        return kp_warn_dup_hash_keys(tc, hn->elements.nodes, cnt,
                                     build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt));
      }

      case PM_KEYWORD_HASH_NODE: {       /* trailing `k: v` / `**h` args → a Hash (becomes kwargs) */
        const pm_keyword_hash_node_t *hn = (const pm_keyword_hash_node_t *)node;
        size_t cnt = hn->elements.size;
        NODE *const h = build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
        return kp_warn_dup_hash_keys(tc, hn->elements.nodes, cnt, ALLOC_node_kwargs_mark(h));   /* the callee binds a tagged Hash to keywords */
      }

      case PM_X_STRING_NODE: {          /* `cmd` → self.`("cmd") */
        const pm_x_string_node_t *xn = (const pm_x_string_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&xn->unescaped, &len);
        const uint32_t bt = korb_intern(tc->c->vm, "`", 1);
        NODE *recv, *arg;
        /* a non-interpolated command literal hands a FROZEN String to `` (CRuby) */
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = bake_self(tc),
                                     arg = ALLOC_node_str_frozen(bytes, len)));
        return kp_send1(tc, bt, kp_line(tc, node), recv, arg);
      }
      case PM_INTERPOLATED_X_STRING_NODE: {   /* `cmd #{x}` → self.`(dstr) */
        const pm_interpolated_x_string_node_t *xn = (const pm_interpolated_x_string_node_t *)node;
        const uint32_t bt = korb_intern(tc->c->vm, "`", 1);
        NODE *recv, *arg;
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = bake_self(tc),
                                     arg = build_dstr(tc, xn->parts.nodes, xn->parts.size)));
        return kp_send1(tc, bt, kp_line(tc, node), recv, arg);
      }
      case PM_INTERPOLATED_STRING_NODE: {
        const pm_interpolated_string_node_t *in = (const pm_interpolated_string_node_t *)node;
        /* the node's own flag bits are FROZEN/MUTABLE here, not forced encoding:
         * each part carries its own, and the concat negotiates from there */
        /* All-literal parts (a heredoc with no interpolation, `"a" "b"`): CRuby
         * makes ONE static literal out of them, so `# frozen_string_literal: true`
         * freezes it and it is not rebuilt per evaluation.  Concatenating at
         * parse time needs every part to agree on the encoding. */
        {
            uint8_t lenc = tc->src_enc; bool all_lit = true; size_t total = 0;
            for (size_t i = 0; i < in->parts.size; i++) {
                const pm_node_t *const pt = in->parts.nodes[i];
                if (!PM_NODE_TYPE_P(pt, PM_STRING_NODE)) { all_lit = false; break; }
                const uint8_t e = kp_forced_enc(pt->flags, tc->src_enc);
                if (i == 0) lenc = e; else if (e != lenc) { all_lit = false; break; }
                total += pm_string_length(&((const pm_string_node_t *)pt)->unescaped);
            }
            if (all_lit) {
                char *const bytes = malloc(total + 1);
                if (bytes == NULL) abort();
                size_t off = 0;
                for (size_t i = 0; i < in->parts.size; i++) {
                    const pm_string_t *const u = &((const pm_string_node_t *)in->parts.nodes[i])->unescaped;
                    const size_t l = pm_string_length(u);
                    memcpy(bytes + off, pm_string_source(u), l);
                    off += l;
                }
                bytes[total] = '\0';
                const uint32_t mode = ((in->base.flags & PM_STRING_FLAGS_FROZEN) || tc->fsl_true) ? 2u
                                    : (tc->fsl_false ? 0u : 1u);
                if (lenc != KORB_ENC_UTF8) return ALLOC_node_str_enc(bytes, (uint32_t)total, lenc, mode);
                if (mode == 2) return ALLOC_node_str_frozen(bytes, (uint32_t)total);
                if (mode == 1) return ALLOC_node_str_chilled(bytes, (uint32_t)total);
                return ALLOC_node_str(bytes, (uint32_t)total);
            }
        }
        return build_dstr(tc, in->parts.nodes, in->parts.size);
      }
      case PM_INTERPOLATED_SYMBOL_NODE: {     /* :"...#{ }..." → dstr.to_sym */
        const pm_interpolated_symbol_node_t *in = (const pm_interpolated_symbol_node_t *)node;
        uint32_t to_sym = korb_intern(tc->c->vm, "to_sym", 6);
        NODE *str;
        WITH_CHAIN(tc, KP_SEND0_SC, (str = build_dstr(tc, in->parts.nodes, in->parts.size)));
        return kp_send0(tc, to_sym, kp_line(tc, node), str);
      }
      case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {   /* /...#{ }.../ → Regexp(dstr, flags) */
        kp_note_backref(tc);
        extern const struct NodeKind kind_node_regexp_dyn;
        const pm_interpolated_regular_expression_node_t *in = (const pm_interpolated_regular_expression_node_t *)node;
        uint32_t flags = in->base.flags & (PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE |
                                           PM_REGULAR_EXPRESSION_FLAGS_EXTENDED |
                                           PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE |
                                           PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT |
                                           PM_REGULAR_EXPRESSION_FLAGS_EUC_JP |
                                           PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J |
                                           PM_REGULAR_EXPRESSION_FLAGS_UTF_8);
        flags |= kp_re_forced_flags(in->base.flags);
        NODE *src;
        WITH_CHAIN(tc, kind_node_regexp_dyn.slot_count, (src = build_dstr_enc(tc, in->parts.nodes, in->parts.size, KORB_ENC_UTF8)));   /* the /u /e /s /n modifier decides, not the file */
        return ALLOC_node_regexp_dyn(flags, src);
      }
      case PM_EMBEDDED_STATEMENTS_NODE: {
        const pm_embedded_statements_node_t *en = (const pm_embedded_statements_node_t *)node;
        if (!en->statements) return ALLOC_node_lit(KORB_NIL);   /* #{} → "" via nil.to_s */
        return transduce_statements(tc, en->statements);
      }
      case PM_EMBEDDED_VARIABLE_NODE:   /* "#@ivar" / "#$gvar" / "#@@cvar" — same as #{...} */
        return transduce(tc, ((const pm_embedded_variable_node_t *)node)->variable);

      /* ---- locals (depth 0 = own frame, depth >= 1 = outer/closure) ---- */
      case PM_LOCAL_VARIABLE_READ_NODE: {
        const pm_local_variable_read_node_t *lr = (const pm_local_variable_read_node_t *)node;
        return lvar_read(tc, node, lr->name, lr->depth);
      }
      case PM_IT_LOCAL_VARIABLE_READ_NODE:    /* `it` — the block's single implicit param (slot 0) */
        return bake_lget(tc, 0);
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
        const pm_local_variable_write_node_t *lw = (const pm_local_variable_write_node_t *)node;
        NODE *rval = transduce(tc, lw->value);   /* register child: no staging */
        return lvar_write(tc, node, lw->name, lw->depth, rval);
      }
      case PM_MULTI_WRITE_NODE: {
        /* pre..., [*splat,] post... = rhs  (depth-0 local targets) */
        const pm_multi_write_node_t *mw = (const pm_multi_write_node_t *)node;
        /* a depth-0 local target; returns its name cid or fails (unsupported) */
        #define MW_LOCAL_CID(t, outcid)                                         \
            do {                                                                \
                if (!PM_NODE_TYPE_P((t), PM_LOCAL_VARIABLE_TARGET_NODE))        \
                    return kp_unsupported(tc, (t), "non-local multi-assign target"); \
                if (((const pm_local_variable_target_node_t *)(t))->depth != 0) \
                    return kp_unsupported(tc, (t), "outer-scope multi-assign target"); \
                (outcid) = ((const pm_local_variable_target_node_t *)(t))->name; \
            } while (0)

        const bool has_splat = mw->rest && PM_NODE_TYPE_P(mw->rest, PM_SPLAT_NODE);
        /* `a, b, = rhs` (trailing comma → implicit rest) just discards extra rhs
         * elements, which the non-splat path already does — treat as no rest. */
        if (mw->rest && !has_splat && !PM_NODE_TYPE_P(mw->rest, PM_IMPLICIT_REST_NODE))
            return kp_unsupported(tc, node, "multi-assign with anonymous rest");

        if (!has_splat) {
            uint32_t nt = (uint32_t)mw->lefts.size;
            /* classify: all depth-0 locals → node_massign (fast); a mix of
             * local / @ivar / CONST → node_massign_het; a call/attribute target
             * (recv.x=) → general synth-temp desugar; anything else → no. */
            bool all_local = true, needs_general = false;
            for (uint32_t i = 0; i < nt; i++) {
                const pm_node_t *t = mw->lefts.nodes[i];
                if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    if (((const pm_local_variable_target_node_t *)t)->depth != 0) {
                        all_local = false; needs_general = true;   /* outer-scope local → general synth-temp desugar (depth-aware) */
                    }
                } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE)) {
                    all_local = false;
                } else if (PM_NODE_TYPE_P(t, PM_CONSTANT_TARGET_NODE)) {
                    /* node_massign_het defines constants at top level; the
                     * general path goes through build_const_set, which keeps the
                     * lexical owner (`A, B = 1` inside `module M` → M::A). */
                    all_local = false; needs_general = true;
                } else if (PM_NODE_TYPE_P(t, PM_CALL_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_INDEX_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_GLOBAL_VARIABLE_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_CLASS_VARIABLE_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_CONSTANT_PATH_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_MULTI_TARGET_NODE)) {   /* nested `(a,b)` / $g / @@cv / M::X / recv.x= / recv[k]= → general desugar */
                    all_local = false; needs_general = true;
                } else {
                    return kp_unsupported(tc, t, "non-local multi-assign target");
                }
            }
            if (needs_general) {
                NODE *const g = massign_general(tc, &mw->lefts, NULL, NULL, transduce(tc, mw->value));
                if (!g) return kp_unsupported(tc, node, "non-local multi-assign target");
                return g;
            }
            if (all_local) {
                int32_t *offs = malloc(sizeof(int32_t) * (nt ? nt : 1));
                if (!offs) abort();
                pm_constant_id_t cid;
                NODE *rhs = transduce(tc, mw->value);             /* register child */
                for (uint32_t i = 0; i < nt; i++) {
                    MW_LOCAL_CID(mw->lefts.nodes[i], cid);
                    offs[i] = (int32_t)lvar_index(tc, mw->lefts.nodes[i], cid) - tc->chain;
                }
                NODE *mn = ALLOC_node_massign(offs, nt, rhs);
                for (uint32_t i = 0; i < nt; i++) bake_add(tc, &offs[i]);
                return mn;
            }
            /* heterogeneous: {kind, data} per target (matches node_massign_het) */
            struct kp_mdesc { int32_t kind; int32_t data; };
            struct kp_mdesc *descs = malloc(sizeof(*descs) * (nt ? nt : 1));
            if (!descs) abort();
            NODE *rhs = transduce(tc, mw->value);                 /* register child */
            for (uint32_t i = 0; i < nt; i++) {
                const pm_node_t *t = mw->lefts.nodes[i];
                if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    pm_constant_id_t cid = ((const pm_local_variable_target_node_t *)t)->name;
                    descs[i].kind = 0;
                    descs[i].data = (int32_t)lvar_index(tc, t, cid) - tc->chain;
                } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE)) {
                    descs[i].kind = 1;
                    descs[i].data = (int32_t)kp_intern_cid(tc, ((const pm_instance_variable_target_node_t *)t)->name);
                } else {       /* PM_CONSTANT_TARGET_NODE */
                    descs[i].kind = 2;
                    descs[i].data = (int32_t)kp_intern_cid(tc, ((const pm_constant_target_node_t *)t)->name);
                }
            }
            NODE *mn = ALLOC_node_massign_het(descs, nt, -1 - tc->chain, rhs);
            bake_add(tc, &mn->u.node_massign_het.self_off);   /* self at base[-1] (bottom header) */
            for (uint32_t i = 0; i < nt; i++) if (descs[i].kind == 0) bake_add(tc, &descs[i].data);
            return mn;
        }

        /* splat path: lefts(npre) | *splat | rights(npost) */
        const pm_splat_node_t *sp = (const pm_splat_node_t *)mw->rest;
        const bool anon_splat = (sp->expression == NULL);    /* `first, *, last = ...` → discard middle */
        /* Any non-(depth-0-local) target (a splat / pre / post that is @ivar / CONST /
         * $g / recv.x= / recv[k]=) → desugar: massign into synth locals with the fast
         * all-local node, then plumb each synth out to its real target. */
        {
            bool all_local = true;
            #define IS_L0(t) (PM_NODE_TYPE_P((t), PM_LOCAL_VARIABLE_TARGET_NODE) && \
                              ((const pm_local_variable_target_node_t *)(t))->depth == 0)
            for (uint32_t i = 0; i < mw->lefts.size && all_local; i++) if (!IS_L0(mw->lefts.nodes[i])) all_local = false;
            for (uint32_t i = 0; i < mw->rights.size && all_local; i++) if (!IS_L0(mw->rights.nodes[i])) all_local = false;
            if (all_local && !anon_splat && !IS_L0(sp->expression)) all_local = false;
            #undef IS_L0
            if (!all_local) {
                NODE *const g = massign_general(tc, &mw->lefts, mw->rest, &mw->rights, transduce(tc, mw->value));
                if (!g) return kp_unsupported(tc, node, "non-local splat target");
                return g;
            }
        }
        uint32_t npre = (uint32_t)mw->lefts.size, npost = (uint32_t)mw->rights.size;
        uint32_t no = npre + 1u + npost;
        int32_t *offs = malloc(sizeof(int32_t) * no);
        if (!offs) abort();
        pm_constant_id_t cid;
        NODE *rhs = transduce(tc, mw->value);
        for (uint32_t i = 0; i < npre; i++) {
            MW_LOCAL_CID(mw->lefts.nodes[i], cid);
            offs[i] = (int32_t)lvar_index(tc, mw->lefts.nodes[i], cid) - tc->chain;
        }
        if (anon_splat) {                                    /* middle → a throwaway synth local */
            offs[npre] = (int32_t)alloc_synth_local(tc) - tc->chain;
        } else {
            MW_LOCAL_CID(sp->expression, cid);
            offs[npre] = (int32_t)lvar_index(tc, sp->expression, cid) - tc->chain;
        }
        for (uint32_t i = 0; i < npost; i++) {
            MW_LOCAL_CID(mw->rights.nodes[i], cid);
            offs[npre + 1u + i] = (int32_t)lvar_index(tc, mw->rights.nodes[i], cid) - tc->chain;
        }
        NODE *mn = ALLOC_node_massign_splat(offs, npre, npost, rhs);
        for (uint32_t i = 0; i < no; i++) bake_add(tc, &offs[i]);
        return mn;
        #undef MW_LOCAL_CID
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        /* x op= v  →  write(x, binop(read(x), v)) */
        const pm_local_variable_operator_write_node_t *ow =
            (const pm_local_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t line = kp_line(tc, node);
        NODE *lhs, *rhs, *comb;
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(tc, op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(tc, opmid, line, lhs, rhs);
        }
        return lvar_write(tc, node, ow->name, ow->depth, comb);
      }
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
        const pm_local_variable_and_write_node_t *aw =
            (const pm_local_variable_and_write_node_t *)node;
        return ALLOC_node_and(lvar_read(tc, node, aw->name, aw->depth),
                              lvar_write(tc, node, aw->name, aw->depth, transduce(tc, aw->value)));
      }
      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
        const pm_local_variable_or_write_node_t *ow =
            (const pm_local_variable_or_write_node_t *)node;
        return ALLOC_node_or(lvar_read(tc, node, ow->name, ow->depth),
                             lvar_write(tc, node, ow->name, ow->depth, transduce(tc, ow->value)));
      }
      case PM_INDEX_OR_WRITE_NODE:           /* recv[k...] ||= value */
      case PM_INDEX_AND_WRITE_NODE: {        /* recv[k...] &&= value */
        const bool is_or = PM_NODE_TYPE_P(node, PM_INDEX_OR_WRITE_NODE);
        /* both node types share receiver/arguments/block/value field layout */
        const pm_index_or_write_node_t *iw = (const pm_index_or_write_node_t *)node;
        size_t argc = iw->arguments ? iw->arguments->arguments.size : 0;
        if (iw->block || argc < 1)
            return kp_unsupported(tc, node, is_or ? "index ||= with block or zero index args"
                                                  : "index &&= with block or zero index args");
        { bool spl = false;                              /* `h[*a] ||= v` — the splat form shares the op= shape */
          for (size_t i = 0; i < argc; i++) if (PM_NODE_TYPE_P(iw->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { spl = true; break; }
          if (spl) return index_opassign_splat(tc, (const pm_index_operator_write_node_t *)node, node, iw->arguments, iw->value); }
        uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
        uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
        uint32_t line = kp_line(tc, node);
        uint32_t t0 = alloc_synth_local(tc);
        uint32_t *tk = malloc(sizeof(uint32_t) * argc); if (!tk) abort();
        for (size_t i = 0; i < argc; i++) tk[i] = alloc_synth_local(tc);
        uint32_t t_val = alloc_synth_local(tc);
        const uint32_t sc_get = (uint32_t)(1 + argc) + KORB_FRAME_HDR;
        const uint32_t sc_set = (uint32_t)(2 + argc) + KORB_FRAME_HDR;

        NODE *stores = bake_lset(tc, t0, transduce(tc, iw->receiver));
        for (size_t i = 0; i < argc; i++)
            stores = ALLOC_node_seq(stores, bake_lset(tc, tk[i], transduce(tc, iw->arguments->arguments.nodes[i])));

        NODE *g_recv, **g_k = malloc(sizeof(NODE *) * argc); if (!g_k) abort();
        { int32_t _s = tc->chain; tc->chain = _s + (int32_t)sc_get;
          g_recv = bake_lget(tc, t0);
          for (size_t i = 0; i < argc; i++) g_k[i] = bake_lget(tc, tk[i]);
          tc->chain = _s; }
        NODE *get = kp_send_n(tc, aref, line, g_recv, g_k, (uint32_t)argc);
        free(g_k);

        /* rhs branch (falsy for ||=, truthy for &&=): t_val = value; recv[k...] = t_val; yield t_val.
         * value is evaluated only when the branch runs (inside the or/and's rhs). */
        NODE *store_val = bake_lset(tc, t_val, transduce(tc, iw->value));
        NODE *set;
        { NODE *s_recv, **s_args = malloc(sizeof(NODE *) * (argc + 1)); if (!s_args) abort();
          int32_t _s = tc->chain; tc->chain = _s + (int32_t)sc_set;
          s_recv = bake_lget(tc, t0);
          for (size_t i = 0; i < argc; i++) s_args[i] = bake_lget(tc, tk[i]);
          s_args[argc] = bake_lget(tc, t_val);
          tc->chain = _s;
          set = kp_send_n(tc, aset, line, s_recv, s_args, (uint32_t)(argc + 1));
          free(s_args); }
        free(tk);
        NODE *set_branch = ALLOC_node_seq(store_val, ALLOC_node_seq(set, bake_lget(tc, t_val)));
        return ALLOC_node_seq(stores, is_or ? ALLOC_node_or(get, set_branch)
                                            : ALLOC_node_and(get, set_branch));
      }
      case PM_INDEX_OPERATOR_WRITE_NODE: {  /* recv[k...] op= value  →  recv[k...] = recv[k...] op value */
        const pm_index_operator_write_node_t *iw = (const pm_index_operator_write_node_t *)node;
        size_t argc = iw->arguments ? iw->arguments->arguments.size : 0;
        if (iw->block || argc < 1)                       /* argc>=1: single OR multi index (grid[y,x] += 1) */
            return kp_unsupported(tc, node, "index op= with block or zero index args");
        { bool spl = false;
          for (size_t i = 0; i < argc; i++) if (PM_NODE_TYPE_P(iw->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { spl = true; break; }
          if (spl) return index_opassign_splat(tc, iw, node, iw->arguments, iw->value); }
        uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
        uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, iw->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, iw->binary_operator);
        uint32_t line = kp_line(tc, node);
        /* temps: t0 = recv, tk[0..argc) = each index arg, t_new = computed value.
         * Single-eval: recv + every index expression are evaluated exactly once. */
        uint32_t t0 = alloc_synth_local(tc);
        uint32_t *tk = malloc(sizeof(uint32_t) * argc); if (!tk) abort();
        for (size_t i = 0; i < argc; i++) tk[i] = alloc_synth_local(tc);
        uint32_t t_new = alloc_synth_local(tc);
        const uint32_t sc_get = (uint32_t)(1 + argc) + KORB_FRAME_HDR;   /* []  children: recv + argc keys      */
        const uint32_t sc_set = (uint32_t)(2 + argc) + KORB_FRAME_HDR;   /* []= children: recv + argc keys + val */

        NODE *stores = bake_lset(tc, t0, transduce(tc, iw->receiver));
        for (size_t i = 0; i < argc; i++)
            stores = ALLOC_node_seq(stores, bake_lset(tc, tk[i], transduce(tc, iw->arguments->arguments.nodes[i])));

        /* t_new = (recv[k...]) op value — into a temp first so the expression yields
         * the ASSIGNED value, not `[]=`'s return (CRuby). */
        NODE *newval;
        const uint32_t bsc = (op != KP_BINOP_NONE) ? kind_node_plus.slot_count : KP_SEND1_SC;
        WITH_CHAIN(tc, bsc, ({
            NODE *g_recv, **g_k = malloc(sizeof(NODE *) * argc), *get, *val;
            if (!g_k) abort();
            { int32_t _s = tc->chain; tc->chain = _s + (int32_t)sc_get;   /* stage the [] send's children */
              g_recv = bake_lget(tc, t0);
              for (size_t i = 0; i < argc; i++) g_k[i] = bake_lget(tc, tk[i]);
              tc->chain = _s; }
            get = kp_send_n(tc, aref, line, g_recv, g_k, (uint32_t)argc);
            free(g_k);
            val = transduce(tc, iw->value);
            newval = (op != KP_BINOP_NONE) ? alloc_binop(tc, op, get, val, line) : kp_send1(tc, opmid, line, get, val);
            newval;
        }));
        NODE *store_newval = bake_lset(tc, t_new, newval);

        /* recv[k...] = t_new */
        NODE *set;
        { NODE *s_recv, **s_args = malloc(sizeof(NODE *) * (argc + 1)); if (!s_args) abort();
          int32_t _s = tc->chain; tc->chain = _s + (int32_t)sc_set;      /* stage the []= send's children */
          s_recv = bake_lget(tc, t0);
          for (size_t i = 0; i < argc; i++) s_args[i] = bake_lget(tc, tk[i]);
          s_args[argc] = bake_lget(tc, t_new);
          tc->chain = _s;
          set = kp_send_n(tc, aset, line, s_recv, s_args, (uint32_t)(argc + 1));
          free(s_args); }
        free(tk);
        /* value of `recv[k...] op= v` is the assigned value (t_new) */
        return ALLOC_node_seq(stores, ALLOC_node_seq(store_newval, ALLOC_node_seq(set, bake_lget(tc, t_new))));
      }

      case PM_CALL_OPERATOR_WRITE_NODE: {  /* recv.attr op= value  →  recv.attr = recv.attr op value */
        const pm_call_operator_write_node_t *cw = (const pm_call_operator_write_node_t *)node;
        uint32_t read_mid  = kp_intern_cid(tc, cw->read_name);    /* getter  (e.g. :hl)  */
        uint32_t write_mid = kp_intern_cid(tc, cw->write_name);   /* setter  (e.g. :hl=) */
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, cw->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, cw->binary_operator);
        uint32_t line = kp_line(tc, node);
        uint32_t t0 = alloc_synth_local(tc), t1 = alloc_synth_local(tc);
        /* evaluate recv once into a temp (single-eval semantics) */
        NODE *store_recv = bake_lset(tc, t0, transduce(tc, cw->receiver));
        /* t1 = (recv.attr) op value — compute the new value first so the whole
         * expression yields the ASSIGNED value, not the setter's return (CRuby). */
        NODE *newval;
        const uint32_t bsc = (op != KP_BINOP_NONE) ? kind_node_plus.slot_count : KP_SEND1_SC;
        WITH_CHAIN(tc, bsc, ({
            NODE *g_recv, *get, *val;
            WITH_CHAIN(tc, KP_SEND0_SC, (g_recv = bake_lget(tc, t0)));
            get = kp_send0(tc, read_mid, line, g_recv);
            val = transduce(tc, cw->value);
            newval = (op != KP_BINOP_NONE) ? alloc_binop(tc, op, get, val, line) : kp_send1(tc, opmid, line, get, val);
        }));
        NODE *store_newval = bake_lset(tc, t1, newval);
        /* recv.attr = t1 */
        NODE *s_recv, *s_val;
        WITH_CHAIN(tc, KP_SEND1_SC, (s_recv = bake_lget(tc, t0), s_val = bake_lget(tc, t1)));
        NODE *set = kp_send1(tc, write_mid, line, s_recv, s_val);
        NODE *body = ALLOC_node_seq(store_newval, ALLOC_node_seq(set, bake_lget(tc, t1)));
        /* `recv&.attr op= v` — a nil receiver yields nil and runs nothing else */
        if (cw->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)
            body = ALLOC_node_nil_guard(bake_lget(tc, t0), body);
        return ALLOC_node_seq(store_recv, body);
      }

      case PM_CALL_OR_WRITE_NODE:        /* recv.attr ||= value */
      case PM_CALL_AND_WRITE_NODE: {     /* recv.attr &&= value */
        /* both share the layout {receiver, read_name, write_name, value}; the only
         * difference is or (||) vs and (&&) around the read, with value evaluated
         * lazily on the taken branch (single-eval receiver). */
        const bool is_or = PM_NODE_TYPE_P(node, PM_CALL_OR_WRITE_NODE);
        const pm_call_or_write_node_t *cw = (const pm_call_or_write_node_t *)node;   /* and-write has an identical prefix */
        uint32_t read_mid  = kp_intern_cid(tc, cw->read_name);
        uint32_t write_mid = kp_intern_cid(tc, cw->write_name);
        uint32_t line = kp_line(tc, node);
        uint32_t t0 = alloc_synth_local(tc), t1 = alloc_synth_local(tc);
        NODE *store_recv = bake_lset(tc, t0, transduce(tc, cw->receiver));
        NODE *g_recv;
        WITH_CHAIN(tc, KP_SEND0_SC, (g_recv = bake_lget(tc, t0)));
        NODE *get = kp_send0(tc, read_mid, line, g_recv);
        NODE *store_val = bake_lset(tc, t1, transduce(tc, cw->value));   /* only the taken branch runs it */
        NODE *s_recv, *s_val;
        WITH_CHAIN(tc, KP_SEND1_SC, (s_recv = bake_lget(tc, t0), s_val = bake_lget(tc, t1)));
        NODE *set = kp_send1(tc, write_mid, line, s_recv, s_val);
        NODE *set_branch = ALLOC_node_seq(store_val, ALLOC_node_seq(set, bake_lget(tc, t1)));
        NODE *body = is_or ? ALLOC_node_or(get, set_branch) : ALLOC_node_and(get, set_branch);
        /* `recv&.attr ||= v` / `&&=` — a nil receiver yields nil, reads nothing */
        if (cw->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)
            body = ALLOC_node_nil_guard(bake_lget(tc, t0), body);
        return ALLOC_node_seq(store_recv, body);
      }

      /* ---- control flow ---- */
      case PM_IF_NODE: {
        const pm_if_node_t *ifn = (const pm_if_node_t *)node;
        NODE *cond = transduce(tc, ifn->predicate);     /* register child */
        NODE *then_b = transduce_statements(tc, ifn->statements);
        NODE *else_b = transduce_opt(tc, ifn->subsequent);
        return ALLOC_node_if(cond, then_b, else_b);
      }
      case PM_ELSE_NODE: {
        const pm_else_node_t *en = (const pm_else_node_t *)node;
        return transduce_statements(tc, en->statements);
      }
      case PM_CASE_MATCH_NODE: {
        /* `case subj; in pat; body; ...; [else e]; end` → subject once into a
         * synth temp, then an if/elsif chain of node_match_pred tests (each binds
         * locals as a side effect, like the one-line `subj in pat`).  No clause +
         * no else → node_match_req on an always-fail desc raises NoMatchingPattern. */
        const pm_case_match_node_t *cn = (const pm_case_match_node_t *)node;
        uint32_t tmp = alloc_synth_local(tc);
        NODE *subj = transduce(tc, cn->predicate);
        NODE *assign = bake_lset(tc, tmp, subj);
        NODE *chain;
        if (cn->else_clause) {
            chain = transduce_statements(tc, cn->else_clause->statements);
        } else {
            struct korb_pat *fail = calloc(1, sizeof(*fail));   /* unknown kind → matcher returns false → raise */
            if (!fail) abort();
            fail->kind = 255;
            chain = ALLOC_node_match_req(bake_lget(tc, tmp), (void *)fail);
        }
        for (size_t i = cn->conditions.size; i-- > 0; ) {
            const pm_in_node_t *in = (const pm_in_node_t *)cn->conditions.nodes[i];
            /* `in pat if guard` / `unless guard` → prism wraps the pattern in an
             * If/Unless node (predicate = guard, statements[0] = the real pattern). */
            const pm_node_t *pat = in->pattern;
            const pm_node_t *guard = NULL; bool guard_unless = false;
            if (PM_NODE_TYPE_P(pat, PM_IF_NODE)) {
                const pm_if_node_t *g = (const pm_if_node_t *)pat;
                guard = g->predicate;
                if (g->statements && g->statements->body.size > 0) pat = g->statements->body.nodes[0];
            } else if (PM_NODE_TYPE_P(pat, PM_UNLESS_NODE)) {
                const pm_unless_node_t *g = (const pm_unless_node_t *)pat;
                guard = g->predicate; guard_unless = true;
                if (g->statements && g->statements->body.size > 0) pat = g->statements->body.nodes[0];
            }
            NODE *body = in->statements ? transduce_statements(tc, in->statements) : lit_nil();
            struct korb_pat *desc = build_pattern_desc(tc, pat);
            NODE *test = ALLOC_node_match_pred(bake_lget(tc, tmp), (void *)desc);
            if (guard) {                                       /* match binds first (lazy &&), then the guard reads bindings */
                NODE *g = transduce(tc, guard);
                if (guard_unless) g = ALLOC_node_not(g);
                test = ALLOC_node_and(test, g);
            }
            chain = ALLOC_node_if(test, body, chain);
        }
        return ALLOC_node_seq(assign, chain);
      }

      case PM_CASE_NODE: {
        /* `case subj; when a, b; body; ...; else e; end` desugared to an if/elsif
         * chain.  With a subject, each `when v` tests `v === subj`, the subject
         * evaluated once into a synthetic temp.  Without a subject, each `when c`
         * is a plain truthiness test. */
        const pm_case_node_t *cn = (const pm_case_node_t *)node;
        const bool has_subj = cn->predicate != NULL;
        NODE *assign = NULL;
        uint32_t tmp = 0;
        if (has_subj) {
            tmp = alloc_synth_local(tc);
            NODE *subj = transduce(tc, cn->predicate);   /* register child: no staging */
            assign = bake_lset(tc, tmp, subj);
        }
        NODE *chain = cn->else_clause
            ? transduce_statements(tc, cn->else_clause->statements)
            : lit_nil();
        for (size_t wi = cn->conditions.size; wi-- > 0; ) {  /* fold last → first */
            const pm_when_node_t *wn = (const pm_when_node_t *)cn->conditions.nodes[wi];
            NODE *body = transduce_statements(tc, wn->statements);
            NODE *cond = NULL;
            for (size_t ci = 0; ci < wn->conditions.size; ci++) {
                const pm_node_t *cv = wn->conditions.nodes[ci];
                if (PM_NODE_TYPE_P(cv, PM_SPLAT_NODE)) {
                    /* `when *pats` — expand at runtime: pats.__korb_when_splat(subj)
                     * (a prelude helper: any? { |p| p === subj }).  Subject-less
                     * case has no value to test each element against. */
                    const pm_splat_node_t *sp = (const pm_splat_node_t *)cv;
                    if (!sp->expression)
                        return kp_unsupported(tc, cv, "when with a bare splat");
                    const uint32_t line = kp_line(tc, cv);
                    if (!has_subj) {                        /* `case; when *pats` → any truthy element */
                        NODE *recv0;
                        WITH_CHAIN(tc, KP_SEND0_SC, (recv0 = transduce(tc, sp->expression)));
                        NODE *test0 = kp_send0(tc, korb_intern(tc->c->vm, "__korb_when_splat_truthy", 24), line, recv0);
                        cond = cond ? ALLOC_node_or(cond, test0) : test0;
                        continue;
                    }
                    NODE *recv, *subj_arg;
                    WITH_CHAIN(tc, KP_SEND1_SC, (recv = transduce(tc, sp->expression),
                                                 subj_arg = bake_lget(tc, tmp)));
                    NODE *test = kp_send1(tc, korb_intern(tc->c->vm, "__korb_when_splat", 17), line, recv, subj_arg);
                    cond = cond ? ALLOC_node_or(cond, test) : test;
                    continue;
                }
                NODE *test;
                if (has_subj) {
                    NODE *vn, *targ; (void)kp_line(tc, cv);
                    WITH_CHAIN(tc, kind_node_caseeq.slot_count, (vn = transduce(tc, cv), targ = bake_lget(tc, tmp)));
                    test = ALLOC_node_caseeq(vn, targ);     /* v === subj (inline for value-eq types) */
                } else {
                    test = transduce(tc, cv);                       /* plain truthiness */
                }
                cond = cond ? ALLOC_node_or(cond, test) : test;
            }
            if (cond == NULL) cond = lit_nil();
            chain = ALLOC_node_if(cond, body, chain);
        }
        return has_subj ? ALLOC_node_seq(assign, chain) : chain;
      }
      case PM_UNLESS_NODE: {
        const pm_unless_node_t *un = (const pm_unless_node_t *)node;
        NODE *cond = transduce(tc, un->predicate);
        NODE *then_b = transduce_statements(tc, un->statements);  /* unless-true */
        NODE *else_b = un->else_clause
            ? transduce_statements(tc, un->else_clause->statements)
            : lit_nil();
        return ALLOC_node_if(cond, else_b, then_b);     /* swapped branches */
      }
      case PM_WHILE_NODE: {
        const pm_while_node_t *wn = (const pm_while_node_t *)node;
        const bool post = PM_NODE_FLAG_P(wn, PM_LOOP_FLAGS_BEGIN_MODIFIER);   /* begin..end while → post-test */
        NODE *cond = transduce(tc, wn->predicate);
        NODE *body = transduce_statements(tc, wn->statements);
        return post ? ALLOC_node_post_while(cond, body, 0) : ALLOC_node_while(cond, body, 0);
      }
      case PM_UNTIL_NODE: {
        const pm_until_node_t *un = (const pm_until_node_t *)node;
        const bool post = PM_NODE_FLAG_P(un, PM_LOOP_FLAGS_BEGIN_MODIFIER);   /* begin..end until → post-test */
        NODE *cond = transduce(tc, un->predicate);
        NODE *body = transduce_statements(tc, un->statements);
        return post ? ALLOC_node_post_while(cond, body, 1) : ALLOC_node_while(cond, body, 1);
      }
      case PM_LAMBDA_NODE: {   /* ->(args) { body } — a lambda Proc */
        const pm_lambda_node_t *ln = (const pm_lambda_node_t *)node;
        tc->pending_lambda_body = true;
        NODE *entry = transduce_block_parts(tc, &ln->locals, ln->parameters, ln->body);
        tc->pending_lambda_body = false;
        if (entry->head.kind != &kind_node_entry) return entry;   /* unsupported params → propagate (don't reify a non-entry) */
        korb_reg_srcloc(tc->c->vm, entry, korb_intern(tc->c->vm, tc->fname, strlen(tc->fname)), kp_line(tc, node));   /* Proc#source_location */
        kp_reg_srcrange(tc, entry, node);   /* Proc#source_range: the whole `-> () { }` */
        entry->u.node_entry.def_line = kp_line(tc, node);
        int32_t self_off = -1 - tc->chain;
        NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, 1u, 0u);   /* `->{}` is syntax: no user override */
        bake_add(tc, &mk->u.node_make_proc.def_env_off);
        bake_add(tc, &mk->u.node_make_proc.self_off);   /* self at base[-1] */
        return mk;
      }
      case PM_MATCH_WRITE_NODE: {
        kp_note_backref(tc);
        /* `/(?<n>…)/ =~ str` — the ONE form where a regexp literal binds its named
         * captures to local variables.  prism hands us the `=~` call plus one
         * local-variable target per name; desugar to
         *     tmp = (re =~ str);  n = $~ && $~[:n];  …;  tmp
         * (`$~` is nil on a failed match, so the locals become nil, as CRuby does).
         * The synthetic tmp keeps the expression's value = the match position. */
        const pm_match_write_node_t *mw = (const pm_match_write_node_t *)node;
        const uint32_t line = kp_line(tc, node);
        const uint32_t tmp = alloc_synth_local(tc);
        NODE *seq = bake_lset(tc, tmp, transduce(tc, (const pm_node_t *)mw->call));
        const uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
        for (size_t i = 0; i < mw->targets.size; i++) {
            const pm_node_t *t = mw->targets.nodes[i];
            if (!PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) continue;
            const pm_local_variable_target_node_t *lt = (const pm_local_variable_target_node_t *)t;
            /* $~ && $~[:name] — read the global twice; it is a VM slot, not a call */
            const uint32_t lastmatch = korb_intern(tc->c->vm, "$~", 2);   /* globals live in the flat const table */
            NODE *md1 = KP_CONST(tc, lastmatch, 0, INT32_MIN, INT32_MIN, 0);
            NODE *md2 = KP_CONST(tc, lastmatch, 0, INT32_MIN, INT32_MIN, 0);
            NODE *sym = ALLOC_node_lit(ID2SYM(kp_intern_cid(tc, lt->name)));
            NODE *val = ALLOC_node_and(md1, kp_send1(tc, aref, line, md2, sym));
            seq = ALLOC_node_seq(seq, lvar_write(tc, t, lt->name, lt->depth, val));
        }
        return ALLOC_node_seq(seq, bake_lget(tc, tmp));
      }
      case PM_MATCH_PREDICATE_NODE: {                /* `expr in pattern` → bool */
        kp_note_backref(tc);
        const pm_match_predicate_node_t *mp = (const pm_match_predicate_node_t *)node;
        struct korb_pat *desc = build_pattern_desc(tc, mp->pattern);
        NODE *subj = transduce(tc, mp->value);
        return ALLOC_node_match_pred(subj, (void *)desc);
      }
      case PM_MATCH_REQUIRED_NODE: {                 /* `expr => pattern` → nil / raise */
        kp_note_backref(tc);
        const pm_match_required_node_t *mr = (const pm_match_required_node_t *)node;
        struct korb_pat *desc = build_pattern_desc(tc, mr->pattern);
        NODE *subj = transduce(tc, mr->value);
        return ALLOC_node_match_req(subj, (void *)desc);
      }

      case PM_FOR_NODE: {
        /* `for VAR in COLL; BODY; end` — VAR + BODY share the enclosing frame
         * (for introduces no scope), so BODY is transduced here, not in a block. */
        const pm_for_node_t *fn = (const pm_for_node_t *)node;
        if (PM_NODE_TYPE_P(fn->index, PM_MULTI_TARGET_NODE)) {
            /* `for k, v in coll` — iterate into a temp, then destructure it into
             * the (enclosing-frame) index targets at the top of the body. */
            const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)fn->index;
            uint32_t orig_local = alloc_synth_local(tc);
            uint32_t iter_local = alloc_synth_local(tc);
            uint32_t e_local    = alloc_synth_local(tc);   /* each element */
            NODE *coll = transduce(tc, fn->collection);
            NODE *decon = massign_general(tc, &mt->lefts, mt->rest, &mt->rights, bake_lget(tc, e_local));
            if (decon == NULL) return kp_unsupported(tc, fn->index, "for-loop index target");
            NODE *body0 = fn->statements ? transduce_statements(tc, fn->statements) : lit_nil();
            NODE *body = decon ? (body0 ? ALLOC_node_seq(decon, body0) : decon) : body0;
            NODE *fnode = ALLOC_node_for((int32_t)orig_local - tc->chain,
                                         (int32_t)iter_local - tc->chain,
                                         (int32_t)e_local - tc->chain,
                                         coll, body);
            bake_add(tc, &fnode->u.node_for.orig_off);
            bake_add(tc, &fnode->u.node_for.iter_off);
            bake_add(tc, &fnode->u.node_for.var_off);
            return fnode;
        }
        if (!PM_NODE_TYPE_P(fn->index, PM_LOCAL_VARIABLE_TARGET_NODE)) {
            /* `for @iv in coll` / @@cv / CONST / $g — iterate into a synth local
             * and assign it to the real target at the top of the body (the same
             * shape the destructuring form uses). */
            uint32_t orig_local = alloc_synth_local(tc);
            uint32_t iter_local = alloc_synth_local(tc);
            uint32_t e_local    = alloc_synth_local(tc);
            NODE *coll = transduce(tc, fn->collection);
            NODE *store = assign_target_from_synth(tc, fn->index, e_local);
            if (store == NULL) return kp_unsupported(tc, fn->index, "for-loop index target");
            NODE *body0 = fn->statements ? transduce_statements(tc, fn->statements) : lit_nil();
            NODE *body = body0 ? ALLOC_node_seq(store, body0) : store;
            NODE *fnode = ALLOC_node_for((int32_t)orig_local - tc->chain,
                                         (int32_t)iter_local - tc->chain,
                                         (int32_t)e_local - tc->chain,
                                         coll, body);
            bake_add(tc, &fnode->u.node_for.orig_off);
            bake_add(tc, &fnode->u.node_for.iter_off);
            bake_add(tc, &fnode->u.node_for.var_off);
            return fnode;
        }
        const pm_local_variable_target_node_t *iv = (const pm_local_variable_target_node_t *)fn->index;
        if (iv->depth != 0)
            return kp_unsupported(tc, fn->index, "for-loop with outer-scope index");
        uint32_t orig_local = alloc_synth_local(tc);
        uint32_t iter_local = alloc_synth_local(tc);
        NODE *coll = transduce(tc, fn->collection);
        NODE *body = fn->statements ? transduce_statements(tc, fn->statements) : lit_nil();
        NODE *fnode = ALLOC_node_for((int32_t)orig_local - tc->chain,
                                     (int32_t)iter_local - tc->chain,
                                     (int32_t)lvar_index(tc, fn->index, iv->name) - tc->chain,
                                     coll, body);
        bake_add(tc, &fnode->u.node_for.orig_off);
        bake_add(tc, &fnode->u.node_for.iter_off);
        bake_add(tc, &fnode->u.node_for.var_off);
        return fnode;
      }
      case PM_AND_NODE: {
        const pm_and_node_t *an = (const pm_and_node_t *)node;
        return ALLOC_node_and(transduce(tc, an->left), transduce(tc, an->right));
      }
      case PM_OR_NODE: {
        const pm_or_node_t *on = (const pm_or_node_t *)node;
        return ALLOC_node_or(transduce(tc, on->left), transduce(tc, on->right));
      }
      case PM_RETURN_NODE: {
        const pm_return_node_t *rn = (const pm_return_node_t *)node;
        /* A `return` inside a block targets the enclosing METHOD (non-local).
         * Find it (method_mid != 0), counting intervening block frames as the
         * env depth; depth 0 (method top-level or no method) → plain return. */
        struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        /* A lambda body is a return boundary of its own, so the walk stops there
         * as it does at a `def`. */
        while (mf->method_mid == 0 && !mf->lambda_body && mf->prev &&
               !mf->class_name_sym && !mf->anon_class_body) { mf = mf->prev; depth++; }
        /* No method: the top-level program frame IS the home (`proc { return }` at
         * top level ends the file), but a class/module body is not — a `return`
         * that reaches one has nowhere to go (LocalJumpError). */
        if (mf->method_mid == 0 && (mf->class_name_sym || mf->anon_class_body)) depth = 0;
        else if (mf->method_mid == 0 && !mf->lambda_body && depth == 0 && rn->arguments)   /* bare top-level return yields no value (CRuby warns only there) */
            fprintf(stderr, "%s: warning: argument of top-level return is ignored\n", tc->fname);
        if (depth == 0) {
            return ALLOC_node_return(kp_jump_args_value(tc, rn->arguments));
        }
        /* An escaped Proc must materialize enough of the env chain for
         * korb_outer_frame_base to reach the home method frame at run time —
         * the same requirement a depth-`depth` variable read has. */
        kp_note_depth(tc, depth);
        /* node_return_outer reads this block frame's EP cell (the frame top) and
         * needs its base too, to recognize an env this very frame owns. */
        NODE *v = kp_jump_args_value(tc, rn->arguments);
        NODE *ro = ALLOC_node_return_outer(-2 - tc->chain, depth, v);
        bake_add(tc, &ro->u.node_return_outer.prev_off);
        return ro;
      }

      /* ---- blocks ---- */
      case PM_DEFINED_NODE: {
        const pm_node_t *v = ((const pm_defined_node_t *)node)->value;
        /* `defined?((expr))` — unwrap parentheses; defined? applies to the last
         * statement inside (so `defined?((a, b = 1, 2))` sees the multi-assign). */
        while (PM_NODE_TYPE_P(v, PM_PARENTHESES_NODE)) {
            const pm_parentheses_node_t *pn = (const pm_parentheses_node_t *)v;
            if (pn->body == NULL || !PM_NODE_TYPE_P(pn->body, PM_STATEMENTS_NODE)) break;
            const pm_statements_node_t *st = (const pm_statements_node_t *)pn->body;
            if (st->body.size == 0) break;
            v = st->body.nodes[st->body.size - 1];
        }
        const int32_t self_off = -1 - tc->chain;
        if (PM_NODE_TYPE_P(v, PM_LOCAL_VARIABLE_READ_NODE) || PM_NODE_TYPE_P(v, PM_IT_LOCAL_VARIABLE_READ_NODE))
            return ALLOC_node_defined(4, 0, 0, INT32_MIN);                          /* "local-variable" */
        if (PM_NODE_TYPE_P(v, PM_CONSTANT_READ_NODE))
            /* self_off: inside a module body self is the module, which is where a
             * pending autoload for this name would be registered */
            { NODE *_d = ALLOC_node_defined(1, kp_intern_cid(tc, ((const pm_constant_read_node_t *)v)->name), self_off,
                                             -4 - tc->chain);
              bake_add(tc, &_d->u.node_defined.self_off); bake_add(tc, &_d->u.node_defined.dc_off); return _d; }
        if (PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_NODE)) {             /* `A::B` */
            const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)v;
            if (cp->parent && PM_NODE_TYPE_P(cp->parent, PM_CONSTANT_READ_NODE))   /* static `A::B` → owner-aware check */
                return ALLOC_node_defined_cpath(kp_intern_cid(tc, ((const pm_constant_read_node_t *)cp->parent)->name),
                                                kp_intern_cid(tc, cp->name));
            if (cp->parent != NULL)                                  /* nested/dynamic parent → evaluate it, then probe */
                return ALLOC_node_defined_cpath_dyn(transduce(tc, cp->parent), kp_intern_cid(tc, cp->name));
            /* `defined?(::TOP)` is `defined?(Object::TOP)` — owner-aware, so a
             * constant private to Object answers nil */
            return ALLOC_node_defined_cpath(korb_intern(tc->c->vm, "Object", 6), kp_intern_cid(tc, cp->name));
        }
        if (PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_READ_NODE))
            { NODE *_d = ALLOC_node_defined(2, kp_intern_cid(tc, ((const pm_instance_variable_read_node_t *)v)->name), self_off, INT32_MIN); bake_add(tc, &_d->u.node_defined.self_off); return _d; }
        if (PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_READ_NODE)) {
            const uint32_t gn = kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)v)->name);
            if (gn == korb_intern(tc->c->vm, "$~", 2) ||        /* always-defined specials */
                gn == korb_intern(tc->c->vm, "$!", 2) ||
                gn == korb_intern(tc->c->vm, "$@", 2))
                return ALLOC_node_defined(12, 200, 0, INT32_MIN);
            return ALLOC_node_defined(3, gn, 0, INT32_MIN);
        }
        if (PM_NODE_TYPE_P(v, PM_NUMBERED_REFERENCE_READ_NODE))          /* $1..$9 → gv iff that group matched */
            return ALLOC_node_defined(12, 100u + ((const pm_numbered_reference_read_node_t *)v)->number, 0, INT32_MIN);
        if (PM_NODE_TYPE_P(v, PM_BACK_REFERENCE_READ_NODE)) {            /* $& $` $' $+ → gv iff non-nil */
            const uint32_t nm = kp_intern_cid(tc, ((const pm_back_reference_read_node_t *)v)->name);
            uint32_t rk = 0;
            if      (nm == korb_intern(tc->c->vm, "$`", 2)) rk = 1;
            else if (nm == korb_intern(tc->c->vm, "$'", 2)) rk = 2;
            else if (nm == korb_intern(tc->c->vm, "$+", 2)) rk = 3;
            return ALLOC_node_defined(12, rk, 0, INT32_MIN);
        }
        if (PM_NODE_TYPE_P(v, PM_CALL_NODE)) {                           /* method call */
            const pm_call_node_t *cn = (const pm_call_node_t *)v;
            if (cn->receiver != NULL) {                                  /* recv.meth: eval recv, check it responds */
                if (PM_NODE_TYPE_P(cn->receiver, PM_SELF_NODE)) {         /* self.meth sees private/protected too */
                    NODE *_d = ALLOC_node_defined(0, kp_intern_cid(tc, cn->name), self_off, INT32_MIN); bake_add(tc, &_d->u.node_defined.self_off); return _d;
                }
                NODE *recv = transduce(tc, cn->receiver);
                NODE *_dc = ALLOC_node_defined_call(recv, kp_intern_cid(tc, cn->name), self_off);
                bake_add(tc, &_dc->u.node_defined_call.self_off);   /* the caller's self decides protected visibility */
                /* the RECEIVER must be defined too: `defined?($unset.foo)` and
                 * `defined?(!@unset)` are both nil in CRuby */
                if (PM_NODE_TYPE_P(cn->receiver, PM_GLOBAL_VARIABLE_READ_NODE) ||
                    PM_NODE_TYPE_P(cn->receiver, PM_INSTANCE_VARIABLE_READ_NODE) ||
                    PM_NODE_TYPE_P(cn->receiver, PM_CLASS_VARIABLE_READ_NODE)) {
                    pm_defined_node_t rdn = *(const pm_defined_node_t *)node;   /* reuse this node's shape */
                    rdn.value = (pm_node_t *)cn->receiver;
                    /* node_and stages nothing (slot_count 0), so both sides sit
                     * at this cursor — an extra chain hop here made the ivar /
                     * cvar receiver read the wrong self cell. */
                    NODE *rchk = transduce(tc, (const pm_node_t *)&rdn);
                    return ALLOC_node_and(rchk, _dc);
                }
                return _dc;
            }
            /* bareword (possibly with args): "method" iff self responds to it. */
            NODE *_d = ALLOC_node_defined(0, kp_intern_cid(tc, cn->name), self_off, INT32_MIN); bake_add(tc, &_d->u.node_defined.self_off); return _d;
        }
        if (PM_NODE_TYPE_P(v, PM_YIELD_NODE)) {   /* "yield" iff the enclosing METHOD got a block */
            struct kp_frame *mf = tc->frame;
            uint32_t depth = 0;
            while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
            if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }
            mf->uses_block = true;
            if (depth > 0) {   /* the block is in an outer frame: the escape must keep it */
                if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
                if (depth > tc->frame->yield_depth)   tc->frame->yield_depth = depth;
            }
            if (depth == 0) return ALLOC_node_defined_yield(-3 - tc->chain);
            NODE *dy = ALLOC_node_defined_yield_outer(-2 - tc->chain, depth, -3);
            bake_add(tc, &dy->u.node_defined_yield_outer.prev_off);
            add_bake_to(mf, &dy->u.node_defined_yield_outer.trio_base);
            return dy;
        }
        if (PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_READ_NODE)) {            /* "class variable" iff present */
            NODE *_d = ALLOC_node_defined_cvar(-1 - tc->chain, -4 - tc->chain,
                                               kp_intern_cid(tc, ((const pm_class_variable_read_node_t *)v)->name));
            bake_add(tc, &_d->u.node_defined_cvar.self_off);
            bake_add(tc, &_d->u.node_defined_cvar.dc_off);
            return _d;
        }
        if (PM_NODE_TYPE_P(v, PM_SUPER_NODE) || PM_NODE_TYPE_P(v, PM_FORWARDING_SUPER_NODE)) {
            /* "super" iff the enclosing method has an MRO successor.  The frame's
             * self and entry cell are read at the same offsets node_super_fwd uses. */
            uint32_t m_mid = tc->frame->method_mid;
            if (m_mid != 0) {
                NODE *_d = ALLOC_node_defined_super(m_mid, -1 - tc->chain, -4 - tc->chain);
                bake_add(tc, &_d->u.node_defined_super.self_off);   /* self at base[-1], entry at base[-4] */
                bake_add(tc, &_d->u.node_defined_super.dc_off);
                return _d;
            }
            {   /* in a block: the question is about the enclosing METHOD's frame */
                struct kp_frame *mf = tc->frame;
                uint32_t depth = 0;
                while (mf->method_mid == 0 && mf->prev && !mf->dm_body) { mf = mf->prev; depth++; }
                if (mf->dm_body) {   /* a define_method body IS a method, but its name is only known at run time */
                    NODE *_d = ALLOC_node_defined_super(korb_intern(tc->c->vm, "__dm_super__", 12),
                                                        -1 - tc->chain, -4 - tc->chain);
                    bake_add(tc, &_d->u.node_defined_super.self_off);
                    bake_add(tc, &_d->u.node_defined_super.dc_off);
                    return _d;
                }
                m_mid = mf->method_mid;
                if (m_mid == 0 || depth == 0) return ALLOC_node_lit(KORB_NIL);   /* top level */
                /* the outer method's entry is its base[-4], whatever its size */
                NODE *ds = ALLOC_node_defined_super_outer(m_mid, -2 - tc->chain, depth, KORB_ID_OFF);
                bake_add(tc, &ds->u.node_defined_super_outer.prev_off);
                return ds;
            }
        }
        if (PM_NODE_TYPE_P(v, PM_SELF_NODE))  return ALLOC_node_defined(6, 0, 0, INT32_MIN);   /* "self" */
        if (PM_NODE_TYPE_P(v, PM_NIL_NODE))   return ALLOC_node_defined(8, 0, 0, INT32_MIN);   /* "nil" */
        if (PM_NODE_TYPE_P(v, PM_TRUE_NODE))  return ALLOC_node_defined(9, 0, 0, INT32_MIN);   /* "true" */
        if (PM_NODE_TYPE_P(v, PM_FALSE_NODE)) return ALLOC_node_defined(10, 0, 0, INT32_MIN);  /* "false" */
        if (PM_NODE_TYPE_P(v, PM_LOCAL_VARIABLE_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_LOCAL_VARIABLE_AND_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_LOCAL_VARIABLE_OR_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_AND_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_OR_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CONSTANT_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_AND_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_OR_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_AND_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_OR_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_INDEX_OPERATOR_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_INDEX_OR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_INDEX_AND_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CONSTANT_AND_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CONSTANT_OR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CONSTANT_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_AND_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_OR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CALL_AND_WRITE_NODE) || PM_NODE_TYPE_P(v, PM_CALL_OR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_CALL_OPERATOR_WRITE_NODE) ||
            PM_NODE_TYPE_P(v, PM_MULTI_WRITE_NODE))
            return ALLOC_node_defined(11, 0, 0, INT32_MIN);                        /* "assignment" (not evaluated) */
        if (PM_NODE_TYPE_P(v, PM_ARRAY_NODE)) {
            /* `defined?([a, b])` is nil when ANY element is undefined: fold the
             * elements' own defined? checks with && and answer "expression". */
            const pm_array_node_t *an = (const pm_array_node_t *)v;
            NODE *chain = NULL;
            for (size_t i = 0; i < an->elements.size; i++) {
                const pm_node_t *el = an->elements.nodes[i];
                if (PM_NODE_TYPE_P(el, PM_SPLAT_NODE) || PM_NODE_TYPE_P(el, PM_KEYWORD_HASH_NODE)) { chain = NULL; break; }
                pm_defined_node_t dn = *(const pm_defined_node_t *)node;    /* reuse this node's shape */
                dn.value = (pm_node_t *)el;
                NODE *chk = transduce(tc, (const pm_node_t *)&dn);
                chain = chain ? ALLOC_node_and(chain, chk) : chk;
            }
            if (chain) {
                NODE *expr;
                WITH_CHAIN(tc, 1, (expr = ALLOC_node_defined(5, 0, 0, INT32_MIN)));
                return ALLOC_node_and(chain, expr);                         /* nil if any element is undefined */
            }
        }
        return ALLOC_node_defined(5, 0, 0, INT32_MIN);                             /* literals / expr → "expression" */
      }
      case PM_YIELD_NODE: {
        const pm_yield_node_t *yn = (const pm_yield_node_t *)node;
        uint32_t line = kp_line(tc, node);
        size_t yargc = yn->arguments ? yn->arguments->arguments.size : 0;
        /* A class/module/`class << obj` body ends the method scope, so a `yield`
         * that reaches one has no block to name.  prism skips this check while
         * parsing an eval (parsing_eval), which is exactly where it still bites. */
        for (const struct kp_frame *f = tc->frame; f && f->method_mid == 0; f = f->prev) {
            if (!f->class_name_sym && !f->anon_class_body) continue;
            if (tc->syntax_err == NULL) {
                static char ybuf[512];
                snprintf(ybuf, sizeof ybuf, "%s:%u: Invalid yield", tc->fname, line);
                tc->syntax_err = ybuf;
            }
            tc->syntax_error = true;
            break;
        }
        /* `yield` reaches the enclosing METHOD's block, not a nested block's.
         * Walk up to the method frame (method_mid != 0), counting block frames. */
        struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
        if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }  /* yield outside a method: legacy path (raises at runtime) */
        mf->uses_block = true;                   /* the method reserves the block trio */
        if (depth > 0) {   /* the block is in an outer frame: the escape must keep it */
            if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
            if (depth > tc->frame->yield_depth)   tc->frame->yield_depth = depth;
        }

        /* `yield(*arr)` / `yield a, *b` — build the args Array and spread it at
         * the block call (one staged child = the array). */
        bool y_splat = false;
        for (size_t i = 0; i < yargc; i++)
            if (PM_NODE_TYPE_P(yn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { y_splat = true; break; }
        if (y_splat) {
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, yn->arguments->arguments.nodes, yargc, (uint32_t)yargc)));
            if (depth == 0)
                return KP_LINK(tc, node_yield_splat, kp_flink_at(tc, line, 1), -3 - (tc->chain + 1), -2 - (tc->chain + 1), -1 - (tc->chain + 1), arr);
            NODE *yo = ALLOC_node_yield_outer_splat(line, -2 - (tc->chain + 1), depth, -3, arr);
            bake_add(tc, &yo->u.node_yield_outer_splat.prev_off);
            add_bake_to(mf, &yo->u.node_yield_outer_splat.trio_base);
            return yo;
        }

        if (depth == 0) {                        /* yield at method top-level: read this frame's trio */
            if (yargc == 0)
                return KP_LINK(tc, node_yield0, kp_flink_at(tc, line, 0), -3 - tc->chain, -2 - tc->chain, -1 - tc->chain);
            if (yargc == 1) {
                NODE *a0;
                WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
                return KP_LINK(tc, node_yield1, kp_flink_at(tc, line, 1), -3 - (tc->chain + 1), -2 - (tc->chain + 1), -1 - (tc->chain + 1), a0);
            }
            NODE **argv = malloc(sizeof(NODE *) * yargc);          /* yield a, b, ... */
            if (!argv) abort();
            const int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)yargc;
            for (size_t i = 0; i < yargc; i++) argv[i] = transduce(tc, yn->arguments->arguments.nodes[i]);
            tc->chain = saved;
            const int32_t off = tc->chain + (int32_t)yargc;
            return KP_LINK(tc, node_yield_n, kp_flink_at(tc, line, (uint32_t)yargc), -3 - off, -2 - off, -1 - off, argv, (uint32_t)yargc);
        }
        /* yield inside a block: trio_base = method frame_size - 3 (add-baked at
         * the method's pop); prev_off addresses this block frame's EP (base[-2]). */
        if (yargc == 0) {
            NODE *yo = ALLOC_node_yield_outer0(line, -2 - tc->chain, depth, -3);
            bake_add(tc, &yo->u.node_yield_outer0.prev_off);     /* this-block fixup */
            add_bake_to(mf, &yo->u.node_yield_outer0.trio_base); /* += method frame_size */
            return yo;
        }
        if (yargc == 1) {
            NODE *a0;
            WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
            NODE *yo = ALLOC_node_yield_outer1(line, -2 - (tc->chain + 1), depth, -3, a0);
            bake_add(tc, &yo->u.node_yield_outer1.prev_off);
            add_bake_to(mf, &yo->u.node_yield_outer1.trio_base);
            return yo;
        }
        NODE **argv = malloc(sizeof(NODE *) * yargc);              /* yield a, b, ... inside a block */
        if (!argv) abort();
        const int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)yargc;
        for (size_t i = 0; i < yargc; i++) argv[i] = transduce(tc, yn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        NODE *yo = ALLOC_node_yield_outer_n(line, -2 - (tc->chain + (int32_t)yargc), depth, -3, argv, (uint32_t)yargc);
        bake_add(tc, &yo->u.node_yield_outer_n.prev_off);
        add_bake_to(mf, &yo->u.node_yield_outer_n.trio_base);
        return yo;
      }
      case PM_NEXT_NODE: {
        const pm_next_node_t *nn = (const pm_next_node_t *)node;
        return ALLOC_node_next(kp_jump_args_value(tc, nn->arguments));   /* `next a, b` → [a, b] */
      }
      case PM_BREAK_NODE: {
        const pm_break_node_t *bn = (const pm_break_node_t *)node;
        return ALLOC_node_break(kp_jump_args_value(tc, bn->arguments));   /* `break a, b` → [a, b] */
      }
      case PM_REDO_NODE:  return ALLOC_node_redo();      /* re-run the block / loop body */
      case PM_RETRY_NODE:                               /* `retry` in a rescue → re-run the begin body */
        return ALLOC_node_retry();

      /* ---- calls / def ---- */
      case PM_CALL_NODE:
        return transduce_call(tc, (const pm_call_node_t *)node);
      case PM_DEF_NODE:
        kp_needs_definee(tc);
        return transduce_def_recv(tc, (const pm_def_node_t *)node, NULL);
      case PM_SINGLETON_CLASS_NODE: {     /* `class << recv; body; end` — run body with self = recv's singleton class */
        const pm_singleton_class_node_t *sc = (const pm_singleton_class_node_t *)node;
        kp_needs_definee(tc);
        /* recv expression evaluated in the ENCLOSING scope → node_sclass's staged child */
        NODE *recv_node;
        WITH_CHAIN(tc, 1, (recv_node = transduce(tc, sc->expression)));
        push_frame(tc, &sc->locals);
        tc->frame->anon_class_body = true;        /* self IS the (unnamed) singleton class here */
        const uint32_t blabel = tc->frame->body_label = kp_body_label(tc, 's', 0);
        NODE *body;
        if (sc->body == NULL)
            body = lit_nil();
        else if (PM_NODE_TYPE_P(sc->body, PM_STATEMENTS_NODE))
            body = transduce_statements(tc, (const pm_statements_node_t *)sc->body);
        else
            body = transduce(tc, sc->body);   /* a begin/rescue/ensure body is just another node */
        void *const linfo = build_locals_info(tc);   /* before pop_frame: it frees the frame */
        uint32_t frame_size = pop_frame(tc);
        NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, 0, -1, NULL, 0, NULL, NULL, linfo, -1, 0, -1, 0, blabel, 0, 0, 0);
        code_repo_add("sclass", entry, true);       /* its own AOT entry */
        korb_reg_srcloc(tc->c->vm, entry, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, node));   /* see transduce_class */
        NODE *_sc = KP_LINK(tc, node_sclass, kp_flink_at(tc, kp_line(tc, node), 1), entry, -1 - tc->chain - 1, -4 - tc->chain - 1, recv_node);   /* -1 extra for the staged recv child */
        bake_add(tc, &_sc->u.node_sclass.self_off);
        bake_add(tc, &_sc->u.node_sclass.dc_off);
        return _sc;
      }
      case PM_CLASS_NODE:
        kp_needs_definee(tc);
        return transduce_class(tc, (const pm_class_node_t *)node);
      case PM_MODULE_NODE:
        kp_needs_definee(tc);
        return transduce_module(tc, (const pm_module_node_t *)node);
      case PM_CONSTANT_READ_NODE: {
        const pm_constant_read_node_t *cr = (const pm_constant_read_node_t *)node;
        NODE *cn = build_const_read(tc, kp_intern_cid(tc, cr->name), kp_line(tc, node));
        return cn;
      }
      case PM_CONSTANT_PATH_NODE: {       /* `A::B` — resolve B owned by the parent
                                           * namespace A (the parent's rightmost name,
                                           * resolved at runtime); so M::C finds M's C. */
        const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)node;
        if (cp->parent != NULL) {                    /* `A::B` — resolve the parent as an expression */
            NODE *par = transduce(tc, cp->parent);
            return KP_CONST_PATH(tc, kp_intern_cid(tc, cp->name), kp_line(tc, node), par);
        }
        /* `::TOP` is `Object::TOP`: routing it through the owner-aware node is
         * what makes private_constant on Object bite. */
        return KP_CONST_PATH(tc, kp_intern_cid(tc, cp->name), kp_line(tc, node),
                             KP_CONST(tc, korb_intern(tc->c->vm, "Object", 6), 0, INT32_MIN, INT32_MIN, 0));
      }

      /* Global variables `$x` reuse the flat const table — the `$` in the name
       * keeps them in a distinct namespace from constants.  Unset reads → nil. */
      case PM_GLOBAL_VARIABLE_READ_NODE: {
        const uint32_t gn = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)node)->name));
        if (gn == korb_intern(tc->c->vm, "$!", 2)) return ALLOC_node_errinfo();   /* $! = current exception */
        if (gn == korb_intern(tc->c->vm, "$@", 2)) return ALLOC_node_errinfo_bt(0);   /* $@ = $!.backtrace */
        if (gn == korb_intern(tc->c->vm, "$~", 2)) kp_note_backref(tc);           /* reading it starts the frame at nil */
        if (gn == korb_intern(tc->c->vm, "$&", 2)) { kp_note_backref(tc); return ALLOC_node_backref(0); }  /* alias 経由 ($MATCH 等) */
        if (gn == korb_intern(tc->c->vm, "$`", 2)) { kp_note_backref(tc); return ALLOC_node_backref(1); }
        if (gn == korb_intern(tc->c->vm, "$'", 2)) { kp_note_backref(tc); return ALLOC_node_backref(2); }
        if (gn == korb_intern(tc->c->vm, "$+", 2)) { kp_note_backref(tc); return ALLOC_node_backref(3); }
        if (gn == korb_intern(tc->c->vm, "$=", 2))                /* removed: warns, always false */
            return ALLOC_node_dollar_eq(0, ALLOC_node_lit(KORB_FALSE));
        return KP_CONST(tc, gn, 0, INT32_MIN, INT32_MIN, 0);
      }
      case PM_NUMBERED_REFERENCE_READ_NODE: {   /* $1..$9 → group n of the last match ($~) */
        const uint32_t num = ((const pm_numbered_reference_read_node_t *)node)->number;
        kp_note_backref(tc);
        return ALLOC_node_backref(100u + num);
      }
      case PM_BACK_REFERENCE_READ_NODE: {       /* $& / $` / $' / $+ */
        kp_note_backref(tc);
        const uint32_t nm = kp_intern_cid(tc, ((const pm_back_reference_read_node_t *)node)->name);
        uint32_t kind = 0;
        if      (nm == korb_intern(tc->c->vm, "$&", 2)) kind = 0;
        else if (nm == korb_intern(tc->c->vm, "$`", 2)) kind = 1;
        else if (nm == korb_intern(tc->c->vm, "$'", 2)) kind = 2;
        else if (nm == korb_intern(tc->c->vm, "$+", 2)) kind = 3;
        return ALLOC_node_backref(kind);
      }
      case PM_GLOBAL_VARIABLE_WRITE_NODE: {
        { NODE *ro = kp_gvar_readonly_write(tc, node, ((const pm_global_variable_write_node_t *)node)->name); if (ro) return ro; }
        if ((kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, ((const pm_global_variable_write_node_t *)node)->name))
            == korb_intern(tc->c->vm, "$~", 2)) kp_note_backref(tc);   /* `$~ = v` is frame-local too */
        if ((kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, ((const pm_global_variable_write_node_t *)node)->name))
            == korb_intern(tc->c->vm, "$@", 2)) {                /* `$@ = v` → $!.set_backtrace(v) */
            /* the value is an @child evaluated at this node's own cursor
             * (slot_count 0), so the chain must NOT shift */
            NODE *v = transduce(tc, ((const pm_global_variable_write_node_t *)node)->value);
            return ALLOC_node_errinfo_bt_set(v);
        }
        const pm_global_variable_write_node_t *gw = (const pm_global_variable_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
        if (strcmp(korb_sym_name(tc->c->vm, name), "$=") == 0)   /* write is warned and dropped */
            return ALLOC_node_dollar_eq(1, transduce(tc, gw->value));
        {   /* deprecated separator globals warn (verbose) on a non-nil write */
            static const char *const dep[] = { "$/", "$-0", "$\\", "$,", "$;", "$-F", "$=", NULL };
            const char *const rn = korb_sym_name(tc->c->vm, name);
            for (uint32_t i = 0; dep[i]; i++) {
                if (strcmp(rn, dep[i]) != 0) continue;
                const char *const written = kp_cid_cstr(tc, gw->name);
                char *msgname = malloc(strlen(written) + 1);   /* immortal (baked) */
                if (!msgname) abort();
                strcpy(msgname, written);
                NODE *v2, *warn;
                /* node_const_set stages its value child; the warn node stages
                 * ITS child one deeper (its own slot_count on top). */
                WITH_CHAIN(tc, kind_node_const_set.slot_count,
                           ({ NODE *inner = transduce(tc, gw->value);   /* @child: evaluated at this cursor */
                              warn = ALLOC_node_deprecated_gvar_warn(msgname, inner); v2 = warn; }));
                (void)v2;
                return build_const_set(tc, name, warn);
            }
        }
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        {   /* a few globals normalize what they store */
            const char *const rn = korb_sym_name(tc->c->vm, name);
            if (strcmp(rn, "$VERBOSE") == 0 || strcmp(rn, "$DEBUG") == 0) val = ALLOC_node_gvar_coerce(0, val);
            else if (strcmp(rn, "$.") == 0)                               val = ALLOC_node_gvar_coerce(1, val);
            else if (strcmp(rn, "$0") == 0)                               val = ALLOC_node_gvar_coerce(2, val);
        }
        return build_const_set(tc, name, val);
      }
      case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {     /* `$x op= v` */
        const pm_global_variable_operator_write_node_t *gw =
            (const pm_global_variable_operator_write_node_t *)node;
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, gw->binary_operator));
        if (op == KP_BINOP_NONE) return kp_unsupported(tc, node, "global operator-assign");
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name)), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = KP_CONST(tc, name, 0, INT32_MIN, INT32_MIN, kp_line(tc, node)), rhs = transduce(tc, gw->value)));
            alloc_binop(tc, op, lhs, rhs, line);
        }));
        return build_const_set(tc, name, binop);
      }
      case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {          /* `$x ||= v` (globals live in the const table) */
        const pm_global_variable_or_write_node_t *gw = (const pm_global_variable_or_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        return ALLOC_node_or(KP_CONST(tc, name, 0, INT32_MIN, INT32_MIN, kp_line(tc, node)), build_const_set(tc, name, val));
      }
      case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: {         /* `$x &&= v` */
        const pm_global_variable_and_write_node_t *gw = (const pm_global_variable_and_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        return ALLOC_node_and(KP_CONST(tc, name, 0, INT32_MIN, INT32_MIN, kp_line(tc, node)), build_const_set(tc, name, val));
      }

      case PM_CONSTANT_WRITE_NODE: {     /* `FOO = expr` → VM const table */
        const pm_constant_write_node_t *cw = (const pm_constant_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, cw->name);
        NODE *val;
        uint32_t sc = kind_node_const_set.slot_count;
        WITH_CHAIN(tc, sc, (val = transduce(tc, cw->value)));
        return build_const_set_at(tc, name, val, node);
      }
      case PM_CONSTANT_OR_WRITE_NODE: {  /* `X ||= v` — assign when undefined or falsy (the read never raises) */
        const pm_constant_or_write_node_t *ow = (const pm_constant_or_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->name);
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, ow->value)));
        NODE *set = build_const_set(tc, name, val);
        /* `defined?(X) && X || (X = v)`: the `&&` short-circuits before the read
         * when X is undefined (no NameError); a truthy X is kept, else assign. */
        NODE *dfn = ALLOC_node_defined(1, name, -1 - tc->chain, -4 - tc->chain);
        bake_add(tc, &dfn->u.node_defined.self_off); bake_add(tc, &dfn->u.node_defined.dc_off);
        NODE *guarded = ALLOC_node_and(dfn, build_const_read(tc, name, kp_line(tc, node)));
        return ALLOC_node_or(guarded, set);
      }
      case PM_CONSTANT_AND_WRITE_NODE: { /* `X &&= v` — read (raises if undefined), assign when truthy */
        const pm_constant_and_write_node_t *aw = (const pm_constant_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->name);
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, aw->value)));
        NODE *set = build_const_set(tc, name, val);
        return ALLOC_node_and(build_const_read(tc, name, kp_line(tc, node)), set);
      }
      case PM_CONSTANT_OPERATOR_WRITE_NODE: {  /* `X op= v` → X = X op v (read raises if undefined) */
        const pm_constant_operator_write_node_t *ow = (const pm_constant_operator_write_node_t *)node;
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, ow->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t name = kp_intern_cid(tc, ow->name), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = build_const_read(tc, name, kp_line(tc, node)), rhs = transduce(tc, ow->value)));
            (op != KP_BINOP_NONE) ? alloc_binop(tc, op, lhs, rhs, line) : kp_send1(tc, opmid, line, lhs, rhs);
        }));
        return build_const_set(tc, name, binop);
      }
      case PM_CONSTANT_PATH_WRITE_NODE: {   /* `A::B = expr` — flat const table → rightmost name,
                                             * owner = the path's parent (`A`) so B nests under A. */
        const pm_constant_path_write_node_t *cpw = (const pm_constant_path_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, cpw->target->name);
        uint32_t owner_name = tc->frame->class_name_sym;
        const pm_node_t *const parent = cpw->target->parent;
        if (parent && PM_NODE_TYPE_P(parent, PM_CONSTANT_READ_NODE))
            owner_name = kp_intern_cid(tc, ((const pm_constant_read_node_t *)parent)->name);
        else if (parent && PM_NODE_TYPE_P(parent, PM_CONSTANT_PATH_NODE) && kp_const_path_static_p(parent)) {
            /* The WHOLE parent path, not just its rightmost name: the owner is
             * resolved by a flat by-name lookup, and a same-named module in
             * another namespace answers to it first (rspec lazily loads
             * diff/lcs/block.rb, and `Diff::LCS::Block = …` landed in a stray
             * `Dotenv::Diff::LCS`).  korb_cref_step walks the dotted form. */
            owner_name = kp_intern_cpath(tc, parent);
            if (owner_name == 0) owner_name = kp_intern_cid(tc, ((const pm_constant_path_node_t *)parent)->name);
        }
        else if (parent) {   /* dynamic owner (`expr::N = v`, e.g. a local holding an anon module) → expr.const_set(:N, v) so N nests under the runtime owner */
            const uint32_t line = kp_line(tc, node);
            NODE *r, *k, *v;
            WITH_CHAIN(tc, KP_SEND2_SC, (r = transduce(tc, parent),
                                         k = ALLOC_node_lit(ID2SYM(name)),
                                         v = transduce(tc, cpw->value)));
            return kp_send2(tc, korb_intern(tc->c->vm, "const_set", 9), line, r, k, v);
        }
        NODE *val;
        uint32_t sc = kind_node_const_set.slot_count;
        WITH_CHAIN(tc, sc, (val = transduce(tc, cpw->value)));
        NODE *cn = ALLOC_node_const_set(name, owner_name, INT32_MIN, val);
        korb_reg_srcloc(tc->c->vm, cn, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, node));   /* Module#const_source_location */
        return cn;
      }
      case PM_CONSTANT_PATH_OR_WRITE_NODE: {   /* `A::B ||= v` (static owner) */
        const pm_constant_path_or_write_node_t *ow = (const pm_constant_path_or_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->target->name), owner;
        if (!const_path_static_owner(tc, ow->target->parent, tc->frame->class_name_sym, &owner)) {
            if (ow->target->parent == NULL) return kp_unsupported(tc, node, "::CONST ||= at top level");
            return kp_dyn_const_opassign(tc, node, ow->target->parent, name, ow->value, true);
        }
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, ow->value)));
        NODE *set = ALLOC_node_const_set(name, owner, INT32_MIN, val);
        /* guard the read with a flat defined? probe of the rightmost name (no NameError) */
        NODE *dfn = ALLOC_node_defined(1, name, -1 - tc->chain, -4 - tc->chain);
        bake_add(tc, &dfn->u.node_defined.self_off); bake_add(tc, &dfn->u.node_defined.dc_off);
        NODE *guarded = ALLOC_node_and(dfn, KP_CONST(tc, name, owner, INT32_MIN, INT32_MIN, kp_line(tc, node)));
        return ALLOC_node_or(guarded, set);
      }
      case PM_CONSTANT_PATH_AND_WRITE_NODE: {  /* `A::B &&= v` (static owner) — read raises if undefined */
        const pm_constant_path_and_write_node_t *aw = (const pm_constant_path_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->target->name), owner;
        if (!const_path_static_owner(tc, aw->target->parent, tc->frame->class_name_sym, &owner)) {
            if (aw->target->parent == NULL) return kp_unsupported(tc, node, "::CONST &&= at top level");
            return kp_dyn_const_opassign(tc, node, aw->target->parent, name, aw->value, false);
        }
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, aw->value)));
        NODE *set = ALLOC_node_const_set(name, owner, INT32_MIN, val);
        return ALLOC_node_and(KP_CONST(tc, name, owner, INT32_MIN, INT32_MIN, kp_line(tc, node)), set);
      }
      case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {  /* `A::B op= v` (static owner) → A::B = A::B op v */
        const pm_constant_path_operator_write_node_t *ow = (const pm_constant_path_operator_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->target->name), owner;
        if (!const_path_static_owner(tc, ow->target->parent, tc->frame->class_name_sym, &owner)) {
            /* Dynamic module part: evaluate it ONCE into a synth local, then
             * mod.const_set(:N, mod.const_get(:N) op v). */
            if (ow->target->parent == NULL) return kp_unsupported(tc, node, "::CONST op= at top level");
            const uint32_t line2 = kp_line(tc, node);
            const uint32_t opmid2 = kp_intern_cid(tc, ow->binary_operator);
            const enum kp_binop op2 = kp_binop_kind(kp_cid_cstr(tc, ow->binary_operator));
            const uint32_t mtmp = alloc_synth_local(tc);
            NODE *const store = bake_lset(tc, mtmp, transduce(tc, ow->target->parent));
            NODE *r2, *k2, *v2;
            WITH_CHAIN(tc, KP_SEND2_SC, ({
                r2 = bake_lget(tc, mtmp);
                k2 = ALLOC_node_lit(ID2SYM(name));
                NODE *lhs2, *rhs2;
                WITH_CHAIN(tc, kind_node_plus.slot_count, ({
                    NODE *gr, *gk;
                    WITH_CHAIN(tc, KP_SEND1_SC, (gr = bake_lget(tc, mtmp), gk = ALLOC_node_lit(ID2SYM(name))));
                    lhs2 = kp_send1(tc, korb_intern(tc->c->vm, "const_get", 9), line2, gr, gk);
                    rhs2 = transduce(tc, ow->value);
                }));
                v2 = (op2 != KP_BINOP_NONE) ? alloc_binop(tc, op2, lhs2, rhs2, line2) : kp_send1(tc, opmid2, line2, lhs2, rhs2);
            }));
            NODE *const setn = kp_send2(tc, korb_intern(tc->c->vm, "const_set", 9), line2, r2, k2, v2);
            return ALLOC_node_seq(store, setn);
        }
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, ow->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = KP_CONST(tc, name, owner, INT32_MIN, INT32_MIN, kp_line(tc, node)), rhs = transduce(tc, ow->value)));
            (op != KP_BINOP_NONE) ? alloc_binop(tc, op, lhs, rhs, line) : kp_send1(tc, opmid, line, lhs, rhs);
        }));
        return ALLOC_node_const_set(name, owner, INT32_MIN, binop);
      }

      case PM_RESCUE_MODIFIER_NODE: {   /* `expr rescue fallback` (catch-all) */
        const pm_rescue_modifier_node_t *rm = (const pm_rescue_modifier_node_t *)node;
        NODE *body = transduce(tc, rm->expression);
        NODE *cls = KP_CONST(tc, korb_intern(tc->c->vm, "StandardError", 13), 0, INT32_MIN, INT32_MIN, 0);
        NODE *resc = transduce(tc, rm->rescue_expression);
        NODE *rescues = ALLOC_node_rescue(cls, resc, ALLOC_node_reraise(), 0, 0u, -tc->chain);  /* catch StandardError */
        bake_add(tc, &rescues->u.node_rescue.base_off);   /* base_off: this frame's own base (backtrace), as build_rescue_chain */
        return ALLOC_node_begin(body, rescues, lit_nil(), lit_nil(), 1u);
      }

      case PM_SUPER_NODE: {           /* super(...) — explicit args */
        const pm_super_node_t *sn = (const pm_super_node_t *)node;
        uint32_t m_mid = tc->frame->method_mid;
        if (m_mid == 0) {
            /* Walk out: a define_method body is a method in its own right (its
             * name is only known at run time — bake a sentinel), while a plain
             * block borrows the name of the def it sits in. */
            for (const struct kp_frame *f = tc->frame; f && m_mid == 0; f = f->prev) {
                if (f->dm_body) break;
                m_mid = f->method_mid;
            }
            if (m_mid == 0) m_mid = korb_intern(tc->c->vm, "__dm_super__", 12);
        }
        uint32_t line = kp_line(tc, node);
        const pm_arguments_node_t *args = sn->arguments;
        size_t argc = args ? args->arguments.size : 0;
        /* `super(args) { block }` — build the args Array + thread the literal block.
         * Staged like node_super_splat (one array child); def_env_off = self_off+1. */
        if (sn->block && PM_NODE_TYPE_P(sn->block, PM_BLOCK_ARGUMENT_NODE) &&
            (((const pm_block_argument_node_t *)sn->block)->expression == NULL ||
             !PM_NODE_TYPE_P(((const pm_block_argument_node_t *)sn->block)->expression, PM_SYMBOL_NODE))) {
            /* `super(args, &blk)` / `&nil` / bare `&` — evaluate the block
             * expression into a rooted synth local; the node #to_proc's it. */
            const pm_block_argument_node_t *ba = (const pm_block_argument_node_t *)sn->block;
            const bool anon_blk = (ba->expression == NULL);
            uint32_t anon_d = 0; int32_t anon_s = -1; NODE *anon_pre = NULL;
            if (anon_blk && !kp_anon_find(tc, KP_ANON_BLK, &anon_d, &anon_s))
                return kp_unsupported(tc, node, "bare & outside a (&) method body");
            uint32_t pslot = anon_blk ? (uint32_t)kp_fwd_blk_local(tc, anon_d, anon_s, &anon_pre)
                                      : alloc_synth_local(tc);
            NODE *pset = anon_blk ? anon_pre : bake_lset(tc, pslot, transduce(tc, ba->expression));
            int32_t soff = -1 - tc->chain - 1, dco = -4 - tc->chain - 1;
            int32_t poff = (int32_t)pslot - tc->chain - 1;
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args ? args->arguments.nodes : NULL, argc, (uint32_t)argc)));
            NODE *_s = ALLOC_node_super_blkproc(m_mid, line, soff, dco, poff, arr);
            bake_add(tc, &_s->u.node_super_blkproc.self_off);
            bake_add(tc, &_s->u.node_super_blkproc.dc_off);
            bake_add(tc, &_s->u.node_super_blkproc.proc_off);
            return pset ? ALLOC_node_seq(pset, _s) : _s;
        }
        if (sn->block) {
            NODE *bentry = kp_block_entry(tc, sn->block);
            if (!bentry) return kp_unsupported(tc, node, "super with a non-literal block");
            int32_t soff = -1 - tc->chain - 1, dco = -4 - tc->chain - 1, deo = -tc->chain - 1;
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args ? args->arguments.nodes : NULL, argc, (uint32_t)argc)));
            NODE *_s = ALLOC_node_super_blk(m_mid, line, soff, dco, bentry, deo, arr);
            bake_add(tc, &_s->u.node_super_blk.self_off);
            bake_add(tc, &_s->u.node_super_blk.dc_off);
            bake_add(tc, &_s->u.node_super_blk.def_env_off);
            return _s;
        }
        /* `super(...)` — the enclosing `def m(...)` already collected the positional
         * args into its synth rest local; the block rides the frame's block cells,
         * which super_fwd forwards anyway. */
        if (argc >= 1 && PM_NODE_TYPE_P(args->arguments.nodes[argc - 1], PM_FORWARDING_ARGUMENTS_NODE)) {
            uint32_t fdep; int32_t fslot, fblk;
            if (!kp_fwd_find(tc, &fdep, &fslot, &fblk))
                return kp_unsupported(tc, node, "... forwarding outside a (...) method body");
            NODE *farr;
            WITH_CHAIN(tc, 1, (farr = (argc == 1)
                ? kp_fwd_args(tc, fdep, fslot)
                : build_array_with_fwd(tc, args->arguments.nodes, argc - 1, fdep, fslot)));
            return emit_super_fwd(tc, m_mid, line, farr);
        }
        /* `super(args)` with no literal block — build the args Array and forward
         * the current method's incoming block (super_fwd). */
        NODE *arr;
        WITH_CHAIN(tc, 1, (arr = build_array(tc, args ? args->arguments.nodes : NULL, argc, (uint32_t)argc)));
        return emit_super_fwd(tc, m_mid, line, arr);
      }
      case PM_FORWARDING_SUPER_NODE: {   /* bare super — forward the method's params */
        const pm_forwarding_super_node_t *fn = (const pm_forwarding_super_node_t *)node;
        uint32_t m_mid = tc->frame->method_mid;
        /* CRuby refuses a bare `super` from a define_method body outright: the
         * block's params are not the method's, so there is nothing to forward. */
        if (m_mid == 0 && tc->frame->dm_body)   /* Ruby's own error, not an implementation gap */
            return ALLOC_node_runtime_error("implicit argument passing of super from method defined by "
                                            "define_method() is not supported. Specify all arguments explicitly.",
                                            kp_line(tc, node));
        /* The parameters to forward belong to the enclosing METHOD frame, which
         * may be `depth` scopes out when the `super` is written inside a block. */
        const struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        if (m_mid == 0) {   /* a plain block borrows the name of the def it sits in */
            for (const struct kp_frame *f = tc->frame; f && m_mid == 0; f = f->prev) {
                if (f->dm_body) break;
                m_mid = f->method_mid;
                mf = f;
                if (m_mid == 0) depth++;
            }
        }
        if (m_mid == 0) return kp_unsupported(tc, node, "super outside a method body");
        uint32_t line = kp_line(tc, node);
        /* `def m(...)` has no named params to walk: everything the caller passed
         * is already in the synth rest local, so a bare `super` forwards exactly
         * what `super(...)` would. */
        if (mf->fwd_slot >= 0) {
            uint32_t fdep; int32_t fslot, fblk;
            if (kp_fwd_find(tc, &fdep, &fslot, &fblk)) {
                NODE *farr;
                /* `def m(a, ...)` keeps `a` as a named param, so the synth rest
                 * holds only what follows it: forward [a, ..., *rest]. */
                const uint32_t lead = mf->method_params;
                WITH_CHAIN(tc, 1, (farr = lead ? build_fwd_args(tc, lead, fslot, 0, 0, NULL, lead + 1, fdep)
                                               : kp_fwd_args(tc, fdep, fslot)));
                if (fn->block) {
                    NODE *bentry = kp_block_entry(tc, (const pm_node_t *)fn->block);
                    if (!bentry) return kp_unsupported(tc, node, "super with a non-literal block");
                    const int32_t soff = -1 - tc->chain - 1, dco = -4 - tc->chain - 1, deo = -tc->chain - 1;
                    NODE *_sb = ALLOC_node_super_blk(m_mid, line, soff, dco, bentry, deo, farr);
                    bake_add(tc, &_sb->u.node_super_blk.self_off);
                    bake_add(tc, &_sb->u.node_super_blk.dc_off);
                    bake_add(tc, &_sb->u.node_super_blk.def_env_off);
                    return _sb;
                }
                return emit_super_fwd(tc, m_mid, line, farr);
            }
        }
        const uint32_t np = mf->method_params;
        const int32_t rest_slot = mf->method_rest_slot;
        const uint32_t pc = mf->method_post_cnt;
        const uint32_t pb = mf->method_post_base >= 0 ? (uint32_t)mf->method_post_base : 0;
        struct korb_kw_info *const kw = mf->method_kw_info;
        const uint32_t has_kw = (kw && (kw->count || kw->kwrest_slot >= 0)) ? 1u : 0u;
        /* bare `super` forwards the method's current args (positional + rest +
         * post + keyword) and its incoming block: build [pos..., *rest, post...,
         * {kwargs}] via super_fwd. */
        const uint32_t total = np + (rest_slot >= 0 ? 1u : 0u) + pc + has_kw;
        NODE *arr;
        WITH_CHAIN(tc, 1, (arr = build_fwd_args(tc, np, rest_slot, pb, pc, kw, total, depth)));
        if (fn->block) {                                  /* `super { ... }` — forwarded args + a literal block */
            NODE *bentry = kp_block_entry(tc, (const pm_node_t *)fn->block);
            if (!bentry) return kp_unsupported(tc, node, "super with a non-literal block");
            const int32_t soff = -1 - tc->chain - 1, dco = -4 - tc->chain - 1, deo = -tc->chain - 1;
            NODE *_s = ALLOC_node_super_blk(m_mid, line, soff, dco, bentry, deo, arr);
            bake_add(tc, &_s->u.node_super_blk.self_off);
            bake_add(tc, &_s->u.node_super_blk.dc_off);
            bake_add(tc, &_s->u.node_super_blk.def_env_off);
            return _s;
        }
        return emit_super_fwd(tc, m_mid, line, arr);
      }

      /* `# shareable_constant_value: literal` wraps the constant write.  koruby
       * has no Ractors, so shareability is a no-op — but the write itself must
       * still run (CRuby's time.rb carries the comment). */
      case PM_SHAREABLE_CONSTANT_NODE:
        return transduce(tc, ((const pm_shareable_constant_node_t *)node)->write);

      default: {
        char what[64];
        snprintf(what, sizeof(what), "syntax (prism node %d)", (int)PM_NODE_TYPE(node));
        return kp_unsupported(tc, node, strdup(what));
      }
    }
}

/* ---------------------------------------------------------------------- */

/* Keep the parser's first diagnostic (with the line it points at) for the
 * SyntaxError the eval path raises — "syntax error in eval string" alone tells
 * the program nothing. */
static void
kp_stash_syntax_msg(CTX *c, const pm_parser_t *parser, const char *fname)
{
    const pm_diagnostic_t *const d0 = (const pm_diagnostic_t *)parser->error_list.head;
    if (d0 == NULL) return;
    /* CRuby's shape (prism's own formatter):
     *   file:LINE: syntax error(s) found
     *   > LINE | <the source line>
     *          |    ^ <what went wrong>
     * Several diagnostics on one line share the source line. */
    static char buf[4096];
    size_t n = 0;
    /* snprintf returns what it WOULD have written, so `n +=` can run past the
     * buffer and make the next `sizeof buf - n` underflow to a huge size_t —
     * _FORTIFY_SOURCE then aborts the process.  A 16 KB source line in an eval
     * did exactly that.  Clamp on every append. */
    #define KP_MSG_APPEND(...) do {                                            \
        if (n + 1 < sizeof buf) {                                              \
            const int w_ = snprintf(buf + n, sizeof buf - n, __VA_ARGS__);     \
            if (w_ > 0) n = (n + (size_t)w_ < sizeof buf) ? n + (size_t)w_     \
                                                          : sizeof buf - 1;    \
        }                                                                      \
    } while (0)
    const int32_t line0 = pm_newline_list_line(&parser->newline_list, d0->location.start, parser->start_line);
    KP_MSG_APPEND("%s:%d: syntax error%s found\n",
                  fname ? fname : "(eval)", line0,
                  parser->error_list.size > 1 ? "s" : "");
    int32_t shown = -1;
    for (const pm_diagnostic_t *d = d0; d != NULL && n + 64 < sizeof buf;
         d = (const pm_diagnostic_t *)d->node.next) {
        const int32_t line = pm_newline_list_line(&parser->newline_list, d->location.start, parser->start_line);
        /* the line's text, without its newline */
        const size_t lidx = (size_t)(line - parser->start_line);
        const uint8_t *const lstart = (lidx < parser->newline_list.size)
                                    ? parser->start + parser->newline_list.offsets[lidx] : parser->start;
        const uint8_t *lend = lstart;
        while (lend < parser->end && *lend != '\n') lend++;
        const int gutter = snprintf(NULL, 0, "%d", line);
        if (line != shown) {
            size_t ltext = (size_t)(lend - lstart);       /* a long line would fill the whole buffer */
            if (ltext > 200) ltext = 200;
            KP_MSG_APPEND("> %d | %.*s\n", line, (int)ltext, (const char *)lstart);
            shown = line;
        }
        size_t col = (size_t)(d->location.start - lstart);
        if (col > 200) col = 200;
        KP_MSG_APPEND("%*s | ", gutter + 2, "");
        for (size_t k = 0; k < col && n + 2 < sizeof buf; k++) buf[n++] = ' ';
        size_t span = (size_t)(d->location.end - d->location.start);   /* ^~~~ covers the offending text */
        if (span > 80) span = 80;
        if (n + 2 < sizeof buf) buf[n++] = '^';
        for (size_t k = 1; k < span && n + 2 < sizeof buf; k++) buf[n++] = '~';
        KP_MSG_APPEND(" %s\n", d->message);
    }
    #undef KP_MSG_APPEND
    buf[n < sizeof buf ? n : sizeof buf - 1] = '\0';
    c->vm->last_syntax_msg = buf;
}

/* An explicit `# frozen_string_literal: false` opts the file out of chilled
 * literals; no comment at all means chilled (Ruby 3.4+).  prism only reports the
 * `true` case through PM_STRING_FLAGS_FROZEN, so scan the magic-comment area. */
/* koruby's own prelude stands in for CRuby's C implementation: mutating a
 * literal in there is an implementation detail no CRuby program can see, so it
 * is not chilled (`Marshal.dump` built its buffer with `out = ""; out << …` and
 * the chilled-literal warning reached the user under -w). */
static bool
kp_internal_source(const char *const fname)
{
    return fname != NULL && strcmp(fname, KORB_PRELUDE_FILE) == 0;
}
/* `want_true` picks which explicit value to report; both answer false when the
 * magic comment is absent. */
static bool
kp_fsl_is(const char *src, size_t len, bool want_true)
{
    const size_t scan = len < 4096 ? len : 4096;
    for (size_t i = 0; i + 24 < scan; i++) {
        if (src[i] != 'f' || memcmp(src + i, "frozen_string_literal", 21) != 0) continue;
        size_t j = i + 21;
        while (j < scan && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j < scan && src[j] == ':') j++;
        while (j < scan && (src[j] == ' ' || src[j] == '\t')) j++;
        if (want_true) return j + 4 <= scan && memcmp(src + j, "true", 4) == 0;
        return j + 5 <= scan && memcmp(src + j, "false", 5) == 0;
    }
    return false;
}
static bool kp_fsl_false(const char *src, size_t len) { return kp_fsl_is(src, len, false); }
static bool kp_fsl_true(const char *src, size_t len)  { return kp_fsl_is(src, len, true); }

NODE *
koruby_parse_source_at(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, bool exit_on_error,
                       const char *src_enc, uint32_t cref_name, const uint32_t *cref_chain, uint32_t cref_chain_len,
                       bool in_eval)
{
    pm_parser_t parser;
    pm_options_t options = { 0 };
    if (OPTION.frozen_literals) pm_options_frozen_string_literal_set(&options, true);   /* --enable-frozen-string-literal */
    pm_options_filepath_set(&options, fname);
    /* eval(str): the program starts in the STRING's encoding, which a magic
     * comment on the first line may then override (prism applies the same
     * first-line rule CRuby does).  NULL = the parser's own default (UTF-8). */
    if (src_enc) pm_options_encoding_set(&options, src_enc);
    pm_options_line_set(&options, first_line);

    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);

    if (parser.error_list.size > 0) {
        if (!exit_on_error) {                        /* eval(str): return NULL so the caller raises SyntaxError */
            kp_stash_syntax_msg(c, &parser, fname);         /* hand the parser's own wording to the SyntaxError */
            pm_node_destroy(&parser, root);
            pm_parser_free(&parser);
            pm_options_free(&options);
            return NULL;
        }
        /* main program: CRuby-compatible exit path — SyntaxError → stderr + exit 1.
         * Same block eval's SyntaxError#message carries, then the summary line
         * CRuby ends with. */
        kp_stash_syntax_msg(c, &parser, fname);
        const pm_diagnostic_t *const d0 = (const pm_diagnostic_t *)parser.error_list.head;
        const int32_t line0 = d0 ? pm_newline_list_line(&parser.newline_list, d0->location.start, parser.start_line) : 0;
        if (c->vm->last_syntax_msg) {   /* the snippet only: the summary line below says the rest */
            const char *const nl = strchr(c->vm->last_syntax_msg, '\n');
            fputs(nl ? nl + 1 : c->vm->last_syntax_msg, stderr);
        }
        fprintf(stderr, "%s:%d: syntax error%s found (SyntaxError)\n",
                fname, line0, parser.error_list.size > 1 ? "s" : "");
        exit(1);
    }

    struct kp_ctx tc = {
        .parser = &parser,
        .c = c,
        .fname = fname,
        .src_enc = kp_src_enc(c, &parser, src, len, src_enc),
        .fsl_false = kp_fsl_false(src, len) || kp_internal_source(fname),
        .fsl_true = kp_fsl_true(src, len) || OPTION.frozen_literals,
        .seed_class_name = cref_name,
        .seed_chain = cref_chain,
        .seed_chain_len = cref_chain_len,
        .in_eval = in_eval,
    };
    NODE *ast = transduce(&tc, root);
    if (ast == NULL) ast = lit_nil();
    for (uint32_t pi = tc.pre_cnt; pi-- > 0; ) ast = ALLOC_node_seq(tc.pre_list[pi], ast);   /* BEGIN { } first */
    free(tc.pre_list);
    free(tc.bake_list);
    free(tc.flink_list);
    if (tc.syntax_error) {                           /* transduce-time SyntaxError (e.g. binding in alternative pattern) */
        pm_node_destroy(&parser, root);
        pm_parser_free(&parser);
        pm_options_free(&options);
        if (!exit_on_error) {                        /* eval(str): caller raises SyntaxError */
            if (tc.syntax_err) c->vm->last_syntax_msg = tc.syntax_err;
            return NULL;
        }
        if (tc.syntax_err) fprintf(stderr, "%s\n", tc.syntax_err);
        fprintf(stderr, "%s: syntax error (SyntaxError)\n", fname);
        exit(1);
    }

    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    pm_options_free(&options);
    return ast;
}

/* eval/require with no explicit position: line numbering starts at 1. */
NODE *
koruby_parse_source(CTX *c, const char *src, size_t len, const char *fname, bool exit_on_error)
{
    return koruby_parse_source_at(c, src, len, fname, 1, exit_on_error, NULL, 0, NULL, 0, false);
}

/* Parse `src` for eval(str, binding): the binding's local names are declared as
 * the program's scope so bare references parse as (depth-0) locals.  Returns the
 * program AST; koruby_toplevel_locals_cnt / koruby_toplevel_local_{syms,cnt} are
 * set to the eval program's frame (caller seeds those slots from the binding and
 * writes them back).  NULL on syntax error. */
NODE *
koruby_parse_binding_eval(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, const char *src_enc,
                          const uint32_t *name_syms, uint32_t name_cnt,
                          const uint32_t *outer_names, const uint32_t *outer_cnt, uint32_t outer_levels,
                          uint32_t method_mid,
                          uint32_t cref_name, const uint32_t *cref_chain, uint32_t cref_chain_len,
                          bool cref_is_caller)
{
    pm_parser_t parser;
    pm_options_t options = { 0 };
    if (OPTION.frozen_literals) pm_options_frozen_string_literal_set(&options, true);   /* --enable-frozen-string-literal */
    pm_options_filepath_set(&options, fname);
    /* eval(str): the program starts in the STRING's encoding, which a magic
     * comment on the first line may then override (prism applies the same
     * first-line rule CRuby does).  NULL = the parser's own default (UTF-8). */
    if (src_enc) pm_options_encoding_set(&options, src_enc);
    pm_options_line_set(&options, first_line);
    /* The binding's levels become prism's OUTER scopes (outermost first), each
     * naming its env slots in index order, so a caller local resolves to depth >= 1
     * and reaches the real env.  A slot with no name (shadowed / implicit / synth)
     * gets a placeholder no source can spell.  The last scope is the program's
     * own: the names a previous eval on this binding added (`extra`). */
    pm_options_scopes_init(&options, outer_levels + 1);
    char (*dummy)[24] = NULL;
    uint32_t outer_total = 0;
    for (uint32_t v = 0; v < outer_levels; v++) outer_total += outer_cnt[v];
    if (outer_total) { dummy = malloc(sizeof *dummy * outer_total); if (!dummy) abort(); }
    for (uint32_t v = 0, off = 0; v < outer_levels; v++) {
        const uint32_t lv = outer_levels - 1 - v;                 /* scopes[0] = outermost level */
        uint32_t loff = 0;
        for (uint32_t k = 0; k < lv; k++) loff += outer_cnt[k];
        pm_options_scope_t *const sc = &options.scopes[v];
        pm_options_scope_init(sc, outer_cnt[lv]);
        for (uint32_t i = 0; i < outer_cnt[lv]; i++) {
            const uint32_t sym = outer_names[loff + i];
            if (sym != 0) {
                const char *const nm = korb_sym_name(c->vm, sym);
                pm_string_constant_init(&sc->locals[i], nm, strlen(nm));
            } else {
                snprintf(dummy[off], sizeof dummy[off], "#%u.%u", lv, i);
                pm_string_constant_init(&sc->locals[i], dummy[off], strlen(dummy[off]));
                off++;
            }
        }
    }
    pm_options_scope_t *scope = &options.scopes[outer_levels];
    pm_options_scope_init(scope, name_cnt);
    for (uint32_t i = 0; i < name_cnt; i++) {
        const char *nm = korb_sym_name(c->vm, name_syms[i]);
        pm_string_constant_init(&scope->locals[i], nm, strlen(nm));
    }
    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);
    if (parser.error_list.size > 0) {
        kp_stash_syntax_msg(c, &parser, fname);
        pm_node_destroy(&parser, root); pm_parser_free(&parser); pm_options_free(&options);
        free(dummy);
        return NULL;
    }
    /* the levels as frames for local resolution (nearest first) */
    struct kp_frame *outer = outer_levels ? calloc(outer_levels, sizeof *outer) : NULL;
    pm_constant_id_list_t *outer_lists = outer_levels ? calloc(outer_levels, sizeof *outer_lists) : NULL;
    if (outer_levels && (!outer || !outer_lists)) abort();
    for (uint32_t lv = 0, loff = 0; lv < outer_levels; loff += outer_cnt[lv], lv++) {
        pm_constant_id_list_t *const ls = &outer_lists[lv];
        ls->size = ls->capacity = outer_cnt[lv];
        ls->ids = outer_cnt[lv] ? calloc(outer_cnt[lv], sizeof *ls->ids) : NULL;
        for (uint32_t i = 0; i < outer_cnt[lv]; i++) {
            const uint32_t sym = outer_names[loff + i];
            if (sym == 0) continue;                                 /* placeholder: never matched */
            const char *const nm = korb_sym_name(c->vm, sym);
            ls->ids[i] = pm_constant_pool_find(&parser.constant_pool, (const uint8_t *)nm, strlen(nm));
        }
        outer[lv].locals = ls;
        outer[lv].fwd_slot = outer[lv].fwd_blk_slot = outer[lv].anon_rest_slot = -1;
    }
    struct kp_ctx tc = { .parser = &parser, .c = c, .fname = fname, .src_enc = kp_src_enc(c, &parser, src, len, src_enc),
                         .fsl_false = kp_fsl_false(src, len), .fsl_true = kp_fsl_true(src, len) || OPTION.frozen_literals,
                         .eval_method_mid = method_mid,
                         .seed_class_name = cref_name, .seed_chain = cref_chain, .seed_chain_len = cref_chain_len,
                         .seed_cref_is_caller = cref_is_caller,
                         .in_eval = true, .eval_outer = outer, .eval_outer_cnt = outer_levels };
    NODE *ast = transduce(&tc, root);
    for (uint32_t lv = 0; lv < outer_levels; lv++) free(outer_lists[lv].ids);
    free(outer_lists); free(outer); free(dummy);
    if (ast == NULL) ast = lit_nil();
    for (uint32_t pi = tc.pre_cnt; pi-- > 0; ) ast = ALLOC_node_seq(tc.pre_list[pi], ast);   /* BEGIN { } first */
    free(tc.pre_list);
    free(tc.bake_list);
    free(tc.flink_list);
    bool serr = tc.syntax_error;
    if (serr && tc.syntax_err) c->vm->last_syntax_msg = tc.syntax_err;   /* the real reason, not the generic */
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    pm_options_free(&options);
    return serr ? NULL : ast;   /* binding in alternative pattern → SyntaxError */
}
