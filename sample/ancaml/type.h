#ifndef ANCAML_TYPE_H
#define ANCAML_TYPE_H 1

#include "context.h"

// MinCaml's monomorphic type language (Type.t).  Type inference is
// Hindley–Milner *without* generalization (let is monomorphic), using
// destructive unification over TY_VAR cells.  Unresolved variables left
// after inference default to int — exactly as MinCaml's Typing module.
typedef struct Type {
    enum type_kind { TY_UNIT, TY_BOOL, TY_INT, TY_FLOAT, TY_FUN, TY_TUPLE, TY_ARRAY, TY_VAR } kind;
    struct Type **args;  int nargs;   struct Type *ret;   // TY_FUN
    struct Type **elems; int nelems;                      // TY_TUPLE
    struct Type *elem;                                    // TY_ARRAY
    struct Type *content;                                 // TY_VAR (NULL if unbound)
    int id;                                               // TY_VAR id, for display
} Type;

Type *ty_unit(void);
Type *ty_bool(void);
Type *ty_int(void);
Type *ty_float(void);
Type *ty_var(void);
Type *ty_fun(Type **args, int nargs, Type *ret);
Type *ty_tuple(Type **elems, int nelems);
Type *ty_array(Type *elem);

const char *ty_str(Type *t);     // human-readable (defaults vars to int)

// Builtin (external) function signature lookup; NULL if `name` is unknown.
Type *ac_external_type(const char *name);

// Run the static type checker over the parsed program.  Returns the number
// of type errors (0 == ok).  The first error is reported to stderr.
struct Program;
int ac_typecheck(struct Program *prog);

#endif // ANCAML_TYPE_H
