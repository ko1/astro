/* Array — moved from builtins.c.  Included from builtins.c so we
 * inherit its includes/macros (KORB_BOOL, korb_intern, etc.). */

/* ---------- Array ---------- */
static VALUE ary_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(korb_ary_len(self));
}
static VALUE ary_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 1) {
        if (FIXNUM_P(argv[0])) return korb_ary_aref(self, FIX2LONG(argv[0]));
        if (BUILTIN_TYPE(argv[0]) == T_RANGE) {
            struct korb_array *a = (struct korb_array *)self;
            struct korb_range *r = (struct korb_range *)argv[0];
            /* Endless / beginless ranges: nil begin/end stand in for
             * 0 / size-1.  Common in Ruby 2.7+ slicing. */
            long b, e;
            if (NIL_P(r->begin))      b = 0;
            else if (FIXNUM_P(r->begin)) b = FIX2LONG(r->begin);
            else return Qnil;
            if (NIL_P(r->end))        e = a->len - 1;
            else if (FIXNUM_P(r->end)) e = FIX2LONG(r->end);
            else return Qnil;
            if (b < 0) b += a->len;
            if (e < 0) e += a->len;
            if (r->exclude_end && !NIL_P(r->end)) e--;
            if (b < 0 || b > a->len) return Qnil;
            if (e >= a->len) e = a->len - 1;
            VALUE res = korb_ary_new();
            for (long i = b; i <= e; i++) korb_ary_push(res, a->ptr[i]);
            return res;
        }
        return Qnil;
    }
    if (argc == 2 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        struct korb_array *a = (struct korb_array *)self;
        long start = FIX2LONG(argv[0]);
        long len = FIX2LONG(argv[1]);
        if (start < 0) start += a->len;
        if (start < 0 || start > a->len || len < 0) return Qnil;
        if (start + len > a->len) len = a->len - start;
        VALUE r = korb_ary_new_capa(len);
        for (long i = 0; i < len; i++) korb_ary_push(r, a->ptr[start + i]);
        return r;
    }
    return Qnil;
}
static VALUE ary_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 2 && FIXNUM_P(argv[0])) {
        korb_ary_aset(self, FIX2LONG(argv[0]), argv[1]);
        return argv[1];
    }
    /* `a[range] = ...` — translate to the (start, len, value) form. */
    if (argc == 2 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_array *a = (struct korb_array *)self;
        struct korb_range *r = (struct korb_range *)argv[0];
        long b = NIL_P(r->begin) ? 0 :
                 (FIXNUM_P(r->begin) ? FIX2LONG(r->begin) : 0);
        long e = NIL_P(r->end) ? a->len - 1 :
                 (FIXNUM_P(r->end) ? FIX2LONG(r->end) : 0);
        if (b < 0) b += a->len;
        if (e < 0) e += a->len;
        if (r->exclude_end && !NIL_P(r->end)) e--;
        if (e < b - 1) e = b - 1;
        VALUE three[3] = { INT2FIX(b), INT2FIX(e - b + 1), argv[1] };
        return ary_aset(c, self, 3, three);
    }
    if (argc == 3 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        /* a[start, len] = value or a[start, len] = array */
        struct korb_array *a = (struct korb_array *)self;
        long start = FIX2LONG(argv[0]);
        long len = FIX2LONG(argv[1]);
        if (start < 0) start += a->len;
        if (start < 0 || len < 0) return argv[2];
        VALUE val = argv[2];
        if (BUILTIN_TYPE(val) == T_ARRAY) {
            struct korb_array *src = (struct korb_array *)val;
            /* Resize if needed */
            long new_len = start + src->len;
            if (new_len > a->len) {
                /* extend with nil first */
                while (a->len < new_len) korb_ary_push(self, Qnil);
            }
            /* If replacing fewer elements than provided, shift */
            if ((long)src->len != len) {
                long diff = (long)src->len - len;
                /* extend / shrink */
                long old = a->len;
                if (diff > 0) {
                    for (long i = 0; i < diff; i++) korb_ary_push(self, Qnil);
                    for (long i = old - 1; i >= start + len; i--) a->ptr[i + diff] = a->ptr[i];
                } else if (diff < 0) {
                    for (long i = start + len; i < old; i++) a->ptr[i + diff] = a->ptr[i];
                    a->len += diff;
                }
            }
            for (long i = 0; i < (long)src->len; i++) {
                if (start + i < a->len) a->ptr[start + i] = src->ptr[i];
            }
        } else {
            /* a[start, len] = single value: replace range with [val] */
            for (long i = start; i < start + len && i < a->len; i++) {
                a->ptr[i] = val;
            }
        }
        return argv[2];
    }
    return Qnil;
}
static VALUE ary_push(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) korb_ary_push(self, argv[i]);
    return self;
}
static VALUE ary_pop(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_pop(self);
}
static VALUE ary_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_aref(self, 0);
}
static VALUE ary_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    return korb_ary_aref(self, len - 1);
}
static VALUE ary_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_each_with_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE args[2] = { korb_ary_aref(self, i), INT2FIX(i) };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(r, m);
    }
    return r;
}
static VALUE ary_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new();
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(r, v);
    }
    return r;
}
static VALUE ary_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE acc = argc > 0 ? argv[0] : korb_ary_aref(self, 0);
    long i = argc > 0 ? 0 : 1;
    for (; i < len; i++) {
        VALUE args[2] = { acc, korb_ary_aref(self, i) };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}
static VALUE ary_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE sep = argc > 0 ? argv[0] : korb_str_new_cstr("");
    VALUE r = korb_str_new("", 0);
    for (long i = 0; i < len; i++) {
        if (i > 0 && BUILTIN_TYPE(sep) == T_STRING) korb_str_concat(r, sep);
        VALUE v = korb_ary_aref(self, i);
        if (BUILTIN_TYPE(v) != T_STRING) v = korb_to_s(v);
        korb_str_concat(r, v);
    }
    return r;
}
static VALUE ary_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_inspect(self);
}
static VALUE ary_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qfalse;
    long la = korb_ary_len(self), lb = korb_ary_len(argv[0]);
    if (la != lb) return Qfalse;
    for (long i = 0; i < la; i++) {
        if (!korb_eq(korb_ary_aref(self, i), korb_ary_aref(argv[0], i))) return Qfalse;
    }
    return Qtrue;
}
static VALUE ary_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    korb_ary_push(self, argv[0]);
    return self;
}
static VALUE ary_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(len);
    for (long i = 0; i < len; i++) korb_ary_push(r, korb_ary_aref(self, i));
    return r;
}


