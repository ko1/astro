

static inline VALUE
barb_p(VALUE v)
{
    baruby_print_value(stdout, v);
    fputc('\n', stdout);
    return v;
}

static inline VALUE
barb_zero(VALUE v)
{
    (void)v;
    return INT2VAL(0);
}

static inline VALUE
barb_add(VALUE a, VALUE b)
{
    return INT2VAL(VAL2INT(a) + VAL2INT(b));
}
