#ifndef ASOM_CONTEXT_H
#define ASOM_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define ASOM_DEBUG 1
#if ASOM_DEBUG
#define ASOM_ASSERT(expr) assert(expr)
#else
#define ASOM_ASSERT(expr) ((void)0)
#endif

#define LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

// -----------------------------------------------------------------------------
// Tagged value representation.
//
//   ...0   -> object pointer (8-byte aligned -> low 3 bits zero)
//   ...1   -> SmallInteger (high 63 bits, sign-extended on shift)
//
// The low-bit tag is the same encoding SOM++ uses, and lets a value fit in a
// machine word with branchless integer fast paths.
// -----------------------------------------------------------------------------

typedef intptr_t VALUE;

#define ASOM_TAG_INT  0x1
#define ASOM_TAG_MASK 0x1

#define ASOM_IS_INT(v)  (((VALUE)(v)) & ASOM_TAG_INT)
#define ASOM_IS_OBJ(v)  ((((VALUE)(v)) & ASOM_TAG_MASK) == 0)
#define ASOM_INT2VAL(i) (((VALUE)(intptr_t)(i) << 1) | ASOM_TAG_INT)
#define ASOM_VAL2INT(v) ((intptr_t)(v) >> 1)
#define ASOM_OBJ2VAL(p) ((VALUE)(p))
#define ASOM_VAL2OBJ(v) ((struct asom_object *)(v))

// Forward declarations.
struct asom_object;
struct asom_class;
struct asom_method;
struct asom_block;
struct asom_string;
struct Node;
typedef uint64_t state_serial_t;

// -----------------------------------------------------------------------------
// Object headers.
// -----------------------------------------------------------------------------

struct asom_object {
    struct asom_class *klass;
    // followed by inline fields[] (count tracked on the class)
};

struct asom_string {
    struct asom_object hdr;
    size_t len;
    const char *bytes; // NUL-terminated, static or arena-owned
};

struct asom_array {
    struct asom_object hdr;
    uint32_t len;
    VALUE *data;        // initialised to nil
};

struct asom_double {
    struct asom_object hdr;
    double value;
};

// Tag identifying which built-in primitive a method is, so call sites can
// rewrite their AST node to a type-specialized variant on first cache fill.
// Generic primitives (and all AST methods) carry ASOM_PRIM_GENERIC.
enum asom_prim_kind {
    ASOM_PRIM_GENERIC = 0,
    ASOM_PRIM_INT_PLUS,
    ASOM_PRIM_INT_MINUS,
    ASOM_PRIM_INT_TIMES,
    ASOM_PRIM_INT_LT,
    ASOM_PRIM_INT_GT,
    ASOM_PRIM_INT_LE,
    ASOM_PRIM_INT_GE,
    ASOM_PRIM_INT_EQ,
};

struct asom_method {
    const char *selector;
    uint32_t num_params;          // not counting receiver
    uint32_t num_locals;
    struct Node *body;            // method body AST (NULL = primitive)
    void *primitive;              // C function pointer if primitive
    struct asom_class *holder;
    enum asom_prim_kind prim_kind; // tag for type-feedback specialization
};

// Open-hash table for fast lookup by interned selector pointer, plus a
// parallel insertion-order array so reflective enumerations
// (`Class>>methods`) return methods in source-definition order. The
// ordered array doesn't dedupe — if a selector is redefined, both entries
// stay so iteration order is stable.
struct asom_method_table {
    uint32_t cap;
    uint32_t cnt;
    struct asom_method **slots;

    uint32_t order_cnt;
    uint32_t order_cap;
    struct asom_method **ordered;
};

struct asom_class {
    struct asom_object hdr;       // metaclass instance header
    const char *name;             // interned
    struct asom_class *superclass;
    struct asom_class *metaclass;
    uint32_t num_instance_fields;
    const char **field_names;     // length = num_instance_fields
    struct asom_method_table methods;
    // Class-side instance fields (the metaclass's slots, lifted onto the
    // class object itself). NULL when the class has no class-side state.
    uint32_t num_class_side_fields;
    VALUE *class_side_fields;
    const char **class_side_field_names;
};

struct asom_block {
    struct asom_object hdr;
    struct asom_method *method;   // per-block method (body, num_params, num_locals)
    struct asom_frame *home;      // home (method) frame, target of ^ NLR
    struct asom_frame *lexical_parent; // immediate enclosing frame (for var lookup)
    VALUE captured_self;          // captured self at block-creation time
};

// -----------------------------------------------------------------------------
// Frame / context.
// -----------------------------------------------------------------------------

struct asom_frame {
    VALUE self;
    VALUE *locals;                  // [args..., locals...]
    struct asom_method *method;
    struct asom_frame *parent;      // caller (for tracebacks)
    struct asom_frame *home;        // home method frame (NLR target)
    struct asom_frame *lexical_parent; // outer frame in lexical chain (var lookup)
    int returned;
    // Frame pooling. `captured` is set when a closure created during this
    // frame's body stores us as its lexical_parent — that pins the frame
    // to the heap (the closure may outlive this call). `pool_slots` records
    // the size class so block_invoke knows which free list to push to.
    bool captured;
    uint16_t pool_slots;            // 0 = not poolable (heap-allocated, leak)
};

struct asom_option {
    bool quiet;
    bool dump_ast;
    bool no_compiled_code;        // --plain
    bool record_all;              // legacy flag still referenced by generated allocators
    bool aot_compile_first;       // -c / --aot-compile-first: bake every method (SD_<Horg>) before run
    bool pgc_at_exit;             // -p / --pg-compile: bake hot entries after a clean run
    bool aot_only;                // --aot-only: skip PGC index lookup at cs_load time
    bool compiled_only;           // --compiled-only: warn if interpreter dispatcher is used
    int  pg_threshold;            // --pg-threshold=N (default 100, env ASOM_PG_THRESHOLD)
    bool verbose;                 // --verbose: trace cs_* operations
    const char *classpath;        // ":"-separated list of dirs to search
    const char *entry_class;      // class name to instantiate and run
    const char *code_store_dir;   // --code-store=DIR (default code_store/, env ASOM_CODE_STORE)
    const char *preload;          // --preload=A,B,C: extra classes to load (and bake) before run
};

extern struct asom_option OPTION;

typedef struct asom_ctx {
    // Globals / well-known objects.
    struct asom_class *cls_object;
    struct asom_class *cls_class;
    struct asom_class *cls_metaclass;
    struct asom_class *cls_nil;
    struct asom_class *cls_boolean;
    struct asom_class *cls_true;
    struct asom_class *cls_false;
    struct asom_class *cls_integer;
    struct asom_class *cls_double;
    struct asom_class *cls_string;
    struct asom_class *cls_symbol;
    struct asom_class *cls_array;
    struct asom_class *cls_block;
    struct asom_class *cls_block1;     // 0-arg blocks
    struct asom_class *cls_block2;     // 1-arg blocks
    struct asom_class *cls_block3;     // 2-arg blocks
    struct asom_class *cls_method;
    struct asom_class *cls_system;
    struct asom_class *cls_random;
    VALUE val_nil;
    VALUE val_true;
    VALUE val_false;

    // Active frame chain.
    struct asom_frame *frame;

    state_serial_t serial;
} CTX;

#endif // ASOM_CONTEXT_H
