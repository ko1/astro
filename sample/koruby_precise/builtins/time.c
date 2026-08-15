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
 * optional), "UTC"/"Z". Returns false if not parseable here.  *utcish reports
 * the forms CRuby treats as UTC itself rather than as a zero offset — "UTC",
 * "Z" and "-00:00" (but not "+00:00", and not a numeric 0). */
static bool korb_parse_tz_offset(VALUE v, intptr_t *out, bool *utcish) {
    if (utcish) *utcish = false;
    if (FIXNUM_P(v))     { *out = FIX2LONG(v);                 return true; }
    if (KORB_FLOAT_P(v)) { *out = (intptr_t)korb_float_val(v); return true; }
    if (KORB_STRING_P(v)) {
        const KorbString *const s = VAL2STR(v);
        const char *const p = korb_strbuf_data(s->buf); const uint32_t n = s->len;
        if ((n == 1 && p[0] == 'Z') || (n == 3 && !memcmp(p, "UTC", 3))) { *out = 0; if (utcish) *utcish = true; return true; }
        if (n == 1) {   /* military zone letters: A..I = +1..+9, K..M = +10..+12,
                         * N..Y = -1..-12 (J is deliberately unassigned) */
            const char z = p[0];
            if (z >= 'A' && z <= 'I') { *out = (intptr_t)(z - 'A' + 1) * 3600; return true; }
            if (z >= 'K' && z <= 'M') { *out = (intptr_t)(z - 'K' + 10) * 3600; return true; }
            if (z >= 'N' && z <= 'Y') { *out = -(intptr_t)(z - 'N' + 1) * 3600; return true; }
            return false;
        }
        if (n >= 3 && (p[0] == '+' || p[0] == '-')) {
            const int sign = (p[0] == '-') ? -1 : 1;
            int hh = 0, mm = 0, ss = 0; uint32_t i = 1;
            if (i + 2 > n || !isdigit((unsigned char)p[i]) || !isdigit((unsigned char)p[i+1])) return false;
            hh = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2;
            if (i < n && p[i] == ':') i++;
            if (i + 2 <= n && isdigit((unsigned char)p[i]) && isdigit((unsigned char)p[i+1])) { mm = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2; }
            if (i < n && p[i] == ':') i++;
            if (i + 2 <= n && isdigit((unsigned char)p[i]) && isdigit((unsigned char)p[i+1])) { ss = (p[i]-'0')*10 + (p[i+1]-'0'); i += 2; }
            if (i != n) return false;                  /* trailing junk */
            if (mm > 59 || ss > 59) return false;      /* "+02:60" is not an offset */
            *out = sign * (hh * 3600 + mm * 60 + ss);
            if (utcish && sign < 0 && *out == 0) *utcish = true;   /* "-00:00" means UTC, "+00:00" does not */
            return true;
        }
    }
    return false;
}

/* CRuby's utc_offset ArgumentError, which quotes the offending value. */
static RESULT korb_raise_bad_utc_offset(CTX *c, VALUE *slots, VALUE v) {
    char *ib = NULL; size_t il = 0;
    FILE *ms = open_memstream(&ib, &il);
    if (ms) { korb_fprint_to_s(c, ms, v); fclose(ms); }
    RESULT r = korb_raise(c, slots, KORB_E_ARGUMENT, 0,
                          "\"+HH:MM\", \"-HH:MM\", \"UTC\" or \"A\"..\"I\",\"K\"..\"Z\" expected for utc_offset: %s",
                          ib ? ib : "");
    free(ib);
    return r;
}

/* @__tz: the timezone object a Time was built with (CRuby's timezone protocol),
 * kept so #zone can hand it back.  @__off still holds the offset it resolved to,
 * so every renderer keeps working off the numeric offset alone. */
static VALUE korb_time_tz_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__tz", 5)); }

static RESULT korb_time_make(CTX *c, VALUE *slots, VALUE cls, double epoch, bool utc);
static bool korb_obj_is_numeric(CTX *c, VALUE v);

