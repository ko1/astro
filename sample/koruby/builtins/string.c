/* String — moved from builtins.c. */

/* String.new(s = "") — start the new string from an optional initial
 * value.  Class#new's generic path goes through korb_object_new which
 * doesn't allocate the String storage; we need a real heap String. */
VALUE str_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) return korb_str_new("", 0);
    if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        struct korb_string *s = (struct korb_string *)argv[0];
        return korb_str_new(s->ptr, s->len);
    }
    return korb_str_new("", 0);
}

/* ---------- String ---------- */
static VALUE str_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    VALUE r = korb_str_dup(self);
    return korb_str_concat(r, argv[0]);
}
static VALUE str_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    return korb_str_concat(self, argv[0]);
}
static VALUE str_bytesize(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(((struct korb_string *)self)->len);
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
    CHECK_FROZEN_RET(c, self, Qnil);
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
    /* Block-less form: koruby has no Enumerator, so return an Array of
     * single-char strings (matches what `.to_a` would yield). */
    if (!korb_block_given()) {
        VALUE r = korb_ary_new();
        for (long i = 0; i < s->len; i++) korb_ary_push(r, korb_str_new(s->ptr + i, 1));
        return r;
    }
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
    CHECK_FROZEN_RET(c, self, Qnil);
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
/* Helper: locate the next match of `pattern` in `s` starting at `from`.
 * Returns (start, len) via out-params and 1 on match, 0 on miss.  We
 * special-case our shim Regexp objects (which embed the pattern as a
 * String ivar) and fall back to byte-string search otherwise. */
static int str_find_pat(VALUE pattern, struct korb_string *s, long from,
                        long *match_start, long *match_len) {
    struct korb_string *p = NULL;
    if (BUILTIN_TYPE(pattern) == T_STRING) {
        p = (struct korb_string *)pattern;
    } else if (!SPECIAL_CONST_P(pattern) && BUILTIN_TYPE(pattern) == T_OBJECT) {
        VALUE src = korb_ivar_get(pattern, korb_intern("@source"));
        if (BUILTIN_TYPE(src) == T_STRING) p = (struct korb_string *)src;
    }
    if (!p || p->len == 0) return 0;
    for (long i = from; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) {
            *match_start = i; *match_len = p->len;
            return 1;
        }
    }
    return 0;
}

static VALUE str_gsub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    extern struct korb_proc *current_block;
    VALUE out = korb_str_new("", 0);
    long start = 0, i = 0;
    long ms, ml;
    while (str_find_pat(argv[0], s, i, &ms, &ml)) {
        korb_str_concat(out, korb_str_new(s->ptr + start, ms - start));
        if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
            struct korb_string *r = (struct korb_string *)argv[1];
            korb_str_concat(out, korb_str_new(r->ptr, r->len));
        } else if (current_block) {
            VALUE m = korb_str_new(s->ptr + ms, ml);
            VALUE r = korb_yield(c, 1, &m);
            if (c->state == KORB_RAISE) return Qnil;
            if (BUILTIN_TYPE(r) == T_STRING) korb_str_concat(out, r);
            else korb_str_concat(out, korb_to_s(r));
        }
        i = ms + (ml > 0 ? ml : 1);
        start = i;
    }
    korb_str_concat(out, korb_str_new(s->ptr + start, s->len - start));
    return out;
}

static VALUE str_sub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    extern struct korb_proc *current_block;
    long ms, ml;
    if (!str_find_pat(argv[0], s, 0, &ms, &ml)) return korb_str_dup(self);
    VALUE out = korb_str_new(s->ptr, ms);
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *r = (struct korb_string *)argv[1];
        korb_str_concat(out, korb_str_new(r->ptr, r->len));
    } else if (current_block) {
        VALUE m = korb_str_new(s->ptr + ms, ml);
        VALUE r = korb_yield(c, 1, &m);
        if (c->state == KORB_RAISE) return Qnil;
        if (BUILTIN_TYPE(r) == T_STRING) korb_str_concat(out, r);
        else korb_str_concat(out, korb_to_s(r));
    }
    korb_str_concat(out, korb_str_new(s->ptr + ms + ml, s->len - ms - ml));
    return out;
}

