#ifndef KORUBY_CONTEXT_H
#define KORUBY_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifndef KORUBY_DEBUG
#define KORUBY_DEBUG 0
#endif

#if KORUBY_DEBUG
#define KORUBY_ASSERT(expr) assert(expr)
#else
#define KORUBY_ASSERT(expr) ((void)0)
#endif

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

/* ----------------------------------------------------------------------
 * VALUE encoding (CRuby-compatible, x86_64 + USE_FLONUM=1).
 *
 *   Qfalse  = 0x00      ...0000 0000
 *   Qnil    = 0x08      ...0000 1000
 *   Qtrue   = 0x14      ...0001 0100
 *   Qundef  = 0x34      ...0011 0100
 *   FIXNUM  = ...x01    low bit  = 1
 *   FLONUM  = ...x10    low 2 bits = 0b10  (encoded double)
 *   SYMBOL  = ...0c     low 8 bits = 0x0c  (static symbol)
 *   pointer = ...000    low 3 bits = 0
 * ---------------------------------------------------------------------- */

typedef uintptr_t VALUE;
typedef uintptr_t ID;

#define Qfalse      ((VALUE)0x00)
#define Qnil        ((VALUE)0x08)
#define Qtrue       ((VALUE)0x14)
#define Qundef      ((VALUE)0x34)

#define FIXNUM_FLAG ((VALUE)0x01)
#define FLONUM_MASK ((VALUE)0x03)
#define FLONUM_FLAG ((VALUE)0x02)
#define SYMBOL_MASK ((VALUE)0xff)
#define SYMBOL_FLAG ((VALUE)0x0c)
#define IMMEDIATE_MASK ((VALUE)0x07)

#define FIXNUM_P(v)   (((VALUE)(v)) & FIXNUM_FLAG)
#define FLONUM_P(v)   (((((VALUE)(v)) & FLONUM_MASK)) == FLONUM_FLAG)
/* True for both FLONUM (immediate Float) and heap T_FLOAT.  Use this
 * everywhere a `BUILTIN_TYPE(v) == T_FLOAT` check used to live. */
#define KORB_IS_FLOAT(v) (FLONUM_P(v) || (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_FLOAT))
#define SYMBOL_P(v)   ((((VALUE)(v)) & SYMBOL_MASK) == SYMBOL_FLAG)
#define NIL_P(v)      ((v) == Qnil)
#define TRUE_P(v)     ((v) == Qtrue)
#define FALSE_P(v)    ((v) == Qfalse)
#define UNDEF_P(v)    ((v) == Qundef)
#define SPECIAL_CONST_P(v) (((VALUE)(v)) & IMMEDIATE_MASK || (v) == Qfalse || (v) == Qnil)
#define IMMEDIATE_P(v)     (((VALUE)(v)) & IMMEDIATE_MASK)
#define RTEST(v)      (((VALUE)(v)) & ~Qnil)

#define INT2FIX(i)    ((VALUE)(((intptr_t)(i)) << 1) | FIXNUM_FLAG)
#define FIX2LONG(v)   ((long)((intptr_t)(v) >> 1))

/* FLONUM (immediate Float) — CRuby-compatible encoding.
 * Rotate-left-by-3 of the IEEE-754 double bits, mask LSB, OR FLONUM_FLAG.
 * Doubles whose top 3 exponent bits aren't 011 or 100 (i.e. NaN / Inf /
 * very large / very small / denorm / 0.0) cannot be encoded; in that
 * case korb_float_new heap-boxes via struct korb_float. */
#define KORB_BIT_ROTL64(x, n)  (((VALUE)(x) << (n)) | ((VALUE)(x) >> (64 - (n))))
#define KORB_BIT_ROTR64(x, n)  (((VALUE)(x) >> (n)) | ((VALUE)(x) << (64 - (n))))

