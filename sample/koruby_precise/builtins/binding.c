/* Binding object — captures the lexical state of a frame at the point
 * `binding` is called, supporting CRuby-compatible
 * local_variable_get/set/defined?/local_variables/eval semantics.
 *
 * The fp pointer is captured into the LIVE caller frame.  Reads and
 * writes go directly through that fp, so updates from inside the
 * binding propagate back to the caller (and vice versa) — matching
 * CRuby's heap-promoted env semantics.
 *
 * Caller-frame liveness: we keep fp as a raw pointer.  Once the caller
 * returns, fp is stale; that case isn't tracked.  Typical use is
 * binding-then-eval-then-discard so this is fine in practice.  New
 * locals introduced via local_variable_set / eval go in the slot area
 * past the caller's locals_cnt (best-effort) or in the extras Hash if
 * fp space runs out.
 */

extern struct korb_proc *running_block;
extern struct Node *korb_g_program_body;
extern ID *korb_body_local_names(struct Node *body);
extern NODE *koruby_parse_with_scope_line(const char *src, size_t len, const char *filename,
                                          const char **scope_locals, size_t scope_locals_n,
                                          int line_offset, char **err_msg);
extern NODE *koruby_parse_with_scope(const char *src, size_t len, const char *filename,
                                      const char **scope_locals, size_t scope_locals_n,
                                      char **err_msg);

/* Find slot index `idx` such that names[idx] == sym_id.  Returns -1
 * if not present. */
static int binding_find_slot(struct korb_binding *b, ID name_id) {
    for (uint32_t i = 0; i < b->names_cnt; i++) {
        if (b->names[i] == name_id) return (int)i;
    }
    return -1;
}

static void binding_append_name(struct korb_binding *b, ID name_id) {
    if (b->names_cnt + 1 > b->names_capa) {
        uint32_t newcap = b->names_capa ? b->names_capa * 2 : 8;
        ID *newp = korb_xmalloc(sizeof(ID) * newcap);
        if (b->names) {
            memcpy(newp, b->names, sizeof(ID) * b->names_cnt);
        }
        b->names = newp;
        b->names_capa = newcap;
    }
    b->names[b->names_cnt++] = name_id;
}

/* Coerce name to ID — accepts Symbol or String, or any object whose
 * #to_str returns a String (CRuby semantics).  Uses CTX from korb_vm
 * for the to_str dispatch. */
static ID binding_arg_to_id(VALUE arg, bool *ok) {
    *ok = true;
    if (SYMBOL_P(arg)) return korb_sym2id(arg);
    if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) {
        struct korb_string *s = (struct korb_string *)arg;
        return korb_intern_n(s->ptr, s->len);
    }
    /* Try #to_str on anything else — Mock / DelegateString / etc.
     * routes here.  Use respond_to? to filter so non-stringy objects
     * fall through to error rather than NoMethodError. */
    if (!SPECIAL_CONST_P(arg) && korb_vm && korb_vm->current_ctx) {
        CTX *c = korb_vm->current_ctx;
        VALUE rt = korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) { c->state = KORB_NORMAL; c->state_value = Qnil; rt = Qfalse; }
        if (RTEST(rt)) {
            VALUE s = korb_funcall(c, arg, korb_intern("to_str"), 0, NULL);
            if (c->state == KORB_NORMAL && !SPECIAL_CONST_P(s) &&
                BUILTIN_TYPE(s) == T_STRING) {
                struct korb_string *str = (struct korb_string *)s;
                return korb_intern_n(str->ptr, str->len);
            }
        }
    }
    *ok = false;
    return 0;
}

