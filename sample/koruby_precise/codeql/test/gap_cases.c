/* Cases value_after_gc.ql does NOT cover: a VALUE merely TAKEN OUT of somewhere
 * (slot / parameter / field) and used after a may-GC call.  value_after_gc.ql
 * only follows a VALUE that a may-GC call PRODUCED, so all three are silent
 * there.  Path contains "/astro/" for the file filter. */
#include <stdint.h>
typedef intptr_t VALUE;
void  *korb_alloc(unsigned long n);
void   sink(VALUE v);
VALUE  slots[64];
typedef struct { VALUE f; } Obj;
Obj   *obj(void);
void   may_gc(void) { korb_alloc(1); }

/* BAD: read out of a slot, collect, then use the copy. */
void gap1(void) { VALUE v = slots[0]; may_gc(); sink(v); }
/* BAD: read out of a struct field, collect, then use the copy. */
void gap2(void) { VALUE v = obj()->f; may_gc(); sink(v); }
/* GOOD: re-read from the slot after the collection. */
void ok1(void) { VALUE v = slots[0]; may_gc(); v = slots[0]; sink(v); }
/* GOOD: no collection between the read and the use. */
void ok2(void) { VALUE v = slots[0]; sink(v); }
