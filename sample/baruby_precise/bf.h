

static inline VALUE
barb_p(CTX *c, VALUE v)
{
    baruby_print_value(c, stdout, v);
    fputc('\n', stdout);
    return v;
}

static inline VALUE
barb_zero(CTX *c, VALUE v)
{
    (void)c;
    (void)v;
    return INT2VAL(0);
}

static inline VALUE
barb_add(CTX *c, VALUE a, VALUE b)
{
    (void)c;
    return INT2VAL(VAL2INT(a) + VAL2INT(b));
}