/* ---------- Array methods (extended) ---------- */

static VALUE ary_sort(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    /* shallow copy */
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    /* simple insertion sort using <=> via < */
    struct korb_array *ra = (struct korb_array *)r;
    for (long i = 1; i < n; i++) {
        VALUE v = ra->ptr[i];
        long j = i - 1;
        while (j >= 0) {
            VALUE comp;
            if (FIXNUM_P(ra->ptr[j]) && FIXNUM_P(v)) {
                if ((intptr_t)v >= (intptr_t)ra->ptr[j]) break;
            } else {
                /* fallback */
                comp = korb_funcall(c, ra->ptr[j], korb_intern("<=>"), 1, &v);
                if (FIXNUM_P(comp) && FIX2LONG(comp) <= 0) break;
                if (!FIXNUM_P(comp)) break;
            }
            ra->ptr[j+1] = ra->ptr[j];
            j--;
        }
        ra->ptr[j+1] = v;
    }
    return r;
}

static VALUE ary_sort_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* yield each, then sort by yielded value */
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    VALUE pairs = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, k);
        korb_ary_push(pair, a->ptr[i]);
        korb_ary_push(pairs, pair);
    }
    /* sort pairs by [0] */
    struct korb_array *p = (struct korb_array *)pairs;
    for (long i = 1; i < n; i++) {
        VALUE pi = p->ptr[i];
        VALUE ki = ((struct korb_array *)pi)->ptr[0];
        long j = i - 1;
        while (j >= 0) {
            VALUE pj = p->ptr[j];
            VALUE kj = ((struct korb_array *)pj)->ptr[0];
            VALUE cmp = korb_funcall(c, kj, korb_intern("<=>"), 1, &ki);
            if (FIXNUM_P(cmp) && FIX2LONG(cmp) <= 0) break;
            p->ptr[j+1] = p->ptr[j];
            j--;
        }
        p->ptr[j+1] = pi;
    }
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, ((struct korb_array *)p->ptr[i])->ptr[1]);
    return r;
}

static VALUE ary_zip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) {
        VALUE tup = korb_ary_new_capa(1 + argc);
        korb_ary_push(tup, a->ptr[i]);
        for (int j = 0; j < argc; j++) {
            if (BUILTIN_TYPE(argv[j]) == T_ARRAY) {
                korb_ary_push(tup, korb_ary_aref(argv[j], i));
            } else korb_ary_push(tup, Qnil);
        }
        korb_ary_push(r, tup);
    }
    return r;
}

static void ary_flatten_into(VALUE r, VALUE src, long depth) {
    struct korb_array *a = (struct korb_array *)src;
    for (long i = 0; i < a->len; i++) {
        if (depth != 0 && BUILTIN_TYPE(a->ptr[i]) == T_ARRAY) {
            ary_flatten_into(r, a->ptr[i], depth - 1);
        } else {
            korb_ary_push(r, a->ptr[i]);
        }
    }
}

static VALUE ary_flatten(CTX *c, VALUE self, int argc, VALUE *argv) {
    long depth = -1;  /* -1 = infinite */
    if (argc >= 1 && FIXNUM_P(argv[0])) depth = FIX2LONG(argv[0]);
    VALUE r = korb_ary_new();
    ary_flatten_into(r, self, depth);
    return r;
}

static VALUE ary_compact(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) if (!NIL_P(a->ptr[i])) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_uniq(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        bool dup = false;
        struct korb_array *ra = (struct korb_array *)r;
        for (long j = 0; j < ra->len; j++) {
            if (korb_eq(ra->ptr[j], a->ptr[i])) { dup = true; break; }
        }
        if (!dup) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) return Qtrue;
    return Qfalse;
}

static VALUE ary_any_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (RTEST(a->ptr[i])) return Qtrue;
    }
    return Qfalse;
}

static VALUE ary_all_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (!RTEST(a->ptr[i])) return Qfalse;
    }
    return Qtrue;
}

static VALUE ary_min(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    for (long i = 1; i < a->len; i++) {
        VALUE cmp = korb_funcall(c, m, korb_intern("<=>"), 1, &a->ptr[i]);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) > 0) m = a->ptr[i];
    }
    return m;
}

static VALUE ary_max(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    for (long i = 1; i < a->len; i++) {
        VALUE cmp = korb_funcall(c, m, korb_intern("<=>"), 1, &a->ptr[i]);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) < 0) m = a->ptr[i];
    }
    return m;
}

static VALUE ary_sum(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE acc = argc > 0 ? argv[0] : INT2FIX(0);
    for (long i = 0; i < a->len; i++) {
        if (FIXNUM_P(acc) && FIXNUM_P(a->ptr[i])) {
            long s;
            if (!__builtin_add_overflow(FIX2LONG(acc), FIX2LONG(a->ptr[i]), &s) && FIXABLE(s))
                acc = INT2FIX(s);
            else acc = korb_int_plus(acc, a->ptr[i]);
        } else {
            acc = korb_funcall(c, acc, korb_intern("+"), 1, &a->ptr[i]);
        }
    }
    return acc;
}

static VALUE ary_each_slice(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i += n) {
        long end = i + n; if (end > a->len) end = a->len;
        VALUE slice = korb_ary_new_capa(end - i);
        for (long j = i; j < end; j++) korb_ary_push(slice, a->ptr[j]);
        korb_yield(c, 1, &slice);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE ary_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* not real Array#step, but stub */
    return self;
}

static VALUE ary_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_ARRAY && korb_eq(self, argv[0]));
}

/* Helpers shared between pack and unpack. */
static long korb_pack_long(VALUE v) {
    if (FIXNUM_P(v)) return FIX2LONG(v);
    return 0;
}
static double korb_pack_double(VALUE v) {
    if (FIXNUM_P(v)) return (double)FIX2LONG(v);
    if (FLONUM_P(v)) return korb_flonum_to_double(v);
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_FLOAT) {
        return ((struct korb_float *)v)->value;
    }
    return 0.0;
}
static int korb_hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return 0;
}

