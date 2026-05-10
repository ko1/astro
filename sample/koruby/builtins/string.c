/* String — moved from builtins.c. */

/* Forward decl — defined in builtins/array.c which is included after
 * builtins/string.c.  Needed for to_int coerce in #getbyte etc. */
static VALUE korb_to_int_or_raise(CTX *c, VALUE v);
/* Forward decl for the to_str coerce helper (defined further below). */
static VALUE str_coerce_arg(CTX *c, VALUE arg);

/* String.new(s = "") — start the new string from an optional initial
 * value.  Class#new's generic path goes through korb_object_new which
 * doesn't allocate the String storage; we need a real heap String. */
VALUE str_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r;
    /* Drop trailing FL_KWARGS hash (encoding: / capacity: kwargs are
     * accepted but treated as informational). */
    int eff_argc = argc;
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->flags & FL_KWARGS)) {
        eff_argc = argc - 1;
    }
    if (eff_argc >= 1) {
        VALUE init = argv[0];
        if (SPECIAL_CONST_P(init) || BUILTIN_TYPE(init) != T_STRING) {
            if (!SPECIAL_CONST_P(init)) {
                VALUE rt = korb_funcall(c, init, korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
                if (c->state == KORB_RAISE) return Qnil;
                if (RTEST(rt)) {
                    init = korb_funcall(c, init, korb_intern("to_str"), 0, NULL);
                    if (c->state == KORB_RAISE) return Qnil;
                }
            }
            if (SPECIAL_CONST_P(init) || BUILTIN_TYPE(init) != T_STRING) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into String",
                           SPECIAL_CONST_P(argv[0]) ? "(special)"
                               : korb_id_name(korb_class_of_class(argv[0])->name));
                return Qnil;
            }
        }
        struct korb_string *s = (struct korb_string *)init;
        r = korb_str_new(s->ptr, s->len);
    } else {
        r = korb_str_new("", 0);
    }
    /* For String subclasses, retag the result with the subclass so
     * `class BP < String; end; BP.new("a").class == BP`.  Top-level
     * `String.new` keeps the default String klass. */
    if (BUILTIN_TYPE(self) == T_CLASS && (struct korb_class *)self != korb_vm->string_class) {
        ((struct RBasic *)r)->klass = self;
    }
    return r;
}

/* ---------- String ---------- */
static VALUE str_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
        /* Try to_str — TypeError if the object doesn't convert. */
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                other = korb_funcall(c, other, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
    }
    VALUE r = korb_str_dup(self);
    return korb_str_concat(r, other);
}
/* Append a single arg to self.  Returns Qfalse on raise (caller stops). */
static bool str_concat_one(CTX *c, VALUE self, VALUE arg);
/* String#<< — accepts exactly one argument (CRuby semantics).  Variadic
 * version is `concat`. */
static bool str_concat_one(CTX *c, VALUE self, VALUE arg);
static VALUE str_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc != 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
        return Qnil;
    }
    if (!str_concat_one(c, self, argv[0])) return Qnil;
    return self;
}
static VALUE str_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    /* String#concat accepts variadic args, appending each in order.
     * Snapshot any String args that alias self up front, so an arg that
     * happens to be self sees its pre-concat byte content for every
     * append (CRuby semantics: `b.concat(b, b)` triples, not doubles). */
    VALUE local[8];
    VALUE *args = (argc <= 8) ? local : (VALUE *)korb_xmalloc(sizeof(VALUE) * argc);
    for (int i = 0; i < argc; i++) {
        VALUE a = argv[i];
        if (a == self && !SPECIAL_CONST_P(a) && BUILTIN_TYPE(a) == T_STRING) {
            struct korb_string *s = (struct korb_string *)a;
            args[i] = korb_str_new(s->ptr, s->len);
        } else {
            args[i] = a;
        }
    }
    for (int i = 0; i < argc; i++) {
        if (!str_concat_one(c, self, args[i])) return Qnil;
    }
    return self;
}
static bool str_concat_one(CTX *c, VALUE self, VALUE arg) {
    /* `str << int` appends the codepoint as bytes (CRuby semantics).
     * koruby is byte-only; reject negatives and out-of-byte values
     * with RangeError to mirror CRuby for the simple ASCII range, but
     * fall through to a single-byte append when 0..255. */
    if (FIXNUM_P(arg)) {
        long cp = FIX2LONG(arg);
        if (cp < 0) {
            VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
            korb_raise(c, (struct korb_class *)eR,
                       "%ld out of char range", cp);
            return false;
        }
        if (cp <= 0x7f) {
            char ch = (char)cp;
            VALUE tmp = korb_str_new(&ch, 1);
            korb_str_concat(self, tmp);
            return true;
        }
        /* Multi-byte: encode as UTF-8. */
        char buf[6];
        int len;
        if (cp <= 0x7ff) {
            buf[0] = (char)(0xc0 | (cp >> 6));
            buf[1] = (char)(0x80 | (cp & 0x3f));
            len = 2;
        } else if (cp <= 0xffff) {
            buf[0] = (char)(0xe0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
            buf[2] = (char)(0x80 | (cp & 0x3f));
            len = 3;
        } else if (cp <= 0x10ffff) {
            buf[0] = (char)(0xf0 | (cp >> 18));
            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
            buf[3] = (char)(0x80 | (cp & 0x3f));
            len = 4;
        } else {
            VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
            korb_raise(c, (struct korb_class *)eR,
                       "%ld out of char range", cp);
            return false;
        }
        VALUE tmp = korb_str_new(buf, len);
        korb_str_concat(self, tmp);
        return true;
    }
    if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) {
        /* Try to_s as a fallback. */
        VALUE s = korb_funcall(c, arg, korb_intern("to_s"), 0, NULL);
        if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) {
            korb_str_concat(self, s);
            return true;
        }
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion to String");
        return false;
    }
    /* Snapshot the arg's bytes if it might alias self (e.g. `b.concat(b, b)`
     * mutates b mid-call; without a snapshot, the second iteration sees
     * the already-grown buffer and we end up doubling instead of tripling). */
    if (arg == self) {
        struct korb_string *src = (struct korb_string *)arg;
        VALUE snap = korb_str_new(src->ptr, src->len);
        korb_str_concat(self, snap);
        return true;
    }
    korb_str_concat(self, arg);
    return true;
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

