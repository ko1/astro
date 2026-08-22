// ASTro node common infrastructure
//
// Provides hash functions, HASH, DUMP, and alloc_dispatcher_name.
//
#include "astro_node.h"   // emit API declarations
// #include this file from your node.c, AFTER defining `node_allocate(size_t)`
// and BEFORE #including astro_code_store.c and the generated files.

// ---------------------------------------------------------------------------
// Hash functions (used by generated node_hash.c)
// ---------------------------------------------------------------------------

static node_hash_t
hash_merge(node_hash_t h, node_hash_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4);
    return h;
}

static node_hash_t
hash_cstr(const char *s)
{
    node_hash_t h = 14695981039346656037ULL; // FNV offset basis for 64-bit
    const node_hash_t FNV_PRIME = 1099511628211ULL;

    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= FNV_PRIME;
    }

    return h;
}

static node_hash_t
hash_uint32(uint32_t ui)
{
    node_hash_t x = (node_hash_t)ui;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;

    return x;
}

__attribute__((unused)) static node_hash_t
hash_uint64(uint64_t u)
{
    node_hash_t x = (node_hash_t)u;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;

    return x;
}

__attribute__((unused)) static node_hash_t
hash_double(double d)
{
    union { double d; uint64_t u; } conv;
    conv.d = d;
    return hash_uint32((uint32_t)(conv.u ^ (conv.u >> 32)));
}

// Write s to fp quoted as a C string literal, escaping special characters so
// dumpers can safely embed arbitrary strings inside source-code comments or
// C literal contexts.  Used by generated DUMP_node_* functions for samples
// that have `const char *` operands; samples without any string operands
// don't reference it, hence the explicit unused-suppression.
__attribute__((unused)) static void
astro_fprintf_cstr(FILE *fp, const char *s)
{
    if (s == NULL) { fputs("\"\"", fp); return; }
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '\\': fputs("\\\\", fp); break;
        case '"':  fputs("\\\"", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (*p < 0x20 || *p == 0x7f) {
                fprintf(fp, "\\x%02x", *p);
            } else {
                fputc(*p, fp);
            }
        }
    }
    fputc('"', fp);
}

static node_hash_t
hash_node(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) {
        return n->head.hash_value;
    }
    else {
        return HASH(n);
    }
}

// ---------------------------------------------------------------------------
// General node operations
// ---------------------------------------------------------------------------

node_hash_t
HASH(NODE *n)
{
    if (n == NULL) {
        return 0;
    }
    /* Hash caching disabled: dispatcher patching can mutate a node's
     * `kind` after the hash was computed (e.g., ascheme_precise's
     * lref → lref_sp post-no_capture-analysis), and a cached pre-patch
     * hash would propagate up to ancestor caches that we can't reliably
     * invalidate (parent pointer covers one path but a node may be
     * reached via multiple unrelated parents).  Recomputing per HASH
     * call is O(node count) per call; with HASH being called only a
     * handful of times per program (at cs_compile / dispatch_name
     * generation), the total cost is amortized.  Drop the flag/cache
     * mechanism entirely; if a future profiler shows HASH dominating,
     * reintroduce caching with explicit invalidation hooks plumbed
     * through every kind-mutation site. */
    if (n->head.kind->hash_func) {
        return (*n->head.kind->hash_func)(n);
    }
    return 0;
}

void
DUMP(FILE *fp, NODE *n, bool oneline)
{
    if (!n) {
        fprintf(fp, "<NULL>");
    }
    else if (n->head.flags.is_dumping) {
        fprintf(fp, "...");
    }
    else {
        n->head.flags.is_dumping = true;
        (*n->head.kind->dumper)(fp, n, oneline);
        n->head.flags.is_dumping = false;
    }
}

// ---------------------------------------------------------------------------
// Print a C string literal with proper escaping (used by generated node_specialize.c)
// ---------------------------------------------------------------------------

__attribute__((unused)) static void
astro_fprint_cstr(FILE *fp, const char *s)
{
    fprintf(fp, "        \"");
    for (; *s; s++) {
        switch (*s) {
        case '"':  fprintf(fp, "\\\""); break;
        case '\\': fprintf(fp, "\\\\"); break;
        case '\n': fprintf(fp, "\\n"); break;
        case '\r': fprintf(fp, "\\r"); break;
        case '\t': fprintf(fp, "\\t"); break;
        default:   fputc(*s, fp);
        }
    }
    fprintf(fp, "\"");
}