/* Append n bytes (little or big endian write) of `val` to buf. */
static void korb_pack_int_bytes(char *buf, long pos, long val, int nbytes, int big_endian) {
    if (big_endian) {
        for (int i = nbytes - 1; i >= 0; i--) buf[pos + (nbytes - 1 - i)] = (char)((val >> (i * 8)) & 0xff);
    } else {
        for (int i = 0; i < nbytes; i++) buf[pos + i] = (char)((val >> (i * 8)) & 0xff);
    }
}

static VALUE ary_pack(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *fmt = korb_str_cstr(argv[0]);
    long fmt_len = (long)strlen(fmt);
    struct korb_array *a = (struct korb_array *)self;
    /* Build into a growable buffer. */
    long cap = 32, plen = 0;
    char *buf = korb_xmalloc_atomic(cap);
    long src_idx = 0;
    #define PACK_RESERVE(extra) do { \
        while (plen + (extra) > cap) { cap *= 2; buf = korb_xrealloc(buf, cap); } \
    } while (0)
    long fp = 0;
    while (fp < fmt_len) {
        char d = fmt[fp++];
        long count = 1;
        bool star = false;
        if (fp < fmt_len) {
            if (fmt[fp] == '*') { star = true; fp++; }
            else if (fmt[fp] >= '0' && fmt[fp] <= '9') {
                count = 0;
                while (fp < fmt_len && fmt[fp] >= '0' && fmt[fp] <= '9') {
                    count = count * 10 + (fmt[fp] - '0'); fp++;
                }
            }
        }
        switch (d) {
          case 'C': case 'c': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(a->ptr[src_idx++]) : 0;
                PACK_RESERVE(1); buf[plen++] = (char)(v & 0xff);
            }
            break;
          }
          case 'n': case 'v': case 's': case 'S': {
            long n = star ? (a->len - src_idx) : count;
            int big = (d == 'n' || d == 's');  /* 's'/'S' are native, but treat as little-endian here */
            if (d == 'n') big = 1;
            else if (d == 'v') big = 0;
            else big = 0;  /* native LE on x86-64 */
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(a->ptr[src_idx++]) : 0;
                PACK_RESERVE(2); korb_pack_int_bytes(buf, plen, v, 2, big); plen += 2;
            }
            break;
          }
          case 'N': case 'V': case 'l': case 'L': case 'i': case 'I': {
            long n = star ? (a->len - src_idx) : count;
            int big = (d == 'N');  /* V/l/L/i/I native LE */
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(a->ptr[src_idx++]) : 0;
                PACK_RESERVE(4); korb_pack_int_bytes(buf, plen, v, 4, big); plen += 4;
            }
            break;
          }
          case 'q': case 'Q': case 'j': case 'J': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(a->ptr[src_idx++]) : 0;
                PACK_RESERVE(8); korb_pack_int_bytes(buf, plen, v, 8, 0); plen += 8;
            }
            break;
          }
          case 'a': case 'A': case 'Z': {
            VALUE sv = (src_idx < a->len) ? a->ptr[src_idx++] : korb_str_new("", 0);
            const char *s = NULL; long slen = 0;
            if (!SPECIAL_CONST_P(sv) && BUILTIN_TYPE(sv) == T_STRING) {
                s = ((struct korb_string *)sv)->ptr;
                slen = ((struct korb_string *)sv)->len;
            }
            long take;
            if (star) take = slen + (d == 'Z' ? 1 : 0);
            else take = count;
            char pad = (d == 'A') ? ' ' : '\0';
            PACK_RESERVE(take);
            for (long i = 0; i < take; i++) {
                buf[plen++] = (i < slen) ? s[i] : pad;
            }
            break;
          }
          case 'H': case 'h': {
            VALUE sv = (src_idx < a->len) ? a->ptr[src_idx++] : korb_str_new("", 0);
            const char *s = NULL; long slen = 0;
            if (!SPECIAL_CONST_P(sv) && BUILTIN_TYPE(sv) == T_STRING) {
                s = ((struct korb_string *)sv)->ptr;
                slen = ((struct korb_string *)sv)->len;
            }
            long n = star ? slen : count;
            if (n > slen) n = slen;
            long nbytes = (n + 1) / 2;
            PACK_RESERVE(nbytes);
            for (long i = 0; i < nbytes; i++) {
                int hi = korb_hex_digit((unsigned char)s[2*i]);
                int lo = (2*i + 1 < n) ? korb_hex_digit((unsigned char)s[2*i + 1]) : 0;
                if (d == 'H') buf[plen + i] = (char)((hi << 4) | lo);
                else          buf[plen + i] = (char)((lo << 4) | hi);
            }
            plen += nbytes;
            break;
          }
          case 'x': {
            long n = count;
            PACK_RESERVE(n);
            for (long i = 0; i < n; i++) buf[plen++] = '\0';
            break;
          }
          case 'd': case 'D': case 'E': case 'G': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                double v = (src_idx < a->len) ? korb_pack_double(a->ptr[src_idx++]) : 0.0;
                PACK_RESERVE(8); memcpy(buf + plen, &v, 8); plen += 8;
            }
            break;
          }
          case 'f': case 'F': case 'e': case 'g': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                float v = (src_idx < a->len) ? (float)korb_pack_double(a->ptr[src_idx++]) : 0.0f;
                PACK_RESERVE(4); memcpy(buf + plen, &v, 4); plen += 4;
            }
            break;
          }
          case ' ': case '\t': case '\n':
            break;  /* whitespace ignored */
          default:
            /* Unknown directive: skip silently (Ruby raises but we log). */
            break;
        }
    }
    #undef PACK_RESERVE
    return korb_str_new(buf, plen);
}