/* Reentrancy guard for the inverted-<=> path: if other.<=>(self) re-enters
 * String#<=> with the same pair, return nil rather than recursing. */
static int str_cmp_inverse_depth = 0;
static VALUE str_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                VALUE r = korb_funcall(c, other, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
                if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) {
                    other = r;
                    goto compare_strings;
                }
            }
            if (str_cmp_inverse_depth > 0) return Qnil;
            rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                              (VALUE[]){ korb_id2sym(korb_intern("<=>")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                str_cmp_inverse_depth++;
                VALUE r = korb_funcall(c, other, korb_intern("<=>"), 1, &self);
                str_cmp_inverse_depth--;
                if (c->state == KORB_RAISE) return Qnil;
                if (FIXNUM_P(r)) {
                    long v = FIX2LONG(r);
                    return INT2FIX(v < 0 ? 1 : v > 0 ? -1 : 0);
                }
            }
        }
        return Qnil;
    }
compare_strings:;
    struct korb_string *a = (struct korb_string *)self;
    struct korb_string *b = (struct korb_string *)other;
    long n = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->ptr, b->ptr, n);
    if (r != 0) return INT2FIX(r < 0 ? -1 : 1);
    if (a->len < b->len) return INT2FIX(-1);
    if (a->len > b->len) return INT2FIX(1);
    return INT2FIX(0);
}

/* Raise CRuby's "comparison of String with X failed" ArgumentError when
 * <=> couldn't reach a result. */
static void str_cmp_raise(CTX *c, VALUE other) {
    VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
    VALUE oi = korb_inspect(other);
    const char *o_str = (!SPECIAL_CONST_P(oi) && BUILTIN_TYPE(oi) == T_STRING)
                            ? korb_str_cstr(oi)
                            : korb_id_name(korb_class_of_class(other)->name);
    korb_raise(c, (struct korb_class *)eArg,
               "comparison of String with %s failed", o_str);
}
static VALUE str_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return Qnil; }
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) < 0);
}
static VALUE str_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return Qnil; }
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) <= 0);
}
static VALUE str_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return Qnil; }
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) > 0);
}
static VALUE str_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = str_cmp(c, self, argc, argv);
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return Qnil; }
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) >= 0);
}
static VALUE str_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
/* String#__chilled? — internal: true iff FL_CHILLED is set.  Used by
 * Ruby-level `+@` to decide whether to return a fresh mutable copy. */
static VALUE str_chilled_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self)) return Qfalse;
    return KORB_BOOL((RBASIC(self)->flags & FL_CHILLED) != 0);
}
static VALUE str_to_sym(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_to_sym(self);
}


/* ---------- String formatting / methods (extended) ---------- */

static VALUE str_format_self(CTX *c, VALUE self, int argc, VALUE *argv);

static VALUE str_split(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new();
    bool has_block = korb_block_given();
    /* When a block is given, yield each piece and return self.  CRuby's
     * String#split{|x|} returns the receiver unchanged. */
    #define EMIT(v) do { \
        if (has_block) { VALUE _v_ = (v); korb_yield(c, 1, &_v_); } \
        else korb_ary_push(r, (v)); \
    } while (0)
    if (argc == 0 || NIL_P(argv[0])) {
        /* split on whitespace */
        long i = 0;
        while (i < s->len) {
            while (i < s->len && (s->ptr[i] == ' ' || s->ptr[i] == '\t' || s->ptr[i] == '\n')) i++;
            if (i >= s->len) break;
            long start = i;
            while (i < s->len && s->ptr[i] != ' ' && s->ptr[i] != '\t' && s->ptr[i] != '\n') i++;
            EMIT(korb_str_new(s->ptr + start, i - start));
        }
        return has_block ? self : r;
    }
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return has_block ? self : r;
    struct korb_string *sep = (struct korb_string *)argv[0];
    if (sep->len == 0) {
        for (long i = 0; i < s->len; i++) EMIT(korb_str_new(s->ptr + i, 1));
        return has_block ? self : r;
    }
    long start = 0;
    for (long i = 0; i + sep->len <= s->len; ) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            EMIT(korb_str_new(s->ptr + start, i - start));
            i += sep->len;
            start = i;
        } else i++;
    }
    EMIT(korb_str_new(s->ptr + start, s->len - start));
    #undef EMIT
    return has_block ? self : r;
}

