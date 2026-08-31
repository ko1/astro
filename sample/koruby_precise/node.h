/* koruby_precise v2 — node.h: AST types + EVAL/DISPATCH declarations. */

#ifndef KORUBY_NODE_H
#define KORUBY_NODE_H 1

#include "context.h"
#include "precise_gc/gc.h"

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

/* Block/lambda parameter introspection (Proc#parameters) — built once at parse
 * time and hung off node_entry.param_info; read ONLY by the cold #parameters
 * method, so it adds nothing to the call/yield hot path.  kind: 0 req, 1 opt,
 * 2 rest, 3 keyreq, 4 key, 5 keyrest, 6 block.  name=0 → anonymous. */
struct korb_param_entry { uint8_t kind; uint32_t name; };
struct korb_param_info  { uint32_t n; struct korb_param_entry e[]; };

/* Inline ivar slot-cache — embedded in @ivar nodes via @ref.  Monomorphic:
 * caches the last hit slot; validated by `ivars[2*slot] == name_sym` so it is
 * GC-safe (no class pointer) and self-correcting across object layouts. */
struct korb_ivcache {
    uint32_t shape_id;   /* object shape this entry is valid for (0 = empty/invalid) */
    int32_t  slot;       /* ivar index in the values array */
};

/* Inline constant cache — embedded in const-read nodes via @ref.  The VM const
 * table is append-only, so a name's index is permanent once assigned; we cache
 * idx+1 (0 = empty) and never need to revalidate (const_vals[idx] is re-derived
 * each read, so reassignment and GC-forwarding are both seen). */
struct korb_constcache {
    uint32_t idx_plus1;
    /* vm->const_serial the entry was resolved under: an include/prepend or a
     * newly defined constant can change which entry the name resolves to. */
    uint64_t serial;
    /* Bare (lexical) const reads only: the enclosing class/module names,
     * outermost→innermost (immortal, parse-baked; NULL for explicit paths /
     * top-level).  Resolving this chain owner-scoped yields the UNIQUE innermost
     * cref class even when a same-named class exists in another namespace (a flat
     * name lookup would pick the wrong one).  Cold-path only — idx_plus1 caches
     * the result. */
    const uint32_t *owner_chain;
    uint32_t chain_len;
};

/* Per-call-site monomorphic method cache — embedded in send nodes via @ref.
 * `klass` is the receiver's dispatch-start class object; valid while serial ==
 * vm->method_serial (bumped on def AND GC, so a moved/reused class pointer can
 * never false-hit — same safety model as the VM mcache). */
/* kind discriminates which fill path wrote the cache, so a call site whose
 * receiver flips between an instance and a class (same mid resolving to both an
 * instance method and a class/singleton method) never reads a stale entry. */
/* KORB_IC_INSTANCE_VIS caches a private/protected instance method: like
 * KORB_IC_INSTANCE (resolved, no re-lookup) but node_send's inline fast path
 * deliberately does NOT match it, so the call always routes through
 * korb_send_cached which runs the visibility guard on the cached entry. */
enum korb_ic_kind { KORB_IC_INSTANCE = 0, KORB_IC_SMETHOD = 1, KORB_IC_NEW = 2, KORB_IC_INSTANCE_VIS = 3 };
/* one-shot flag for a node that must act only the first time it runs (END { }). */
struct korb_oncecell { uint8_t done; };

