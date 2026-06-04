/* Array — moved from builtins.c.  Included from builtins.c so we
 * inherit its includes/macros (KORB_BOOL, korb_intern, etc.). */

/* KORB_ARY_YIELD_FRAME — park a cross-yield root (typically an accumulator
 * array) in a synthetic frame's last_line slot, made current for the
 * duration of a yield loop.
 *
 * Rationale: korb_yield runs the block body at the block's own (lower) sp,
 * shrinking the GC scan range [stack_base, c->sp_top).  A root parked in an
 * sp[] slot ABOVE that level falls outside the range and gets collected
 * under a moving GC (STRESS).  The frame chain, by contrast, is ALWAYS
 * walked by visit_roots (last_line / last_match are forwarded), so a root
 * stashed there survives regardless of how far the block lowers sp_top.
 * Same idiom as node_plus parking its lhs across the rhs eval.
 *
 * The synthetic frame inherits self / fp / cref / current_class / file from
 * the live frame so the block's lvar / ivar / const / $~ lookups still
 * resolve against the surrounding receiver.  Caller MUST restore
 * c->current_frame = fr.prev on every exit path (including early returns
 * from a propagating block result).  `init_expr` may itself be a GC point;
 * it is evaluated before the frame is linked and its result is held only
 * across the immediately-following assignment (no GC in between). */
#define KORB_ARY_YIELD_FRAME(c, fr, init_expr)                       \
    struct korb_frame fr = {                                         \
        .prev          = (c)->current_frame,                         \
        .self          = (c)->current_frame->self,                   \
        .fp            = (c)->current_frame->fp,                      \
        .cref          = (c)->current_frame->cref,                   \
        .current_class = (c)->current_frame->current_class,          \
        .current_file  = (c)->current_frame->current_file,           \
        .block         = (c)->current_frame->block,                  \
        .last_line     = Qnil,                                       \
        .last_match    = (c)->current_frame->last_match,             \
    };                                                               \
    fr.last_line = (init_expr);                                      \
    (c)->current_frame = &fr

/* Array#to_a — for a plain Array, returns self.  For subclasses,
 * returns a fresh Array with the same contents (CRuby semantics). */
static RESULT ary_to_a(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (korb_class_of_class(self) == KORB_VM(c)->array_class) return RESULT_OK(self);
    long len = korb_ary_len(self);
    /* R5: result is pre-sized so push never grows, but korb_ary_new_capa is a
     * GC point — park it at sp[0] and re-derive the source from its slot. */
    sp[0] = korb_ary_new_capa(c, sp + 1, len);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < len; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    return RESULT_OK(sp[0]);
}

/* Array#to_ary / Array#deconstruct — return self.  to_ary is the
 * canonical "I behave as an array" hook used in argument splatting and
 * pattern matching; deconstruct is the analogous pattern-match hook. */
static RESULT ary_self(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(self);
}

/* Coerce v to an Integer via #to_int (CRuby protocol).  Returns the
 * Fixnum/Bignum on success.  On failure propagates the raise via RESULT.
 * Already-Integer values pass through. */
static RESULT korb_to_int_or_raise(CTX *c, VALUE v) {
    if (FIXNUM_P(v)) return RESULT_OK(v);
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_BIGNUM) return RESULT_OK(v);
    /* Float / mock / user object — try #to_int.  Float defines to_int
     * (truncates), heap objects can override.  Special-const values
     * other than Float (true/false/nil/Symbol) reject below. */
    bool is_real = !SPECIAL_CONST_P(v);
    bool is_float = FLONUM_P(v) || (is_real && BUILTIN_TYPE(v) == T_FLOAT);
    if (is_real || is_float) {
        /* respond_to? and to_int are GC points, and `v` is a bare param
         * VALUE with no caller slot to re-read.  Park it on the value stack
         * so the moving GC forwards it; otherwise the second funcall (and
         * the error-message class lookups) dispatch on a dead pointer. */
        VALUE *const vroot = c->sp_top;
        vroot[0] = v;
        c->sp_top = vroot + 1;
        RESULT _rt = korb_funcall(c, c->sp_top, vroot[0], korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_int")) });
        if (_rt.state != KORB_NORMAL) { c->sp_top = vroot; return _rt; }
        if (RTEST(_rt.value)) {
            RESULT _ri = korb_funcall(c, c->sp_top, vroot[0], korb_intern("to_int"), 0, NULL);
            if (_ri.state != KORB_NORMAL) { c->sp_top = vroot; return _ri; }
            VALUE r = _ri.value;
            if (FIXNUM_P(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_BIGNUM)) {
                c->sp_top = vroot;
                return RESULT_OK(r);
            }
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            const char *src_n = is_float ? "Float"
                                : korb_id_name(korb_class_of_class(vroot[0])->name);
            RESULT _e = korb_raise(c, (struct korb_class *)eT,
                       "can't convert %s to Integer (%s#to_int gives %s)",
                       src_n, src_n,
                       SPECIAL_CONST_P(r) ? "(special)"
                           : korb_id_name(korb_class_of_class(r)->name));
            c->sp_top = vroot;
            return _e;
        }
        /* respond_to? was false: re-read the forwarded v for the tail's
         * error message, then pop the park. */
        v = vroot[0];
        c->sp_top = vroot;
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    const char *cn;
    if (v == Qtrue) cn = "true";
    else if (v == Qfalse) cn = "false";
    else if (v == Qnil) cn = "nil";
    else if (FLONUM_P(v)) cn = "Float";
    else if (SYMBOL_P(v)) cn = "Symbol";
    else if (SPECIAL_CONST_P(v)) cn = "(special)";
    else cn = korb_id_name(korb_class_of_class(v)->name);
    return korb_raise(c, (struct korb_class *)eT,
               "no implicit conversion of %s into Integer", cn);
}

/* ---------- Array ---------- */
static RESULT ary_size(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX(korb_ary_len(self)));
}
/* Try to coerce v to a long via #to_int (CRuby semantics): on success
 * store result in *out and return RESULT_OK(Qtrue); on failure (no
 * to_int) return RESULT_OK(Qfalse); on TypeError (to_int returns
 * non-Integer) return the raise RESULT. */
static RESULT ary_aref_to_long(CTX *c, VALUE v, long *out) {
    if (FIXNUM_P(v)) { *out = FIX2LONG(v); return RESULT_OK(Qtrue); }
    if (SPECIAL_CONST_P(v)) return RESULT_OK(Qfalse);
    struct korb_class *k = korb_class_of_class(v);
    if (!k || !korb_class_find_method(k, korb_intern("to_int"))) return RESULT_OK(Qfalse);
    RESULT tr = korb_funcall_r(c, c->sp_top, v, korb_intern("to_int"), 0, NULL);
    if (tr.state != KORB_NORMAL) return tr;
    if (FIXNUM_P(tr.value)) { *out = FIX2LONG(tr.value); return RESULT_OK(Qtrue); }
    return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                      "can't convert %s to Integer (#to_int gave non-Integer)",
                      korb_id_name(k->name));
}

static RESULT ary_aref(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc == 1) {
        if (FIXNUM_P(argv[0])) return RESULT_OK(korb_ary_aref(self, FIX2LONG(argv[0])));
        if (BUILTIN_TYPE(argv[0]) == T_RANGE) {
            struct korb_array *a = (struct korb_array *)self;
            struct korb_range *r = (struct korb_range *)argv[0];
            /* Endless / beginless ranges: nil begin/end stand in for
             * 0 / size-1.  Common in Ruby 2.7+ slicing. */
            long b, e;
            if (NIL_P(r->begin))      b = 0;
            else if (FIXNUM_P(r->begin)) b = FIX2LONG(r->begin);
            else return RESULT_OK(Qnil);
            if (NIL_P(r->end))        e = a->len - 1;
            else if (FIXNUM_P(r->end)) e = FIX2LONG(r->end);
            else return RESULT_OK(Qnil);
            if (b < 0) b += a->len;
            if (e < 0) e += a->len;
            if (r->exclude_end && !NIL_P(r->end)) e--;
            if (b < 0 || b > a->len) return RESULT_OK(Qnil);
            if (e >= a->len) e = a->len - 1;
            /* R5: res grows in the loop (push GC point); park it at sp[0] and
             * re-derive the source from its GC slot each iteration. */
            sp[0] = korb_ary_new(c, sp + 1);
            for (long i = b; i <= e; i++) {
                struct korb_array *sa = (struct korb_array *)sp[-argc - 1];
                korb_ary_push(c, sp + 1, sp[0], korb_ary_items(sa)[i]);
            }
            return RESULT_OK(sp[0]);
        }
        /* Try #to_int coercion (CRuby semantics — falls back to integer
         * index when arg responds to #to_int but isn't a Fixnum/Range). */
        long idx;
        RESULT cr = ary_aref_to_long(c, argv[0], &idx);
        if (cr.state != KORB_NORMAL) return cr;
        if (RTEST(cr.value)) return RESULT_OK(korb_ary_aref(self, idx));
        return RESULT_OK(Qnil);
    }
    if (argc == 2) {
        long start, len;
        RESULT cr1 = ary_aref_to_long(c, argv[0], &start);
        if (cr1.state != KORB_NORMAL) return cr1;
        RESULT cr2 = ary_aref_to_long(c, argv[1], &len);
        if (cr2.state != KORB_NORMAL) return cr2;
        if (!RTEST(cr1.value) || !RTEST(cr2.value)) return RESULT_OK(Qnil);
        struct korb_array *a = (struct korb_array *)self;
        if (start < 0) start += a->len;
        if (start < 0 || start > a->len || len < 0) return RESULT_OK(Qnil);
        if (start + len > a->len) len = a->len - start;
        /* R5: pre-sized; park result and re-derive source after the GC point. */
        sp[0] = korb_ary_new_capa(c, sp + 1, len);
        a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < len; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[start + i]);
        return RESULT_OK(sp[0]);
    }
    return RESULT_OK(Qnil);
}
/* Reject indices that would resize the array beyond a reasonable
 * limit.  CRuby uses LONG_MAX/sizeof(VALUE) (~1.15e18); we use the
 * same bound — large enough that real code never hits it but small
 * enough that test_aset_error's `[0][LONGP] = 2` raises IndexError
 * instead of OOM-killing the process while expanding to 2^63 slots. */
#define KORB_ARY_MAX_LEN ((long)(LONG_MAX / sizeof(VALUE)))
/* Returns RESULT_OK(Qnil) on success, raise RESULT on too-big index. */
static RESULT korb_ary_check_index(CTX *c, long idx) {
    if (idx >= KORB_ARY_MAX_LEN) {
        VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
        return korb_raise(c, (struct korb_class *)eIE, "index %ld too big", idx);
    }
    return RESULT_OK(Qnil);
}
static RESULT ary_aset(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc == 2 && FIXNUM_P(argv[0])) {
        long i = FIX2LONG(argv[0]);
        struct korb_array *a = (struct korb_array *)self;
        if (i < 0 && i + a->len < 0) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE,
                       "index %ld too small for array; minimum: -%ld",
                       i, a->len);
        }
        CHECK(korb_ary_check_index(c, i));
        korb_ary_aset(c, c->sp_top, self, i, argv[1]);
        return RESULT_OK(argv[1]);
    }
    if (argc == 2 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_BIGNUM) {
        VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
        return korb_raise(c, (struct korb_class *)eIE, "index too big");
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
        sp[0] = self;
        sp[1] = INT2FIX(b);
        sp[2] = INT2FIX(e - b + 1);
        sp[3] = argv[1];
        return ary_aset(c, 3, sp + 4);
    }
    if (argc == 3 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        /* a[start, len] = value or a[start, len] = array */
        struct korb_array *a = (struct korb_array *)self;
        long start = FIX2LONG(argv[0]);
        long len = FIX2LONG(argv[1]);
        if (len < 0) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE, "negative length (%ld)", len);
        }
        long orig_start = start;
        if (start < 0) start += a->len;
        if (start < 0) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE,
                       "index %ld too small for array; minimum: -%ld",
                       orig_start, a->len);
        }
        CHECK(korb_ary_check_index(c, start));
        VALUE val = argv[2];
        /* If rhs is not an Array but responds to #to_ary, coerce it.
         * Subclasses of Array keep their identity (CRuby skips the
         * conversion for subclasses). */
        if (!SPECIAL_CONST_P(val) && BUILTIN_TYPE(val) != T_ARRAY) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, val, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            /* respond_to? is a GC point — re-read val from its scanned arg
             * slot before dispatching to_ary on it. */
            val = argv[2];
            if (RTEST(rt)) {
                VALUE coerced = UNWRAP(korb_funcall(c, c->sp_top, val, korb_intern("to_ary"), 0, NULL));
                if (!SPECIAL_CONST_P(coerced) && BUILTIN_TYPE(coerced) == T_ARRAY) {
                    val = coerced;
                }
            }
        }
        if (BUILTIN_TYPE(val) == T_ARRAY) {
            /* Park val (src) at sp[0]: it may be a coerced C-local not held in
             * any GC-scanned slot, and the korb_ary_push loops below are GC
             * points that move both self and src.  self stays live in the
             * receiver slot sp[-argc-1]; re-derive `a` from it after each
             * push loop (moving GC forwards the array object itself). */
            sp[0] = val;
            c->sp_top = sp + 1;
            struct korb_array *src = (struct korb_array *)sp[0];
            /* Snapshot src into a fresh buffer when src aliases self,
             * otherwise the shift below will scribble over the values
             * we're about to copy.  CRuby's `b[1, 0] = b` test pins this. */
            long src_len = src->len;
            VALUE *src_buf;
            bool snapped = false;
            if (sp[0] == sp[-argc - 1]) {
                src_buf = src_len > 0 ? korb_xmalloc(sizeof(VALUE) * src_len) : NULL;
                for (long i = 0; i < src_len; i++) src_buf[i] = korb_ary_items(src)[i];
                snapped = true;
            } else {
                src_buf = korb_ary_items(src);
            }
            a = (struct korb_array *)sp[-argc - 1];
            /* CRuby a[start, len] = src_array:
             *  - If start > a->len, pad with nil up to start.
             *  - Replace elements at [start, start+len) with src_array.
             *  - Resize via shift if src_array.len != len.
             */
            if (start > a->len) {
                /* Re-derive `a` in the condition too: each push grows (and
                 * moves) the array, leaving the C-local stale. */
                while (((struct korb_array *)sp[-argc - 1])->len < start) korb_ary_push(c, sp + 1, sp[-argc - 1], Qnil);
                a = (struct korb_array *)sp[-argc - 1];
            }
            long avail_len = a->len - start;
            if (len > avail_len) len = avail_len;
            long diff = src_len - len;
            long old = a->len;
            if (diff > 0) {
                for (long i = 0; i < diff; i++) korb_ary_push(c, sp + 1, sp[-argc - 1], Qnil);
                a = (struct korb_array *)sp[-argc - 1];
                for (long i = old - 1; i >= start + len; i--) korb_ary_items(a)[i + diff] = korb_ary_items(a)[i];
                /* If src aliases self, we wrote into the snapped buf before
                 * push; otherwise re-read src's (possibly moved) buffer. */
                if (!snapped) src_buf = korb_ary_items((struct korb_array *)sp[0]);
            } else if (diff < 0) {
                for (long i = start + len; i < old; i++) korb_ary_items(a)[i + diff] = korb_ary_items(a)[i];
                a->len += diff;
            }
            for (long i = 0; i < src_len; i++) {
                korb_ary_items(a)[start + i] = src_buf[i];
            }
            c->sp_top = sp;
        } else {
            /* `a[start, len] = val` — when val is NOT an Array, CRuby
             * replaces the slice [start, start+len) with the SINGLE
             * element val (i.e. removes len elements, inserts 1).
             * If start > a->len, pad with nil first.
             * Park val (a possibly-coerced C-local) at sp[0] across the
             * korb_ary_push GC points; re-derive `a` from the receiver slot. */
            sp[0] = val;
            c->sp_top = sp + 1;
            if (start > a->len) {
                /* Re-derive `a` in the condition too: each push grows (and
                 * moves) the array, leaving the C-local stale. */
                while (((struct korb_array *)sp[-argc - 1])->len < start) korb_ary_push(c, sp + 1, sp[-argc - 1], Qnil);
                a = (struct korb_array *)sp[-argc - 1];
            }
            long avail_len = a->len - start;
            if (len > avail_len) len = avail_len;
            long diff = 1 - len;  /* +1 inserted, -len removed */
            long old = a->len;
            if (diff > 0) {
                for (long i = 0; i < diff; i++) korb_ary_push(c, sp + 1, sp[-argc - 1], Qnil);
                a = (struct korb_array *)sp[-argc - 1];
                for (long i = old - 1; i >= start + len; i--) korb_ary_items(a)[i + diff] = korb_ary_items(a)[i];
            } else if (diff < 0) {
                for (long i = start + len; i < old; i++) korb_ary_items(a)[i + diff] = korb_ary_items(a)[i];
                a->len += diff;
            }
            korb_ary_items(a)[start] = sp[0];
            c->sp_top = sp;
        }
        return RESULT_OK(argv[2]);
    }
    return RESULT_OK(Qnil);
}
static RESULT ary_push(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    /* Moving GC: korb_ary_push can grow the backing → GC → move the array
     * handle.  Re-read self from the GC-tracked receiver slot sp[-argc-1] each
     * iteration; argv[i] sits in scanned arg slots so it stays valid. */
    for (int i = 0; i < argc; i++) korb_ary_push(c, c->sp_top, sp[-argc - 1], argv[i]);
    return RESULT_OK(sp[-argc - 1]);
}
static RESULT ary_pop(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc > 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", argc);
    }
    if (argc >= 1) {
        VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
        if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);  /* Bignum n: way bigger than array */
        long n = FIX2LONG(iv);
        if (n < 0) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg, "negative array size");
        }
        /* korb_to_int_or_raise is a GC point — re-read self. */
        self = sp[-argc - 1];
        long alen = korb_ary_len(self);
        long take = n > alen ? alen : n;
        long start = alen - take;
        /* R5: pre-sized; park result and re-derive source after the GC point. */
        sp[0] = korb_ary_new_capa(c, sp + 1, take);
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = start; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
        a->len = start;
        return RESULT_OK(sp[0]);
    }
    return RESULT_OK(korb_ary_pop(self));
}
static RESULT ary_first(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ary_aref(self, 0));
}
static RESULT ary_last(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long len = korb_ary_len(self);
    return RESULT_OK(korb_ary_aref(self, len - 1));
}
static RESULT ary_each(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("each"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &arg);
    }
    /* CRuby semantics: re-read length each iteration so the block can grow /
     * shrink the array.  Moving GC: the receiver is a moving array handle
     * and korb_yield runs the block body at a lower sp (shrinking the scan
     * range), so park it in a synthetic frame's last_match (frame chain is
     * always walked) and re-read it each iteration. */
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE recv = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(recv);
}
static RESULT ary_each_with_index(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long len = korb_ary_len(self);
    if (!korb_block_given(c)) {
        /* R5: park result at sp[0] and the current pair at sp[1] across the
         * per-element korb_ary_new_capa / pushes.  Reserve sp_top = sp+2 so
         * both parked slots stay in the GC scan range across each alloc. */
        sp[0] = 0;
        sp[1] = 0;
        c->sp_top = sp + 2;
        sp[0] = korb_ary_new_capa(c, sp + 2, len);
        for (long i = 0; i < len; i++) {
            sp[1] = korb_ary_new_capa(c, sp + 2, 2);
            korb_ary_push(c, sp + 2, sp[1], korb_ary_aref(sp[-argc - 1], i));
            korb_ary_push(c, sp + 2, sp[1], INT2FIX(i));
            korb_ary_push(c, sp + 2, sp[0], sp[1]);
        }
        c->sp_top = sp;
        return RESULT_OK(sp[0]);
    }
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    /* Re-read length each step so the block may grow the array (CRuby). */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        /* receiver moves across korb_yield — re-read from the frame slot. */
        VALUE args[2] = { korb_ary_aref(fr.last_match, i), INT2FIX(i) };
        RESULT _y = korb_yield(c, 2, args);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE recv = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(recv);
}
static RESULT ary_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) return RESULT_OK(self);
    long len = korb_ary_len(self);
    /* Park BOTH the result array (fr.last_line) and the source receiver
     * (fr.last_match) across the per-element korb_yield.  korb_yield runs
     * the block body at the block's own (lower) sp, shrinking the GC scan
     * range [stack_base, sp_top); roots parked in sp[] slots above that
     * level fall outside the range and get collected.  The frame chain is
     * ALWAYS walked by visit_roots (last_line / last_match forwarded), so
     * the synthetic frame keeps both alive regardless of sp_top.  Same
     * idiom as node_plus.  (last_match doubles as $~ for the block body;
     * iterator blocks rarely read $~, and koruby Regexp is astrorge-
     * pending, so reusing it here is acceptable.) */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, len));
    /* Re-read the receiver from its GC slot — `self` is a stale C-local
     * after the korb_ary_new_capa above moved it. */
    fr.last_match = sp[-argc - 1]; /* park source receiver */
    /* Re-read length each step so the block may grow the array (CRuby
     * "tolerates increasing size"); len is only the capa hint. */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        korb_ary_push(c, c->sp_top, fr.last_line, _y.value);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}