/* Compute the chomp length given an optional argument.
 * Returns the new length (<= s->len).
 *   * No argument: chomp the universal record separator: trailing "\r\n",
 *     "\r", or "\n" (single trailing). $/ is conventionally "\n" and we
 *     don't track $/ assignments, so we always use the universal form.
 *   * nil: don't strip anything (return s->len).
 *   * "": strip all trailing "\r\n" / "\n" pairs/runs but NOT a final "\r".
 *   * String suffix: strip exactly that suffix once if present (using
 *     to_str coerce; TypeError if to_str doesn't return a String). */
static long str_chomp_compute(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long n = s->len;
    if (argc < 1) {
        if (n >= 2 && s->ptr[n-2] == '\r' && s->ptr[n-1] == '\n') return n - 2;
        if (n >= 1 && (s->ptr[n-1] == '\n' || s->ptr[n-1] == '\r')) return n - 1;
        return n;
    }
    VALUE arg = argv[0];
    if (NIL_P(arg)) return n;
    if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) {
        VALUE rt = korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return n;
        if (RTEST(rt)) {
            VALUE r = korb_funcall(c, arg, korb_intern("to_str"), 0, NULL);
            if (c->state == KORB_RAISE) return n;
            if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) {
                arg = r;
                goto have_str;
            }
        }
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into String",
                   SPECIAL_CONST_P(arg) ? "(special)"
                       : korb_id_name(korb_class_of_class(arg)->name));
        return n;
    }
have_str:;
    struct korb_string *p = (struct korb_string *)arg;
    /* Special: chomp("\n") behaves like the no-argument form — strip any
     * trailing "\r\n", "\n", or "\r". */
    if (p->len == 1 && p->ptr[0] == '\n') {
        if (n >= 2 && s->ptr[n-2] == '\r' && s->ptr[n-1] == '\n') return n - 2;
        if (n >= 1 && (s->ptr[n-1] == '\n' || s->ptr[n-1] == '\r')) return n - 1;
        return n;
    }
    if (p->len == 0) {
        /* Empty arg: paragraph mode — strip trailing newline runs (with
         * preceding optional CR), but stop short of stripping a trailing
         * lone "\r" with no "\n" after it. */
        long m = n;
        while (m > 0 && s->ptr[m-1] == '\n') {
            m--;
            if (m > 0 && s->ptr[m-1] == '\r') m--;
        }
        return m;
    }
    if (p->len <= n && memcmp(s->ptr + n - p->len, p->ptr, p->len) == 0) {
        return n - p->len;
    }
    return n;
}

static VALUE str_chomp(CTX *c, VALUE self, int argc, VALUE *argv) {
    long n = str_chomp_compute(c, self, argc, argv);
    if (c->state == KORB_RAISE) return Qnil;
    return korb_str_new(((struct korb_string *)self)->ptr, n);
}

static VALUE str_chomp_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long n = str_chomp_compute(c, self, argc, argv);
    if (c->state == KORB_RAISE) return Qnil;
    if (n == s->len) return Qnil;
    s->len = n;
    if (s->capa > s->len) s->ptr[s->len] = 0;
    return self;
}

/* CRuby's String#strip / #lstrip / #rstrip whitespace set: ASCII space,
 * '\t', '\n', '\v', '\f', '\r', plus '\0' (NUL — only for rstrip and the
 * trailing portion of strip; CRuby strips trailing NUL). */
static inline bool str_is_ws(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
           ch == '\f' || ch == '\r';
}
static inline bool str_is_lstrip_ws(unsigned char ch) {
    /* lstrip also strips leading NUL bytes (CRuby behavior). */
    return str_is_ws(ch) || ch == '\0';
}
static inline bool str_is_rstrip_ws(unsigned char ch) {
    /* rstrip strips ASCII whitespace + trailing NUL. */
    return str_is_ws(ch) || ch == '\0';
}

static VALUE str_strip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    while (end > start && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    return korb_str_new(s->ptr + start, end - start);
}

static VALUE str_lstrip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long start = 0;
    while (start < s->len && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    return korb_str_new(s->ptr + start, s->len - start);
}

static VALUE str_rstrip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long end = s->len;
    while (end > 0 && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    return korb_str_new(s->ptr, end);
}

static VALUE str_lstrip_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long start = 0;
    while (start < s->len && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    if (start == 0) return Qnil;
    long new_len = s->len - start;
    memmove(s->ptr, s->ptr + start, new_len);
    s->len = new_len;
    if (s->capa > new_len) s->ptr[new_len] = 0;
    return self;
}

static VALUE str_rstrip_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long end = s->len;
    while (end > 0 && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    if (end == s->len) return Qnil;
    s->len = end;
    if (s->capa > end) s->ptr[end] = 0;
    return self;
}

