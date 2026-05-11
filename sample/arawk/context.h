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
// Uninitialised awk variables are ARAWK_UNINIT (singleton); they coerce
// to "" in string context and 0 in numeric context.  String literals
// and field values are heap awk_obj with type ARAWK_T_STRING.  Numeric
// constants outside fixnum range box to ARAWK_T_FLOAT.  Associative
// arrays are ARAWK_T_ARRAY.
typedef int64_t VALUE;

#define ARAWK_FIX_MAX     ((int64_t)((1LL << 62) - 1))
#define ARAWK_FIX_MIN     ((int64_t)(-(1LL << 62)))
#define ARAWK_IS_FIX(v)   ((int64_t)(v) & 1LL)
#define ARAWK_FIX(n)      (((VALUE)(int64_t)(n) << 1) | 1LL)
#define ARAWK_FIX_VAL(v)  ((int64_t)(v) >> 1)
#define ARAWK_IS_PTR(v)   (((int64_t)(v) & 1LL) == 0)
#define ARAWK_PTR(v)      ((struct awk_obj *)(uintptr_t)(v))
#define ARAWK_OBJ_VAL(p)  ((VALUE)(uintptr_t)(p))

enum awk_type {
    ARAWK_T_UNINIT = 1,      // singleton — uninitialised awk variable
    ARAWK_T_FLOAT,
    ARAWK_T_STRING,
    ARAWK_T_STRNUM,          // string from field/getline — numeric coercion if shape matches
    ARAWK_T_ARRAY,           // associative array
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

extern struct awk_obj ARAWK_UNINIT_OBJ;
#define ARAWK_UNINIT  ARAWK_OBJ_VAL(&ARAWK_UNINIT_OBJ)

// Thread-local-ish pointer to the active CTX.  Set by main.c right
// after create_context.  Runtime helpers like arawk_to_cstr read
// CONVFMT / OFMT from `ARAWK_CURRENT_CTX->env[...]` without having
// to plumb a CTX through every call site.  Single-CTX program model;
// fine for our embedding.
struct CTX_struct;
extern struct CTX_struct *ARAWK_CURRENT_CTX;

// Allocators (runtime.c).
struct awk_obj *arawk_alloc(int type);
VALUE arawk_make_float (double d);
VALUE arawk_make_int   (int64_t v);                  // fixnum if fits, else heap float
VALUE arawk_make_string(const char *s, size_t len);  // type = ARAWK_T_STRING
VALUE arawk_make_strnum(const char *s, size_t len);  // type = ARAWK_T_STRNUM (field values)
VALUE arawk_make_array (void);

// Coercions / accessors.  awk's number/string duality: every value has
// both a numeric and string view.  Fields and getline input are
// "string-numeric" (strnum) — they coerce to number if the shape is
// numeric, otherwise behave as plain strings.
double      arawk_to_num   (VALUE v);
const char *arawk_to_cstr  (VALUE v, char *buf, size_t buflen, size_t *out_len);
VALUE       arawk_to_string(VALUE v);  // forces a STRING VALUE

// Equality / comparison.  awk's comparison rule: if both operands are
// numeric (or strnum-with-numeric-shape), compare as numbers; else as
// strings.
bool arawk_eq(VALUE a, VALUE b);
int  arawk_cmp(VALUE a, VALUE b);

// Truthiness: awk's `if (x)` is true iff x is non-zero numeric or
// non-empty string.  Uninitialised → false.
static inline bool
arawk_is_truthy(VALUE v)
{
    if (LIKELY(ARAWK_IS_FIX(v))) return ARAWK_FIX_VAL(v) != 0;
    if (v == ARAWK_UNINIT) return false;
    struct awk_obj *o = ARAWK_PTR(v);
    switch (o->type) {
      case ARAWK_T_FLOAT:  return o->dbl != 0.0;
      case ARAWK_T_STRING: return o->str.len != 0;
      case ARAWK_T_STRNUM:
        // strnum: number if it parses as one; otherwise string emptiness.
        if (o->str.len == 0) return false;
        { char *end; double d = strtod(o->str.chars, &end);
          if (end != o->str.chars && *end == '\0') return d != 0.0;
          return true; }
      default: return false;
    }
}

// Associative array operations.
VALUE arawk_arr_get(VALUE arr, const char *key, size_t key_len);
void  arawk_arr_set(VALUE arr, const char *key, size_t key_len, VALUE val);
bool  arawk_arr_has(VALUE arr, const char *key, size_t key_len);
void  arawk_arr_del(VALUE arr, const char *key, size_t key_len);

// String operations.
VALUE arawk_concat(VALUE a, VALUE b);
VALUE arawk_substr (VALUE s, int64_t pos, int64_t len);  // 1-based pos
VALUE arawk_substr2(VALUE s, int64_t pos);                // to end
size_t arawk_length(VALUE v);
int64_t arawk_index(VALUE haystack, VALUE needle);        // 1-based, 0 not found
VALUE arawk_tolower(VALUE v);
VALUE arawk_toupper(VALUE v);

// printf-style formatting.  `arawk_sprintf_v` builds a fresh STRING
// VALUE from `(fmt, args[])`; `arawk_printf` writes it to fp.
VALUE arawk_sprintf_v(VALUE fmt, VALUE *args, size_t nargs);
void  arawk_printf(FILE *fp, VALUE fmt, VALUE *args, size_t nargs);

// split(s, arr, sep) — destructively populates arr (slot index resolved
// by parser) with the parts of s using sep.  Returns number of parts.
// If sep is empty, default FS is used.
int64_t arawk_split(VALUE s, VALUE arr, VALUE sep);

// `int(x)` — truncate toward zero.
VALUE arawk_int(VALUE v);

// Output.
void  arawk_print_value(FILE *fp, VALUE v);                // OFMT-aware
void  arawk_print_record(FILE *fp, VALUE *items, size_t n,
                       const char *ofs, size_t ofs_len,
                       const char *ors, size_t ors_len);

// Output stream cache (Phase 1.9).  `arawk_open_*` looks up a cached
// FILE * by (mode, dest-string) or opens a new one.  At program exit,
// `arawk_close_all_streams` flushes and pcloses/fcloses each.
//
//   mode 'w': popen(dest, "w")           — pipe output
//   mode 'o': fopen(dest, "w")           — overwrite file
//   mode 'a': fopen(dest, "a")           — append to file
FILE *arawk_open_stream(int mode, VALUE dest);
void  arawk_close_all_streams(void);

// `close(name)` — flush + close one previously-opened stream by name.
// Returns 0 on success, -1 if no stream named `dest` is open.
int   arawk_close_stream(VALUE dest);

// `fflush()` / `fflush("")` / `fflush(name)`.  Empty / no arg flushes
// stdout and all open output streams; a name flushes just that one.
// Returns 0 on success, -1 if name doesn't refer to an open stream.
int   arawk_fflush_all(void);
int   arawk_fflush_stream(VALUE dest);

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
#define RESULT_NEXT_()       ((RESULT){ARAWK_FIX(0), RESULT_NEXT})
#define RESULT_EXIT_(v)      ((RESULT){(v), RESULT_EXIT})
#define RESULT_BREAK_()      ((RESULT){ARAWK_FIX(0), RESULT_BREAK})
#define RESULT_CONTINUE_()   ((RESULT){ARAWK_FIX(0), RESULT_CONTINUE})
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
// access, arawk_ensure_fields(c) populates `fields[]` from `record`
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
#define ARAWK_GLOB_NR        0
#define ARAWK_GLOB_NF        1
#define ARAWK_GLOB_FS        2
#define ARAWK_GLOB_OFS       3
#define ARAWK_GLOB_ORS       4
#define ARAWK_GLOB_RS        5
#define ARAWK_GLOB_FILENAME  6
#define ARAWK_GLOB_FNR       7
#define ARAWK_GLOB_SUBSEP    8
#define ARAWK_GLOB_CONVFMT   9
#define ARAWK_GLOB_OFMT      10
#define ARAWK_GLOB_RSTART    11
#define ARAWK_GLOB_RLENGTH   12
#define ARAWK_GLOB_ENVIRON   13     // associative array populated from `environ`
#define ARAWK_GLOB_ARGC      14
#define ARAWK_GLOB_ARGV      15
#define ARAWK_GLOB_RESERVED  16     // first user slot

// User-defined function (Phase 1.8).  Registered by node_def at
// program startup; looked up by node_call_user via name.
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

// Side table for variadic-arity nodes (e.g. node_print with N items).
// Same convention as astr / pystro: parser packs NODE pointers into a
// flat array and the operand stores (base_idx, count).
extern struct Node **ARAWK_NODE_TABLE;
extern uint32_t      ARAWK_NODE_TABLE_LEN;

// Max number of slots in a single user-function frame (params + extra
// locals).  Must match the VLA size used in node_call_user.
#define ARAWK_FRAME_MAX 64

// Special-variable access helpers used by node_eval / runtime.
VALUE arawk_get_nr (const CTX *c);
VALUE arawk_get_nf (const CTX *c);
VALUE arawk_get_field (CTX *c, int64_t n);   // $0 / $N
VALUE arawk_get_field_v (CTX *c, VALUE idx); // $(expr)
void  arawk_set_nr (CTX *c, VALUE v);
void  arawk_set_nf (CTX *c, VALUE v);
void  arawk_set_field (CTX *c, int64_t n, VALUE v);

// Input loop: read one record from current input source into c->rec.
// Returns false on EOF (across all input files).  Updates NR / FNR /
// FILENAME / $0 / NF.  Field splitting happens lazily.
bool arawk_input_next_record(CTX *c);

// `getline` low-level read.  Reads one record (RS-delimited) into a
// caller-supplied growable buffer.  Returns 1 on success, 0 at EOF,
// -1 on I/O error.  The buffer is owned by the caller and reused
// across calls; pass `*buf_capa = 0` on first call to allocate.
int arawk_read_record_into(FILE *fp, char **buf, size_t *buf_len, size_t *buf_capa);

// `getline` builtins — six forms (POSIX) covering current-input /
// file / cmd × $0-or-var.  Each returns 1 on read, 0 on EOF, -1 on
// I/O error.  Side effects (which of NR / FNR / NF / $0 / FILENAME
// are updated) follow POSIX:
//   getline             → NR FNR NF $0 (across-file boundary may also FILENAME)
//   getline var         → NR FNR        (var = line)
//   getline < file      →           NF $0
//   getline var < file  →                  (var = line)
//   cmd | getline       →           NF $0
//   cmd | getline var   →                  (var = line)
int arawk_getline_cur     (CTX *c, VALUE *out_line);     // out_line ignored (sets $0)
int arawk_getline_cur_var (CTX *c, VALUE *out_line);     // out_line ← line as STRNUM
int arawk_getline_file    (CTX *c, VALUE dest);          // $0
int arawk_getline_file_var(CTX *c, VALUE dest, VALUE *out_line);
int arawk_getline_cmd     (CTX *c, VALUE cmd);           // $0
int arawk_getline_cmd_var (CTX *c, VALUE cmd, VALUE *out_line);

// Slow paths for arithmetic.  Inline fast paths below check fixnum-
// fixnum first; everything else (string-numeric coercion, float, etc.)
// goes through the slow path.
VALUE arawk_add_slow(VALUE a, VALUE b);
VALUE arawk_sub_slow(VALUE a, VALUE b);
VALUE arawk_mul_slow(VALUE a, VALUE b);
VALUE arawk_div_slow(VALUE a, VALUE b);
VALUE arawk_mod_slow(VALUE a, VALUE b);
VALUE arawk_pow_slow(VALUE a, VALUE b);
VALUE arawk_neg_slow(VALUE a);

static inline VALUE
arawk_add(VALUE a, VALUE b)
{
    if (LIKELY(ARAWK_IS_FIX(a) & ARAWK_IS_FIX(b))) {
        int64_t la = ARAWK_FIX_VAL(a), lb = ARAWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r) &&
                   r <= ARAWK_FIX_MAX && r >= ARAWK_FIX_MIN)) {
            return ARAWK_FIX(r);
        }
    }
    return arawk_add_slow(a, b);
}

