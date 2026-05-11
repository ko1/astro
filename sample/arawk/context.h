#ifndef ARAWK_CONTEXT_H
#define ARAWK_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>

// Boehm-Demers-Weiser conservative GC (libgc).  Forward-declared so
// generated SD .c files don't need a -I path to <gc/gc.h>; they only
// see GC_malloc as an opaque extern symbol.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_init(void);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// VALUE encoding — 1-bit LSB fixnum tag, modeled on astr / pystro:
//
//   xxxx_xxx1 → fixnum (signed 63-bit value)
//   xxxx_xxx0 → ptr to `struct awk_obj` (heap-allocated, 8-byte aligned)
//
// Uninitialised awk variables are AWK_UNINIT (singleton); they coerce
// to "" in string context and 0 in numeric context.  String literals
// and field values are heap awk_obj with type AWK_T_STRING.  Numeric
// constants outside fixnum range box to AWK_T_FLOAT.  Associative
// arrays are AWK_T_ARRAY.
typedef int64_t VALUE;

#define AWK_FIX_MAX     ((int64_t)((1LL << 62) - 1))
#define AWK_FIX_MIN     ((int64_t)(-(1LL << 62)))
#define AWK_IS_FIX(v)   ((int64_t)(v) & 1LL)
#define AWK_FIX(n)      (((VALUE)(int64_t)(n) << 1) | 1LL)
#define AWK_FIX_VAL(v)  ((int64_t)(v) >> 1)
#define AWK_IS_PTR(v)   (((int64_t)(v) & 1LL) == 0)
#define AWK_PTR(v)      ((struct awk_obj *)(uintptr_t)(v))
#define AWK_OBJ_VAL(p)  ((VALUE)(uintptr_t)(p))

enum awk_type {
    AWK_T_UNINIT = 1,      // singleton — uninitialised awk variable
    AWK_T_FLOAT,
    AWK_T_STRING,
    AWK_T_STRNUM,          // string from field/getline — numeric coercion if shape matches
    AWK_T_ARRAY,           // associative array
};

struct awk_array_entry {
    char  *key;            // NUL-terminated, GC_malloc_atomic
    size_t key_len;
    VALUE  val;
    struct awk_array_entry *next;
};

struct awk_array {
    struct awk_array_entry **buckets;
    size_t bucket_cnt;
    size_t entry_cnt;
};

struct awk_obj {
    int type;
    union {
        double dbl;
        struct { char *chars; size_t len; } str;
        struct awk_array arr;
    };
};

extern struct awk_obj AWK_UNINIT_OBJ;
#define AWK_UNINIT  AWK_OBJ_VAL(&AWK_UNINIT_OBJ)

// Allocators (runtime.c).
struct awk_obj *awk_alloc(int type);
VALUE awk_make_float (double d);
VALUE awk_make_int   (int64_t v);                  // fixnum if fits, else heap float
VALUE awk_make_string(const char *s, size_t len);  // type = AWK_T_STRING
VALUE awk_make_strnum(const char *s, size_t len);  // type = AWK_T_STRNUM (field values)
VALUE awk_make_array (void);

// Coercions / accessors.  awk's number/string duality: every value has
// both a numeric and string view.  Fields and getline input are
// "string-numeric" (strnum) — they coerce to number if the shape is
// numeric, otherwise behave as plain strings.
double      awk_to_num   (VALUE v);
const char *awk_to_cstr  (VALUE v, char *buf, size_t buflen, size_t *out_len);
VALUE       awk_to_string(VALUE v);  // forces a STRING VALUE

// Equality / comparison.  awk's comparison rule: if both operands are
// numeric (or strnum-with-numeric-shape), compare as numbers; else as
// strings.
bool awk_eq(VALUE a, VALUE b);
int  awk_cmp(VALUE a, VALUE b);

// Truthiness: awk's `if (x)` is true iff x is non-zero numeric or
// non-empty string.  Uninitialised → false.
static inline bool
awk_is_truthy(VALUE v)
{
    if (LIKELY(AWK_IS_FIX(v))) return AWK_FIX_VAL(v) != 0;
    if (v == AWK_UNINIT) return false;
    struct awk_obj *o = AWK_PTR(v);
    switch (o->type) {
      case AWK_T_FLOAT:  return o->dbl != 0.0;
      case AWK_T_STRING: return o->str.len != 0;
      case AWK_T_STRNUM:
        // strnum: number if it parses as one; otherwise string emptiness.
        if (o->str.len == 0) return false;
        { char *end; double d = strtod(o->str.chars, &end);
          if (end != o->str.chars && *end == '\0') return d != 0.0;
          return true; }
      default: return false;
    }
}

// Associative array operations.
VALUE awk_arr_get(VALUE arr, const char *key, size_t key_len);
void  awk_arr_set(VALUE arr, const char *key, size_t key_len, VALUE val);
bool  awk_arr_has(VALUE arr, const char *key, size_t key_len);
void  awk_arr_del(VALUE arr, const char *key, size_t key_len);