static VALUE str_strip_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    while (end > start && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    if (start == 0 && end == s->len) return Qnil;
    long new_len = end - start;
    if (start > 0) memmove(s->ptr, s->ptr + start, new_len);
    s->len = new_len;
    if (s->capa > new_len) s->ptr[new_len] = 0;
    return self;
}

static VALUE str_to_i(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *end;
    long v = strtol(s->ptr, &end, argc > 0 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10);
    return INT2FIX(v);
}

/* CRuby's String#to_f parses a leading numeric literal with the same
 * lexical structure as a Ruby float literal:
 *   * optional ASCII whitespace
 *   * optional sign
 *   * digits with optional underscores between consecutive digits
 *   * optional fractional part (`.<digits>` with the same underscore rule)
 *   * optional exponent (`e[<sign>]<digits>`)
 *   * stops at the first non-matching byte; remaining input is ignored.
 * Hex/oct prefixes are NOT recognized. "Infinity" / "NaN" return 0.0. */
static VALUE str_to_f(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_string *s = (const struct korb_string *)self;
    const char *p = s->ptr, *e = s->ptr + s->len;
    /* Skip leading ASCII whitespace. */
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' ||
                     *p == '\v' || *p == '\f' || *p == '\r')) p++;
    /* Build a sanitized copy: copy bytes that match Ruby's float-literal
     * grammar, dropping underscores between digits. */
    char buf[64];
    long blen = 0;
    if (p < e && (*p == '+' || *p == '-')) { buf[blen++] = *p++; }
    bool saw_digit = false;
    /* Integer part. */
    while (p < e && *p >= '0' && *p <= '9') {
        if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
        saw_digit = true;
        p++;
        if (p < e && *p == '_' && p + 1 < e && p[1] >= '0' && p[1] <= '9') p++;
    }
    /* Fractional part. */
    if (p < e && *p == '.' && p + 1 < e && p[1] >= '0' && p[1] <= '9') {
        if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
        p++;
        while (p < e && *p >= '0' && *p <= '9') {
            if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
            saw_digit = true;
            p++;
            if (p < e && *p == '_' && p + 1 < e && p[1] >= '0' && p[1] <= '9') p++;
        }
    }
    /* Exponent. */
    if (saw_digit && p < e && (*p == 'e' || *p == 'E')) {
        long save_blen = blen;
        const char *save_p = p;
        if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
        p++;
        if (p < e && (*p == '+' || *p == '-')) {
            if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
            p++;
        }
        bool exp_digit = false;
        while (p < e && *p >= '0' && *p <= '9') {
            if (blen + 1 < (long)sizeof(buf)) buf[blen++] = *p;
            exp_digit = true;
            p++;
            if (p < e && *p == '_' && p + 1 < e && p[1] >= '0' && p[1] <= '9') p++;
        }
        if (!exp_digit) { blen = save_blen; p = save_p; }
    }
    if (!saw_digit) return korb_float_new(0.0);
    buf[blen] = '\0';
    return korb_float_new(strtod(buf, NULL));
}

/* String#byteslice — byte-indexed slice.  koruby is byte-only so this
 * is identical to #[] for the integer / range / (idx, len) forms. */
static VALUE str_byteslice(CTX *c, VALUE self, int argc, VALUE *argv);
static VALUE str_append_as_bytes(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Append each arg's bytes to self.  koruby is byte-only so this is
     * a glorified concat that ignores Encoding. */
    for (int i = 0; i < argc; i++) {
        if (FIXNUM_P(argv[i])) {
            char ch = (char)(FIX2LONG(argv[i]) & 0xff);
            VALUE tmp = korb_str_new(&ch, 1);
            korb_str_concat(self, tmp);
        } else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING) {
            korb_str_concat(self, argv[i]);
        }
    }
    return self;
}
static VALUE str_setbyte(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || !FIXNUM_P(argv[0]) || !FIXNUM_P(argv[1])) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    long i = FIX2LONG(argv[0]);
    long b = FIX2LONG(argv[1]);
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) {
        VALUE eI = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
        korb_raise(c, (struct korb_class *)eI, "index %ld out of string", i);
        return Qnil;
    }
    s->ptr[i] = (char)(b & 0xff);
    return argv[1];
}
static VALUE str_getbyte(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
        return Qnil;
    }
    long i;
    if (FIXNUM_P(argv[0])) {
        i = FIX2LONG(argv[0]);
    } else {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return Qnil;
        i = FIX2LONG(iv);
    }
    struct korb_string *s = (struct korb_string *)self;
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) return Qnil;
    return INT2FIX((unsigned char)s->ptr[i]);
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

