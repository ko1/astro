/* Shared runtime helpers for koruby — used by both the REPL main and
 * the standalone exe driver emitted by `--generate-executable`.
 *
 * Also houses the AROH_VISIT_ROOTS / AROH_SCAN_EDGES out-of-line
 * implementations (declared in context.h, dispatched here so the heavy
 * switch on heap-obj type and the chain walks live in one place). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "object.h"
#include "node.h"
#include "precise_gc/gc.h"

extern struct koruby_option OPTION;

/* ---------------------------------------------------------------------------
 * Precise GC root scan + per-type edge walk.
 *
 * koruby_visit_roots: walks every root slot the GC must see — the linear
 * c->stack_base..c->sp value-stack range, CTX-held VALUEs, the cref and
 * current_frame chains, and the korb_vm globals.  Sample-side helpers
 * place live VALUEs into ARO_ROOT_SCOPE-managed slots (or sp[] directly)
 * so they appear in the value-stack range.
 *
 * koruby_scan_edges: dispatches on `head.flags & T_MASK` and visits the
 * outgoing edges of one heap object.  basic.klass is visited HERE (not
 * in the per-type helper) — a class/module obj's basic.klass would be
 * double-forwarded if visit_class_edges also visited it, leading to a
 * runaway memcpy phantom into to_top under Cheney.
 * --------------------------------------------------------------------------- */

static inline void
visit_value_slot(void *ctx, koruby_edge_fn fn, VALUE *slot)
{
    VALUE v = *slot;
    if (v == 0 || SPECIAL_CONST_P(v)) return;
    fn(ctx, (void **)slot);
}

static inline void
visit_ptr_slot(void *ctx, koruby_edge_fn fn, void **slot)
{
    if (*slot == NULL) return;
    fn(ctx, slot);
}

static void
visit_method_table(void *ctx, koruby_edge_fn fn,
                   struct korb_method_table *mt)
{
    if (!mt || !mt->buckets) return;
    /* Sanity: bucket_cnt is initialized to 16 and doubles on resize; a
     * sensible upper bound for koruby's hot tables.  Out-of-range here
     * indicates a stale class struct (= 8-byte head misinterpreted as
     * class) being scanned — bail rather than dereference garbage. */
    if (mt->bucket_cnt == 0 || mt->bucket_cnt > (1u << 20)) return;
    for (uint32_t i = 0; i < mt->bucket_cnt; i++) {
        for (struct korb_method_table_entry *e = mt->buckets[i]; e; e = e->next) {
            struct korb_method *m = e->method;
            if (!m) continue;
            /* korb_module_include flattens module methods into klass's
             * table by storing the same `m` pointer in both tables
             * (entry->include_depth > 0 marks the imported copy).  Per-method
             * heap edges (= defining_class, def_cref, u.proc.proc) must be
             * visited exactly once per GC cycle — otherwise the second visit
             * passes the already-rewritten to-space addr to forward, which
             * memcpy's a phantom obj into to_top → runaway scan.  Restrict
             * the walk to depth-0 entries (= the owning class). */
            if (e->include_depth != 0) continue;
            visit_ptr_slot(ctx, fn, (void **)&m->defining_class);
            for (struct korb_cref *cr = m->def_cref; cr; cr = cr->prev) {
                visit_ptr_slot(ctx, fn, (void **)&cr->klass);
            }
            if (m->type == KORB_METHOD_PROC) {
                visit_ptr_slot(ctx, fn, (void **)&m->u.proc.proc);
            }
        }
    }
}

static void
visit_const_chain(void *ctx, koruby_edge_fn fn, struct korb_const_entry *head)
{
    for (struct korb_const_entry *e = head; e; e = e->next) {
        visit_value_slot(ctx, fn, &e->value);
    }
}

/* NOTE: deliberately does NOT visit k->basic.klass — koruby_scan_edges
 * handles that BEFORE dispatching here.  Visiting it twice would re-pass
 * the (already to-space-rewritten) slot value into forward, which under
 * Cheney memcpy's a phantom obj into to_top → scan loop runaway. */
