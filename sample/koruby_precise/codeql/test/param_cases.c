/* Does the value-after-gc rule see a stale VALUE that arrived as a PARAMETER?
 * This is the shape of the korb_re_str_span SEGV (2026-08-19). */
typedef long VALUE;
void *korb_alloc(int n);
void  may_gc(void);
void  sink(VALUE v);
VALUE prod(void);
VALUE slots[64];

void *korb_alloc(int n) { (void)n; return 0; }
void  may_gc(void)      { korb_alloc(1); }
VALUE prod(void)        { return (VALUE)korb_alloc(16); }

/* BAD-param: the caller's VALUE is used after a may-GC call (the real bug). */
void bad_param(VALUE v) { may_gc(); sink(v); }

/* BAD-local (the shape the current rule already catches). */
void bad_local(void) { VALUE v = prod(); may_gc(); sink(v); }

/* GOOD-param: parked in a rooted slot and re-read. */
void good_param(VALUE v) { slots[0] = v; may_gc(); sink(slots[0]); }
