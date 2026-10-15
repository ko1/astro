/* koruby_precise v2 — node.h: AST types + EVAL/DISPATCH declarations. */

#ifndef KORUBY_NODE_H
#define KORUBY_NODE_H 1

#include "context.h"
#include "precise_gc/gc.h"
#include "astro_hole.h"

typedef struct Node NODE;
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, korb_value *slots);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
RESULT EVAL(CTX *c, NODE *n, korb_value *slots);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

/* M0 has no profile-guided hash; HORG/HOPT both fall back to the
 * structural hash (the code store references HOPT only when PG naming is
 * active, which M0 never enables). */
#define HORG(n) HASH(n)
#define HOPT(n) hash_node_opt_root(n)   /* EXPERIMENT KORB_TYHINT: node_hopt.c (storageless type hints in the key) */
node_hash_t hash_node_opt(struct Node *n);        /* node.c */
node_hash_t hash_node_opt_root(struct Node *n);

#define DISPATCHER_NAME(n) \
    ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

NODE *code_repo_find(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value : 1;
        bool has_hash_opt : 1;      /* referenced by astro_code_store.c (PGC path) */
        bool is_specialized : 1;
        bool is_specializing : 1;
        bool is_dumping : 1;
        bool no_inline : 1;
        bool is_vcall : 1;          /* bare identifier (`foo`): a miss is NameError */
        bool yield_simple : 1;      /* node_entry block: korb_block_yield's fast path applies (set on first yield) */
        bool ty_observed : 1;       /* EXPERIMENT KORB_TYHINT: ty_l / ty_r hold the operand kinds this site saw */
        uint8_t ty_l, ty_r;         /* bit (kind - 1) per korb_ty_kind seen (a body can be the site itself, so not head.line) */
        uint8_t sd_inl;             /* node_send: which AOT-only inline body its SD calls (enum korb_sdi, 0 = none) */
        uint16_t ran;               /* EXPERIMENT KORB_COLDCUT: interpreter dispatches of this node, saturating (DISPATCH_* prologue) */
    } flags;
    const struct NodeKind *kind;
    node_hash_t hash_value;
    node_hash_t hash_opt;       /* PGC identity — unused in M0 */
    int32_t line;               /* PGC index key — unused in M0 */
    uint32_t nholes;            /* SPECIALIZE-time: hole count of this subtree's SD */
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;
    const astro_hole_t *pool;   /* hole table of this SD instance (idea_code_store.md §7) */
};
_Static_assert(sizeof(struct NodeFlags) <= 8, "NodeFlags must not grow the node head");
/* NodeHead carries pool/nholes: enables the framework's pool-mode paths. */
#define ASTRO_NODEHEAD_POOL 1

/* Inline method-call cache — embedded in call nodes via @ref.  Valid while
 * serial matches vm->method_serial. */
/* Top-level (`self` is main) function calls land here rather than in the
 * inline cache: there is no receiver class to key on, only the method serial.
 * Fattened for the same reason korb_inlcache is — the hot path used to walk
 * cc->m to read `kind`/`is_simple`, then korb_invoke_simple walked m->body and
 * body->head.dispatcher, touching two more cache lines per call.  fib / tak /
 * ackermann / method_call are all top-level defs, so that was the main
 * recursion path. */
struct korb_callcache {
    uint64_t serial;
    struct korb_method *m;
    struct Node *body;               /* = m->simple_entry (dispatch の第 2 引数) */
    /* non-NULL であることが「この (サイト, callee) 組が simple entry 高速路に乗る」の
     * 判定そのもの。別フラグは要らない — どのみち呼ぶために load する。 */
    node_dispatcher_func_t dispatch; /* m->simple_entry->head.dispatcher at fill time */
};

/* Block/lambda parameter introspection (Proc#parameters) — built once at parse
 * time and hung off node_entry.param_info; read ONLY by the cold #parameters
 * method, so it adds nothing to the call/yield hot path.  kind: 0 req, 1 opt,
 * 2 rest, 3 keyreq, 4 key, 5 keyrest, 6 block, 7 nokey (`**nil`),
 * 8 noblock (`&nil`).  name=0 → anonymous. */
struct korb_param_entry { uint8_t kind; uint32_t name; };
struct korb_param_info  { uint32_t n; uint8_t r2k; struct korb_param_entry e[]; };   /* r2k: Proc#ruby2_keywords — per ISEQ, so a dup'd Proc shares it */

/* `&nil` is always the last entry, so the "refuses a block" test is one load. */
static inline bool
korb_pi_no_block(const void *const param_info)
{
    const struct korb_param_info *const pi = (const struct korb_param_info *)param_info;
    return pi != NULL && pi->n != 0 && pi->e[pi->n - 1].kind == 8;
}

/* Local-variable NAMES of one ISEQ scope, indexed by frame slot — built once at
 * parse time and hung off the scope's entry: korb_method.locals_info for a
 * method, node_entry.locals_info for a block / class body / lambda.  This is
 * what lets C name the locals of an arbitrary live frame (debugger): the frame
 * identity cell gives the entry, the entry gives the names.  names[i] == 0 is an
 * anonymous slot; slots at or past `n` are synthetic temporaries and never
 * named.  Cold-read only, so it costs nothing on any call path. */
struct korb_locals_info { uint32_t n; uint32_t names[]; };

/* Binding scope table (parse.c kp_binding_scope_tbl): set on a triple's DEPTH
 * word for an implicit parameter (`it`), which #implicit_parameter_* reaches
 * but #local_variable_* must not see. */
#define KORB_BIND_IMPLICIT 0x80000000u
/* …and for an anonymous parameter slot (`*`, `**`, `&`, `...`): only an eval'd
 * string's forwarding reaches it, never #local_variable_* */
#define KORB_BIND_ANON     0x40000000u
#define KORB_BIND_DEPTH(t) ((t)[1] & ~(KORB_BIND_IMPLICIT | KORB_BIND_ANON))

/* Inline ivar slot-cache — embedded in @ivar nodes via @ref.  Monomorphic:
 * caches the last hit slot; validated by `ivars[2*slot] == name_sym` so it is
 * GC-safe (no class pointer) and self-correcting across object layouts. */
struct korb_ivcache {
    uint32_t shape_id;   /* object shape this entry is valid for (0 = empty/invalid), or KORB_IVC_CLASS */
    int32_t  slot;       /* ivar index in the values array; for a class, the pair index in its class_ivars hash */
    uint32_t from_shape; /* node_ivar_set: the shape that transitions to shape_id by adding this ivar (0 = none) */
};
/* node_cvar_get's: in one method (key = its frame identity, which fixes the lexical cref) the @@x it read —
 * the defining class and the pair's position in its cvars hash.  Valid while no class variable was
 * defined / removed (cvar_serial), no MRO changed and no GC moved the class (method_serial). */
struct korb_cvcache {
    uint64_t   serial;     /* method_serial */
    uint64_t   cvserial;   /* cvar_serial */
    uint64_t   mro;        /* mro_serial */
    korb_value key;        /* the method frame's identity cell (KORB_FID_METHOD) */
    korb_value owner;      /* the class whose cvars hold it */
    int32_t    idx;
};
#define KORB_IVC_CLASS UINT32_MAX   /* never a real shape id: the receiver was a class (validated by the key at `slot`) */
/* node_ivar_get's: two shapes (a reader shared by several classes, attr_reader's entry above all).  The second
 * entry also remembers a shape that has no such ivar (slot2 = -1: the read is nil) without a walk each time. */
struct korb_ivgcache {
    uint32_t shape_id;   /* as korb_ivcache (or KORB_IVC_CLASS) */
    int32_t  slot;
    uint32_t shape_id2;  /* the entry the last fill pushed out, or a shape without the ivar */
    int32_t  slot2;      /* -1: absent */
};

/* Inline constant cache — embedded in const-read nodes via @ref.  The VM const
 * table is append-only, so a name's index is permanent once assigned; we cache
 * idx+1 (0 = empty) and never need to revalidate (const_vals[idx] is re-derived
 * each read, so reassignment and GC-forwarding are both seen). */
/* node_const_path's cache (`A::B`): the owner by its GC-stable class serial */
struct korb_cpcache {
    uint32_t idx_plus1;
    uint32_t parent_serial;
    uint64_t serial;          /* vm->const_serial; private_constant / deprecate_constant bump it too */
};

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
    uint32_t cref_serial;   /* a read whose cref is unnamed (`class << self`): that cref's KorbClass.serial (GC-stable) */
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
enum korb_ic_kind { KORB_IC_INSTANCE = 0, KORB_IC_SMETHOD = 1, KORB_IC_NEW = 2, KORB_IC_INSTANCE_VIS = 3, KORB_IC_SEND = 4 };
/* one-shot flag for a node that must act only the first time it runs (END { }). */
struct korb_oncecell { uint8_t done; };
/* node_str_frozen: the literal's slot in the fstring pool; node_regexp: in vm->once_vals (+1; 0 = not yet).  The
 * pool is append-only and a GC root, so the index stays valid while the value moves. */
struct korb_fscache { uint32_t idx_plus1; };
/* a super site's last answer: (self's class, the running entry) → method.  Dies at every GC (classes may move,
 * method_serial bumps) and is refilled from the forwarded vm->scache; a site rarely sees a second (class, entry). */
struct korb_supcache { uint64_t serial; korb_value klass; const struct korb_method *cur; struct korb_method *m; korb_value def_class; };

/* Fat inline cache: besides the resolved entry it copies what the simple-ISEQ
 * fast path needs (body, its dispatcher, frame sizes), so a cached call touches
 * only the site's own cache line — not the korb_method (libc heap) and the body
 * node header as well.  optcarrot: ~1.5 L1d misses per 0-arg self-call, ~30% of
 * them on those two lines.  `simple` is set only for a plain ISEQ that
 * korb_invoke_simple can run; the serial guard keeps it coherent (refinements
 * stamp a never-matching serial; a code-store swap bumps method_serial). */
#define KORB_CASEEQ_MARK 0x8000FFFFu   /* korb_inlcache.smid of a class's Module#=== cache (korb_caseeq_class_cached) */
#define KORB_SDI_MARK 0x80000000u   /* korb_inlcache.smid of an instance cache whose method is an SDI builtin: MARK | sdi_k */
struct korb_inlcache {
    uint64_t serial;
    korb_value    klass;
    struct korb_method *m;
    korb_value    def_class;
    struct Node *body;               /* = m->simple_entry (dispatch の第 2 引数) */
    /* non-NULL であることが「この (サイト, callee) 組が simple entry 高速路に乗る」の
     * 判定そのもの。別フラグは要らない — どのみち呼ぶために load する。 */
    node_dispatcher_func_t dispatch; /* m->simple_entry->head.dispatcher at fill time */
    uint8_t  kind;
    uint8_t  shared;                 /* EXPERIMENT acct45: an interned, shared cache (in kind's pad): never written through korb_ic_fill */
    uint8_t  poly;                   /* EXPERIMENT acct45 (a node's own cache): the site missed while shared -> keeps its own cache for good */
    uint8_t  cfunc_ok;               /* EXPERIMENT acct46: 0 = no; else m is a C method callable with this site's argc, or (dispatch != NULL)
                                      * a builtin class's Ruby method with an entry, and this is the receiver's tag code korb_send_alt
                                      * compares (korb_recv_tag_is); 0xFF = compare recv == klass (a class receiver's C method), 0xFE = its
                                      * simple ISEQ */
    uint32_t smid;                   /* KORB_IC_SEND: the name `send(:name, ...)` resolved (in kind's pad) */
};

/* node_head.h provides NodeKind, per-node structs, the Node union, and
 * ALLOC_* declarations. */
#include "node_head.h"

/* EXPERIMENT KORB_COLDCUT: a subtree the producing run never dispatched is not baked into its
 * entry's SD; the SD calls the subtree's runtime dispatcher (no_inline).  KORUBY_COLDCUT=1. */
#define KORB_NODE_RAN(n) ((n)->head.flags.ran += ((n)->head.flags.ran != UINT16_MAX))
void korb_node_walk(struct Node *n, node_walk_fn_t fn, void *a);   /* pre-order; fn returns "descend" */
unsigned korb_coldcut_threshold(void);   /* KORUBY_COLDCUT=N: 0 = off; a node / body dispatched < N times is cold */
struct korb_cc_stats { uint32_t entries, entries_ran, nodes, cold; };
void korb_coldcut_pass(struct Node *root, struct korb_cc_stats *st);   /* marks !ran nodes under root no_inline */
void korb_coldcut_report(const char *path);                            /* KORUBY_COLDCUT_DUMP: per-kind dispatch-count buckets */

/* ---- runtime helpers (korb_runtime.c) — the slots ABI surface ---------- */

/* korb_alloc: THE publish point (c->slots_top = slots) + allocation.
 * Returns the zero-initialized payload with head.flags = type. */
void *korb_alloc(CTX *c, korb_value *slots, size_t size, unsigned int type);

RESULT korb_str_new(CTX *c, korb_value *slots, const char *bytes, uint32_t len);

/* Float (heap-boxed double).  korb_num_to_d extracts a double from an Integer
 * or Float (returns false if neither). */
