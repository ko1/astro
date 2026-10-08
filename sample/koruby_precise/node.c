/* koruby_precise v2 — node.c: AST infrastructure glue.
 *
 * Pulls in the framework runtime helpers (#include style — they need our
 * NODE / node_hash_t / NodeKind types) and the ASTroGen-generated files,
 * then provides EVAL / OPTIMIZE / INIT and the code repository. */

#include "node.h"
#include "astro_code_store.h"
#include "astro_build.h"

size_t node_cnt;

/* AST nodes are immortal (v2_design §9.3): plain calloc, never freed. */
static NODE *
node_allocate(size_t size)
{
    NODE *n = calloc(1, size);
    if (!n) { fprintf(stderr, "koruby_precise: out of memory (AST)\n"); abort(); }
    node_cnt++;
    return n;
}

/* --- framework runtime helpers (order matters; see docs/usage.md) ------- */
#include "astro_node.c"
#include "astro_code_store.c"
#include "astro_build.c"

/* --build embed emitters (hand-written; used by generated node_emit_ast.c) */
#include "node_embed.c"

/* --- generated code ------------------------------------------------------ */
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_hopt.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_emit_ast.c"
#include "node_alloc.c"

RESULT
EVAL(CTX *c, NODE *n, korb_value *slots)
{
    return (*n->head.dispatcher)(c, n, slots);
}

NODE *
OPTIMIZE(NODE *n)
{
    /* No-op by design.  SD binding must happen only AFTER parse, once
     * pop_frame's bake_add fixup has finalized every offset (self_off /
     * lget off / ivar self_off / ...).  main.c relies on this: it dlopens
     * the code store (INIT) only after PARSE and then binds SDs on the
     * finalized AST via swap_in_cached_sds() (program root + code_repo
     * bodies; inner nodes are inlined into their parent's baked SD).
     *
     * Calling astro_cs_load(n) here — inside every ALLOC — was a no-op for
     * the main program (store not yet loaded during its parse) but ACTIVE
     * and WRONG for require'd / eval'd files, which are parsed at runtime
     * with the store already dlopen'd: it bound each node to SD_<hash> using
     * the PRE-fixup offset, then pop_frame changed the offset but left the
     * stale dispatcher, so e.g. `self` in a require'd class body resolved to
     * the wrong frame slot (nil) under --compiled-only.  Runtime-parsed code
     * now runs on the generic dispatchers, which read the fixed-up field. */
    return n;
}

void
INIT(void)
{
    /* src_dir must be absolute (KORUBY_SRC_DIR): generated SDs `#include` node.h
     * via this path, and the binary may run from any CWD (e.g. optcarrot from the
     * ROM dir).  A "." here only worked when run from the source dir. */
    extern uint64_t koruby_store_version(void);   /* main.c */
    astro_cs_init("code_store", KORUBY_SRC_DIR, koruby_store_version());
}

/* --- code repository ------------------------------------------------------
 * Every method body registered here becomes its own astro_cs_compile entry:
 * call sites dispatch through body->head.dispatcher at runtime, which the
 * specializer cannot constant-fold (docs/usage.md "Entry nodes"). */

static struct code_repo {
    uint32_t size, capa;
    struct code_entry {
        const char *name;
        NODE *body;
        bool skip_specialize;   /* duplicate hash — SD already emitted */
    } *entries;
} code_repo;