static RESULT ary_select(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) return RESULT_OK(self);
    /* Park result (fr.last_line) + source receiver (fr.last_match) across
     * the per-element korb_yield via the frame chain (see ary_map).  Re-read
     * length each step so the block may grow the array (CRuby semantics). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (RTEST(_y.value)) korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_aref(fr.last_match, i));
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}
static RESULT ary_reduce(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
        /* Symbol form.  Park acc (fr.last_line) + receiver (fr.last_match)
         * across the per-element korb_funcall via the frame chain. */
        if (sym_idx == 0) { /* reduce(:+) */
            if (len == 0) return RESULT_OK(Qnil);
            acc = korb_ary_aref(self, 0);
            i = 1;
        } else {            /* reduce(init, :+) */
            acc = argv[0];
            i = 0;
        }
        KORB_ARY_YIELD_FRAME(c, fr, acc);
        fr.last_match = sp[-argc - 1];
        for (; i < len; i++) {
            VALUE other = korb_ary_aref(fr.last_match, i);
            RESULT _r = korb_funcall(c, c->sp_top, fr.last_line, op, 1, &other);
            if (_r.state != KORB_NORMAL) { c->current_frame = fr.prev; return _r; }
            fr.last_line = _r.value;
        }
        VALUE result = fr.last_line;
        c->current_frame = fr.prev;
        return RESULT_OK(result);
    }
    /* Block form.  Park acc (fr.last_line) + receiver (fr.last_match). */
    acc = argc > 0 ? argv[0] : korb_ary_aref(self, 0);
    KORB_ARY_YIELD_FRAME(c, fr, acc);
    fr.last_match = sp[-argc - 1];
    i = argc > 0 ? 0 : 1;
    for (; i < len; i++) {
        VALUE args[2] = { fr.last_line, korb_ary_aref(fr.last_match, i) };
        RESULT _y = korb_yield(c, 2, args);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        fr.last_line = _y.value;
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}
static RESULT ary_join(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* CRuby short-circuit: empty array returns "" without touching sep. */
    if (korb_ary_len(self) == 0) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
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
            sep = korb_str_new_cstr(c, c->sp_top, "");
        }
    } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        sep = argv[0];
    } else {
        VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) }));
        if (!RTEST(rt)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into String",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
        }
        sep = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("to_str"), 0, NULL));
        if (SPECIAL_CONST_P(sep) || BUILTIN_TYPE(sep) != T_STRING) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "can't convert to String (to_str returned non-String)");
        }
    }
    /* Pin self / sep / result / per-iter element in sp[0..3].
     * Inner alloc helpers publish c->sp_top = sp+4 themselves.  Re-read self:
     * the separator to_str coercion above is a GC point. */
    sp[0] = sp[-argc - 1];
    sp[1] = sep;
    sp[2] = 0;
    sp[3] = Qnil;
    sp[2] = korb_str_new(c, sp + 4, "", 0);
    long len = korb_ary_len(sp[0]);
    for (long i = 0; i < len; i++) {
        if (i > 0 && BUILTIN_TYPE(sp[1]) == T_STRING) korb_str_concat(c, sp + 4, sp[2], sp[1]);
        sp[3] = korb_ary_aref(sp[0], i);
        if (BUILTIN_TYPE(sp[3]) != T_STRING) sp[3] = korb_to_s(c, sp + 4, sp[3]);
        korb_str_concat(c, sp + 4, sp[2], sp[3]);
    }
    return RESULT_OK(sp[2]);
}
static RESULT ary_inspect(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_inspect(c, c->sp_top, self));
}

/* Array#to_h — convert [[k,v], [k,v], ...] (or yield-pair-from-block)
 * into a Hash.  With a block, the block's return value (a 2-element
 * Array) supplies the pair for each element — mirrors CRuby's
 * `[1,2,3].to_h { |i| [i, i*i] }` form. */
/* Array#to_h — new sp/RESULT ABI (cfunc_r).  Migrated from legacy
 * VALUE/c->state cfunc as part of Phase 8 staged migration.  Uses
 * korb_funcall_r + UNWRAP for in-band exception propagation. */
static RESULT ary_to_h(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    if (argc > 0) {
        return korb_raise_argument_error(c,
                   "wrong number of arguments (given %d, expected 0)", argc);
    }
    /* The Hash is libc-backed (non-moving), so `h` is a stable C-local.  The
     * source receiver (fr.last_match) and the moving per-element `pair`
     * (fr.last_line) are parked in a synthetic frame across the korb_yield /
     * to_ary funcall GC points (frame chain always scanned). */
    VALUE h = korb_hash_new(c, sp);
    bool has_block = korb_block_given(c);
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-1];
    /* Re-read length each step so a block may grow the array (CRuby
     * "tolerates increasing size during iteration"). */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        if (has_block) {
            VALUE v = korb_ary_aref(fr.last_match, i);
            RESULT _y = korb_yield_r(c, 1, &v);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            fr.last_line = _y.value;
        } else {
            fr.last_line = korb_ary_aref(fr.last_match, i);
        }
        /* CRuby: try to_ary for non-Array elements (but not to_a). */
        if (SPECIAL_CONST_P(fr.last_line) || BUILTIN_TYPE(fr.last_line) != T_ARRAY) {
            if (!SPECIAL_CONST_P(fr.last_line)) {
                VALUE arg = korb_id2sym(korb_intern("to_ary"));
                RESULT _rt = korb_funcall_r(c, c->sp_top, fr.last_line, korb_intern("respond_to?"), 1, &arg);
                if (_rt.state != KORB_NORMAL) { c->current_frame = fr.prev; return _rt; }
                if (RTEST(_rt.value)) {
                    RESULT _ta = korb_funcall_r(c, c->sp_top, fr.last_line, korb_intern("to_ary"), 0, NULL);
                    if (_ta.state != KORB_NORMAL) { c->current_frame = fr.prev; return _ta; }
                    fr.last_line = _ta.value;
                }
            }
            if (SPECIAL_CONST_P(fr.last_line) || BUILTIN_TYPE(fr.last_line) != T_ARRAY) {
                VALUE bad = fr.last_line;
                c->current_frame = fr.prev;
                return korb_raise_type_error(c,
                           "wrong element type %s at %ld (expected array)",
                           SPECIAL_CONST_P(bad) ? "(special)"
                               : korb_id_name(korb_class_of_class(bad)->name),
                           i);
            }
        }
        /* pair does not span a GC from here: extract both elements before
         * korb_hash_aset (whose only GC receives key/val by value). */
        struct korb_array *p = (struct korb_array *)fr.last_line;
        if (p->len != 2) {
            long plen = p->len;
            c->current_frame = fr.prev;
            return korb_raise_argument_error(c,
                       "wrong array length at %ld (expected 2, was %ld)", i, plen);
        }
        VALUE pk = korb_ary_items(p)[0];
        VALUE pv = korb_ary_items(p)[1];
        korb_hash_aset(c, h, pk, pv);
    }
    c->current_frame = fr.prev;
    return RESULT_OK(h);
}
/* New sp-based RESULT-returning ABI (Phase 3 PoC).
 *
 * Convention:
 *   sp[-2] = self (the array on the LHS)
 *   sp[-1] = other (the RHS arg)
 *   sp[0..] = scratch (unused here)
 *
 * Both slots are in c->sp_top range so visit_roots auto-forwards them across
 * any GC fired by inner korb_eq dispatches. */
static RESULT ary_eq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;  /* alloc 前 sync: korb_eq -> method dispatch が GC を起こしうる */
    if (BUILTIN_TYPE(sp[-1]) != T_ARRAY) {
        /* CRuby: rhs not an Array → try rhs.==(self) if it responds_to?
         * :to_ary (Array-like mock objects).  Otherwise false. */
        if (!SPECIAL_CONST_P(sp[-1])) {
            VALUE arg_sym = korb_id2sym(korb_intern("to_ary"));
            VALUE rt = UNWRAP(korb_funcall_r(c, c->sp_top, sp[-1], korb_intern("respond_to?"), 1, &arg_sym));
            if (RTEST(rt)) {
                VALUE r = UNWRAP(korb_funcall_r(c, c->sp_top, sp[-1], korb_intern("=="), 1, &sp[-2]));
                return RESULT_OK(RTEST(r) ? Qtrue : Qfalse);
            }
        }
        return RESULT_OK(Qfalse);
    }
    long la = korb_ary_len(sp[-2]);
    long lb = korb_ary_len(sp[-1]);
    if (la != lb) return RESULT_OK(Qfalse);
    /* Recursion guard for self-referential arrays. */
    static __thread VALUE eq_stk_a[64];
    static __thread VALUE eq_stk_b[64];
    static __thread int eq_top = 0;
    for (int j = 0; j < eq_top; j++) {
        if (eq_stk_a[j] == sp[-2] && eq_stk_b[j] == sp[-1]) return RESULT_OK(Qtrue);
    }
    if (eq_top < 64) {
        eq_stk_a[eq_top] = sp[-2];
        eq_stk_b[eq_top] = sp[-1];
        eq_top++;
    }
    RESULT result = RESULT_OK(Qtrue);
    for (long i = 0; i < la; i++) {
        /* Re-read sp[-2]/sp[-1] each iter — they're slot-tracked, so even
         * if korb_eq's inner dispatch fires GC and moves the arrays, the
         * next iteration's korb_ary_aref reads the forwarded address. */
        VALUE a = korb_ary_aref(sp[-2], i);
        VALUE b = korb_ary_aref(sp[-1], i);
        /* CRuby: Array#== checks identity first (so [NaN] == [NaN] is
         * true via NaN.equal?(NaN)). */
        if (a == b) continue;
        bool eq = korb_eq(c, a, b);
        /* Element-level user-dispatch fallback for mock-style ==: only
         * when a is a user-defined object (not built-in numeric / string
         * / collection). */
        if (!eq && !FIXNUM_P(a) && !FLONUM_P(a) && !SPECIAL_CONST_P(a)) {
            enum korb_type ta = BUILTIN_TYPE(a);
            if (ta != T_STRING && ta != T_ARRAY && ta != T_HASH &&
                ta != T_RANGE && ta != T_BIGNUM && ta != T_FLOAT) {
                RESULT _er = korb_funcall_r(c, c->sp_top, a, korb_intern("=="), 1, &b);
                if (_er.state != KORB_NORMAL) { result = _er; goto done_eq; }
                eq = RTEST(_er.value);
            }
        }
        if (!eq) { result = RESULT_OK(Qfalse); goto done_eq; }
    }
done_eq:
    if (eq_top > 0) eq_top--;
    return result;
}
static RESULT ary_lshift(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    /* korb_ary_push parks self at sp[0] and re-derives it after the grow
     * GC, so sp[0] holds the forwarded handle on return; the C-local self
     * is stale.  Return the parked (forwarded) slot. */
    korb_ary_push(c, sp, self, argv[0]);
    return RESULT_OK(sp[0]);
}
static RESULT ary_dup(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long len = korb_ary_len(self);
    /* R5: park result; re-read self from its GC slot after the new_capa. */
    sp[0] = korb_ary_new_capa(c, sp + 1, len);
    for (long i = 0; i < len; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(sp[-argc - 1], i));
    return RESULT_OK(sp[0]);
}


/* ---------- Array methods (extended) ---------- */

/* Compare two values using either the supplied block or default `<=>`,
 * returning a negative/zero/positive long like a C sort comparator.
 * On incomparable (`<=>` returns nil) or raised exception, sets *err
 * to the propagating RESULT and returns 0. */
static long ary_sort_compare(CTX *c, VALUE x, VALUE y, bool has_block, RESULT *err) {
    /* Park x/y across the comparator (yield / <=> funcall = GC points) so the
     * nil-result error path can build its message from live (forwarded)
     * handles rather than stale C-locals. */
    VALUE *const root = c->sp_top;
    root[0] = x; root[1] = y;
    c->sp_top = root + 2;
    VALUE r;
    if (has_block) {
        VALUE pair[2] = { root[0], root[1] };
        RESULT _r = korb_yield(c, 2, pair);
        if (_r.state != KORB_NORMAL) { c->sp_top = root; *err = _r; return 0; }
        r = _r.value;
    } else if (FIXNUM_P(x) && FIXNUM_P(y)) {
        c->sp_top = root;
        return (intptr_t)x < (intptr_t)y ? -1 : (intptr_t)x > (intptr_t)y ? 1 : 0;
    } else {
        RESULT _r = korb_funcall(c, c->sp_top, root[0], korb_intern("<=>"), 1, &root[1]);
        if (_r.state != KORB_NORMAL) { c->sp_top = root; *err = _r; return 0; }
        r = _r.value;
    }
    /* CRuby: sort block return is used by sign — Fixnum sign extracted
     * directly; Bignum compared against 0 via korb_int_cmp; Float by
     * sign; nil → raise ArgumentError (CRuby semantics). */
    long ret = 0;
    if (FIXNUM_P(r)) ret = FIX2LONG(r);
    else if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_BIGNUM) {
        ret = korb_int_cmp(r, INT2FIX(0));
    } else if (KORB_IS_FLOAT(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT)) {
        double d = korb_num2dbl(r);
        ret = d < 0 ? -1 : d > 0 ? 1 : 0;
    } else if (NIL_P(r)) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        *err = korb_raise(c, (struct korb_class *)eArg,
                   "comparison of %s with %s failed",
                   korb_id_name(korb_class_of_class(root[0])->name),
                   korb_id_name(korb_class_of_class(root[1])->name));
    }
    c->sp_top = root;
    return ret;
}

/* In-place insertion sort.  The array handle (fr.last_line) and the lifted
 * probe element (fr.last_line's [1] re-read) are parked in a synthetic frame
 * across the ary_sort_compare yield/funcall GC points (frame chain always
 * scanned).  The probe is kept in an sp slot rather than a frame slot so the
 * frame's last_match ($~) is not disturbed; the caller passes a 1-slot
 * staging base `sp` whose sp[0] is reserved here. */
static RESULT ary_sort_in_place(CTX *c, VALUE *sp, VALUE av, bool has_block) {
    long n = korb_ary_len(av);
    KORB_ARY_YIELD_FRAME(c, fr, av);   /* the array */
    sp[0] = 0;
    c->sp_top = sp + 1;                /* protect the parked probe across compares */
    RESULT _ret = RESULT_OK(Qnil);
    for (long i = 1; i < n; i++) {
        sp[0] = korb_ary_items((struct korb_array *)fr.last_line)[i];   /* probe */
        long j = i - 1;
        while (j >= 0) {
            VALUE xj = korb_ary_items((struct korb_array *)fr.last_line)[j];
            long cmp = ary_sort_compare(c, xj, sp[0], has_block, &_ret);
            if (_ret.state != KORB_NORMAL) { c->current_frame = fr.prev; return _ret; }
            if (cmp <= 0) break;
            struct korb_array *ra = (struct korb_array *)fr.last_line;
            korb_ary_items(ra)[j+1] = korb_ary_items(ra)[j];
            j--;
        }
        korb_ary_items((struct korb_array *)fr.last_line)[j+1] = sp[0];
    }
    c->current_frame = fr.prev;
    return _ret;
}

static RESULT ary_sort(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long n = korb_ary_len(self);
    /* Park the copy at sp[0] while building it; ary_sort_in_place re-parks
     * it in its own frame and stages the probe at sp+1. */
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    c->sp_top = sp + 1;
    for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(sp[-argc - 1], i));
    CHECK(ary_sort_in_place(c, sp + 1, sp[0], korb_block_given(c)));
    VALUE result = sp[0];
    c->sp_top = sp;
    return RESULT_OK(result);
}

/* Array#sort! — mutates self, returns self.  The existing
 * registration aliases sort! to ary_sort which would build a copy and
 * return it; for the bang form we need to sort the receiver directly. */
static RESULT ary_sort_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    CHECK(ary_sort_in_place(c, sp, self, korb_block_given(c)));
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT ary_sort_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* yield each, then sort by yielded value.  Park the [key,val] pairs
     * array (fr.last_line) and the source receiver (fr.last_match) in a
     * synthetic frame across the per-element korb_yield and the <=> funcalls
     * (the frame chain is always scanned; sp[] slots above the block/method
     * body's sp would be collected).  The current pair / yielded key are
     * transient and re-read from the framed pairs each iteration. */
    long n = korb_ary_len(self);
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, n));
    fr.last_match = sp[-argc - 1];
    /* Re-read length each step so the block may grow the array (CRuby
     * "tolerates increasing size during iteration"); n is only the capa hint. */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE elem = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &elem);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        sp[0] = _y.value;                 /* yielded key (transient) */
        c->sp_top = sp + 1;
        sp[1] = korb_ary_new_capa(c, sp + 2, 2);   /* the pair */
        korb_ary_push(c, sp + 2, sp[1], sp[0]);
        korb_ary_push(c, sp + 2, sp[1], korb_ary_aref(fr.last_match, i));
        korb_ary_push(c, sp + 2, fr.last_line, sp[1]);
        c->sp_top = sp;
    }
    /* insertion sort the pairs by [0].  pairs stay in fr.last_line; the
     * lifted-out pair `pi` is parked in fr.last_match (the receiver is no
     * longer needed) across the <=> funcalls.  ki is re-read from fr.last_match
     * and passed by value (korb_funcall snapshots its args at entry). */
    for (long i = 1; i < n; i++) {
        fr.last_match = korb_ary_aref(fr.last_line, i);   /* pi */
        long j = i - 1;
        while (j >= 0) {
            VALUE pj = korb_ary_aref(fr.last_line, j);
            VALUE kj = korb_ary_aref(pj, 0);
            VALUE ki = korb_ary_aref(fr.last_match, 0);
            RESULT _c = korb_funcall(c, c->sp_top, kj, korb_intern("<=>"), 1, &ki);
            if (_c.state != KORB_NORMAL) { c->current_frame = fr.prev; return _c; }
            VALUE cmp = _c.value;
            if (FIXNUM_P(cmp) && FIX2LONG(cmp) <= 0) break;
            struct korb_array *p = (struct korb_array *)fr.last_line;
            korb_ary_items(p)[j+1] = korb_ary_items(p)[j];
            j--;
        }
        korb_ary_items((struct korb_array *)fr.last_line)[j+1] = fr.last_match;
    }
    /* extract [1] of each pair into the result (pre-sized; park at sp[0]). */
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    c->sp_top = sp + 1;
    for (long i = 0; i < n; i++)
        korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(korb_ary_aref(fr.last_line, i), 1));
    VALUE result = sp[0];
    c->sp_top = sp;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT ary_zip(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Normalize each non-Array arg to an Array via #to_ary / #to_a / #each,
     * collecting the cooked handles into a parked Array.  Three roots span
     * the to_ary funcalls and the per-row korb_yield: the cooked-sources
     * array (fc.last_match), the source receiver (fc.last_line) and the
     * result accumulator (fr.last_line) — parked in two chained synthetic
     * frames (frame chain always scanned).  A nil cooked entry contributes
     * nil for every position.  The per-row tuple is transient at sp[0]. */
    bool has_block = korb_block_given(c);
    KORB_ARY_YIELD_FRAME(c, fc, korb_ary_new_capa(c, c->sp_top, argc));   /* cooked[] */
    fc.last_match = fc.last_line;             /* cooked-sources array */
    fc.last_line  = sp[-argc - 1];            /* source receiver */
    KORB_ARY_YIELD_FRAME(c, fr,
        has_block ? Qnil : korb_ary_new_capa(c, c->sp_top, korb_ary_len(sp[-argc - 1])));  /* result */
    for (int j = 0; j < argc; j++) {
        VALUE v = (sp - argc)[j];
        VALUE cooked = Qnil;
        if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_ARRAY) {
            cooked = v;
        } else {
            /* Try to_ary, then to_a. */
            VALUE rt_ary = UNWRAP(korb_funcall(c, c->sp_top, v, korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            if (RTEST(rt_ary)) {
                VALUE r2 = UNWRAP(korb_funcall(c, c->sp_top, v, korb_intern("to_ary"), 0, NULL));
                if (!SPECIAL_CONST_P(r2) && BUILTIN_TYPE(r2) == T_ARRAY) cooked = r2;
            }
            if (NIL_P(cooked)) {
                VALUE rt_a = UNWRAP(korb_funcall(c, c->sp_top, v, korb_intern("respond_to?"), 1,
                                          (VALUE[]){ korb_id2sym(korb_intern("to_a")) }));
                if (RTEST(rt_a)) {
                    VALUE r2 = UNWRAP(korb_funcall(c, c->sp_top, v, korb_intern("to_a"), 0, NULL));
                    if (!SPECIAL_CONST_P(r2) && BUILTIN_TYPE(r2) == T_ARRAY) cooked = r2;
                }
            }
            if (NIL_P(cooked)) {
                /* CRuby fallback: if arg responds to #each, collect up to
                 * self's length via __zip_each__ (each with break at size). */
                VALUE rt_each = UNWRAP(korb_funcall(c, c->sp_top, v, korb_intern("respond_to?"), 1,
                                             (VALUE[]){ korb_id2sym(korb_intern("each")) }));
                if (RTEST(rt_each)) {
                    VALUE args[2] = { v, INT2FIX(korb_ary_len(fc.last_line)) };
                    VALUE arr = UNWRAP(korb_funcall_r(c, c->sp_top,
                        (VALUE)KORB_VM(c)->array_class,
                        korb_intern("__zip_each__"), 2, args));
                    if (!SPECIAL_CONST_P(arr) && BUILTIN_TYPE(arr) == T_ARRAY) cooked = arr;
                }
            }
        }
        korb_ary_push(c, c->sp_top, fc.last_match, cooked);   /* nil tuple element if unset */
    }

    long n = korb_ary_len(fc.last_line);
    for (long i = 0; i < n; i++) {
        sp[0] = korb_ary_new_capa(c, sp + 1, 1 + argc);
        c->sp_top = sp + 1;
        korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(fc.last_line, i));
        for (int j = 0; j < argc; j++) {
            VALUE cooked = korb_ary_aref(fc.last_match, j);
            if (NIL_P(cooked)) {
                korb_ary_push(c, sp + 1, sp[0], Qnil);
            } else {
                korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(cooked, i));
            }
        }
        if (has_block) {
            RESULT _y = korb_yield(c, 1, &sp[0]);
            if (_y.state != KORB_NORMAL) { c->sp_top = sp; c->current_frame = fc.prev; return _y; }
        } else {
            korb_ary_push(c, sp + 1, fr.last_line, sp[0]);
        }
        c->sp_top = sp;
    }
    VALUE result = fr.last_line;
    c->current_frame = fc.prev;
    return RESULT_OK(result);
}

