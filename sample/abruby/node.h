#ifndef ABRUBY_NODE_H
#define ABRUBY_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);         // alias: structural hash (Horg)
node_hash_t HORG(NODE *n);         // structural-only hash
node_hash_t HOPT(NODE *n);         // structural + profile hash
node_hash_t hash_node_opt(NODE *n); // cached HOPT for NODE* operands
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);
char *SPECIALIZED_SRC(NODE *n);

VALUE abruby_ivar_get(VALUE self, ID name);
void abruby_ivar_set(CTX *c, VALUE self, ID name, VALUE val);
struct abruby_method *abruby_find_method(const struct abruby_class *klass, ID name);

VALUE abruby_wrap_class(struct abruby_class *klass);
void abruby_class_set_const(struct abruby_class *klass, ID name, VALUE val);
struct abruby_class *abruby_unwrap_class(VALUE obj);
VALUE abruby_new_object(CTX *c, struct abruby_class *klass);

// String helpers
VALUE abruby_str_new(CTX *c, VALUE rb_str);
VALUE abruby_str_new_cstr(CTX *c, const char *s);
VALUE abruby_str_rstr(VALUE ab_str);

// Array/Hash helpers
VALUE abruby_ary_new(CTX *c, VALUE rb_ary);
VALUE abruby_hash_new_wrap(CTX *c, VALUE rb_hash);
void abruby_node_mark(void *ptr);

void code_repo_add(const char *name, NODE *body, bool force_add);
VALUE abruby_wrap_node(NODE *n);

// exception support
VALUE abruby_exception_new(CTX *c, const struct abruby_frame *frame, VALUE message);

// Cold helpers moved out of node.def / node_eval.c so that each SD_*.o
// doesn't carry its own copy.  Defined once in node_helper.c (→ abruby.so);
// referenced as externs from node_eval.c, SD_*.o, PGSD_*.o.

// PUSH_FRAME / POP_FRAME: pair of block-scoped macros that push a synthetic
// frame onto c->current_frame for the duration of a { ... } region, then
// restore.  Used by cold helpers that need to raise abruby exceptions at a
// specific caller_node line (the exception's backtrace walks current_frame).
#define PUSH_FRAME(node) do { \
    struct abruby_frame _push_frame; \
    _push_frame.prev = c->current_frame; \
    _push_frame.caller_node = (node); \
    _push_frame.block = NULL; \
    _push_frame.self = c->current_frame->self; \
    _push_frame.fp = c->current_frame->fp; \
    _push_frame.entry = c->current_frame->entry; \
    c->current_frame = &_push_frame

#define POP_FRAME() \
    c->current_frame = _push_frame.prev; \
    } while (0)

RESULT node_fixnum_plus_overflow(CTX *c, VALUE lv, VALUE rv);
RESULT node_fixnum_minus_overflow(CTX *c, VALUE lv, VALUE rv);
RESULT node_fixnum_mul_overflow(CTX *c, long a, long b);
RESULT raise_argc_error(CTX *c, NODE *call_site, unsigned int given,
                        unsigned int required, unsigned int max_params,
                        bool has_rest);
struct ivar_cache;
VALUE node_ivar_get_slow(VALUE self, ID name, struct ivar_cache *ic);
void  node_ivar_set_slow(CTX * restrict c, VALUE self, ID name, VALUE v,
                         struct ivar_cache *ic);
// Dispatcher retuning on observed runtime type.  Called from arithmetic /
// comparison slow paths when the operand types move to a different kind
// (e.g. fixnum_plus → integer_plus after a Bignum operand is seen).  The
// guard inside early-returns for already-specialized (SD_*) nodes.
struct NodeKind;  // opaque here; full def comes via node_head.h below.
void  swap_dispatcher(NODE *n, const struct NodeKind *target_kind);
// From abruby.c — prototype needed by node_helper.c's node_ivar_set_slow.
void  abruby_object_grow_ivars(struct abruby_object *obj, unsigned int new_cnt);

// Invoke the block attached to the current frame from a cfunc body.
// Returns the block's result.  Propagates RAISE / RETURN / BREAK upward
// (the cfunc should return these unchanged so the surrounding
// dispatch_method_frame_with_block demotes them); NEXT is caught here and
// demoted to NORMAL with the next value.  If no block is attached, returns
// a RESULT_RAISE with a LocalJumpError-style abruby exception.
RESULT abruby_yield(CTX *c, unsigned int argc, VALUE *argv);