// ---------------------------------------------------------------------------
// Dispatcher name allocation (used by generated node_specialize.c)
// ---------------------------------------------------------------------------

// Emission mode for SD_<hash> names during SPECIALIZE:
//   0 = Horg (structural)   — default, used by AOT (--compile).  Prefix SD_.
//   1 = Hopt (profile-aware) — set transiently by astro_cs_compile during
//                              PGC bake.  Prefix PGSD_ so a glance at the
//                              code store / symbol table distinguishes AOT
//                              and PGC outputs.
// Hosts that don't provide HOPT() leave this as 0 forever; HOPT() is never
// called in that case.
static int astro_cs_use_hopt_name = 0;

// ---------------------------------------------------------------------------
// SD-source comment emission gating
// ---------------------------------------------------------------------------
//
// During SPECIALIZE, the auto-generated SPECIALIZE_node_xxx (lib/astrogen.rb)
// and the host's custom specializers (e.g. castro_gen.rb's call_static
// override) emit a `// (node_…)` comment ahead of every dispatcher
// definition.  These comments document the AST shape the SD was built
// from — handy when staring at the generated SD source while debugging.
//
// However in programs with many no_inline callees (= big shared helpers
// inlined into N caller TUs), the comment text balloons because each
// caller's commented dispatcher carries the full callee subtree dump.
// On a 10K-LOC bench (140 callers × one big helper), the framework's
// auto-DUMP made the SD source 1.19 GB, of which ≈99.93% was comments —
// gcc still has to lex through them.
//
// `astro_emit_sd_comments_p()` answers "should the comment fprintf
// fire?" with one cached env-var read.  Default is OFF (= no comments)
// since the size cost dominates the debugging value at scale; set
// `ASTRO_SD_COMMENTS=1` to re-enable.
__attribute__((unused))
static int
astro_emit_sd_comments_p(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("ASTRO_SD_COMMENTS");
        cached = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static node_hash_t alloc_dispatcher_name_hash(NODE *n);

static const char *
alloc_dispatcher_name(NODE *n)
{
    const char *prefix = astro_cs_use_hopt_name ? "PGSD_" : "SD_";
    char buff[128];
    snprintf(buff, sizeof(buff), "%s%lx", prefix,
             (unsigned long)alloc_dispatcher_name_hash(n));
    char *name = malloc(strlen(buff) + 1);
    strcpy(name, buff);
    return name;
}

// Pick Horg or Hopt for SD_* name emission.  The Hopt path is defined by
// the host in node.c; we call it only when the host has flipped the
// mode flag (it wouldn't have flipped it without a working HOPT()).
static node_hash_t
alloc_dispatcher_name_hash(NODE *n)
{
    if (astro_cs_use_hopt_name) return HOPT(n);
    return hash_node(n);
}

// ---------------------------------------------------------------------------
// AST → C source emitters
// ---------------------------------------------------------------------------
//
// Used by `--generate-executable`: an existing AST is walked and the
// equivalent ALLOC_<kind>(...) construction is written to a FILE *.  The
// generated source, compiled and linked into the exe, lets the standalone
// binary reconstruct the AST at startup without re-running the parser.
//
// Two emission strategies share the same per-NODE EMIT_AST_<kind> helpers
// (auto-generated by ASTroGen):
//
//   1. Recursive nested-ALLOC mode (astro_emit_ast_c_file).  Output is
//      one expression with sub-trees inlined.  Simple but breaks on:
//        - node sharing (the same NODE referenced from multiple parents
//          → duplicate ALLOCs at exe runtime, breaking identity-based
//          machinery like code_repo lookup),
//        - cycles (recursive function bodies referencing themselves via
//          sp_body / body operands → infinite recursion during emit).
//      Calc + other zero-sharing samples use this.
//
//   2. Flat DAG mode (astro_emit_ast_c_program).  DFS-assigns each
//      unique NODE a sequential ID, emits one `_n[i] = ALLOC_<kind>(...)`
//      line per node in dependency order, and patches cycle back-edges
//      after all ALLOCs.  Naruby and any other sample with sharing /
//      recursion use this.
//
// EMIT_AST_<kind> emits NODE * operands via astro_emit_ast_c_child(),
// which behaves differently in the two modes.

// ---------------------------------------------------------------------------
// Mode 1: recursive (calc-style)
// ---------------------------------------------------------------------------

// Forward decls — actual definitions further down.
struct astro_emit_ctx;
static struct astro_emit_ctx *astro_emit_ast_active_ctx = NULL;

void
astro_emit_ast_c(FILE *fp, NODE *n)
{
    if (n == NULL) {
        fprintf(fp, "NULL");
        return;
    }
    if (!n->head.kind || !n->head.kind->emit_ast) {
        fprintf(fp, "/* astro_emit_ast: kind has no emitter */NULL");
        return;
    }
    (*n->head.kind->emit_ast)(fp, n);
}

void
astro_emit_ast_c_file(FILE *fp, NODE *root,
                      const char *func_name,
                      const char *include_header)
{
    fprintf(fp, "// Auto-generated by astro_emit_ast_c_file.\n");
    if (include_header) {
        fprintf(fp, "#include \"%s\"\n", include_header);
    }
    fprintf(fp, "\n");
    fprintf(fp, "NODE *\n");
    fprintf(fp, "%s(void)\n", func_name);
    fprintf(fp, "{\n");
    fprintf(fp, "    return ");
    astro_emit_ast_active_ctx = NULL;   // recursive mode
    astro_emit_ast_c(fp, root);
    fprintf(fp, ";\n");
    fprintf(fp, "}\n");
}

// ---------------------------------------------------------------------------
// Mode 2: flat / DAG (naruby-style)
// ---------------------------------------------------------------------------

struct astro_emit_visited_entry {
    NODE *node;
    int id;
    bool in_progress;        // currently on DFS stack — back-edge target
};

struct astro_emit_fixup {
    int parent_id;
    NODE *parent;            // for kind-aware field lookup
    const char *field_path;  // strdup'd, e.g. "node_def.body"
    int child_id;            // resolved during emit_program
    NODE *child;
};

struct astro_emit_ctx {
    struct astro_emit_visited_entry *visited;
    size_t visited_size;
    size_t visited_capa;

    struct astro_emit_fixup *fixups;
    size_t fixups_size;
    size_t fixups_capa;

    FILE *fp;                // for child-ref emits
    int next_id;
    bool in_collect_phase;   // true during DFS pre-walk

    // NODE* → visited slot map (open addressing).  A prelude-sized program
    // visits tens of thousands of nodes with an edge per operand; the linear
    // scans this replaces made emission quadratic (~2 min for koruby's
    // prelude embed).
    NODE **map_keys;
    size_t *map_slots;
    size_t map_mask;         // capacity - 1 (capacity is a power of two)
    size_t map_count;
};

static void
astro_emit_map_insert(struct astro_emit_ctx *ctx, NODE *n, size_t slot)
{
    if (!ctx->map_keys || ctx->map_count * 2 >= ctx->map_mask + 1) {   // load factor 1/2
        size_t old_capa = ctx->map_keys ? ctx->map_mask + 1 : 0;
        size_t capa = old_capa ? old_capa * 2 : 64;
        NODE **keys = calloc(capa, sizeof(*keys));
        size_t *slots = calloc(capa, sizeof(*slots));
        if (!keys || !slots) { fprintf(stderr, "astro_emit: oom\n"); exit(1); }
        for (size_t i = 0; i < old_capa; i++) {
            NODE *k = ctx->map_keys[i];
            if (!k) continue;
            size_t j = ((uintptr_t)k >> 4) & (capa - 1);
            while (keys[j]) j = (j + 1) & (capa - 1);
            keys[j] = k;
            slots[j] = ctx->map_slots[i];
        }
        free(ctx->map_keys);
        free(ctx->map_slots);
        ctx->map_keys = keys;
        ctx->map_slots = slots;
        ctx->map_mask = capa - 1;
    }
    size_t j = ((uintptr_t)n >> 4) & ctx->map_mask;
    while (ctx->map_keys[j]) j = (j + 1) & ctx->map_mask;
    ctx->map_keys[j] = n;
    ctx->map_slots[j] = slot;
    ctx->map_count++;
}

// visited slot for n, or (size_t)-1.
static size_t
astro_emit_map_find(const struct astro_emit_ctx *ctx, NODE *n)
{
    if (!ctx->map_keys) return (size_t)-1;
    size_t j = ((uintptr_t)n >> 4) & ctx->map_mask;
    while (ctx->map_keys[j]) {
        if (ctx->map_keys[j] == n) return ctx->map_slots[j];
        j = (j + 1) & ctx->map_mask;
    }
    return (size_t)-1;
}

static int
astro_emit_ctx_lookup(const struct astro_emit_ctx *ctx, NODE *n)
{
    size_t slot = astro_emit_map_find(ctx, n);
    return slot == (size_t)-1 ? -1 : ctx->visited[slot].id;
}

static bool
astro_emit_ctx_in_progress(const struct astro_emit_ctx *ctx, NODE *n)
{
    size_t slot = astro_emit_map_find(ctx, n);
    return slot == (size_t)-1 ? false : ctx->visited[slot].in_progress;
}

static void
astro_emit_ctx_grow_visited(struct astro_emit_ctx *ctx)
{
    if (ctx->visited_size >= ctx->visited_capa) {
        size_t capa = ctx->visited_capa == 0 ? 32 : ctx->visited_capa * 2;
        ctx->visited = realloc(ctx->visited, sizeof(*ctx->visited) * capa);
        if (!ctx->visited) { fprintf(stderr, "astro_emit: oom\n"); exit(1); }
        ctx->visited_capa = capa;
    }
}

// Recursive DFS collector: walks all NODE * fields of `n` and assigns
// post-order IDs.  Cycles are detected via the in_progress flag —
// re-entered nodes get NO id at the back-edge but are queued as fixups
// after the outer call returns.
//
// We don't have a generic "iterate over NODE * children" since each
// NODE_DEF has its own struct.  Instead, the EMIT_AST_<kind> functions
// emit a child reference for each NODE * operand; here we run them
// with a "collector" trampoline that records the child instead of
// emitting source text.  Implemented by overloading astro_emit_ast_c_child
// to look at ctx->in_collect_phase.

static void astro_emit_ctx_visit(struct astro_emit_ctx *ctx, NODE *n);

// Called from EMIT_AST_<kind> for each NODE * operand.  Defined here
// in the runtime; the generated node_emit_ast.c sees the macro below
// and skips its static fallback so this strong version is used.
#define ASTRO_EMIT_AST_C_CHILD_DEFINED 1
void
astro_emit_ast_c_child(FILE *fp, NODE *child)
{
    struct astro_emit_ctx *ctx = astro_emit_ast_active_ctx;
    if (!ctx) {
        // Recursive mode: nested ALLOC.
        astro_emit_ast_c(fp, child);
        return;
    }
    if (ctx->in_collect_phase) {
        // DFS pre-walk: recurse to populate visited[].
        if (child) astro_emit_ctx_visit(ctx, child);
        return;
    }
    // Emit phase.
    if (child == NULL) { fprintf(fp, "NULL"); return; }
    int id = astro_emit_ctx_lookup(ctx, child);
    if (id < 0) {
        // Should never happen — collect phase visited every reachable
        // NODE.  Defensive: emit NULL.
        fprintf(fp, "NULL");
        return;
    }
    if (astro_emit_ctx_in_progress(ctx, child)) {
        // Back-edge to a node currently on the DFS stack — its ALLOC
        // hasn't been emitted yet, so we can't reference _n[id] (slot
        // is zero at runtime when this code executes).  Emit NULL and
        // queue a fixup.
        fprintf(fp, "NULL");
        // Note: parent_id and parent are filled in by the caller of
        // EMIT_AST_<kind> after the ALLOC line is emitted.  field_path
        // we leave to the caller too (we don't know the parent's
        // operand name from here).  Fixup is queued externally — see
        // astro_emit_ctx_emit_node().
        // Actually: we have no caller context here to record which
        // operand slot this back-edge fills.  For now, the cycle
        // resolution is done by recording the entire (parent, child)
        // edge and relying on a per-kind walker to patch.  Since the
        // backslot detection isn't precise here, emit NULL and trust
        // the caller to record a fixup post-line.  See cycle_fixup
        // logic in astro_emit_ast_c_program.
        return;
    }
    fprintf(fp, "_n[%d]", id);
}

static void
astro_emit_ctx_visit(struct astro_emit_ctx *ctx, NODE *n)
{
    if (n == NULL) return;
    int existing = astro_emit_ctx_lookup(ctx, n);
    if (existing >= 0) return;  // already visited (in this DFS), or in_progress

    // Push as in_progress before recursing into children.
    astro_emit_ctx_grow_visited(ctx);
    size_t my_slot = ctx->visited_size++;
    ctx->visited[my_slot].node = n;
    ctx->visited[my_slot].id = -1;       // assigned post-order
    ctx->visited[my_slot].in_progress = true;
    astro_emit_map_insert(ctx, n, my_slot);

    // Recurse into children by invoking EMIT_AST with the collect
    // trampoline active.  We discard the FILE * output; the
    // astro_emit_ast_c_child() collector callback above does the actual
    // recursion.
    if (n->head.kind && n->head.kind->emit_ast) {
        // Run the EMIT_AST function with a /dev/null-like sink.  We
        // don't actually need its textual output during collection;
        // we just need it to call astro_emit_ast_c_child for each
        // NODE * operand.
        FILE *sink = ctx->fp;            // any FILE * works; output ignored
        // Snapshot fp position is unnecessary — we write but caller
        // truncates fp before emit phase by writing to a separate
        // memstream during collect.  For simplicity, use a no-op file
        // (we'll later confirm fp behaviour is benign).
        (void)sink;
        // Use a scratch memstream so collect-phase scribbles don't
        // pollute the output FILE.
        char *scratch_buf = NULL;
        size_t scratch_len = 0;
        FILE *scratch = open_memstream(&scratch_buf, &scratch_len);
        if (scratch) {
            (*n->head.kind->emit_ast)(scratch, n);
            fclose(scratch);
            free(scratch_buf);
        }
    }

    // Post-order: assign ID now (after all children).
    ctx->visited[my_slot].id = ctx->next_id++;
    ctx->visited[my_slot].in_progress = false;
}

// Per-NODE emit: prefix with "_n[id] = " and append ";".  Used after
// collect-phase by astro_emit_ast_c_program.
static void
astro_emit_ctx_emit_node(struct astro_emit_ctx *ctx, FILE *fp, NODE *n)
{
    int id = astro_emit_ctx_lookup(ctx, n);
    fprintf(fp, "    _n[%d] = ", id);
    // Run the EMIT_AST function with the emit-phase active.
    (*n->head.kind->emit_ast)(fp, n);
    fprintf(fp, ";\n");
}

// Public: write a full function definition for a self-contained AST
// builder that handles sharing and cycles.
//
// Layout of generated function:
//
//     NODE *<func_name>(void) {
//         static NODE *_n[N] = {0};
//         if (_n[ROOT]) return _n[ROOT];   // idempotent on re-call
//         _n[0] = ALLOC_node_num(1);
//         _n[1] = ALLOC_node_add(_n[0], NULL);
//         /* fixups for cycle back-edges */
//         _n[1]->u.node_add.rv = _n[2];
//         return _n[ROOT];
//     }
//
// Fixups for back-edges are NOT auto-emitted in this version because we
// don't carry per-operand-slot context through astro_emit_ast_c_child.
// Samples that exercise cycles (= recursive functions) must call
// astro_emit_ast_c_program_with_cycle_walker() and provide a callback,
// OR (simpler) defer back-edge resolution to a startup-time pass like
// callsite_resolve.  The naruby driver uses the latter.

// Look up an SD name for `n` in the framework's compile log (populated
// by astro_cs_compile when astro_cs_log_compiles is on).  Returns the
// SD function name (e.g. "SD_abcd") if `n`'s hash is known to be a
// real (non-empty) SD, NULL otherwise.  Implemented in
// astro_code_store.c via the compile_log accessors.
extern bool astro_cs_log_compiles;
extern uint32_t astro_cs_compile_log_size(void);
extern void     astro_cs_compile_log_get(uint32_t i, node_hash_t *out_hash,
                                         const char **out_name);

static const char *
astro_emit_lookup_sd(NODE *n)
{
    if (!n) return NULL;
    node_hash_t h = hash_node(n);
    uint32_t n_entries = astro_cs_compile_log_size();
    for (uint32_t i = 0; i < n_entries; i++) {
        node_hash_t eh;
        const char *en;
        astro_cs_compile_log_get(i, &eh, &en);
        if (eh == h) return en;
    }
    return NULL;
}

void
astro_emit_ast_c_program_params(FILE *fp, NODE *root,
                                const char *func_name,
                                const char *include_header,
                                const char *params)
{
    if (!params) params = "void";
    if (root == NULL) {
        fprintf(fp, "NODE *%s(%s) { return NULL; }\n", func_name, params);
        return;
    }
    struct astro_emit_ctx ctx = { 0 };
    ctx.fp = fp;
    ctx.in_collect_phase = true;
    astro_emit_ast_active_ctx = &ctx;

    // Pass 1: DFS to populate visited[] and assign post-order IDs.
    astro_emit_ctx_visit(&ctx, root);

    int root_id = astro_emit_ctx_lookup(&ctx, root);

    fprintf(fp, "// Auto-generated by astro_emit_ast_c_program.\n");
    if (include_header) {
        fprintf(fp, "#include \"%s\"\n", include_header);
    }
    fprintf(fp, "\n");

    // Forward decls for every SD we'll patch into a node's dispatcher
    // pointer below.  ASTRO_SD_PROTO is provided by node_head.h, derived
    // by ASTroGen from the first NODE_DEF's signature.
    {
        // Dedup by name against the (small) set already emitted — the same SD
        // matches every visited node sharing its hash.
        const uint32_t log_n = astro_cs_compile_log_size();
        const char **seen = log_n ? calloc(log_n, sizeof(*seen)) : NULL;
        uint32_t n_seen = 0;
        for (size_t j = 0; j < ctx.visited_size; j++) {
            const char *sd = astro_emit_lookup_sd(ctx.visited[j].node);
            if (!sd) continue;
            bool dup = false;
            for (uint32_t k = 0; k < n_seen; k++) {
                if (strcmp(seen[k], sd) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (n_seen < log_n) seen[n_seen++] = sd;
            fprintf(fp, "ASTRO_SD_PROTO(%s);\n", sd);
        }
        if (n_seen) fprintf(fp, "\n");
        free(seen);
    }

    fprintf(fp, "NODE *\n");
    fprintf(fp, "%s(%s)\n", func_name, params);
    fprintf(fp, "{\n");
    fprintf(fp, "    static NODE *_n[%zu] = {0};\n", ctx.visited_size);
    fprintf(fp, "    if (_n[%d]) return _n[%d];\n", root_id, root_id);

    // Pass 2: emit ALLOC line for each NODE, ordered by ID (post-order
    // ⇒ children before parents).  For nodes whose hash matches an SD
    // in the compile log, also patch in the SD dispatcher so the exe
    // doesn't need any runtime cs_load step.
    ctx.in_collect_phase = false;
    NODE **by_id = calloc((size_t)ctx.next_id, sizeof(*by_id));
    for (size_t j = 0; j < ctx.visited_size; j++) {
        if (ctx.visited[j].id >= 0) by_id[ctx.visited[j].id] = ctx.visited[j].node;
    }
    for (int id = 0; id < ctx.next_id; id++) {
        NODE *n = by_id[id];
        if (!n) continue;
        astro_emit_ctx_emit_node(&ctx, fp, n);
        const char *sd = astro_emit_lookup_sd(n);
        if (sd) {
            fprintf(fp,
                    "    _n[%d]->head.dispatcher = (node_dispatcher_func_t)%s;\n",
                    id, sd);
            fprintf(fp, "    _n[%d]->head.flags.is_specialized = true;\n", id);
        }
    }
    fprintf(fp, "    return _n[%d];\n", root_id);
    fprintf(fp, "}\n");

    astro_emit_ast_active_ctx = NULL;
    free(by_id);
    free(ctx.map_keys);
    free(ctx.map_slots);
    free(ctx.visited);
    free(ctx.fixups);
}

void
astro_emit_ast_c_program(FILE *fp, NODE *root,
                         const char *func_name,
                         const char *include_header)
{
    astro_emit_ast_c_program_params(fp, root, func_name, include_header, NULL);
}