static void
visit_class_edges(void *ctx, koruby_edge_fn fn, struct korb_class *k)
{
    if (!k) return;
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

/* Forward decls — defined after koruby_visit_roots. */
void koruby_register_libc_obj(struct RBasic *obj);
static void koruby_visit_libc_obj_internals_via_registry(struct CTX_struct *c, void *ctx, koruby_edge_fn fn);

void
koruby_visit_roots(CTX *c, void *ctx, koruby_edge_fn fn)
{
    /* (a) Value stack — c->stack_base..c->sp linear range.  Contract is
     * that every sp-advancing site zero-fills the new slots before any
     * alloc that can fire GC (= ARO_ROOT_SCOPE_START's invariant).  We
     * do NOT keep a high-water mark and lazy-clear popped slots — that
     * pattern hides a sp-up-without-zero-fill bug as a slow-leak SEGV
     * rather than catching it at the violating site. */
    if (c->stack_base && c->sp) {
        for (VALUE *p = c->stack_base; p < c->sp; p++) {
            visit_value_slot(ctx, fn, p);
        }
    }
    /* (b) CTX-held VALUEs. */
    visit_value_slot(ctx, fn, &c->current_frame->self);
    visit_value_slot(ctx, fn, &c->state_value);
    visit_ptr_slot(ctx, fn, (void **)&c->current_frame->current_class);
    /* (c+d) current_frame chain — visit all per-frame heap refs INCLUDING
     * each frame's cref chain.  Method dispatch sets frame.cref =
     * mc->def_cref (a SEPARATE chain captured at def time) which does
     * NOT extend the caller's cref chain — so walking only the head
     * frame's cref leaves outer frames' cref->klass stale.  PURGE
     * (mprotect) immediately catches the resulting SEGV when control
     * returns to that outer frame (e.g. const_set inside a class body
     * whose body called a method).
     *
     * Also visit c->sentinel_frame unconditionally: korb_eval_string
     * pushes a top_frame with prev=NULL (so a stray top-level `return`
     * inside a load'd file doesn't target the caller's block frame),
     * which disconnects the sentinel from the head chain.  Without
     * explicit walking, sentinel.self (= main_obj) goes stale across
     * bootstrap GCs and the next dispatch on it SEGVs.
     *
     * Visiting the same slot twice when chains share suffixes / a
     * frame is double-counted is safe because forwarding is idempotent
     * (header forwarding bit + already-forwarded check in the backend). */
    for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
        visit_value_slot(ctx, fn, &f->self);
        visit_value_slot(ctx, fn, &f->last_line);
        visit_value_slot(ctx, fn, &f->last_match);
        visit_ptr_slot(ctx, fn, (void **)&f->block);
        visit_ptr_slot(ctx, fn, (void **)&f->current_class);
        for (struct korb_cref *cr = f->cref; cr; cr = cr->prev) {
            visit_ptr_slot(ctx, fn, (void **)&cr->klass);
        }
    }
    {
        struct korb_frame *f = &c->sentinel_frame;
        visit_value_slot(ctx, fn, &f->self);
        visit_value_slot(ctx, fn, &f->last_line);
        visit_value_slot(ctx, fn, &f->last_match);
        visit_ptr_slot(ctx, fn, (void **)&f->block);
        visit_ptr_slot(ctx, fn, (void **)&f->current_class);
        for (struct korb_cref *cr = f->cref; cr; cr = cr->prev) {
            visit_ptr_slot(ctx, fn, (void **)&cr->klass);
        }
    }
    {
        struct korb_cref *cr = &c->top_cref;
        visit_ptr_slot(ctx, fn, (void **)&cr->klass);
    }
    /* (d') current_block / running_block — file-scope globals holding
     * the active block proc (for yield) and the currently-executing
     * block (for break/next targeting).  Without visiting these, GC
     * moves the proc but the globals stay at the old address; the
     * next korb_yield reads a stale proc pointer and SEGVs in
     * blk->self / blk->env access.  Discovered by running
     * `[1, 2].each { |t| puts t }` under BARUBY_GC_STRESS=1.
     *
     * Procs are libc-allocated (= NOT GC-arena), so forward_payload
     * returns them as-is and scan_edges never runs on their fields.
     * We must manually walk each live proc's self / enclosing_block
     * so they auto-update when the underlying heap obj moves.  The
     * set of reachable procs = running_block + each frame's block. */
    {
        extern struct korb_proc *current_block;
        extern struct korb_proc *running_block;
        visit_ptr_slot(ctx, fn, (void **)&current_block);
        visit_ptr_slot(ctx, fn, (void **)&running_block);
        if (running_block) {
            visit_value_slot(ctx, fn, &running_block->self);
            visit_ptr_slot(ctx, fn, (void **)&running_block->enclosing_block);
        }
        if (current_block) {
            visit_value_slot(ctx, fn, &current_block->self);
            visit_ptr_slot(ctx, fn, (void **)&current_block->enclosing_block);
        }
        for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
            if (f->block) {
                visit_value_slot(ctx, fn, &f->block->self);
                visit_ptr_slot(ctx, fn, (void **)&f->block->enclosing_block);
            }
        }
    }
    /* (e) korb_vm globals — all class / module pointers + main_obj +
     * globals method-table.  korb_vm itself lives in libc memory; only
     * the heap pointers it holds need visiting. */
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
    /* (f) Walk the libc-obj registry to update interior heap-ref
     * fields.  See koruby_visit_libc_obj_internals_via_registry for
     * the rationale (forward_payload returns libc objs as-is, so
     * scan_edges never runs on them and their interior arena
     * references stay stale across GC cycles). */
    koruby_visit_libc_obj_internals_via_registry(c, ctx, fn);
    /* All class pointers in heap have just been forwarded.  Cached
     * method_cache->klass fields (= scattered across all call-site
     * NODEs, NOT walked here) now hold stale addresses.  Bumping
     * method_serial invalidates them; the next dispatch site refills
     * with the current klass.  Without this, the next `Foo.method`
     * call dispatches via an stale class pointer → SEGV in
     * method_table_get when method_serial check passes but klass moved. */
    if (korb_vm) {
        korb_vm->method_serial++;
        extern state_serial_t korb_g_method_serial;
        korb_g_method_serial = korb_vm->method_serial;
    }
}

