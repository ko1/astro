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

/* Float (heap-boxed double).  korb_num_to_d extracts a double from an Integer
 * or Float (returns false if neither). */
RESULT korb_float_new(CTX *c, VALUE *slots, double d);
bool   korb_num_to_d(VALUE v, double *out);
/* numeric arithmetic with a Float operand.  op: 0=+ 1=- 2=* 3=/ 4=% */
RESULT korb_num_arith(CTX *c, VALUE *slots, VALUE l, VALUE rhs, int op, uint32_t line);

/* string interpolation step: acc (String) + to_s(part) → new String */
RESULT korb_str_interp(CTX *c, VALUE *slots, VALUE_REF acc, VALUE part);

/* Array (korb_runtime.c) */
RESULT korb_ary_new(CTX *c, VALUE *slots, uint32_t capa);
RESULT korb_ary_push_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE elem);

/* Hash (korb_runtime.c) */
RESULT korb_hash_new(CTX *c, VALUE *slots, uint32_t capa);
RESULT korb_hash_set(CTX *c, VALUE *slots, VALUE_REF href, VALUE_REF kref, VALUE val);

/* Range (korb_runtime.c) — begin staged (rooted), end by value */
RESULT korb_range_new(CTX *c, VALUE *slots, VALUE_REF bref, VALUE end, uint32_t exclude_end);

/* Object / instance variables (korb_runtime.c) */
RESULT korb_obj_new(CTX *c, VALUE *slots, VALUE klass);
VALUE  korb_ivar_get(VALUE self, VALUE name_sym);
RESULT korb_ivar_set(CTX *c, VALUE *slots, VALUE_REF selfref, VALUE name_sym, VALUE val);

/* Classes + constants (korb_runtime.c) */
RESULT korb_class_new(CTX *c, VALUE *slots, uint32_t name_sym, VALUE superclass);
void   korb_const_define(CTX *c, uint32_t name_sym, VALUE val);
VALUE  korb_const_get(struct korb_vm *vm, uint32_t name_sym);   /* nil if absent */
void   korb_class_def_method(CTX *c, VALUE klass, uint32_t mid, NODE *body,
                             uint32_t params_cnt, uint32_t locals_cnt, uint32_t uses_block);
/* attr_reader/writer/accessor: define a getter/setter on the class. */
void   korb_class_def_attr(CTX *c, VALUE klass, uint32_t mid, uint32_t ivar_sym, int is_writer);
/* parse-time descriptor list for node_attr (one entry per generated method). */
struct korb_attr_desc { uint32_t mid; uint32_t ivar; uint8_t is_writer; };
/* `class Name ... end`: create/find the class + run its body (self = class). */
RESULT korb_class_body(CTX *c, VALUE *slots, uint32_t name_sym, NODE *body_entry, VALUE superclass);
/* `super`: invoke mid from def_class's superclass, same self.  args at slots[-argc..]. */
RESULT korb_super(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                  VALUE def_class, VALUE self, NODE *block, VALUE *def_env, VALUE captured_self);

/* Exception hierarchy (korb_runtime.c) */
void korb_init_exception_classes(CTX *c, VALUE *slots);
void korb_init_builtin_classes(CTX *c, VALUE *slots);   /* Object/Integer/String/... class objects */
bool korb_exc_matches(CTX *c, VALUE exc, VALUE rescue_class);   /* exc kind_of? rescue_class */

/* binop slow paths (fast paths live in node.def bodies) */
RESULT korb_plus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);
RESULT korb_mul_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);
RESULT korb_sub_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);
RESULT korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line);
bool   korb_value_eq(VALUE a, VALUE b);

/* ---- receiver method dispatch (x.foo) — enum/fn in context.h ------------- */
enum korb_class korb_class_of(VALUE v);
const char *korb_class_name(enum korb_class cls);
void   korb_def_cmethod(CTX *c, enum korb_class cls, const char *name,
                        korb_method_fn fn, int32_t arity);
void   korb_def_cmethod_blk(CTX *c, enum korb_class cls, const char *name,
                            korb_method_blk_fn fn, int32_t arity);
/* Dispatch `recv.mid(args)`: recv at slots[-argc-1], args at slots[-argc..]. */
RESULT korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc);
/* Same, with a literal block (recv.mid(args) { ... }) handed to the method.
 * `captured_self` is the caller's self (the block's lexical self). */
RESULT korb_send_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                     uint32_t argc, NODE *block, VALUE *def_env, VALUE captured_self);

/* Invoke a block (node_entry + def_env) with `argc` args from `argv`.  The
 * block frame is laid out at the cursor `slots`; its self cell gets
 * `captured_self`.  NEXT is folded to NORMAL. */
RESULT korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                        const VALUE *argv, uint32_t argc, VALUE captured_self);

/* method machinery */
void   korb_method_define(CTX *c, uint32_t mid, NODE *body,
                          uint32_t params_cnt, uint32_t locals_cnt,
                          uint32_t uses_block);
void   korb_builtin_define(CTX *c, const char *name, korb_builtin_fn fn,
                           int32_t params_cnt);
/* `self` is the callee's receiver (the caller's self for a no-receiver call);
 * korb_call writes it into the callee frame's self cell (base[fs-1]). */
RESULT korb_call(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                 struct korb_callcache *cc, uint32_t argc, VALUE self);

/* Call with a literal block (docs/v2_blocks_design.md).  `block` is a
 * node_entry NODE; `def_env` is the caller frame base (block's captured outer
 * vars); `captured_self` is the caller's self (the block's lexical self).  All
 * three land in the callee frame's reserved cells for `yield`. */
RESULT korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                     struct korb_callcache *cc, uint32_t argc,
                     VALUE self, NODE *block, VALUE *def_env, VALUE captured_self);

/* yield to the current method's block.  `block_entry` / `def_env` /
 * `captured_self` are the (odd-tagged) reserved frame cells. */
RESULT korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
                  VALUE block_entry, VALUE def_env, VALUE captured_self);

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
