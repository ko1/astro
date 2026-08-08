/* Fixture for koruby/borrow-escape and koruby/aro-borrow-unused. */
#define ARO_BORROW /* marker (empty; CodeQL keys on the MacroInvocation) */

typedef long VALUE;
typedef struct KorbStrBuf { unsigned len; char data[]; } KorbStrBuf;
typedef struct KorbString { unsigned len; KorbStrBuf *buf; } KorbString;
#define VAL2STR(v) ((KorbString *)(v))

/* a real accessor: touches the interior, returns a borrow (both OK). */
ARO_BORROW static char *sb_data(VALUE v) { return VAL2STR(v)->buf->data; }

/* ESCAPE: a non-accessor returns a raw borrow → expect one borrow-escape alert. */
char *leak(VALUE v) { return sb_data(v); }

/* OK: an accessor MAY return a borrow (its contract) — not an escape. */
ARO_BORROW static char *ok_accessor(VALUE v) { return sb_data(v); }

/* UNUSED: marked ARO_BORROW but touches no interior → expect one unused alert. */
ARO_BORROW static int bogus(void) { return 42; }
int use_bogus(void) { return bogus() + (int)(long)leak(0) + (int)(long)ok_accessor(0); }
