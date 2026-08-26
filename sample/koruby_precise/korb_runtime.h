/* koruby_precise v2 — korb_runtime.h
 *
 * Shared surface for the runtime core (korb_runtime.c) and the per-class builtin
 * method files under builtins/ (all #included into korb_runtime.c's translation
 * unit).  Holds the common SELF_* receiver accessors, block-guard / delegation
 * macros, and small cross-class inline helpers so each builtin file inherits
 * them without re-declaring.
 */
#ifndef KORUBY_RUNTIME_H
#define KORUBY_RUNTIME_H 1

#include <math.h>
#include "node.h"

/* KORB_FRAME_SLACK is defined in node.h (included above) so the inlined call
 * fast path sees it in every TU, including the code_store SDs. */

/* captured_self is threaded as a POINTER to the defining frame's (scanned,
 * GC-stable) self cell so it never goes stale across the GCs a block-iterating
 * builtin triggers.  NULL = no block context (ordinary call); deref yields the
 * always-fresh self.  KORB_CSELF_VAL guards the NULL case for VALUE consumers. */
#define KORB_CSELF_VAL(cs) ((cs) ? *(cs) : KORB_NIL)

/* ---- receiver accessors -------------------------------------------------- *
 * In every builtin method `self` is the rooted VALUE_REF receiver; SELF_<T>
 * decodes it to the concrete payload (or scalar for Int/Float). */
#define SELF_INT   FIX2LONG(VALUE_REF_GET(self))
#define SELF_FLT   (korb_float_val(VALUE_REF_GET(self)))
#define SELF_STR   VAL2STR(VALUE_REF_GET(self))
#define SELF_ARY   VAL2ARY(VALUE_REF_GET(self))
#define SELF_HASH  VAL2HASH(VALUE_REF_GET(self))
#define SELF_RANGE VAL2RANGE(VALUE_REF_GET(self))
#define SELF_RAT   VAL2RAT(VALUE_REF_GET(self))
#define SELF_CPX   VAL2CPX(VALUE_REF_GET(self))
#define SELF_ENUM  VAL2ENUM(VALUE_REF_GET(self))
#define SELF_SET   VAL2SET(VALUE_REF_GET(self))

/* ---- block-given guard (yielding methods) -------------------------------- *
 * Used inside a method whose params are (c, slots, ..., block, ...): bail with
 * NotImplementedError when called without a block (eager Enumerator gap). */
/* no block → return an Enumerator (to_enum(:method); the method name is the tail
 * of `what` after '#').  self must be the receiver VALUE_REF in scope. */
#define REQUIRE_BLOCK(what) \
    do { if (UNLIKELY(block == NULL)) { \
        const char *_wn__ = strrchr(what, '#'); _wn__ = _wn__ ? _wn__ + 1 : (what); \
        slots[0] = VALUE_REF_GET(self); \
        slots[1] = ID2SYM(korb_intern(c->vm, _wn__, (uint32_t)strlen(_wn__))); \
        return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); \
    } } while (0)
#define ARY_REQUIRE_BLOCK(what) REQUIRE_BLOCK(what)

/* ---- Set → Array delegation --------------------------------------------- *
 * Define a Set method that forwards to the Array method over the backing
 * elements array (Set is array-backed). */
#define KORB_SET_DELEG_BLK(name, target) \
    static RESULT name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { \
        slots[0] = SELF_SET->elems; return target(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself); }
#define KORB_SET_DELEG(name, target) \
    static RESULT name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        slots[0] = SELF_SET->elems; return target(c, slots + 1, VALUE_REF_AT(&slots[0]), a); }

/* ---- small cross-class numeric helpers ----------------------------------- */

/* Floored float modulo (sign follows divisor) — Integer/Float op Float. */
static inline double korb_float_fmod(double s, double f) {
    double r = fmod(s, f);
    if (r != 0.0 && ((r < 0) != (f < 0))) r += f;
    return r;
}

#endif /* KORUBY_RUNTIME_H */
