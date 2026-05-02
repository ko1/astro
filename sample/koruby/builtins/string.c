/* String — moved from builtins.c. */

/* ---------- String ---------- */
static VALUE str_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    VALUE r = korb_str_dup(self);
    return korb_str_concat(r, argv[0]);
}
static VALUE str_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_concat(self, argv[0]);
}
static VALUE str_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(((struct korb_string *)self)->len);
}
static VALUE str_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(self, argv[0]));
}

static VALUE str_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    struct korb_string *a = (struct korb_string *)self;
    struct korb_string *b = (struct korb_string *)argv[0];
    long n = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->ptr, b->ptr, n);
    if (r != 0) return INT2FIX(r < 0 ? -1 : 1);
    if (a->len < b->len) return INT2FIX(-1);
    if (a->len > b->len) return INT2FIX(1);
    return INT2FIX(0);
}

static VALUE str_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) < 0);
}
static VALUE str_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) <= 0);
}
static VALUE str_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) > 0);
}
static VALUE str_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) >= 0);
}
static VALUE str_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE str_to_sym(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_to_sym(self);
}


/* ---------- String formatting / methods (extended) ---------- */

static VALUE str_format_self(CTX *c, VALUE self, int argc, VALUE *argv);

static VALUE str_split(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new();
    if (argc == 0 || NIL_P(argv[0])) {
        /* split on whitespace */
        long i = 0;
        while (i < s->len) {
            while (i < s->len && (s->ptr[i] == ' ' || s->ptr[i] == '\t' || s->ptr[i] == '\n')) i++;
            if (i >= s->len) break;
            long start = i;
            while (i < s->len && s->ptr[i] != ' ' && s->ptr[i] != '\t' && s->ptr[i] != '\n') i++;
            korb_ary_push(r, korb_str_new(s->ptr + start, i - start));
        }
        return r;
    }
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return r;
    struct korb_string *sep = (struct korb_string *)argv[0];
    if (sep->len == 0) {
        for (long i = 0; i < s->len; i++) korb_ary_push(r, korb_str_new(s->ptr + i, 1));
        return r;
    }
    long start = 0;
    for (long i = 0; i + sep->len <= s->len; ) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(s->ptr + start, i - start));
            i += sep->len;
            start = i;
        } else i++;
    }
    korb_ary_push(r, korb_str_new(s->ptr + start, s->len - start));
    return r;
}

static VALUE str_chomp(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long n = s->len;
    if (n > 0 && s->ptr[n-1] == '\n') n--;
    if (n > 0 && s->ptr[n-1] == '\r') n--;
    return korb_str_new(s->ptr, n);
}

static VALUE str_strip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && (s->ptr[start] == ' ' || s->ptr[start] == '\t' || s->ptr[start] == '\n' || s->ptr[start] == '\r')) start++;
    while (end > start && (s->ptr[end-1] == ' ' || s->ptr[end-1] == '\t' || s->ptr[end-1] == '\n' || s->ptr[end-1] == '\r')) end--;
    return korb_str_new(s->ptr + start, end - start);
}

static VALUE str_to_i(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *end;
    long v = strtol(s->ptr, &end, argc > 0 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10);
    return INT2FIX(v);
}

static VALUE str_to_f(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(strtod(((struct korb_string *)self)->ptr, NULL));
}

static VALUE str_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (argc == 1 && FIXNUM_P(argv[0])) {
        long i = FIX2LONG(argv[0]);
        if (i < 0) i += s->len;
        if (i < 0 || i >= s->len) return Qnil;
        return korb_str_new(s->ptr + i, 1);
    }
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        if (b < 0) b += s->len;
        if (e < 0) e += s->len;
        if (b < 0 || b > s->len) return Qnil;
        if (r->exclude_end) e -= 1;
        if (e >= s->len) e = s->len - 1;
        long len = e - b + 1;
        if (len < 0) len = 0;
        return korb_str_new(s->ptr + b, len);
    }
    if (argc == 2 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        long i = FIX2LONG(argv[0]);
        long len = FIX2LONG(argv[1]);
        if (i < 0) i += s->len;
        if (i < 0 || i > s->len) return Qnil;
        if (i + len > s->len) len = s->len - i;
        if (len < 0) len = 0;
        return korb_str_new(s->ptr + i, len);
    }
    return Qnil;
}

static VALUE str_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* not used by optcarrot main path; stub */
    return Qnil;
}

static VALUE str_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *needle = (struct korb_string *)argv[0];
    long start = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 0;
    if (start < 0) start += s->len;
    if (start < 0) start = 0;
    if (needle->len == 0) return INT2FIX(start <= s->len ? start : s->len);
    for (long i = start; i + needle->len <= s->len; i++) {
        if (memcmp(s->ptr + i, needle->ptr, needle->len) == 0) return INT2FIX(i);
    }
    return Qnil;
}