/* Libc-allocated heap objs (korb_array / korb_hash / korb_proc /
 * korb_range / korb_float / korb_bignum) hold interior heap-pointer
 * fields that the GC framework does NOT auto-update: forward_payload
 * returns libc-region addrs as-is and koruby_scan_edges is never
 * called on them.  We maintain a global doubly-linked list of all
 * allocated libc objs (registered at construction time).  In
 * visit_roots phase (f), walk the list and visit each obj's interior
 * heap-pointer fields so they auto-forward when GC moves the
 * referent (e.g. when array elements reference a moving class /
 * arena obj).  The list never shrinks during a run — libc objs
 * are not reclaimed.  Memory leak is bounded by test scale. */
struct koruby_libc_obj_entry {
    struct RBasic *obj;
    struct koruby_libc_obj_entry *next;
};

static struct koruby_libc_obj_entry *koruby_g_libc_obj_list = NULL;

void koruby_register_libc_obj(struct RBasic *obj) {
    struct koruby_libc_obj_entry *e =
        (struct koruby_libc_obj_entry *)malloc(sizeof(*e));
    if (!e) return;  /* OOM — leak the obj from the registry, harmless */
    e->obj = obj;
    e->next = koruby_g_libc_obj_list;
    koruby_g_libc_obj_list = e;
}

static void
koruby_visit_libc_obj_internals_via_registry(struct CTX_struct *c, void *ctx, koruby_edge_fn fn) {
    for (struct koruby_libc_obj_entry *e = koruby_g_libc_obj_list; e; e = e->next) {
        struct RBasic *b = e->obj;
        if (!b) continue;
        /* basic.klass for libc objs is stale-prone — visit it
         * unconditionally so it auto-forwards when the class moves. */
        visit_ptr_slot(ctx, fn, (void **)&b->klass);
        int t = (int)(b->head.flags & T_MASK);
        switch (t) {
          case T_ARRAY: {
              struct korb_array *a = (struct korb_array *)b;
              if (a->ptr && a->len > 0) {
                  for (long j = 0; j < a->len; j++) {
                      visit_value_slot(ctx, fn, &a->ptr[j]);
                  }
              }
              break;
          }
          case T_HASH: {
              struct korb_hash *h = (struct korb_hash *)b;
              for (struct korb_hash_entry *he = h->first; he; he = he->next) {
                  visit_value_slot(ctx, fn, &he->key);
                  visit_value_slot(ctx, fn, &he->value);
              }
              visit_value_slot(ctx, fn, &h->default_value);
              visit_value_slot(ctx, fn, &h->default_proc);
              break;
          }
          case T_RANGE: {
              struct korb_range *r = (struct korb_range *)b;
              visit_value_slot(ctx, fn, &r->begin);
              visit_value_slot(ctx, fn, &r->end);
              break;
          }
          case T_PROC: {
              struct korb_proc *p = (struct korb_proc *)b;
              /* p->env walking SKIPPED.  For in-scope procs, env points
               * into the value stack which is already covered by
               * visit_roots phase (a).  For escaped procs (env
               * heap-snapshotted by korb_proc_snapshot_env_maybe), the
               * heap region is libc-malloc'd and visit_roots may not
               * reach it — but walking it here on a dead/recycled
               * proc corrupts memory.  Trade-off: leak some escaped
               * proc closure refs (= they may become stale) rather
               * than corrupt active state.  Symptom we avoid: under
               * STRESS, test_block / test_fiber random SEGV with
               * basic.klass = FIXNUM 2^32. */
              visit_value_slot(ctx, fn, &p->self);
              visit_ptr_slot(ctx, fn, (void **)&p->enclosing_block);
              visit_ptr_slot(ctx, fn, (void **)&p->lexical_parent_block);
              for (struct korb_cref *cr = p->cref; cr; cr = cr->prev) {
                  visit_ptr_slot(ctx, fn, (void **)&cr->klass);
              }
              break;
          }
          case T_FLOAT:
          case T_BIGNUM:
          case T_STRING:
              /* Leaf: only klass to update (already done). */
              break;
          case T_DATA: {
              /* T_DATA covers Method / Binding / Fiber etc.  Different
               * concrete structs, but the first two fields (RBasic +
               * one VALUE) are common to Method (= receiver) and the
               * Method-derived objs.  Visiting just basic.klass + an
               * optional first VALUE field covers the common case
               * without dereferencing past the known layout.  Other
               * T_DATA types' interior heap refs are not covered here
               * (would require type tag dispatch beyond head.flags). */
              struct korb_method_obj *m = (struct korb_method_obj *)b;
              if (b->klass == (VALUE)korb_vm->method_class) {
                  visit_value_slot(ctx, fn, &m->receiver);
              }
              break;
          }
          default:
              break;
        }
    }
}