NODE *
code_repo_find(node_hash_t h)
{
    if (h != 0) {
        for (uint32_t i = 0; i < code_repo.size; i++) {
            if (HASH(code_repo.entries[i].body) == h) {
                return code_repo.entries[i].body;
            }
        }
    }
    return NULL;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    if (body == NULL) return;
    /* Registration must be cheap: HASH has no cache (see astro_node.c), so a
     * dedup scan here recomputes every registered body's subtree hash —
     * O(bodies²), ~0.26 s of the startup for the 1037-body prelude.  The
     * force=true path (all of parse) therefore skips dedup entirely; duplicate
     * hashes are deduped where they matter, in astro_cs_compile (in-session
     * dedup + on-disk cache + compile log).  Only the record_all path
     * (force=false; bake-coverage tooling) still scans. */
    bool found = false;
    if (!force) {
        found = code_repo_find(HASH(body)) != NULL;
        if (found) return;
    }

    if (code_repo.size == code_repo.capa) {
        uint32_t capa = code_repo.capa ? code_repo.capa * 2 : 16;
        struct code_entry *e = realloc(code_repo.entries, sizeof(*e) * capa);
        if (!e) { fprintf(stderr, "koruby_precise: out of memory (code repo)\n"); abort(); }
        code_repo.entries = e;
        code_repo.capa = capa;
    }
    struct code_entry *ce = &code_repo.entries[code_repo.size++];
    ce->name = name;
    ce->body = body;
    ce->skip_specialize = found;
}

uint32_t code_repo_count(void) { return code_repo.size; }

/* EXPERIMENT (KORUBY_ENTRY_MAP): entry name, SD name, file:line per registered body */
bool korb_srcloc_find(const struct korb_vm *vm, const struct Node *node, uint32_t *file_sym, uint32_t *line);
void
korb_entry_map_dump(const struct korb_vm *vm, const char *path)
{
    FILE *const f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < code_repo.size; i++) {
        NODE *const b = code_repo.entries[i].body;
        uint32_t fs = 0, ln = 0;
        const bool ok = korb_srcloc_find(vm, b, &fs, &ln);
        fprintf(f, "SD_%lx\t%s\t%s\t%u\t%s\n", (unsigned long)HASH(b), code_repo.entries[i].name ? code_repo.entries[i].name : "-",
                ok ? korb_sym_name(vm, fs) : "?", ln, b->head.kind ? b->head.kind->default_dispatcher_name : "?");
    }
    fclose(f);
}
NODE *code_repo_body_at(uint32_t i) { return code_repo.entries[i].body; }
const char *code_repo_name_at(uint32_t i) { return code_repo.entries[i].name; }
bool code_repo_skip_specialize_at(uint32_t i) { return code_repo.entries[i].skip_specialize; }

/* ---- EXPERIMENT KORB_TYHINT ---------------------------------------------------------------- */
/* HOPT counterpart of hash_node (the recursion entry in HOPT_<name>): cached per node in head.hash_opt */
node_hash_t
hash_node_opt(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    const node_hash_t h = (*n->head.kind->hopt_func)(n);
    n->head.hash_opt = h; n->head.flags.has_hash_opt = true;
    return h;
}
node_hash_t
hash_node_opt_root(NODE *n)
{
    return n ? (*n->head.kind->hopt_func)(n) : 0;
}
void
korb_ty_observe(CTX *c, NODE *n, unsigned lk, unsigned rk)
{
    struct korb_vm *const vm = c->vm;
    if (!n->head.flags.ty_observed) {
        n->head.flags.ty_observed = true; n->head.flags.ty_l = n->head.flags.ty_r = 0;
        if (vm->ty_n == vm->ty_capa) {
            vm->ty_capa = vm->ty_capa ? vm->ty_capa * 2 : 1024;
            vm->ty_nodes = realloc(vm->ty_nodes, sizeof(*vm->ty_nodes) * vm->ty_capa);
            if (!vm->ty_nodes) abort();
        }
        vm->ty_nodes[vm->ty_n++] = n;
    }
    n->head.flags.ty_l |= (uint8_t)(1u << (lk - 1)); n->head.flags.ty_r |= (uint8_t)(1u << (rk - 1));
}
void
korb_tyhint_dump(const struct korb_vm *vm, const char *path)
{
    FILE *const f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < vm->ty_n; i++) {
        const NODE *const n = vm->ty_nodes[i];
        fprintf(f, "%s\t%02x\t%02x\t%u\n", n->head.kind->default_dispatcher_name, n->head.flags.ty_l, n->head.flags.ty_r, korb_tyhint_of(n));
    }
    fclose(f);
}