struct korb_inlcache {
    uint64_t serial;
    VALUE    klass;
    struct korb_method *m;
    VALUE    def_class;
    uint8_t  kind;
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
/* Cold: unconditionally heap-box a double (out-of-flonum-range / ±0.0 sign). */
RESULT korb_float_box(CTX *c, VALUE *slots, double d);
/* Float-literal pool: a deduped, GC-rooted boxed Float for a non-flonum literal. */
VALUE korb_flit_get(CTX *c, VALUE *slots, double d);
/* Hot float result: inline the flonum encode; PLT-call only on the cold box
 * path so representable results never leave the SD. */
static inline RESULT korb_flo(CTX *c, VALUE *slots, double d) {
    const VALUE imm = korb_d2flo(d);
    if (LIKELY(imm)) return RESULT_OK(imm);
    return korb_float_box(c, slots, d);
}
RESULT korb_rat_new(CTX *c, VALUE *slots, korb_sword_t num, korb_sword_t den);
RESULT korb_rat_new_v(CTX *c, VALUE *slots, VALUE num, VALUE den);   /* num/den Integer (Fixnum or Bignum) */
RESULT korb_cpx_new(CTX *c, VALUE *slots, VALUE re, VALUE im);
RESULT korb_regexp_new(CTX *c, VALUE *slots, VALUE source, uint32_t flags);
/* $~ backref accessor: kind 0=$& 1=$` 2=$' 3=$+ 100+n=$n (from the last match). */
RESULT korb_backref(CTX *c, VALUE *slots, int kind);
bool korb_re_caseeq_backref(CTX *c, VALUE *slots, VALUE pat, VALUE val);   /* Regexp#=== that sets $~ */
void korb_re_sync_floor(CTX *c);   /* push the current C-stack floor to the regex engine */
RESULT korb_method_new(CTX *c, VALUE *slots, VALUE recv, uint32_t mid);
bool   korb_num_to_d(VALUE v, double *out);
/* numeric arithmetic with a Float operand.  op: 0=+ 1=- 2=* 3=/ 4=% */
RESULT korb_num_arith(CTX *c, VALUE *slots, VALUE l, VALUE rhs, int op, uint32_t line);

/* 多倍長 Integer (bignum.c)。Integer 引数は Fixnum か Bignum。 */
RESULT korb_int_arith(CTX *c, VALUE *slots, VALUE a, VALUE b, int op, uint32_t line);  /* op 0+ 1- 2* 3/ 4% */
RESULT korb_int_pow(CTX *c, VALUE *slots, VALUE base, VALUE expv, uint32_t line);
RESULT korb_int_bitwise(CTX *c, VALUE *slots, VALUE a, VALUE b, int op);  /* op 0& 1| 2^ */
RESULT korb_int_rat_divmod(CTX *c, VALUE *slots, VALUE s, VALUE rat, int op);  /* op 0 div 1 modulo 2 divmod 3 remainder */
RESULT korb_int_intdiv(CTX *c, VALUE *slots, VALUE a, VALUE b, int op);  /* Fixnum/Bignum; op 0 div 1 modulo 2 divmod 3 remainder */
RESULT korb_int_shift(CTX *c, VALUE *slots, VALUE a, korb_sword_t amount);
RESULT korb_big_neg(CTX *c, VALUE *slots, VALUE v);
int    korb_int_cmp(VALUE a, VALUE b);
int    korb_big_flo_cmp(VALUE bi, double d);   /* exact Integer<=>double, 2 = NaN */
double korb_big_to_d(VALUE v);

/* string interpolation step: acc (String) + to_s(part) → new String */
RESULT korb_str_interp(CTX *c, VALUE *slots, VALUE_REF acc, VALUE part);

/* Array (korb_runtime.c) */
RESULT korb_ary_new(CTX *c, VALUE *slots, uint32_t capa);
RESULT korb_capture_backtrace(CTX *c, VALUE *slots);   /* snapshot vm->bt into exc at slots[0] */
RESULT korb_ary_push_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE elem);
void   korb_ary_store_at(CTX *c, VALUE ary, uint32_t i, VALUE val);   /* in-range ary[i]=val (WB) */
int32_t korb_hash_find(const KorbHash *h, VALUE key);   /* index of key in pair array, or -1 (Hash#[] fast path) */
RESULT korb_ary_concat_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE val);

/* Hash (korb_runtime.c) */
RESULT korb_hash_new(CTX *c, VALUE *slots, uint32_t capa);
RESULT korb_hash_set(CTX *c, VALUE *slots, VALUE_REF href, VALUE_REF kref, VALUE val);
RESULT korb_hash_merge_val(CTX *c, VALUE *slots, VALUE_REF href, VALUE src);

/* Range (korb_runtime.c) — begin staged (rooted), end by value */
RESULT korb_range_new(CTX *c, VALUE *slots, VALUE_REF bref, VALUE end, uint32_t exclude_end);

/* alias new old (and Module#alias_method) — copy a method under a new name. */
RESULT korb_do_alias(CTX *c, VALUE *slots, VALUE klass, uint32_t newm, uint32_t oldm);

/* Object / instance variables (korb_runtime.c) */
RESULT korb_obj_new(CTX *c, VALUE *slots, VALUE klass);
VALUE  korb_ivar_get(CTX *c, VALUE self, VALUE name_sym);
bool   korb_ivar_defined(CTX *c, VALUE self, VALUE name_sym);
bool   korb_responds_to(CTX *c, VALUE self, uint32_t mid);   /* defined?(method) */
/* write val into an existing ivar slot (cache-hit fast path; routes the WB). */
void   korb_ivar_store_at(CTX *c, struct KorbObject *o, uint32_t slot, VALUE val);
/* ivar index of `sym` (raw id) in object `shape`, or -1 if absent (shape IC).
 * inline (walks the shape parent chain) so it folds into the SDs — node_ivar_*
 * and node_send's attr-reader fast path call it without a cross-module hop. */
static inline int32_t korb_shape_index(struct korb_vm *vm, uint32_t shape, uint32_t sym)
{
    while (shape) {
        const struct korb_shape *s = &vm->shapes[shape];
        if (s->edge_sym == sym) return (int32_t)s->ivar_count - 1;
        shape = s->parent;
    }
    return -1;
}
RESULT korb_ivar_set(CTX *c, VALUE *slots, VALUE_REF selfref, VALUE name_sym, VALUE val);

