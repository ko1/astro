/* Shared runtime helpers for koruby — used by both the REPL main and
 * the standalone exe driver emitted by `--generate-executable`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "object.h"
#include "node.h"

extern struct koruby_option OPTION;

CTX *
koruby_setup_ctx(const char *current_file)
{
    CTX *c = korb_xcalloc(1, sizeof(CTX));
    korb_vm->current_ctx = c;
    /* The value stack is heap allocated so GC scans it.  16M slots. */
    size_t stack_size = 16 * 1024 * 1024;
    c->stack_base = korb_xmalloc(stack_size * sizeof(VALUE));
    for (size_t i = 0; i < stack_size; i++) c->stack_base[i] = Qnil;
    c->stack_end  = c->stack_base + stack_size;
    c->fp = c->stack_base;
    c->sp = c->fp;
    c->self = korb_vm->main_obj;
    c->current_class = korb_vm->object_class;
    static struct korb_cref top_cref;
    top_cref.klass = korb_vm->object_class;
    top_cref.prev = NULL;
    c->cref = &top_cref;
    c->current_file = current_file;
    c->state = KORB_NORMAL;
    c->method_serial = korb_vm->method_serial;
    return c;
}

void
koruby_eval_bootstrap(CTX *c)
{
    extern const char koruby_bootstrap_src[];
    extern const size_t koruby_bootstrap_len;
    VALUE br = korb_eval_string(c, koruby_bootstrap_src,
                                koruby_bootstrap_len, "<bootstrap>");
    (void)br;
    if (c->state == KORB_RAISE) {
        VALUE s = korb_inspect(c->state_value);
        fprintf(stderr, "bootstrap failure: %s\n", korb_str_cstr(s));
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
}

/* Run ast with CRuby-style exception / throw / SystemExit / at_exit
 * handling.  Returns the process exit code. */
int
koruby_run_ast(CTX *c, NODE *ast)
{
    VALUE r = EVAL(c, ast);
    (void)r;
    if (c->state == KORB_THROW) {
        VALUE eUTE = korb_const_get(korb_vm->object_class,
                                    korb_intern("UncaughtThrowError"));
        VALUE tag = Qnil;
        if (!SPECIAL_CONST_P(c->state_value) &&
            BUILTIN_TYPE(c->state_value) == T_ARRAY) {
            struct korb_array *pair = (struct korb_array *)c->state_value;
            if (pair->len >= 1) tag = pair->ptr[0];
        }
        VALUE tag_s = korb_inspect(tag);
        char buf[256];
        snprintf(buf, sizeof(buf), "uncaught throw %s", korb_str_cstr(tag_s));
        c->state = KORB_RAISE;
        if (eUTE && !SPECIAL_CONST_P(eUTE) && BUILTIN_TYPE(eUTE) == T_CLASS) {
            c->state_value = korb_exc_new((struct korb_class *)eUTE, buf);
        } else {
            c->state_value = korb_exc_new(NULL, buf);
        }
    }
    if (c->state == KORB_RAISE) {
        VALUE exc = c->state_value;
        VALUE eSE = korb_const_get(korb_vm->object_class,
                                   korb_intern("SystemExit"));
        if (eSE && !SPECIAL_CONST_P(eSE) && !SPECIAL_CONST_P(exc) &&
            BUILTIN_TYPE(exc) == T_OBJECT) {
            struct korb_class *exc_cls =
                (struct korb_class *)((struct RBasic *)exc)->klass;
            struct korb_class *se_cls = (struct korb_class *)eSE;
            bool is_se = false;
            for (struct korb_class *kk = exc_cls; kk; kk = kk->super) {
                if (kk == se_cls) { is_se = true; break; }
            }
            if (is_se) {
                int code = 0;
                VALUE st = korb_ivar_get(exc, korb_intern("@status"));
                if (FIXNUM_P(st)) code = (int)FIX2LONG(st);
                extern void korb_run_at_exit_hooks(CTX *c);
                korb_run_at_exit_hooks(c);
                return code;
            }
        }
        VALUE s = korb_inspect(c->state_value);
        fprintf(stderr, "unhandled exception: %s\n", korb_str_cstr(s));
        extern void korb_run_at_exit_hooks(CTX *c);
        korb_run_at_exit_hooks(c);
        return 1;
    }
    extern void korb_run_at_exit_hooks(CTX *c);
    korb_run_at_exit_hooks(c);
    return 0;
}
