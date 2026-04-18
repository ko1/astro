// Cold/slow helper functions shared across SD_*.o.
//
// These used to be `static __attribute__((noinline, cold))` inside node.def
// and hence were emitted into every SD_*.o (via #include "node_eval.c"),
// bloating all.so linearly with the number of SDs.  Moving them here — with
// external linkage — means each helper lives once in abruby.so, and the
// SD_*.o files resolve them as undefined externs at dlopen time (abruby.so's
// symbol scope is available to all.so because Ruby loads C extensions with
// RTLD_GLOBAL).
//
// The bodies are kept line-for-line identical to the originals.  Callers
// reach them through prototypes declared in node.h.

#include "node.h"

// === Fixnum arithmetic overflow helpers ===================================
// Used when fixnum-tagged add/sub/mul would overflow a 63-bit signed range —
// promote to Bignum and retry via CRuby's rb_big_*.

RESULT
node_fixnum_plus_overflow(CTX *c, VALUE lv, VALUE rv)
{
    return RESULT_OK(abruby_bignum_new(c, LONG2NUM(FIX2LONG(lv) + FIX2LONG(rv))));
}

RESULT
node_fixnum_minus_overflow(CTX *c, VALUE lv, VALUE rv)
{
    return RESULT_OK(abruby_bignum_new(c, LONG2NUM(FIX2LONG(lv) - FIX2LONG(rv))));
}

// === Instance variable cache-miss slow paths ==============================

VALUE
node_ivar_get_slow(VALUE self, ID name, struct ivar_cache *ic)
{
    const struct abruby_object *obj =
        (const struct abruby_object *)ABRUBY_DATA_PTR(self);
    VALUE slot_fix;
    if (ab_id_table_lookup(&obj->klass->ivar_shape, name, &slot_fix)) {
        unsigned int slot = (unsigned int)FIX2ULONG(slot_fix);
        if (slot < obj->ivar_cnt) {
            // Reads don't transition → post == pre.
            uint32_t shape = abruby_shape_id_read(self);
            ic->shape_id = shape;
            ic->post_shape_id = shape;
            ic->slot = slot;
            return abruby_object_ivar_read(obj, slot);
        }
    }
    return Qnil;
}

void
node_ivar_set_slow(CTX * restrict c, VALUE self, ID name, VALUE v, struct ivar_cache *ic)
{
    struct abruby_object *obj =
        (struct abruby_object *)ABRUBY_DATA_PTR(self);
    struct abruby_class *klass = (struct abruby_class *)obj->klass;
    // Look up the slot in the class shape; add it if new.
    VALUE slot_fix;
    unsigned int slot;
    if (ab_id_table_lookup(&klass->ivar_shape, name, &slot_fix)) {
        slot = (unsigned int)FIX2ULONG(slot_fix);
    } else {
        slot = klass->ivar_shape.cnt;
        ab_id_table_insert(&klass->ivar_shape, name, LONG2FIX((long)slot));
    }
    // Remember the shape BEFORE the grow, so subsequent sets on fresh
    // objects arriving at the same pre-shape hit the fast path.
    uint32_t pre_shape = abruby_shape_id_read(self);
    abruby_object_grow_ivars(obj, slot + 1);
    if (slot < ABRUBY_OBJECT_INLINE_IVARS) {
        obj->ivars[slot] = v;
    } else {
        obj->extra_ivars[slot - ABRUBY_OBJECT_INLINE_IVARS] = v;
    }
    uint32_t post_shape = abruby_shape_for(c->abm, klass, obj->ivar_cnt);
    if (post_shape != pre_shape) {
        abruby_shape_id_write(self, post_shape);
    }
    ic->shape_id = pre_shape;
    ic->post_shape_id = post_shape;
    ic->slot = slot;
}

// === ArgumentError for wrong-argc call sites =============================

RESULT
raise_argc_error(CTX *c, NODE *call_site,
                 unsigned int given, unsigned int required,
                 unsigned int max_params, bool has_rest)
{
    char buf[128];
    if (has_rest) {
        snprintf(buf, sizeof(buf),
                 "wrong number of arguments (given %u, expected %u+)",
                 given, required);
    } else if (required == max_params) {
        snprintf(buf, sizeof(buf),
                 "wrong number of arguments (given %u, expected %u)",
                 given, required);
    } else {
        snprintf(buf, sizeof(buf),
                 "wrong number of arguments (given %u, expected %u..%u)",
                 given, required, max_params);
    }
    VALUE exc;
    PUSH_FRAME(call_site);
    exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, buf));
    POP_FRAME();
    return (RESULT){exc, RESULT_RAISE};
}

// === Fixnum multiplication overflow (cont'd) =============================

RESULT
node_fixnum_mul_overflow(CTX *c, long a, long b)
{
    // Inlined copy of integer_mul_calc(c, LONG2FIX(a), LONG2FIX(b)).  We
    // can't call integer_mul_calc directly from here because it lives as
    // `static inline` in node.def and is not visible outside node_eval.c.
    VALUE lv = LONG2FIX(a), rv = LONG2FIX(b);
    VALUE l = AB_INT_UNWRAP(lv), r = AB_INT_UNWRAP(rv);
    VALUE result;
    if (RB_TYPE_P(l, T_BIGNUM))      result = rb_big_mul(l, r);
    else if (RB_TYPE_P(r, T_BIGNUM)) result = rb_big_mul(r, l);  // commutative
    else                             result = rb_funcall(l, c->ids->op_mul, 1, r);
    return RESULT_OK(AB_NUM_WRAP(c, result));
}