static VALUE str_unpack(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new();
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return r;
    const char *fmt = korb_str_cstr(argv[0]);
    long fmt_len = (long)strlen(fmt);
    struct korb_string *s = (struct korb_string *)self;
    const unsigned char *src = (const unsigned char *)s->ptr;
    long src_len = s->len;
    long src_idx = 0;
    long fp = 0;
    while (fp < fmt_len) {
        char d = fmt[fp++];
        long count = 1;
        bool star = false;
        if (fp < fmt_len) {
            if (fmt[fp] == '*') { star = true; fp++; }
            else if (fmt[fp] >= '0' && fmt[fp] <= '9') {
                count = 0;
                while (fp < fmt_len && fmt[fp] >= '0' && fmt[fp] <= '9') {
                    count = count * 10 + (fmt[fp] - '0'); fp++;
                }
            }
        }
        switch (d) {
          case 'C': case 'c': {
            long n = star ? (src_len - src_idx) : count;
            for (long i = 0; i < n && src_idx < src_len; i++) {
                int b = src[src_idx++];
                if (d == 'c' && b >= 128) b -= 256;
                korb_ary_push(r, INT2FIX(b));
            }
            break;
          }
          case 'n': case 'v': case 's': case 'S': {
            long n = star ? ((src_len - src_idx) / 2) : count;
            int big = (d == 'n');
            for (long i = 0; i < n && src_idx + 2 <= src_len; i++) {
                long v;
                if (big) v = ((long)src[src_idx] << 8) | src[src_idx + 1];
                else     v = src[src_idx] | ((long)src[src_idx + 1] << 8);
                src_idx += 2;
                if (d == 's' && v >= 0x8000) v -= 0x10000;
                korb_ary_push(r, INT2FIX(v));
            }
            break;
          }
          case 'N': case 'V': case 'l': case 'L': case 'i': case 'I': {
            long n = star ? ((src_len - src_idx) / 4) : count;
            int big = (d == 'N');
            for (long i = 0; i < n && src_idx + 4 <= src_len; i++) {
                long v;
                if (big) v = ((long)src[src_idx] << 24) | ((long)src[src_idx + 1] << 16)
                           | ((long)src[src_idx + 2] << 8)  |  (long)src[src_idx + 3];
                else     v = (long)src[src_idx] | ((long)src[src_idx + 1] << 8)
                           | ((long)src[src_idx + 2] << 16) | ((long)src[src_idx + 3] << 24);
                src_idx += 4;
                if (d == 'l' && v >= 0x80000000L) v -= 0x100000000L;
                korb_ary_push(r, INT2FIX(v));
            }
            break;
          }
          case 'q': case 'Q': case 'j': case 'J': {
            long n = star ? ((src_len - src_idx) / 8) : count;
            for (long i = 0; i < n && src_idx + 8 <= src_len; i++) {
                long v = 0;
                for (int b = 0; b < 8; b++) v |= ((long)src[src_idx + b]) << (b * 8);
                src_idx += 8;
                korb_ary_push(r, INT2FIX(v));
            }
            break;
          }
          case 'a': case 'A': case 'Z': {
            long n = star ? (src_len - src_idx) : count;
            if (n > src_len - src_idx) n = src_len - src_idx;
            long real = n;
            if (d == 'A') {
                while (real > 0 && (src[src_idx + real - 1] == ' ' ||
                                    src[src_idx + real - 1] == '\0')) real--;
            } else if (d == 'Z') {
                long z = 0;
                while (z < n && src[src_idx + z] != '\0') z++;
                real = z;
                /* still consume the null if present */
                if (z < n) n = z + 1;
            }
            korb_ary_push(r, korb_str_new((const char *)(src + src_idx), real));
            src_idx += n;
            break;
          }
          case 'H': case 'h': {
            long n = star ? (2 * (src_len - src_idx)) : count;
            long bytes_needed = (n + 1) / 2;
            if (bytes_needed > src_len - src_idx) bytes_needed = src_len - src_idx;
            char *out = korb_xmalloc_atomic(n + 1);
            long o = 0;
            for (long i = 0; i < bytes_needed && o < n; i++) {
                unsigned char b = src[src_idx + i];
                int hi = (b >> 4) & 0xf, lo = b & 0xf;
                static const char *hex = "0123456789abcdef";
                if (d == 'H') {
                    out[o++] = hex[hi];
                    if (o < n) out[o++] = hex[lo];
                } else {
                    out[o++] = hex[lo];
                    if (o < n) out[o++] = hex[hi];
                }
            }
            out[o] = 0;
            korb_ary_push(r, korb_str_new(out, o));
            src_idx += bytes_needed;
            break;
          }
          case 'x':
            src_idx += count;
            break;
          case 'd': case 'D': case 'E': case 'G': {
            long n = star ? ((src_len - src_idx) / 8) : count;
            for (long i = 0; i < n && src_idx + 8 <= src_len; i++) {
                double v;
                memcpy(&v, src + src_idx, 8);
                src_idx += 8;
                korb_ary_push(r, korb_float_new(v));
            }
            break;
          }
          case 'f': case 'F': case 'e': case 'g': {
            long n = star ? ((src_len - src_idx) / 4) : count;
            for (long i = 0; i < n && src_idx + 4 <= src_len; i++) {
                float v;
                memcpy(&v, src + src_idx, 4);
                src_idx += 4;
                korb_ary_push(r, korb_float_new((double)v));
            }
            break;
          }
          case ' ': case '\t': case '\n':
            break;
          default:
            break;
        }
    }
    return r;
}

static VALUE ary_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) == T_ARRAY) {
            struct korb_array *o = (struct korb_array *)argv[i];
            for (long j = 0; j < o->len; j++) korb_ary_push(self, o->ptr[j]);
        }
    }
    return self;
}

static VALUE ary_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_ARRAY) return korb_ary_new();
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        bool found = false;
        for (long j = 0; j < b->len; j++) if (korb_eq(a->ptr[i], b->ptr[j])) { found = true; break; }
        if (!found) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    extern struct korb_proc *current_block;
    if (argc < 1 && current_block) {
        for (long i = 0; i < a->len; i++) {
            VALUE r = korb_yield(c, 1, &a->ptr[i]);
            if (c->state == KORB_RAISE) return Qnil;
            if (!NIL_P(r) && r != Qfalse) return INT2FIX(i);
        }
        return Qnil;
    }
    if (argc < 1) return Qnil;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) return INT2FIX(i);
    return Qnil;
}

