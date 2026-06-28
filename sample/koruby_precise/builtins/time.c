/* koruby_precise — time.c: a Time class backed by a double epoch (seconds since
 * 1970).  #included into korb_runtime.c's TU.  Covers the common surface: Time.now
 * / at / utc / gm / local / mktime / new, component accessors, arithmetic,
 * comparison, strftime, to_s.  Sub-second precision is double-limited (no Rational
 * nsec); leap seconds are ignored (POSIX time_t semantics). */
#include <time.h>

/* A Time stores @__t (Float epoch seconds) and @__utc (true = render in UTC). */
static VALUE korb_time_t_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__t", 4)); }
static VALUE korb_time_utc_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__utc", 6)); }

static double korb_time_epoch(CTX *c, VALUE t) {
    const VALUE v = korb_ivar_get(c, t, korb_time_t_sym(c->vm));
    double d = 0; korb_num_to_d(v, &d); return d;
}
static bool korb_time_is_utc(CTX *c, VALUE t) {
    return korb_ivar_get(c, t, korb_time_utc_sym(c->vm)) == KORB_TRUE;
}

/* Build a Time instance for `cls` (Time or a subclass) with the given epoch. */
static RESULT korb_time_make(CTX *c, VALUE *slots, VALUE cls, double epoch, bool utc) {
    slots[0] = UNWRAP(korb_obj_new(c, slots, cls));               /* the Time object (rooted) */
    VALUE_REF tref = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, epoch));       /* @__t (may GC; obj re-read via tref) */
    CHECK(korb_ivar_set(c, slots + 2, tref, korb_time_t_sym(c->vm), slots[1]));
    CHECK(korb_ivar_set(c, slots + 2, tref, korb_time_utc_sym(c->vm), utc ? KORB_TRUE : KORB_FALSE));
    return RESULT_OK(VALUE_REF_GET(tref));
}

/* broken-down time for a Time value (UTC or local per its flag). */
static void korb_time_tm(CTX *c, VALUE t, struct tm *out) {
    const time_t s = (time_t)korb_time_epoch(c, t);
    if (korb_time_is_utc(c, t)) gmtime_r(&s, out); else localtime_r(&s, out);
}

/* ---- Time.<class methods> ------------------------------------------------- */
static RESULT korb_m_time_now(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_time_make(c, slots, VALUE_REF_GET(self), (double)time(NULL), false);
}
static RESULT korb_m_time_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double e = 0;
    const VALUE v = VALUE_SLICE_GET(a, 0);
    if (KORB_OBJECT_P(v) && korb_responds_to(c, v, korb_intern(c->vm, "to_f", 4)) &&
        korb_ivar_get(c, v, korb_time_t_sym(c->vm)) != KORB_NIL)
        e = korb_time_epoch(c, v);                                /* Time.at(time) */
    else if (!korb_num_to_d(v, &e)) {                             /* try #to_int, else TypeError */
        if (korb_responds_to(c, v, korb_intern(c->vm, "to_int", 6))) {
            slots[0] = v;
            RESULT ir = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (!korb_num_to_d(ir.value, &e))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(VALUE_SLICE_GET(a, 0)));
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(v));
    }
    if (VALUE_SLICE_LEN(a) >= 2) {                                /* Time.at(sec, usec) → add microseconds */
        double usec = 0; korb_num_to_d(VALUE_SLICE_GET(a, 1), &usec);
        e += usec / 1e6;
    }
    return korb_time_make(c, slots, VALUE_REF_GET(self), e, false);
}
/* shared: build from (year, mon=1, day=1, hour=0, min=0, sec=0) components. */
static RESULT korb_time_from_parts(CTX *c, VALUE *slots, VALUE cls, VALUE_SLICE a, bool utc) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    intptr_t comp[6] = { 1970, 1, 1, 0, 0, 0 };
    const intptr_t defs = (intptr_t)VALUE_SLICE_LEN(a);
    for (intptr_t i = 0; i < 6 && i < defs; i++) {
        const VALUE cv = VALUE_SLICE_GET(a, i);
        if (FIXNUM_P(cv)) comp[i] = FIX2LONG(cv);
        else if (KORB_FLOAT_P(cv)) comp[i] = (intptr_t)korb_float_val(cv);
    }
    tm.tm_year = (int)comp[0] - 1900; tm.tm_mon = (int)comp[1] - 1; tm.tm_mday = (int)comp[2];
    tm.tm_hour = (int)comp[3]; tm.tm_min = (int)comp[4]; tm.tm_sec = (int)comp[5];
    tm.tm_isdst = -1;
    const time_t e = utc ? timegm(&tm) : mktime(&tm);
    return korb_time_make(c, slots, cls, (double)e, utc);
}
static RESULT korb_m_time_utc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, true);
}
static RESULT korb_m_time_local(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, false);
}
static RESULT korb_m_time_new(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 0) return korb_time_make(c, slots, VALUE_REF_GET(self), (double)time(NULL), false);
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, false);   /* Time.new(y,m,...) → local */
}