/* Expand a tr-spec ("a-zA-Z") into a 256-byte character table where
 * tbl[ch] = mapped_char_or_(-1 if not in spec).  Both `from` and `to`
 * are expanded into their full char sequences first; if `to` is
 * shorter, its last char is repeated.  Negation (^) on the `from`
 * side is supported. */
static long str_tr_expand(const char *spec, long len, char *out, long out_cap) {
    long w = 0;
    long i = 0;
    while (i < len && w < out_cap) {
        if (i + 2 < len && spec[i+1] == '-') {
            unsigned char a = (unsigned char)spec[i];
            unsigned char b = (unsigned char)spec[i+2];
            if (b < a) { unsigned char t = a; a = b; b = t; }
            for (int k = a; k <= b && w < out_cap; k++) out[w++] = (char)k;
            i += 3;
        } else {
            out[w++] = spec[i++];
        }
    }
    return w;
}

/* Run tr/tr_s with `squeeze` controlling whether runs of replaced chars
 * collapse.  tr_s squeezes; tr doesn't. */
static VALUE str_tr_impl(VALUE self, int argc, VALUE *argv, bool squeeze) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING)
        return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *from_in = (struct korb_string *)argv[0];
    struct korb_string *to_in   = (struct korb_string *)argv[1];
    bool negate = (from_in->len > 0 && from_in->ptr[0] == '^');
    const char *fs = negate ? from_in->ptr + 1 : from_in->ptr;
    long fs_len = negate ? from_in->len - 1 : from_in->len;
    char from_buf[512], to_buf[512];
    long from_n = str_tr_expand(fs, fs_len, from_buf, sizeof(from_buf));
    long to_n   = str_tr_expand(to_in->ptr, to_in->len, to_buf, sizeof(to_buf));
    int tbl[256];
    for (int k = 0; k < 256; k++) tbl[k] = -1;
    if (negate) {
        char repl = to_n > 0 ? to_buf[to_n - 1] : '\0';
        for (int k = 0; k < 256; k++) tbl[k] = (unsigned char)repl;
        for (long j = 0; j < from_n; j++) tbl[(unsigned char)from_buf[j]] = -1;
    } else {
        for (long j = 0; j < from_n; j++) {
            int repl = j < to_n ? (unsigned char)to_buf[j]
                                : (to_n > 0 ? (unsigned char)to_buf[to_n - 1] : -2);
            tbl[(unsigned char)from_buf[j]] = repl;
        }
    }
    char *out = korb_xmalloc_atomic(s->len + 1);
    long w = 0;
    int last_repl = -1;
    for (long i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->ptr[i];
        int repl = tbl[ch];
        if (repl == -1) {
            out[w++] = (char)ch;
            last_repl = -1;
        } else if (repl == -2) {
            last_repl = -1;
        } else {
            if (squeeze && repl == last_repl) continue;
            out[w++] = (char)repl;
            last_repl = repl;
        }
    }
    out[w] = 0;
    return korb_str_new(out, w);
}

static VALUE str_tr(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_tr_impl(self, argc, argv, false);
}

static VALUE str_tr_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_tr_impl(self, argc, argv, true);
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

/* String#center(width, padstr=" ") — center self within `width` cols. */
static VALUE str_center(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return self;
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return self;
    }
    long extra = width - s->len;
    long left = extra / 2, right = extra - left;
    char *buf = korb_xmalloc_atomic(width);
    for (long i = 0; i < left;  i++) buf[i] = pad[i % padlen];
    memcpy(buf + left, s->ptr, s->len);
    for (long i = 0; i < right; i++) buf[left + s->len + i] = pad[i % padlen];
    return korb_str_new(buf, width);
}

