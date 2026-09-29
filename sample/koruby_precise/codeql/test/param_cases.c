/* Does the value-after-gc rule see a stale korb_value that arrived as a PARAMETER?
 * This is the shape of the korb_re_str_span SEGV (2026-08-19). */
typedef long korb_value;
void *korb_alloc(int n);
void  may_gc(void);
void  sink(korb_value v);
korb_value prod(void);
korb_value slots[64];

void *korb_alloc(int n) { (void)n; return 0; }
void  may_gc(void)      { korb_alloc(1); }
korb_value prod(void)        { return (korb_value)korb_alloc(16); }

/* BAD-param: the caller's korb_value is used after a may-GC call (the real bug). */
void bad_param(korb_value v) { may_gc(); sink(v); }

/* BAD-local (the shape the current rule already catches). */
void bad_local(void) { korb_value v = prod(); may_gc(); sink(v); }

/* GOOD-param: parked in a rooted slot and re-read. */
void good_param(korb_value v) { slots[0] = v; may_gc(); sink(slots[0]); }
