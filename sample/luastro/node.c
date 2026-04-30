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
#include <ctype.h>
#include <dirent.h>
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

// Post-process every astro_cs_compile-produced SD source file to make
// the otherwise-`static inline` inner SD bodies externally visible
// (under their original name) without losing inlining at the in-source
// call sites.
//
// Why: ASTroGen emits inner SDs as `static inline` so that gcc can
// devirtualize the function-pointer chain inside the SD module.  But
// `static` makes them invisible to dlsym, so at runtime
// `astro_cs_load` only finds the single externally-named root SD —
// every other AST node falls back to `n->head.dispatcher` =
// `&DISPATCH_<name>` (in the host binary).  When the SD body chains
// runtime dispatch through a child's `head.dispatcher`, that path goes
// out of `all.so`, into the host's `DISPATCH_*`, and back, on every
// per-node touch — which on `mandelbrot` was ~50% of cycles.
//
// Fix: rewrite every `SD_<hash>` reference inside the file to
// `SD_<hash>_INL` (so the in-source function-pointer chain still
// inlines through `static inline`), then append a single extern
// wrapper `SD_<hash>(...)` per SD that just forwards to its `_INL`
// counterpart.  The wrapper is what dlsym now finds and what
// `head.dispatcher` ends up pointing to; gcc inlines the wrapper's
// body to a tail call so the runtime cost is one extra `jmp`.
//
// `cs hit` count goes from 2 → ~80 nodes resolved per mandelbrot run
// and AOT-c drops a further ~13%.
static void
luastro_export_sd_wrappers(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *src = (char *)malloc(sz + 1);
    if (!src) { fclose(fp); return; }
    if (fread(src, 1, sz, fp) != (size_t)sz) { free(src); fclose(fp); return; }
    src[sz] = '\0';
    fclose(fp);

    // Collect every `SD_<hex>` token that appears as a function DEFINITION
    // in this file — i.e. the name sits at the start of a line followed
    // by `(`.  Forward-decls (one-liners ending with `;`) reference SDs
    // whose body lives in another `.c`; emitting a wrapper here would
    // create an undefined-symbol reference at dlopen time.
    //
    // Pattern matched (after the upcoming `_INL` rename, but the rename
    // doesn't affect the start-of-line property): `\nSD_<hex>(`.
    size_t name_cap = 256, name_cnt = 0;
    char (*names)[20] = (char (*)[20])malloc(name_cap * 20);
    for (const char *p = src; *p; ) {
        bool at_line_start = (p == src) || (p[-1] == '\n');
        if (at_line_start && p[0] == 'S' && p[1] == 'D' && p[2] == '_') {
            const char *q = p + 3;
            while (isxdigit((unsigned char)*q)) q++;
            size_t len = q - p;
            // Definition: name immediately followed by `(`.
            if (len >= 4 && len < 20 && *q == '(') {
                bool dup = false;
                for (size_t i = 0; i < name_cnt; i++) {
                    if (strncmp(names[i], p, len) == 0 && names[i][len] == '\0') { dup = true; break; }
                }
                if (!dup) {
                    if (name_cnt >= name_cap) {
                        name_cap *= 2;
                        names = (char (*)[20])realloc(names, name_cap * 20);
                    }
                    memcpy(names[name_cnt], p, len);
                    names[name_cnt][len] = '\0';
                    name_cnt++;
                }
            }
            p = q;
        } else {
            p++;
        }
    }

    // Rewrite every SD_<hash> token in src to SD_<hash>_INL.  Worst-case
    // length: 4 extra bytes per token.  Walk the source, copy to a new
    // buffer, replace as we go.
    size_t out_cap = sz + name_cnt * 8 + 4096;
    char *out = (char *)malloc(out_cap);
    size_t out_len = 0;
    for (const char *p = src; *p; ) {
        if (p[0] == 'S' && p[1] == 'D' && p[2] == '_' &&
            (p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *q = p + 3;
            while (isxdigit((unsigned char)*q)) q++;
            size_t len = q - p;
            if (len >= 4 && len < 20) {
                memcpy(out + out_len, p, len);
                out_len += len;
                memcpy(out + out_len, "_INL", 4);
                out_len += 4;
                p = q;
                continue;
            }
        }
        out[out_len++] = *p++;
    }

    // Append the extern wrappers.
    const char *banner =
        "\n// Externally-visible thin wrappers — make every SD reachable\n"
        "// via dlsym so the runtime astro_cs_load can patch every node's\n"
        "// head.dispatcher to its specialized SD (rather than only the\n"
        "// chunk root).  See luastro_export_sd_wrappers for the why.\n";
    size_t banner_len = strlen(banner);
    if (out_len + banner_len + 1 >= out_cap) {
        out_cap = out_len + banner_len + name_cnt * 128 + 1024;
        out = (char *)realloc(out, out_cap);
    }
    memcpy(out + out_len, banner, banner_len);
    out_len += banner_len;
    for (size_t i = 0; i < name_cnt; i++) {
        char line[256];
        // `weak` so identical SD shapes shared across multiple chunks
        // (chunk root + every closure body) link without a duplicate-
        // symbol error — the linker keeps one and discards the rest.
        int n = snprintf(line, sizeof(line),
            "__attribute__((weak)) RESULT %s(CTX *c, NODE *n, LuaValue *frame) { return %s_INL(c, n, frame); }\n",
            names[i], names[i]);
        if (out_len + n + 1 >= out_cap) {
            out_cap = (out_len + n + 1) * 2;
            out = (char *)realloc(out, out_cap);
        }
        memcpy(out + out_len, line, n);
        out_len += n;
    }
    out[out_len] = '\0';

    fp = fopen(path, "w");
    if (fp) {
        fwrite(out, 1, out_len, fp);
        fclose(fp);
    }
    free(out);
    free(names);
    free(src);
}

static void
luastro_export_all_sds(void)
{
    // Walk code_store/c/*.c and rewrite each.  The dir layout is set
    // by astro_cs_init's "code_store" arg in INIT().
    DIR *d = opendir("code_store/c");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        size_t nlen = strlen(name);
        if (nlen <= 2 || strcmp(name + nlen - 2, ".c") != 0) continue;
        if (strncmp(name, "SD_", 3) != 0 && strncmp(name, "PGSD_", 5) != 0) continue;
        char path[ASTRO_CS_PATH_MAX];
        snprintf(path, sizeof(path), "code_store/c/%s", name);
        luastro_export_sd_wrappers(path);
    }
    closedir(d);
}

