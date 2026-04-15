#include "builtin.h"

// Fixnum fast path: both Fixnum -> direct C arithmetic.
// Otherwise: AB_INT_UNWRAP/AB_NUM_UNWRAP to get raw CRuby values,
// call rb_funcall/rb_big_*, wrap result with AB_NUM_WRAP.

static RESULT ab_integer_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(AB_NUM_WRAP(c, LONG2NUM(FIX2LONG(self) + FIX2LONG(argv[0]))));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_plus(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("+"), 1, ra)));
}
static RESULT ab_integer_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(AB_NUM_WRAP(c, LONG2NUM(FIX2LONG(self) - FIX2LONG(argv[0]))));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_minus(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("-"), 1, ra)));
}
static RESULT ab_integer_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_mul(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("*"), 1, ra)));
}
static RESULT ab_integer_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        if (UNLIKELY(b == 0)) {
            VALUE exc = abruby_exception_new(c, c->current_frame, abruby_str_new_cstr(c, "divided by 0"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long d = a / b;
        // Ruby floor division: adjust if signs differ and there's a remainder
        if ((a ^ b) < 0 && d * b != a) d--;
        return RESULT_OK(LONG2FIX(d));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_div(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("/"), 1, ra)));
}
static RESULT ab_integer_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        if (UNLIKELY(b == 0)) {
            VALUE exc = abruby_exception_new(c, c->current_frame, abruby_str_new_cstr(c, "divided by 0"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long r = a % b;
        // Ruby modulo: result has same sign as divisor
        if (r != 0 && (r ^ b) < 0) r += b;
        return RESULT_OK(LONG2FIX(r));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_modulo(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("%"), 1, ra)));
}
static RESULT ab_integer_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(c, rb_big_pow(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("**"), 1, ra)));
}
static RESULT ab_integer_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(AB_NUM_WRAP(c, LONG2NUM(-FIX2LONG(self))));
    VALUE rs = AB_INT_UNWRAP(self);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("-@"), 0)));
}

// Ordered compare (lt/le/gt/ge): fixnum fast path, then bignum via rb_big_cmp.
#define AB_INT_CMP(name, op, op_sym)                                          \
    static RESULT ab_integer_##name(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { \
        if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))                       \
            return RESULT_OK(FIX2LONG(self) op FIX2LONG(argv[0]) ? Qtrue : Qfalse); \
        VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);           \
        if (RB_TYPE_P(rs, T_BIGNUM))                                           \
            return RESULT_OK(FIX2LONG(rb_big_cmp(rs, ra)) op 0 ? Qtrue : Qfalse); \
        return RESULT_OK(RTEST(rb_funcall(rs, rb_intern(op_sym), 1, ra)) ? Qtrue : Qfalse); \
    }
AB_INT_CMP(lt, <,  "<")
AB_INT_CMP(le, <=, "<=")
AB_INT_CMP(gt, >,  ">")
AB_INT_CMP(ge, >=, ">=")
#undef AB_INT_CMP

// Equality: fixnum identity (tagged pointers are canonical); bignum via rb_big_eq.
// invert=0 → eq, invert=1 → neq. Branch on invert is compile-time constant.
#define AB_INT_EQ(name, invert)                                                \
    static RESULT ab_integer_##name(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { \
        VALUE eq;                                                              \
        if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {                     \
            eq = self == argv[0] ? Qtrue : Qfalse;                             \
        } else {                                                               \
            VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);       \
            if (RB_TYPE_P(rs, T_BIGNUM))                                       \
                eq = rb_big_eq(rs, ra);                                        \
            else                                                               \
                eq = RTEST(rb_funcall(rs, rb_intern("=="), 1, ra)) ? Qtrue : Qfalse; \
        }                                                                      \
        return RESULT_OK((invert) ? (eq == Qtrue ? Qfalse : Qtrue) : eq);      \
    }
AB_INT_EQ(eq,  0)
AB_INT_EQ(neq, 1)
#undef AB_INT_EQ

static RESULT ab_integer_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) {
        char buf[32]; snprintf(buf, sizeof(buf), "%ld", FIX2LONG(self));
        return RESULT_OK(abruby_str_new_cstr(c, buf));
    }
    return RESULT_OK(abruby_str_new(c, rb_big2str(AB_INT_UNWRAP(self), 10)));
}
static RESULT ab_integer_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return ab_integer_inspect(c, self, 0, NULL);
}
static RESULT ab_integer_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) return RESULT_OK(FIX2LONG(self) == 0 ? Qtrue : Qfalse);
    return RESULT_OK(Qfalse);
}
static RESULT ab_integer_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(abruby_float_new_wrap(c, rb_float_new((double)FIX2LONG(self))));
    return RESULT_OK(abruby_float_new_wrap(c, rb_funcall(AB_INT_UNWRAP(self), rb_intern("to_f"), 0)));
}
static RESULT ab_integer_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) { long v = FIX2LONG(self); return RESULT_OK(AB_NUM_WRAP(c, LONG2NUM(v < 0 ? -v : v))); }
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(AB_INT_UNWRAP(self), rb_intern("abs"), 0)));
}

static RESULT ab_integer_cmp(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        return RESULT_OK(LONG2FIX(a < b ? -1 : (a > b ? 1 : 0)));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(rb_big_cmp(rs, ra));
    return RESULT_OK(rb_funcall(rs, rb_intern("<=>"), 1, ra));
}
static RESULT ab_integer_lshift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("<<"), 1, ra)));
}
static RESULT ab_integer_rshift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) >> FIX2LONG(argv[0])));
    if (NIL_P(argv[0])) return RESULT_OK(INT2FIX(0));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern(">>"), 1, ra)));
}
static RESULT ab_integer_band(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) & FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("&"), 1, ra)));
}
static RESULT ab_integer_bor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) | FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("|"), 1, ra)));
}
static RESULT ab_integer_bxor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) ^ FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("^"), 1, ra)));
}
static RESULT ab_integer_bnot(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(AB_NUM_WRAP(c, LONG2NUM(~FIX2LONG(self))));
    VALUE rs = AB_INT_UNWRAP(self);
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(rs, rb_intern("~"), 0)));
}

