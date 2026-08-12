/* koruby_precise — time.c: a Time class backed by a double epoch (seconds since
 * 1970).  #included into korb_runtime.c's TU.  Covers the common surface: Time.now
 * / at / utc / gm / local / mktime / new, component accessors, arithmetic,
 * comparison, strftime, to_s.  Sub-second precision is double-limited (no Rational
 * nsec); leap seconds are ignored (POSIX time_t semantics). */
#include <time.h>

/* A Time stores @__t (Float epoch seconds) and @__utc (true = render in UTC). */
static VALUE korb_time_t_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__t", 4)); }
static VALUE korb_time_utc_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__utc", 6)); }
static VALUE korb_time_off_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__off", 6)); }

/* If this Time was built with an explicit numeric utc_offset, its value (seconds);
 * false for a plain local/UTC time. */
static bool korb_time_fixed_off(CTX *c, VALUE t, intptr_t *out) {
    const VALUE v = korb_ivar_get(c, t, korb_time_off_sym(c->vm));
    if (!FIXNUM_P(v)) return false;
    *out = FIX2LONG(v); return true;
}
/* Parse a utc_offset argument → seconds: Integer/Float, "±HH[:MM[:SS]]" (colons
 * optional), "UTC"/"Z". Returns false if not parseable here. */
static bool korb_parse_tz_offset(VALUE v, intptr_t *out) {
    if (FIXNUM_P(v))     { *out = FIX2LONG(v);                 return true; }
    if (KORB_FLOAT_P(v)) { *out = (intptr_t)korb_float_val(v); return true; }
    if (KORB_STRING_P(v)) {
        const KorbString *const s = VAL2STR(v);
        const char *const p = korb_strbuf_data(s->buf); const uint32_t n = s->len;
        if ((n == 1 && p[0] == 'Z') || (n == 3 && !memcmp(p, "UTC", 3))) { *out = 0; return true; }
        if (n >= 3 && (p[0] == '+' || p[0] == '-')) {
            const int sign = (p[0] == '-') ? -1 : 1;
            int hh = 0, mm = 0, ss = 0; uint32_t i = 1;
            if (i + 2 > n || !isdigit((unsigned char)p[i]) || !isdigit((unsigned char)p[i+1])) return false;
            hh = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2;
            if (i < n && p[i] == ':') i++;
            if (i + 2 <= n && isdigit((unsigned char)p[i]) && isdigit((unsigned char)p[i+1])) { mm = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2; }
            if (i < n && p[i] == ':') i++;
            if (i + 2 <= n && isdigit((unsigned char)p[i]) && isdigit((unsigned char)p[i+1])) { ss = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2; }
            *out = sign * (hh * 3600 + mm * 60 + ss);
            return true;
        }
    }
    return false;
}

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
/* @__ns: the exact nanosecond sub-second (0..999_999_999), set at construction
 * when it is known precisely — the double @__t epoch cannot hold sub-second
 * precision for present-day timestamps, so sub-second accessors read this. */
static VALUE korb_time_ns_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__ns", 5)); }
static long korb_time_nsec_of(CTX *c, VALUE t) {
    const VALUE nv = korb_ivar_get(c, t, korb_time_ns_sym(c->vm));
    if (FIXNUM_P(nv)) return (long)FIX2LONG(nv);
    const double e = korb_time_epoch(c, t);            /* fallback: reconstruct from the (lossy) epoch */
    long ns = (long)((e - (double)(time_t)e) * 1e9 + 0.5);
    return ns < 0 ? 0 : ns > 999999999 ? 999999999 : ns;
}
/* korb_time_make + an exact nanosecond sub-second (computed from the small
 * fractional part, not the large epoch, so `Rational(100,1000)` → 100_000_000). */
static RESULT korb_time_make_ns(CTX *c, VALUE *slots, VALUE cls, double sec_int, long nsec, bool utc) {
    slots[0] = UNWRAP(korb_time_make(c, slots, cls, sec_int + (double)nsec / 1e9, utc));
    CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), korb_time_ns_sym(c->vm), LONG2FIX((intptr_t)nsec)));
    return RESULT_OK(slots[0]);
}

