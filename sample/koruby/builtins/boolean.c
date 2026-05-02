/* Boolean (TrueClass / FalseClass / NilClass) — moved from builtins.c. */

/* ---------- Boolean ---------- */
static VALUE true_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("true"); }
static VALUE false_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("false"); }
static VALUE nil_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr(""); }
static VALUE nil_inspect(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("nil"); }