RESULT korb_float_new(CTX *c, korb_value *slots, double d);
/* Cold: unconditionally heap-box a double (out-of-flonum-range / ±0.0 sign). */
RESULT korb_float_box(CTX *c, korb_value *slots, double d);
/* Float-literal pool: a deduped, GC-rooted boxed Float for a non-flonum literal. */
korb_value korb_flit_get(CTX *c, korb_value *slots, double d);
/* Hot float result: inline the flonum encode; PLT-call only on the cold box
 * path so representable results never leave the SD. */
static inline RESULT korb_flo(CTX *c, korb_value *slots, double d) {
    const korb_value imm = korb_d2flo(d);
    if (LIKELY(imm)) return RESULT_OK(imm);
    return korb_float_box(c, slots, d);
}
RESULT korb_rat_new(CTX *c, korb_value *slots, korb_sword_t num, korb_sword_t den);
RESULT korb_rat_new_v(CTX *c, korb_value *slots, korb_value num, korb_value den);   /* num/den Integer (Fixnum or Bignum) */
RESULT korb_cpx_new(CTX *c, korb_value *slots, korb_value re, korb_value im);
RESULT korb_regexp_new(CTX *c, korb_value *slots, korb_value source, uint32_t flags);
/* $~ backref accessor: kind 0=$& 1=$` 2=$' 3=$+ 100+n=$n (from the last match). */
RESULT korb_backref(CTX *c, korb_value *slots, int kind);
bool korb_re_caseeq_backref(CTX *c, korb_value *slots, korb_value pat, korb_value val);   /* Regexp#=== that sets $~ */
void korb_re_sync_floor(CTX *c);   /* push the current C-stack floor to the regex engine */
RESULT korb_method_new(CTX *c, korb_value *slots, korb_value recv, uint32_t mid);
bool   korb_num_to_d(korb_value v, double *out);
/* numeric arithmetic with a Float operand.  op: 0=+ 1=- 2=* 3=/ 4=% */
RESULT korb_num_arith(CTX *c, korb_value *slots, korb_value l, korb_value rhs, int op, uint32_t line);

/* 多倍長 Integer (bignum.c)。Integer 引数は Fixnum か Bignum。 */
RESULT korb_int_arith(CTX *c, korb_value *slots, korb_value a, korb_value b, int op, uint32_t line);  /* op 0+ 1- 2* 3/ 4% */
RESULT korb_int_pow(CTX *c, korb_value *slots, korb_value base, korb_value expv, uint32_t line);
RESULT korb_int_bitwise(CTX *c, korb_value *slots, korb_value a, korb_value b, int op);  /* op 0& 1| 2^ */
RESULT korb_int_rat_divmod(CTX *c, korb_value *slots, korb_value s, korb_value rat, int op);  /* op 0 div 1 modulo 2 divmod 3 remainder */
RESULT korb_int_intdiv(CTX *c, korb_value *slots, korb_value a, korb_value b, int op);  /* Fixnum/Bignum; op 0 div 1 modulo 2 divmod 3 remainder */
RESULT korb_int_shift(CTX *c, korb_value *slots, korb_value a, korb_sword_t amount);
RESULT korb_big_neg(CTX *c, korb_value *slots, korb_value v);
int    korb_int_cmp(korb_value a, korb_value b);
int    korb_big_flo_cmp(korb_value bi, double d);   /* exact Integer<=>double, 2 = NaN */
double korb_big_to_d(korb_value v);
double korb_int_ratio_to_d(korb_value num, korb_value den);   /* nearest double of num/den (Integers) */

/* string interpolation step: acc (String) + to_s(part) → new String */
RESULT korb_str_interp(CTX *c, korb_value *slots, korb_ref acc, korb_value part, uint32_t owned);

/* Array (korb_runtime.c) */
RESULT korb_ary_new(CTX *c, korb_value *slots, uint32_t capa);
RESULT korb_capture_backtrace(CTX *c, korb_value *slots, const korb_value *fbase);   /* position list into the exc at slots[0]; fbase = the rescuing frame's base (NULL: unwound frames only) */
RESULT korb_ary_push_val(CTX *c, korb_value *slots, korb_ref aref, korb_value elem);
void   korb_ary_store_at(CTX *c, korb_value ary, uint32_t i, korb_value val);   /* in-range ary[i]=val (WB) */
int32_t korb_hash_find(const KorbHash *h, korb_value key);   /* index of key in pair array, or -1 (Hash#[] fast path) */
/* korb_hash_find's immediate-key path without its frame: an immediate is eql? only to itself, so a small
 * (unindexed) Hash is scanned by identity in place (railsbench: 919 of 1,573 lookups a request, 56% misses). */
static inline int32_t korb_hash_find_inl(const KorbHash *const h, const korb_value key) {
    if (LIKELY(h->idx_mask == 0 && (key & 7u) != 0 && !FLONUM_P(key))) {   /* fixnum / symbol / true / false (a flonum is eql? by value: 0.0 and -0.0) */
        const korb_value *const d = korb_hash_pairs(h);
        for (uint32_t i = 0; i < h->len; i++) if (d[2 * i] == key) return (int32_t)i;
        return -1;
    }
    return korb_hash_find(h, key);
}
RESULT korb_ary_concat_val(CTX *c, korb_value *slots, korb_ref aref, korb_value val, uint32_t line);

/* Hash (korb_runtime.c) */
RESULT korb_hash_new(CTX *c, korb_value *slots, uint32_t capa);
RESULT korb_hash_set(CTX *c, korb_value *slots, korb_ref href, korb_ref kref, korb_value val);
RESULT korb_hash_merge_val(CTX *c, korb_value *slots, korb_ref href, korb_value src, uint32_t line);

/* Range (korb_runtime.c) — begin staged (rooted), end by value */
RESULT korb_range_new(CTX *c, korb_value *slots, korb_ref bref, korb_value end, uint32_t exclude_end);

/* alias new old (and Module#alias_method) — copy a method under a new name. */
RESULT korb_do_alias(CTX *c, korb_value *slots, korb_value klass, uint32_t newm, uint32_t oldm);

/* Object / instance variables (korb_runtime.c) */
RESULT korb_obj_new(CTX *c, korb_value *slots, korb_value klass);
korb_value  korb_ivar_get(CTX *c, korb_value self, korb_value name_sym);
bool   korb_ivar_defined(CTX *c, korb_value self, korb_value name_sym);
bool   korb_responds_to(CTX *c, korb_value self, uint32_t mid);   /* defined?(method) */
/* write val into an existing ivar slot (cache-hit fast path; routes the WB). */
void   korb_ivar_store_at(CTX *c, struct KorbObject *o, uint32_t slot, korb_value val);
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
RESULT korb_ivar_set(CTX *c, korb_value *slots, korb_ref selfref, korb_value name_sym, korb_value val);

/* Classes + constants (korb_runtime.c) */
RESULT korb_class_new(CTX *c, korb_value *slots, uint32_t name_sym, korb_value superclass);
RESULT korb_cvar_get(CTX *c, korb_value *slots, korb_value self, korb_value entry_cell, uint32_t sym_id, uint32_t soft);
RESULT korb_cvar_get_c(CTX *c, korb_value *slots, korb_value self, korb_value entry_cell, uint32_t sym_id, uint32_t soft, struct korb_cvcache *cc, uint32_t line);   /* fills cc */
korb_value  korb_cvar_block_cref(CTX *c, const korb_value *frame_base);
void   korb_method_clear_def_cref(korb_value klass, uint32_t mid);   /* a def in a class body of an eval: no eval cref */   /* a block's lexical class for @@x (nil: no home method) */
void   korb_method_set_block_cref(CTX *c, korb_value klass, uint32_t mid, const korb_value *frame_base);   /* a def run by a block in a method: that method's class */
RESULT korb_cvar_set(CTX *c, korb_value *slots, korb_value self, korb_value entry_cell, uint32_t sym_id, korb_value val);
void   korb_const_define(CTX *c, uint32_t name_sym, korb_value val);
korb_value  korb_const_get_path(struct korb_vm *vm, uint32_t name_sym);
bool   korb_const_path_missing(struct korb_vm *vm, uint32_t name_sym, char *out, size_t outsz);   /* first unresolvable segment of a dotted path */
korb_value  korb_fstr_get(CTX *c, korb_value *slots, const char *bytes, uint32_t len, uint32_t enc);
uint32_t    korb_fstr_slot(const struct korb_vm *vm, korb_value v);
korb_value  korb_fstr_find(const struct korb_vm *vm, const char *bytes, uint32_t len, uint32_t enc);   /* the pool entry, or 0 */
void   korb_const_define_owned(CTX *c, uint32_t name_sym, korb_value val, korb_value owner);
bool   korb_mod_hook_custom(CTX *c, korb_value mod, uint32_t mid);
bool   korb_rescue_custom_eqq(CTX *c, korb_value cls);   /* rescue clause must dispatch a user #=== */
bool   korb_default_eq_p(CTX *c, korb_value v, uint32_t mid);   /* #== / #!= is still the identity default */
bool   korb_obj_eq_default_p(CTX *c, korb_value v, bool neq);   /* a user instance's #== (and #!=) is still BasicObject's */
bool   korb_big_over_long(korb_value v);   /* a Bignum no C long can hold (index args refuse only these) */
const char *korb_enc_name_of(const struct korb_vm *vm, uint32_t idx);
const char *korb_enc_inspect_name_of(const struct korb_vm *vm, uint32_t idx);   /* "BINARY (ASCII-8BIT)" in error messages */
uint32_t korb_enc_index_pub(struct korb_vm *vm, const char *name);   /* encoding name → header index (registers) */            /* encoding index → name */
bool   korb_enc_ascii_compat_idx(const struct korb_vm *vm, uint32_t idx);        /* ASCII-compatible encoding? */
bool   korb_str_enc_combine(const struct korb_vm *vm, korb_value a, korb_value b, uint32_t *out);   /* shared encoding */
/* CRuby rb_cmperr: "comparison of <class of l> with <inspect-or-class of r>
 * failed: <coercion was not possible | comparator returned nil>". */
RESULT korb_raise_cmperr(CTX *c, korb_value *slots, uint32_t line, korb_value l, korb_value r, bool coercion);
/* Fire a pending autoload on an intermediate component of a baked class path. */
RESULT korb_const_path_autoload(CTX *c, korb_value *slots, uint32_t path_sym, bool *fired);
RESULT korb_raise_enc_compat(CTX *c, korb_value *slots, uint32_t ea, uint32_t eb);
RESULT korb_raise_enc_compat_msg(CTX *c, korb_value *slots, const char *msg);
RESULT korb_raise_nested(CTX *c, korb_value *slots, const char *owner, const char *name, const char *msg);   /* Encoding::CompatibilityError */    /* Encoding::CompatibilityError */   /* module overrides a Module hook (const_added) */   /* owner = defining module (nil = top-level) — for Module#constants */
korb_value  korb_const_get(struct korb_vm *vm, uint32_t name_sym);   /* nil if absent */
/* command-line -I / -r: prepend a directory to $LOAD_PATH, and require a
 * feature by name (the same path search `require` itself does) */
void   korb_load_path_unshift(CTX *c, korb_value *slots, const char *dir);
RESULT korb_require_feature(CTX *c, korb_value *slots, const char *name);
void korb_seed_provided_features(CTX *c, korb_value *slots);   /* CRuby-parity pre-required features */
/* `$!` stack (per-CTX, GC-visited): push the exception on entering a rescue
 * body, pop on exit; top == `$!`.  See node_rescue / node_errinfo. */
void   korb_errinfo_push(CTX *c, korb_value v);
void   korb_errinfo_pop(CTX *c);
korb_value  korb_errinfo_top(const CTX *c);
uint32_t korb_const_index(const struct korb_vm *vm, uint32_t name_sym);  /* UINT32_MAX if absent */
uint32_t korb_const_index_owned(const struct korb_vm *vm, uint32_t name_sym, korb_value owner);   /* (name, owner) — owner-aware scoped read */
void korb_const_index_sync(struct korb_vm *vm);   /* rebuild the (name, owner) index after a tombstone */
uint32_t korb_const_in_ancestry(const struct korb_vm *vm, korb_value cref, uint32_t name_sym);    /* cref's ancestry (MRO order) */
uint32_t korb_const_in_ancestry_al(CTX *c, korb_value cref, uint32_t name_sym, korb_value *al_mod);
uint32_t korb_const_in_ancestry_scoped_al(CTX *c, korb_value recv, uint32_t name_sym, korb_value *al_mod);   /* scoped, same */    /* same; stops at a pending autoload */
uint32_t korb_const_in_ancestry_scoped(const struct korb_vm *vm, korb_value recv, uint32_t name_sym);   /* Recv::NAME — skips Object */
RESULT korb_obj_singleton(CTX *c, korb_value *slots, korb_value obj);
RESULT korb_definee_class(CTX *c, korb_value *slots);   /* c->def_definee, an instance_eval receiver's singleton made on first use */
/* `uses_block` operand of node_def/node_singleton_def: a small flags word. */
#define KORB_DEFF_USES_BLOCK   1u
#define KORB_DEFF_USES_BACKREF 2u
/* `def m(...)`: koruby compiles the forwarding parameter to a plain *rest, so the
 * method has to carry keywords through it exactly the way ruby2_keywords does. */
#define KORB_DEFF_FWD_ALL      4u
void   korb_class_def_method(CTX *c, korb_value klass, uint32_t mid, NODE *body, NODE *entry,
                             uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                             uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info, void *locals_info);