static VALUE str_rindex(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *needle = (struct korb_string *)argv[0];
    long start = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : s->len;
    if (start < 0) start += s->len;
    if (start > s->len - needle->len) start = s->len - needle->len;
    if (start < 0) return Qnil;
    if (needle->len == 0) return INT2FIX(start);
    for (long i = start; i >= 0; i--) {
        if (memcmp(s->ptr + i, needle->ptr, needle->len) == 0) return INT2FIX(i);
    }
    return Qnil;
}

static VALUE str_chars(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new_capa(s->len);
    for (long i = 0; i < s->len; i++) korb_ary_push(r, korb_str_new(s->ptr + i, 1));
    return r;
}

static VALUE str_bytes(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new_capa(s->len);
    for (long i = 0; i < s->len; i++) korb_ary_push(r, INT2FIX((unsigned char)s->ptr[i]));
    return r;
}

static VALUE str_each_char(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (long i = 0; i < s->len; i++) {
        VALUE ch = korb_str_new(s->ptr + i, 1);
        korb_yield(c, 1, &ch);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE str_start_with(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_end_with(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr + s->len - p->len, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    if (p->len == 0) return Qtrue;
    for (long i = 0; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_replace(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *o = (struct korb_string *)argv[0];
    s->ptr = korb_xmalloc_atomic(o->len + 1);
    memcpy(s->ptr, o->ptr, o->len);
    s->ptr[o->len] = 0;
    s->len = o->len;
    s->capa = o->len;
    return self;
}

static VALUE str_reverse(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) r[i] = s->ptr[s->len - 1 - i];
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_upcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_downcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_string *)self)->len == 0);
}

static VALUE str_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return korb_str_new("", 0);
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_str_new("", 0);
    for (long i = 0; i < n; i++) korb_str_concat(r, self);
    (void)s;
    return r;
}

static VALUE str_hash(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)(korb_hash_value(self) >> 1));
}

static VALUE str_sum(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Simple checksum: sum of bytes mod 65536 (default bits=16) */
    struct korb_string *s = (struct korb_string *)self;
    long bits = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 16;
    unsigned long sum = 0;
    for (long i = 0; i < s->len; i++) sum += (unsigned char)s->ptr[i];
    if (bits > 0 && bits < 64) sum &= ((1UL << bits) - 1);
    return INT2FIX((long)sum);
}

static VALUE str_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String === other ⇒ same as == */
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(self, argv[0]));
}

static VALUE str_match_op(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String#=~ regex — we don't have regex, return nil */
    (void)c; (void)self; (void)argc; (void)argv;
    return Qnil;
}

static VALUE str_match_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String#match? — false (no regex) */
    return Qfalse;
}

static VALUE str_match(CTX *c, VALUE self, int argc, VALUE *argv) {
    return Qnil;
}

static VALUE str_scan(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_new();
}

/* simplistic gsub: replace all non-overlapping occurrences of pattern in self.
 * pattern is treated as a literal string (no regex support). */
static VALUE str_gsub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    struct korb_string *r = (struct korb_string *)argv[1];
    if (p->len == 0) return korb_str_dup(self);
    VALUE out = korb_str_new("", 0);
    long start = 0;
    for (long i = 0; i + p->len <= s->len; ) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) {
            korb_str_concat(out, korb_str_new(s->ptr + start, i - start));
            korb_str_concat(out, korb_str_new(r->ptr, r->len));
            i += p->len;
            start = i;
        } else i++;
    }
    korb_str_concat(out, korb_str_new(s->ptr + start, s->len - start));
    return out;
}

static VALUE str_sub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    struct korb_string *r = (struct korb_string *)argv[1];
    if (p->len == 0) return korb_str_dup(self);
    for (long i = 0; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) {
            VALUE out = korb_str_new(s->ptr, i);
            korb_str_concat(out, korb_str_new(r->ptr, r->len));
            korb_str_concat(out, korb_str_new(s->ptr + i + p->len, s->len - i - p->len));
            return out;
        }
    }
    return korb_str_dup(self);
}

static VALUE str_tr(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* simplistic: each char in arg0 maps to corresponding char in arg1 */
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *from = (struct korb_string *)argv[0];
    struct korb_string *to = (struct korb_string *)argv[1];
    char *out = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        out[i] = ch;
        for (long j = 0; j < from->len; j++) {
            if (from->ptr[j] == ch) {
                if (j < to->len) out[i] = to->ptr[j];
                else if (to->len > 0) out[i] = to->ptr[to->len-1];
                break;
            }
        }
    }
    out[s->len] = 0;
    return korb_str_new(out, s->len);
}