// Disable parent/jit_status/dispatch_cnt fields for release builds.
// Under ABRUBY_DEBUG we re-enable parent so failure diagnostics can walk
// up to the containing root AST (see EVAL's NULL-dispatcher check below).
#if ABRUBY_DEBUG
#define ASTRO_NODEHEAD_PARENT
#endif
// #define ASTRO_NODEHEAD_JIT_STATUS
// #define ASTRO_NODEHEAD_DISPATCH_CNT

struct NodeHead {
    // --- cold zone (used only during SPECIALIZE / HASH / GC) ---
    node_hash_t hash_value;   // Horg cache (structural, never changes after parse)
    node_hash_t hash_opt;     // Hopt cache (structural + profile; set at bake time)
    const char *dispatcher_name;
    VALUE rb_wrapper;  // for GC (T_DATA wrapper)

    // --- warm zone ---
    struct NodeFlags {
        bool has_hash_value;  // Horg cached
        bool has_hash_opt;    // Hopt cached / pre-populated from index
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
        bool kind_swapped;    // swap_dispatcher changed kind since alloc
    } flags;
    const struct NodeKind *kind;
    int32_t line;  // source location (for backtrace)

#ifdef ASTRO_NODEHEAD_PARENT
    struct Node *parent;
#endif

    // --- hot zone (accessed on every EVAL, adjacent to union data) ---
    node_dispatcher_func_t dispatcher;
};

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

// NodeKind struct and function pointer typedefs are generated here.
// Node struct definitions follow NodeHead.
#include "node_head.h"

// Code store: declared here so swap_dispatcher can re-query it after kind
// mutation without each SD_*.c having to reach into runtime/ separately.
bool astro_cs_load(NODE *n, const char *file);

#if ABRUBY_DEBUG
// Walk up via head.parent to the nearest @noinline boundary (def / class /
// module / block_literal) — the smallest enclosing compilation entry.  If
// no such ancestor exists, returns the outermost reachable node.  Returns
// n itself on NULL / empty input.
static inline struct Node *
abruby_debug_enclosing_entry(struct Node *n)
{
    if (!n) return NULL;
    struct Node *cur = n;
    while (cur->head.parent) {
        cur = cur->head.parent;
        if (cur->head.flags.no_inline) break;
    }
    return cur;
}

#endif

static inline RESULT
EVAL(CTX *c, NODE *n)
{
#if ABRUBY_DEBUG
    if (n->head.dispatcher == NULL) {
        const char *kind_name = (n->head.kind && n->head.kind->default_dispatcher_name)
            ? n->head.kind->default_dispatcher_name : "<unknown>";
        unsigned long h = n->head.flags.has_hash_value ? n->head.hash_value : HASH(n);
        fprintf(stderr, "\nABRUBY_BUG: NULL dispatcher on node %p\n", (void*)n);
        fprintf(stderr, "  kind=%s line=%d hash=%lx is_spec=%d\n",
                kind_name, n->head.line, h, n->head.flags.is_specialized);
        fprintf(stderr, "  ---- failing node ----\n  ");
        DUMP(stderr, n, true);
        fprintf(stderr, "\n");
        struct Node *root = abruby_debug_enclosing_entry(n);
        if (root && root != n) {
            const char *rname = (root->head.kind && root->head.kind->default_dispatcher_name)
                ? root->head.kind->default_dispatcher_name : "<unknown>";
            fprintf(stderr, "  ---- enclosing entry (%s, line=%d) ----\n  ",
                    rname, root->head.line);
            DUMP(stderr, root, true);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  ----------------------\n");
        fflush(stderr);
        rb_bug("ABRUBY: NULL dispatcher at EVAL (kind=%s, line=%d, hash=%lx)",
               kind_name, n->head.line, h);
    }
#endif
    return (*n->head.dispatcher)(c, n);
}

// Include builtin macros needed by node_eval.c (AB_NUM_WRAP, AB_INT_UNWRAP, etc.)
#include "builtin/builtin.h"

#endif