/* Body for byteslice — same as aref since koruby is byte-only. */
static VALUE str_byteslice(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_aref(c, self, argc, argv);
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
    if (argc < 1) return Qnil;
    /* Coerce arg via #to_str; raises TypeError when arg is not a string-
     * convertible (Integer, etc.).  Note: rindex does NOT call #to_int. */
    VALUE arg = argv[0];
    if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) {
        arg = str_coerce_arg(c, arg);
        if (UNDEF_P(arg) || c->state == KORB_RAISE) return Qnil;
    }
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *needle = (struct korb_string *)arg;
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
    /* Block form: yield each byte to the block, return self. */
    if (korb_block_given()) {
        for (long i = 0; i < s->len; i++) {
            VALUE b = INT2FIX((unsigned char)s->ptr[i]);
            korb_yield(c, 1, &b);
            if (c->state == KORB_RAISE) return Qnil;
        }
        return self;
    }
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
    if (argc < 1) return Qfalse;
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return Qfalse;
            if (RTEST(rt)) {
                other = korb_funcall(c, other, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return Qfalse;
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
            return Qfalse;
        }
    }
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)other;
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

/* Generic in-place case mutator: walks the buffer and applies `xform`
 * to each byte.  Returns self if anything changed, nil otherwise
 * (matching CRuby's `!` semantics).  Frozen-checked. */
static VALUE str_case_bang(CTX * restrict c, VALUE self, char (*xform)(char)) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    /* Buffer may be shared (from a dup or substr); allocate fresh
     * before mutating so we don't trample the source. */
    char *buf = korb_xmalloc_atomic(s->len + 1);
    bool changed = false;
    for (long i = 0; i < s->len; i++) {
        char ch = xform(s->ptr[i]);
        if (ch != s->ptr[i]) changed = true;
        buf[i] = ch;
    }
    buf[s->len] = 0;
    if (!changed) return Qnil;
    s->ptr = buf;
    return self;
}

static char xform_upcase(char ch)   { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }
static char xform_downcase(char ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static char xform_swapcase(char ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch >= 'A' && ch <= 'Z') return ch + 32;
    return ch;
}

static VALUE str_upcase_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_case_bang(c, self, xform_upcase);
}
static VALUE str_downcase_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_case_bang(c, self, xform_downcase);
}
static VALUE str_swapcase_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_case_bang(c, self, xform_swapcase);
}

/* String#capitalize! — first char up, rest down.  Returns self if
 * anything changed, nil otherwise. */
static VALUE str_capitalize_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return Qnil;
    char *buf = korb_xmalloc_atomic(s->len + 1);
    bool changed = false;
    for (long i = 0; i < s->len; i++) {
        char ch = (i == 0) ? xform_upcase(s->ptr[i]) : xform_downcase(s->ptr[i]);
        if (ch != s->ptr[i]) changed = true;
        buf[i] = ch;
    }
    buf[s->len] = 0;
    if (!changed) return Qnil;
    s->ptr = buf;
    return self;
}

/* String#reverse! — reverse in place.  Always returns self (CRuby
 * does too — empty/single-char strings still return self, not nil). */
static VALUE str_reverse_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) buf[i] = s->ptr[s->len - 1 - i];
    buf[s->len] = 0;
    s->ptr = buf;
    return self;
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
    /* Simple checksum: sum of bytes mod (1<<bits), default bits=16. */
    struct korb_string *s = (struct korb_string *)self;
    long bits = 16;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) {
            bits = FIX2LONG(argv[0]);
        } else {
            VALUE iv = korb_to_int_or_raise(c, argv[0]);
            if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return Qnil;
            bits = FIX2LONG(iv);
        }
    }
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

/* gsub! / sub!: in-place mutating variants.  Return self (or nil if
 * no match).  We re-use the gsub/sub implementations to compute the
 * new content and copy it back into self's buffer. */
static VALUE str_gsub_bang(CTX * restrict c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long ms, ml;
    if (argc < 1 || !str_find_pat(argv[0], s, 0, &ms, &ml)) return Qnil;
    VALUE replaced = str_gsub(c, self, argc, argv);
    if (c->state == KORB_RAISE || BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
    const struct korb_string *r = (const struct korb_string *)replaced;
    char *buf = korb_xmalloc_atomic(r->len + 1);
    memcpy(buf, r->ptr, r->len); buf[r->len] = 0;
    s->ptr = buf;
    s->len = r->len;
    return self;
}

static VALUE str_sub_bang(CTX * restrict c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long ms, ml;
    if (argc < 1 || !str_find_pat(argv[0], s, 0, &ms, &ml)) return Qnil;
    VALUE replaced = str_sub(c, self, argc, argv);
    if (c->state == KORB_RAISE || BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
    const struct korb_string *r = (const struct korb_string *)replaced;
    char *buf = korb_xmalloc_atomic(r->len + 1);
    memcpy(buf, r->ptr, r->len); buf[r->len] = 0;
    s->ptr = buf;
    s->len = r->len;
    return self;
}

/* String#scan(pattern) — return Array of all non-overlapping matches.
 * Treats pattern as a literal string when given a String, mirroring
 * koruby's gsub/sub behavior (no regex without astrorge). */
static VALUE str_scan(CTX * restrict c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(self) != T_STRING) return korb_ary_new();
    const struct korb_string *s = (const struct korb_string *)self;
    VALUE out = korb_ary_new();
    long ms, ml, i = 0;
    while (str_find_pat(argv[0], (struct korb_string *)s, i, &ms, &ml)) {
        korb_ary_push(out, korb_str_new(s->ptr + ms, ml));
        i = ms + (ml > 0 ? ml : 1);
    }
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

/* tr! / tr_s!: in-place.  Return self if changed, nil otherwise. */
static VALUE str_tr_bang_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool squeeze) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string * const s = (struct korb_string *)self;
    VALUE replaced = str_tr_impl(self, argc, argv, squeeze);
    if (BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
    const struct korb_string * const r = (const struct korb_string *)replaced;
    if (r->len == s->len && memcmp(r->ptr, s->ptr, s->len) == 0) return Qnil;
    char * const buf = korb_xmalloc_atomic(r->len + 1);
    memcpy(buf, r->ptr, r->len); buf[r->len] = 0;
    s->ptr = buf;
    s->len = r->len;
    return self;
}

static VALUE str_tr_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_tr_bang_impl(c, self, argc, argv, false);
}

