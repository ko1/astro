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
    bool it_param;            /* `{ it * 2 }` — slot 0 is the implicit `it` (prism lists no name for it) */
    bool module_function_mode; /* no-arg `module_function` seen in this class/module body */
    uint32_t method_mid;      /* enclosing def's name (0 = not a method body) — for super */
    bool dm_body;             /* this block frame is a define_method body: it IS a method at run time,
                               * so a `super` inside it names the entry being executed, not any def
                               * it happens to sit inside lexically. */
    uint32_t class_name_sym;  /* enclosing class/module body's const name (0 = not a class body) — for Module.nesting */
    bool anon_class_body;     /* `class << obj` — the cref is a class with no name, so constants must be
                               * owned via the runtime self instead of a baked name */
    bool anon_cref_method;    /* a method defined in such a body: its cref is that unnamed class, which only
                               * the frame's method-entry cell can name at runtime */
    uint32_t method_params;   /* enclosing def's positional param count — for forwarding super */
    bool depth_shift;         /* END { } body: prism resolved its locals against the ENCLOSING scope
                               * (depth 0), but the body runs as a closure, so every reference is one
                               * level out. */
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
    int32_t **add_cells;      /* yield-in-block trio-index cells: fixed up by += frame_size at pop */
    uint32_t add_cnt, add_capa;
    struct kp_frame *prev;
};

struct kp_ctx {
    pm_parser_t *parser;
    CTX *c;
    const char *fname;
    struct kp_frame *frame;
    bool next_block_is_dm;    /* the literal block about to be transduced is a define_method body */
    int32_t chain;            /* staging depth at the current program point */
    /* BEGIN { } bodies, hoisted to the very front of the program (CRuby runs
     * them once, before everything else — including a -n/-p gets loop). */
    NODE   **pre_list;
    uint32_t pre_cnt, pre_capa;
    int32_t **bake_list;      /* lvar-offset cells awaiting locals_cnt fixup */
    uint32_t bake_cnt, bake_capa;
    bool syntax_error;        /* transduce-time SyntaxError (e.g. binding in alternative pattern) */
    bool pending_depth_shift; /* the next pushed frame is an END { } body (see kp_frame.depth_shift) */
    uint8_t src_enc;          /* KORB_ENC_* for this file's string literals (from the magic comment) */
};

/* The file's `# encoding:` magic comment, as prism resolved it, mapped onto the
 * string encoding tag (a name koruby has no tag for is registered on the spot;
 * the literal's bytes are unaffected either way). */