/* attr_reader/writer/accessor: define a getter/setter on the class. */
void   korb_class_def_attr(CTX *c, korb_value klass, uint32_t mid, uint32_t ivar_sym, int is_writer);
RESULT korb_fire_method_added(CTX *c, korb_value *slots, korb_value definee, uint32_t mid);
korb_value korb_object_class(const struct korb_vm *vm);
RESULT korb_fire_def_hook(CTX *c, korb_value *slots, korb_value mod, uint32_t mid, const char *hook, uint32_t hook_len);
void   korb_class_undef_slot(struct korb_vm *vm, KorbClass *k, korb_value cls, uint32_t mid);
/* parse-time descriptor list for node_attr (one entry per generated method). */
struct korb_attr_desc { uint32_t mid; uint32_t ivar; uint8_t is_writer; };
bool korb_attr_overridden_p(CTX *c, korb_value klass, uint32_t mid);
/* `class Name ... end`: create/find the class + run its body (self = class). */
enum { KORB_CB_MODULE = 1, KORB_CB_TOP = 2 };   /* korb_class_body flags; TOP = `class ::C` */
RESULT korb_class_body(CTX *c, korb_value *slots, uint32_t name_sym, NODE *body_entry, korb_value superclass, int flags, korb_value enclosing, const korb_value *caller_base, uint32_t caller_line);
RESULT korb_sclass_body(CTX *c, korb_value *slots, NODE *body_entry, korb_value recv, korb_value enclosing, const korb_value *caller_base, uint32_t caller_line);
RESULT korb_do_include(CTX *c, korb_value *slots, korb_value klass, korb_slice mods);
RESULT korb_do_prepend(CTX *c, korb_value *slots, korb_value klass, korb_slice mods);
/* `super`: invoke mid from def_class's superclass, same self.  args at slots[-argc..]. */
RESULT korb_super(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc,
                  korb_value def_class, korb_value self, NODE *block, korb_value *def_env, korb_value captured_self,
                  const korb_value *frame_top, struct korb_supcache *const sc);
/* node_super0..3: argc args staged at slots[-argc..-1]; bi/de/cs are the method's block trio cells */
static inline RESULT korb_super_staged(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc,
                                       korb_value dc, korb_value self, korb_value bi, korb_value de, korb_value cs,
                                       const korb_value *frame_base, struct korb_supcache *const sc) {
    NODE *block = NULL; korb_value *def_env = NULL; korb_value cself = KORB_NIL;
    if ((uintptr_t)bi & 1u) {                             /* a block was passed to this frame */
        block = (NODE *)((uintptr_t)bi & ~(uintptr_t)1u);
        def_env = (korb_value *)(uintptr_t)de;
        cself = cs;
    }
    /* the window node_super_fwd hands korb_super: one cell above the unstaged cursor */
    korb_value *const w = slots - argc + 1;
    for (uint32_t i = argc; i-- > 0; ) w[i] = slots[(korb_sword_t)i - (korb_sword_t)argc];
    return korb_super(c, w + argc, mid, line, argc, dc, self, block, def_env, cself, frame_base, sc);
}

/* Exception hierarchy (korb_runtime.c) */
void korb_init_exception_classes(CTX *c, korb_value *slots);
void korb_init_builtin_classes(CTX *c, korb_value *slots);   /* Object/Integer/String/... class objects */
bool korb_exc_matches(CTX *c, korb_value exc, korb_value rescue_class);   /* exc kind_of? rescue_class */