/* broken-down time for a Time value (UTC or local per its flag). */
static void korb_time_tm(CTX *c, VALUE t, struct tm *out) {
    intptr_t off;
    if (korb_time_fixed_off(c, t, &off)) {          /* fixed-offset time: wall clock = UTC(epoch + off) */
        const time_t s = (time_t)(korb_time_epoch(c, t) + off);
        gmtime_r(&s, out);
        out->tm_gmtoff = off; out->tm_isdst = 0;
        return;
    }
    const time_t s = (time_t)korb_time_epoch(c, t);
    if (korb_time_is_utc(c, t)) gmtime_r(&s, out); else localtime_r(&s, out);
}

/* ---- Time.<class methods> ------------------------------------------------- */
static RESULT korb_m_time_now(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_time_make(c, slots, VALUE_REF_GET(self), (double)time(NULL), false);
}
static RESULT korb_m_time_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double e = 0;
    VALUE v = VALUE_SLICE_GET(a, 0);
    if (KORB_OBJECT_P(v) && korb_responds_to(c, v, korb_intern(c->vm, "to_f", 4)) &&
        korb_ivar_get(c, v, korb_time_t_sym(c->vm)) != KORB_NIL)
        e = korb_time_epoch(c, v);                                /* Time.at(time) */
    else if (!korb_num_to_d(v, &e)) {                             /* try #to_int, else TypeError */
        if (korb_responds_to_coerce_p(c, slots, &v, korb_intern(c->vm, "to_int", 6))) {
            slots[0] = v;
            RESULT ir = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (!korb_num_to_d(ir.value, &e))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(VALUE_SLICE_GET(a, 0)));
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(v));
    }
    const bool first_is_time = KORB_OBJECT_P(v) && korb_ivar_get(c, v, korb_time_t_sym(c->vm)) != KORB_NIL;
    int64_t sec = (int64_t)floor(e);
    long nsec = (long)((e - floor(e)) * 1e9 + 0.5);              /* sub-second from a fractional sec arg */
    if (VALUE_SLICE_LEN(a) >= 2 && !(VALUE_SLICE_LEN(a) == 2 && KORB_HASH_P(VALUE_SLICE_GET(a, 1)))) {   /* Time.at(sec, subsec[, unit]); a lone trailing Hash is kwargs */
        if (first_is_time)                                       /* Time.at(time, subsec) is a TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert Time into an exact number");
        const VALUE sv2 = VALUE_SLICE_GET(a, 1);
        double subv;
        if (!korb_num_to_d(sv2, &subv))                          /* nil / String / non-Numeric → TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(sv2));
        double per_ns = 1000.0;                                  /* default unit = :microsecond */
        if (VALUE_SLICE_LEN(a) >= 3 && VALUE_SLICE_GET(a, 2) != KORB_NIL) {
            const VALUE u = VALUE_SLICE_GET(a, 2);
            const char *un = SYMBOL_P(u) ? korb_sym_name(c->vm, SYM2ID(u)) : (KORB_STRING_P(u) ? korb_strbuf_data(VAL2STR(u)->buf) : "");
            if (!strcmp(un, "millisecond")) per_ns = 1e6;
            else if (!strcmp(un, "microsecond") || !strcmp(un, "usec")) per_ns = 1000.0;
            else if (!strcmp(un, "nanosecond") || !strcmp(un, "nsec")) per_ns = 1.0;
            else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unexpected unit: %s", un);
        }
        nsec += (long)(subv * per_ns + 0.5);
    }
    sec += nsec / 1000000000; nsec %= 1000000000;                /* carry overflow into seconds */
    return korb_time_make_ns(c, slots, VALUE_REF_GET(self), (double)sec, nsec, false);
}
/* shared: build from (year, mon=1, day=1, hour=0, min=0, sec=0) components. */
/* Parse a Time.new component String to an integer.  For the month a 3-letter
 * English name (jan..dec, any case) is also accepted. */