/* Tracks an in-progress descent through nested arrays to detect cycles
 * (`a << a; a.flatten`).  CRuby raises ArgumentError once it hits a
 * subarray it's already descended into. */
static bool ary_flatten_stack_contains(VALUE stack_ary, VALUE v) {
    long n = korb_ary_len(stack_ary);
    for (long i = 0; i < n; i++) if (korb_ary_aref(stack_ary, i) == v) return true;
    return false;
}
/* Recursive flatten worker.  All cross-GC roots live in caller-owned value
 * stack slots (payload-as-VALUE: result array, source array and the cycle
 * stack all move under STRESS).  `result` and `stack` are STABLE slot
 * addresses shared by every recursion level (the VALUEs they hold are
 * GC-scanned/forwarded in place).  `sp` is this level's private staging
 * base: sp[0] holds THIS level's source array across the GC points.
 * c->sp_top must already cover [stack_base, sp+1) on entry. */
static RESULT ary_flatten_into(CTX *c, VALUE *result, VALUE *stack,
                               VALUE *sp, long depth) {
    c->sp_top = sp + 1;
    for (long i = 0; i < korb_ary_len(sp[0]); i++) {
        VALUE el = korb_ary_aref(sp[0], i);
        VALUE coerced = el;
        bool is_ary = !SPECIAL_CONST_P(el) && BUILTIN_TYPE(el) == T_ARRAY;
        if (depth != 0 && !is_ary) {
            /* Try #to_ary if the element responds to it (CRuby flattens
             * via #to_ary, not method_missing).  Park el at sp[1] across
             * the funcalls (it may be non-Array but still moving). */
            sp[1] = el;
            c->sp_top = sp + 2;
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, sp[1], korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            if (RTEST(rt)) {
                VALUE ar = UNWRAP(korb_funcall(c, c->sp_top, sp[1], korb_intern("to_ary"), 0, NULL));
                if (NIL_P(ar)) {
                    /* nil result: leave element as is. */
                } else if (SPECIAL_CONST_P(ar) || BUILTIN_TYPE(ar) != T_ARRAY) {
                    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                    return korb_raise(c, (struct korb_class *)eT,
                               "can't convert to Array (to_ary returned non-Array)");
                } else {
                    coerced = ar;
                    is_ary = true;
                }
            }
            el = sp[1];
            c->sp_top = sp + 1;
        }
        if (depth != 0 && is_ary) {
            if (ary_flatten_stack_contains(*stack, coerced)) {
                VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                return korb_raise(c, (struct korb_class *)eA, "tried to flatten recursive array");
            }
            /* Recurse: push coerced onto the shared cycle stack and into the
             * next level's source slot at sp[1].  result/stack slots are
             * unchanged (shared). */
            sp[1] = coerced;
            korb_ary_push(c, sp + 2, *stack, sp[1]);
            CHECK(ary_flatten_into(c, result, stack, sp + 1, depth - 1));
            (void)korb_ary_pop(*stack);   /* pop the cycle marker */
            c->sp_top = sp + 1;
        } else {
            korb_ary_push(c, sp + 1, *result, el);
        }
    }
    return RESULT_OK(Qnil);
}

static RESULT ary_flatten(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long depth = -1;
    if (argc >= 1 && !NIL_P(argv[0])) {
        VALUE d = argv[0];
        if (!FIXNUM_P(d)) {
            d = UNWRAP(korb_to_int_or_raise(c, d));
        }
        if (!FIXNUM_P(d)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
        }
        depth = FIX2LONG(d);
    }
    /* Park result (sp[0]), cycle stack (sp[1]) and the root source (sp[2])
     * so they survive flatten_into's GC points; result/stack slot addresses
     * are passed down and stay live for the whole recursion. */
    sp[0] = 0;
    sp[1] = 0;
    sp[2] = sp[-argc - 1];
    c->sp_top = sp + 3;
    sp[0] = korb_ary_new(c, sp + 3);
    sp[1] = korb_ary_new(c, sp + 3);
    /* Push self so the immediate `a << a` cycle is caught. */
    korb_ary_push(c, sp + 3, sp[1], sp[2]);
    CHECK(ary_flatten_into(c, &sp[0], &sp[1], &sp[2], depth));
    c->sp_top = sp;
    return RESULT_OK(sp[0]);
}

/* Array#flatten! — destructive: replace self with the flattened result.
 * Returns self if flattening changed anything, nil otherwise.  Raises
 * FrozenError unconditionally on a frozen receiver before doing any
 * argument coercion (CRuby semantic for the bang). */
static RESULT ary_flatten_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    long depth = -1;
    if (argc >= 1 && !NIL_P(argv[0])) {
        VALUE d = argv[0];
        if (!FIXNUM_P(d)) {
            d = UNWRAP(korb_to_int_or_raise(c, d));
        }
        if (!FIXNUM_P(d)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
        }
        depth = FIX2LONG(d);
    }
    /* Compute the flattened result in a fresh array, then check whether
     * it differs from self.  Replace self's storage on change.  Park
     * result (sp[0]), cycle stack (sp[1]) and the root source (sp[2]) so
     * they survive flatten_into's GC points; self stays live in the
     * receiver slot sp[-argc-1]. */
    sp[0] = 0;
    sp[1] = 0;
    sp[2] = sp[-argc - 1];
    c->sp_top = sp + 3;
    sp[0] = korb_ary_new(c, sp + 3);
    sp[1] = korb_ary_new(c, sp + 3);
    korb_ary_push(c, sp + 3, sp[1], sp[2]);
    CHECK(ary_flatten_into(c, &sp[0], &sp[1], &sp[2], depth));
    c->sp_top = sp;
    struct korb_array *me = (struct korb_array *)sp[-argc - 1];
    struct korb_array *fr = (struct korb_array *)sp[0];
    bool changed = (me->len != fr->len);
    if (!changed) {
        for (long i = 0; i < me->len; i++) {
            if (korb_ary_items(me)[i] != korb_ary_items(fr)[i]) { changed = true; break; }
        }
    }
    if (!changed) return RESULT_OK(Qnil);
    /* Adopt the new buffer: take over fr's backing payload object (and its
     * length).  Replaces the old `me->ptr = fr->ptr; me->capa = fr->capa`
     * buffer-steal — backing carries both the elements and the capacity. */
    me->backing = fr->backing;
    me->len = fr->len;
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT ary_compact(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: r grows in the loop (push GC point); park r at sp[0] and re-derive
     * the source from its GC slot each iteration. */
    long len = korb_ary_len(self);
    sp[0] = korb_ary_new(c, sp + 1);
    for (long i = 0; i < len; i++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        if (!NIL_P(korb_ary_items(a)[i])) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    return RESULT_OK(sp[0]);
}

/* Array#compact! — destructive: remove nil in place; return self if any
 * change, nil if no nil was removed (CRuby semantic for the bang). */
static RESULT ary_compact_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_array *a = (struct korb_array *)self;
    long w = 0;
    bool any = false;
    for (long r = 0; r < a->len; r++) {
        if (NIL_P(korb_ary_items(a)[r])) { any = true; continue; }
        if (w != r) korb_ary_items(a)[w] = korb_ary_items(a)[r];
        w++;
    }
    if (!any) return RESULT_OK(Qnil);
    a->len = w;
    return RESULT_OK(self);
}

static RESULT ary_uniq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long len = korb_ary_len(self);
    /* R5: korb_eql may call user #eql?/#hash (GC point) and push grows r;
     * park r at sp[0], re-derive source a and result ra from their GC slots. */
    sp[0] = korb_ary_new(c, sp + 1);
    /* CRuby Array#uniq uses eql? (with hash) — distinguishes 1 from 1.0
     * and uses the user-defined #hash + #eql? when present. */
    for (long i = 0; i < len; i++) {
        bool dup = false;
        /* Re-derive ra in the condition too: korb_eql below dispatches a user
         * #eql?/#hash (GC point) that moves the result array, so a cached
         * `ra` goes stale before the next `j < ra->len` check. */
        for (long j = 0; j < ((struct korb_array *)sp[0])->len; j++) {
            struct korb_array *a = (struct korb_array *)sp[-argc - 1];
            struct korb_array *ra = (struct korb_array *)sp[0];
            if (korb_ary_items(ra)[j] == korb_ary_items(a)[i]) { dup = true; break; }
            if (korb_eql(c, korb_ary_items(ra)[j], korb_ary_items(a)[i])) { dup = true; break; }
        }
        if (!dup) {
            struct korb_array *a = (struct korb_array *)sp[-argc - 1];
            korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
        }
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_include(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    /* R5: a held across korb_funcall(==) — re-derive from its slot each iter. */
    long alen = korb_ary_len(self);
    /* CRuby calls element == obj (left-to-right), letting user-defined
     * == on elements decide.  korb_eq does identity-shortcut + dispatches
     * to ==, but we want to dispatch on the ELEMENT's == (not obj's). */
    for (long i = 0; i < alen; i++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        if (korb_ary_items(a)[i] == argv[0]) return RESULT_OK(Qtrue);  /* identity fast path */
        VALUE r = UNWRAP(korb_funcall(c, c->sp_top, korb_ary_items(a)[i], korb_intern("=="), 1, &argv[0]));
        if (RTEST(r)) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

/* Predicates with optional pattern arg + optional block.
 *   any?           — any element truthy
 *   any?(pat)      — pat === elem for any element
 *   any? { blk }   — blk(elem) truthy for any element
 * If both pattern and block are given, CRuby uses the block (and warns).
 * Returns Qtrue/Qfalse. */
/* Result.value holds Qtrue / Qfalse; non-NORMAL state propagates raise. */
static RESULT ary_predicate_match(CTX *c, VALUE elem, int argc, VALUE *argv) {
    if (korb_block_given(c)) {
        VALUE r = UNWRAP(korb_yield(c, 1, &elem));
        return RESULT_OK(RTEST(r) ? Qtrue : Qfalse);
    }
    if (argc >= 1) {
        VALUE r = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("==="), 1, &elem));
        return RESULT_OK(RTEST(r) ? Qtrue : Qfalse);
    }
    return RESULT_OK(RTEST(elem) ? Qtrue : Qfalse);
}

static RESULT ary_predicate_argc_check(CTX *c, int argc) {
    if (argc > 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                          "wrong number of arguments (given %d, expected 0..1)", argc);
    }
    return RESULT_OK(Qnil);
}

/* Shared predicate driver for any?/all?/none?/one?.  Parks the receiver
 * (fr.last_match) and the optional `===` pattern (fr.last_line) in a
 * synthetic frame so they survive ary_predicate_match's yield/funcall GC
 * points (frame chain always scanned).  Returns the count of truthy
 * matches, short-circuiting once `stop_at` is reached (or scanning all if
 * stop_at < 0). */
static RESULT ary_predicate_count(CTX *c, int argc, VALUE *sp, long stop_at, long *out) {
    KORB_ARY_YIELD_FRAME(c, fr, argc >= 1 ? (sp - argc)[0] : Qnil);  /* pattern */
    fr.last_match = sp[-argc - 1];                                   /* receiver */
    long count = 0;
    /* Re-read length each step so a block may grow the array (CRuby). */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE elem = korb_ary_aref(fr.last_match, i);
        VALUE patv = fr.last_line;
        RESULT _m = ary_predicate_match(c, elem, argc, argc >= 1 ? &patv : NULL);
        if (_m.state != KORB_NORMAL) { c->current_frame = fr.prev; return _m; }
        if (RTEST(_m.value)) {
            count++;
            if (stop_at >= 0 && count >= stop_at) break;
        }
    }
    c->current_frame = fr.prev;
    *out = count;
    return RESULT_OK(Qnil);
}

static RESULT ary_any_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    CHECK(ary_predicate_argc_check(c, argc));
    long count = 0;
    CHECK(ary_predicate_count(c, argc, sp, 1, &count));
    return RESULT_OK(KORB_BOOL(count >= 1));
}

static RESULT ary_all_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    CHECK(ary_predicate_argc_check(c, argc));
    long alen = korb_ary_len(sp[-argc - 1]);
    long count = 0;
    CHECK(ary_predicate_count(c, argc, sp, -1, &count));
    return RESULT_OK(KORB_BOOL(count == alen));
}

static RESULT ary_none_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    CHECK(ary_predicate_argc_check(c, argc));
    long count = 0;
    CHECK(ary_predicate_count(c, argc, sp, 1, &count));
    return RESULT_OK(KORB_BOOL(count == 0));
}

static RESULT ary_one_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    CHECK(ary_predicate_argc_check(c, argc));
    long count = 0;
    CHECK(ary_predicate_count(c, argc, sp, 2, &count));
    return RESULT_OK(KORB_BOOL(count == 1));
}

static RESULT ary_min(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(Qnil);
    bool has_block = korb_block_given(c);
    /* CRuby min/max block convention: block.call(probe, running) — if it
     * returns < 0 the probe is smaller than the running min (so swap).
     * This is the opposite of sort's convention, which is also why the
     * cmp variable here is interpreted with the probe as the LHS. */
    /* Park self (fr.last_match) + running min (fr.last_line) in a synthetic
     * frame: with a block, ary_sort_compare yields and korb_yield lowers
     * c->sp_top below the receiver/accumulator sp slots, so a moving GC would
     * not forward them (sp[]-only parking went stale → SEGV in range/max).
     * The frame chain is always scanned regardless of sp. */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_items((struct korb_array *)sp[-argc - 1])[0]);
    fr.last_match = sp[-argc - 1];
    RESULT _ret = RESULT_OK(Qnil);
    for (long i = 1; i < alen; i++) {
        VALUE probe = korb_ary_items((struct korb_array *)fr.last_match)[i];
        long cmp = ary_sort_compare(c, probe, fr.last_line, has_block, &_ret);
        if (_ret.state != KORB_NORMAL) { c->current_frame = fr.prev; return _ret; }
        if (cmp < 0) fr.last_line = korb_ary_items((struct korb_array *)fr.last_match)[i];
    }
    VALUE r = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(r);
}

static RESULT ary_max(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(Qnil);
    bool has_block = korb_block_given(c);
    /* Park self (fr.last_match) + running max (fr.last_line) in a synthetic
     * frame: with a block, ary_sort_compare yields and korb_yield lowers
     * c->sp_top below the receiver/accumulator sp slots, so a moving GC would
     * not forward them (sp[]-only parking went stale → SEGV in range/max).
     * The frame chain is always scanned regardless of sp. */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_items((struct korb_array *)sp[-argc - 1])[0]);
    fr.last_match = sp[-argc - 1];
    RESULT _ret = RESULT_OK(Qnil);
    for (long i = 1; i < alen; i++) {
        VALUE probe = korb_ary_items((struct korb_array *)fr.last_match)[i];
        long cmp = ary_sort_compare(c, probe, fr.last_line, has_block, &_ret);
        if (_ret.state != KORB_NORMAL) { c->current_frame = fr.prev; return _ret; }
        if (cmp > 0) fr.last_line = korb_ary_items((struct korb_array *)fr.last_match)[i];
    }
    VALUE r = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(r);
}

static RESULT ary_sum(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park the accumulator (fr.last_line; may be a Bignum/Float heap object)
     * and the source receiver (fr.last_match) across the korb_yield /
     * korb_funcall GC points (frame chain always scanned). */
    KORB_ARY_YIELD_FRAME(c, fr, argc > 0 ? argv[0] : INT2FIX(0));
    fr.last_match = sp[-argc - 1];
    bool has_block = korb_block_given(c);
    /* CRuby applies Kahan compensated summation when the running sum
     * becomes a Float.  Maintain a side compensation value. */
    bool kahan_active = false;
    double kahan_sum = 0.0, kahan_c = 0.0;
    if (FLONUM_P(fr.last_line) || (!SPECIAL_CONST_P(fr.last_line) && BUILTIN_TYPE(fr.last_line) == T_FLOAT)) {
        kahan_active = true;
        kahan_sum = korb_num2dbl(fr.last_line);
    }
    /* Re-read length each step: with a block, CRuby lets it grow the array
     * (shared "tolerates increasing size" spec). */
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE elt = korb_ary_aref(fr.last_match, i);
        if (has_block) {
            RESULT _y = korb_yield(c, 1, &elt);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            elt = _y.value;
        }
        bool elt_float = FLONUM_P(elt) ||
            (!SPECIAL_CONST_P(elt) && BUILTIN_TYPE(elt) == T_FLOAT);
        if (kahan_active) {
            double v = elt_float ? korb_num2dbl(elt) :
                       (FIXNUM_P(elt) ? (double)FIX2LONG(elt) : korb_num2dbl(elt));
            /* Kahan: y = v - c; t = sum + y; c = (t - sum) - y; sum = t */
            double y = v - kahan_c;
            double t = kahan_sum + y;
            kahan_c = (t - kahan_sum) - y;
            kahan_sum = t;
            continue;
        }
        if (elt_float) {
            kahan_active = true;
            kahan_sum = (FIXNUM_P(fr.last_line) ? (double)FIX2LONG(fr.last_line) : korb_num2dbl(fr.last_line))
                        + korb_num2dbl(elt);
            kahan_c = 0.0;
            continue;
        }
        if (FIXNUM_P(fr.last_line) && FIXNUM_P(elt)) {
            long s;
            if (!__builtin_add_overflow(FIX2LONG(fr.last_line), FIX2LONG(elt), &s) && FIXABLE(s))
                fr.last_line = INT2FIX(s);
            else fr.last_line = korb_int_plus(fr.last_line, elt);
        } else {
            RESULT _p = korb_funcall(c, c->sp_top, fr.last_line, korb_intern("+"), 1, &elt);
            if (_p.state != KORB_NORMAL) { c->current_frame = fr.prev; return _p; }
            fr.last_line = _p.value;
        }
    }
    if (kahan_active) { VALUE f = korb_float_new(c, c->sp_top, kahan_sum); c->current_frame = fr.prev; return RESULT_OK(f); }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT ary_each_slice(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(Qnil);
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return RESULT_OK(Qnil);
    long alen = korb_ary_len(self);
    bool has_block = korb_block_given(c);
    /* Park collected (fr.last_line) + source receiver (fr.last_match) in a
     * synthetic frame; the current slice is built at sp[0] (reserved across
     * its push-grow, then yielded / pushed). */
    KORB_ARY_YIELD_FRAME(c, fr, has_block ? Qnil : korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < alen; i += n) {
        long end = i + n; if (end > alen) end = alen;
        sp[0] = korb_ary_new_capa(c, sp + 1, end - i);
        c->sp_top = sp + 1;
        for (long j = i; j < end; j++) {
            korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(fr.last_match, j));
        }
        if (has_block) {
            RESULT _y = korb_yield(c, 1, &sp[0]);
            if (_y.state != KORB_NORMAL) { c->sp_top = sp; c->current_frame = fr.prev; return _y; }
        } else {
            korb_ary_push(c, sp + 1, fr.last_line, sp[0]);
        }
        c->sp_top = sp;
    }
    VALUE ret = has_block ? sp[-argc - 1] : fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(ret);
}

static RESULT ary_step(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* not real Array#step, but stub */
    return RESULT_OK(self);
}

static RESULT ary_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_ARRAY && korb_eq(c, self, argv[0])));
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

