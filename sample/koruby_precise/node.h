/* koruby_precise v2 — node.h: AST types + EVAL/DISPATCH declarations. */

#ifndef KORUBY_NODE_H
#define KORUBY_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *slots);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
RESULT EVAL(CTX *c, NODE *n, VALUE *slots);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

/* M0 has no profile-guided hash; HORG/HOPT both fall back to the
 * structural hash (the code store references HOPT only when PG naming is
 * active, which M0 never enables). */
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

#define DISPATCHER_NAME(n) \
    ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

NODE *code_repo_find(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;      /* referenced by astro_code_store.c (PGC path) */
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;
    const struct NodeKind *kind;
    node_hash_t hash_value;
    node_hash_t hash_opt;       /* PGC identity — unused in M0 */
    int32_t line;               /* PGC index key — unused in M0 */
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;
};

/* Inline method-call cache — embedded in call nodes via @ref.  Valid while
 * serial matches vm->method_serial. */
struct korb_callcache {
    uint64_t serial;
    struct korb_method *m;
};

/* node_head.h provides NodeKind, per-node structs, the Node union, and
 * ALLOC_* declarations. */
#include "node_head.h"

/* ---- runtime helpers (korb_runtime.c) — the slots ABI surface ---------- */

/* korb_alloc: THE publish point (c->slots_top = slots) + allocation.
 * Returns the zero-initialized payload with head.flags = type. */
void *korb_alloc(CTX *c, VALUE *slots, size_t size, unsigned int type);

RESULT korb_str_new(CTX *c, VALUE *slots, const char *bytes, uint32_t len);

/* binop slow paths (fast paths live in node.def bodies) */
RESULT korb_plus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);
RESULT korb_mul_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);
RESULT korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line);
bool   korb_value_eq(VALUE a, VALUE b);

/* method machinery */
void   korb_method_define(CTX *c, uint32_t mid, NODE *body,
                          uint32_t params_cnt, uint32_t locals_cnt,
                          uint32_t uses_block);
void   korb_builtin_define(CTX *c, const char *name, korb_builtin_fn fn,
                           int32_t params_cnt);
RESULT korb_call(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                 struct korb_callcache *cc, uint32_t argc);

/* Call with a literal block (M1, docs/v2_blocks_design.md).  `block` is a
 * node_entry NODE carrying {body, params_cnt, locals_cnt}; `def_env` is the
 * caller's frame base (slots pointer to the block's captured outer vars).
 * The callee receives them in its frame's top 2 cells for `yield`. */
RESULT korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                     struct korb_callcache *cc, uint32_t argc,
                     NODE *block, VALUE *def_env);

/* yield to the current method's block.  `block_entry` / `def_env` are the
 * (odd-tagged) frame-top cells, read by the node_yield body. */
RESULT korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
                  VALUE block_entry, VALUE def_env);

/* node_entry field accessors (block body metadata) — defined in node.def's
 * node_entry struct; korb_yield reads them off the node_entry NODE. */
uint32_t korb_entry_params_cnt(NODE *entry);
uint32_t korb_entry_locals_cnt(NODE *entry);
NODE    *korb_entry_body(NODE *entry);

/* raise + uncaught-exception report */
RESULT korb_raise(CTX *c, VALUE *slots, unsigned int etype, uint32_t line,
                  const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void   korb_report_uncaught(CTX *c, VALUE exc);

enum korb_etype {
    KORB_E_RUNTIME = 0,
    KORB_E_TYPE,
    KORB_E_ARGUMENT,
    KORB_E_ZERODIV,
    KORB_E_NOMETHOD,
    KORB_E_SYSSTACK,
    KORB_E_NOTIMPL,
    KORB_E_NAME,
    KORB_E_LOCALJUMP,
};

/* class names for messages: "Integer" / "an instance of String" forms */
const char *korb_type_name(VALUE v);
const char *korb_a_type_name(VALUE v);

/* symbols */
uint32_t korb_intern(struct korb_vm *vm, const char *name, size_t len);
const char *korb_sym_name(const struct korb_vm *vm, uint32_t id);

/* printing (no GC allocation — writes directly to fp) */
void korb_fprint_to_s(CTX *c, FILE *fp, VALUE v);
void korb_fprint_inspect(CTX *c, FILE *fp, VALUE v);

/* CTX lifecycle (korb_runtime.c) */
CTX *korb_ctx_new(void);
void korb_ctx_free(CTX *c);

/* parser entry (parse.c).  `fname` is used for diagnostics only; the
 * caller (main.c) reads the source (file / -e / stdin). */
NODE *koruby_parse_source(CTX *c, const char *src, size_t len, const char *fname);
extern uint32_t koruby_toplevel_locals_cnt;

/* code repo iteration (main.c) — every method body is its own AOT entry
 * because call sites dispatch through body->head.dispatcher at runtime. */
uint32_t code_repo_count(void);
NODE *code_repo_body_at(uint32_t i);
bool code_repo_skip_specialize_at(uint32_t i);

extern size_t node_cnt;

#endif /* KORUBY_NODE_H */
