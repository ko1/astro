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
            if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
            long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
            if (b < 0) b += a->len;
            if (e < 0) e += a->len;
            if (r->exclude_end) e--;
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

static VALUE ary_pack(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* very limited pack — just "C*" (bytes) */
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *fmt = korb_str_cstr(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (strcmp(fmt, "C*") == 0) {
        char *buf = korb_xmalloc_atomic(a->len + 1);
        for (long i = 0; i < a->len; i++) {
            buf[i] = FIXNUM_P(a->ptr[i]) ? (char)(FIX2LONG(a->ptr[i]) & 0xff) : 0;
        }
        buf[a->len] = 0;
        return korb_str_new(buf, a->len);
    }
    return korb_str_new("", 0);
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
    if (argc < 1) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
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