/* Classes + constants (korb_runtime.c) */
RESULT korb_class_new(CTX *c, VALUE *slots, uint32_t name_sym, VALUE superclass);
RESULT korb_cvar_get(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, uint32_t soft);
RESULT korb_cvar_set(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, VALUE val);
void   korb_const_define(CTX *c, uint32_t name_sym, VALUE val);
VALUE  korb_const_get_path(struct korb_vm *vm, uint32_t name_sym);
VALUE  korb_fstr_get(CTX *c, VALUE *slots, const char *bytes, uint32_t len, uint32_t enc);
void   korb_const_define_owned(CTX *c, uint32_t name_sym, VALUE val, VALUE owner);
bool   korb_mod_hook_custom(CTX *c, VALUE mod, uint32_t mid);
bool   korb_rescue_custom_eqq(CTX *c, VALUE cls);   /* rescue clause must dispatch a user #=== */
bool   korb_default_eq_p(CTX *c, VALUE v, uint32_t mid);   /* #== / #!= is still the identity default */
const char *korb_enc_name_of(const struct korb_vm *vm, uint32_t idx);
uint32_t korb_enc_index_pub(struct korb_vm *vm, const char *name);   /* encoding name → header index (registers) */            /* encoding index → name */
bool   korb_enc_ascii_compat_idx(const struct korb_vm *vm, uint32_t idx);        /* ASCII-compatible encoding? */
bool   korb_str_enc_combine(const struct korb_vm *vm, VALUE a, VALUE b, uint32_t *out);   /* shared encoding */
RESULT korb_raise_enc_compat(CTX *c, VALUE *slots, uint32_t ea, uint32_t eb);
RESULT korb_raise_enc_compat_msg(CTX *c, VALUE *slots, const char *msg);
RESULT korb_raise_nested(CTX *c, VALUE *slots, const char *owner, const char *name, const char *msg);   /* Encoding::CompatibilityError */    /* Encoding::CompatibilityError */   /* module overrides a Module hook (const_added) */   /* owner = defining module (nil = top-level) — for Module#constants */
VALUE  korb_const_get(struct korb_vm *vm, uint32_t name_sym);   /* nil if absent */
/* command-line -I / -r: prepend a directory to $LOAD_PATH, and require a
 * feature by name (the same path search `require` itself does) */
void   korb_load_path_unshift(CTX *c, VALUE *slots, const char *dir);
RESULT korb_require_feature(CTX *c, VALUE *slots, const char *name);
void korb_seed_provided_features(CTX *c, VALUE *slots);   /* CRuby-parity pre-required features */
/* `$!` stack (per-CTX, GC-visited): push the exception on entering a rescue
 * body, pop on exit; top == `$!`.  See node_rescue / node_errinfo. */
void   korb_errinfo_push(CTX *c, VALUE v);
void   korb_errinfo_pop(CTX *c);
VALUE  korb_errinfo_top(const CTX *c);
uint32_t korb_const_index(const struct korb_vm *vm, uint32_t name_sym);  /* UINT32_MAX if absent */
uint32_t korb_const_index_owned(const struct korb_vm *vm, uint32_t name_sym, VALUE owner);   /* (name, owner) — owner-aware scoped read */
uint32_t korb_const_in_ancestry(const struct korb_vm *vm, VALUE cref, uint32_t name_sym);    /* cref's ancestry (MRO order) */
uint32_t korb_const_in_ancestry_scoped(const struct korb_vm *vm, VALUE recv, uint32_t name_sym);   /* Recv::NAME — skips Object */
RESULT korb_obj_singleton(CTX *c, VALUE *slots, VALUE obj);
void   korb_class_def_method(CTX *c, VALUE klass, uint32_t mid, NODE *body,
                             uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                             uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info);
/* attr_reader/writer/accessor: define a getter/setter on the class. */
void   korb_class_def_attr(CTX *c, VALUE klass, uint32_t mid, uint32_t ivar_sym, int is_writer);
RESULT korb_fire_method_added(CTX *c, VALUE *slots, VALUE definee, uint32_t mid);
RESULT korb_fire_def_hook(CTX *c, VALUE *slots, VALUE mod, uint32_t mid, const char *hook, uint32_t hook_len);
void   korb_class_undef_slot(KorbClass *k, VALUE cls, uint32_t mid);
/* parse-time descriptor list for node_attr (one entry per generated method). */
struct korb_attr_desc { uint32_t mid; uint32_t ivar; uint8_t is_writer; };
/* `class Name ... end`: create/find the class + run its body (self = class). */
RESULT korb_class_body(CTX *c, VALUE *slots, uint32_t name_sym, NODE *body_entry, VALUE superclass, int is_module, VALUE enclosing);
RESULT korb_sclass_body(CTX *c, VALUE *slots, NODE *body_entry, VALUE recv, VALUE enclosing);
RESULT korb_do_include(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods);
RESULT korb_do_prepend(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods);
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
RESULT korb_minus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line);   /* `-` cold ladder (complex/float/rat/array/user/raise) */
RESULT korb_user_binop(CTX *c, VALUE *slots, VALUE l, VALUE rhs, const char *op, bool *handled);
RESULT korb_try_coerce(CTX *c, VALUE *slots, VALUE l, VALUE rhs, const char *op, uint32_t line, bool *handled);   /* node_div coerce path */
RESULT korb_make_proc(CTX *c, VALUE *slots, struct Node *entry, VALUE *def_env, VALUE self_val, uint32_t is_lambda);
RESULT korb_make_binding(CTX *c, VALUE *slots, VALUE *frame_base, const uint32_t *scope_tbl, uint32_t name_cnt, VALUE self_val);
const uint32_t *korb_binding_tbl_flat(const uint32_t *syms, uint32_t cnt);   /* single-level packed scope table */
void   korb_env_store(CTX *c, struct KorbEnv *e, uint32_t index, VALUE v);
RESULT korb_str_mod(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs);
RESULT korb_rat_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op);
RESULT korb_cpx_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op);
RESULT korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line);
bool   korb_value_eq(VALUE a, VALUE b);