static RESULT ary_pack(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    const char *fmt = korb_str_cstr(argv[0]);
    long fmt_len = (long)strlen(fmt);
    /* Build into a growable libc buffer (bytes only — GC-safe).  R5: the
     * source Array `a` is held across korb_str_new (the 'a'/'A'/'Z' empty-pad
     * case), so re-derive it from its GC slot at the top of each format step.
     * Source element strings are libc-backed (non-moving), so their ptr is
     * stable. */
    struct korb_array *a = (struct korb_array *)self;
    long cap = 32, plen = 0;
    char *buf = korb_xmalloc_atomic(cap);
    long src_idx = 0;
    #define PACK_RESERVE(extra) do { \
        while (plen + (extra) > cap) { cap *= 2; buf = korb_xrealloc(buf, cap); } \
    } while (0)
    long fp = 0;
    while (fp < fmt_len) {
        a = (struct korb_array *)sp[-argc - 1];
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
                long v = (src_idx < a->len) ? korb_pack_long(korb_ary_items(a)[src_idx++]) : 0;
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
                long v = (src_idx < a->len) ? korb_pack_long(korb_ary_items(a)[src_idx++]) : 0;
                PACK_RESERVE(2); korb_pack_int_bytes(buf, plen, v, 2, big); plen += 2;
            }
            break;
          }
          case 'N': case 'V': case 'l': case 'L': case 'i': case 'I': {
            long n = star ? (a->len - src_idx) : count;
            int big = (d == 'N');  /* V/l/L/i/I native LE */
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(korb_ary_items(a)[src_idx++]) : 0;
                PACK_RESERVE(4); korb_pack_int_bytes(buf, plen, v, 4, big); plen += 4;
            }
            break;
          }
          case 'q': case 'Q': case 'j': case 'J': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                long v = (src_idx < a->len) ? korb_pack_long(korb_ary_items(a)[src_idx++]) : 0;
                PACK_RESERVE(8); korb_pack_int_bytes(buf, plen, v, 8, 0); plen += 8;
            }
            break;
          }
          case 'a': case 'A': case 'Z': {
            VALUE sv = (src_idx < a->len) ? korb_ary_items(a)[src_idx++] : korb_str_new(c, c->sp_top, "", 0);
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
            VALUE sv = (src_idx < a->len) ? korb_ary_items(a)[src_idx++] : korb_str_new(c, c->sp_top, "", 0);
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
                double v = (src_idx < a->len) ? korb_pack_double(korb_ary_items(a)[src_idx++]) : 0.0;
                PACK_RESERVE(8); memcpy(buf + plen, &v, 8); plen += 8;
            }
            break;
          }
          case 'f': case 'F': case 'e': case 'g': {
            long n = star ? (a->len - src_idx) : count;
            for (long i = 0; i < n; i++) {
                float v = (src_idx < a->len) ? (float)korb_pack_double(korb_ary_items(a)[src_idx++]) : 0.0f;
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
    return RESULT_OK(korb_str_new(c, c->sp_top, buf, plen));
}

static RESULT str_unpack(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: park the result array at sp[0] — it is held across the push grows
     * and korb_str_new / korb_float_new GC points below; staging happens at
     * sp+1.  The string HANDLE is moving, so re-read self from its slot after
     * korb_ary_new / korb_str_cstr; the byte buffer s->ptr is libc-stable
     * once read, so `src` stays valid for the whole loop. */
    sp[0] = korb_ary_new(c, sp + 1);
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(sp[0]);
    const char *fmt = korb_str_cstr(argv[0]);
    long fmt_len = (long)strlen(fmt);
    self = sp[-argc - 1];   /* re-read: moving handle stale across the allocs */
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
                korb_ary_push(c, sp + 1, sp[0], INT2FIX(b));
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
                korb_ary_push(c, sp + 1, sp[0], INT2FIX(v));
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
                korb_ary_push(c, sp + 1, sp[0], INT2FIX(v));
            }
            break;
          }
          case 'q': case 'Q': case 'j': case 'J': {
            long n = star ? ((src_len - src_idx) / 8) : count;
            for (long i = 0; i < n && src_idx + 8 <= src_len; i++) {
                long v = 0;
                for (int b = 0; b < 8; b++) v |= ((long)src[src_idx + b]) << (b * 8);
                src_idx += 8;
                korb_ary_push(c, sp + 1, sp[0], INT2FIX(v));
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
            korb_ary_push(c, sp + 1, sp[0], korb_str_new(c, sp + 1, (const char *)(src + src_idx), real));
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
            korb_ary_push(c, sp + 1, sp[0], korb_str_new(c, sp + 1, out, o));
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
                korb_ary_push(c, sp + 1, sp[0], korb_float_new(c, sp + 1, v));
            }
            break;
          }
          case 'f': case 'F': case 'e': case 'g': {
            long n = star ? ((src_len - src_idx) / 4) : count;
            for (long i = 0; i < n && src_idx + 4 <= src_len; i++) {
                float v;
                memcpy(&v, src + src_idx, 4);
                src_idx += 4;
                korb_ary_push(c, sp + 1, sp[0], korb_float_new(c, sp + 1, (double)v));
            }
            break;
          }
          case ' ': case '\t': case '\n':
            break;
          default:
            break;
        }
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_concat(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc == 0) return RESULT_OK(self);
    /* Coerce each arg to an Array (CRuby #to_ary semantics) and park the
     * coerced sources on the value stack at sp[0..argc).  Parking (rather
     * than libc shadow buffers) keeps the source handles scanned + forwarded
     * across both the #to_ary GC points and the push-grow GC points below —
     * including the `ary.concat(ary)` self-alias case, where sp[i] holds the
     * receiver and is forwarded right alongside sp[-argc-1].  Per-arg source
     * lengths are snapshot before any push so a self-alias only copies the
     * original elements (CRuby behaviour). */
    long lens[argc];
    for (int i = 0; i < argc; i++) sp[i] = Qnil;   /* zero-fill before publishing */
    c->sp_top = sp + argc;
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_ARRAY) {
            RESULT tr = korb_funcall_r(c, c->sp_top, arg, korb_intern("to_ary"), 0, NULL);
            if (tr.state != KORB_NORMAL) { c->sp_top = sp; return tr; }
            arg = tr.value;
            if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_ARRAY) { lens[i] = 0; continue; }
        }
        sp[i] = arg;
        lens[i] = korb_ary_len(arg);
    }
    /* Push to the re-read receiver, re-deriving each source from its parked
     * slot every iteration (the push-grow GC moves both). */
    for (int i = 0; i < argc; i++) {
        for (long j = 0; j < lens[i]; j++) {
            const struct korb_array *o = (const struct korb_array *)sp[i];
            korb_ary_push(c, c->sp_top, sp[-argc - 1], korb_ary_items(o)[j]);
        }
    }
    c->sp_top = sp;
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#+ — non-destructive concat (CRuby semantics).  Coerces the
 * argument via #to_ary if not already an Array. */
RESULT ary_plus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(self);
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
        if (!SPECIAL_CONST_P(other)) {
            /* Use respond_to? so mock objects (method_missing) and
             * plain instances both go through #to_ary.  Whatever
             * to_ary raises (NoMethodError, RuntimeError, etc.)
             * propagates up unchanged — only the "to_ary returned
             * non-Array" case yields TypeError. */
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            if (RTEST(rt)) {
                other = UNWRAP(korb_funcall(c, c->sp_top, other, korb_intern("to_ary"), 0, NULL));
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Array",
                       korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    /* R5: result pre-sized (push won't grow); park it at sp[1] and the coerced
     * `other` at sp[0], then re-derive l/r after the korb_ary_new_capa GC.
     * Re-read self for its length: the #to_ary coercion above is a GC point
     * that leaves the `self` C-local stale. */
    sp[0] = other;
    long llen = korb_ary_len(sp[-argc - 1]), rlen = korb_ary_len(other);
    sp[1] = korb_ary_new_capa(c, sp + 2, llen + rlen);
    {
        struct korb_array *l = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < llen; i++) korb_ary_push(c, sp + 2, sp[1], korb_ary_items(l)[i]);
        struct korb_array *r = (struct korb_array *)sp[0];
        for (long i = 0; i < rlen; i++) korb_ary_push(c, sp + 2, sp[1], korb_ary_items(r)[i]);
    }
    return RESULT_OK(sp[1]);
}

static RESULT ary_minus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_ARRAY) return RESULT_OK(korb_ary_new(c, c->sp_top));
    long alen = korb_ary_len(self);
    /* R5: korb_eq may dispatch user #== (GC point) and push grows r; park r at
     * sp[0] and re-derive a (slot) and b (argv[0] slot) each iteration. */
    sp[0] = korb_ary_new(c, sp + 1);
    for (long i = 0; i < alen; i++) {
        bool found = false;
        struct korb_array *b = (struct korb_array *)argv[0];
        for (long j = 0; j < b->len; j++) {
            struct korb_array *a = (struct korb_array *)sp[-argc - 1];
            b = (struct korb_array *)argv[0];
            if (korb_eq(c, korb_ary_items(a)[i], korb_ary_items(b)[j])) { found = true; break; }
        }
        if (!found) {
            struct korb_array *a = (struct korb_array *)sp[-argc - 1];
            korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
        }
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_index(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);

    if (argc < 1 && c->current_block) {
        /* Park the receiver (fr.last_match) across the per-element yield. */
        KORB_ARY_YIELD_FRAME(c, fr, Qnil);
        fr.last_match = sp[-argc - 1];
        /* Re-read length each step (block may grow the array, CRuby). */
        for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
            VALUE v = korb_ary_aref(fr.last_match, i);
            RESULT _y = korb_yield(c, 1, &v);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            if (!NIL_P(_y.value) && _y.value != Qfalse) { c->current_frame = fr.prev; return RESULT_OK(INT2FIX(i)); }
        }
        c->current_frame = fr.prev;
        return RESULT_OK(Qnil);
    }
    if (argc < 1) {
        /* No block, no arg: return Enumerator. */
        VALUE method_sym = korb_id2sym(korb_intern("index"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    /* Arg form: park receiver (fr.last_match) + the target (fr.last_line)
     * across the per-element #== funcall. */
    KORB_ARY_YIELD_FRAME(c, fr, (sp - argc)[0]);
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < alen; i++) {
        VALUE elt = korb_ary_aref(fr.last_match, i);
        if (elt == fr.last_line) { c->current_frame = fr.prev; return RESULT_OK(INT2FIX(i)); }
        RESULT er = korb_funcall_r(c, c->sp_top, elt, korb_intern("=="), 1, &fr.last_line);
        if (er.state != KORB_NORMAL) { c->current_frame = fr.prev; return er; }
        if (RTEST(er.value)) { c->current_frame = fr.prev; return RESULT_OK(INT2FIX(i)); }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(Qnil);
}

static RESULT ary_reverse(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: pre-sized; park result and re-derive source after the new_capa GC. */
    long alen = korb_ary_len(self);
    sp[0] = korb_ary_new_capa(c, sp + 1, alen);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = alen - 1; i >= 0; i--) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_rotate_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (korb_ary_len(self) <= 1) return RESULT_OK(self);
    long n;
    if (argc >= 1) {
        VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
        if (!FIXNUM_P(iv)) return RESULT_OK(self);
        n = FIX2LONG(iv);
    } else {
        n = 1;
    }
    /* R5: re-derive self/a after the korb_to_int_or_raise GC point (the
     * `return RESULT_OK(self)` sites below would otherwise hand back a
     * stale handle). */
    self = sp[-argc - 1];
    struct korb_array *a = (struct korb_array *)sp[-argc - 1];
    long len = a->len;
    n = n % len;
    if (n < 0) n += len;
    if (n == 0) return RESULT_OK(self);
    /* Half rotate: swap halves directly — covers the optcarrot hot path
     * `@bg_pixels.rotate!(8)` where @bg_pixels has 16 elements (rotate by
     * half).  Memcpy through a stack buffer is one fewer pass than the
     * 3-reverse trick. */
    if (n + n == len && len <= 64) {
        VALUE tmp[32];
        long half = n;
        memcpy(tmp,        korb_ary_items(a),        half * sizeof(VALUE));
        memcpy(korb_ary_items(a),     korb_ary_items(a) + half, half * sizeof(VALUE));
        memcpy(korb_ary_items(a) + half, tmp,        half * sizeof(VALUE));
        return RESULT_OK(self);
    }
    /* General rotate left by n: 3-reverse trick (no extra alloc, GC safe). */
    /* reverse [0..n-1] */
    for (long i = 0, j = n - 1; i < j; i++, j--) { VALUE t = korb_ary_items(a)[i]; korb_ary_items(a)[i] = korb_ary_items(a)[j]; korb_ary_items(a)[j] = t; }
    /* reverse [n..len-1] */
    for (long i = n, j = len - 1; i < j; i++, j--) { VALUE t = korb_ary_items(a)[i]; korb_ary_items(a)[i] = korb_ary_items(a)[j]; korb_ary_items(a)[j] = t; }
    /* reverse [0..len-1] */
    for (long i = 0, j = len - 1; i < j; i++, j--) { VALUE t = korb_ary_items(a)[i]; korb_ary_items(a)[i] = korb_ary_items(a)[j]; korb_ary_items(a)[j] = t; }
    return RESULT_OK(self);
}

static RESULT ary_rotate(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long n;
    if (argc >= 1) {
        VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
        if (!FIXNUM_P(iv)) return RESULT_OK(korb_ary_new(c, c->sp_top));
        n = FIX2LONG(iv);
    } else {
        n = 1;
    }
    /* R5: re-derive self/a after korb_to_int_or_raise (a GC point); result
     * is pre-sized. */
    self = sp[-argc - 1];
    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(korb_ary_new(c, c->sp_top));
    n = n % alen;
    if (n < 0) n += alen;
    sp[0] = korb_ary_new_capa(c, sp + 1, alen);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[(i + n) % alen]);
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_reverse_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0, j = a->len - 1; i < j; i++, j--) {
        VALUE t = korb_ary_items(a)[i]; korb_ary_items(a)[i] = korb_ary_items(a)[j]; korb_ary_items(a)[j] = t;
    }
    return RESULT_OK(self);
}

static RESULT ary_clear(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 0) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0)", argc);
    }
    CHECK_FROZEN_R(c, self);
    ((struct korb_array *)self)->len = 0;
    return RESULT_OK(self);
}

static RESULT ary_unshift(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    /* shift right argc times.  R5: the nil-padding pushes can grow self (GC),
     * so re-derive a from its slot afterwards before the in-place shuffle. */
    long oldlen = korb_ary_len(self);
    /* Push to the re-read handle: the `self` C-local goes stale once a
     * prior push grows the array (moving GC). */
    for (int i = 0; i < argc; i++) korb_ary_push(c, c->sp_top, sp[-argc - 1], Qnil);
    struct korb_array *a = (struct korb_array *)sp[-argc - 1];
    for (long i = oldlen - 1; i >= 0; i--) korb_ary_items(a)[i + argc] = korb_ary_items(a)[i];
    for (int i = 0; i < argc; i++) korb_ary_items(a)[i] = argv[i];
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT ary_shift(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc > 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", argc);
    }
    struct korb_array *a = (struct korb_array *)self;
    if (argc >= 1) {
        VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
        if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);
        long n = FIX2LONG(iv);
        if (n < 0) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg, "negative array size");
        }
        /* R5: re-derive self/a after korb_to_int_or_raise (a GC point);
         * result pre-sized so the copy-out pushes won't grow. */
        self = sp[-argc - 1];
        long alen = korb_ary_len(self);
        long take = n > alen ? alen : n;
        sp[0] = korb_ary_new_capa(c, sp + 1, take);
        a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < take; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
        for (long i = 0; i + take < a->len; i++) korb_ary_items(a)[i] = korb_ary_items(a)[i + take];
        a->len -= take;
        return RESULT_OK(sp[0]);
    }
    if (a->len == 0) return RESULT_OK(Qnil);
    VALUE v = korb_ary_items(a)[0];
    for (long i = 0; i + 1 < a->len; i++) korb_ary_items(a)[i] = korb_ary_items(a)[i+1];
    a->len--;
    return RESULT_OK(v);
}

static RESULT ary_transpose(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (korb_ary_len(self) == 0) return RESULT_OK(korb_ary_new(c, c->sp_top));
    long n_outer = korb_ary_len(self);
    /* Normalize each inner element via to_ary if it isn't already an Array
     * (CRuby semantics).  The coerced inner-array handles move under STRESS,
     * so collect them into a parked Array at sp[0] (re-read by index), the
     * result at sp[1] and the current row at sp[2].  self stays in the
     * receiver slot sp[-argc-1]. */
    sp[0] = 0;
    sp[1] = 0;
    sp[2] = 0;
    c->sp_top = sp + 3;
    sp[0] = korb_ary_new_capa(c, sp + 3, n_outer);
    for (long j = 0; j < n_outer; j++) {
        VALUE inner = korb_ary_aref(sp[-argc - 1], j);
        if (!SPECIAL_CONST_P(inner) && BUILTIN_TYPE(inner) == T_ARRAY) {
            korb_ary_push(c, sp + 3, sp[0], inner);
            continue;
        }
        if (!SPECIAL_CONST_P(inner)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, inner, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            if (RTEST(rt)) {
                VALUE r = UNWRAP(korb_funcall(c, c->sp_top, inner, korb_intern("to_ary"), 0, NULL));
                if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_ARRAY) {
                    korb_ary_push(c, sp + 3, sp[0], r);
                    continue;
                }
            }
        }
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into Array",
                   SPECIAL_CONST_P(inner) ? "(special)"
                       : korb_id_name(korb_class_of_class(inner)->name));
    }
    long n_inner = korb_ary_len(korb_ary_aref(sp[0], 0));
    /* Verify all rows have the same length. */
    for (long j = 1; j < n_outer; j++) {
        if (korb_ary_len(korb_ary_aref(sp[0], j)) != n_inner) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE,
                       "element size differs (%ld should be %ld)",
                       korb_ary_len(korb_ary_aref(sp[0], j)), n_inner);
        }
    }
    sp[1] = korb_ary_new_capa(c, sp + 3, n_inner);
    for (long i = 0; i < n_inner; i++) {
        sp[2] = korb_ary_new_capa(c, sp + 3, n_outer);
        for (long j = 0; j < n_outer; j++) {
            VALUE inner = korb_ary_aref(sp[0], j);
            korb_ary_push(c, sp + 3, sp[2], korb_ary_aref(inner, i));
        }
        korb_ary_push(c, sp + 3, sp[1], sp[2]);
    }
    c->sp_top = sp;
    return RESULT_OK(sp[1]);
}

static RESULT ary_count(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: a is held across korb_yield / korb_eq(user #==) GC points — re-derive
     * from its slot each iteration. */
    long alen = korb_ary_len(self);

    if (argc == 0 && c->current_block) {
        /* Park the receiver (fr.last_match) across the per-element yield. */
        long n = 0;
        KORB_ARY_YIELD_FRAME(c, fr, Qnil);
        fr.last_match = sp[-argc - 1];
        /* CRuby re-reads the length each step so the block may grow the array
         * (shared "tolerates increasing size during iteration" spec). */
        for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
            VALUE v = korb_ary_aref(fr.last_match, i);
            RESULT _y = korb_yield(c, 1, &v);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            if (!NIL_P(_y.value) && _y.value != Qfalse) n++;
        }
        c->current_frame = fr.prev;
        return RESULT_OK(INT2FIX(n));
    }
    if (argc == 0) return RESULT_OK(INT2FIX(alen));
    /* Arg form: korb_eq may dispatch a user #== (GC point).  Park receiver
     * (fr.last_match) and the compared value (fr.last_line). */
    long n = 0;
    KORB_ARY_YIELD_FRAME(c, fr, (sp - argc)[0]);
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < alen; i++) {
        if (korb_eq(c, korb_ary_aref(fr.last_match, i), fr.last_line)) n++;
    }
    c->current_frame = fr.prev;
    return RESULT_OK(INT2FIX(n));
}

