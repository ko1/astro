/* CodeQL fixture for koruby/interior-encapsulation.
 * Only ARO_BORROW-marked accessors may reach into a GC object's raw layout;
 * any other function touching KorbStrBuf::data / KorbArrayItems::data is a
 * violation. */
#define ARO_BORROW /* marker (empty under gcc; MacroInvocation is what CodeQL keys on) */

typedef long VALUE;
typedef struct KorbStrBuf { unsigned len; char data[]; } KorbStrBuf;
typedef struct KorbString { unsigned len; KorbStrBuf *buf; } KorbString;
#define VAL2STR(v) ((KorbString *)(v))

/* OK: the annotated accessor is allowed to reach into the raw layout. */
ARO_BORROW static const char *str_data(VALUE v) { return VAL2STR(v)->buf->data; }

/* OK: goes through the accessor, never touches the interior directly. */
char first_ok(VALUE v) { return str_data(v)[0]; }

/* VIOLATION: a non-accessor reaches into the interior directly. */
char first_bad(VALUE v) { return VAL2STR(v)->buf->data[0]; }
