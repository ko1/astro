/* Array — moved from builtins.c.  Included from builtins.c so we
 * inherit its includes/macros (KORB_BOOL, korb_intern, etc.). */

/* Array#to_a — for a plain Array, returns self.  For subclasses,
 * returns a fresh Array with the same contents (CRuby semantics). */
static VALUE ary_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (korb_class_of_class(self) == korb_vm->array_class) return self;
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* Array#to_ary / Array#deconstruct — return self.  to_ary is the
 * canonical "I behave as an array" hook used in argument splatting and
 * pattern matching; deconstruct is the analogous pattern-match hook. */
static VALUE ary_self(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
}

/* Coerce v to an Integer via #to_int (CRuby protocol).  Returns the
 * Fixnum/Bignum on success.  On failure raises TypeError and returns
 * Qundef.  Already-Integer values pass through. */
static VALUE korb_to_int_or_raise(CTX *c, VALUE v) {
    if (FIXNUM_P(v)) return v;
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_BIGNUM) return v;
    /* Float / mock / user object — try #to_int.  Float defines to_int
     * (truncates), heap objects can override.  Special-const values
     * other than Float (true/false/nil/Symbol) reject below. */
    bool is_real = !SPECIAL_CONST_P(v);
    bool is_float = FLONUM_P(v) || (is_real && BUILTIN_TYPE(v) == T_FLOAT);
    if (is_real || is_float) {
        VALUE rt = korb_funcall(c, v, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_int")) });
        if (c->state == KORB_RAISE) return Qundef;
        if (RTEST(rt)) {
            VALUE r = korb_funcall(c, v, korb_intern("to_int"), 0, NULL);
            if (c->state == KORB_RAISE) return Qundef;
            if (FIXNUM_P(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_BIGNUM)) {
                return r;
            }
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            const char *src_n = is_float ? "Float"
                                : korb_id_name(korb_class_of_class(v)->name);
            korb_raise(c, (struct korb_class *)eT,
                       "can't convert %s to Integer (%s#to_int gives %s)",
                       src_n, src_n,
                       SPECIAL_CONST_P(r) ? "(special)"
                           : korb_id_name(korb_class_of_class(r)->name));
            return Qundef;
        }
    }
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    const char *cn;
    if (v == Qtrue) cn = "true";
    else if (v == Qfalse) cn = "false";
    else if (v == Qnil) cn = "nil";
    else if (FLONUM_P(v)) cn = "Float";
    else if (SYMBOL_P(v)) cn = "Symbol";
    else if (SPECIAL_CONST_P(v)) cn = "(special)";
    else cn = korb_id_name(korb_class_of_class(v)->name);
    korb_raise(c, (struct korb_class *)eT,
               "no implicit conversion of %s into Integer", cn);
    return Qundef;
}

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
            VALUE res = korb_ary_new(c, c->sp);
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
        VALUE r = korb_ary_new_capa(c, c->sp, len);
        for (long i = 0; i < len; i++) korb_ary_push(r, a->ptr[start + i]);
        return r;
    }
    return Qnil;
}
/* Reject indices that would resize the array beyond a reasonable
 * limit.  CRuby uses LONG_MAX/sizeof(VALUE) (~1.15e18); we use the
 * same bound — large enough that real code never hits it but small
 * enough that test_aset_error's `[0][LONGP] = 2` raises IndexError
 * instead of OOM-killing the process while expanding to 2^63 slots. */
#define KORB_ARY_MAX_LEN ((long)(LONG_MAX / sizeof(VALUE)))
static bool korb_ary_check_index(CTX *c, long idx) {
    if (idx >= KORB_ARY_MAX_LEN) {
        VALUE eIE = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
        korb_raise(c, (struct korb_class *)eIE, "index %ld too big", idx);
        return false;
    }
    return true;
}
static VALUE ary_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc == 2 && FIXNUM_P(argv[0])) {
        long i = FIX2LONG(argv[0]);
        struct korb_array *a = (struct korb_array *)self;
        if (i < 0 && i + a->len < 0) {
            VALUE eIE = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
            korb_raise(c, (struct korb_class *)eIE,
                       "index %ld too small for array; minimum: -%ld",
                       i, a->len);
            return Qnil;
        }
        if (!korb_ary_check_index(c, i)) return Qnil;
        korb_ary_aset(self, i, argv[1]);
        return argv[1];
    }
    if (argc == 2 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_BIGNUM) {
        VALUE eIE = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
        korb_raise(c, (struct korb_class *)eIE, "index too big");
        return Qnil;
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
        if (!korb_ary_check_index(c, start)) return argv[2];
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
            /* `a[start, len] = val` — when val is NOT an Array, CRuby
             * replaces the slice [start, start+len) with the SINGLE
             * element val (i.e. removes len elements, inserts 1).
             * If start > a->len, pad with nil first. */
            if (start > a->len) {
                while (a->len < start) korb_ary_push(self, Qnil);
            }
            long avail_len = a->len - start;
            if (len > avail_len) len = avail_len;
            long diff = 1 - len;  /* +1 inserted, -len removed */
            long old = a->len;
            if (diff > 0) {
                for (long i = 0; i < diff; i++) korb_ary_push(self, Qnil);
                for (long i = old - 1; i >= start + len; i--) a->ptr[i + diff] = a->ptr[i];
            } else if (diff < 0) {
                for (long i = start + len; i < old; i++) a->ptr[i + diff] = a->ptr[i];
                a->len += diff;
            }
            a->ptr[start] = val;
        }
        return argv[2];
    }
    return Qnil;
}
static VALUE ary_push(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    for (int i = 0; i < argc; i++) korb_ary_push(self, argv[i]);
    return self;
}
static VALUE ary_pop(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc > 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", argc);
        return Qnil;
    }
    if (argc >= 1) {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (UNDEF_P(iv)) return Qnil;
        if (!FIXNUM_P(iv)) return Qnil;  /* Bignum n: way bigger than array */
        long n = FIX2LONG(iv);
        if (n < 0) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg, "negative array size");
            return Qnil;
        }
        struct korb_array *a = (struct korb_array *)self;
        long take = n > a->len ? a->len : n;
        VALUE out = korb_ary_new_capa(c, c->sp, take);
        long start = a->len - take;
        for (long i = start; i < a->len; i++) korb_ary_push(out, a->ptr[i]);
        a->len = start;
        return out;
    }
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
    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("each"));
        return korb_funcall(c, self, korb_intern("to_enum"), 1, &arg);
    }
    /* CRuby semantics: re-read the length each iteration so the block
     * can grow / shrink the array.  Out-of-range index after a shrink
     * stops iteration normally. */
    for (long i = 0; i < korb_ary_len(self); i++) {
        VALUE v = korb_ary_aref(self, i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_each_with_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    if (!korb_block_given(c)) {
        VALUE r = korb_ary_new_capa(c, c->sp, len);
        for (long i = 0; i < len; i++) {
            VALUE pair = korb_ary_new_capa(c, c->sp, 2);
            korb_ary_push(pair, korb_ary_aref(self, i));
            korb_ary_push(pair, INT2FIX(i));
            korb_ary_push(r, pair);
        }
        return r;
    }
    for (long i = 0; i < len; i++) {
        VALUE args[2] = { korb_ary_aref(self, i), INT2FIX(i) };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!korb_block_given(c)) return self;
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(c, c->sp, len);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(r, m);
    }
    return r;
}
static VALUE ary_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!korb_block_given(c)) return self;
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new(c, c->sp);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(r, v);
    }
    return r;
}
static VALUE ary_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* CRuby's reduce / inject overloads:
     *   reduce(sym)              — reduce by sending sym (e.g. :+)
     *   reduce(init, sym)        — same with explicit init
     *   reduce { |a, b| ... }    — block-driven, no init
     *   reduce(init) { |a, b| }  — block-driven with init
     * The Symbol form short-circuits the yield path entirely. */
    long len = korb_ary_len(self);
    /* Detect "last positional arg is a Symbol" → reduce-by-method form. */
    ID op = 0;
    int sym_idx = -1;
    if (argc >= 1 && SYMBOL_P(argv[argc - 1]) && !korb_block_given(c)) {
        op = korb_sym2id(argv[argc - 1]);
        sym_idx = argc - 1;
    }
    VALUE acc;
    long i;
    if (op != 0) {
        /* Symbol form */
        if (sym_idx == 0) { /* reduce(:+) */
            if (len == 0) return Qnil;
            acc = korb_ary_aref(self, 0);
            i = 1;
        } else {            /* reduce(init, :+) */
            acc = argv[0];
            i = 0;
        }
        for (; i < len; i++) {
            VALUE other = korb_ary_aref(self, i);
            acc = korb_funcall(c, acc, op, 1, &other);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        return acc;
    }
    /* Block form */
    acc = argc > 0 ? argv[0] : korb_ary_aref(self, 0);
    i = argc > 0 ? 0 : 1;
    for (; i < len; i++) {
        VALUE args[2] = { acc, korb_ary_aref(self, i) };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}
static VALUE ary_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* CRuby short-circuit: empty array returns "" without touching sep. */
    if (korb_ary_len(self) == 0) return korb_str_new(c, c->sp, "", 0);
    /* Resolve the separator:
     *  - no arg or explicit nil: use $, (CRuby default), else "".
     *  - String: use as is.
     *  - other: call #to_str (TypeError on failure / non-String result).
     */
    VALUE sep;
    if (argc < 1 || NIL_P(argv[0])) {
        VALUE g = korb_gvar_get(korb_intern("$,"));
        if (!SPECIAL_CONST_P(g) && BUILTIN_TYPE(g) == T_STRING) {
            sep = g;
        } else {
            sep = korb_str_new_cstr(c, c->sp, "");
        }
    } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        sep = argv[0];
    } else {
        VALUE rt = korb_funcall(c, argv[0], korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return Qnil;
        if (!RTEST(rt)) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
        sep = korb_funcall(c, argv[0], korb_intern("to_str"), 0, NULL);
        if (c->state == KORB_RAISE) return Qnil;
        if (SPECIAL_CONST_P(sep) || BUILTIN_TYPE(sep) != T_STRING) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "can't convert to String (to_str returned non-String)");
            return Qnil;
        }
    }
    /* Pin self / sep / result / per-iter element across korb_to_s /
     * korb_str_new / korb_str_concat GC fires (PURGE catches the
     * stale-pointer faster than STRESS alone). */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 4) {
        rs[0] = self;
        rs[1] = sep;
        rs[2] = korb_str_new(c, c->sp, "", 0);  /* result */
        rs[3] = Qnil;                  /* per-iter element */
        long len = korb_ary_len(rs[0]);
        for (long i = 0; i < len; i++) {
            if (i > 0 && BUILTIN_TYPE(rs[1]) == T_STRING) korb_str_concat(c, c->sp, rs[2], rs[1]);
            rs[3] = korb_ary_aref(rs[0], i);
            if (BUILTIN_TYPE(rs[3]) != T_STRING) rs[3] = korb_to_s(c, c->sp, rs[3]);
            korb_str_concat(c, c->sp, rs[2], rs[3]);
        }
        ret = rs[2];
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}
static VALUE ary_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_inspect(c, c->sp, self);
}