/* ---- Time#<instance methods> ---------------------------------------------- */
static RESULT korb_m_time_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(LONG2FIX((intptr_t)korb_time_epoch(c, VALUE_REF_GET(self))));
}
static RESULT korb_m_time_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_float_new(c, slots, korb_time_epoch(c, VALUE_REF_GET(self)));
}
#define TIME_COMPONENT(name, field, adj) \
    static RESULT korb_m_time_##name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        (void)slots; (void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); \
        return RESULT_OK(LONG2FIX(tm.field + (adj))); }
TIME_COMPONENT(year,  tm_year, 1900)
TIME_COMPONENT(mon,   tm_mon,  1)
TIME_COMPONENT(mday,  tm_mday, 0)
TIME_COMPONENT(hour,  tm_hour, 0)
TIME_COMPONENT(min,   tm_min,  0)
TIME_COMPONENT(sec,   tm_sec,  0)
TIME_COMPONENT(wday,  tm_wday, 0)
TIME_COMPONENT(yday,  tm_yday, 1)
static RESULT korb_m_time_usec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const double e = korb_time_epoch(c, VALUE_REF_GET(self));
    /* truncate toward zero (CRuby), with a tiny epsilon to absorb the float
     * reconstruction error for exact integer microseconds. */
    return RESULT_OK(LONG2FIX((intptr_t)((e - (double)(time_t)e) * 1e6 + 1e-6)));
}
static RESULT korb_m_time_utc_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_time_is_utc(c, VALUE_REF_GET(self)) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_time_getutc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE t = VALUE_REF_GET(self);
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t), true);
}
static RESULT korb_m_time_getlocal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE t = VALUE_REF_GET(self);
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t), false);
}
static RESULT korb_m_time_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE t = VALUE_REF_GET(self);
    double d = 0; korb_num_to_d(VALUE_SLICE_GET(a, 0), &d);
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t) + d, korb_time_is_utc(c, t));
}
static RESULT korb_m_time_minus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE t = VALUE_REF_GET(self);
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (KORB_OBJECT_P(o) && korb_ivar_get(c, o, korb_time_t_sym(c->vm)) != KORB_NIL)   /* Time - Time → seconds (Float) */
        return korb_float_new(c, slots, korb_time_epoch(c, t) - korb_time_epoch(c, o));
    double d = 0; korb_num_to_d(o, &d);
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t) - d, korb_time_is_utc(c, t));
}
static RESULT korb_m_time_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o) || korb_ivar_get(c, o, korb_time_t_sym(c->vm)) == KORB_NIL) return RESULT_OK(KORB_NIL);
    const double x = korb_time_epoch(c, VALUE_REF_GET(self)), y = korb_time_epoch(c, o);
    return RESULT_OK(LONG2FIX(x < y ? -1 : x > y ? 1 : 0));
}
static RESULT korb_m_time_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o) || korb_ivar_get(c, o, korb_time_t_sym(c->vm)) == KORB_NIL) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(korb_time_epoch(c, VALUE_REF_GET(self)) == korb_time_epoch(c, o) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_time_strftime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(!KORB_STRING_P(VALUE_SLICE_GET(a, 0))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    char fmt[512]; uint32_t fl = VAL2STR(VALUE_SLICE_GET(a, 0))->len;
    if (fl >= sizeof fmt) fl = sizeof fmt - 1;
    memcpy(fmt, VAL2STR(VALUE_SLICE_GET(a, 0))->buf->data, fl); fmt[fl] = 0;   /* copy off the movable source */
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    char out[1024];
    const size_t n = strftime(out, sizeof out, fmt, &tm);
    return korb_str_new(c, slots, out, (uint32_t)n);
}
static RESULT korb_m_time_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    char out[64];
    const size_t n = strftime(out, sizeof out, korb_time_is_utc(c, VALUE_REF_GET(self)) ? "%Y-%m-%d %H:%M:%S UTC" : "%Y-%m-%d %H:%M:%S %z", &tm);
    return korb_str_new(c, slots, out, (uint32_t)n);
}