static VALUE ary_reverse(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = a->len - 1; i >= 0; i--) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_rotate_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len <= 1) return self;
    long n = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 1;
    long len = a->len;
    n = n % len;
    if (n < 0) n += len;
    if (n == 0) return self;
    /* Half rotate: swap halves directly — covers the optcarrot hot path
     * `@bg_pixels.rotate!(8)` where @bg_pixels has 16 elements (rotate by
     * half).  Memcpy through a stack buffer is one fewer pass than the
     * 3-reverse trick. */
    if (n + n == len && len <= 64) {
        VALUE tmp[32];
        long half = n;
        memcpy(tmp,        a->ptr,        half * sizeof(VALUE));
        memcpy(a->ptr,     a->ptr + half, half * sizeof(VALUE));
        memcpy(a->ptr + half, tmp,        half * sizeof(VALUE));
        return self;
    }
    /* General rotate left by n: 3-reverse trick (no extra alloc, GC safe). */
    /* reverse [0..n-1] */
    for (long i = 0, j = n - 1; i < j; i++, j--) { VALUE t = a->ptr[i]; a->ptr[i] = a->ptr[j]; a->ptr[j] = t; }
    /* reverse [n..len-1] */
    for (long i = n, j = len - 1; i < j; i++, j--) { VALUE t = a->ptr[i]; a->ptr[i] = a->ptr[j]; a->ptr[j] = t; }
    /* reverse [0..len-1] */
    for (long i = 0, j = len - 1; i < j; i++, j--) { VALUE t = a->ptr[i]; a->ptr[i] = a->ptr[j]; a->ptr[j] = t; }
    return self;
}

static VALUE ary_rotate(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long n = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 1;
    if (a->len == 0) return korb_ary_new();
    n = n % a->len;
    if (n < 0) n += a->len;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[(i + n) % a->len]);
    return r;
}

static VALUE ary_reverse_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0, j = a->len - 1; i < j; i++, j--) {
        VALUE t = a->ptr[i]; a->ptr[i] = a->ptr[j]; a->ptr[j] = t;
    }
    return self;
}

static VALUE ary_clear(CTX *c, VALUE self, int argc, VALUE *argv) {
    ((struct korb_array *)self)->len = 0;
    return self;
}

static VALUE ary_unshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    /* shift right argc times */
    long oldlen = a->len;
    for (int i = 0; i < argc; i++) korb_ary_push(self, Qnil);
    for (long i = oldlen - 1; i >= 0; i--) a->ptr[i + argc] = a->ptr[i];
    for (int i = 0; i < argc; i++) a->ptr[i] = argv[i];
    return self;
}

static VALUE ary_shift(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE v = a->ptr[0];
    for (long i = 0; i + 1 < a->len; i++) a->ptr[i] = a->ptr[i+1];
    a->len--;
    return v;
}

static VALUE ary_transpose(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return korb_ary_new();
    /* All inner arrays must be same length */
    long n_outer = a->len;
    long n_inner = (BUILTIN_TYPE(a->ptr[0]) == T_ARRAY) ? ((struct korb_array *)a->ptr[0])->len : 0;
    VALUE r = korb_ary_new_capa(n_inner);
    for (long i = 0; i < n_inner; i++) {
        VALUE row = korb_ary_new_capa(n_outer);
        for (long j = 0; j < n_outer; j++) {
            VALUE inner = a->ptr[j];
            if (BUILTIN_TYPE(inner) == T_ARRAY && i < ((struct korb_array *)inner)->len) {
                korb_ary_push(row, ((struct korb_array *)inner)->ptr[i]);
            } else korb_ary_push(row, Qnil);
        }
        korb_ary_push(r, row);
    }
    return r;
}

static VALUE ary_count(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    extern struct korb_proc *current_block;
    if (argc == 0 && current_block) {
        long n = 0;
        for (long i = 0; i < a->len; i++) {
            VALUE r = korb_yield(c, 1, &a->ptr[i]);
            if (c->state == KORB_RAISE) return Qnil;
            if (!NIL_P(r) && r != Qfalse) n++;
        }
        return INT2FIX(n);
    }
    if (argc == 0) return INT2FIX(a->len);
    long n = 0;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) n++;
    return INT2FIX(n);
}

static VALUE ary_drop(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (n < 0) n = 0;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(a->len - n);
    for (long i = n; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_take(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (n < 0) n = 0;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_fill(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Array#fill(val) — fill all slots with val */
    if (argc < 1) return self;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) a->ptr[i] = argv[0];
    return self;
}

static VALUE ary_sample(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    return a->ptr[0]; /* deterministic stub */
}

static VALUE ary_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_array *)self)->len == 0);
}

static VALUE ary_find(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE m = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) return a->ptr[i];
    }
    return Qnil;
}

static VALUE ary_min_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    VALUE mk = korb_yield(c, 1, &m);
    if (c->state != KORB_NORMAL) return Qnil;
    for (long i = 1; i < a->len; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE cmp = korb_funcall(c, mk, korb_intern("<=>"), 1, &k);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) > 0) { m = a->ptr[i]; mk = k; }
    }
    return m;
}

static VALUE ary_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Array#* — n: repeat, str: join */
    if (argc < 1) return self;
    struct korb_array *a = (struct korb_array *)self;
    if (FIXNUM_P(argv[0])) {
        long n = FIX2LONG(argv[0]);
        if (n < 0) n = 0;
        VALUE r = korb_ary_new_capa(a->len * n);
        for (long i = 0; i < n; i++)
            for (long j = 0; j < a->len; j++) korb_ary_push(r, a->ptr[j]);
        return r;
    }
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        /* join with sep */
        VALUE r = korb_str_new("", 0);
        for (long i = 0; i < a->len; i++) {
            if (i > 0) korb_str_concat(r, argv[0]);
            VALUE s = a->ptr[i];
            if (BUILTIN_TYPE(s) != T_STRING) s = korb_to_s(s);
            korb_str_concat(r, s);
        }
        return r;
    }
    return self;
}

