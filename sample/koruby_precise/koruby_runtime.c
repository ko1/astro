/* Shared runtime helpers for koruby — used by both the REPL main and
 * the standalone exe driver emitted by `--generate-executable`.
 *
 * Also houses the AROH_VISIT_ROOTS / AROH_SCAN_EDGES out-of-line
 * implementations (declared in context.h, dispatched here so the
 * heavy switch on heap-obj type lives in one place). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "object.h"
#include "node.h"
#include "precise_gc/gc.h"

extern struct koruby_option OPTION;

/* ---------------------------------------------------------------------------
 * Precise GC integration — root scan + edge walk.
 *
 * Phase 2-3 implementation.  Sample exposes these as out-of-line
 * functions (= called from AROH_VISIT_ROOTS / AROH_SCAN_EDGES macros
 * defined in context.h) because the per-type dispatch is heavy enough
 * that inline expansion bloats every framework backend translation unit.
 * --------------------------------------------------------------------------- */

/* Forward decl: framework's per-edge callback signature. */
typedef void (*koruby_edge_fn)(void *ctx, void **slot);

/* Visit one VALUE slot.  Skips immediates (= fixnum / flonum / symbol /
 * Qfalse / Qnil / Qtrue / Qundef) and NULL since the framework treats
 * those as non-heap.  For real heap pointers, dispatches via the
 * framework's ARO_GC_VISIT_EDGE_PTR (= raw typed-ptr edge, koruby
 * stores raw VALUE bits, no scramble decode). */
static inline void
visit_value_slot(void *ctx, koruby_edge_fn fn, VALUE *slot)
{
    VALUE v = *slot;
    if (v == 0 || SPECIAL_CONST_P(v)) return;
    fn(ctx, (void **)slot);
}

/* Visit a raw typed-pointer slot (= struct korb_class *, char *, etc.).
 * Skips NULL. */
static inline void
visit_ptr_slot(void *ctx, koruby_edge_fn fn, void **slot)
{
    if (*slot == NULL) return;
    fn(ctx, slot);
}

/* Forward decl: class methods table walk (= contains korb_method *
 * which is libc-managed but holds VALUE-ish links). */
static void visit_method_table(void *ctx, koruby_edge_fn fn,
                               struct korb_method_table *mt);
static void visit_const_chain(void *ctx, koruby_edge_fn fn,
                              struct korb_const_entry *head);
static void visit_class_edges(void *ctx, koruby_edge_fn fn,
                              struct korb_class *k);

/* Visit the constants linked list. */
static void
visit_const_chain(void *ctx, koruby_edge_fn fn, struct korb_const_entry *head)
{
    for (struct korb_const_entry *e = head; e; e = e->next) {
        visit_value_slot(ctx, fn, &e->value);
    }
}

/* Visit the method table buckets.  Each entry chains korb_method *
 * which holds defining_class / body (= Node *, NOT GC) / def_cref. */
static void
visit_method_table(void *ctx, koruby_edge_fn fn, struct korb_method_table *mt)
{
    if (!mt || !mt->buckets) return;
    for (uint32_t i = 0; i < mt->bucket_cnt; i++) {
        for (struct korb_method_table_entry *e = mt->buckets[i];
             e; e = e->next) {
            struct korb_method *m = e->method;
            if (!m) continue;
            visit_ptr_slot(ctx, fn, (void **)&m->defining_class);
            /* m->def_cref is a libc-malloc'd cref chain; cref->klass is
             * a heap-managed korb_class — walk the chain. */
            for (struct korb_cref *cr = m->def_cref; cr; cr = cr->prev) {
                visit_ptr_slot(ctx, fn, (void **)&cr->klass);
            }
            if (m->type == KORB_METHOD_PROC) {
                visit_ptr_slot(ctx, fn, (void **)&m->u.proc.proc);
            }
        }
    }
}

/* Visit korb_class edges.  Includes super, includes[], prepends[],
 * methods, constants, class_ivars[], cvars[]. */
static void
visit_class_edges(void *ctx, koruby_edge_fn fn, struct korb_class *k)
{
    if (!k) return;
    visit_ptr_slot(ctx, fn, (void **)&k->basic.klass);
    visit_ptr_slot(ctx, fn, (void **)&k->super);
    for (uint32_t i = 0; i < k->includes_cnt; i++) {
        visit_ptr_slot(ctx, fn, (void **)&k->includes[i]);
    }
    for (uint32_t i = 0; i < k->prepends_cnt; i++) {
        visit_ptr_slot(ctx, fn, (void **)&k->prepends[i]);
    }
    visit_method_table(ctx, fn, &k->methods);
    visit_const_chain(ctx, fn, k->constants);
    for (uint32_t i = 0; i < k->class_ivar_cnt; i++) {
        visit_value_slot(ctx, fn, &k->class_ivars[i].value);
    }
    for (uint32_t i = 0; i < k->cvar_cnt; i++) {
        visit_value_slot(ctx, fn, &k->cvars[i].value);
    }
    visit_ptr_slot(ctx, fn, (void **)&k->anon_parent);
}