static uint8_t
kp_src_enc(CTX *c, const pm_parser_t *parser)
{
    /* -K<letter> sets the script encoding too, unless a magic comment already
     * picked one (prism reports UTF-8 for "no comment"). */
    const char *const name = parser->encoding ? parser->encoding->name : NULL;
    if (OPTION.kcode && (name == NULL || strcmp(name, "UTF-8") == 0))
        return (uint8_t)korb_enc_index_pub(c->vm, OPTION.kcode);
    if (name == NULL) return KORB_ENC_UTF8;
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
static NODE *build_const_set(struct kp_ctx *tc, uint32_t name_cid, NODE *val);
static NODE *index_opassign_splat(struct kp_ctx *tc, const pm_index_operator_write_node_t *iw, const pm_node_t *node,
                                  const pm_arguments_node_t *args, const pm_node_t *value);

/* node_send takes a parse-time NODE* array [recv, args...]; these build the
 * common small-arity synthetic sends used throughout the parser (op-assign,
 * []/[]= desugar, case/when ===, ...).  The staging depth a caller must reserve
 * is the element count (recv + args). */
/* synthetic sends target public methods ([]/===/+/...), so self_off = INT32_MIN
 * disables the private/protected visibility guard (no caller-self needed). */
static NODE *kp_send0(uint32_t mid, uint32_t line, NODE *recv) {
    NODE **argv = malloc(sizeof(NODE *)); if (!argv) abort();
    argv[0] = recv;
    return ALLOC_node_send(mid, line, INT32_MIN, argv, 1);
}
static NODE *kp_send1(uint32_t mid, uint32_t line, NODE *recv, NODE *a0) {
    NODE **argv = malloc(sizeof(NODE *) * 2); if (!argv) abort();
    argv[0] = recv; argv[1] = a0;
    return ALLOC_node_send(mid, line, INT32_MIN, argv, 2);
}
static NODE *kp_send2(uint32_t mid, uint32_t line, NODE *recv, NODE *a0, NODE *a1) {
    NODE **argv = malloc(sizeof(NODE *) * 3); if (!argv) abort();
    argv[0] = recv; argv[1] = a0; argv[2] = a1;
    return ALLOC_node_send(mid, line, INT32_MIN, argv, 3);
}
/* recv.mid(args[0..n)) — n args of any count (recv + n → n+1 children). */
static NODE *kp_send_n(uint32_t mid, uint32_t line, NODE *recv, NODE *const *args, uint32_t n) {
    NODE **argv = malloc(sizeof(NODE *) * (1u + n)); if (!argv) abort();
    argv[0] = recv;
    for (uint32_t i = 0; i < n; i++) argv[1u + i] = args[i];
    return ALLOC_node_send(mid, line, INT32_MIN, argv, 1u + n);
}
/* Staging depth a caller must reserve to build a kp_sendN node.  node_send is
 * @framehdr, so its dispatcher advances the cursor by (children + KORB_FRAME_HDR)
 * — the reserved meta cells (EP/magic) sit below the staged recv/args.  Any
 * WITH_CHAIN that builds the recv/args of a kp_sendN MUST use these so the
 * parse-time chain bump matches the runtime cursor advance (else the children's
 * baked lget offsets are off by KORB_FRAME_HDR and read the wrong slot). */
#define KP_SEND0_SC (1u + KORB_FRAME_HDR)   /* recv only            */
#define KP_SEND1_SC (2u + KORB_FRAME_HDR)   /* recv + 1 arg         */
#define KP_SEND2_SC (3u + KORB_FRAME_HDR)   /* recv + 2 args        */
#define KP_SENDN_SC(n) ((1u + (n)) + KORB_FRAME_HDR)   /* recv + n args */

/* ---------------------------------------------------------------------- */

static uint32_t
kp_line(struct kp_ctx *tc, const pm_node_t *node)
{
    const int32_t line = pm_newline_list_line(&tc->parser->newline_list,
                                             node->location.start,
                                             tc->parser->start_line);
    return (uint32_t)line;                  /* eval can set a negative first line: kept as-is, printed signed */
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
    f->saved_chain = tc->chain;
    f->synth_cnt = 0;
    f->uses_block = false;
    f->it_param = false;
    f->module_function_mode = false;
    f->method_mid = 0;
    f->dm_body = false;
    f->method_params = 0;
    f->depth_shift = tc->pending_depth_shift;
    f->fwd_slot = -1;
    f->fwd_blk_slot = -1;
    f->anon_rest_slot = -1;
    f->anon_blk_slot = -1;
    f->anon_kwrest_slot = -1;
    f->method_rest_slot = -1;
    f->method_post_base = -1;
    f->method_post_cnt = 0;
    f->method_kw_info = NULL;
    f->class_name_sym = 0;
    f->anon_class_body = false;
    f->anon_cref_method = false;
    f->max_ref_depth = 0;
    f->add_cells = NULL;
    f->add_cnt = f->add_capa = 0;
    f->prev = tc->frame;
    tc->frame = f;
    tc->chain = 0;
}

/* the innermost lexically-enclosing class/module const name (0 = top-level) —
 * the cref for owner-aware bare constant reads (walk the frame chain). */
static uint32_t kp_cref_owner(struct kp_ctx *tc) {
    for (struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0) return f->class_name_sym;
    return 0;
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

/* Returns the frame size.  Every frame reserves self(fs-1) + def_class(fs-2)
 * on top; a yielding frame also reserves the 3-cell block group below them.
 * Layout top-down:
 *   [locals... | block_entry(fs-5) | def_env(fs-4) | captured_self(fs-3)
 *              | def_class(fs-2) | self(fs-1)] */
static uint32_t
pop_frame(struct kp_ctx *tc)
{
    struct kp_frame *f = tc->frame;
    /* bottom-header: self lives at base[-1] (not a top cell); the only reserved
     * top cell is the method entry.  +1u (entry) instead of +2u (entry+self). */
    uint32_t frame_size = (uint32_t)f->locals->size + f->synth_cnt + 1u + (f->uses_block ? 3u : 0u);
    for (uint32_t i = f->bake_base; i < tc->bake_cnt; i++) {
        *tc->bake_list[i] -= (int32_t)frame_size;
    }
    tc->bake_cnt = f->bake_base;
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
    tc->frame = f->prev;
    free(f);
    return frame_size;
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
        if (list->ids[i] == cid) return (uint32_t)i;
    }
    kp_failf(tc, node, "koruby_precise: local '%s' not in scope table", kp_cid_cstr(tc, cid));
}

/* Index of an outer variable `depth` enclosing scopes out (prism depth). */
static uint32_t
lvar_index_at(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    struct kp_frame *f = tc->frame;
    for (uint32_t d = 0; d < depth; d++) {
        f = f ? f->prev : NULL;
    }
    if (!f) kp_failf(tc, node, "koruby_precise: outer scope depth %u not found", depth);
    for (size_t i = 0; i < f->locals->size; i++) {
        if (f->locals->ids[i] == cid) return (uint32_t)i;
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
    return (uint32_t)tc->frame->locals->size + tc->frame->synth_cnt++;
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
/* @@x read/write: self at base[-1] (baked); the method-entry / def_class cell is
 * now the frame top (base[fs-1] = -1-chain, not baked, since the self cell is gone). */
static NODE *
bake_cvar_get(struct kp_ctx *tc, uint32_t name, uint32_t soft)
{
    NODE *n = ALLOC_node_cvar_get(-1 - tc->chain, -1 - tc->chain, name, soft);
    bake_add(tc, &n->u.node_cvar_get.self_off);
    return n;
}
static NODE *
bake_cvar_set(struct kp_ctx *tc, uint32_t name, NODE *val)
{
    NODE *n = ALLOC_node_cvar_set(-1 - tc->chain, -1 - tc->chain, name, val);
    bake_add(tc, &n->u.node_cvar_set.self_off);
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

static NODE *
transduce_statements(struct kp_ctx *tc, const pm_statements_node_t *stmts)
{
    if (stmts == NULL || stmts->body.size == 0) return lit_nil();
    NODE *acc = NULL;
    for (size_t i = 0; i < stmts->body.size; i++) {
        NODE *one = transduce(tc, stmts->body.nodes[i]);
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
        NODE *cls = ALLOC_node_const(korb_intern(tc->c->vm, "StandardError", 13), 0, INT32_MIN, INT32_MIN);
        NODE *nd = ALLOC_node_rescue(cls, body, next, resc_var, flags);
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
        NODE *nd = splat ? ALLOC_node_rescue_splat(cls, body, next, resc_var, flags)
                         : ALLOC_node_rescue(cls, body, next, resc_var, flags);
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
static struct korb_pat *
build_pattern_desc(struct kp_ctx *tc, const pm_node_t *pat)
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
            if (ap->constant) p->value_node = transduce(tc, ap->constant);   /* `Const[...]` → Const === subject first */
            p->elems = calloc(p->n ? p->n : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_desc(tc, ap->requireds.nodes[i]);
            return p;
        }
        if (ap->rest) {                                       /* `[pre..., *rest, post...]` (optionally `Const[...]`) */
            p->kind = 6; p->n = (uint32_t)ap->requireds.size; p->npost = (uint32_t)ap->posts.size;
            if (ap->constant) p->value_node = transduce(tc, ap->constant);
            const uint32_t total = p->n + p->npost;
            p->elems = calloc(total ? total : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_desc(tc, ap->requireds.nodes[i]);
            for (uint32_t i = 0; i < p->npost; i++) p->elems[p->n + i] = build_pattern_desc(tc, ap->posts.nodes[i]);
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
            if (fp->constant) p->value_node = transduce(tc, fp->constant);   /* `Const[*, …, *]` */
            p->elems = calloc(p->n ? p->n : 1, sizeof(struct korb_pat *));
            for (uint32_t i = 0; i < p->n; i++) p->elems[i] = build_pattern_desc(tc, fp->requireds.nodes[i]);
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
            if (hp->constant) p->value_node = transduce(tc, hp->constant);   /* `Const(k: …)` → Const === subject */
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
                    p->elems[i] = build_pattern_desc(tc, as->value);
                } else {                                      /* `key:` shorthand → bind local named `key` */
                    int32_t li = -1;
                    for (size_t j = 0; j < tc->frame->locals->size; j++) {
                        pm_constant_t *ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, tc->frame->locals->ids[j]);
                        if (ct->length == klen && memcmp(ct->start, kname, klen) == 0) { li = (int32_t)j; break; }
                    }
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
            p->elems[0] = build_pattern_desc(tc, cp->value);
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
        p->elems[0] = build_pattern_desc(tc, ap->left);
        p->elems[1] = build_pattern_desc(tc, ap->right);
        return p;
    } else if (PM_NODE_TYPE_P(pat, PM_PINNED_VARIABLE_NODE)) {      /* `^var` → var === subject */
        p->kind = 7;   /* pin: value_node is a frame-local read → must EVAL in the match frame (base), not cur */
        p->value_node = transduce(tc, (const pm_node_t *)((const pm_pinned_variable_node_t *)pat)->variable);
        return p;
    } else if (PM_NODE_TYPE_P(pat, PM_PINNED_EXPRESSION_NODE)) {    /* `^(expr)` */
        p->kind = 1;
        p->value_node = transduce(tc, (const pm_node_t *)((const pm_pinned_expression_node_t *)pat)->expression);
        return p;
    }
    /* value pattern: `pattern === subject` (constant / literal / range / etc.) */
    p->kind = 1;
    p->value_node = transduce(tc, pat);
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

/* ---- operators --------------------------------------------------------- */

extern const struct NodeKind kind_node_plus;         /* all binops share slot_count */
extern const struct NodeKind kind_node_caseeq;       /* case/when `v === subj` */
extern const struct NodeKind kind_node_entry;        /* block/lambda entry — guards proc/block reify */
extern const struct NodeKind kind_node_aref;         /* recv[idx] */
extern const struct NodeKind kind_node_aset;         /* recv[idx] = val */
extern const struct NodeKind kind_node_ary_push;     /* array-literal push chain */
extern const struct NodeKind kind_node_ary_concat;   /* array-literal splat (*) chain */
extern const struct NodeKind kind_node_const_set;    /* FOO = expr */
static NODE *build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa);
static NODE *build_array_with_fwd(struct kp_ctx *tc, struct pm_node **elems, size_t n);
static NODE *kp_make_binding_node(struct kp_ctx *tc, uint32_t line);
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

static NODE *
alloc_binop(enum kp_binop op, NODE *lhs, NODE *rhs, uint32_t line)
{
    switch (op) {
      case KP_PLUS:  return ALLOC_node_plus(lhs, rhs, line);
      case KP_MINUS: return ALLOC_node_minus(lhs, rhs, line);
      case KP_MUL:   return ALLOC_node_mul(lhs, rhs, line);
      case KP_DIV:   return ALLOC_node_div(lhs, rhs, line);
      case KP_MOD:   return ALLOC_node_mod(lhs, rhs, line);
      case KP_LT:    return ALLOC_node_lt(lhs, rhs, line);
      case KP_LE:    return ALLOC_node_le(lhs, rhs, line);
      case KP_GT:    return ALLOC_node_gt(lhs, rhs, line);
      case KP_GE:    return ALLOC_node_ge(lhs, rhs, line);
      case KP_EQ:    return ALLOC_node_eq(lhs, rhs);
      case KP_NEQ:   return ALLOC_node_neq(lhs, rhs);
      case KP_BAND:  return ALLOC_node_band(lhs, rhs, line);
      case KP_BOR:   return ALLOC_node_bor(lhs, rhs, line);
      case KP_BXOR:  return ALLOC_node_bxor(lhs, rhs, line);
      case KP_SHL:   return ALLOC_node_shl(lhs, rhs, line);
      case KP_SHR:   return ALLOC_node_shr(lhs, rhs, line);
      default:       abort();
    }
}

/* ---- calls -------------------------------------------------------------- */

/* Build Proc#parameters metadata once at parse time (cold; never touched on the
 * call/yield hot path).  Stored on node_entry.param_info.  Raw kinds (req kept
 * as req); #parameters converts positional req→opt for non-lambda procs. */
static void *
build_param_info(struct kp_ctx *tc, const pm_node_t *blk_params)
{
    if (!blk_params) return NULL;
    if (PM_NODE_TYPE_P(blk_params, PM_NUMBERED_PARAMETERS_NODE)) {
        uint32_t n = ((const pm_numbered_parameters_node_t *)blk_params)->maximum;
        struct korb_param_info *pi = malloc(sizeof(*pi) + n * sizeof(struct korb_param_entry));
        if (!pi) abort();
        pi->n = n;
        for (uint32_t i = 0; i < n; i++) { char nm[8]; snprintf(nm, sizeof(nm), "_%u", i + 1);
            pi->e[i].kind = 0; pi->e[i].name = korb_intern(tc->c->vm, nm, (uint32_t)strlen(nm)); }
        return pi;
    }
    if (PM_NODE_TYPE_P(blk_params, PM_IT_PARAMETERS_NODE)) {
        struct korb_param_info *pi = malloc(sizeof(*pi) + sizeof(struct korb_param_entry));
        if (!pi) abort();
        pi->n = 1; pi->e[0].kind = 0; pi->e[0].name = 0;   /* `it` is anonymous */
        return pi;
    }
    const pm_parameters_node_t *ps = NULL;
    if (PM_NODE_TYPE_P(blk_params, PM_BLOCK_PARAMETERS_NODE))
        ps = ((const pm_block_parameters_node_t *)blk_params)->parameters;
    else if (PM_NODE_TYPE_P(blk_params, PM_PARAMETERS_NODE))
        ps = (const pm_parameters_node_t *)blk_params;
    if (!ps) return NULL;
    uint32_t n = (uint32_t)(ps->requireds.size + ps->optionals.size + (ps->rest ? 1 : 0) +
                            ps->posts.size + ps->keywords.size + (ps->keyword_rest ? 1 : 0) + (ps->block ? 1 : 0));
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
    if (ps->rest) {   /* anonymous `*` reports the name :* (Ruby 3.x), a named rest its name */
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
    }
    if (ps->block) {   /* anonymous `&` → :& */
        const pm_constant_id_t bn = ((const pm_block_parameter_node_t *)ps->block)->name;
        pi->e[k].kind = 6; pi->e[k].name = bn ? kp_intern_cid(tc, bn) : korb_intern(tc->c->vm, "&", 1); k++;
    }
    #undef PI_ADD
    pi->n = k;
    return pi;
}

/* Parse a block literal into a node_entry (its own scope; registered as an
 * AOT entry like a method body).  docs/v2_blocks_design.md. */
static NODE *
transduce_block_parts(struct kp_ctx *tc, const pm_constant_id_list_t *blk_locals,
                      const pm_node_t *blk_params, const pm_node_t *blk_body)
{
    push_frame(tc, blk_locals);
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
            if (ps->block) {                            /* `|&blk|`: bind the forwarded block as a Proc into its local */
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
                            kp_failf(tc, (const pm_node_t *)op, "koruby_precise: block optional not locals[%u]", loc);
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
                     * local indexes — keep that combination unsupported.) */
                    const bool anon_rest =
                        PM_NODE_TYPE_P(ps->rest, PM_IMPLICIT_REST_NODE) ||
                        (PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE) &&
                         !((const pm_rest_parameter_node_t *)ps->rest)->name);
                    if (anon_rest) {
                        if (ps->posts.size) { pop_frame(tc); return kp_unsupported(tc, ps->rest, "anonymous block rest with post params"); }
                        rest_slot = -2;
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
                    uint32_t cap2 = 32, len2 = 0;
                    uint8_t *sp2 = malloc(cap2);
                    if (!sp2) abort();
                    /* header: number of LOGICAL params before *rest (locals and
                     * logical params diverge once a param destructures) */
                    sp2[len2++] = (uint8_t)(ps->requireds.size + ps->optionals.size);
                    #define SP2_PUT(b) do { if (len2 == cap2) { cap2 *= 2; sp2 = realloc(sp2, cap2); if (!sp2) abort(); } sp2[len2++] = (uint8_t)(b); } while (0)
                    bool bad2 = false;
                    uint32_t li2 = 0;
                    #define SP2_LEAF(node_, cid_) do {                         const uint32_t _li = (uint32_t)lvar_index(tc, (node_), (cid_));                         if (_li > 254) { bad2 = true; break; }                         SP2_PUT(0x00); SP2_PUT(_li); li2++; } while (0)
                    for (uint32_t pass = 0; pass < 3 && !bad2; pass++) {
                        const pm_node_list_t *lst = pass == 0 ? &ps->requireds
                                                  : pass == 1 ? &ps->optionals : &ps->posts;
                        for (uint32_t i = 0; i < lst->size && !bad2; i++) {
                            const pm_node_t *p = lst->nodes[i];
                            if (PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE))
                                SP2_LEAF(p, ((const pm_required_parameter_node_t *)p)->name);
                            else if (PM_NODE_TYPE_P(p, PM_OPTIONAL_PARAMETER_NODE))
                                SP2_LEAF(p, ((const pm_optional_parameter_node_t *)p)->name);
                            else if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) {
                                const pm_multi_target_node_t *mt3 = (const pm_multi_target_node_t *)p;
                                if (mt3->rest || mt3->rights.size || mt3->lefts.size == 0 || mt3->lefts.size > 254) { bad2 = true; break; }
                                SP2_PUT(0xFF); SP2_PUT(mt3->lefts.size);
                                for (uint32_t j = 0; j < mt3->lefts.size && !bad2; j++) {
                                    const pm_node_t *t3 = mt3->lefts.nodes[j];
                                    if (PM_NODE_TYPE_P(t3, PM_LOCAL_VARIABLE_TARGET_NODE))
                                        SP2_LEAF(t3, ((const pm_local_variable_target_node_t *)t3)->name);
                                    else if (PM_NODE_TYPE_P(t3, PM_REQUIRED_PARAMETER_NODE))
                                        SP2_LEAF(t3, ((const pm_required_parameter_node_t *)t3)->name);
                                    else bad2 = true;
                                }
                            } else bad2 = true;
                        }
                        if (pass == 1 && rest_slot >= 0) { SP2_PUT(0x00); SP2_PUT((uint32_t)rest_slot); li2++; }   /* the rest param */
                    }
                    #undef SP2_LEAF
                    #undef SP2_PUT
                    if (bad2) { free(sp2); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "destructuring block param with opt/rest"); }
                    destructure_spec = sp2;
                    destr_len = len2;
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
                     * identity mapping no longer holds — refuse instead of
                     * writing into the wrong local (raises when the block runs). */
                    if (lvar_index(tc, t, cid) != i) { pop_frame(tc); return kp_unsupported(tc, t, "repeated name in a destructuring block parameter"); }
                }
                bparams = 1;
                destructure_n = (uint32_t)mt->lefts.size;
            } else {
                bparams = (uint32_t)ps->requireds.size;
                bool any_destr = false;
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
                        if (lvar_index(tc, p, cid) != i && kp_cid_cstr(tc, cid)[0] != '_') {   /* `_`-repeat: legal, binds positionally */
                            pop_frame(tc);
                            return kp_unsupported(tc, p, "repeated block parameter name");
                        }
                    }
                } else {
                    /* mixed scalar + |(...)| destructuring params, e.g. |a, (k, v)|
                     * or nested |(a, (b, c))|.  The spec is a byte stream, one
                     * entry per top-level param: 0x00 = scalar (1 local), else
                     * 0xFF <k> followed by k nested entries (a destructured
                     * group).  Locals are assigned to leaves left to right. */
                    uint32_t cap = 16, len = 0;
                    uint8_t *spec = malloc(cap);
                    if (!spec) abort();
                    uint32_t loc = 0;
                    bool bad = false;
                    #define SPEC_PUT(b) do { if (len == cap) { cap *= 2; spec = realloc(spec, cap); if (!spec) abort(); } spec[len++] = (uint8_t)(b); } while (0)
                    /* recursive encoder (iterative over a small work stack would
                     * need its own bookkeeping; params nest only a few deep) */
                    struct { const pm_node_list_t *list; uint32_t i; } stk[16];
                    uint32_t sp = 0;
                    const pm_node_list_t *cur = &ps->requireds;
                    uint32_t ci = 0;
                    for (;;) {
                        if (ci >= cur->size) {
                            if (sp == 0) break;
                            sp--; cur = stk[sp].list; ci = stk[sp].i;
                            continue;
                        }
                        const pm_node_t *p = cur->nodes[ci++];
                        if (PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE) ||
                            PM_NODE_TYPE_P(p, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                            pm_constant_id_t cid = PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)
                                ? ((const pm_required_parameter_node_t *)p)->name
                                : ((const pm_local_variable_target_node_t *)p)->name;
                            /* a repeated `_` binds positionally; any other name
                             * that lands on a different local is unsupported */
                            {   /* leaf: encode the TARGET local (a repeated `_`
                                 * shares one local but still consumes a position) */
                                const uint32_t li = (uint32_t)lvar_index(tc, p, cid);
                                if (li > 254) { bad = true; break; }
                                SPEC_PUT(0x00); SPEC_PUT(li);
                            }
                            loc++;
                        } else if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) {
                            const pm_multi_target_node_t *mt2 = (const pm_multi_target_node_t *)p;
                            if (mt2->lefts.size > 254 || mt2->rights.size > 254 || sp >= 16) { bad = true; break; }
                            if (mt2->rest) {
                                /* |(a, *b, c)| — 0xFE <nleft> <rest_named> <nright>,
                                 * then the left entries and the right entries
                                 * (the rest leaf, when named, sits between them). */
                                bool rest_named = false;
                                if (PM_NODE_TYPE_P(mt2->rest, PM_SPLAT_NODE)) {
                                    const pm_splat_node_t *spn = (const pm_splat_node_t *)mt2->rest;
                                    rest_named = (spn->expression != NULL);
                                } else if (PM_NODE_TYPE_P(mt2->rest, PM_REST_PARAMETER_NODE)) {
                                    rest_named = (((const pm_rest_parameter_node_t *)mt2->rest)->name != 0);
                                } else if (!PM_NODE_TYPE_P(mt2->rest, PM_IMPLICIT_REST_NODE)) { bad = true; break; }
                                SPEC_PUT(0xFE); SPEC_PUT(mt2->lefts.size); SPEC_PUT(rest_named ? 1 : 0); SPEC_PUT(mt2->rights.size);
                                /* leaves in order: lefts, [rest], rights — encode
                                 * them by walking the three lists inline */
                                for (uint32_t z = 0; z < mt2->lefts.size && !bad; z++) {
                                    const pm_node_t *lt = mt2->lefts.nodes[z];
                                    if (!PM_NODE_TYPE_P(lt, PM_LOCAL_VARIABLE_TARGET_NODE) &&
                                        !PM_NODE_TYPE_P(lt, PM_REQUIRED_PARAMETER_NODE)) { bad = true; break; }
                                    pm_constant_id_t cid2 = PM_NODE_TYPE_P(lt, PM_REQUIRED_PARAMETER_NODE)
                                        ? ((const pm_required_parameter_node_t *)lt)->name
                                        : ((const pm_local_variable_target_node_t *)lt)->name;
                                    { const uint32_t li = (uint32_t)lvar_index(tc, lt, cid2);
                                      if (li > 254) { bad = true; break; }
                                      SPEC_PUT(0x00); SPEC_PUT(li); }
                                    loc++;
                                }
                                if (bad) break;
                                if (rest_named) {
                                    const pm_node_t *rn = mt2->rest;
                                    pm_constant_id_t rcid = PM_NODE_TYPE_P(rn, PM_SPLAT_NODE)
                                        ? ((const pm_local_variable_target_node_t *)((const pm_splat_node_t *)rn)->expression)->name
                                        : ((const pm_rest_parameter_node_t *)rn)->name;
                                    { const uint32_t li = (uint32_t)lvar_index(tc, rn, rcid);
                                      if (li > 254) { bad = true; break; }
                                      SPEC_PUT(li); }        /* the rest leaf's local (0xFE header already emitted) */
                                    loc++;
                                }
                                for (uint32_t z = 0; z < mt2->rights.size && !bad; z++) {
                                    const pm_node_t *rt = mt2->rights.nodes[z];
                                    if (!PM_NODE_TYPE_P(rt, PM_LOCAL_VARIABLE_TARGET_NODE) &&
                                        !PM_NODE_TYPE_P(rt, PM_REQUIRED_PARAMETER_NODE)) { bad = true; break; }
                                    pm_constant_id_t cid2 = PM_NODE_TYPE_P(rt, PM_REQUIRED_PARAMETER_NODE)
                                        ? ((const pm_required_parameter_node_t *)rt)->name
                                        : ((const pm_local_variable_target_node_t *)rt)->name;
                                    { const uint32_t li = (uint32_t)lvar_index(tc, rt, cid2);
                                      if (li > 254) { bad = true; break; }
                                      SPEC_PUT(0x00); SPEC_PUT(li); }
                                    loc++;
                                }
                                if (bad) break;
                                continue;
                            }
                            if (mt2->rights.size || mt2->lefts.size == 0) { bad = true; break; }
                            SPEC_PUT(0xFF); SPEC_PUT(mt2->lefts.size);
                            stk[sp].list = cur; stk[sp].i = ci; sp++;
                            cur = &mt2->lefts; ci = 0;
                        } else { bad = true; break; }
                    }
                    #undef SPEC_PUT
                    if (bad) { free(spec); pop_frame(tc); return kp_unsupported(tc, (const pm_node_t *)ps, "destructuring block parameter"); }
                    destructure_spec = spec;
                    destr_len = len;
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
    uint16_t *cap_ns = NULL;
    if (cap_depth > 0) {
        cap_ns = malloc(sizeof(uint16_t) * cap_depth);
        if (!cap_ns) abort();
        struct kp_frame *encl = tc->frame->prev;
        for (uint32_t k = 0; k < cap_depth; k++) {
            cap_ns[k] = encl ? (uint16_t)encl->locals->size : 0;
            encl = encl ? encl->prev : NULL;
        }
    }
    uint32_t frame_size = pop_frame(tc);    /* block locals (+2 if the block yields) */
    NODE *entry = ALLOC_node_entry(body, bparams, frame_size, destructure_n, destructure_spec, destr_len, cap_depth, cap_ns, rest_slot, opt_defaults, req_cnt, kw_info, build_param_info(tc, blk_params), blk_param_slot, post_cnt);
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
    NODE *body = kp_send0(sym_id, 0, recv);
    uint32_t frame_size = pop_frame(tc);
    NODE *entry = ALLOC_node_entry(body, 1, frame_size, 0, NULL, 0, 0, NULL, -1, NULL, 0, NULL, NULL, -1, 0);
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
    NODE *call = ALLOC_node_call_blk(mid, line, entry, -(tc->chain + (int32_t)cnt + KORB_FRAME_HDR), argv, cnt);
    bake_add(tc, &call->u.node_call_blk.def_env_off);
    return call;
}

