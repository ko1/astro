// luastro — main translation unit.
//
// This file glues the auto-generated node_*.c files (from node.def via
// ASTroGen) together with the runtime helpers in lua_runtime.c and the
// front-end in lua_tokenizer.c / lua_parser.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "node.h"
#include "context.h"

// --- User-provided allocator ------------------------------------

// Track every allocated NODE so we can invalidate the (cached
// `head.hash_value` + patched `head.dispatcher`) state after the parser
// runs late kind-mutating passes (`pf_finalize_local_refs`,
// `pf_fold_constants`).  On a run where `all.so` is already loaded,
// `OPTIMIZE` (called from each `ALLOC_*`) caches the hash and patches
// the dispatcher *before* mutation.  Without invalidation the captured
// slot's `node_local_get→node_box_get` rewrite leaves the cached hash
// stale and the dispatcher pointing at the local_get-shaped SD, so
// captured-local reads return the raw `LuaBox*` instead of the unboxed
// value, surfacing later as "attempt to index a unknown value".
static struct {
    NODE   **arr;
    uint32_t cnt;
    uint32_t cap;
} g_all_nodes = {0};

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (!n) { fprintf(stderr, "node_allocate: out of memory\n"); exit(1); }
    if (g_all_nodes.cnt == g_all_nodes.cap) {
        g_all_nodes.cap = g_all_nodes.cap ? g_all_nodes.cap * 2 : 256;
        g_all_nodes.arr = (NODE **)realloc(g_all_nodes.arr,
                                           g_all_nodes.cap * sizeof(NODE *));
        if (!g_all_nodes.arr) { fprintf(stderr, "node_allocate: oom (track)\n"); exit(1); }
    }
    g_all_nodes.arr[g_all_nodes.cnt++] = n;
    return n;
}

// Reset every tracked node's cached hash and patched dispatcher, then
// re-run OPTIMIZE so each one picks up the SD that matches its current
// (post-mutation) kind.  Called once from `PARSE_lua` after the
// kind-mutating passes finish.  See comment on `g_all_nodes` for why.
void
luastro_reoptimize_all(void)
{
    for (uint32_t i = 0; i < g_all_nodes.cnt; i++) {
        NODE *n = g_all_nodes.arr[i];
        if (!n || !n->head.kind) continue;
        n->head.flags.has_hash_value = false;
        n->head.flags.is_specialized = false;
        n->head.dispatcher           = n->head.kind->default_dispatcher;
        n->head.dispatcher_name      = n->head.kind->default_dispatcher_name;
    }
    for (uint32_t i = 0; i < g_all_nodes.cnt; i++) {
        NODE *n = g_all_nodes.arr[i];
        if (n) OPTIMIZE(n);
    }
}

// --- Dispatcher tracing ---------------------------------------

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
#if LUASTRO_DEBUG_EVAL
    if (end) c->rec_cnt--;
    else {
        for (int i = 0; i < c->rec_cnt; i++) fputc(' ', stderr);
        fprintf(stderr, "%s\n", n->head.dispatcher_name);
        c->rec_cnt++;
    }
#else
    (void)c; (void)n; (void)end;
#endif
}

// HORG / HOPT split.  HORG is the structural hash (canonical name —
// swapped variants share); HOPT is the profile-aware hash (actual name
// — swapped variants differ).  PGC bake names baked SDs by HOPT; the
// (HORG, file, line) → HOPT index lets the next process compute HORG
// at parse time and look up the corresponding HOPT to dlsym.
node_hash_t HORG(NODE *n) { return HASH(n); }

node_hash_t
HOPT(NODE *n)
{
    if (n == NULL) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    if (n->head.kind->hopt_func) {
        n->head.flags.has_hash_opt = true;
        return n->head.hash_opt = (*n->head.kind->hopt_func)(n);
    }
    return 0;
}

// hash_node_opt: HOPT counterpart of hash_node (the recursion entry point
// in generated HOPT_<name> bodies).  Caches per-NODE so deep trees don't
// recompute.  Mirrors astro_node.c's `hash_node`.
node_hash_t
hash_node_opt(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    return HOPT(n);
}

// --- ASTro common infrastructure ------------------------------

#include "astro_node.c"

// --- Code store ----------------------------------------------

#include "astro_code_store.c"

// --- Per-language EVAL / OPTIMIZE -----------------------------

RESULT
EVAL(CTX *c, NODE *n, LuaValue *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}

static int g_opt_hit = 0;
static int g_opt_miss = 0;

// Source filename for the current parse / run, set by main.c just
// before parsing.  Threaded into astro_cs_load so PGC lookup can
// compute (HORG, file, line) → HOPT.  NULL falls back to AOT-only
// load (SD_<HORG>, no PGC index lookup).
const char *luastro_current_src_file = NULL;

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    if (astro_cs_load(n, luastro_current_src_file)) g_opt_hit++; else g_opt_miss++;
    return n;
}

void luastro_optimize_stats(void) {
    if (OPTION.verbose) fprintf(stderr, "luastro: cs hit=%d miss=%d\n", g_opt_hit, g_opt_miss);
}