static RESULT ary_drop(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(self);
    VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
    /* korb_to_int_or_raise is a GC point — re-read self. */
    self = sp[-argc - 1];
    if (!FIXNUM_P(iv)) return RESULT_OK(self);
    long n = FIX2LONG(iv);
    if (n < 0) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "attempt to drop negative size");
    }
    long alen = korb_ary_len(self);
    if (n > alen) n = alen;
    /* R5: pre-sized; park result and re-derive source after the new_capa GC. */
    sp[0] = korb_ary_new_capa(c, sp + 1, alen - n);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = n; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_take(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "attempt to take negative size");
    }
    long alen = korb_ary_len(self);
    if (n > alen) n = alen;
    /* R5: pre-sized; park result and re-derive source after the new_capa GC. */
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_fill(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_array *a = (struct korb_array *)self;
    bool has_block = korb_block_given(c);
    /* With a block, signature is fill { |i| ... } / fill(start) / fill(start, len).
     * Without a block, fill(val[, start[, length]]) or fill(val, range). */
    long start = 0, len = a->len;
    int idx_arg_base = has_block ? 0 : 1;
    if (!has_block && argc < 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 0, expected 1..3)");
    }
    /* Maximum argc: with block 0..2 (start, length); without block 1..3
     * (val, start, length).  More than that → ArgumentError. */
    int max_argc = has_block ? 2 : 3;
    if (argc > max_argc) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected %d..%d)",
                   argc, has_block ? 0 : 1, max_argc);
    }

    /* Range form: fill[, range] / fill(val, range) (no block: idx_arg_base=1;
     * with block: idx_arg_base=0).  Also: if no idx args at all, len is
     * computed below and idx defaults to 0. */
    if (argc == idx_arg_base + 3) {
        /* spec: fill(val, range, len) — never valid; raise TypeError. */
        if (!SPECIAL_CONST_P(argv[idx_arg_base]) &&
            BUILTIN_TYPE(argv[idx_arg_base]) == T_RANGE) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of Range into Integer");
        }
    }
    if (argc >= idx_arg_base + 1 && !SPECIAL_CONST_P(argv[idx_arg_base]) &&
        BUILTIN_TYPE(argv[idx_arg_base]) == T_RANGE) {
        /* The range and its endpoints are moving handles, and the to_int
         * coercions below are GC points; a cached `struct korb_range *r`
         * goes stale across them.  Always re-read the range (and its
         * begin/end) from argv[idx_arg_base] (scanned + forwarded by
         * visit_roots) at each use. */
        #define RNG ((struct korb_range *)argv[idx_arg_base])
        long b, e;
        bool excl;
        /* Coerce begin via to_int. */
        VALUE rbeg = RNG->begin;
        if (NIL_P(rbeg)) {
            b = 0;
        } else if (FIXNUM_P(rbeg)) {
            b = FIX2LONG(rbeg);
        } else if (!SPECIAL_CONST_P(rbeg)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, rbeg, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (!RTEST(rt)) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion into Integer");
            }
            VALUE iv = UNWRAP(korb_funcall(c, c->sp_top, RNG->begin, korb_intern("to_int"), 0, NULL));
            if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);
            b = FIX2LONG(iv);
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
        }
        VALUE rend = RNG->end;
        excl = RNG->exclude_end;
        a = (struct korb_array *)sp[-argc - 1];  /* re-derive after begin coercion GC */
        if (NIL_P(rend)) {
            e = a->len - 1;
        } else if (FIXNUM_P(rend)) {
            e = FIX2LONG(rend);
            if (excl) e -= 1;
        } else if (!SPECIAL_CONST_P(rend)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, rend, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (!RTEST(rt)) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion into Integer");
            }
            VALUE iv = UNWRAP(korb_funcall(c, c->sp_top, RNG->end, korb_intern("to_int"), 0, NULL));
            if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);
            e = FIX2LONG(iv);
            if (RNG->exclude_end) e -= 1;
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "no implicit conversion into Integer");
        }
        #undef RNG
        a = (struct korb_array *)sp[-argc - 1];  /* re-derive after end coercion GC */
        long orig_b = b;
        if (b < 0) b += a->len;
        if (e < 0) e += a->len;
        if (b < 0) {
            /* CRuby raises RangeError when the range starts before the
             * array's first index. */
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR,
                       "%ld out of range", orig_b);
        }
        if (e >= a->len) {
            /* Grow array to accommodate. */
            if (e > (1L << 30)) {
                VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
                return korb_raise(c, (struct korb_class *)eR, "range too large");
            }
            /* R5: push grows self (GC) — re-derive a each iteration, and
             * push to the re-read handle (the `self` C-local is stale once
             * a prior push moved the array). */
            while ((a = (struct korb_array *)sp[-argc - 1])->len <= e)
                korb_ary_push(c, c->sp_top, sp[-argc - 1], Qnil);
        }
        start = b;
        len = e - b + 1;
    } else if (argc >= idx_arg_base + 1 && FIXNUM_P(argv[idx_arg_base])) {
        start = FIX2LONG(argv[idx_arg_base]);
        if (start < 0) start += a->len;
        if (start < 0) start = 0;
        if (argc >= idx_arg_base + 2 && FIXNUM_P(argv[idx_arg_base + 1])) {
            len = FIX2LONG(argv[idx_arg_base + 1]);
            if (len < 0) return RESULT_OK(self);
        } else if (argc >= idx_arg_base + 2 && !NIL_P(argv[idx_arg_base + 1]) &&
                   !FIXNUM_P(argv[idx_arg_base + 1])) {
            /* Non-Fixnum, non-nil length: e.g. Bignum → RangeError. */
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR, "length out of range");
        } else {
            len = a->len - start;
        }
        /* (start, len) form may also grow the array (CRuby does). */
        if (start + len > a->len) {
            /* Cap growth to avoid OOM when given absurd lengths. */
            if (len > (1L << 30)) {
                VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
                return korb_raise(c, (struct korb_class *)eR, "length too large");
            }
            /* R5: push grows self (GC) — re-derive a each iteration, and
             * push to the re-read handle (the `self` C-local is stale once
             * a prior push moved the array). */
            while ((a = (struct korb_array *)sp[-argc - 1])->len < start + len)
                korb_ary_push(c, c->sp_top, sp[-argc - 1], Qnil);
        }
    }
    /* R5: re-derive a after any growth above. */
    a = (struct korb_array *)sp[-argc - 1];
    /* Re-read self too: the growth push loops / block yields above are GC
     * points, so the original `self` C-local is a stale (moved) handle.
     * Returning it would hand a dead pointer to the caller's next dispatch
     * (korb_class_of_class SEGV under STRESS+PURGE, e.g. fill(...).should). */
    if (start >= a->len) return RESULT_OK(sp[-argc - 1]);
    long end = start + len;
    if (end > a->len) end = a->len;
    if (has_block) {
        for (long i = start; i < end; i++) {
            VALUE iv = INT2FIX(i);
            VALUE r = UNWRAP(korb_yield(c, 1, &iv));
            /* R5: re-derive a after the yield GC point. */
            a = (struct korb_array *)sp[-argc - 1];
            korb_ary_items(a)[i] = r;
        }
    } else {
        for (long i = start; i < end; i++) korb_ary_items(a)[i] = argv[0];
    }
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT ary_sample(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(argc >= 1 ? korb_ary_new(c, c->sp_top) : Qnil);
    /* sample (no arg) → one random element.
     * sample(n) → fresh Array of n random elements without replacement. */
    if (argc < 1) {
        const struct korb_array *a = (const struct korb_array *)self;
        return RESULT_OK(korb_ary_items(a)[rand() % alen]);
    }
    if (!FIXNUM_P(argv[0])) return RESULT_OK(Qnil);
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return RESULT_OK(korb_ary_new(c, c->sp_top));
    /* R5: the copy is pre-sized (no grow), but korb_ary_new_capa is a GC point —
     * park the copy at sp[0] and re-derive the source from its slot.  The
     * Fisher-Yates shuffle uses only rand() (no GC), so `out`/`tmp` stay valid. */
    if (n >= alen) {
        sp[0] = korb_ary_new_capa(c, sp + 1, alen);
        {
            const struct korb_array *a = (const struct korb_array *)sp[-argc - 1];
            for (long i = 0; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
        }
        struct korb_array *out = (struct korb_array *)sp[0];
        for (long i = out->len - 1; i > 0; i--) {
            long j = rand() % (i + 1);
            VALUE t = korb_ary_items(out)[i]; korb_ary_items(out)[i] = korb_ary_items(out)[j]; korb_ary_items(out)[j] = t;
        }
        return RESULT_OK(sp[0]);
    }
    sp[0] = korb_ary_new_capa(c, sp + 1, alen);
    {
        const struct korb_array *a = (const struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    {
        struct korb_array *tmp = (struct korb_array *)sp[0];
        for (long i = 0; i < n; i++) {
            long j = i + (rand() % (tmp->len - i));
            VALUE t = korb_ary_items(tmp)[i]; korb_ary_items(tmp)[i] = korb_ary_items(tmp)[j]; korb_ary_items(tmp)[j] = t;
        }
    }
    /* second result parked at sp[1]; source pairs array stays at sp[0]. */
    sp[1] = korb_ary_new_capa(c, sp + 2, n);
    {
        struct korb_array *tmp = (struct korb_array *)sp[0];
        for (long i = 0; i < n; i++) korb_ary_push(c, sp + 2, sp[1], korb_ary_items(tmp)[i]);
    }
    return RESULT_OK(sp[1]);
}

static RESULT ary_empty_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(((struct korb_array *)self)->len == 0));
}

static RESULT ary_find(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park the receiver (fr.last_match) across the per-element korb_yield. */
    long alen = korb_ary_len(self);
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < alen; i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (RTEST(_y.value)) { VALUE r = korb_ary_aref(fr.last_match, i); c->current_frame = fr.prev; return RESULT_OK(r); }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(Qnil);
}

static RESULT ary_min_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(Qnil);
    /* Three roots span the korb_yield / <=> funcall GC points: the running
     * min element `m`, its yielded key `mk`, and the source receiver.  Park
     * them in two chained synthetic frames (frame chain always scanned):
     *   fm.last_line = m, fm.last_match = mk
     *   fr.last_line = receiver
     * The per-iteration candidate v / its key k are transient (re-read /
     * snapshot at the funcall call site) and live in sp slots. */
    KORB_ARY_YIELD_FRAME(c, fm, korb_ary_aref(sp[-argc - 1], 0));   /* m */
    KORB_ARY_YIELD_FRAME(c, fr, sp[-argc - 1]);                     /* receiver */
    RESULT _y0 = korb_yield(c, 1, &fm.last_line);
    if (_y0.state != KORB_NORMAL) { c->current_frame = fm.prev; return _y0; }
    fm.last_match = _y0.value;     /* mk */
    for (long i = 1; i < alen; i++) {
        VALUE v = korb_ary_aref(fr.last_line, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fm.prev; return _y; }
        fr.last_match = _y.value;   /* k: candidate key, park across funcall */
        RESULT _cmp = korb_funcall(c, c->sp_top, fm.last_match, korb_intern("<=>"), 1, &fr.last_match);
        if (_cmp.state != KORB_NORMAL) { c->current_frame = fm.prev; return _cmp; }
        VALUE cmp = _cmp.value;
        long sign = 0;
        if (FIXNUM_P(cmp)) sign = FIX2LONG(cmp);
        else if (!SPECIAL_CONST_P(cmp) && BUILTIN_TYPE(cmp) == T_BIGNUM) sign = korb_int_cmp(cmp, INT2FIX(0));
        if (sign > 0) { fm.last_line = korb_ary_aref(fr.last_line, i); fm.last_match = fr.last_match; }
    }
    VALUE result = fm.last_line;
    c->current_frame = fm.prev;
    return RESULT_OK(result);
}

static RESULT ary_mul(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Array#* — n: repeat, str: join.  argc == 0 → ArgumentError (CRuby). */
    if (argc != 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    struct korb_array *a = (struct korb_array *)self;
    /* nil argument → TypeError (CRuby). */
    if (NIL_P(argv[0])) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT, "no implicit conversion from nil to integer");
    }
    /* CRuby semantics: try #to_str first (treat as join sep).  Only if
     * the argument doesn't respond to :to_str do we fall back to #to_int. */
    /* Re-read the recv from argv[0] (a GC-scanned slot) for each coerce
     * funcall — `arg` as a C-local goes stale across the respond_to?/to_int
     * GC.  `arg` only holds the final coerced result (a string from to_str,
     * or a fixnum immediate). */
    VALUE arg = argv[0];
    if (!FIXNUM_P(arg) && BUILTIN_TYPE(arg) != T_STRING) {
        VALUE rt_str = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) }));
        if (RTEST(rt_str)) {
            VALUE s = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("to_str"), 0, NULL));
            if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) {
                arg = s;
            }
        } else {
            VALUE rt_int = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (RTEST(rt_int)) {
                VALUE iv = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("to_int"), 0, NULL));
                if (FIXNUM_P(iv)) arg = iv;
            } else {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into Integer",
                           korb_id_name(korb_class_of_class(argv[0])->name));
            }
        }
    }
    if (FIXNUM_P(arg)) {
        long n = FIX2LONG(arg);
        if (n < 0) {
            VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eA, "negative argument");
        }
        long total;
        /* self moved across the coerce funcalls — re-read from its slot. */
        self = sp[-argc - 1];
        long alen = korb_ary_len(self);
        if (__builtin_mul_overflow(alen, n, &total) ||
            total > (long)(LONG_MAX / sizeof(VALUE))) {
            VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eA, "argument too big");
        }
        /* R5: pre-sized; park result and re-derive source after the new_capa. */
        sp[0] = korb_ary_new_capa(c, sp + 1, total);
        {
            struct korb_array *aa = (struct korb_array *)sp[-argc - 1];
            for (long i = 0; i < n; i++)
                for (long j = 0; j < alen; j++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(aa)[j]);
        }
        return RESULT_OK(sp[0]);
    }
    if (BUILTIN_TYPE(arg) == T_STRING) {
        /* Park sep / result / per-iter element in sp[0..2] across
         * each korb_to_s / korb_str_concat GC fire.  Pin sep BEFORE
         * any alloc — `arg` is a C-local that goes stale once
         * korb_str_new fires GC. */
        sp[1] = arg;                 /* separator (pin) — first! */
        sp[0] = 0;
        sp[2] = 0;
        c->sp_top = sp + 3;          /* keep sep/result/elem parked across the loop */
        sp[0] = korb_str_new(c, sp + 3, "", 0);
        /* self moved during korb_str_new — re-read length from the slot. */
        long alen = korb_ary_len(sp[-argc - 1]);
        for (long i = 0; i < alen; i++) {
            struct korb_array *aa = (struct korb_array *)sp[-argc - 1];
            if (i > 0) korb_str_concat(c, sp + 3, sp[0], sp[1]);
            sp[2] = korb_ary_items(aa)[i];
            if (BUILTIN_TYPE(sp[2]) != T_STRING) sp[2] = korb_to_s(c, sp + 3, sp[2]);
            korb_str_concat(c, sp + 3, sp[0], sp[2]);
        }
        c->sp_top = sp;
        return RESULT_OK(sp[0]);
    }
    return RESULT_OK(self);
}

static RESULT ary_max_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) return RESULT_OK(Qnil);
    /* See ary_min_by for the two-frame rooting layout (m / mk / receiver /
     * candidate key all span the yield + <=> funcall GC points). */
    KORB_ARY_YIELD_FRAME(c, fm, korb_ary_aref(sp[-argc - 1], 0));   /* m */
    KORB_ARY_YIELD_FRAME(c, fr, sp[-argc - 1]);                     /* receiver */
    RESULT _y0 = korb_yield(c, 1, &fm.last_line);
    if (_y0.state != KORB_NORMAL) { c->current_frame = fm.prev; return _y0; }
    fm.last_match = _y0.value;     /* mk */
    for (long i = 1; i < alen; i++) {
        VALUE v = korb_ary_aref(fr.last_line, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fm.prev; return _y; }
        fr.last_match = _y.value;   /* k: candidate key, park across funcall */
        RESULT _cmp = korb_funcall(c, c->sp_top, fm.last_match, korb_intern("<=>"), 1, &fr.last_match);
        if (_cmp.state != KORB_NORMAL) { c->current_frame = fm.prev; return _cmp; }
        VALUE cmp = _cmp.value;
        long sign = 0;
        if (FIXNUM_P(cmp)) sign = FIX2LONG(cmp);
        else if (!SPECIAL_CONST_P(cmp) && BUILTIN_TYPE(cmp) == T_BIGNUM) sign = korb_int_cmp(cmp, INT2FIX(0));
        if (sign < 0) { fm.last_line = korb_ary_aref(fr.last_line, i); fm.last_match = fr.last_match; }
    }
    VALUE result = fm.last_line;
    c->current_frame = fm.prev;
    return RESULT_OK(result);
}

/* Try #to_int on argv; return Qundef if it doesn't respond, or
 * propagate raise via RESULT. */
static RESULT ary_try_to_int(CTX *c, VALUE v) {
    if (FIXNUM_P(v)) return RESULT_OK(v);
    if (SPECIAL_CONST_P(v)) return RESULT_OK(Qundef);
    /* v is by-value; it goes stale across the respond_to?/to_int funcalls.
     * Park it in the GC root stack (no sp slot available in this helper). */
    VALUE *const vroot = AROH_ROOT_STACK_TOP(c);
    vroot[0] = v;
    AROH_ROOT_STACK_SET_TOP(c, vroot + 1);
    VALUE to_int_sym = korb_id2sym(korb_intern("to_int"));
    RESULT _rt = korb_funcall(c, c->sp_top, vroot[0], korb_intern("respond_to?"), 1, &to_int_sym);
    if (_rt.state != KORB_NORMAL) { AROH_ROOT_STACK_SET_TOP(c, vroot); return _rt; }
    if (!RTEST(_rt.value)) { AROH_ROOT_STACK_SET_TOP(c, vroot); return RESULT_OK(Qundef); }
    RESULT _iv = korb_funcall(c, c->sp_top, vroot[0], korb_intern("to_int"), 0, NULL);
    AROH_ROOT_STACK_SET_TOP(c, vroot);
    if (_iv.state != KORB_NORMAL) return _iv;
    if (!FIXNUM_P(_iv.value)) return RESULT_OK(Qundef);
    return RESULT_OK(_iv.value);
}

/* Common helper: remove a[start, len] in place and return the removed
 * elements as a new Array.  Caller has already validated that
 * 0 <= start <= a->len and len >= 0 and start + len <= a->len. */
static VALUE ary_remove_range(CTX *c, struct korb_array *a, long start, long len) {
    /* R5: korb_ary_new_capa is a GC point and `a` is held across it — park the
     * array handle at sp[1] and the result at sp[0], re-derive a, then do the
     * in-place shift (no further GC).  The result is pre-sized so push won't
     * grow. */
    VALUE *const sp = c->sp_top;
    sp[1] = (VALUE)a;
    sp[0] = korb_ary_new_capa(c, sp + 2, len);
    a = (struct korb_array *)sp[1];
    for (long i = 0; i < len; i++) korb_ary_push(c, sp + 2, sp[0], korb_ary_items(a)[start + i]);
    for (long i = start; i + len < a->len; i++) korb_ary_items(a)[i] = korb_ary_items(a)[i + len];
    a->len -= len;
    return sp[0];
}

static RESULT ary_slice_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    struct korb_array *a = (struct korb_array *)self;
    /* (start, len) form: returns Array (possibly empty), or nil if start
     * out of range. */
    if (argc == 2) {
        VALUE iv0 = UNWRAP(ary_try_to_int(c, argv[0]));
        VALUE iv1 = UNWRAP(ary_try_to_int(c, argv[1]));
        if (UNDEF_P(iv0) || UNDEF_P(iv1) ) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion into Integer");
        }
        a = (struct korb_array *)sp[-argc - 1];  /* R5: re-derive after try_to_int GC */
        long start = FIX2LONG(iv0);
        long len = FIX2LONG(iv1);
        if (start < 0) start += a->len;
        if (start < 0 || start > a->len) return RESULT_OK(Qnil);
        if (len < 0) return RESULT_OK(Qnil);
        if (start + len > a->len) len = a->len - start;
        return RESULT_OK(ary_remove_range(c, a, start, len));
    }
    /* Range form. */
    if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        long b, e;
        if (NIL_P(r->begin)) b = 0;
        else if (FIXNUM_P(r->begin)) b = FIX2LONG(r->begin);
        else return RESULT_OK(Qnil);
        if (NIL_P(r->end)) e = a->len - 1;
        else if (FIXNUM_P(r->end)) {
            e = FIX2LONG(r->end);
            if (e < 0) e += a->len;
            if (r->exclude_end) e -= 1;
        } else return RESULT_OK(Qnil);
        if (b < 0) b += a->len;
        if (b < 0 || b > a->len) return RESULT_OK(Qnil);
        long len = e - b + 1;
        if (len < 0) len = 0;
        if (b + len > a->len) len = a->len - b;
        return RESULT_OK(ary_remove_range(c, a, b, len));
    }
    /* (idx) form: returns single element (or nil). */
    VALUE iv = UNWRAP(ary_try_to_int(c, argv[0]));
    if (UNDEF_P(iv) ) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion into Integer");
    }
    a = (struct korb_array *)sp[-argc - 1];  /* R5: re-derive after try_to_int GC */
    long start = FIX2LONG(iv);
    if (start < 0) start += a->len;
    if (start < 0 || start >= a->len) return RESULT_OK(Qnil);
    VALUE elt = korb_ary_items(a)[start];
    for (long i = start; i + 1 < a->len; i++) korb_ary_items(a)[i] = korb_ary_items(a)[i + 1];
    a->len -= 1;
    return RESULT_OK(elt);
}

/* Array#slice — non-destructive: same dispatch but no mutation.  This
 * shares the logic with element_reference. */