// String operations.
VALUE awk_concat(VALUE a, VALUE b);
VALUE awk_substr (VALUE s, int64_t pos, int64_t len);  // 1-based pos
VALUE awk_substr2(VALUE s, int64_t pos);                // to end
size_t awk_length(VALUE v);
int64_t awk_index(VALUE haystack, VALUE needle);        // 1-based, 0 not found
VALUE awk_tolower(VALUE v);
VALUE awk_toupper(VALUE v);

// printf-style formatting.  `awk_sprintf_v` builds a fresh STRING
// VALUE from `(fmt, args[])`; `awk_printf` writes it to fp.
VALUE awk_sprintf_v(VALUE fmt, VALUE *args, size_t nargs);
void  awk_printf(FILE *fp, VALUE fmt, VALUE *args, size_t nargs);

// split(s, arr, sep) — destructively populates arr (slot index resolved
// by parser) with the parts of s using sep.  Returns number of parts.
// If sep is empty, default FS is used.
int64_t awk_split(VALUE s, VALUE arr, VALUE sep);

// `int(x)` — truncate toward zero.
VALUE awk_int(VALUE v);

// Output.
void  awk_print_value(FILE *fp, VALUE v);                // OFMT-aware
void  awk_print_record(FILE *fp, VALUE *items, size_t n,
                       const char *ofs, size_t ofs_len,
                       const char *ors, size_t ors_len);

// Output stream cache (Phase 1.9).  `awk_open_*` looks up a cached
// FILE * by (mode, dest-string) or opens a new one.  At program exit,
// `awk_close_all_streams` flushes and pcloses/fcloses each.
//
//   mode 'w': popen(dest, "w")           — pipe output
//   mode 'o': fopen(dest, "w")           — overwrite file
//   mode 'a': fopen(dest, "a")           — append to file
FILE *awk_open_stream(int mode, VALUE dest);
void  awk_close_all_streams(void);

// ---------------------------------------------------------------------------
// 2-register RESULT (modeled on naruby / castro / astr).  awk control
// flow: `next` (skip to next record), `nextfile`, `exit` propagate as
// non-NORMAL state; `break` / `continue` are local to loops and use the
// same mechanism.
// ---------------------------------------------------------------------------

#define RESULT_NORMAL    0u
#define RESULT_NEXT      1u   // `next` — skip to next input record
#define RESULT_NEXTFILE  2u   // `nextfile`
#define RESULT_EXIT      3u   // `exit [n]` — terminate after running END
#define RESULT_BREAK     4u
#define RESULT_CONTINUE  5u
#define RESULT_RETURN    6u   // for user-defined functions (Phase 2+)

typedef struct {
    VALUE        value;
    unsigned int state;
} RESULT;

#define RESULT_OK(v)         ((RESULT){(v), RESULT_NORMAL})
#define RESULT_NEXT_()       ((RESULT){AWK_FIX(0), RESULT_NEXT})
#define RESULT_EXIT_(v)      ((RESULT){(v), RESULT_EXIT})
#define RESULT_BREAK_()      ((RESULT){AWK_FIX(0), RESULT_BREAK})
#define RESULT_CONTINUE_()   ((RESULT){AWK_FIX(0), RESULT_CONTINUE})
#define RESULT_RETURN_(v)    ((RESULT){(v), RESULT_RETURN})

#define UNWRAP(r) ({ RESULT _r = (r); if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; _r.value; })

// ---------------------------------------------------------------------------
// CTX / option struct.
// ---------------------------------------------------------------------------

struct awk_option {
    bool dump_ast;
    bool plain;
    bool compile_first;
    bool skip_bake;
    bool compile_only;
    bool clear_store;
    bool record_all;              // required by generated node_alloc.c
    const char *program_text;     // -e PROG
    const char *program_file;     // -f FILE
    char **input_files;           // remaining argv (NULL → stdin)
    int    input_file_cnt;
};

// Used by generated ALLOC_node_*; no user-defined functions in Phase
// 0+1 so this is a no-op stub.  Phase 2+ wires up the real code repo.
struct Node;
void code_repo_add(const char *name, struct Node *body, bool force_add);

extern struct awk_option OPTION;

// Per-record runtime state.  Fields are split lazily — on first
// access, awk_ensure_fields(c) populates `fields[]` from `record`
// using FS.  Writes to $N or NF rebuild `record` from fields using
// OFS.  $0 is `record_v` when valid, else freshly built.
struct awk_record {
    char    *record;             // $0 raw bytes (GC_malloc_atomic, NUL-terminated)
    size_t   record_len;
    VALUE    record_v;           // cached $0 VALUE, or 0 if unset
    VALUE   *fields;              // $1..$NF, lazily allocated; size = fields_capa
    int      nf;                  // current NF
    int      fields_capa;
    bool     fields_split;        // false → fields[] needs (re)build from record
};