static NODE *
transduce_func_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    const pm_arguments_node_t *args = cn->arguments;
    size_t argc = args ? args->arguments.size : 0;

    /* block_given? — reaches the enclosing METHOD's block, not a nested block's.
     * Walk up to the method frame (like yield); flat read at method level, else
     * resolve through `depth` env links to the method frame's biseq. */
    if (argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "block_given?") == 0) {
        struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
        if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }   /* outside a method: legacy flat path */
        mf->uses_block = true;                                     /* the method reserves the block trio */
        if (depth == 0)
            return ALLOC_node_block_given(-4 - tc->chain);        /* method top-level: this frame's biseq cell */
        NODE *bg = ALLOC_node_block_given_outer(-2 - tc->chain, depth, -4);   /* prev_off = this block's env link; trio_base += method frame_size */
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
        return kp_make_binding_node(tc, kp_line(tc, (const pm_node_t *)cn));
    }

    /* `eval(str)` — CRuby evaluates in the CALLER's binding.  koruby has no
     * runtime frame metadata, so bake a binding for the call site and pass it
     * as the hidden 2nd argument (only the plain 1-arg form; an explicit
     * binding / splat form goes through the normal path).  A user-defined
     * `eval` would see the extra Binding — same caveat as CRuby's own
     * "eval is special" corners, accepted for compatibility. */
    if (cn->receiver == NULL && argc == 1 && cn->block == NULL &&
        !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE) &&
        !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_FORWARDING_ARGUMENTS_NODE) &&
        strcmp(kp_cid_cstr(tc, cn->name), "eval") == 0) {
        const uint32_t line = kp_line(tc, (const pm_node_t *)cn);
        const uint32_t cnt = 3;                          /* [self, str, binding] */
        NODE **argv = malloc(sizeof(NODE *) * cnt);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;
        argv[0] = bake_self(tc);
        argv[1] = transduce(tc, cn->arguments->arguments.nodes[0]);
        argv[2] = kp_make_binding_node(tc, line);
        tc->chain = saved;
        return ALLOC_node_call(korb_intern(tc->c->vm, "eval", 4), line, argv, cnt);
    }

    /* bare `instance_eval(str...)` — same hidden-binding contract as the
     * explicit-receiver lowering in transduce_call (caller locals). */
    if (cn->block == NULL && argc >= 1 && argc <= 3 &&
        strcmp(kp_cid_cstr(tc, cn->name), "instance_eval") == 0) {
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
            return ALLOC_node_call(kp_intern_cid(tc, cn->name), line2, argv, cnt);
        }
    }

    /* `local_variables` — desugar to `binding.local_variables`.  The Binding node is
     * built staged as the send's receiver (KP_SEND0_SC) so its baked offsets match. */
    if (cn->receiver == NULL && argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "local_variables") == 0) {
        NODE *nb;
        WITH_CHAIN(tc, KP_SEND0_SC, (nb = kp_make_binding_node(tc, line)));
        return kp_send0(korb_intern(tc->c->vm, "local_variables", 15), line, nb);
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
                { NODE *_na = ALLOC_node_attr(-1 - tc->chain, descs, count); bake_add(tc, &_na->u.node_attr.self_off); return _na; }
            }
        }
    }

    if (cn->block) {
        /* proc { } / lambda { } — reify the literal block into a Proc object. */
        if (argc == 0 && PM_NODE_TYPE_P(cn->block, PM_BLOCK_NODE)) {
            const char *cnm = kp_cid_cstr(tc, cn->name);
            int is_lam = !strcmp(cnm, "lambda");
            if (is_lam || !strcmp(cnm, "proc")) {
                NODE *entry = transduce_block(tc, (const pm_block_node_t *)cn->block);
                if (entry->head.kind != &kind_node_entry) return entry;   /* unsupported params → propagate (don't reify a non-entry) */
                int32_t self_off = -1 - tc->chain;
                NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, (uint32_t)is_lam);
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
                if (anon_blk && tc->frame->anon_blk_slot < 0)
                    return kp_unsupported(tc, (const pm_node_t *)cn, "bare & outside a (&) method body");
                uint32_t pslot = anon_blk ? (uint32_t)tc->frame->anon_blk_slot : alloc_synth_local(tc);
                NODE *pset = anon_blk ? NULL : bake_lset(tc, pslot, transduce(tc, ba->expression));
                bool has_splat = false;
                for (size_t i = 0; i < argc; i++)
                    if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
                if (has_splat) {
                    /* `f(*arr, &proc)` (implicit self) — build the args Array; one
                     * staged child = the array, self via self_off, proc via proc_off. */
                    int32_t self_off = -1 - tc->chain - 1;
                    int32_t proc_off = (int32_t)pslot - tc->chain - 1;
                    NODE *arr;
                    WITH_CHAIN(tc, 1, (arr = build_array(tc, args->arguments.nodes, argc, (uint32_t)argc)));
                    NODE *_cs = ALLOC_node_call_splat_blkproc(mid, line, self_off, proc_off, arr);
                    bake_add(tc, &_cs->u.node_call_splat_blkproc.self_off);
                    bake_add(tc, &_cs->u.node_call_splat_blkproc.proc_off);
                    return pset ? ALLOC_node_seq(pset, _cs) : _cs;
                }
                uint32_t cnt = 1u + (uint32_t)argc;     /* self receiver + args */
                NODE **argv = malloc(sizeof(NODE *) * cnt);
                if (!argv) abort();
                int32_t saved = tc->chain;
                tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
                argv[0] = bake_self(tc);   /* self → base[-1] */
                for (size_t i = 0; i < argc; i++)
                    argv[1 + i] = transduce(tc, args->arguments.nodes[i]);
                tc->chain = saved;
                NODE *call = ALLOC_node_call_blkproc(mid, line, (int32_t)pslot - tc->chain - (int32_t)cnt - KORB_FRAME_HDR, argv, cnt);
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
                WITH_CHAIN(tc, 1, (arr = build_array(tc, args->arguments.nodes, argc, (uint32_t)argc)));
                NODE *_cs = ALLOC_node_call_splat_blk(mid, line, self_off, entry, def_env_off, arr);
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
        if (tc->frame->fwd_slot < 0)
            return kp_unsupported(tc, (const pm_node_t *)cn, "... forwarding outside a (...) method body");
        int32_t self_off = -1 - tc->chain - 1;
        int32_t proc_off = tc->frame->fwd_blk_slot - tc->chain - 1;
        NODE *arr;
        WITH_CHAIN(tc, 1, (arr = (argc == 1)
            ? bake_lget(tc, (uint32_t)tc->frame->fwd_slot)
            : build_array_with_fwd(tc, args->arguments.nodes, argc - 1)));   /* f(a, ..., ...) */
        NODE *_cs = ALLOC_node_call_splat_blkproc(mid, line, self_off, proc_off, arr);
        bake_add(tc, &_cs->u.node_call_splat_blkproc.self_off);
        bake_add(tc, &_cs->u.node_call_splat_blkproc.proc_off);
        return _cs;
    }

    /* actual `*splat` call f(*arr): build the args Array, dispatch dynamically */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            int32_t self_off = -1 - tc->chain - 1;       /* one staged child: the args array */
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args->arguments.nodes, argc, (uint32_t)argc)));
            { NODE *_cs = ALLOC_node_call_splat(mid, line, self_off, arr); bake_add(tc, &_cs->u.node_call_splat.self_off); return _cs; }
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
            return ALLOC_node_call_kw(mid, line, pos_argc, (const char *)(const void *)kw_syms, argv, cnt);
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
            return ALLOC_node_call_kws(mid, line, argv, cnt);
        return ALLOC_node_call(mid, line, argv, cnt);
    }
}

