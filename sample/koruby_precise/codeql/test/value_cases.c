/* Self-test fixture for value_after_gc.ql.
 * BAD = a heap korb_value held in a local across a may-GC call (stale under moving GC).
 * GOOD = staged in slots[] / re-read / no GC between def and use / consumed as an
 *        argument before that call's own GC.
 * The path must contain "/astro/" for the query's file filter; this file lives
 * under .../astro/... so it qualifies. */
#include <stdint.h>
typedef intptr_t korb_value;

void  *korb_alloc(unsigned long n);   /* the GC seed */
korb_value  prod(void);                    /* may-gc, returns a fresh heap korb_value */
void   may_gc(void);                  /* may-gc, returns void */
void   sink(korb_value v);                 /* not may-gc */
void   may_gc_sink(korb_value v);          /* may-gc */
korb_value  slots[64];

korb_value prod(void)        { return (korb_value)korb_alloc(16); }   /* → korb_alloc: may-gc, korb_value */
void  may_gc(void)      { korb_alloc(1); }
void  may_gc_sink(korb_value v) { (void)v; korb_alloc(1); }

/* BAD 1: local held across an explicit may-GC call, then used. */
void bad1(void) { korb_value v = prod(); may_gc(); sink(v); }

/* BAD 2: first producer's result held across the second producer's GC. */
void bad2(void) { korb_value a = prod(); korb_value b = prod(); sink(a); sink(b); }

/* BAD 3: alias — q copies the held korb_value, used after GC. */
void bad3(void) { korb_value v = prod(); korb_value q = v; may_gc(); sink(q); }

/* GOOD 1: no may-GC between def and use. */
void good1(void) { korb_value v = prod(); sink(v); }

/* GOOD 2: staged in slots[] (array element, not a StackVariable) and re-read. */
void good2(void) { slots[0] = prod(); may_gc(); sink(slots[0]); }

/* GOOD 3: re-read from the slot into the local after GC (a fresh SSA def). */
void good3(void) { korb_value v = prod(); slots[0] = v; may_gc(); v = slots[0]; sink(v); }

/* GOOD 4: consumed as an argument to a may-GC call — read before that call's GC. */
void good4(void) { korb_value v = prod(); may_gc_sink(v); }