static inline VALUE
korb_double_to_flonum(double d) {
    union { double d; VALUE v; } t;
    t.d = d;
    /* CRuby's encoding: only doubles whose top 3 exponent bits are
     * 011 or 100 fit (bits ∈ {3, 4}).  Everything else — 0.0, NaN, Inf,
     * denorms, very large / very small — heap-boxes via the caller. */
    int bits = (int)((t.v >> 60) & 0x7);
    if (((bits - 3) & ~0x01) != 0) return 0;
    /* Special-case 1.72723e-77 (all-zero mantissa with bits=3) — CRuby
     * reserves that pattern for +0.0 encoded as flonum.  We just heap. */
    if (t.v == 0x3000000000000000ULL) return 0;
    return (KORB_BIT_ROTL64(t.v, 3) & ~(VALUE)0x01) | FLONUM_FLAG;
}

static inline double
korb_flonum_to_double(VALUE v) {
    union { double d; VALUE v; } t;
    VALUE b63 = v >> 63;
    t.v = KORB_BIT_ROTR64((2 - b63) | (v & ~(VALUE)0x03), 3);
    return t.d;
}

#define FIXNUM_MAX  ((intptr_t)((((uintptr_t)1) << (sizeof(VALUE)*8 - 2)) - 1))
#define FIXNUM_MIN  (-FIXNUM_MAX - 1)
#define POSFIXABLE(i) ((i) <= FIXNUM_MAX)
#define NEGFIXABLE(i) ((i) >= FIXNUM_MIN)
#define FIXABLE(i)    (POSFIXABLE(i) && NEGFIXABLE(i))

/* heap object types */
enum korb_type {
    T_NONE = 0,
    T_OBJECT,
    T_CLASS,
    T_MODULE,
    T_FLOAT,
    T_BIGNUM,
    T_STRING,
    T_ARRAY,
    T_HASH,
    T_SYMBOL,
    T_PROC,
    T_RANGE,
    T_NODE,
    T_DATA,
    T_LAST
};

#define T_MASK 0x1f
#define FL_FROZEN ((VALUE)0x20)

struct RBasic {
    VALUE flags;       /* low 5 bits = type, others = flags */
    VALUE klass;
};

/* BUILTIN_TYPE returns the heap-object type, or T_NONE for any immediate
 * value.  This lets callers safely check `BUILTIN_TYPE(v) == T_FOO` without
 * a separate SPECIAL_CONST_P guard. */
#define BUILTIN_TYPE(v) (SPECIAL_CONST_P(v) ? T_NONE : (enum korb_type)((((struct RBasic *)(v))->flags) & T_MASK))
#define RBASIC(v)       ((struct RBasic *)(v))

/* forward */
struct Node;
struct korb_class;
struct korb_method;
struct korb_array;
struct korb_string;
struct korb_hash;
struct korb_bignum;
struct korb_object;
struct korb_proc;

/* options */
struct koruby_option {
    bool no_compiled_code;
    bool compile_only;
    bool no_generate_specialized_code;
    bool dump_ast;
    bool quiet;
    bool verbose;
    bool jit;
    bool record_all;
};
extern struct koruby_option OPTION;

/* serial for method cache */
typedef uint64_t state_serial_t;

/* method cache (inline cache).  Holds the resolved body AST + its dispatcher
 * pointer so the dispatch hot path can do a direct call without going through
 * method->ast.body->head.dispatcher (one indirect call instead of two). */
struct CTX_struct;
typedef VALUE (*korb_dispatcher_t)(struct CTX_struct *c, struct Node *n);
/* Per-callsite specialized prologue.  At method_cache_fill time we pick
 * one of: ast_simple (no rest, no opt), ast_general (rest/opt), cfunc.
 * Then dispatch is a single indirect call with no in-function branching. */
struct method_cache;
typedef VALUE (*korb_prologue_t)(struct CTX_struct *c, struct Node *callsite,
                                 VALUE recv, uint32_t argc, uint32_t arg_index,
                                 struct korb_proc *block, struct method_cache *mc);