/* Allocate a new korb_binding and populate from the calling frame. */
static struct korb_binding *binding_alloc_from(CTX *c, VALUE recv) {
    /* Inside a Binding#eval body, `binding` should return a fresh
     * binding cloned from the eval's binding so the inner binding sees
     * eval-introduced lvars (e.g. `eval("c=2; binding.local_variables")`
     * must include :c).  Clone names + extras from the eval binding. */
    if (c->current_eval_binding) {
        struct korb_binding *src = (struct korb_binding *)c->current_eval_binding;
        struct korb_binding *b = korb_xcalloc(1, sizeof(*b));
        b->basic.head.flags = T_DATA;
        b->basic.klass = korb_vm ? (VALUE)korb_vm->binding_class : 0;
        b->self = src->self;
        b->cref = src->cref;
        b->method_name = src->method_name;
        b->source_file = src->source_file;
        b->source_line = src->source_line;
        b->extra_vars = Qnil;
    b->outer_vars = Qnil;
    b->outer_names_cnt = 0;
        if (!NIL_P(src->extra_vars) && BUILTIN_TYPE(src->extra_vars) == T_HASH) {
            b->extra_vars = korb_hash_new();
            struct korb_hash *sh = (struct korb_hash *)src->extra_vars;
            for (struct korb_hash_entry *e = sh->first; e; e = e->next) {
                korb_hash_aset(b->extra_vars, e->key, e->value);
            }
        }
        for (uint32_t i = 0; i < src->names_cnt; i++) {
            binding_append_name(b, src->names[i]);
        }
        if (src->fp && b->names_cnt > 0) {
            VALUE *heap = korb_xmalloc(sizeof(VALUE) * (b->names_cnt + 16));
            for (uint32_t i = 0; i < b->names_cnt; i++) {
                heap[i] = src->fp[src->base + i];
                if (UNDEF_P(heap[i])) heap[i] = Qnil;
            }
            for (uint32_t i = b->names_cnt; i < b->names_cnt + 16; i++) heap[i] = Qnil;
            b->fp = heap;
            b->base = 0;
        }
        b->live_fp = src->live_fp;
        b->live_base = src->live_base;
        b->live_frame_id = src->live_frame_id;
        return b;
    }
    struct korb_binding *b = korb_xcalloc(1, sizeof(*b));
    b->basic.head.flags = T_DATA;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->binding_class : 0;
    /* Binding's self should be the caller's lexical self (the one who
     * SYNTACTICALLY called `binding`), not the cfunc receiver.  When
     * called via `obj.send(:binding)`, the receiver is `obj` but the
     * caller's self may be something else.  Priority:
     *   1. running_block.self — closest lexical scope; for
     *      Class.new { ... } the block's self is the new class (not
     *      the method's caller).
     *   2. current_frame->self — calling AST method's self.
     *   3. main_obj — top-level fallback. */
    if (running_block) {
        b->self = running_block->self;
    } else if (c->current_frame && c->current_frame->method) {
        b->self = c->current_frame->self;
    } else if (korb_vm) {
        b->self = korb_vm->main_obj;
    } else {
        b->self = recv;
    }
    b->extra_vars = Qnil;
    b->outer_vars = Qnil;
    b->outer_names_cnt = 0;

    /* The cfunc is one step above the user's frame (cfunc prologue
     * doesn't push a frame, so c->current_frame IS the caller's
     * AST-method frame).  For block context, the running block's
     * locals shadow enclosing method/block locals; we capture the
     * inner block first, then merge in any names from the enclosing
     * method that aren't already present so binding can see both. */
    ID *names = NULL;
    VALUE *fp = c->fp;
    uint32_t base = 0;
    if (running_block && running_block->body) {
        names = korb_body_local_names(running_block->body);
        base = running_block->param_base;
    }
    bool inside_method = false;
    if (!names && c->current_frame && c->current_frame->method &&
        c->current_frame->method->type == KORB_METHOD_AST) {
        names = c->current_frame->method->u.ast.local_names;
        if (c->current_frame->fp) fp = c->current_frame->fp;
        base = 0;
        inside_method = true;  /* even if names == NULL — empty-locals method */
    }
    if (!names && !inside_method && korb_g_program_body) {
        names = korb_body_local_names(korb_g_program_body);
        base = 0;
    }
    /* Crefs along c->cref are typically stack-allocated (class body /
     * module body / class << self push `struct korb_cref new_cref;` on
     * the C stack and link it).  When this binding outlives the stack
     * frame that pushed those crefs, the saved pointer dangles and
     * a later `eval(..., binding)` that walks cref->klass reads
     * uninitialized memory → SEGV.  Deep-copy onto the heap. */
    {
        struct korb_cref **dst = &b->cref;
        for (struct korb_cref *src = c->cref; src; src = src->prev) {
            struct korb_cref *cp = korb_xmalloc(sizeof(*cp));
            cp->klass = src->klass;
            cp->prev  = NULL;
            *dst = cp;
            dst = &cp->prev;
        }
    }
    if (c->current_frame && c->current_frame->method) {
        b->method_name = c->current_frame->method->name;
    }
    /* Capture source file/line of the `binding` call site.  The cfunc
     * prologue records the dispatched callsite into c->last_cfunc_callsite
     * before invoking us, so its node head holds the line we want. */
    b->source_file = NULL;
    b->source_line = 0;
    if (c->last_cfunc_callsite) {
        b->source_file = c->last_cfunc_callsite->head.source_file;
        b->source_line = c->last_cfunc_callsite->head.line;
    }
    if (!b->source_file && c->current_file) {
        b->source_file = c->current_file;
    }