static VALUE str_tr_s_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    return str_tr_bang_impl(c, self, argc, argv, true);
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
                long v;
                if (conv == 'c' && ai < argc && !SPECIAL_CONST_P(argv[ai]) &&
                    BUILTIN_TYPE(argv[ai]) == T_STRING) {
                    /* %c with a String arg: use the first byte. */
                    const struct korb_string *cs = (const struct korb_string *)argv[ai];
                    v = cs->len > 0 ? (unsigned char)cs->ptr[0] : 0;
                } else {
                    v = ai < argc && FIXNUM_P(argv[ai]) ? FIX2LONG(argv[ai]) : 0;
                }
                if (conv == 'b') {
                    /* Build raw binary digits, then apply flags/width
                     * manually (snprintf has no %b).  spec format:
                     * `%[flags][width][.prec]b`.  Re-parse the spec
                     * we already built to extract them. */
                    char tmp[256]; int tl = 0;
                    bool neg = (v < 0);
                    unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
                    if (uv == 0) tmp[tl++] = '0';
                    while (uv) { tmp[tl++] = '0' + (uv & 1); uv >>= 1; }
                    for (int j = 0; j < tl/2; j++) { char tch = tmp[j]; tmp[j] = tmp[tl-1-j]; tmp[tl-1-j] = tch; }
                    tmp[tl] = 0;
                    /* Re-parse spec for flags/width/prec.  spec is %[flags][width][.prec]b */
                    bool flag_minus = false, flag_zero = false, flag_hash = false;
                    int width = 0, prec = -1;
                    int p = 1;
                    while (p < sl && (spec[p] == '-' || spec[p] == '+' || spec[p] == ' ' || spec[p] == '#' || spec[p] == '0')) {
                        if (spec[p] == '-') flag_minus = true;
                        if (spec[p] == '0') flag_zero = true;
                        if (spec[p] == '#') flag_hash = true;
                        p++;
                    }
                    while (p < sl && spec[p] >= '0' && spec[p] <= '9') { width = width*10 + (spec[p]-'0'); p++; }
                    if (p < sl && spec[p] == '.') {
                        p++; prec = 0;
                        while (p < sl && spec[p] >= '0' && spec[p] <= '9') { prec = prec*10 + (spec[p]-'0'); p++; }
                    }
                    /* `..1` for negative two's-complement format (CRuby
                     * semantics for `%b` with neg int): add prefix. */
                    char prefix[8]; int pfx_len = 0;
                    if (flag_hash && tl > 0 && tmp[0] != '0') { prefix[pfx_len++] = '0'; prefix[pfx_len++] = 'b'; }
                    if (neg) {
                        /* Two's complement representation: prepend "..1" */
                        prefix[pfx_len++] = '.'; prefix[pfx_len++] = '.'; prefix[pfx_len++] = '1';
                    }
                    /* Apply precision: pad digits with leading 0s to prec. */
                    int digit_pad = 0;
                    if (prec >= 0 && tl < prec) digit_pad = prec - tl;
                    int content_len = pfx_len + digit_pad + tl;
                    /* Apply width with space/zero padding. */
                    int total_pad = (width > content_len) ? (width - content_len) : 0;
                    int bp = 0;
                    if (!flag_minus) {
                        char pad_ch = (flag_zero && prec < 0) ? '0' : ' ';
                        if (pad_ch == '0' && pfx_len > 0) {
                            /* zero-pad goes after prefix */
                            for (int k = 0; k < pfx_len && bp < (int)sizeof(buf)-1; k++) buf[bp++] = prefix[k];
                            for (int k = 0; k < total_pad && bp < (int)sizeof(buf)-1; k++) buf[bp++] = '0';
                            pfx_len = 0;  /* consumed */
                        } else {
                            for (int k = 0; k < total_pad && bp < (int)sizeof(buf)-1; k++) buf[bp++] = pad_ch;
                        }
                    }
                    for (int k = 0; k < pfx_len && bp < (int)sizeof(buf)-1; k++) buf[bp++] = prefix[k];
                    for (int k = 0; k < digit_pad && bp < (int)sizeof(buf)-1; k++) buf[bp++] = '0';
                    for (int k = 0; k < tl && bp < (int)sizeof(buf)-1; k++) buf[bp++] = tmp[k];
                    if (flag_minus) {
                        for (int k = 0; k < total_pad && bp < (int)sizeof(buf)-1; k++) buf[bp++] = ' ';
                    }
                    buf[bp] = 0;
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
    const struct korb_string *s = (const struct korb_string *)self;
    if (!korb_block_given()) {
        /* No block: return Enumerator (CRuby semantics).  Call to_enum
         * with the source method captured so #size works and chained
         * each(&blk) re-dispatches with the user's block. */
        VALUE arg = korb_id2sym(korb_intern("each_byte"));
        return korb_funcall(c, self, korb_intern("to_enum"), 1, &arg);
    }
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
    /* Decode the leading UTF-8 codepoint.  ASCII (0xxxxxxx) returns
     * the byte directly; multi-byte sequences combine continuation
     * bytes into the full code-point integer.  Match CRuby's #ord
     * (which returns the codepoint, not the raw byte). */
    const unsigned char *p = (const unsigned char *)s->ptr;
    long len = s->len;
    unsigned char b0 = p[0];
    if (b0 < 0x80) return INT2FIX(b0);
    int seq_len;
    long cp;
    if ((b0 & 0xe0) == 0xc0) { seq_len = 2; cp = b0 & 0x1f; }
    else if ((b0 & 0xf0) == 0xe0) { seq_len = 3; cp = b0 & 0x0f; }
    else if ((b0 & 0xf8) == 0xf0) { seq_len = 4; cp = b0 & 0x07; }
    else return INT2FIX(b0);  /* invalid leading byte: fall back */
    if (len < seq_len) return INT2FIX(b0);
    for (int i = 1; i < seq_len; i++) {
        unsigned char bi = p[i];
        if ((bi & 0xc0) != 0x80) return INT2FIX(b0);
        cp = (cp << 6) | (bi & 0x3f);
    }
    return INT2FIX(cp);
}