/* ---- receiver method dispatch (x.foo) — enum/fn in context.h ------------- */
enum korb_class korb_class_of(VALUE v);
VALUE korb_class_obj_of(CTX *c, VALUE self);   /* the class object of a value (NilClass for nil, etc.) */
const char *korb_class_name(enum korb_class cls);
void   korb_def_cmethod(CTX *c, enum korb_class cls, const char *name,
                        korb_method_fn fn, int32_t arity);
void   korb_def_cmethod_blk(CTX *c, enum korb_class cls, const char *name,
                            korb_method_blk_fn fn, int32_t arity);
/* Dispatch `recv.mid(args)`: recv at slots[-argc-1], args at slots[-argc..]. */
RESULT korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc);
RESULT korb_send_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                        struct korb_inlcache *ic, VALUE caller_self);   /* caller_self = KORB_UNDEF → no visibility check */
RESULT korb_call_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                        struct korb_callcache *cc, struct korb_inlcache *ic,
                        uint32_t argc, VALUE self);
/* Implicit-self keyword call: positionals at base[0..pos_argc), keyword VALUEs at
 * base[pos_argc..+kw_argc) named by kw_syms[]; binds keywords without a Hash when
 * the callee allows (else materializes one). */
RESULT korb_call_kw(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, struct korb_callcache *cc,
                    struct korb_inlcache *ic, uint32_t pos_argc, const uint32_t *kw_syms,
                    uint32_t kw_argc, VALUE self);
/* Same, with a literal block (recv.mid(args) { ... }) handed to the method.
 * `captured_self` is the caller's self (the block's lexical self). */
RESULT korb_send_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                     uint32_t argc, NODE *block, VALUE *def_env, VALUE *captured_self);

/* &block forwarding sentinel: when passed as `def_env`, the block was forwarded
 * from a Proc and `captured_self` points to the (rooted) Proc slot — korb_block_
 * yield re-reads proc->env / proc->self from it each yield (GC-safe; the Proc may
 * move during the call, but its slot is scanned and its iseq is immortal). */
#define KORB_BLK_FWD ((VALUE *)2)

/* `block` sentinel: a forwarded Symbol#to_proc / Method#to_proc proc (iseq == NULL,
 * so there is no block frame to build).  Paired with def_env == KORB_BLK_FWD and a
 * captured_self pointing at the (scanned) Proc slot; korb_block_yield re-reads the
 * proc from *captured_self each yield and dispatches the send (GC-safe). */
#define KORB_BLK_CPROC ((NODE *)2)

/* Invoke a block (node_entry + def_env) with `argc` args from `argv`.  The
 * block frame is laid out at the cursor `slots`; its self cell gets
 * `captured_self`.  NEXT is folded to NORMAL. */
RESULT korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                        const VALUE *argv, uint32_t argc, VALUE *captured_self);

/* `break` ownership — see CTX::break_blk.  The innermost block yield a break
 * passes through claims it by recording which block entry raised it; the call
 * site that was handed that same entry as a *literal* block is the one the
 * break unwinds to.  Identity is the entry node alone: the env a block runs
 * with changes when it is promoted into a Proc (`&b`), so it cannot be part of
 * the key.  `break` in a lambda is a plain return from the lambda. */
static inline RESULT korb_break_claim(CTX *c, RESULT r, struct Node *block, bool is_lambda) {
    if (r.state == KORB_BREAK) {
        if (is_lambda) r.state = KORB_NORMAL;
        else if (c->break_blk == NULL) c->break_blk = block;
    }
    return r;
}
/* True when a pending KORB_BREAK is this call's to swallow.  A forwarded block
 * (`m(&b)`) is never this call's — it belongs to whoever wrote the literal.
 * An unclaimed break counts as mine, so C paths that never claim keep working.
 * Clears the claim. */
static inline bool korb_break_owned(CTX *c, const struct Node *block, const VALUE *def_env) {
    if (def_env == KORB_BLK_FWD) return false;
    if (c->break_blk != NULL && c->break_blk != block) return false;
    c->break_blk = NULL;
    return true;
}

/* keyword-parameter metadata for a method (NULL on the method if no keywords).
 * `slot` is the local index; `deflt` NULL = required keyword. */
struct korb_kw_entry { uint32_t mid; uint32_t slot; struct Node *deflt; };
struct korb_kw_info  { uint32_t count; int32_t kwrest_slot; struct korb_kw_entry *entries; };

/* method machinery */
void   korb_method_define(CTX *c, uint32_t mid, NODE *body,
                          uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                          uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info);
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
                     VALUE self, NODE *block, VALUE *def_env, VALUE *captured_self);

/* Coerce a `&obj` block argument to a Proc (obj.to_proc), writing it back to *pslot. */
RESULT korb_blockarg_to_proc(CTX *c, VALUE *slots, VALUE *pslot, uint32_t line);

