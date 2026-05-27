/* Boolean (TrueClass / FalseClass / NilClass) — moved from builtins.c. */

/* ---------- Boolean ---------- */
static VALUE true_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("true"); }
static VALUE false_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("false"); }
static VALUE nil_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr(""); }
static VALUE nil_inspect(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("nil"); }

/* Boolean and / or / xor (Kernel#&|^ on true/false/nil).  CRuby:
 *   true & x  → x is truthy
 *   false & x → false (x not even called for side effects? actually it is)
 *   nil & x   → false
 *   true | x  → true
 *   false | x → x is truthy
 *   nil | x   → x is truthy
 *   true ^ x  → !truthy(x)
 *   false ^ x → truthy(x)
 *   nil ^ x   → truthy(x) */
static bool korb_truthy(VALUE v) { return !NIL_P(v) && v != Qfalse; }
static VALUE true_and(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_truthy(argv[0]));
}
static VALUE true_or(CTX *c, VALUE self, int argc, VALUE *argv) { return Qtrue; }
static VALUE true_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(!korb_truthy(argv[0]));
}
static VALUE false_and(CTX *c, VALUE self, int argc, VALUE *argv) { return Qfalse; }
static VALUE false_or(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_truthy(argv[0]));
}
static VALUE false_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_truthy(argv[0]));
}
static VALUE nil_and(CTX *c, VALUE self, int argc, VALUE *argv) { return Qfalse; }
static VALUE nil_or(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_truthy(argv[0]));
}
static VALUE nil_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_truthy(argv[0]));
}
static VALUE nil_to_a(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_ary_new(); }
static VALUE nil_to_h(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_hash_new(); }
static VALUE nil_to_f(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_float_new(0.0); }
static VALUE nil_to_i(CTX *c, VALUE self, int argc, VALUE *argv) { return INT2FIX(0); }
static VALUE nil_nil_p(CTX *c, VALUE self, int argc, VALUE *argv) { return Qtrue; }