    /* Copy primary names (innermost scope). */
    if (names) {
        for (size_t i = 0; names[i] != 0; i++) {
            binding_append_name(b, names[i]);
        }
    }

    /* When called from inside a Kernel#eval body, the eval'd program may
     * have introduced new lvars (e.g. `eval("c=2; binding.local_variables")`
     * — :c is eval-introduced).  Pull those into extras so they appear at
     * the front of local_variables (CRuby's innermost-first order). */
    if (c->current_eval_program_body) {
        ID *eval_names = korb_body_local_names(c->current_eval_program_body);
        if (eval_names) {
            if (NIL_P(b->extra_vars)) b->extra_vars = korb_hash_new();
            for (size_t i = 0; eval_names[i] != 0; i++) {
                if (binding_find_slot(b, eval_names[i]) >= 0) continue;
                binding_append_name(b, eval_names[i]);
                /* Default to nil — value is whatever fp slot holds, but
                 * we don't have a stable way to read the eval body's
                 * temp slot here.  The eval body has already executed
                 * and its program-level lvars live in fp; copy them in
                 * if fp is wide enough. */
                VALUE val = Qnil;
                if (c->fp) {
                    /* eval body uses caller's fp at offset (scope_locals_n
                     * + ...).  The eval body's local_names index gives us
                     * the slot directly (since eval mode skips node_scope
                     * fp shift, names are at fp[i]). */
                    val = c->fp[i];
                    if (UNDEF_P(val)) val = Qnil;
                }
                korb_hash_aset(b->extra_vars, korb_id2sym(eval_names[i]), val);
            }
        }
    }

    /* Snapshot the live frame slots into a heap buffer so the binding
     * survives the caller's frame return.  Also remember the original
     * fp/base + frame_id so local_variable_set / eval write-through to
     * the live frame (CRuby's heap-promote semantics, approximated). */
    b->live_fp = fp;
    b->live_base = base;
    b->live_frame_id = (c->current_frame ? c->current_frame->frame_id : 0);
    /* Register on the live frame so we get a final-state snapshot
     * when the frame epilogue runs.  Without this, `bind = binding;
     * b = 1; @x = bind` produces a binding where bind.eval('b')
     * sees the moment-of-take value (nil) rather than the final
     * value (1) — losing one of CRuby's heap-promote properties. */
    if (c->current_frame) {
        b->next_in_frame = (struct korb_binding *)c->current_frame->bindings_head;
        c->current_frame->bindings_head = b;
    }
    if (fp && b->names_cnt > 0) {
        VALUE *heap = korb_xmalloc(sizeof(VALUE) * (b->names_cnt + 16));
        for (uint32_t i = 0; i < b->names_cnt; i++) {
            heap[i] = fp[base + i];
            if (UNDEF_P(heap[i])) heap[i] = Qnil;
        }
        for (uint32_t i = b->names_cnt; i < b->names_cnt + 16; i++) heap[i] = Qnil;
        b->fp = heap;
        b->base = 0;
    } else {
        b->fp = fp;
        b->base = base;
    }

    /* Lexical inheritance — walk the lexical_parent_block chain.  Each
     * outer block contributes any lvars not already in our table.
     * Outer-block lvars are stored in b->outer_vars and counted via
     * outer_names_cnt so binding_local_variables shows them at the END
     * of the list (innermost-first ordering — CRuby compat). */
    if (running_block) {
        if (NIL_P(b->outer_vars)) b->outer_vars = korb_hash_new();
        struct korb_proc *parent = running_block->lexical_parent_block;
        while (parent && parent->body) {
            ID *parent_names = korb_body_local_names(parent->body);
            if (parent_names) {
                VALUE *src_fp = c->fp ? c->fp : parent->env;
                for (size_t i = 0; parent_names[i] != 0; i++) {
                    if (binding_find_slot(b, parent_names[i]) >= 0) continue;
                    binding_append_name(b, parent_names[i]);
                    b->outer_names_cnt++;
                    VALUE val = src_fp ? src_fp[parent->param_base + i] : Qnil;
                    if (UNDEF_P(val)) val = Qnil;
                    korb_hash_aset(b->outer_vars,
                                   korb_id2sym(parent_names[i]), val);
                }
            }
            parent = parent->lexical_parent_block;
        }
        /* Note: a toplevel proc's outer scope is its creation-time
         * program body, not necessarily korb_g_program_body (which is
         * the initially-loaded script — when run_rubyspec.rb loads a
         * spec file, blocks created inside the spec see the spec's
         * toplevel scope, not the runner's).  We don't currently track
         * per-proc creation_program_body, so we skip top-level outer
         * walk and miss those lvars in the binding.  Acceptable
         * trade-off vs leaking the runner's lvars into specs. */
        if (running_block->defining_method &&
            running_block->defining_method->type == KORB_METHOD_AST) {
            struct korb_method *dm = (struct korb_method *)running_block->defining_method;
            ID *outer_names = dm->u.ast.local_names;
            if (outer_names) {
                VALUE *outer_fp = NULL;
                for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
                    if (f->method == dm) { outer_fp = f->fp; break; }
                }
                for (size_t i = 0; outer_names[i] != 0; i++) {
                    if (binding_find_slot(b, outer_names[i]) >= 0) continue;
                    binding_append_name(b, outer_names[i]);
                    b->outer_names_cnt++;
                    if (outer_fp) {
                        korb_hash_aset(b->outer_vars,
                                       korb_id2sym(outer_names[i]),
                                       outer_fp[i]);
                    }
                }
            }
        }
    }
    return b;
}