static NODE *
transduce_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    if (cn->receiver == NULL) {
        return transduce_func_call(tc, cn);
    }

    const char *name = kp_cid_cstr(tc, cn->name);
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    size_t argc = cn->arguments ? cn->arguments->arguments.size : 0;

    /* `Module.nesting` — the lexically-enclosing class/module bodies, innermost
     * first.  Resolved at parse time from the frame chain (the names) and at
     * runtime to the live class objects (koruby's flat const table). */
    if (argc == 0 && cn->block == NULL && strcmp(name, "nesting") == 0 &&
        PM_NODE_TYPE_P(cn->receiver, PM_CONSTANT_READ_NODE) &&
        strcmp(kp_cid_cstr(tc, ((const pm_constant_read_node_t *)cn->receiver)->name), "Module") == 0) {
        uint32_t syms[64]; uint32_t n = 0;
        for (struct kp_frame *f = tc->frame; f && n < 64; f = f->prev)
            if (f->class_name_sym != 0) syms[n++] = f->class_name_sym;
        uint32_t *baked = malloc(sizeof(uint32_t) * (n ? n : 1));   /* immortal (baked into the node) */
        if (!baked) abort();
        for (uint32_t i = 0; i < n; i++) baked[i] = syms[i];
        return ALLOC_node_nesting((const char *)(const void *)baked, n);
    }

    /* operator calls → dedicated binop / unary nodes (a splat arg has no fixed
     * arity, so it falls through to the generic splat send below) */
    enum kp_binop op = kp_binop_kind(name);
    if (op != KP_BINOP_NONE && argc == 1 &&
        !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE)) {
        uint32_t n_slots = kind_node_plus.slot_count;   /* lhs staging (all binops alike) */
        NODE *lhs, *rhs;
        WITH_CHAIN(tc, n_slots, (lhs = transduce(tc, cn->receiver),
                                 rhs = transduce(tc, cn->arguments->arguments.nodes[0])));
        return alloc_binop(op, lhs, rhs, line);
    }
    if (strcmp(name, "-@") == 0 && argc == 0) {
        return ALLOC_node_neg(transduce(tc, cn->receiver), line);
    }
    if (strcmp(name, "!") == 0 && argc == 0) {
        return ALLOC_node_not(transduce(tc, cn->receiver));
    }

    /* recv[idx] / recv[idx] = val → type-fast-path nodes (Array+fixnum inline,
     * else deopt to a real send).  No block, no splat args. */
    if (!cn->block) {
        if (mid == tc->c->vm->mid_aref && argc == 1 &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE)) {
            NODE *recv, *idx;
            WITH_CHAIN(tc, kind_node_aref.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0])));
            return ALLOC_node_aref(line, recv, idx);
        }
        if (mid == tc->c->vm->mid_aset && argc == 2 &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE) &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[1], PM_SPLAT_NODE)) {
            NODE *recv, *idx, *val;
            WITH_CHAIN(tc, kind_node_aset.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0]),
                        val  = transduce(tc, cn->arguments->arguments.nodes[1])));
            return ALLOC_node_aset(line, recv, idx, val);
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
                if (anon_blk && tc->frame->anon_blk_slot < 0)
                    return kp_unsupported(tc, (const pm_node_t *)cn, "bare & outside a (&) method body");
                uint32_t pslot = anon_blk ? (uint32_t)tc->frame->anon_blk_slot : alloc_synth_local(tc);
                NODE *pset = anon_blk ? NULL : bake_lset(tc, pslot, transduce(tc, ba->expression));
                bool has_splat = false;
                for (size_t i = 0; i < argc; i++)
                    if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
                if (has_splat) {
                    /* `recv.m(*arr, &proc)` — build the args Array; node_send_splat_blkproc
                     * coerces the proc and forwards it (2 staged children: recv + array). */
                    int32_t proc_off = (int32_t)pslot - tc->chain - 2;
                    NODE *recv, *arr;
                    WITH_CHAIN(tc, 2, (recv = RECV_NODE(),
                                       arr  = build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc)));
                    NODE *_cs = ALLOC_node_send_splat_blkproc(mid, line, proc_off, recv, arr);
                    bake_add(tc, &_cs->u.node_send_splat_blkproc.proc_off);
                    return SAFE_WRAP(pset ? ALLOC_node_seq(pset, _cs) : _cs);
                }
                uint32_t sc = 1u + (uint32_t)argc;
                NODE **argv = malloc(sizeof(NODE *) * sc);
                if (!argv) abort();
                int32_t saved = tc->chain;
                tc->chain = saved + (int32_t)sc + KORB_FRAME_HDR;   /* @framehdr cursor +HDR */
                argv[0] = RECV_NODE();
                for (size_t i = 0; i < argc; i++)
                    argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
                tc->chain = saved;
                NODE *call = ALLOC_node_send_blkproc(mid, line, (int32_t)pslot - tc->chain - (int32_t)sc - KORB_FRAME_HDR, argv, sc);
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
                                   arr  = build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc)));
                NODE *_cs = ALLOC_node_send_splat_blk(mid, line, self_off, entry, def_env_off, recv, arr);
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
        NODE *call = ALLOC_node_send_blk(mid, line, self_off, entry, -(tc->chain + (int32_t)sc + KORB_FRAME_HDR), argv, sc);
        bake_add(tc, &call->u.node_send_blk.def_env_off);
        bake_add(tc, &call->u.node_send_blk.self_off);    /* captured self at base[-1] (bottom header) */
        return SAFE_WRAP(call);
    }
    #undef RECV_NODE
    #undef SAFE_WRAP
    /* `recv.m(...)` — same forwarding as the implicit-self case, with the receiver
     * as the extra staged child. */
    if (argc >= 1 && PM_NODE_TYPE_P(cn->arguments->arguments.nodes[argc - 1], PM_FORWARDING_ARGUMENTS_NODE)) {
        if (tc->frame->fwd_slot < 0)
            return kp_unsupported(tc, (const pm_node_t *)cn, "... forwarding outside a (...) method body");
        int32_t proc_off = tc->frame->fwd_blk_slot - tc->chain - 2;
        NODE *recv, *arr;
        WITH_CHAIN(tc, 2, (recv = transduce(tc, cn->receiver),
                           arr  = (argc == 1)
                               ? bake_lget(tc, (uint32_t)tc->frame->fwd_slot)
                               : build_array_with_fwd(tc, cn->arguments->arguments.nodes, argc - 1)));
        NODE *_cs = ALLOC_node_send_splat_blkproc(mid, line, proc_off, recv, arr);
        bake_add(tc, &_cs->u.node_send_splat_blkproc.proc_off);
        return _cs;
    }
    /* actual `*splat` receiver call recv.m(*arr): build args Array, dynamic dispatch */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            NODE *recv, *arr;
            WITH_CHAIN(tc, 2, (recv = transduce(tc, cn->receiver),
                               arr  = build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc)));
            /* an element assignment evaluates to the assigned value, not to []='s
             * return value, even when the index list is splatted */
            if (mid == tc->c->vm->mid_aset)
                return ALLOC_node_send_splat_aset(mid, line, recv, arr);
            return ALLOC_node_send_splat(mid, line, recv, arr);
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
            NODE *call = ALLOC_node_send(mid, line, self_off, argv, cnt);
            bake_add(tc, &call->u.node_send.self_off);
            return call;
        }
    }

    /* unified send (any fixed arity): stage [recv, arg0..] into a parse-time
     * NODE* array; node_send / node_send_safe stage them into consecutive slots
     * at eval.  Safe navigation (recv&.m) nil-checks recv before dispatch. */
    {
        bool safe = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;
        uint32_t cnt = 1u + (uint32_t)argc;
        int32_t self_off = -1 - tc->chain - (int32_t)cnt - KORB_FRAME_HDR;   /* caller self at base[-1], from the +HDR-shifted cursor */
        NODE **argv = malloc(sizeof(NODE *) * cnt);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)cnt + KORB_FRAME_HDR;   /* @framehdr: cursor +HDR (node_send/safe reserve a frame header) */
        argv[0] = transduce(tc, cn->receiver);
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        if (safe) return ALLOC_node_send_safe(mid, line, argv, cnt);
        /* trailing `**h` / `k: v, **h` bundle → drop an empty kwargs Hash at call time */
        if (argc >= 1 && PM_NODE_TYPE_P(cn->arguments->arguments.nodes[argc - 1], PM_KEYWORD_HASH_NODE)) {
            NODE *ckws = ALLOC_node_send_kws(mid, line, self_off, argv, cnt);
            bake_add(tc, &ckws->u.node_send_kws.self_off);
            return ckws;
        }
        NODE *call = ALLOC_node_send(mid, line, self_off, argv, cnt);
        bake_add(tc, &call->u.node_send.self_off);    /* fixed up by the caller frame_size (base[-1] = self) */
        return call;
    }
}

/* ---- def ----------------------------------------------------------------- */

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
        if (ps->block) {
            if (!ps->block->name) anon_blk_param = true;
            else                  blk_param_name = ps->block->name;
        }
        /* posts without a rest = required params after optionals, e.g. `def m(a=1, b)`:
         * supported — they bind from the tail, optionals fill the middle. */
        if (ps->rest && !PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE))
            return kp_unsupported(tc, (const pm_node_t *)dn, "forwarding rest parameter");
        /* anonymous `*` rest collects surplus args into a synth local (the body
         * can't name it).  With posts the post-slot layout assumes a named rest,
         * so reject that rarer combo. */
        if (ps->rest && ((const pm_rest_parameter_node_t *)ps->rest)->name == 0 && ps->posts.size)
            return kp_unsupported(tc, (const pm_node_t *)dn, "anonymous rest parameter with post parameters");
        req_cnt = (uint32_t)ps->requireds.size;
        opt_cnt = (uint32_t)ps->optionals.size;
        params_cnt = req_cnt + opt_cnt;   /* positional fixed slots; rest/keywords follow */
    }

    const bool anon_cref = tc->frame->anon_class_body || tc->frame->anon_cref_method;
    push_frame(tc, &dn->locals);
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
        rest_slot = rn ? (int32_t)lvar_index(tc, ps->rest, rn)   /* named rest → its local slot */
                       : (int32_t)alloc_synth_local(tc);          /* anonymous `*` → synth slot (bare `*` forwards it) */
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
    for (uint32_t i = 0; i < post_cnt; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) continue;      /* destructured below */
        if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, p, "non-plain post parameter"); }
        pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
        if (lvar_index(tc, p, cid) != post_base + i
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
    }
    for (uint32_t i = 0; i < post_cnt; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        if (!PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) continue;
        const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)p;
        NODE *const un = massign_general(tc, &mt->lefts, mt->rest, &mt->rights, bake_lget(tc, post_base + i));
        if (!un) { pop_frame(tc); return kp_unsupported(tc, p, "destructuring parameter"); }
        body = ALLOC_node_seq(un, body);
    }

    if (blk_param_name) {                  /* `&blk` → materialize the block into the local at body entry */
        tc->frame->uses_block = true;      /* reserve the frame's block cells */
        int32_t dst = (int32_t)lvar_index(tc, (const pm_node_t *)ps->block, blk_param_name) - tc->chain;
        NODE *bp = ALLOC_node_blkparam(dst, -4 - tc->chain, -3 - tc->chain, -2 - tc->chain);
        bake_add(tc, &bp->u.node_blkparam.dst_off);
        body = ALLOC_node_seq(bp, body);
    } else if (tc->frame->fwd_blk_slot >= 0 || tc->frame->anon_blk_slot >= 0) {
        /* `def m(...)` / `def m(&)` → same, into the synth forwarding slot */
        tc->frame->uses_block = true;
        int32_t slot = tc->frame->fwd_blk_slot >= 0 ? tc->frame->fwd_blk_slot : tc->frame->anon_blk_slot;
        NODE *bp = ALLOC_node_blkparam(slot - tc->chain, -4 - tc->chain, -3 - tc->chain, -2 - tc->chain);
        bake_add(tc, &bp->u.node_blkparam.dst_off);
        body = ALLOC_node_seq(bp, body);
    }
    uint32_t uses_block = tc->frame->uses_block ? 1u : 0u;
    uint32_t frame_size = pop_frame(tc);   /* = locals + 2 if the method yields */

    uint32_t mid = kp_intern_cid(tc, dn->name);
    /* Method#source_location: remember the (file, line) of this def's body. */
    korb_reg_srcloc(tc->c->vm, body, korb_intern(tc->c->vm, tc->fname, strlen(tc->fname)), kp_line(tc, (const pm_node_t *)dn));
    /* full param list (names + kinds) for Method#parameters — cold-read only, like the block node_entry's. */
    void *pinfo = build_param_info(tc, dn->parameters ? (const pm_node_t *)dn->parameters : NULL);
    /* self at the def site (enclosing frame) = the default definee */
    NODE *def;
    if (mod_func) {
        /* module_function: define as instance method AND as a singleton on self. */
        NODE *idef = ALLOC_node_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, -1 - tc->chain, -1 - tc->chain);
        bake_add(tc, &idef->u.node_def.self_off);          /* definee = self at base[-1] */
        NODE *sdef = ALLOC_node_singleton_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, recv_node);
        def = ALLOC_node_seq(idef, sdef);
    } else if (recv_node) {
        def = ALLOC_node_singleton_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, recv_node);
    } else {
        def = ALLOC_node_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, pinfo, -1 - tc->chain, -1 - tc->chain);
        bake_add(tc, &def->u.node_def.self_off);           /* definee = self at base[-1] */
    }

    /* Every method body is its own AOT entry: call sites reach it through
     * body->head.dispatcher at runtime (specializer can't fold that). */
    code_repo_add(korb_sym_name(tc->c->vm, mid), body, true);
    return def;
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
    if (PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_PATH_NODE)) {
        const pm_node_t *const parent = ((const pm_constant_path_node_t *)cn->constant_path)->parent;
        path_owner = kp_intern_cpath(tc, parent);
        if (path_owner == 0) dyn_base = parent;
    }

    /* path base + superclass expression (both evaluated in the ENCLOSING scope)
     * → node_class's staged children; nil when absent. */
    NODE *base_node, *super_node;
    WITH_CHAIN(tc, 2, (base_node  = dyn_base ? transduce(tc, dyn_base) : ALLOC_node_lit(KORB_NIL),
                       super_node = cn->superclass ? transduce(tc, cn->superclass)
                                                   : ALLOC_node_lit(KORB_NIL)));

    push_frame(tc, &cn->locals);
    tc->frame->class_name_sym = name_sym;       /* for Module.nesting */
    NODE *body;
    if (cn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(cn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)cn->body);
    else
        body = transduce(tc, cn->body);   /* a begin/rescue/ensure body is just another node */
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, -1, NULL, 0, NULL, NULL, -1, 0);
    code_repo_add("class", entry, true);          /* its own AOT entry */
    NODE *_ncls = ALLOC_node_class(name_sym, entry, -1 - tc->chain - 2, path_owner, base_node, super_node);   /* self_off = enclosing self (base[-1]); -2 for the staged base+super children */
    korb_reg_srcloc(tc->c->vm, _ncls, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)cn));   /* Module#const_source_location */
    bake_add(tc, &_ncls->u.node_class.self_off);
    return _ncls;
}

extern const struct NodeKind kind_node_const_set;
static NODE *assign_target_from_synth(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local);

