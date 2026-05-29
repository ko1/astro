/* String — moved from builtins.c. */

/* Forward decl — defined in builtins/array.c which is included after
 * builtins/string.c.  Needed for to_int coerce in #getbyte etc. */
static VALUE korb_to_int_or_raise(CTX *c, VALUE v);
/* Forward decl for the to_str coerce helper (defined further below). */
static VALUE str_coerce_arg(CTX *c, VALUE arg);
/* Forward decl for UTF-8 char→byte index translation (defined later). */
static int str_char_range_to_bytes(const char *p, long byte_len,
                                   long char_start, long char_count,
                                   long *out_start, long *out_len);

/* String.new(s = "") — start the new string from an optional initial
 * value.  Class#new's generic path goes through korb_object_new which
 * doesn't allocate the String storage; we need a real heap String. */
/* String#initialize(s = "") — copy contents from s into self.  Default
 * implementation used by both String.new and subclass overrides via
 * super.  encoding:/capacity: kwargs are accepted but informational. */
RESULT str_initialize(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    int eff_argc = argc;
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        eff_argc = argc - 1;
    }
    if (eff_argc == 0) return RESULT_OK(self);
    VALUE init = argv[0];
    if (SPECIAL_CONST_P(init) || BUILTIN_TYPE(init) != T_STRING) {
        if (!SPECIAL_CONST_P(init)) {
            VALUE rt = korb_funcall(c, init, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (RTEST(rt)) {
                init = korb_funcall(c, init, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            }
        }
        if (SPECIAL_CONST_P(init) || BUILTIN_TYPE(init) != T_STRING) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    struct korb_string *src = (struct korb_string *)init;
    struct korb_string *dst = (struct korb_string *)self;
    /* Replace contents in-place (copy buffer so source can be mutated
     * later without affecting us). */
    char *buf = korb_xmalloc_atomic(src->len + 1);
    if (src->len > 0) memcpy(buf, src->ptr, src->len);
    buf[src->len] = 0;
    dst->ptr = buf;
    dst->len = src->len;
    dst->capa = src->len;
    return RESULT_OK(self);
}

RESULT str_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE *argv = sp - argc;

    /* Allocate an empty String of self's class, then dispatch initialize
     * so subclass overrides apply (CRuby semantics).  Re-read `self`
     * from sp[-argc-1] AFTER the alloc since GC may have moved the
     * class (T_CLASS is arena-allocated). */
    VALUE r = korb_str_new(c, c->sp, "", 0);
    VALUE self = sp[-argc - 1];
    if (BUILTIN_TYPE(self) == T_CLASS) {
        ((struct RBasic *)r)->klass = self;
    }
    /* See ary_class_new comment: stage [r, argv...] on sp, bump c->sp
     * past the staging so the AST dispatcher's zero-fill on return
     * doesn't clobber r, and read back from sp[0] (GC may have moved r). */
    sp[0] = r;
    for (int i = 0; i < argc; i++) sp[1 + i] = argv[i];
    VALUE *prev_sp = c->sp;
    c->sp = sp + 1 + argc;
    UNWRAP(korb_funcall_r(c, r, korb_intern("initialize"), argc, sp + 1));
    r = sp[0];
    c->sp = prev_sp;
    return RESULT_OK(r);
}

/* ---------- String ---------- */
static RESULT str_plus(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park self + other across korb_str_dup's alloc-can-GC.  Without
     * protection, the C-local `other` (= argv[0] snapshot or to_str
     * result) goes stale and korb_str_concat reads garbage->len which
     * overflows the freshly-malloc'd buffer into adjacent libc/gc obj
     * memory, corrupting a hash header that later crashes scan_edges. */
    VALUE result;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = self;
        rs[1] = argv[0];
        if (SPECIAL_CONST_P(rs[1]) || BUILTIN_TYPE(rs[1]) != T_STRING) {
            if (!SPECIAL_CONST_P(rs[1])) {
                VALUE rt = korb_funcall(c, rs[1], korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
                if (c->state == KORB_RAISE) { ARO_ROOT_SCOPE_CANCEL(c, rs); return RESULT_OK(Qnil); }
                if (RTEST(rt)) {
                    rs[1] = korb_funcall(c, rs[1], korb_intern("to_str"), 0, NULL);
                    if (c->state == KORB_RAISE) { ARO_ROOT_SCOPE_CANCEL(c, rs); return RESULT_OK(Qnil); }
                }
            }
            if (SPECIAL_CONST_P(rs[1]) || BUILTIN_TYPE(rs[1]) != T_STRING) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into String",
                           SPECIAL_CONST_P(argv[0]) ? "(special)"
                               : korb_id_name(korb_class_of_class(argv[0])->name));
                ARO_ROOT_SCOPE_CANCEL(c, rs); return RESULT_OK(Qnil);
            }
        }
        VALUE r = korb_str_dup(c, c->sp, rs[0]);
        result = korb_str_concat(c, c->sp, r, rs[1]);
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(result);
}
/* Append a single arg to self.  Returns Qfalse on raise (caller stops). */
static bool str_concat_one(CTX *c, VALUE self, VALUE arg);
/* String#<< — accepts exactly one argument (CRuby semantics).  Variadic
 * version is `concat`. */
static bool str_concat_one(CTX *c, VALUE self, VALUE arg);
static RESULT str_lshift(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc != 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    /* Pin self across str_concat_one's potential GC fires (korb_str_new
     * etc. inside concat_one fires GC if korb_funcall is taken).
     * Without pinning, the C-param `self` goes stale after the first
     * forwarding cycle and the modification lands on the OLD obj,
     * leaving the caller's variable (= the TO-space copy) unchanged
     * (= `s << "y"; s << "z"` leaves s as "x" instead of "xyz"). */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = self;
        rs[1] = argv[0];
        if (str_concat_one(c, rs[0], rs[1])) {
            ret = rs[0];
        }
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
}
static RESULT str_concat(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
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
            args[i] = korb_str_new(c, c->sp, s->ptr, s->len);
        } else {
            args[i] = a;
        }
    }
    for (int i = 0; i < argc; i++) {
        if (!str_concat_one(c, self, args[i])) return RESULT_OK(Qnil);
    }
    return RESULT_OK(self);
}
static bool str_concat_one(CTX *c, VALUE self, VALUE arg) {
    /* Pin self / arg / tmp across korb_str_new + korb_str_concat —
     * korb_string is arena-allocated and moves under STRESS; without
     * pinning, `s << "y"` writes to a moved-out address (= no-op
     * from the caller's perspective). */
    bool ok = false;
    ARO_ROOT_SCOPE_START(c, rs, 3) {
        rs[0] = self;
        rs[1] = arg;
        rs[2] = Qnil;  /* scratch for tmp */
    /* `str << int` appends the codepoint as bytes (CRuby semantics).
     * koruby is byte-only; reject negatives and out-of-byte values
     * with RangeError to mirror CRuby for the simple ASCII range, but
     * fall through to a single-byte append when 0..255. */
    if (FIXNUM_P(rs[1])) {
        long cp = FIX2LONG(rs[1]);
        if (cp < 0) {
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            DROP_RESULT(korb_raise(c, (struct korb_class *)eR,
                       "%ld out of char range", cp));
            ok = false; goto done;
        }
        if (cp <= 0x7f) {
            char ch = (char)cp;
            rs[2] = korb_str_new(c, c->sp, &ch, 1);
            korb_str_concat(c, c->sp, rs[0], rs[2]);
            ok = true; goto done;
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
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            DROP_RESULT(korb_raise(c, (struct korb_class *)eR,
                       "%ld out of char range", cp));
            return false;
        }
        rs[2] = korb_str_new(c, c->sp, buf, len);
        korb_str_concat(c, c->sp, rs[0], rs[2]);
        ok = true; goto done;
    }
    if (SPECIAL_CONST_P(rs[1]) || BUILTIN_TYPE(rs[1]) != T_STRING) {
        /* Try to_s as a fallback. */
        rs[2] = korb_funcall(c, rs[1], korb_intern("to_s"), 0, NULL);
        if (!SPECIAL_CONST_P(rs[2]) && BUILTIN_TYPE(rs[2]) == T_STRING) {
            korb_str_concat(c, c->sp, rs[0], rs[2]);
            ok = true; goto done;
        }
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        DROP_RESULT(korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion to String"));
        ok = false; goto done;
    }
    /* Snapshot the arg's bytes if it might alias self (e.g. `b.concat(b, b)`
     * mutates b mid-call; without a snapshot, the second iteration sees
     * the already-grown buffer and we end up doubling instead of tripling). */
    if (rs[1] == rs[0]) {
        struct korb_string *src = (struct korb_string *)rs[1];
        rs[2] = korb_str_new(c, c->sp, src->ptr, src->len);
        korb_str_concat(c, c->sp, rs[0], rs[2]);
        ok = true; goto done;
    }
    korb_str_concat(c, c->sp, rs[0], rs[1]);
    ok = true;
done: ;
    } ARO_ROOT_SCOPE_END(c, rs);
    return ok;
}
static RESULT str_bytesize(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX(((struct korb_string *)self)->len));
}
static RESULT str_size(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX(((struct korb_string *)self)->len));
}
static RESULT str_eq(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(c, self, argv[0])));
}