/* Kernel#binding — return a Binding bound to caller's frame. */
static VALUE kernel_binding_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    return (VALUE)binding_alloc_from(c, self);
}

/* Binding#receiver */
static VALUE binding_receiver(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    return ((struct korb_binding *)self)->self;
}

/* Read a name's value: extras Hash takes precedence (binding-introduced
 * locals live there because caller's fp temp slots can be reused), then
 * fall back to fp[base + slot] for caller-frame names, then to outer_vars
 * for lexical-parent names. */
static VALUE binding_read_name(struct korb_binding *b, ID name_id) {
    VALUE sym = korb_id2sym(name_id);
    if (!NIL_P(b->extra_vars) && BUILTIN_TYPE(b->extra_vars) == T_HASH) {
        struct korb_hash *h = (struct korb_hash *)b->extra_vars;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (e->key == sym) return e->value;
        }
    }
    int idx = binding_find_slot(b, name_id);
    if (idx >= 0) {
        /* If it's a lexical-parent name (in the outer-trailing range),
         * read from outer_vars instead of fp. */
        uint32_t outer_start = (b->names_cnt > b->outer_names_cnt)
                                ? b->names_cnt - b->outer_names_cnt : 0;
        if ((uint32_t)idx >= outer_start &&
            !NIL_P(b->outer_vars) && BUILTIN_TYPE(b->outer_vars) == T_HASH) {
            struct korb_hash *oh = (struct korb_hash *)b->outer_vars;
            for (struct korb_hash_entry *e = oh->first; e; e = e->next) {
                if (e->key == sym) return e->value;
            }
        }
        if (b->fp) {
            VALUE v = b->fp[b->base + idx];
            return UNDEF_P(v) ? Qnil : v;
        }
    }
    return Qundef;
}

/* Binding#local_variable_get(name) */
static VALUE binding_local_variable_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    struct korb_binding *b = (struct korb_binding *)self;
    bool ok = false;
    ID name_id = binding_arg_to_id(argv[0], &ok);
    if (!ok) {
        korb_raise(c, NULL, "binding: name must be Symbol or String");
        return Qnil;
    }
    VALUE v = binding_read_name(b, name_id);
    if (UNDEF_P(v)) {
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "local variable '%s' is not defined for binding",
                   korb_id_name(name_id));
        return Qnil;
    }
    return v;
}

/* Validate a name as a legal local-variable identifier.
 *   - must start with [a-z_]
 *   - rest [a-zA-Z0-9_] (CRuby) — though we accept a bit more for
 *     non-ASCII names (e.g. "い") since `_` / Unicode letters are
 *     legitimate local names in Ruby.
 *   - $foo (global) and @foo (ivar) and "?foo" / special vars are
 *     rejected (CRuby raises NameError).
 */
static bool binding_valid_lvar_name(ID name_id) {
    const char *s = korb_id_name(name_id);
    if (!s || !s[0]) return false;
    /* First char must be lowercase letter or underscore (or non-ASCII). */
    unsigned char c0 = (unsigned char)s[0];
    if (!(c0 == '_' || (c0 >= 'a' && c0 <= 'z') || c0 >= 0x80)) return false;
    for (size_t i = 1; s[i]; i++) {
        unsigned char ci = (unsigned char)s[i];
        if (ci == '_' || (ci >= 'a' && ci <= 'z') ||
            (ci >= 'A' && ci <= 'Z') || (ci >= '0' && ci <= '9') ||
            ci >= 0x80) continue;
        return false;
    }
    return true;
}