static bool korb_parse_time_str(const char *s, uint32_t len, bool is_month, intptr_t *out) {
    static const char *const mon[12] = { "jan","feb","mar","apr","may","jun",
                                         "jul","aug","sep","oct","nov","dec" };
    if (is_month && len >= 3) {
        char lo[3];
        for (int k = 0; k < 3; k++) lo[k] = (char)tolower((unsigned char)s[k]);
        for (int m = 0; m < 12; m++)
            if (lo[0] == mon[m][0] && lo[1] == mon[m][1] && lo[2] == mon[m][2]) { *out = m + 1; return true; }
    }
    char buf[64]; if (len >= sizeof buf) return false;
    memcpy(buf, s, len); buf[len] = '\0';
    char *end; const long v = strtol(buf, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (end == buf || *end != '\0') return false;
    *out = (intptr_t)v; return true;
}

static RESULT korb_time_from_parts(CTX *c, VALUE *slots, VALUE cls, VALUE_SLICE a, bool utc) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    intptr_t comp[6] = { 1970, 1, 1, 0, 0, 0 };
    double subsec = 0.0;                                     /* fractional seconds (Float/Rational sec arg) */
    const intptr_t defs = (intptr_t)VALUE_SLICE_LEN(a);
    slots[0] = cls;                                          /* root cls across #to_int/#to_str dispatch */
    for (intptr_t i = 0; i < 6 && i < defs; i++) {
        VALUE cv = VALUE_SLICE_GET(a, i);
        if (i == 5 && !FIXNUM_P(cv) && (KORB_FLOAT_P(cv) || KORB_RATIONAL_P(cv))) {  /* fractional seconds */
            double sv = 0; if (korb_num_to_d(cv, &sv)) { comp[5] = (intptr_t)sv; subsec = sv - (double)comp[5]; }
            continue;
        }
        if (cv == KORB_NIL) {                                /* nil year is a TypeError; later nils keep the default */
            if (i == 0) return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion from nil to integer");
            continue;
        }
        if (FIXNUM_P(cv)) { comp[i] = FIX2LONG(cv); continue; }
        if (KORB_FLOAT_P(cv)) { comp[i] = (intptr_t)korb_float_val(cv); continue; }
        if (!KORB_STRING_P(cv)) {                            /* coerce: #to_int, else #to_str → parse */
            if (korb_responds_to(c, cv, korb_intern(c->vm, "to_int", 6))) {
                slots[1] = cv;
                RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                if (FIXNUM_P(r.value)) { comp[i] = FIX2LONG(r.value); cls = slots[0]; continue; }
                if (KORB_FLOAT_P(r.value)) { comp[i] = (intptr_t)korb_float_val(r.value); cls = slots[0]; continue; }
            }
            if (korb_responds_to(c, cv, korb_intern(c->vm, "to_str", 6))) {
                slots[1] = cv;
                RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                cls = slots[0];
                if (KORB_STRING_P(r.value)) cv = r.value;
                else return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, i)));
            } else return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(cv));
        }
        if (KORB_STRING_P(cv)) {                             /* String component → parse (month accepts names) */
            const KorbString *cs = VAL2STR(cv);
            if (!korb_parse_time_str(korb_strbuf_data(cs->buf), cs->len, i == 1, &comp[i]))
                return korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "argument out of range");
        }
    }
    if (defs >= 7) {                                         /* Time.utc/local 7th arg = microseconds */
        const VALUE uv = VALUE_SLICE_GET(a, 6);
        double us = 0;
        if (FIXNUM_P(uv)) us = (double)FIX2LONG(uv);
        else if (KORB_FLOAT_P(uv)) us = korb_float_val(uv);
        else korb_num_to_d(uv, &us);
        subsec += us / 1e6;
    }
    cls = slots[0];                                          /* re-read after any GC move */
    /* CRuby raises ArgumentError for out-of-range components (no mktime rollover). */
    if (comp[1] < 1 || comp[1] > 12 || comp[2] < 1 || comp[2] > 31 ||
        comp[3] < 0 || comp[3] > 24 || comp[4] < 0 || comp[4] > 59 || comp[5] < 0 || comp[5] > 60)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument out of range");
    tm.tm_year = (int)comp[0] - 1900; tm.tm_mon = (int)comp[1] - 1; tm.tm_mday = (int)comp[2];
    tm.tm_hour = (int)comp[3]; tm.tm_min = (int)comp[4]; tm.tm_sec = (int)comp[5];
    tm.tm_isdst = -1;
    const time_t e = utc ? timegm(&tm) : mktime(&tm);
    /* exact nsec from the small fractional part (accurate), not the large epoch. */
    long nsec = (long)(subsec * 1e9 + 0.5);
    if (nsec < 0) nsec = 0; else if (nsec > 999999999) nsec = 999999999;
    return korb_time_make_ns(c, slots, cls, (double)e, nsec, utc);
}
static RESULT korb_m_time_utc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, true);
}
static RESULT korb_m_time_local(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, false);
}
static RESULT korb_m_time_new(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 0) return korb_time_make(c, slots, VALUE_REF_GET(self), (double)time(NULL), false);
    /* Time.new(y,m,d,h,min,s, utc_offset): the 7th arg fixes the zone offset; the
     * components are wall-clock in that offset, so epoch = timegm(components) - off. */
    if (VALUE_SLICE_LEN(a) >= 7 && VALUE_SLICE_GET(a, 6) != KORB_NIL) {
        VALUE offv = VALUE_SLICE_GET(a, 6);
        intptr_t off;
        if (!korb_parse_tz_offset(offv, &off)) {                 /* try #to_int */
            if (KORB_OBJECT_P(offv) && korb_responds_to_coerce_p(c, slots, &offv, korb_intern(c->vm, "to_int", 6))) {
                slots[0] = offv;
                RESULT ir = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
                if (!FIXNUM_P(ir.value)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "\"+HH:MM\", \"-HH:MM\", UTC or utc_offset expected");
                off = FIX2LONG(ir.value);
            } else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "\"+HH:MM\", \"-HH:MM\", UTC or utc_offset expected");
        }
        if (off <= -86400 || off >= 86400) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "utc_offset out of range");
        struct tm tm; memset(&tm, 0, sizeof tm);
        intptr_t comp[6] = { 1970, 1, 1, 0, 0, 0 };
        for (uint32_t i = 0; i < 6 && i < VALUE_SLICE_LEN(a); i++) {
            const VALUE cv = VALUE_SLICE_GET(a, i);
            if (FIXNUM_P(cv)) comp[i] = FIX2LONG(cv);
            else if (KORB_FLOAT_P(cv)) comp[i] = (intptr_t)korb_float_val(cv);
        }
        if (comp[1] < 1 || comp[1] > 12 || comp[2] < 1 || comp[2] > 31 ||
            comp[3] < 0 || comp[3] > 24 || comp[4] < 0 || comp[4] > 59 || comp[5] < 0 || comp[5] > 60)
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument out of range");
        tm.tm_year = (int)comp[0] - 1900; tm.tm_mon = (int)comp[1] - 1; tm.tm_mday = (int)comp[2];
        tm.tm_hour = (int)comp[3]; tm.tm_min = (int)comp[4]; tm.tm_sec = (int)comp[5];
        const double e = (double)timegm(&tm) - (double)off;
        slots[0] = UNWRAP(korb_time_make(c, slots, VALUE_REF_GET(self), e, false));   /* the Time (rooted) */
        (void)korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), korb_time_off_sym(c->vm), LONG2FIX(off));
        return RESULT_OK(slots[0]);
    }
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
    return RESULT_OK(LONG2FIX(korb_time_nsec_of(c, VALUE_REF_GET(self)) / 1000));   /* microseconds (truncated) */
}
/* Time#nsec / #tv_nsec — the nanosecond part (0..999_999_999). */
static RESULT korb_m_time_nsec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(LONG2FIX(korb_time_nsec_of(c, VALUE_REF_GET(self))));
}
/* Time#subsec → the fractional second as a Rational (nsec/1e9), or 0. */
static RESULT korb_m_time_subsec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const long ns = korb_time_nsec_of(c, VALUE_REF_GET(self));
    return ns == 0 ? RESULT_OK(LONG2FIX(0)) : korb_rat_new(c, slots, ns, 1000000000);
}
/* Time#to_r → the epoch as a Rational: seconds + nsec/1e9. */
static RESULT korb_m_time_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE t = VALUE_REF_GET(self);
    const int64_t sec = (int64_t)korb_time_epoch(c, t);
    return korb_rat_new(c, slots, (intptr_t)(sec * 1000000000 + korb_time_nsec_of(c, t)), 1000000000);
}
static RESULT korb_m_time_utc_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_time_is_utc(c, VALUE_REF_GET(self)) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_time_getutc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE t = VALUE_REF_GET(self);
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t), true);
}
/* getlocal / localtime: with no argument the process time zone renders the
 * instant; with a utc_offset ("+09:00" or seconds) the result carries that fixed
 * offset instead, the same representation Time.new(..., utc_offset) produces. */
