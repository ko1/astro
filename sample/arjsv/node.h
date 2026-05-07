#ifndef ARJSV_NODE_H
#define ARJSV_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef int (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
int EVAL(CTX *c, NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

// No PG support — HOPT == HORG == HASH (required by runtime/astro_code_store.c).
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

// Wrap / unwrap NODE as a Ruby T_DATA so the GC can keep it alive while
// referenced from a Schema's children-array.  Each NODE has at most one
// wrapper (cached in head.rb_wrapper).
VALUE arjsv_wrap_node(NODE *n);
NODE *arjsv_unwrap_node(VALUE v);
void arjsv_node_mark(void *ptr);
extern const rb_data_type_t arjsv_node_type;

// Numeric helpers shared between node.def and arjsv.c.
static inline bool
arjsv_value_is_number(VALUE v)
{
    return RB_INTEGER_TYPE_P(v) || RB_FLOAT_TYPE_P(v);
}

static inline double
arjsv_value_to_double(VALUE v)
{
    if (RB_FLOAT_TYPE_P(v)) return RFLOAT_VALUE(v);
    return NUM2DBL(v);
}

// Compute the type bitmask for a Ruby VALUE.  Returns 0 if v is none of the
// JSON-recognised types (e.g. a Ruby Symbol — JSON has no symbols).
static inline uint32_t
arjsv_value_types(VALUE v)
{
    if (v == Qnil) return ARJSV_T_NULL;
    if (v == Qtrue || v == Qfalse) return ARJSV_T_BOOLEAN;
    if (RB_INTEGER_TYPE_P(v)) return ARJSV_T_INTEGER | ARJSV_T_NUMBER;
    if (RB_FLOAT_TYPE_P(v)) {
        double d = RFLOAT_VALUE(v);
        if (isfinite(d) && d == floor(d)) {
            return ARJSV_T_INTEGER | ARJSV_T_NUMBER;
        }
        return ARJSV_T_NUMBER;
    }
    if (RB_TYPE_P(v, T_STRING)) return ARJSV_T_STRING;
    if (RB_TYPE_P(v, T_ARRAY))  return ARJSV_T_ARRAY;
    if (RB_TYPE_P(v, T_HASH))   return ARJSV_T_OBJECT;
    return 0;
}

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                  // referenced by runtime/astro_code_store.c (Hopt index key)

    VALUE rb_wrapper;
};

#include "node_head.h"

#endif // ARJSV_NODE_H