struct method_cache {
    state_serial_t serial;
    struct korb_class *klass;
    struct korb_method *method;
    struct Node *body;             /* cached body NODE for AST methods */
    korb_dispatcher_t dispatcher;  /* body's dispatcher (specialized SD or default) */
    korb_prologue_t prologue;      /* selected at fill time — see above */
    uint32_t locals_cnt;
    uint32_t required_params_cnt;
    uint32_t total_params_cnt;     /* required + optional + rest(0/1) + post */
    int      rest_slot;            /* -1 if no *rest */
    int      block_slot;           /* -1 if no &blk */
    uint32_t post_params_cnt;      /* params after *rest */
    int      kwh_save_slot;        /* slot to stash kwargs hash; -1 if no kwargs */
    uint8_t  type;                 /* 0=AST, 1=CFUNC */
    bool     is_simple_frame;      /* method body has no yield/super/block_given/_block — slim prologue */
    VALUE (*cfunc)(struct CTX_struct *, VALUE, int, VALUE *);
    struct korb_cref *def_cref;    /* lexical cref captured at def-time */
};

/* call cache for func calls (similar) */
struct call_cache {
    state_serial_t serial;
    struct Node *body;
    uint32_t locals_cnt;
    uint32_t params_cnt;
};

/* inline ivar cache: each ivar AST node carries one of these.  The cache
 * is monomorphic on the receiver's class — same class ⇒ same slot. */
struct ivar_cache {
    struct korb_class *klass;
    int32_t slot;             /* -1 if name not present in klass */
};

/* lexical constant scope: chain of currently-nested classes/modules */
struct korb_cref {
    struct korb_class *klass;
    struct korb_cref *prev;
};

/* execution context */
typedef struct CTX_struct {
    VALUE *stack_base;
    VALUE *stack_end;
    VALUE *fp;            /* current frame: locals and arg slots */
    VALUE *sp;            /* high-water mark for GC scanning */
    VALUE self;
    struct korb_class *current_class; /* def-target class (for top-level: Object) */
    struct korb_cref *cref;           /* lexical const scope */
    const char *current_file;       /* for backtrace + require_relative */

    state_serial_t method_serial;

    /* unified exceptional control state.  When state != KORB_NORMAL, all
     * EVAL_ARG sites bail out and propagate.  state_value carries the
     * payload (exception object / return value / break value). */
    int   state;
    VALUE state_value;

    /* for call site & frame info */
    struct korb_frame *current_frame;

    /* Most-recent callsite of a cfunc dispatch — set by prologue_cfunc_inl
     * before calling the cfunc, so cfunc bodies (e.g. kernel_raise) can
     * record the line of the call into a backtrace. */
    struct Node *last_cfunc_callsite;
} CTX;

#define KORB_NORMAL 0
#define KORB_RAISE  1
#define KORB_RETURN 2
#define KORB_BREAK  3
#define KORB_NEXT   4
#define KORB_RETRY  5
#define KORB_REDO   6
#define KORB_THROW  7

/* current_frame chain (for backtrace + GC root) */
struct korb_frame {
    struct korb_frame *prev;
    struct Node *caller_node;  /* for backtrace */
    struct korb_method *method;
    VALUE self;
    VALUE *fp;
    uint32_t locals_cnt;
    struct korb_proc *block;   /* block passed to this method (NULL if none) */
};

/* push/pop frame helpers via macro */
#define KORB_PUSH_FRAME(c, mtd, fp_, locals_, caller) \
    struct korb_frame _frame_ = {                     \
        .prev = (c)->current_frame,                 \
        .caller_node = (caller),                    \
        .method = (mtd),                            \
        .self = (c)->self,                          \
        .fp = (fp_),                                \
        .locals_cnt = (locals_),                    \
    };                                              \
    (c)->current_frame = &_frame_;                  \
    do{}while(0)

#define KORB_POP_FRAME(c) \
    do { (c)->current_frame = _frame_.prev; } while (0)

#endif /* KORUBY_CONTEXT_H */