/* sprintf — limited; supports %d %s %x %o %X %b %f %g %% %c, with width/0pad */
static VALUE kernel_format(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    struct korb_string *fmt = (struct korb_string *)argv[0];
    int ai = 1;
    VALUE out = korb_str_new("", 0);
    for (long i = 0; i < fmt->len; i++) {
        if (fmt->ptr[i] != '%') { korb_str_concat(out, korb_str_new(fmt->ptr + i, 1)); continue; }
        i++;
        char spec[64]; int sl = 0;
        spec[sl++] = '%';
        /* flags */
        while (i < fmt->len && (fmt->ptr[i] == '-' || fmt->ptr[i] == '+' || fmt->ptr[i] == ' ' || fmt->ptr[i] == '#' || fmt->ptr[i] == '0')) {
            spec[sl++] = fmt->ptr[i++];
        }
        /* width */
        while (i < fmt->len && fmt->ptr[i] >= '0' && fmt->ptr[i] <= '9') spec[sl++] = fmt->ptr[i++];
        /* precision */
        if (i < fmt->len && fmt->ptr[i] == '.') {
            spec[sl++] = fmt->ptr[i++];
            while (i < fmt->len && fmt->ptr[i] >= '0' && fmt->ptr[i] <= '9') spec[sl++] = fmt->ptr[i++];
        }
        if (i >= fmt->len) break;
        char conv = fmt->ptr[i];
        spec[sl++] = conv;
        spec[sl] = 0;
        char buf[256];
        switch (conv) {
            case '%': buf[0] = '%'; buf[1] = 0; break;
            case 'd': case 'i': case 'u':
            case 'x': case 'X': case 'o': case 'b': case 'c': {
                long v = ai < argc && FIXNUM_P(argv[ai]) ? FIX2LONG(argv[ai]) : 0;
                if (conv == 'b') {
                    /* binary — manually */
                    char tmp[64]; int tl = 0;
                    unsigned long uv = (unsigned long)v;
                    if (uv == 0) tmp[tl++] = '0';
                    while (uv) { tmp[tl++] = '0' + (uv & 1); uv >>= 1; }
                    for (int j = 0; j < tl/2; j++) { char tch = tmp[j]; tmp[j] = tmp[tl-1-j]; tmp[tl-1-j] = tch; }
                    tmp[tl] = 0;
                    snprintf(buf, sizeof(buf), "%s", tmp);
                } else {
                    /* replace conv with ld */
                    if (conv == 'd' || conv == 'i' || conv == 'u') {
                        spec[sl-1] = 'l'; spec[sl++] = 'd'; spec[sl] = 0;
                        snprintf(buf, sizeof(buf), spec, v);
                    } else {
                        snprintf(buf, sizeof(buf), spec, (unsigned long)v);
                    }
                }
                ai++;
                break;
            }
            case 'f': case 'g': case 'e': case 'E': case 'G': {
                double dv = ai < argc ? korb_num2dbl(argv[ai]) : 0.0;
                snprintf(buf, sizeof(buf), spec, dv);
                ai++;
                break;
            }
            case 's': {
                VALUE v = ai < argc ? argv[ai] : korb_str_new("", 0);
                if (BUILTIN_TYPE(v) != T_STRING) v = korb_to_s(v);
                snprintf(buf, sizeof(buf), spec, ((struct korb_string *)v)->ptr);
                ai++;
                break;
            }
            default:
                snprintf(buf, sizeof(buf), "%%%c", conv);
        }
        korb_str_concat(out, korb_str_new_cstr(buf));
    }
    return out;
}

/* printf — format then write to stdout */
static VALUE kernel_printf(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) return Qnil;
    VALUE s = kernel_format(c, self, argc, argv);
    fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
    return Qnil;
}

/* String#% — same as format but self is the format string */
static VALUE str_percent(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* arg may be a single value or an Array */
    VALUE *fargv;
    int fargc;
    VALUE single[2];
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        single[0] = self;
        VALUE *full = korb_xmalloc((1 + a->len) * sizeof(VALUE));
        full[0] = self;
        for (long i = 0; i < a->len; i++) full[1+i] = a->ptr[i];
        return kernel_format(c, self, 1 + (int)a->len, full);
    }
    /* single arg or multiple */
    VALUE *full = korb_xmalloc((1 + argc) * sizeof(VALUE));
    full[0] = self;
    for (int i = 0; i < argc; i++) full[1+i] = argv[i];
    return kernel_format(c, self, 1 + argc, full);
}