/* Reentrancy guard for the inverted-<=> path: if other.<=>(self) re-enters
 * String#<=> with the same pair, return nil rather than recursing. */
static int str_cmp_inverse_depth = 0;
static RESULT str_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (RTEST(rt)) {
                VALUE r = korb_funcall(c, other, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
                if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) {
                    other = r;
                    goto compare_strings;
                }
            }
            if (str_cmp_inverse_depth > 0) return RESULT_OK(Qnil);
            rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                              (VALUE[]){ korb_id2sym(korb_intern("<=>")) });
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (RTEST(rt)) {
                str_cmp_inverse_depth++;
                VALUE r = korb_funcall(c, other, korb_intern("<=>"), 1, &self);
                str_cmp_inverse_depth--;
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
                if (FIXNUM_P(r)) {
                    long v = FIX2LONG(r);
                    return RESULT_OK(INT2FIX(v < 0 ? 1 : v > 0 ? -1 : 0));
                }
            }
        }
        return RESULT_OK(Qnil);
    }
compare_strings:;
    struct korb_string *a = (struct korb_string *)self;
    struct korb_string *b = (struct korb_string *)other;
    long n = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->ptr, b->ptr, n);
    if (r != 0) return RESULT_OK(INT2FIX(r < 0 ? -1 : 1));
    if (a->len < b->len) return RESULT_OK(INT2FIX(-1));
    if (a->len > b->len) return RESULT_OK(INT2FIX(1));
    return RESULT_OK(INT2FIX(0));
}

/* Raise CRuby's "comparison of String with X failed" ArgumentError when
 * <=> couldn't reach a result. */
static void str_cmp_raise(CTX *c, VALUE other) {
    VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    VALUE oi = korb_inspect(c, c->sp, other);
    const char *o_str = (!SPECIAL_CONST_P(oi) && BUILTIN_TYPE(oi) == T_STRING)
                            ? korb_str_cstr(oi)
                            : korb_id_name(korb_class_of_class(other)->name);
    DROP_RESULT(korb_raise(c, (struct korb_class *)eArg,
               "comparison of String with %s failed", o_str));
}
static RESULT str_lt(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE r = UNWRAP(str_cmp(c, argc, sp));
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return RESULT_OK(Qnil); }
    return RESULT_OK(KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) < 0));
}
static RESULT str_le(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE r = UNWRAP(str_cmp(c, argc, sp));
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return RESULT_OK(Qnil); }
    return RESULT_OK(KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) <= 0));
}
static RESULT str_gt(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE r = UNWRAP(str_cmp(c, argc, sp));
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return RESULT_OK(Qnil); }
    return RESULT_OK(KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) > 0));
}
static RESULT str_ge(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE r = UNWRAP(str_cmp(c, argc, sp));
    if (NIL_P(r)) { str_cmp_raise(c, argv[0]); return RESULT_OK(Qnil); }
    return RESULT_OK(KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) >= 0));
}
static RESULT str_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
 return RESULT_OK(self); }
/* String#__chilled? — internal: true iff FL_CHILLED is set.  Used by
 * Ruby-level `+@` to decide whether to return a fresh mutable copy. */
static RESULT str_chilled_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self)) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL((RBASIC(self)->head.flags & FL_CHILLED) != 0));
}
static RESULT str_to_sym(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_str_to_sym(self));
}


/* ---------- String formatting / methods (extended) ---------- */

static VALUE str_format_self(CTX *c, VALUE self, int argc, VALUE *argv);

static RESULT str_split(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Pin self + sep (= argv[0]) + result array + per-iteration piece
     * across korb_str_new / korb_ary_new / korb_ary_push / korb_yield
     * GC fires.  Without this, under STRESS the C-local `s` goes
     * stale (s is arena T_STRING) and s->len / s->ptr return moved-
     * out address fields, producing negative lengths that abort
     * korb_xmalloc with SIGABRT. */
    bool has_block = korb_block_given(c);
    VALUE ret = Qnil;
    int rs_cap = 4;
    ARO_ROOT_SCOPE_START(c, rs, 4) {
        (void)rs_cap;
        rs[0] = self;
        rs[1] = (argc >= 1) ? argv[0] : Qnil;
        rs[2] = korb_ary_new(c, c->sp);           /* result */
        /* rs[3] holds the per-iter piece across korb_ary_push / yield */
        #define EMIT(v) do { \
            rs[3] = (v); \
            if (has_block) korb_yield(c, 1, &rs[3]); \
            else korb_ary_push(rs[2], rs[3]); \
        } while (0)
        struct korb_string *s = (struct korb_string *)rs[0];
        if (argc == 0 || NIL_P(rs[1])) {
            long i = 0;
            while (i < s->len) {
                while (i < s->len && (s->ptr[i] == ' ' || s->ptr[i] == '\t' || s->ptr[i] == '\n')) i++;
                if (i >= s->len) break;
                long start = i;
                while (i < s->len && s->ptr[i] != ' ' && s->ptr[i] != '\t' && s->ptr[i] != '\n') i++;
                EMIT(korb_str_new(c, c->sp, s->ptr + start, i - start));
                s = (struct korb_string *)rs[0];  /* reload */
            }
            ret = has_block ? rs[0] : rs[2];
        } else if (BUILTIN_TYPE(rs[1]) != T_STRING) {
            ret = has_block ? rs[0] : rs[2];
        } else {
            struct korb_string *sep = (struct korb_string *)rs[1];
            if (sep->len == 0) {
                for (long i = 0; i < s->len; i++) {
                    EMIT(korb_str_new(c, c->sp, s->ptr + i, 1));
                    s = (struct korb_string *)rs[0];  /* reload */
                    sep = (struct korb_string *)rs[1];
                }
                ret = has_block ? rs[0] : rs[2];
            } else {
                long start = 0;
                for (long i = 0; i + sep->len <= s->len; ) {
                    if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
                        EMIT(korb_str_new(c, c->sp, s->ptr + start, i - start));
                        s = (struct korb_string *)rs[0];  /* reload */
                        sep = (struct korb_string *)rs[1];
                        i += sep->len;
                        start = i;
                    } else i++;
                }
                EMIT(korb_str_new(c, c->sp, s->ptr + start, s->len - start));
                ret = has_block ? rs[0] : rs[2];
            }
        }
        #undef EMIT
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
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
    VALUE arg;
    if (argc < 1) {
        /* CRuby: no arg → use $/ as the separator (defaults to "\n"). */
        VALUE rs = korb_gvar_get(korb_intern("$/"));
        if (NIL_P(rs) || SPECIAL_CONST_P(rs) || BUILTIN_TYPE(rs) != T_STRING) {
            if (n >= 2 && s->ptr[n-2] == '\r' && s->ptr[n-1] == '\n') return n - 2;
            if (n >= 1 && (s->ptr[n-1] == '\n' || s->ptr[n-1] == '\r')) return n - 1;
            return n;
        }
        arg = rs;
        goto have_str;
    }
    arg = argv[0];
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
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        DROP_RESULT(korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into String",
                   SPECIAL_CONST_P(arg) ? "(special)"
                       : korb_id_name(korb_class_of_class(arg)->name)));
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

static RESULT str_chomp(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long n = str_chomp_compute(c, self, argc, argv);
    if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
    return RESULT_OK(korb_str_new(c, c->sp, ((struct korb_string *)self)->ptr, n));
}

static RESULT str_chomp_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    long n = str_chomp_compute(c, self, argc, argv);
    if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
    if (n == s->len) return RESULT_OK(Qnil);
    s->len = n;
    if (s->capa > s->len) s->ptr[s->len] = 0;
    return RESULT_OK(self);
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

static RESULT str_strip(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    while (end > start && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr + start, end - start));
}

static RESULT str_lstrip(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    long start = 0;
    while (start < s->len && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr + start, s->len - start));
}

static RESULT str_rstrip(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    long end = s->len;
    while (end > 0 && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr, end));
}

static RESULT str_lstrip_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    long start = 0;
    while (start < s->len && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    if (start == 0) return RESULT_OK(Qnil);
    long new_len = s->len - start;
    memmove(s->ptr, s->ptr + start, new_len);
    s->len = new_len;
    if (s->capa > new_len) s->ptr[new_len] = 0;
    return RESULT_OK(self);
}

