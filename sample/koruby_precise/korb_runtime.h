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

/* Frame-push headroom: covers in-frame expression staging without a per-node
 * check (v2_design §3.5). */
#define KORB_FRAME_SLACK 1024

/* ---- receiver accessors -------------------------------------------------- *
 * In every builtin method `self` is the rooted VALUE_REF receiver; SELF_<T>
 * decodes it to the concrete payload (or scalar for Int/Float). */
#define SELF_INT   FIX2LONG(VALUE_REF_GET(self))
#define SELF_FLT   (VAL2FLT(VALUE_REF_GET(self))->val)
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
#define REQUIRE_BLOCK(what) \
    do { if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, \
        what " without a block (Enumerator) is not supported"); } while (0)
#define ARY_REQUIRE_BLOCK(what) REQUIRE_BLOCK(what)

/* ---- Set → Array delegation --------------------------------------------- *
 * Define a Set method that forwards to the Array method over the backing
 * elements array (Set is array-backed). */
#define KORB_SET_DELEG_BLK(name, target) \
    static RESULT name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { \
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