/* Array#to_h — convert [[k,v], [k,v], ...] (or yield-pair-from-block)
 * into a Hash.  With a block, the block's return value (a 2-element
 * Array) supplies the pair for each element — mirrors CRuby's
 * `[1,2,3].to_h { |i| [i, i*i] }` form. */
static VALUE ary_to_h(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    VALUE h = korb_hash_new(c, c->sp);
    bool has_block = korb_block_given(c);
    for (long i = 0; i < a->len; i++) {
        VALUE pair = a->ptr[i];
        if (has_block) {
            pair = korb_yield(c, 1, &a->ptr[i]);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        if (BUILTIN_TYPE(pair) != T_ARRAY || ((struct korb_array *)pair)->len != 2) {
            VALUE eType = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eType,
                       "wrong element type (expected 2-element Array)");
            return Qnil;
        }
        struct korb_array *p = (struct korb_array *)pair;
        korb_hash_aset(c, h, p->ptr[0], p->ptr[1]);
    }
    return h;
}
/* New sp-based RESULT-returning ABI (Phase 3 PoC).
 *
 * Convention:
 *   sp[-2] = self (the array on the LHS)
 *   sp[-1] = other (the RHS arg)
 *   sp[0..] = scratch (unused here)
 *
 * Both slots are in c->sp range so visit_roots auto-forwards them across
 * any GC fired by inner korb_eq dispatches.  No ARO_ROOT_SCOPE_START
 * boilerplate needed. */
static RESULT ary_eq(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;  /* alloc 前 sync: korb_eq -> method dispatch が GC を起こしうる */
    if (BUILTIN_TYPE(sp[-1]) != T_ARRAY) return RESULT_OK(Qfalse);
    long la = korb_ary_len(sp[-2]);
    long lb = korb_ary_len(sp[-1]);
    if (la != lb) return RESULT_OK(Qfalse);
    for (long i = 0; i < la; i++) {
        /* Re-read sp[-2]/sp[-1] each iter — they're slot-tracked, so even
         * if korb_eq's inner dispatch fires GC and moves the arrays, the
         * next iteration's korb_ary_aref reads the forwarded address. */
        if (!korb_eq(c, korb_ary_aref(sp[-2], i), korb_ary_aref(sp[-1], i))) {
            return RESULT_OK(Qfalse);
        }
        if (UNLIKELY(c->state != KORB_NORMAL)) {
            RESULT r = { c->state_value, (uint8_t)c->state };
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            return r;
        }
    }
    return RESULT_OK(Qtrue);
}
static VALUE ary_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    korb_ary_push(self, argv[0]);
    return self;
}
static VALUE ary_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(c, c->sp, len);
    for (long i = 0; i < len; i++) korb_ary_push(r, korb_ary_aref(self, i));
    return r;
}


/* ---------- Array methods (extended) ---------- */

/* Compare two values using either the supplied block or default `<=>`,
 * returning a negative/zero/positive long like a C sort comparator. */
static long ary_sort_compare(CTX *c, VALUE x, VALUE y, bool has_block) {
    VALUE r;
    if (has_block) {
        VALUE pair[2] = { x, y };
        r = korb_yield(c, 2, pair);
    } else if (FIXNUM_P(x) && FIXNUM_P(y)) {
        return (intptr_t)x < (intptr_t)y ? -1 : (intptr_t)x > (intptr_t)y ? 1 : 0;
    } else {
        r = korb_funcall(c, x, korb_intern("<=>"), 1, &y);
    }
    /* CRuby: sort block return is used by sign — Fixnum sign extracted
     * directly; Bignum compared against 0 via korb_int_cmp; Float by
     * sign; nil → caller raises ArgumentError (we treat as equal for now). */
    if (FIXNUM_P(r)) return FIX2LONG(r);
    if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_BIGNUM) {
        return korb_int_cmp(r, INT2FIX(0));
    }
    if (KORB_IS_FLOAT(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT)) {
        double d = korb_num2dbl(r);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    return 0;
}

static void ary_sort_in_place(CTX *c, struct korb_array *ra, bool has_block) {
    long n = ra->len;
    /* Pin the "probe" value v across korb_yield/funcall GC fires.  The
     * array storage (ra->ptr[]) is libc-tracked so its entries auto-
     * forward, but the C-local `v` would go stale after GC moves the
     * referent.  Stage on the value stack so visit_roots picks it up. */
    ARO_ROOT_SCOPE_START(c, rs, 1) {
        for (long i = 1; i < n; i++) {
            rs[0] = ra->ptr[i];
            long j = i - 1;
            while (j >= 0) {
                long cmp = ary_sort_compare(c, ra->ptr[j], rs[0], has_block);
                if (c->state != KORB_NORMAL) goto done;
                if (cmp <= 0) break;
                ra->ptr[j+1] = ra->ptr[j];
                j--;
            }
            ra->ptr[j+1] = rs[0];
        }
done:   ;
    } ARO_ROOT_SCOPE_END(c, rs);
}

static VALUE ary_sort(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    VALUE r = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    ary_sort_in_place(c, (struct korb_array *)r, korb_block_given(c));
    return r;
}

/* Array#sort! — mutates self, returns self.  The existing
 * registration aliases sort! to ary_sort which would build a copy and
 * return it; for the bang form we need to sort the receiver directly. */
static VALUE ary_sort_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    ary_sort_in_place(c, (struct korb_array *)self, korb_block_given(c));
    return self;
}

static VALUE ary_sort_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* yield each, then sort by yielded value */
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    VALUE pairs = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE pair = korb_ary_new_capa(c, c->sp, 2);
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
    VALUE r = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(r, ((struct korb_array *)p->ptr[i])->ptr[1]);
    return r;
}