/* Binding#local_variable_set(name, val) — write fp[base + slot]; if
 * the name isn't tracked yet, append a new slot past the existing
 * names.  Slots beyond the caller's frame area land in the extras
 * Hash to avoid corrupting the caller's temp slots.
 */
static VALUE binding_local_variable_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qnil;
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    struct korb_binding *b = (struct korb_binding *)self;
    bool ok = false;
    ID name_id = binding_arg_to_id(argv[0], &ok);
    if (!ok) {
        korb_raise(c, NULL, "binding: name must be Symbol or String");
        return Qnil;
    }
    if (!binding_valid_lvar_name(name_id)) {
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "wrong local variable name '%s' for binding",
                   korb_id_name(name_id));
        return Qnil;
    }
    int idx = binding_find_slot(b, name_id);
    if (idx >= 0) {
        if (b->fp) b->fp[b->base + idx] = argv[1];
        /* Write-through to live frame if it's still on the stack.
         * Detected via frame_id: walk c->current_frame chain looking
         * for a matching frame_id; only write if we find it (otherwise
         * the slot may have been reused for unrelated data). */
        if (b->live_fp && b->live_frame_id) {
            for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
                if (f->frame_id == b->live_frame_id) {
                    b->live_fp[b->live_base + idx] = argv[1];
                    break;
                }
            }
        }
        /* Mirror to extras when present so subsequent reads remain
         * stable even if caller reuses the underlying slot. */
        if (!NIL_P(b->extra_vars) && BUILTIN_TYPE(b->extra_vars) == T_HASH) {
            struct korb_hash *h = (struct korb_hash *)b->extra_vars;
            VALUE sym = korb_id2sym(name_id);
            for (struct korb_hash_entry *e = h->first; e; e = e->next) {
                if (e->key == sym) { e->value = argv[1]; break; }
            }
        }
        return argv[1];
    }
    /* New name: store in extras Hash to avoid clobbering caller temps. */
    if (NIL_P(b->extra_vars)) b->extra_vars = korb_hash_new();
    korb_hash_aset(b->extra_vars, korb_id2sym(name_id), argv[1]);
    binding_append_name(b, name_id);
    return argv[1];
}

/* Binding#local_variable_defined?(name) */
static VALUE binding_local_variable_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qfalse;
    struct korb_binding *b = (struct korb_binding *)self;
    bool ok = false;
    ID name_id = binding_arg_to_id(argv[0], &ok);
    if (!ok) return Qfalse;
    return KORB_BOOL(binding_find_slot(b, name_id) >= 0);
}

/* Binding#local_variables — Array of Symbol names.  CRuby orders the
 * list innermost-first:
 *   1. Set-introduced (extras Hash) — appear at the FRONT.
 *   2. Primary scope_locals — middle.
 *   3. Lexical-parent (outer_vars) — at the END.
 * This matches CRuby's "newest scope first, oldest last" rule.
 * Underscore-prefix names (`_`, `__foo`) are filtered (CRuby compat).
 */
static VALUE binding_local_variables_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return arr;
    struct korb_binding *b = (struct korb_binding *)self;
    VALUE extras = b->extra_vars;
    bool has_extras = (!NIL_P(extras) && BUILTIN_TYPE(extras) == T_HASH);
    /* 1. Extras first. */
    if (has_extras) {
        struct korb_hash *h = (struct korb_hash *)extras;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (!SYMBOL_P(e->key)) continue;
            const char *cname = korb_id_name(korb_sym2id(e->key));
            if (cname && cname[0] == '_' && cname[1] == 0) continue;
            if (cname && cname[0] == '_' && cname[1] == '_') continue;
            korb_ary_push(arr, e->key);
        }
    }
    /* 2. Primary names — first (names_cnt - outer_names_cnt) entries.
     * Skip those already covered by extras. */
    uint32_t primary_n = (b->names_cnt > b->outer_names_cnt)
                         ? b->names_cnt - b->outer_names_cnt : 0;
    for (uint32_t i = 0; i < primary_n; i++) {
        const char *cname = korb_id_name(b->names[i]);
        if (cname && cname[0] == '_' && cname[1] == 0) continue;
        if (cname && cname[0] == '_' && cname[1] == '_') continue;
        VALUE sym = korb_id2sym(b->names[i]);
        if (has_extras) {
            struct korb_hash *h = (struct korb_hash *)extras;
            bool seen = false;
            for (struct korb_hash_entry *e = h->first; e; e = e->next) {
                if (e->key == sym) { seen = true; break; }
            }
            if (seen) continue;
        }
        korb_ary_push(arr, sym);
    }
    /* 3. Outer (lexical-parent) names — last block of names[]. */
    for (uint32_t i = primary_n; i < b->names_cnt; i++) {
        const char *cname = korb_id_name(b->names[i]);
        if (cname && cname[0] == '_' && cname[1] == 0) continue;
        if (cname && cname[0] == '_' && cname[1] == '_') continue;
        VALUE sym = korb_id2sym(b->names[i]);
        if (has_extras) {
            struct korb_hash *h = (struct korb_hash *)extras;
            bool seen = false;
            for (struct korb_hash_entry *e = h->first; e; e = e->next) {
                if (e->key == sym) { seen = true; break; }
            }
            if (seen) continue;
        }
        korb_ary_push(arr, sym);
    }
    return arr;
}

