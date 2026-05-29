#ifndef VALUE_H
#define VALUE_H 1

#include "context.h"

// Heap allocation (GC-managed).
anpy_str  *anpy_str_new(const char *s, int32_t len);
anpy_list *anpy_list_new(int32_t len);            // elements initialised to NONE

// Accessors.
static inline enum anpy_kind obj_kind(VALUE v) { return ((anpy_obj *)(v))->kind; }
static inline bool is_str(VALUE v)  { return IS_PTR(v) && obj_kind(v) == K_STR; }
static inline bool is_list(VALUE v) { return IS_PTR(v) && obj_kind(v) == K_LIST; }
static inline bool is_inst(VALUE v) { return IS_PTR(v) && obj_kind(v) == K_OBJ; }

// Operations (runtime errors abort via anpy_runtime_error).
VALUE anpy_add(CTX *c, VALUE a, VALUE b);          // int+int, str+str, list+list
VALUE anpy_index(CTX *c, VALUE seq, VALUE idx);    // str[i] / list[i]
void  anpy_index_set(CTX *c, VALUE seq, VALUE idx, VALUE val);  // list[i] = v
VALUE anpy_len(CTX *c, VALUE v);
bool  anpy_eq(CTX *c, VALUE a, VALUE b);           // == for int/bool/str

// for-loop iteration over str/list (kept tiny so node_for can drive the loop
// itself with EVAL_ARG(body) — see node.def).  ChocoPy lists are fixed-length,
// so the length is computed once.
long  anpy_seq_len(CTX *c, VALUE v);               // str/list length; errors otherwise
VALUE anpy_seq_get(VALUE v, long i);               // element i (str -> length-1 str)
int32_t anpy_strcmp_eq(VALUE a, VALUE b);

void  anpy_print(CTX *c, VALUE v);                 // print() builtin
VALUE anpy_input(CTX *c);                          // input() builtin

void  anpy_runtime_error(CTX *c, const char *fmt, ...);

// REPL/error unwinding hooks (implemented in node.c).
#include <setjmp.h>
jmp_buf *anpy_get_jmp(void);
void anpy_set_jmp_active(int v);

#endif // VALUE_H