/* General multi-assign desugar: `lefts..., *rest, rights... = rhs`.  Runs the
 * same node_massign_splat the all-local fast path uses, but into synth locals,
 * then plumbs each synth out to its real target — so every target kind (local at
 * any depth / @ivar / CONST / $g / recv.x= / recv[k]= / nested group) inherits
 * the node's #to_ary coercion and CRuby's nil padding.  `rest` may be NULL or an
 * implicit rest (no splat: the extras are collected into a throwaway synth and
 * dropped), or a splat whose expression is NULL (anonymous `*`).  The result
 * value is the original rhs, as a multi-assign expression yields.  Returns NULL
 * if some target is unsupported.  `rhs` must be built at the current chain. */
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
    if (!synth || !offs) abort();
    for (uint32_t k = 0; k < no; k++) { synth[k] = alloc_synth_local(tc); offs[k] = (int32_t)synth[k] - tc->chain; }
    NODE *massign = ALLOC_node_massign_splat(offs, npre, npost, rhs);
    for (uint32_t k = 0; k < no; k++) bake_add(tc, &offs[k]);
    const uint32_t rettmp = alloc_synth_local(tc);          /* hold the massign value across the plumbing */
    NODE *acc = bake_lset(tc, rettmp, massign);
    for (uint32_t i = 0; i < no; i++) {
        const pm_node_t *const t = i < npre  ? lefts->nodes[i]
                                 : i == npre ? splat_t      /* NULL for `*` / no splat → nothing to plumb */
                                             : rights->nodes[i - npre - 1u];
        if (!t) continue;
        NODE *const a = assign_target_from_synth(tc, t, synth[i]);
        if (!a) { free(synth); return NULL; }
        acc = ALLOC_node_seq(acc, a);
    }
    free(synth);                                           /* offs stays: the node owns it */
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
assign_target_from_synth(struct kp_ctx *tc, const pm_node_t *t, uint32_t src_local)
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
        NODE *r, *k, *v;
        WITH_CHAIN(tc, KP_SEND2_SC, (r = transduce(tc, cpt->parent),
                                     k = ALLOC_node_lit(ID2SYM(kp_intern_cid(tc, cpt->name))),
                                     v = bake_lget(tc, src_local)));
        return kp_send2(korb_intern(tc->c->vm, "const_set", 9), line, r, k, v);
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
        NODE *r, *v;
        WITH_CHAIN(tc, KP_SEND1_SC, (r = transduce(tc, ct->receiver), v = bake_lget(tc, src_local)));
        return kp_send1(kp_intern_cid(tc, ct->name), line, r, v);
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
            return ALLOC_node_send_splat_aset(aset, line, r, arr);
        }
        NODE **const av = malloc(sizeof(NODE *) * (na + 1u));
        if (!av) abort();
        const int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)KP_SENDN_SC(na + 1u);
        NODE *const r = transduce(tc, it->receiver);
        for (uint32_t i = 0; i < na; i++) av[i] = transduce(tc, ia[i]);
        av[na] = bake_lget(tc, src_local);
        tc->chain = saved;
        NODE *const s = kp_send_n(aset, line, r, av, na + 1u);
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
        get = ALLOC_node_send_splat(aref, line, g_recv, g_arr);
        val = transduce(tc, value);
        newval = logic ? (logic == 1 ? ALLOC_node_or(get, val) : ALLOC_node_and(get, val))
               : (op != KP_BINOP_NONE) ? alloc_binop(op, get, val, line)
               : kp_send1(opmid, line, get, val);
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
    NODE *const store = ALLOC_node_send_splat(aset, line, s_recv, s_arr);
    if (logic) {
        /* ||= assigns only when the read was falsy, &&= only when it was truthy —
         * re-test the computed value against the original read so a no-op stays
         * a no-op (`h[*k] &&= v` must not create the key). */
        NODE *guard;
        { const int32_t _s = tc->chain; tc->chain = _s + 2;
          NODE *g2_recv = bake_lget(tc, t0), *g2_arr = bake_lget(tc, ta);
          tc->chain = _s;
          guard = ALLOC_node_send_splat(aref, line, g2_recv, g2_arr); }
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
static NODE *
build_fwd_kwargs(struct kp_ctx *tc, struct korb_kw_info *kw, uint32_t n)
{
    if (n == 0) {                                        /* base: {} (+ **kwrest merged on top) */
        NODE *h = ALLOC_node_hash_new(kw->count);
        if (kw->kwrest_slot >= 0) {
            NODE *src; const uint32_t sc = kind_node_hash_merge.slot_count;
            WITH_CHAIN(tc, sc, (src = bake_lget(tc, kw->kwrest_slot)));
            h = ALLOC_node_hash_merge(h, src);
        }
        return h;
    }
    const uint32_t sc = kind_node_hash_set.slot_count;
    NODE *acc, *key, *val;
    WITH_CHAIN(tc, sc, (acc = build_fwd_kwargs(tc, kw, n - 1),
                        key = ALLOC_node_lit(ID2SYM(kw->entries[n - 1].mid)),
                        val = bake_lget(tc, kw->entries[n - 1].slot)));
    return ALLOC_node_hash_set(acc, key, val);
}

/* Build [pos0..pos_{np-1}, *rest, post0..post_{pc-1}(, {kwargs})] as an Array NODE — the
 * argument list a bare `super` forwards (positional params, the current rest
 * array splatted, then post params).  Inside-out like build_array; `total` =
 * np + (rest?1:0) + pc, indexing the segments in that order. */
static NODE *
build_fwd_args(struct kp_ctx *tc, uint32_t np, int32_t rest_slot, uint32_t post_base, uint32_t pc,
               struct korb_kw_info *kw, uint32_t total)
{
    if (total == 0) return ALLOC_node_array_new(0);
    const uint32_t bi = total - 1;                        /* build index of the last element */
    const uint32_t has_rest = (rest_slot >= 0) ? 1u : 0u;
    const bool has_kw = (kw && (kw->count || kw->kwrest_slot >= 0));
    const uint32_t sc = kind_node_ary_push.slot_count;
    if (has_kw && bi == np + has_rest + pc) {             /* trailing kwargs hash element */
        NODE *acc, *kwh;
        WITH_CHAIN(tc, sc, (acc = build_fwd_args(tc, np, rest_slot, post_base, pc, kw, total - 1),
                            kwh = build_fwd_kwargs(tc, kw, kw->count)));
        return ALLOC_node_ary_push(acc, ALLOC_node_kwargs_mark(kwh));   /* forwarded as keywords */
    }
    bool is_rest = false; int32_t slot;
    if (bi < np)                          slot = (int32_t)bi;                          /* positional */
    else if (has_rest && bi == np)      { slot = rest_slot; is_rest = true; }          /* *rest (splat) */
    else                                  slot = (int32_t)(post_base + (bi - np - has_rest));  /* post */
    NODE *acc, *elem;
    WITH_CHAIN(tc, sc, (acc  = build_fwd_args(tc, np, rest_slot, post_base, pc, kw, total - 1),
                        elem = bake_lget(tc, slot)));
    return is_rest ? ALLOC_node_ary_concat(acc, elem) : ALLOC_node_ary_push(acc, elem);
}

static NODE *
emit_super_fwd(struct kp_ctx *tc, uint32_t m_mid, uint32_t line, NODE *arr)
{
    tc->frame->uses_block = true;                         /* reserve the block trio */
    int32_t soff = -1 - tc->chain - 1, dco = -1 - tc->chain - 1;
    int32_t bo = -4 - tc->chain - 1, deo = -3 - tc->chain - 1, cso = -2 - tc->chain - 1;
    NODE *_s = ALLOC_node_super_fwd(m_mid, line, soff, dco, bo, deo, cso, arr);
    bake_add(tc, &_s->u.node_super_fwd.self_off);
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
    if (PM_NODE_TYPE_P(mn->constant_path, PM_CONSTANT_PATH_NODE)) {
        const pm_node_t *const parent = ((const pm_constant_path_node_t *)mn->constant_path)->parent;
        path_owner = kp_intern_cpath(tc, parent);
        if (path_owner == 0) dyn_base = parent;
    }
    /* the base expression is evaluated in the ENCLOSING scope → staged child */
    NODE *base_node;
    WITH_CHAIN(tc, 1, (base_node = dyn_base ? transduce(tc, dyn_base) : ALLOC_node_lit(KORB_NIL)));
    push_frame(tc, &mn->locals);
    tc->frame->class_name_sym = name_sym;       /* for Module.nesting */
    NODE *body;
    if (mn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(mn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)mn->body);
    else
        body = transduce(tc, mn->body);   /* a begin/rescue/ensure body is just another node */
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, -1, NULL, 0, NULL, NULL, -1, 0);
    code_repo_add("module", entry, true);
    NODE *_nmod = ALLOC_node_module(name_sym, entry, -1 - tc->chain - 1, path_owner, base_node);   /* self_off = enclosing self (base[-1]); -1 for the staged base child */
    korb_reg_srcloc(tc->c->vm, _nmod, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), kp_line(tc, (const pm_node_t *)mn));   /* Module#const_source_location */
    bake_add(tc, &_nmod->u.node_module.self_off);
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
        const int32_t ars = tc->frame->anon_rest_slot;
        if (ars < 0) return build_array(tc, elems, n - 1, capa);   /* no anon rest in scope → nothing */
        NODE *acc0, *elem0;
        uint32_t sc0 = kind_node_ary_push.slot_count;
        WITH_CHAIN(tc, sc0, (acc0  = build_array(tc, elems, n - 1, capa),
                             elem0 = bake_lget(tc, (uint32_t)ars)));   /* splat the collected rest array */
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
build_array_with_fwd(struct kp_ctx *tc, struct pm_node **elems, size_t n)
{
    NODE *acc, *fwd;
    uint32_t sc = kind_node_ary_push.slot_count;
    WITH_CHAIN(tc, sc, (acc = build_array(tc, elems, n, (uint32_t)n),
                        fwd = bake_lget(tc, (uint32_t)tc->frame->fwd_slot)));
    return ALLOC_node_ary_concat(acc, fwd);
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
        if (f->method_mid || f->class_name_sym || f->anon_class_body) break;   /* scope barrier */
    }
    uint32_t total = 0;
    for (uint32_t d = 0; d < L; d++) {
        const pm_constant_id_list_t *ls = levels[d]->locals;
        total += (uint32_t)ls->size + ((levels[d]->it_param && ls->size == 0) ? 1u : 0u);
    }
    uint32_t *tbl = malloc(sizeof(uint32_t) * (1 + L + 3 * (total ? total : 1)));   /* immortal */
    if (!tbl) abort();
    tbl[0] = L;
    uint32_t cnt = 0;
    for (uint32_t d = 0; d < L; d++) {
        const pm_constant_id_list_t *ls = levels[d]->locals;
        const bool it_slot = levels[d]->it_param && ls->size == 0;
        tbl[1 + d] = (uint32_t)ls->size + (it_slot ? 1u : 0u);
        for (uint32_t i = 0; i < tbl[1 + d]; i++) {
            const uint32_t sym = (it_slot && i == 0)
                ? korb_intern(tc->c->vm, "it", 2)
                : kp_intern_cid(tc, ls->ids[i - (it_slot ? 1u : 0u)]);
            bool shadowed = false;                       /* inner name wins */
            for (uint32_t k = 0; k < cnt; k++)
                if (tbl[1 + L + 3 * k] == sym) { shadowed = true; break; }
            if (shadowed) continue;
            tbl[1 + L + 3 * cnt]     = sym;
            tbl[1 + L + 3 * cnt + 1] = d;
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
kp_make_binding_node(struct kp_ctx *tc, uint32_t line)
{
    uint32_t cnt = 0;
    uint32_t *tbl = kp_binding_scope_tbl(tc, &cnt);
    const int32_t self_off = -1 - tc->chain;
    NODE *nb = ALLOC_node_binding(-tc->chain, self_off, (const char *)(const void *)tbl, cnt);
    bake_add(tc, &nb->u.node_binding.def_env_off);    /* frame base shifts with frame_size */
    bake_add(tc, &nb->u.node_binding.self_off);       /* self at base[-1] (bottom header) */
    korb_reg_srcloc(tc->c->vm, nb, korb_intern(tc->c->vm, tc->fname, (uint32_t)strlen(tc->fname)), line);  /* for Binding#source_location */
    return nb;
}

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
            const int32_t aks = tc->frame->anon_kwrest_slot;
            if (aks < 0) return build_hash(tc, assocs, n - 1, capa);   /* no anon kwrest in scope → nothing */
            NODE *acc0, *src0;
            uint32_t sc0 = kind_node_hash_merge.slot_count;
            WITH_CHAIN(tc, sc0, (acc0 = build_hash(tc, assocs, n - 1, capa),
                                 src0 = bake_lget(tc, (uint32_t)aks)));
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

/* Interpolated string `"a#{x}b"` → inside-out concat chain (same shape as
 * build_array).  The accumulator is always a String; each part is appended
 * via its to_s inside node_dstr_concat. */
static NODE *
build_dstr(struct kp_ctx *tc, struct pm_node **parts, size_t n)
{
    if (n == 0) return ALLOC_node_str("", 0);
    NODE *acc, *part;
    uint32_t sc = kind_node_dstr_concat.slot_count;
    WITH_CHAIN(tc, sc, (acc  = build_dstr(tc, parts, n - 1),
                        part = transduce(tc, parts[n - 1])));
    return ALLOC_node_dstr_concat(acc, part);
}

/* Bake the lexically-enclosing class/module names (outermost→innermost) into a
 * constant node's cache, so the node resolves its cref by walking the chain
 * owner-scoped instead of picking the first same-named module in the table. */
static void
bake_cref_chain(struct kp_ctx *tc, struct korb_constcache *cache)
{
    uint32_t depth = 0;
    for (struct kp_frame *f = tc->frame; f; f = f->prev)
        if (f->class_name_sym != 0) depth++;
    if (depth == 0) return;
    uint32_t *const chain = malloc(sizeof(uint32_t) * depth);   /* immortal (compile-time) */
    if (chain == NULL) return;
    uint32_t i = depth;                                         /* fill innermost→outermost; store outermost at [0] */
    for (struct kp_frame *f = tc->frame; f && i > 0; f = f->prev)
        if (f->class_name_sym != 0) chain[--i] = f->class_name_sym;
    cache->owner_chain = chain;
    cache->chain_len = depth;
}

/* Build a bare constant read (`NAME`) node, resolving via the cref owner + the
 * baked lexical enclosing chain — same as the PM_CONSTANT_READ_NODE case.  Used
 * to synthesize the read side of `NAME op= v` op-assignments. */
static NODE *
build_const_read(struct kp_ctx *tc, uint32_t name_cid)
{
    /* `class << obj` bodies have an unnamed cref: pass self so the read can look
     * in the singleton class itself. */
    /* An UNNAMED cref (a `class << obj` body, or a method defined in one) can only
     * be found at runtime: self IS the class in the body, and in a method the
     * frame's method-entry cell names it.  The entry cell is only meaningful in a
     * method frame, so it is baked only there. */
    const bool anon = tc->frame->anon_class_body || tc->frame->anon_cref_method;
    NODE *cn = ALLOC_node_const(name_cid, kp_cref_owner(tc), anon ? -1 - tc->chain : INT32_MIN,
                                tc->frame->anon_cref_method ? -1 - tc->chain : INT32_MIN);
    if (anon) bake_add(tc, &cn->u.node_const.self_off);   /* self at base[-1]; dc_off stays cursor-relative */
    bake_cref_chain(tc, &cn->u.node_const.cache);
    return cn;
}

/* Build a bare constant write (`NAME = v`) node with the same baked chain: the
 * owner must be the lexical module, not whichever module of that name happens
 * to come first in the table. */
static NODE *
build_const_set(struct kp_ctx *tc, uint32_t name_cid, NODE *val)
{
    const bool anon = tc->frame->anon_class_body;
    NODE *cn = ALLOC_node_const_set(name_cid, tc->frame->class_name_sym,
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

/* Resolve the owner module name for a `PARENT::NAME` constant-path target with a
 * STATIC parent (a constant read or nested constant path).  Returns true and sets
 * *owner_name; returns false for a dynamic parent (`expr::NAME`, unsupported for
 * op-assign) — matching the owner resolution in PM_CONSTANT_PATH_WRITE_NODE. */
static bool
const_path_static_owner(struct kp_ctx *tc, const pm_node_t *parent, uint32_t frame_default, uint32_t *owner_name)
{
    if (parent == NULL) { *owner_name = frame_default; return true; }   /* `::X` — top-level frame default */
    if (PM_NODE_TYPE_P(parent, PM_CONSTANT_READ_NODE)) { *owner_name = kp_intern_cid(tc, ((const pm_constant_read_node_t *)parent)->name); return true; }
    if (PM_NODE_TYPE_P(parent, PM_CONSTANT_PATH_NODE)) { *owner_name = kp_intern_cid(tc, ((const pm_constant_path_node_t *)parent)->name); return true; }
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
        /* a \u escape makes the literal UTF-8 whatever the file's encoding is (CRuby) */
        uint8_t lenc = tc->src_enc;
        if (lenc != KORB_ENC_UTF8) {
            const char *const src = (const char *)sn->base.location.start;
            const size_t slen = (size_t)(sn->base.location.end - sn->base.location.start);
            for (size_t i = 0; i + 1 < slen; i++)
                if (src[i] == '\\' && src[i + 1] == 'u') { lenc = KORB_ENC_UTF8; break; }
        }
        if (lenc != KORB_ENC_UTF8) return ALLOC_node_str_enc(bytes, len, lenc);
        if (sn->base.flags & PM_STRING_FLAGS_FROZEN) return ALLOC_node_str_frozen(bytes, len);   /* # frozen_string_literal: true */
        return ALLOC_node_str(bytes, len);
      }
      case PM_REGULAR_EXPRESSION_NODE: {   /* /pat/ → Regexp (matching via astrogre) */
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
        return ALLOC_node_regexp(bytes, len, flags);
      }
      case PM_SYMBOL_NODE: {
        const pm_symbol_node_t *sn = (const pm_symbol_node_t *)node;
        size_t len = pm_string_length(&sn->unescaped);
        uint32_t id = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), len);
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
        if (nm == UINT32_MAX || om == UINT32_MAX) return kp_unsupported(tc, node, "alias with a dynamic-symbol name");
        NODE *na = ALLOC_node_alias(nm, om, -1 - tc->chain);   /* self (the class) at base[-1] */
        bake_add(tc, &na->u.node_alias.self_off);
        return na;
      }
      case PM_UNDEF_NODE: {          /* undef foo, bar — retire methods on the enclosing class */
        const pm_undef_node_t *un = (const pm_undef_node_t *)node;
        const uint32_t cnt = (uint32_t)un->names.size;
        uint32_t *mids = malloc(sizeof(uint32_t) * (cnt ? cnt : 1));   /* immortal (baked into the node) */
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
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = ALLOC_node_const(korb_intern(tc->c->vm, "Encoding", 8), 0, INT32_MIN, INT32_MIN),
                                     arg = ALLOC_node_str(nm, (uint32_t)strlen(nm))));
        return kp_send1(korb_intern(tc->c->vm, "find", 4), kp_line(tc, node), recv, arg);
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
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = bake_ivar_get(tc, name),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = bake_ivar_get(tc, name),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(opmid, line, lhs, rhs);
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
            comb = alloc_binop(op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = bake_cvar_get(tc, name, 0),
                                         rhs = transduce(tc, ow->value)));
            comb = kp_send1(opmid, line, lhs, rhs);
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
        return build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_KEYWORD_HASH_NODE: {       /* trailing `k: v` / `**h` args → a Hash (becomes kwargs) */
        const pm_keyword_hash_node_t *hn = (const pm_keyword_hash_node_t *)node;
        size_t cnt = hn->elements.size;
        NODE *const h = build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
        return ALLOC_node_kwargs_mark(h);   /* the callee binds a tagged Hash to keywords */
      }

      case PM_X_STRING_NODE: {          /* `cmd` → self.`("cmd") */
        const pm_x_string_node_t *xn = (const pm_x_string_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&xn->unescaped, &len);
        const uint32_t bt = korb_intern(tc->c->vm, "`", 1);
        NODE *recv, *arg;
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = bake_self(tc),
                                     arg = ALLOC_node_str(bytes, len)));
        return kp_send1(bt, kp_line(tc, node), recv, arg);
      }
      case PM_INTERPOLATED_X_STRING_NODE: {   /* `cmd #{x}` → self.`(dstr) */
        const pm_interpolated_x_string_node_t *xn = (const pm_interpolated_x_string_node_t *)node;
        const uint32_t bt = korb_intern(tc->c->vm, "`", 1);
        NODE *recv, *arg;
        WITH_CHAIN(tc, KP_SEND1_SC, (recv = bake_self(tc),
                                     arg = build_dstr(tc, xn->parts.nodes, xn->parts.size)));
        return kp_send1(bt, kp_line(tc, node), recv, arg);
      }
      case PM_INTERPOLATED_STRING_NODE: {
        const pm_interpolated_string_node_t *in = (const pm_interpolated_string_node_t *)node;
        return build_dstr(tc, in->parts.nodes, in->parts.size);
      }
      case PM_INTERPOLATED_SYMBOL_NODE: {     /* :"...#{ }..." → dstr.to_sym */
        const pm_interpolated_symbol_node_t *in = (const pm_interpolated_symbol_node_t *)node;
        uint32_t to_sym = korb_intern(tc->c->vm, "to_sym", 6);
        NODE *str;
        WITH_CHAIN(tc, KP_SEND0_SC, (str = build_dstr(tc, in->parts.nodes, in->parts.size)));
        return kp_send0(to_sym, kp_line(tc, node), str);
      }
      case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {   /* /...#{ }.../ → Regexp(dstr, flags) */
        extern const struct NodeKind kind_node_regexp_dyn;
        const pm_interpolated_regular_expression_node_t *in = (const pm_interpolated_regular_expression_node_t *)node;
        uint32_t flags = in->base.flags & (PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE |
                                           PM_REGULAR_EXPRESSION_FLAGS_EXTENDED |
                                           PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE |
                                           PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT |
                                           PM_REGULAR_EXPRESSION_FLAGS_EUC_JP |
                                           PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J |
                                           PM_REGULAR_EXPRESSION_FLAGS_UTF_8);
        NODE *src;
        WITH_CHAIN(tc, kind_node_regexp_dyn.slot_count, (src = build_dstr(tc, in->parts.nodes, in->parts.size)));
        return ALLOC_node_regexp_dyn(flags, src);
      }
      case PM_EMBEDDED_STATEMENTS_NODE: {
        const pm_embedded_statements_node_t *en = (const pm_embedded_statements_node_t *)node;
        if (!en->statements) return ALLOC_node_lit(KORB_NIL);   /* #{} → "" via nil.to_s */
        return transduce_statements(tc, en->statements);
      }

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
                } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_CONSTANT_TARGET_NODE)) {
                    all_local = false;
                } else if (PM_NODE_TYPE_P(t, PM_CALL_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_INDEX_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_GLOBAL_VARIABLE_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_CONSTANT_PATH_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_MULTI_TARGET_NODE)) {   /* nested `(a,b)` / $g / M::X / recv.x= / recv[k]= → general desugar */
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
            comb = alloc_binop(op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(opmid, line, lhs, rhs);
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
        NODE *get = kp_send_n(aref, line, g_recv, g_k, (uint32_t)argc);
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
          set = kp_send_n(aset, line, s_recv, s_args, (uint32_t)(argc + 1));
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
            get = kp_send_n(aref, line, g_recv, g_k, (uint32_t)argc);
            free(g_k);
            val = transduce(tc, iw->value);
            newval = (op != KP_BINOP_NONE) ? alloc_binop(op, get, val, line) : kp_send1(opmid, line, get, val);
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
          set = kp_send_n(aset, line, s_recv, s_args, (uint32_t)(argc + 1));
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
            get = kp_send0(read_mid, line, g_recv);
            val = transduce(tc, cw->value);
            newval = (op != KP_BINOP_NONE) ? alloc_binop(op, get, val, line) : kp_send1(opmid, line, get, val);
        }));
        NODE *store_newval = bake_lset(tc, t1, newval);
        /* recv.attr = t1 */
        NODE *s_recv, *s_val;
        WITH_CHAIN(tc, KP_SEND1_SC, (s_recv = bake_lget(tc, t0), s_val = bake_lget(tc, t1)));
        NODE *set = kp_send1(write_mid, line, s_recv, s_val);
        return ALLOC_node_seq(store_recv, ALLOC_node_seq(store_newval,
                   ALLOC_node_seq(set, bake_lget(tc, t1))));
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
        NODE *get = kp_send0(read_mid, line, g_recv);
        NODE *store_val = bake_lset(tc, t1, transduce(tc, cw->value));   /* only the taken branch runs it */
        NODE *s_recv, *s_val;
        WITH_CHAIN(tc, KP_SEND1_SC, (s_recv = bake_lget(tc, t0), s_val = bake_lget(tc, t1)));
        NODE *set = kp_send1(write_mid, line, s_recv, s_val);
        NODE *set_branch = ALLOC_node_seq(store_val, ALLOC_node_seq(set, bake_lget(tc, t1)));
        return ALLOC_node_seq(store_recv, is_or ? ALLOC_node_or(get, set_branch)
                                                : ALLOC_node_and(get, set_branch));
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
                        NODE *test0 = kp_send0(korb_intern(tc->c->vm, "__korb_when_splat_truthy", 24), line, recv0);
                        cond = cond ? ALLOC_node_or(cond, test0) : test0;
                        continue;
                    }
                    NODE *recv, *subj_arg;
                    WITH_CHAIN(tc, KP_SEND1_SC, (recv = transduce(tc, sp->expression),
                                                 subj_arg = bake_lget(tc, tmp)));
                    NODE *test = kp_send1(korb_intern(tc->c->vm, "__korb_when_splat", 17), line, recv, subj_arg);
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
        NODE *entry = transduce_block_parts(tc, &ln->locals, ln->parameters, ln->body);
        if (entry->head.kind != &kind_node_entry) return entry;   /* unsupported params → propagate (don't reify a non-entry) */
        korb_reg_srcloc(tc->c->vm, entry, korb_intern(tc->c->vm, tc->fname, strlen(tc->fname)), kp_line(tc, node));   /* Proc#source_location */
        int32_t self_off = -1 - tc->chain;
        NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, 1u);
        bake_add(tc, &mk->u.node_make_proc.def_env_off);
        bake_add(tc, &mk->u.node_make_proc.self_off);   /* self at base[-1] */
        return mk;
      }
      case PM_MATCH_WRITE_NODE: {
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
            NODE *md1 = ALLOC_node_const(lastmatch, 0, INT32_MIN, INT32_MIN);
            NODE *md2 = ALLOC_node_const(lastmatch, 0, INT32_MIN, INT32_MIN);
            NODE *sym = ALLOC_node_lit(ID2SYM(kp_intern_cid(tc, lt->name)));
            NODE *val = ALLOC_node_and(md1, kp_send1(aref, line, md2, sym));
            seq = ALLOC_node_seq(seq, lvar_write(tc, t, lt->name, lt->depth, val));
        }
        return ALLOC_node_seq(seq, bake_lget(tc, tmp));
      }
      case PM_MATCH_PREDICATE_NODE: {                /* `expr in pattern` → bool */
        const pm_match_predicate_node_t *mp = (const pm_match_predicate_node_t *)node;
        struct korb_pat *desc = build_pattern_desc(tc, mp->pattern);
        NODE *subj = transduce(tc, mp->value);
        return ALLOC_node_match_pred(subj, (void *)desc);
      }
      case PM_MATCH_REQUIRED_NODE: {                 /* `expr => pattern` → nil / raise */
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
        while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
        if (mf->method_mid == 0) depth = 0;
        if (depth == 0) {
            return ALLOC_node_return(kp_jump_args_value(tc, rn->arguments));
        }
        /* node_return_outer reads this block frame's PREV cell (prev_off, like
         * node_ivar_set's self_off — a VALUE @child reserves no sibling slot). */
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
            return ALLOC_node_defined(4, 0, 0);                          /* "local-variable" */
        if (PM_NODE_TYPE_P(v, PM_CONSTANT_READ_NODE))
            /* self_off: inside a module body self is the module, which is where a
             * pending autoload for this name would be registered */
            { NODE *_d = ALLOC_node_defined(1, kp_intern_cid(tc, ((const pm_constant_read_node_t *)v)->name), self_off);
              bake_add(tc, &_d->u.node_defined.self_off); return _d; }
        if (PM_NODE_TYPE_P(v, PM_CONSTANT_PATH_NODE)) {             /* `A::B` */
            const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)v;
            if (cp->parent && PM_NODE_TYPE_P(cp->parent, PM_CONSTANT_READ_NODE))   /* static `A::B` → owner-aware check */
                return ALLOC_node_defined_cpath(kp_intern_cid(tc, ((const pm_constant_read_node_t *)cp->parent)->name),
                                                kp_intern_cid(tc, cp->name));
            if (cp->parent != NULL)                                  /* nested/dynamic parent → evaluate it, then probe */
                return ALLOC_node_defined_cpath_dyn(transduce(tc, cp->parent), kp_intern_cid(tc, cp->name));
            { NODE *_d = ALLOC_node_defined(1, kp_intern_cid(tc, cp->name), self_off);   /* `::TOP` → flat probe */
              bake_add(tc, &_d->u.node_defined.self_off); return _d; }
        }
        if (PM_NODE_TYPE_P(v, PM_INSTANCE_VARIABLE_READ_NODE))
            { NODE *_d = ALLOC_node_defined(2, kp_intern_cid(tc, ((const pm_instance_variable_read_node_t *)v)->name), self_off); bake_add(tc, &_d->u.node_defined.self_off); return _d; }
        if (PM_NODE_TYPE_P(v, PM_GLOBAL_VARIABLE_READ_NODE)) {
            const uint32_t gn = kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)v)->name);
            if (gn == korb_intern(tc->c->vm, "$~", 2) ||        /* always-defined specials */
                gn == korb_intern(tc->c->vm, "$!", 2) ||
                gn == korb_intern(tc->c->vm, "$@", 2))
                return ALLOC_node_defined(12, 200, 0);
            return ALLOC_node_defined(3, gn, 0);
        }
        if (PM_NODE_TYPE_P(v, PM_NUMBERED_REFERENCE_READ_NODE))          /* $1..$9 → gv iff that group matched */
            return ALLOC_node_defined(12, 100u + ((const pm_numbered_reference_read_node_t *)v)->number, 0);
        if (PM_NODE_TYPE_P(v, PM_BACK_REFERENCE_READ_NODE)) {            /* $& $` $' $+ → gv iff non-nil */
            const uint32_t nm = kp_intern_cid(tc, ((const pm_back_reference_read_node_t *)v)->name);
            uint32_t rk = 0;
            if      (nm == korb_intern(tc->c->vm, "$`", 2)) rk = 1;
            else if (nm == korb_intern(tc->c->vm, "$'", 2)) rk = 2;
            else if (nm == korb_intern(tc->c->vm, "$+", 2)) rk = 3;
            return ALLOC_node_defined(12, rk, 0);
        }
        if (PM_NODE_TYPE_P(v, PM_CALL_NODE)) {                           /* method call */
            const pm_call_node_t *cn = (const pm_call_node_t *)v;
            if (cn->receiver != NULL) {                                  /* recv.meth: eval recv, check it responds */
                if (PM_NODE_TYPE_P(cn->receiver, PM_SELF_NODE)) {         /* self.meth sees private/protected too */
                    NODE *_d = ALLOC_node_defined(0, kp_intern_cid(tc, cn->name), self_off); bake_add(tc, &_d->u.node_defined.self_off); return _d;
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
            NODE *_d = ALLOC_node_defined(0, kp_intern_cid(tc, cn->name), self_off); bake_add(tc, &_d->u.node_defined.self_off); return _d;
        }
        if (PM_NODE_TYPE_P(v, PM_YIELD_NODE)) {   /* "yield" iff the enclosing METHOD got a block */
            struct kp_frame *mf = tc->frame;
            uint32_t depth = 0;
            while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
            if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }
            mf->uses_block = true;
            if (depth == 0) return ALLOC_node_defined_yield(-4 - tc->chain);
            NODE *dy = ALLOC_node_defined_yield_outer(-2 - tc->chain, depth, -4);
            bake_add(tc, &dy->u.node_defined_yield_outer.prev_off);
            add_bake_to(mf, &dy->u.node_defined_yield_outer.trio_base);
            return dy;
        }
        if (PM_NODE_TYPE_P(v, PM_CLASS_VARIABLE_READ_NODE)) {            /* "class variable" iff present */
            NODE *_d = ALLOC_node_defined_cvar(-1 - tc->chain, -1 - tc->chain,
                                               kp_intern_cid(tc, ((const pm_class_variable_read_node_t *)v)->name));
            bake_add(tc, &_d->u.node_defined_cvar.self_off);
            return _d;
        }
        if (PM_NODE_TYPE_P(v, PM_SUPER_NODE) || PM_NODE_TYPE_P(v, PM_FORWARDING_SUPER_NODE)) {
            /* "super" iff the enclosing method has an MRO successor.  The frame's
             * self and entry cell are read at the same offsets node_super_fwd uses. */
            const uint32_t m_mid = tc->frame->method_mid;
            /* outside a method body (top level, or a block — koruby has no `super`
             * there yet): CRuby answers "super" from a block, we can only say nil. */
            if (m_mid == 0) return ALLOC_node_lit(KORB_NIL);
            NODE *_d = ALLOC_node_defined_super(m_mid, -1 - tc->chain, -1 - tc->chain);
            bake_add(tc, &_d->u.node_defined_super.self_off);   /* self at base[-1]; dc_off stays cursor-relative (as in node_super_fwd) */
            return _d;
        }
        if (PM_NODE_TYPE_P(v, PM_SELF_NODE))  return ALLOC_node_defined(6, 0, 0);   /* "self" */
        if (PM_NODE_TYPE_P(v, PM_NIL_NODE))   return ALLOC_node_defined(8, 0, 0);   /* "nil" */
        if (PM_NODE_TYPE_P(v, PM_TRUE_NODE))  return ALLOC_node_defined(9, 0, 0);   /* "true" */
        if (PM_NODE_TYPE_P(v, PM_FALSE_NODE)) return ALLOC_node_defined(10, 0, 0);  /* "false" */
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
            return ALLOC_node_defined(11, 0, 0);                        /* "assignment" (not evaluated) */
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
                WITH_CHAIN(tc, 1, (expr = ALLOC_node_defined(5, 0, 0)));
                return ALLOC_node_and(chain, expr);                         /* nil if any element is undefined */
            }
        }
        return ALLOC_node_defined(5, 0, 0);                             /* literals / expr → "expression" */
      }
      case PM_YIELD_NODE: {
        const pm_yield_node_t *yn = (const pm_yield_node_t *)node;
        uint32_t line = kp_line(tc, node);
        size_t yargc = yn->arguments ? yn->arguments->arguments.size : 0;
        /* `yield` reaches the enclosing METHOD's block, not a nested block's.
         * Walk up to the method frame (method_mid != 0), counting block frames. */
        struct kp_frame *mf = tc->frame;
        uint32_t depth = 0;
        while (mf->method_mid == 0 && mf->prev) { mf = mf->prev; depth++; }
        if (mf->method_mid == 0) { mf = tc->frame; depth = 0; }  /* yield outside a method: legacy path (raises at runtime) */
        mf->uses_block = true;                   /* the method reserves the block trio */

        /* `yield(*arr)` / `yield a, *b` — build the args Array and spread it at
         * the block call (one staged child = the array). */
        bool y_splat = false;
        for (size_t i = 0; i < yargc; i++)
            if (PM_NODE_TYPE_P(yn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { y_splat = true; break; }
        if (y_splat) {
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, yn->arguments->arguments.nodes, yargc, (uint32_t)yargc)));
            if (depth == 0)
                return ALLOC_node_yield_splat(line, -4 - (tc->chain + 1), -3 - (tc->chain + 1), -2 - (tc->chain + 1), arr);
            NODE *yo = ALLOC_node_yield_outer_splat(line, -2 - (tc->chain + 1), depth, -4, arr);
            bake_add(tc, &yo->u.node_yield_outer_splat.prev_off);
            add_bake_to(mf, &yo->u.node_yield_outer_splat.trio_base);
            return yo;
        }

        if (depth == 0) {                        /* yield at method top-level: read this frame's trio */
            if (yargc == 0)
                return ALLOC_node_yield0(line, -4 - tc->chain, -3 - tc->chain, -2 - tc->chain);
            if (yargc == 1) {
                NODE *a0;
                WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
                return ALLOC_node_yield1(line, -4 - (tc->chain + 1), -3 - (tc->chain + 1), -2 - (tc->chain + 1), a0);
            }
            NODE **argv = malloc(sizeof(NODE *) * yargc);          /* yield a, b, ... */
            if (!argv) abort();
            const int32_t saved = tc->chain;
            tc->chain = saved + (int32_t)yargc;
            for (size_t i = 0; i < yargc; i++) argv[i] = transduce(tc, yn->arguments->arguments.nodes[i]);
            tc->chain = saved;
            const int32_t off = tc->chain + (int32_t)yargc;
            return ALLOC_node_yield_n(line, -4 - off, -3 - off, -2 - off, argv, (uint32_t)yargc);
        }
        /* yield inside a block: trio_base = method frame_size - 4 (add-baked at
         * the method's pop); prev_off addresses this block frame's PREV cell. */
        if (yargc == 0) {
            NODE *yo = ALLOC_node_yield_outer0(line, -2 - tc->chain, depth, -4);
            bake_add(tc, &yo->u.node_yield_outer0.prev_off);     /* this-block fixup */
            add_bake_to(mf, &yo->u.node_yield_outer0.trio_base); /* += method frame_size */
            return yo;
        }
        if (yargc == 1) {
            NODE *a0;
            WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
            NODE *yo = ALLOC_node_yield_outer1(line, -2 - (tc->chain + 1), depth, -4, a0);
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
        NODE *yo = ALLOC_node_yield_outer_n(line, -2 - (tc->chain + (int32_t)yargc), depth, -4, argv, (uint32_t)yargc);
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
        return transduce_def_recv(tc, (const pm_def_node_t *)node, NULL);
      case PM_SINGLETON_CLASS_NODE: {     /* `class << recv; body; end` — run body with self = recv's singleton class */
        const pm_singleton_class_node_t *sc = (const pm_singleton_class_node_t *)node;
        /* recv expression evaluated in the ENCLOSING scope → node_sclass's staged child */
        NODE *recv_node;
        WITH_CHAIN(tc, 1, (recv_node = transduce(tc, sc->expression)));
        push_frame(tc, &sc->locals);
        tc->frame->anon_class_body = true;        /* self IS the (unnamed) singleton class here */
        NODE *body;
        if (sc->body == NULL)
            body = lit_nil();
        else if (PM_NODE_TYPE_P(sc->body, PM_STATEMENTS_NODE))
            body = transduce_statements(tc, (const pm_statements_node_t *)sc->body);
        else
            body = transduce(tc, sc->body);   /* a begin/rescue/ensure body is just another node */
        uint32_t frame_size = pop_frame(tc);
        NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, 0, NULL, -1, NULL, 0, NULL, NULL, -1, 0);
        code_repo_add("sclass", entry, true);       /* its own AOT entry */
        NODE *_sc = ALLOC_node_sclass(entry, -1 - tc->chain - 1, -1 - tc->chain - 1, recv_node);   /* -1 extra for the staged recv child */
        bake_add(tc, &_sc->u.node_sclass.self_off);
        return _sc;
      }
      case PM_CLASS_NODE:
        return transduce_class(tc, (const pm_class_node_t *)node);
      case PM_MODULE_NODE:
        return transduce_module(tc, (const pm_module_node_t *)node);
      case PM_CONSTANT_READ_NODE: {
        const pm_constant_read_node_t *cr = (const pm_constant_read_node_t *)node;
        NODE *cn = build_const_read(tc, kp_intern_cid(tc, cr->name));
        return cn;
      }
      case PM_CONSTANT_PATH_NODE: {       /* `A::B` — resolve B owned by the parent
                                           * namespace A (the parent's rightmost name,
                                           * resolved at runtime); so M::C finds M's C. */
        const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)node;
        if (cp->parent != NULL) {                    /* `A::B` — resolve the parent as an expression */
            NODE *par = transduce(tc, cp->parent);
            return ALLOC_node_const_path(kp_intern_cid(tc, cp->name), par);
        }
        return ALLOC_node_const(kp_intern_cid(tc, cp->name), 0, INT32_MIN, INT32_MIN);   /* `::TOP` */
      }

      /* Global variables `$x` reuse the flat const table — the `$` in the name
       * keeps them in a distinct namespace from constants.  Unset reads → nil. */
      case PM_GLOBAL_VARIABLE_READ_NODE: {
        const uint32_t gn = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)node)->name));
        if (gn == korb_intern(tc->c->vm, "$!", 2)) return ALLOC_node_errinfo();   /* $! = current exception */
        if (gn == korb_intern(tc->c->vm, "$@", 2)) return ALLOC_node_errinfo_bt(0);   /* $@ = $!.backtrace */
        if (gn == korb_intern(tc->c->vm, "$&", 2)) return ALLOC_node_backref(0);  /* alias 経由 ($MATCH 等) */
        if (gn == korb_intern(tc->c->vm, "$`", 2)) return ALLOC_node_backref(1);
        if (gn == korb_intern(tc->c->vm, "$'", 2)) return ALLOC_node_backref(2);
        if (gn == korb_intern(tc->c->vm, "$+", 2)) return ALLOC_node_backref(3);
        return ALLOC_node_const(gn, 0, INT32_MIN, INT32_MIN);
      }
      case PM_NUMBERED_REFERENCE_READ_NODE: {   /* $1..$9 → group n of the last match ($~) */
        const uint32_t num = ((const pm_numbered_reference_read_node_t *)node)->number;
        return ALLOC_node_backref(100u + num);
      }
      case PM_BACK_REFERENCE_READ_NODE: {       /* $& / $` / $' / $+ */
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
            == korb_intern(tc->c->vm, "$@", 2)) {                /* `$@ = v` → $!.set_backtrace(v) */
            /* the value is an @child evaluated at this node's own cursor
             * (slot_count 0), so the chain must NOT shift */
            NODE *v = transduce(tc, ((const pm_global_variable_write_node_t *)node)->value);
            return ALLOC_node_errinfo_bt_set(v);
        }
        const pm_global_variable_write_node_t *gw = (const pm_global_variable_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
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
                       (lhs = ALLOC_node_const(name, 0, INT32_MIN, INT32_MIN), rhs = transduce(tc, gw->value)));
            alloc_binop(op, lhs, rhs, line);
        }));
        return build_const_set(tc, name, binop);
      }
      case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {          /* `$x ||= v` (globals live in the const table) */
        const pm_global_variable_or_write_node_t *gw = (const pm_global_variable_or_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        return ALLOC_node_or(ALLOC_node_const(name, 0, INT32_MIN, INT32_MIN), build_const_set(tc, name, val));
      }
      case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: {         /* `$x &&= v` */
        const pm_global_variable_and_write_node_t *gw = (const pm_global_variable_and_write_node_t *)node;
        uint32_t name = (kp_gvar_alias_seed(tc), kp_gvar_resolve)(kp_intern_cid(tc, gw->name));
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        return ALLOC_node_and(ALLOC_node_const(name, 0, INT32_MIN, INT32_MIN), build_const_set(tc, name, val));
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
        NODE *guarded = ALLOC_node_and(ALLOC_node_defined(1, name, 0), build_const_read(tc, name));
        return ALLOC_node_or(guarded, set);
      }
      case PM_CONSTANT_AND_WRITE_NODE: { /* `X &&= v` — read (raises if undefined), assign when truthy */
        const pm_constant_and_write_node_t *aw = (const pm_constant_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->name);
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, aw->value)));
        NODE *set = build_const_set(tc, name, val);
        return ALLOC_node_and(build_const_read(tc, name), set);
      }
      case PM_CONSTANT_OPERATOR_WRITE_NODE: {  /* `X op= v` → X = X op v (read raises if undefined) */
        const pm_constant_operator_write_node_t *ow = (const pm_constant_operator_write_node_t *)node;
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, ow->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t name = kp_intern_cid(tc, ow->name), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = build_const_read(tc, name), rhs = transduce(tc, ow->value)));
            (op != KP_BINOP_NONE) ? alloc_binop(op, lhs, rhs, line) : kp_send1(opmid, line, lhs, rhs);
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
        else if (parent && PM_NODE_TYPE_P(parent, PM_CONSTANT_PATH_NODE))
            owner_name = kp_intern_cid(tc, ((const pm_constant_path_node_t *)parent)->name);   /* rightmost of the parent path */
        else if (parent) {   /* dynamic owner (`expr::N = v`, e.g. a local holding an anon module) → expr.const_set(:N, v) so N nests under the runtime owner */
            const uint32_t line = kp_line(tc, node);
            NODE *r, *k, *v;
            WITH_CHAIN(tc, KP_SEND2_SC, (r = transduce(tc, parent),
                                         k = ALLOC_node_lit(ID2SYM(name)),
                                         v = transduce(tc, cpw->value)));
            return kp_send2(korb_intern(tc->c->vm, "const_set", 9), line, r, k, v);
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
        if (!const_path_static_owner(tc, ow->target->parent, tc->frame->class_name_sym, &owner))
            return kp_unsupported(tc, node, "constant-path ||= with a dynamic module part");
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, ow->value)));
        NODE *set = ALLOC_node_const_set(name, owner, INT32_MIN, val);
        /* guard the read with a flat defined? probe of the rightmost name (no NameError) */
        NODE *guarded = ALLOC_node_and(ALLOC_node_defined(1, name, 0), ALLOC_node_const(name, owner, INT32_MIN, INT32_MIN));
        return ALLOC_node_or(guarded, set);
      }
      case PM_CONSTANT_PATH_AND_WRITE_NODE: {  /* `A::B &&= v` (static owner) — read raises if undefined */
        const pm_constant_path_and_write_node_t *aw = (const pm_constant_path_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->target->name), owner;
        if (!const_path_static_owner(tc, aw->target->parent, tc->frame->class_name_sym, &owner))
            return kp_unsupported(tc, node, "constant-path &&= with a dynamic module part");
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, aw->value)));
        NODE *set = ALLOC_node_const_set(name, owner, INT32_MIN, val);
        return ALLOC_node_and(ALLOC_node_const(name, owner, INT32_MIN, INT32_MIN), set);
      }
      case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {  /* `A::B op= v` (static owner) → A::B = A::B op v */
        const pm_constant_path_operator_write_node_t *ow = (const pm_constant_path_operator_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->target->name), owner;
        if (!const_path_static_owner(tc, ow->target->parent, tc->frame->class_name_sym, &owner))
            return kp_unsupported(tc, node, "constant-path op= with a dynamic module part");
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, ow->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = ALLOC_node_const(name, owner, INT32_MIN, INT32_MIN), rhs = transduce(tc, ow->value)));
            (op != KP_BINOP_NONE) ? alloc_binop(op, lhs, rhs, line) : kp_send1(opmid, line, lhs, rhs);
        }));
        return ALLOC_node_const_set(name, owner, INT32_MIN, binop);
      }

      case PM_RESCUE_MODIFIER_NODE: {   /* `expr rescue fallback` (catch-all) */
        const pm_rescue_modifier_node_t *rm = (const pm_rescue_modifier_node_t *)node;
        NODE *body = transduce(tc, rm->expression);
        NODE *cls = ALLOC_node_const(korb_intern(tc->c->vm, "StandardError", 13), 0, INT32_MIN, INT32_MIN);
        NODE *resc = transduce(tc, rm->rescue_expression);
        NODE *rescues = ALLOC_node_rescue(cls, resc, ALLOC_node_reraise(), 0, 0u);  /* catch StandardError */
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
            if (anon_blk && tc->frame->anon_blk_slot < 0)
                return kp_unsupported(tc, node, "bare & outside a (&) method body");
            uint32_t pslot = anon_blk ? (uint32_t)tc->frame->anon_blk_slot : alloc_synth_local(tc);
            NODE *pset = anon_blk ? NULL : bake_lset(tc, pslot, transduce(tc, ba->expression));
            int32_t soff = -1 - tc->chain - 1, dco = -1 - tc->chain - 1;
            int32_t poff = (int32_t)pslot - tc->chain - 1;
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args ? args->arguments.nodes : NULL, argc, (uint32_t)argc)));
            NODE *_s = ALLOC_node_super_blkproc(m_mid, line, soff, dco, poff, arr);
            bake_add(tc, &_s->u.node_super_blkproc.self_off);
            bake_add(tc, &_s->u.node_super_blkproc.proc_off);
            return pset ? ALLOC_node_seq(pset, _s) : _s;
        }
        if (sn->block) {
            NODE *bentry = kp_block_entry(tc, sn->block);
            if (!bentry) return kp_unsupported(tc, node, "super with a non-literal block");
            int32_t soff = -1 - tc->chain - 1, dco = -1 - tc->chain - 1, deo = -tc->chain - 1;
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args ? args->arguments.nodes : NULL, argc, (uint32_t)argc)));
            NODE *_s = ALLOC_node_super_blk(m_mid, line, soff, dco, bentry, deo, arr);
            bake_add(tc, &_s->u.node_super_blk.self_off);
            bake_add(tc, &_s->u.node_super_blk.def_env_off);
            return _s;
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
        if (m_mid == 0 && tc->frame->dm_body)
            return kp_unsupported(tc, node, "implicit argument passing of super from method defined by "
                                            "define_method() is not supported. Specify all arguments explicitly.");
        if (m_mid == 0) {   /* a plain block borrows the name of the def it sits in */
            for (const struct kp_frame *f = tc->frame; f && m_mid == 0; f = f->prev) {
                if (f->dm_body) break;
                m_mid = f->method_mid;
            }
        }
        if (m_mid == 0) return kp_unsupported(tc, node, "super outside a method body");
        uint32_t line = kp_line(tc, node);
        const uint32_t np = tc->frame->method_params;
        const int32_t rest_slot = tc->frame->method_rest_slot;
        const uint32_t pc = tc->frame->method_post_cnt;
        const uint32_t pb = tc->frame->method_post_base >= 0 ? (uint32_t)tc->frame->method_post_base : 0;
        struct korb_kw_info *const kw = tc->frame->method_kw_info;
        const uint32_t has_kw = (kw && (kw->count || kw->kwrest_slot >= 0)) ? 1u : 0u;
        /* bare `super` forwards the method's current args (positional + rest +
         * post + keyword) and its incoming block: build [pos..., *rest, post...,
         * {kwargs}] via super_fwd. */
        const uint32_t total = np + (rest_slot >= 0 ? 1u : 0u) + pc + has_kw;
        NODE *arr;
        WITH_CHAIN(tc, 1, (arr = build_fwd_args(tc, np, rest_slot, pb, pc, kw, total)));
        if (fn->block) {                                  /* `super { ... }` — forwarded args + a literal block */
            NODE *bentry = kp_block_entry(tc, (const pm_node_t *)fn->block);
            if (!bentry) return kp_unsupported(tc, node, "super with a non-literal block");
            const int32_t soff = -1 - tc->chain - 1, dco = -1 - tc->chain - 1, deo = -tc->chain - 1;
            NODE *_s = ALLOC_node_super_blk(m_mid, line, soff, dco, bentry, deo, arr);
            bake_add(tc, &_s->u.node_super_blk.self_off);
            bake_add(tc, &_s->u.node_super_blk.def_env_off);
            return _s;
        }
        return emit_super_fwd(tc, m_mid, line, arr);
      }

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
    const pm_diagnostic_t *const d = (const pm_diagnostic_t *)parser->error_list.head;
    if (d == NULL) return;
    static char buf[256];
    const int32_t line = pm_newline_list_line(&parser->newline_list, d->location.start, parser->start_line);
    snprintf(buf, sizeof buf, "%s:%d: %s", fname ? fname : "(eval)", line, d->message);
    c->vm->last_syntax_msg = buf;
}