static RESULT str_rstrip_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    long end = s->len;
    while (end > 0 && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    if (end == s->len) return RESULT_OK(Qnil);
    s->len = end;
    if (s->capa > end) s->ptr[end] = 0;
    return RESULT_OK(self);
}

static RESULT str_strip_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && str_is_lstrip_ws((unsigned char)s->ptr[start])) start++;
    while (end > start && str_is_rstrip_ws((unsigned char)s->ptr[end-1])) end--;
    if (start == 0 && end == s->len) return RESULT_OK(Qnil);
    long new_len = end - start;
    if (start > 0) memmove(s->ptr, s->ptr + start, new_len);
    s->len = new_len;
    if (s->capa > new_len) s->ptr[new_len] = 0;
    return RESULT_OK(self);
}

static RESULT str_to_i(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    char *end;
    long v = strtol(s->ptr, &end, argc > 0 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10);
    return RESULT_OK(INT2FIX(v));
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
static RESULT str_to_f(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
    if (!saw_digit) return RESULT_OK(korb_float_new(c, c->sp, 0.0));
    buf[blen] = '\0';
    return RESULT_OK(korb_float_new(c, c->sp, strtod(buf, NULL)));
}

/* String#byteslice — byte-indexed slice.  koruby is byte-only so this
 * is identical to #[] for the integer / range / (idx, len) forms. */
static RESULT str_byteslice(CTX *c, int argc, VALUE *sp);
static RESULT str_append_as_bytes(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Append each arg's bytes to self.  koruby is byte-only so this is
     * a glorified concat that ignores Encoding. */
    for (int i = 0; i < argc; i++) {
        if (FIXNUM_P(argv[i])) {
            char ch = (char)(FIX2LONG(argv[i]) & 0xff);
            VALUE tmp = korb_str_new(c, c->sp, &ch, 1);
            korb_str_concat(c, c->sp, self, tmp);
        } else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING) {
            korb_str_concat(c, c->sp, self, argv[i]);
        }
    }
    return RESULT_OK(self);
}
static RESULT str_setbyte(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 2 || !FIXNUM_P(argv[0]) || !FIXNUM_P(argv[1])) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    long i = FIX2LONG(argv[0]);
    long b = FIX2LONG(argv[1]);
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) {
        VALUE eI = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
        return korb_raise(c, (struct korb_class *)eI, "index %ld out of string", i);
    }
    s->ptr[i] = (char)(b & 0xff);
    return RESULT_OK(argv[1]);
}
static RESULT str_getbyte(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    long i;
    if (FIXNUM_P(argv[0])) {
        i = FIX2LONG(argv[0]);
    } else {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return RESULT_OK(Qnil);
        i = FIX2LONG(iv);
    }
    struct korb_string *s = (struct korb_string *)self;
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) return RESULT_OK(Qnil);
    return RESULT_OK(INT2FIX((unsigned char)s->ptr[i]));
}

static RESULT str_aref(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    if (argc == 1 && FIXNUM_P(argv[0])) {
        long char_idx = FIX2LONG(argv[0]);
        long bstart, blen;
        int rc = str_char_range_to_bytes(s->ptr, s->len, char_idx, 1, &bstart, &blen);
        if (rc != 0 || blen == 0) return RESULT_OK(Qnil);
        return RESULT_OK(korb_str_new(c, c->sp, s->ptr + bstart, blen));
    }
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        if (!FIXNUM_P(r->begin) && !NIL_P(r->begin)) return RESULT_OK(Qnil);
        if (!FIXNUM_P(r->end) && !NIL_P(r->end)) return RESULT_OK(Qnil);
        /* Count codepoints once so negative indices and exclude_end can
         * be normalized.  Otherwise `s[1...-1]` would compute the range
         * with raw negative numbers and miss the actual substring. */
        long total_cp = 0;
        {
            long bb = 0;
            while (bb < s->len) {
                unsigned char c0 = (unsigned char)s->ptr[bb];
                int n = ((c0 & 0x80) == 0x00) ? 1 :
                        ((c0 & 0xE0) == 0xC0) ? 2 :
                        ((c0 & 0xF0) == 0xE0) ? 3 :
                        ((c0 & 0xF8) == 0xF0) ? 4 : 1;
                bb += n; total_cp++;
            }
        }
        long b = NIL_P(r->begin) ? 0 : FIX2LONG(r->begin);
        long e = NIL_P(r->end) ? total_cp - 1 : FIX2LONG(r->end);
        if (b < 0) b += total_cp;
        if (e < 0) e += total_cp;
        if (r->exclude_end && !NIL_P(r->end)) e -= 1;
        long count = e - b + 1;
        if (count < 0) count = 0;
        if (b < 0 || b > total_cp) return RESULT_OK(Qnil);
        long bstart, blen;
        int rc = str_char_range_to_bytes(s->ptr, s->len, b, count, &bstart, &blen);
        if (rc != 0) return RESULT_OK(Qnil);
        return RESULT_OK(korb_str_new(c, c->sp, s->ptr + bstart, blen));
    }
    if (argc == 2 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        long char_idx = FIX2LONG(argv[0]);
        long char_cnt = FIX2LONG(argv[1]);
        if (char_cnt < 0) return RESULT_OK(Qnil);
        long bstart, blen;
        int rc = str_char_range_to_bytes(s->ptr, s->len, char_idx, char_cnt, &bstart, &blen);
        if (rc != 0) return RESULT_OK(Qnil);
        return RESULT_OK(korb_str_new(c, c->sp, s->ptr + bstart, blen));
    }
    return RESULT_OK(Qnil);
}

/* Body for byteslice — same as aref since koruby is byte-only. */
static RESULT str_byteslice(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(UNWRAP(str_aref(c, argc, sp)));
}

/* String#[]= — replace a slice of bytes with a string.  Forms:
 *   s[i] = "..."         → replace 1 char at i (insert if "" or longer)
 *   s[i, len] = "..."    → replace len chars starting at i
 *   s[range] = "..."     → replace characters in range
 *   s[regex] = "..."     → not impl (regex out of scope)
 *   s[regex, group] = "..." → not impl
 *   s[match_str] = "..." → replace first occurrence
 * Returns the rhs value (not self) per CRuby. */
/* Convert (char_idx, char_count) to (byte_start, byte_len) for a UTF-8
 * string.  Returns 0 on success, -1 on out-of-range (start out of bounds),
 * -2 on negative count.  start may be == codepoint count (append-style). */
static int str_char_range_to_bytes(const char *p, long byte_len,
                                   long char_start, long char_count,
                                   long *out_start, long *out_len) {
    /* Walk forward, recording each codepoint start. */
    long count = 0;
    long b = 0;
    /* First pass: count codepoints + remember byte offsets up to max needed. */
    long total_cp = 0;
    {
        long bb = 0;
        while (bb < byte_len) {
            unsigned char c0 = (unsigned char)p[bb];
            int n = ((c0 & 0x80) == 0x00) ? 1 :
                    ((c0 & 0xE0) == 0xC0) ? 2 :
                    ((c0 & 0xF0) == 0xE0) ? 3 :
                    ((c0 & 0xF8) == 0xF0) ? 4 : 1;
            bb += n;
            total_cp++;
        }
    }
    if (char_start < 0) char_start += total_cp;
    if (char_start < 0 || char_start > total_cp) return -1;
    if (char_count < 0) return -2;
    /* Walk to char_start */
    while (b < byte_len && count < char_start) {
        unsigned char c0 = (unsigned char)p[b];
        int n = ((c0 & 0x80) == 0x00) ? 1 :
                ((c0 & 0xE0) == 0xC0) ? 2 :
                ((c0 & 0xF0) == 0xE0) ? 3 :
                ((c0 & 0xF8) == 0xF0) ? 4 : 1;
        b += n;
        count++;
    }
    *out_start = b;
    /* Walk to char_start + char_count */
    long b2 = b;
    long count2 = 0;
    while (b2 < byte_len && count2 < char_count) {
        unsigned char c0 = (unsigned char)p[b2];
        int n = ((c0 & 0x80) == 0x00) ? 1 :
                ((c0 & 0xE0) == 0xC0) ? 2 :
                ((c0 & 0xF0) == 0xE0) ? 3 :
                ((c0 & 0xF8) == 0xF0) ? 4 : 1;
        b2 += n;
        count2++;
    }
    *out_len = b2 - b;
    return 0;
}