static VALUE ary_zip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) {
        VALUE tup = korb_ary_new_capa(c, c->sp, 1 + argc);
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

/* Tracks an in-progress descent through nested arrays to detect cycles
 * (`a << a; a.flatten`).  CRuby raises ArgumentError once it hits a
 * subarray it's already descended into. */
struct ary_flatten_stack {
    VALUE *items;
    long len;
    long capa;
};
static bool ary_flatten_stack_contains(const struct ary_flatten_stack *s, VALUE v) {
    for (long i = 0; i < s->len; i++) if (s->items[i] == v) return true;
    return false;
}
static int ary_flatten_into(CTX *c, VALUE r, VALUE src, long depth,
                            struct ary_flatten_stack *stack) {
    struct korb_array *a = (struct korb_array *)src;
    for (long i = 0; i < a->len; i++) {
        VALUE el = a->ptr[i];
        VALUE coerced = el;
        bool is_ary = !SPECIAL_CONST_P(el) && BUILTIN_TYPE(el) == T_ARRAY;
        if (depth != 0 && !is_ary) {
            /* Try #to_ary if the element responds to it (CRuby flattens
             * via #to_ary, not method_missing).  Skip on Array — it
             * already IS an array. */
            VALUE rt = korb_funcall(c, el, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) });
            if (c->state == KORB_RAISE) return -1;
            if (RTEST(rt)) {
                VALUE ar = korb_funcall(c, el, korb_intern("to_ary"), 0, NULL);
                if (c->state == KORB_RAISE) return -1;
                if (NIL_P(ar)) {
                    /* nil result: leave element as is. */
                } else if (SPECIAL_CONST_P(ar) || BUILTIN_TYPE(ar) != T_ARRAY) {
                    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                    korb_raise(c, (struct korb_class *)eT,
                               "can't convert to Array (to_ary returned non-Array)");
                    return -1;
                } else {
                    coerced = ar;
                    is_ary = true;
                }
            }
        }
        if (depth != 0 && is_ary) {
            if (ary_flatten_stack_contains(stack, coerced)) {
                VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
                korb_raise(c, (struct korb_class *)eA, "tried to flatten recursive array");
                return -1;
            }
            if (stack->len == stack->capa) {
                long nc = stack->capa ? stack->capa * 2 : 8;
                VALUE *nb = korb_xmalloc(sizeof(VALUE) * nc);
                for (long k = 0; k < stack->len; k++) nb[k] = stack->items[k];
                stack->items = nb; stack->capa = nc;
            }
            stack->items[stack->len++] = coerced;
            int rc = ary_flatten_into(c, r, coerced, depth - 1, stack);
            stack->len--;
            if (rc != 0) return rc;
        } else {
            korb_ary_push(r, el);
        }
    }
    return 0;
}

static VALUE ary_flatten(CTX *c, VALUE self, int argc, VALUE *argv) {
    long depth = -1;
    if (argc >= 1 && !NIL_P(argv[0])) {
        VALUE d = argv[0];
        if (!FIXNUM_P(d)) {
            d = korb_to_int_or_raise(c, d);
            if (c->state == KORB_RAISE) return Qnil;
        }
        if (!FIXNUM_P(d)) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
            return Qnil;
        }
        depth = FIX2LONG(d);
    }
    VALUE r = korb_ary_new(c, c->sp);
    struct ary_flatten_stack stack = { NULL, 0, 0 };
    /* Push self so the immediate `a << a` cycle is caught. */
    VALUE init[1] = { self };
    stack.items = init; stack.len = 1; stack.capa = 1;
    ary_flatten_into(c, r, self, depth, &stack);
    return r;
}

/* Array#flatten! — destructive: replace self with the flattened result.
 * Returns self if flattening changed anything, nil otherwise.  Raises
 * FrozenError unconditionally on a frozen receiver before doing any
 * argument coercion (CRuby semantic for the bang). */
static VALUE ary_flatten_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    long depth = -1;
    if (argc >= 1 && !NIL_P(argv[0])) {
        VALUE d = argv[0];
        if (!FIXNUM_P(d)) {
            d = korb_to_int_or_raise(c, d);
            if (c->state == KORB_RAISE) return Qnil;
        }
        if (!FIXNUM_P(d)) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
            return Qnil;
        }
        depth = FIX2LONG(d);
    }
    /* Compute the flattened result in a fresh array, then check whether
     * it differs from self.  Replace self's storage on change. */
    VALUE r = korb_ary_new(c, c->sp);
    struct ary_flatten_stack stack = { NULL, 0, 0 };
    VALUE init[1] = { self };
    stack.items = init; stack.len = 1; stack.capa = 1;
    ary_flatten_into(c, r, self, depth, &stack);
    if (c->state == KORB_RAISE) return Qnil;
    struct korb_array *me = (struct korb_array *)self;
    struct korb_array *fr = (struct korb_array *)r;
    bool changed = (me->len != fr->len);
    if (!changed) {
        for (long i = 0; i < me->len; i++) {
            if (me->ptr[i] != fr->ptr[i]) { changed = true; break; }
        }
    }
    if (!changed) return Qnil;
    /* Adopt the new buffer. */
    me->ptr = fr->ptr;
    me->len = fr->len;
    me->capa = fr->capa;
    return self;
}

static VALUE ary_compact(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new(c, c->sp);
    for (long i = 0; i < a->len; i++) if (!NIL_P(a->ptr[i])) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* Array#compact! — destructive: remove nil in place; return self if any
 * change, nil if no nil was removed (CRuby semantic for the bang). */
static VALUE ary_compact_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    long w = 0;
    bool any = false;
    for (long r = 0; r < a->len; r++) {
        if (NIL_P(a->ptr[r])) { any = true; continue; }
        if (w != r) a->ptr[w] = a->ptr[r];
        w++;
    }
    if (!any) return Qnil;
    a->len = w;
    return self;
}

static VALUE ary_uniq(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new(c, c->sp);
    for (long i = 0; i < a->len; i++) {
        bool dup = false;
        struct korb_array *ra = (struct korb_array *)r;
        for (long j = 0; j < ra->len; j++) {
            if (korb_eq(c, ra->ptr[j], a->ptr[i])) { dup = true; break; }
        }
        if (!dup) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_array *a = (struct korb_array *)self;
    /* CRuby calls element == obj (left-to-right), letting user-defined
     * == on elements decide.  korb_eq does identity-shortcut + dispatches
     * to ==, but we want to dispatch on the ELEMENT's == (not obj's). */
    for (long i = 0; i < a->len; i++) {
        if (a->ptr[i] == argv[0]) return Qtrue;  /* identity fast path */
        VALUE r = korb_funcall(c, a->ptr[i], korb_intern("=="), 1, &argv[0]);
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(r)) return Qtrue;
    }
    return Qfalse;
}

/* Predicates with optional pattern arg + optional block.
 *   any?           — any element truthy
 *   any?(pat)      — pat === elem for any element
 *   any? { blk }   — blk(elem) truthy for any element
 * If both pattern and block are given, CRuby uses the block (and warns).
 * Returns Qtrue/Qfalse. */
static int ary_predicate_match(CTX *c, VALUE elem, int argc, VALUE *argv) {
    if (korb_block_given(c)) {
        VALUE r = korb_yield(c, 1, &elem);
        return RTEST(r);
    }
    if (argc >= 1) {
        VALUE r = korb_funcall(c, argv[0], korb_intern("==="), 1, &elem);
        return RTEST(r);
    }
    return RTEST(elem);
}

static VALUE ary_any_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (ary_predicate_match(c, a->ptr[i], argc, argv)) return Qtrue;
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return Qfalse;
}

static VALUE ary_all_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (!ary_predicate_match(c, a->ptr[i], argc, argv)) return Qfalse;
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return Qtrue;
}

static VALUE ary_none_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (ary_predicate_match(c, a->ptr[i], argc, argv)) return Qfalse;
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return Qtrue;
}

static VALUE ary_one_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    long count = 0;
    for (long i = 0; i < a->len; i++) {
        if (ary_predicate_match(c, a->ptr[i], argc, argv)) {
            count++;
            if (count > 1) return Qfalse;
        }
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return KORB_BOOL(count == 1);
}

static VALUE ary_min(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    bool has_block = korb_block_given(c);
    /* CRuby min/max block convention: block.call(probe, running) — if it
     * returns < 0 the probe is smaller than the running min (so swap).
     * This is the opposite of sort's convention, which is also why the
     * cmp variable here is interpreted with the probe as the LHS. */
    VALUE ret;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = a->ptr[0];
        for (long i = 1; i < a->len; i++) {
            rs[1] = a->ptr[i];
            long cmp = ary_sort_compare(c, rs[1], rs[0], has_block);
            if (c->state != KORB_NORMAL) break;
            if (cmp < 0) rs[0] = rs[1];
        }
        ret = rs[0];
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}

static VALUE ary_max(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    bool has_block = korb_block_given(c);
    /* Same convention as ary_min — block.call(probe, running).  If it
     * returns > 0 the probe is greater than running max, so swap. */
    VALUE ret;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = a->ptr[0];
        for (long i = 1; i < a->len; i++) {
            rs[1] = a->ptr[i];
            long cmp = ary_sort_compare(c, rs[1], rs[0], has_block);
            if (c->state != KORB_NORMAL) break;
            if (cmp > 0) rs[0] = rs[1];
        }
        ret = rs[0];
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}

static VALUE ary_sum(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE acc = argc > 0 ? argv[0] : INT2FIX(0);
    bool has_block = korb_block_given(c);
    for (long i = 0; i < a->len; i++) {
        VALUE elt = a->ptr[i];
        /* Block form: yield each element through the block and use the
         * result.  Init value is NOT block-mapped. */
        if (has_block) {
            elt = korb_yield(c, 1, &elt);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        if (FIXNUM_P(acc) && FIXNUM_P(elt)) {
            long s;
            if (!__builtin_add_overflow(FIX2LONG(acc), FIX2LONG(elt), &s) && FIXABLE(s))
                acc = INT2FIX(s);
            else acc = korb_int_plus(acc, elt);
        } else {
            acc = korb_funcall(c, acc, korb_intern("+"), 1, &elt);
        }
    }
    return acc;
}

static VALUE ary_each_slice(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    bool has_block = korb_block_given(c);
    VALUE collected = has_block ? Qnil : korb_ary_new(c, c->sp);
    for (long i = 0; i < a->len; i += n) {
        long end = i + n; if (end > a->len) end = a->len;
        VALUE slice = korb_ary_new_capa(c, c->sp, end - i);
        for (long j = i; j < end; j++) korb_ary_push(slice, a->ptr[j]);
        if (has_block) {
            korb_yield(c, 1, &slice);
            if (c->state != KORB_NORMAL) return Qnil;
        } else {
            korb_ary_push(collected, slice);
        }
    }
    return has_block ? self : collected;
}

static VALUE ary_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* not real Array#step, but stub */
    return self;
}