static RESULT korb_m_time_getlocal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE t = VALUE_REF_GET(self);
    const double e = korb_time_epoch(c, t);
    const VALUE cls = korb_class_obj_of(c, t);
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        intptr_t off;
        if (!korb_parse_tz_offset(VALUE_SLICE_GET(a, 0), &off)) {
            /* Rational / #to_int / #to_str offsets — anything that reduces to
             * seconds or to the "+HH:MM" string form. */
            VALUE offv = VALUE_SLICE_GET(a, 0);
            static const struct { const char *nm; uint32_t len; } conv[] = { { "to_int", 6 }, { "to_str", 6 }, { "to_r", 4 } };
            bool got = false;
            for (size_t ci = 0; ci < sizeof conv / sizeof conv[0] && !got; ci++) {
                const uint32_t mid = korb_intern(c->vm, conv[ci].nm, conv[ci].len);
                if (!korb_responds_to(c, offv, mid)) continue;
                slots[0] = offv;
                const RESULT ir = korb_send_impl(c, slots + 1, mid, 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
                if (KORB_RATIONAL_P(ir.value)) {           /* Rational seconds → truncate */
                    slots[0] = ir.value;
                    const RESULT tr = korb_send(c, slots + 1, korb_intern(c->vm, "to_i", 4), 0, 0);
                    if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
                    got = korb_parse_tz_offset(tr.value, &off);
                } else {
                    got = korb_parse_tz_offset(ir.value, &off);
                }
            }
            if (!got)
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "\"+HH:MM\", \"-HH:MM\", UTC or utc_offset expected");
        }
        if (off <= -86400 || off >= 86400) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "utc_offset out of range");
        slots[0] = UNWRAP(korb_time_make(c, slots, cls, e, false));
        (void)korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), korb_time_off_sym(c->vm), LONG2FIX(off));
        return RESULT_OK(slots[0]);
    }
    return korb_time_make(c, slots, cls, e, false);
}
static RESULT korb_m_time_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE t = VALUE_REF_GET(self);
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(KORB_OBJECT_P(o) && korb_ivar_get(c, o, korb_time_t_sym(c->vm)) != KORB_NIL))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "time + time?");   /* Time + Time is invalid */
    double d = 0;
    if (UNLIKELY(!korb_num_to_d(o, &d)))                              /* String / non-Numeric → TypeError */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(o));
    return korb_time_make(c, slots, korb_class_obj_of(c, t), korb_time_epoch(c, t) + d, korb_time_is_utc(c, t));
}
static RESULT korb_m_time_minus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE t = VALUE_REF_GET(self);
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (KORB_OBJECT_P(o) && korb_ivar_get(c, o, korb_time_t_sym(c->vm)) != KORB_NIL)   /* Time - Time → seconds (Float) */
        return korb_float_new(c, slots, korb_time_epoch(c, t) - korb_time_epoch(c, o));
    double d = 0;
    if (UNLIKELY(!korb_num_to_d(o, &d)))                              /* String / non-Numeric → TypeError */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(o));
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
    memcpy(fmt, korb_strbuf_data(VAL2STR(VALUE_SLICE_GET(a, 0))->buf), fl); fmt[fl] = 0;   /* copy off the movable source */
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    const bool is_utc = korb_time_is_utc(c, VALUE_REF_GET(self));
    if (is_utc) tm.tm_zone = "UTC";                          /* CRuby reports "UTC" (%Z), not libc's "GMT" */
    /* Nanosecond part, for the Ruby-only %L / %N sub-second directives. */
    const long nsec = korb_time_nsec_of(c, VALUE_REF_GET(self));
    const long gmtoff = tm.tm_gmtoff;
    /* Pre-expand the directives libc strftime does not know (%L, %[w]N, %:z /
     * %::z, %v); everything else is copied verbatim for the strftime call. */
    char efmt[1024]; size_t o = 0;
    for (uint32_t i = 0; i < fl && o + 32 < sizeof efmt; i++) {
        if (fmt[i] != '%') { efmt[o++] = fmt[i]; continue; }
        const uint32_t pct = i; i++;
        while (i < fl && strchr("-_0^#", fmt[i])) i++;        /* flags */
        int ncolon = 0; while (i < fl && fmt[i] == ':') { ncolon++; i++; }   /* %:z colons */
        int width = -1; { int w = 0; bool any = false; while (i < fl && isdigit((unsigned char)fmt[i])) { w = w*10 + (fmt[i]-'0'); any = true; i++; } if (any) width = w; }
        if (i >= fl) { efmt[o++] = '%'; break; }
        const char spec = fmt[i];
        if (spec == 'N' || spec == 'L') {                    /* fractional seconds */
            char nb[16]; snprintf(nb, sizeof nb, "%09ld", nsec);   /* 9 digits */
            int wd = width >= 0 ? width : (spec == 'L' ? 3 : 9);
            for (int k = 0; k < wd && o + 1 < sizeof efmt; k++) efmt[o++] = (k < 9) ? nb[k] : '0';   /* truncate / right-pad */
        } else if (spec == 'z' && ncolon > 0) {              /* %:z / %::z offset with colons */
            long ao = gmtoff < 0 ? -gmtoff : gmtoff; int hh = (int)(ao/3600), mm = (int)((ao%3600)/60), ss = (int)(ao%60);
            char zb[16];
            if (ncolon >= 2) snprintf(zb, sizeof zb, "%c%02d:%02d:%02d", gmtoff < 0 ? '-' : '+', hh, mm, ss);
            else             snprintf(zb, sizeof zb, "%c%02d:%02d", gmtoff < 0 ? '-' : '+', hh, mm);
            for (const char *p = zb; *p && o + 1 < sizeof efmt; p++) efmt[o++] = *p;
        } else if (spec == 'v') {                            /* VMS date: " 3-FEB-2001" */
            for (const char *p = "%e-%^b-%Y"; *p && o + 1 < sizeof efmt; p++) efmt[o++] = *p;
        } else {                                             /* copy the whole directive verbatim for strftime */
            for (uint32_t k = pct; k <= i && o + 1 < sizeof efmt; k++) efmt[o++] = fmt[k];
        }
    }
    efmt[o] = 0;
    char out[1024];
    const size_t n = strftime(out, sizeof out, efmt, &tm);
    return korb_str_new(c, slots, out, (uint32_t)n);
}
/* asctime / ctime → the fixed C-locale "Www Mmm dd hh:mm:ss yyyy" form (day
 * space-padded to width 2, no trailing newline).  ctime is the local-time view. */