/* String#eql? — content equality; rejects non-strings. */
static VALUE str_eql(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    extern VALUE str_eq(CTX *c, VALUE self, int argc, VALUE *argv);
    return str_eq(c, self, argc, argv);
}

/* String#clone — fresh independent copy.  Preserves frozen state
 * (clone does, dup doesn't — matching CRuby). */
static VALUE str_clone(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_string *s = (const struct korb_string *)self;
    VALUE r = korb_str_new(s->ptr, s->len);
    if (korb_obj_frozen_p(self)) {
        ((struct RBasic *)r)->flags |= FL_FROZEN;
    }
    return r;
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
        /* Underscore separator between digits is allowed (CRuby
         * `"A_BAD_BABE".hex == 0xABADBABE`) but only when surrounded
         * by valid hex digits. */
        if (ch == '_' && any && i + 1 < len) {
            char nx = s->ptr[i + 1];
            bool nx_hex = (nx >= '0' && nx <= '9') ||
                          (nx >= 'a' && nx <= 'f') ||
                          (nx >= 'A' && nx <= 'F');
            if (nx_hex) { i++; continue; }
        }
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
        else if (p == 'd' || p == 'D') { base = 10; i += 2; }
        /* otherwise stay at 8, leading '0' itself is part of the number */
    }
    long v = 0;
    bool any = false;
    while (i < len) {
        char ch = s->ptr[i];
        if (ch == '_' && any && i + 1 < len) {
            char nx = s->ptr[i + 1];
            int nd = -1;
            if      (nx >= '0' && nx <= '9') nd = nx - '0';
            else if (nx >= 'a' && nx <= 'f') nd = 10 + (nx - 'a');
            else if (nx >= 'A' && nx <= 'F') nd = 10 + (nx - 'A');
            if (nd >= 0 && nd < base) { i++; continue; }
        }
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
    /* Coerce each arg via #to_str if not String; raise TypeError on
     * failure (CRuby semantics). */
    VALUE local[8];
    VALUE *args = (argc <= 8) ? local : (VALUE *)korb_xmalloc(sizeof(VALUE) * argc);
    for (int i = 0; i < argc; i++) {
        VALUE a = argv[i];
        if (SPECIAL_CONST_P(a) || BUILTIN_TYPE(a) != T_STRING) {
            if (!SPECIAL_CONST_P(a)) {
                VALUE rt = korb_funcall(c, a, korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
                if (c->state == KORB_RAISE) return Qnil;
                if (RTEST(rt)) {
                    a = korb_funcall(c, a, korb_intern("to_str"), 0, NULL);
                    if (c->state == KORB_RAISE) return Qnil;
                }
            }
            if (SPECIAL_CONST_P(a) || BUILTIN_TYPE(a) != T_STRING) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into String",
                           SPECIAL_CONST_P(argv[i]) ? "(special)"
                               : korb_id_name(korb_class_of_class(argv[i])->name));
                return Qnil;
            }
        }
        args[i] = a;
    }
    argv = args;
    /* Concatenate args into a single buffer first to keep the math simple. */
    long extra = 0;
    for (int i = 0; i < argc; i++) {
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
    if (argc < 2) return self;
    long pos;
    if (FIXNUM_P(argv[0])) {
        pos = FIX2LONG(argv[0]);
    } else {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return Qnil;
        pos = FIX2LONG(iv);
    }
    /* Coerce other to a String via #to_str (TypeError on failure). */
    VALUE other = (SPECIAL_CONST_P(argv[1]) || BUILTIN_TYPE(argv[1]) != T_STRING)
        ? str_coerce_arg(c, argv[1]) : argv[1];
    if (UNDEF_P(other) || c->state == KORB_RAISE) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)other;
    long orig_pos = pos;
    if (pos < 0) pos = s->len + pos + 1;
    /* CRuby raises IndexError when the (possibly negative-adjusted)
     * index is out of range. */
    if (pos < 0 || pos > s->len) {
        VALUE eI = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
        korb_raise(c, (struct korb_class *)eI,
                   "index %ld out of string", orig_pos);
        return Qnil;
    }
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