void
koruby_scan_edges(void *payload, size_t payload_size,
                  void *ctx, koruby_edge_fn fn)
{
    (void)payload_size;
    struct RBasic *b = (struct RBasic *)payload;
    /* klass field — common to every heap obj.  Visited ONCE here so
     * sub-dispatch (visit_class_edges etc.) MUST NOT visit it again. */
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
          /* string.ptr is libc-malloc'd byte buffer, not a GC heap obj. */
          break;
      case T_ARRAY: {
          struct korb_array *a = (struct korb_array *)payload;
          /* a->ptr is libc-malloc'd VALUE[]; walk its contents directly. */
          for (long i = 0; i < a->len; i++) {
              visit_value_slot(ctx, fn, &a->ptr[i]);
          }
          break;
      }
      case T_HASH: {
          /* korb_hash is libc-allocated (= korb_xmalloc).  A T_HASH-flagged
           * obj on the gc arena means scan landed on a corrupted header
           * (= an upstream gc_size mismatch or flag overwrite).  Bail
           * rather than deref garbage. */
          extern struct CTX_struct koruby_bootstrap_ctx;
          (void)koruby_bootstrap_ctx;
          /* No-op: hashes don't live in the gc arena so there are no
           * edges to scan here.  If a legit hash ends up in arena (=
           * via direct aro_gc_alloc), this needs revisiting. */
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
          /* Numeric scalars — no heap-pointer edges.  Bignum's mpz_t is
           * libc-malloc'd via GMP, freed by Phase 5 finalizer. */
          break;
      case T_CLASS:
      case T_MODULE:
          visit_class_edges(ctx, fn, (struct korb_class *)payload);
          break;
      case T_PROC: {
          struct korb_proc *p = (struct korb_proc *)payload;
          /* env is a libc-malloc'd VALUE[] of captured locals. */
          if (p->env) {
              for (uint32_t i = 0; i < p->env_size; i++) {
                  visit_value_slot(ctx, fn, &p->env[i]);
              }
          }
          visit_value_slot(ctx, fn, &p->self);
          visit_ptr_slot(ctx, fn, (void **)&p->enclosing_block);
          for (struct korb_cref *cr = p->cref; cr; cr = cr->prev) {
              visit_ptr_slot(ctx, fn, (void **)&cr->klass);
          }
          visit_ptr_slot(ctx, fn, (void **)&p->lexical_parent_block);
          break;
      }
      case T_DATA:
      case T_SYMBOL:
      case T_NODE:
      case T_NONE:
      default:
          /* No outgoing edges, or sample handles separately. */
          break;
    }
}

/* Top-level cref lives in CTX (c->top_cref) so visit_roots can ALWAYS
 * walk it regardless of which frame is current.  Method dispatch's
 * mc->def_cref is a SEPARATE chain (= korb_cref_dup creates a copy at
 * def time) that doesn't reach this top_cref, so walking only
 * c->current_frame->cref misses it when the head frame's cref is
 * def_cref → stale top_cref.klass under PURGE.  Per-CTX (no file-scope
 * global) so multiple interpreters can coexist. */

CTX *
koruby_setup_ctx(const char *current_file)
{
    /* Reuse the bootstrap CTX that korb_runtime_init initialized — it
     * already has c->astro_gc bound, c->stack_base mmap'd, c->sp = base.
     * koruby_setup_ctx just attaches the per-run roots (self / cref /
     * current_file / state). */
    CTX *c = korb_vm->current_ctx;
    c->current_frame->self = korb_vm->main_obj;
    c->current_frame->current_class = korb_vm->object_class;
    c->top_cref.klass = korb_vm->object_class;
    c->top_cref.prev = NULL;
    c->current_frame->cref = &c->top_cref;
    c->current_frame->current_file = current_file;
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
    VALUE r = EVAL(c, ast, c->current_frame->fp);
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
