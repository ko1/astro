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
 * c->stack_base..c->sp_top value-stack range, CTX-held VALUEs, the cref and
 * current_frame chains, and the korb_vm globals.  Sample-side helpers
 * place live VALUEs into sp[] staging slots so they appear in the
 * value-stack range.
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
    extern uint64_t korb_g_gc_gen;
    for (uint32_t i = 0; i < mt->bucket_cnt; i++) {
        for (struct korb_method_table_entry *e = mt->buckets[i]; e; e = e->next) {
            struct korb_method *m = e->method;
            if (!m) continue;
            /* korb_module_include flattens module methods into klass's table
             * by storing the same `m` pointer in both tables (the importing
             * class's entry has include_depth > 0).  Per-method heap edges
             * (= defining_class, def_cref, u.proc.proc) must be forwarded
             * exactly ONCE per GC cycle — a second forward of the same slot
             * passes an already-to-space addr to forward, which memcpy's a
             * phantom obj into to_top → runaway scan.  Dedup per-method via a
             * visit-generation stamp (NOT by "depth-0 entries only": a method
             * defined directly on a class reachable only via an including
             * class's flattened table appears solely at depth>=1, and the old
             * rule then never forwarded its def_cref->klass → stale class →
             * SEGV in const_lookup, e.g. Hash#to_h's `instance_of?(Hash)`). */
            if (m->gc_visit_gen == korb_g_gc_gen) continue;
            m->gc_visit_gen = korb_g_gc_gen;
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
    /* Bump GC generation so inline ivar_cache entries (which key on
     * korb_class * pointer) become invalid this cycle.  Without this,
     * a moving GC can place class B at the address class A had — a
     * stale ivar_cache entry would claim B has A's slot for the same
     * AST-node access, leading to a wrong-slot read on B's instance. */
    extern uint64_t korb_g_gc_gen;
    korb_g_gc_gen++;
    /* (a) Value stack — c->stack_base..c->sp_top linear range.  Contract is
     * that every sp-advancing site zero-fills the new slots before any
     * alloc that can fire GC.  We do NOT keep a high-water mark and
     * lazy-clear popped slots — that pattern hides a sp-up-without-zero-fill
     * bug as a slow-leak SEGV rather than catching it at the violating site. */
    if (c->stack_base && c->sp_top) {
        for (VALUE *p = c->stack_base; p < c->sp_top; p++) {
            visit_value_slot(ctx, fn, p);
        }
    }
    /* (b) CTX-held VALUEs. */
    visit_value_slot(ctx, fn, &c->current_frame->self);
    /* Receiver parked by a method prologue mid-argument-processing (held as a
     * bare C-local across the rest-array / kwargs GC points before reaching
     * frame.self). */
    visit_value_slot(ctx, fn, &c->dispatch_recv_root);
    /* c->state / c->state_value field 削除済 (Phase 8d-R5).  THROW/RAISE
     * values now propagate via RESULT.value through the call chain so
     * there's nothing here to visit. */
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
    /* Walk frame chain with a hard depth cap to defend against a
     * corrupted .prev (= dangling stack frame whose prev was overwritten
     * after the frame was popped, or an uninitialized .prev slot).
     * Normal Ruby code rarely exceeds depth 500; capping at 4096 is
     * generous and turns runaway frame-walk crashes into a quiet
     * truncation under STRESS+PURGE. */
    int frame_depth = 0;
    for (struct korb_frame *f = c->current_frame; f && frame_depth < 4096; f = f->prev, frame_depth++) {
        visit_value_slot(ctx, fn, &f->self);
        visit_value_slot(ctx, fn, &f->last_line);
        visit_value_slot(ctx, fn, &f->last_match);
        visit_ptr_slot(ctx, fn, (void **)&f->block);
        visit_ptr_slot(ctx, fn, (void **)&f->current_class);
        int cref_depth = 0;
        for (struct korb_cref *cr = f->cref; cr && cref_depth < 64; cr = cr->prev, cref_depth++) {
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
    /* Active eval/require top_frames: each is pushed with prev=NULL (control-
     * flow isolation), which disconnects it from c->current_frame's chain
     * while a deeper nested eval is running.  Walk them via the CTX-resident
     * eval_frame_chain so their self (= main_obj) / cref stay forwarded.
     * Idempotent if a frame is also the current head (forwarding is). */
    for (struct korb_frame *ef = c->eval_frame_chain; ef; ef = ef->eval_prev) {
        visit_value_slot(ctx, fn, &ef->self);
        visit_value_slot(ctx, fn, &ef->last_line);
        visit_value_slot(ctx, fn, &ef->last_match);
        visit_ptr_slot(ctx, fn, (void **)&ef->block);
        visit_ptr_slot(ctx, fn, (void **)&ef->current_class);
        for (struct korb_cref *cr = ef->cref; cr; cr = cr->prev) {
            visit_ptr_slot(ctx, fn, (void **)&cr->klass);
        }
        /* The SUSPENDED caller context below this eval: eval_prev only links the
         * top_frames, but an open class/module body whose body called require
         * has body frames between its top_frame and the require site.  Those
         * frames are unreachable from c->current_frame (the deeper eval cut the
         * chain at prev=NULL) and not in eval_frame_chain — walk them via the
         * saved eval_caller_frame so their self / current_class / cref->klass
         * stay forwarded (otherwise const_get on the open module derefs a
         * retired-plane class → SEGV under STRESS+PURGE). */
        int fdepth = 0;
        for (struct korb_frame *f = ef->eval_caller_frame; f && fdepth < 4096; f = f->prev, fdepth++) {
            visit_value_slot(ctx, fn, &f->self);
            visit_value_slot(ctx, fn, &f->last_line);
            visit_value_slot(ctx, fn, &f->last_match);
            visit_ptr_slot(ctx, fn, (void **)&f->block);
            visit_ptr_slot(ctx, fn, (void **)&f->current_class);
            int cdepth = 0;
            for (struct korb_cref *cr = f->cref; cr && cdepth < 64; cr = cr->prev, cdepth++) {
                visit_ptr_slot(ctx, fn, (void **)&cr->klass);
            }
        }
    }
    /* korb_yield self-save chain — keep each suspended yield's enclosing self
     * forwarded across the (nested) block body GC.  See CTX.yield_self_chain. */
    for (struct korb_yield_self_save *ys = c->yield_self_chain; ys; ys = ys->prev) {
        visit_value_slot(ctx, fn, &ys->self);
    }
    {
        struct korb_cref *cr = &c->top_cref;
        visit_ptr_slot(ctx, fn, (void **)&cr->klass);
    }
    /* (d') c->current_block / c->running_block — held on CTX so future
     * Fiber / Thread support gets these per-ctx for free.  Procs are
     * libc-allocated (forward_payload returns them as-is), so we must
     * manually walk each live proc's self / enclosing_block. */
    {
        visit_ptr_slot(ctx, fn, (void **)&c->current_block);
        visit_ptr_slot(ctx, fn, (void **)&c->running_block);
        if (c->running_block) {
            visit_value_slot(ctx, fn, &c->running_block->self);
            visit_ptr_slot(ctx, fn, (void **)&c->running_block->enclosing_block);
        }
        if (c->current_block) {
            visit_value_slot(ctx, fn, &c->current_block->self);
            visit_ptr_slot(ctx, fn, (void **)&c->current_block->enclosing_block);
        }
        for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
            if (f->block) {
                visit_value_slot(ctx, fn, &f->block->self);
                visit_ptr_slot(ctx, fn, (void **)&f->block->enclosing_block);
            }
        }
    }
    /* (d'') gvars table — `$!` stores the currently-rescued exception
     * (arena-allocated korb_object).  Without visiting, GC moves the exc
     * but gvars.vals[$!_idx] stays at the old slot; the next `raise`
     * inside a rescue body reads stale $! and SEGVs in the cause-link
     * cycle walk.  Reproducer:
     *   begin; raise "inner"; rescue; raise "outer"; end
     * under STRESS+PURGE. */
    {
        extern uint32_t koruby_gvars_size(void);
        extern VALUE *koruby_gvars_vals(void);
        VALUE *gv = koruby_gvars_vals();
        uint32_t gn = koruby_gvars_size();
        for (uint32_t i = 0; i < gn; i++) {
            visit_value_slot(ctx, fn, &gv[i]);
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
        visit_value_slot(ctx, fn, &korb_vm->frozen_true_str);
        visit_value_slot(ctx, fn, &korb_vm->frozen_false_str);
        visit_value_slot(ctx, fn, &korb_vm->frozen_nil_str);
        visit_value_slot(ctx, fn, &korb_vm->generic_ivars);
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
              /* Arrays are now arena objects (payload-as-VALUE), not libc;
               * they shouldn't appear in this libc registry.  Visit the
               * backing reference defensively in case a stale entry remains. */
              struct korb_array *a = (struct korb_array *)b;
              visit_value_slot(ctx, fn, &a->backing);
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
              visit_value_slot(ctx, fn, &p->self);
              visit_ptr_slot(ctx, fn, (void **)&p->enclosing_block);
              visit_ptr_slot(ctx, fn, (void **)&p->lexical_parent_block);
              /* Cap chain depth at 64 — sanity bound to detect
               * corruption / loops in cref chains.  Normal Ruby code
               * has cref nesting depth ~10 max. */
              int chain_depth = 0;
              for (struct korb_cref *cr = p->cref; cr && chain_depth < 64; cr = cr->prev, chain_depth++) {
                  visit_ptr_slot(ctx, fn, (void **)&cr->klass);
              }
              /* p->env walking — only for ESCAPED procs (env is
               * libc-malloc'd by korb_proc_snapshot_env_*).  Skip when
               * env points into the value stack — those are walked by
               * phase (a) when c->sp_top covers them, or are leftover for a
               * popped frame (= unreachable; values can be stale but
               * walking them would re-introduce dead arena refs).  The
               * libc snapshot env is fixed-size (env_size) and not
               * freed, so walking is safe.  Under STRESS+PURGE this
               * prevents stale closure-captured class refs from
               * becoming PROT_NONE addresses that SEGV when the body
               * is re-invoked (heredoc_spec.rb, proc_spec.rb etc.). */
              if (p->env && p->env_size > 0 &&
                  (c->stack_base == NULL ||
                   (VALUE *)p->env < c->stack_base ||
                   (VALUE *)p->env >= c->stack_end)) {
                  for (uint32_t j = 0; j < p->env_size; j++) {
                      visit_value_slot(ctx, fn, &p->env[j]);
                  }
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
              } else if (korb_vm->fiber_class &&
                         b->klass == (VALUE)korb_vm->fiber_class) {
                  /* Fiber: scan its suspended stack/frames (or, while it runs,
                   * its suspended resumer's) — see korb_scan_fiber_roots. */
                  korb_scan_fiber_roots((VALUE)b, ctx, fn);
              } else if (korb_vm->binding_class &&
                         b->klass == (VALUE)korb_vm->binding_class) {
                  /* Binding: its self / captured lvars / extras are moving
                   * arena objects held only by this libc-allocated binding,
                   * so they must be forwarded here or `binding.eval`/
                   * local_variable_get read a stale self/value under STRESS. */
                  struct korb_binding *bnd = (struct korb_binding *)b;
                  visit_value_slot(ctx, fn, &bnd->self);
                  visit_value_slot(ctx, fn, &bnd->extra_vars);
                  visit_value_slot(ctx, fn, &bnd->outer_vars);
                  /* fp slots: only when fp is the binding's own heap snapshot.
                   * When fp aliases the live value stack (fp == live_fp) those
                   * slots are already covered by the value-stack scan; visiting
                   * again would double-forward. */
                  if (bnd->fp && bnd->fp != bnd->live_fp) {
                      for (uint32_t i = 0; i < bnd->names_cnt; i++) {
                          visit_value_slot(ctx, fn, &bnd->fp[bnd->base + i]);
                      }
                  }
                  int cref_depth = 0;
                  for (struct korb_cref *cr = bnd->cref; cr && cref_depth < 64;
                       cr = cr->prev, cref_depth++) {
                      visit_ptr_slot(ctx, fn, (void **)&cr->klass);
                  }
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
          /* Handle: the only edge is the backing payload reference.  The
           * elements themselves are walked when the framework visits the
           * T_ARY_BACKING object directly (= reader co-located with data,
           * see docs/array_payload_value.md). */
          struct korb_array *a = (struct korb_array *)payload;
          visit_value_slot(ctx, fn, &a->backing);
          break;
      }
      case T_ARY_BACKING: {
          /* Backing payload for T_ARRAY.  Element count derived from the
           * header gc_size (= offsetof(items) + N*sizeof(VALUE)).  Walk all
           * N slots: the live prefix holds elements, the unused tail is 0
           * (zero-filled by aro_gc_alloc) which visit_value_slot skips. */
          struct korb_ary_backing *bk = (struct korb_ary_backing *)payload;
          long n = (long)((b->head.gc_size - offsetof(struct korb_ary_backing, items))
                          / sizeof(VALUE));
          for (long i = 0; i < n; i++) {
              visit_value_slot(ctx, fn, &bk->items[i]);
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
     * already has c->astro_gc bound, c->stack_base mmap'd, c->sp_top = base.
     * koruby_setup_ctx just attaches the per-run roots (self / cref /
     * current_file / state). */
    CTX *c = korb_vm->current_ctx;
    /* Bind the per-CTX machine pointer.  Initially this just mirrors the
     * global; future migration: replace `korb_vm->X` with `c->mch->X`
     * call-site by call-site, then make `korb_vm` private to setup. */
    c->mch = korb_vm;
    c->current_frame->self = korb_vm->main_obj;
    c->current_frame->current_class = korb_vm->object_class;
    c->top_cref.klass = korb_vm->object_class;
    c->top_cref.prev = NULL;
    c->current_frame->cref = &c->top_cref;
    c->current_frame->current_file = current_file;
    c->method_serial = korb_vm->method_serial;
    return c;
}

void
koruby_eval_bootstrap(CTX *c)
{
    extern const char koruby_bootstrap_src[];
    extern const size_t koruby_bootstrap_len;
    RESULT _r = korb_eval_string(c, koruby_bootstrap_src,
                                  koruby_bootstrap_len, "<bootstrap>");
    if (_r.state == KORB_RAISE) {
        VALUE s = korb_inspect(c, c->sp_top, _r.value);
        fprintf(stderr, "bootstrap failure: %s\n", korb_str_cstr(s));
    }
}

/* Run ast with CRuby-style exception / throw / SystemExit / at_exit
 * handling.  Returns the process exit code. */
int
koruby_run_ast(CTX *c, NODE *ast)
{
    RESULT _br = EVAL(c, ast, c->current_frame->fp);
    if (_br.state == KORB_THROW) {
        /* Pin eUTE (sp[0]) / tag (sp[1]) / tag_s (sp[2]) — korb_inspect /
         * korb_exc_new fire GC. */
        VALUE *sp = c->sp_top;
        sp[0] = korb_const_get(KORB_VM(c)->object_class,
                                korb_intern("UncaughtThrowError"));
        sp[1] = Qnil;
        sp[2] = Qnil;
        c->sp_top = sp + 3;
        if (!SPECIAL_CONST_P(_br.value) &&
            BUILTIN_TYPE(_br.value) == T_ARRAY) {
            struct korb_array *pair = (struct korb_array *)_br.value;
            if (pair->len >= 1) sp[1] = korb_ary_items(pair)[0];
        }
        sp[2] = korb_inspect(c, sp + 3, sp[1]);
        char buf[256];
        snprintf(buf, sizeof(buf), "uncaught throw %s", korb_str_cstr(sp[2]));
        _br.state = KORB_RAISE;
        if (sp[0] && !SPECIAL_CONST_P(sp[0]) && BUILTIN_TYPE(sp[0]) == T_CLASS) {
            _br.value = korb_exc_new(c, sp + 3, (struct korb_class *)sp[0], buf);
        } else {
            _br.value = korb_exc_new(c, sp + 3, NULL, buf);
        }
        c->sp_top = sp;
    }
    if (_br.state == KORB_RAISE) {
        VALUE exc = _br.value;
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
        VALUE s = korb_inspect(c, c->sp_top, _br.value);
        fprintf(stderr, "unhandled exception: %s\n", korb_str_cstr(s));
        extern void korb_run_at_exit_hooks(CTX *c);
        korb_run_at_exit_hooks(c);
        return 1;
    }
    extern void korb_run_at_exit_hooks(CTX *c);
    korb_run_at_exit_hooks(c);
    return 0;
}