/* CRuby's timezone-object protocol: an object implementing #local_to_utc (and
 * optionally #utc_to_local) converts between wall clock and UTC.  `base` is the
 * epoch of the side we already have — the wall-clock components read as if they
 * were UTC for to_utc, the real instant otherwise — and is handed to the zone as
 * a UTC Time, which is the "Time-like argument" the protocol specifies.  The
 * zone's answer only has to reduce to an epoch via #to_i; its own zone/offset are
 * ignored, so the utc_offset is just the difference.  *handled stays false when
 * the object does not implement the direction asked for, which lets the caller
 * fall through to its own "bad offset" error. */
static RESULT
korb_tz_convert(CTX *c, VALUE *slots, VALUE zone, double base, bool to_utc, intptr_t *off, bool *handled)
{
    *handled = false;
    if (!KORB_OBJECT_P(zone)) return RESULT_OK(KORB_NIL);
    const uint32_t mid = korb_intern(c->vm, to_utc ? "local_to_utc" : "utc_to_local", 12);
    if (!korb_responds_to(c, zone, mid)) return RESULT_OK(KORB_NIL);
    slots[0] = zone;
    slots[1] = UNWRAP(korb_time_make(c, slots + 1, korb_const_get(c->vm, korb_intern(c->vm, "Time", 4)),
                                     (double)(time_t)base, true));
    const RESULT r = korb_send(c, slots + 2, mid, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[0] = r.value;
    const uint32_t to_i = korb_intern(c->vm, "to_i", 4);
    RESULT ir = RESULT_OK(KORB_NIL);
    if (korb_responds_to(c, slots[0], to_i)) {
        ir = korb_send(c, slots + 1, to_i, 0, 0);
        if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
    }
    if (!FIXNUM_P(ir.value))
        return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_coerce_name(c, slots[0]));
    const intptr_t got = FIX2LONG(ir.value);
    *off = to_utc ? (intptr_t)base - got : got - (intptr_t)base;
    if (*off <= -86400 || *off >= 86400)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "utc_offset out of range");
    *handled = true;
    return RESULT_OK(KORB_NIL);
}

/* A Rational utc_offset: seconds truncated toward zero into *off, plus the exact
 * value in *offx when it is not whole (CRuby's #utc_offset hands back the
 * Rational itself).  *got reports whether it reduced to a usable offset. */
static RESULT
korb_tz_offset_of_rational(CTX *c, VALUE *slots, VALUE_REF r, intptr_t *off, VALUE *offx, bool *got)
{
    slots[0] = VALUE_REF_GET(r);
    const RESULT tr = korb_send(c, slots + 1, korb_intern(c->vm, "to_i", 4), 0, 0);
    if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
    *got = korb_parse_tz_offset(tr.value, off, NULL);
    double d = 0;
    if (*got && korb_num_to_d(VALUE_REF_GET(r), &d) && d != (double)*off) *offx = VALUE_REF_GET(r);
    return RESULT_OK(KORB_NIL);
}

/* Resolve a zone argument — Time.new's 7th positional, `in:`, getlocal's
 * argument — to a utc_offset in seconds.  Accepts the numeric/String forms
 * directly, then #to_int / #to_str / #to_r, then the timezone-object protocol
 * (`base`/`to_utc` are only used for that).
 *
 * The two object-valued results stay in the caller's slots so they survive the
 * allocation of the Time itself: on return slots[0] is the timezone object (nil
 * if the zone was a plain offset) and slots[1] the exact Numeric offset (nil
 * unless it is a fractional number of seconds).  The caller must therefore build
 * its Time from slots+2 and hand those two slots to korb_time_set_zone.
 * *utcish is set for the "UTC"/"Z"/"-00:00" forms, which mark the Time as UTC. */