static RESULT str_aset(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 2) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    /* Normalize rhs to a String.  CRuby uses #to_str and raises TypeError
     * if the result isn't a String. */
    VALUE val = argv[argc - 1];
    if (SPECIAL_CONST_P(val) || BUILTIN_TYPE(val) != T_STRING) {
        VALUE rt = korb_funcall(c, val, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (!RTEST(rt)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(val) ? "(special)"
                           : korb_id_name(korb_class_of_class(val)->name));
        }
        VALUE coerced = korb_funcall(c, val, korb_intern("to_str"), 0, NULL);
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (SPECIAL_CONST_P(coerced) || BUILTIN_TYPE(coerced) != T_STRING) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "can't convert to String (to_str returned non-String)");
        }
        val = coerced;
    }
    struct korb_string *vs = (struct korb_string *)val;
    long start = 0, len = 0;
    if (argc == 2 && FIXNUM_P(argv[0])) {
        long char_idx = FIX2LONG(argv[0]);
        int rc = str_char_range_to_bytes(s->ptr, s->len, char_idx, 1, &start, &len);
        if (rc == -1 || (rc == 0 && len == 0 && char_idx != 0 && char_idx != -1)) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE, "index %ld out of string", char_idx);
        }
    } else if (argc == 3 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        long char_idx = FIX2LONG(argv[0]);
        long char_cnt = FIX2LONG(argv[1]);
        int rc = str_char_range_to_bytes(s->ptr, s->len, char_idx, char_cnt, &start, &len);
        if (rc == -1) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE, "index %ld out of string", char_idx);
        }
        if (rc == -2) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE, "negative length %ld", char_cnt);
        }
    } else if (argc == 2 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        long b = NIL_P(r->begin) ? 0 : (FIXNUM_P(r->begin) ? FIX2LONG(r->begin) : 0);
        long e = NIL_P(r->end) ? s->len - 1 : (FIXNUM_P(r->end) ? FIX2LONG(r->end) : s->len - 1);
        if (b < 0) b += s->len;
        if (e < 0) e += s->len;
        if (r->exclude_end && !NIL_P(r->end)) e--;
        if (b < 0 || b > s->len) {
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR, "out of range");
        }
        if (e < b - 1) e = b - 1;
        start = b; len = e - b + 1;
        if (start + len > s->len) len = s->len - start;
    } else if (argc == 2 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        struct korb_string *needle = (struct korb_string *)argv[0];
        long pos = -1;
        for (long i = 0; i + needle->len <= s->len; i++) {
            if (memcmp(s->ptr + i, needle->ptr, needle->len) == 0) { pos = i; break; }
        }
        if (pos < 0) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE, "string not matched");
        }
        start = pos; len = needle->len;
    } else {
        return RESULT_OK(val);
    }
    /* Splice: s = s[0...start] + vs + s[start+len..] */
    long new_len = s->len - len + vs->len;
    char *new_ptr = korb_xmalloc_atomic(new_len + 1);
    memcpy(new_ptr, s->ptr, start);
    memcpy(new_ptr + start, vs->ptr, vs->len);
    memcpy(new_ptr + start + vs->len, s->ptr + start + len, s->len - start - len);
    new_ptr[new_len] = 0;
    s->ptr = new_ptr;
    s->len = new_len;
    return RESULT_OK(val);
}

static RESULT str_index(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *needle = (struct korb_string *)argv[0];
    long start = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 0;
    if (start < 0) start += s->len;
    if (start < 0) start = 0;
    if (needle->len == 0) return RESULT_OK(INT2FIX(start <= s->len ? start : s->len));
    for (long i = start; i + needle->len <= s->len; i++) {
        if (memcmp(s->ptr + i, needle->ptr, needle->len) == 0) return RESULT_OK(INT2FIX(i));
    }
    return RESULT_OK(Qnil);
}

static RESULT str_rindex(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    /* Coerce arg via #to_str; raises TypeError when arg is not a string-
     * convertible (Integer, etc.).  Note: rindex does NOT call #to_int. */
    VALUE arg = argv[0];
    if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) {
        arg = str_coerce_arg(c, arg);
        if (UNDEF_P(arg) || c->state == KORB_RAISE) return RESULT_OK(Qnil);
    }
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *needle = (struct korb_string *)arg;
    long start = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : s->len;
    if (start < 0) start += s->len;
    if (start > s->len - needle->len) start = s->len - needle->len;
    if (start < 0) return RESULT_OK(Qnil);
    if (needle->len == 0) return RESULT_OK(INT2FIX(start));
    for (long i = start; i >= 0; i--) {
        if (memcmp(s->ptr + i, needle->ptr, needle->len) == 0) return RESULT_OK(INT2FIX(i));
    }
    return RESULT_OK(Qnil);
}

static RESULT str_chars(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Pin self + result across korb_str_new / korb_ary_push GC fires. */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = self;
        rs[1] = korb_ary_new_capa(c, c->sp, ((struct korb_string *)rs[0])->len);
        struct korb_string *s = (struct korb_string *)rs[0];
        for (long i = 0; i < s->len; i++) {
            korb_ary_push(rs[1], korb_str_new(c, c->sp, s->ptr + i, 1));
            s = (struct korb_string *)rs[0];  /* reload */
        }
        ret = rs[1];
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
}

static RESULT str_bytes(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Use c->current_frame->self (auto-tracked) instead of C-local
     * self parameter. */
    if (korb_block_given(c)) {
        for (long i = 0; i < ((struct korb_string *)c->current_frame->self)->len; i++) {
            struct korb_string *s = (struct korb_string *)c->current_frame->self;
            VALUE b = INT2FIX((unsigned char)s->ptr[i]);
            korb_yield(c, 1, &b);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        }
        return RESULT_OK(c->current_frame->self);
    }
    VALUE r = korb_ary_new_capa(c, c->sp, ((struct korb_string *)c->current_frame->self)->len);
    for (long i = 0; i < ((struct korb_string *)c->current_frame->self)->len; i++) {
        struct korb_string *s = (struct korb_string *)c->current_frame->self;
        korb_ary_push(r, INT2FIX((unsigned char)s->ptr[i]));
    }
    return RESULT_OK(r);
}

static RESULT str_each_char(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Block-less form: koruby has no Enumerator, so return an Array of
     * single-char strings (matches what `.to_a` would yield).  Use
     * c->current_frame->self (auto-tracked by visit_roots frame walk)
     * inside the loop instead of the C-local self parameter, which
     * goes stale across allocations under STRESS+PURGE. */
    if (!korb_block_given(c)) {
        VALUE r = korb_ary_new(c, c->sp);
        for (long i = 0; i < ((struct korb_string *)c->current_frame->self)->len; i++) {
            struct korb_string *s = (struct korb_string *)c->current_frame->self;
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr + i, 1));
        }
        return RESULT_OK(r);
    }
    for (long i = 0; i < ((struct korb_string *)c->current_frame->self)->len; i++) {
        struct korb_string *s = (struct korb_string *)c->current_frame->self;
        VALUE ch = korb_str_new(c, c->sp, s->ptr + i, 1);
        korb_yield(c, 1, &ch);
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(c->current_frame->self);
}

static RESULT str_start_with(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT str_end_with(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr + s->len - p->len, p->ptr, p->len) == 0) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT str_include(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return RESULT_OK(Qfalse);
            if (RTEST(rt)) {
                other = korb_funcall(c, other, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return RESULT_OK(Qfalse);
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_STRING) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)other;
    if (p->len == 0) return RESULT_OK(Qtrue);
    for (long i = 0; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT str_replace(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *o = (struct korb_string *)argv[0];
    s->ptr = korb_xmalloc_atomic(o->len + 1);
    memcpy(s->ptr, o->ptr, o->len);
    s->ptr[o->len] = 0;
    s->len = o->len;
    s->capa = o->len;
    return RESULT_OK(self);
}

static RESULT str_reverse(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    /* Reverse by UTF-8 codepoint, not raw byte: walk forward to find
     * each char's byte run, then write it to the appropriate trailing
     * slot.  Continuation bytes have the top two bits 10. */
    long dst = s->len;
    long i = 0;
    while (i < s->len) {
        long j = i + 1;
        while (j < s->len && (((unsigned char)s->ptr[j] & 0xC0) == 0x80)) j++;
        long n = j - i;
        dst -= n;
        memcpy(r + dst, s->ptr + i, n);
        i = j;
    }
    r[s->len] = 0;
    return RESULT_OK(korb_str_new(c, c->sp, r, s->len));
}

static RESULT str_upcase(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return RESULT_OK(korb_str_new(c, c->sp, r, s->len));
}

static RESULT str_downcase(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return RESULT_OK(korb_str_new(c, c->sp, r, s->len));
}

static RESULT str_empty_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(((struct korb_string *)self)->len == 0));
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

static RESULT str_upcase_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_case_bang(c, self, xform_upcase));
}
static RESULT str_downcase_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_case_bang(c, self, xform_downcase));
}
static RESULT str_swapcase_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_case_bang(c, self, xform_swapcase));
}

/* String#capitalize! — first char up, rest down.  Returns self if
 * anything changed, nil otherwise. */