static RESULT korb_m_time_asctime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    char out[64];
    const size_t n = strftime(out, sizeof out, "%a %b %e %H:%M:%S %Y", &tm);
    return korb_str_new(c, slots, out, (uint32_t)n);
}
static RESULT korb_m_time_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    char out[64];
    const size_t n = strftime(out, sizeof out, korb_time_is_utc(c, VALUE_REF_GET(self)) ? "%Y-%m-%d %H:%M:%S UTC" : "%Y-%m-%d %H:%M:%S %z", &tm);
    return korb_str_new(c, slots, out, (uint32_t)n);
}

/* day-of-week predicates. */
static RESULT korb_m_time_sunday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==0?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_monday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==1?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_tuesday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==2?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_wednesday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==3?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_thursday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==4?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_friday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==5?KORB_TRUE:KORB_FALSE); }
static RESULT korb_m_time_saturday(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)slots;(void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm); return RESULT_OK(tm.tm_wday==6?KORB_TRUE:KORB_FALSE); }
/* Time#to_a → [sec, min, hour, mday, mon, year, wday, yday, isdst, zone]. */
static RESULT korb_m_time_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);   /* tm is a stable C struct across the allocs below */
    const bool utc = korb_time_is_utc(c, VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_ary_new(c, slots, 10));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    const intptr_t vals[8] = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1,
                               tm.tm_year + 1900, tm.tm_wday, tm.tm_yday + 1 };
    for (int i = 0; i < 8; i++) CHECK(korb_ary_push_val(c, slots + 1, arr, LONG2FIX(vals[i])));
    CHECK(korb_ary_push_val(c, slots + 1, arr, tm.tm_isdst > 0 ? KORB_TRUE : KORB_FALSE));
    const char *zone = utc ? "UTC" : (tm.tm_zone ? tm.tm_zone : "");
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, zone, (uint32_t)strlen(zone)));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}