static RESULT
korb_time_zone_arg(CTX *c, VALUE *slots, VALUE zv, double base, bool to_utc, intptr_t *off, bool *utcish)
{
    slots[0] = KORB_NIL;                                   /* timezone object */
    slots[1] = KORB_NIL;                                   /* exact offset     */
    *utcish = false;
    if (korb_parse_tz_offset(zv, off, utcish)) goto range;
    /* a String offset has to match the format outright — no coercion (and in
     * particular not String#to_r, which happily turns "J" into 0) */
    if (KORB_STRING_P(zv)) return korb_raise_bad_utc_offset(c, slots + 2, zv);
    {
        slots[2] = zv;                                     /* park: every path below dispatches (a GC point) */
        const VALUE_REF zr = VALUE_REF_AT(&slots[2]);
        static const struct { const char *nm; uint32_t len; } conv[] = { { "to_int", 6 }, { "to_str", 6 }, { "to_r", 4 } };
        bool got = false;
        if (KORB_RATIONAL_P(zv)) CHECK(korb_tz_offset_of_rational(c, slots + 3, zr, off, &slots[1], &got));
        for (size_t ci = 0; ci < sizeof conv / sizeof conv[0] && !got; ci++) {
            const uint32_t mid = korb_intern(c->vm, conv[ci].nm, conv[ci].len);
            /* #to_r is only honoured from a Numeric (CRuby); a bare object with
             * #to_r is a TypeError like any other non-offset */
            if (conv[ci].len == 4 && !korb_obj_is_numeric(c, VALUE_REF_GET(zr))) continue;
            if (!korb_responds_to(c, VALUE_REF_GET(zr), mid)) continue;
            slots[3] = VALUE_REF_GET(zr);
            const RESULT ir = korb_send(c, slots + 4, mid, 0, 0);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (KORB_RATIONAL_P(ir.value)) {
                slots[3] = ir.value;
                CHECK(korb_tz_offset_of_rational(c, slots + 4, VALUE_REF_AT(&slots[3]), off, &slots[1], &got));
            } else {
                got = korb_parse_tz_offset(ir.value, off, NULL);
            }
        }
        if (!got) {
            bool handled = false;
            CHECK(korb_tz_convert(c, slots + 3, VALUE_REF_GET(zr), base, to_utc, off, &handled));
            if (handled) { slots[0] = VALUE_REF_GET(zr); return RESULT_OK(KORB_NIL); }
            return korb_raise(c, slots + 3, KORB_E_TYPE, 0, "can't convert %s into an exact number",
                              korb_coerce_name(c, VALUE_REF_GET(zr)));
        }
    }
range:
    if (*off <= -86400 || *off >= 86400)
        return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "utc_offset out of range");
    return RESULT_OK(KORB_NIL);
}

/* @__offx: the exact (non-whole-second) utc_offset, when there is one. */
static VALUE korb_time_offx_sym(struct korb_vm *vm) { return ID2SYM(korb_intern(vm, "@__offx", 7)); }

/* Stamp the resolved zone onto a freshly built Time.  Every argument is a rooted
 * slot: korb_ivar_set can grow the ivar table, i.e. it is a GC point. */