static RESULT str_capitalize_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_STRING) return RESULT_OK(Qnil);
    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return RESULT_OK(Qnil);
    char *buf = korb_xmalloc_atomic(s->len + 1);
    bool changed = false;
    for (long i = 0; i < s->len; i++) {
        char ch = (i == 0) ? xform_upcase(s->ptr[i]) : xform_downcase(s->ptr[i]);
        if (ch != s->ptr[i]) changed = true;
        buf[i] = ch;
    }
    buf[s->len] = 0;
    if (!changed) return RESULT_OK(Qnil);
    s->ptr = buf;
    return RESULT_OK(self);
}

/* String#reverse! — reverse in place.  Always returns self (CRuby
 * does too — empty/single-char strings still return self, not nil). */
static RESULT str_reverse_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_STRING) return RESULT_OK(Qnil);
    CHECK_FROZEN_R(c, self);
    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len + 1);
    long dst = s->len;
    long i = 0;
    while (i < s->len) {
        long j = i + 1;
        while (j < s->len && (((unsigned char)s->ptr[j] & 0xC0) == 0x80)) j++;
        long n = j - i;
        dst -= n;
        memcpy(buf + dst, s->ptr + i, n);
        i = j;
    }
    buf[s->len] = 0;
    s->ptr = buf;
    return RESULT_OK(self);
}

static RESULT str_mul(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(argv[0])) return RESULT_OK(self);
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return RESULT_OK(korb_str_new(c, c->sp, "", 0));
    /* Park self + result across korb_str_new's GC.  Without protect,
     * self (C param) goes stale after the first alloc, and
     * korb_str_concat(c, c->sp, r, stale_self) reads garbage->len → buffer
     * overflow → corrupts adjacent obj's header. */
    VALUE result;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = self;
        rs[1] = korb_str_new(c, c->sp, "", 0);
        for (long i = 0; i < n; i++) korb_str_concat(c, c->sp, rs[1], rs[0]);
        result = rs[1];
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(result);
}

static RESULT str_hash(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX((long)(korb_hash_value(c, self) >> 1)));
}

static RESULT str_sum(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Simple checksum: sum of bytes mod (1<<bits), default bits=16. */
    struct korb_string *s = (struct korb_string *)self;
    long bits = 16;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) {
            bits = FIX2LONG(argv[0]);
        } else {
            VALUE iv = korb_to_int_or_raise(c, argv[0]);
            if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return RESULT_OK(Qnil);
            bits = FIX2LONG(iv);
        }
    }
    unsigned long sum = 0;
    for (long i = 0; i < s->len; i++) sum += (unsigned char)s->ptr[i];
    if (bits > 0 && bits < 64) sum &= ((1UL << bits) - 1);
    return RESULT_OK(INT2FIX((long)sum));
}

static RESULT str_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* String === other ⇒ same as == */
    return RESULT_OK(KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(c, self, argv[0])));
}

static RESULT str_match_op(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* String#=~ regex — we don't have regex, return nil */
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(Qnil);
}

static RESULT str_match_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* String#match? — false (no regex) */
    return RESULT_OK(Qfalse);
}

static RESULT str_match(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(Qnil);
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

static RESULT str_gsub(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(korb_str_dup(c, c->sp, self));
    
    VALUE ret = Qnil;
    /* Pin self, pat (argv[0]), repl (argv[1]), out, scratch m
     * across str_concat / str_new / korb_yield GC fires. */
    ARO_ROOT_SCOPE_START(c, rs, 5) {
        rs[0] = self;
        rs[1] = argv[0];
        rs[2] = (argc >= 2) ? argv[1] : Qnil;
        rs[3] = korb_str_new(c, c->sp, "", 0);  /* out */
        rs[4] = Qnil;                 /* m / r scratch */
        struct korb_string *s = (struct korb_string *)rs[0];
        long start = 0, i = 0;
        long ms, ml;
        while (str_find_pat(rs[1], s, i, &ms, &ml)) {
            korb_str_concat(c, c->sp, rs[3], korb_str_new(c, c->sp, s->ptr + start, ms - start));
            s = (struct korb_string *)rs[0];
            if (argc >= 2 && BUILTIN_TYPE(rs[2]) == T_STRING) {
                struct korb_string *r = (struct korb_string *)rs[2];
                korb_str_concat(c, c->sp, rs[3], korb_str_new(c, c->sp, r->ptr, r->len));
                s = (struct korb_string *)rs[0];
            } else if (c->current_block) {
                rs[4] = korb_str_new(c, c->sp, s->ptr + ms, ml);
                rs[4] = korb_yield(c, 1, &rs[4]);
                if (c->state == KORB_RAISE) { ret = Qnil; goto gsub_done; }
                if (BUILTIN_TYPE(rs[4]) == T_STRING) korb_str_concat(c, c->sp, rs[3], rs[4]);
                else korb_str_concat(c, c->sp, rs[3], korb_to_s(c, c->sp, rs[4]));
                s = (struct korb_string *)rs[0];
            }
            i = ms + (ml > 0 ? ml : 1);
            start = i;
        }
        korb_str_concat(c, c->sp, rs[3], korb_str_new(c, c->sp, s->ptr + start, s->len - start));
        ret = rs[3];
    gsub_done: ;
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
}

static RESULT str_sub(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(korb_str_dup(c, c->sp, self));
    
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 5) {
        rs[0] = self;
        rs[1] = argv[0];
        rs[2] = (argc >= 2) ? argv[1] : Qnil;
        rs[3] = Qnil;  /* out */
        rs[4] = Qnil;  /* m / r scratch */
        struct korb_string *s = (struct korb_string *)rs[0];
        long ms, ml;
        if (!str_find_pat(rs[1], s, 0, &ms, &ml)) { ret = korb_str_dup(c, c->sp, rs[0]); goto sub_done; }
        rs[3] = korb_str_new(c, c->sp, s->ptr, ms);
        s = (struct korb_string *)rs[0];
        if (argc >= 2 && BUILTIN_TYPE(rs[2]) == T_STRING) {
            struct korb_string *r = (struct korb_string *)rs[2];
            korb_str_concat(c, c->sp, rs[3], korb_str_new(c, c->sp, r->ptr, r->len));
            s = (struct korb_string *)rs[0];
        } else if (c->current_block) {
            rs[4] = korb_str_new(c, c->sp, s->ptr + ms, ml);
            rs[4] = korb_yield(c, 1, &rs[4]);
            if (c->state == KORB_RAISE) { ret = Qnil; goto sub_done; }
            if (BUILTIN_TYPE(rs[4]) == T_STRING) korb_str_concat(c, c->sp, rs[3], rs[4]);
            else korb_str_concat(c, c->sp, rs[3], korb_to_s(c, c->sp, rs[4]));
            s = (struct korb_string *)rs[0];
        }
        korb_str_concat(c, c->sp, rs[3], korb_str_new(c, c->sp, s->ptr + ms + ml, s->len - ms - ml));
        ret = rs[3];
    sub_done: ;
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
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
    /* stage [self, argv...] onto sp and call new-ABI str_gsub */
    c->sp[0] = self;
    for (int i = 0; i < argc; i++) c->sp[1 + i] = argv[i];
    RESULT _g = str_gsub(c, argc, c->sp + 1 + argc);
    if (_g.state != KORB_NORMAL) {
        c->state = _g.state; c->state_value = _g.value;
        return Qnil;
    }
    VALUE replaced = _g.value;
    if (BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
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
    c->sp[0] = self;
    for (int i = 0; i < argc; i++) c->sp[1 + i] = argv[i];
    RESULT _s = str_sub(c, argc, c->sp + 1 + argc);
    if (_s.state != KORB_NORMAL) {
        c->state = _s.state; c->state_value = _s.value;
        return Qnil;
    }
    VALUE replaced = _s.value;
    if (BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
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
    if (argc < 1 || BUILTIN_TYPE(self) != T_STRING) return korb_ary_new(c, c->sp);
    const struct korb_string *s = (const struct korb_string *)self;
    VALUE out = korb_ary_new(c, c->sp);
    long ms, ml, i = 0;
    while (str_find_pat(argv[0], (struct korb_string *)s, i, &ms, &ml)) {
        korb_ary_push(out, korb_str_new(c, c->sp, s->ptr + ms, ml));
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
static VALUE str_tr_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool squeeze) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING)
        return korb_str_dup(c, c->sp, self);
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
    return korb_str_new(c, c->sp, out, w);
}

static RESULT str_tr(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_tr_impl(c, self, argc, argv, false));
}

static RESULT str_tr_s(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_tr_impl(c, self, argc, argv, true));
}

/* tr! / tr_s!: in-place.  Return self if changed, nil otherwise. */
static VALUE str_tr_bang_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool squeeze) {
    if (BUILTIN_TYPE(self) != T_STRING) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_string * const s = (struct korb_string *)self;
    VALUE replaced = str_tr_impl(c, self, argc, argv, squeeze);
    if (BUILTIN_TYPE(replaced) != T_STRING) return Qnil;
    const struct korb_string * const r = (const struct korb_string *)replaced;
    if (r->len == s->len && memcmp(r->ptr, s->ptr, s->len) == 0) return Qnil;
    char * const buf = korb_xmalloc_atomic(r->len + 1);
    memcpy(buf, r->ptr, r->len); buf[r->len] = 0;
    s->ptr = buf;
    s->len = r->len;
    return self;
}