static VALUE ary_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_ARRAY && korb_eq(c, self, argv[0]));
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
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(c, c->sp, "", 0);
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
            VALUE sv = (src_idx < a->len) ? a->ptr[src_idx++] : korb_str_new(c, c->sp, "", 0);
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
            VALUE sv = (src_idx < a->len) ? a->ptr[src_idx++] : korb_str_new(c, c->sp, "", 0);
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
    return korb_str_new(c, c->sp, buf, plen);
}

static VALUE str_unpack(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new(c, c->sp);
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
            korb_ary_push(r, korb_str_new(c, c->sp, (const char *)(src + src_idx), real));
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
            korb_ary_push(r, korb_str_new(c, c->sp, out, o));
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
                korb_ary_push(r, korb_float_new(c, c->sp, v));
            }
            break;
          }
          case 'f': case 'F': case 'e': case 'g': {
            long n = star ? ((src_len - src_idx) / 4) : count;
            for (long i = 0; i < n && src_idx + 4 <= src_len; i++) {
                float v;
                memcpy(&v, src + src_idx, 4);
                src_idx += 4;
                korb_ary_push(r, korb_float_new(c, c->sp, (double)v));
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
    CHECK_FROZEN_RET(c, self, Qnil);
    /* Snapshot all source arrays' contents BEFORE any push — this handles
     * `ary.concat(ary)` (self-concat) and `ary.concat(ary, ary)` correctly,
     * even when args alias self.  We also coerce non-Array args via #to_ary
     * (CRuby semantics). */
    long total = 0;
    VALUE *bufs[16];                 /* per-arg snapshot ptr (or argv[i] if T_ARRAY without self-aliasing concern) */
    long  lens[16];                  /* per-arg snapshot len */
    if (argc > 16) {
        /* Fallback for ridiculously many args — process sequentially with
         * per-iter src_len snapshot.  Doesn't handle full self-alias case
         * but argc>16 is not a real workload. */
        for (int i = 0; i < argc; i++) {
            VALUE arg = argv[i];
            if (BUILTIN_TYPE(arg) != T_ARRAY) {
                if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_ARRAY) {
                    arg = korb_funcall(c, arg, korb_intern("to_ary"), 0, NULL);
                    if (c->state != KORB_NORMAL) return Qnil;
                    if (BUILTIN_TYPE(arg) != T_ARRAY) continue;
                }
            }
            struct korb_array *o = (struct korb_array *)arg;
            long src_len = o->len;
            for (long j = 0; j < src_len; j++) korb_ary_push(self, o->ptr[j]);
        }
        return self;
    }
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_ARRAY) {
            arg = korb_funcall(c, arg, korb_intern("to_ary"), 0, NULL);
            if (c->state != KORB_NORMAL) return Qnil;
            if (BUILTIN_TYPE(arg) != T_ARRAY) { bufs[i] = NULL; lens[i] = 0; continue; }
        }
        struct korb_array *o = (struct korb_array *)arg;
        lens[i] = o->len;
        /* Copy snapshot into a temp libc buffer so self-aliased pushes
         * later don't corrupt our source view. */
        if (lens[i] > 0) {
            bufs[i] = korb_xmalloc(lens[i] * sizeof(VALUE));
            for (long j = 0; j < lens[i]; j++) bufs[i][j] = o->ptr[j];
        } else {
            bufs[i] = NULL;
        }
        total += lens[i];
    }
    for (int i = 0; i < argc; i++) {
        for (long j = 0; j < lens[i]; j++) korb_ary_push(self, bufs[i][j]);
    }
    (void)total;
    return self;
}

/* Array#+ — non-destructive concat (CRuby semantics).  Coerces the
 * argument via #to_ary if not already an Array. */
VALUE ary_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return self;
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
        if (!SPECIAL_CONST_P(other)) {
            /* Use respond_to? so mock objects (method_missing) and
             * plain instances both go through #to_ary.  Whatever
             * to_ary raises (NoMethodError, RuntimeError, etc.)
             * propagates up unchanged — only the "to_ary returned
             * non-Array" case yields TypeError. */
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                other = korb_funcall(c, other, korb_intern("to_ary"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Array",
                       korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
    }
    struct korb_array *l = (struct korb_array *)self;
    struct korb_array *r = (struct korb_array *)other;
    VALUE result = korb_ary_new_capa(c, c->sp, l->len + r->len);
    for (long i = 0; i < l->len; i++) korb_ary_push(result, l->ptr[i]);
    for (long i = 0; i < r->len; i++) korb_ary_push(result, r->ptr[i]);
    return result;
}

static VALUE ary_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_ARRAY) return korb_ary_new(c, c->sp);
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    VALUE r = korb_ary_new(c, c->sp);
    for (long i = 0; i < a->len; i++) {
        bool found = false;
        for (long j = 0; j < b->len; j++) if (korb_eq(c, a->ptr[i], b->ptr[j])) { found = true; break; }
        if (!found) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    
    if (argc < 1 && c->current_block) {
        for (long i = 0; i < a->len; i++) {
            VALUE r = korb_yield(c, 1, &a->ptr[i]);
            if (c->state == KORB_RAISE) return Qnil;
            if (!NIL_P(r) && r != Qfalse) return INT2FIX(i);
        }
        return Qnil;
    }
    if (argc < 1) return Qnil;
    for (long i = 0; i < a->len; i++) if (korb_eq(c, a->ptr[i], argv[0])) return INT2FIX(i);
    return Qnil;
}

static VALUE ary_reverse(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = a->len - 1; i >= 0; i--) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_rotate_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    if (a->len <= 1) return self;
    long n;
    if (argc >= 1) {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (UNDEF_P(iv)) return Qnil;
        if (!FIXNUM_P(iv)) return self;
        n = FIX2LONG(iv);
    } else {
        n = 1;
    }
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
    long n;
    if (argc >= 1) {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (UNDEF_P(iv)) return Qnil;
        if (!FIXNUM_P(iv)) return korb_ary_new(c, c->sp);
        n = FIX2LONG(iv);
    } else {
        n = 1;
    }
    if (a->len == 0) return korb_ary_new(c, c->sp);
    n = n % a->len;
    if (n < 0) n += a->len;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[(i + n) % a->len]);
    return r;
}

static VALUE ary_reverse_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0, j = a->len - 1; i < j; i++, j--) {
        VALUE t = a->ptr[i]; a->ptr[i] = a->ptr[j]; a->ptr[j] = t;
    }
    return self;
}

static VALUE ary_clear(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 0) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0)", argc);
        return Qnil;
    }
    CHECK_FROZEN_RET(c, self, Qnil);
    ((struct korb_array *)self)->len = 0;
    return self;
}

static VALUE ary_unshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    /* shift right argc times */
    long oldlen = a->len;
    for (int i = 0; i < argc; i++) korb_ary_push(self, Qnil);
    for (long i = oldlen - 1; i >= 0; i--) a->ptr[i + argc] = a->ptr[i];
    for (int i = 0; i < argc; i++) a->ptr[i] = argv[i];
    return self;
}

static VALUE ary_shift(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc > 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", argc);
        return Qnil;
    }
    struct korb_array *a = (struct korb_array *)self;
    if (argc >= 1) {
        VALUE iv = korb_to_int_or_raise(c, argv[0]);
        if (UNDEF_P(iv)) return Qnil;
        if (!FIXNUM_P(iv)) return Qnil;
        long n = FIX2LONG(iv);
        if (n < 0) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg, "negative array size");
            return Qnil;
        }
        long take = n > a->len ? a->len : n;
        VALUE out = korb_ary_new_capa(c, c->sp, take);
        for (long i = 0; i < take; i++) korb_ary_push(out, a->ptr[i]);
        for (long i = 0; i + take < a->len; i++) a->ptr[i] = a->ptr[i + take];
        a->len -= take;
        return out;
    }
    if (a->len == 0) return Qnil;
    VALUE v = a->ptr[0];
    for (long i = 0; i + 1 < a->len; i++) a->ptr[i] = a->ptr[i+1];
    a->len--;
    return v;
}