/* String#ljust / rjust */
static VALUE str_ljust(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return self;
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return self;
    }
    long extra = width - s->len;
    char *buf = korb_xmalloc_atomic(width);
    memcpy(buf, s->ptr, s->len);
    for (long i = 0; i < extra; i++) buf[s->len + i] = pad[i % padlen];
    return korb_str_new(buf, width);
}
static VALUE str_rjust(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return self;
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return self;
    }
    long extra = width - s->len;
    char *buf = korb_xmalloc_atomic(width);
    for (long i = 0; i < extra; i++) buf[i] = pad[i % padlen];
    memcpy(buf + extra, s->ptr, s->len);
    return korb_str_new(buf, width);
}

/* String#chop / chop! */
static VALUE str_chop(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return korb_str_new("", 0);
    long n = s->len - 1;
    if (n > 0 && s->ptr[n] == '\n' && s->ptr[n-1] == '\r') n--;
    return korb_str_new(s->ptr, n);
}
static VALUE str_chop_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return Qnil;
    long n = s->len - 1;
    if (n > 0 && s->ptr[n] == '\n' && s->ptr[n-1] == '\r') n--;
    s->len = n;
    s->ptr[n] = 0;
    return self;
}

/* tr-style char-class bitmap.  `^` at the start inverts the set; `a-z`
 * expands to a range. */
static void str_charclass_build(const char *spec, long len, unsigned char *bits) {
    bool invert = false;
    long i = 0;
    if (len > 0 && spec[0] == '^') { invert = true; i = 1; }
    memset(bits, 0, 256);
    while (i < len) {
        if (i + 2 < len && spec[i+1] == '-') {
            unsigned char a = (unsigned char)spec[i];
            unsigned char b = (unsigned char)spec[i+2];
            if (b < a) { unsigned char t = a; a = b; b = t; }
            for (int k = a; k <= b; k++) bits[k] = 1;
            i += 3;
        } else {
            bits[(unsigned char)spec[i]] = 1;
            i++;
        }
    }
    if (invert) for (int k = 0; k < 256; k++) bits[k] = !bits[k];
}

static VALUE str_count_chars(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return INT2FIX(0);
    unsigned char bits[256];
    struct korb_string *cs = (struct korb_string *)argv[0];
    str_charclass_build(cs->ptr, cs->len, bits);
    struct korb_string *s = (struct korb_string *)self;
    long n = 0;
    for (long i = 0; i < s->len; i++) if (bits[(unsigned char)s->ptr[i]]) n++;
    return INT2FIX(n);
}

static VALUE str_delete_chars(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    unsigned char bits[256];
    struct korb_string *cs = (struct korb_string *)argv[0];
    str_charclass_build(cs->ptr, cs->len, bits);
    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len > 0 ? s->len : 1);
    long w = 0;
    for (long i = 0; i < s->len; i++) {
        if (!bits[(unsigned char)s->ptr[i]]) buf[w++] = s->ptr[i];
    }
    return korb_str_new(buf, w);
}

static VALUE str_squeeze(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    bool any_class = (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_STRING);
    unsigned char bits[256];
    if (any_class) {
        struct korb_string *cs = (struct korb_string *)argv[0];
        str_charclass_build(cs->ptr, cs->len, bits);
    }
    char *buf = korb_xmalloc_atomic(s->len > 0 ? s->len : 1);
    long w = 0;
    int prev = -1;
    for (long i = 0; i < s->len; i++) {
        unsigned char ch = s->ptr[i];
        if ((int)ch == prev && (!any_class || bits[ch])) continue;
        buf[w++] = ch;
        prev = ch;
    }
    return korb_str_new(buf, w);
}

static VALUE str_swapcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len > 0 ? s->len : 1);
    for (long i = 0; i < s->len; i++) {
        unsigned char ch = s->ptr[i];
        if (ch >= 'a' && ch <= 'z')      buf[i] = ch - 32;
        else if (ch >= 'A' && ch <= 'Z') buf[i] = ch + 32;
        else                              buf[i] = ch;
    }
    return korb_str_new(buf, s->len);
}

