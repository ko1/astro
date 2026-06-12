// Builtin functions (M0).  Builtins return plain VALUE (no RESULT) so
// they must not raise; raising operations live in node.def / node.c
// where the RESULT channel is available.

static inline VALUE
korb_p(CTX *c, VALUE v)
{
    koruby_print_value(c, stdout, v);    /* inspect format */
    fputc('\n', stdout);
    return v;
}

static inline VALUE
korb_puts(CTX *c, VALUE v)
{
    koruby_puts_value(c, stdout, v);     /* CRuby puts semantics */
    return VAL_NIL;
}

static inline VALUE
korb_print(CTX *c, VALUE v)
{
    koruby_print_value_tos(c, stdout, v);  /* to_s, no newline */
    return VAL_NIL;
}