static RESULT korb_time_set_zone(CTX *c, VALUE *slots, VALUE_REF t, intptr_t off, VALUE_REF tzobj, VALUE_REF offx)
{
    CHECK(korb_ivar_set(c, slots, t, korb_time_off_sym(c->vm), LONG2FIX(off)));
    if (VALUE_REF_GET(tzobj) != KORB_NIL)
        CHECK(korb_ivar_set(c, slots, t, korb_time_tz_sym(c->vm), VALUE_REF_GET(tzobj)));
    if (VALUE_REF_GET(offx) != KORB_NIL)
        CHECK(korb_ivar_set(c, slots, t, korb_time_offx_sym(c->vm), VALUE_REF_GET(offx)));
    return RESULT_OK(VALUE_REF_GET(t));
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
/* The wall clock with its real resolution.  time(2) truncates to whole seconds,
 * which makes Time#usec/#nsec always 0 and breaks every "has at least
 * microsecond precision" / elapsed-time check. */
static void korb_time_now_parts(double *sec_int, long *nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec_int = (double)ts.tv_sec;
    *nsec = (long)ts.tv_nsec;
}
/* Time.now(in: zone) — same `in:` keyword as Time.new (offset or timezone object). */
static RESULT korb_m_time_now(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    VALUE in_zv = KORB_NIL;
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "in", 2)));
        if (ix >= 0) in_zv = korb_items_data(h->items)[2 * ix + 1];
    } else if (UNLIKELY(n >= 1)) {
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0)", n);
    }
    double sec; long ns; korb_time_now_parts(&sec, &ns);
    if (in_zv == KORB_NIL) return korb_time_make_ns(c, slots, VALUE_REF_GET(self), sec, ns, false);
    intptr_t in_off; bool utcish;                            /* slots[0]=zone obj, slots[1]=exact offset */
    CHECK(korb_time_zone_arg(c, slots, in_zv, sec, false, &in_off, &utcish));
    slots[2] = UNWRAP(korb_time_make_ns(c, slots + 2, VALUE_REF_GET(self), sec, ns, utcish));
    return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), in_off,
                              VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
}
/* Is `v` a Numeric (by class ancestry — used to gate the #to_r coercion)? */
static bool korb_obj_is_numeric(CTX *c, VALUE v) {
    if (!KORB_OBJECT_P(v)) return false;
    const VALUE num = korb_const_get(c->vm, korb_intern(c->vm, "Numeric", 7));
    return KORB_CLASS_P(num) && korb_class_has_ancestor(korb_dispatch_class(c, v), num);
}

/* Split a numeric epoch into whole seconds + nanoseconds.  A Rational is split
 * with integer math (num/den), because a double cannot hold "seconds since 1970"
 * to nanosecond precision — Time.at(t.to_r) must round-trip. */