/* yield to the current method's block.  `block_entry` / `def_env` /
 * `captured_self` are the (odd-tagged) reserved frame cells. */
RESULT korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
                  VALUE block_entry, VALUE def_env, VALUE *captured_self);
RESULT korb_yield_outer(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
                        VALUE prev_handle, uint32_t depth, int32_t trio_base);
VALUE *korb_outer_frame_base(VALUE prev_handle, uint32_t depth);
VALUE *korb_outer_frame_base_at(VALUE *ep_cell, VALUE prev_handle, uint32_t depth);
RESULT korb_exc_ivar_set(CTX *c, VALUE *slots, VALUE_REF excref, VALUE name_sym, VALUE val);   /* set an exception ivar (e.g. @__name), usable from node_eval.c */

/* Pattern-matching (`expr in/ => pattern`) compiled descriptor + runtime matcher.
 * kind: 0 binding (write base[bind_off]), 1 value (value_node === subject),
 * 2 array ([elems...] exact length), 3 hash ({keys[i]: elems[i]...}). */
struct korb_pat {
    uint8_t  kind;
    int32_t  bind_off;            /* kind 0/4: frame slot offset (baked); kind 6: *rest slot (-1 = anonymous) */
    struct Node *value_node;      /* kind 1: NODE to EVAL → `pat === subject` */
    uint32_t n;                   /* kind 2/3: count; kind 6: pre-rest count */
    uint32_t npost;               /* kind 6: post-rest count (elems[n..n+npost) are the post patterns) */
    struct korb_pat **elems;      /* kind 2/4/5/6: sub-patterns; kind 3: per-key patterns */
    VALUE   *keys;                /* kind 3: symbol keys */
};
RESULT korb_pat_match(CTX *c, VALUE *base, VALUE *cur, VALUE_REF subjref, const struct korb_pat *p);

/* node_entry field accessors (block body metadata) — defined in node.def's
 * node_entry struct; korb_yield reads them off the node_entry NODE. */
uint32_t korb_entry_params_cnt(NODE *entry);
uint32_t korb_entry_locals_cnt(NODE *entry);
NODE    *korb_entry_body(NODE *entry);

/* raise + uncaught-exception report */
RESULT korb_raise(CTX *c, VALUE *slots, unsigned int etype, uint32_t line,
                  const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void   korb_report_uncaught(CTX *c, VALUE exc);
int    korb_system_exit_status(CTX *c, VALUE exc);   /* SystemExit → its status, else -1 */
int    korb_drain_at_exit(CTX *c, VALUE *slots);   /* run at_exit blocks (main.c); >=0 = exit status a handler asked for */
void   korb_io_flush_std(struct korb_vm *vm);     /* flush stdout/stderr (no stdio to do it at exit) */

/* a Hash the call site wrote as keyword arguments (KORB_FL_KWARGS) */
static inline bool korb_kwargs_hash_p(VALUE v) {
    return KORB_HASH_P(v) && (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_KWARGS) != 0;
}

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
    KORB_E_RANGE,
    KORB_E_INDEX,
    KORB_E_KEY,
    KORB_E_FROZEN,
    KORB_E_UNCAUGHT_THROW,
    KORB_E_STOP_ITERATION,
    KORB_E_MATH_DOMAIN,
    KORB_E_FLOAT_DOMAIN,
    KORB_E_NO_MATCHING_PATTERN,
    KORB_E_NO_MATCHING_PATTERN_KEY,
    KORB_E_SYNTAX,
    KORB_E_LOADERR,
    KORB_E_IOERROR,
    KORB_E_REGEXP,
};

/* class names for messages: "Integer" / "an instance of String" forms */
const char *korb_type_name(VALUE v);
const char *korb_a_type_name(VALUE v);
/* coercion-error form: nil/true/false spelled out, a user instance as its own
 * class name rather than the generic "Object" */
const char *korb_coerce_name(CTX *c, VALUE v);
/* a constant name registered for autoload on `mod` but not yet loaded */
bool korb_autoload_registered_p(CTX *c, VALUE mod, uint32_t sym);
void korb_const_set_private(CTX *c, VALUE owner, uint32_t sym, bool private_p);   /* private_constant */
bool korb_const_private_p(const struct korb_vm *vm, VALUE owner, uint32_t sym);
void korb_const_set_deprecated(CTX *c, VALUE owner, uint32_t sym);
bool korb_const_deprecated_p(const struct korb_vm *vm, VALUE owner, uint32_t sym);
void korb_const_deprecated_warn(CTX *c, VALUE *slots, VALUE owner, uint32_t sym);
void korb_class_desc_into(CTX *c, VALUE cls, char *out, size_t outsz);
void korb_name_error_where(CTX *c, VALUE *slots, VALUE *excp, uint32_t name, VALUE recv);
NODE *koruby_parse_source_at(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, bool exit_on_error);
RESULT korb_rescue_splat_list(CTX *c, VALUE *slots, VALUE *listslot);
bool korb_defined_call_p(CTX *c, VALUE *slots, uint32_t mid, int32_t self_off);
VALUE korb_cvar_cref(VALUE self, VALUE entry_cell);
VALUE korb_cvar_self_class_pub(CTX *c, VALUE self);   /* @@var fallback scope for a rebound self */
VALUE korb_const_cref(VALUE self, VALUE entry_cell);   /* like korb_cvar_cref, but keeps a `class << obj` singleton */
VALUE korb_cvar_owner(VALUE cref, VALUE sym, int32_t *idx_out);
struct korb_method *korb_super_find(CTX *c, uint32_t mid, VALUE entry_cell, VALUE self, VALUE *out_def_class);
VALUE korb_cref_resolve(struct korb_vm *vm, const uint32_t *chain, uint32_t chain_len, uint32_t owner_name);