static VALUE ary_max_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    VALUE mk = korb_yield(c, 1, &m);
    if (c->state != KORB_NORMAL) return Qnil;
    for (long i = 1; i < a->len; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE cmp = korb_funcall(c, mk, korb_intern("<=>"), 1, &k);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) < 0) { m = a->ptr[i]; mk = k; }
    }
    return m;
}

static VALUE ary_slice_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Array#slice!(start, len) — remove and return that range */
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long start = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (start < 0) start += a->len;
    long len = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 1;
    if (start < 0 || start >= a->len) return Qnil;
    if (start + len > a->len) len = a->len - start;
    if (len < 0) len = 0;
    VALUE r = korb_ary_new_capa(len);
    for (long i = 0; i < len; i++) korb_ary_push(r, a->ptr[start + i]);
    /* shift remaining */
    for (long i = start; i + len < a->len; i++) a->ptr[i] = a->ptr[i + len];
    a->len -= len;
    return r;
}

static VALUE ary_each_with_object(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE memo = argv[0];
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE args[2] = { a->ptr[i], memo };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return memo;
}


/* ---------- Array#hash (content-based) ---------- */
/* FNV-1a-style mix over each element's hash.  For FIXNUM/SYMBOL/special
 * we use the value bits directly; for heap objects we use the address
 * (stable for the lifetime of the array, matches Ruby's behavior closely
 * enough for `[1,2].hash == [1,2].hash` to hold). */
/* Array#assoc — find a sub-array whose first element == arg. */
static VALUE ary_assoc(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE e = a->ptr[i];
        if (BUILTIN_TYPE(e) == T_ARRAY && ((struct korb_array *)e)->len > 0 &&
            korb_eq(((struct korb_array *)e)->ptr[0], argv[0])) {
            return e;
        }
    }
    return Qnil;
}

/* Array#rassoc — same but matches the second element. */
static VALUE ary_rassoc(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE e = a->ptr[i];
        if (BUILTIN_TYPE(e) == T_ARRAY && ((struct korb_array *)e)->len > 1 &&
            korb_eq(((struct korb_array *)e)->ptr[1], argv[0])) {
            return e;
        }
    }
    return Qnil;
}

/* Array#at(i) — like a[i] for a single integer index. */
static VALUE ary_at(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    return korb_ary_aref(self, FIX2LONG(argv[0]));
}

/* Array#delete(obj) — remove all == matches; return obj if found else nil. */
static VALUE ary_delete(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long w = 0;
    bool found = false;
    for (long r = 0; r < a->len; r++) {
        if (korb_eq(a->ptr[r], argv[0])) {
            found = true;
        } else {
            a->ptr[w++] = a->ptr[r];
        }
    }
    a->len = w;
    return found ? argv[0] : Qnil;
}

/* Array#delete_at(i) — remove element at i, return removed or nil. */
static VALUE ary_delete_at(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(argv[0]);
    if (i < 0) i += a->len;
    if (i < 0 || i >= a->len) return Qnil;
    VALUE r = a->ptr[i];
    for (long j = i; j + 1 < a->len; j++) a->ptr[j] = a->ptr[j + 1];
    a->len--;
    return r;
}

/* Array#delete_if { |x| ... } — remove where block returns truthy. */
static VALUE ary_delete_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long w = 0;
    for (long r = 0; r < a->len; r++) {
        VALUE elt = a->ptr[r];
        VALUE drop = korb_yield(c, 1, &elt);
        if (c->state == KORB_RAISE) return Qnil;
        if (NIL_P(drop) || drop == Qfalse) {
            a->ptr[w++] = elt;
        }
    }
    a->len = w;
    return self;
}

/* Array#reject { |x| ... } — like delete_if but returns a new array. */
static VALUE ary_reject(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        VALUE drop = korb_yield(c, 1, &a->ptr[i]);
        if (c->state == KORB_RAISE) return Qnil;
        if (NIL_P(drop) || drop == Qfalse) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

/* Array#insert(i, *elts) — splice elts into self starting at i. */
static VALUE ary_insert(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(argv[0]);
    if (i < 0) i += a->len + 1;
    if (i < 0) i = 0;
    long ins = argc - 1;
    if (ins == 0) return self;
    while (a->len < i) korb_ary_push(self, Qnil);
    for (long k = 0; k < ins; k++) korb_ary_push(self, Qnil);
    for (long k = a->len - 1; k >= i + ins; k--) a->ptr[k] = a->ptr[k - ins];
    for (long k = 0; k < ins; k++) a->ptr[i + k] = argv[1 + k];
    return self;
}

/* Array#replace(other) — destructive replace. */
static VALUE ary_replace(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return self;
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    a->len = 0;
    for (long i = 0; i < b->len; i++) korb_ary_push(self, b->ptr[i]);
    return self;
}

/* Array#each_index { |i| ... } — yields successive indices. */
static VALUE ary_each_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE iv = INT2FIX(i);
        korb_yield(c, 1, &iv);
        if (c->state == KORB_RAISE) return Qnil;
    }
    return self;
}

/* Array#clone — shallow copy (same as dup for our purposes). */
static VALUE ary_clone(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* Array#eql? — for our impl, same as ==. */
static VALUE ary_eql(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* eql? is type-strict, including for elements: [1, 2.0].eql?([1, 2])
     * is false because 2.0.eql?(2) is false. */
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qfalse;
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    if (a->len != b->len) return Qfalse;
    for (long i = 0; i < a->len; i++) {
        VALUE r = korb_funcall(c, a->ptr[i], korb_intern("eql?"), 1, &b->ptr[i]);
        if (!RTEST(r)) return Qfalse;
    }
    return Qtrue;
}

/* Array#<=> — lexical comparison. */
static VALUE ary_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    long n = a->len < b->len ? a->len : b->len;
    for (long i = 0; i < n; i++) {
        VALUE r = korb_funcall(c, a->ptr[i], korb_intern("<=>"), 1, &b->ptr[i]);
        if (!FIXNUM_P(r) || FIX2LONG(r) != 0) return r;
    }
    if (a->len == b->len) return INT2FIX(0);
    return INT2FIX(a->len < b->len ? -1 : 1);
}