// Variadic-operand children live in `LUASTRO_NODE_ARR` (the parser's
// side array), referenced from `@noinline` nodes (`node_local_decl`,
// multi-arg calls, table constructors).  ASTroGen's specializer walks
// only typed `NODE *` operands so those side-array children never get
// their own SD baked, leaving their `head.dispatcher` stuck at
// `&DISPATCH_<name>` and forcing a host-binary bounce on every touch.
//
// Compile each side-array entry directly so every node in the
// program ends up addressable by `dlsym(SD_<hash>)`.
extern NODE     **LUASTRO_NODE_ARR;
extern uint32_t   LUASTRO_NODE_ARR_CNT;

static void
luastro_specialize_side_array(const char *file)
{
    for (uint32_t i = 0; i < LUASTRO_NODE_ARR_CNT; i++) {
        if (LUASTRO_NODE_ARR[i]) astro_cs_compile(LUASTRO_NODE_ARR[i], file);
    }
}

void
luastro_specialize_all(NODE *root, const char *file)
{
    if (root) astro_cs_compile(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_compile(CR.entries[i].body, file);
    }
    // Side-array walk: also bake SDs for variadic-operand children
    // that ASTroGen's typed-operand specializer skips.  This raises the
    // cs-hit rate from ~80% to ~95% on mandelbrot etc.
    luastro_specialize_side_array(file);
    luastro_export_all_sds();   // post-process before the gcc build runs
    astro_cs_build(NULL);
    astro_cs_reload();
    // After reload, re-resolve the live nodes' dispatchers so this
    // very run picks up the freshly-baked SDs (otherwise only the
    // *next* invocation benefits).
    if (root) astro_cs_load(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_load(CR.entries[i].body, file);
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