// True if any tracked NODE has fired `swap_dispatcher` during the
// profile run.  Lets `-p` fall back to AOT-bake (SD_<HORG>) when the
// program never observed a hot type-mono arith / compare path — PGC
// bake's only win there is keying SDs by HOPT, but with HOPT == HORG
// at every site the lookup just adds a hopt_index probe per cs_load
// without any speedup.  json / permute / richards / deltablue see this
// regression today; backing off to AOT recovers the 2-11% delta.
bool
luastro_any_kind_swapped(void)
{
    for (uint32_t i = 0; i < g_all_nodes.cnt; i++) {
        NODE *n = g_all_nodes.arr[i];
        if (n && n->head.flags.kind_swapped) return true;
    }
    return false;
}

// --- code_repo: register named entries (function bodies) for code store

struct code_entry {
    const char *name;
    NODE       *body;
    bool        skip_specialize;
};
static struct {
    struct code_entry *entries;
    uint32_t cnt, cap;
} CR = {0};

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)force;
    if (!body) return;
    if (CR.cnt == CR.cap) {
        CR.cap = CR.cap ? CR.cap * 2 : 16;
        CR.entries = (struct code_entry *)realloc(CR.entries, CR.cap * sizeof(struct code_entry));
    }
    CR.entries[CR.cnt++] = (struct code_entry){name, body, false};
}


// Variadic-operand entries live in `LUASTRO_NODE_ARR` (the parser-side
// array; populated by `reg_node_arr` for multi-arg calls, table
// constructors, multi-assign rhs, multi-return lists, ...).  Each
// entry is dispatched at runtime via `EVAL(c, LUASTRO_NODE_ARR[idx +
// k], frame)` from inside its parent NODE's EVAL body, so the SD
// specialiser cannot fold the dispatcher value: the indexed read into
// the side array is opaque to it.  Each such entry needs its own
// public extern SD so `astro_cs_load` can dlsym and patch the
// dispatcher.
extern NODE     **LUASTRO_NODE_ARR;
extern uint32_t   LUASTRO_NODE_ARR_CNT;

void
luastro_specialize_all(NODE *root, const char *file)
{
    // Three flavours of entry node, each known at parse time:
    //
    //   1. The chunk root — passed in.  Whole-tree EVAL anchor.
    //   2. Closure bodies in CR.entries — every `function ... end` in
    //      the chunk gets registered via `code_repo_add` and is later
    //      dispatched from `node_call_N` via `cl->body->head.dispatcher`,
    //      which is a runtime-indirect read (cl is a runtime LuaClosure).
    //   3. Variadic-operand entries in LUASTRO_NODE_ARR — see comment
    //      above on why these need their own SD.
    //
    // We compile each entry once.  astro_cs_compile dedups by hash, so
    // identical sub-shapes shared across entries collapse to a single
    // SD_<hash>.c and a single all.so symbol.

    if (root) astro_cs_compile(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_compile(CR.entries[i].body, file);
    }
    for (uint32_t i = 0; i < LUASTRO_NODE_ARR_CNT; i++) {
        if (LUASTRO_NODE_ARR[i]) astro_cs_compile(LUASTRO_NODE_ARR[i], file);
    }

    astro_cs_build(NULL);
    astro_cs_reload();

    // After reload, re-resolve the live nodes' dispatchers so this
    // very run picks up the freshly-baked SDs (otherwise only the
    // *next* invocation benefits, since `head.dispatcher` was locked in
    // at allocation time to interp's DISPATCH_).
    if (root) astro_cs_load(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_load(CR.entries[i].body, file);
    }
    for (uint32_t i = 0; i < LUASTRO_NODE_ARR_CNT; i++) {
        if (LUASTRO_NODE_ARR[i]) astro_cs_load(LUASTRO_NODE_ARR[i], file);
    }
}

// --- Generated code -------------------------------------------

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_hopt.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// --- INIT -----------------------------------------------------

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}

// --- Stack-of-cells allocator (for boxed locals) ---------------
//
// luastro_ensure_box(slot) returns a pointer to the LuaValue stored
// inside a heap LuaBox cell that aliases the given frame slot.  The
// frame slot itself is rewritten to store a tagged-pointer LuaValue
// referencing the box.  Once a slot is boxed, it stays boxed for the
// rest of the frame's life — the node_local_get/_set generated for
// captured-locals reads through the box (see node_box_get / node_box_set
// in node.def).

LuaValue *
luastro_ensure_box(LuaValue *slot)
{
    if (LV_IS_BOX(*slot)) {
        struct LuaBox *bx = (struct LuaBox *)(uintptr_t)*slot;
        return &bx->value;
    }
    struct LuaBox *bx = (struct LuaBox *)calloc(1, sizeof(struct LuaBox));
    luastro_gc_register(bx, LUA_TBOXED);
    bx->value = *slot;
    *slot = (LuaValue)(uintptr_t)bx;
    return &bx->value;
}