static RESULT ary_slice(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    struct korb_array *a = (struct korb_array *)self;
    if (argc == 2) {
        VALUE iv0 = UNWRAP(ary_try_to_int(c, argv[0]));
        VALUE iv1 = UNWRAP(ary_try_to_int(c, argv[1]));
        if (UNDEF_P(iv0) || UNDEF_P(iv1) ) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion into Integer");
        }
        a = (struct korb_array *)sp[-argc - 1];  /* R5: re-derive after try_to_int GC */
        long start = FIX2LONG(iv0);
        long len = FIX2LONG(iv1);
        if (start < 0) start += a->len;
        if (start < 0 || start > a->len) return RESULT_OK(Qnil);
        if (len < 0) return RESULT_OK(Qnil);
        if (start + len > a->len) len = a->len - start;
        /* R5: pre-sized; park result and re-derive source after new_capa. */
        sp[0] = korb_ary_new_capa(c, sp + 1, len);
        a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < len; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[start + i]);
        return RESULT_OK(sp[0]);
    }
    if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        long b, e;
        if (NIL_P(r->begin)) b = 0;
        else if (FIXNUM_P(r->begin)) b = FIX2LONG(r->begin);
        else return RESULT_OK(Qnil);
        if (NIL_P(r->end)) e = a->len - 1;
        else if (FIXNUM_P(r->end)) {
            e = FIX2LONG(r->end);
            if (e < 0) e += a->len;
            if (r->exclude_end) e -= 1;
        } else return RESULT_OK(Qnil);
        if (b < 0) b += a->len;
        if (b < 0 || b > a->len) return RESULT_OK(Qnil);
        long len = e - b + 1;
        if (len < 0) len = 0;
        if (b + len > a->len) len = a->len - b;
        /* R5: pre-sized; park result and re-derive source after new_capa. */
        sp[0] = korb_ary_new_capa(c, sp + 1, len);
        a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < len; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[b + i]);
        return RESULT_OK(sp[0]);
    }
    VALUE iv = UNWRAP(ary_try_to_int(c, argv[0]));
    if (UNDEF_P(iv) ) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion into Integer");
    }
    a = (struct korb_array *)sp[-argc - 1];  /* R5: re-derive after try_to_int GC */
    long start = FIX2LONG(iv);
    if (start < 0) start += a->len;
    if (start < 0 || start >= a->len) return RESULT_OK(Qnil);
    return RESULT_OK(korb_ary_items(a)[start]);
}

static RESULT ary_each_with_object(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    /* Park the receiver (fr.last_match) + the memo object (fr.last_line,
     * also the return value) across the per-element korb_yield. */
    long alen = korb_ary_len(self);
    KORB_ARY_YIELD_FRAME(c, fr, (sp - argc)[0]);   /* memo */
    fr.last_match = sp[-argc - 1];                  /* receiver */
    for (long i = 0; i < alen; i++) {
        VALUE args[2] = { korb_ary_aref(fr.last_match, i), fr.last_line };
        RESULT _y = korb_yield(c, 2, args);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}


/* ---------- Array#hash (content-based) ---------- */
/* FNV-1a-style mix over each element's hash.  For FIXNUM/SYMBOL/special
 * we use the value bits directly; for heap objects we use the address
 * (stable for the lifetime of the array, matches Ruby's behavior closely
 * enough for `[1,2].hash == [1,2].hash` to hold). */
/* Array#assoc — find a sub-array whose first element == arg. */
static RESULT ary_assoc(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Call == on the entry's first element (so user-defined == can
     * decide).  Non-array entries get implicit to_ary coercion (CRuby:
     * "calls to_ary on non-array elements"). */
    /* R5: a (self) and the matched entry are held across korb_funcall (to_ary
     * and ==) GC points — re-derive a from its slot, park entry at sp[0]. */
    long alen = korb_ary_len(self);
    c->sp_top = sp + 1;   /* publish: protect parked entry across the funcalls */
    for (long i = 0; i < alen; i++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        VALUE e = korb_ary_items(a)[i];
        if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_ARRAY) {
            sp[0] = e;
        } else if (!SPECIAL_CONST_P(e)) {
            /* Try to_ary; if it doesn't respond / raises, skip. */
            struct korb_class *k = korb_class_of_class(e);
            if (!k || !korb_class_find_method(k, korb_intern("to_ary"))) continue;
            RESULT _ta = korb_funcall(c, c->sp_top, e, korb_intern("to_ary"), 0, NULL);
            if (_ta.state != KORB_NORMAL) continue;
            sp[0] = _ta.value;
            if (SPECIAL_CONST_P(sp[0]) || BUILTIN_TYPE(sp[0]) != T_ARRAY) continue;
        } else {
            continue;
        }
        struct korb_array *ea = (struct korb_array *)sp[0];
        if (ea->len == 0) continue;
        VALUE eq_args[1] = { argv[0] };
        VALUE r = UNWRAP(korb_funcall(c, c->sp_top, korb_ary_items(ea)[0], korb_intern("=="), 1, eq_args));
        if (RTEST(r)) return RESULT_OK(sp[0]);
    }
    return RESULT_OK(Qnil);
}

/* Array#rassoc — same but matches the second element. */
static RESULT ary_rassoc(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: re-derive a from its slot; park the matched entry at sp[0]. */
    long alen = korb_ary_len(self);
    c->sp_top = sp + 1;   /* publish: protect parked entry across the funcalls */
    for (long i = 0; i < alen; i++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        VALUE e = korb_ary_items(a)[i];
        if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_ARRAY) {
            sp[0] = e;
        } else if (!SPECIAL_CONST_P(e)) {
            struct korb_class *k = korb_class_of_class(e);
            if (!k || !korb_class_find_method(k, korb_intern("to_ary"))) continue;
            RESULT _ta = korb_funcall(c, c->sp_top, e, korb_intern("to_ary"), 0, NULL);
            if (_ta.state != KORB_NORMAL) continue;
            sp[0] = _ta.value;
            if (SPECIAL_CONST_P(sp[0]) || BUILTIN_TYPE(sp[0]) != T_ARRAY) continue;
        } else {
            continue;
        }
        struct korb_array *ea = (struct korb_array *)sp[0];
        if (ea->len < 2) continue;
        VALUE eq_args[1] = { argv[0] };
        VALUE r = UNWRAP(korb_funcall(c, c->sp_top, korb_ary_items(ea)[1], korb_intern("=="), 1, eq_args));
        if (RTEST(r)) return RESULT_OK(sp[0]);
    }
    return RESULT_OK(Qnil);
}

/* Array#at(i) — like a[i] for a single integer index. */
static RESULT ary_at(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
    if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);
    /* R5: re-read self from its slot after the to_int GC point. */
    return RESULT_OK(korb_ary_aref(sp[-argc - 1], FIX2LONG(iv)));
}

/* Array#fetch(idx[, default]) {block}
 *  * idx is coerced via #to_int.
 *  * If idx is in range, returns the element.
 *  * Otherwise: yields idx to a block (if given) and returns its value;
 *    else returns the explicit default arg (if given);
 *    else raises IndexError. */
static RESULT ary_fetch(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
    if (!FIXNUM_P(iv)) return RESULT_OK(Qnil);
    long i = FIX2LONG(iv);
    /* korb_to_int_or_raise is a GC point — re-read self (the C-local is a
     * stale, possibly moved handle). */
    struct korb_array *a = (struct korb_array *)sp[-argc - 1];
    long norm = i < 0 ? i + a->len : i;
    if (norm >= 0 && norm < a->len) return RESULT_OK(korb_ary_items(a)[norm]);
    if (korb_block_given(c)) {
        VALUE arg[1] = { argv[0] };
        return korb_yield(c, 1, arg);
    }
    if (argc == 2) return RESULT_OK(argv[1]);
    VALUE eI = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
    return korb_raise(c, (struct korb_class *)eI,
               "index %ld outside of array bounds: %ld...%ld",
               i, -a->len, a->len);
}

/* Array#fetch_values(*indexes) {block} — like fetch but for many indexes
 * at once.  Returns an array.  Without a block, raises IndexError on the
 * first missing index. */
static RESULT ary_fetch_values(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park result (fr.last_line) + source receiver (fr.last_match) in a
     * synthetic frame across the per-index korb_yield: yield lowers sp_top
     * below the cfunc's sp[] slots, so plain sp parking is collected.  Index
     * args (argv[k]) are typically fixnums; re-derive the source from
     * fr.last_match each iteration.  Restore c->current_frame on all exits. */
    bool block_p = korb_block_given(c);
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];   /* source receiver (re-read post-alloc) */
    for (int k = 0; k < argc; k++) {
        RESULT _ivr = korb_to_int_or_raise(c, argv[k]);
        if (_ivr.state != KORB_NORMAL) { c->current_frame = fr.prev; return _ivr; }
        if (!FIXNUM_P(_ivr.value)) { c->current_frame = fr.prev; return RESULT_OK(Qnil); }
        long i = FIX2LONG(_ivr.value);
        struct korb_array *a = (struct korb_array *)fr.last_match;
        long norm = i < 0 ? i + a->len : i;
        if (norm >= 0 && norm < a->len) {
            korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_items(a)[norm]);
        } else if (block_p) {
            VALUE arg[1] = { argv[k] };
            RESULT _yr = korb_yield(c, 1, arg);
            if (_yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return _yr; }
            korb_ary_push(c, c->sp_top, fr.last_line, _yr.value);
        } else {
            a = (struct korb_array *)fr.last_match;
            long alen = a->len;
            VALUE eI = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            c->current_frame = fr.prev;
            return korb_raise(c, (struct korb_class *)eI,
                       "index %ld outside of array bounds: %ld...%ld",
                       i, -alen, alen);
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Array#delete(obj) — remove all == matches; return obj if found else nil. */
static RESULT ary_delete(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    /* Frozen check is conditional: CRuby only raises FrozenError when a
     * modification would actually happen.  Scan first; raise if we'd
     * remove anything. */
    /* R5: a held across korb_funcall(==) GC — re-derive from its slot. */
    long alen = korb_ary_len(self);
    /* First pass: count matches (use full == dispatch so user override
     * participates).  Don't mutate yet. */
    long matches = 0;
    for (long r = 0; r < alen; r++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        VALUE eq_args[1] = { argv[0] };
        VALUE r_eq = UNWRAP(korb_funcall(c, c->sp_top, korb_ary_items(a)[r], korb_intern("=="), 1, eq_args));
        if (RTEST(r_eq)) matches++;
    }
    if (matches == 0) {
        /* No modification — block fallback (CRuby returns block's value
         * if a block is given, else nil).  Don't raise FrozenError. */
        if (korb_block_given(c)) {
            VALUE blk_args[1] = { argv[0] };
            return korb_yield(c, 1, blk_args);
        }
        return RESULT_OK(Qnil);
    }
    /* Will modify — now enforce frozen check. */
    CHECK_FROZEN_R(c, self);
    long w = 0;
    alen = korb_ary_len(self);
    for (long r = 0; r < alen; r++) {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        VALUE eq_args[1] = { argv[0] };
        VALUE r_eq = UNWRAP(korb_funcall(c, c->sp_top, korb_ary_items(a)[r], korb_intern("=="), 1, eq_args));
        if (!RTEST(r_eq)) korb_ary_items(a)[w++] = korb_ary_items(a)[r];
    }
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        a->len = w;
    }
    return RESULT_OK(argv[0]);
}

/* Array#delete_at(i) — remove element at i, return removed or nil. */
static RESULT ary_delete_at(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    /* Coerce non-Integer index via #to_int (CRuby semantics).  Try
     * unconditionally so method_missing-based mocks also coerce. */
    VALUE idx = argv[0];
    if (!FIXNUM_P(idx) && (SPECIAL_CONST_P(idx) || BUILTIN_TYPE(idx) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(idx)) {
            RESULT _tr = korb_funcall(c, c->sp_top, idx, korb_intern("to_int"), 0, NULL);
            if (_tr.state == KORB_RAISE) {
                /* Swallow only NoMethodError (so the original index sticks);
                 * other raises propagate. */
                VALUE bang = _tr.value;
                VALUE eNo = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
                if (SPECIAL_CONST_P(bang) || SPECIAL_CONST_P(eNo) ||
                    BUILTIN_TYPE(eNo) != T_CLASS) return RESULT_OK(Qnil);
                struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                bool is_nm = false;
                for (struct korb_class *kk = bk; kk; kk = kk->super) {
                    if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                }
                if (!is_nm) return _tr;
                /* swallowed — fall through with original idx */
            } else if (_tr.state != KORB_NORMAL) {
                return _tr;
            } else if (FIXNUM_P(_tr.value)) {
                idx = _tr.value;
            }
        }
        if (!FIXNUM_P(idx)) return RESULT_OK(Qnil);
    }
    if (!FIXNUM_P(idx)) return RESULT_OK(Qnil);
    /* self may have moved across the to_int funcall — re-read from the
     * GC-scanned staging slot, not the stale C-local. */
    self = sp[-argc - 1];
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(idx);
    if (i < 0) i += a->len;
    if (i < 0 || i >= a->len) return RESULT_OK(Qnil);
    VALUE r = korb_ary_items(a)[i];
    for (long j = i; j + 1 < a->len; j++) korb_ary_items(a)[j] = korb_ary_items(a)[j + 1];
    a->len--;
    return RESULT_OK(r);
}

/* Array#delete_if { |x| ... } — remove where block returns truthy. */
static RESULT ary_delete_if(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("delete_if"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &arg);
    }
    CHECK_FROZEN_R(c, self);
    /* Park the receiver (fr.last_match) across the per-element korb_yield;
     * re-read the element from the framed receiver (w<=r keeps a[r] intact).
     * Re-read the length each step so a block may grow the array during the
     * scan (CRuby "tolerates increasing size"); compaction continues into the
     * appended region. */
    long w = 0;
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (long r = 0; r < korb_ary_len(fr.last_match); r++) {
        VALUE elt = korb_ary_aref(fr.last_match, r);
        RESULT _y = korb_yield(c, 1, &elt);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (NIL_P(_y.value) || _y.value == Qfalse) {
            korb_ary_items((struct korb_array *)fr.last_match)[w++] = korb_ary_aref(fr.last_match, r);
        }
    }
    ((struct korb_array *)fr.last_match)->len = w;
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Array#reject! — like delete_if, but returns nil when nothing was
 * removed (CRuby semantic for the bang).  No block → Enumerator. */
static RESULT ary_reject_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("reject!"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &arg);
    }
    CHECK_FROZEN_R(c, self);
    /* Park the receiver (fr.last_match) across the per-element korb_yield;
     * re-read the element from the framed receiver after the yield (the
     * compaction writes w<=r, so a[r] is still intact when read). */
    long w = 0;
    bool changed = false;
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    /* Re-read length each step so a block may grow the array (CRuby). */
    for (long r = 0; r < korb_ary_len(fr.last_match); r++) {
        VALUE elt = korb_ary_aref(fr.last_match, r);
        RESULT _y = korb_yield(c, 1, &elt);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (NIL_P(_y.value) || _y.value == Qfalse) {
            korb_ary_items((struct korb_array *)fr.last_match)[w++] = korb_ary_aref(fr.last_match, r);
        } else {
            changed = true;
        }
    }
    if (!changed) { c->current_frame = fr.prev; return RESULT_OK(Qnil); }
    ((struct korb_array *)fr.last_match)->len = w;
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Array#reject { |x| ... } — like delete_if but returns a new array. */
/* Array#reverse_each — yields elements in reverse order; no block →
 * Array (Enumerator stand-in). */
static RESULT ary_reverse_each(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("reverse_each"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    /* Park the receiver (fr.last_match) across the per-element korb_yield. */
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (long i = korb_ary_len(fr.last_match) - 1; i >= 0; i--) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT ary_reject(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Non-mutating — reject! is the in-place form (registered as
     * delete_if, which does mutate and is FROZEN-checked). */
    if (!korb_block_given(c)) return RESULT_OK(self);
    /* Park result (fr.last_line) + receiver (fr.last_match) across the
     * per-element korb_yield via the frame chain (see ary_map).  Re-read
     * length each step so the block may grow the array (CRuby semantics). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (NIL_P(_y.value) || _y.value == Qfalse) {
            korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_aref(fr.last_match, i));
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Array#insert(i, *elts) — splice elts into self starting at i. */
static RESULT ary_insert(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 0, expected 1+)");
    }
    if (argc == 1) return RESULT_OK(self);  /* `arr.insert(i)` with no values is no-op */
    VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));
    /* korb_to_int_or_raise is a GC point; re-read self (the `self` C-local
     * is now a stale, possibly moved handle). */
    self = sp[-argc - 1];
    if (!FIXNUM_P(iv)) return RESULT_OK(self);
    struct korb_array *a = (struct korb_array *)self;
    long i = FIX2LONG(iv);
    if (i < 0) {
        long real = i + a->len + 1;
        if (real < 0) {
            VALUE eIE = korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError"));
            return korb_raise(c, (struct korb_class *)eIE,
                       "index %ld too small for array; minimum: -%ld", i, a->len + 1);
        }
        i = real;
    }
    long ins = argc - 1;
    if (ins == 0) return RESULT_OK(self);
    /* R5: the padding/space pushes grow self (GC) — re-derive a each iteration
     * and push to the re-read handle (the `self` C-local goes stale once a
     * prior push moves the array), then re-derive again before the shuffle. */
    while ((a = (struct korb_array *)sp[-argc - 1])->len < i) korb_ary_push(c, c->sp_top, sp[-argc - 1], Qnil);
    for (long k = 0; k < ins; k++) korb_ary_push(c, c->sp_top, sp[-argc - 1], Qnil);
    a = (struct korb_array *)sp[-argc - 1];
    for (long k = a->len - 1; k >= i + ins; k--) korb_ary_items(a)[k] = korb_ary_items(a)[k - ins];
    for (long k = 0; k < ins; k++) korb_ary_items(a)[i + k] = argv[1 + k];
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#replace(other) — destructive replace. */
static RESULT ary_replace(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    VALUE other = argv[0];
    /* Coerce non-Array via #to_ary (CRuby semantics) — TypeError if neither
     * an Array nor respond_to?(:to_ary). */
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
            if (RTEST(rt)) {
                RESULT tr = korb_funcall_r(c, c->sp_top, other, korb_intern("to_ary"), 0, NULL);
                if (tr.state != KORB_NORMAL) return tr;
                other = tr.value;
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_ARRAY) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                              "no implicit conversion of %s into Array",
                              SPECIAL_CONST_P(argv[0]) ? "(special)" :
                              korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    /* The #to_ary coercion above is a GC point — re-read self. */
    self = sp[-argc - 1];
    /* Self-replace is a no-op (CRuby semantics). */
    if (self == other) return RESULT_OK(self);
    /* R5: park the (possibly coerced) source at sp[0]; the push-grows below
     * move it, so re-derive b each iteration, and push to the re-read self
     * handle (the C-local goes stale once a push grows the array). */
    sp[0] = other;
    ((struct korb_array *)sp[-argc - 1])->len = 0;
    {
        long blen = korb_ary_len(sp[0]);
        for (long i = 0; i < blen; i++) {
            struct korb_array *b = (struct korb_array *)sp[0];
            korb_ary_push(c, sp + 1, sp[-argc - 1], korb_ary_items(b)[i]);
        }
    }
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#each_index { |i| ... } — yields successive indices. */
static RESULT ary_each_index(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("each_index"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    /* Park the receiver (fr.last_match) across the per-element korb_yield so
     * its length stays readable (indices themselves are immediates). */
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE iv = INT2FIX(i);
        RESULT _y = korb_yield(c, 1, &iv);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Array#clone — shallow copy (same as dup for our purposes). */
static RESULT ary_clone(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: pre-sized; park result and re-derive source after the new_capa. */
    long alen = korb_ary_len(self);
    sp[0] = korb_ary_new_capa(c, sp + 1, alen);
    {
        const struct korb_array *a = (const struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    /* clone preserves frozen state (`dup` does not) — match CRuby. */
    if (korb_obj_frozen_p(sp[-argc - 1])) {
        ((struct RBasic *)sp[0])->head.flags |= FL_FROZEN;
    }
    return RESULT_OK(sp[0]);
}

/* Array#eql? — for our impl, same as ==. */
static __thread VALUE ary_eql_stk_a[64];
static __thread VALUE ary_eql_stk_b[64];
static __thread int ary_eql_top = 0;
static RESULT ary_eql(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* eql? is type-strict, including for elements: [1, 2.0].eql?([1, 2])
     * is false because 2.0.eql?(2) is false. */
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return RESULT_OK(Qfalse);
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    if (a->len != b->len) return RESULT_OK(Qfalse);
    /* Recursion guard: on re-entry with the same (self, other), assume equal. */
    for (int j = 0; j < ary_eql_top; j++) {
        if (ary_eql_stk_a[j] == self && ary_eql_stk_b[j] == argv[0]) {
            return RESULT_OK(Qtrue);
        }
    }
    if (ary_eql_top < 64) {
        ary_eql_stk_a[ary_eql_top] = self;
        ary_eql_stk_b[ary_eql_top] = argv[0];
        ary_eql_top++;
    }
    RESULT result = RESULT_OK(Qtrue);
    /* R5: a/b held across korb_funcall(eql?) — re-derive from their GC slots
     * (self slot and argv[0] slot) each iteration. */
    long alen = a->len;
    for (long i = 0; i < alen; i++) {
        a = (struct korb_array *)sp[-argc - 1];
        b = (struct korb_array *)argv[0];
        RESULT _er = korb_funcall(c, c->sp_top, korb_ary_items(a)[i], korb_intern("eql?"), 1, &korb_ary_items(b)[i]);
        if (_er.state != KORB_NORMAL) { result = _er; goto done; }
        if (!RTEST(_er.value)) { result = RESULT_OK(Qfalse); goto done; }
    }
done:
    if (ary_eql_top > 0) ary_eql_top--;
    return result;
}

/* Array#<=> — lexical comparison.  Recursive arrays: on re-entry with
 * the same (self, other) pair, return 0 (CRuby's rb_exec_recursive_paired
 * behavior) so the outer loop continues without infinite recursion. */
static __thread VALUE ary_cmp_stk_a[64];
static __thread VALUE ary_cmp_stk_b[64];
static __thread int ary_cmp_top = 0;
static RESULT ary_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* CRuby: Array#<=> tries #to_ary on non-Array argument. */
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) {
        if (SPECIAL_CONST_P(argv[0])) return RESULT_OK(Qnil);
        VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
        if (!RTEST(rt)) return RESULT_OK(Qnil);
        VALUE coerced = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("to_ary"), 0, NULL));
        if (SPECIAL_CONST_P(coerced) || BUILTIN_TYPE(coerced) != T_ARRAY) return RESULT_OK(Qnil);
        argv[0] = coerced;
        /* The to_ary coercion is a GC point — re-read self. */
        self = sp[-argc - 1];
    }
    /* Recursion guard: re-entering on same (self, other) returns 0. */
    for (int j = 0; j < ary_cmp_top; j++) {
        if (ary_cmp_stk_a[j] == self && ary_cmp_stk_b[j] == argv[0]) {
            return RESULT_OK(INT2FIX(0));
        }
    }
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    long n = a->len < b->len ? a->len : b->len;
    if (ary_cmp_top < 64) {
        ary_cmp_stk_a[ary_cmp_top] = self;
        ary_cmp_stk_b[ary_cmp_top] = argv[0];
        ary_cmp_top++;
    }
    RESULT result;
    /* R5: a/b held across korb_funcall(<=>) — re-derive from their GC slots. */
    for (long i = 0; i < n; i++) {
        a = (struct korb_array *)sp[-argc - 1];
        b = (struct korb_array *)argv[0];
        RESULT _er = korb_funcall(c, c->sp_top, korb_ary_items(a)[i], korb_intern("<=>"), 1, &korb_ary_items(b)[i]);
        if (_er.state != KORB_NORMAL) { result = _er; goto done; }
        VALUE r = _er.value;
        if (!FIXNUM_P(r) || FIX2LONG(r) != 0) { result = RESULT_OK(r); goto done; }
    }
    if (a->len == b->len) { result = RESULT_OK(INT2FIX(0)); goto done; }
    result = RESULT_OK(INT2FIX(a->len < b->len ? -1 : 1));
done:
    if (ary_cmp_top > 0) ary_cmp_top--;
    return result;
}

/* Helpers / impl for combination + permutation.  Returns RESULT to
 * propagate yield-side raise/break/next without c->state. */
/* Recursive combination worker.  src (source array) and buf (the working
 * tuple) are held across korb_yield / push-grow / the recursion, so they
 * live in the caller-owned synthetic frame `fr` (fr.last_match = src,
 * fr.last_line = buf) which is current for the whole operation — the frame
 * chain is always scanned, so moving GC keeps them live regardless of how
 * far yield lowers sp_top.  result_or_nil is Qnil in the (only) block path. */
static RESULT ary_combine(CTX *c, struct korb_frame *fr, long r, long start,
                           VALUE result_or_nil) {
    if (korb_ary_len(fr->last_line) == r) {
        /* sp staging is above c->sp_top; the copy is built and immediately
         * consumed (yield / push), no second cross-GC hold needed. */
        VALUE copy = korb_ary_new_capa(c, c->sp_top, r);
        VALUE buf = fr->last_line;
        for (long i = 0; i < r; i++) korb_ary_push(c, c->sp_top, copy, korb_ary_aref(buf, i));
        if (NIL_P(result_or_nil)) CHECK(korb_yield(c, 1, &copy));
        else korb_ary_push(c, c->sp_top, result_or_nil, copy);
        return RESULT_OK(Qnil);
    }
    long n = korb_ary_len(fr->last_match);
    for (long i = start; i < n; i++) {
        korb_ary_push(c, c->sp_top, fr->last_line, korb_ary_aref(fr->last_match, i));
        RESULT _rc = ary_combine(c, fr, r, i + 1, result_or_nil);
        ((struct korb_array *)fr->last_line)->len--;
        if (_rc.state != KORB_NORMAL) return _rc;
    }
    return RESULT_OK(Qnil);
}

static RESULT ary_combination(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(Qnil);
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
        VALUE e = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), argc + 1, call_argv));
        if (SPECIAL_CONST_P(e)) return RESULT_OK(e);
        long n = korb_ary_len(sp[-argc - 1]);  /* R5: a stale after to_enum */
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
        return RESULT_OK(e);
    }
    if (r < 0 || r > a->len) return RESULT_OK(self);
    /* Park source (fr.last_match) + working tuple buf (fr.last_line) in a
     * synthetic frame current for the whole recursion (see ary_combine). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, r));
    fr.last_match = sp[-argc - 1];
    RESULT _rc = ary_combine(c, &fr, r, 0, Qnil);
    c->current_frame = fr.prev;
    CHECK(_rc);
    return RESULT_OK(sp[-argc - 1]);
}

/* src (fr.last_match) and the working tuple buf (fr.last_line) are parked
 * in the synthetic frame across korb_yield / push-grow / the recursion (the
 * frame chain is always scanned).  `used[]` is a plain C bool buffer — it
 * only ever holds true/false, which are immediates, so no moving handles. */