/* Binding#eval implementation factored out so Kernel#eval can call
 * it directly when given a Binding as the second arg. */
VALUE binding_eval_via(CTX *c, struct korb_binding *b, VALUE *argv, int argc) {
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "binding.eval: argument must be a String");
        return Qnil;
    }
    struct korb_string *s = (struct korb_string *)argv[0];
    const char *filename = "(eval)";
    if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) && BUILTIN_TYPE(argv[1]) == T_STRING) {
        filename = ((struct korb_string *)argv[1])->ptr;
    }
    int line_offset = 0;
    if (argc >= 3 && FIXNUM_P(argv[2])) {
        line_offset = (int)FIX2LONG(argv[2]);
    }
    /* Build scope_locals from binding's names.  Always allocate (even
     * when empty) so koruby_parse_with_scope sees a non-NULL pointer
     * and treats this as eval-with-binding mode (skips node_scope
     * fp-shift). */
    const char **scope_locals = NULL;
    size_t scope_locals_n = b->names_cnt;
    {
        size_t alloc_n = scope_locals_n ? scope_locals_n : 1;
        scope_locals = korb_xmalloc(sizeof(char *) * alloc_n);
        for (uint32_t i = 0; i < b->names_cnt; i++) {
            scope_locals[i] = korb_id_name(b->names[i]);
        }
    }
    /* If the live caller frame is still on the stack, refresh our
     * heap snapshot from it so the eval body sees the latest values
     * the method has written.  Mirrors CRuby's heap-promoted env. */
    if (b->live_fp && b->live_frame_id) {
        for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
            if (f->frame_id == b->live_frame_id) {
                /* Refresh primary names only; binding-introduced names
                 * live in our heap snapshot. */
                /* names_cnt may include both primary and added.  The
                 * primary count = whatever was in fp at create time,
                 * but we don't track it separately.  Refresh all
                 * slots — values for added names from live_fp are
                 * meaningless but harmless (extras Hash overrides
                 * anyway). */
                for (uint32_t i = 0; i < b->names_cnt; i++) {
                    b->fp[b->base + i] = f->fp[b->live_base + i];
                }
                break;
            }
        }
    }
    /* Snapshot bind.names_cnt before parse so we know which names
     * are NEW after merging in the eval body's introduced lvars.
     * Also snapshot heap fp values so we can detect which slots the
     * eval body actually modified — those (and only those) get
     * write-through to the live caller frame. */
    uint32_t orig_names_cnt = b->names_cnt;
    VALUE pre_snapshot[orig_names_cnt > 0 ? orig_names_cnt : 1];
    if (b->fp) {
        for (uint32_t i = 0; i < orig_names_cnt; i++) {
            pre_snapshot[i] = b->fp[b->base + i];
        }
    }
    char *err_msg = NULL;
    NODE *ast = koruby_parse_with_scope_line(s->ptr, (size_t)s->len, filename,
                                              scope_locals, scope_locals_n,
                                              line_offset, &err_msg);
    if (err_msg) {
        VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SyntaxError"));
        korb_raise(c, (struct korb_class *)eSE, "%s", err_msg);
        return Qnil;
    }
    if (!ast) return Qnil;

    /* Pull the parsed program's full local-name list and merge any new
     * names into the binding's table.  This lets `bind.eval('x = 1')`
     * be followed by `bind.local_variable_get(:x) == 1`. */
    {
        ID *prog_names = korb_body_local_names(ast);
        if (prog_names) {
            for (size_t i = 0; prog_names[i] != 0; i++) {
                if (binding_find_slot(b, prog_names[i]) < 0) {
                    binding_append_name(b, prog_names[i]);
                }
            }
        }
    }

    /* Pre-populate fp slots for binding-introduced names from extras.
     * Caller's own slots already hold live values (we share fp).  After
     * eval returns we pull updated values back into extras so subsequent
     * local_variable_get sees them. */
    if (!NIL_P(b->extra_vars) && BUILTIN_TYPE(b->extra_vars) == T_HASH && b->fp) {
        struct korb_hash *h = (struct korb_hash *)b->extra_vars;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (!SYMBOL_P(e->key)) continue;
            ID id = korb_sym2id(e->key);
            int idx = binding_find_slot(b, id);
            if (idx >= 0) b->fp[b->base + idx] = e->value;
        }
    }

    /* Switch to the binding's fp / self / cref for the duration of
     * the eval body.  At return, restore. */
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    struct korb_cref *prev_cref = c->cref;
    const char *prev_file = c->current_file;
    void *prev_eval_binding = c->current_eval_binding;
    if (b->fp) c->fp = b->fp + b->base;
    c->self = b->self;
    if (b->cref) c->cref = b->cref;
    c->current_file = filename;
    c->current_eval_binding = (void *)b;

    extern struct Node *OPTIMIZE(struct Node *n);
    OPTIMIZE(ast);
    VALUE r = EVAL(c, ast, c->fp);

    c->current_eval_binding = prev_eval_binding;
    c->current_file = prev_file;
    c->cref = prev_cref;
    c->self = prev_self;
    c->fp = prev_fp;

    /* Write-through eval body's slot updates to the live frame, but
     * only for slots the eval body actually MODIFIED (heap value
     * differs from the pre-eval snapshot).  Untouched slots stay as
     * the live frame had them — avoids clobbering the caller's bind
     * lvar with binding's own snapshot of nil. */
    if (b->fp && b->live_fp && b->live_frame_id) {
        bool live = false;
        for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
            if (f->frame_id == b->live_frame_id) { live = true; break; }
        }
        if (live) {
            for (uint32_t i = 0; i < orig_names_cnt; i++) {
                if (b->fp[b->base + i] != pre_snapshot[i]) {
                    b->live_fp[b->live_base + i] = b->fp[b->base + i];
                }
            }
        }
    }

    /* Sync fp back into extras for binding-introduced names.  Names
     * not in the original caller frame (i.e. introduced by previous
     * local_variable_set / eval) live in extras as the source of
     * truth — caller's temp slots can be reused, so we have to
     * snapshot whatever the eval body wrote. */
    if (b->fp) {
        if (NIL_P(b->extra_vars)) b->extra_vars = korb_hash_new();
        for (uint32_t i = orig_names_cnt; i < b->names_cnt; i++) {
            VALUE v = b->fp[b->base + i];
            if (UNDEF_P(v)) v = Qnil;
            korb_hash_aset(b->extra_vars, korb_id2sym(b->names[i]), v);
        }
    }
    return r;
}