// Integer#times { |i| ... } — yields i = 0, 1, ..., self - 1.
// Returns self (Ruby semantics).  BREAK / RAISE / RETURN propagate up
// for the surrounding dispatch_method_frame_with_block to handle.
static RESULT ab_integer_times(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (UNLIKELY(!FIXNUM_P(self))) {
        // Bignum#times: iterate up to self by integer increment.  Rare
        // enough to just convert to long and fall through; abruby's
        // Bignum wrapping makes this slightly awkward, so for MVP we
        // only support Fixnum receivers.
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "Bignum#times is not supported in Phase 1"));
        return (RESULT){exc, RESULT_RAISE};
    }
    long n = FIX2LONG(self);
    for (long i = 0; i < n; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = abruby_yield(c, 1, &iv);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
    }
    return RESULT_OK(self);
}

// Integer#step(limit, step=1) { |i| ... } — yields self, self+step, ...
// until i would cross `limit`.  Supports negative step (counts down).  Only
// Fixnum receivers/args are supported (matches Integer#times scope).
static RESULT ab_integer_even_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    long v = FIXNUM_P(self) ? FIX2LONG(self) : NUM2LONG(AB_INT_UNWRAP(self));
    return RESULT_OK(v % 2 == 0 ? Qtrue : Qfalse);
}
static RESULT ab_integer_odd_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    long v = FIXNUM_P(self) ? FIX2LONG(self) : NUM2LONG(AB_INT_UNWRAP(self));
    return RESULT_OK(v % 2 != 0 ? Qtrue : Qfalse);
}
static RESULT ab_integer_step(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (UNLIKELY(!FIXNUM_P(self)) || argc < 1 || !FIXNUM_P(argv[0])) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "Integer#step: only Fixnum self/limit supported"));
        return (RESULT){exc, RESULT_RAISE};
    }
    long start = FIX2LONG(self);
    long limit = FIX2LONG(argv[0]);
    long step  = 1;
    if (argc >= 2) {
        if (!FIXNUM_P(argv[1])) {
            VALUE exc = abruby_exception_new(c, c->current_frame,
                abruby_str_new_cstr(c, "Integer#step: only Fixnum step supported"));
            return (RESULT){exc, RESULT_RAISE};
        }
        step = FIX2LONG(argv[1]);
    }
    if (step == 0) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "Integer#step: step can't be zero"));
        return (RESULT){exc, RESULT_RAISE};
    }
    bool has_block = c->current_frame && c->current_frame->block;
    if (has_block) {
        if (step > 0) {
            for (long i = start; i <= limit; i += step) {
                VALUE iv = LONG2FIX(i);
                RESULT r = abruby_yield(c, 1, &iv);
                if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
            }
        } else {
            for (long i = start; i >= limit; i += step) {
                VALUE iv = LONG2FIX(i);
                RESULT r = abruby_yield(c, 1, &iv);
                if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
            }
        }
        return RESULT_OK(self);
    }
    VALUE rb_ary = rb_ary_new();
    if (step > 0) {
        for (long i = start; i <= limit; i += step) rb_ary_push(rb_ary, LONG2FIX(i));
    } else {
        for (long i = start; i >= limit; i += step) rb_ary_push(rb_ary, LONG2FIX(i));
    }
    return RESULT_OK(abruby_ary_new(c, rb_ary));
}

static RESULT ab_integer_aref(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long val = FIX2LONG(self);
        long idx = FIX2LONG(argv[0]);
        if (idx < 0) return RESULT_OK(INT2FIX(0));
        if (idx >= (long)(sizeof(long) * 8)) return RESULT_OK(INT2FIX(val < 0 ? 1 : 0));
        return RESULT_OK(INT2FIX((val >> idx) & 1));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_INT_UNWRAP(argv[0]);
    return RESULT_OK(rb_funcall(rs, rb_intern("[]"), 1, ra));
}

void
Init_abruby_integer(void)
{
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("inspect"), ab_integer_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("to_s"),   ab_integer_to_s,    0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("+"),      ab_integer_add,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("-"),      ab_integer_sub,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("*"),      ab_integer_mul,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("/"),      ab_integer_div,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("%"),      ab_integer_mod,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("**"),     ab_integer_pow,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("-@"),     ab_integer_neg,     0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("<"),      ab_integer_lt,      1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("<="),     ab_integer_le,      1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern(">"),      ab_integer_gt,      1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern(">="),     ab_integer_ge,      1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("=="),     ab_integer_eq,      1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("!="),     ab_integer_neq,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("<=>"),    ab_integer_cmp,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("<<"),     ab_integer_lshift,  1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern(">>"),     ab_integer_rshift,  1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("&"),      ab_integer_band,    1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("|"),      ab_integer_bor,     1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("^"),      ab_integer_bxor,    1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("~"),      ab_integer_bnot,    0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("to_f"),   ab_integer_to_f,    0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("zero?"),  ab_integer_zero_p,  0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("abs"),    ab_integer_abs,     0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("[]"),     ab_integer_aref,    1);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("even?"),  ab_integer_even_p,  0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("odd?"),   ab_integer_odd_p,   0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("times"),  ab_integer_times,   0);
    abruby_class_add_cfunc(ab_tmpl_integer_class, rb_intern("step"),   ab_integer_step,   -1);
}