/* mutation guard: raise FrozenError (returning from the caller) if `v` is a
 * frozen heap object.  Used at the top of in-place mutators. */
RESULT korb_raise_frozen(CTX *c, VALUE *slots, VALUE v);   /* "can't modify frozen <Type>: <inspect>" */
RESULT korb_check_def_frozen(CTX *c, VALUE *slots, VALUE definee);   /* def on a frozen class / singleton-of-frozen-obj → FrozenError */
#define KORB_CHECK_FROZEN(c, slots, v) do {                                    \
    const VALUE _kf = (v);                                                     \
    if (UNLIKELY(AROH_IS_GC_OBJECT(_kf) &&                                     \
                 (((const AroObjectHeader *)(uintptr_t)_kf)->flags & KORB_FL_FROZEN))) \
        return korb_raise_frozen((c), (slots), _kf);                           \
} while (0)
/* leaner variant for hot paths where `v` is already known to be a heap object
 * of a known type (skips the tag check). */
#define KORB_CHECK_FROZEN_HEAP(c, slots, v, tname) do {                        \
    if (UNLIKELY(((const AroObjectHeader *)(uintptr_t)(v))->flags & KORB_FL_FROZEN)) \
        return korb_raise_frozen((c), (slots), (v));                           \
} while (0)

/* string→integer parse (Fixnum or, on overflow with GMP, a Bignum); base 0 =
 * auto-detect 0x/0b/0o/leading-0.  Used by node_bignum (beyond-Fixnum literal). */
bool korb_str_to_int(CTX *c, VALUE *slots, const char *s, uint32_t len, int base, VALUE *out);

/* symbols */
uint32_t korb_intern(struct korb_vm *vm, const char *name, size_t len);
bool korb_responds_to_public(CTX *c, VALUE self, uint32_t mid);
bool korb_responds_to_coerce(CTX *c, VALUE *slots, VALUE self, uint32_t mid);   /* defined in korb_runtime.c; called from node_eval.c (defined?) */
bool korb_responds_to_coerce_p(CTX *c, VALUE *slots, VALUE *selfp, uint32_t mid);
RESULT korb_massign_coerce(CTX *c, VALUE *slots);   /* to_ary for multiple assignment (node_eval.c) */
void korb_reg_srcloc(struct korb_vm *vm, struct Node *node, uint32_t file_sym, uint32_t line);
bool korb_get_srcloc(struct korb_vm *vm, const struct Node *node, uint32_t *file_sym, uint32_t *line);
RESULT korb_coerce_to_int_pub(CTX *c, VALUE *slots, VALUE *v);
RESULT korb_str_dup_pub(CTX *c, VALUE *slots, VALUE *src);
void korb_fprint_inspect_s(CTX *c, VALUE *slots, FILE *fp, VALUE v);
bool korb_find_bang_override(CTX *c, VALUE v);
void korb_relocate_object_methods(CTX *c, VALUE *slots);
RESULT korb_class_dup(CTX *c, VALUE *slots, VALUE src);
void korb_warn(CTX *c, VALUE *slots, const char *fmt, ...);
void korb_warn_ignore_verbose(CTX *c, VALUE *slots, const char *fmt, ...);   /* category warnings ($VERBOSE-independent) */
void korb_warn_const_redef(CTX *c, VALUE *slots, uint32_t name_sym, VALUE owner);
void korb_warn_const_redef_at(CTX *c, VALUE *slots, uint32_t name_sym, VALUE owner,
                              const char *file, uint32_t line0);   /* with the assignment's position */
void korb_const_reg_loc(struct korb_vm *vm, uint32_t name_sym, VALUE owner, uint32_t file_sym, uint32_t line);
bool korb_const_get_loc(const struct korb_vm *vm, uint32_t name_sym, VALUE owner, uint32_t *file_sym, uint32_t *line);
const char *korb_sym_name(const struct korb_vm *vm, uint32_t id);

/* printing (no GC allocation — writes directly to fp) */
void korb_fprint_to_s(CTX *c, FILE *fp, VALUE v);
void korb_fprint_inspect(CTX *c, FILE *fp, VALUE v);
void korb_desc_inspect(CTX *c, VALUE v, char *buf, size_t sz);
RESULT korb_raise_not_sym(CTX *c, VALUE *slots, VALUE v);
RESULT korb_raise_no_int(CTX *c, VALUE *slots, VALUE v);
/* Loop preemption: cheap probe + the actual yield (builtins/thread.c). */
bool   korb_loop_wants_yield(const CTX *c);
RESULT korb_loop_yield(CTX *c, VALUE *slots);

