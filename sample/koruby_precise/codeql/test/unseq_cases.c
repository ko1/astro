/* Self-test fixture for unsequenced_gc_arg.ql.
 * BAD  = one argument list holds both a may-GC call and a korb_value the GC can move.
 * GOOD = the allocation is hoisted into its own statement, or the co-argument
 *        cannot move (immediate / not a korb_value), or there is only one argument.
 * The path must contain "/astro/" for the query's file filter. */
#include <stdint.h>
typedef intptr_t korb_value;

void  *korb_alloc(unsigned long n);        /* the GC seed */
korb_value  prod(void);                          /* may-gc, returns a fresh korb_value */
unsigned intern(const char *s);             /* not may-gc */
void   define(unsigned name, korb_value val, korb_value owner);   /* not may-gc */
void   sink2(korb_value a, korb_value b);
korb_value  slots[64];

korb_value prod(void) { return (korb_value)korb_alloc(16); }

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
void good2(void) { define(intern("MODE"), prod(), (korb_value)0); }

/* GOOD 3: no allocation in the list at all. */
void good3(void) { define(intern("A"), slots[0], slots[1]); }

/* GOOD 4: a single argument — nothing to be unsequenced against. */
void good4(void) { sink2(prod(), (korb_value)0); }