static VALUE ary_transpose(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return korb_ary_new(c, c->sp);
    /* Normalize each inner element via to_ary if it isn't already an
     * Array — CRuby semantics. */
    long n_outer = a->len;
    VALUE *coerced = korb_xmalloc(n_outer * sizeof(VALUE));
    for (long j = 0; j < n_outer; j++) {
        VALUE inner = a->ptr[j];
        if (!SPECIAL_CONST_P(inner) && BUILTIN_TYPE(inner) == T_ARRAY) {
            coerced[j] = inner;
            continue;
        }
        if (!SPECIAL_CONST_P(inner)) {
            VALUE rt = korb_funcall(c, inner, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                VALUE r = korb_funcall(c, inner, korb_intern("to_ary"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
                if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_ARRAY) {
                    coerced[j] = r;
                    continue;
                }
            }
        }
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into Array",
                   SPECIAL_CONST_P(inner) ? "(special)"
                       : korb_id_name(korb_class_of_class(inner)->name));
        return Qnil;
    }
    long n_inner = ((struct korb_array *)coerced[0])->len;
    /* Verify all rows have the same length. */
    for (long j = 1; j < n_outer; j++) {
        if (((struct korb_array *)coerced[j])->len != n_inner) {
            VALUE eIE = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
            korb_raise(c, (struct korb_class *)eIE,
                       "element size differs (%ld should be %ld)",
                       ((struct korb_array *)coerced[j])->len, n_inner);
            return Qnil;
        }
    }
    VALUE r = korb_ary_new_capa(c, c->sp, n_inner);
    for (long i = 0; i < n_inner; i++) {
        VALUE row = korb_ary_new_capa(c, c->sp, n_outer);
        for (long j = 0; j < n_outer; j++) {
            VALUE inner = coerced[j];
            korb_ary_push(row, ((struct korb_array *)inner)->ptr[i]);
        }
        korb_ary_push(r, row);
    }
    return r;
}

static VALUE ary_count(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    
    if (argc == 0 && c->current_block) {
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
    for (long i = 0; i < a->len; i++) if (korb_eq(c, a->ptr[i], argv[0])) n++;
    return INT2FIX(n);
}

static VALUE ary_drop(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return self;
    VALUE iv = korb_to_int_or_raise(c, argv[0]);
    if (UNDEF_P(iv)) return Qnil;
    if (!FIXNUM_P(iv)) return self;
    long n = FIX2LONG(iv);
    if (n < 0) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA, "attempt to drop negative size");
        return Qnil;
    }
    struct korb_array *a = (struct korb_array *)self;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len - n);
    for (long i = n; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_take(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA, "attempt to take negative size");
        return Qnil;
    }
    struct korb_array *a = (struct korb_array *)self;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_fill(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    bool has_block = korb_block_given(c);
    /* With a block, signature is fill { |i| ... } / fill(start) / fill(start, len).
     * Without a block, fill(val[, start[, length]]). */
    long start = 0, len = a->len;
    int idx_arg_base = has_block ? 0 : 1;
    if (!has_block && argc < 1) return self;
    if (argc >= idx_arg_base + 1 && FIXNUM_P(argv[idx_arg_base])) {
        start = FIX2LONG(argv[idx_arg_base]);
        if (start < 0) start += a->len;
        if (start < 0) start = 0;
    }
    if (argc >= idx_arg_base + 2 && FIXNUM_P(argv[idx_arg_base + 1])) {
        len = FIX2LONG(argv[idx_arg_base + 1]);
        if (len < 0) return self;
    } else if (argc >= idx_arg_base + 1) {
        len = a->len - start;
    }
    if (start >= a->len) return self;
    long end = start + len;
    if (end > a->len) end = a->len;
    if (has_block) {
        for (long i = start; i < end; i++) {
            VALUE iv = INT2FIX(i);
            VALUE r = korb_yield(c, 1, &iv);
            if (c->state != KORB_NORMAL) return Qnil;
            a->ptr[i] = r;
        }
    } else {
        for (long i = start; i < end; i++) a->ptr[i] = argv[0];
    }
    return self;
}

static VALUE ary_sample(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_array *a = (const struct korb_array *)self;
    if (a->len == 0) return argc >= 1 ? korb_ary_new(c, c->sp) : Qnil;
    /* sample (no arg) → one random element.
     * sample(n) → fresh Array of n random elements without replacement. */
    if (argc < 1) {
        return a->ptr[rand() % a->len];
    }
    if (!FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return korb_ary_new(c, c->sp);
    if (n >= a->len) {
        VALUE shuf = korb_ary_new_capa(c, c->sp, a->len);
        for (long i = 0; i < a->len; i++) korb_ary_push(shuf, a->ptr[i]);
        struct korb_array *out = (struct korb_array *)shuf;
        for (long i = out->len - 1; i > 0; i--) {
            long j = rand() % (i + 1);
            VALUE t = out->ptr[i]; out->ptr[i] = out->ptr[j]; out->ptr[j] = t;
        }
        return shuf;
    }
    VALUE shuf = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(shuf, a->ptr[i]);
    struct korb_array *tmp = (struct korb_array *)shuf;
    for (long i = 0; i < n; i++) {
        long j = i + (rand() % (tmp->len - i));
        VALUE t = tmp->ptr[i]; tmp->ptr[i] = tmp->ptr[j]; tmp->ptr[j] = t;
    }
    VALUE out = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(out, tmp->ptr[i]);
    return out;
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
    /* Pin running-min m + its key mk + per-iter probe value v + key k
     * across korb_yield / funcall GC fires. */
    VALUE ret;
    ARO_ROOT_SCOPE_START(c, rs, 4) {
        rs[0] = a->ptr[0];                                /* m: running min */
        rs[1] = korb_yield(c, 1, &rs[0]);                 /* mk: m's key */
        if (c->state != KORB_NORMAL) { ret = Qnil; goto done_min_by; }
        for (long i = 1; i < a->len; i++) {
            rs[2] = a->ptr[i];                            /* v: probe */
            rs[3] = korb_yield(c, 1, &rs[2]);             /* k: v's key */
            if (c->state != KORB_NORMAL) { ret = Qnil; goto done_min_by; }
            VALUE cmp = korb_funcall(c, rs[1], korb_intern("<=>"), 1, &rs[3]);
            if (c->state != KORB_NORMAL) { ret = Qnil; goto done_min_by; }
            long sign = 0;
            if (FIXNUM_P(cmp)) sign = FIX2LONG(cmp);
            else if (!SPECIAL_CONST_P(cmp) && BUILTIN_TYPE(cmp) == T_BIGNUM) sign = korb_int_cmp(cmp, INT2FIX(0));
            if (sign > 0) { rs[0] = rs[2]; rs[1] = rs[3]; }
        }
        ret = rs[0];
done_min_by: ;
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}

static VALUE ary_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Array#* — n: repeat, str: join.  argc == 0 → ArgumentError (CRuby). */
    if (argc < 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 0, expected 1)");
        return Qnil;
    }
    struct korb_array *a = (struct korb_array *)self;
    if (FIXNUM_P(argv[0])) {
        long n = FIX2LONG(argv[0]);
        if (n < 0) {
            VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eA, "negative argument");
            return Qnil;
        }
        long total;
        if (__builtin_mul_overflow(a->len, n, &total) ||
            total > (long)(LONG_MAX / sizeof(VALUE))) {
            VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eA, "argument too big");
            return Qnil;
        }
        VALUE r = korb_ary_new_capa(c, c->sp, total);
        for (long i = 0; i < n; i++)
            for (long j = 0; j < a->len; j++) korb_ary_push(r, a->ptr[j]);
        return r;
    }
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        /* join with sep — pin result + sep + per-iter element across
         * each korb_to_s / korb_str_concat GC fire. */
        VALUE ret = Qnil;
        ARO_ROOT_SCOPE_START(c, rs, 3) {
            rs[0] = korb_str_new(c, c->sp, "", 0);  /* result */
            rs[1] = argv[0];              /* separator (pin) */
            for (long i = 0; i < a->len; i++) {
                if (i > 0) korb_str_concat(c, c->sp, rs[0], rs[1]);
                rs[2] = a->ptr[i];
                if (BUILTIN_TYPE(rs[2]) != T_STRING) rs[2] = korb_to_s(c, c->sp, rs[2]);
                korb_str_concat(c, c->sp, rs[0], rs[2]);
            }
            ret = rs[0];
        } ARO_ROOT_SCOPE_END(c, rs);
        return ret;
    }
    return self;
}

static VALUE ary_max_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    /* Pin running-max + key + per-iter probe + key — see ary_min_by. */
    VALUE ret;
    ARO_ROOT_SCOPE_START(c, rs, 4) {
        rs[0] = a->ptr[0];
        rs[1] = korb_yield(c, 1, &rs[0]);
        if (c->state != KORB_NORMAL) { ret = Qnil; goto done_max_by; }
        for (long i = 1; i < a->len; i++) {
            rs[2] = a->ptr[i];
            rs[3] = korb_yield(c, 1, &rs[2]);
            if (c->state != KORB_NORMAL) { ret = Qnil; goto done_max_by; }
            VALUE cmp = korb_funcall(c, rs[1], korb_intern("<=>"), 1, &rs[3]);
            if (c->state != KORB_NORMAL) { ret = Qnil; goto done_max_by; }
            long sign = 0;
            if (FIXNUM_P(cmp)) sign = FIX2LONG(cmp);
            else if (!SPECIAL_CONST_P(cmp) && BUILTIN_TYPE(cmp) == T_BIGNUM) sign = korb_int_cmp(cmp, INT2FIX(0));
            if (sign < 0) { rs[0] = rs[2]; rs[1] = rs[3]; }
        }
        ret = rs[0];
done_max_by: ;
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}