static bool korb_epoch_parts(VALUE v, int64_t *sec, long *nsec) {
    if (FIXNUM_P(v)) { *sec = (int64_t)FIX2LONG(v); *nsec = 0; return true; }
    if (KORB_RATIONAL_P(v)) {
        const VALUE nv = VAL2RAT(v)->num, dv = VAL2RAT(v)->den;
        if (FIXNUM_P(nv) && FIXNUM_P(dv) && FIX2LONG(dv) != 0) {
            const __int128 num = (__int128)FIX2LONG(nv), den = (__int128)FIX2LONG(dv);
            __int128 q = num / den, r = num % den;
            if (r < 0) { q -= 1; r += den; }                  /* floor division */
            *sec = (int64_t)q;
            *nsec = (long)((r * 1000000000 + den / 2) / den); /* round to nearest ns */
            if (*nsec >= 1000000000) { *sec += 1; *nsec -= 1000000000; }
            return true;
        }
    }
    double d;
    if (!korb_num_to_d(v, &d)) return false;
    *sec = (int64_t)floor(d);
    *nsec = (long)((d - floor(d)) * 1e9 + 0.5);
    if (*nsec >= 1000000000) { *sec += 1; *nsec -= 1000000000; }
    return true;
}
static RESULT korb_m_time_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int64_t sec = 0; long nsec = 0;
    bool src_utc = false;
    VALUE v = VALUE_SLICE_GET(a, 0);
    if (KORB_OBJECT_P(v) && korb_ivar_get(c, v, korb_time_t_sym(c->vm)) != KORB_NIL) {
        sec = (int64_t)floor(korb_time_epoch(c, v));              /* Time.at(time) — keep its zone */
        nsec = korb_time_nsec_of(c, v);
        src_utc = korb_time_is_utc(c, v);
    } else if (!korb_epoch_parts(v, &sec, &nsec)) {               /* try #to_r, then #to_int, else TypeError */
        bool got = false;
        /* CRuby only takes #to_r from a Numeric (a bare object with #to_r is a
         * TypeError), so check the class before asking. */
        if (korb_obj_is_numeric(c, v) && korb_responds_to_coerce_p(c, slots, &v, korb_intern(c->vm, "to_r", 4))) {
            slots[0] = v;
            RESULT rr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_r", 4), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
            got = korb_epoch_parts(rr.value, &sec, &nsec);
        }
        if (!got && korb_responds_to_coerce_p(c, slots, &v, korb_intern(c->vm, "to_int", 6))) {
            slots[0] = v;
            RESULT ir = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            got = korb_epoch_parts(ir.value, &sec, &nsec);
        }
        if (!got) {   /* name the real class (korb_type_name says "Object" for any instance) */
            char cls[192];
            const VALUE bad = VALUE_SLICE_GET(a, 0);
            const VALUE k = KORB_OBJECT_P(bad) ? VAL2OBJ(bad)->klass : KORB_NIL;
            if (KORB_CLASS_P(k) && VAL2CLASS(k)->name_sym) korb_class_qname_into(c, k, cls, sizeof cls);
            else snprintf(cls, sizeof cls, "%s", korb_type_name(bad));
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", cls);
        }
    }
    const bool first_is_time = KORB_OBJECT_P(v) && korb_ivar_get(c, v, korb_time_t_sym(c->vm)) != KORB_NIL;
    /* a trailing Hash is kwargs (only `in:`), never a positional argument */
    uint32_t alen = VALUE_SLICE_LEN(a);
    VALUE in_zv = KORB_NIL;
    if (alen >= 2 && KORB_HASH_P(VALUE_SLICE_GET(a, alen - 1))) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, alen - 1));
        const int32_t ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "in", 2)));
        if (ix >= 0) in_zv = korb_items_data(h->items)[2 * ix + 1];
        alen--;
    }
    if (alen >= 2) {                                             /* Time.at(sec, subsec[, unit]) */
        if (first_is_time)                                       /* Time.at(time, subsec) is a TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert Time into an exact number");
        const VALUE sv2 = VALUE_SLICE_GET(a, 1);
        double subv;
        if (!korb_num_to_d(sv2, &subv))                          /* nil / String / non-Numeric → TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into an exact number", korb_type_name(sv2));
        double per_ns = 1000.0;                                  /* default unit = :microsecond */
        if (alen >= 3 && VALUE_SLICE_GET(a, 2) != KORB_NIL) {
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
    if (in_zv == KORB_NIL)
        return korb_time_make_ns(c, slots, VALUE_REF_GET(self), (double)sec, nsec, src_utc);
    intptr_t off; bool utcish;                                   /* Time.at(x, in: zone) */
    CHECK(korb_time_zone_arg(c, slots, in_zv, (double)sec, false, &off, &utcish));
    slots[2] = UNWRAP(korb_time_make_ns(c, slots + 2, VALUE_REF_GET(self), (double)sec, nsec, utcish));
    return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), off,
                              VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
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
/* Parse a Ruby 3.2+ Time.new(String) ISO-8601-like value.  Accepts
 *   YYYY[-MM[-DD[ T](hh:mm[:ss[.frac]])]] [offset]
 * where offset is Z, UTC, or +/-HH[:]MM[[:]SS].  Returns 0 on success,
 * 1 when a date is present but the time part is missing ("no time
 * information"), 2 on any malformed input.  comp[6] = Y,Mon,D,H,Min,S;
 * *nsec = fractional seconds truncated to nanoseconds; *off / *has_off
 * carry an explicit zone offset when the string supplies one. */
static int korb_time_iso_parse(const char *s, uint32_t len,
                               intptr_t comp[6], long *nsec, intptr_t *off, bool *has_off) {
    comp[0] = 1970; comp[1] = 1; comp[2] = 1; comp[3] = 0; comp[4] = 0; comp[5] = 0;
    *nsec = 0; *off = 0; *has_off = false;
    uint32_t i = 0;
    #define KISO_SP() do { while (i < len && (s[i] == ' ' || s[i] == '\t')) i++; } while (0)
    #define KISO_NUM(dst) do { uint32_t z0 = i; long zv = 0; \
        while (i < len && isdigit((unsigned char)s[i])) { zv = zv * 10 + (s[i] - '0'); i++; } \
        if (i == z0) { return 2; } (dst) = zv; } while (0)
    KISO_NUM(comp[0]);                                   /* year (required) */
    bool date_more = false;
    if (i < len && s[i] == '-') { date_more = true; i++; KISO_NUM(comp[1]);
        if (i < len && s[i] == '-') { i++; KISO_NUM(comp[2]); } }
    bool have_time = false;
    if (i < len && (s[i] == 'T' || s[i] == 't')) { i++; have_time = true; }
    else if (i < len && s[i] == ' ') {                   /* space may precede time OR offset */
        uint32_t j = i; while (j < len && s[j] == ' ') j++;
        uint32_t k = j; while (k < len && isdigit((unsigned char)s[k])) k++;
        if (k > j && k < len && s[k] == ':') { i = j; have_time = true; }
    }
    if (have_time) {
        KISO_NUM(comp[3]); if (i >= len || s[i] != ':') return 2; i++;
        KISO_NUM(comp[4]);
        if (i < len && s[i] == ':') { i++; KISO_NUM(comp[5]);
            if (i < len && s[i] == '.') { i++; uint32_t f0 = i; long ns = 0; int cnt = 0;
                while (i < len && isdigit((unsigned char)s[i])) { if (cnt < 9) { ns = ns * 10 + (s[i] - '0'); cnt++; } i++; }
                if (i == f0) { return 2; } while (cnt < 9) { ns *= 10; cnt++; } *nsec = ns; } }
    } else if (date_more) {
        return 1;                                        /* date given but no time */
    }
    KISO_SP();
    if (i < len) {
        if (s[i] == 'Z' || s[i] == 'z') { *has_off = true; *off = 0; i++; }
        else if (i + 3 <= len && !memcmp(s + i, "UTC", 3)) { *has_off = true; *off = 0; i += 3; }
        else if (s[i] == '+' || s[i] == '-') {
            const int sign = (s[i] == '-') ? -1 : 1; i++;
            if (i + 2 > len || !isdigit((unsigned char)s[i]) || !isdigit((unsigned char)s[i+1])) return 2;
            int hh = (s[i]-'0')*10 + (s[i+1]-'0'); i += 2; int mm = 0, ss = 0;
            if (i < len && s[i] == ':') i++;
            if (i + 2 <= len && isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i+1])) { mm = (s[i]-'0')*10 + (s[i+1]-'0'); i += 2; }
            if (i < len && s[i] == ':') i++;
            if (i + 2 <= len && isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i+1])) { ss = (s[i]-'0')*10 + (s[i+1]-'0'); i += 2; }
            *has_off = true; *off = sign * (hh * 3600 + mm * 60 + ss);
        } else return 2;
    }
    KISO_SP();
    if (i != len) return 2;                              /* trailing garbage */
    return 0;
    #undef KISO_SP
    #undef KISO_NUM
}