/* Binding#source_location — [file, line] of where the binding was
 * created. */
static VALUE binding_source_location(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    struct korb_binding *b = (struct korb_binding *)self;
    VALUE arr = korb_ary_new();
    const char *file = b->source_file ? b->source_file : "(eval)";
    int line = b->source_line ? b->source_line : 0;
    korb_ary_push(arr, korb_str_new_cstr(file));
    korb_ary_push(arr, INT2FIX(line));
    return arr;
}

/* Snapshot the frame's locals into each registered binding's heap.
 * Called from prologue_ast_*_inl just before c->fp is restored. */
void korb_binding_snapshot_frame(struct korb_frame *f) {
    struct korb_binding *b = (struct korb_binding *)f->bindings_head;
    while (b) {
        struct korb_binding *next = b->next_in_frame;
        if (b->fp && b->live_fp == f->fp) {
            uint32_t n = b->names_cnt;
            uint32_t copy_n = (n > f->locals_cnt) ? f->locals_cnt : n;
            for (uint32_t i = 0; i < copy_n; i++) {
                b->fp[b->base + i] = f->fp[b->live_base + i];
            }
            /* Frame is about to be popped — invalidate live_fp /
             * live_frame_id so subsequent get/set don't try to read
             * stale stack memory. */
            b->live_fp = NULL;
            b->live_frame_id = 0;
        }
        b->next_in_frame = NULL;
        b = next;
    }
    f->bindings_head = NULL;
}