/* CTX lifecycle (korb_runtime.c) */
CTX *korb_ctx_new(void);
void korb_define_argv(CTX *c, int n, char *const *args, const char *prog);   /* top-level ARGV + $0 */
void korb_ctx_free(CTX *c);

/* parser entry (parse.c).  `fname` is used for diagnostics only; the
 * caller (main.c) reads the source (file / -e / stdin). */
NODE *koruby_parse_source(CTX *c, const char *src, size_t len, const char *fname, bool exit_on_error);
NODE *koruby_parse_binding_eval(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line,
                                const uint32_t *name_syms, uint32_t name_cnt);
extern uint32_t koruby_toplevel_locals_cnt;
extern const uint32_t *koruby_toplevel_local_syms;   /* toplevel local-name table (for TOPLEVEL_BINDING) */
extern uint32_t koruby_toplevel_local_cnt;

/* --build embed support.  korb_embed_* (korb_runtime.c) rebuild parse-built
 * side structures at exe startup — called from the generated _embed.c.
 * koruby_emit_set_vm (node_embed.c) points the emitters at the bake VM. */
NODE **korb_embed_nodes(uint32_t cnt, ...);
uint32_t *korb_embed_syms(CTX *c, uint32_t cnt, ...);
void *korb_embed_kw_info(CTX *c, uint32_t count, int32_t kwrest_slot, ...);
void *korb_embed_param_info(CTX *c, uint32_t n, ...);
void *korb_embed_u8(uint32_t cnt, ...);
void *korb_embed_u16(uint32_t cnt, ...);
void *korb_embed_i32(uint32_t cnt, ...);
void *korb_embed_het_descs(CTX *c, uint32_t cnt, ...);
void *korb_embed_attr_descs(CTX *c, uint32_t cnt, ...);
uint32_t *korb_embed_binding_scope(CTX *c, uint32_t L, ...);
void *korb_embed_pat(CTX *c, uint32_t kind, int32_t bind_off, struct Node *value_node,
                     uint32_t n, uint32_t npost, uint32_t ecnt, ...);
void koruby_emit_set_vm(const struct korb_vm *vm);
void koruby_emit_cstr_len(FILE *fp, const char *p, uint32_t len);

/* code repo iteration (main.c) — every method body is its own AOT entry
 * because call sites dispatch through body->head.dispatcher at runtime. */
uint32_t code_repo_count(void);
NODE *code_repo_body_at(uint32_t i);
const char *code_repo_name_at(uint32_t i);
bool code_repo_skip_specialize_at(uint32_t i);

/* Specialize a file loaded after startup (require / eval-string): bind its AST +
 * newly-registered bodies [repo_from, count) to baked SDs, and — under
 * --aot-compile/--pg-compile — compile them first.  Defined in main.c.  Call
 * once, after the file is parsed (offsets finalized).  No-op under --plain. */
void korb_load_time_specialize(NODE *ast, uint32_t repo_from, const char *file);

extern size_t node_cnt;

/* Frame-push headroom: covers in-frame expression staging without a per-node
 * check (v2_design §3.5).  Defined here (not only korb_runtime.h) so the
 * inlined call fast path below sees it in every TU, including the SDs. */
#ifndef KORB_FRAME_SLACK
#define KORB_FRAME_SLACK 1024
#endif

/* Per-frame header cells reserved BELOW the locals base (design A bottom header):
 *   base[-1] = self (staged receiver, read directly — no copy to a top cell)
 *   base[-2] = EP   (open closure env / prev link)
 *   base[-3] = magic (frame type/flags/signature — integrity)
 * @children call nodes reserve KORB_FRAME_HDR cells via the dispatcher; internal
 * dispatch (korb_send_impl/korb_call_impl) reserves them by shifting the frame. */
#define KORB_FRAME_HDR 2

/* Offset (from a frame's locals base) of the EP cell — the open-env / prev-link
 * slot.  All EP reads/writes and the closure PREV-chain walk go through this so
 * the layout is defined in one place.  (Step 1 of the bottom-header migration:
 * EP moved from base[-1] to base[-2]; self still lives at the frame top until
 * Step 2 relocates it into base[-1].) */
#define KORB_EP_OFF (-2)
static inline VALUE korb_ep_get(const VALUE *const base) { return base[KORB_EP_OFF]; }
static inline void  korb_ep_set(VALUE *const base, const VALUE v) { base[KORB_EP_OFF] = v; }

/* Per-frame "magic" cell at base[-3] (CRuby-style frame integrity marker).  Holds
 * a signature + frame-type tag; the low bit is set so the GC scan treats it as a
 * non-pointer immediate and skips it.  Reserved+zeroed on every frame's setup
 * path already; korb_frame_magic_set writes the marker and korb_frame_magic_check
 * verifies it on return — catching base mismatch / stack corruption / EP underflow.
 * Active under ASTRO_DEBUG (or -DKORB_FRAME_MAGIC); otherwise compiled out (no-op,
 * the cell stays 0 as the reserve paths leave it). */