// Special-variable slots in env (fixed layout, written by input loop).
// User-visible names map to these slots in the parser.
#define AWK_GLOB_NR     0
#define AWK_GLOB_NF     1
#define AWK_GLOB_FS     2
#define AWK_GLOB_OFS    3
#define AWK_GLOB_ORS    4
#define AWK_GLOB_RS     5
#define AWK_GLOB_FILENAME 6
#define AWK_GLOB_FNR    7
#define AWK_GLOB_SUBSEP 8
#define AWK_GLOB_RESERVED 16     // first user slot

// User-defined function (Phase 1.8).  Registered by arawk_node_def at
// program startup; looked up by arawk_node_call_user via name.
struct function_entry {
    const char  *name;
    struct Node *body;
    unsigned int params_cnt;
    unsigned int locals_cnt;     // total frame slots (params + extra locals)
};

typedef struct CTX_struct {
    VALUE        *env;           // global variable slots (incl. specials at fixed indices)
    VALUE        *fp;            // frame pointer (= env at top level; user func calls install a fresh frame)
    struct awk_record rec;       // current record state

    // User-defined function table.
    struct function_entry *func_set;
    unsigned int  func_set_cnt;

    // Input loop bookkeeping.
    FILE        *cur_input;
    int          cur_input_idx;  // index into OPTION.input_files
    bool         input_done;
} CTX;

// Side table for variadic-arity nodes (e.g. arawk_node_print with N items).
// Same convention as astr / pystro: parser packs NODE pointers into a
// flat array and the operand stores (base_idx, count).
extern struct Node **ARAWK_NODE_TABLE;
extern uint32_t      ARAWK_NODE_TABLE_LEN;

// Max number of slots in a single user-function frame (params + extra
// locals).  Must match the VLA size used in arawk_node_call_user.
#define ARAWK_FRAME_MAX 64

// Special-variable access helpers used by node_eval / runtime.
VALUE awk_get_nr (const CTX *c);
VALUE awk_get_nf (const CTX *c);
VALUE awk_get_field (CTX *c, int64_t n);   // $0 / $N
VALUE awk_get_field_v (CTX *c, VALUE idx); // $(expr)
void  awk_set_nr (CTX *c, VALUE v);
void  awk_set_nf (CTX *c, VALUE v);
void  awk_set_field (CTX *c, int64_t n, VALUE v);

// Input loop: read one record from current input source into c->rec.
// Returns false on EOF (across all input files).  Updates NR / FNR /
// FILENAME / $0 / NF.  Field splitting happens lazily.
bool awk_input_next_record(CTX *c);

// Slow paths for arithmetic.  Inline fast paths below check fixnum-
// fixnum first; everything else (string-numeric coercion, float, etc.)
// goes through the slow path.
VALUE awk_add_slow(VALUE a, VALUE b);
VALUE awk_sub_slow(VALUE a, VALUE b);
VALUE awk_mul_slow(VALUE a, VALUE b);
VALUE awk_div_slow(VALUE a, VALUE b);
VALUE awk_mod_slow(VALUE a, VALUE b);
VALUE awk_pow_slow(VALUE a, VALUE b);
VALUE awk_neg_slow(VALUE a);

static inline VALUE
awk_add(VALUE a, VALUE b)
{
    if (LIKELY(AWK_IS_FIX(a) & AWK_IS_FIX(b))) {
        int64_t la = AWK_FIX_VAL(a), lb = AWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r) &&
                   r <= AWK_FIX_MAX && r >= AWK_FIX_MIN)) {
            return AWK_FIX(r);
        }
    }
    return awk_add_slow(a, b);
}

static inline VALUE
awk_sub(VALUE a, VALUE b)
{
    if (LIKELY(AWK_IS_FIX(a) & AWK_IS_FIX(b))) {
        int64_t la = AWK_FIX_VAL(a), lb = AWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_sub_overflow(la, lb, &r) &&
                   r <= AWK_FIX_MAX && r >= AWK_FIX_MIN)) {
            return AWK_FIX(r);
        }
    }
    return awk_sub_slow(a, b);
}

static inline VALUE
awk_mul(VALUE a, VALUE b)
{
    if (LIKELY(AWK_IS_FIX(a) & AWK_IS_FIX(b))) {
        int64_t la = AWK_FIX_VAL(a), lb = AWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_mul_overflow(la, lb, &r) &&
                   r <= AWK_FIX_MAX && r >= AWK_FIX_MIN)) {
            return AWK_FIX(r);
        }
    }
    return awk_mul_slow(a, b);
}

// awk's `/` is always floating-point; no integer-divide fast path.
static inline VALUE awk_div(VALUE a, VALUE b) { return awk_div_slow(a, b); }
static inline VALUE awk_mod(VALUE a, VALUE b) { return awk_mod_slow(a, b); }
static inline VALUE awk_pow(VALUE a, VALUE b) { return awk_pow_slow(a, b); }

static inline VALUE
awk_neg(VALUE a)
{
    if (LIKELY(AWK_IS_FIX(a))) {
        int64_t la = AWK_FIX_VAL(a);
        if (LIKELY(la != AWK_FIX_MIN)) return AWK_FIX(-la);
    }
    return awk_neg_slow(a);
}

#endif // ARAWK_CONTEXT_H