void korb_init_time(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "Time", 4), korb_builtin_class_obj(vm, KORB_C_OBJECT))).value;
    korb_const_define(c, korb_intern(vm, "Time", 4), slots[0]);
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;   /* class methods on Time's singleton */
    korb_class_def_cfn(c, slots[1], "now",     korb_m_time_now,   0);
    korb_class_def_cfn(c, slots[1], "at",      korb_m_time_at,    -1);
    korb_class_def_cfn(c, slots[1], "utc",     korb_m_time_utc,   -1);
    korb_class_def_cfn(c, slots[1], "gm",      korb_m_time_utc,   -1);
    korb_class_def_cfn(c, slots[1], "local",   korb_m_time_local, -1);
    korb_class_def_cfn(c, slots[1], "mktime",  korb_m_time_local, -1);
    korb_class_def_cfn(c, slots[1], "new",     korb_m_time_new,   -1);
    const VALUE t = slots[0];
    korb_class_def_cfn(c, t, "to_i",  korb_m_time_to_i,  0);
    korb_class_def_cfn(c, t, "to_f",  korb_m_time_to_f,  0);
    korb_class_def_cfn(c, t, "year",  korb_m_time_year,  0);
    korb_class_def_cfn(c, t, "month", korb_m_time_mon,   0);
    korb_class_def_cfn(c, t, "mon",   korb_m_time_mon,   0);
    korb_class_def_cfn(c, t, "day",   korb_m_time_mday,  0);
    korb_class_def_cfn(c, t, "mday",  korb_m_time_mday,  0);
    korb_class_def_cfn(c, t, "hour",  korb_m_time_hour,  0);
    korb_class_def_cfn(c, t, "min",   korb_m_time_min,   0);
    korb_class_def_cfn(c, t, "sec",   korb_m_time_sec,   0);
    korb_class_def_cfn(c, t, "wday",  korb_m_time_wday,  0);
    korb_class_def_cfn(c, t, "yday",  korb_m_time_yday,  0);
    korb_class_def_cfn(c, t, "usec",  korb_m_time_usec,  0);
    korb_class_def_cfn(c, t, "utc?",  korb_m_time_utc_q, 0);
    korb_class_def_cfn(c, t, "gmt?",  korb_m_time_utc_q, 0);
    korb_class_def_cfn(c, t, "getutc",   korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "getgm",    korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "getlocal", korb_m_time_getlocal, 0);
    korb_class_def_cfn(c, t, "utc",       korb_m_time_getutc,   0);   /* instance utc/gmtime → UTC view */
    korb_class_def_cfn(c, t, "gmtime",    korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "localtime", korb_m_time_getlocal, 0);
    korb_class_def_cfn(c, t, "+",   korb_m_time_plus,  1);
    korb_class_def_cfn(c, t, "-",   korb_m_time_minus, 1);
    korb_class_def_cfn(c, t, "<=>", korb_m_time_cmp,   1);
    korb_class_def_cfn(c, t, "==",  korb_m_time_eq,    1);
    korb_class_def_cfn(c, t, "strftime", korb_m_time_strftime, 1);
    korb_class_def_cfn(c, t, "to_s",     korb_m_time_to_s, 0);
    korb_class_def_cfn(c, t, "inspect",  korb_m_time_to_s, 0);
    /* Comparable derives < <= > >= between/clamp from Time#<=>. */
    const VALUE comp = korb_const_get(vm, korb_intern(vm, "Comparable", 10));
    if (KORB_CLASS_P(comp)) { slots[1] = comp; (void)korb_do_include(c, slots + 2, slots[0], VALUE_SLICE_MAKE(&slots[1], 1)); }
}