#define KORB_MAGIC_OFF (-3)
enum korb_frame_type {
    KORB_FT_METHOD = 1, KORB_FT_BLOCK, KORB_FT_EVAL, KORB_FT_TOPLEVEL, KORB_FT_FIBER,
};
/* signature in the high bits, frame type in bits 1..7, bit 0 = 1 (GC-skip). */
#define KORB_FMAGIC(ft) (((VALUE)0xF7A3C5E900ULL) | ((VALUE)(ft) << 1) | 1u)
#if ASTRO_DEBUG || defined(KORB_FRAME_MAGIC)
static inline void korb_frame_magic_set(VALUE *const base, const enum korb_frame_type ft) {
    base[KORB_MAGIC_OFF] = KORB_FMAGIC(ft);
}
static inline void korb_frame_magic_check(const VALUE *const base, const enum korb_frame_type ft, const char *const where) {
    const VALUE got = base[KORB_MAGIC_OFF], want = KORB_FMAGIC(ft);
    if (UNLIKELY(got != want)) {
        fprintf(stderr, "koruby_precise: FRAME MAGIC mismatch at %s: base=%p got=0x%llx want=0x%llx "
                "(frame corruption / wrong base / EP underflow)\n",
                where, (const void *)base, (unsigned long long)got, (unsigned long long)want);
        abort();
    }
}
#else
static inline void korb_frame_magic_set(VALUE *const base, const enum korb_frame_type ft) { (void)base; (void)ft; }
static inline void korb_frame_magic_check(const VALUE *const base, const enum korb_frame_type ft, const char *const where) { (void)base; (void)ft; (void)where; }
#endif

/* Cold helpers used by the inlined simple-call fast path below; defined in
 * korb_runtime.c (the SD / all.so reaches them as exported symbols, only on
 * the rare open-env-close / exception-backtrace paths). */
RESULT korb_close_ret(CTX *c, VALUE *scratch, VALUE *frame_base, RESULT r);
void   korb_bt_append(struct korb_vm *vm, uint32_t line, const char *name);

/* Streamlined invocation of a "simple" ISEQ method (only required positional
 * params; the caller guarantees m->is_simple).  always_inline so it folds into
 * the dispatch site — both korb_call_cached / korb_send_cached (korb_runtime.c)
 * and node_call's specialized dispatcher (code_store SD) inline it, removing a
 * cross-module call layer on the hot recursive / method-call path. */
/* True if frame `base` owns an open env in its EP cell base[-1] (a clean even
 * KorbEnv whose loc is this frame) → its return must close it.  base[-1] is the
 * receiver cell (staged by the caller): consumable as the EP once self is read. */
static inline bool korb_frame_escaped(const VALUE *base) {
    const VALUE pv = korb_ep_get(base);
    return pv != 0 && (pv & 1u) == 0 && VAL2ENV(pv)->loc == base;
}

static inline __attribute__((always_inline, no_stack_protector)) RESULT
korb_invoke_simple(CTX *c, VALUE *slots, struct korb_method *m, uint32_t argc,
                   uint32_t line, uint32_t mid, VALUE self, VALUE def_class)
{
    if (UNLIKELY(argc != (uint32_t)m->params_cnt))
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
    VALUE *const base = slots - argc;
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit))
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    /* zero only the genuine locals (body locals + synth temps) that the body
     * may read/GC-scan; the top cell (method-entry) is set just below, so zeroing
     * it is wasted (a hot-path win for arg-only methods like fib).  Inline the
     * small counts (the vast majority of methods) to skip the __memset PLT call. */
    if (locals_cnt - 1 > argc) {
        const uint32_t nz = locals_cnt - 1 - argc;
        VALUE *const z = base + argc;
        switch (nz) {
            case 4: z[3] = 0;   /* fallthrough */
            case 3: z[2] = 0;   /* fallthrough */
            case 2: z[1] = 0;   /* fallthrough */
            case 1: z[0] = 0; break;
            default: memset(z, 0, nz * sizeof(VALUE));
        }
    }
    base[locals_cnt - 1] = (VALUE)((uintptr_t)m | 1u);   /* method entry at frame top (tagged); super/__method__ source */
    korb_ep_set(base, 0);                                 /* EP cell (base[-2]): no open env yet */
    korb_frame_magic_set(base, KORB_FT_METHOD);           /* base[-3] integrity marker (no-op unless KORB_FRAME_MAGIC) */
    /* self is already at base[-1] (the caller's staged receiver) — not copied to a
     * top cell.  Every korb_invoke_simple caller stages self there (bottom header). */
    (void)def_class; (void)self;
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) {
        /* Consume only a return targeted at this method (NULL = nearest-method,
         * the common case) — a block's `return` aimed at an outer method passes
         * through unchanged. */
        if (c->return_target == NULL || c->return_target == base) {
            r.state = KORB_NORMAL;
            c->return_target = NULL;
        }
    }
    else if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {   /* non-exception RAISE payload (thread kill / throw) → no backtrace */
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid));
        e->line = line;
    }
    korb_frame_magic_check(base, KORB_FT_METHOD, "korb_invoke_simple");   /* frame integrity (no-op unless KORB_FRAME_MAGIC) */
    if (UNLIKELY(korb_frame_escaped(base))) r = korb_close_ret(c, base + locals_cnt, base, r);
    return r;
}

#endif /* KORUBY_NODE_H */