static RESULT ary_perm(CTX *c, struct korb_frame *fr, long r,
                        bool *used, VALUE result_or_nil) {
    if (korb_ary_len(fr->last_line) == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp_top, r);
        VALUE buf = fr->last_line;
        for (long i = 0; i < r; i++) korb_ary_push(c, c->sp_top, copy, korb_ary_aref(buf, i));
        if (NIL_P(result_or_nil)) CHECK(korb_yield(c, 1, &copy));
        else korb_ary_push(c, c->sp_top, result_or_nil, copy);
        return RESULT_OK(Qnil);
    }
    long n = korb_ary_len(fr->last_match);
    for (long i = 0; i < n; i++) {
        if (used[i]) continue;
        used[i] = true;
        korb_ary_push(c, c->sp_top, fr->last_line, korb_ary_aref(fr->last_match, i));
        RESULT _rp = ary_perm(c, fr, r, used, result_or_nil);
        ((struct korb_array *)fr->last_line)->len--;
        used[i] = false;
        if (_rp.state != KORB_NORMAL) return _rp;
    }
    return RESULT_OK(Qnil);
}

static RESULT ary_permutation(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_array *a = (struct korb_array *)self;
    long r = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : a->len;
    
    /* No block: return Enumerator with size = n! / (n-r)! when 0<=r<=n,
     * else 0.  CRuby semantics. */
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("permutation"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        VALUE e = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), argc + 1, call_argv));
        if (SPECIAL_CONST_P(e)) return RESULT_OK(e);
        long n = korb_ary_len(sp[-argc - 1]);  /* R5: a stale after to_enum */
        long sz = 0;
        if (r >= 0 && r <= n) {
            sz = 1;
            for (long i = 0; i < r; i++) sz *= (n - i);
        }
        korb_ivar_set(e, korb_intern("@__size"), INT2FIX(sz));
        return RESULT_OK(e);
    }
    if (r < 0 || r > a->len) return RESULT_OK(self);
    /* Park source (fr.last_match) + working tuple buf (fr.last_line) in a
     * synthetic frame; `used` is a C bool buffer (immediate contents). */
    long alen = korb_ary_len(self);
    bool *used = alen > 0 ? korb_xmalloc(sizeof(bool) * alen) : NULL;
    for (long i = 0; i < alen; i++) used[i] = false;
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, r));
    fr.last_match = sp[-argc - 1];
    RESULT _rp = ary_perm(c, &fr, r, used, Qnil);
    c->current_frame = fr.prev;
    CHECK(_rp);
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#cycle(n=nil) — yield each element n times (or forever if nil).
 * Implemented in C so `break` from the block cleanly exits all the
 * nested loops (the bootstrap-Ruby version has nested blk.call inside
 * each{} inside loop{} and break doesn't propagate out reliably). */
static RESULT ary_cycle(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("cycle"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        VALUE e = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), argc + 1, call_argv));
        /* Cycle size: ary.len * n for positive n, 0 for <=0, Infinity for nil arg. */
        VALUE size_val;
        if (argc < 1 || NIL_P(argv[0])) {
            VALUE finf = korb_const_get(KORB_VM(c)->float_class, korb_intern("INFINITY"));
            size_val = UNDEF_P(finf) ? Qnil : finf;
        } else if (FIXNUM_P(argv[0])) {
            long n = FIX2LONG(argv[0]);
            /* R5: a went stale across korb_funcall(to_enum) — use the slot. */
            size_val = n > 0 ? INT2FIX(korb_ary_len(sp[-argc - 1]) * n) : INT2FIX(0);
        } else {
            size_val = Qnil;
        }
        korb_ivar_set(e, korb_intern("@__size"), size_val);
        return RESULT_OK(e);
    }
    long n = -1;  /* -1 means infinite */
    if (argc >= 1 && !NIL_P(argv[0])) {
        VALUE nv = argv[0];
        if (FIXNUM_P(nv)) {
            n = FIX2LONG(nv);
        } else if (FLONUM_P(nv) || (!SPECIAL_CONST_P(nv) && BUILTIN_TYPE(nv) == T_FLOAT)) {
            n = (long)korb_num2dbl(nv);
        } else if (!SPECIAL_CONST_P(nv) && BUILTIN_TYPE(nv) == T_BIGNUM) {
            n = LONG_MAX;
        } else if (!SPECIAL_CONST_P(nv)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (RTEST(rt)) {
                VALUE iv = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("to_int"), 0, NULL));
                if (FIXNUM_P(iv)) n = FIX2LONG(iv);
                else {
                    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                    return korb_raise(c, (struct korb_class *)eT,
                               "no implicit conversion into Integer");
                }
            } else {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                /* nv went stale across the respond_to? funcall GC; re-read the
                 * forwarded arg slot for the class-name in the error. */
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of %s into Integer",
                           korb_id_name(korb_class_of_class(argv[0])->name));
            }
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion into Integer");
        }
        if (n <= 0) return RESULT_OK(Qnil);
    }
    /* Park the receiver (fr.last_match) across the per-element korb_yield. */
    if (korb_ary_len(sp[-argc - 1]) == 0) return RESULT_OK(Qnil);
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    long iter = 0;
    while (n < 0 || iter < n) {
        long alen = korb_ary_len(fr.last_match);
        if (alen == 0) { c->current_frame = fr.prev; return RESULT_OK(Qnil); }  /* cleared mid-iter */
        for (long i = 0; i < alen; i++) {
            VALUE v = korb_ary_aref(fr.last_match, i);
            RESULT _y = korb_yield(c, 1, &v);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        }
        iter++;
    }
    c->current_frame = fr.prev;
    return RESULT_OK(Qnil);
}

/* Array#repeated_combination(r) — combinations with repetition.  src
 * (fr.last_match) + working tuple buf (fr.last_line) parked in the frame. */
static RESULT ary_rcombine(CTX *c, struct korb_frame *fr, long r, long start,
                            VALUE result_or_nil) {
    if (korb_ary_len(fr->last_line) == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp_top, r);
        VALUE buf = fr->last_line;
        for (long i = 0; i < r; i++) korb_ary_push(c, c->sp_top, copy, korb_ary_aref(buf, i));
        if (NIL_P(result_or_nil)) CHECK(korb_yield(c, 1, &copy));
        else korb_ary_push(c, c->sp_top, result_or_nil, copy);
        return RESULT_OK(Qnil);
    }
    long n = korb_ary_len(fr->last_match);
    for (long i = start; i < n; i++) {
        korb_ary_push(c, c->sp_top, fr->last_line, korb_ary_aref(fr->last_match, i));
        RESULT _rc = ary_rcombine(c, fr, r, i, result_or_nil);  /* i, not i+1 — repetition */
        ((struct korb_array *)fr->last_line)->len--;
        if (_rc.state != KORB_NORMAL) return _rc;
    }
    return RESULT_OK(Qnil);
}

static RESULT ary_repeated_combination(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("repeated_combination"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), argc + 1, call_argv);
    }
    if (r < 0) return RESULT_OK(self);
    if (r == 0) {
        VALUE empty = korb_ary_new(c, c->sp_top);
        CHECK(korb_yield(c, 1, &empty));
        /* Re-read self: korb_yield moved the receiver array; the C-local
         * `self` is stale and returning it left the enumerator generator
         * block's send-result dangling (SEGV via .to_a under STRESS). */
        return RESULT_OK(sp[-argc - 1]);
    }
    if (a->len == 0) return RESULT_OK(self);
    /* Park source (fr.last_match) + working tuple buf (fr.last_line). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, r));
    fr.last_match = sp[-argc - 1];
    RESULT _rc = ary_rcombine(c, &fr, r, 0, Qnil);
    c->current_frame = fr.prev;
    CHECK(_rc);
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#repeated_permutation(r) — permutations with repetition.  src
 * (fr.last_match) + working tuple buf (fr.last_line) parked in the frame. */
static RESULT ary_rperm(CTX *c, struct korb_frame *fr, long r,
                         VALUE result_or_nil) {
    if (korb_ary_len(fr->last_line) == r) {
        VALUE copy = korb_ary_new_capa(c, c->sp_top, r);
        VALUE buf = fr->last_line;
        for (long i = 0; i < r; i++) korb_ary_push(c, c->sp_top, copy, korb_ary_aref(buf, i));
        if (NIL_P(result_or_nil)) CHECK(korb_yield(c, 1, &copy));
        else korb_ary_push(c, c->sp_top, result_or_nil, copy);
        return RESULT_OK(Qnil);
    }
    long n = korb_ary_len(fr->last_match);
    for (long i = 0; i < n; i++) {
        korb_ary_push(c, c->sp_top, fr->last_line, korb_ary_aref(fr->last_match, i));
        RESULT _rp = ary_rperm(c, fr, r, result_or_nil);
        ((struct korb_array *)fr->last_line)->len--;
        if (_rp.state != KORB_NORMAL) return _rp;
    }
    return RESULT_OK(Qnil);
}

static RESULT ary_repeated_permutation(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long r = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    
    if (!c->current_block) {
        VALUE method_sym = korb_id2sym(korb_intern("repeated_permutation"));
        VALUE *call_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
        call_argv[0] = method_sym;
        for (int i = 0; i < argc; i++) call_argv[i + 1] = argv[i];
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), argc + 1, call_argv);
    }
    if (r < 0) return RESULT_OK(self);
    if (r == 0) {
        VALUE empty = korb_ary_new(c, c->sp_top);
        CHECK(korb_yield(c, 1, &empty));
        /* Re-read self: korb_yield moved the receiver array; the C-local
         * `self` is stale and returning it left the enumerator generator
         * block's send-result dangling (SEGV via .to_a under STRESS). */
        return RESULT_OK(sp[-argc - 1]);
    }
    if (a->len == 0) return RESULT_OK(self);
    /* Park source (fr.last_match) + working tuple buf (fr.last_line). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new_capa(c, c->sp_top, r));
    fr.last_match = sp[-argc - 1];
    RESULT _rp = ary_rperm(c, &fr, r, Qnil);
    c->current_frame = fr.prev;
    CHECK(_rp);
    return RESULT_OK(sp[-argc - 1]);
}

/* Array#product(*others) — Cartesian product. */
static RESULT ary_product(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long n = argc + 1;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_ARRAY) return RESULT_OK(Qnil);
    }
    /* Collect the n source arrays into a parked Array (fr.last_match) so the
     * handles survive the per-row korb_yield / push-grow (libc-cached
     * handles would go stale under moving GC); the result accumulator (no-
     * block mode) sits in fr.last_line.  idx[] is plain C longs.  Both
     * frame slots are walked by visit_roots regardless of sp_top. */
    KORB_ARY_YIELD_FRAME(c, fr,
        c->current_block ? Qnil : korb_ary_new(c, c->sp_top));   /* result */
    fr.last_match = korb_ary_new_capa(c, c->sp_top, n);          /* sources */
    korb_ary_push(c, c->sp_top, fr.last_match, sp[-argc - 1]);
    for (int i = 0; i < argc; i++) korb_ary_push(c, c->sp_top, fr.last_match, (sp - argc)[i]);
    /* Total size sanity: materializing a huge product is hopeless. */
    if (!c->current_block) {
        long total = 1;
        for (long i = 0; i < n; i++) {
            long li = korb_ary_len(korb_ary_aref(fr.last_match, i));
            if (li == 0) { total = 0; break; }
            if (__builtin_mul_overflow(total, li, &total) ||
                total > (long)(LONG_MAX / sizeof(VALUE) / 4)) {
                c->current_frame = fr.prev;
                VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
                return korb_raise(c, (struct korb_class *)eR, "too big to product");
            }
        }
    }
    long *idx = korb_xcalloc(n, sizeof(long));
    while (true) {
        sp[0] = korb_ary_new_capa(c, sp + 1, n);   /* row, parked across push-grow */
        c->sp_top = sp + 1;
        bool empty = false;
        for (long i = 0; i < n; i++) {
            VALUE src = korb_ary_aref(fr.last_match, i);
            if (korb_ary_len(src) == 0) { empty = true; break; }
            korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(src, idx[i]));
        }
        if (empty) { c->sp_top = sp; break; }
        if (c->current_block) {
            RESULT _y = korb_yield(c, 1, &sp[0]);
            if (_y.state != KORB_NORMAL) { c->sp_top = sp; c->current_frame = fr.prev; return _y; }
        } else {
            korb_ary_push(c, sp + 1, fr.last_line, sp[0]);
        }
        c->sp_top = sp;
        long j = n - 1;
        while (j >= 0) {
            idx[j]++;
            if (idx[j] < korb_ary_len(korb_ary_aref(fr.last_match, j))) break;
            idx[j] = 0;
            j--;
        }
        if (j < 0) break;
    }
    VALUE ret = c->current_block ? sp[-argc - 1] : fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(ret);
}

/* Array.new(size = 0, default = nil) — create an array of `size` slots
 * pre-filled with `default`, or, if a block is given, with the block's
 * return value for each index. */
/* Array[] — class method, equivalent to an array literal of the args. */
RESULT ary_class_brackets(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *const argv = sp - argc;   /* args stay rooted in their scanned slots */

    /* The new array's handle is parked at sp[0] by korb_ary_new_capa and
     * kept rooted there across the push loop (korb_ary_push raises sp_top
     * above sp[0] before each grow).  Read self/argv fresh from their
     * scanned slots — the C-locals would go stale across the moving GC. */
    sp[0] = korb_ary_new_capa(c, sp, (long)argc);
    /* Honor the receiver class — `MySubclass[1,2,3]` returns a
     * MySubclass instance (CRuby Array.[] semantics).  Default cases
     * (Array.[]) keep Array as the basic.klass. */
    VALUE self = sp[-argc - 1];      /* re-read post-alloc (slot is scanned) */
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS &&
        self != (VALUE)KORB_VM(c)->array_class) {
        ((struct RBasic *)sp[0])->klass = self;
    }
    for (int i = 0; i < argc; i++) korb_ary_push(c, sp, sp[0], argv[i]);
    return RESULT_OK(sp[0]);
}

/* Array#initialize — populate an already-allocated Array in place.  Called
 * via `Array.allocate.send(:initialize, ...)` or as part of `Array.new`
 * after the allocation step (CRuby semantic).  Returns self. */