static RESULT str_tr_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_tr_bang_impl(c, self, argc, argv, false));
}

static RESULT str_tr_s_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(str_tr_bang_impl(c, self, argc, argv, true));
}

/* sprintf — limited; supports %d %s %x %o %X %b %f %g %% %c, with width/0pad */
static RESULT kernel_format(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp, "", 0));
    /* Pin fmt (= argv[0]) and out across korb_str_new / korb_str_concat
     * GC fires.  Without this, under STRESS the C-local `fmt` goes
     * stale on every alloc inside the loop and fmt->ptr / fmt->len
     * read moved-out fields, producing wrong format output or false
     * return values from sprintf. */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = argv[0];  /* fmt */
        rs[1] = korb_str_new(c, c->sp, "", 0);  /* out */
    int ai = 1;
    struct korb_string *fmt = (struct korb_string *)rs[0];
    for (long i = 0; i < fmt->len; i++) {
        if (fmt->ptr[i] != '%') {
            korb_str_concat(c, c->sp, rs[1], korb_str_new(c, c->sp, fmt->ptr + i, 1));
            fmt = (struct korb_string *)rs[0];  /* reload */
            continue;
        }
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
                VALUE v = ai < argc ? argv[ai] : korb_str_new(c, c->sp, "", 0);
                if (BUILTIN_TYPE(v) != T_STRING) v = korb_to_s(c, c->sp, v);
                snprintf(buf, sizeof(buf), spec, ((struct korb_string *)v)->ptr);
                ai++;
                break;
            }
            default:
                snprintf(buf, sizeof(buf), "%%%c", conv);
        }
        korb_str_concat(c, c->sp, rs[1], korb_str_new_cstr(c, c->sp, buf));
        fmt = (struct korb_string *)rs[0];  /* reload after potential GC */
    }
    ret = rs[1];
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
}

/* printf — format then write to stdout */
static RESULT kernel_printf(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc == 0) return RESULT_OK(Qnil);
    VALUE s = UNWRAP(kernel_format(c, argc, sp));
    fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
    return RESULT_OK(Qnil);
}

/* String#center(width, padstr=" ") — center self within `width` cols. */
static RESULT str_center(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return RESULT_OK(self);
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return RESULT_OK(self);
    }
    long extra = width - s->len;
    long left = extra / 2, right = extra - left;
    char *buf = korb_xmalloc_atomic(width);
    for (long i = 0; i < left;  i++) buf[i] = pad[i % padlen];
    memcpy(buf + left, s->ptr, s->len);
    for (long i = 0; i < right; i++) buf[left + s->len + i] = pad[i % padlen];
    return RESULT_OK(korb_str_new(c, c->sp, buf, width));
}

/* String#ljust / rjust */
static RESULT str_ljust(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return RESULT_OK(self);
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return RESULT_OK(self);
    }
    long extra = width - s->len;
    char *buf = korb_xmalloc_atomic(width);
    memcpy(buf, s->ptr, s->len);
    for (long i = 0; i < extra; i++) buf[s->len + i] = pad[i % padlen];
    return RESULT_OK(korb_str_new(c, c->sp, buf, width));
}
static RESULT str_rjust(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long width = FIX2LONG(argv[0]);
    struct korb_string *s = (struct korb_string *)self;
    if (width <= s->len) return RESULT_OK(self);
    const char *pad = " "; long padlen = 1;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        struct korb_string *ps = (struct korb_string *)argv[1];
        pad = ps->ptr; padlen = ps->len;
        if (padlen == 0) return RESULT_OK(self);
    }
    long extra = width - s->len;
    char *buf = korb_xmalloc_atomic(width);
    for (long i = 0; i < extra; i++) buf[i] = pad[i % padlen];
    memcpy(buf + extra, s->ptr, s->len);
    return RESULT_OK(korb_str_new(c, c->sp, buf, width));
}

/* String#chop / chop! */
/* Walk back from the end of a UTF-8 string by one codepoint.  Returns
 * the byte offset where the last codepoint starts.  Continuation
 * bytes are (top two bits == 10). */
static long str_prev_char_offset(const char *p, long len) {
    if (len == 0) return 0;
    long i = len - 1;
    while (i > 0 && ((unsigned char)p[i] & 0xC0) == 0x80) i--;
    return i;
}

static RESULT str_chop(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return RESULT_OK(korb_str_new(c, c->sp, "", 0));
    long n = str_prev_char_offset(s->ptr, s->len);
    /* CRLF treated as one character. */
    if (n > 0 && s->ptr[n] == '\n' && s->ptr[n - 1] == '\r') n--;
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr, n));
}
static RESULT str_chop_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    CHECK_FROZEN_R(c, self);
    if (s->len == 0) return RESULT_OK(Qnil);
    long n = str_prev_char_offset(s->ptr, s->len);
    if (n > 0 && s->ptr[n] == '\n' && s->ptr[n - 1] == '\r') n--;
    s->len = n;
    s->ptr[n] = 0;
    return RESULT_OK(self);
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

static RESULT str_count_chars(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(INT2FIX(0));
    unsigned char bits[256];
    struct korb_string *cs = (struct korb_string *)argv[0];
    str_charclass_build(cs->ptr, cs->len, bits);
    struct korb_string *s = (struct korb_string *)self;
    long n = 0;
    for (long i = 0; i < s->len; i++) if (bits[(unsigned char)s->ptr[i]]) n++;
    return RESULT_OK(INT2FIX(n));
}

static RESULT str_delete_chars(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(self);
    unsigned char bits[256];
    struct korb_string *cs = (struct korb_string *)argv[0];
    str_charclass_build(cs->ptr, cs->len, bits);
    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len > 0 ? s->len : 1);
    long w = 0;
    for (long i = 0; i < s->len; i++) {
        if (!bits[(unsigned char)s->ptr[i]]) buf[w++] = s->ptr[i];
    }
    return RESULT_OK(korb_str_new(c, c->sp, buf, w));
}

static RESULT str_squeeze(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
    return RESULT_OK(korb_str_new(c, c->sp, buf, w));
}

static RESULT str_swapcase(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    char *buf = korb_xmalloc_atomic(s->len > 0 ? s->len : 1);
    for (long i = 0; i < s->len; i++) {
        unsigned char ch = s->ptr[i];
        if (ch >= 'a' && ch <= 'z')      buf[i] = ch - 32;
        else if (ch >= 'A' && ch <= 'Z') buf[i] = ch + 32;
        else                              buf[i] = ch;
    }
    return RESULT_OK(korb_str_new(c, c->sp, buf, s->len));
}

static RESULT str_capitalize(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return RESULT_OK(korb_str_new(c, c->sp, "", 0));
    char *buf = korb_xmalloc_atomic(s->len);
    unsigned char first = s->ptr[0];
    buf[0] = (first >= 'a' && first <= 'z') ? first - 32 : first;
    for (long i = 1; i < s->len; i++) {
        unsigned char ch = s->ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    return RESULT_OK(korb_str_new(c, c->sp, buf, s->len));
}

/* String#lines(sep = "\n", chomp: false) — split on separator. */
static RESULT str_lines(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    const char *sep = "\n";
    long sep_len = 1;
    bool chomp = false;
    int posargc = argc;

    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *kw = (struct korb_hash *)argv[argc - 1];
        VALUE chomp_key = korb_id2sym(korb_intern("chomp"));
        for (struct korb_hash_entry *e = kw->first; e; e = e->next) {
            if (korb_eql(c, e->key, chomp_key)) {
                chomp = RTEST(e->value);
                break;
            }
        }
        posargc--;
    }
    if (posargc > 0 && !SPECIAL_CONST_P(argv[0]) &&
        BUILTIN_TYPE(argv[0]) == T_STRING) {
        struct korb_string *septv = (struct korb_string *)argv[0];
        sep = septv->ptr;
        sep_len = septv->len;
    }

    VALUE r = korb_ary_new(c, c->sp);
    if (sep_len == 0) {
        korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, s->len));
        return RESULT_OK(r);
    }

    long start = 0;
    for (long i = 0; i + sep_len <= s->len; ) {
        if (memcmp(s->ptr + i, sep, sep_len) == 0) {
            long end = i + sep_len;
            long take_len;
            if (chomp) {
                take_len = i - start;
                if (sep[sep_len - 1] == '\n' &&
                    take_len > 0 && s->ptr[start + take_len - 1] == '\r') {
                    take_len--;
                }
            } else {
                take_len = end - start;
            }
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr + start, take_len));
            start = end;
            i = end;
        } else {
            i++;
        }
    }
    if (start < s->len) {
        long take_len = s->len - start;
        if (chomp && sep[sep_len - 1] == '\n' &&
            take_len > 0 && s->ptr[start + take_len - 1] == '\r') {
            take_len--;
        }
        korb_ary_push(r, korb_str_new(c, c->sp, s->ptr + start, take_len));
    }
    return RESULT_OK(r);
}

