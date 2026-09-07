/* Self-test fixture for unsequenced_gc_arg.ql.
 * BAD  = one argument list holds both a may-GC call and a VALUE the GC can move.
 * GOOD = the allocation is hoisted into its own statement, or the co-argument
 *        cannot move (immediate / not a VALUE), or there is only one argument.
 * The path must contain "/astro/" for the query's file filter. */
#include <stdint.h>
typedef intptr_t VALUE;

void  *korb_alloc(unsigned long n);        /* the GC seed */
VALUE  prod(void);                          /* may-gc, returns a fresh VALUE */
unsigned intern(const char *s);             /* not may-gc */
void   define(unsigned name, VALUE val, VALUE owner);   /* not may-gc */
void   sink2(VALUE a, VALUE b);
VALUE  slots[64];

VALUE prod(void) { return (VALUE)korb_alloc(16); }

/* BAD 1: the real shape — allocation next to a slot read (File::NULL). */
void bad1(void) { define(intern("NULL"), prod(), slots[1]); }

/* BAD 2: same, with the operands the other way round. */
void bad2(void) { define(intern("X"), slots[1], prod()); }

/* BAD 3: two producers — the first result is an unrooted temporary while the
 * second one collects. */
void bad3(void) { sink2(prod(), prod()); }

/* GOOD 1: hoisted — the allocation finishes before the slot is read. */
void good1(void) { slots[2] = prod(); define(intern("NULL"), slots[2], slots[1]); }

/* GOOD 2: the co-argument is an immediate, which never moves. */
void good2(void) { define(intern("MODE"), prod(), (VALUE)0); }

/* GOOD 3: no allocation in the list at all. */
void good3(void) { define(intern("A"), slots[0], slots[1]); }

/* GOOD 4: a single argument — nothing to be unsequenced against. */
void good4(void) { sink2(prod(), (VALUE)0); }
