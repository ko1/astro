/* Boolean (TrueClass / FalseClass / NilClass) — sp/RESULT ABI.
 *
 * Convention: argc-0 cfunc has self at sp[-1].  argc-1 cfunc has self
 * at sp[-2], arg0 at sp[-1]. */

/* ---------- Boolean ---------- */
/* CRuby semantics: true.to_s / false.to_s / nil.to_s return the same
 * frozen string each call.  Cached on korb_vm; no allocation. */
static RESULT true_to_s(CTX *c, int argc, VALUE *sp)  { (void)sp; return RESULT_OK(korb_vm->frozen_true_str); }
static RESULT false_to_s(CTX *c, int argc, VALUE *sp) { (void)sp; return RESULT_OK(korb_vm->frozen_false_str); }
static RESULT nil_to_s(CTX *c, int argc, VALUE *sp)   { (void)sp; return RESULT_OK(korb_vm->frozen_nil_str); }
static RESULT nil_inspect(CTX *c, int argc, VALUE *sp) { return RESULT_OK(korb_str_new_cstr(c, sp, "nil")); }

/* Boolean and / or / xor (Kernel#&|^ on true/false/nil). */
static bool korb_truthy(VALUE v) { return !NIL_P(v) && v != Qfalse; }
static RESULT true_and(CTX *c, int argc, VALUE *sp)  { return RESULT_OK(KORB_BOOL(korb_truthy(sp[-1]))); }
static RESULT true_or(CTX *c, int argc, VALUE *sp)   { return RESULT_OK(Qtrue); }
static RESULT true_xor(CTX *c, int argc, VALUE *sp)  { return RESULT_OK(KORB_BOOL(!korb_truthy(sp[-1]))); }
static RESULT false_and(CTX *c, int argc, VALUE *sp) { return RESULT_OK(Qfalse); }
static RESULT false_or(CTX *c, int argc, VALUE *sp)  { return RESULT_OK(KORB_BOOL(korb_truthy(sp[-1]))); }
static RESULT false_xor(CTX *c, int argc, VALUE *sp) { return RESULT_OK(KORB_BOOL(korb_truthy(sp[-1]))); }
static RESULT nil_and(CTX *c, int argc, VALUE *sp)   { return RESULT_OK(Qfalse); }
static RESULT nil_or(CTX *c, int argc, VALUE *sp)    { return RESULT_OK(KORB_BOOL(korb_truthy(sp[-1]))); }
static RESULT nil_xor(CTX *c, int argc, VALUE *sp)   { return RESULT_OK(KORB_BOOL(korb_truthy(sp[-1]))); }
static RESULT nil_to_a(CTX *c, int argc, VALUE *sp)  { c->sp = sp; return RESULT_OK(korb_ary_new()); }
static RESULT nil_to_h(CTX *c, int argc, VALUE *sp)  { c->sp = sp; return RESULT_OK(korb_hash_new()); }
static RESULT nil_to_f(CTX *c, int argc, VALUE *sp)  { return RESULT_OK(korb_float_new(c, sp, 0.0)); }
static RESULT nil_to_i(CTX *c, int argc, VALUE *sp)  { return RESULT_OK(INT2FIX(0)); }
static RESULT nil_nil_p(CTX *c, int argc, VALUE *sp) { return RESULT_OK(Qtrue); }