static RESULT korb_m_time_new(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    /* A trailing Hash is keyword arguments (in:, precision:); Time.new never
     * takes a positional Hash.  Extract `in:` (a fixed zone offset) and drop the
     * hash from the positional count. */
    VALUE in_zv = KORB_NIL;
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "in", 2)));
        if (ix >= 0) in_zv = korb_items_data(h->items)[2 * ix + 1];
        n--;
    }
    const bool has_in = (in_zv != KORB_NIL);
    if (n == 0 && !has_in) {
        double sec; long ns; korb_time_now_parts(&sec, &ns);
        return korb_time_make_ns(c, slots, VALUE_REF_GET(self), sec, ns, false);
    }
    if (n == 0) {                                            /* Time.new(in: zone) → now in that zone */
        double sec; long ns; korb_time_now_parts(&sec, &ns);
        intptr_t in_off; bool utcish;
        CHECK(korb_time_zone_arg(c, slots, in_zv, sec, false, &in_off, &utcish));
        slots[2] = UNWRAP(korb_time_make_ns(c, slots + 2, VALUE_REF_GET(self), sec, ns, utcish));
        return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), in_off,
                                  VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
    }
    /* Time.new(String): a single String argument is an ISO-8601-like timestamp. */
    if (n == 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0))) {
        const KorbString *const str = VAL2STR(VALUE_SLICE_GET(a, 0));
        intptr_t comp[6]; long nsec; intptr_t off; bool has_off;
        const int pr = korb_time_iso_parse(korb_strbuf_data(str->buf), str->len, comp, &nsec, &off, &has_off);
        if (pr == 1) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no time information in %.*s", (int)str->len, korb_strbuf_data(str->buf));
        if (pr != 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument out of range");
        /* An offset in the string wins over `in:` (CRuby); otherwise `in:` applies. */
        const bool eff_has = has_off || has_in;
        if (comp[1] < 1 || comp[1] > 12 || comp[2] < 1 || comp[2] > 31 ||
            comp[3] < 0 || comp[3] > 24 || comp[4] < 0 || comp[4] > 59 || comp[5] < 0 || comp[5] > 60 ||
            (has_off && (off <= -86400 || off >= 86400)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument out of range");
        struct tm tm; memset(&tm, 0, sizeof tm);
        tm.tm_year = (int)comp[0] - 1900; tm.tm_mon = (int)comp[1] - 1; tm.tm_mday = (int)comp[2];
        tm.tm_hour = (int)comp[3]; tm.tm_min = (int)comp[4]; tm.tm_sec = (int)comp[5]; tm.tm_isdst = -1;
        intptr_t eff_off = off; bool utcish = false;
        slots[0] = slots[1] = KORB_NIL;                    /* zone obj / exact offset */
        if (!has_off && has_in) {
            struct tm tmw = tm;                            /* timegm normalizes in place */
            CHECK(korb_time_zone_arg(c, slots, in_zv, (double)timegm(&tmw), true, &eff_off, &utcish));
        }
        const double e = eff_has ? (double)timegm(&tm) - (double)eff_off : (double)mktime(&tm);
        slots[2] = UNWRAP(korb_time_make_ns(c, slots + 2, VALUE_REF_GET(self), e, nsec, utcish));
        if (eff_has) return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), eff_off,
                                               VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
        return RESULT_OK(slots[2]);
    }
    /* Time.new(y,m,d,h,min,s, utc_offset): the 7th arg (or `in:`) fixes the zone
     * offset; the components are wall-clock in that offset, epoch = timegm - off. */
    const bool pos_off = (n >= 7 && VALUE_SLICE_GET(a, 6) != KORB_NIL);
    if (pos_off && has_in) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "timezone argument given as positional and keyword arguments");
    if (pos_off || has_in) {
        struct tm tm; memset(&tm, 0, sizeof tm);
        intptr_t comp[6] = { 1970, 1, 1, 0, 0, 0 };
        for (uint32_t i = 0; i < 6 && i < n; i++) {
            const VALUE cv = VALUE_SLICE_GET(a, i);
            if (FIXNUM_P(cv)) comp[i] = FIX2LONG(cv);
            else if (KORB_FLOAT_P(cv)) comp[i] = (intptr_t)korb_float_val(cv);
        }
        if (comp[1] < 1 || comp[1] > 12 || comp[2] < 1 || comp[2] > 31 ||
            comp[3] < 0 || comp[3] > 24 || comp[4] < 0 || comp[4] > 59 || comp[5] < 0 || comp[5] > 60)
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument out of range");
        tm.tm_year = (int)comp[0] - 1900; tm.tm_mon = (int)comp[1] - 1; tm.tm_mday = (int)comp[2];
        tm.tm_hour = (int)comp[3]; tm.tm_min = (int)comp[4]; tm.tm_sec = (int)comp[5];
        /* the components are wall clock in the given zone: `wall` is them read as
         * if UTC, which is both what a timezone object is handed and what a fixed
         * offset is subtracted from */
        struct tm tmw = tm;                                  /* timegm normalizes in place */
        const double wall = (double)timegm(&tmw);
        intptr_t off; bool utcish;
        CHECK(korb_time_zone_arg(c, slots, pos_off ? VALUE_SLICE_GET(a, 6) : in_zv, wall, true, &off, &utcish));
        double offd = (double)off;
        if (slots[1] != KORB_NIL) korb_num_to_d(slots[1], &offd);      /* exact fractional offset */
        slots[2] = UNWRAP(korb_time_make(c, slots + 2, VALUE_REF_GET(self), wall - offd, utcish));
        return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), off,
                                  VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
    }
    /* Plain local Time.new(y,m,...).  Reached only when no trailing kwargs Hash is
     * present (a Hash carrying `in:` took the offset path above), so `a` holds
     * exactly the positional components. */
    return korb_time_from_parts(c, slots, VALUE_REF_GET(self), a, false);
}