static RESULT str_partition(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *sep = (struct korb_string *)argv[0];
    VALUE r = korb_ary_new_capa(c, c->sp, 3);
    if (sep->len == 0 || sep->len > s->len) {
        korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, s->len));
        korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
        korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
        return RESULT_OK(r);
    }
    for (long i = 0; i + sep->len <= s->len; i++) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, i));
            korb_ary_push(r, korb_str_new(c, c->sp, sep->ptr, sep->len));
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr + i + sep->len, s->len - i - sep->len));
            return RESULT_OK(r);
        }
    }
    korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, s->len));
    korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
    korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
    return RESULT_OK(r);
}

static RESULT str_rpartition(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *sep = (struct korb_string *)argv[0];
    VALUE r = korb_ary_new_capa(c, c->sp, 3);
    if (sep->len == 0 || sep->len > s->len) {
        korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
        korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
        korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, s->len));
        return RESULT_OK(r);
    }
    for (long i = s->len - sep->len; i >= 0; i--) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, i));
            korb_ary_push(r, korb_str_new(c, c->sp, sep->ptr, sep->len));
            korb_ary_push(r, korb_str_new(c, c->sp, s->ptr + i + sep->len, s->len - i - sep->len));
            return RESULT_OK(r);
        }
    }
    korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
    korb_ary_push(r, korb_str_new(c, c->sp, "", 0));
    korb_ary_push(r, korb_str_new(c, c->sp, s->ptr, s->len));
    return RESULT_OK(r);
}

/* String#succ — alphabetic increment; ASCII-only, simplified rules. */
static RESULT str_succ(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) return RESULT_OK(korb_str_new(c, c->sp, "", 0));
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
        return RESULT_OK(korb_str_new(c, c->sp, grown, s->len + 1));
    }
    return RESULT_OK(korb_str_new(c, c->sp, buf, s->len));
}

/* String#each_byte — yields each byte as Integer. */
static RESULT str_each_byte(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        /* No block: return Enumerator (CRuby semantics).  Call to_enum
         * with the source method captured so #size works and chained
         * each(&blk) re-dispatches with the user's block. */
        VALUE arg = korb_id2sym(korb_intern("each_byte"));
        return RESULT_OK(korb_funcall(c, self, korb_intern("to_enum"), 1, &arg));
    }
    /* Read self from c->current_frame->self (auto-tracked). */
    for (long i = 0; i < ((struct korb_string *)c->current_frame->self)->len; i++) {
        struct korb_string *s = (struct korb_string *)c->current_frame->self;
        VALUE b = INT2FIX((unsigned char)s->ptr[i]);
        korb_yield(c, 1, &b);
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
    }
    return RESULT_OK(c->current_frame->self);
}

/* String#ord */
static RESULT str_ord(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_string *s = (struct korb_string *)self;
    if (s->len == 0) {
        return korb_raise(c, NULL, "empty string");
    }
    /* Decode the leading UTF-8 codepoint.  ASCII (0xxxxxxx) returns
     * the byte directly; multi-byte sequences combine continuation
     * bytes into the full code-point integer.  Match CRuby's #ord
     * (which returns the codepoint, not the raw byte). */
    const unsigned char *p = (const unsigned char *)s->ptr;
    long len = s->len;
    unsigned char b0 = p[0];
    if (b0 < 0x80) return RESULT_OK(INT2FIX(b0));
    int seq_len;
    long cp;
    if ((b0 & 0xe0) == 0xc0) { seq_len = 2; cp = b0 & 0x1f; }
    else if ((b0 & 0xf0) == 0xe0) { seq_len = 3; cp = b0 & 0x0f; }
    else if ((b0 & 0xf8) == 0xf0) { seq_len = 4; cp = b0 & 0x07; }
    else return RESULT_OK(INT2FIX(b0));  /* invalid leading byte: fall back */
    if (len < seq_len) return RESULT_OK(INT2FIX(b0));
    for (int i = 1; i < seq_len; i++) {
        unsigned char bi = p[i];
        if ((bi & 0xc0) != 0x80) return RESULT_OK(INT2FIX(b0));
        cp = (cp << 6) | (bi & 0x3f);
    }
    return RESULT_OK(INT2FIX(cp));
}

/* String#eql? — content equality; rejects non-strings. */
static RESULT str_eql(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qfalse);
    return str_eq(c, argc, sp);
}

/* String#clone — fresh independent copy.  Preserves frozen state
 * (clone does, dup doesn't — matching CRuby). */
static RESULT str_clone(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const struct korb_string *s = (const struct korb_string *)self;
    VALUE r = korb_str_new(c, c->sp, s->ptr, s->len);
    if (korb_obj_frozen_p(self)) {
        ((struct RBasic *)r)->head.flags |= FL_FROZEN;
    }
    return RESULT_OK(r);
}

/* String#% — same as format but self is the format string.  When the
 * argument is a Hash, the format string supports `%{name}` lookups
 * (e.g. `"%{a}+%{b}" % {a:1, b:2}` → `"1+2"`); otherwise we
 * delegate to the Array / single-arg printf-style path. */
static RESULT str_percent(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_HASH) {
        struct korb_string *fmt = (struct korb_string *)self;
        struct korb_hash *h = (struct korb_hash *)argv[0];
        VALUE out = korb_str_new(c, c->sp, "", 0);
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
                        VALUE v = korb_hash_aref(c, (VALUE)h, key);
                        if (UNDEF_P(v)) v = Qnil;
                        VALUE vs = korb_to_s(c, c->sp, v);
                        korb_str_concat(c, c->sp, out, vs);
                        i = j + 1;
                        continue;
                    }
                }
            }
            korb_str_concat(c, c->sp, out, korb_str_new(c, c->sp, fmt->ptr + i, 1));
            i++;
        }
        return RESULT_OK(out);
    }
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        /* kernel_format ABI: self = format string, argv[0] = format string,
         * argv[1..] = format args.  Stage [self, fmt, a[0..len-1]] on sp. */
        sp[0] = self;        /* kernel_format's self (format string) */
        sp[1] = self;        /* argv[0] = format string */
        for (long i = 0; i < a->len; i++) sp[2 + i] = a->ptr[i];
        return kernel_format(c, 1 + (int)a->len, sp + 2 + (int)a->len);
    }
    /* single arg or multiple */
    sp[0] = self;        /* kernel_format's self */
    sp[1] = self;        /* argv[0] = format string */
    for (int i = 0; i < argc; i++) sp[2 + i] = argv[i];
    return kernel_format(c, 1 + argc, sp + 2 + argc);
}

/* ---------- String#hex ----------
 * Parses an optional sign, optional "0x"/"0X" prefix, then hex digits.
 * Stops at first non-digit; returns 0 for fully unparsable input. */
static RESULT str_hex(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
    if (!any) return RESULT_OK(INT2FIX(0));
    return RESULT_OK(INT2FIX(v * sign));
}

/* ---------- String#oct ----------
 * Returns the integer parsed using base inferred from prefix:
 *   "0x"/"0X" → 16, "0b"/"0B" → 2, "0o"/"0O" or just leading '0' → 8,
 *   anything else → 10.  Sign-aware; 0 on no digits parsed. */
static RESULT str_oct(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
    if (!any) return RESULT_OK(INT2FIX(0));
    return RESULT_OK(INT2FIX(v * sign));
}

/* ---------- String#prepend ----------
 * Mutates self by inserting other(s) at position 0; returns self. */
static RESULT str_prepend(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
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
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
                if (RTEST(rt)) {
                    a = korb_funcall(c, a, korb_intern("to_str"), 0, NULL);
                    if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
                }
            }
            if (SPECIAL_CONST_P(a) || BUILTIN_TYPE(a) != T_STRING) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into String",
                           SPECIAL_CONST_P(argv[i]) ? "(special)"
                               : korb_id_name(korb_class_of_class(argv[i])->name));
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
    return RESULT_OK(self);
}

/* ---------- String#insert(pos, str) ----------
 * Mutates self.  pos can be negative (counts from end + 1, so -1
 * inserts before the last char as in CRuby).  Returns self. */