static VALUE ary_slice_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    /* Array#slice!(start, len) — remove and return that range */
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long start = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (start < 0) start += a->len;
    long len = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 1;
    if (start < 0 || start >= a->len) return Qnil;
    if (start + len > a->len) len = a->len - start;
    if (len < 0) len = 0;
    VALUE r = korb_ary_new_capa(c, c->sp, len);
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
    /* Call == on the entry's first element (so user-defined == can
     * decide).  Non-array entries get implicit to_ary coercion (CRuby:
     * "calls to_ary on non-array elements"). */
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE e = a->ptr[i];
        VALUE entry;
        if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_ARRAY) {
            entry = e;
        } else if (!SPECIAL_CONST_P(e)) {
            /* Try to_ary; if it doesn't respond / raises, skip. */
            struct korb_class *k = korb_class_of_class(e);
            if (!k || !korb_class_find_method(k, korb_intern("to_ary"))) continue;
            entry = korb_funcall(c, e, korb_intern("to_ary"), 0, NULL);
            if (c->state != KORB_NORMAL) { c->state = KORB_NORMAL; continue; }
            if (SPECIAL_CONST_P(entry) || BUILTIN_TYPE(entry) != T_ARRAY) continue;
        } else {
            continue;
        }
        struct korb_array *ea = (struct korb_array *)entry;
        if (ea->len == 0) continue;
        VALUE eq_args[1] = { argv[0] };
        VALUE r = korb_funcall(c, ea->ptr[0], korb_intern("=="), 1, eq_args);
        if (RTEST(r)) return entry;
    }
    return Qnil;
}

/* Array#rassoc — same but matches the second element. */
static VALUE ary_rassoc(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE e = a->ptr[i];
        VALUE entry;
        if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_ARRAY) {
            entry = e;
        } else if (!SPECIAL_CONST_P(e)) {
            struct korb_class *k = korb_class_of_class(e);
            if (!k || !korb_class_find_method(k, korb_intern("to_ary"))) continue;
            entry = korb_funcall(c, e, korb_intern("to_ary"), 0, NULL);
            if (c->state != KORB_NORMAL) { c->state = KORB_NORMAL; continue; }
            if (SPECIAL_CONST_P(entry) || BUILTIN_TYPE(entry) != T_ARRAY) continue;
        } else {
            continue;
        }
        struct korb_array *ea = (struct korb_array *)entry;
        if (ea->len < 2) continue;
        VALUE eq_args[1] = { argv[0] };
        VALUE r = korb_funcall(c, ea->ptr[1], korb_intern("=="), 1, eq_args);
        if (RTEST(r)) return entry;
    }
    return Qnil;
}

/* Array#at(i) — like a[i] for a single integer index. */
static VALUE ary_at(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
        return Qnil;
    }
    VALUE iv = korb_to_int_or_raise(c, argv[0]);
    if (UNDEF_P(iv)) return Qnil;
    if (!FIXNUM_P(iv)) return Qnil;
    return korb_ary_aref(self, FIX2LONG(iv));
}

/* Array#fetch(idx[, default]) {block}
 *  * idx is coerced via #to_int.
 *  * If idx is in range, returns the element.
 *  * Otherwise: yields idx to a block (if given) and returns its value;
 *    else returns the explicit default arg (if given);
 *    else raises IndexError. */
static VALUE ary_fetch(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
        return Qnil;
    }
    VALUE iv = korb_to_int_or_raise(c, argv[0]);
    if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return Qnil;
    long i = FIX2LONG(iv);
    struct korb_array *a = (struct korb_array *)self;
    long norm = i < 0 ? i + a->len : i;
    if (norm >= 0 && norm < a->len) return a->ptr[norm];
    if (korb_block_given(c)) {
        VALUE arg[1] = { argv[0] };
        return korb_yield(c, 1, arg);
    }
    if (argc == 2) return argv[1];
    VALUE eI = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
    korb_raise(c, (struct korb_class *)eI,
               "index %ld outside of array bounds: %ld...%ld",
               i, -a->len, a->len);
    return Qnil;
}

/* Array#fetch_values(*indexes) {block} — like fetch but for many indexes
 * at once.  Returns an array.  Without a block, raises IndexError on the
 * first missing index. */
static VALUE ary_fetch_values(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new(c, c->sp);
    bool block_p = korb_block_given(c);
    struct korb_array *a = (struct korb_array *)self;
    for (int k = 0; k < argc; k++) {
        VALUE iv = korb_to_int_or_raise(c, argv[k]);
        if (c->state == KORB_RAISE || !FIXNUM_P(iv)) return Qnil;
        long i = FIX2LONG(iv);
        long norm = i < 0 ? i + a->len : i;
        if (norm >= 0 && norm < a->len) {
            korb_ary_push(r, a->ptr[norm]);
        } else if (block_p) {
            VALUE arg[1] = { argv[k] };
            VALUE yv = korb_yield(c, 1, arg);
            if (c->state == KORB_RAISE) return Qnil;
            korb_ary_push(r, yv);
        } else {
            VALUE eI = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
            korb_raise(c, (struct korb_class *)eI,
                       "index %ld outside of array bounds: %ld...%ld",
                       i, -a->len, a->len);
            return Qnil;
        }
    }
    return r;
}

/* Array#delete(obj) — remove all == matches; return obj if found else nil. */
static VALUE ary_delete(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    /* Frozen check is conditional: CRuby only raises FrozenError when a
     * modification would actually happen.  Scan first; raise if we'd
     * remove anything. */
    struct korb_array *a = (struct korb_array *)self;
    /* First pass: count matches (use full == dispatch so user override
     * participates).  Don't mutate yet. */
    long matches = 0;
    for (long r = 0; r < a->len; r++) {
        VALUE eq_args[1] = { argv[0] };
        VALUE r_eq = korb_funcall(c, a->ptr[r], korb_intern("=="), 1, eq_args);
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(r_eq)) matches++;
    }
    if (matches == 0) {
        /* No modification — block fallback (CRuby returns block's value
         * if a block is given, else nil).  Don't raise FrozenError. */
        if (korb_block_given(c)) {
            VALUE blk_args[1] = { argv[0] };
            return korb_yield(c, 1, blk_args);
        }
        return Qnil;
    }
    /* Will modify — now enforce frozen check. */
    CHECK_FROZEN_RET(c, self, Qnil);
    long w = 0;
    for (long r = 0; r < a->len; r++) {
        VALUE eq_args[1] = { argv[0] };
        VALUE r_eq = korb_funcall(c, a->ptr[r], korb_intern("=="), 1, eq_args);
        if (c->state == KORB_RAISE) return Qnil;
        if (!RTEST(r_eq)) a->ptr[w++] = a->ptr[r];
    }
    a->len = w;
    return argv[0];
}

/* Array#delete_at(i) — remove element at i, return removed or nil. */
static VALUE ary_delete_at(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    /* Coerce non-Integer index via #to_int (CRuby semantics).  Try
     * unconditionally so method_missing-based mocks also coerce. */
    VALUE idx = argv[0];
    if (!FIXNUM_P(idx) && (SPECIAL_CONST_P(idx) || BUILTIN_TYPE(idx) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(idx)) {
            VALUE coerced = korb_funcall(c, idx, korb_intern("to_int"), 0, NULL);
            if (c->state == KORB_RAISE) {
                /* swallow NoMethodError so the original index sticks. */
                VALUE bang = c->state_value;
                VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                    BUILTIN_TYPE(eNo) == T_CLASS) {
                    struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                    bool is_nm = false;
                    for (struct korb_class *kk = bk; kk; kk = kk->super) {
                        if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                    }
                    if (is_nm) { c->state = KORB_NORMAL; c->state_value = Qnil; }
                    else return Qnil;
                } else return Qnil;
            } else if (FIXNUM_P(coerced)) {
                idx = coerced;
            }
        }
        if (!FIXNUM_P(idx)) return Qnil;
    }
    if (!FIXNUM_P(idx)) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(idx);
    if (i < 0) i += a->len;
    if (i < 0 || i >= a->len) return Qnil;
    VALUE r = a->ptr[i];
    for (long j = i; j + 1 < a->len; j++) a->ptr[j] = a->ptr[j + 1];
    a->len--;
    return r;
}

/* Array#delete_if { |x| ... } — remove where block returns truthy. */
static VALUE ary_delete_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("delete_if"));
        return korb_funcall(c, self, korb_intern("to_enum"), 1, &arg);
    }
    CHECK_FROZEN_RET(c, self, Qnil);
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

/* Array#reject! — like delete_if, but returns nil when nothing was
 * removed (CRuby semantic for the bang).  No block → Enumerator. */