void
koruby_visit_roots(CTX *c, void *ctx, koruby_edge_fn fn)
{
    /* (a) Value stack — c->stack_base..c->sp. */
    for (VALUE *p = c->stack_base; p < c->sp; p++) {
        visit_value_slot(ctx, fn, p);
    }
    /* (b) CTX-held VALUEs. */
    visit_value_slot(ctx, fn, &c->self);
    visit_value_slot(ctx, fn, &c->state_value);
    visit_ptr_slot(ctx, fn, (void **)&c->current_class);
    /* cref chain */
    for (struct korb_cref *cr = c->cref; cr; cr = cr->prev) {
        visit_ptr_slot(ctx, fn, (void **)&cr->klass);
    }
    /* current_frame chain — each frame has VALUEs (self / last_line /
     * last_match) and pointer fields. */
    for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
        visit_value_slot(ctx, fn, &f->self);
        visit_value_slot(ctx, fn, &f->last_line);
        visit_value_slot(ctx, fn, &f->last_match);
    }
    /* (c) korb_vm globals — all class pointers + main_obj + globals method table. */
    if (korb_vm) {
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->object_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->class_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->module_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->integer_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->float_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->string_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->array_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->hash_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->symbol_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->true_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->false_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->nil_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->proc_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->range_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->kernel_module);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->comparable_module);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->enumerable_module);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->numeric_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->fiber_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->method_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->binding_class);
        visit_ptr_slot(ctx, fn, (void **)&korb_vm->main_obj_class);
        visit_value_slot(ctx, fn, &korb_vm->main_obj);
        visit_method_table(ctx, fn, &korb_vm->globals);
    }
}

void
koruby_scan_edges(void *payload, size_t payload_size, void *ctx,
                  koruby_edge_fn fn)
{
    (void)payload_size;
    struct RBasic *b = (struct RBasic *)payload;
    /* klass field is common to all heap objs. */
    visit_ptr_slot(ctx, fn, (void **)&b->klass);
    int t = (b->head.flags & T_MASK);
    switch (t) {
      case T_OBJECT: {
          struct korb_object *o = (struct korb_object *)payload;
          for (uint32_t i = 0; i < o->ivar_cnt; i++) {
              visit_value_slot(ctx, fn, &o->ivars[i]);
          }
          break;
      }
      case T_STRING:
          /* korb_string.ptr is libc-malloc'd byte buffer, not GC heap. */
          break;
      case T_ARRAY: {
          struct korb_array *a = (struct korb_array *)payload;
          /* a->ptr is libc-malloc'd VALUE[] — walk its contents
           * directly; the buffer itself isn't a GC heap obj so the
           * framework won't visit it independently. */
          for (long i = 0; i < a->len; i++) {
              visit_value_slot(ctx, fn, &a->ptr[i]);
          }
          break;
      }
      case T_HASH: {
          struct korb_hash *h = (struct korb_hash *)payload;
          /* Walk insertion-order chain to visit every entry's key/value.
           * Entries are libc-malloc'd; their VALUEs are GC heap refs. */
          for (struct korb_hash_entry *e = h->first; e; e = e->next) {
              visit_value_slot(ctx, fn, &e->key);
              visit_value_slot(ctx, fn, &e->value);
          }
          visit_value_slot(ctx, fn, &h->default_value);
          visit_value_slot(ctx, fn, &h->default_proc);
          break;
      }
      case T_RANGE: {
          struct korb_range *r = (struct korb_range *)payload;
          visit_value_slot(ctx, fn, &r->begin);
          visit_value_slot(ctx, fn, &r->end);
          break;
      }
      case T_FLOAT:
      case T_BIGNUM:
          /* Numeric scalars — no heap-pointer edges.  bignum's mpz_t is
           * libc-malloc'd via GMP, handled by Phase 5 finalizer. */
          break;
      case T_CLASS:
      case T_MODULE: {
          visit_class_edges(ctx, fn, (struct korb_class *)payload);
          break;
      }
      case T_PROC: {
          struct korb_proc *p = (struct korb_proc *)payload;
          /* env is a libc-malloc'd VALUE[] of captured locals. */
          if (p->env) {
              for (uint32_t i = 0; i < p->env_size; i++) {
                  visit_value_slot(ctx, fn, &p->env[i]);
              }
          }
          /* enclosing block / self */
          break;
      }
      case T_DATA:
      case T_SYMBOL:
      case T_NODE:
      case T_NONE:
      default:
          /* Either no edges or sample handles separately. */
          break;
    }
}

CTX *
koruby_setup_ctx(const char *current_file)
{
    CTX *c = korb_xcalloc(1, sizeof(CTX));
    korb_vm->current_ctx = c;
    /* The value stack is heap allocated (libc malloc, NOT GC heap) so
     * the framework can scan it via AROH_VISIT_ROOTS without colliding
     * with GC heap object iteration.  16M slots. */
    size_t stack_size = 16 * 1024 * 1024;
    c->stack_base = korb_xmalloc(stack_size * sizeof(VALUE));
    for (size_t i = 0; i < stack_size; i++) c->stack_base[i] = Qnil;
    c->stack_end  = c->stack_base + stack_size;
    c->fp = c->stack_base;
    c->sp = c->fp;
    c->env = c->stack_base;  /* root scan lower bound */
    /* Initialize the precise GC instance now that CTX is set up.
     * aro_gc_init binds c->astro_gc; subsequent aro_gc_alloc calls
     * read from / write to c->astro_gc + use AROH_VISIT_ROOTS for
     * root scan.  Must come BEFORE any aro_gc_alloc on this CTX. */
    aro_gc_init(c);
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