static VALUE str_capitalize(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return korb_str_new("", 0);
    char *buf = korb_xmalloc_atomic(s->len);
    unsigned char first = s->ptr[0];
    buf[0] = (first >= 'a' && first <= 'z') ? first - 32 : first;
    for (long i = 1; i < s->len; i++) {
        unsigned char ch = s->ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    return korb_str_new(buf, s->len);
}

/* String#lines — split on \n, keep newlines. */
static VALUE str_lines(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new();
    long start = 0;
    for (long i = 0; i < s->len; i++) {
        if (s->ptr[i] == '\n') {
            korb_ary_push(r, korb_str_new(s->ptr + start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < s->len) korb_ary_push(r, korb_str_new(s->ptr + start, s->len - start));
    return r;
}

static VALUE str_partition(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *sep = (struct korb_string *)argv[0];
    VALUE r = korb_ary_new_capa(3);
    if (sep->len == 0 || sep->len > s->len) {
        korb_ary_push(r, korb_str_new(s->ptr, s->len));
        korb_ary_push(r, korb_str_new("", 0));
        korb_ary_push(r, korb_str_new("", 0));
        return r;
    }
    for (long i = 0; i + sep->len <= s->len; i++) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(s->ptr, i));
            korb_ary_push(r, korb_str_new(sep->ptr, sep->len));
            korb_ary_push(r, korb_str_new(s->ptr + i + sep->len, s->len - i - sep->len));
            return r;
        }
    }
    korb_ary_push(r, korb_str_new(s->ptr, s->len));
    korb_ary_push(r, korb_str_new("", 0));
    korb_ary_push(r, korb_str_new("", 0));
    return r;
}

static VALUE str_rpartition(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *sep = (struct korb_string *)argv[0];
    VALUE r = korb_ary_new_capa(3);
    if (sep->len == 0 || sep->len > s->len) {
        korb_ary_push(r, korb_str_new("", 0));
        korb_ary_push(r, korb_str_new("", 0));
        korb_ary_push(r, korb_str_new(s->ptr, s->len));
        return r;
    }
    for (long i = s->len - sep->len; i >= 0; i--) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(s->ptr, i));
            korb_ary_push(r, korb_str_new(sep->ptr, sep->len));
            korb_ary_push(r, korb_str_new(s->ptr + i + sep->len, s->len - i - sep->len));
            return r;
        }
    }
    korb_ary_push(r, korb_str_new("", 0));
    korb_ary_push(r, korb_str_new("", 0));
    korb_ary_push(r, korb_str_new(s->ptr, s->len));
    return r;
}

/* String#succ — alphabetic increment; ASCII-only, simplified rules. */
static VALUE str_succ(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return korb_str_new("", 0);
    char *buf = korb_xmalloc_atomic(s->len);
    memcpy(buf, s->ptr, s->len);
    long i = s->len - 1;
    bool overflow = false;
    while (i >= 0) {
        unsigned char ch = buf[i];
        if      (ch >= 'a' && ch <  'z') { buf[i] = ch + 1; overflow = false; break; }
        else if (ch >= 'A' && ch <  'Z') { buf[i] = ch + 1; overflow = false; break; }
        else if (ch >= '0' && ch <  '9') { buf[i] = ch + 1; overflow = false; break; }
        else if (ch == 'z') { buf[i] = 'a'; overflow = true; i--; continue; }
        else if (ch == 'Z') { buf[i] = 'A'; overflow = true; i--; continue; }
        else if (ch == '9') { buf[i] = '0'; overflow = true; i--; continue; }
        else                { buf[i] = ch + 1; overflow = false; break; }
    }
    if (overflow) {
        char *grown = korb_xmalloc_atomic(s->len + 1);
        char first  = s->ptr[0];
        grown[0] = (first >= '0' && first <= '9') ? '1'
                 : (first >= 'a' && first <= 'z') ? 'a' : 'A';
        memcpy(grown + 1, buf, s->len);
        return korb_str_new(grown, s->len + 1);
    }
    return korb_str_new(buf, s->len);
}