static inline VALUE
arawk_sub(VALUE a, VALUE b)
{
    if (LIKELY(ARAWK_IS_FIX(a) & ARAWK_IS_FIX(b))) {
        int64_t la = ARAWK_FIX_VAL(a), lb = ARAWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_sub_overflow(la, lb, &r) &&
                   r <= ARAWK_FIX_MAX && r >= ARAWK_FIX_MIN)) {
            return ARAWK_FIX(r);
        }
    }
    return arawk_sub_slow(a, b);
}

static inline VALUE
arawk_mul(VALUE a, VALUE b)
{
    if (LIKELY(ARAWK_IS_FIX(a) & ARAWK_IS_FIX(b))) {
        int64_t la = ARAWK_FIX_VAL(a), lb = ARAWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_mul_overflow(la, lb, &r) &&
                   r <= ARAWK_FIX_MAX && r >= ARAWK_FIX_MIN)) {
            return ARAWK_FIX(r);
        }
    }
    return arawk_mul_slow(a, b);
}

// awk's `/` is always floating-point; no integer-divide fast path.
static inline VALUE arawk_div(VALUE a, VALUE b) { return arawk_div_slow(a, b); }
static inline VALUE arawk_mod(VALUE a, VALUE b) { return arawk_mod_slow(a, b); }
static inline VALUE arawk_pow(VALUE a, VALUE b) { return arawk_pow_slow(a, b); }

static inline VALUE
arawk_neg(VALUE a)
{
    if (LIKELY(ARAWK_IS_FIX(a))) {
        int64_t la = ARAWK_FIX_VAL(a);
        if (LIKELY(la != ARAWK_FIX_MIN)) return ARAWK_FIX(-la);
    }
    return arawk_neg_slow(a);
}

#endif // ARAWK_CONTEXT_H
