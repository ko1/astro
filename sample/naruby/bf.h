

static inline VALUE
narb_p(VALUE v)
{
    printf("p:%ld\n", v);
    return v;
}

static inline VALUE
narb_zero(VALUE v)
{
    return 0;
}

static inline VALUE
narb_add(VALUE a, VALUE b)
{
    return a + b;
}