/* String#each_byte — yields each byte as Integer. */
static VALUE str_each_byte(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (long i = 0; i < s->len; i++) {
        VALUE b = INT2FIX((unsigned char)s->ptr[i]);
        korb_yield(c, 1, &b);
        if (c->state == KORB_RAISE) return Qnil;
    }
    return self;
}

/* String#ord */
static VALUE str_ord(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) {
        korb_raise(c, NULL, "empty string");
        return Qnil;
    }
    return INT2FIX((unsigned char)s->ptr[0]);
}

/* String#eql? — content equality; rejects non-strings. */
static VALUE str_eql(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    extern VALUE str_eq(CTX *c, VALUE self, int argc, VALUE *argv);
    return str_eq(c, self, argc, argv);
}

/* String#clone — fresh independent copy. */
static VALUE str_clone(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    return korb_str_new(s->ptr, s->len);
}

/* String#% — same as format but self is the format string.  When the
 * argument is a Hash, the format string supports `%{name}` lookups
 * (e.g. `"%{a}+%{b}" % {a:1, b:2}` → `"1+2"`); otherwise we
 * delegate to the Array / single-arg printf-style path. */
static VALUE str_percent(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_HASH) {
        struct korb_string *fmt = (struct korb_string *)self;
        struct korb_hash *h = (struct korb_hash *)argv[0];
        VALUE out = korb_str_new("", 0);
        long i = 0;
        while (i < fmt->len) {
            if (i + 1 < fmt->len && fmt->ptr[i] == '%' && fmt->ptr[i+1] == '{') {
                long j = i + 2;
                while (j < fmt->len && fmt->ptr[j] != '}') j++;
                if (j < fmt->len) {
                    long klen = j - (i + 2);
                    char keybuf[256];
                    if (klen < (long)sizeof(keybuf)) {
                        memcpy(keybuf, fmt->ptr + i + 2, klen);
                        keybuf[klen] = 0;
                        VALUE key = korb_id2sym(korb_intern(keybuf));
                        VALUE v = korb_hash_aref((VALUE)h, key);
                        if (UNDEF_P(v)) v = Qnil;
                        VALUE vs = korb_to_s(v);
                        korb_str_concat(out, vs);
                        i = j + 1;
                        continue;
                    }
                }
            }
            korb_str_concat(out, korb_str_new(fmt->ptr + i, 1));
            i++;
        }
        return out;
    }
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
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

/* ---------- String#hex ----------
 * Parses an optional sign, optional "0x"/"0X" prefix, then hex digits.
 * Stops at first non-digit; returns 0 for fully unparsable input. */