/* ---- Time#<instance methods> ---------------------------------------------- */
static RESULT korb_m_time_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    /* seconds since the epoch, FLOORED (a pre-1970 stamp truncates the wrong way
     * with a plain cast: Time.at(-1.5).to_i is -2, not -1) */
    return RESULT_OK(LONG2FIX((intptr_t)floor(korb_time_epoch(c, VALUE_REF_GET(self)))));
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
        intptr_t off; bool utcish;
        CHECK(korb_time_zone_arg(c, slots, VALUE_SLICE_GET(a, 0), e, false, &off, &utcish));
        /* re-read the class: resolving the zone dispatches, i.e. it can move objects */
        slots[2] = UNWRAP(korb_time_make(c, slots + 2, korb_class_obj_of(c, VALUE_REF_GET(self)), e, utcish));
        return korb_time_set_zone(c, slots + 3, VALUE_REF_AT(&slots[2]), off,
                                  VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]));
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
    if (x != y) return RESULT_OK(LONG2FIX(x < y ? -1 : 1));
    const long xn = korb_time_nsec_of(c, VALUE_REF_GET(self)), yn = korb_time_nsec_of(c, o);
    return RESULT_OK(LONG2FIX(xn < yn ? -1 : xn > yn ? 1 : 0));
}
static RESULT korb_m_time_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o) || korb_ivar_get(c, o, korb_time_t_sym(c->vm)) == KORB_NIL) return RESULT_OK(KORB_FALSE);
    /* the epoch double loses sub-second bits for present-day stamps, so the
     * exact @__ns decides ties */
    if (korb_time_epoch(c, VALUE_REF_GET(self)) != korb_time_epoch(c, o)) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(korb_time_nsec_of(c, VALUE_REF_GET(self)) == korb_time_nsec_of(c, o) ? KORB_TRUE : KORB_FALSE);
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
    (void)a;
    const VALUE tz = korb_ivar_get(c, VALUE_REF_GET(self), korb_time_tz_sym(c->vm));
    if (tz != KORB_NIL) return RESULT_OK(tz);                /* built from a timezone object */
    intptr_t off;
    if (korb_time_fixed_off(c, VALUE_REF_GET(self), &off) && !korb_time_is_utc(c, VALUE_REF_GET(self)))
        return RESULT_OK(KORB_NIL);                          /* a bare numeric offset has no zone name */
    struct tm tm; korb_time_tm(c, VALUE_REF_GET(self), &tm);
    const char *z = korb_time_is_utc(c, VALUE_REF_GET(self)) ? "UTC" : (tm.tm_zone ? tm.tm_zone : "");
    return korb_str_new(c, slots, z, (uint32_t)strlen(z));
}
static RESULT korb_m_time_utc_offset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const VALUE ox = korb_ivar_get(c, VALUE_REF_GET(self), korb_time_offx_sym(c->vm));
    if (ox != KORB_NIL) return RESULT_OK(ox);                /* exact (fractional) offset */
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
    korb_class_def_cfn(c, slots[1], "now",     korb_m_time_now,   -1);
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