/* Convert a character index (positive or negative) into a byte
 * offset for a UTF-8 string.  Returns -1 if out of range (caller
 * should raise IndexError). */
static long str_char_to_byte_index(const char *p, long byte_len, long char_idx) {
    if (char_idx >= 0) {
        long b = 0;
        long i = 0;
        while (b < byte_len && i < char_idx) {
            int n = 1;
            unsigned char c0 = (unsigned char)p[b];
            if      ((c0 & 0x80) == 0x00) n = 1;
            else if ((c0 & 0xE0) == 0xC0) n = 2;
            else if ((c0 & 0xF0) == 0xE0) n = 3;
            else if ((c0 & 0xF8) == 0xF0) n = 4;
            if (b + n > byte_len) return -1;
            b += n;
            i++;
        }
        if (i < char_idx) return -1;
        return b;
    } else {
        /* Count from end: walk forward, recording each codepoint start,
         * then index from the back. */
        long count = 0;
        long b = 0;
        while (b < byte_len) {
            int n = 1;
            unsigned char c0 = (unsigned char)p[b];
            if      ((c0 & 0x80) == 0x00) n = 1;
            else if ((c0 & 0xE0) == 0xC0) n = 2;
            else if ((c0 & 0xF0) == 0xE0) n = 3;
            else if ((c0 & 0xF8) == 0xF0) n = 4;
            b += n;
            count++;
        }
        long want = count + char_idx + 1; /* insert-style: -1 means "before end" */
        if (want < 0) return -1;
        if (want == count) return byte_len;
        /* Walk again to find the want'th char start. */
        long i = 0;
        b = 0;
        while (b < byte_len && i < want) {
            int n = 1;
            unsigned char c0 = (unsigned char)p[b];
            if      ((c0 & 0x80) == 0x00) n = 1;
            else if ((c0 & 0xE0) == 0xC0) n = 2;
            else if ((c0 & 0xF0) == 0xE0) n = 3;
            else if ((c0 & 0xF8) == 0xF0) n = 4;
            b += n;
            i++;
        }
        return b;
    }
}

static RESULT str_insert(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 2) return RESULT_OK(self);
    long pos;
    if (FIXNUM_P(argv[0])) {
        pos = FIX2LONG(argv[0]);
    } else {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return RESULT_OK(Qnil);
        pos = FIX2LONG(iv);
    }
    /* Coerce other to a String via #to_str (TypeError on failure). */
    VALUE other = (SPECIAL_CONST_P(argv[1]) || BUILTIN_TYPE(argv[1]) != T_STRING)
        ? str_coerce_arg(c, argv[1]) : argv[1];
    if (UNDEF_P(other) || c->state == KORB_RAISE) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)other;
    long orig_pos = pos;
    long bpos = str_char_to_byte_index(s->ptr, s->len, pos);
    if (bpos < 0) {
        VALUE eI = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
        return korb_raise(c, (struct korb_class *)eI,
                   "index %ld out of string", orig_pos);
    }
    long total = s->len + p->len;
    char *np = korb_xmalloc_atomic(total + 1);
    memcpy(np, s->ptr, bpos);
    memcpy(np + bpos, p->ptr, p->len);
    memcpy(np + bpos + p->len, s->ptr + bpos, s->len - bpos);
    np[total] = 0;
    s->ptr = np;
    s->len = total;
    s->capa = total;
    return RESULT_OK(self);
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
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    DROP_RESULT(korb_raise(c, (struct korb_class *)eT,
               "no implicit conversion of %s into String",
               SPECIAL_CONST_P(arg) ? "(special)"
                   : korb_id_name(korb_class_of_class(arg)->name)));
    return Qundef;
}

/* Returns true if a UTF-8 prefix/suffix match would split a multi-byte
 * codepoint.  For prefix removal, byte at `boundary` (= prefix len) must
 * NOT be a UTF-8 continuation byte.  For suffix, byte at `boundary - 1`
 * must terminate cleanly — equivalent here since we check the byte right
 * after the prefix end. */
static bool str_at_char_boundary(const char *p, long boundary, long total_len) {
    if (boundary >= total_len) return true;
    return ((unsigned char)p[boundary] & 0xC0) != 0x80;
}

static RESULT str_delete_prefix(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(korb_str_new(c, c->sp, ((struct korb_string *)self)->ptr,
                                       ((struct korb_string *)self)->len));
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0 &&
        str_at_char_boundary(s->ptr, p->len, s->len))
        return RESULT_OK(korb_str_new(c, c->sp, s->ptr + p->len, s->len - p->len));
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr, s->len));
}

static RESULT str_delete_suffix(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(korb_str_new(c, c->sp, ((struct korb_string *)self)->ptr,
                                       ((struct korb_string *)self)->len));
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    long cut = s->len - p->len;
    if (p->len <= s->len && memcmp(s->ptr + cut, p->ptr, p->len) == 0 &&
        str_at_char_boundary(s->ptr, cut, s->len))
        return RESULT_OK(korb_str_new(c, c->sp, s->ptr, cut));
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr, s->len));
}

/* In-place variants: mutate self; return self on change, nil on no-op. */
static RESULT str_delete_prefix_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1) return RESULT_OK(Qnil);
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    if (p->len == 0 || p->len > s->len ||
        memcmp(s->ptr, p->ptr, p->len) != 0 ||
        !str_at_char_boundary(s->ptr, p->len, s->len)) return RESULT_OK(Qnil);
    long new_len = s->len - p->len;
    char *np = korb_xmalloc_atomic(new_len + 1);
    memcpy(np, s->ptr + p->len, new_len);
    np[new_len] = 0;
    s->ptr = np;
    s->len = new_len;
    s->capa = new_len;
    return RESULT_OK(self);
}

static RESULT str_delete_suffix_bang(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1) return RESULT_OK(Qnil);
    VALUE arg = str_coerce_arg(c, argv[0]);
    if (UNDEF_P(arg)) return RESULT_OK(Qnil);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)arg;
    long cut = s->len - p->len;
    if (p->len == 0 || p->len > s->len ||
        memcmp(s->ptr + cut, p->ptr, p->len) != 0 ||
        !str_at_char_boundary(s->ptr, cut, s->len)) return RESULT_OK(Qnil);
    s->len -= p->len;
    if (s->capa > s->len) s->ptr[s->len] = 0;
    return RESULT_OK(self);
}

/* ---------- String#each_line (real impl) ----------
 * Was registered as str_split, which split on whitespace.  Walks the
 * string yielding each line including its trailing '\n'; returns self. */
static RESULT str_each_line(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const struct korb_string *s = (const struct korb_string *)self;
    bool has_block = korb_block_given(c);
    VALUE collected = has_block ? Qnil : korb_ary_new(c, c->sp);
    long start = 0;
    for (long i = 0; i < s->len; i++) {
        if (s->ptr[i] == '\n') {
            VALUE line = korb_str_new(c, c->sp, s->ptr + start, i - start + 1);
            if (has_block) {
                korb_yield(c, 1, &line);
                if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
            } else {
                korb_ary_push(collected, line);
            }
            start = i + 1;
        }
    }
    if (start < s->len) {
        VALUE line = korb_str_new(c, c->sp, s->ptr + start, s->len - start);
        if (has_block) {
            korb_yield(c, 1, &line);
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        } else {
            korb_ary_push(collected, line);
        }
    }
    return RESULT_OK(has_block ? self : collected);
}

/* Encoding stubs (we don't track per-string encoding). */
RESULT _str_encoding(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_class *cEnc = (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("Encoding"));
    if (!cEnc) return RESULT_OK(Qnil);
    return RESULT_OK(korb_const_get(cEnc, korb_intern("UTF_8")));
}
RESULT _str_force_encoding(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(self);
}
/* String#b — return a copy of self with ASCII-8BIT encoding.  We don't
 * track per-string encoding so a dup is enough for behavioral equality. */
RESULT _str_b(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_STRING) return RESULT_OK(self);
    struct korb_string *s = (struct korb_string *)self;
    return RESULT_OK(korb_str_new(c, c->sp, s->ptr, s->len));
}
RESULT _enc_name(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE name = korb_ivar_get(self, korb_intern("@name"));
    if (UNDEF_P(name) || NIL_P(name)) return RESULT_OK(korb_str_new_cstr(c, c->sp, "UTF-8"));
    return RESULT_OK(name);
}
RESULT _enc_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return _enc_name(c, argc, sp);
}
RESULT _enc_default_external(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_const_get((struct korb_class *)self, korb_intern("UTF_8")));
}
RESULT _enc_default_internal(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(Qnil);
}
RESULT _enc_find(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_const_get((struct korb_class *)self, korb_intern("UTF_8")));
}