static VALUE ary_reject_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("reject!"));
        return korb_funcall(c, self, korb_intern("to_enum"), 1, &arg);
    }
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_array *a = (struct korb_array *)self;
    long w = 0;
    bool changed = false;
    for (long r = 0; r < a->len; r++) {
        VALUE elt = a->ptr[r];
        VALUE drop = korb_yield(c, 1, &elt);
        if (c->state == KORB_RAISE) return Qnil;
        if (NIL_P(drop) || drop == Qfalse) {
            a->ptr[w++] = elt;
        } else {
            changed = true;
        }
    }
    if (!changed) return Qnil;
    a->len = w;
    return self;
}

/* Array#reject { |x| ... } — like delete_if but returns a new array. */
/* Array#reverse_each — yields elements in reverse order; no block →
 * Array (Enumerator stand-in). */
static VALUE ary_reverse_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (!korb_block_given(c)) {
        VALUE r = korb_ary_new_capa(c, c->sp, a->len);
        for (long i = a->len - 1; i >= 0; i--) korb_ary_push(r, a->ptr[i]);
        return r;
    }
    for (long i = a->len - 1; i >= 0; i--) {
        korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE ary_reject(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Non-mutating — reject! is the in-place form (registered as
     * delete_if, which does mutate and is FROZEN-checked). */
    if (!korb_block_given(c)) return self;
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new(c, c->sp);
    for (long i = 0; i < a->len; i++) {
        VALUE drop = korb_yield(c, 1, &a->ptr[i]);
        if (c->state == KORB_RAISE) return Qnil;
        if (NIL_P(drop) || drop == Qfalse) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

/* Array#insert(i, *elts) — splice elts into self starting at i. */
static VALUE ary_insert(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc < 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 0, expected 1+)");
        return Qnil;
    }
    if (argc == 1) return self;  /* `arr.insert(i)` with no values is no-op */
    VALUE iv = korb_to_int_or_raise(c, argv[0]);
    if (UNDEF_P(iv)) return Qnil;
    if (!FIXNUM_P(iv)) return self;
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(iv);
    if (i < 0) {
        long real = i + a->len + 1;
        if (real < 0) {
            VALUE eIE = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
            korb_raise(c, (struct korb_class *)eIE,
                       "index %ld too small for array; minimum: -%ld", i, a->len + 1);
            return Qnil;
        }
        i = real;
    }
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
    CHECK_FROZEN_RET(c, self, Qnil);
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return self;
    /* Self-replace is a no-op (CRuby semantics). */
    if (self == argv[0]) return self;
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
    const struct korb_array *a = (const struct korb_array *)self;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    /* clone preserves frozen state (`dup` does not) — match CRuby. */
    if (korb_obj_frozen_p(self)) {
        ((struct RBasic *)r)->head.flags |= FL_FROZEN;
    }
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
    if (c->state != KORB_NORMAL) return;
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp, r);
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
        if (c->state != KORB_NORMAL) return;
    }
}

static VALUE ary_combination(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    
    /* No block: return an Enumerator (CRuby semantics).  Override
     * @__size to the binomial coefficient C(n, r) so #size reports
     * the actual combination count (or 0 when r < 0 or r > n). */
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("combination"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        VALUE e = korb_funcall(c, self, korb_intern("to_enum"), argc + 1, call_argv);
        if (c->state == KORB_RAISE || SPECIAL_CONST_P(e)) return e;
        long n = a->len;
        long sz = 0;
        if (r >= 0 && r <= n) {
            /* Compute C(n, r) using the multiplicative formula. */
            long k = (r < n - r) ? r : (n - r);
            sz = 1;
            for (long i = 0; i < k; i++) {
                sz = sz * (n - i) / (i + 1);
            }
        }
        korb_ivar_set(e, korb_intern("@__size"), INT2FIX(sz));
        return e;
    }
    if (r < 0 || r > a->len) return self;
    VALUE buf = korb_ary_new_capa(c, c->sp, r);
    ary_combine(c, a, r, 0, buf, Qnil);
    return self;
}

static void ary_perm(CTX *c, struct korb_array *a, long r,
                      VALUE used, VALUE buf, VALUE result_or_nil) {
    if (c->state != KORB_NORMAL) return;
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp, r);
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
        if (c->state != KORB_NORMAL) return;
    }
}

static VALUE ary_permutation(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long r = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : a->len;
    
    /* No block: return Enumerator with size = n! / (n-r)! when 0<=r<=n,
     * else 0.  CRuby semantics. */
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("permutation"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        VALUE e = korb_funcall(c, self, korb_intern("to_enum"), argc + 1, call_argv);
        if (c->state == KORB_RAISE || SPECIAL_CONST_P(e)) return e;
        long n = a->len;
        long sz = 0;
        if (r >= 0 && r <= n) {
            sz = 1;
            for (long i = 0; i < r; i++) sz *= (n - i);
        }
        korb_ivar_set(e, korb_intern("@__size"), INT2FIX(sz));
        return e;
    }
    if (r < 0 || r > a->len) return self;
    VALUE used = korb_ary_new_capa(c, c->sp, a->len);
    for (long i = 0; i < a->len; i++) korb_ary_push(used, Qfalse);
    VALUE buf = korb_ary_new_capa(c, c->sp, r);
    ary_perm(c, a, r, used, buf, Qnil);
    return self;
}

/* Array#cycle(n=nil) — yield each element n times (or forever if nil).
 * Implemented in C so `break` from the block cleanly exits all the
 * nested loops (the bootstrap-Ruby version has nested blk.call inside
 * each{} inside loop{} and break doesn't propagate out reliably). */
static VALUE ary_cycle(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("cycle"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        return korb_funcall(c, self, korb_intern("to_enum"), argc + 1, call_argv);
    }
    long n = -1;  /* -1 means infinite */
    if (argc >= 1 && !NIL_P(argv[0])) {
        if (FIXNUM_P(argv[0])) n = FIX2LONG(argv[0]);
        else if (BUILTIN_TYPE(argv[0]) == T_BIGNUM) n = LONG_MAX;
        if (n <= 0) return Qnil;
    }
    if (a->len == 0) return Qnil;
    long iter = 0;
    while (n < 0 || iter < n) {
        if (a->len == 0) return Qnil;  /* CRuby: cleared mid-iter → exit */
        for (long i = 0; i < a->len; i++) {
            korb_yield(c, 1, &a->ptr[i]);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        iter++;
    }
    return Qnil;
}

/* Array#repeated_combination(r) — combinations with repetition. */
static void ary_rcombine(CTX *c, struct korb_array *a, long r, long start,
                          VALUE buf, VALUE result_or_nil) {
    if (c->state != KORB_NORMAL) return;
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp, r);
        struct korb_array *bb = (struct korb_array *)buf;
        for (long i = 0; i < bb->len; i++) korb_ary_push(copy, bb->ptr[i]);
        if (NIL_P(result_or_nil)) korb_yield(c, 1, &copy);
        else korb_ary_push(result_or_nil, copy);
        return;
    }
    for (long i = start; i < a->len; i++) {
        korb_ary_push(buf, a->ptr[i]);
        ary_rcombine(c, a, r, i, buf, result_or_nil);  /* i, not i+1 — repetition */
        ((struct korb_array *)buf)->len--;
        if (c->state != KORB_NORMAL) return;
    }
}

static VALUE ary_repeated_combination(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("repeated_combination"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        return korb_funcall(c, self, korb_intern("to_enum"), argc + 1, call_argv);
    }
    if (r < 0) return self;
    if (r == 0) {
        VALUE empty = korb_ary_new(c, c->sp);
        korb_yield(c, 1, &empty);
        return self;
    }
    if (a->len == 0) return self;
    VALUE buf = korb_ary_new_capa(c, c->sp, r);
    ary_rcombine(c, a, r, 0, buf, Qnil);
    return self;
}

/* Array#repeated_permutation(r) — permutations with repetition. */
static void ary_rperm(CTX *c, struct korb_array *a, long r,
                       VALUE buf, VALUE result_or_nil) {
    if (c->state != KORB_NORMAL) return;
    if (((struct korb_array *)buf)->len == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp, r);
        struct korb_array *bb = (struct korb_array *)buf;
        for (long i = 0; i < bb->len; i++) korb_ary_push(copy, bb->ptr[i]);
        if (NIL_P(result_or_nil)) korb_yield(c, 1, &copy);
        else korb_ary_push(result_or_nil, copy);
        return;
    }
    for (long i = 0; i < a->len; i++) {
        korb_ary_push(buf, a->ptr[i]);
        ary_rperm(c, a, r, buf, result_or_nil);
        ((struct korb_array *)buf)->len--;
        if (c->state != KORB_NORMAL) return;
    }
}

