/* Symbol — moved from builtins.c.  Includes Symbol#to_proc shim. */

/* ---------- Symbol#to_proc ---------- */
static VALUE sym_to_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Allocate a marker Proc whose body is NULL and whose `self` is the
     * Symbol value.  korb_yield/proc_call detect (body==NULL && SYMBOL_P(self))
     * and dispatch as `arg.send(sym, *rest)`. */
    struct korb_proc *p = korb_xcalloc(1, sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = (VALUE)korb_vm->proc_class;
    p->body = NULL;
    p->env = NULL;
    p->env_size = 0;
    p->params_cnt = 1;
    p->param_base = 0;
    p->self = self;            /* the symbol itself */
    p->is_lambda = false;
    return (VALUE)p;
}

/* (range ext folded into builtins/range.c) */
/* (integer ext folded into builtins/integer.c) */
/* (float ext folded into builtins/float.c) */

/* ---------- Symbol ---------- */
static VALUE sym_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(korb_id_name(korb_sym2id(self)));
}
static VALUE sym_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(self == argv[0]);
}

