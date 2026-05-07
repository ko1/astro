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
    struct korb_binding *b = korb_xcalloc(1, sizeof(*b));
    b->basic.flags = T_DATA;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->binding_class : 0;
    b->self = recv;
    b->extra_vars = Qnil;

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
    if (!names && c->current_frame && c->current_frame->method &&
        c->current_frame->method->type == KORB_METHOD_AST) {
        names = c->current_frame->method->u.ast.local_names;
        if (c->current_frame->fp) fp = c->current_frame->fp;
        base = 0;
    }
    if (!names && korb_g_program_body) {
        names = korb_body_local_names(korb_g_program_body);
        base = 0;
    }
    b->fp = fp;
    b->base = base;
    b->cref = c->cref;
    if (c->current_frame && c->current_frame->method) {
        b->method_name = c->current_frame->method->name;
    }

    /* Copy primary names (innermost scope) — preserve slot indexing so
     * fp[base + i] hits the i-th local. */
    if (names) {
        for (size_t i = 0; names[i] != 0; i++) {
            binding_append_name(b, names[i]);
        }
    }

    /* Lexical inheritance — walk the lexical_parent_block chain.  Each
     * outer block contributes any lvars not already in our table.
     * Outer-block lvars get snapshot into extras (we don't know which
     * fp slot still holds them, since the outer block may be on a
     * different point of the stack).  After walking blocks, also
     * include defining_method's locals for the same reason. */
    if (running_block) {
        if (NIL_P(b->extra_vars)) b->extra_vars = korb_hash_new();
        struct korb_proc *parent = running_block->lexical_parent_block;
        while (parent && parent->body) {
            ID *parent_names = korb_body_local_names(parent->body);
            if (parent_names) {
                /* The currently-running block / lambda's env covers the
                 * entire closure chain: when a fresh-env-path block was
                 * called, the heap fp it allocated includes every outer
                 * slot up through env_size.  Any block created inside
                 * captures that same fp, so c->fp here holds live
                 * values for ALL ancestors' locals.  Read from c->fp
                 * with the parent's param_base offset rather than
                 * parent->env (which is the snapshot taken at parent's
                 * creation time and may be stale by now). */
                VALUE *src_fp = c->fp ? c->fp : parent->env;
                for (size_t i = 0; parent_names[i] != 0; i++) {
                    if (binding_find_slot(b, parent_names[i]) >= 0) continue;
                    binding_append_name(b, parent_names[i]);
                    VALUE val = src_fp ? src_fp[parent->param_base + i] : Qnil;
                    if (UNDEF_P(val)) val = Qnil;
                    korb_hash_aset(b->extra_vars,
                                   korb_id2sym(parent_names[i]), val);
                }
            }
            parent = parent->lexical_parent_block;
        }
        /* defining_method (the lexically-enclosing method) — its
         * locals should also be visible from inside the block. */
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
                    if (outer_fp) {
                        korb_hash_aset(b->extra_vars,
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
 * fall back to fp[base + slot] for caller-frame names. */
static VALUE binding_read_name(struct korb_binding *b, ID name_id) {
    if (!NIL_P(b->extra_vars) && BUILTIN_TYPE(b->extra_vars) == T_HASH) {
        struct korb_hash *h = (struct korb_hash *)b->extra_vars;
        VALUE sym = korb_id2sym(name_id);
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (e->key == sym) return e->value;
        }
    }
    int idx = binding_find_slot(b, name_id);
    if (idx < 0 || !b->fp) return Qundef;
    VALUE v = b->fp[b->base + idx];
    return UNDEF_P(v) ? Qnil : v;
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
 * list with binding-introduced (extras) names first, then the primary
 * captured names.  Iterate extras in reverse-insertion order (mirrors
 * CRuby's stack-of-frames behavior), then primary names in declaration
 * order. */
static VALUE binding_local_variables_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return arr;
    struct korb_binding *b = (struct korb_binding *)self;
    /* Build a "is in extras" bitset on the fly. */
    VALUE extras = b->extra_vars;
    bool has_extras = (!NIL_P(extras) && BUILTIN_TYPE(extras) == T_HASH);
    /* Extras first (in insertion order, typical CRuby behavior). */
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
    /* Primary names — skip any already covered by extras. */
    for (uint32_t i = 0; i < b->names_cnt; i++) {
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

/* Binding#eval(src [, file [, line]]) — parse src with the binding's
 * names in scope, then run the resulting AST with c->fp set to the
 * binding's fp and self / cref restored.  Updates to caller's lvars
 * propagate via direct fp writes; new locals introduced inside the
 * eval body are added to the binding's name table after parse so the
 * caller can read them later via local_variable_get. */
static VALUE binding_eval_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_DATA) return Qnil;
    if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "binding.eval: argument must be a String");
        return Qnil;
    }
    struct korb_binding *b = (struct korb_binding *)self;
    struct korb_string *s = (struct korb_string *)argv[0];
    const char *filename = "(eval)";
    if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) && BUILTIN_TYPE(argv[1]) == T_STRING) {
        filename = ((struct korb_string *)argv[1])->ptr;
    }
    /* Build scope_locals from binding's names. */
    const char **scope_locals = NULL;
    size_t scope_locals_n = b->names_cnt;
    if (scope_locals_n > 0) {
        scope_locals = korb_xmalloc(sizeof(char *) * scope_locals_n);
        for (uint32_t i = 0; i < b->names_cnt; i++) {
            scope_locals[i] = korb_id_name(b->names[i]);
        }
    }
    /* Snapshot bind.names_cnt before parse so we know which names
     * are NEW after merging in the eval body's introduced lvars. */
    uint32_t orig_names_cnt = b->names_cnt;
    char *err_msg = NULL;
    NODE *ast = koruby_parse_with_scope(s->ptr, (size_t)s->len, filename,
                                         scope_locals, scope_locals_n, &err_msg);
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
    VALUE r = EVAL(c, ast);

    c->current_eval_binding = prev_eval_binding;
    c->current_file = prev_file;
    c->cref = prev_cref;
    c->self = prev_self;
    c->fp = prev_fp;

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
 * created.  For now return a stub. */
static VALUE binding_source_location(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    korb_ary_push(arr, korb_str_new_cstr("(eval)"));
    korb_ary_push(arr, INT2FIX(0));
    return arr;
}
