#ifndef ASOM_RUNTIME_H
#define ASOM_RUNTIME_H 1

#include "context.h"

// Per-call inline cache; mutable side-data, intentionally excluded from the
// node Merkle hash so identical send sites share specialized dispatchers.
struct asom_callcache {
    state_serial_t serial;
    struct asom_class *cached_class;
    struct asom_method *cached_method;
};

// String / symbol interning. Both return tagged VALUEs for asom_string objects;
// the symbol pool guarantees pointer equality for identical symbol names.
VALUE asom_intern_string(CTX *c, const char *bytes);
VALUE asom_intern_symbol(CTX *c, const char *bytes);

// String literal interning at parse time, before CTX exists.
const char *asom_intern_cstr(const char *s);

// Globals (class-name lookup; nil/true/false/Smalltalk).
VALUE asom_global_get(CTX *c, const char *name);
void  asom_global_set(CTX *c, const char *name, VALUE v);

// Block / closure construction.
VALUE asom_make_block(CTX *c, struct Node *body, uint32_t num_params, uint32_t num_locals);
VALUE asom_block_invoke(CTX *c, struct asom_block *b, VALUE *args, uint32_t nargs);

// Send — slow path (cache miss + DNU). The IC fast path is the
// `static inline asom_send` below.
VALUE asom_send_slow(CTX *c, VALUE receiver, const char *selector,
                     uint32_t nargs, VALUE *args, struct asom_callcache *cc);

// AST-method invocation slow path (heap-allocates a frame, longjmp setjmp,
// runs the body via EVAL). The IC fast path inlines the primitive case
// below before falling through to this on AST-bodies.
VALUE asom_invoke_ast(CTX *c, struct asom_method *m, VALUE recv,
                      VALUE *args, uint32_t nargs);

// Inline-cached method invocation. The primitive fast path is small enough
// to inline at every send site (saves ~5–10ns/call: avoids the indirect
// function call into asom_runtime.c). For AST methods we tail-call into
// asom_invoke_ast (which sets up the frame).
typedef VALUE (*asom_prim0_t)(CTX *, VALUE);
typedef VALUE (*asom_prim1_t)(CTX *, VALUE, VALUE);
typedef VALUE (*asom_prim2_t)(CTX *, VALUE, VALUE, VALUE);
typedef VALUE (*asom_prim3_t)(CTX *, VALUE, VALUE, VALUE, VALUE);

static inline VALUE
asom_invoke_method(CTX *c, struct asom_method *m, VALUE recv,
                   VALUE *args, uint32_t nargs)
{
    if (__builtin_expect(m->primitive != NULL, 1)) {
        switch (nargs) {
            case 0: return ((asom_prim0_t)m->primitive)(c, recv);
            case 1: return ((asom_prim1_t)m->primitive)(c, recv, args[0]);
            case 2: return ((asom_prim2_t)m->primitive)(c, recv, args[0], args[1]);
            case 3: return ((asom_prim3_t)m->primitive)(c, recv, args[0], args[1], args[2]);
        }
    }
    return asom_invoke_ast(c, m, recv, args, nargs);
}
VALUE asom_super_send(CTX *c, VALUE receiver, const char *selector,
                      uint32_t nargs, VALUE *args, struct asom_callcache *cc);

// Non-local return out of a block; unwinds via longjmp through the home frame.
void asom_nonlocal_return(CTX *c, VALUE v);

// Class machinery. Inlined here so SD shards (compiled in their own TU
// against the bare `node.h` → `asom_runtime.h`) don't pay a GOT indirect
// call per send to fetch the receiver's class. The body is tiny (one
// tag check + a struct field load) so always-inlining is unambiguous.
static inline struct asom_class *
asom_class_of(CTX *c, VALUE v)
{
    if (ASOM_IS_INT(v)) return c->cls_integer;
    struct asom_object *o = ASOM_VAL2OBJ(v);
    return o->klass;
}

// IC fast path. Inlined into every node_send / SD_*.c call site so a cache
// hit costs only the inline integer compares + an asom_invoke call. The
// slow path (asom_send_slow) handles allocation, method lookup, and the
// `doesNotUnderstand:arguments:` retry.
static inline VALUE
asom_send(CTX *c, VALUE receiver, const char *selector,
          uint32_t nargs, VALUE *args, struct asom_callcache *cc)
{
    struct asom_class *cls = asom_class_of(c, receiver);
    if (__builtin_expect(cc != NULL
                         && cc->serial == c->serial
                         && cc->cached_class == cls, 1)) {
        return asom_invoke_method(c, cc->cached_method, receiver, args, nargs);
    }
    return asom_send_slow(c, receiver, selector, nargs, args, cc);
}
struct asom_method *asom_class_lookup(struct asom_class *cls, const char *selector);
void asom_class_define_method(struct asom_class *cls, struct asom_method *m);

// Object allocation.
VALUE asom_object_new(CTX *c, struct asom_class *cls);
VALUE asom_array_new (CTX *c, uint32_t len);
VALUE asom_double_new(CTX *c, double v);
VALUE asom_string_new(CTX *c, const char *bytes, size_t len);

// Class loader: parse a .som file (path) and register the class.
struct asom_class *asom_load_class(CTX *c, const char *name);

// Bootstrapping.
void asom_runtime_init(CTX *c);
void asom_classpath_add(const char *dir);

// Entry-point tracking for AOT/PGC compilation. Every AST method body
// installed from a .som file is registered so `--aot-compile-first` and
// `--pg-compile` can walk them.
struct asom_entry {
    struct Node *body;
    const char *label;        // "ClassName>>selector" — interned
    const char *file;         // .som file the body came from (NULL for synthetic)
};

void asom_register_entry(struct Node *body, const char *label, const char *file);
struct asom_entry *asom_entries(uint32_t *count_out);

#endif // ASOM_RUNTIME_H
