/* CodeQL fixture for koruby/borrow-after-gc.
 * Mock koruby types just enough for the query's structural matches.
 * Each function is labelled BUG (expect an alert) or OK (expect none). */
#include <string.h>

typedef long VALUE;
typedef struct KorbStrBuf { unsigned len; char data[]; } KorbStrBuf;
typedef struct KorbString { unsigned len; KorbStrBuf *buf; } KorbString;
typedef struct CTX CTX;

void *korb_alloc(CTX *c, VALUE *s, unsigned long n, unsigned t); /* the GC seed */
VALUE korb_ary_new(CTX *c, VALUE *s) { return (VALUE)korb_alloc(c, s, 8, 3); } /* may-gc */
static int nogc_cmp(const char *a, const char *b) { return a[0] - b[0]; }      /* no-gc leaf */
void sink(char c);

#define VAL2STR(v) ((KorbString *)(v))

/* BUG: hold p across a may-gc call, then use. */
void case_bug(CTX *c, VALUE *s, VALUE str) {
  char *p = VAL2STR(str)->buf->data;  /* borrow */
  korb_ary_new(c, s);                 /* may-gc */
  sink(p[0]);                         /* <-- expect ALERT */
}

/* OK: use happens before any may-gc. */
void case_ok_use_before(CTX *c, VALUE *s, VALUE str) {
  char *p = VAL2STR(str)->buf->data;
  sink(p[0]);
  korb_ary_new(c, s);
}

/* OK: only a no-gc leaf between borrow and use. */
void case_ok_nogc_between(VALUE str) {
  char *p = VAL2STR(str)->buf->data;
  nogc_cmp(p, "x");
  sink(p[0]);
}

/* OK: re-derive each iteration (new SSA def per iter; the idiom that FP'd). */
void case_ok_rederive_loop(CTX *c, VALUE *s, VALUE str, int n) {
  for (int i = 0; i < n; i++) {
    char *p = VAL2STR(str)->buf->data; /* re-derive */
    sink(p[0]);                        /* use before may-gc */
    korb_ary_new(c, s);                /* may-gc after use */
  }
}

/* BUG: loop-carried — derive once, use after may-gc inside the loop. */
void case_bug_loop_carried(CTX *c, VALUE *s, VALUE str, int n) {
  char *p = VAL2STR(str)->buf->data;  /* derive once */
  for (int i = 0; i < n; i++) {
    korb_ary_new(c, s);               /* may-gc */
    sink(p[0]);                        /* <-- expect ALERT (stale for iter >= 1) */
  }
}

/* BUG: extraction via &data[i] (address-of element). */
void case_bug_addr_of(CTX *c, VALUE *s, VALUE str) {
  char *p = &VAL2STR(str)->buf->data[2]; /* borrow: interior pointer */
  korb_ary_new(c, s);                    /* may-gc */
  sink(p[0]);                            /* <-- expect ALERT */
}

/* BUG: borrow aliased into another pointer, used after may-gc. */
void case_bug_alias(CTX *c, VALUE *s, VALUE str) {
  char *p = VAL2STR(str)->buf->data;  /* borrow */
  char *q = p;                        /* alias */
  korb_ary_new(c, s);                 /* may-gc */
  sink(q[0]);                         /* <-- expect ALERT (q is stale too) */
}