static RESULT ary_initialize(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..2)", argc);
    }
    struct korb_array *a = (struct korb_array *)self;
    /* Reset to empty (CRuby's #initialize replaces the contents). */
    a->len = 0;
    if (argc == 0) return RESULT_OK(self);
    /* Single Array-arg form: replace with a copy of the other array.
     * Skip the to_ary coerce when arg is a String (CRuby's behavior). */
    VALUE first = argv[0];
    if (argc == 1 && !SPECIAL_CONST_P(first) && BUILTIN_TYPE(first) == T_ARRAY) {
        /* R5: push-grow moves both src and self (the receiver) — re-derive
         * each from its GC-scanned staging slot every iteration. */
        long srclen = korb_ary_len(argv[0]);
        for (long i = 0; i < srclen; i++) {
            struct korb_array *src = (struct korb_array *)argv[0];
            korb_ary_push(c, c->sp_top, sp[-argc - 1], korb_ary_items(src)[i]);
        }
        return RESULT_OK(sp[-argc - 1]);
    }
    /* Try to_ary coerce when the first arg isn't already an integer-like.
     * Use respond_to?(:to_ary, true) so private to_ary is also seen. */
    if (argc == 1 && !SPECIAL_CONST_P(first) && BUILTIN_TYPE(first) != T_BIGNUM) {
        if (!FIXNUM_P(first)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, first, korb_intern("respond_to?"), 2,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")), Qtrue }));
            if (RTEST(rt)) {
                /* R5: park the coerced source at sp[0] — push-grows move it. */
                sp[0] = UNWRAP(korb_funcall(c, c->sp_top, first, korb_intern("__send__"), 1,
                                              (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
                if (!SPECIAL_CONST_P(sp[0]) && BUILTIN_TYPE(sp[0]) == T_ARRAY) {
                    long srclen = korb_ary_len(sp[0]);
                    for (long i = 0; i < srclen; i++) {
                        struct korb_array *src = (struct korb_array *)sp[0];
                        korb_ary_push(c, sp + 1, sp[-argc - 1], korb_ary_items(src)[i]);
                    }
                    return RESULT_OK(sp[-argc - 1]);
                }
            }
        }
    }
    /* The to_ary coercion above is a GC point — re-read first from its arg
     * slot before the type checks / size coercion below dereference it. */
    first = argv[0];
    /* size/default form: coerce size via #to_int when needed. */
    long size = 0;
    if (FIXNUM_P(first)) {
        size = FIX2LONG(first);
    } else if (!SPECIAL_CONST_P(first) && BUILTIN_TYPE(first) == T_BIGNUM) {
        /* Bignum size → ArgumentError ("array size too big"). */
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "array size too big");
    } else if (!SPECIAL_CONST_P(first)) {
        VALUE iv = UNWRAP(korb_funcall(c, c->sp_top, first, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
        /* respond_to? is a GC point — re-read first from its arg slot before
         * dispatching to_int (and for the error message below). */
        first = argv[0];
        if (RTEST(iv)) {
            VALUE n = UNWRAP(korb_funcall(c, c->sp_top, first, korb_intern("to_int"), 0, NULL));
            if (!FIXNUM_P(n)) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion of (special) into Integer");
            }
            size = FIX2LONG(n);
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Integer",
                       korb_id_name(korb_class_of_class(first)->name));
        }
    } else {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion into Integer");
    }
    if (size < 0) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "negative array size");
    }
    if (size > (1L << 30)) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "array size too big");
    }
    if (argc == 2 && c->current_block) {
        /* CRuby: when both default and block are given, the block wins
         * and a warning is emitted.  Use the block. */
    }
    if (c->current_block) {
        /* Park the receiver array across the per-element korb_yield: yield
         * runs the block body at a lower sp_top, shrinking the GC scan range
         * below sp[-argc-1], so even the staging slot goes stale.  The frame
         * chain is always walked, so fr.last_line keeps self alive. */
        KORB_ARY_YIELD_FRAME(c, fr, sp[-argc - 1]);
        for (long i = 0; i < size; i++) {
            VALUE iv = INT2FIX(i);
            RESULT _y = korb_yield(c, 1, &iv);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            korb_ary_push(c, c->sp_top, fr.last_line, _y.value);
        }
        VALUE result = fr.last_line;
        c->current_frame = fr.prev;
        return RESULT_OK(result);
    } else {
        /* R5: re-read the default from its argv slot each iter (push-grow GC
         * can move it if it's a heap object), and re-read self (the receiver)
         * from its GC-scanned staging slot — the C-local goes stale too. */
        for (long i = 0; i < size; i++)
            korb_ary_push(c, c->sp_top, sp[-argc - 1], argc >= 2 ? argv[1] : Qnil);
    }
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT ary_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;

    /* Allocate an empty array of self's class.  Subclass support: when
     * `class A < Array; def initialize(a, b); self << a << b; end; end`,
     * we must dispatch `A#initialize` (which may differ from
     * Array#initialize), not the size/default short-cut.  Falling back
     * through korb_funcall_r ensures Ruby method-resolution applies.
     * Re-read self from sp[-argc-1] AFTER alloc since T_CLASS is
     * arena-allocated and can move under STRESS+PURGE. */
    VALUE arr = korb_ary_new(c, c->sp_top);
    VALUE self = sp[-argc - 1];
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS) {
        ((struct korb_array *)arr)->basic.klass = self;
    }
    /* Stage [arr, argv...] on sp.  CRITICAL: bump c->sp_top past the staging
     * so the AST dispatcher's [prev_sp, new_sp) zero-fill on return
     * doesn't clobber arr at sp[0].  Read back from sp[0] after dispatch
     * since GC may have moved arr (the C-local goes stale). */
    sp[0] = arr;
    for (int i = 0; i < argc; i++) sp[1 + i] = argv[i];
    VALUE *prev_sp = c->sp_top;
    c->sp_top = sp + 1 + argc;
    UNWRAP(korb_funcall_r(c, c->sp_top, arr, korb_intern("initialize"), argc, sp + 1));
    arr = sp[0];
    c->sp_top = prev_sp;
    return RESULT_OK(arr);
}

RESULT ary_hash_content(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_ARRAY) return RESULT_OK(INT2FIX(0));
    struct korb_array *a = (struct korb_array *)self;
    uint64_t h = 0xcbf29ce484222325ULL;  /* FNV-1a init */
    for (long i = 0; i < a->len; i++) {
        VALUE elt = korb_ary_items(a)[i];
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
    return RESULT_OK(INT2FIX(r >> 1));
}

/* ---------- Array#dig ----------
 * Walks a chain of indices: a.dig(i, j, k) == a[i][j][k], returning nil
 * the moment any intermediate is nil.  After the first hop it dispatches
 * the rest via #dig so Hash/Struct chains compose. */
static RESULT ary_dig(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments to dig (0 for 1+)");
    }
    /* Index must be Integer (or convertible via #to_int) — non-numeric
     * raises TypeError (CRuby semantics). */
    if (!FIXNUM_P(argv[0]) && (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_BIGNUM)) {
        VALUE klass_v = (VALUE)korb_class_of_class(argv[0]);
        if (klass_v && korb_class_find_method((struct korb_class *)klass_v,
                                                korb_intern("to_int"))) {
            VALUE coerced = UNWRAP(korb_funcall(c, c->sp_top, argv[0], korb_intern("to_int"), 0, NULL));
            argv[0] = coerced;
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Integer",
                       korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    /* R5: self may have moved across the to_int funcall above — re-read it
     * from its GC slot. */
    VALUE first = korb_ary_aref(sp[-argc - 1], FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 0);
    if (argc == 1) return RESULT_OK(first);
    if (NIL_P(first)) return RESULT_OK(Qnil);
    /* Intermediate must respond to #dig — else TypeError. */
    VALUE next_klass = (VALUE)korb_class_of_class(first);
    if (!next_klass || !korb_class_find_method((struct korb_class *)next_klass,
                                                 korb_intern("dig"))) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "%s does not have #dig method",
                   korb_id_name(korb_class_of_class(first)->name));
    }
    return korb_funcall(c, c->sp_top, first, korb_intern("dig"), argc - 1, argv + 1);
}

/* ---------- Array#take_while / drop_while ---------- */
static RESULT ary_take_while(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park result (fr.last_line) + receiver (fr.last_match) across the
     * per-element korb_yield via the frame chain (see ary_map).  Re-read the
     * length each step so the block may grow the array (CRuby semantics). */
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (!RTEST(_y.value)) break;
        korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_aref(fr.last_match, i));
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT ary_drop_while(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park the receiver (fr.last_match) across the per-element korb_yield;
     * the result is built afterward (no yield in that loop).  Re-read the
     * length each step so the block may grow the array (CRuby semantics). */
    long i = 0;
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    for (; i < korb_ary_len(fr.last_match); i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (!RTEST(_y.value)) break;
    }
    fr.last_line = korb_ary_new(c, c->sp_top);
    for (; i < korb_ary_len(fr.last_match); i++) {
        korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_aref(fr.last_match, i));
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* ---------- Array#flat_map ----------
 * Concatenates one level of nesting: if the block returns an Array the
 * elements are appended; otherwise the value itself is appended.
 * Previously aliased to #map, which is wrong for the common
 * `[[1,2],[3,4]].flat_map { |x| x }` shape. */
static RESULT ary_flat_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park result (fr.last_line) + receiver (fr.last_match) across the
     * per-element korb_yield via the frame chain (see ary_map).  The yielded
     * value is parked at sp[0] across the inner push-grow loop (no yield
     * there, so an sp slot under a reservation is safe — the pushes self-
     * publish c->sp_top to cover it). */
    long alen = korb_ary_len(self);
    KORB_ARY_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    for (long i = 0; i < alen; i++) {
        VALUE v = korb_ary_aref(fr.last_match, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        sp[0] = _y.value;
        c->sp_top = sp + 1;
        if (!SPECIAL_CONST_P(sp[0]) && BUILTIN_TYPE(sp[0]) == T_ARRAY) {
            long malen = korb_ary_len(sp[0]);
            for (long j = 0; j < malen; j++) {
                korb_ary_push(c, sp + 1, fr.last_line, korb_ary_aref(sp[0], j));
            }
        } else {
            korb_ary_push(c, sp + 1, fr.last_line, sp[0]);
        }
        c->sp_top = sp;
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* ---------- first(n) / last(n) overloads ----------
 * Existing ary_first/ary_last only handle the zero-arg form.  The
 * one-arg form returns up to n leading / trailing elements as a new
 * array; n > size yields the whole array, n == 0 an empty array. */
static RESULT ary_first_n(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_array *a = (struct korb_array *)self;
    if (argc > 1) {
        return korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 0..1)", argc);
    }
    if (argc < 1) return RESULT_OK(a->len == 0 ? Qnil : korb_ary_items(a)[0]);
    /* Integer / Bignum: convert to long; out-of-long-range Bignum
     * counts as a too-big size (CRuby raises RangeError there). */
    long n;
    VALUE arg = argv[0];
    if (!FIXNUM_P(arg) && (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(arg)) {
            RESULT _tr = korb_funcall(c, c->sp_top, arg, korb_intern("to_int"), 0, NULL);
            if (_tr.state == KORB_RAISE) {
                /* swallow NoMethodError, propagate other errors.  Hoist the
                 * NoMethodError intern BEFORE reading object_class (its GC
                 * would otherwise leave object_class stale via C arg-eval
                 * order) and park the in-flight exception across that GC. */
                ID nme_id = korb_intern("NoMethodError");
                sp[0] = _tr.value;
                c->sp_top = sp + 1;
                VALUE eNo = korb_const_get(KORB_VM(c)->object_class, nme_id);
                VALUE bang = sp[0];
                c->sp_top = sp;
                if (SPECIAL_CONST_P(bang) || SPECIAL_CONST_P(eNo) ||
                    BUILTIN_TYPE(eNo) != T_CLASS) return RESULT_OK(Qnil);
                struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                bool is_nm = false;
                for (struct korb_class *kk = bk; kk; kk = kk->super) {
                    if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                }
                if (!is_nm) { _tr.value = bang; return _tr; }
                /* swallowed; fall through with the (possibly forwarded) arg */
                arg = argv[0];
            } else if (_tr.state != KORB_NORMAL) {
                return _tr;
            } else {
                arg = _tr.value;
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
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR, "bignum too big to convert into 'long'");
        }
    } else {
        return korb_raise_type_error(c, "no implicit conversion from %s into Integer",
                              korb_id_name(korb_class_of_class(argv[0])->name));
    }
    if (n < 0) {
        return korb_raise_argument_error(c, "negative array size");
    }
    /* R5: a/self went stale across the to_int funcall — re-derive from the
     * GC-scanned staging slot (sp[-argc-1]), not the stale C-local. */
    self = sp[-argc - 1];
    long alen = korb_ary_len(self);
    if (n > alen) n = alen;
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    {
        struct korb_array *aa = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(aa)[i]);
    }
    return RESULT_OK(sp[0]);
}

static RESULT ary_last_n(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_array *a = (struct korb_array *)self;
    if (argc > 1) {
        return korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 0..1)", argc);
    }
    if (argc < 1) return RESULT_OK(a->len == 0 ? Qnil : korb_ary_items(a)[a->len - 1]);
    long n;
    VALUE arg = argv[0];
    if (!FIXNUM_P(arg) && (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_BIGNUM)) {
        if (!SPECIAL_CONST_P(arg)) {
            RESULT _tr = korb_funcall(c, c->sp_top, arg, korb_intern("to_int"), 0, NULL);
            if (_tr.state == KORB_RAISE) {
                /* Same intern-hoist + exception-park as ary_first_n: the
                 * NoMethodError intern's GC must not leave object_class /
                 * bang stale. */
                ID nme_id = korb_intern("NoMethodError");
                sp[0] = _tr.value;
                c->sp_top = sp + 1;
                VALUE eNo = korb_const_get(KORB_VM(c)->object_class, nme_id);
                VALUE bang = sp[0];
                c->sp_top = sp;
                if (SPECIAL_CONST_P(bang) || SPECIAL_CONST_P(eNo) ||
                    BUILTIN_TYPE(eNo) != T_CLASS) return RESULT_OK(Qnil);
                struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                bool is_nm = false;
                for (struct korb_class *kk = bk; kk; kk = kk->super) {
                    if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                }
                if (!is_nm) { _tr.value = bang; return _tr; }
                /* swallowed; fall through with the (possibly forwarded) arg */
                arg = argv[0];
            } else if (_tr.state != KORB_NORMAL) {
                return _tr;
            } else {
                arg = _tr.value;
            }
        }
    }
    if (FIXNUM_P(arg)) {
        n = FIX2LONG(arg);
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_BIGNUM) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "bignum too big to convert into 'long'");
    } else {
        return korb_raise_type_error(c, "no implicit conversion from %s into Integer",
                              korb_id_name(korb_class_of_class(argv[0])->name));
    }
    if (n < 0) {
        return korb_raise_argument_error(c, "negative array size");
    }
    /* R5: a/self went stale across the to_int funcall — re-derive from the
     * GC-scanned staging slot (sp[-argc-1]), not the stale C-local. */
    self = sp[-argc - 1];
    long alen = korb_ary_len(self);
    if (n > alen) n = alen;
    long start = alen - n;
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    {
        struct korb_array *aa = (struct korb_array *)sp[-argc - 1];
        for (long i = start; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(aa)[i]);
    }
    return RESULT_OK(sp[0]);
}

/* ---------- Array#shuffle ----------
 * Fisher–Yates over a copy.  Uses rand(3); good enough for tests and the
 * occasional `.sample` cousin (already implemented).  Doesn't mutate self. */
static RESULT ary_shuffle(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* R5: pre-sized copy parked at sp[0]; re-derive source after new_capa.  The
     * Fisher-Yates shuffle uses only rand() (no GC), so ra stays valid. */
    long alen = korb_ary_len(self);
    sp[0] = korb_ary_new_capa(c, sp + 1, alen);
    {
        struct korb_array *a = (struct korb_array *)sp[-argc - 1];
        for (long i = 0; i < alen; i++) korb_ary_push(c, sp + 1, sp[0], korb_ary_items(a)[i]);
    }
    struct korb_array *ra = (struct korb_array *)sp[0];
    for (long i = ra->len - 1; i > 0; i--) {
        long j = (long)(((unsigned long)rand()) % (unsigned long)(i + 1));
        VALUE tmp = korb_ary_items(ra)[i];
        korb_ary_items(ra)[i] = korb_ary_items(ra)[j];
        korb_ary_items(ra)[j] = tmp;
    }
    return RESULT_OK(sp[0]);
}

/* ---------- Array#each_cons(n) ----------
 * Sliding window of size n.  No block: returns Array<Array> of all
 * windows (koruby has no Enumerator).  With block: yields each window. */
static RESULT ary_each_cons(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(Qnil);
    long n = FIX2LONG(argv[0]);
    long alen = korb_ary_len(self);
    bool has_block = korb_block_given(c);
    /* Park out (fr.last_line) + source receiver (fr.last_match); the current
     * window is built at sp[0] (reserved across its push-grow). */
    KORB_ARY_YIELD_FRAME(c, fr, has_block ? Qnil : korb_ary_new(c, c->sp_top));
    fr.last_match = sp[-argc - 1];
    if (n <= 0 || n > alen) {
        VALUE ret = has_block ? Qnil : fr.last_line;
        c->current_frame = fr.prev;
        return RESULT_OK(ret);
    }
    for (long i = 0; i + n <= alen; i++) {
        sp[0] = korb_ary_new_capa(c, sp + 1, n);
        c->sp_top = sp + 1;
        for (long j = 0; j < n; j++) {
            korb_ary_push(c, sp + 1, sp[0], korb_ary_aref(fr.last_match, i + j));
        }
        if (has_block) {
            RESULT _y = korb_yield(c, 1, &sp[0]);
            if (_y.state != KORB_NORMAL) { c->sp_top = sp; c->current_frame = fr.prev; return _y; }
        } else {
            korb_ary_push(c, sp + 1, fr.last_line, sp[0]);
        }
        c->sp_top = sp;
    }
    VALUE ret = has_block ? Qnil : fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(ret);
}

/* ---------- Array#minmax_by ----------
 * Returns [min_elem, max_elem] keyed by the block's return value;
 * [nil, nil] for an empty array. */
static RESULT ary_minmax_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    long alen = korb_ary_len(self);
    if (alen == 0) {
        VALUE r = korb_ary_new_capa(c, c->sp_top, 2);
        korb_ary_push(c, c->sp_top, r, Qnil);
        korb_ary_push(c, c->sp_top, r, Qnil);
        return RESULT_OK(r);
    }
    /* Six roots span the per-element korb_yield + <=> funcalls: running
     * min_e/min_k, max_e/max_k, the source receiver and the per-iter key k.
     * Park them in three chained synthetic frames (frame chain always
     * scanned): fmin{min_e,min_k}, fmax{max_e,max_k}, frcv{receiver,k}. */
    KORB_ARY_YIELD_FRAME(c, fmin, korb_ary_aref(sp[-argc - 1], 0));  /* min_e */
    KORB_ARY_YIELD_FRAME(c, fmax, korb_ary_aref(sp[-argc - 1], 0));  /* max_e */
    KORB_ARY_YIELD_FRAME(c, frcv, sp[-argc - 1]);                    /* receiver */
    RESULT _y0 = korb_yield(c, 1, &fmin.last_line);
    if (_y0.state != KORB_NORMAL) { c->current_frame = fmin.prev; return _y0; }
    fmin.last_match = _y0.value;   /* min_k */
    fmax.last_match = _y0.value;   /* max_k */
    for (long i = 1; i < alen; i++) {
        VALUE v = korb_ary_aref(frcv.last_line, i);
        RESULT _y = korb_yield(c, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fmin.prev; return _y; }
        frcv.last_match = _y.value;   /* k, parked across the funcalls */
        RESULT _cmin = korb_funcall(c, c->sp_top, frcv.last_match, korb_intern("<=>"), 1, &fmin.last_match);
        if (_cmin.state != KORB_NORMAL) { c->current_frame = fmin.prev; return _cmin; }
        if (FIXNUM_P(_cmin.value) && FIX2LONG(_cmin.value) < 0) {
            fmin.last_line = korb_ary_aref(frcv.last_line, i); fmin.last_match = frcv.last_match;
        }
        RESULT _cmax = korb_funcall(c, c->sp_top, frcv.last_match, korb_intern("<=>"), 1, &fmax.last_match);
        if (_cmax.state != KORB_NORMAL) { c->current_frame = fmin.prev; return _cmax; }
        if (FIXNUM_P(_cmax.value) && FIX2LONG(_cmax.value) > 0) {
            fmax.last_line = korb_ary_aref(frcv.last_line, i); fmax.last_match = frcv.last_match;
        }
    }
    VALUE r = korb_ary_new_capa(c, c->sp_top, 2);
    korb_ary_push(c, c->sp_top, r, fmin.last_line);
    korb_ary_push(c, c->sp_top, r, fmax.last_line);
    c->current_frame = fmin.prev;
    return RESULT_OK(r);
}

/* ---------- Array#bsearch ----------
 * Find-minimum mode only (block returns boolean).  Assumes the array is
 * sorted and the block result transitions from false to true exactly
 * once; returns the first true element, nil if all are false. */
static RESULT ary_bsearch(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE arg = korb_id2sym(korb_intern("bsearch"));
        VALUE e = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &arg));
        /* bsearch's returned Enumerator has unknown size (CRuby semantics). */
        korb_ivar_set(e, korb_intern("@__size"), Qnil);
        return RESULT_OK(e);
    }
    /* Park the candidate `found` (fr.last_line) + the source receiver
     * (fr.last_match) across the per-probe korb_yield (frame chain scanned). */
    long alen = korb_ary_len(self);
    long lo = 0, hi = alen;
    KORB_ARY_YIELD_FRAME(c, fr, Qnil);   /* found */
    fr.last_match = sp[-argc - 1];
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        VALUE probe = korb_ary_aref(fr.last_match, mid);
        RESULT _y = korb_yield(c, 1, &probe);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        VALUE r = _y.value;
        if (TRUE_P(r) || FALSE_P(r) || NIL_P(r)) {
            /* Find-minimum mode: return the first element for which the
             * block returned a truthy value. */
            if (RTEST(r)) { fr.last_line = korb_ary_aref(fr.last_match, mid); hi = mid; }
            else lo = mid + 1;
        } else if (FIXNUM_P(r) || (!SPECIAL_CONST_P(r) &&
                                    (BUILTIN_TYPE(r) == T_BIGNUM ||
                                     BUILTIN_TYPE(r) == T_FLOAT)) ||
                   FLONUM_P(r)) {
            /* Find-any mode: 0 = match; negative = answer is right of mid;
             * positive = answer is left of mid. */
            double d;
            if (FIXNUM_P(r)) d = (double)FIX2LONG(r);
            else d = korb_num2dbl(r);
            if (d == 0.0) { VALUE m = korb_ary_aref(fr.last_match, mid); c->current_frame = fr.prev; return RESULT_OK(m); }
            if (d < 0.0) lo = mid + 1;
            else hi = mid;
        } else {
            c->current_frame = fr.prev;
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "wrong argument type %s (must be numeric, true, false or nil)",
                       SPECIAL_CONST_P(r) ? "(special)"
                           : korb_id_name(korb_class_of_class(r)->name));
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