/* binop slow paths (fast paths live in node.def bodies) */
bool korb_subclass_op_p(CTX *c, korb_value l, uint32_t mid);   /* builtin subclass with a Ruby-level operator */
RESULT korb_plus_slow(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_mul_slow(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_sub_slow(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_minus_slow(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);   /* `-` cold ladder (complex/float/rat/array/user/raise) */
RESULT korb_ivar_arith_slow(CTX *c, korb_value *slots, int32_t self_off, uint32_t name, uint32_t sub, korb_value rhs, struct korb_ivcache *ic, uint32_t line);   /* `@x +=`/`-=` off the fixnum fast path */
RESULT korb_user_binop(CTX *c, korb_value *slots, korb_value l, korb_value rhs, const char *op, bool *handled);
RESULT korb_try_coerce(CTX *c, korb_value *slots, korb_value l, korb_value *rhs, const char *op, uint32_t line, bool *handled);   /* node_div coerce path */
RESULT korb_make_proc(CTX *c, korb_value *slots, struct Node *entry, korb_value *def_env, korb_value self_val, uint32_t is_lambda);
RESULT korb_make_binding(CTX *c, korb_value *slots, korb_value *frame_base, const uint32_t *scope_tbl, uint32_t name_cnt, korb_value self_val, uint32_t cref_name);
/* `binding` is an ordinary Kernel method: dispatch a class's own #binding when
 * it has one (*handled = true), else leave the node to build the Binding. */
RESULT korb_binding_user_method(CTX *c, korb_value *slots, korb_value self, bool *handled);
/* A Kernel name the parser lowers to a node is still an ordinary method: when the
 * receiver's class defines its own, dispatch that instead (*handled = true). */
RESULT korb_kernel_user_method(CTX *c, korb_value *slots, korb_value self, uint32_t mid,
                               NODE *block, korb_value *def_env, korb_value *cself, bool *handled);
const uint32_t *korb_binding_tbl_flat(const uint32_t *syms, uint32_t cnt);   /* single-level packed scope table */
void   korb_env_store(CTX *c, struct KorbEnv *e, uint32_t index, korb_value v);
RESULT korb_str_mod(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs);
RESULT korb_rat_arith(CTX *c, korb_value *slots, korb_value l, korb_value r, int op);
RESULT korb_cpx_arith(CTX *c, korb_value *slots, korb_value l, korb_value r, int op);
RESULT korb_cmp_slow(CTX *c, korb_value *slots, korb_value l, korb_value r, int op, uint32_t line);
/* the node call sites of the four slow paths: a reopened String/Array operator sends first */
bool korb_cvar_defined_p(CTX *c, korb_value self, korb_value entry_cell, uint32_t sym_id);
RESULT korb_cmp_slow_n(CTX *c, korb_value *slots, korb_value l, korb_value r, int op, uint32_t line);
RESULT korb_plus_slow_n(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_minus_slow_n(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_mul_slow_n(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
RESULT korb_str_mod_n(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint32_t line);
bool   korb_value_eq(korb_value a, korb_value b);

/* ---- receiver method dispatch (x.foo) — enum/fn in context.h ------------- */
enum korb_class korb_class_of(korb_value v);
korb_value korb_class_obj_of(CTX *c, korb_value self);   /* the class object of a value (NilClass for nil, etc.) */
const char *korb_class_name(enum korb_class cls);
void   korb_def_cmethod(CTX *c, enum korb_class cls, const char *name,
                        korb_method_fn fn, int32_t arity);
void   korb_def_cmethod_blk(CTX *c, enum korb_class cls, const char *name,
                            korb_method_blk_fn fn, int32_t arity);
/* Dispatch `recv.mid(args)`: recv at slots[-argc-1], args at slots[-argc..]. */
RESULT korb_send(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc);
RESULT korb_send_vis(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc, korb_value caller_self);
RESULT korb_str_shl_str(CTX *c, korb_value *slots, korb_ref self, korb_ref arg);   /* node_shl: String#<< String */
bool korb_caseeq_class_cached(CTX *c, korb_value w, korb_value subj, struct korb_inlcache *ic, RESULT *out);   /* node_caseeq */
bool korb_kind_of_p(CTX *c, korb_value obj, korb_value klass);   /* obj.is_a?(klass), klass a class / module (AOT inline is_a?) */
RESULT korb_send_blk_ic(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc, NODE *block,
                        korb_value *def_env, korb_value *captured_self, korb_value flink, korb_value caller_self,
                        struct korb_inlcache *ic);   /* node_send_blk */
RESULT korb_send_blk_fwd(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc, NODE *block,
                         korb_value *def_env, korb_value *captured_self, korb_value flink, korb_value caller_self,
                         struct korb_inlcache *ic);   /* node_send_blkfwd: the method's own block passed on */
RESULT korb_call_blk_ic(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, struct korb_callcache *cc, uint32_t argc,
                        korb_value self, NODE *block, korb_value *def_env, korb_value *captured_self, struct korb_inlcache *ic);   /* node_call_blk / _blkproc */
RESULT korb_send_cfunc_hit(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, uint32_t argc, const struct korb_method *m);   /* EXPERIMENT acct46 */
RESULT korb_send_simple_hit(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, uint32_t argc, const struct korb_inlcache *ic);
RESULT korb_send_entry_hit(CTX *c, korb_value *slots, const struct korb_inlcache *ic, uint32_t argc, uint32_t mid);
RESULT korb_send_klass_hit(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, uint32_t argc,
                           struct korb_inlcache *ic, korb_value caller_self, struct korb_inlcache *site_ic);
RESULT korb_send_alt(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, uint32_t argc,
                     struct korb_inlcache *ic, korb_value caller_self, struct korb_inlcache *site_ic);   /* sdi20: node_send's miss side */
struct korb_ivgcache;
korb_value korb_ivar_get_alt(CTX *c, korb_value self, uint32_t name, struct korb_ivgcache *ic);   /* sdi20: node_ivar_get's miss side */
struct korb_ivcache;
RESULT korb_ivar_set_alt(CTX *c, korb_value *slots, korb_value *selfp, uint32_t name, korb_value val, struct korb_ivcache *ic);   /* sdi22 */
RESULT korb_send_cached(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, uint32_t argc,
                        struct korb_inlcache *ic, korb_value caller_self,
                        struct korb_inlcache *site_ic);   /* caller_self = KORB_UNDEF → no visibility check; site_ic = the node's own cache (shared-cc patch key) or NULL */
/* `site` is the call node, or NULL when there is none; read ONLY on the
 * method-miss path (for head.flags.is_vcall), never on the hot path. */
RESULT korb_call_cached(CTX *c, korb_value *slots, uint32_t mid, korb_value flink,
                        struct korb_callcache *cc, struct korb_inlcache *ic,
                        uint32_t argc, korb_value self, const NODE *site, struct korb_inlcache *site_ic);
/* EXPERIMENT acct45: fill the site's own cache, intern the shared one for the key and re-point the SD's immediates at
 * it; returns the cache the caller must use from here on (the shared one, or the site's own when not shared). */
void korb_cc_check_fail(const struct korb_inlcache *ic, uint8_t kind);
struct korb_inlcache *korb_ic_fill_site(CTX *c, struct korb_inlcache *site_ic, struct korb_inlcache *ic, uint64_t serial, korb_value klass,
                                        struct korb_method *m, korb_value def_class, uint8_t kind, uint32_t argc, uint32_t mid, uint32_t smid);
/* Implicit-self keyword call: positionals at base[0..pos_argc), keyword korb_values at
 * base[pos_argc..+kw_argc) named by kw_syms[]; binds keywords without a Hash when
 * the callee allows (else materializes one). */
RESULT korb_send_kw(CTX *c, korb_value *slots, uint32_t mid, korb_value flink, struct korb_inlcache *ic,
                    uint32_t pos_argc, const uint32_t *kw_syms, uint32_t kw_argc, korb_value caller_self);   /* node_send_kw */
RESULT korb_call_kw(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, struct korb_callcache *cc,
                    struct korb_inlcache *ic, uint32_t pos_argc, const uint32_t *kw_syms,
                    uint32_t kw_argc, korb_value self);
/* Same, with a literal block (recv.mid(args) { ... }) handed to the method.
 * `captured_self` is the caller's self (the block's lexical self). */
uint8_t korb_sdi_kind(const struct korb_vm *vm, uint32_t mid, uint32_t argc);   /* parse: node_send → enum korb_sdi */
const char *korb_sdi_eval_name(const struct Node *n, const char *base);          /* SPECIALIZE: the EVAL its SD calls */
void korb_sdi_init(CTX *c);                                                      /* after boot: fill vm->sdi_fn */
bool korb_deopt_send_ic(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc,
                       struct korb_inlcache *ic, RESULT *out);   /* the same for any other non-class receiver / public method */
bool korb_deopt_cfunc_cached(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             struct korb_inlcache *ic, RESULT *out);   /* node_aref / node_aset deopt on String/Array/Hash */
bool korb_deopt_cfunc_cached_any(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, uint32_t argc,
                                 struct korb_inlcache *ic, RESULT *out);   /* the same for any receiver (node_caseeq) */
/* A one-argument operator deopt (`==` / `!=` / `===` on a user object) through the node's cache */
static inline RESULT korb_send1_ic(CTX *c, korb_value *slots, uint32_t mid, uint32_t line, struct korb_inlcache *ic)
{
    RESULT dr;
    if (korb_deopt_send_ic(c, slots, mid, line, 1, ic, &dr)) return dr;
    return korb_send(c, slots, mid, 0, 1);
}
RESULT korb_send_blk(CTX *c, korb_value *slots, uint32_t mid, uint32_t line,   /* flink: the site's baked link (0 = staged by the node) */
                     uint32_t argc, NODE *block, korb_value *def_env, korb_value *captured_self, korb_value flink,
                     korb_value caller_self);   /* caller_self: the private/protected guard (KORB_UNDEF = none) */

/* &block forwarding sentinel: when passed as `def_env`, the block was forwarded
 * from a Proc and `captured_self` points to the (rooted) Proc slot — korb_block_
 * yield re-reads proc->env / proc->self from it each yield (GC-safe; the Proc may
 * move during the call, but its slot is scanned and its iseq is immortal). */
#define KORB_BLK_FWD ((korb_value *)2)

/* `block` sentinel: a forwarded Symbol#to_proc / Method#to_proc proc (iseq == NULL,
 * so there is no block frame to build).  Paired with def_env == KORB_BLK_FWD and a
 * captured_self pointing at the (scanned) Proc slot; korb_block_yield re-reads the
 * proc from *captured_self each yield and dispatches the send (GC-safe). */
#define KORB_BLK_CPROC ((NODE *)2)

/* Invoke a block (node_entry + def_env) with `argc` args from `argv`.  The
 * block frame is laid out at the cursor `slots`; its self cell gets
 * `captured_self`.  NEXT is folded to NORMAL. */
RESULT korb_block_yield(CTX *c, korb_value *slots, NODE *block, korb_value *def_env,
                        const korb_value *argv, uint32_t argc, korb_value *captured_self);

/* `break` ownership — see CTX::break_blk.  The innermost block yield a break
 * passes through claims it by recording which block entry raised it; the call
 * site that was handed that same entry as a *literal* block is the one the
 * break unwinds to.  Identity is the entry node alone: the env a block runs
 * with changes when it is promoted into a Proc (`&b`), so it cannot be part of
 * the key.  `break` in a lambda is a plain return from the lambda. */
static inline RESULT korb_break_claim(CTX *c, RESULT r, struct Node *block, bool is_lambda) {
    if (r.state == KORB_BREAK && c->break_blk == NULL) {   /* a break an inner block claimed passes through */
        if (is_lambda) r.state = KORB_NORMAL;
        else c->break_blk = block;
    }
    return r;
}
/* True when a pending KORB_BREAK is this call's to swallow.  A forwarded block
 * (`m(&b)`) is never this call's — it belongs to whoever wrote the literal.
 * An unclaimed break counts as mine, so C paths that never claim keep working.
 * Clears the claim. */
static inline bool korb_break_owned(CTX *c, const struct Node *block, const korb_value *def_env) {
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
bool   korb_fstring_p(const struct korb_vm *vm, korb_value v);
void   korb_method_define(CTX *c, uint32_t mid, NODE *body, NODE *entry,
                          uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                          uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info, void *locals_info,
                          bool priv);
void   korb_builtin_define(CTX *c, const char *name, korb_builtin_fn fn,
                           int32_t params_cnt);
/* `self` is the callee's receiver (the caller's self for a no-receiver call);
 * korb_call writes it into the callee frame's self cell (base[fs-1]). */
RESULT korb_call(CTX *c, korb_value *slots, uint32_t mid, uint32_t line,
                 struct korb_callcache *cc, uint32_t argc, korb_value self);

/* Call with a literal block (docs/v2_blocks_design.md).  `block` is a
 * node_entry NODE; `def_env` is the caller frame base (block's captured outer
 * vars); `captured_self` is the caller's self (the block's lexical self).  All
 * three land in the callee frame's reserved cells for `yield`. */
RESULT korb_call_blk(CTX *c, korb_value *slots, uint32_t mid, uint32_t line,
                     struct korb_callcache *cc, uint32_t argc,
                     korb_value self, NODE *block, korb_value *def_env, korb_value *captured_self);
RESULT korb_call_blk_fwd(CTX *c, korb_value *slots, uint32_t mid, uint32_t line,
                         struct korb_callcache *cc, uint32_t argc,
                         korb_value self, NODE *block, korb_value *def_env, korb_value *captured_self);   /* node_call_blkfwd */

/* Coerce a `&obj` block argument to a Proc (obj.to_proc), writing it back to *pslot. */
RESULT korb_blockarg_to_proc(CTX *c, korb_value *slots, korb_value *pslot, uint32_t line);

/* yield to the current method's block.  `block_entry` / `def_env` /
 * `captured_self` are the (odd-tagged) reserved frame cells. */
RESULT korb_yield(CTX *c, korb_value *slots, uint32_t argc, uint32_t line,
                  korb_value block_entry, korb_value def_env, korb_value *captured_self);
RESULT korb_yield_outer(CTX *c, korb_value *slots, uint32_t argc, uint32_t line,
                        const korb_value *ep, uint32_t depth, int32_t trio_base);
korb_value *korb_outer_frame_base(korb_value prev_handle, uint32_t depth);
korb_value *korb_outer_frame_base_at(korb_value *ep_cell, korb_value prev_handle, uint32_t depth);
korb_value  korb_outer_handle_at(const korb_value *frame_base, korb_value prev_handle, uint32_t depth);
/* The enclosing method's block trio, or NULL once that frame returned — then
 * *proc_out is the Proc its env kept (0 = there is no such block at all). */
korb_value *korb_outer_trio(const CTX *c, const korb_value *ep, uint32_t depth, int32_t trio_base, korb_value *proc_out);
RESULT korb_exc_ivar_set(CTX *c, korb_value *slots, korb_ref excref, korb_value name_sym, korb_value val);   /* set an exception ivar (e.g. @__name), usable from node_eval.c */

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
    korb_value   *keys;                /* kind 3: symbol keys */
    struct Node *outw;            /* root only: writes the outer-scope binds out after a match (NULL: none) */
    int32_t  dc_off;              /* root of a case/in clause: frame cells [subject, #deconstruct cache] (0: none) */
    uint32_t line;                /* the pattern's line: a #deconstruct(_keys) raise names it */
};
RESULT korb_pat_match(CTX *c, korb_value *base, korb_value *cur, korb_ref subjref, const struct korb_pat *p);

/* node_entry field accessors (block body metadata) — defined in node.def's
 * node_entry struct; korb_yield reads them off the node_entry NODE. */
uint32_t korb_entry_params_cnt(NODE *entry);
uint32_t korb_entry_locals_cnt(NODE *entry);
NODE    *korb_entry_body(NODE *entry);

/* raise + uncaught-exception report */
/* A C method's argument-count check runs before its frame is entered, so a plain
 * korb_raise there unwinds with no frame of its own and the backtrace starts at
 * the caller; CRuby names the method ("Hash#[]=").  Raises ArgumentError and
 * pushes that frame, the same one the post-call unwind pushes. */
RESULT korb_raise_arity(CTX *c, korb_value *slots, const struct korb_method *m, uint32_t mid,
                        uint32_t line, const char *fmt, ...)
                        __attribute__((format(printf, 6, 7)));
/* An operator node (node_plus / node_div / node_lt …) whose cold path answered
 * with a raise having dispatched nothing: a frame is pushed only at a dispatch
 * site, so the method CRuby shows ("Integer#/") would be missing.  No-op unless
 * nothing has named a frame for this exception yet AND the receiver really has
 * that operator — `nil + 1` raises at the call site in CRuby too. */
void korb_op_bt(CTX *c, RESULT r, korb_value recv, const char *op, uint32_t oplen, uint32_t line);
RESULT korb_raise_op_nomethod(CTX *c, korb_value *slots, uint32_t line, const char *op, korb_value recv, korb_value arg);   /* arg KORB_UNDEF: unary */
RESULT korb_raise(CTX *c, korb_value *slots, unsigned int etype, uint32_t line,
                  const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void   korb_report_uncaught(CTX *c, korb_value exc);
void   korb_report_uncaught_detailed(CTX *c, korb_value *slots, korb_value exc);
int    korb_system_exit_status(CTX *c, korb_value exc);   /* SystemExit → its status, else -1 */
int    korb_uncaught_signo(CTX *c, korb_value exc, bool *quiet);   /* signal an uncaught exc dies by, else 0 */
void   korb_die_by_signal(int sig);   /* returns only if the default action does not kill */
int    korb_drain_at_exit(CTX *c, korb_value *slots);   /* run at_exit blocks (main.c); >=0 = exit status a handler asked for */
int    korb_drain_at_exit_x(CTX *c, korb_value *slots, bool *exited);   /* same, plus "a handler called exit" */
void   korb_io_flush_std(struct korb_vm *vm);     /* flush stdout/stderr (no stdio to do it at exit) */
void   korb_io_flush_all(struct korb_vm *vm);     /* every open stream: the process is leaving */

/* a Hash the call site wrote as keyword arguments (KORB_FL_KWARGS) */
static inline bool korb_kwargs_hash_p(korb_value v) {
    return KORB_HASH_P(v) && (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_KWARGS) != 0;
}

bool korb_sdi_class_ok(CTX *c, korb_value kl, unsigned k);   /* korb_runtime.c: the AOT inline send's miss side */
korb_value korb_sdi_klass_slow(const struct korb_vm *vm, korb_value r);   /* korb_runtime.c: korb_sdi_klass past a plain instance */
struct korb_vm;
void korb_sdi_preinit(struct korb_vm *vm);   /* korb_runtime.c */

/* #respond_to?'s answer cache slot for (dispatch class, name, include_all): builtins/set.c fills it,
 * the AOT inline send reads it */
static inline uint32_t korb_rt_index(korb_value rk, uint32_t mid, bool priv) {
    return (uint32_t)(((uintptr_t)rk >> 4) ^ (mid * 2654435761u) ^ priv) & 255u;
}

/* an empty `**h` at a call site: no argument at all (CRuby 3.0) */
static inline bool korb_empty_kwsplat_p(korb_value v) {
    return korb_kwargs_hash_p(v) && VAL2HASH(v)->len == 0;
}

/* CRuby's "these were keywords" is a property of the call, not of the Hash.  We
 * carry it on the object, so a Hash bound to a fixed positional parameter has to
 * lose it here — otherwise `def o(a,h); i(a,h); end; o(1, k => v)` would still
 * reach `i`'s keyword params.  A `*rest` binding keeps the mark: that is exactly
 * what ruby2_keywords forwarding rides on. */
static inline void korb_kwargs_unmark(korb_value v) {
    if (UNLIKELY(korb_kwargs_hash_p(v)))
        ((AroObjectHeader *)(uintptr_t)v)->flags &= (uint16_t)~KORB_FL_KWARGS;
}
RESULT korb_r2k_unmarked_copy(CTX *c, korb_value *slots, korb_value *cell);   /* korb_runtime.c: *cell → a plain copy */
void korb_kwargs_unmark_pos_cold(CTX *c, korb_value *slots, korb_value *cell);   /* korb_runtime.c */
/* korb_kwargs_unmark for a fixed positional parameter: a ruby2_keywords-marked Hash is
 * also replaced by an unmarked copy (CRuby), so the caller's stays marked and distinct.
 * The common path is korb_kwargs_unmark's one test; all else is out of line. */
static inline void korb_kwargs_unmark_pos(CTX *c, korb_value *slots, korb_value *const cell) {
    if (UNLIKELY(korb_kwargs_hash_p(*cell))) korb_kwargs_unmark_pos_cold(c, slots, cell);
}

/* The persistent ruby2_keywords mark (Hash.ruby2_keywords_hash?). */
static inline bool korb_r2k_hash_p(korb_value v) {
    return KORB_HASH_P(v) && (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_R2K) != 0;
}
static inline void korb_r2k_set(korb_value v) {
    ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_R2K;
}

/* A splat call site (`f(*args)`) is the one place a ruby2_keywords-marked Hash
 * becomes keywords again: re-arm the call-scoped mark on the last spread
 * element.  Everywhere else the persistent mark is inert. */
static inline void korb_splat_kwmark(korb_value *const last) {
    if (UNLIKELY(korb_r2k_hash_p(*last))) ((AroObjectHeader *)(uintptr_t)*last)->flags |= KORB_FL_KWARGS;
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
const char *korb_type_name(korb_value v);
const char *korb_a_type_name(korb_value v);
/* coercion-error form: nil/true/false spelled out, a user instance as its own
 * class name rather than the generic "Object" */
const char *korb_coerce_name(CTX *c, korb_value v);
/* a constant name registered for autoload on `mod` but not yet loaded */
bool korb_autoload_registered_p(CTX *c, korb_value mod, uint32_t sym);
RESULT korb_autoload_fire(CTX *c, korb_value *slots, korb_value mod, uint32_t sym, uint32_t line, const korb_value *fbase);
void korb_const_set_private(CTX *c, korb_value owner, uint32_t sym, bool private_p);   /* private_constant */
bool korb_const_private_p(const struct korb_vm *vm, korb_value owner, uint32_t sym);
RESULT korb_check_const_path_private(CTX *c, korb_value *slots, korb_value enclosing, uint32_t sym, bool scoped);
RESULT korb_undef_one(CTX *c, korb_value *slots, korb_value cls, uint32_t mid);   /* `undef` / undef_method, one name */
const char *korb_re_literal_error(CTX *c, const char *pat, uint32_t len, uint32_t flags);   /* NULL = the engine accepts it */
const char *korb_re_literal_warning(CTX *c);   /* the accepted literal's compile warning, or NULL */
uint32_t korb_re_literal_derived(const char *bytes, uint32_t len, uint32_t flags);
korb_value korb_exc_ivar_get_pub(korb_value exc, korb_value name_sym);   /* an Exception's side-hash ivar */
void korb_const_set_deprecated(CTX *c, korb_value owner, uint32_t sym);
bool korb_const_deprecated_p(const struct korb_vm *vm, korb_value owner, uint32_t sym);
void korb_const_deprecated_warn(CTX *c, korb_value *slots, korb_value owner, uint32_t sym);
void korb_class_desc_into(CTX *c, korb_value cls, char *out, size_t outsz);
void korb_name_error_where(CTX *c, korb_value *slots, korb_value *excp, uint32_t name, korb_value recv);
NODE *koruby_parse_source_at(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, bool exit_on_error,
                             const char *src_enc, uint32_t cref_name,
                             const uint32_t *cref_chain, uint32_t cref_chain_len, bool in_eval);
RESULT korb_rescue_splat_list(CTX *c, korb_value *slots, korb_value *listslot);
bool korb_defined_call_p(CTX *c, korb_value *slots, uint32_t mid, int32_t self_off);
korb_value korb_cvar_cref(korb_value self, korb_value entry_cell);
korb_value korb_cvar_self_class_pub(CTX *c, korb_value self);   /* @@var fallback scope for a rebound self */
korb_value korb_eval_body_ns(const CTX *c, korb_value self);   /* instance_eval(String) on a module: its singleton, not self */
korb_value korb_const_cref(const struct korb_vm *vm, korb_value self, korb_value entry_cell);
korb_value korb_const_cref_at(const struct korb_vm *vm, korb_value self, const korb_value *id_cell);   /* a block: its home method's cref */   /* like korb_cvar_cref, but keeps a `class << obj` singleton */
korb_value korb_body_cref(CTX *c, korb_value self, korb_value entry_cell, uint32_t cref_name);   /* cref of a def/undef in a method body */
RESULT korb_undef_cref(CTX *c, korb_value *slots, korb_value self, korb_value entry_cell, uint32_t cref_name);   /* the class an `undef` acts on */
bool korb_user_const_missing_p(CTX *c, korb_value mod);
RESULT korb_const_path_miss(CTX *c, korb_value *slots, uint32_t name, uint32_t line, int32_t fb_off, struct korb_cpcache *cache);   /* node_const_path's slow path */   /* a const_missing other than Module's default */
korb_value korb_once_get(const struct korb_vm *vm, const struct Node *n);   /* `/.../o`: 0 until the first run */
void korb_once_set(struct korb_vm *vm, const struct Node *n, korb_value v);
void  korb_mark_sdef(korb_value klass, uint32_t mid);   /* the entry was written `def recv.m` */
korb_value korb_cref_from_path(struct korb_vm *vm, uint32_t sym);   /* "A::B" (one interned path) → the class/module, nil if unresolved */
korb_value korb_cvar_owner(korb_value cref, korb_value sym, int32_t *idx_out);
korb_value korb_cvar_owner_top(korb_value cref, korb_value sym, int32_t *idx_out, korb_value *front_out);   /* topmost owner + nearest one */
RESULT korb_cvar_check_overtaken(CTX *c, korb_value *slots, korb_value front, korb_value top, uint32_t sym_id);
void korb_cvar_class_name(CTX *c, korb_value cls, char *out, size_t outsz);
struct korb_method *korb_super_find(CTX *c, uint32_t mid, korb_value entry_cell, korb_value self, korb_value *out_def_class);
korb_value korb_cref_resolve(struct korb_vm *vm, const uint32_t *chain, uint32_t chain_len, uint32_t owner_name);
bool korb_const_object_fallback_p(const struct korb_vm *vm, korb_value cref);   /* a Class whose chain misses Object gets no top-level fallback */

/* mutation guard: raise FrozenError (returning from the caller) if `v` is a
 * frozen heap object.  Used at the top of in-place mutators. */
RESULT korb_raise_frozen(CTX *c, korb_value *slots, korb_value v);   /* "can't modify frozen <Type>: <inspect>" */
/* Same, but reported at `line` — for a write done inline, which has no dispatch
 * to re-aim the raise at its call site. */
RESULT korb_raise_frozen_at(CTX *c, korb_value *slots, korb_value v, uint32_t line);
RESULT korb_unchill(CTX *c, korb_value *slots, korb_value v);   /* chilled String: warn once, drop the mark, allow the mutation */
RESULT korb_check_def_frozen(CTX *c, korb_value *slots, korb_value definee);   /* def on a frozen class / singleton-of-frozen-obj → FrozenError */
/* The genuinely-frozen path stays a plain tail return, so the guard's shape is
 * unchanged for it.  Only a CHILLED String (a Ruby 3.4 literal) takes the second
 * bit test and the warn-then-continue call — and that happens once per object,
 * since korb_unchill clears the mark. */
#define KORB_CHECK_FROZEN_AT(c, slots, v, line) do {                           \
    const korb_value _kf = (v);                                                     \
    if (UNLIKELY(AROH_IS_GC_OBJECT(_kf) &&                                     \
                 (((const AroObjectHeader *)(uintptr_t)_kf)->flags & KORB_FL_FROZEN))) { \
        if (((const AroObjectHeader *)(uintptr_t)_kf)->flags & KORB_FL_CHILLED) \
            { const RESULT _fr = korb_unchill((c), (slots), _kf);              \
              if (UNLIKELY(_fr.state != KORB_NORMAL)) return _fr; }            \
        else return korb_raise_frozen_at((c), (slots), _kf, (line));           \
    }                                                                          \
} while (0)
#define KORB_CHECK_FROZEN(c, slots, v) KORB_CHECK_FROZEN_AT((c), (slots), (v), 0)
/* leaner variant for hot paths where `v` is already known to be a heap object
 * of a known type (skips the tag check). */
#define KORB_CHECK_FROZEN_HEAP_AT(c, slots, v, tname, line) do {               \
    const uint16_t _hf = ((const AroObjectHeader *)(uintptr_t)(v))->flags;     \
    if (UNLIKELY(_hf & KORB_FL_FROZEN)) {                                      \
        if (_hf & KORB_FL_CHILLED)                                             \
            { const RESULT _fr = korb_unchill((c), (slots), (v));              \
              if (UNLIKELY(_fr.state != KORB_NORMAL)) return _fr; }            \
        else return korb_raise_frozen_at((c), (slots), (v), (line));           \
    }                                                                          \
} while (0)
#define KORB_CHECK_FROZEN_HEAP(c, slots, v, tname) KORB_CHECK_FROZEN_HEAP_AT((c), (slots), (v), (tname), 0)

/* string→integer parse (Fixnum or, on overflow with GMP, a Bignum); base 0 =
 * auto-detect 0x/0b/0o/leading-0.  Used by node_bignum (beyond-Fixnum literal). */
bool korb_str_to_int(CTX *c, korb_value *slots, const char *s, uint32_t len, int base, korb_value *out);

/* symbols */
uint32_t korb_intern(struct korb_vm *vm, const char *name, size_t len);
/* korb_intern for a C string literal: the same literal is the same name, so its address keys a per-vm cache
 * (Array#join interned "to_str" / "to_ary" per element; railsbench: 1.5% of its instructions in the symbol table) */
static inline uint32_t
korb_intern_lit(struct korb_vm *const vm, const char *const lit, const size_t len)
{
    struct korb_ilit_ent *const e = &vm->ilit[((uintptr_t)lit >> 3 ^ len) & (sizeof vm->ilit / sizeof vm->ilit[0] - 1)];
    if (LIKELY(e->p == lit && e->len == len)) return e->id;
    const uint32_t id = korb_intern(vm, lit, len);
    e->p = lit; e->len = (uint32_t)len; e->id = id;
    return id;
}
/* intern under an explicit encoding (symbol literals carry their file's). */
uint32_t korb_intern_enc(struct korb_vm *vm, const char *name, size_t len, uint32_t enc);
bool korb_responds_to_public(CTX *c, korb_value self, uint32_t mid);
bool korb_responds_to_coerce(CTX *c, korb_value *slots, korb_value self, uint32_t mid);   /* defined in korb_runtime.c; called from node_eval.c (defined?) */
bool korb_responds_to_coerce_p(CTX *c, korb_value *slots, korb_value *selfp, uint32_t mid);
RESULT korb_massign_coerce(CTX *c, korb_value *slots, uint32_t line);   /* to_ary for multiple assignment (node_eval.c) */
void korb_reg_srcloc(struct korb_vm *vm, struct Node *node, uint32_t file_sym, uint32_t line);
RESULT korb_raise_line_fill(CTX *c, RESULT r, uint32_t line);   /* a raise that named no line gets `line` */
void korb_note_source(struct korb_vm *vm, const char *fname, const char *src, size_t len, int32_t first_line, bool in_eval);
int32_t korb_cov_attach(struct korb_vm *vm, const char *fname, const char *src, size_t len, int32_t first_line, bool in_eval);
bool korb_cov_mark_line(struct korb_vm *vm, uint32_t fi, uint32_t line);
void korb_cov_first_hit(struct korb_cov_file *f, uint32_t line);
int32_t korb_cov_add_method(struct korb_vm *vm, uint32_t fi, uint32_t name, uint32_t sl, uint32_t sc, uint32_t el, uint32_t ec);
bool korb_srcloc_find(const struct korb_vm *vm, const struct Node *node, uint32_t *file_sym, uint32_t *line);
/* Record what `path` resolves to now (Location#absolute_path); NULL if no such file. */
const char *korb_reg_abspath(struct korb_vm *vm, const char *path);
bool korb_get_srcloc(struct korb_vm *vm, const struct Node *node, uint32_t *file_sym, uint32_t *line);
/* Ruby 4.1 #source_range: the extent of a def/block/lambda node (see korb_srcrange). */
void korb_reg_srcrange(struct korb_vm *vm, struct Node *node, uint32_t start_col, uint32_t end_line, uint32_t end_col);
bool korb_get_srcrange(const struct korb_vm *restrict vm, const struct Node *node,
                       uint32_t *restrict start_col, uint32_t *restrict end_line, uint32_t *restrict end_col);
RESULT korb_coerce_to_int_pub(CTX *c, korb_value *slots, korb_value *v);
RESULT korb_str_dup_pub(CTX *c, korb_value *slots, korb_value *src);
void korb_fprint_inspect_s(CTX *c, korb_value *slots, FILE *fp, korb_value v);
bool korb_find_bang_override(CTX *c, korb_value v);
void korb_relocate_object_methods(CTX *c, korb_value *slots);
RESULT korb_singleton_copy(CTX *c, korb_value *slots, korb_value dst, korb_value src_sing);
RESULT korb_class_dup(CTX *c, korb_value *slots, korb_value src);
void korb_warn(CTX *c, korb_value *slots, const char *fmt, ...);
void korb_warn_serious(CTX *c, korb_value *slots, uint32_t mid, bool undefining);   /* __send__ / __id__ / object_id / initialize */
void korb_warn_ignore_verbose(CTX *c, korb_value *slots, const char *fmt, ...);   /* category warnings ($VERBOSE-independent) */
void korb_warn_deprecated(CTX *c, korb_value *slots, const char *msg);   /* gated on Warning[:deprecated] */
void korb_warn_at(CTX *c, korb_value *slots, const char *file, uint32_t line, const char *fmt, ...);
void korb_flush_parse_warnings(CTX *c, korb_value *slots);
void korb_set_proctitle(struct korb_vm *vm, const char *s, size_t n);   /* $0 = */
RESULT korb_re_validate_pub(CTX *c, korb_value *slots, korb_value src, uint32_t flags);   /* RegexpError for a bad pattern */
bool korb_hook_undefed_p(CTX *c, korb_value obj, uint32_t mid);         /* a hook method undef'd on obj's class */   /* prism's warnings from the last parse */
void korb_warn_caller(CTX *c, korb_value *slots, const korb_value *link, const char *fmt, ...);   /* positioned at the C method's caller */

void korb_warn_const_redef(CTX *c, korb_value *slots, uint32_t name_sym, korb_value owner);
RESULT korb_const_assign_at(CTX *c, korb_value *slots, const struct Node *n, uint32_t name_sym, korb_value ownc, korb_value val,
                            const korb_value *fbase);
void korb_warn_const_redef_at(CTX *c, korb_value *slots, uint32_t name_sym, korb_value owner,
                              const char *file, uint32_t line0);   /* with the assignment's position */
void korb_const_reg_loc(struct korb_vm *vm, uint32_t name_sym, korb_value owner, uint32_t file_sym, uint32_t line);
bool korb_const_get_loc(const struct korb_vm *vm, uint32_t name_sym, korb_value owner, uint32_t *file_sym, uint32_t *line);
const char *korb_sym_name(const struct korb_vm *vm, uint32_t id);

/* printing (no GC allocation — writes directly to fp) */
void korb_fprint_to_s(CTX *c, FILE *fp, korb_value v);
void korb_fprint_inspect(CTX *c, FILE *fp, korb_value v);
void korb_desc_inspect(CTX *c, korb_value v, char *buf, size_t sz);
RESULT korb_raise_not_sym(CTX *c, korb_value *slots, korb_value v);
RESULT korb_raise_no_int(CTX *c, korb_value *slots, korb_value v);
/* Loop preemption: cheap probe + the actual yield (builtins/thread.c). */
bool   korb_loop_wants_yield(const CTX *c);
RESULT korb_loop_yield(CTX *c, korb_value *slots);

/* CTX lifecycle (korb_runtime.c) */
CTX *korb_ctx_new(void);
void korb_define_argv(CTX *c, int n, char *const *args, const char *prog);   /* top-level ARGV + $0 */
void korb_ctx_free(CTX *c);

/* parser entry (parse.c).  `fname` is used for diagnostics only; the
 * caller (main.c) reads the source (file / -e / stdin). */
NODE *koruby_parse_source(CTX *c, const char *src, size_t len, const char *fname, bool exit_on_error);
NODE *koruby_parse_binding_eval(CTX *c, const char *src, size_t len, const char *fname, int32_t first_line, const char *src_enc,
                                const uint32_t *name_syms, uint32_t name_cnt,
                                const uint32_t *outer_names, const uint32_t *outer_cnt, uint32_t outer_levels,
                                uint32_t method_mid,
                                uint32_t cref_name, const uint32_t *cref_chain, uint32_t cref_chain_len,
                                bool cref_is_caller);
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
void *korb_embed_locals_info(CTX *c, uint32_t n, ...);
void *korb_embed_u8(uint32_t cnt, ...);
void *korb_embed_u16(uint32_t cnt, ...);
void *korb_embed_i32(uint32_t cnt, ...);
void *korb_embed_het_descs(CTX *c, uint32_t cnt, ...);
void *korb_embed_attr_descs(CTX *c, uint32_t cnt, ...);
uint32_t *korb_embed_binding_scope(CTX *c, uint32_t L, ...);
void *korb_embed_pat(CTX *c, uint32_t kind, int32_t bind_off, struct Node *value_node, struct Node *outw, int32_t dc_off,
                     uint32_t n, uint32_t npost, uint32_t line, uint32_t ecnt, ...);
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
void korb_load_time_specialize(CTX *c, NODE *ast, uint32_t repo_from, const char *file);

extern size_t node_cnt;

/* Frame-push headroom: covers in-frame expression staging without a per-node
 * check (v2_design §3.5).  Defined here (not only korb_runtime.h) so the
 * inlined call fast path below sees it in every TU, including the SDs. */
#ifndef KORB_FRAME_SLACK
#define KORB_FRAME_SLACK 1024
#endif

/* Per-frame header cells reserved BELOW the locals base (design A bottom header):
 *   base[-1] = self     (staged receiver, read directly — no copy to a top cell)
 *   base[-2] = EP       (open closure env / block PREV link)
 *   base[-3] = link     (where it was called from — see "Frame link" below)
 *   base[-4] = identity (what frame this is: method / block / file / C method)
 * @children call nodes reserve KORB_FRAME_HDR cells via the dispatcher; internal
 * dispatch (korb_send_impl/korb_call_impl) reserves them by shifting the frame.
 * Nothing is reserved ABOVE the locals: the frame is exactly its locals. */
#define KORB_FRAME_HDR 3
_Static_assert(KORB_FRAME_HDR == 3, "the @framehdr staging fills base[-2], base[-3] and base[-4] by name");

/* Offset (from a frame's locals base) of the IDENTITY cell.  The caller stores
 * the callee's method entry here and it STAYS: identity next to the link is what
 * lets the walk step base → base with no per-kind arithmetic, and what lets a C
 * function ask which method it is running in.  Every writer tags the low 3 bits
 * with a KORB_FID_* kind, so the GC root scan reads the cell as an immediate.
 * It sits BELOW the EP so that the EP — which the closure chain reaches on an
 * arbitrary outer frame at run time — keeps a fixed offset. */
#define KORB_ID_OFF (-4)
static inline korb_value korb_id_get(const korb_value *const base) { return base[KORB_ID_OFF]; }
static inline void  korb_id_set(korb_value *const base, const korb_value v) { base[KORB_ID_OFF] = v; }

/* The EP: the frame's open closure env, or a block frame's PREV link. */
#define KORB_EP_OFF (-2)
static inline korb_value korb_ep_get(const korb_value *const base) { return base[KORB_EP_OFF]; }
static inline void  korb_ep_set(korb_value *const base, const korb_value v) { base[KORB_EP_OFF] = v; }

/* Per-frame cell at base[-3]: the frame LINK (see "Frame link" below).
 *
 * base[-3] is the link ONLY where a call site or a re-staging dispatch reserved
 * it.  Everywhere else it holds whatever the caller had there, which is why the
 * walk validates every hop (tag, slot-stack range, and an identity it
 * recognizes) instead of trusting the cell. */
#define KORB_FLINK_OFF (-3)

/* ---------------------------------------------------------------------------
 * Frame link (backtrace): the caller chain, recorded by no one.
 *
 * A live frame's position is (its method, the line it is currently at).  The
 * method is already in the frame — the tagged marker cell at its top, written
 * for `super` / `__method__`.  The missing halves are the line and a way to get
 * from one frame to the frame below it.
 *
 * Both come from the cell the call site ALREADY stores to: the callee's
 * base[-3], zeroed before the arguments are evaluated (a live GC-scanned cell
 * must not hold stale bits).  That cell sits exactly at the caller's cursor, so
 * the CALLER'S BASE is a parse-time constant `chain + locals_cnt` cells below
 * it — the staging depth of the call site plus its frame size, both of which
 * the parser knows (the frame size only at scope end, so pop_frame folds it in;
 * see parse.c kp_flink).  Storing (line, that distance) instead of 0 is the
 * same store, and it REPLACES the line the call site used to materialize: the
 * cold tails read the line back out of the frame instead of keeping it live
 * across the dispatch.
 *
 * Aiming the distance at the BASE (not at a marker inside the frame) is what
 * makes the walk uniform: from a base, the identity is base[-4] and the next
 * link is base[-3], whatever kind of frame it is and however many locals it has.
 *
 * Layout.  Bit 0 is always 1, so the GC root scan reads the cell as an
 * immediate; bits 1..2 pick the form, and every pointer form is one OR:
 *   bit 1 == 0    DIRECT   dist = bits 2..13 (+ bits 31.. for a run-time one),
 *                          line = bits 14..30, SIGNED (eval(str, file, -100)).
 *                          A baked one stays under 2^31, which is what lets it
 *                          be stored as an immediate.
 *   (v & 7) == 3  FORWARD  a link cell elsewhere: read the link there instead
 *                          (a dispatch that rebuilt the window left it behind).
 *   (v & 7) == 7  CFRAME   a C method's argument window, used as its base: the
 *                          identity is base[-4] like any frame, and its own
 *                          position is the line in base[-3].
 * (Bit 2 belongs to `dist`, so only bit 1 may separate the forms; the pointer
 * forms are 8-aligned addresses, where all three low bits are free.)
 * ------------------------------------------------------------------------- */
#define KORB_FLINK_DIST_LO_BITS 12u
#define KORB_FLINK_DIST_MAX     (1u << KORB_FLINK_DIST_LO_BITS)
#define KORB_FLINK_LINE_BITS    17u
#define KORB_FLINK_LINE_MIN     (-(1 << (KORB_FLINK_LINE_BITS - 1)))
#define KORB_FLINK_LINE_MAX     ((1 << (KORB_FLINK_LINE_BITS - 1)) - 1)
/* The line is SIGNED: `eval(str, file, -100)` is legal Ruby and its frames
 * report negative positions, so the field round-trips them. */
static inline uint32_t korb_flink_line(const korb_value f) {
    const uint32_t v = (uint32_t)((f >> 14) & ((1u << KORB_FLINK_LINE_BITS) - 1u));
    /* shift in unsigned (a set sign bit would overflow int32), extend as signed */
    return (uint32_t)((int32_t)(v << (32 - KORB_FLINK_LINE_BITS)) >> (32 - KORB_FLINK_LINE_BITS));
}
static inline uint32_t korb_flink_dist(const korb_value f) {
    return (uint32_t)((f >> 2) & (KORB_FLINK_DIST_MAX - 1u)) |
           (uint32_t)(((f >> 31) & 0xFFFFu) << KORB_FLINK_DIST_LO_BITS);
}
static inline uint32_t korb_flink_line_bits(uint32_t line) {
    const int32_t l = (int32_t)line;
    const int32_t c = l < KORB_FLINK_LINE_MIN ? KORB_FLINK_LINE_MIN
                    : l > KORB_FLINK_LINE_MAX ? KORB_FLINK_LINE_MAX : l;
    return (uint32_t)c & ((1u << KORB_FLINK_LINE_BITS) - 1u);
}
/* Parse-time link: fits a signed 32-bit immediate, so the call site stores it
 * without materializing it in a register.  A staging depth past the field (a
 * call buried in a huge literal) drops the link rather than aiming it wrong. */
static inline uint32_t korb_flink_make(uint32_t line, uint32_t dist) {
    if (dist >= KORB_FLINK_DIST_MAX) dist = 0;
    return (korb_flink_line_bits(line) << 14) | (dist << 2) | 1u;
}
/* The packed line field of an existing link.  A line that came out of a link
 * is already clamped, so carrying it in this form (rather than as a signed
 * line) keeps re-clamping off the paths that rebuild a link. */
static inline uint32_t korb_flink_line_bits_of(const korb_value f) {
    return (uint32_t)((f >> 14) & ((1u << KORB_FLINK_LINE_BITS) - 1u));
}
/* Run-time DIRECT link: the distance is a pointer difference, so it gets the
 * wide field too (and no longer fits an immediate — which is fine, nothing
 * stores this one from a call site). */
static inline korb_value korb_flink_make_wide_bits(uint32_t line_bits, uint64_t dist) {
    if (dist >= (1ull << 28)) dist = 0;
    return ((korb_value)(dist >> KORB_FLINK_DIST_LO_BITS) << 31) |
           ((korb_value)line_bits << 14) |
           ((korb_value)(dist & (KORB_FLINK_DIST_MAX - 1u)) << 2) | 1u;
}
static inline korb_value korb_flink_make_wide(uint32_t line, uint64_t dist) {
    return korb_flink_make_wide_bits(korb_flink_line_bits(line), dist);
}

/* The two pointer forms.  Both are one OR on a frame base (8-aligned, so the
 * tag bits are free) — and the CFRAME one is ORed once per block-taking C call
 * (kept in c->cfunc_link), so linking a frame C placed costs a load and a store.
 * FORWARD is the exception: it names a link CELL, not a base. */
static inline korb_value korb_flink_fwd(const korb_value *const cell) { return (korb_value)(uintptr_t)cell | 3u; }
static inline korb_value korb_flink_cframe(const korb_value *const base) { return (korb_value)(uintptr_t)base | 7u; }
static inline const korb_value *korb_flink_ptr(const korb_value f) { return (const korb_value *)(uintptr_t)(f & ~(korb_value)7u); }

/* True when a baked link carries a distance, which a call site only does when it
 * also reserved the callee's header cells (@framehdr).  A site that stages its
 * args flush against its own cursor (node_shl) bakes distance 0 — the cells below
 * ITS window are the caller's live slots, not a header to write into.  Only the
 * low field is tested: a baked link never uses the wide one. */
#define KORB_FLINK_FRAMED(f) (((f) & ((korb_value)(KORB_FLINK_DIST_MAX - 1u) << 2)) != 0)

/* Park a parse-baked link for a dispatch that builds the callee window itself
 * (a splat call reserves no header cells to store it in; a yield hands one to
 * korb_block_yield).  `slots` is the node's own cursor, which is what the baked
 * distance is measured from. */
static inline const korb_value *korb_flink_base(const korb_value flink, const korb_value *const slots)
{
    const uint32_t d = korb_flink_dist(flink);
    return d ? slots - d : NULL;
}
static inline void korb_flink_stage(CTX *c, const korb_value flink, const korb_value *const slots)
{
    c->carry_base = korb_flink_base(flink, slots);
    c->carry_line_bits = korb_flink_line_bits_of(flink);
}
/* Same, from a frame base known only at run time (a `yield` inside a block: the
 * block frame's base is where its EP cell is). */
static inline void korb_flink_stage_at(CTX *c, const korb_value *const base, const uint32_t line)
{
    c->carry_base = base;
    c->carry_line_bits = korb_flink_line_bits(line);
}

/* The frame base a node addresses through its baked dc_off (which points at the
 * identity cell, base[-4]), or NULL when the node has none. */
#define KORB_FBASE_AT(slots, dc_off) ((dc_off) != INT32_MIN ? &(slots)[(dc_off) - KORB_ID_OFF] : NULL)

/* Frame identity kinds, in the low 3 bits of base[-4] (every pointer they carry
 * is 8-aligned).  A method frame's identity is its method entry — the cell the
 * caller already fills to hand the callee over. */
#define KORB_FID_METHOD 1u   /* korb_method * | 1 */
#define KORB_FID_BLOCK  3u   /* node_entry NODE * | 3 */
/* node_const dc_off of the `$x ||=` / `$x &&=` read: it does not warn when $x is unset */
#define KORB_GVAR_QUIET INT32_MAX
#define KORB_FID_MAIN   5u   /* a file's top-level frame: (file sym << 32) | (locals << 4) | (required << 3) | tag */
#define KORB_FID_CFUNC  7u   /* a C method running a block: korb_method * | 7.  Its "frame" is the argument
                              * window, which has no Ruby locals — so it has no EP. */

/* A file frame's identity: the file, plus whether it was require'd (CRuby labels
 * that frame '<top (required)>'). */
/* The prelude stands in for CRuby's core methods written in Ruby, which live in
 * files named "<internal:...>" — specs test for that prefix. */
#define KORB_PRELUDE_FILE "<internal:prelude>"

#define KORB_FID_MAIN_REQUIRED     8u
/* An eval frame: the symbol is the LABEL it borrows from the scope the string was
 * compiled into (CRuby names the eval iseq after it), not a file. */
#define KORB_FID_MAIN_LABEL       16u
static inline korb_value korb_fid_main(uint32_t fsym, bool required) {
    return ((korb_value)fsym << 32) | (required ? (korb_value)KORB_FID_MAIN_REQUIRED : 0) | KORB_FID_MAIN;
}

/* A C method's argument window IS the frame CRuby shows for it ("Kernel#puts"),
 * and it is what a Ruby method called back into from C has to link to — without
 * it the chain ends at that Ruby method.  So a C dispatch through a framed window
 * names itself here for as long as it runs: a load, three stores and a restore
 * per C call, which is what a backtrace that does not stop at `puts` costs.
 * `base` is the argument window = the callee frame's locals base. */
static inline korb_value korb_cframe_enter(CTX *c, korb_value *const base, const struct korb_method *const m) {
    const korb_value saved = c->cfunc_link;
    korb_id_set(base, (korb_value)((uintptr_t)m | KORB_FID_CFUNC));
    c->cfunc_link = korb_flink_cframe(base);
    return saved;
}
/* The identity is NOT cleared: the cell is the call site's header, and every
 * frame built on it writes its own identity first (the @framehdr dispatcher
 * zeroes it before the args run; korb_invoke_* overwrite it).  It is tagged, so
 * the GC root scan skips it either way.  (Parking `saved` in the frame's unused
 * EP cell instead of a C local was tried and measured WORSE: methodchain
 * +4.01% vs +3.18%.) */
static inline void korb_cframe_leave(CTX *c, const korb_value saved) {
    c->cfunc_link = saved;
}

/* Frame locals by NAME (debugger).  `base` is any live frame's locals base — the
 * backtrace link chain yields one for every frame.  The names come from the
 * frame's ISEQ (struct korb_locals_info), so nothing is baked at the reading
 * site.  A block frame's enclosing scopes are searched too, through the EP
 * chain.  false = the name is not in scope there. */
bool korb_frame_local_get(CTX *c, const korb_value *base, uint32_t name_sym, korb_value *out);
bool korb_frame_local_set(CTX *c, korb_value *base, uint32_t name_sym, korb_value v);
uint32_t korb_frame_local_names(CTX *c, const korb_value *base, uint32_t *out, uint32_t max);

/* Cold helpers used by the inlined simple-call fast path below; defined in
 * korb_runtime.c (the SD / all.so reaches them as exported symbols, only on
 * the rare open-env-close / exception-backtrace paths). */
RESULT korb_close_ret(CTX *c, korb_value *scratch, korb_value *frame_base, RESULT r);
RESULT korb_invoke_ret_cold(CTX *c, korb_value *base, uint32_t locals_cnt, RESULT r, uint32_t line, uint32_t mid);
/* node_simple_entry の冷たい尾: RETURN の消費と脱出フレームのクローズ (callee 側の
 * 事情だけ。line / mid は要らない)。 */
RESULT korb_entry_ret_cold(CTX *c, korb_value *base, uint32_t locals_cnt, RESULT r);
/* 呼び出し側に残る冷たい尾: RAISE のバックトレースだけ。base も locals_cnt も
 * 要らないので、dispatch をまたいで生存させる値が無くなる。 */
RESULT korb_call_ret_cold(CTX *c, RESULT r, korb_value flink, uint32_t mid, korb_value fid);
RESULT korb_call_ret_cold_s(CTX *c, RESULT r, const korb_value *slots, uint32_t argc, uint32_t mid);   /* sdi28 */
RESULT korb_entry_exit(CTX *c, korb_value *base, uint32_t locals_cnt, RESULT r);   /* sdi28: node_simple_entry's slow exit */
RESULT korb_entry_overflow(CTX *c, korb_value *slots);   /* sdi28: node_simple_entry's stack overflow */
RESULT korb_send_site(CTX *c, korb_value *slots, uint32_t mid, int32_t self_off, uint32_t argv_cnt, struct korb_inlcache *ic,
                      struct korb_inlcache *site_ic);   /* sdi28: a whole node_send, out of line (the SDI sends' fallback) */
/* main.c: a node synthesized at runtime (attr_reader entry) — bind to a baked SD / instantiate like a parsed body. */
void   korb_synth_node_ready(NODE *n);
void   korb_optent_put(struct korb_vm *vm, const struct Node *body, struct Node **es);   /* parse.c: the per-argc entries it built */
void   korb_bt_unwind(CTX *c, KorbException *e, uint32_t line, uint32_t mid, const struct korb_method *m);
void   korb_bt_note_ptr(struct korb_vm *vm, const void *p);   /* a frame-top marker target (cold: definition time) */
const char *korb_bt_cur_file(const struct korb_vm *vm);
void   korb_dispatchers_swapped(struct korb_vm *vm);   /* code-store swap → refill fat inline caches */

/* Streamlined invocation of a "simple" ISEQ method (only required positional
 * params; the caller guarantees m->is_simple).  always_inline so it folds into
 * the dispatch site — both korb_call_cached / korb_send_cached (korb_runtime.c)
 * and node_call's specialized dispatcher (code_store SD) inline it, removing a
 * cross-module call layer on the hot recursive / method-call path. */
/* -DKORB_FRAME_AUDIT: a METHOD frame's EP cell must be 0 on entry — an open env
 * only appears once korb_make_proc runs in it, and a block's PREV link is odd.
 * Anything else means the dispatch reused live caller slots as the header. */
#ifdef KORB_FRAME_AUDIT
void korb_frame_audit_ep(const korb_value *base, uint32_t mid, const char *where);
#define KORB_AUDIT_EP(base, mid) \
    do { if (korb_ep_get(base) != 0) korb_frame_audit_ep((base), (mid), __func__); } while (0)
#else
#define KORB_AUDIT_EP(base, mid) ((void)0)
#endif

/* True if frame `base` owns an OPEN env in its EP cell (a clean even KorbEnv
 * whose loc is this frame) → its return must close it.  `closed` matters: a
 * later frame can land on the same slots address while holding an env that was
 * closed at ITS defining frame's return (a define_method body's EP is the proc's
 * env, whose loc is that long-gone frame).  Re-closing copies the new frame's
 * locals over the captured ones — the values silently change under the closure. */
/* The `$~` cell (builtins/regexp.c).  Ruby scopes the backref to the METHOD
 * frame: an ISEQ invoke parks the caller's and starts at nil. */
korb_value *korb_re_tilde_cell(CTX *c);
void korb_bref_grow(CTX *c);   /* korb_runtime.c: out of line, every method entry with a backref scope inlines the push (sdi29) */
static inline void korb_bref_push(CTX *c, korb_value v, const korb_value *const base) {
    if (UNLIKELY(c->bref_n == c->bref_cap)) korb_bref_grow(c);
    c->bref_base[c->bref_n] = base;
    c->bref_saved[c->bref_n++] = v;
}
const korb_value *korb_block_home_base(const struct korb_vm *vm, const korb_value *base);   /* korb_runtime.c */
void korb_dregx_src_enc(struct korb_vm *vm, korb_value src);   /* korb_runtime.c */
static inline korb_value korb_bref_pop(CTX *c) {
    return LIKELY(c->bref_n != 0) ? c->bref_saved[--c->bref_n] : KORB_NIL;
}

static inline bool korb_frame_escaped(const korb_value *base) {
    const korb_value pv = korb_ep_get(base);
    return pv != 0 && (pv & 7u) == 0 && VAL2ENV(pv)->loc == base && !VAL2ENV(pv)->closed;
}

static inline __attribute__((always_inline, no_stack_protector)) RESULT
korb_invoke_simple(CTX *c, korb_value *slots, struct korb_method *m, uint32_t argc,
                   uint32_t line, uint32_t mid, korb_value self, korb_value def_class)
{
    if (UNLIKELY(argc != (uint32_t)m->params_cnt))
        return korb_raise_arity(c, slots, m, mid, line,
                                "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
    korb_value *const base = slots - argc;
    if (argc != 0) korb_kwargs_unmark_pos(c, slots, &base[argc - 1]);   /* fixed positional: the call-site kwargs mark stops here */
    KORB_AUDIT_EP(base, mid);
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    korb_value *const resv = base + locals_cnt + KORB_FRAME_SLACK;
    if (UNLIKELY(resv > c->slots_limit || &cstack_probe < c->cstack_limit))
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    if (UNLIKELY(resv > c->slots_high_water)) c->slots_high_water = resv;
    /* zero the genuine locals (body locals + synth temps) that the body may
     * read/GC-scan.  Inline the small counts (the vast majority of methods) to
     * skip the __memset PLT call.  The EP needs no store: whoever reserved the
     * header cells zeroed base[-2] before the arguments were evaluated. */
    if (locals_cnt > argc) {
        const uint32_t nz = locals_cnt - argc;
        korb_value *const z = base + argc;
        switch (nz) {
            case 4: z[3] = 0;   /* fallthrough */
            case 3: z[2] = 0;   /* fallthrough */
            case 2: z[1] = 0;   /* fallthrough */
            case 1: z[0] = 0; break;
            default: memset(z, 0, nz * sizeof(korb_value));
        }
    }
    korb_id_set(base, (korb_value)((uintptr_t)m | KORB_FID_METHOD));   /* identity at base[-4]; super/__method__/backtrace source */
    /* self is already at base[-1] (the caller's staged receiver) — not copied to a
     * top cell.  Every korb_invoke_simple caller stages self there (bottom header). */
    (void)def_class; (void)self;
    NODE *const body = m->body;
    const bool bref_scope = UNLIKELY(m->uses_backref != 0);   /* $~ is per method frame */
    if (bref_scope) { korb_value *const bcell = korb_re_tilde_cell(c); korb_bref_push(c, *bcell, base); *bcell = KORB_NIL; }
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (bref_scope) *korb_re_tilde_cell(c) = korb_bref_pop(c);   /* re-read: the table may have grown */
    if (r.state == KORB_RETURN) {
        /* Consume only a return targeted at this method (NULL = nearest-method,
         * the common case) — a block's `return` aimed at an outer method passes
         * through unchanged. */
        if (c->return_target == NULL || c->return_target == base) {
            r.state = KORB_NORMAL;
            c->return_target = NULL;
        }
    }
    else if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value))   /* non-exception RAISE payload (thread kill / throw): no backtrace */
        korb_bt_unwind(c, VAL2EXC(r.value), line, mid, m);
    if (UNLIKELY(korb_frame_escaped(base))) r = korb_close_ret(c, base + locals_cnt, base, r);
    return r;
}


/* EXPERIMENT acct48 (korb_runtime.c): the binding stub an instance calls, and the undo when its cache is refilled. */
RESULT korb_entry_call(CTX *c, NODE *body, korb_value *slots, const struct korb_inlcache *ic);
void korb_dc_refill(struct korb_vm *vm, struct korb_inlcache *ic, const struct korb_method *m);   /* before a bound site's cache is refilled */
node_dispatcher_func_t korb_hot_tramp(void);   /* main.c: the weave trampoline a body may still be on */

/* Cache-driven simple invoke.  The frame setup now lives in the callee's
 * node_simple_entry SD (where params_cnt / locals_cnt are constants), so all
 * that remains here is: write the callee's identity into base[-4] — where it
 * stays for the life of the frame — and dispatch.  Nothing but `line` / `mid`
 * — themselves baked immediates — stays live across the call. */
static inline __attribute__((always_inline, no_stack_protector)) RESULT
korb_invoke_entry_ic(CTX *c, korb_value *slots, const struct korb_inlcache *ic, uint32_t argc,
                     uint32_t mid)
{
    korb_id_set(slots - argc, (korb_value)((uintptr_t)ic->m | KORB_FID_METHOD));   /* the callee frame's identity */
    /* the body's dispatcher now, not the one cached at fill: a body still on the weave trampoline when the
     * site filled stays there otherwise (each call went through korb_first_dispatch's forward, 16% of o.m0) */
#if defined(ASTRO_SD_PATCH)
    /* EXPERIMENT acct48: in a woven instance the call goes to a host stub that binds this very call to the body's
     * dispatcher (a direct rel32, as a PLT entry would), so later calls are one predicted direct call, not an
     * indirect one through two loads. */
    const RESULT r = korb_entry_call(c, ic->body, slots, ic);
#else
    const RESULT r = (*ic->body->head.dispatcher)(c, ic->body, slots);
#endif
    if (LIKELY(r.state == KORB_NORMAL)) return r;
    /* The site's line rides the frame link it stored in base[-3]; reading it
     * back here keeps it out of the hot path entirely. */
    return korb_call_ret_cold_s(c, r, slots, argc, mid);   /* sdi28: the argument loads in the shared tail */
}

/* Mirror for the top-level call cache. */
static inline __attribute__((always_inline, no_stack_protector)) RESULT
korb_invoke_entry_cc(CTX *c, korb_value *slots, const struct korb_callcache *cc, uint32_t argc,
                     uint32_t mid)
{
    korb_id_set(slots - argc, (korb_value)((uintptr_t)cc->m | KORB_FID_METHOD));
    const RESULT r = (*cc->body->head.dispatcher)(c, cc->body, slots);   /* the body's current one (see korb_invoke_entry_ic) */
    if (LIKELY(r.state == KORB_NORMAL)) return r;
    return korb_call_ret_cold_s(c, r, slots, argc, mid);   /* sdi28: the argument loads in the shared tail */
}

/* ---- EXPERIMENT KORB_TYHINT: per-site operand kinds, observed in the interpreter, baked into the SD ---- */
typedef uint32_t korb_tyhint_t;
enum korb_ty_kind { KORB_TY_NONE = 0, KORB_TY_FIX = 1, KORB_TY_FLO, KORB_TY_IMM, KORB_TY_STR, KORB_TY_ARY, KORB_TY_HASH, KORB_TY_OBJ, KORB_TY_OTHER };
#define KORB_TY2(a, b) (((a) << 4) | (b))
#define KORB_TY_IMM_P(v) (!AROH_IS_GC_OBJECT(v) && !FIXNUM_P(v) && !FLONUM_P(v))
static inline unsigned
korb_ty_kind(const korb_value v)
{
    if (FIXNUM_P(v)) return KORB_TY_FIX;
    if (FLONUM_P(v)) return KORB_TY_FLO;
    if (!AROH_IS_GC_OBJECT(v)) return KORB_TY_IMM;
    const uint16_t f = ((const AroObjectHeader *)(uintptr_t)v)->flags & (KORB_OBJ_TYPE_MASK | KORB_FL_HAS_KLASS);
    if (f == KORB_OBJ_STRING) return KORB_TY_STR;
    if (f == KORB_OBJ_ARRAY) return KORB_TY_ARY;
    if (f == KORB_OBJ_HASH) return KORB_TY_HASH;
    if (f == KORB_OBJ_OBJECT) return KORB_TY_OBJ;
    return KORB_TY_OTHER;
}
/* the hint an observed site bakes: KORB_TY2(lhs kind, rhs kind), each 0 unless exactly one kind was seen; 0 = the generic body */
static inline korb_tyhint_t
korb_tyhint_of(const struct Node *n)
{
    if (!n->head.flags.ty_observed) return 0;
    const uint32_t l = n->head.flags.ty_l, r = n->head.flags.ty_r;
    if (__builtin_popcount(l) != 1) return 0;   /* one lhs kind; the rhs kind only when it is one too (else 0) */
    return KORB_TY2(__builtin_ctz(l) + 1, __builtin_popcount(r) == 1 ? __builtin_ctz(r) + 1 : 0);
}
void korb_ty_observe(CTX *c, struct Node *n, unsigned lk, unsigned rk);   /* node.c */
#ifdef ASTRO_SD_POOL
#define KORB_TY_OBSERVE(c, n, l, r) ((void)0)
#else
#define KORB_TY_OBSERVE(c, n, l, r) do { if (UNLIKELY((c)->vm->ty_observe)) korb_ty_observe((c), (n), korb_ty_kind(l), korb_ty_kind(r)); } while (0)
#endif
RESULT korb_eq_generic(CTX *c, korb_value *slots, korb_value l, korb_value rhs, uint64_t flink);   /* node.def, binary only */
RESULT korb_neq_generic(CTX *c, korb_value *slots, korb_value l, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi23) */
RESULT korb_aset_generic(CTX *c, korb_value *slots, uint64_t flink, korb_ref recv, korb_ref idx, korb_value val, struct korb_inlcache *ic);   /* sdi27 */
RESULT korb_caseeq_generic(CTX *c, korb_value *slots, korb_ref wv, korb_value subj, struct korb_inlcache *ic, korb_value flink);   /* sdi27 */
RESULT korb_plus_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_minus_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_mul_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_lt_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_gt_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_le_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_ge_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, uint64_t flink);   /* node.def, binary only (sdi26) */
RESULT korb_aref_generic(CTX *c, korb_value *slots, korb_value r, korb_value idx, uint64_t flink, struct korb_inlcache *ic);
RESULT korb_shl_generic(CTX *c, korb_value *slots, korb_ref lhs, korb_value rhs, struct korb_inlcache *ic, uint64_t flink);   /* sdi29 */
korb_value korb_str_frozen_fill(CTX *c, korb_value *slots, const char *bytes, uint32_t len, struct korb_fscache *fc);   /* sdi29 */
RESULT korb_not_generic(CTX *c, korb_value *slots, korb_value v);   /* sdi29 */

/* Builtin receiver → class object (2 loads); mirrors korb_runtime.c's korb_builtin_class_obj. */
static inline korb_value
korb_bi_class(const struct korb_vm *vm, enum korb_class e)
{
    const uint32_t idx = vm->class_obj_idx[e];
    return (idx == UINT32_MAX) ? KORB_NIL : vm->const_vals[idx];
}

/* EXPERIMENT acct46 (from korb_runtime.c): the builtin class a C-method cache is keyed on for this receiver, or nil
 * (a plain / singleton-classed object).  Immediates after the heap objects a C-method send mostly sees. */
static inline __attribute__((always_inline)) korb_value
korb_send_builtin_klass(struct korb_vm *vm, const korb_value recv)
{
    if (UNLIKELY(!AROH_IS_GC_OBJECT(recv))) {
        if (FIXNUM_P(recv)) return korb_bi_class(vm, KORB_C_INTEGER);
        if (FLONUM_P(recv)) return korb_bi_class(vm, KORB_C_FLOAT);
        if (SYMBOL_P(recv)) return korb_bi_class(vm, KORB_C_SYMBOL);
        if (recv == KORB_NIL) return korb_bi_class(vm, KORB_C_NIL);
        if (recv == KORB_TRUE) return korb_bi_class(vm, KORB_C_TRUE);
        if (recv == KORB_FALSE) return korb_bi_class(vm, KORB_C_FALSE);
        return KORB_NIL;
    }
    if (((const AroObjectHeader *)(uintptr_t)recv)->flags & KORB_FL_HAS_KLASS) return KORB_NIL;
    switch (KORB_OBJ_TYPE(recv)) {
      case KORB_OBJ_ARRAY:     return korb_bi_class(vm, KORB_C_ARRAY);
      case KORB_OBJ_STRING:    return korb_bi_class(vm, KORB_C_STRING);
      case KORB_OBJ_HASH:      return korb_bi_class(vm, KORB_C_HASH);
      case KORB_OBJ_PROC:      return korb_bi_class(vm, KORB_C_PROC);
      case KORB_OBJ_RANGE:     return korb_bi_class(vm, KORB_C_RANGE);
      case KORB_OBJ_REGEXP:    return korb_bi_class(vm, KORB_C_REGEXP);
      case KORB_OBJ_MATCHDATA: return korb_bi_class(vm, KORB_C_MATCHDATA);
      default:                 return KORB_NIL;
    }
}


/* Fill an inline cache.  The `simple` fast path is installed only when the
 * callee has a node_simple_entry AND this site's argc matches its arity — the
 * ic is per site, so argc is fixed for it.  That moves the arity check off the
 * hot path entirely: a mismatched site simply never caches, and falls through
 * to the general path which raises ArgumentError. */
/* EXPERIMENT acct46: the tag code of a builtin class, for the SD's receiver test (0 = not a builtin class) */
#define KORB_TAG_IMM 0xF0u
static inline uint8_t
korb_bi_tag_code_slow(const struct korb_vm *vm, korb_value klass)   /* korb_sdi_init's, once per builtin class */
{
    static const struct { enum korb_class c; unsigned t; } tab[] = {
        { KORB_C_STRING, KORB_OBJ_STRING }, { KORB_C_ARRAY, KORB_OBJ_ARRAY }, { KORB_C_HASH, KORB_OBJ_HASH }, { KORB_C_PROC, KORB_OBJ_PROC },
        { KORB_C_RANGE, KORB_OBJ_RANGE }, { KORB_C_REGEXP, KORB_OBJ_REGEXP }, { KORB_C_MATCHDATA, KORB_OBJ_MATCHDATA } };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) if (korb_bi_class(vm, tab[i].c) == klass) return (uint8_t)(tab[i].t + 1);
    static const enum korb_class imm[] = { KORB_C_INTEGER, KORB_C_FLOAT, KORB_C_SYMBOL, KORB_C_NIL, KORB_C_TRUE, KORB_C_FALSE };
    for (unsigned i = 0; i < 6; i++) if (korb_bi_class(vm, imm[i]) == klass) return (uint8_t)(KORB_TAG_IMM + 1 + i);
    return 0;
}
/* the class's own copy: a cache fill reads one byte, not 13 class compares (railsbench refills ~190 caches a request) */
static inline uint8_t
korb_bi_tag_code(const struct korb_vm *vm, korb_value klass)
{
    (void)vm;
    return KORB_CLASS_P(klass) ? VAL2CLASS(klass)->bi_tag : 0;
}
static inline bool
korb_recv_tag_is(korb_value recv, uint8_t code)
{
    if (LIKELY(AROH_IS_GC_OBJECT(recv))) {
        const uint16_t f = ((const AroObjectHeader *)(uintptr_t)recv)->flags;
        return !(f & KORB_FL_HAS_KLASS) && (unsigned)(f & KORB_OBJ_TYPE_MASK) + 1 == code;
    }
    if (code < KORB_TAG_IMM) return false;
    if (FIXNUM_P(recv)) return code == KORB_TAG_IMM + 1;
    if (FLONUM_P(recv)) return code == KORB_TAG_IMM + 2;
    if (SYMBOL_P(recv)) return code == KORB_TAG_IMM + 3;
    return (recv == KORB_NIL && code == KORB_TAG_IMM + 4) || (recv == KORB_TRUE && code == KORB_TAG_IMM + 5) || (recv == KORB_FALSE && code == KORB_TAG_IMM + 6);
}
static inline void
korb_ic_fill_raw(const struct korb_vm *vm, struct korb_inlcache *ic, uint64_t serial, korb_value klass, struct korb_method *m,
                 korb_value def_class, uint8_t kind, uint32_t argc)
{
    ic->serial = serial; ic->klass = klass; ic->m = m; ic->def_class = def_class; ic->kind = kind;
    ic->body = NULL; ic->dispatch = NULL;
    ic->cfunc_ok = 0;   /* EXPERIMENT acct46: what the SD may call itself (the tag test / class compare selects it) */
    ic->smid = (kind == KORB_IC_INSTANCE && m != NULL && m->sdi_k) ? (KORB_SDI_MARK | m->sdi_k) : 0;   /* the AOT inline send's one-word hit test (KORB_SDI_HIT) */
    if (m != NULL && m->kind == KORB_METHOD_CFUNC && (m->params_cnt < 0 || (uint32_t)m->params_cnt == argc)) {
        if (kind == KORB_IC_INSTANCE) ic->cfunc_ok = korb_bi_tag_code(vm, klass);
        else if (kind == KORB_IC_SMETHOD && !m->uses_block && m->visibility == 0) ic->cfunc_ok = 0xFF;
    } else if (kind == KORB_IC_SMETHOD && m != NULL && m->kind == KORB_METHOD_ISEQ && m->is_simple && m->visibility == 0) {
        ic->cfunc_ok = 0xFE;   /* a class's simple public method: korb_send_simple_hit (dispatch stays NULL: node_send's
                                * plain-object check must not take a class-method cache for an instance of that class) */
    }
    if (m == NULL || kind != KORB_IC_INSTANCE) return;
    if (m->simple_entry != NULL && argc == (uint32_t)m->params_cnt) {   /* ISEQ simple entry or attr_reader (its ivar node) */
        ic->body = m->simple_entry; ic->dispatch = m->simple_entry->head.dispatcher;
    } else if (m->opt_entries != NULL && argc >= m->req_cnt && argc <= (uint32_t)m->params_cnt && !vm->refinements_active) {
        NODE *const e = m->opt_entries[argc - m->req_cnt];   /* optionals left out: korb_opt_entries_build */
        ic->body = e; ic->dispatch = e->head.dispatcher;
    }
    /* a builtin receiver's Ruby method with an entry (String#html_safe?): korb_send_alt's tag test goes straight to it */
    if (ic->dispatch != NULL && ic->cfunc_ok == 0) ic->cfunc_ok = korb_bi_tag_code(vm, klass);
}

/* The same, for a site's own cache: a shared one (EXPERIMENT acct45) is only ever refreshed by korb_cc_intern. */
static inline void
korb_ic_fill(struct korb_vm *vm, struct korb_inlcache *ic, uint64_t serial, korb_value klass, struct korb_method *m,
             korb_value def_class, uint8_t kind, uint32_t argc)
{
    if (UNLIKELY(ic->shared)) { korb_cc_check_fail(ic, kind); return; }
    /* EXPERIMENT acct48: a bound site (vm->dc_n: any exist) keeps its direct call when the refill lands on the same
     * method (a serial bump: railsbench refills every site about once per 250 requests); another method unbinds it */
    if (UNLIKELY(vm->dc_n != 0) && !ic->poly) korb_dc_refill(vm, ic, m);
    korb_ic_fill_raw(vm, ic, serial, klass, m, def_class, kind, argc);
}

/* Mirror of korb_ic_fill for the top-level call cache. */
static inline void
korb_cc_fill(struct korb_callcache *cc, uint64_t serial, struct korb_method *m, uint32_t argc)
{
    cc->serial = serial; cc->m = m;
    if (m != NULL && m->simple_entry != NULL && argc == (uint32_t)m->params_cnt) {
        cc->body = m->simple_entry; cc->dispatch = m->simple_entry->head.dispatcher;
    } else {
        cc->body = NULL; cc->dispatch = NULL;
    }
}

#endif /* KORUBY_NODE_H */