static VALUE str_hex(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long i = 0, len = s->len;
    while (i < len && (s->ptr[i] == ' ' || s->ptr[i] == '\t')) i++;
    int sign = 1;
    if (i < len && (s->ptr[i] == '+' || s->ptr[i] == '-')) {
        if (s->ptr[i] == '-') sign = -1;
        i++;
    }
    if (i + 1 < len && s->ptr[i] == '0' && (s->ptr[i+1] == 'x' || s->ptr[i+1] == 'X')) i += 2;
    long v = 0;
    bool any = false;
    while (i < len) {
        char ch = s->ptr[i];
        int d;
        if      (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
        else if (ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
        else break;
        v = v * 16 + d;
        any = true;
        i++;
    }
    if (!any) return INT2FIX(0);
    return INT2FIX(v * sign);
}

/* ---------- String#oct ----------
 * Returns the integer parsed using base inferred from prefix:
 *   "0x"/"0X" → 16, "0b"/"0B" → 2, "0o"/"0O" or just leading '0' → 8,
 *   anything else → 10.  Sign-aware; 0 on no digits parsed. */
static VALUE str_oct(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long i = 0, len = s->len;
    while (i < len && (s->ptr[i] == ' ' || s->ptr[i] == '\t')) i++;
    int sign = 1;
    if (i < len && (s->ptr[i] == '+' || s->ptr[i] == '-')) {
        if (s->ptr[i] == '-') sign = -1;
        i++;
    }
    int base = 8;
    if (i + 1 < len && s->ptr[i] == '0') {
        char p = s->ptr[i+1];
        if (p == 'x' || p == 'X') { base = 16; i += 2; }
        else if (p == 'b' || p == 'B') { base = 2;  i += 2; }
        else if (p == 'o' || p == 'O') { base = 8;  i += 2; }
        /* otherwise stay at 8, leading '0' itself is part of the number */
    }
    long v = 0;
    bool any = false;
    while (i < len) {
        char ch = s->ptr[i];
        int d = -1;
        if      (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
        else if (ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = true;
        i++;
    }
    if (!any) return INT2FIX(0);
    return INT2FIX(v * sign);
}

/* ---------- String#prepend ----------
 * Mutates self by inserting other(s) at position 0; returns self. */
static VALUE str_prepend(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    /* Concatenate args into a single buffer first to keep the math simple. */
    long extra = 0;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) return self;
        extra += ((struct korb_string *)argv[i])->len;
    }
    long total = extra + s->len;
    char *np = korb_xmalloc_atomic(total + 1);
    long w = 0;
    for (int i = 0; i < argc; i++) {
        struct korb_string *p = (struct korb_string *)argv[i];
        memcpy(np + w, p->ptr, p->len); w += p->len;
    }
    memcpy(np + w, s->ptr, s->len);
    np[total] = 0;
    s->ptr = np;
    s->len = total;
    s->capa = total;
    return self;
}

/* ---------- String#insert(pos, str) ----------
 * Mutates self.  pos can be negative (counts from end + 1, so -1
 * inserts before the last char as in CRuby).  Returns self. */
static VALUE str_insert(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc < 2 || !FIXNUM_P(argv[0]) || BUILTIN_TYPE(argv[1]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[1];
    long pos = FIX2LONG(argv[0]);
    if (pos < 0) pos = s->len + pos + 1;
    if (pos < 0) pos = 0;
    if (pos > s->len) pos = s->len;
    long total = s->len + p->len;
    char *np = korb_xmalloc_atomic(total + 1);
    memcpy(np, s->ptr, pos);
    memcpy(np + pos, p->ptr, p->len);
    memcpy(np + pos + p->len, s->ptr + pos, s->len - pos);
    np[total] = 0;
    s->ptr = np;
    s->len = total;
    s->capa = total;
    return self;
}

/* ---------- String#delete_prefix / delete_suffix ----------
 * Non-mutating; returns the string without the prefix/suffix or a copy
 * of self if the prefix/suffix doesn't match. */
static VALUE str_delete_prefix(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0)
        return korb_str_new(s->ptr + p->len, s->len - p->len);
    return korb_str_new(s->ptr, s->len);
}

static VALUE str_delete_suffix(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    if (p->len <= s->len && memcmp(s->ptr + s->len - p->len, p->ptr, p->len) == 0)
        return korb_str_new(s->ptr, s->len - p->len);
    return korb_str_new(s->ptr, s->len);
}

/* ---------- String#each_line (real impl) ----------
 * Was registered as str_split, which split on whitespace.  Walks the
 * string yielding each line including its trailing '\n'; returns self. */
static VALUE str_each_line(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long start = 0;
    for (long i = 0; i < s->len; i++) {
        if (s->ptr[i] == '\n') {
            VALUE line = korb_str_new(s->ptr + start, i - start + 1);
            korb_yield(c, 1, &line);
            if (c->state != KORB_NORMAL) return Qnil;
            start = i + 1;
        }
    }
    if (start < s->len) {
        VALUE line = korb_str_new(s->ptr + start, s->len - start);
        korb_yield(c, 1, &line);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
