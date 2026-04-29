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

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (!n) { fprintf(stderr, "node_allocate: out of memory\n"); exit(1); }
    return n;
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

// HOPT() fallback — we don't run a separate profile-aware hash, so
// alias it to the structural hash.  alloc_dispatcher_name_hash only
// calls this when astro_cs_use_hopt_name is set, which we never do in
// luastro.
static node_hash_t HOPT(NODE *n) { return n ? HASH(n) : 0; }
node_hash_t HORG(NODE *n) { return HASH(n); }

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

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    if (astro_cs_load(n, NULL)) g_opt_hit++; else g_opt_miss++;
    return n;
}

void luastro_optimize_stats(void) {
    if (OPTION.verbose) fprintf(stderr, "luastro: cs hit=%d miss=%d\n", g_opt_hit, g_opt_miss);
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

void
luastro_specialize_all(NODE *root, const char *file)
{
    if (root) astro_cs_compile(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_compile(CR.entries[i].body, file);
    }
    astro_cs_build(NULL);
    astro_cs_reload();
    // After reload, re-resolve the live nodes' dispatchers so this
    // very run picks up the freshly-baked SDs (otherwise only the
    // *next* invocation benefits).
    if (root) astro_cs_load(root, NULL);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_load(CR.entries[i].body, NULL);
    }
}

// --- Generated code -------------------------------------------

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// node_specialized.c contains the AOT/PGC-baked SD_*/PGSD_* dispatchers
// and an `sc_entries[]` array (see luastro_specialize_all).  When we
// build the plain interpreter, this file may be empty (created by the
// Makefile rule), so guard against missing definitions.
#include "node_specialized.c"

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
