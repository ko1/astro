/* Self-test fixture for value_after_gc.ql.
 * BAD = a heap VALUE held in a local across a may-GC call (stale under moving GC).
 * GOOD = staged in slots[] / re-read / no GC between def and use / consumed as an
 *        argument before that call's own GC.
 * The path must contain "/astro/" for the query's file filter; this file lives
 * under .../astro/... so it qualifies. */
#include <stdint.h>
typedef intptr_t VALUE;

void  *korb_alloc(unsigned long n);   /* the GC seed */
VALUE  prod(void);                    /* may-gc, returns a fresh heap VALUE */
void   may_gc(void);                  /* may-gc, returns void */
void   sink(VALUE v);                 /* not may-gc */
void   may_gc_sink(VALUE v);          /* may-gc */
VALUE  slots[64];

VALUE prod(void)        { return (VALUE)korb_alloc(16); }   /* → korb_alloc: may-gc, VALUE */
void  may_gc(void)      { korb_alloc(1); }
void  may_gc_sink(VALUE v) { (void)v; korb_alloc(1); }

/* BAD 1: local held across an explicit may-GC call, then used. */
void bad1(void) { VALUE v = prod(); may_gc(); sink(v); }

/* BAD 2: first producer's result held across the second producer's GC. */
void bad2(void) { VALUE a = prod(); VALUE b = prod(); sink(a); sink(b); }

/* BAD 3: alias — q copies the held VALUE, used after GC. */
void bad3(void) { VALUE v = prod(); VALUE q = v; may_gc(); sink(q); }

/* GOOD 1: no may-GC between def and use. */
void good1(void) { VALUE v = prod(); sink(v); }

/* GOOD 2: staged in slots[] (array element, not a StackVariable) and re-read. */
void good2(void) { slots[0] = prod(); may_gc(); sink(slots[0]); }

/* GOOD 3: re-read from the slot into the local after GC (a fresh SSA def). */
void good3(void) { VALUE v = prod(); slots[0] = v; may_gc(); v = slots[0]; sink(v); }

/* GOOD 4: consumed as an argument to a may-GC call — read before that call's GC. */
void good4(void) { VALUE v = prod(); may_gc_sink(v); }