NODE *
koruby_parse_source_at(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, bool exit_on_error)
{
    pm_parser_t parser;
    pm_options_t options = { 0 };
    if (OPTION.frozen_literals) pm_options_frozen_string_literal_set(&options, true);   /* --enable-frozen-string-literal */
    pm_options_filepath_set(&options, fname);
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
        /* main program: CRuby-compatible exit path — SyntaxError → stderr + exit 1 */
        for (const pm_diagnostic_t *d = (const pm_diagnostic_t *)parser.error_list.head;
             d != NULL; d = (const pm_diagnostic_t *)d->node.next) {
            int32_t line = pm_newline_list_line(&parser.newline_list,
                                                d->location.start, parser.start_line);
            fprintf(stderr, "%s:%d: %s\n", fname, line, d->message);
        }
        fprintf(stderr, "%s: syntax error (SyntaxError)\n", fname);
        exit(1);
    }

    struct kp_ctx tc = {
        .parser = &parser,
        .c = c,
        .fname = fname,
        .src_enc = kp_src_enc(c, &parser),
    };
    NODE *ast = transduce(&tc, root);
    if (ast == NULL) ast = lit_nil();
    for (uint32_t pi = tc.pre_cnt; pi-- > 0; ) ast = ALLOC_node_seq(tc.pre_list[pi], ast);   /* BEGIN { } first */
    free(tc.pre_list);
    free(tc.bake_list);
    if (tc.syntax_error) {                           /* transduce-time SyntaxError (e.g. binding in alternative pattern) */
        pm_node_destroy(&parser, root);
        pm_parser_free(&parser);
        pm_options_free(&options);
        if (!exit_on_error) return NULL;             /* eval(str): caller raises SyntaxError */
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
    return koruby_parse_source_at(c, src, len, fname, 1, exit_on_error);
}