static RESULT korb_m_time_zone(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    const char *z = korb_time_is_utc(c, VALUE_REF_GET(self)) ? "UTC" : (tm.tm_zone ? tm.tm_zone : "");
    return korb_str_new(c, slots, z, (uint32_t)strlen(z));
}
static RESULT korb_m_time_utc_offset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    intptr_t off;
    if (korb_time_fixed_off(c, VALUE_REF_GET(self), &off)) return RESULT_OK(LONG2FIX(off));   /* explicit utc_offset */
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    return RESULT_OK(LONG2FIX(korb_time_is_utc(c, VALUE_REF_GET(self)) ? 0 : (intptr_t)tm.tm_gmtoff));
}
static RESULT korb_m_time_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const double e = korb_time_epoch(c, VALUE_REF_GET(self));
    const int nd = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    const double scale = pow(10.0, nd);
    const double r = round(e * scale) / scale;
    return korb_time_make(c, slots, korb_const_get(c->vm, korb_intern(c->vm, "Time", 4)), r, korb_time_is_utc(c, VALUE_REF_GET(self)));
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
    korb_class_def_cfn(c, t, "tv_usec", korb_m_time_usec, 0);
    korb_class_def_cfn(c, t, "nsec",  korb_m_time_nsec,  0);
    korb_class_def_cfn(c, t, "tv_nsec", korb_m_time_nsec, 0);
    korb_class_def_cfn(c, t, "subsec", korb_m_time_subsec, 0);
    korb_class_def_cfn(c, t, "to_r",  korb_m_time_to_r,  0);
    korb_class_def_cfn(c, t, "tv_sec", korb_m_time_to_i, 0);
    korb_class_def_cfn(c, t, "to_a",  korb_m_time_to_a,  0);
    korb_class_def_cfn(c, t, "zone",       korb_m_time_zone,       0);
    korb_class_def_cfn(c, t, "utc_offset", korb_m_time_utc_offset, 0);
    korb_class_def_cfn(c, t, "gmt_offset", korb_m_time_utc_offset, 0);
    korb_class_def_cfn(c, t, "gmtoff",     korb_m_time_utc_offset, 0);
    korb_class_def_cfn(c, t, "getutc",     korb_m_time_getutc,     0);
    korb_class_def_cfn(c, t, "getgm",      korb_m_time_getutc,     0);
    korb_class_def_cfn(c, t, "getlocal",   korb_m_time_getlocal,   -1);
    korb_class_def_cfn(c, t, "round",      korb_m_time_round,      -1);
    korb_class_def_cfn(c, t, "sunday?",    korb_m_time_sunday,    0);
    korb_class_def_cfn(c, t, "monday?",    korb_m_time_monday,    0);
    korb_class_def_cfn(c, t, "tuesday?",   korb_m_time_tuesday,   0);
    korb_class_def_cfn(c, t, "wednesday?", korb_m_time_wednesday, 0);
    korb_class_def_cfn(c, t, "thursday?",  korb_m_time_thursday,  0);
    korb_class_def_cfn(c, t, "friday?",    korb_m_time_friday,    0);
    korb_class_def_cfn(c, t, "saturday?",  korb_m_time_saturday,  0);
    korb_class_def_cfn(c, t, "utc?",  korb_m_time_utc_q, 0);
    korb_class_def_cfn(c, t, "gmt?",  korb_m_time_utc_q, 0);
    korb_class_def_cfn(c, t, "getutc",   korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "getgm",    korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "getlocal", korb_m_time_getlocal, -1);
    korb_class_def_cfn(c, t, "utc",       korb_m_time_getutc,   0);   /* instance utc/gmtime → UTC view */
    korb_class_def_cfn(c, t, "gmtime",    korb_m_time_getutc,   0);
    korb_class_def_cfn(c, t, "localtime", korb_m_time_getlocal, -1);
    korb_class_def_cfn(c, t, "+",   korb_m_time_plus,  1);
    korb_class_def_cfn(c, t, "-",   korb_m_time_minus, 1);
    korb_class_def_cfn(c, t, "<=>", korb_m_time_cmp,   1);
    korb_class_def_cfn(c, t, "==",  korb_m_time_eq,    1);
    korb_class_def_cfn(c, t, "eql?", korb_m_time_eq,   1);
    korb_class_def_cfn(c, t, "strftime", korb_m_time_strftime, 1);
    korb_class_def_cfn(c, t, "asctime",  korb_m_time_asctime, 0);
    korb_class_def_cfn(c, t, "ctime",    korb_m_time_asctime, 0);
    korb_class_def_cfn(c, t, "to_s",     korb_m_time_to_s, 0);
    korb_class_def_cfn(c, t, "inspect",  korb_m_time_to_s, 0);
    /* Comparable derives < <= > >= between/clamp from Time#<=>. */
    const VALUE comp = korb_const_get(vm, korb_intern(vm, "Comparable", 10));
    if (KORB_CLASS_P(comp)) { slots[1] = comp; (void)korb_do_include(c, slots + 2, slots[0], VALUE_SLICE_MAKE(&slots[1], 1)); }
}