/* Coerce arg to a String via #to_str; on failure raises TypeError. */
static VALUE str_coerce_arg(CTX *c, VALUE arg) {
    if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) return arg;
    if (!SPECIAL_CONST_P(arg)) {
        VALUE rt = korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return Qundef;
        if (RTEST(rt)) {
            VALUE r = korb_funcall(c, arg, korb_intern("to_str"), 0, NULL);
            if (c->state == KORB_RAISE) return Qundef;
            if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) return r;
        }
    }
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eT,
               "no implicit conversion of %s into String",
               SPECIAL_CONST_P(arg) ? "(special)"
                   : korb_id_name(korb_class_of_class(arg)->name));
    return Qundef;
}

static VALUE str_delete_prefix(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_str_new(((struct korb_string *)self)->ptr,
                                       ((struct korb_string *)self)->len);
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0)
        return korb_str_new(s->ptr + p->len, s->len - p->len);
    return korb_str_new(s->ptr, s->len);
}

static VALUE str_delete_suffix(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_str_new(((struct korb_string *)self)->ptr,
                                       ((struct korb_string *)self)->len);
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len <= s->len && memcmp(s->ptr + s->len - p->len, p->ptr, p->len) == 0)
        return korb_str_new(s->ptr, s->len - p->len);
    return korb_str_new(s->ptr, s->len);
}

/* In-place variants: mutate self; return self on change, nil on no-op. */
static VALUE str_delete_prefix_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc < 1) return Qnil;
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len == 0 || p->len > s->len ||
        memcmp(s->ptr, p->ptr, p->len) != 0) return Qnil;
    long new_len = s->len - p->len;
    char *np = korb_xmalloc_atomic(new_len + 1);
    memcpy(np, s->ptr + p->len, new_len);
    np[new_len] = 0;
    s->ptr = np;
    s->len = new_len;
    s->capa = new_len;
    return self;
}

static VALUE str_delete_suffix_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc < 1) return Qnil;
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return Qnil;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len == 0 || p->len > s->len ||
        memcmp(s->ptr + s->len - p->len, p->ptr, p->len) != 0) return Qnil;
    s->len -= p->len;
    if (s->capa > s->len) s->ptr[s->len] = 0;
    return self;
}

/* ---------- String#each_line (real impl) ----------
 * Was registered as str_split, which split on whitespace.  Walks the
 * string yielding each line including its trailing '\n'; returns self. */
static VALUE str_each_line(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_string *s = (const struct korb_string *)self;
    bool has_block = korb_block_given();
    VALUE collected = has_block ? Qnil : korb_ary_new();
    long start = 0;
    for (long i = 0; i < s->len; i++) {
        if (s->ptr[i] == '\n') {
            VALUE line = korb_str_new(s->ptr + start, i - start + 1);
            if (has_block) {
                korb_yield(c, 1, &line);
                if (c->state != KORB_NORMAL) return Qnil;
            } else {
                korb_ary_push(collected, line);
            }
            start = i + 1;
        }
    }
    if (start < s->len) {
        VALUE line = korb_str_new(s->ptr + start, s->len - start);
        if (has_block) {
            korb_yield(c, 1, &line);
            if (c->state != KORB_NORMAL) return Qnil;
        } else {
            korb_ary_push(collected, line);
        }
    }
    return has_block ? self : collected;
}

/* Encoding stubs (we don't track per-string encoding). */
VALUE _str_encoding(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *cEnc = (struct korb_class *)korb_const_get(korb_vm->object_class, korb_intern("Encoding"));
    if (!cEnc) return Qnil;
    return korb_const_get(cEnc, korb_intern("UTF_8"));
}
VALUE _str_force_encoding(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
}
/* String#b — return a copy of self with ASCII-8BIT encoding.  We don't
 * track per-string encoding so a dup is enough for behavioral equality. */
VALUE _str_b(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    return korb_str_new(s->ptr, s->len);
}
VALUE _enc_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE name = korb_ivar_get(self, korb_intern("@name"));
    if (UNDEF_P(name) || NIL_P(name)) return korb_str_new_cstr("UTF-8");
    return name;
}
VALUE _enc_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return _enc_name(c, self, argc, argv);
}
VALUE _enc_default_external(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_const_get((struct korb_class *)self, korb_intern("UTF_8"));
}
VALUE _enc_default_internal(CTX *c, VALUE self, int argc, VALUE *argv) {
    return Qnil;
}
VALUE _enc_find(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_const_get((struct korb_class *)self, korb_intern("UTF_8"));
}
