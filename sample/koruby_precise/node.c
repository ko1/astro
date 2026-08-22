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
#include "node_specialize.c"
#include "node_replace.c"
#include "node_emit_ast.c"
#include "node_alloc.c"

RESULT
EVAL(CTX *c, NODE *n, VALUE *slots)
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
    astro_cs_init("code_store", KORUBY_SRC_DIR, 0);
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
    bool found = code_repo_find(HASH(body)) != NULL;
    if (!force && found) return;

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
NODE *code_repo_body_at(uint32_t i) { return code_repo.entries[i].body; }
const char *code_repo_name_at(uint32_t i) { return code_repo.entries[i].name; }
bool code_repo_skip_specialize_at(uint32_t i) { return code_repo.entries[i].skip_specialize; }