static VALUE ary_repeated_permutation(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("repeated_permutation"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        return korb_funcall(c, self, korb_intern("to_enum"), argc + 1, call_argv);
    }
    if (r < 0) return self;
    if (r == 0) {
        VALUE empty = korb_ary_new(c, c->sp);
        korb_yield(c, 1, &empty);
        return self;
    }
    if (a->len == 0) return self;
    VALUE buf = korb_ary_new_capa(c, c->sp, r);
    ary_rperm(c, a, r, buf, Qnil);
    return self;
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
    /* Total size sanity: if no block given, materializing > 1e8 rows
     * is hopeless.  Compute product of sizes with overflow detection
     * and raise RangeError early (CRuby does the same). */
    
    if (!c->current_block) {
        long total = 1;
        for (long i = 0; i < n; i++) {
            if (arrays[i]->len == 0) { total = 0; break; }
            if (__builtin_mul_overflow(total, arrays[i]->len, &total) ||
                total > (long)(LONG_MAX / sizeof(VALUE) / 4)) {
                VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
                korb_raise(c, (struct korb_class *)eR, "too big to product");
                return Qnil;
            }
        }
    }
    VALUE result = c->current_block ? Qnil : korb_ary_new(c, c->sp);
    long *idx = korb_xcalloc(n, sizeof(long));
    while (true) {
        VALUE row = korb_ary_new_capa(c, c->sp, n);
        bool empty = false;
        for (long i = 0; i < n; i++) {
            if (arrays[i]->len == 0) { empty = true; break; }
            korb_ary_push(row, arrays[i]->ptr[idx[i]]);
        }
        if (empty) break;
        if (c->current_block) {
            korb_yield(c, 1, &row);
            if (c->state != KORB_NORMAL) return self;
        } else {
            korb_ary_push(result, row);
        }
        long j = n - 1;
        while (j >= 0) {
            idx[j]++;
            if (idx[j] < arrays[j]->len) break;
            idx[j] = 0;
            j--;
        }
        if (j < 0) break;
    }
    return c->current_block ? self : result;
}

/* Array.new(size = 0, default = nil) — create an array of `size` slots
 * pre-filled with `default`, or, if a block is given, with the block's
 * return value for each index. */
/* Array[] — class method, equivalent to an array literal of the args. */
VALUE ary_class_brackets(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new_capa(c, c->sp, (long)argc);
    /* Honor the receiver class — `MySubclass[1,2,3]` returns a
     * MySubclass instance (CRuby Array.[] semantics).  Default cases
     * (Array.[]) keep Array as the basic.klass. */
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS &&
        self != (VALUE)korb_vm->array_class) {
        ((struct RBasic *)r)->klass = self;
    }
    for (int i = 0; i < argc; i++) korb_ary_push(r, argv[i]);
    return r;
}

static VALUE ary_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    long size = 0;
    VALUE def = Qnil;
    if (argc >= 1 && FIXNUM_P(argv[0])) size = FIX2LONG(argv[0]);
    if (argc >= 2) def = argv[1];
    /* Single-array-arg form: Array.new([1,2,3]) — copy. */
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *src = (struct korb_array *)argv[0];
        VALUE r = korb_ary_new_capa(c, c->sp, src->len);
        for (long i = 0; i < src->len; i++) korb_ary_push(r, src->ptr[i]);
        return r;
    }
    if (size < 0) size = 0;
    VALUE arr = korb_ary_new_capa(c, c->sp, size);
    /* For subclass calls (`class A < Array; end; A.new`) reroute the
     * allocation's basic.klass to `self` so the result inspects /
     * dispatches as the subclass. */
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS &&
        (struct korb_class *)self != korb_vm->array_class) {
        ((struct korb_array *)arr)->basic.klass = self;
    }
    
    if (c->current_block) {
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
    /* Index must be Integer (or convertible via #to_int) — non-numeric
     * raises TypeError (CRuby semantics). */
    if (!FIXNUM_P(argv[0]) && (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_BIGNUM)) {
        VALUE klass_v = (VALUE)korb_class_of_class(argv[0]);
        if (klass_v && korb_class_find_method((struct korb_class *)klass_v,
                                                korb_intern("to_int"))) {
            VALUE coerced = korb_funcall(c, argv[0], korb_intern("to_int"), 0, NULL);
            if (c->state == KORB_RAISE) return Qnil;
            argv[0] = coerced;
        } else {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Integer",
                       korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
    }
    VALUE first = korb_ary_aref(self, FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 0);
    if (argc == 1) return first;
    if (NIL_P(first)) return Qnil;
    /* Intermediate must respond to #dig — else TypeError. */
    VALUE next_klass = (VALUE)korb_class_of_class(first);
    if (!next_klass || !korb_class_find_method((struct korb_class *)next_klass,
                                                 korb_intern("dig"))) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "%s does not have #dig method",
                   korb_id_name(korb_class_of_class(first)->name));
        return Qnil;
    }
    return korb_funcall(c, first, korb_intern("dig"), argc - 1, argv + 1);
}

/* ---------- Array#take_while / drop_while ---------- */
static VALUE ary_take_while(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new(c, c->sp);
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
    VALUE r = korb_ary_new(c, c->sp);
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
    VALUE r = korb_ary_new(c, c->sp);
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
    if (argc > 1) {
        korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 0..1)", argc);
        return Qnil;
    }
    if (argc < 1) return a->len == 0 ? Qnil : a->ptr[0];
    /* Integer / Bignum: convert to long; out-of-long-range Bignum
     * counts as a too-big size (CRuby raises RangeError there). */
    long n;
    VALUE arg = argv[0];
    if (!FIXNUM_P(arg) && (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(arg)) {
            VALUE coerced = korb_funcall(c, arg, korb_intern("to_int"), 0, NULL);
            if (c->state == KORB_RAISE) {
                /* swallow NoMethodError, propagate other errors. */
                VALUE bang = c->state_value;
                VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                    BUILTIN_TYPE(eNo) == T_CLASS) {
                    struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                    bool is_nm = false;
                    for (struct korb_class *kk = bk; kk; kk = kk->super) {
                        if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                    }
                    if (is_nm) { c->state = KORB_NORMAL; c->state_value = Qnil; }
                    else return Qnil;
                } else return Qnil;
            } else {
                arg = coerced;
            }
        }
    }
    if (FIXNUM_P(arg)) {
        n = FIX2LONG(arg);
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_BIGNUM) {
        /* Bignum that fits in long is fine (CRuby's first/last accept
         * up to LONG_MAX).  Out-of-long-range bignum → RangeError. */
        struct korb_bignum *bn = (struct korb_bignum *)arg;
        mpz_ptr z = (mpz_ptr)bn->mpz;
        if (mpz_fits_slong_p(z)) {
            n = mpz_get_si(z);
        } else {
            VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
            korb_raise(c, (struct korb_class *)eR, "bignum too big to convert into 'long'");
            return Qnil;
        }
    } else {
        korb_raise_type_error(c, "no implicit conversion from %s into Integer",
                              korb_id_name(korb_class_of_class(argv[0])->name));
        return Qnil;
    }
    if (n < 0) {
        korb_raise_argument_error(c, "negative array size");
        return Qnil;
    }
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_last_n(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (argc > 1) {
        korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 0..1)", argc);
        return Qnil;
    }
    if (argc < 1) return a->len == 0 ? Qnil : a->ptr[a->len - 1];
    long n;
    VALUE arg = argv[0];
    if (!FIXNUM_P(arg) && (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(arg)) {
            VALUE coerced = korb_funcall(c, arg, korb_intern("to_int"), 0, NULL);
            if (c->state == KORB_RAISE) {
                VALUE bang = c->state_value;
                VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                    BUILTIN_TYPE(eNo) == T_CLASS) {
                    struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                    bool is_nm = false;
                    for (struct korb_class *kk = bk; kk; kk = kk->super) {
                        if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                    }
                    if (is_nm) { c->state = KORB_NORMAL; c->state_value = Qnil; }
                    else return Qnil;
                } else return Qnil;
            } else {
                arg = coerced;
            }
        }
    }
    if (FIXNUM_P(arg)) {
        n = FIX2LONG(arg);
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_BIGNUM) {
        VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
        korb_raise(c, (struct korb_class *)eR, "bignum too big to convert into 'long'");
        return Qnil;
    } else {
        korb_raise_type_error(c, "no implicit conversion from %s into Integer",
                              korb_id_name(korb_class_of_class(argv[0])->name));
        return Qnil;
    }
    if (n < 0) {
        korb_raise_argument_error(c, "negative array size");
        return Qnil;
    }
    if (n > a->len) n = a->len;
    long start = a->len - n;
    VALUE r = korb_ary_new_capa(c, c->sp, n);
    for (long i = start; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

/* ---------- Array#shuffle ----------
 * Fisher–Yates over a copy.  Uses rand(3); good enough for tests and the
 * occasional `.sample` cousin (already implemented).  Doesn't mutate self. */
static VALUE ary_shuffle(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(c, c->sp, a->len);
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

/* ---------- Array#each_cons(n) ----------
 * Sliding window of size n.  No block: returns Array<Array> of all
 * windows (koruby has no Enumerator).  With block: yields each window. */
static VALUE ary_each_cons(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    bool has_block = korb_block_given(c);
    VALUE out = has_block ? Qnil : korb_ary_new(c, c->sp);
    if (n <= 0 || n > a->len) return has_block ? Qnil : out;
    for (long i = 0; i + n <= a->len; i++) {
        VALUE win = korb_ary_new_capa(c, c->sp, n);
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
    VALUE pair = korb_ary_new_capa(c, c->sp, 2);
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