/* Helpers / impl for combination + permutation. */
static void ary_combine(CTX *c, struct korb_array *a, long r, long start,
                         VALUE buf, VALUE result_or_nil) {
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(r);
        struct korb_array *bb = (struct korb_array *)buf;
        for (long i = 0; i < bb->len; i++) korb_ary_push(copy, bb->ptr[i]);
        if (NIL_P(result_or_nil)) korb_yield(c, 1, &copy);
        else korb_ary_push(result_or_nil, copy);
        return;
    }
    for (long i = start; i < a->len; i++) {
        korb_ary_push(buf, a->ptr[i]);
        ary_combine(c, a, r, i + 1, buf, result_or_nil);
        ((struct korb_array *)buf)->len--;
    }
}

static VALUE ary_combination(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (r < 0 || r > a->len) return korb_ary_new();
    VALUE buf = korb_ary_new_capa(r);
    extern struct korb_proc *current_block;
    if (current_block) {
        ary_combine(c, a, r, 0, buf, Qnil);
        return self;
    }
    VALUE result = korb_ary_new();
    ary_combine(c, a, r, 0, buf, result);
    return result;
}

static void ary_perm(CTX *c, struct korb_array *a, long r,
                      VALUE used, VALUE buf, VALUE result_or_nil) {
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(r);
        struct korb_array *bb = (struct korb_array *)buf;
        for (long i = 0; i < bb->len; i++) korb_ary_push(copy, bb->ptr[i]);
        if (NIL_P(result_or_nil)) korb_yield(c, 1, &copy);
        else korb_ary_push(result_or_nil, copy);
        return;
    }
    struct korb_array *uu = (struct korb_array *)used;
    for (long i = 0; i < a->len; i++) {
        if (uu->ptr[i] == Qtrue) continue;
        uu->ptr[i] = Qtrue;
        korb_ary_push(buf, a->ptr[i]);
        ary_perm(c, a, r, used, buf, result_or_nil);
        ((struct korb_array *)buf)->len--;
        uu->ptr[i] = Qfalse;
    }
}

static VALUE ary_permutation(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long r = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : a->len;
    if (r < 0 || r > a->len) return korb_ary_new();
    VALUE used = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(used, Qfalse);
    VALUE buf = korb_ary_new_capa(r);
    extern struct korb_proc *current_block;
    if (current_block) {
        ary_perm(c, a, r, used, buf, Qnil);
        return self;
    }
    VALUE result = korb_ary_new();
    ary_perm(c, a, r, used, buf, result);
    return result;
}

/* Array#product(*others) — Cartesian product. */
static VALUE ary_product(CTX *c, VALUE self, int argc, VALUE *argv) {
    long n = argc + 1;
    struct korb_array **arrays = korb_xmalloc(sizeof(*arrays) * n);
    arrays[0] = (struct korb_array *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_ARRAY) return Qnil;
        arrays[i + 1] = (struct korb_array *)argv[i];
    }
    VALUE result = korb_ary_new();
    long *idx = korb_xcalloc(n, sizeof(long));
    while (true) {
        VALUE row = korb_ary_new_capa(n);
        bool empty = false;
        for (long i = 0; i < n; i++) {
            if (arrays[i]->len == 0) { empty = true; break; }
            korb_ary_push(row, arrays[i]->ptr[idx[i]]);
        }
        if (empty) break;
        korb_ary_push(result, row);
        long j = n - 1;
        while (j >= 0) {
            idx[j]++;
            if (idx[j] < arrays[j]->len) break;
            idx[j] = 0;
            j--;
        }
        if (j < 0) break;
    }
    return result;
}

/* Array.new(size = 0, default = nil) — create an array of `size` slots
 * pre-filled with `default`, or, if a block is given, with the block's
 * return value for each index. */
static VALUE ary_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    long size = 0;
    VALUE def = Qnil;
    if (argc >= 1 && FIXNUM_P(argv[0])) size = FIX2LONG(argv[0]);
    if (argc >= 2) def = argv[1];
    /* Single-array-arg form: Array.new([1,2,3]) — copy. */
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *src = (struct korb_array *)argv[0];
        VALUE r = korb_ary_new_capa(src->len);
        for (long i = 0; i < src->len; i++) korb_ary_push(r, src->ptr[i]);
        return r;
    }
    if (size < 0) size = 0;
    VALUE arr = korb_ary_new_capa(size);
    extern struct korb_proc *current_block;
    if (current_block) {
        for (long i = 0; i < size; i++) {
            VALUE iv = INT2FIX(i);
            VALUE v = korb_yield(c, 1, &iv);
            if (c->state == KORB_RAISE) return Qnil;
            korb_ary_push(arr, v);
        }
    } else {
        for (long i = 0; i < size; i++) korb_ary_push(arr, def);
    }
    return arr;
}

VALUE ary_hash_content(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_ARRAY) return INT2FIX(0);
    struct korb_array *a = (struct korb_array *)self;
    uint64_t h = 0xcbf29ce484222325ULL;  /* FNV-1a init */
    for (long i = 0; i < a->len; i++) {
        VALUE elt = a->ptr[i];
        uint64_t eh;
        if (FIXNUM_P(elt) || SYMBOL_P(elt) || NIL_P(elt) || TRUE_P(elt) || FALSE_P(elt)) {
            eh = (uint64_t)elt;
        } else if (FLONUM_P(elt)) {
            eh = (uint64_t)elt;
        } else if (BUILTIN_TYPE(elt) == T_STRING) {
            /* hash by content for strings */
            struct korb_string *s = (struct korb_string *)elt;
            eh = 0xcbf29ce484222325ULL;
            for (long j = 0; j < s->len; j++) {
                eh ^= (uint64_t)(unsigned char)s->ptr[j];
                eh *= 0x100000001b3ULL;
            }
        } else {
            eh = (uint64_t)elt;
        }
        h ^= eh;
        h *= 0x100000001b3ULL;
    }
    /* Drop the top bit so the result fits in a signed long → FIXNUM. */
    long r = (long)(h & 0x7fffffffffffffffULL);
    return INT2FIX(r >> 1);
}

/* ---------- Array#dig ----------
 * Walks a chain of indices: a.dig(i, j, k) == a[i][j][k], returning nil
 * the moment any intermediate is nil.  After the first hop it dispatches
 * the rest via #dig so Hash/Struct chains compose. */