/* Parse `src` for eval(str, binding): the binding's local names are declared as
 * the program's scope so bare references parse as (depth-0) locals.  Returns the
 * program AST; koruby_toplevel_locals_cnt / koruby_toplevel_local_{syms,cnt} are
 * set to the eval program's frame (caller seeds those slots from the binding and
 * writes them back).  NULL on syntax error. */
NODE *
koruby_parse_binding_eval(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line,
                          const uint32_t *name_syms, uint32_t name_cnt)
{
    pm_parser_t parser;
    pm_options_t options = { 0 };
    if (OPTION.frozen_literals) pm_options_frozen_string_literal_set(&options, true);   /* --enable-frozen-string-literal */
    pm_options_filepath_set(&options, fname);
    pm_options_line_set(&options, first_line);
    /* declare the binding's locals so the eval code recognises them as locals
     * (prism folds a single declared scope into the parsed program's own scope). */
    pm_options_scopes_init(&options, 1);
    pm_options_scope_t *scope = &options.scopes[0];
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
        return NULL;
    }
    struct kp_ctx tc = { .parser = &parser, .c = c, .fname = fname, .src_enc = kp_src_enc(c, &parser) };
    NODE *ast = transduce(&tc, root);
    if (ast == NULL) ast = lit_nil();
    for (uint32_t pi = tc.pre_cnt; pi-- > 0; ) ast = ALLOC_node_seq(tc.pre_list[pi], ast);   /* BEGIN { } first */
    free(tc.pre_list);
    free(tc.bake_list);
    bool serr = tc.syntax_error;
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    pm_options_free(&options);
    return serr ? NULL : ast;   /* binding in alternative pattern → SyntaxError */
}
