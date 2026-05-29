#ifndef CHECK_H
#define CHECK_H 1

#include "context.h"

// Static type representation (ChocoPy §2.4).
typedef struct Type {
    enum type_kind { T_CLASS, T_LIST, T_NONE, T_EMPTY } kind;
    const char *cls;     // T_CLASS: "int"/"bool"/"str"/"object"/<user class>
    struct Type *elem;   // T_LIST element type
} Type;

Type *type_class(const char *name);
Type *type_list(Type *elem);
Type *type_none(void);
Type *type_empty(void);
const char *type_str(Type *t);   // human-readable, for error messages

// Run the static type checker over the parsed program.  Returns the number
// of errors found (0 == ok).  Errors are printed to stderr.
struct Program;     // from parse.h
int anpy_typecheck(struct Program *prog);

#endif // CHECK_H