/* Method dispatch for Binding#eval — receiver-bound wrapper. */
static VALUE binding_eval_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    return binding_eval_via(c, (struct korb_binding *)self, argv, argc);
}

/* Binding#dup / Binding#clone — deep-copy the names list and extras
 * Hash so subsequent local_variable_set / eval on the original
 * doesn't leak into the copy (and vice versa).  fp / self / cref /
 * method_name are shared (immutable for our purposes). */
static VALUE binding_dup_clone_impl(CTX *c, VALUE self, bool preserve_frozen, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return self;
    struct korb_binding *src = (struct korb_binding *)self;
    struct korb_binding *dst = korb_xcalloc(1, sizeof(*dst));
    dst->basic.head.flags = T_DATA;
    dst->basic.klass = src->basic.klass;
    /* Frozen handling: clone preserves source's frozen flag (with optional
     * `freeze: true/false` kwarg override); dup always produces unfrozen. */
    int freeze_arg = -1;
    if (preserve_frozen && argc >= 1 &&
        !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_HASH) {
        VALUE fk = korb_id2sym(korb_intern("freeze"));
        struct korb_hash *h = (struct korb_hash *)argv[0];
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (korb_eql(e->key, fk)) {
                if (e->value == Qfalse) freeze_arg = 0;
                else if (e->value == Qtrue) freeze_arg = 1;
                break;
            }
        }
    }
    if (freeze_arg == 1) {
        dst->basic.head.flags |= FL_FROZEN;
    } else if (freeze_arg == 0) {
        /* explicit unfreeze */
    } else if (preserve_frozen && (src->basic.head.flags & FL_FROZEN)) {
        dst->basic.head.flags |= FL_FROZEN;
    }
    dst->fp = src->fp;
    dst->base = src->base;
    dst->self = src->self;
    dst->cref = src->cref;
    dst->method_name = src->method_name;
    /* Names — deep copy. */
    if (src->names_cnt > 0) {
        dst->names = korb_xmalloc(sizeof(ID) * src->names_cnt);
        memcpy(dst->names, src->names, sizeof(ID) * src->names_cnt);
        dst->names_cnt = src->names_cnt;
        dst->names_capa = src->names_cnt;
    }
    /* Extras — deep copy each entry. */
    if (!NIL_P(src->extra_vars) && BUILTIN_TYPE(src->extra_vars) == T_HASH) {
        dst->extra_vars = korb_hash_new();
        struct korb_hash *sh = (struct korb_hash *)src->extra_vars;
        for (struct korb_hash_entry *e = sh->first; e; e = e->next) {
            korb_hash_aset(dst->extra_vars, e->key, e->value);
        }
    } else {
        dst->extra_vars = Qnil;
    }
    return (VALUE)dst;
}

static VALUE binding_clone_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    return binding_dup_clone_impl(c, self, true, argc, argv);
}
static VALUE binding_dup_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    return binding_dup_clone_impl(c, self, false, argc, argv);
}

/* Proc#binding — create a new Binding from the proc's captured env. */
static VALUE proc_binding_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    struct korb_proc *p = (struct korb_proc *)self;
    struct korb_binding *b = korb_xcalloc(1, sizeof(*b));
    b->basic.head.flags = T_DATA;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->binding_class : 0;
    b->self = p->self;
    b->cref = p->cref;
    b->extra_vars = Qnil;
    b->outer_vars = Qnil;
    b->source_file = NULL;
    b->source_line = 0;
    /* Names: from the proc's body local_names. */
    ID *names = p->body ? korb_body_local_names(p->body) : NULL;
    if (names) {
        for (size_t i = 0; names[i] != 0; i++) {
            binding_append_name(b, names[i]);
        }
    }
    /* fp: snapshot the proc's env (already heap-allocated for closures). */
    if (p->env && b->names_cnt > 0) {
        VALUE *heap = korb_xmalloc(sizeof(VALUE) * (b->names_cnt + 16));
        for (uint32_t i = 0; i < b->names_cnt; i++) {
            heap[i] = p->env[p->param_base + i];
            if (UNDEF_P(heap[i])) heap[i] = Qnil;
        }
        for (uint32_t i = b->names_cnt; i < b->names_cnt + 16; i++) heap[i] = Qnil;
        b->fp = heap;
        b->base = 0;
    } else {
        b->fp = p->env;
        b->base = p->param_base;
    }
    /* No live frame — proc's env is already heap-promoted. */
    b->live_fp = NULL;
    b->live_base = 0;
    b->live_frame_id = 0;
    return (VALUE)b;
}