static VALUE ary_dig(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments to dig (0 for 1+)");
        return Qnil;
    }
    VALUE first = korb_ary_aref(self, FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 0);
    if (argc == 1) return first;
    if (NIL_P(first)) return Qnil;
    return korb_funcall(c, first, korb_intern("dig"), argc - 1, argv + 1);
}

/* ---------- Array#take_while / drop_while ---------- */
static VALUE ary_take_while(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        VALUE m = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (!RTEST(m)) break;
        korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_drop_while(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long i = 0;
    for (; i < a->len; i++) {
        VALUE m = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (!RTEST(m)) break;
    }
    VALUE r = korb_ary_new();
    for (; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* ---------- Array#flat_map ----------
 * Concatenates one level of nesting: if the block returns an Array the
 * elements are appended; otherwise the value itself is appended.
 * Previously aliased to #map, which is wrong for the common
 * `[[1,2],[3,4]].flat_map { |x| x }` shape. */
static VALUE ary_flat_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        VALUE m = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (!SPECIAL_CONST_P(m) && BUILTIN_TYPE(m) == T_ARRAY) {
            struct korb_array *ma = (struct korb_array *)m;
            for (long j = 0; j < ma->len; j++) korb_ary_push(r, ma->ptr[j]);
        } else {
            korb_ary_push(r, m);
        }
    }
    return r;
}

/* ---------- first(n) / last(n) overloads ----------
 * Existing ary_first/ary_last only handle the zero-arg form.  The
 * one-arg form returns up to n leading / trailing elements as a new
 * array; n > size yields the whole array, n == 0 an empty array. */
static VALUE ary_first_n(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (argc < 1) return a->len == 0 ? Qnil : a->ptr[0];
    if (!FIXNUM_P(argv[0])) {
        korb_raise(c, NULL, "first: non-Integer argument");
        return Qnil;
    }
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        korb_raise(c, NULL, "negative array size");
        return Qnil;
    }
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_last_n(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (argc < 1) return a->len == 0 ? Qnil : a->ptr[a->len - 1];
    if (!FIXNUM_P(argv[0])) {
        korb_raise(c, NULL, "last: non-Integer argument");
        return Qnil;
    }
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        korb_raise(c, NULL, "negative array size");
        return Qnil;
    }
    if (n > a->len) n = a->len;
    long start = a->len - n;
    VALUE r = korb_ary_new_capa(n);
    for (long i = start; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* ---------- Array#shuffle ----------
 * Fisher–Yates over a copy.  Uses rand(3); good enough for tests and the
 * occasional `.sample` cousin (already implemented).  Doesn't mutate self. */
static VALUE ary_shuffle(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    struct korb_array *ra = (struct korb_array *)r;
    for (long i = ra->len - 1; i > 0; i--) {
        long j = (long)(((unsigned long)rand()) % (unsigned long)(i + 1));
        VALUE tmp = ra->ptr[i];
        ra->ptr[i] = ra->ptr[j];
        ra->ptr[j] = tmp;
    }
    return r;
}

/* ---------- Array#one? ----------
 * Without a block: true iff exactly one element is truthy.
 * With a block: true iff the block returns truthy for exactly one element. */
static VALUE ary_one_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long count = 0;
    for (long i = 0; i < a->len; i++) {
        VALUE v;
        if (korb_block_given()) {
            v = korb_yield(c, 1, &a->ptr[i]);
            if (c->state != KORB_NORMAL) return Qnil;
        } else {
            v = a->ptr[i];
        }
        if (RTEST(v)) {
            count++;
            if (count > 1) return Qfalse;
        }
    }
    return KORB_BOOL(count == 1);
}

/* ---------- Array#each_cons(n) ----------
 * Sliding window of size n.  No block: returns Array<Array> of all
 * windows (koruby has no Enumerator).  With block: yields each window. */
static VALUE ary_each_cons(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    bool has_block = korb_block_given();
    VALUE out = has_block ? Qnil : korb_ary_new();
    if (n <= 0 || n > a->len) return has_block ? Qnil : out;
    for (long i = 0; i + n <= a->len; i++) {
        VALUE win = korb_ary_new_capa(n);
        for (long j = 0; j < n; j++) korb_ary_push(win, a->ptr[i + j]);
        if (has_block) {
            korb_yield(c, 1, &win);
            if (c->state != KORB_NORMAL) return Qnil;
        } else {
            korb_ary_push(out, win);
        }
    }
    return has_block ? Qnil : out;
}

/* ---------- Array#minmax_by ----------
 * Returns [min_elem, max_elem] keyed by the block's return value;
 * [nil, nil] for an empty array. */
static VALUE ary_minmax_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE pair = korb_ary_new_capa(2);
    if (a->len == 0) {
        korb_ary_push(pair, Qnil);
        korb_ary_push(pair, Qnil);
        return pair;
    }
    VALUE min_e = a->ptr[0], max_e = a->ptr[0];
    VALUE min_k = korb_yield(c, 1, &a->ptr[0]);
    if (c->state != KORB_NORMAL) return Qnil;
    VALUE max_k = min_k;
    for (long i = 1; i < a->len; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE cmp_min = korb_funcall(c, k, korb_intern("<=>"), 1, &min_k);
        if (FIXNUM_P(cmp_min) && FIX2LONG(cmp_min) < 0) { min_e = a->ptr[i]; min_k = k; }
        VALUE cmp_max = korb_funcall(c, k, korb_intern("<=>"), 1, &max_k);
        if (FIXNUM_P(cmp_max) && FIX2LONG(cmp_max) > 0) { max_e = a->ptr[i]; max_k = k; }
    }
    korb_ary_push(pair, min_e);
    korb_ary_push(pair, max_e);
    return pair;
}

/* ---------- Array#bsearch ----------
 * Find-minimum mode only (block returns boolean).  Assumes the array is
 * sorted and the block result transitions from false to true exactly
 * once; returns the first true element, nil if all are false. */
static VALUE ary_bsearch(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long lo = 0, hi = a->len;
    VALUE found = Qnil;
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        VALUE r = korb_yield(c, 1, &a->ptr[mid]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(r)) {
            found = a->ptr[mid];
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return found;
}

